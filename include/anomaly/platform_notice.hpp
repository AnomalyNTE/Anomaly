#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace anomaly {

// The first-launch notice is authored as Markdown and rendered by the shell.
// Only the subset below is understood.
enum class NoticeBlockKind : std::uint8_t {
    // "# text".
    Heading,
    // "## text" and "### text".
    Subheading,
    // Consecutive plain lines, joined.
    Paragraph,
    // "- text" or "* text".
    Bullet,
    // "1. text"; the number is kept in NoticeBlock::marker.
    Numbered,
    // "---", "***" or "___".
    Separator,
};

enum class NoticeRunStyle : std::uint8_t {
    Text,
    // **text**
    Emphasis,
    // `text`
    Code,
    // [text](url); only http(s) targets stay clickable.
    Link,
};

struct NoticeRun final {
    std::string text;
    NoticeRunStyle style{NoticeRunStyle::Text};
    std::string url;
};

// GitHub-style alert severity, from a "> [!CAUTION]" marker line.
enum class NoticeAlert : std::uint8_t {
    None,
    Note,
    Tip,
    Important,
    Warning,
    Caution,
};

struct NoticeBlock final {
    NoticeBlockKind kind{NoticeBlockKind::Paragraph};
    std::vector<NoticeRun> runs;
    // Set by Numbered, e.g. "1.".
    std::string marker;
    // Set for every line that belongs to an alert, so the card can draw them as
    // one callout.
    NoticeAlert alert{NoticeAlert::None};
};

struct NoticeDocument final {
    std::vector<NoticeBlock> blocks;
};

// `version` fills every "{version}" placeholder.
[[nodiscard]] NoticeDocument ParseNoticeMarkdown(
    std::string_view markdown, std::uint32_t version);

// Nothing when the file is unreadable or over the size limit.
[[nodiscard]] std::optional<NoticeDocument> LoadNoticeMarkdown(
    const std::filesystem::path& path, std::uint32_t version);

// Localized document, then English; nothing when neither exists.
[[nodiscard]] std::optional<NoticeDocument> LoadNoticeDocument(
    const std::filesystem::path& root, std::string_view document,
    std::string_view locale_name, std::uint32_t version);

[[nodiscard]] std::filesystem::path NoticeDocumentPath(
    const std::filesystem::path& root, std::string_view document,
    std::string_view locale_name);

// The published copy of a document, fetched over HTTPS. The shell prefers it so
// a notice can be corrected without shipping a build; the local file stays the
// fallback when the fetch fails.
[[nodiscard]] std::string BuildNoticeRemoteUrl(
    std::string_view document, std::string_view locale_name);
[[nodiscard]] std::optional<NoticeDocument> DownloadNoticeDocument(
    std::string_view document, std::string_view locale_name, std::uint32_t version,
    std::stop_token stop_token = {});

inline constexpr std::string_view kNoticeDocumentDirectory = "notices";
inline constexpr std::string_view kFirstRunNoticeDocument = "first-run";
inline constexpr std::string_view kNoticeRemoteBaseUrl =
    "https://raw.githubusercontent.com/AnomalyNTE/Anomaly/refs/heads/main/notices/";
// Upper bound on what the shell will parse.
inline constexpr std::size_t kNoticeDocumentMaximumBytes = 64U * 1024U;

}  // namespace anomaly
