#include "anomaly/runtime_dispatchers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool TestOwnedLifecycleAndWorkerDomains() {
    anomaly::RuntimeDispatchers dispatchers({2, 32});
    if (!Check(dispatchers.StartWorkers(), "Worker dispatchers did not start")) return false;

    std::jthread lifecycle([&](std::stop_token stop_token) {
        dispatchers.RunLifecycle(stop_token);
    });
    std::mutex mutex;
    std::set<std::thread::id> worker_threads;
    std::thread::id lifecycle_thread;

    const auto lifecycle_task = dispatchers.Post(
        anomaly::ExecutionDomain::Lifecycle, "fixture", 1, [&] {
            lifecycle_thread = std::this_thread::get_id();
        });
    std::vector<anomaly::DomainTaskHandle> worker_tasks;
    for (int index = 0; index < 8; ++index) {
        worker_tasks.push_back(dispatchers.Post(
            anomaly::ExecutionDomain::Worker, "fixture", 1, [&] {
                std::scoped_lock lock(mutex);
                worker_threads.insert(std::this_thread::get_id());
            }));
    }

    bool result = Check(static_cast<bool>(lifecycle_task),
                        "Lifecycle post was rejected") &&
        Check(std::ranges::all_of(worker_tasks, [](auto task) {
            return static_cast<bool>(task);
        }), "Worker post was rejected") &&
        Check(dispatchers.Drain("fixture", 1, 2s),
              "Owned dispatch domains did not drain") &&
        Check(lifecycle_thread == dispatchers.BoundThread(
                                     anomaly::ExecutionDomain::Lifecycle),
              "Lifecycle callback ran on the wrong thread") &&
        Check(worker_threads.size() == 2,
              "Worker lanes did not use their owned threads");

    lifecycle.request_stop();
    lifecycle.join();
    dispatchers.RequestStop();
    dispatchers.JoinWorkers();
    return result;
}

bool TestExternalGameAndRenderAnchors() {
    anomaly::RuntimeDispatchers dispatchers;
    std::thread::id game_thread;
    std::thread::id render_thread;
    const auto game = dispatchers.Post(anomaly::ExecutionDomain::Game, "anchors", 7, [&] {
        game_thread = std::this_thread::get_id();
    });
    const auto render = dispatchers.Post(
        anomaly::ExecutionDomain::Render, "anchors", 7, [&] {
            render_thread = std::this_thread::get_id();
        });

    const auto expected_game = std::this_thread::get_id();
    bool result = Check(dispatchers.PumpGame() == 1, "Game anchor did not pump") &&
        Check(game_thread == expected_game, "Game callback used the wrong thread");
    std::thread render_anchor([&] {
        static_cast<void>(dispatchers.PumpRender());
    });
    render_anchor.join();

    result = Check(dispatchers.GetTask(game)->state == anomaly::TaskState::Completed,
                   "Game task did not complete") &&
        Check(dispatchers.GetTask(render)->state == anomaly::TaskState::Completed,
              "Render task did not complete") &&
        Check(render_thread == dispatchers.BoundThread(anomaly::ExecutionDomain::Render),
              "Render callback used the wrong thread") && result;
    dispatchers.RequestStop();
    return result;
}

bool TestGenerationCancellationAcrossDomains() {
    anomaly::RuntimeDispatchers dispatchers({2, 32});
    if (!Check(dispatchers.StartWorkers(), "Cancellation workers did not start")) return false;
    dispatchers.SetGeneration("plugin", 1);
    std::atomic_int called{};
    const auto game = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "plugin", 1, [&] { ++called; });
    const auto render = dispatchers.Post(
        anomaly::ExecutionDomain::Render, "plugin", 1, [&] { ++called; });
    dispatchers.SetGeneration("plugin", 2);

    bool result = Check(dispatchers.PumpGame() == 0 && dispatchers.PumpRender() == 0,
                        "Old generation reached an external anchor") &&
        Check(called.load() == 0, "Old generation callback was invoked") &&
        Check(dispatchers.GetTask(game)->state == anomaly::TaskState::Cancelled,
              "Game generation was not cancelled") &&
        Check(dispatchers.GetTask(render)->state == anomaly::TaskState::Cancelled,
              "Render generation was not cancelled") &&
        Check(dispatchers.Drain("plugin", 1, 1s),
              "Cancelled generation did not drain");

    dispatchers.RequestStop();
    dispatchers.JoinWorkers();
    const auto rejected = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "plugin", 2, [] {});
    return Check(!rejected, "Stopped dispatchers accepted new work") && result;
}

bool TestStopCancelsPendingAcrossDomains() {
    anomaly::RuntimeDispatchers dispatchers({2, 32});
    if (!Check(dispatchers.StartWorkers(), "Stop workers did not start")) return false;
    std::atomic_int called{};
    const auto game = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "stop", 1, [&] { ++called; });
    const auto render = dispatchers.Post(
        anomaly::ExecutionDomain::Render, "stop", 1, [&] { ++called; });
    dispatchers.RequestStop();
    dispatchers.JoinWorkers();

    return Check(dispatchers.PumpGame() == 0 && dispatchers.PumpRender() == 0,
                 "Stopped external domains invoked pending work") &&
        Check(called.load() == 0, "Stopped dispatcher invoked a pending callback") &&
        Check(dispatchers.GetTask(game)->state == anomaly::TaskState::Cancelled,
              "Stopped game task was not cancelled") &&
        Check(dispatchers.GetTask(render)->state == anomaly::TaskState::Cancelled,
              "Stopped render task was not cancelled");
}

bool TestDrainObservesCrossDomainPosts() {
    anomaly::RuntimeDispatchers dispatchers({1, 32});
    if (!Check(dispatchers.StartWorkers(), "Cross-domain worker did not start")) return false;
    std::atomic_bool lifecycle_called{};
    const auto worker = dispatchers.Post(
        anomaly::ExecutionDomain::Worker, "cross-domain", 1, [&] {
            static_cast<void>(dispatchers.Post(
                anomaly::ExecutionDomain::Lifecycle, "cross-domain", 1, [&] {
                    lifecycle_called.store(true);
                }));
        });
    std::jthread lifecycle([&](std::stop_token stop_token) {
        dispatchers.RunLifecycle(stop_token);
    });

    bool result = Check(static_cast<bool>(worker),
                        "Cross-domain worker post was rejected") &&
        Check(dispatchers.Drain("cross-domain", 1, 2s),
              "Drain returned before a cross-domain post settled") &&
        Check(lifecycle_called.load(),
              "Drain missed work posted into an earlier domain");
    lifecycle.request_stop();
    lifecycle.join();
    dispatchers.RequestStop();
    dispatchers.JoinWorkers();
    return result;
}

bool TestConcurrentJoinWaitsForWorkers() {
    anomaly::RuntimeDispatchers dispatchers({1, 32});
    if (!Check(dispatchers.StartWorkers(), "Join worker did not start")) return false;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool release{};
    static_cast<void>(dispatchers.Post(
        anomaly::ExecutionDomain::Worker, "join", 1, [&] {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }));
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return entered; })) {
            dispatchers.RequestStop();
            dispatchers.JoinWorkers();
            return Check(false, "Join worker callback did not start");
        }
    }

    std::atomic_int joined{};
    std::thread first([&] {
        dispatchers.JoinWorkers();
        ++joined;
    });
    std::thread second([&] {
        dispatchers.JoinWorkers();
        ++joined;
    });
    std::this_thread::sleep_for(20ms);
    bool result = Check(joined.load() == 0,
                        "A concurrent Join returned before the worker exited");
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    condition.notify_all();
    first.join();
    second.join();
    return Check(joined.load() == 2,
                 "Concurrent Join callers did not observe worker exit") && result;
}

bool TestWorkerSelfJoinDefersToExternalJoin() {
    anomaly::RuntimeDispatchers dispatchers({1, 32});
    if (!Check(dispatchers.StartWorkers(), "Self-join worker did not start")) return false;
    std::mutex mutex;
    std::condition_variable condition;
    bool returned{};
    static_cast<void>(dispatchers.Post(
        anomaly::ExecutionDomain::Worker, "self-join", 1, [&] {
            dispatchers.JoinWorkers();
            {
                std::scoped_lock lock(mutex);
                returned = true;
            }
            condition.notify_all();
        }));
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return returned; })) {
            return Check(false, "Worker self-Join deadlocked");
        }
    }
    dispatchers.JoinWorkers();
    return Check(true, "External Join did not settle a deferred self-Join");
}

bool TestUnrelatedPostsDoNotDelayDrain() {
    anomaly::RuntimeDispatchers dispatchers;
    std::atomic_bool keep_posting{true};
    std::thread producer([&] {
        while (keep_posting.load()) {
            const auto handle = dispatchers.Post(
                anomaly::ExecutionDomain::Game, "unrelated", 9, [] {});
            if (handle) static_cast<void>(dispatchers.Cancel(handle));
        }
    });
    const bool drained = dispatchers.Drain("target", 1, 500ms);
    keep_posting.store(false);
    producer.join();
    dispatchers.RequestStop();
    return Check(drained, "Unrelated owner traffic delayed Drain");
}

bool TestInvokeTimeoutRequiresCompletionDrain() {
    anomaly::RuntimeDispatchers dispatchers;
    std::jthread lifecycle([&](std::stop_token stop_token) {
        dispatchers.RunLifecycle(stop_token);
    });

    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool release{};
    std::atomic_bool finished{};
    std::atomic<DWORD> invoke_result{ERROR_SUCCESS};
    std::thread invoker([&] {
        invoke_result.store(dispatchers.Invoke(
            anomaly::ExecutionDomain::Lifecycle,
            [&] {
                std::unique_lock lock(mutex);
                entered = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release; });
                finished.store(true, std::memory_order_release);
            },
            50ms));
    });

    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return entered; })) {
            invoker.join();
            lifecycle.request_stop();
            lifecycle.join();
            return Check(false, "Invoke callback did not enter Running");
        }
    }
    invoker.join();
    bool result = Check(invoke_result.load() == ERROR_TIMEOUT,
                        "Invoke did not report the bounded timeout") &&
        Check(!dispatchers.DrainInvocations(20ms),
              "Invocation drain returned while a Running callback was blocked") &&
        Check(!finished.load(std::memory_order_acquire),
              "Running callback completed before its release gate");

    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    condition.notify_all();
    result = Check(dispatchers.DrainInvocations(2s),
                   "Invocation drain did not observe callback completion") && result;
    result = Check(finished.load(std::memory_order_acquire),
                   "Timed-out invocation callback did not finish") && result;

    lifecycle.request_stop();
    lifecycle.join();
    dispatchers.RequestStop();
    return result;
}

bool TestQueuedInvokeCancellationSettlesTracker() {
    anomaly::RuntimeDispatchers dispatchers;
    std::jthread lifecycle([&](std::stop_token stop_token) {
        dispatchers.RunLifecycle(stop_token);
    });

    std::mutex mutex;
    std::condition_variable condition;
    bool blocker_entered{};
    bool release_blocker{};
    static_cast<void>(dispatchers.Post(
        anomaly::ExecutionDomain::Lifecycle, "invoke-blocker", 1, [&] {
            std::unique_lock lock(mutex);
            blocker_entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release_blocker; });
        }));
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return blocker_entered; })) {
            lifecycle.request_stop();
            lifecycle.join();
            dispatchers.RequestStop();
            return Check(false, "Invoke cancellation blocker did not enter");
        }
    }

    std::atomic<DWORD> invoke_result{ERROR_SUCCESS};
    std::atomic_bool callback_called{};
    std::thread invoker([&] {
        invoke_result.store(dispatchers.Invoke(
            anomaly::ExecutionDomain::Lifecycle,
            [&] { callback_called.store(true, std::memory_order_release); },
            5s));
    });
    // Let the invoker publish its ticket and enqueue behind the blocker before
    // the stop path cancels queued work.
    std::this_thread::sleep_for(50ms);
    dispatchers.RequestStop();
    {
        std::scoped_lock lock(mutex);
        release_blocker = true;
    }
    condition.notify_all();
    invoker.join();
    lifecycle.join();

    bool result = Check(invoke_result.load() == ERROR_CANCELLED,
                        "Queued Invoke did not settle as cancelled") &&
        Check(!callback_called.load(std::memory_order_acquire),
              "Cancelled queued Invoke callback was invoked") &&
        Check(dispatchers.DrainInvocations(2s),
              "Cancelled queued Invoke left an active tracker ticket");
    return result;
}

bool TestQueuedInvokeCancellationKeepsAdmissionOpen() {
    anomaly::RuntimeDispatchers dispatchers;
    std::jthread lifecycle([&](std::stop_token stop_token) {
        dispatchers.RunLifecycle(stop_token);
    });

    std::mutex mutex;
    std::condition_variable condition;
    bool blocker_entered{};
    bool release_blocker{};
    static_cast<void>(dispatchers.Post(
        anomaly::ExecutionDomain::Lifecycle, "invoke-admission-blocker", 1, [&] {
            std::unique_lock lock(mutex);
            blocker_entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release_blocker; });
        }));
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return blocker_entered; })) {
            lifecycle.request_stop();
            lifecycle.join();
            dispatchers.RequestStop();
            return Check(false, "Admission blocker did not enter");
        }
    }

    std::atomic<DWORD> cancelled_result{ERROR_SUCCESS};
    std::atomic_bool cancelled_callback_called{};
    std::thread invoker([&] {
        cancelled_result.store(dispatchers.Invoke(
            anomaly::ExecutionDomain::Lifecycle,
            [&] { cancelled_callback_called.store(true, std::memory_order_release); },
            5s));
    });
    std::this_thread::sleep_for(50ms);
    dispatchers.CancelQueuedInvocations();
    {
        std::scoped_lock lock(mutex);
        release_blocker = true;
    }
    condition.notify_all();
    invoker.join();

    bool result = Check(cancelled_result.load() == ERROR_CANCELLED,
                        "Queued invocation was not cancelled by the targeted drain") &&
        Check(!cancelled_callback_called.load(std::memory_order_acquire),
              "Targeted invocation cancellation invoked user callback") &&
        Check(dispatchers.DrainInvocations(2s),
              "Targeted invocation cancellation left tracker work active");

    std::atomic_bool admitted_callback_called{};
    const DWORD admitted_result = dispatchers.Invoke(
        anomaly::ExecutionDomain::Lifecycle,
        [&] { admitted_callback_called.store(true, std::memory_order_release); }, 2s);
    result = Check(admitted_result == ERROR_SUCCESS &&
                       admitted_callback_called.load(std::memory_order_acquire),
                   "Targeted cancellation closed admission for later affinity work") && result;

    dispatchers.CloseInvocations();
    const DWORD closed_result = dispatchers.Invoke(
        anomaly::ExecutionDomain::Lifecycle, [] {}, 20ms);
    result = Check(closed_result == ERROR_CANCELLED,
                   "Closed invocation gate accepted new work") && result;
    lifecycle.request_stop();
    lifecycle.join();
    dispatchers.RequestStop();
    return result;
}

bool TestGenerationTransitionAllowsDestructorReentry() {
    anomaly::RuntimeDispatchers dispatchers;
    struct ReenterOnDestroy {
        anomaly::RuntimeDispatchers* dispatchers{};
        ~ReenterOnDestroy() {
            dispatchers->SetGeneration("reentrant", 3);
        }
    };

    dispatchers.SetGeneration("reentrant", 1);
    auto reenter = std::make_shared<ReenterOnDestroy>();
    reenter->dispatchers = &dispatchers;
    const auto old = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "reentrant", 1, [reenter] {});
    reenter.reset();
    dispatchers.SetGeneration("reentrant", 2);

    std::atomic_bool current_called{};
    const auto stale = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "reentrant", 2, [] {});
    const auto current = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "reentrant", 3, [&] {
            current_called.store(true);
        });
    bool result = Check(dispatchers.PumpGame() == 1,
                        "Reentrant generation transition invoked the wrong task count") &&
        Check(dispatchers.GetTask(old)->state == anomaly::TaskState::Cancelled,
              "Reentrant transition did not cancel the old generation") &&
        Check(!stale, "Outer generation reopened after a reentrant transition") &&
        Check(current && current_called.load(),
              "Reentrant generation did not become current");
    dispatchers.RequestStop();
    return result;
}

bool TestGenerationTransitionAllowsCrossThreadDestructorReentry() {
    anomaly::RuntimeDispatchers dispatchers;
    struct TransitionSync {
        std::mutex mutex;
        std::condition_variable condition;
        bool destruction_started{};
        bool transition_returned{};
        bool timed_out{};
    };
    struct WaitForTransitionOnDestroy {
        std::shared_ptr<TransitionSync> sync;

        ~WaitForTransitionOnDestroy() {
            {
                std::scoped_lock lock(sync->mutex);
                sync->destruction_started = true;
            }
            sync->condition.notify_all();
            std::unique_lock lock(sync->mutex);
            if (!sync->condition.wait_for(
                    lock, 2s, [&] { return sync->transition_returned; })) {
                sync->timed_out = true;
            }
        }
    };

    dispatchers.SetGeneration("cross-thread-reentrant", 1);
    const auto sync = std::make_shared<TransitionSync>();
    auto wait_on_destroy = std::make_shared<WaitForTransitionOnDestroy>();
    wait_on_destroy->sync = sync;
    const auto old = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "cross-thread-reentrant", 1,
        [wait_on_destroy] {});
    wait_on_destroy.reset();

    std::thread concurrent_transition([&] {
        {
            std::unique_lock lock(sync->mutex);
            sync->condition.wait(lock, [&] { return sync->destruction_started; });
        }
        dispatchers.SetGeneration("cross-thread-reentrant", 3);
        {
            std::scoped_lock lock(sync->mutex);
            sync->transition_returned = true;
        }
        sync->condition.notify_all();
    });

    dispatchers.SetGeneration("cross-thread-reentrant", 2);
    concurrent_transition.join();

    std::atomic_bool current_called{};
    const auto stale = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "cross-thread-reentrant", 2, [] {});
    const auto current = dispatchers.Post(
        anomaly::ExecutionDomain::Game, "cross-thread-reentrant", 3, [&] {
            current_called.store(true);
        });
    bool result = Check(!sync->timed_out,
                        "Callback target destruction waited on a transition lock") &&
        Check(dispatchers.PumpGame() == 1,
              "Cross-thread reentrant transition invoked the wrong task count") &&
        Check(dispatchers.GetTask(old)->state == anomaly::TaskState::Cancelled,
              "Cross-thread reentry did not cancel the old generation") &&
        Check(!stale, "Outer generation reopened after cross-thread reentry") &&
        Check(current && current_called.load(),
              "Cross-thread reentrant generation did not become current");
    dispatchers.RequestStop();
    return result;
}

}  // namespace

int main() {
    if (!TestOwnedLifecycleAndWorkerDomains()) return 1;
    if (!TestExternalGameAndRenderAnchors()) return 2;
    if (!TestGenerationCancellationAcrossDomains()) return 3;
    if (!TestStopCancelsPendingAcrossDomains()) return 4;
    if (!TestDrainObservesCrossDomainPosts()) return 5;
    if (!TestConcurrentJoinWaitsForWorkers()) return 6;
    if (!TestWorkerSelfJoinDefersToExternalJoin()) return 7;
    if (!TestUnrelatedPostsDoNotDelayDrain()) return 8;
    if (!TestInvokeTimeoutRequiresCompletionDrain()) return 9;
    if (!TestQueuedInvokeCancellationSettlesTracker()) return 10;
    if (!TestQueuedInvokeCancellationKeepsAdmissionOpen()) return 11;
    if (!TestGenerationTransitionAllowsDestructorReentry()) return 12;
    if (!TestGenerationTransitionAllowsCrossThreadDestructorReentry()) return 13;
    return 0;
}
