#include "plugin_manager.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

const ue5mem::PluginView* Find(
    const std::vector<ue5mem::PluginView>& plugins, const std::string_view id) {
    const auto found = std::ranges::find_if(
        plugins, [&](const ue5mem::PluginView& plugin) { return plugin.id == id; });
    return found == plugins.end() ? nullptr : &*found;
}

const anomaly::IpcEndpointDiagnostics* Endpoint(
    const ue5mem::PluginView& plugin, const std::string_view id) {
    const auto& endpoints = plugin.platform_diagnostics.ipc_endpoints;
    const auto found = std::ranges::find_if(
        endpoints, [&](const anomaly::IpcEndpointDiagnostics& endpoint) {
            return endpoint.id == id;
        });
    return found == endpoints.end() ? nullptr : &*found;
}

bool Active(const ue5mem::PluginView* plugin) {
    return plugin != nullptr && plugin->state == "active" && plugin->generation != 0;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixture_root(argv[1]);
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-ipc-plugin-integration-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path plugins_root = root / L"plugins";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(plugins_root, error);
    if (error) return 3;
    std::filesystem::copy(
        fixture_root, plugins_root,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        error);
    if (error) return 4;

    int result = 0;
    {
        ue5mem::PluginManager manager(root, L"plugins");
        manager.LoadAll();
        for (const auto& plugin : manager.Plugins()) {
            static_cast<void>(manager.SetEnabled(plugin.id, true));
        }
        const auto initial = manager.DiagnosticsSnapshot().plugins;
        const auto* c_provider = Find(initial, "dev.anomaly.ipc-c-provider");
        const auto* cpp_consumer = Find(initial, "dev.anomaly.ipc-cpp-consumer");
        const auto* cpp_provider = Find(initial, "dev.anomaly.ipc-cpp-provider");
        const auto* c_consumer = Find(initial, "dev.anomaly.ipc-c-consumer");
        if (initial.size() != 4 || !Active(c_provider) || !Active(cpp_consumer) ||
            !Active(cpp_provider) || !Active(c_consumer)) {
            std::cerr << "C/C++ IPC fixture packages did not activate\n";
            for (const std::string& event : manager.Events()) std::cerr << event << '\n';
            result = 5;
        } else {
            const auto* c_endpoint = Endpoint(*c_provider, "dev.anomaly.ipc.c-provider");
            const auto* cpp_endpoint = Endpoint(*cpp_provider, "dev.anomaly.ipc.cpp-provider");
            if (c_endpoint == nullptr || cpp_endpoint == nullptr || c_endpoint->calls != 1 ||
                cpp_endpoint->calls != 1 || c_endpoint->consumers !=
                    std::vector<std::string>{"dev.anomaly.ipc-cpp-consumer"} ||
                cpp_endpoint->consumers !=
                    std::vector<std::string>{"dev.anomaly.ipc-c-consumer"} ||
                c_endpoint->request_schema_hash.size() != 64 ||
                cpp_endpoint->response_schema_hash.size() != 64) {
                std::cerr << "IPC endpoint diagnostics omitted ABI-boundary calls\n";
                result = 6;
            }
        }

        if (result == 0) {
            const std::uint64_t c_provider_generation = c_provider->generation;
            const std::uint64_t cpp_consumer_generation = cpp_consumer->generation;
            const std::uint64_t cpp_provider_generation = cpp_provider->generation;
            const std::uint64_t c_consumer_generation = c_consumer->generation;
            const std::filesystem::path manifest =
                plugins_root / L"CProvider" / L"manifest.json";
            std::ofstream(manifest, std::ios::binary | std::ios::app) << ' ';
            manager.MaintenancePluginState();
            std::this_thread::sleep_for(800ms);
            manager.MaintenancePluginState();

            const auto reloaded = manager.DiagnosticsSnapshot().plugins;
            c_provider = Find(reloaded, "dev.anomaly.ipc-c-provider");
            cpp_consumer = Find(reloaded, "dev.anomaly.ipc-cpp-consumer");
            cpp_provider = Find(reloaded, "dev.anomaly.ipc-cpp-provider");
            c_consumer = Find(reloaded, "dev.anomaly.ipc-c-consumer");
            const auto* endpoint = c_provider == nullptr
                ? nullptr : Endpoint(*c_provider, "dev.anomaly.ipc.c-provider");
            if (!Active(c_provider) || !Active(cpp_consumer) || !Active(cpp_provider) ||
                !Active(c_consumer) || c_provider->generation <= c_provider_generation ||
                cpp_consumer->generation <= cpp_consumer_generation ||
                cpp_provider->generation != cpp_provider_generation ||
                c_consumer->generation != c_consumer_generation || endpoint == nullptr ||
                endpoint->generation != c_provider->generation || endpoint->calls != 1) {
                std::cerr << "provider reload did not replace the endpoint/dependent generation\n";
                for (const std::string& event : manager.Events()) std::cerr << event << '\n';
                result = 7;
            }
        }

        if (result == 0) {
            const std::string diagnostics = manager.DiagnosticsJson();
            if (diagnostics.find("\"ipcEndpoints\"") == std::string::npos ||
                diagnostics.find("dev.anomaly.ipc.c-provider") == std::string::npos ||
                diagnostics.find("\"requestSchema\"") == std::string::npos ||
                diagnostics.find("\"pendingCalls\":0") == std::string::npos) {
                std::cerr << "plugin diagnostics JSON omitted IPC telemetry\n";
                result = 8;
            }
        }
        manager.UnloadAll();
        if (result == 0 && !manager.Plugins().empty()) result = 9;
    }
    std::filesystem::remove_all(root, error);
    return result;
}
