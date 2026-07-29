#pragma once

#include "anomaly/dispatcher.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace anomaly {

enum class ExecutionDomain : std::uint8_t {
    Lifecycle,
    Worker,
    Game,
    Render,
};

struct DomainTaskHandle {
    ExecutionDomain domain{ExecutionDomain::Lifecycle};
    std::size_t lane{};
    TaskHandle task;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(task);
    }
    friend bool operator==(DomainTaskHandle, DomainTaskHandle) = default;
};

struct RuntimeDispatchersOptions {
    std::size_t worker_threads{1};
    std::size_t terminal_history_capacity{1024};
};

class RuntimeDispatchers final {
public:
    using Callback = Dispatcher::Callback;

    // The owner must outlive callbacks; destroying this object from one of its
    // own Lifecycle/Game/Render/Worker callbacks is invalid.
    explicit RuntimeDispatchers(RuntimeDispatchersOptions options = {});
    ~RuntimeDispatchers();

    RuntimeDispatchers(const RuntimeDispatchers&) = delete;
    RuntimeDispatchers& operator=(const RuntimeDispatchers&) = delete;
    RuntimeDispatchers(RuntimeDispatchers&&) = delete;
    RuntimeDispatchers& operator=(RuntimeDispatchers&&) = delete;

    [[nodiscard]] bool StartWorkers() noexcept;
    // Rejects new external posts and cancels queued work while keeping domain
    // workers alive for lifecycle stop callbacks that still need affinity.
    void CloseExternalPosts() noexcept;
    [[nodiscard]] bool DrainExternalWork(std::chrono::milliseconds timeout) noexcept;
    void RequestStop() noexcept;
    void JoinWorkers() noexcept;
    // Bind the lifecycle dispatcher before startup callbacks begin. Runtime
    // startup runs on this same thread before RunLifecycle enters its pump.
    void BindLifecycleToCurrentThread() noexcept;

    [[nodiscard]] DomainTaskHandle Post(
        ExecutionDomain domain,
        std::string owner,
        std::uint64_t generation,
        Callback callback);
    // Submit one bounded synchronous operation to a domain. Calls made from
    // the already-bound domain execute inline; otherwise the operation is
    // queued and the caller waits up to timeout. This is used by ServiceGraph
    // start/stop affinity enforcement and never runs repository work inline.
    [[nodiscard]] DWORD Invoke(
        ExecutionDomain domain,
        Callback callback,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));
    // Invoke can return ERROR_TIMEOUT after a callback has entered Running;
    // Cancel only removes queued work. Callers that are about to destroy
    // objects captured by an invocation must first wait for this drain after
    // stopping new submissions through their own lifecycle gate.
    [[nodiscard]] bool DrainInvocations(
        std::chrono::milliseconds timeout = std::chrono::seconds(30));
    // Cancels queued invocation callbacks while leaving the admission gate
    // open for affinity work needed by service teardown.
    void CancelQueuedInvocations() noexcept;
    // Closes the invocation admission gate and cancels queued invocation
    // callbacks without stopping unrelated dispatcher domains.
    void CloseInvocations() noexcept;
    [[nodiscard]] bool Cancel(DomainTaskHandle handle);
    [[nodiscard]] std::size_t CancelOwnerGeneration(
        std::string_view owner, std::uint64_t generation);
    void SetGeneration(std::string owner, std::uint64_t generation);
    [[nodiscard]] bool Drain(
        std::string_view owner,
        std::uint64_t generation,
        std::chrono::milliseconds timeout);

    void RunLifecycle(std::stop_token stop_token) noexcept;
    [[nodiscard]] std::size_t PumpGame(
        std::size_t max_callbacks = (std::numeric_limits<std::size_t>::max)()) noexcept;
    [[nodiscard]] std::size_t PumpRender(
        std::size_t max_callbacks = (std::numeric_limits<std::size_t>::max)()) noexcept;

    [[nodiscard]] std::optional<TaskSnapshot> GetTask(DomainTaskHandle handle) const;
    [[nodiscard]] std::thread::id BoundThread(
        ExecutionDomain domain, std::size_t lane = 0) const noexcept;
    [[nodiscard]] std::size_t WorkerCount() const noexcept;
    [[nodiscard]] bool IsAccepting() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
