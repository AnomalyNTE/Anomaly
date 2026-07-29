#pragma once

#include "anomaly/plugin_catalog.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace anomaly {

struct PluginEnablementState {
    bool default_enabled{};
    std::map<std::string, bool, std::less<>> plugins;
};

struct PluginEnablementDecision {
    bool enabled{};
    bool configured{};
    std::string reason;
};

class PluginEnablementStore final {
public:
    explicit PluginEnablementStore(std::filesystem::path file);

    [[nodiscard]] bool Load(std::string* error = nullptr);
    [[nodiscard]] bool Save(std::string* error = nullptr) const;
    [[nodiscard]] const PluginEnablementState& State() const noexcept { return state_; }
    [[nodiscard]] bool SetPluginEnabled(
        const PluginCatalogSnapshot& catalog, std::string_view plugin_id,
        bool enabled, std::string* error = nullptr);
    [[nodiscard]] std::map<std::string, PluginEnablementDecision, std::less<>> Resolve(
        const PluginCatalogSnapshot& catalog) const;

private:
    std::filesystem::path file_;
    PluginEnablementState state_;
};

}  // namespace anomaly
