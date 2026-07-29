#include "anomaly/plugin_list.hpp"

#include "anomaly/semver.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <limits>
#include <utility>

namespace anomaly {
namespace {

using nlohmann::json;

std::string GetString(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it != object.end() && it->is_string()) return it->get<std::string>();
    return {};
}

std::vector<std::string> GetStringArray(const json& object, const char* key) {
    std::vector<std::string> values;
    const auto it = object.find(key);
    if (it != object.end() && it->is_array()) {
        for (const auto& element : *it) {
            if (element.is_string()) values.push_back(element.get<std::string>());
        }
    }
    return values;
}

bool GetOptionalUint32(const json& object, const char* key, std::uint32_t& result) {
    const auto it = object.find(key);
    if (it == object.end()) return true;
    std::uint64_t value{};
    if (it->is_number_unsigned()) {
        value = it->get<std::uint64_t>();
    } else if (it->is_number_integer()) {
        const auto signed_value = it->get<std::int64_t>();
        if (signed_value <= 0) return false;
        value = static_cast<std::uint64_t>(signed_value);
    } else {
        return false;
    }
    if (value > (std::numeric_limits<std::uint32_t>::max)()) return false;
    result = static_cast<std::uint32_t>(value);
    return true;
}

bool QualifiedId(std::string_view value) {
    if (value.size() < 3 || value.size() > 255) return false;
    bool saw_separator{};
    std::size_t segment_size{};
    bool previous_hyphen{};
    for (const char character : value) {
        if (character == '.') {
            if (segment_size == 0 || previous_hyphen) return false;
            saw_separator = true;
            segment_size = 0;
            previous_hyphen = false;
            continue;
        }
        const bool alpha_numeric =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        if (!alpha_numeric && character != '-') return false;
        if (segment_size == 0 && character == '-') return false;
        if (++segment_size > 63) return false;
        previous_hyphen = character == '-';
    }
    return saw_separator && segment_size != 0 && !previous_hyphen;
}

bool GetBool(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

}  // namespace

PluginListParseResult ParsePluginList(std::string_view json_text) {
    PluginListParseResult result;

    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        result.error = PluginListParseError::InvalidJson;
        result.message = "plugin list is not valid JSON";
        return result;
    }
    if (!root.is_array()) {
        result.error = PluginListParseError::NotAnArray;
        result.message = "plugin list root must be a JSON array";
        return result;
    }

    for (const auto& item : root) {
        if (!item.is_object()) {
            ++result.skipped;
            continue;
        }

        PluginListEntry entry;
        entry.name = GetString(item, "Name");
        entry.internal_name = GetString(item, "InternalName");
        entry.version = GetString(item, "Version");
        entry.download_link_install = GetString(item, "DownloadLinkInstall");

        // Skip incomplete entries rather than failing the whole list.
        if (entry.name.empty() || !QualifiedId(entry.internal_name) ||
            !ParseSemanticVersion(entry.version) || entry.download_link_install.empty()) {
            ++result.skipped;
            continue;
        }

        entry.author = GetString(item, "Author");
        entry.punchline = GetString(item, "Punchline");
        entry.description = GetString(item, "Description");
        entry.games = GetStringArray(item, "Games");
        if (!GetOptionalUint32(item, "ApiMajor", entry.api_major)) {
            ++result.skipped;
            continue;
        }
        entry.tags = GetStringArray(item, "Tags");
        entry.repo_url = GetString(item, "RepoUrl");
        entry.icon_url = GetString(item, "IconUrl");
        entry.download_link_update = GetString(item, "DownloadLinkUpdate");
        if (entry.download_link_update.empty()) {
            entry.download_link_update = entry.download_link_install;
        }
        entry.accepts_feedback = GetBool(item, "AcceptsFeedback");

        result.entries.push_back(std::move(entry));
    }

    return result;
}

}  // namespace anomaly
