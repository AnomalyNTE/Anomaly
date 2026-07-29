#pragma once

#include "anomaly/plugin_catalog.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace anomaly {

struct PluginShadowGeneration {
    std::string plugin_id;
    std::uint64_t generation{};
    std::filesystem::path package_root;
    std::filesystem::path entry_file;
    PluginManifest manifest;
};

struct PluginShadowResult {
    std::optional<PluginShadowGeneration> shadow;
    std::string error;

    [[nodiscard]] bool Ok() const noexcept { return shadow.has_value(); }
};

class PluginShadowStore final {
public:
    explicit PluginShadowStore(std::filesystem::path root);

    [[nodiscard]] PluginShadowResult Stage(const PluginCatalogEntry& entry);
    void Retire(const PluginShadowGeneration& generation) noexcept;
    void CleanupPluginExcept(
        std::string_view plugin_id, std::optional<std::uint64_t> keep_generation) noexcept;
    void Cleanup() noexcept;

    [[nodiscard]] const std::filesystem::path& Root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    std::uint64_t next_generation_{1};
};

}  // namespace anomaly
