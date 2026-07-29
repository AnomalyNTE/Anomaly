#include "anomaly/plugin_repository_config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <utility>

namespace anomaly {
namespace {

using nlohmann::json;

constexpr std::size_t kMaximumConfigBytes = 256ULL * 1024ULL;

// Reads a string field, tolerating both the native ("url") and Dalamud-style
// ("Url") capitalizations so a pasted Dalamud channel list still loads.
const json* FindField(const json& object, std::string_view lower, std::string_view upper) {
    if (const auto it = object.find(lower); it != object.end()) return &*it;
    if (const auto it = object.find(upper); it != object.end()) return &*it;
    return nullptr;
}

}  // namespace

PluginRepositoryConfigResult ParsePluginRepositoryConfig(std::string_view json_text) {
    PluginRepositoryConfigResult result;

    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        result.message = "plugin repository config is not a JSON object";
        return result;
    }

    if (const auto it = root.find("enabled"); it != root.end() && it->is_boolean()) {
        result.config.enabled = it->get<bool>();
    }
    if (const auto it = root.find("allowInsecureSources");
        it != root.end() && it->is_boolean()) {
        result.config.allow_insecure_sources = it->get<bool>();
    }
    if (const auto it = root.find("repositories"); it != root.end() && it->is_array()) {
        for (const auto& element : *it) {
            PluginRepositoryEntry entry;
            if (element.is_string()) {
                entry.url = element.get<std::string>();
            } else if (element.is_object()) {
                if (const auto* url = FindField(element, "url", "Url"); url && url->is_string()) {
                    entry.url = url->get<std::string>();
                }
                if (const auto* on = FindField(element, "enabled", "IsEnabled");
                    on && on->is_boolean()) {
                    entry.enabled = on->get<bool>();
                }
            } else {
                continue;
            }
            if (!entry.url.empty()) result.config.repositories.push_back(std::move(entry));
        }
    }

    result.ok = true;
    return result;
}

PluginRepositoryConfigResult LoadPluginRepositoryConfig(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        PluginRepositoryConfigResult result;
        result.message = "plugin repository config is missing";
        return result;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    if (text.size() > kMaximumConfigBytes) {
        PluginRepositoryConfigResult result;
        result.message = "plugin repository config is too large";
        return result;
    }
    return ParsePluginRepositoryConfig(text);
}

bool IsPluginRepositoryUriAllowed(
    std::string_view uri, bool allow_insecure_sources) noexcept {
    constexpr std::size_t kMaximumUriBytes = 4096;
    if (uri.empty() || uri.size() > kMaximumUriBytes) return false;
    for (const unsigned char character : uri) {
        if (character <= 0x20 || character == 0x7f) return false;
    }
    if (uri.starts_with("https://")) return uri.size() > std::string_view{"https://"}.size();
    return allow_insecure_sources && uri.starts_with("file://") &&
        uri.size() > std::string_view{"file://"}.size();
}

std::string SerializePluginRepositoryConfig(const PluginRepositoryConfig& config) {
    json root;
    root["schemaVersion"] = 1;
    root["enabled"] = config.enabled;
    root["allowInsecureSources"] = config.allow_insecure_sources;
    auto& repositories = root["repositories"] = json::array();
    for (const auto& entry : config.repositories) {
        if (entry.url.empty()) continue;
        repositories.push_back(json{{"url", entry.url}, {"enabled", entry.enabled}});
    }
    return root.dump(2) + "\n";
}

}  // namespace anomaly
