#include "plugin_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root(argv[1]);
    ue5mem::PluginManager manager(root, root);
    manager.LoadAll();
    if (!manager.SetEnabled("anomaly.fixture.faulting", true)) return 3;
    if (manager.Plugins().size() != 1) return 3;

    manager.GameUpdate(1.0 / 60.0);
    manager.GameUpdate(1.0 / 60.0);
    manager.GameUpdate(1.0 / 60.0);

    const auto plugins = manager.Plugins();
    if (plugins.size() != 1 || plugins.front().state != "faulted" ||
        plugins.front().update_metrics.calls != 1 ||
        plugins.front().update_metrics.faults != 1 ||
        plugins.front().status_reason.find("Update callback") == std::string::npos) {
        std::cerr << "faulting callback was not isolated after its first exception\n";
        return 1;
    }
    const auto events = manager.Events();
    const auto fault_logs = static_cast<std::size_t>(std::count_if(
        events.begin(), events.end(), [](const std::string& event) {
            return event.find("exception in ABI v1 update: anomaly.fixture.faulting") !=
                std::string::npos;
        }));
    if (fault_logs != 1) {
        std::cerr << "faulting callback emitted repeated frame logs\n";
        return 1;
    }
    manager.UnloadAll();
    return 0;
}
