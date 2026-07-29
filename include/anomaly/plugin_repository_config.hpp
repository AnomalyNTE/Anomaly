#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

// One third-party plugin channel: a URL serving a flat pluginmaster.json list,
// plus a per-channel enable toggle (Dalamud-style). A disabled channel is kept
// in the configuration but neither fetched nor offered in the catalog.
struct PluginRepositoryEntry {
    std::string url;      // pluginmaster.json URL
    bool enabled{true};

    friend bool operator==(const PluginRepositoryEntry&, const PluginRepositoryEntry&) = default;
};

// Configuration for the Dalamud-style plugin channels: a list of channels, each
// serving a flat JSON array of PluginListEntry (a "pluginmaster.json"). There is
// no signing; trust comes from the user choosing which channels to enable.
struct PluginRepositoryConfig {
    bool enabled{true};                             // master switch for third-party channels
    std::vector<PluginRepositoryEntry> repositories;
    bool allow_insecure_sources{};                  // permit file:// URLs for controlled testing

    friend bool operator==(const PluginRepositoryConfig&, const PluginRepositoryConfig&) = default;
};

struct PluginRepositoryConfigResult {
    bool ok{};
    std::string message;
    PluginRepositoryConfig config;
};

[[nodiscard]] PluginRepositoryConfigResult ParsePluginRepositoryConfig(std::string_view json_text);
[[nodiscard]] PluginRepositoryConfigResult LoadPluginRepositoryConfig(
    const std::filesystem::path& path);

// Production channels and packages must use HTTPS. file:// is available only
// when the configuration explicitly opts into controlled local fixtures.
[[nodiscard]] bool IsPluginRepositoryUriAllowed(
    std::string_view uri, bool allow_insecure_sources) noexcept;

// Serializes the configuration back to the on-disk JSON form ("schemaVersion",
// "enabled", "allowInsecureSources", "repositories":[{ "url", "enabled" }]).
[[nodiscard]] std::string SerializePluginRepositoryConfig(const PluginRepositoryConfig& config);

}  // namespace anomaly
