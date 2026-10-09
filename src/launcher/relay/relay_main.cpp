// Anomaly.Relay.dll -- the second stage of the relay injection flow.
//
// The launcher maps this image into an innocent host process (see ManualMapRelayOptions
// in manual_map.hpp). The relay then performs, from inside that host, the work the
// launcher used to do itself: wait for the game process to appear, map and bootstrap the
// Anomaly core inside it, and report the outcome back through the named result channel.
// The process tree attributes the injection to the host, not to the launcher.
//
// The image follows the manual-map contract of the launcher's remote mapper: it exports
// AnomalyStart (which receives a RelayStartInfo marshaled in place of an
// AnomalyStartInfo) and AnomalyManualMapCxxThrow (compiled in from
// src/runtime/manual_map_exception.cpp), and it links the manual-map library compiled
// TLS-free so it carries no loader-managed static TLS.

#include "anomaly/core_api.h"
#include "anomaly/launcher/manual_map.hpp"
#include "anomaly/launcher/relay_map.hpp"

#include <Windows.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>

namespace {

using anomaly::launcher::ANOMALY_RELAY_RESULT_V1_SIZE;
using anomaly::launcher::ANOMALY_RELAY_RESULT_VERSION;
using anomaly::launcher::ANOMALY_RELAY_START_INFO_V1_SIZE;
using anomaly::launcher::ANOMALY_RELAY_START_INFO_VERSION;
using anomaly::launcher::ManualMapError;
using anomaly::launcher::ManualMapOptions;
using anomaly::launcher::ManualMapResult;
using anomaly::launcher::RelayResult;
using anomaly::launcher::RelayStartInfo;
using anomaly::launcher::TargetCaptureOutcome;
using anomaly::launcher::TargetCaptureRequest;
using anomaly::launcher::WaitForNewTargetProcess;

std::wstring FieldString(const wchar_t* field) noexcept {
    return std::wstring(field);
}

RelayResult MakeResult() noexcept {
    RelayResult result{};
    result.struct_size = ANOMALY_RELAY_RESULT_V1_SIZE;
    result.version = ANOMALY_RELAY_RESULT_VERSION;
    return result;
}

void CopyResultMessage(RelayResult& result, const std::string& message) noexcept {
    const std::size_t limit = std::size(result.message) - 1;
    std::size_t index = 0;
    for (const char character : message) {
        if (index == limit) break;
        result.message[index++] = static_cast<wchar_t>(character);
    }
    result.message[index] = L'\0';
}

RelayResult MakeFailureResult(
    ManualMapError error, DWORD win32_error, const std::string& message) noexcept {
    RelayResult result = MakeResult();
    result.error = static_cast<std::uint32_t>(error);
    result.win32_error = win32_error;
    CopyResultMessage(result, message);
    return result;
}

RelayResult MakeMappedResult(
    const ManualMapResult& mapped, DWORD game_process_id) noexcept {
    RelayResult result = MakeResult();
    result.error = static_cast<std::uint32_t>(mapped.error);
    result.win32_error = mapped.win32_error;
    result.runtime_start_error = mapped.runtime_start_error;
    result.game_process_id = game_process_id;
    result.remote_image = mapped.remote_image;
    CopyResultMessage(result, mapped.message);
    return result;
}

void PublishResult(
    const RelayStartInfo& info, const RelayResult& result) noexcept {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, info.result_mapping_name);
    if (mapping == nullptr) return;
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(RelayResult));
    if (view != nullptr) {
        std::memcpy(view, &result, sizeof(RelayResult));
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, info.result_event_name);
    if (event != nullptr) {
        SetEvent(event);
        CloseHandle(event);
    }
}

// Ends a target whose mapping started but failed, mirroring the direct launch flow: a
// game left carrying a partially mapped core is not something to keep running. Failures
// before the image was allocated leave the game alone, exactly as the direct flow does.
void TerminateFailedTarget(DWORD process_id, const ManualMapResult& mapped) noexcept {
    if (mapped.Ok() || mapped.remote_image == 0) return;
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process_id);
    if (process == nullptr) return;
    static_cast<void>(TerminateProcess(process, ERROR_PROCESS_ABORTED));
    static_cast<void>(WaitForSingleObject(process, 5000));
    CloseHandle(process);
}

DWORD RunRelay(const RelayStartInfo& info) noexcept {
    try {
        TargetCaptureRequest request;
        request.executable_name = FieldString(info.game_executable_name);
        request.armed_event_name = FieldString(info.armed_event_name);
        request.armed_timeout = std::chrono::milliseconds(info.armed_timeout_ms);
        request.timeout = std::chrono::milliseconds(info.capture_timeout_ms);
        request.excluded_pids.assign(
            info.excluded_pids,
            info.excluded_pids + std::min<std::size_t>(
                info.excluded_pid_count, std::size(info.excluded_pids)));
        const TargetCaptureOutcome captured = WaitForNewTargetProcess(request);
        if (captured.error != ManualMapError::None) {
            PublishResult(info, MakeFailureResult(
                captured.error, captured.win32_error, captured.message));
            return captured.win32_error;
        }

        ManualMapOptions options;
        options.process_id = captured.process_id;
        options.core_path = std::filesystem::path(FieldString(info.core_path));
        options.runtime_root = std::filesystem::path(FieldString(info.runtime_root));
        options.log_directory = std::filesystem::path(FieldString(info.log_directory));
        options.timeout = std::chrono::milliseconds(info.attach_timeout_ms);
        options.loader_timeout = std::chrono::milliseconds(info.loader_timeout_ms);
        const ManualMapResult mapped =
            anomaly::launcher::ManualMapAttachCoreToProcessId(captured.process_id, options);
        TerminateFailedTarget(captured.process_id, mapped);
        PublishResult(info, MakeMappedResult(mapped, captured.process_id));
        return mapped.Ok() ? ERROR_SUCCESS : mapped.win32_error;
    } catch (...) {
        PublishResult(info, MakeFailureResult(
            ManualMapError::BootstrapFailure, ERROR_UNHANDLED_EXCEPTION,
            "relay attach raised an unexpected exception"));
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyStart(
    const AnomalyStartInfo* start_info) {
    // The launcher's remote bootstrap marshals a RelayStartInfo for this image; the
    // signature stays the manual-map contract's, the payload is this relay's.
    if (start_info == nullptr) return ERROR_INVALID_PARAMETER;
    const auto* info = reinterpret_cast<const RelayStartInfo*>(start_info);
    if (info->struct_size != ANOMALY_RELAY_START_INFO_V1_SIZE ||
        info->version != ANOMALY_RELAY_START_INFO_VERSION) {
        return ERROR_INVALID_PARAMETER;
    }
    return RunRelay(*info);
}
