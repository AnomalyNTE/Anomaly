#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace anomaly::launcher {

enum class ProxyInstallationState : std::uint8_t {
    Unavailable,
    NotInstalled,
    Enabled,
    Disabled,
    UpdateAvailable,
    Conflict,
};

enum class ProxyInstallationAction : std::uint8_t {
    None,
    Install,
    Enable,
    Disable,
    Update,
};

[[nodiscard]] constexpr ProxyInstallationAction ProxyInstallationActionForState(
    const ProxyInstallationState state) noexcept {
    switch (state) {
    case ProxyInstallationState::NotInstalled:
        return ProxyInstallationAction::Install;
    case ProxyInstallationState::Enabled:
        return ProxyInstallationAction::Disable;
    case ProxyInstallationState::Disabled:
        return ProxyInstallationAction::Enable;
    case ProxyInstallationState::UpdateAvailable:
        return ProxyInstallationAction::Update;
    case ProxyInstallationState::Unavailable:
    case ProxyInstallationState::Conflict:
        return ProxyInstallationAction::None;
    }
    return ProxyInstallationAction::None;
}

enum class ProxyInstallationError : std::uint8_t {
    None,
    InvalidGameDirectory,
    SourceUnavailable,
    RuntimeUnavailable,
    Conflict,
    IntegrityFailure,
    IoFailure,
};

struct ProxyInstallationStatus final {
    ProxyInstallationState state{ProxyInstallationState::Unavailable};
    ProxyInstallationError error{ProxyInstallationError::None};
    std::filesystem::path game_directory;
    std::string message;
    bool proxy_enabled{};

    [[nodiscard]] bool Ok() const noexcept {
        return error == ProxyInstallationError::None;
    }
};

struct ProxyInstallationSource final {
    std::filesystem::path proxy;
    std::filesystem::path runtime_directory;
};

// Files an installation of the runtime keeps when a newer package is published over it: they hold
// the user's settings, repository configuration and run data rather than anything the package
// owns. The path is relative to the installed `Anomaly` directory, which is also the layout
// inside a runtime package, so an update of the launcher folder itself uses the same list.
[[nodiscard]] bool PreservesInstalledRuntimeFile(const std::filesystem::path& relative) noexcept;

[[nodiscard]] ProxyInstallationStatus InspectProxyInstallation(
    const std::filesystem::path& game_directory,
    const ProxyInstallationSource& source);

[[nodiscard]] ProxyInstallationStatus InstallProxyRuntime(
    const std::filesystem::path& game_directory,
    const ProxyInstallationSource& source);

[[nodiscard]] ProxyInstallationStatus SetProxyEnabled(
    const std::filesystem::path& game_directory,
    const ProxyInstallationSource& source,
    bool enabled);

// Removes the recognized proxy and the entire game-local Anomaly directory, including user data.
[[nodiscard]] ProxyInstallationStatus UninstallProxy(
    const std::filesystem::path& game_directory,
    const ProxyInstallationSource& source);

}  // namespace anomaly::launcher
