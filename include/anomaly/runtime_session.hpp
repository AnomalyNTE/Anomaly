#pragma once

#include "anomaly/core_api.h"
#include "anomaly/runtime_dispatchers.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace anomaly {

class ServiceGraph;

struct RuntimeStartContext {
    std::uint32_t bootstrap_abi_version{ANOMALY_BOOTSTRAP_ABI_VERSION};
    AnomalyBootstrapType bootstrap_type{ANOMALY_BOOTSTRAP_TYPE_UNKNOWN};
    HMODULE bootstrap_module{};
    HMODULE game_module{};
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    // Borrowed on input. RuntimeSession duplicates the handle before Start returns.
    HANDLE external_stop_event{};
};

struct RuntimeWorker {
    std::string name;
    std::function<DWORD(std::stop_token)> run;
};

struct RuntimeSessionSnapshot {
    AnomalyRuntimeState state{ANOMALY_RUNTIME_STATE_DORMANT};
    DWORD last_error{ERROR_SUCCESS};
    std::uint64_t generation{};
};

struct RuntimeSessionOptions {
    std::shared_ptr<ServiceGraph> services;
    RuntimeDispatchersOptions dispatcher_options{};
    std::function<DWORD(std::stop_token)> initialize;
    // Invoked after runtime workers have joined and while dispatcher domains
    // remain alive, so plugin generations can drain before service shutdown.
    std::function<DWORD(std::chrono::milliseconds)> stop_plugins;
    std::chrono::milliseconds plugin_stop_timeout{std::chrono::seconds(1)};
    std::function<void()> shutdown;
    // Invoked exactly once after the final Stopped state is published and
    // before the lifecycle thread exits. Join synchronizes its completion. An
    // observer exception is contained and does not change the final snapshot.
    // The observer must not call Join or destroy its owning session.
    std::function<void(RuntimeSessionSnapshot)> on_stopped;
    std::vector<RuntimeWorker> workers;
};

class RuntimeSession final {
public:
    // The session must outlive work submitted through Dispatchers(). Destroying
    // the session from one of its dispatcher callbacks is invalid.
    RuntimeSession(RuntimeStartContext start_context, RuntimeSessionOptions options);
    ~RuntimeSession();

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    RuntimeSession(RuntimeSession&&) = delete;
    RuntimeSession& operator=(RuntimeSession&&) = delete;

    [[nodiscard]] DWORD Start() noexcept;
    void RequestStop() noexcept;
    [[nodiscard]] bool WaitForStop(std::chrono::milliseconds timeout) const noexcept;
    void Join() noexcept;

    [[nodiscard]] RuntimeSessionSnapshot Snapshot() const noexcept;
    [[nodiscard]] const RuntimeStartContext& StartContext() const noexcept;
    [[nodiscard]] RuntimeDispatchers& Dispatchers() noexcept;
    [[nodiscard]] const RuntimeDispatchers& Dispatchers() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
