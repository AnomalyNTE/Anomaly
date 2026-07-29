#include "anomaly/launcher/proxy_installation.hpp"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

void Write(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void WriteAnomalyProxy(const std::filesystem::path& path, std::string_view release) {
    std::string image(512, '\0');
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 128;
    std::memcpy(image.data(), &dos, sizeof(dos));
    constexpr DWORD signature = IMAGE_NT_SIGNATURE;
    std::memcpy(image.data() + dos.e_lfanew, &signature, sizeof(signature));
    constexpr std::string_view start_entry{"AnomalyStart"};
    constexpr std::string_view core_path{
        "A\0n\0o\0m\0a\0l\0y\0.\0C\0o\0r\0e\0.\0d\0l\0l\0", 32};
    std::memcpy(image.data() + 192, start_entry.data(), start_entry.size());
    std::memcpy(image.data() + 224, core_path.data(), core_path.size());
    std::memcpy(image.data() + 320, release.data(), release.size());
    Write(path, image);
}

}  // namespace

int main() {
    bool result = true;
    using Action = anomaly::launcher::ProxyInstallationAction;
    using State = anomaly::launcher::ProxyInstallationState;
    result = Check(
        anomaly::launcher::ProxyInstallationActionForState(State::Unavailable) ==
                Action::None &&
            anomaly::launcher::ProxyInstallationActionForState(State::NotInstalled) ==
                Action::Install &&
            anomaly::launcher::ProxyInstallationActionForState(State::Enabled) ==
                Action::Disable &&
            anomaly::launcher::ProxyInstallationActionForState(State::Disabled) ==
                Action::Enable &&
            anomaly::launcher::ProxyInstallationActionForState(State::UpdateAvailable) ==
                Action::Update &&
            anomaly::launcher::ProxyInstallationActionForState(State::Conflict) ==
                Action::None,
        "proxy installation states did not map to the expected launcher actions") && result;

    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-launcher-proxy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto payload = root / L"payload";
    const auto game = root / L"game";
    const auto collision = root / L"collision";
    const auto runtime_collision = root / L"runtime-collision";
    const auto update = root / L"update";
    const auto transient_update = root / L"transient-update";
    WriteAnomalyProxy(payload / L"dwmapi.dll", "release-2");
    Write(payload / L"Anomaly" / L"Anomaly.Core.dll", "anomaly-core");
    Write(payload / L"Anomaly" / L"profiles" / L"nte-current.json", "{}");
    Write(payload / L"Anomaly" / L"anomaly.ini", "packaged-config");
    Write(game / L"HTGame.exe", "fixture");
    Write(collision / L"HTGame.exe", "fixture");
    Write(runtime_collision / L"HTGame.exe", "fixture");
    Write(update / L"HTGame.exe", "fixture");
    Write(runtime_collision / L"Anomaly" / L"Anomaly.Core.dll", "other-core");
    WriteAnomalyProxy(update / L"dwmapi.dll.disabled", "release-1");
    Write(update / L"Anomaly" / L"Anomaly.Core.dll", "old-core");
    Write(update / L"Anomaly" / L"profiles" / L"nte-current.json", "old-profile");
    Write(update / L"Anomaly" / L"anomaly.ini", "user-config");
    Write(update / L"Anomaly" / L"logs" / L"previous.log", "old-log");
    Write(update / L"Anomaly" / L"plugins" / L"Custom" / L"plugin.dll", "custom");
    Write(transient_update / L"HTGame.exe", "fixture");
    WriteAnomalyProxy(transient_update / L"dwmapi.dll", "release-1");
    Write(transient_update / L"Anomaly" / L"Anomaly.Core.dll", "old-core");

    const anomaly::launcher::ProxyInstallationSource source{
        payload / L"dwmapi.dll", payload / L"Anomaly"};
    auto status = anomaly::launcher::InspectProxyInstallation(game, source);
    result = Check(
        status.Ok() &&
            status.state == anomaly::launcher::ProxyInstallationState::NotInstalled,
        "clean game directory was not reported as uninstalled") && result;

    status = anomaly::launcher::InstallProxyRuntime(game, source);
    result = Check(
        status.Ok() && status.state == anomaly::launcher::ProxyInstallationState::Enabled &&
            std::filesystem::is_regular_file(game / L"dwmapi.dll") &&
            std::filesystem::is_regular_file(game / L"Anomaly" / L"Anomaly.Core.dll") &&
            !std::filesystem::exists(game / L"Anomaly" / L"versions") &&
            !std::filesystem::exists(
                game / L"Anomaly" / L"state" / L"runtime-selection.json") &&
            std::filesystem::is_regular_file(
                game / L"Anomaly" / L"profiles" / L"nte-current.json"),
        "clean proxy/runtime installation failed") && result;

    WriteAnomalyProxy(game / L"dwmapi.dll", "proxy-drift");
    status = anomaly::launcher::InspectProxyInstallation(game, source);
    result = Check(
        status.Ok() && status.state == anomaly::launcher::ProxyInstallationState::Enabled,
        "proxy hash incorrectly controlled Runtime update availability") && result;

    status = anomaly::launcher::SetProxyEnabled(game, source, false);
    result = Check(
        status.Ok() && status.state == anomaly::launcher::ProxyInstallationState::Disabled &&
            !std::filesystem::exists(game / L"dwmapi.dll") &&
            std::filesystem::is_regular_file(game / L"dwmapi.dll.disabled"),
        "proxy disable rename failed") && result;

    status = anomaly::launcher::SetProxyEnabled(game, source, true);
    result = Check(
        status.Ok() && status.state == anomaly::launcher::ProxyInstallationState::Enabled &&
            std::filesystem::is_regular_file(game / L"dwmapi.dll") &&
            !std::filesystem::exists(game / L"dwmapi.dll.disabled"),
        "proxy enable rename failed") && result;

    Write(collision / L"dwmapi.dll", "other-proxy");
    status = anomaly::launcher::InstallProxyRuntime(collision, source);
    result = Check(
        status.state == anomaly::launcher::ProxyInstallationState::Conflict &&
            status.error == anomaly::launcher::ProxyInstallationError::Conflict,
        "unrelated proxy collision was not rejected") && result;
    std::ifstream collision_input(collision / L"dwmapi.dll", std::ios::binary);
    std::string collision_text{
        std::istreambuf_iterator<char>(collision_input), std::istreambuf_iterator<char>()};
    result = Check(collision_text == "other-proxy", "unrelated proxy was modified") && result;

    status = anomaly::launcher::InspectProxyInstallation(update, source);
    result = Check(
        status.Ok() &&
            status.state == anomaly::launcher::ProxyInstallationState::UpdateAvailable,
        "older Anomaly installation was not offered an update") && result;
    status = anomaly::launcher::InstallProxyRuntime(update, source);
    result = Check(
        status.Ok() && status.state == anomaly::launcher::ProxyInstallationState::Disabled &&
            !std::filesystem::exists(update / L"dwmapi.dll") &&
            std::filesystem::is_regular_file(update / L"dwmapi.dll.disabled") &&
            std::filesystem::file_size(update / L"dwmapi.dll.disabled") ==
                std::filesystem::file_size(source.proxy) &&
            std::filesystem::file_size(update / L"Anomaly" / L"Anomaly.Core.dll") ==
                std::filesystem::file_size(
                    source.runtime_directory / L"Anomaly.Core.dll"),
        "disabled Anomaly installation did not update in place") && result;
    std::ifstream config_input(update / L"Anomaly" / L"anomaly.ini", std::ios::binary);
    std::string config_text{
        std::istreambuf_iterator<char>(config_input), std::istreambuf_iterator<char>()};
    result = Check(
        config_text == "user-config" &&
            std::filesystem::is_regular_file(
                update / L"Anomaly" / L"logs" / L"previous.log") &&
            std::filesystem::is_regular_file(
                update / L"Anomaly" / L"plugins" / L"Custom" / L"plugin.dll"),
        "Runtime update did not preserve mutable installation data") && result;
    std::ifstream profile_input(
        update / L"Anomaly" / L"profiles" / L"nte-current.json", std::ios::binary);
    std::string profile_text{
        std::istreambuf_iterator<char>(profile_input), std::istreambuf_iterator<char>()};
    result = Check(
        profile_text == "{}",
        "Runtime update did not replace packaged release files") && result;

    const HANDLE transient_lock = CreateFileW(
        (transient_update / L"Anomaly").c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    result = Check(
        transient_lock != INVALID_HANDLE_VALUE,
        "transient Runtime directory lock could not be created") && result;
    std::thread release_lock;
    if (transient_lock != INVALID_HANDLE_VALUE) {
        release_lock = std::thread([transient_lock] {
            Sleep(250);
            CloseHandle(transient_lock);
        });
    }
    status = anomaly::launcher::InstallProxyRuntime(transient_update, source);
    if (release_lock.joinable()) release_lock.join();
    result = Check(
        status.Ok() && status.state == anomaly::launcher::ProxyInstallationState::Enabled &&
            std::filesystem::file_size(
                transient_update / L"Anomaly" / L"Anomaly.Core.dll") ==
                std::filesystem::file_size(
                    source.runtime_directory / L"Anomaly.Core.dll"),
        "Runtime update did not tolerate a transient directory lock") && result;

    status = anomaly::launcher::InstallProxyRuntime(runtime_collision, source);
    result = Check(
        status.state == anomaly::launcher::ProxyInstallationState::Conflict &&
            status.error == anomaly::launcher::ProxyInstallationError::Conflict &&
            !std::filesystem::exists(runtime_collision / L"dwmapi.dll"),
        "mismatched existing core was paired with the launcher proxy") && result;

    Write(game / L"dwmapi.dll.disabled", "anomaly-proxy");
    status = anomaly::launcher::InspectProxyInstallation(game, source);
    result = Check(
        status.state == anomaly::launcher::ProxyInstallationState::Conflict,
        "ambiguous enabled/disabled proxy state was accepted") && result;

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    return result ? 0 : 1;
}
