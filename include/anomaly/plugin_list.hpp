#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

// A single entry in a Dalamud-style flat plugin repository list. The list is a
// JSON array of these objects, served as-is over HTTPS (e.g. from
// raw.githubusercontent.com). There is no signing: trust is established by the
// user choosing which repository URLs to enable.
struct PluginListEntry {
    std::string name;                  // display name ("Name")
    std::string internal_name;         // == plugin manifest id ("InternalName")
    std::string version;               // semantic version ("Version")
    std::string author;                // ("Author")
    std::string punchline;             // one-line summary ("Punchline")
    std::string description;           // ("Description")
    std::vector<std::string> games;    // Anomaly game ids ("Games"), e.g. ["nte"]
    std::uint32_t api_major{};         // required Anomaly ABI major ("ApiMajor")
    std::vector<std::string> tags;     // ("Tags")
    std::string repo_url;              // ("RepoUrl")
    std::string icon_url;              // ("IconUrl")
    std::string download_link_install; // zip download URL ("DownloadLinkInstall")
    std::string download_link_update;  // ("DownloadLinkUpdate"); falls back to install
    bool accepts_feedback{};           // ("AcceptsFeedback")
};

enum class PluginListParseError : std::uint8_t {
    None,
    InvalidJson,
    NotAnArray,
};

struct PluginListParseResult {
    PluginListParseError error{PluginListParseError::None};
    std::string message;
    std::vector<PluginListEntry> entries;
    std::uint32_t skipped{};  // objects dropped for missing required fields

    [[nodiscard]] bool Ok() const noexcept {
        return error == PluginListParseError::None;
    }
};

// Parses a plugin repository list. Tolerant like Dalamud: non-object elements
// and entries missing a required field (Name / InternalName / Version /
// DownloadLinkInstall) are skipped (counted in `skipped`) rather than failing
// the whole list. A malformed document or a non-array root is a hard error.
[[nodiscard]] PluginListParseResult ParsePluginList(std::string_view json_text);

}  // namespace anomaly
