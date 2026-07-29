#include "plugin_manager.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

const ue5mem::PluginStopDiagnostic* FindDiagnostic(
    const std::vector<ue5mem::PluginStopDiagnostic>& diagnostics,
    std::string_view id) {
    const auto found = std::find_if(
        diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
            return diagnostic.id == id;
        });
    return found == diagnostics.end() ? nullptr : &*found;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    {
        ue5mem::PluginManager manager(root, root);
        manager.LoadAll();
        if (!manager.SetEnabled("anomaly.fixture.stop-deadline.fast", true) ||
            !manager.SetEnabled("anomaly.fixture.stop-deadline.slow", true)) {
            std::cerr << "deadline fixtures could not be explicitly enabled\n";
            return 3;
        }
        const auto active = manager.Plugins();
        if (active.size() != 2) {
            std::cerr << "deadline fixtures did not both activate (count="
                      << active.size() << ")\n";
            for (const auto& plugin : active) {
                std::cerr << "  plugin id=" << plugin.id << " state=" << plugin.state
                          << " reason=" << plugin.status_reason << " source="
                          << plugin.source.string() << "\n";
            }
            for (const auto& event : manager.Events()) {
                std::cerr << "  event: " << event << "\n";
            }
            return 3;
        }

        std::vector<std::pair<std::string, std::uint64_t>> cancelled_generations;
        manager.SetQueuedCallbackCanceller(
            [&cancelled_generations](std::string_view id, std::uint64_t generation) {
                cancelled_generations.emplace_back(id, generation);
            });

        const auto started = std::chrono::steady_clock::now();
        if (manager.StopForRuntime(20ms)) {
            std::cerr << "global stop deadline unexpectedly unloaded every generation\n";
            return 4;
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed > 80ms) {
            std::cerr << "global stop deadline was applied once per generation\n";
            return 5;
        }

        const auto diagnostics = manager.StopDiagnostics();
        const auto* slow = FindDiagnostic(
            diagnostics, "anomaly.fixture.stop-deadline.slow");
        const auto* fast = FindDiagnostic(
            diagnostics, "anomaly.fixture.stop-deadline.fast");
        if (diagnostics.size() != 2 || slow == nullptr || fast == nullptr ||
            slow->reason != "on_stop timed out" || !slow->timed_out ||
            fast->reason != "host stop deadline exceeded" || !fast->timed_out) {
            std::cerr << "global stop deadline diagnostics were incomplete\n";
            return 6;
        }
        if (cancelled_generations.size() != active.size() ||
            std::ranges::any_of(active, [&](const auto& plugin) {
                return std::ranges::none_of(
                    cancelled_generations, [&](const auto& cancelled) {
                        return cancelled.first == plugin.id &&
                            cancelled.second == plugin.generation;
                    });
            })) {
            std::cerr << "global stop did not cancel every queued generation\n";
            return 7;
        }

        const auto quarantined = manager.Plugins();
        if (quarantined.size() != 2 || std::ranges::any_of(
                quarantined, [](const auto& plugin) {
                    return plugin.state != "quarantined" || plugin.enabled || plugin.visible;
                })) {
            std::cerr << "deadline-expired generations did not retain mapped quarantine state\n";
            return 8;
        }

        // Let the detached slow callback leave its lifecycle pin before manager teardown.
        std::this_thread::sleep_for(120ms);
    }
    return 0;
}
