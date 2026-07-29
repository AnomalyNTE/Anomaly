#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace anomaly::launcher {

struct LauncherConfiguration final {
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
};

struct LauncherConfigurationLoadResult final {
    LauncherConfiguration configuration;
    bool loaded{};
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;
};

struct LauncherConfigurationSaveResult final {
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return win32_error == ERROR_SUCCESS; }
};

struct LauncherDiscoveryOptions final {
    std::filesystem::path payload_root;
    std::vector<std::filesystem::path> running_game_executables;
    std::vector<std::filesystem::path> running_launcher_executables;
    std::vector<std::filesystem::path> search_roots;
    bool include_system_locations{true};
};

[[nodiscard]] std::filesystem::path LauncherConfigurationPath(
    const std::filesystem::path& payload_root) noexcept;
[[nodiscard]] LauncherConfigurationLoadResult LoadLauncherConfiguration(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] LauncherConfigurationSaveResult SaveLauncherConfiguration(
    const std::filesystem::path& path,
    const LauncherConfiguration& configuration) noexcept;

[[nodiscard]] bool IsNteGameDirectory(
    const std::filesystem::path& directory) noexcept;
[[nodiscard]] bool IsNteLauncherExecutable(
    const std::filesystem::path& executable) noexcept;
[[nodiscard]] LauncherConfiguration DiscoverLauncherConfiguration(
    const LauncherConfiguration& preferred,
    const LauncherDiscoveryOptions& options);

}  // namespace anomaly::launcher
