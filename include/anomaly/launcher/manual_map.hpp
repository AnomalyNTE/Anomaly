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

// How the mapped core is made to run inside the target. A process that was just created can be entered
// from a thread this process is allowed to create; a game that has been running for a while is better
// entered through threads it already owns, because a new thread in the target is the part of an
// injection that is easiest to observe from inside it.
enum class ManualMapDelivery : std::uint8_t {
    // CreateRemoteThread. Deterministic: the payload runs on a thread whose whole life this process
    // owns, and its exit code reports the outcome. Needs PROCESS_CREATE_THREAD.
    RemoteThread,
    // Redirect the instruction pointer of a thread that is parked in one of ntdll's system call stubs.
    // No thread is created so there is nothing to notice, but the thread is briefly suspended and its
    // context is rewritten, and the redirect is only safe on threads waiting inside a stub.
    ThreadHijack,
    // ThreadHijack, then RemoteThread, sharing the timeout between them. This is what a live attach uses:
    // a game that refuses a new thread can still be entered through its own.
    Automatic,
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
    ManualMapDelivery delivery{ManualMapDelivery::RemoteThread};
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
    // Injected into the launcher so the session can be started without the user touching the launcher
    // UI. It hides the launcher's own window from the inside and signals the armed event below
    // immediately before asking the launcher to start the game. When empty, nothing is injected and
    // capture polls from the moment the launcher is started, waiting for the player to start the game
    // from the launcher's own window.
    std::filesystem::path hook_path;
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
[[nodiscard]] ManualMapLaunchResult LaunchAndManualMapRuntimeCore(
    const ManualMapLaunchOptions& options) noexcept;
// Manual-maps the runtime core into a process that is already running, so the game does not have to be
// started by this launcher. The target is attached to as it is: nothing is relaunched, nothing is
// terminated on failure, and the caller decides whether a failed attach leaves the game running.
[[nodiscard]] ManualMapResult AttachToRunningProcess(const ManualMapOptions& options) noexcept;

}  // namespace anomaly::launcher
