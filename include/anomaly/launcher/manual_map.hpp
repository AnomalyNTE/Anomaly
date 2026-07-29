#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly::launcher {

struct AttachableProcess final {
    DWORD process_id{};
    std::wstring executable_name;
    std::filesystem::path executable_path;
    bool owned_by_current_user{};
    bool x64{};
    DWORD inspection_error{};

    [[nodiscard]] bool Compatible() const noexcept {
        return process_id != 0 && owned_by_current_user && x64 && inspection_error == ERROR_SUCCESS;
    }
};

enum class ManualMapError : std::uint8_t {
    None,
    ProcessLaunchFailure,
    ProcessControlFailure,
    ProcessUnavailable,
    AccessDenied,
    DifferentUser,
    IncompatibleArchitecture,
    ImageUnavailable,
    ImageInvalid,
    AlreadyAttached,
    DependencyFailure,
    AllocationFailure,
    WriteFailure,
    ProtectionFailure,
    BootstrapFailure,
    Timeout,
};

struct ManualMapOptions final {
    DWORD process_id{};
    std::filesystem::path core_path;
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
};

struct ManualMapResult final {
    ManualMapError error{ManualMapError::None};
    DWORD win32_error{ERROR_SUCCESS};
    DWORD runtime_start_error{ERROR_SUCCESS};
    std::uintptr_t remote_image{};
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return error == ManualMapError::None; }
};

struct ManualMapLaunchOptions final {
    std::filesystem::path launcher_path;
    std::wstring launcher_arguments;
    std::filesystem::path working_directory;
    std::wstring target_executable_name{L"HTGame.exe"};
    DWORD creation_flags{};
    std::chrono::milliseconds target_timeout{std::chrono::minutes(2)};
    std::chrono::milliseconds loader_timeout{std::chrono::minutes(2)};
    ManualMapOptions manual_map;
};

struct ManualMapLaunchResult final {
    ManualMapResult mapping;
    DWORD process_id{};

    [[nodiscard]] bool Ok() const noexcept {
        return process_id != 0 && mapping.Ok();
    }
};

[[nodiscard]] AttachableProcess InspectAttachableProcess(DWORD process_id) noexcept;
[[nodiscard]] std::vector<AttachableProcess> EnumerateAttachableProcesses(
    std::wstring_view executable_name = L"HTGame.exe") noexcept;
[[nodiscard]] ManualMapResult ManualMapRuntimeCore(const ManualMapOptions& options) noexcept;
[[nodiscard]] ManualMapLaunchResult LaunchAndManualMapRuntimeCore(
    const ManualMapLaunchOptions& options) noexcept;

}  // namespace anomaly::launcher
