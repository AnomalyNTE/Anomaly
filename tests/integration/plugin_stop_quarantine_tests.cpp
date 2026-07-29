#include "plugin_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root(argv[1]);
    {
        ue5mem::PluginManager manager(root, root);
        manager.LoadAll();
        if (!manager.SetEnabled("anomaly.fixture.stop-timeout", true)) return 3;
        const auto active = manager.Plugins();
        if (active.size() != 1 || active.front().state != "active") return 3;
        const std::string plugin_id = active.front().id;
        const std::uint64_t generation = active.front().generation;
        auto callback = manager.AcquireCallback(plugin_id, generation);
        if (!callback) {
            std::cerr << "active generation did not grant a callback lease\n";
            return 4;
        }
        callback = {};

        manager.UnloadAll();
        if (manager.AcquireCallback(plugin_id, generation)) {
            std::cerr << "stopped generation still granted a callback lease\n";
            return 5;
        }
        const auto quarantined = manager.Plugins();
        if (quarantined.size() != 1 || quarantined.front().state != "quarantined" ||
            quarantined.front().enabled || quarantined.front().visible ||
            quarantined.front().status_reason != "on_stop status=6") {
            std::cerr << "stop timeout did not retain the mapped generation in quarantine\n";
            return 1;
        }
        const auto diagnostics = manager.StopDiagnostics();
        if (diagnostics.size() != 1 || diagnostics.front().id != plugin_id ||
            diagnostics.front().generation != generation || !diagnostics.front().drained ||
            diagnostics.front().resources == 0 ||
            diagnostics.front().reason != "on_stop status=6") {
            std::cerr << "stop diagnostics did not retain generation drain evidence\n";
            return 6;
        }
        const auto events = manager.Events();
        const auto quarantine_logs = static_cast<std::size_t>(std::count_if(
            events.begin(), events.end(), [](const std::string& event) {
                return event.find(
                    "plugin quarantined after stop failure: anomaly.fixture.stop-timeout "
                    "on_stop status=6") != std::string::npos;
            }));
        if (quarantine_logs != 1) {
            std::cerr << "stop timeout did not emit one quarantine event\n";
            return 1;
        }
        manager.LoadAll();
        if (manager.Plugins().size() != 1) {
            std::cerr << "quarantined plugin ID was loaded a second time\n";
            return 1;
        }
    }
    bool restart_blocked{};
    try {
        ue5mem::PluginManager unsafe_restart(root, root);
    } catch (const std::logic_error&) {
        restart_blocked = true;
    }
    if (!restart_blocked) return 1;
    return 0;
}
