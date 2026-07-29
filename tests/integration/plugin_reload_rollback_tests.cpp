#include "plugin_manager.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path source_package(argv[1]);
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-reload-rollback-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path package = root / L"plugins" / L"PluginManagerFixture";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(package, error);
    std::filesystem::copy(
        source_package, package,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        error);
    if (error) return 3;

    bool result{};
    {
        ue5mem::PluginManager manager(root, L"plugins");
        AnomalyUiServiceV1 ui{};
        ui.struct_size = sizeof(ui);
        ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
        manager.SetUiService(&ui);
        manager.LoadAll();
        if (!manager.SetEnabled("anomaly.test.plugin-manager-fixture", true)) return 4;
        const auto initial = manager.Plugins();
        if (initial.size() != 1 || initial.front().id != "anomaly.test.plugin-manager-fixture" ||
            initial.front().state != "active") {
            return 4;
        }
        const std::uint64_t stable_generation = initial.front().generation;

        std::ofstream(package / L"plugin.dll", std::ios::binary | std::ios::trunc)
            << "invalid replacement";
        const bool reload_failed = !manager.Reload(
            "anomaly.test.plugin-manager-fixture");

        const auto restored = manager.Plugins();
        const bool rollback_event = std::ranges::any_of(
            manager.Events(), [](const std::string& event) {
                return event.find(
                    "plugin package reload rolled back: PluginManagerFixture") !=
                    std::string::npos;
            });
        result = reload_failed && restored.size() == 1 &&
            restored.front().id == "anomaly.test.plugin-manager-fixture" &&
            restored.front().state == "active" &&
            restored.front().generation == stable_generation && rollback_event;
        manager.UnloadAll();
    }
    std::filesystem::remove_all(root, error);
    if (!result) {
        std::cerr << "failed plugin generation did not restore the previous shadow\n";
        return 1;
    }
    return 0;
}
