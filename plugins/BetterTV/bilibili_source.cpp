// Bilibili link parsing for the television source.
//
// Accepts a bare BV/av identifier, a bilibili video page URL, a b23.tv share
// link, or share text containing one of those URLs. A short link is resolved
// one redirect at a time without ever leaving HTTPS or the allowlisted hosts.
// Only URL shapes are validated here; whether the page exists is decided by the
// downloader later. Every diagnostic string is English and only ever logged.

#include "bilibili_source.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace better_tv_video {
namespace {

constexpr wchar_t kUserAgent[] = L"Anomaly-BetterTV/1.0";
constexpr std::string_view kShortHost = "b23.tv";
constexpr std::array<std::string_view, 3> kVideoHosts{
    "bilibili.com", "www.bilibili.com", "m.bilibili.com"};
constexpr std::string_view kVideoPathPrefix = "/video/";
constexpr std::string_view kVideoOrigin = "https://www.bilibili.com/video/";
constexpr std::string_view kBvPrefix = "BV";
constexpr std::string_view kAvPrefix = "av";
constexpr std::size_t kBvIdentifierLength = 12;  // "BV" plus ten characters.
// One GET without following redirects: name, connect, send and receive budgets.
constexpr int kResolveTimeoutMilliseconds = 3000;
constexpr int kConnectTimeoutMilliseconds = 3000;
constexpr int kSendTimeoutMilliseconds = 5000;
constexpr int kReceiveTimeoutMilliseconds = 5000;

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

bool IsAsciiSpace(const char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
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

std::string_view TrimAscii(const std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsAsciiSpace(text[begin])) ++begin;
    while (end > begin && IsAsciiSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// A pasted share text puts the URL next to Chinese copy or quotes; everything
// after whitespace, a non-ASCII byte or a delimiter is not part of the link.
bool IsUrlTerminator(const char value) {
    const unsigned char byte = static_cast<unsigned char>(value);
    if (byte <= 0x20 || byte >= 0x7F) return true;
    return value == '"' || value == '\'' || value == '<' || value == '>' ||
           value == '\\' || value == '|';
}

std::string_view ExtractUrlFromText(const std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t found = text.find("http", offset);
        if (found == std::string_view::npos) break;
        const std::string_view candidate = text.substr(found);
        const std::size_t scheme_length = StartsWithIgnoreCase(candidate, "https://")
                                              ? std::string_view{"https://"}.size()
                                              : StartsWithIgnoreCase(candidate, "http://")
                                                    ? std::string_view{"http://"}.size()
                                                    : 0;
        if (scheme_length != 0) {
            std::size_t end = 0;
            while (end < candidate.size() && !IsUrlTerminator(candidate[end])) ++end;
            return candidate.substr(0, end);
        }
        offset = found + 1;
    }
    return {};
}

struct ParsedUrl final {
    std::string scheme;
    std::string host;
    std::string path;
    std::string query;
};

bool IsValidHostCharacter(const char value) {
    return IsAsciiAlnum(value) || value == '-' || value == '.';
}

std::optional<ParsedUrl> ParseAbsoluteUrl(const std::string_view text) {
    const std::size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) return std::nullopt;
    ParsedUrl url;
    url.scheme.assign(text.substr(0, scheme_end));
    for (std::size_t index = 0; index < url.scheme.size(); ++index) {
        const char value = url.scheme[index];
        const bool valid = index == 0 ? IsAsciiAlpha(value)
                                      : IsAsciiAlnum(value) || value == '+' || value == '-' ||
                                            value == '.';
        if (!valid) return std::nullopt;
        url.scheme[index] = ToLowerAscii(value);
    }

    std::string_view rest = text.substr(scheme_end + 3);
    const std::size_t fragment = rest.find('#');
    if (fragment != std::string_view::npos) rest = rest.substr(0, fragment);

    const std::size_t authority_end = rest.find_first_of("/?");
    const std::string_view authority = rest.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;

    std::string_view host = authority;
    const std::size_t port_separator = authority.rfind(':');
    if (port_separator != std::string_view::npos) {
        const std::string_view port = authority.substr(port_separator + 1);
        host = authority.substr(0, port_separator);
        if (port.empty()) return std::nullopt;
        for (const char value : port) {
            if (!IsAsciiDigit(value)) return std::nullopt;
        }
        const bool default_port = (url.scheme == "https" && port == "443") ||
                                  (url.scheme == "http" && port == "80");
        if (!default_port) return std::nullopt;
    }
    if (host.empty() || host.front() == '.' || host.back() == '.') return std::nullopt;
    for (const char value : host) {
        if (!IsValidHostCharacter(value)) return std::nullopt;
    }
    url.host.assign(host);
    for (char& value : url.host) value = ToLowerAscii(value);

    const std::string_view remainder =
        authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);
    const std::size_t query_start = remainder.find('?');
    std::string_view path = remainder.substr(0, query_start);
    if (query_start != std::string_view::npos) url.query.assign(remainder.substr(query_start + 1));
    if (path.empty()) path = "/";
    for (const char value : path) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte <= 0x20 || byte == 0x7F) return std::nullopt;
    }
    url.path.assign(path);
    for (const char value : url.query) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte <= 0x20 || byte == 0x7F) return std::nullopt;
    }
    return url;
}

bool IsShortHost(const std::string_view host) {
    return host == kShortHost;
}

bool IsVideoHost(const std::string_view host) {
    for (const std::string_view candidate : kVideoHosts) {
        if (host == candidate) return true;
    }
    return false;
}

bool ParseBvIdentifier(const std::string_view text, std::string* bvid) {
    if (text.size() != kBvIdentifierLength || !StartsWithIgnoreCase(text, kBvPrefix)) {
        return false;
    }
    for (std::size_t index = kBvPrefix.size(); index < text.size(); ++index) {
        if (!IsAsciiAlnum(text[index])) return false;
    }
    bvid->assign(kBvPrefix);
    bvid->append(text.substr(kBvPrefix.size()));
    return true;
}

bool ParseAvIdentifier(const std::string_view text, std::uint64_t* aid) {
    if (text.size() <= kAvPrefix.size() || !StartsWithIgnoreCase(text, kAvPrefix)) {
        return false;
    }
    const char* const begin = text.data() + kAvPrefix.size();
    const char* const end = text.data() + text.size();
    std::uint64_t value = 0;
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end || value == 0) return false;
    *aid = value;
    return true;
}

// "?p=2" selects the second page. Anything else (missing, zero, negative,
// malformed, out of range) falls back to P1 instead of failing the link.
int ParsePageValue(const std::string_view text) {
    if (text.empty()) return 1;
    std::uint32_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) return 1;
    if (value == 0 || value > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return 1;
    }
    return static_cast<int>(value);
}

int ParsePageNumber(const std::string_view query) {
    std::string_view rest = query;
    while (!rest.empty()) {
        const std::size_t separator = rest.find('&');
        const std::string_view pair = rest.substr(0, separator);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && EqualsIgnoreCase(pair.substr(0, equals), "p")) {
            return ParsePageValue(pair.substr(equals + 1));
        }
        if (separator == std::string_view::npos) break;
        rest.remove_prefix(separator + 1);
    }
    return 1;
}

BilibiliPage MakeBvPage(std::string bvid, const int page_number) {
    BilibiliPage page;
    page.page = page_number;
    page.canonical_url.assign(kVideoOrigin);
    page.canonical_url.append(bvid);
    if (page_number > 1) {
        page.canonical_url.append("?p=");
        page.canonical_url.append(std::to_string(page_number));
    }
    page.cache_key.assign("bv:");
    page.cache_key.append(bvid);
    page.cache_key.append(":p");
    page.cache_key.append(std::to_string(page_number));
    page.bvid = std::move(bvid);
    return page;
}

BilibiliPage MakeAvPage(const std::uint64_t aid, const int page_number) {
    const std::string identifier = "av" + std::to_string(aid);
    BilibiliPage page;
    page.aid = aid;
    page.page = page_number;
    page.canonical_url.assign(kVideoOrigin);
    page.canonical_url.append(identifier);
    if (page_number > 1) {
        page.canonical_url.append("?p=");
        page.canonical_url.append(std::to_string(page_number));
    }
    page.cache_key.assign("av:");
    page.cache_key.append(std::to_string(aid));
    page.cache_key.append(":p");
    page.cache_key.append(std::to_string(page_number));
    return page;
}

bool ExtractVideoSegment(const std::string_view path, std::string_view* segment) {
    if (!StartsWithIgnoreCase(path, kVideoPathPrefix)) return false;
    std::string_view remainder = path.substr(kVideoPathPrefix.size());
    const std::size_t separator = remainder.find('/');
    if (separator != std::string_view::npos) {
        if (remainder.substr(separator) != "/") return false;
        remainder = remainder.substr(0, separator);
    }
    if (remainder.empty()) return false;
    *segment = remainder;
    return true;
}

BilibiliLinkResult Failure(const BilibiliLinkError error, std::string detail) {
    BilibiliLinkResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

BilibiliLinkResult Resolved(BilibiliPage page) {
    BilibiliLinkResult result;
    result.page = std::move(page);
    return result;
}

BilibiliLinkResult ResolveVideoPage(const ParsedUrl& url) {
    std::string_view segment;
    if (!ExtractVideoSegment(url.path, &segment)) {
        return Failure(BilibiliLinkError::NotVideoPage,
                       "link does not point at a /video/ page");
    }
    const int page_number = ParsePageNumber(url.query);
    std::string bvid;
    if (ParseBvIdentifier(segment, &bvid)) {
        return Resolved(MakeBvPage(std::move(bvid), page_number));
    }
    std::uint64_t aid = 0;
    if (ParseAvIdentifier(segment, &aid)) {
        return Resolved(MakeAvPage(aid, page_number));
    }
    return Failure(BilibiliLinkError::NotVideoPage,
                   "video page path carries no BV or av identifier");
}

BilibiliLinkResult ResolveShortLink(const std::string& start_url, const RedirectFetcher& fetch) {
    std::unordered_set<std::string> visited;
    std::string current = start_url;
    int followed = 0;
    for (;;) {
        const std::optional<ParsedUrl> url = ParseAbsoluteUrl(current);
        if (!url.has_value()) {
            return Failure(BilibiliLinkError::Malformed,
                           "redirect target is not a valid absolute URL");
        }
        if (url->scheme != "https") {
            return Failure(BilibiliLinkError::NotHttps, "redirect target does not use HTTPS");
        }
        if (IsVideoHost(url->host)) return ResolveVideoPage(*url);
        if (!IsShortHost(url->host)) {
            return Failure(BilibiliLinkError::UnsupportedHost,
                           "redirect target host is not allowlisted");
        }

        std::string key = url->host;
        key.append(url->path);
        if (!url->query.empty()) {
            key.push_back('?');
            key.append(url->query);
        }
        if (!visited.insert(std::move(key)).second) {
            return Failure(BilibiliLinkError::RedirectInvalid,
                           "short link redirect loop detected");
        }
        if (followed >= kBilibiliMaxRedirects) {
            return Failure(BilibiliLinkError::RedirectLimit,
                           "short link follows more than five redirects");
        }
        if (!fetch) {
            return Failure(BilibiliLinkError::Network, "short link needs a redirect fetcher");
        }
        const HttpRedirectResult hop = fetch(current);
        if (!hop.ok) {
            return Failure(BilibiliLinkError::Network, "short link request failed");
        }
        if (hop.status < 300 || hop.status >= 400) {
            if (hop.status >= 200 && hop.status < 300) {
                return Failure(BilibiliLinkError::RedirectInvalid,
                               "short link did not return a redirect");
            }
            return Failure(BilibiliLinkError::Network,
                           "short link request returned HTTP " + std::to_string(hop.status));
        }
        if (hop.location.empty()) {
            return Failure(BilibiliLinkError::RedirectInvalid,
                           "redirect response has no Location header");
        }
        current = hop.location;
        ++followed;
    }
}

class WinHttpHandle final {
public:
    explicit WinHttpHandle(const HINTERNET handle) noexcept : handle_(handle) {}
    ~WinHttpHandle() {
        if (handle_ != nullptr) WinHttpCloseHandle(handle_);
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }

private:
    HINTERNET handle_;
};

bool Utf8ToWide(const std::string_view text, std::wstring* wide) {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
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
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
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

}  // namespace

BilibiliLinkResult ParseBilibiliLink(const std::string_view input, const RedirectFetcher& fetch) {
    const std::string_view trimmed = TrimAscii(input);
    if (trimmed.empty()) return Failure(BilibiliLinkError::Empty, "input is empty");

    // A bare identifier may carry a page query ("BV1xx411c7mD?p=2").
    const std::size_t query_start = trimmed.find('?');
    const std::string_view identifier = trimmed.substr(0, query_start);
    const std::string_view query =
        query_start == std::string_view::npos ? std::string_view{} : trimmed.substr(query_start + 1);
    std::string bvid;
    if (ParseBvIdentifier(identifier, &bvid)) {
        return Resolved(MakeBvPage(std::move(bvid), ParsePageNumber(query)));
    }
    std::uint64_t aid = 0;
    if (ParseAvIdentifier(identifier, &aid)) {
        return Resolved(MakeAvPage(aid, ParsePageNumber(query)));
    }

    const std::string_view link = ExtractUrlFromText(trimmed);
    if (link.empty()) {
        return Failure(BilibiliLinkError::UnsupportedInput,
                       "input contains no bilibili video link");
    }
    const std::optional<ParsedUrl> url = ParseAbsoluteUrl(link);
    if (!url.has_value()) {
        return Failure(BilibiliLinkError::Malformed, "link is not a valid absolute URL");
    }
    if (url->scheme != "https") {
        return Failure(BilibiliLinkError::NotHttps, "link does not use HTTPS");
    }
    if (IsShortHost(url->host)) return ResolveShortLink(std::string(link), fetch);
    if (!IsVideoHost(url->host)) {
        return Failure(BilibiliLinkError::UnsupportedHost,
                       "link host is not a bilibili video host");
    }
    return ResolveVideoPage(*url);
}

BilibiliLinkResult ParseBilibiliLink(const std::string_view input) {
    return ParseBilibiliLink(input, RedirectFetcher(&FetchRedirect));
}

bool NeedsRedirectResolution(const std::string_view input) {
    const std::string_view trimmed = TrimAscii(input);
    if (trimmed.empty()) return false;
    const std::string_view link = ExtractUrlFromText(trimmed);
    if (link.empty()) return false;
    const std::optional<ParsedUrl> url = ParseAbsoluteUrl(link);
    return url.has_value() && url->scheme == "https" && IsShortHost(url->host);
}

HttpRedirectResult FetchRedirect(const std::string_view url) {
    HttpRedirectResult result;
    std::wstring wide_url;
    if (!Utf8ToWide(url, &wide_url)) return result;

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        return result;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (host.empty() || path.empty()) return result;

    const WinHttpHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.get() == nullptr) return result;
    WinHttpSetTimeouts(session.get(), kResolveTimeoutMilliseconds, kConnectTimeoutMilliseconds,
                       kSendTimeoutMilliseconds, kReceiveTimeoutMilliseconds);
    // The parser follows every hop itself so it can validate each target; the
    // session must never silently follow a redirect to another host.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                          sizeof(redirect_policy))) {
        return result;
    }

    const WinHttpHandle connection(
        WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (connection.get() == nullptr) return result;

    const WinHttpHandle request(
        WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (request.get() == nullptr) return result;
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return result;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return result;
    }
    result.ok = true;
    result.status = static_cast<int>(status);
    if (status < 300 || status >= 400) return result;

    DWORD location_size = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &location_size, WINHTTP_NO_HEADER_INDEX);
    if (location_size < sizeof(wchar_t)) return result;
    std::wstring location(static_cast<std::size_t>(location_size / sizeof(wchar_t)), L'\0');
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                             location.data(), &location_size, WINHTTP_NO_HEADER_INDEX)) {
        return result;
    }
    location.resize(static_cast<std::size_t>(location_size / sizeof(wchar_t)));
    while (!location.empty() && location.back() == L'\0') location.pop_back();
    WideToUtf8(location, &result.location);
    return result;
}

}  // namespace better_tv_video
