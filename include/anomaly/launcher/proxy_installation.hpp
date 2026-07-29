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

    [[nodiscard]] bool Ok() const noexcept {
        return error == ProxyInstallationError::None;
    }
};

struct ProxyInstallationSource final {
    std::filesystem::path proxy;
    std::filesystem::path runtime_directory;
};

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

}  // namespace anomaly::launcher
