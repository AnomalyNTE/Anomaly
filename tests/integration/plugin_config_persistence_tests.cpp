#include "plugin_manager.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kPluginId = "anomaly.fixture.config-persistence";

bool Expect(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool WriteManifest(const std::filesystem::path& package) {
    std::ofstream output(package / L"manifest.json", std::ios::binary | std::ios::trunc);
    output << R"json({
  "schemaVersion": 2,
  "id": "anomaly.fixture.config-persistence",
  "name": "Config Persistence Fixture",
  "author": "Anomaly",
  "version": "1.0.0",
  "entry": "plugin.dll",
  "api": {"major": 1, "minMinor": 0, "maxMinor": 0},
  "games": ["nte"],
  "builds": ["nte-win64-*"],
  "loadPhase": "game-ready",
  "services": [{"id": "anomaly.config", "minVersion": 1}],
  "capabilities": ["configuration"]
})json";
    return static_cast<bool>(output);
}

bool IsActiveFixture(const ue5mem::PluginManager& manager, std::uint64_t* generation = nullptr) {
    const auto plugins = manager.Plugins();
    if (plugins.size() != 1 || plugins.front().id != kPluginId ||
        plugins.front().state != "active") {
        return false;
    }
    if (generation != nullptr) *generation = plugins.front().generation;
    const auto diagnostics = manager.DiagnosticsSnapshot();
    return diagnostics.plugins.size() == 1 &&
        diagnostics.plugins.front().platform_diagnostics.resources.configs == 1;
}

bool ContainsPhase(const std::filesystem::path& state_file, const std::string_view phase) {
    std::ifstream input(state_file, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return input.good() || input.eof() ? contents.find(phase) != std::string::npos : false;
}

bool IsRestartFixtureActive(const std::filesystem::path& root) {
    const std::filesystem::path config_file = root / L"config" / L"plugins" /
        L"anomaly.fixture.config-persistence" / L"config-settings.json";
    if (!Expect(ContainsPhase(config_file, "\"phase\":2"),
            "restart verifier did not receive settings persisted by the prior process")) {
        return false;
    }
    {
        ue5mem::PluginManager manager(root, L"plugins");
        manager.LoadAll();
        if (!Expect(IsActiveFixture(manager), "restart verifier config fixture did not activate")) {
            return false;
        }
    }
    return Expect(ContainsPhase(config_file, "\"phase\":3"),
        "restart verifier did not restore and persist prior settings");
}

bool RunRestartVerifier(const std::filesystem::path& root) {
    std::wstring executable(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return false;
    executable.resize(length);
    std::wstring command_line = L"\"" + executable + L"\" --verify-restart \"" +
        root.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process)) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code{};
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return completed && exit_code == 0;
}

bool VerifyPersistence(
    const std::filesystem::path& root, const std::filesystem::path& fixture) {
    const std::filesystem::path package = root / L"plugins" / L"ConfigPersistence";
    const std::filesystem::path config_file = root / L"config" / L"plugins" /
        L"anomaly.fixture.config-persistence" / L"config-settings.json";
    const std::filesystem::path legacy_state_file = root / L"state" / L"plugins" /
        L"anomaly.fixture.config-persistence" / L"config-settings.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (error || !std::filesystem::create_directories(package, error) || error ||
        !std::filesystem::copy_file(
            fixture, package / L"plugin.dll", std::filesystem::copy_options::overwrite_existing,
            error) ||
        error || !WriteManifest(package)) {
        return false;
    }
    if (!std::filesystem::create_directories(legacy_state_file.parent_path(), error) || error) {
        return false;
    }
    {
        std::ofstream legacy_state(legacy_state_file, std::ios::binary);
        legacy_state << "{\"schemaVersion\":1,\"data\":{\"phase\":3}}";
        if (!legacy_state) return false;
    }

    std::uint64_t first_generation{};
    {
        ue5mem::PluginManager manager(root, L"plugins");
        AnomalyUiServiceV1 ui{};
        ui.struct_size = sizeof(ui);
        ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
        manager.SetUiService(&ui);
        manager.LoadAll();
        if (!Expect(manager.SetEnabled(kPluginId, true),
                "config fixture explicit enable failed")) {
            return false;
        }
        manager.GameUpdate(1.0 / 60.0);
        manager.Draw(nullptr);
        if (!Expect(IsActiveFixture(manager, &first_generation), "initial config fixture did not activate") ||
            !Expect(!std::filesystem::exists(config_file), "fixture wrote settings before on_stop") ||
            !Expect(ContainsPhase(legacy_state_file, "\"phase\":3"),
                "legacy state configuration was changed") ||
            !Expect(manager.Reload(kPluginId), "config fixture reload failed")) {
            return false;
        }
        std::uint64_t reloaded_generation{};
        if (!Expect(IsActiveFixture(manager, &reloaded_generation), "reloaded config fixture did not activate") ||
            !Expect(reloaded_generation > first_generation, "reload did not create a new generation") ||
            !Expect(ContainsPhase(config_file, "\"phase\":1"),
                "reload did not persist settings below the configuration root") ||
            !Expect(ContainsPhase(legacy_state_file, "\"phase\":3"),
                "reload read or replaced the legacy state configuration")) {
            return false;
        }
    }

    if (!Expect(ContainsPhase(config_file, "\"phase\":2"),
            "manager shutdown did not persist reloaded settings from on_stop")) {
        return false;
    }

    if (!Expect(RunRestartVerifier(root),
            "fresh process did not restore and persist prior settings")) {
        return false;
    }

    std::filesystem::remove_all(root, error);
    return !error;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--verify-restart") {
        return IsRestartFixtureActive(std::filesystem::path(argv[2])) ? 0 : 1;
    }
    if (argc != 2) return 2;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-plugin-config-persistence-" + std::to_wstring(GetCurrentProcessId()));
    return VerifyPersistence(root, std::filesystem::path(argv[1])) ? 0 : 1;
}
