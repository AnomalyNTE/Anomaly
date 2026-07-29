#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace anomaly {

struct TaskHandle {
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(TaskHandle, TaskHandle) = default;
};

enum class TaskState : std::uint8_t {
    Queued,
    Running,
    Completed,
    Cancelled,
    Failed,
};

struct TaskSnapshot {
    TaskHandle handle;
    std::string owner;
    std::uint64_t generation{};
    TaskState state{TaskState::Queued};
    std::string failure_message;
};

struct TaskFailure {
    TaskHandle handle;
    std::string owner;
    std::uint64_t generation{};
    std::string message;
    std::exception_ptr exception;
};

struct DispatcherOptions {
    std::size_t terminal_history_capacity{1024};
};

class Dispatcher final {
public:
    using Callback = std::function<void()>;

    explicit Dispatcher(DispatcherOptions options = {});
    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;

    [[nodiscard]] TaskHandle Post(
        std::string owner, std::uint64_t generation, Callback callback);
    [[nodiscard]] bool Cancel(TaskHandle handle);
    [[nodiscard]] std::size_t CancelPending() noexcept;
    [[nodiscard]] bool DrainAll(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t CancelOwnerGeneration(
        std::string_view owner, std::uint64_t generation);

    void SetGeneration(std::string owner, std::uint64_t generation);
    [[nodiscard]] bool Drain(
        std::string_view owner,
        std::uint64_t generation,
        std::chrono::milliseconds timeout);

    void BindToCurrentThread() noexcept;
    [[nodiscard]] bool IsCurrentThread() const noexcept;
    [[nodiscard]] std::thread::id BoundThread() const noexcept;

    // Returns the number of callbacks invoked. Cancelled queue entries do not count.
    [[nodiscard]] std::size_t Pump(
        std::size_t max_callbacks = (std::numeric_limits<std::size_t>::max)()) noexcept;

    // Binds to the current thread and pumps until stop is requested.
    void Run(std::stop_token stop_token) noexcept;

    [[nodiscard]] std::optional<TaskSnapshot> GetTask(TaskHandle handle) const;
    [[nodiscard]] std::optional<TaskState> GetState(TaskHandle handle) const;
    [[nodiscard]] std::vector<TaskFailure> Failures() const;
    [[nodiscard]] std::size_t TrackedTaskCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
