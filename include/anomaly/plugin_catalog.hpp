#pragma once

#include "anomaly/plugin_compatibility.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anomaly {

enum class PluginCatalogStatus : std::uint8_t {
    Valid,
    InvalidManifest,
    InvalidPackage,
    DuplicateId,
    Incompatible,
};

struct PluginCatalogIssue {
    std::string code;
    std::string path;
    std::string message;
};

struct PluginCatalogEntry {
    std::filesystem::path package_root;
    std::filesystem::path manifest_file;
    std::filesystem::path entry_file;
    std::optional<PluginManifest> manifest;
    PluginCatalogStatus status{PluginCatalogStatus::InvalidManifest};
    std::vector<PluginCatalogIssue> issues;
    std::optional<PluginCompatibilityResult> compatibility;

    [[nodiscard]] std::string_view Id() const noexcept {
        return manifest ? std::string_view(manifest->id) : std::string_view{};
    }
    [[nodiscard]] bool LoadCandidate() const noexcept {
        return status == PluginCatalogStatus::Valid;
    }
};

class PluginCatalogSnapshot final {
public:
    explicit PluginCatalogSnapshot(std::vector<PluginCatalogEntry> entries = {});

    [[nodiscard]] const std::vector<PluginCatalogEntry>& Entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const PluginCatalogEntry* Find(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<AvailablePluginVersion> AvailablePlugins() const;

private:
    std::vector<PluginCatalogEntry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};

[[nodiscard]] PluginCatalogSnapshot DiscoverPluginCatalog(
    const std::filesystem::path& plugin_root,
    const std::optional<PluginCompatibilityContext>& compatibility = std::nullopt);

}  // namespace anomaly
