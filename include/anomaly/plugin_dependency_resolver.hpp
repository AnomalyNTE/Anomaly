#pragma once

#include "anomaly/plugin_catalog.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anomaly {

class CompatibilityIndex final {
public:
    CompatibilityIndex(
        const PluginCatalogSnapshot& catalog,
        std::vector<AvailableServiceVersion> services = {});

    [[nodiscard]] const AvailablePluginVersion* Plugin(std::string_view id) const noexcept;
    [[nodiscard]] const AvailableServiceVersion* Service(std::string_view id) const noexcept;
    [[nodiscard]] bool Valid() const noexcept { return duplicate_ids_.empty(); }
    [[nodiscard]] const std::vector<std::string>& DuplicateIds() const noexcept {
        return duplicate_ids_;
    }

private:
    std::vector<AvailablePluginVersion> plugins_;
    std::vector<AvailableServiceVersion> services_;
    std::unordered_map<std::string, std::size_t> plugin_index_;
    std::unordered_map<std::string, std::size_t> service_index_;
    std::vector<std::string> duplicate_ids_;
};

enum class PluginDependencyState : std::uint8_t {
    Ready,
    InvalidCatalog,
    MissingDependency,
    VersionConflict,
    DependencyCycle,
    BlockedTransitively,
};

struct PluginDependencyNode {
    std::string id;
    PluginDependencyState state{PluginDependencyState::Ready};
    std::vector<std::string> diagnostics;
};

struct PluginDependencyPlan {
    std::vector<PluginDependencyNode> nodes;
    std::vector<std::string> load_order;
    std::vector<std::string> stop_order;

    [[nodiscard]] const PluginDependencyNode* Find(std::string_view id) const noexcept;
};

[[nodiscard]] PluginDependencyPlan ResolvePluginDependencies(
    const PluginCatalogSnapshot& catalog);

}  // namespace anomaly
