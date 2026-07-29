#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

enum class ServiceLifetime {
    Provided,
    Singleton,
    PluginScoped,
};

enum class ServiceStartup {
    Blocking,
    Async,
    Lazy,
};

enum class ServiceAffinity {
    Lifecycle,
    Game,
    Render,
    Worker,
    Any,
};

enum class ServiceState {
    Registered,
    Starting,
    Ready,
    Degraded,
    Failed,
    Stopping,
    Stopped,
};

struct ServiceDependency {
    std::string id;
    std::uint32_t minimum_version{1};
};

struct ServiceDescriptor {
    std::string id;
    std::uint32_t version{1};
    ServiceLifetime lifetime{ServiceLifetime::Singleton};
    ServiceStartup startup{ServiceStartup::Blocking};
    ServiceAffinity affinity{ServiceAffinity::Lifecycle};
    std::vector<ServiceDependency> required_dependencies;
    std::vector<ServiceDependency> optional_dependencies;
    std::function<DWORD(std::stop_token)> start;
    std::function<void()> stop;
};

struct ServiceDependencySnapshot {
    std::string id;
    std::uint32_t minimum_version{1};
    bool optional{};
    bool resolved{};
    std::uint32_t resolved_version{};
    ServiceState state{ServiceState::Registered};
};

struct ServiceSnapshot {
    std::string id;
    std::uint32_t version{};
    ServiceLifetime lifetime{ServiceLifetime::Singleton};
    ServiceStartup startup{ServiceStartup::Blocking};
    ServiceAffinity affinity{ServiceAffinity::Lifecycle};
    ServiceState state{ServiceState::Registered};
    DWORD error{ERROR_SUCCESS};
    std::chrono::microseconds startup_duration{};
    // Execution evidence captured by the production composition root. A zero
    // thread id means that the callback has not run yet.
    std::uint64_t start_thread_id{};
    std::uint64_t stop_thread_id{};
    std::chrono::microseconds start_queue_delay{};
    std::chrono::microseconds stop_queue_delay{};
    bool affinity_bypassed{};
    std::vector<ServiceDependencySnapshot> dependencies;
    std::vector<std::string> failure_chain;
};

struct ServiceAffinityExecutors {
    // The executor owns the dispatch and waits for completion. Implementations
    // should return ERROR_NOT_READY when the requested domain is not available.
    std::function<DWORD(
        ServiceAffinity, std::function<DWORD(std::stop_token)>, std::stop_token)> start;
    std::function<void(ServiceAffinity, std::function<void()>)> stop;
};

struct ServiceFailureSnapshot {
    std::string service_id;
    DWORD error{ERROR_SUCCESS};
    std::string caused_by;
};

struct ServiceGraphSnapshot {
    bool built{};
    bool stop_requested{};
    bool startup_active{};
    bool blocking_startup_complete{};
    bool async_startup_complete{};
    DWORD error{ERROR_SUCCESS};
    DWORD async_startup_error{ERROR_SUCCESS};
    std::vector<ServiceFailureSnapshot> failures;
    std::vector<ServiceSnapshot> services;
};

class ServiceGraph final {
public:
    ServiceGraph();
    ~ServiceGraph();

    ServiceGraph(const ServiceGraph&) = delete;
    ServiceGraph& operator=(const ServiceGraph&) = delete;
    ServiceGraph(ServiceGraph&&) = delete;
    ServiceGraph& operator=(ServiceGraph&&) = delete;

    [[nodiscard]] DWORD Register(ServiceDescriptor descriptor) noexcept;
    // Must be set before Build. Leaving it unset keeps the standalone/direct
    // execution behavior used by unit fixtures.
    void SetAffinityExecutors(ServiceAffinityExecutors executors) noexcept;
    [[nodiscard]] DWORD Build() noexcept;
    [[nodiscard]] DWORD StartAll(std::stop_token stop_token = {}) noexcept;
    [[nodiscard]] DWORD StartService(
        std::string_view service_id, std::stop_token stop_token = {}) noexcept;
    [[nodiscard]] DWORD WaitForAsync(std::stop_token stop_token = {}) noexcept;
    // Cancels incomplete starts but retains Ready services for reverse-order StopAll.
    void CancelStartup() noexcept;
    void StopAll() noexcept;

    [[nodiscard]] ServiceGraphSnapshot Snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
