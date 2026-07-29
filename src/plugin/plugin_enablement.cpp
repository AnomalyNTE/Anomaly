#include "anomaly/plugin_enablement.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace anomaly {
namespace {

bool LoadPluginStates(
    const nlohmann::json& json, PluginEnablementState& state, std::string* error) {
    if (!json.is_object() || json.size() > 10000) {
        if (error) *error = "plugin enablement map is invalid";
        return false;
    }
    for (auto iterator = json.begin(); iterator != json.end(); ++iterator) {
        if (!iterator.value().is_boolean() || iterator.key().empty() ||
            iterator.key().size() > 255) {
            if (error) *error = "plugin enablement entry is invalid";
            return false;
        }
        state.plugins.emplace(iterator.key(), iterator.value().get<bool>());
    }
    return true;
}

void SetDependencies(
    const PluginCatalogSnapshot& catalog, PluginEnablementState& state,
    std::string_view id, std::set<std::string>& visited) {
    if (!visited.insert(std::string(id)).second) return;
    state.plugins[std::string(id)] = true;
    const auto* entry = catalog.Find(id);
    if (entry == nullptr || !entry->manifest) return;
    for (const auto& dependency : entry->manifest->dependencies) {
        if (!dependency.optional) SetDependencies(catalog, state, dependency.id, visited);
    }
}

void DisableDependents(
    const PluginCatalogSnapshot& catalog, PluginEnablementState& state,
    std::string_view id, std::set<std::string>& visited) {
    if (!visited.insert(std::string(id)).second) return;
    state.plugins[std::string(id)] = false;
    for (const auto& candidate : catalog.Entries()) {
        if (!candidate.manifest) continue;
        const bool depends = std::any_of(
            candidate.manifest->dependencies.begin(), candidate.manifest->dependencies.end(),
            [&](const auto& dependency) { return !dependency.optional && dependency.id == id; });
        if (depends) DisableDependents(catalog, state, candidate.manifest->id, visited);
    }
}

}  // namespace

PluginEnablementStore::PluginEnablementStore(std::filesystem::path file)
    : file_(std::move(file)) {}

bool PluginEnablementStore::Load(std::string* error) {
    try {
        std::error_code file_error;
        if (!std::filesystem::exists(file_, file_error)) {
            state_ = {};
            return Save(error);
        }
        if (std::filesystem::file_size(file_, file_error) > 1024U * 1024U || file_error) {
            if (error) *error = "plugin enablement file is too large";
            return false;
        }
        std::ifstream input(file_, std::ios::binary);
        const nlohmann::json json = nlohmann::json::parse(input);
        if (!json.is_object() || json.size() != 3 ||
            !json.contains("schemaVersion") || json.at("schemaVersion") != 1 ||
            !json.contains("defaultEnabled") || !json.at("defaultEnabled").is_boolean() ||
            !json.contains("plugins")) {
            if (error) *error = "plugin enablement document is invalid";
            return false;
        }
        PluginEnablementState loaded;
        loaded.default_enabled = json.at("defaultEnabled").get<bool>();
        if (!LoadPluginStates(json.at("plugins"), loaded, error)) return false;
        state_ = std::move(loaded);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool PluginEnablementStore::Save(std::string* error) const {
    try {
        nlohmann::json plugins = nlohmann::json::object();
        for (const auto& [id, enabled] : state_.plugins) plugins[id] = enabled;
        nlohmann::json json{{"schemaVersion", 1},
            {"defaultEnabled", state_.default_enabled}, {"plugins", std::move(plugins)}};
        std::error_code file_error;
        std::filesystem::create_directories(file_.parent_path(), file_error);
        if (file_error) throw std::runtime_error("enablement directory create failed");
        const auto temporary = file_.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << json.dump(2) << '\n';
        output.flush();
        output.close();
        if (!output) throw std::runtime_error("plugin enablement write failed");
        std::filesystem::remove(file_, file_error);
        file_error.clear();
        std::filesystem::rename(temporary, file_, file_error);
        if (file_error) throw std::runtime_error("plugin enablement publish failed");
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool PluginEnablementStore::SetPluginEnabled(
    const PluginCatalogSnapshot& catalog, std::string_view plugin_id,
    bool enabled, std::string* error) {
    if (catalog.Find(plugin_id) == nullptr) {
        if (error) *error = "plugin is not present in the catalog";
        return false;
    }
    std::set<std::string> visited;
    if (enabled) SetDependencies(catalog, state_, plugin_id, visited);
    else DisableDependents(catalog, state_, plugin_id, visited);
    return Save(error);
}

std::map<std::string, PluginEnablementDecision, std::less<>>
PluginEnablementStore::Resolve(const PluginCatalogSnapshot& catalog) const {
    std::map<std::string, PluginEnablementDecision, std::less<>> decisions;
    for (const auto& entry : catalog.Entries()) {
        if (!entry.manifest) continue;
        const auto configured = state_.plugins.find(entry.manifest->id);
        decisions[entry.manifest->id] = {
            configured == state_.plugins.end() ? state_.default_enabled : configured->second,
            configured != state_.plugins.end(),
            configured == state_.plugins.end() ? "enablement default" : "enablement override"};
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& entry : catalog.Entries()) {
            if (!entry.manifest) continue;
            auto& decision = decisions[entry.manifest->id];
            if (!decision.enabled) continue;
            for (const auto& dependency : entry.manifest->dependencies) {
                if (dependency.optional) continue;
                const auto dependency_decision = decisions.find(dependency.id);
                if (dependency_decision == decisions.end()) continue;
                const auto configured = state_.plugins.find(dependency.id);
                if (configured != state_.plugins.end() && !configured->second) {
                    decision.enabled = false;
                    decision.reason = "disabled because dependency " + dependency.id + " is disabled";
                    changed = true;
                    break;
                }
                if (!dependency_decision->second.enabled) {
                    dependency_decision->second.enabled = true;
                    dependency_decision->second.reason =
                        "enabled as dependency of " + entry.manifest->id;
                    changed = true;
                }
            }
        }
    }
    return decisions;
}

}  // namespace anomaly
