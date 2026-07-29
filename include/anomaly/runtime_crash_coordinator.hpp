#pragma once

#include "anomaly/runtime_recovery.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace anomaly {

enum class RuntimeCrashCoordinatorError : std::uint8_t {
    None,
    InvalidOptions,
    ProcessUnavailable,
    MarkerRejected,
    MonitorUnavailable,
    MonitorLaunchFailed,
    RecoveryRejected,
    IoFailure,
};

struct RuntimeCrashCoordinatorResult {
    RuntimeCrashCoordinatorError error{RuntimeCrashCoordinatorError::None};
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;
    std::string incident_id;

    [[nodiscard]] bool Ok() const noexcept {
        return error == RuntimeCrashCoordinatorError::None;
    }
};

struct RuntimeCrashCoordinatorOptions {
    std::filesystem::path runtime_root;
    std::filesystem::path monitor_executable;
    DWORD process_id{};
    std::uint64_t session_generation{};
    std::string runtime_version;
};

class RuntimeCrashCoordinatorClient final {
public:
    explicit RuntimeCrashCoordinatorClient(RuntimeCrashCoordinatorOptions options);
    ~RuntimeCrashCoordinatorClient();

    RuntimeCrashCoordinatorClient(const RuntimeCrashCoordinatorClient&) = delete;
    RuntimeCrashCoordinatorClient& operator=(const RuntimeCrashCoordinatorClient&) = delete;

    [[nodiscard]] RuntimeCrashCoordinatorResult Start() noexcept;
    [[nodiscard]] RuntimeCrashCoordinatorResult SetFailureContext(
        RuntimeFailureSource source,
        std::string profile_id = {},
        std::string plugin_id = {},
        std::uint64_t plugin_generation = 0) noexcept;
    [[nodiscard]] RuntimeCrashCoordinatorResult MarkHealthy() noexcept;
    [[nodiscard]] RuntimeCrashCoordinatorResult MarkStopping() noexcept;
    [[nodiscard]] bool WaitForMonitor(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] std::string IncidentId() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct RuntimeCrashMonitorOptions {
    std::filesystem::path runtime_root;
    DWORD process_id{};
    std::uint64_t process_creation_time{};
    std::uint64_t session_generation{};
    std::string incident_id;
};

[[nodiscard]] RuntimeCrashCoordinatorResult RunRuntimeCrashMonitor(
    const RuntimeCrashMonitorOptions& options) noexcept;

[[nodiscard]] const char* RuntimeCrashCoordinatorErrorName(
    RuntimeCrashCoordinatorError error) noexcept;

}  // namespace anomaly
