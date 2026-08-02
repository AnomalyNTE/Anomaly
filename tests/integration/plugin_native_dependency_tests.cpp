#include "plugin_manager.hpp"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kFirstPluginId = "anomaly.fixture.native-dependency";
constexpr std::string_view kConflictPluginId =
    "anomaly.fixture.native-dependency-conflict";

bool Check(bool condition, std::string_view message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool WritePackage(
    const std::filesystem::path& package,
    const std::filesystem::path& plugin,
    const std::filesystem::path& dependency,
    std::string_view id) {
    std::error_code error;
    std::filesystem::create_directories(package, error);
    if (error || !std::filesystem::copy_file(
            plugin, package / L"plugin.dll",
            std::filesystem::copy_options::overwrite_existing, error) || error) {
        return false;
    }
    error.clear();
    if (!std::filesystem::copy_file(
            dependency, package / dependency.filename(),
            std::filesystem::copy_options::overwrite_existing, error) || error) {
        return false;
    }
    std::ofstream manifest(package / L"manifest.json", std::ios::binary | std::ios::trunc);
    manifest << R"({"schemaVersion":2,"id":")" << id
             << R"(","name":"Native Dependency Fixture","author":"Anomaly","version":"1.0.0",)"
                R"("entry":"plugin.dll","api":{"major":1,"minMinor":0,"maxMinor":0},)"
                 R"("games":["nte"],"builds":["nte-win64-*"],"loadPhase":"game-ready",)"
                 R"("capabilities":[]})";
    return static_cast<bool>(manifest);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-native-dependency-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const std::filesystem::path plugins = root / L"plugins";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (!WritePackage(plugins / L"A-First", argv[1], argv[2], kFirstPluginId) ||
        !WritePackage(plugins / L"B-Conflict", argv[1], argv[2], kConflictPluginId)) {
        std::filesystem::remove_all(root, cleanup_error);
        return 3;
    }

    bool result{};
    {
        ue5mem::PluginManager manager(root, plugins);
        manager.LoadAll();
        const bool first_enabled = manager.SetEnabled(kFirstPluginId, true);
        const bool conflict_enabled = manager.SetEnabled(kConflictPluginId, true);
        const auto plugins_after_load = manager.Plugins();
        const auto first = std::ranges::find_if(
            plugins_after_load, [](const auto& plugin) { return plugin.id == kFirstPluginId; });
        const auto conflict = std::ranges::find_if(
            plugins_after_load, [](const auto& plugin) { return plugin.id == kConflictPluginId; });
        const bool first_loaded = first_enabled && first != plugins_after_load.end() &&
            first->enabled && first->state == "active";
        const bool conflict_rejected = !conflict_enabled && conflict != plugins_after_load.end() &&
            !conflict->enabled && conflict->state == "faulted";
        const bool conflict_reported = std::ranges::any_of(
            manager.Events(), [](const std::string& event) {
                return event.find("plugin native dependency denied: package=") !=
                        std::string::npos &&
                    event.find("B-Conflict") != std::string::npos &&
                    event.find("module=anomaly_fixture_private_dependency.dll") !=
                        std::string::npos &&
                    event.find("code=module-name-conflict") != std::string::npos &&
                    event.find("loaded=") != std::string::npos;
            });
        result = Check(first_loaded, "first package did not activate") &&
            Check(conflict_rejected, "conflicting package did not remain faulted") &&
            Check(
                conflict_reported,
                "same-name private DLL conflict was not rejected before activation");
        manager.UnloadAll();
    }

    std::filesystem::remove_all(root, cleanup_error);
    return result ? 0 : 1;
}
