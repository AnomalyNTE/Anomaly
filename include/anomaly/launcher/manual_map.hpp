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
    // Budget for mapping and bootstrapping the core image.
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
    // Budget for the target's loader to publish its module list. Used by the by-pid
    // attach path, which is entered from the relay flow; the launch flow keeps its own
    // loader wait next to target capture.
    std::chrono::milliseconds loader_timeout{std::chrono::minutes(2)};
};

// Instead of mapping the core into the game from this process, the launcher maps a small
// relay image into an innocent host process and the relay performs the capture and the
// mapping. The process tree then attributes the injection to the host, not to the
// launcher. Both paths must be empty to use the direct in-process injection.
struct ManualMapRelayOptions final {
    // Signed, common, policy-free host executable (for example notepad.exe).
    std::filesystem::path host_executable;
    // Anomaly.Relay.dll, resolved by the caller next to the runtime payload.
    std::filesystem::path relay_image;

    [[nodiscard]] bool Enabled() const noexcept {
        return !host_executable.empty() && !relay_image.empty();
    }
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
    ManualMapRelayOptions relay;
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

// Waits for a game process that did not exist when the call started, optionally gated on
// the launcher hook's armed event. Used by the relay image; snapshotting the process set
// before the game can spawn is what keeps "new" meaningful.
struct TargetCaptureRequest final {
    std::wstring executable_name{L"HTGame.exe"};
    std::wstring armed_event_name;
    std::chrono::milliseconds armed_timeout{std::chrono::seconds(90)};
    std::chrono::milliseconds timeout{std::chrono::minutes(2)};
    // When non-empty, these are the processes that "did not exist" means: a target pid
    // outside this set is the new one. The caller takes responsibility for having
    // snapshotted at the right moment. When empty, the set is snapshotted here, at call
    // time -- which is only correct when the target cannot appear before this call.
    std::vector<DWORD> excluded_pids;
};

struct TargetCaptureOutcome final {
    DWORD process_id{};
    ManualMapError error{ManualMapError::ProcessUnavailable};
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;
};

[[nodiscard]] TargetCaptureOutcome WaitForNewTargetProcess(
    const TargetCaptureRequest& request) noexcept;

// Maps and bootstraps the core in an already-running process. This is the second hop of
// the relay flow, but it is also the building block for any attach-by-pid feature.
[[nodiscard]] ManualMapResult ManualMapAttachCoreToProcessId(
    DWORD process_id, const ManualMapOptions& options) noexcept;

}  // namespace anomaly::launcher
