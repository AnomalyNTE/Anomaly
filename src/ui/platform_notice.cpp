#include "anomaly/platform_notice.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <utility>

namespace anomaly {
namespace {

// Closes a WinHTTP handle on every exit path.
struct HttpHandle final {
    HINTERNET value{};
    explicit HttpHandle(const HINTERNET handle) noexcept : value(handle) {}
    ~HttpHandle() {
        if (value != nullptr) WinHttpCloseHandle(value);
    }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
};

[[nodiscard]] bool IsSpace(const char character) noexcept {
    return character == ' ' || character == '\t';
}

[[nodiscard]] std::string_view TrimLeft(std::string_view value) noexcept {
    while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
    return value;
}

[[nodiscard]] std::string_view TrimRight(std::string_view value) noexcept {
    while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
    return value;
}

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    return TrimRight(TrimLeft(value));
}

[[nodiscard]] bool StartsWith(const std::string_view value, const std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

// A line of dashes/asterisks/underscores is a rule.
[[nodiscard]] bool IsSeparator(const std::string_view line) noexcept {
    if (line.size() < 3) return false;
    const char marker = line.front();
    if (marker != '-' && marker != '*' && marker != '_') return false;
    return std::all_of(line.begin(), line.end(),
        [marker](const char character) { return character == marker || IsSpace(character); });
}

// "1. " / "12) "; the marker keeps the author's numbering.
[[nodiscard]] bool ParseNumberedMarker(
    const std::string_view line, std::string& marker, std::string_view& rest) noexcept {
    std::size_t digits = 0;
    while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') ++digits;
    if (digits == 0 || digits > 3 || digits + 1 >= line.size()) return false;
    const char punctuation = line[digits];
    if (punctuation != '.' && punctuation != ')') return false;
    if (!IsSpace(line[digits + 1])) return false;
    marker.assign(line.substr(0, digits));
    marker.push_back(punctuation);
    rest = TrimLeft(line.substr(digits + 1));
    return true;
}

[[nodiscard]] bool IsHttpUrl(const std::string_view value) noexcept {
    return StartsWith(value, "http://") || StartsWith(value, "https://");
}

// "> [!CAUTION]" and the rest of GitHub's alerts.
[[nodiscard]] NoticeAlert ParseAlert(const std::string_view text) noexcept {
    if (text.size() < 4U || text.front() != '[' || text.back() != ']' || text[1] != '!') {
        return NoticeAlert::None;
    }
    const std::string_view name = text.substr(2, text.size() - 3);
    if (name == "NOTE") return NoticeAlert::Note;
    if (name == "TIP") return NoticeAlert::Tip;
    if (name == "IMPORTANT") return NoticeAlert::Important;
    if (name == "WARNING") return NoticeAlert::Warning;
    if (name == "CAUTION") return NoticeAlert::Caution;
    return NoticeAlert::None;
}

void ReplaceVersion(std::string& text, const std::string_view placeholder,
    const std::string_view version) {
    std::size_t position = 0;
    while ((position = text.find(placeholder, position)) != std::string::npos) {
        text.replace(position, placeholder.size(), version);
        position += version.size();
    }
}

void AppendRun(std::vector<NoticeRun>& runs, const std::string_view text,
    const NoticeRunStyle style, std::string url, const std::string_view version) {
    if (text.empty()) return;
    NoticeRun run;
    run.text = std::string(text);
    run.style = style;
    run.url = std::move(url);
    ReplaceVersion(run.text, "{version}", version);
    ReplaceVersion(run.url, "{version}", version);
    // Merge adjacent plain text into one run.
    if (run.style == NoticeRunStyle::Text && !runs.empty() &&
        runs.back().style == NoticeRunStyle::Text) {
        runs.back().text.append(run.text);
        return;
    }
    runs.push_back(std::move(run));
}

// Inline subset: **emphasis**, `code`, [label](url).
[[nodiscard]] std::vector<NoticeRun> ParseInline(
    const std::string_view line, const std::string_view version) {
    std::vector<NoticeRun> runs;
    std::string pending;
    std::size_t index = 0;
    const auto flush_pending = [&] {
        if (!pending.empty()) {
            AppendRun(runs, pending, NoticeRunStyle::Text, {}, version);
            pending.clear();
        }
    };
    while (index < line.size()) {
        const char character = line[index];
        if (character == '*' && index + 1 < line.size() && line[index + 1] == '*') {
            const std::size_t close = line.find("**", index + 2);
            if (close != std::string_view::npos && close > index + 2) {
                flush_pending();
                AppendRun(runs, line.substr(index + 2, close - index - 2),
                    NoticeRunStyle::Emphasis, {}, version);
                index = close + 2;
                continue;
            }
        }
        if (character == '`') {
            const std::size_t close = line.find('`', index + 1);
            if (close != std::string_view::npos && close > index + 1) {
                flush_pending();
                AppendRun(runs, line.substr(index + 1, close - index - 1),
                    NoticeRunStyle::Code, {}, version);
                index = close + 1;
                continue;
            }
        }
        if (character == '[') {
            const std::size_t label_end = line.find(']', index + 1);
            if (label_end != std::string_view::npos && label_end + 1 < line.size() &&
                line[label_end + 1] == '(') {
                const std::size_t target_end = line.find(')', label_end + 2);
                if (target_end != std::string_view::npos && target_end > label_end + 2) {
                    const std::string_view label = line.substr(index + 1, label_end - index - 1);
                    const std::string_view target =
                        line.substr(label_end + 2, target_end - label_end - 2);
                    flush_pending();
                    AppendRun(runs, label,
                        IsHttpUrl(target) ? NoticeRunStyle::Link : NoticeRunStyle::Text,
                        IsHttpUrl(target) ? std::string(target) : std::string{}, version);
                    index = target_end + 1;
                    continue;
                }
            }
        }
        pending.push_back(character);
        ++index;
    }
    flush_pending();
    return runs;
}

[[nodiscard]] NoticeDocument ParseLines(
    const std::string_view markdown, const std::string_view version) {
    NoticeDocument document;
    std::string paragraph_text;
    std::vector<NoticeRun> item_runs;
    std::string item_marker;
    NoticeAlert alert = NoticeAlert::None;

    const auto flush_paragraph = [&] {
        if (paragraph_text.empty()) return;
        NoticeBlock block;
        block.kind = NoticeBlockKind::Paragraph;
        block.runs = ParseInline(paragraph_text, version);
        block.alert = alert;
        if (!block.runs.empty()) document.blocks.push_back(std::move(block));
        paragraph_text.clear();
    };
    const auto flush_item = [&] {
        if (item_runs.empty()) return;
        NoticeBlock block;
        block.kind = item_marker.empty() ? NoticeBlockKind::Bullet
                                         : NoticeBlockKind::Numbered;
        block.marker = item_marker;
        block.runs = std::move(item_runs);
        block.alert = alert;
        document.blocks.push_back(std::move(block));
        item_runs.clear();
        item_marker.clear();
    };
    const auto flush_all = [&] {
        flush_paragraph();
        flush_item();
    };

    std::size_t offset = 0;
    while (offset <= markdown.size()) {
        const std::size_t newline = markdown.find('\n', offset);
        const std::size_t end = newline == std::string_view::npos ? markdown.size() : newline;
        std::string_view line = markdown.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        offset = newline == std::string_view::npos ? markdown.size() + 1 : newline + 1;

        const std::string_view trimmed = Trim(line);
        // "> " carries quoted prose; a bare ">" is the blank line inside a quote.
        const bool quoted = StartsWith(trimmed, ">");
        const std::string_view body = quoted
            ? TrimLeft(trimmed.size() > 1 ? trimmed.substr(1) : std::string_view{})
            : trimmed;
        if (body.empty()) {
            flush_all();
            continue;
        }
        if (const NoticeAlert marker = ParseAlert(body); marker != NoticeAlert::None) {
            flush_all();
            alert = marker;
            continue;
        }
        if (!quoted) alert = NoticeAlert::None;
        if (IsSeparator(body)) {
            flush_all();
            NoticeBlock block;
            block.kind = NoticeBlockKind::Separator;
            block.alert = alert;
            document.blocks.push_back(std::move(block));
            continue;
        }
        if (const std::string_view heading = StartsWith(body, "# ")
                ? TrimLeft(body.substr(1)) : std::string_view{};
            !heading.empty()) {
            flush_all();
            NoticeBlock block;
            block.kind = NoticeBlockKind::Heading;
            block.runs = ParseInline(heading, version);
            block.alert = alert;
            if (!block.runs.empty()) document.blocks.push_back(std::move(block));
            continue;
        }
        if (StartsWith(body, "## ") || StartsWith(body, "### ")) {
            flush_all();
            const std::size_t hashes = StartsWith(body, "### ") ? 3U : 2U;
            NoticeBlock block;
            block.kind = NoticeBlockKind::Subheading;
            block.runs = ParseInline(TrimLeft(body.substr(hashes)), version);
            block.alert = alert;
            if (!block.runs.empty()) document.blocks.push_back(std::move(block));
            continue;
        }
        if (StartsWith(body, "- ") || StartsWith(body, "* ")) {
            flush_all();
            item_runs = ParseInline(TrimLeft(body.substr(1)), version);
            continue;
        }
        std::string marker;
        std::string_view rest;
        if (ParseNumberedMarker(body, marker, rest)) {
            flush_all();
            item_marker = std::move(marker);
            item_runs = ParseInline(rest, version);
            continue;
        }
        // Hard breaks join with a space; the card wraps to its own width.
        if (!paragraph_text.empty()) paragraph_text.push_back(' ');
        paragraph_text.append(body);
    }
    flush_all();
    return document;
}

}  // namespace

NoticeDocument ParseNoticeMarkdown(
    const std::string_view markdown, const std::uint32_t version) {
    std::string_view text = markdown;
    // Drop a BOM, which Notepad writes.
    if (StartsWith(text, "\xEF\xBB\xBF")) text.remove_prefix(3);
    const std::string version_text = std::to_string(version);
    return ParseLines(text, version_text);
}

std::optional<NoticeDocument> LoadNoticeMarkdown(
    const std::filesystem::path& path, const std::uint32_t version) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kNoticeDocumentMaximumBytes) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) return std::nullopt;
    return ParseNoticeMarkdown(buffer.str(), version);
}


std::string BuildNoticeRemoteUrl(
    const std::string_view document, const std::string_view locale_name) {
    std::string url(kNoticeRemoteBaseUrl);
    url.append(document);
    url.push_back('.');
    url.append(locale_name.empty() ? std::string_view{"en-US"} : locale_name);
    url.append(".md");
    return url;
}

std::optional<NoticeDocument> DownloadNoticeDocument(
    const std::string_view document, const std::string_view locale_name,
    const std::uint32_t version, const std::stop_token stop_token) {
    const std::string url = BuildNoticeRemoteUrl(document, locale_name);
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
        static_cast<int>(url.size()), nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
        static_cast<int>(url.size()), wide.data(), required);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = DWORD(-1);
    components.dwHostNameLength = DWORD(-1);
    components.dwUrlPathLength = DWORD(-1);
    components.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        return std::nullopt;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    HttpHandle session(WinHttpOpen(L"Anomaly/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
    HttpHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connection) return std::nullopt;
    HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return std::nullopt;
    }
    DWORD status{};
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        return std::nullopt;
    }
    std::string body;
    body.reserve(4096);
    while (!stop_token.stop_requested()) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request.get(), &available)) return std::nullopt;
        if (available == 0) break;
        const std::size_t offset = body.size();
        if (offset + available > kNoticeDocumentMaximumBytes) return std::nullopt;
        body.resize(offset + available);
        DWORD read{};
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &read)) {
            return std::nullopt;
        }
        body.resize(offset + read);
        if (read == 0) break;
    }
    if (stop_token.stop_requested() || body.empty()) return std::nullopt;
    return ParseNoticeMarkdown(body, version);
}

std::filesystem::path NoticeDocumentPath(
    const std::filesystem::path& root, const std::string_view document,
    const std::string_view locale_name) {
    std::string file(document);
    file.push_back('.');
    file.append(locale_name.empty() ? std::string_view{"en-US"} : locale_name);
    file.append(".md");
    return root / std::filesystem::path(kNoticeDocumentDirectory) / file;
}

std::optional<NoticeDocument> LoadNoticeDocument(
    const std::filesystem::path& root, const std::string_view document,
    const std::string_view locale_name, const std::uint32_t version) {
    if (auto localized =
            LoadNoticeMarkdown(NoticeDocumentPath(root, document, locale_name), version);
        localized.has_value()) {
        return localized;
    }
    return LoadNoticeMarkdown(NoticeDocumentPath(root, document, "en-US"), version);
}

}  // namespace anomaly
