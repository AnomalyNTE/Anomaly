#include "anomaly/runtime_session.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kGameOwner = "game-fixture";
constexpr std::size_t kTickCount = 6;
constexpr std::size_t kCallbacksPerTick = 2;
constexpr std::size_t kGenerationSwitchTick = 3;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    void Reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(value_));
        }
        value_ = value;
    }

private:
    HANDLE value_{};
};

class CapturedThread final {
public:
    CapturedThread() = default;
    ~CapturedThread() {
        const HANDLE value = value_.load(std::memory_order_acquire);
        if (value != nullptr) static_cast<void>(CloseHandle(value));
    }

    CapturedThread(const CapturedThread&) = delete;
    CapturedThread& operator=(const CapturedThread&) = delete;

    [[nodiscard]] bool CaptureCurrent() noexcept {
        HANDLE duplicated{};
        if (DuplicateHandle(
                GetCurrentProcess(),
                GetCurrentThread(),
                GetCurrentProcess(),
                &duplicated,
                SYNCHRONIZE,
                FALSE,
                0) == FALSE) {
            return false;
        }

        HANDLE expected{};
        if (!value_.compare_exchange_strong(
                expected, duplicated, std::memory_order_release,
                std::memory_order_relaxed)) {
            static_cast<void>(CloseHandle(duplicated));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool WasCaptured() const noexcept {
        return value_.load(std::memory_order_acquire) != nullptr;
    }

    [[nodiscard]] bool HasExited() const noexcept {
        const HANDLE value = value_.load(std::memory_order_acquire);
        return value != nullptr && WaitForSingleObject(value, 0) == WAIT_OBJECT_0;
    }

private:
    std::atomic<HANDLE> value_{};
};

struct GameInvocation final {
    std::size_t tick{};
    std::size_t slot{};
    std::uint64_t generation{};

    friend bool operator==(const GameInvocation&, const GameInvocation&) = default;
};

anomaly::RuntimeStartContext StartContext() {
    anomaly::RuntimeStartContext result;
    result.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL;
    result.bootstrap_module = GetModuleHandleW(nullptr);
    result.game_module = GetModuleHandleW(nullptr);
    result.runtime_root = L".";
    result.log_directory = L".";
    return result;
}

bool WaitForEvent(HANDLE event, std::chrono::milliseconds timeout = 2s) noexcept {
    return WaitForSingleObject(event, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
}

DWORD WaitForStopToken(std::stop_token stop_token) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock(mutex);
    condition.wait(lock, stop_token, [] { return false; });
    return ERROR_SUCCESS;
}

bool IsTaskState(
    const anomaly::RuntimeDispatchers& dispatchers,
    anomaly::DomainTaskHandle handle,
    anomaly::TaskState expected) {
    const auto task = dispatchers.GetTask(handle);
    return task.has_value() && task->state == expected;
}

}  // namespace

int main() {
    UniqueHandle session_worker_started(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle lifecycle_ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle dispatcher_worker_ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!Check(session_worker_started && lifecycle_ready && dispatcher_worker_ready,
               "Failed to create fixture synchronization events")) {
        return 1;
    }

    CapturedThread lifecycle_thread;
    CapturedThread session_worker_thread;
    CapturedThread dispatcher_worker_thread;
    std::atomic_bool session_worker_exited{};
    std::atomic_bool shutdown_called{};
    std::atomic_bool shutdown_after_workers{};

    anomaly::RuntimeSessionOptions options;
    options.dispatcher_options.worker_threads = 1;
    options.dispatcher_options.terminal_history_capacity = 64;
    options.workers.push_back({
        "game-fixture-session-worker",
        [&](std::stop_token stop_token) {
            static_cast<void>(session_worker_thread.CaptureCurrent());
            static_cast<void>(SetEvent(session_worker_started.Get()));
            const DWORD result = WaitForStopToken(stop_token);
            session_worker_exited.store(true, std::memory_order_release);
            return result;
        }});
    options.shutdown = [&] {
        shutdown_after_workers.store(
            session_worker_exited.load(std::memory_order_acquire) &&
                dispatcher_worker_thread.HasExited(),
            std::memory_order_release);
        shutdown_called.store(true, std::memory_order_release);
    };

    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "RuntimeSession rejected fixture startup");
    if (!result) return 2;

    const auto lifecycle_probe = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Lifecycle,
        "game-fixture-lifecycle",
        1,
        [&] {
            static_cast<void>(lifecycle_thread.CaptureCurrent());
            static_cast<void>(SetEvent(lifecycle_ready.Get()));
        });
    result = Check(static_cast<bool>(lifecycle_probe),
                   "Lifecycle readiness probe was rejected") && result;
    result = Check(WaitForEvent(lifecycle_ready.Get()),
                   "RuntimeSession did not reach its lifecycle run loop") && result;
    result = Check(session.Dispatchers().Drain("game-fixture-lifecycle", 1, 2s),
                   "Lifecycle readiness probe did not drain") && result;
    result = Check(session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING,
                   "RuntimeSession was not Running at the first game tick") && result;
    result = Check(WaitForEvent(session_worker_started.Get()) &&
                       session_worker_thread.WasCaptured(),
                   "RuntimeSession worker did not start") && result;

    const auto dispatcher_worker_probe = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Worker,
        "game-fixture-dispatcher-worker",
        1,
        [&] {
            static_cast<void>(dispatcher_worker_thread.CaptureCurrent());
            static_cast<void>(SetEvent(dispatcher_worker_ready.Get()));
        });
    result = Check(static_cast<bool>(dispatcher_worker_probe),
                   "Dispatcher worker readiness probe was rejected") && result;
    result = Check(WaitForEvent(dispatcher_worker_ready.Get()),
                   "Dispatcher worker readiness probe did not run") && result;
    result = Check(
                 session.Dispatchers().Drain("game-fixture-dispatcher-worker", 1, 2s),
                 "Dispatcher worker readiness probe did not drain") && result;

    auto& dispatchers = session.Dispatchers();
    constexpr std::string_view kTickCallbackOwner = "game-fixture-tick-callback";
    dispatchers.SetGeneration(std::string(kTickCallbackOwner), 1);
    std::atomic_bool tick_enabled{true};
    std::mutex tick_mutex;
    std::vector<std::string> tick_trace;
    std::size_t plugin_updates{};
    const auto run_tick = [&] {
        if (!tick_enabled.load(std::memory_order_acquire)) return;
        static_cast<void>(dispatchers.PumpGame());
        std::scoped_lock lock(tick_mutex);
        if (tick_enabled.load(std::memory_order_relaxed)) {
            tick_trace.emplace_back("update");
            ++plugin_updates;
        }
    };
    const auto pump_before_update = dispatchers.Post(
        anomaly::ExecutionDomain::Game,
        std::string(kTickCallbackOwner),
        1,
        [&] { tick_trace.emplace_back("pump"); });
    result = Check(
                 static_cast<bool>(pump_before_update),
                 "Tick-order Game work was rejected") && result;
    run_tick();
    result = Check(
                 tick_trace == std::vector<std::string>{"pump", "update"} &&
                     plugin_updates == 1,
                 "Game dispatcher work did not run before the same-tick plugin update") && result;

    std::size_t disabled_pumps{};
    const auto disabled_tick_work = dispatchers.Post(
        anomaly::ExecutionDomain::Game,
        std::string(kTickCallbackOwner),
        1,
        [&] { ++disabled_pumps; });
    tick_enabled.store(false, std::memory_order_release);
    run_tick();
    result = Check(
                 static_cast<bool>(disabled_tick_work) && disabled_pumps == 0 &&
                     plugin_updates == 1 &&
                     IsTaskState(
                         dispatchers, disabled_tick_work, anomaly::TaskState::Queued),
                 "Disabled tick pumped Game work or updated plugins") && result;
    result = Check(
                 dispatchers.PumpGame() == 1 && disabled_pumps == 1 &&
                     dispatchers.Drain(kTickCallbackOwner, 1, 2s),
                 "Disabled-tick fixture work did not drain during cleanup") && result;

    dispatchers.SetGeneration(std::string(kGameOwner), 1);
    const std::thread::id game_thread = std::this_thread::get_id();
    bool wrong_game_thread{};
    int stale_invocations{};
    std::uint64_t generation = 1;
    std::vector<GameInvocation> expected;
    std::vector<GameInvocation> actual;
    std::vector<anomaly::DomainTaskHandle> completed_tasks;
    std::vector<anomaly::DomainTaskHandle> stale_tasks;

    for (std::size_t tick = 0; tick < kTickCount; ++tick) {
        if (tick == kGenerationSwitchTick) {
            for (std::size_t slot = 0; slot < kCallbacksPerTick; ++slot) {
                stale_tasks.push_back(dispatchers.Post(
                    anomaly::ExecutionDomain::Game,
                    std::string(kGameOwner),
                    1,
                    [&] { ++stale_invocations; }));
            }
            result = Check(
                         stale_tasks.size() == kCallbacksPerTick &&
                             stale_tasks[0] && stale_tasks[1],
                         "Generation-one queued work was rejected before the transition") &&
                result;

            dispatchers.SetGeneration(std::string(kGameOwner), 2);
            generation = 2;
            const auto rejected_old_generation = dispatchers.Post(
                anomaly::ExecutionDomain::Game,
                std::string(kGameOwner),
                1,
                [&] { ++stale_invocations; });
            result = Check(!rejected_old_generation,
                           "Closed generation accepted work after the transition") && result;
        }

        for (std::size_t slot = 0; slot < kCallbacksPerTick; ++slot) {
            const GameInvocation invocation{tick, slot, generation};
            expected.push_back(invocation);
            completed_tasks.push_back(dispatchers.Post(
                anomaly::ExecutionDomain::Game,
                std::string(kGameOwner),
                generation,
                [&, invocation] {
                    if (std::this_thread::get_id() != game_thread) wrong_game_thread = true;
                    actual.push_back(invocation);
                }));
            result = Check(static_cast<bool>(completed_tasks.back()),
                           "Game tick post was rejected while Running") && result;
        }

        result = Check(dispatchers.PumpGame(kCallbacksPerTick) == kCallbacksPerTick,
                       "A bounded game tick pumped the wrong callback count") && result;
    }

    result = Check(actual == expected,
                   "Game dispatcher did not preserve fixed-tick FIFO order") && result;
    result = Check(!wrong_game_thread &&
                       dispatchers.BoundThread(anomaly::ExecutionDomain::Game) == game_thread,
                   "Game dispatcher callbacks did not stay on the game anchor thread") && result;
    result = Check(stale_invocations == 0,
                   "Generation-one queued work ran after generation two became current") && result;
    for (const auto task : stale_tasks) {
        result = Check(
                     IsTaskState(dispatchers, task, anomaly::TaskState::Cancelled),
                     "Generation-one queued task was not marked Cancelled") && result;
    }
    for (const auto task : completed_tasks) {
        result = Check(
                     IsTaskState(dispatchers, task, anomaly::TaskState::Completed),
                     "Pumped game task was not marked Completed") && result;
    }
    result = Check(dispatchers.Drain(kGameOwner, 1, 2s) &&
                       dispatchers.Drain(kGameOwner, 2, 2s),
                   "Game generations did not drain after the fixed ticks") && result;

    session.RequestStop();
    result = Check(session.WaitForStop(2s),
                   "RuntimeSession did not stop after the game fixture") && result;
    session.Join();

    const auto rejected_after_stop = dispatchers.Post(
        anomaly::ExecutionDomain::Game,
        std::string(kGameOwner),
        2,
        [] {});
    const auto snapshot = session.Snapshot();
    result = Check(!rejected_after_stop && !dispatchers.IsAccepting(),
                   "Stopped RuntimeSession accepted Game dispatcher work") && result;
    result = Check(snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED &&
                       snapshot.last_error == ERROR_SUCCESS,
                   "RuntimeSession did not finish the fixture cleanly") && result;
    result = Check(shutdown_called.load(std::memory_order_acquire) &&
                       shutdown_after_workers.load(std::memory_order_acquire),
                   "RuntimeSession shutdown ran before owned workers exited") && result;
    result = Check(lifecycle_thread.HasExited() && session_worker_thread.HasExited() &&
                       dispatcher_worker_thread.HasExited(),
                   "RuntimeSession left an owned thread running") && result;
    return result ? 0 : 3;
}
