#include "plugin_manager.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

constexpr std::string_view kStableId = "anomaly.fixture.enablement-stable";
constexpr std::string_view kBaseId = "anomaly.fixture.enablement-base";
constexpr std::string_view kChildId = "anomaly.fixture.enablement-child";
constexpr std::string_view kStopTimeoutId = "anomaly.fixture.stop-timeout";
constexpr std::string_view kIdentityManifestV3 = "anomaly.fixture.identity-manifest-v3";

const ue5mem::PluginView* Find(
    const std::vector<ue5mem::PluginView>& plugins, std::string_view id) {
    const auto found = std::ranges::find_if(
        plugins, [&](const auto& plugin) { return plugin.id == id; });
    return found == plugins.end() ? nullptr : &*found;
}

bool WritePackage(
    const std::filesystem::path& package, const std::filesystem::path& source,
    std::string_view id, std::string_view dependency = {},
    std::string_view version = "1.0.0") {
    std::error_code error;
    std::filesystem::create_directories(package, error);
    if (error || !std::filesystem::copy_file(
            source, package / L"plugin.dll",
            std::filesystem::copy_options::overwrite_existing, error) || error) {
        return false;
    }
    std::ofstream manifest(package / L"manifest.json", std::ios::binary | std::ios::trunc);
    manifest << R"({"schemaVersion":2,"id":")" << id
             << R"(","name":"Enablement Fixture","author":"Anomaly","version":")"
             << version << R"(",)"
                R"("entry":"plugin.dll","api":{"major":1,"minMinor":0,"maxMinor":0},)"
                R"("games":["nte"],"builds":["nte-win64-*"],"loadPhase":"game-ready",)"
                R"("capabilities":[])";
    if (!dependency.empty()) {
        manifest << R"(,"dependencies":[{"id":")" << dependency
                 << R"(","version":">=1.0.0","optional":false}])";
    }
    manifest << '}';
    return static_cast<bool>(manifest);
}

bool ReplaceBinary(
    const std::filesystem::path& package, const std::filesystem::path& source) {
    std::error_code error;
    const bool copied = std::filesystem::copy_file(
        source, package / L"plugin.dll",
        std::filesystem::copy_options::overwrite_existing, error);
    return copied && !error;
}

bool BreakBinary(const std::filesystem::path& package) {
    std::ofstream output(
        package / L"plugin.dll", std::ios::binary | std::ios::trunc);
    output << "not a dynamic library";
    return static_cast<bool>(output);
}

bool Expect(bool condition, std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    const auto legacy_root = std::filesystem::temp_directory_path() /
        (L"anomaly-enable-legacy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto legacy_file = legacy_root / L"config" / L"plugin-enablement.json";
    std::error_code legacy_error;
    std::filesystem::create_directories(legacy_file.parent_path(), legacy_error);
    std::ofstream legacy_output(legacy_file, std::ios::binary | std::ios::trunc);
    legacy_output << R"({"schemaVersion":1,"activeProfile":"default","profiles":[{"id":"default","name":"Default","defaultEnabled":true,"plugins":{}}]})";
    legacy_output.close();
    anomaly::PluginEnablementStore legacy_store(legacy_file);
    std::string legacy_load_error;
    if (!Expect(!legacy_error && !legacy_store.Load(&legacy_load_error),
            "legacy multi-profile enablement document was accepted")) {
        return 18;
    }
    std::filesystem::remove_all(legacy_root, legacy_error);

    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-enable-reconcile-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto plugins = root / L"plugins";
    const bool fixtures_ready =
        WritePackage(plugins / L"Stable", argv[1], kStableId) &&
        WritePackage(plugins / L"Base", argv[2], kBaseId) &&
        WritePackage(plugins / L"Child", argv[3], kChildId, kBaseId);
    if (!fixtures_ready) return 3;

    bool result = true;
    {
        ue5mem::PluginManager manager(root, plugins);
        manager.LoadAll();

        auto views = manager.Plugins();
        const auto* stable = Find(views, kStableId);
        const auto* base = Find(views, kBaseId);
        const auto* child = Find(views, kChildId);
        result = Expect(
            views.size() == 3 && stable != nullptr && !stable->enabled &&
                stable->state == "disabled" && stable->status_reason == "enablement default" &&
                base != nullptr && !base->enabled && child != nullptr && !child->enabled,
            "newly discovered plugins were not disabled by default") && result;
        result = Expect(
            WritePackage(plugins / L"Stable", argv[1], kStableId, {}, "1.1.0"),
            "disabled reload fixture update failed") && result;
        result = Expect(manager.Reload(kStableId),
                        "disabled plugin package reload failed") && result;
        views = manager.Plugins();
        stable = Find(views, kStableId);
        result = Expect(
            stable != nullptr && !stable->enabled && stable->state == "disabled" &&
                stable->generation == 0 && stable->version == "1.1.0",
            "disabled package reload changed enablement or skipped package discovery") && result;
        result = Expect(
            WritePackage(plugins / L"Stable", argv[1], kStableId),
            "disabled reload fixture restore failed") && result;
        result = Expect(manager.SetEnabled(kStableId, true),
                        "explicit stable plugin enable failed") && result;
        result = Expect(manager.SetEnabled(kChildId, true),
                        "explicit dependent plugin enable failed") && result;
        views = manager.Plugins();
        stable = Find(views, kStableId);
        base = Find(views, kBaseId);
        child = Find(views, kChildId);
        result = Expect(
            views.size() == 3 && stable != nullptr && stable->enabled && base != nullptr &&
                base->enabled && child != nullptr && child->enabled,
            "explicit plugin enablement did not activate fixtures and dependencies") && result;
        if (!result) return 4;
        const std::uint64_t stable_generation = stable->generation;

        result = Expect(manager.SetEnabled(kBaseId, false),
                        "dependency root disable failed") && result;
        views = manager.Plugins();
        stable = Find(views, kStableId);
        base = Find(views, kBaseId);
        child = Find(views, kChildId);
        result = Expect(
            stable != nullptr && stable->enabled && stable->generation == stable_generation &&
                base != nullptr && !base->enabled && child != nullptr && !child->enabled,
            "dependency disable restarted an unrelated plugin or missed its dependent") && result;
        result = Expect(BreakBinary(plugins / L"Base"),
                        "dependency failure fixture write failed") && result;
        result = Expect(!manager.SetEnabled(kChildId, true),
                        "dependent enable ignored a failed requirement") && result;
        views = manager.Plugins();
        stable = Find(views, kStableId);
        base = Find(views, kBaseId);
        child = Find(views, kChildId);
        result = Expect(
            stable != nullptr && stable->enabled && stable->generation == stable_generation &&
                base != nullptr && !base->enabled && base->state == "faulted" &&
                child != nullptr && !child->enabled &&
                child->state == "dependency-blocked",
            "failed requirement did not block its downstream generation") && result;

        result = Expect(ReplaceBinary(plugins / L"Base", argv[2]),
                        "dependency failure fixture restore failed") && result;
        result = Expect(manager.SetEnabled(kChildId, true),
                        "same-value enable did not retry failed generations") && result;
        views = manager.Plugins();
        stable = Find(views, kStableId);
        base = Find(views, kBaseId);
        child = Find(views, kChildId);
        result = Expect(
            stable != nullptr && stable->enabled && stable->generation == stable_generation &&
                base != nullptr && base->enabled && child != nullptr && child->enabled,
            "same-value retry did not restore the dependency chain selectively") && result;
        const std::uint64_t base_generation = base == nullptr ? 0 : base->generation;

        result = Expect(manager.SetEnabled(kChildId, false),
                        "dependent disable failed") && result;
        views = manager.Plugins();
        stable = Find(views, kStableId);
        base = Find(views, kBaseId);
        child = Find(views, kChildId);
        result = Expect(
            stable != nullptr && stable->enabled && stable->generation == stable_generation &&
                base != nullptr && base->enabled && base->generation == base_generation &&
                child != nullptr && !child->enabled,
            "dependent disable restarted its requirement or an unrelated plugin") && result;

    }

    const auto enablement_file = root / L"config" / L"plugin-enablement.json";
    std::ifstream enablement_input(enablement_file, std::ios::binary);
    const std::string enablement_json{
        std::istreambuf_iterator<char>(enablement_input), std::istreambuf_iterator<char>()};
    result = Expect(
        enablement_json.find(R"("schemaVersion": 1)") != std::string::npos &&
            enablement_json.find(R"("defaultEnabled": false)") != std::string::npos &&
            enablement_json.find(R"("profiles")") == std::string::npos &&
            enablement_json.find(R"("activeProfile")") == std::string::npos,
        "plugin enablement did not persist the single-state schema v1 document") && result;
    {
        ue5mem::PluginManager manager(root, plugins);
        manager.LoadAll();
        const auto views = manager.Plugins();
        const auto* stable = Find(views, kStableId);
        const auto* base = Find(views, kBaseId);
        const auto* child = Find(views, kChildId);
        result = Expect(
            stable != nullptr && stable->enabled && base != nullptr && base->enabled &&
                child != nullptr && !child->enabled &&
                child->status_reason == "enablement override",
            "global plugin enablement state was not restored after manager restart") && result;
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (!result) return 5;

    const auto identity_root = std::filesystem::temp_directory_path() /
        (L"anomaly-enable-identity-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto identity_plugins = identity_root / L"plugins";
    if (!WritePackage(
            identity_plugins / L"Mismatch", argv[1], kIdentityManifestV3)) {
        return 9;
    }
    {
        ue5mem::PluginManager manager(identity_root, identity_plugins);
        manager.LoadAll();
        result = Expect(!manager.SetEnabled(kIdentityManifestV3, true),
                        "identity mismatch plugin reported successful enablement") && result;
        const auto events = manager.Events();
        const auto mismatches = std::ranges::count_if(events, [](const std::string& event) {
            return event.find("plugin identity mismatch: manifest=") != std::string::npos;
        });
        const auto identity_views = manager.Plugins();
        const auto* mismatch = Find(identity_views, kIdentityManifestV3);
        result = Expect(
            identity_views.size() == 1 && mismatch != nullptr && !mismatch->enabled &&
                mismatch->generation == 0 && mismatch->state == "faulted" && mismatches == 1 &&
                !std::filesystem::exists(
                    identity_root / L"state" / L"plugins" /
                    L"anomaly.fixture.enablement-stable"),
            "catalog shadow accepted a mismatched manifest package") && result;
    }
    std::filesystem::remove_all(identity_root, cleanup_error);
    if (!result) return 10;

    const auto observer_root = std::filesystem::temp_directory_path() /
        (L"anomaly-enable-observer-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto observer_plugins = observer_root / L"plugins";
    if (!WritePackage(observer_plugins / L"Stable", argv[1], kStableId)) return 16;
    {
        std::size_t activation_events{};
        ue5mem::PluginManager manager(
            observer_root, observer_plugins, {}, {}, {}, {}, {}, {}, {},
            [&](std::string_view, std::uint64_t, bool) {
                ++activation_events;
                throw std::runtime_error("observer failure");
            });
        manager.LoadAll();
        result = Expect(manager.SetEnabled(kStableId, true),
                        "observer fixture explicit enable failed") && result;
        const auto views = manager.Plugins();
        const auto* stable = Find(views, kStableId);
        result = Expect(
            stable != nullptr && stable->enabled && activation_events == 2,
            "activation diagnostics disrupted plugin loading or leaked its context") && result;
    }
    std::filesystem::remove_all(observer_root, cleanup_error);
    if (!result) return 17;

    const auto policy_root = std::filesystem::temp_directory_path() /
        (L"anomaly-enable-policy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto policy_plugins = policy_root / L"plugins";
    if (!WritePackage(policy_plugins / L"Stable", argv[1], kStableId)) return 14;
    {
        ue5mem::PluginManager manager(
            policy_root, policy_plugins, {}, {}, {}, {}, {}, {},
            [](const anomaly::PluginManifest& manifest) {
                return manifest.id.starts_with("anomaly.builtin.");
            });
        manager.LoadAll();
        auto views = manager.Plugins();
        const auto* stable = Find(views, kStableId);
        result = Expect(
            stable != nullptr && !stable->enabled && stable->state == "suspended" &&
                !manager.SetEnabled(kStableId, true),
            "Runtime recovery policy allowed a suspended third-party plugin") && result;
        manager.ReloadAll();
        views = manager.Plugins();
        stable = Find(views, kStableId);
        result = Expect(
            stable != nullptr && !stable->enabled && stable->state == "suspended",
            "plugin reload bypassed the Runtime recovery policy") && result;
    }
    std::filesystem::remove_all(policy_root, cleanup_error);
    if (!result) return 15;

    const auto reinstall_root = std::filesystem::temp_directory_path() /
        (L"anomaly-enable-reinstall-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto reinstall_plugins = reinstall_root / L"plugins";
    const auto reinstall_package = reinstall_plugins / L"Stable";
    if (!WritePackage(reinstall_package, argv[1], kStableId)) return 19;
    {
        ue5mem::PluginManager manager(reinstall_root, reinstall_plugins);
        manager.LoadAll();
        result = Expect(manager.SetEnabled(kStableId, false),
                        "reinstall fixture explicit disable failed") && result;

        std::filesystem::remove_all(reinstall_package, cleanup_error);
        manager.MaintenancePluginState();
        std::this_thread::sleep_for(800ms);
        manager.MaintenancePluginState();
        auto views = manager.Plugins();
        result = Expect(
            Find(views, kStableId) == nullptr,
            "uninstalled disabled plugin retained a stale installed view") && result;

        result = Expect(
            WritePackage(reinstall_package, argv[1], kStableId),
            "reinstall fixture package restore failed") && result;
        manager.MaintenancePluginState();
        std::this_thread::sleep_for(800ms);
        manager.MaintenancePluginState();
        views = manager.Plugins();
        const auto* stable = Find(views, kStableId);
        result = Expect(
            stable != nullptr && !stable->enabled && stable->state == "disabled" &&
                stable->status_reason == "enablement override",
            "reinstalled plugin did not restore its persisted disabled view") && result;
        const bool enabled = manager.SetEnabled(kStableId, true);
        views = manager.Plugins();
        stable = Find(views, kStableId);
        result = Expect(enabled && stable != nullptr && stable->enabled,
                        "reinstalled plugin could not be enabled") && result;
    }
    std::filesystem::remove_all(reinstall_root, cleanup_error);
    if (!result) return 20;

    // Keep this scenario last: a quarantined module deliberately survives manager teardown.
    const auto quarantine_root = std::filesystem::temp_directory_path() /
        (L"anomaly-enable-quarantine-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto quarantine_plugins = quarantine_root / L"plugins";
    if (!WritePackage(quarantine_plugins / L"Base", argv[2], kBaseId) ||
        !WritePackage(
            quarantine_plugins / L"Child", argv[4], kStopTimeoutId, kBaseId)) {
        return 11;
    }
    {
        ue5mem::PluginManager manager(quarantine_root, quarantine_plugins);
        manager.LoadAll();
        if (!Expect(manager.SetEnabled(kStopTimeoutId, true),
                    "quarantine fixtures explicit enable failed")) {
            return 12;
        }
        auto views = manager.Plugins();
        const auto* base = Find(views, kBaseId);
        const auto* child = Find(views, kStopTimeoutId);
        if (!Expect(base != nullptr && base->enabled && child != nullptr && child->enabled,
                    "quarantine dependency fixtures did not activate")) {
            return 12;
        }
        const std::uint64_t base_generation = base->generation;
        result = Expect(
            !manager.SetEnabled(kBaseId, false),
            "dependency disable reported success after its consumer quarantined") && result;
        views = manager.Plugins();
        base = Find(views, kBaseId);
        child = Find(views, kStopTimeoutId);
        result = Expect(
            views.size() == 2 && base != nullptr && base->enabled &&
                base->generation == base_generation && child != nullptr &&
                !child->enabled && child->state == "quarantined",
            "quarantined consumer did not retain its required dependency generation") && result;
    }
    std::filesystem::remove_all(quarantine_root, cleanup_error);
    if (!result) return 13;
    std::cout << "plugin enablement reconciles only changed dependency generations\n";
    return 0;
}
