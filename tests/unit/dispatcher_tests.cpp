#include "anomaly/dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool TestFifoAndReentrantPost() {
    anomaly::Dispatcher dispatcher;
    std::vector<int> order;
    const auto first = dispatcher.Post("fifo", 1, [&] {
        order.push_back(1);
        static_cast<void>(dispatcher.Post("fifo", 1, [&] { order.push_back(4); }));
    });
    const auto second = dispatcher.Post("fifo", 1, [&] { order.push_back(2); });
    const auto third = dispatcher.Post("fifo", 1, [&] { order.push_back(3); });

    return Check(dispatcher.Pump() == 4, "Pump did not invoke all FIFO callbacks") &&
        Check(order == std::vector<int>({1, 2, 3, 4}), "Dispatcher violated FIFO order") &&
        Check(dispatcher.GetState(first) == anomaly::TaskState::Completed,
              "First FIFO task did not complete") &&
        Check(dispatcher.GetState(second) == anomaly::TaskState::Completed,
              "Second FIFO task did not complete") &&
        Check(dispatcher.GetState(third) == anomaly::TaskState::Completed,
              "Third FIFO task did not complete");
}

bool TestThreadAffinity() {
    anomaly::Dispatcher dispatcher;
    std::thread::id callback_thread;
    std::thread producer([&] {
        static_cast<void>(dispatcher.Post("affinity", 1, [&] {
            callback_thread = std::this_thread::get_id();
        }));
    });
    producer.join();

    const auto pump_thread = std::this_thread::get_id();
    dispatcher.BindToCurrentThread();
    return Check(dispatcher.IsCurrentThread(), "Dispatcher did not bind to the pump thread") &&
        Check(dispatcher.BoundThread() == pump_thread, "Bound thread ID is incorrect") &&
        Check(dispatcher.Pump() == 1, "Affinity callback was not invoked") &&
        Check(callback_thread == pump_thread, "Callback ran outside the bound thread");
}

bool TestCancelBeforeStart() {
    anomaly::Dispatcher dispatcher;
    std::atomic_bool called{};
    const auto handle = dispatcher.Post("cancel", 7, [&] { called.store(true); });

    return Check(dispatcher.Cancel(handle), "Queued task was not cancelled") &&
        Check(!dispatcher.Cancel(handle), "Cancellation was not idempotently rejected") &&
        Check(dispatcher.GetState(handle) == anomaly::TaskState::Cancelled,
              "Cancelled task has the wrong state") &&
        Check(dispatcher.Drain("cancel", 7, 0ms), "Cancelled task did not drain") &&
        Check(dispatcher.Pump() == 0, "Cancelled task counted as invoked") &&
        Check(!called.load(), "Cancelled callback was invoked");
}

bool TestOldGenerationIsDiscarded() {
    anomaly::Dispatcher dispatcher;
    std::vector<int> calls;
    dispatcher.SetGeneration("plugin", 1);
    const auto old_queued = dispatcher.Post("plugin", 1, [&] { calls.push_back(1); });
    dispatcher.SetGeneration("plugin", 2);
    const auto old_late = dispatcher.Post("plugin", 1, [&] { calls.push_back(2); });
    const auto current = dispatcher.Post("plugin", 2, [&] { calls.push_back(3); });

    return Check(dispatcher.Pump() == 1, "Generation filter invoked the wrong task count") &&
        Check(calls == std::vector<int>({3}), "An old generation callback was invoked") &&
        Check(dispatcher.GetState(old_queued) == anomaly::TaskState::Cancelled,
              "Queued old generation task was not cancelled") &&
        Check(dispatcher.GetState(old_late) == anomaly::TaskState::Cancelled,
              "Late old generation task was not cancelled") &&
        Check(dispatcher.GetState(current) == anomaly::TaskState::Completed,
              "Current generation task did not complete") &&
        Check(dispatcher.Drain("plugin", 1, 0ms), "Old generation did not drain");
}

bool TestInFlightDrain() {
    anomaly::Dispatcher dispatcher;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool release{};
    const auto handle = dispatcher.Post("slow", 4, [&] {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
    });

    std::thread pump([&] { static_cast<void>(dispatcher.Pump()); });
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return entered; })) {
            release = true;
            condition.notify_all();
            pump.join();
            return Check(false, "In-flight callback did not start");
        }
    }

    bool result = Check(dispatcher.GetState(handle) == anomaly::TaskState::Running,
                        "In-flight task did not report Running") &&
        Check(!dispatcher.Drain("slow", 4, 20ms), "Drain ignored an in-flight callback");
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    condition.notify_all();
    result = Check(dispatcher.Drain("slow", 4, 2s),
                   "Drain did not observe callback completion") && result;
    pump.join();
    return Check(dispatcher.GetState(handle) == anomaly::TaskState::Completed,
                 "Drained task did not complete") && result;
}

bool TestExceptionIsolation() {
    anomaly::Dispatcher dispatcher;
    bool survivor_called{};
    const auto failed = dispatcher.Post("fault", 3, [] {
        throw std::runtime_error("fixture failure");
    });
    const auto survivor = dispatcher.Post("fault", 3, [&] { survivor_called = true; });

    const auto invoked = dispatcher.Pump();
    const auto failures = dispatcher.Failures();
    return Check(invoked == 2, "Exception stopped the pump") &&
        Check(survivor_called, "Task after exception was not invoked") &&
        Check(dispatcher.GetState(failed) == anomaly::TaskState::Failed,
              "Throwing task did not report Failed") &&
        Check(dispatcher.GetState(survivor) == anomaly::TaskState::Completed,
              "Task after exception did not complete") &&
        Check(failures.size() == 1, "Exception was not recorded exactly once") &&
        Check(failures.front().handle == failed, "Failure record has the wrong handle") &&
        Check(failures.front().message == "fixture failure",
              "Failure record lost the exception message") &&
        Check(failures.front().exception != nullptr,
              "Failure record lost the exception pointer");
}

bool TestOwnerIsolation() {
    anomaly::Dispatcher dispatcher;
    std::vector<int> calls;
    const auto owner_a = dispatcher.Post("owner-a", 9, [&] { calls.push_back(1); });
    const auto owner_b = dispatcher.Post("owner-b", 9, [&] { calls.push_back(2); });

    return Check(dispatcher.CancelOwnerGeneration("owner-a", 9) == 1,
                 "Owner cancellation count is incorrect") &&
        Check(dispatcher.Drain("owner-a", 9, 0ms), "Cancelled owner did not drain") &&
        Check(!dispatcher.Drain("owner-b", 9, 0ms),
              "Drain crossed the owner boundary") &&
        Check(dispatcher.Pump() == 1, "Owner cancellation affected another owner") &&
        Check(calls == std::vector<int>({2}), "Wrong owner callback was invoked") &&
        Check(dispatcher.GetState(owner_a) == anomaly::TaskState::Cancelled,
              "Cancelled owner task has the wrong state") &&
        Check(dispatcher.GetState(owner_b) == anomaly::TaskState::Completed,
              "Independent owner task did not complete");
}

bool TestTerminalHistoryIsBounded() {
    anomaly::Dispatcher dispatcher({8});
    std::vector<anomaly::TaskHandle> handles;
    for (int index = 0; index < 64; ++index) {
        handles.push_back(dispatcher.Post("history", 1, [] {}));
    }
    if (!Check(dispatcher.Pump() == handles.size(),
               "History fixture did not execute every task")) {
        return false;
    }

    return Check(dispatcher.TrackedTaskCount() == 8,
                 "Terminal task history exceeded its configured capacity") &&
        Check(!dispatcher.GetState(handles.front()).has_value(),
              "Expired terminal task remained queryable") &&
        Check(dispatcher.GetState(handles.back()) == anomaly::TaskState::Completed,
              "Newest terminal task was evicted");
}

bool TestBlockingRunLoop() {
    anomaly::Dispatcher dispatcher;
    std::mutex mutex;
    std::condition_variable condition;
    bool called{};
    std::thread::id callback_thread;
    std::jthread executor([&](std::stop_token stop_token) {
        dispatcher.Run(stop_token);
    });

    const auto handle = dispatcher.Post("run-loop", 1, [&] {
        {
            std::scoped_lock lock(mutex);
            called = true;
            callback_thread = std::this_thread::get_id();
        }
        condition.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return called; })) {
            executor.request_stop();
            executor.join();
            return Check(false, "Blocking run loop did not wake for posted work");
        }
    }
    executor.request_stop();
    executor.join();

    return Check(dispatcher.GetState(handle) == anomaly::TaskState::Completed,
                 "Blocking run loop task did not complete") &&
        Check(callback_thread == dispatcher.BoundThread(),
              "Blocking run loop callback used the wrong thread");
}

bool TestCancelAllPending() {
    anomaly::Dispatcher dispatcher;
    std::atomic_int called{};
    std::vector<anomaly::TaskHandle> handles;
    for (int index = 0; index < 12; ++index) {
        handles.push_back(dispatcher.Post("stop", 1, [&] { ++called; }));
    }
    bool result = Check(dispatcher.CancelPending() == handles.size(),
                        "CancelPending returned the wrong count") &&
        Check(dispatcher.CancelPending() == 0,
              "CancelPending was not idempotent") &&
        Check(dispatcher.Pump() == 0, "Cancelled pending work was invoked") &&
        Check(called.load() == 0, "Cancelled pending callback ran");
    for (const auto handle : handles) {
        result = Check(dispatcher.GetState(handle) == anomaly::TaskState::Cancelled,
                       "CancelPending left a non-cancelled task") && result;
    }
    return result;
}

bool TestZeroPumpDoesNotBindThread() {
    anomaly::Dispatcher dispatcher;
    std::thread probe([&] {
        static_cast<void>(dispatcher.Pump(0));
    });
    probe.join();
    const auto main_thread = std::this_thread::get_id();
    dispatcher.BindToCurrentThread();
    return Check(dispatcher.BoundThread() == main_thread,
                 "Pump(0) bound dispatcher affinity to a probe thread");
}

}  // namespace

int main() {
    if (!TestFifoAndReentrantPost()) return 1;
    if (!TestThreadAffinity()) return 2;
    if (!TestCancelBeforeStart()) return 3;
    if (!TestOldGenerationIsDiscarded()) return 4;
    if (!TestInFlightDrain()) return 5;
    if (!TestExceptionIsolation()) return 6;
    if (!TestOwnerIsolation()) return 7;
    if (!TestTerminalHistoryIsBounded()) return 8;
    if (!TestBlockingRunLoop()) return 9;
    if (!TestCancelAllPending()) return 10;
    if (!TestZeroPumpDoesNotBindThread()) return 11;
    return 0;
}
