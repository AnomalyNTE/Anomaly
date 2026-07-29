#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace anomaly {

enum class RuntimeLaunchError : std::uint8_t {
    None,
    InvalidRoot,
    CoreUnavailable,
};

struct RuntimeLaunchOptions final {
    std::filesystem::path installation_root;
};

struct RuntimeLaunchResult final {
    RuntimeLaunchError error{RuntimeLaunchError::None};
    std::string message;
    std::filesystem::path core_path;
    std::filesystem::path runtime_root;
    std::string version;

    [[nodiscard]] bool Ok() const noexcept {
        return error == RuntimeLaunchError::None && !core_path.empty();
    }
};

// Resolves the Core from the single current Runtime layout.
[[nodiscard]] RuntimeLaunchResult ResolveRuntimeLaunch(
    const RuntimeLaunchOptions& options) noexcept;

[[nodiscard]] const char* RuntimeLaunchErrorName(RuntimeLaunchError error) noexcept;

}  // namespace anomaly
