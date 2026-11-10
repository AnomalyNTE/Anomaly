// WinHTTP transport for the bilibili API and media downloads.
//
// API documents use a manual redirect loop: every hop is revalidated as an
// absolute HTTPS target, at most five redirects are followed, and the caller's
// cancel flag is polled between hops and while the body is read. Media
// transports reuse the same headers and expose a chunked stream whose receive
// timeout is short so the downloader can poll its own cancel flag. The security
// predicate only trusts the literal host: media URLs from the API still may not
// point at loopback or private address literals.
//
// Every diagnostic string is English and only ever logged.

#include "http_client.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace better_tv_video {
namespace {

constexpr wchar_t kUserAgent[] = L"Anomaly-BetterTV/1.0";
constexpr wchar_t kReferer[] = L"https://www.bilibili.com/";
constexpr wchar_t kOrigin[] = L"https://www.bilibili.com";
constexpr wchar_t kDefaultAccept[] = L"application/json, text/plain, */*";
// One request hop: name resolution, connect, send and receive budgets.
constexpr int kResolveTimeoutMilliseconds = 3000;
constexpr int kConnectTimeoutMilliseconds = 3000;
constexpr int kSendTimeoutMilliseconds = 5000;
constexpr int kReceiveTimeoutMilliseconds = 5000;
// Media reads poll the caller's cancel flag between chunks; a stalled transfer
// must surface quickly instead of blocking for the API receive budget.
constexpr int kMediaReceiveTimeoutMilliseconds = 2000;
constexpr int kMaxRedirectHops = 5;
// API documents are small; the cap bounds how much memory one response can use.
constexpr std::size_t kMaxDocumentBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxHostLength = 253;
constexpr std::size_t kMaxLabelLength = 63;
constexpr std::string_view kLoopbackHostName = "localhost";
constexpr std::string_view kLoopbackHostSuffix = ".localhost";

char ToLowerAscii(const char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool IsAsciiDigit(const char value) {
    return value >= '0' && value <= '9';
}

bool IsAsciiAlpha(const char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiAlnum(const char value) {
    return IsAsciiAlpha(value) || IsAsciiDigit(value);
}

bool EqualsIgnoreCase(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ToLowerAscii(left[index]) != ToLowerAscii(right[index])) return false;
    }
    return true;
}

bool StartsWithIgnoreCase(const std::string_view text, const std::string_view prefix) {
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool EndsWith(const std::string_view text, const std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

bool FailWithDetail(std::string* detail, std::string message) {
    if (detail != nullptr) *detail = std::move(message);
    return false;
}

std::string WinHttpFailureDetail(const std::string_view step) {
    return std::string(step) + " failed (error " + std::to_string(GetLastError()) + ")";
}

bool Utf8ToWide(const std::string_view text, std::wstring* wide) {
    if (text.empty() || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return false;
    std::wstring buffer(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), buffer.data(), length) != length) {
        return false;
    }
    *wide = std::move(buffer);
    return true;
}

bool WideToUtf8(const std::wstring& text, std::string* utf8) {
    if (text.empty()) {
        utf8->clear();
        return true;
    }
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return false;
    std::string buffer(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buffer.data(),
                            length, nullptr, nullptr) != length) {
        return false;
    }
    *utf8 = std::move(buffer);
    return true;
}

// An absolute HTTPS URL with a literal host this transport can connect to.
struct HttpsTarget final {
    std::string host;
    std::wstring host_wide;
    std::wstring path_wide;
    // Bilibili's media CDN may expose an explicit non-default TLS port.
    INTERNET_PORT port{INTERNET_DEFAULT_HTTPS_PORT};
};

bool ParseIpv4Literal(const std::string_view host, std::array<std::uint8_t, 4>* octets) {
    std::size_t index = 0;
    for (int part = 0; part < 4; ++part) {
        if (index >= host.size()) return false;
        std::size_t end = host.find('.', index);
        if (part == 3) {
            if (end != std::string_view::npos) return false;
            end = host.size();
        } else if (end == std::string_view::npos) {
            return false;
        }
        const std::string_view octet = host.substr(index, end - index);
        if (octet.empty() || octet.size() > 3) return false;
        if (octet.size() > 1 && octet.front() == '0') return false;
        int value = 0;
        for (const char digit : octet) {
            if (!IsAsciiDigit(digit)) return false;
            value = value * 10 + (digit - '0');
        }
        if (value > 255) return false;
        (*octets)[static_cast<std::size_t>(part)] = static_cast<std::uint8_t>(value);
        index = end + 1;
    }
    return true;
}

bool IsIpv4LiteralHost(const std::string_view host, std::array<std::uint8_t, 4>* octets) {
    if (host.empty()) return false;
    for (const char value : host) {
        if (!IsAsciiDigit(value) && value != '.') return false;
    }
    return ParseIpv4Literal(host, octets);
}

bool IsPrivateOrLoopbackIpv4(const std::array<std::uint8_t, 4>& octets) {
    const std::uint8_t first = octets[0];
    const std::uint8_t second = octets[1];
    return first == 0 || first == 10 || first == 127 ||
           (first == 172 && second >= 16 && second <= 31) ||
           (first == 192 && second == 168) ||
           (first == 169 && second == 254);
}

bool ValidateHost(const std::string_view host, std::string* detail) {
    bool numeric = !host.empty();
    for (const char value : host) {
        if (!IsAsciiDigit(value) && value != '.') {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        std::array<std::uint8_t, 4> octets{};
        if (!ParseIpv4Literal(host, &octets)) {
            *detail = "URL host is a malformed IPv4 address";
            return false;
        }
        return true;
    }

    // Domain name: dot separated labels of alphanumerics and hyphens.
    std::size_t begin = 0;
    while (begin <= host.size()) {
        std::size_t end = host.find('.', begin);
        if (end == std::string_view::npos) end = host.size();
        const std::string_view label = host.substr(begin, end - begin);
        if (label.empty() || label.size() > kMaxLabelLength || label.front() == '-' ||
            label.back() == '-') {
            *detail = "URL host has an invalid label";
            return false;
        }
        for (const char value : label) {
            if (!IsAsciiAlnum(value) && value != '-') {
                *detail = "URL host has an invalid character";
                return false;
            }
        }
        if (end == host.size()) break;
        begin = end + 1;
    }
    return true;
}

bool ParseHttpsTarget(const std::string_view url, HttpsTarget* target, std::string* detail) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() <= kScheme.size() || !StartsWithIgnoreCase(url, kScheme)) {
        return FailWithDetail(detail, "URL does not use HTTPS");
    }
    std::string_view rest = url.substr(kScheme.size());
    const std::size_t fragment = rest.find('#');
    if (fragment != std::string_view::npos) rest = rest.substr(0, fragment);

    const std::size_t authority_end = rest.find_first_of("/?");
    const std::string_view authority = rest.substr(0, authority_end);
    if (authority.empty()) return FailWithDetail(detail, "URL has no host");
    if (authority.find('@') != std::string_view::npos) {
        return FailWithDetail(detail, "URL carries userinfo");
    }
    // The transport only connects to DNS names and IPv4 literals; rejecting
    // bracketed IPv6 literals keeps the host checks unambiguous.
    if (authority.find('[') != std::string_view::npos ||
        authority.find(']') != std::string_view::npos) {
        return FailWithDetail(detail, "URL uses an IPv6 literal host");
    }

    std::string_view host = authority;
    const std::size_t port_separator = authority.rfind(':');
    if (port_separator != std::string_view::npos) {
        const std::string_view port = authority.substr(port_separator + 1);
        host = authority.substr(0, port_separator);
        if (port.empty() || port.size() > 5) {
            return FailWithDetail(detail, "URL has an invalid port");
        }
        unsigned value = 0;
        for (const char digit : port) {
            if (!IsAsciiDigit(digit)) return FailWithDetail(detail, "URL has an invalid port");
            value = value * 10U + static_cast<unsigned>(digit - '0');
        }
        if (value == 0 || value > 65535U) {
            return FailWithDetail(detail, "URL has an invalid port");
        }
        target->port = static_cast<INTERNET_PORT>(value);
    }
    if (host.empty() || host.size() > kMaxHostLength) {
        return FailWithDetail(detail, "URL has an invalid host");
    }
    std::string lower_host;
    lower_host.reserve(host.size());
    for (const char value : host) lower_host.push_back(ToLowerAscii(value));
    std::string host_detail;
    if (!ValidateHost(lower_host, &host_detail)) return FailWithDetail(detail, host_detail);

    std::string_view path = authority_end == std::string_view::npos
                                ? std::string_view{}
                                : rest.substr(authority_end);
    std::string path_and_query;
    if (path.empty()) {
        path_and_query = "/";
    } else if (path.front() == '?') {
        path_and_query = "/";
        path_and_query.append(path);
    } else {
        path_and_query.assign(path);
    }
    for (const char value : path_and_query) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte <= 0x20 || byte == 0x7F) {
            return FailWithDetail(detail, "URL path contains a control character");
        }
    }

    target->host = std::move(lower_host);
    if (!Utf8ToWide(target->host, &target->host_wide)) {
        return FailWithDetail(detail, "URL host is not valid text");
    }
    if (!Utf8ToWide(path_and_query, &target->path_wide)) {
        return FailWithDetail(detail, "URL path is not valid text");
    }
    return true;
}

// Owns one WinHTTP handle; resetting or destroying it closes the handle.
class WinHttpHandle final {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(const HINTERNET handle) noexcept : handle_(handle) {}
    ~WinHttpHandle() { Reset(nullptr); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    void Reset(HINTERNET handle) {
        if (handle_ != nullptr) WinHttpCloseHandle(handle_);
        handle_ = handle;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }

private:
    HINTERNET handle_{};
};

bool ApplyTransportOptions(const HINTERNET session, const int receive_timeout_milliseconds,
                           std::string* detail) {
    if (!WinHttpSetTimeouts(session, kResolveTimeoutMilliseconds, kConnectTimeoutMilliseconds,
                            kSendTimeoutMilliseconds, receive_timeout_milliseconds)) {
        return FailWithDetail(detail, WinHttpFailureDetail("WinHTTP timeout setup"));
    }
    // Redirects are followed by hand so every target can be revalidated; the
    // session must never silently follow one.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                          sizeof(redirect_policy))) {
        return FailWithDetail(detail, WinHttpFailureDetail("WinHTTP redirect policy"));
    }
    return true;
}

bool AddRequestHeaders(const HINTERNET request, std::string* detail) {
    const std::wstring headers = std::wstring(L"Referer: ") + kReferer + L"\r\nOrigin: " +
                                 kOrigin + L"\r\nAccept: " + kDefaultAccept + L"\r\n";
    if (!WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return FailWithDetail(detail, WinHttpFailureDetail("WinHTTP request headers"));
    }
    return true;
}

bool QueryStatusCode(const HINTERNET request, DWORD* status) {
    DWORD size = sizeof(*status);
    return WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, status, &size,
                               WINHTTP_NO_HEADER_INDEX) != FALSE;
}

bool QueryLocation(const HINTERNET request, std::string* location) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (size < sizeof(wchar_t)) return false;
    std::wstring buffer(static_cast<std::size_t>(size / sizeof(wchar_t)), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                             buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    buffer.resize(static_cast<std::size_t>(size / sizeof(wchar_t)));
    while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    return WideToUtf8(buffer, location);
}

bool ReadDocumentBody(const HINTERNET request, const std::function<bool()>& cancelled,
                      std::string* body, std::string* detail) {
    for (;;) {
        if (cancelled && cancelled()) return FailWithDetail(detail, "request cancelled");
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            return FailWithDetail(detail, WinHttpFailureDetail("response read"));
        }
        if (available == 0) return true;
        if (body->size() + available > kMaxDocumentBytes) {
            return FailWithDetail(detail, "response body exceeds the document limit");
        }
        const std::size_t offset = body->size();
        body->resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body->data() + offset, available, &read)) {
            body->resize(offset);
            return FailWithDetail(detail, WinHttpFailureDetail("response read"));
        }
        body->resize(offset + read);
        if (read == 0) return true;
    }
}

class WinHttpMediaStream final : public HttpMediaStream {
public:
    bool Open(std::string_view url, std::uint64_t* total_bytes, std::string* detail) override;
    int Read(std::span<std::uint8_t> buffer, std::string* detail) override;
    void Close() override;

private:
    WinHttpHandle session_;
    WinHttpHandle connection_;
    WinHttpHandle request_;
};

bool WinHttpMediaStream::Open(const std::string_view url, std::uint64_t* total_bytes,
                              std::string* detail) {
    Close();
    if (total_bytes != nullptr) *total_bytes = 0;

    HttpsTarget target;
    if (!ParseHttpsTarget(url, &target, detail)) return false;

    session_.Reset(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session_.get() == nullptr) {
        return FailWithDetail(detail, WinHttpFailureDetail("WinHTTP session"));
    }
    if (!ApplyTransportOptions(session_.get(), kMediaReceiveTimeoutMilliseconds, detail)) {
        Close();
        return false;
    }
    connection_.Reset(WinHttpConnect(session_.get(), target.host_wide.c_str(), target.port, 0));
    if (connection_.get() == nullptr) {
        Close();
        return FailWithDetail(detail, WinHttpFailureDetail("media connection"));
    }
    request_.Reset(WinHttpOpenRequest(connection_.get(), L"GET", target.path_wide.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE));
    if (request_.get() == nullptr) {
        Close();
        return FailWithDetail(detail, WinHttpFailureDetail("media request"));
    }
    if (!AddRequestHeaders(request_.get(), detail)) {
        Close();
        return false;
    }
    if (!WinHttpSendRequest(request_.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        Close();
        return FailWithDetail(detail, WinHttpFailureDetail("media request send"));
    }
    if (!WinHttpReceiveResponse(request_.get(), nullptr)) {
        Close();
        return FailWithDetail(detail, WinHttpFailureDetail("media response"));
    }
    DWORD status = 0;
    if (!QueryStatusCode(request_.get(), &status)) {
        Close();
        return FailWithDetail(detail, WinHttpFailureDetail("media status"));
    }
    if (status != 200) {
        Close();
        return FailWithDetail(detail, "media request returned HTTP " + std::to_string(status));
    }

    DWORD64 content_length = 0;
    DWORD length_size = sizeof(content_length);
    if (WinHttpQueryHeaders(request_.get(),
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                            WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_size,
                            WINHTTP_NO_HEADER_INDEX)) {
        // Zero stays meaningful: the server did not report a length, so the
        // downloader falls back to the size announced by the API.
        if (total_bytes != nullptr) *total_bytes = static_cast<std::uint64_t>(content_length);
    }
    return true;
}

int WinHttpMediaStream::Read(const std::span<std::uint8_t> buffer, std::string* detail) {
    if (request_.get() == nullptr) {
        FailWithDetail(detail, "media stream is not open");
        return -1;
    }
    if (buffer.empty()) return 0;
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request_.get(), &available)) {
        FailWithDetail(detail, WinHttpFailureDetail("media stream read"));
        return -1;
    }
    if (available == 0) return 0;
    const std::size_t capacity =
        (std::min)(static_cast<std::size_t>(available), buffer.size());
    DWORD read = 0;
    if (!WinHttpReadData(request_.get(), buffer.data(), static_cast<DWORD>(capacity), &read)) {
        FailWithDetail(detail, WinHttpFailureDetail("media stream read"));
        return -1;
    }
    return static_cast<int>(read);
}

void WinHttpMediaStream::Close() {
    request_.Reset(nullptr);
    connection_.Reset(nullptr);
    session_.Reset(nullptr);
}

}  // namespace

HttpTextResult HttpGetText(const std::string_view url, const std::function<bool()>& cancelled) {
    HttpTextResult result;
    std::string current(url);
    int followed = 0;
    for (;;) {
        if (cancelled && cancelled()) {
            result.detail = "request cancelled";
            return result;
        }
        HttpsTarget target;
        if (!ParseHttpsTarget(current, &target, &result.detail)) return result;

        WinHttpHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.get() == nullptr) {
            result.detail = WinHttpFailureDetail("WinHTTP session");
            return result;
        }
        if (!ApplyTransportOptions(session.get(), kReceiveTimeoutMilliseconds, &result.detail)) {
            return result;
        }
        WinHttpHandle connection(WinHttpConnect(session.get(), target.host_wide.c_str(),
                                                target.port, 0));
        if (connection.get() == nullptr) {
            result.detail = WinHttpFailureDetail("api connection");
            return result;
        }
        WinHttpHandle request(
            WinHttpOpenRequest(connection.get(), L"GET", target.path_wide.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               WINHTTP_FLAG_SECURE));
        if (request.get() == nullptr) {
            result.detail = WinHttpFailureDetail("api request");
            return result;
        }
        if (!AddRequestHeaders(request.get(), &result.detail)) return result;
        if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            result.detail = WinHttpFailureDetail("api request send");
            return result;
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) {
            result.detail = WinHttpFailureDetail("api response");
            return result;
        }
        DWORD status = 0;
        if (!QueryStatusCode(request.get(), &status)) {
            result.detail = WinHttpFailureDetail("api status");
            return result;
        }
        if (status >= 300 && status < 400) {
            std::string location;
            if (!QueryLocation(request.get(), &location) || location.empty()) {
                result.detail = "redirect response has no Location header";
                return result;
            }
            if (followed >= kMaxRedirectHops) {
                result.detail = "request follows more than five redirects";
                return result;
            }
            if (!StartsWithIgnoreCase(location, "https://")) {
                result.detail = "redirect target is not an absolute HTTPS URL";
                return result;
            }
            current = std::move(location);
            ++followed;
            continue;
        }
        result.status = static_cast<int>(status);
        if (status < 200 || status >= 300) {
            result.detail = "request returned HTTP " + std::to_string(status);
            return result;
        }
        std::string body;
        if (!ReadDocumentBody(request.get(), cancelled, &body, &result.detail)) return result;
        result.ok = true;
        result.body = std::move(body);
        return result;
    }
}

HttpMediaStreamFactory DefaultMediaStreamFactory() {
    return [] { return std::make_unique<WinHttpMediaStream>(); };
}

bool IsSafeMediaUrl(const std::string_view url, std::string* detail) {
    HttpsTarget target;
    std::string reason;
    if (!ParseHttpsTarget(url, &target, &reason)) return FailWithDetail(detail, reason);

    if (target.host == kLoopbackHostName || EndsWith(target.host, kLoopbackHostSuffix)) {
        return FailWithDetail(detail, "media URL uses a loopback host name");
    }
    std::array<std::uint8_t, 4> octets{};
    if (IsIpv4LiteralHost(target.host, &octets) && IsPrivateOrLoopbackIpv4(octets)) {
        return FailWithDetail(detail, "media URL uses a private or loopback address");
    }
    if (detail != nullptr) detail->clear();
    return true;
}

}  // namespace better_tv_video
