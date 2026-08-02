#include "anomaly/runtime_session.hpp"
#include "anomaly/service_graph.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

anomaly::RuntimeStartContext StartContext(HANDLE external_stop_event = nullptr) {
    anomaly::RuntimeStartContext result;
    result.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL;
    result.bootstrap_module = GetModuleHandleW(nullptr);
    result.game_module = GetModuleHandleW(nullptr);
    result.runtime_root = L".";
    result.log_directory = L".";
    result.external_stop_event = external_stop_event;
    return result;
}

DWORD WaitForToken(std::stop_token stop_token, std::atomic_bool& stopped) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock(mutex);
    condition.wait(lock, stop_token, [] { return false; });
    stopped.store(true);
    return ERROR_SUCCESS;
}

bool TestAcceptedThenRunningAndStop() {
    const HANDLE initialize_gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Check(initialize_gate != nullptr, "CreateEvent initialize gate failed")) return false;

    std::atomic_bool worker_stopped{};
    std::atomic_bool shutdown_called{};
    std::atomic_int on_stopped_calls{};
    std::atomic_bool on_stopped_after_shutdown{};
    anomaly::RuntimeSessionSnapshot on_stopped_snapshot;
    anomaly::RuntimeSessionOptions options;
    options.initialize = [initialize_gate](std::stop_token) {
        return WaitForSingleObject(initialize_gate, 2000) == WAIT_OBJECT_0
            ? ERROR_SUCCESS
            : ERROR_TIMEOUT;
    };
    options.workers.push_back({"worker", [&worker_stopped](std::stop_token stop_token) {
        return WaitForToken(stop_token, worker_stopped);
    }});
    options.shutdown = [&] {
        shutdown_called.store(worker_stopped.load());
    };
    options.on_stopped = [&](anomaly::RuntimeSessionSnapshot snapshot) {
        on_stopped_snapshot = snapshot;
        on_stopped_after_shutdown.store(shutdown_called.load());
        on_stopped_calls.fetch_add(1);
    };

    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS, "Start was not accepted") &&
        Check(session.Snapshot().state != ANOMALY_RUNTIME_STATE_RUNNING,
              "Start reported completion while initialization was blocked");
    SetEvent(initialize_gate);
    result = Check(WaitUntil([&] {
        return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING;
    }), "Session did not reach Running") && result;
    session.RequestStop();
    session.RequestStop();
    result = Check(session.WaitForStop(2s), "Session did not stop in two seconds") && result;
    session.Join();
    session.Join();
    session.RequestStop();
    session.Join();
    const auto snapshot = session.Snapshot();
    result = Check(snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED, "Session state is not Stopped") &&
        Check(snapshot.last_error == ERROR_SUCCESS, "Successful stop recorded an error") &&
        Check(worker_stopped.load(), "Worker did not observe its stop token") &&
        Check(shutdown_called.load(), "Shutdown ran before workers joined") &&
        Check(on_stopped_calls.load() == 1,
              "Repeated stop/join invoked on_stopped more than once") &&
        Check(on_stopped_after_shutdown.load(),
              "on_stopped ran before shutdown completed") &&
        Check(on_stopped_snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED &&
                  on_stopped_snapshot.last_error == ERROR_SUCCESS &&
                  on_stopped_snapshot.generation == snapshot.generation,
              "on_stopped did not receive the final successful snapshot") && result;
    CloseHandle(initialize_gate);
    return result;
}

bool TestWorkerFailureStopsSession() {
    std::atomic_int on_stopped_calls{};
    anomaly::RuntimeSessionSnapshot on_stopped_snapshot;
    anomaly::RuntimeSessionOptions options;
    options.workers.push_back({"failure", [](std::stop_token) {
        return ERROR_BAD_COMMAND;
    }});
    options.on_stopped = [&](anomaly::RuntimeSessionSnapshot snapshot) {
        on_stopped_snapshot = snapshot;
        on_stopped_calls.fetch_add(1);
        throw 1;
    };
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS, "Failure session was not accepted");
    result = Check(session.WaitForStop(2s), "Worker failure did not stop the session") && result;
    session.Join();
    session.RequestStop();
    session.Join();
    const auto snapshot = session.Snapshot();
    return Check(snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED,
                  "Failed session did not finish its release path") &&
        Check(snapshot.last_error == ERROR_BAD_COMMAND,
              "Worker failure was not preserved") &&
        Check(on_stopped_calls.load() == 1 &&
                  on_stopped_snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED &&
                  on_stopped_snapshot.last_error == ERROR_BAD_COMMAND &&
                  on_stopped_snapshot.generation == snapshot.generation,
              "Failed session did not publish its final on_stopped snapshot") && result;
}

bool TestStopBeforeStartNotifiesOnce() {
    std::atomic_int on_stopped_calls{};
    anomaly::RuntimeSessionSnapshot on_stopped_snapshot;
    anomaly::RuntimeSessionOptions options;
    options.on_stopped = [&](anomaly::RuntimeSessionSnapshot snapshot) {
        on_stopped_snapshot = snapshot;
        on_stopped_calls.fetch_add(1);
    };
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    session.RequestStop();
    session.RequestStop();
    bool result = Check(session.Start() == ERROR_CANCELLED,
                        "Pre-stopped session did not reject Start as cancelled") &&
        Check(session.WaitForStop(0ms),
              "Pre-stopped session did not publish Stopped");
    session.Join();
    session.Join();
    const auto snapshot = session.Snapshot();
    return Check(on_stopped_calls.load() == 1,
                 "Pre-stopped session invoked on_stopped more than once") &&
        Check(on_stopped_snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED &&
                  on_stopped_snapshot.last_error == ERROR_CANCELLED &&
                  on_stopped_snapshot.generation == snapshot.generation,
              "Pre-stopped session did not publish its final cancelled snapshot") && result;
}

bool TestExternalStopEventIsDuplicated() {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE event_control{};
    if (!Check(event != nullptr, "CreateEvent external stop failed")) return false;
    if (!Check(DuplicateHandle(
            GetCurrentProcess(), event, GetCurrentProcess(), &event_control,
            EVENT_MODIFY_STATE, FALSE, 0) != FALSE,
            "DuplicateHandle control event failed")) {
        CloseHandle(event);
        return false;
    }

    std::atomic_bool worker_stopped{};
    anomaly::RuntimeSessionOptions options;
    options.workers.push_back({"worker", [&worker_stopped](std::stop_token stop_token) {
        return WaitForToken(stop_token, worker_stopped);
    }});
    anomaly::RuntimeSession session(StartContext(event), std::move(options));
    const HANDLE owned_event = session.StartContext().external_stop_event;
    CloseHandle(event);

    bool result = Check(owned_event != nullptr && owned_event != event,
                        "Session did not expose its duplicated stop event") &&
        Check(WaitForSingleObject(owned_event, 0) == WAIT_TIMEOUT,
              "Duplicated stop event was invalid or unexpectedly signaled") &&
        Check(session.Start() == ERROR_SUCCESS, "External event session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING;
        }), "External event session did not reach Running");
    SetEvent(event_control);
    result = Check(session.WaitForStop(2s), "External event did not stop the session") && result;
    session.Join();
    result = Check(worker_stopped.load(), "External stop did not stop the worker") && result;
    CloseHandle(event_control);
    return result;
}

bool TestInitializationFailureSkipsWorkers() {
    std::atomic_bool worker_started{};
    std::atomic_bool dispatcher_post_accepted{};
    std::atomic_bool dispatcher_worker_started{};
    std::atomic_bool dispatcher_worker_drained{};
    std::atomic_bool shutdown_called{};
    anomaly::RuntimeSession* session_ptr{};
    anomaly::RuntimeSessionOptions options;
    options.initialize = [&](std::stop_token) {
        const auto task = session_ptr->Dispatchers().Post(
            anomaly::ExecutionDomain::Worker, "failed-initialize", 1, [&] {
                dispatcher_worker_started.store(true);
            });
        dispatcher_post_accepted.store(static_cast<bool>(task));
        if (task) {
            dispatcher_worker_drained.store(
                session_ptr->Dispatchers().Drain("failed-initialize", 1, 2s));
        }
        return ERROR_BAD_CONFIGURATION;
    };
    options.shutdown = [&shutdown_called] { shutdown_called.store(true); };
    options.workers.push_back({"must-not-run", [&worker_started](std::stop_token) {
        worker_started.store(true);
        return ERROR_SUCCESS;
    }});
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    session_ptr = &session;
    bool result = Check(session.Start() == ERROR_SUCCESS, "Initialization failure was not accepted") &&
        Check(session.WaitForStop(2s), "Initialization failure did not stop the session");
    session.Join();
    const auto snapshot = session.Snapshot();
    const auto rejected = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Worker, "failed-initialize", 1, [] {});
    return Check(!worker_started.load(), "Worker started after initialization failed") &&
        Check(dispatcher_post_accepted.load() && dispatcher_worker_started.load() &&
                  dispatcher_worker_drained.load(),
              "Dispatcher worker was unavailable during initialization") &&
        Check(!rejected, "Failed initialization left dispatcher workers accepting work") &&
        Check(shutdown_called.load(), "Failed initialization skipped shutdown") &&
        Check(snapshot.last_error == ERROR_BAD_CONFIGURATION,
              "Initialization failure was not preserved") && result;
}

bool TestStopCancelsInitialization() {
    std::atomic_bool initialization_stopped{};
    anomaly::RuntimeSessionOptions options;
    options.initialize = [&initialization_stopped](std::stop_token stop_token) {
        return WaitForToken(stop_token, initialization_stopped);
    };
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS, "Cancellation session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state ==
                ANOMALY_RUNTIME_STATE_STARTING_BLOCKING_SERVICES;
        }), "Cancellation session did not enter initialization");
    session.RequestStop();
    result = Check(session.WaitForStop(2s), "Initialization ignored its stop token") && result;
    session.Join();
    return Check(initialization_stopped.load(), "Initialization did not observe cancellation") &&
        Check(session.Snapshot().last_error == ERROR_SUCCESS,
              "Initialization cancellation recorded a failure") && result;
}

bool TestExternalStopCancelsInitialization() {
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Check(event != nullptr, "CreateEvent external initialization stop failed")) return false;

    std::atomic_bool initialization_stopped{};
    std::atomic_bool shutdown_called{};
    anomaly::RuntimeSessionOptions options;
    options.initialize = [&initialization_stopped](std::stop_token stop_token) {
        return WaitForToken(stop_token, initialization_stopped);
    };
    options.shutdown = [&shutdown_called] { shutdown_called.store(true); };
    anomaly::RuntimeSession session(StartContext(event), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "External initialization session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state ==
                ANOMALY_RUNTIME_STATE_STARTING_BLOCKING_SERVICES;
        }), "External initialization session did not enter initialization");
    SetEvent(event);
    result = Check(session.WaitForStop(2s),
                   "External event did not cancel initialization") && result;
    session.Join();
    result = Check(initialization_stopped.load(),
                   "External event did not reach the initialization stop token") &&
        Check(shutdown_called.load(),
              "External initialization cancellation skipped shutdown") && result;
    CloseHandle(event);
    return result;
}

bool TestPresignaledExternalStop() {
    const HANDLE event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!Check(event != nullptr, "CreateEvent presignaled stop failed")) return false;

    std::atomic_bool worker_started{};
    anomaly::RuntimeSessionOptions options;
    options.workers.push_back({"must-not-stay-running", [&worker_started](std::stop_token) {
        worker_started.store(true);
        return ERROR_SUCCESS;
    }});
    anomaly::RuntimeSession session(StartContext(event), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "Presignaled external session was not accepted") &&
        Check(session.WaitForStop(2s), "Presignaled external event did not stop the session");
    session.Join();
    result = Check(session.Snapshot().state == ANOMALY_RUNTIME_STATE_STOPPED,
                   "Presignaled external session did not reach Stopped") && result;
    CloseHandle(event);
    return result;
}

bool TestServiceGraphLifecycle() {
    std::mutex events_mutex;
    std::vector<std::string> events;
    const auto record = [&](std::string event) {
        std::scoped_lock lock(events_mutex);
        events.push_back(std::move(event));
    };

    auto services = std::make_shared<anomaly::ServiceGraph>();
    anomaly::ServiceDescriptor dependency;
    dependency.id = "dependency";
    dependency.start = [&](std::stop_token) {
        record("start:dependency");
        return ERROR_SUCCESS;
    };
    dependency.stop = [&] { record("stop:dependency"); };
    anomaly::ServiceDescriptor dependent;
    dependent.id = "dependent";
    dependent.required_dependencies.push_back({"dependency", 1});
    dependent.start = [&](std::stop_token) {
        record("start:dependent");
        return ERROR_SUCCESS;
    };
    dependent.stop = [&] { record("stop:dependent"); };
    if (!Check(services->Register(std::move(dependent)) == ERROR_SUCCESS &&
                   services->Register(std::move(dependency)) == ERROR_SUCCESS,
               "Session service registration failed")) {
        return false;
    }

    anomaly::RuntimeSessionOptions options;
    options.services = services;
    options.workers.push_back({"worker", [&](std::stop_token stop_token) {
        record("start:worker");
        while (!stop_token.stop_requested()) std::this_thread::sleep_for(1ms);
        record("stop:worker");
        return ERROR_SUCCESS;
    }});
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "Service graph session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING;
        }), "Service graph session did not reach Running");
    session.RequestStop();
    result = Check(session.WaitForStop(2s), "Service graph session did not stop") && result;
    session.Join();
    std::scoped_lock lock(events_mutex);
    return Check(events == std::vector<std::string>{
                       "start:dependency", "start:dependent", "start:worker",
                       "stop:worker", "stop:dependent", "stop:dependency"},
                 "RuntimeSession did not own service/worker stop order") && result;
}

bool TestAsyncServiceFailureStopsSession() {
    auto services = std::make_shared<anomaly::ServiceGraph>();
    std::atomic_int root_stops{};
    std::atomic_int async_stops{};
    std::atomic_bool async_started{};
    std::mutex async_mutex;
    std::condition_variable_any async_condition;
    bool release_async{};

    anomaly::ServiceDescriptor root;
    root.id = "async-failure-root";
    root.start = [](std::stop_token) { return ERROR_SUCCESS; };
    root.stop = [&] { ++root_stops; };
    anomaly::ServiceDescriptor failure;
    failure.id = "async-failure";
    failure.startup = anomaly::ServiceStartup::Async;
    failure.required_dependencies.push_back({"async-failure-root", 1});
    failure.start = [&](std::stop_token stop_token) {
        async_started.store(true);
        std::unique_lock lock(async_mutex);
        static_cast<void>(async_condition.wait(
            lock, stop_token, [&] { return release_async; }));
        return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_BAD_COMMAND;
    };
    failure.stop = [&] { ++async_stops; };
    if (!Check(services->Register(std::move(failure)) == ERROR_SUCCESS &&
                   services->Register(std::move(root)) == ERROR_SUCCESS,
               "Async failure session services were not registered")) {
        return false;
    }

    anomaly::RuntimeSessionOptions options;
    options.services = services;
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "Async failure session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING &&
                async_started.load();
        }), "Async failure session did not enter Running");

    std::mutex worker_mutex;
    std::condition_variable worker_condition;
    bool worker_entered{};
    bool release_worker{};
    const auto worker = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Worker, "async-monitor-fixture", 1, [&] {
            std::unique_lock lock(worker_mutex);
            worker_entered = true;
            worker_condition.notify_all();
            worker_condition.wait(lock, [&] { return release_worker; });
        });
    {
        std::unique_lock lock(worker_mutex);
        result = Check(worker && worker_condition.wait_for(
                                     lock, 2s, [&] { return worker_entered; }),
                       "Async monitor fixture did not occupy the worker lane") && result;
    }
    {
        std::scoped_lock lock(async_mutex);
        release_async = true;
    }
    async_condition.notify_all();
    result = Check(WaitUntil([&] {
                       return session.Snapshot().last_error == ERROR_BAD_COMMAND;
                   }), "Async service failure was not propagated outside the worker lane") && result;
    {
        std::scoped_lock lock(worker_mutex);
        release_worker = true;
    }
    worker_condition.notify_all();
    result = Check(session.WaitForStop(2s),
                   "Async service failure did not stop the session") && result;
    session.Join();
    services->StopAll();
    const auto snapshot = session.Snapshot();
    return Check(snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED,
                 "Async failure session did not reach Stopped") &&
        Check(snapshot.last_error == ERROR_BAD_COMMAND,
              "Async service error was not preserved") &&
        Check(root_stops.load() == 1 && async_stops.load() == 0,
              "Async failure rollback or final StopAll ran more than once") && result;
}

bool TestSessionStopCancelsAsyncServicesWithoutFailure() {
    auto services = std::make_shared<anomaly::ServiceGraph>();
    std::atomic_int root_stops{};
    std::atomic_int async_stops{};
    std::atomic_bool async_started{};
    std::atomic_bool worker_started{};
    std::atomic_bool worker_returned{};
    std::atomic_bool dispatcher_wait_started{};
    std::atomic_bool dispatcher_wait_returned{};
    std::atomic<DWORD> dispatcher_wait_result{ERROR_GEN_FAILURE};
    std::atomic_int premature_service_stops{};
    std::mutex async_mutex;
    std::condition_variable_any async_condition;

    anomaly::ServiceDescriptor root;
    root.id = "async-cancel-root";
    root.start = [](std::stop_token) { return ERROR_SUCCESS; };
    root.stop = [&] {
        if (!worker_returned.load() || !dispatcher_wait_returned.load()) {
            ++premature_service_stops;
        }
        ++root_stops;
    };
    anomaly::ServiceDescriptor async;
    async.id = "async-cancel";
    async.startup = anomaly::ServiceStartup::Async;
    async.required_dependencies.push_back({"async-cancel-root", 1});
    async.start = [&](std::stop_token stop_token) {
        async_started.store(true);
        std::unique_lock lock(async_mutex);
        async_condition.wait(lock, stop_token, [] { return false; });
        return ERROR_SUCCESS;
    };
    async.stop = [&] {
        if (!worker_returned.load() || !dispatcher_wait_returned.load()) {
            ++premature_service_stops;
        }
        ++async_stops;
    };
    if (!Check(services->Register(std::move(async)) == ERROR_SUCCESS &&
                   services->Register(std::move(root)) == ERROR_SUCCESS,
               "Async cancellation session services were not registered")) {
        return false;
    }

    anomaly::RuntimeSessionOptions options;
    options.services = services;
    options.workers.push_back({"async-cancel-worker", [&](std::stop_token stop_token) {
                                   worker_started.store(true);
                                   std::mutex wait_mutex;
                                   std::unique_lock lock(wait_mutex);
                                   std::condition_variable_any wait_condition;
                                   wait_condition.wait(
                                       lock, stop_token, [] { return false; });
                                   worker_returned.store(true);
                                   return ERROR_SUCCESS;
                               }});
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "Async cancellation session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING &&
                async_started.load() && worker_started.load();
        }), "Async cancellation service did not start after Running");
    const auto dispatcher_wait = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Worker, "async-cancel-waiter", 1, [&] {
            dispatcher_wait_started.store(true);
            dispatcher_wait_result.store(services->WaitForAsync());
            dispatcher_wait_returned.store(true);
        });
    result = Check(dispatcher_wait && WaitUntil([&] {
                       return dispatcher_wait_started.load();
                   }), "Dispatcher worker did not begin waiting for async services") && result;
    session.RequestStop();
    result = Check(session.WaitForStop(2s),
                   "Async cancellation session did not stop") && result;
    session.Join();
    services->StopAll();
    const auto snapshot = session.Snapshot();
    return Check(snapshot.last_error == ERROR_SUCCESS,
                 "Normal async cancellation recorded a session failure") &&
        Check(root_stops.load() == 1 && async_stops.load() == 1,
              "Async cancellation rollback or final StopAll ran more than once") &&
        Check(dispatcher_wait_returned.load() &&
                  dispatcher_wait_result.load() == ERROR_CANCELLED,
              "CancelStartup did not release the dispatcher async waiter") &&
        Check(premature_service_stops.load() == 0,
              "Async services stopped before session workers returned") && result;
}

bool TestSessionOwnsRuntimeDispatchers() {
    anomaly::RuntimeSessionOptions options;
    options.dispatcher_options.worker_threads = 2;
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Dispatchers().WorkerCount() == 2,
                        "Session did not apply dispatcher worker options") &&
        Check(session.Start() == ERROR_SUCCESS,
              "Dispatcher session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING;
        }), "Dispatcher session did not reach Running");

    std::mutex worker_mutex;
    std::set<std::thread::id> worker_threads;
    std::thread::id lifecycle_thread;
    std::thread::id game_thread;
    std::thread::id render_thread;
    std::vector<anomaly::DomainTaskHandle> handles;
    handles.push_back(session.Dispatchers().Post(
        anomaly::ExecutionDomain::Lifecycle, "session-domains", 1, [&] {
            lifecycle_thread = std::this_thread::get_id();
        }));
    for (int index = 0; index < 8; ++index) {
        handles.push_back(session.Dispatchers().Post(
            anomaly::ExecutionDomain::Worker, "session-domains", 1, [&] {
                std::scoped_lock lock(worker_mutex);
                worker_threads.insert(std::this_thread::get_id());
            }));
    }
    const auto game = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Game, "session-domains", 1, [&] {
            game_thread = std::this_thread::get_id();
        });
    const auto render = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Render, "session-domains", 1, [&] {
            render_thread = std::this_thread::get_id();
        });
    handles.push_back(game);
    handles.push_back(render);

    const auto game_anchor = std::this_thread::get_id();
    result = Check(std::ranges::all_of(handles, [](const auto handle) {
                       return static_cast<bool>(handle);
                   }), "Session dispatcher rejected work while Running") &&
        Check(session.Dispatchers().PumpGame() == 1,
              "Session Game dispatcher did not pump") && result;
    std::thread render_anchor([&] {
        static_cast<void>(session.Dispatchers().PumpRender());
    });
    render_anchor.join();
    result = Check(session.Dispatchers().Drain("session-domains", 1, 2s),
                   "Session-owned dispatchers did not drain") && result;

    {
        std::scoped_lock lock(worker_mutex);
        result = Check(worker_threads.size() == 2,
                       "Session did not drive both dispatcher worker lanes") && result;
    }
    result = Check(lifecycle_thread != std::thread::id{} &&
                       lifecycle_thread == session.Dispatchers().BoundThread(
                           anomaly::ExecutionDomain::Lifecycle),
                   "Session lifecycle dispatcher used the wrong thread") &&
        Check(game_thread == game_anchor &&
                  game_thread == session.Dispatchers().BoundThread(
                      anomaly::ExecutionDomain::Game),
              "Session Game dispatcher used the wrong anchor") &&
        Check(render_thread != std::thread::id{} &&
                  render_thread == session.Dispatchers().BoundThread(
                      anomaly::ExecutionDomain::Render),
              "Session Render dispatcher used the wrong anchor") && result;

    session.RequestStop();
    result = Check(session.WaitForStop(2s),
                   "Dispatcher session did not stop") && result;
    session.Join();
    return result;
}

bool TestSessionStopCancelsDispatcherWork() {
    std::atomic_int blockers_returned{};
    std::atomic_bool shutdown_after_dispatchers{};
    anomaly::RuntimeSessionOptions options;
    options.shutdown = [&] {
        shutdown_after_dispatchers.store(blockers_returned.load() == 2);
    };
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "Cancellation dispatcher session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING;
        }), "Cancellation dispatcher session did not reach Running");

    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    int entered{};
    bool release{};
    const auto blocker = [&] {
        std::unique_lock lock(gate_mutex);
        ++entered;
        gate_condition.notify_all();
        gate_condition.wait(lock, [&] { return release; });
        ++blockers_returned;
    };
    const auto running_lifecycle = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Lifecycle, "session-blockers", 1, blocker);
    const auto running_worker = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Worker, "session-blockers", 1, blocker);
    {
        std::unique_lock lock(gate_mutex);
        if (!gate_condition.wait_for(lock, 2s, [&] { return entered == 2; })) {
            release = true;
            lock.unlock();
            gate_condition.notify_all();
            session.RequestStop();
            session.Join();
            return Check(false, "Dispatcher blockers did not start");
        }
    }

    std::atomic_int pending_called{};
    const auto pending_lifecycle = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Lifecycle, "session-pending", 2,
        [&] { ++pending_called; });
    const auto pending_worker = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Worker, "session-pending", 2,
        [&] { ++pending_called; });
    const auto pending_game = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Game, "session-pending", 2,
        [&] { ++pending_called; });
    const auto pending_render = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Render, "session-pending", 2,
        [&] { ++pending_called; });
    result = Check(running_lifecycle && running_worker && pending_lifecycle &&
                       pending_worker && pending_game && pending_render,
                   "Cancellation fixture posts were rejected") && result;

    session.RequestStop();
    result = Check(!session.WaitForStop(20ms),
                   "Session stopped before running dispatcher callbacks returned") && result;
    {
        std::scoped_lock lock(gate_mutex);
        release = true;
    }
    gate_condition.notify_all();
    result = Check(session.WaitForStop(2s),
                   "Session did not settle stopped dispatcher work") && result;
    session.Join();

    const auto is_cancelled = [&](anomaly::DomainTaskHandle handle) {
        const auto task = session.Dispatchers().GetTask(handle);
        return task.has_value() && task->state == anomaly::TaskState::Cancelled;
    };
    result = Check(pending_called.load() == 0,
                   "Session stop invoked pending dispatcher work") &&
        Check(is_cancelled(pending_lifecycle) && is_cancelled(pending_worker) &&
                  is_cancelled(pending_game) && is_cancelled(pending_render),
              "Session stop did not cancel every dispatcher domain") &&
        Check(shutdown_after_dispatchers.load(),
              "Session shutdown ran before dispatcher callbacks returned") && result;

    const std::array rejected{
        session.Dispatchers().Post(
            anomaly::ExecutionDomain::Lifecycle, "stopped", 1, [] {}),
        session.Dispatchers().Post(
            anomaly::ExecutionDomain::Worker, "stopped", 1, [] {}),
        session.Dispatchers().Post(
            anomaly::ExecutionDomain::Game, "stopped", 1, [] {}),
        session.Dispatchers().Post(
            anomaly::ExecutionDomain::Render, "stopped", 1, [] {})};
    return Check(std::ranges::none_of(rejected, [](const auto handle) {
                     return static_cast<bool>(handle);
                 }), "Stopped session accepted dispatcher work") && result;
}

bool TestPluginStoppingStateAndAffinityDrain() {
    using namespace std::chrono_literals;
    auto services = std::make_shared<anomaly::ServiceGraph>();
    std::atomic_bool plugin_stop_entered{};
    std::atomic_bool plugin_stop_returned{};
    std::atomic_bool service_stop_called{};
    std::thread::id service_start_thread;
    std::thread::id service_stop_thread;
    std::mutex order_mutex;
    std::vector<std::string> order;

    anomaly::ServiceDescriptor worker_service;
    worker_service.id = "phase9.worker-affinity-stop";
    worker_service.affinity = anomaly::ServiceAffinity::Worker;
    worker_service.start = [&](std::stop_token) {
        service_start_thread = std::this_thread::get_id();
        std::scoped_lock lock(order_mutex);
        order.push_back("service.start");
        return ERROR_SUCCESS;
    };
    worker_service.stop = [&] {
        service_stop_thread = std::this_thread::get_id();
        service_stop_called.store(true);
        std::scoped_lock lock(order_mutex);
        order.push_back("service.stop");
    };
    if (!Check(
            services->Register(std::move(worker_service)) == ERROR_SUCCESS,
            "Phase 9 affinity service registration failed")) {
        return false;
    }

    anomaly::RuntimeSessionOptions options;
    options.services = services;
    options.stop_plugins = [&](std::chrono::milliseconds) {
        plugin_stop_entered.store(true);
        {
            std::scoped_lock lock(order_mutex);
            order.push_back("plugins.stop");
        }
        std::this_thread::sleep_for(80ms);
        plugin_stop_returned.store(true);
        return ERROR_SUCCESS;
    };
    anomaly::RuntimeSession session(StartContext(), std::move(options));
    bool result = Check(session.Start() == ERROR_SUCCESS,
                        "Phase 9 stop-state session was not accepted") &&
        Check(WaitUntil([&] {
            return session.Snapshot().state == ANOMALY_RUNTIME_STATE_RUNNING;
        }), "Phase 9 stop-state session did not reach Running");
    session.RequestStop();
    result = Check(WaitUntil([&] {
                       return plugin_stop_entered.load() &&
                           session.Snapshot().state ==
                               ANOMALY_RUNTIME_STATE_STOPPING_PLUGINS;
                   }),
                   "Session did not expose STOPPING_PLUGINS while draining") && result;
    result = Check(session.WaitForStop(2s),
                   "Phase 9 stop-state session did not stop") && result;
    session.Join();

    const auto snapshot = session.Snapshot();
    std::scoped_lock lock(order_mutex);
    const auto plugin_position = std::find(order.begin(), order.end(), "plugins.stop");
    const auto service_position = std::find(order.begin(), order.end(), "service.stop");
    return Check(plugin_stop_returned.load(),
                 "Plugin stop callback did not complete") &&
        Check(service_stop_called.load(), "Affinity service stop callback did not run") &&
        Check(service_start_thread == session.Dispatchers().BoundThread(
                   anomaly::ExecutionDomain::Worker),
               "Affinity service start callback used the wrong thread") &&
        Check(service_stop_thread == session.Dispatchers().BoundThread(
                   anomaly::ExecutionDomain::Worker),
               "Affinity service stop callback used the wrong thread") &&
        Check(plugin_position != order.end() && service_position != order.end() &&
                  plugin_position < service_position,
              "Plugin stop did not precede service stop") &&
        Check(snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED,
              "Phase 9 stop-state session did not reach Stopped") && result;
}

}  // namespace

int main() {
    if (!TestAcceptedThenRunningAndStop()) return 1;
    if (!TestWorkerFailureStopsSession()) return 2;
    if (!TestStopBeforeStartNotifiesOnce()) return 3;
    if (!TestExternalStopEventIsDuplicated()) return 4;
    if (!TestInitializationFailureSkipsWorkers()) return 5;
    if (!TestStopCancelsInitialization()) return 6;
    if (!TestExternalStopCancelsInitialization()) return 7;
    if (!TestPresignaledExternalStop()) return 8;
    if (!TestServiceGraphLifecycle()) return 9;
    if (!TestAsyncServiceFailureStopsSession()) return 10;
    if (!TestSessionStopCancelsAsyncServicesWithoutFailure()) return 11;
    if (!TestSessionOwnsRuntimeDispatchers()) return 12;
    if (!TestSessionStopCancelsDispatcherWork()) return 13;
    if (!TestPluginStoppingStateAndAffinityDrain()) return 14;
    return 0;
}
