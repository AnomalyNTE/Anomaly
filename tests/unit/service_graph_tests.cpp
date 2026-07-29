#include "anomaly/service_graph.hpp"
#include "anomaly/service_graph_diagnostics.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
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

class Recorder final {
public:
    void Add(std::string value) {
        std::scoped_lock lock(mutex_);
        values_.push_back(std::move(value));
    }

    std::vector<std::string> Values() const {
        std::scoped_lock lock(mutex_);
        return values_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> values_;
};

anomaly::ServiceDescriptor Descriptor(
    std::string id, Recorder* recorder = nullptr, DWORD start_result = ERROR_SUCCESS) {
    anomaly::ServiceDescriptor descriptor;
    descriptor.id = std::move(id);
    const std::string callback_id = descriptor.id;
    descriptor.start = [recorder, callback_id, start_result](std::stop_token) {
        if (recorder != nullptr) recorder->Add("start:" + callback_id);
        return start_result;
    };
    descriptor.stop = [recorder, callback_id] {
        if (recorder != nullptr) recorder->Add("stop:" + callback_id);
    };
    return descriptor;
}

const anomaly::ServiceSnapshot* FindService(
    const anomaly::ServiceGraphSnapshot& snapshot, const std::string& id) {
    for (const anomaly::ServiceSnapshot& service : snapshot.services) {
        if (service.id == id) return &service;
    }
    return nullptr;
}

bool CheckValues(
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected,
    const char* message) {
    if (actual == expected) return true;
    std::cerr << message << ":";
    for (const std::string& value : actual) std::cerr << ' ' << value;
    std::cerr << '\n';
    return false;
}

bool TestDuplicateId() {
    anomaly::ServiceGraph graph;
    bool result = Check(graph.Register(Descriptor("duplicate")) == ERROR_SUCCESS,
                        "First duplicate registration failed") &&
        Check(graph.Register(Descriptor("duplicate")) == ERROR_SUCCESS,
              "Second duplicate registration failed");
    result = Check(graph.Build() == ERROR_DUP_NAME, "Duplicate ID was accepted") && result;
    const auto snapshot = graph.Snapshot();
    return Check(!snapshot.built, "Duplicate graph was marked built") &&
        Check(snapshot.error == ERROR_DUP_NAME, "Duplicate error was not retained") &&
        Check(snapshot.failures.size() >= 2, "Duplicate diagnostics omitted a descriptor") &&
        result;
}

bool TestMissingRequiredDependency() {
    anomaly::ServiceGraph graph;
    auto service = Descriptor("dependent");
    service.required_dependencies.push_back({"missing", 1});
    bool result = Check(graph.Register(std::move(service)) == ERROR_SUCCESS,
                        "Missing-dependency registration failed") &&
        Check(graph.Build() == ERROR_NOT_FOUND, "Missing dependency was accepted");
    const auto snapshot = graph.Snapshot();
    const auto* dependent = FindService(snapshot, "dependent");
    return Check(dependent != nullptr, "Missing-dependency snapshot omitted service") &&
        Check(dependent->state == anomaly::ServiceState::Failed,
              "Missing dependency did not fail its owner") &&
        Check(!dependent->dependencies.empty() && !dependent->dependencies[0].resolved,
              "Missing dependency was reported as resolved") &&
        result;
}

bool TestDependencyVersion() {
    anomaly::ServiceGraph graph;
    auto provider = Descriptor("provider");
    provider.version = 2;
    auto consumer = Descriptor("consumer");
    consumer.required_dependencies.push_back({"provider", 3});
    bool result = Check(graph.Register(std::move(provider)) == ERROR_SUCCESS,
                        "Version provider registration failed") &&
        Check(graph.Register(std::move(consumer)) == ERROR_SUCCESS,
              "Version consumer registration failed") &&
        Check(graph.Build() == ERROR_REVISION_MISMATCH,
              "Insufficient dependency version was accepted");
    return result;
}

bool TestCycle() {
    anomaly::ServiceGraph graph;
    auto first = Descriptor("first");
    first.required_dependencies.push_back({"second", 1});
    auto second = Descriptor("second");
    second.required_dependencies.push_back({"third", 1});
    auto third = Descriptor("third");
    third.required_dependencies.push_back({"first", 1});
    bool result = Check(graph.Register(std::move(first)) == ERROR_SUCCESS,
                        "First cycle registration failed") &&
        Check(graph.Register(std::move(second)) == ERROR_SUCCESS,
              "Second cycle registration failed") &&
        Check(graph.Register(std::move(third)) == ERROR_SUCCESS,
              "Third cycle registration failed") &&
        Check(graph.Build() == ERROR_CIRCULAR_DEPENDENCY, "Dependency cycle was accepted");
    const auto snapshot = graph.Snapshot();
    return Check(snapshot.failures.size() == 3, "Cycle chain did not identify all members") &&
        result;
}

bool TestTopologyReverseStopAndSnapshot() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    auto leaf = Descriptor("leaf", &recorder);
    leaf.startup = anomaly::ServiceStartup::Async;
    leaf.affinity = anomaly::ServiceAffinity::Worker;
    leaf.required_dependencies.push_back({"middle", 2});
    auto root = Descriptor("root", &recorder);
    root.version = 4;
    root.startup = anomaly::ServiceStartup::Lazy;
    auto middle = Descriptor("middle", &recorder);
    middle.version = 2;
    middle.required_dependencies.push_back({"root", 4});

    bool result = Check(graph.Register(std::move(leaf)) == ERROR_SUCCESS,
                        "Leaf registration failed") &&
        Check(graph.Register(std::move(root)) == ERROR_SUCCESS,
              "Root registration failed") &&
        Check(graph.Register(std::move(middle)) == ERROR_SUCCESS,
              "Middle registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Topology graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS, "Topology graph did not start") &&
        Check(graph.WaitForAsync() == ERROR_SUCCESS,
              "Topology async startup did not complete") &&
        CheckValues(recorder.Values(), {"start:root", "start:middle", "start:leaf"},
                    "Services did not start in dependency order");

    const auto running = graph.Snapshot();
    const auto* leaf_snapshot = FindService(running, "leaf");
    result = Check(leaf_snapshot != nullptr, "Running snapshot omitted leaf") &&
        Check(leaf_snapshot->state == anomaly::ServiceState::Ready,
              "Leaf did not reach Ready") &&
        Check(leaf_snapshot->startup == anomaly::ServiceStartup::Async,
              "Snapshot lost startup mode") &&
        Check(leaf_snapshot->affinity == anomaly::ServiceAffinity::Worker,
              "Snapshot lost affinity") &&
        Check(!leaf_snapshot->dependencies.empty() &&
                  leaf_snapshot->dependencies[0].resolved &&
                  leaf_snapshot->dependencies[0].resolved_version == 2,
              "Snapshot lost dependency resolution") && result;

    graph.StopAll();
    result = CheckValues(
        recorder.Values(),
        {"start:root", "start:middle", "start:leaf",
         "stop:leaf", "stop:middle", "stop:root"},
        "Services did not stop in reverse dependency order") && result;
    return result;
}

bool TestProvidedServiceIsNotOwned() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    anomaly::ServiceDescriptor provided;
    provided.id = "provided";
    provided.version = 3;
    provided.lifetime = anomaly::ServiceLifetime::Provided;
    auto consumer = Descriptor("consumer", &recorder);
    consumer.required_dependencies.push_back({"provided", 2});

    bool result = Check(graph.Register(std::move(consumer)) == ERROR_SUCCESS,
                        "Provided consumer registration failed") &&
        Check(graph.Register(std::move(provided)) == ERROR_SUCCESS,
              "Provided registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Provided graph did not build");
    const auto built = graph.Snapshot();
    const auto* provided_snapshot = FindService(built, "provided");
    result = Check(provided_snapshot != nullptr &&
                       provided_snapshot->lifetime == anomaly::ServiceLifetime::Provided &&
                       provided_snapshot->state == anomaly::ServiceState::Ready,
                   "Provided service was not published Ready") && result;
    result = Check(graph.StartAll() == ERROR_SUCCESS, "Provided consumer did not start") &&
        CheckValues(recorder.Values(), {"start:consumer"},
                    "Provided service invoked an owned start callback") && result;
    graph.StopAll();
    const auto stopped = graph.Snapshot();
    provided_snapshot = FindService(stopped, "provided");
    return CheckValues(recorder.Values(), {"start:consumer", "stop:consumer"},
                       "Provided service invoked an owned stop callback") &&
        Check(provided_snapshot != nullptr &&
                  provided_snapshot->state == anomaly::ServiceState::Ready,
              "StopAll revoked a non-owned Provided service") && result;
}

bool TestPartialFailureAndFailureChain() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    auto root = Descriptor("root", &recorder);
    auto failure = Descriptor("failure", &recorder, ERROR_BAD_COMMAND);
    failure.required_dependencies.push_back({"root", 1});
    auto blocked = Descriptor("blocked", &recorder);
    blocked.required_dependencies.push_back({"failure", 1});
    bool result = Check(graph.Register(std::move(blocked)) == ERROR_SUCCESS,
                        "Blocked registration failed") &&
        Check(graph.Register(std::move(failure)) == ERROR_SUCCESS,
              "Failure registration failed") &&
        Check(graph.Register(std::move(root)) == ERROR_SUCCESS,
              "Root registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Partial-failure graph did not build") &&
        Check(graph.StartAll() == ERROR_BAD_COMMAND,
              "Partial-failure graph returned the wrong error") &&
        CheckValues(recorder.Values(), {"start:root", "start:failure", "stop:root"},
                    "Partial failure did not roll back Ready services");

    const auto snapshot = graph.Snapshot();
    const auto* root_snapshot = FindService(snapshot, "root");
    const auto* failed_snapshot = FindService(snapshot, "failure");
    const auto* blocked_snapshot = FindService(snapshot, "blocked");
    result = Check(snapshot.error == ERROR_BAD_COMMAND,
                   "Graph did not preserve the root startup error") &&
        Check(root_snapshot != nullptr && root_snapshot->state == anomaly::ServiceState::Stopped,
              "Ready dependency was not stopped after failure") &&
        Check(failed_snapshot != nullptr &&
                  failed_snapshot->state == anomaly::ServiceState::Failed &&
                  failed_snapshot->error == ERROR_BAD_COMMAND,
              "Failed service diagnostics were lost") &&
        Check(blocked_snapshot != nullptr &&
                  blocked_snapshot->state == anomaly::ServiceState::Failed &&
                  blocked_snapshot->failure_chain ==
                      std::vector<std::string>{"failure", "blocked"},
              "Dependent failure chain was not retained") &&
        Check(snapshot.failures.size() == 2,
              "Failure snapshot did not retain root and dependent failures") && result;
    graph.StopAll();
    return Check(recorder.Values().size() == 3,
                 "StopAll stopped a rolled-back service twice") && result;
}

bool TestCancellationRollsBackReadyServices() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    std::atomic_bool cancellation_service_started{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;

    auto root = Descriptor("root", &recorder);
    anomaly::ServiceDescriptor cancellable = Descriptor("cancellable", &recorder);
    cancellable.required_dependencies.push_back({"root", 1});
    cancellable.start = [&](std::stop_token stop_token) {
        recorder.Add("start:cancellable");
        cancellation_service_started.store(true);
        std::unique_lock lock(wait_mutex);
        wait_condition.wait(lock, stop_token, [] { return false; });
        return ERROR_CANCELLED;
    };
    bool result = Check(graph.Register(std::move(cancellable)) == ERROR_SUCCESS,
                        "Cancellable registration failed") &&
        Check(graph.Register(std::move(root)) == ERROR_SUCCESS,
              "Cancellation root registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Cancellation graph did not build");

    std::stop_source external_stop;
    std::atomic<DWORD> start_result{ERROR_GEN_FAILURE};
    std::jthread starter([&] {
        start_result.store(graph.StartAll(external_stop.get_token()));
    });
    result = Check(WaitUntil([&] { return cancellation_service_started.load(); }),
                   "Cancellable service did not begin") && result;
    external_stop.request_stop();
    starter.join();
    result = Check(start_result.load() == ERROR_CANCELLED,
                   "Cancellation did not reach StartAll") &&
        CheckValues(recorder.Values(),
                    {"start:root", "start:cancellable", "stop:root"},
                    "Cancellation did not roll back Ready services") && result;
    const auto snapshot = graph.Snapshot();
    const auto* cancelled = FindService(snapshot, "cancellable");
    return Check(snapshot.stop_requested, "Cancellation did not stop the graph") &&
        Check(snapshot.error == ERROR_CANCELLED, "Cancellation error was not retained") &&
        Check(cancelled != nullptr && cancelled->state == anomaly::ServiceState::Failed,
              "Cancelled service was not marked Failed") && result;
}

bool TestOptionalMissingAndLazyStart() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    auto lazy = Descriptor("lazy", &recorder);
    lazy.startup = anomaly::ServiceStartup::Lazy;
    auto consumer = Descriptor("consumer", &recorder);
    consumer.optional_dependencies.push_back({"missing", 1});
    bool result = Check(graph.Register(std::move(lazy)) == ERROR_SUCCESS,
                        "Lazy registration failed") &&
        Check(graph.Register(std::move(consumer)) == ERROR_SUCCESS,
              "Optional consumer registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Missing optional dependency failed Build") &&
        Check(graph.StartAll() == ERROR_SUCCESS, "Optional graph did not start") &&
        CheckValues(recorder.Values(), {"start:consumer"},
                    "StartAll eagerly started an unneeded Lazy service");

    auto snapshot = graph.Snapshot();
    const auto* consumer_snapshot = FindService(snapshot, "consumer");
    const auto* lazy_snapshot = FindService(snapshot, "lazy");
    result = Check(consumer_snapshot != nullptr &&
                       consumer_snapshot->state == anomaly::ServiceState::Degraded,
                   "Missing optional dependency did not produce Degraded state") &&
        Check(!consumer_snapshot->dependencies.empty() &&
                  consumer_snapshot->dependencies[0].optional &&
                  !consumer_snapshot->dependencies[0].resolved,
              "Missing optional dependency snapshot was incorrect") &&
        Check(lazy_snapshot != nullptr &&
                  lazy_snapshot->state == anomaly::ServiceState::Registered,
              "Unneeded Lazy service did not remain Registered") && result;

    result = Check(graph.StartService("lazy") == ERROR_SUCCESS,
                   "Explicit Lazy startup failed") &&
        CheckValues(recorder.Values(), {"start:consumer", "start:lazy"},
                    "Explicit Lazy startup did not invoke its callback") && result;
    graph.StopAll();
    return CheckValues(recorder.Values(),
                       {"start:consumer", "start:lazy", "stop:consumer", "stop:lazy"},
                       "Lazy and eager services stopped in the wrong order") && result;
}

bool TestStopIsIdempotentAndCallbacksAreUnlocked() {
    anomaly::ServiceGraph graph;
    std::atomic_uint32_t stops{};
    std::atomic_bool start_snapshot_read{};
    std::atomic_bool stop_snapshot_read{};
    anomaly::ServiceDescriptor service;
    service.id = "snapshot-reader";
    service.start = [&](std::stop_token) {
        const auto snapshot = graph.Snapshot();
        start_snapshot_read.store(
            !snapshot.services.empty() &&
            snapshot.services[0].state == anomaly::ServiceState::Starting);
        std::this_thread::sleep_for(2ms);
        return ERROR_SUCCESS;
    };
    service.stop = [&] {
        const auto snapshot = graph.Snapshot();
        stop_snapshot_read.store(
            !snapshot.services.empty() &&
            snapshot.services[0].state == anomaly::ServiceState::Stopping);
        stops.fetch_add(1);
    };
    bool result = Check(graph.Register(std::move(service)) == ERROR_SUCCESS,
                        "Snapshot-reader registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Snapshot-reader graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS, "Snapshot-reader graph did not start");
    const auto running = graph.Snapshot();
    const auto* reader = FindService(running, "snapshot-reader");
    result = Check(reader != nullptr && reader->startup_duration >= 1ms,
                   "Startup duration was not captured") && result;
    graph.StopAll();
    graph.StopAll();
    return Check(stops.load() == 1, "StopAll was not idempotent") &&
        Check(start_snapshot_read.load(), "Graph lock was held during start callback") &&
        Check(stop_snapshot_read.load(), "Graph lock was held during stop callback") && result;
}

bool TestAsyncReadyWaveRunsConcurrently() {
    anomaly::ServiceGraph graph;
    std::atomic_uint32_t started{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;
    bool release{};

    const auto async_descriptor = [&](std::string id) {
        anomaly::ServiceDescriptor descriptor = Descriptor(std::move(id));
        descriptor.startup = anomaly::ServiceStartup::Async;
        descriptor.start = [&](std::stop_token stop_token) {
            started.fetch_add(1);
            wait_condition.notify_all();
            std::unique_lock lock(wait_mutex);
            wait_condition.wait(lock, stop_token, [&] { return release; });
            return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_SUCCESS;
        };
        return descriptor;
    };

    bool result = Check(graph.Register(async_descriptor("first")) == ERROR_SUCCESS,
                        "First async registration failed") &&
        Check(graph.Register(async_descriptor("second")) == ERROR_SUCCESS,
              "Second async registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Async wave graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS,
              "StartAll did not release its empty blocking gate") &&
        Check(WaitUntil([&] { return started.load() == 2; }),
              "Independent async services did not start concurrently");

    const auto starting = graph.Snapshot();
    result = Check(starting.startup_active, "Async startup was not reported active") &&
        Check(starting.blocking_startup_complete,
              "Empty blocking startup gate was not reported complete") &&
        Check(!starting.async_startup_complete,
              "Blocked async startup was reported complete") && result;

    {
        std::scoped_lock lock(wait_mutex);
        release = true;
    }
    wait_condition.notify_all();
    result = Check(graph.WaitForAsync() == ERROR_SUCCESS,
                   "Concurrent async startup did not complete") && result;
    const auto ready = graph.Snapshot();
    result = Check(!ready.startup_active && ready.async_startup_complete &&
                       ready.async_startup_error == ERROR_SUCCESS,
                   "Completed async startup snapshot was incorrect") && result;
    graph.StopAll();
    return result;
}

bool TestBlockingWaitsForAsyncDependency() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    std::atomic_bool dependency_started{};
    std::atomic_bool blocking_started{};
    std::atomic_bool start_returned{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;
    bool release{};

    auto dependency = Descriptor("dependency", &recorder);
    dependency.startup = anomaly::ServiceStartup::Async;
    dependency.start = [&](std::stop_token stop_token) {
        recorder.Add("start:dependency");
        dependency_started.store(true);
        std::unique_lock lock(wait_mutex);
        wait_condition.wait(lock, stop_token, [&] { return release; });
        return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_SUCCESS;
    };
    auto blocking = Descriptor("blocking", &recorder);
    blocking.required_dependencies.push_back({"dependency", 1});
    blocking.start = [&](std::stop_token) {
        recorder.Add("start:blocking");
        blocking_started.store(true);
        return ERROR_SUCCESS;
    };

    bool result = Check(graph.Register(std::move(blocking)) == ERROR_SUCCESS,
                        "Blocking service registration failed") &&
        Check(graph.Register(std::move(dependency)) == ERROR_SUCCESS,
              "Async dependency registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Blocking dependency graph did not build");
    std::atomic<DWORD> start_result{ERROR_GEN_FAILURE};
    std::jthread starter([&] {
        start_result.store(graph.StartAll());
        start_returned.store(true);
    });
    result = Check(WaitUntil([&] { return dependency_started.load(); }),
                   "Async dependency did not begin") &&
        Check(!blocking_started.load(),
              "Blocking service started before its async dependency was Ready") &&
        Check(!start_returned.load(),
              "StartAll released before its blocking service was Ready") && result;

    {
        std::scoped_lock lock(wait_mutex);
        release = true;
    }
    wait_condition.notify_all();
    starter.join();
    result = Check(start_result.load() == ERROR_SUCCESS,
                   "StartAll failed after its async dependency became Ready") &&
        Check(blocking_started.load(), "Dependent blocking service did not start") &&
        Check(graph.WaitForAsync() == ERROR_SUCCESS,
              "Dependency startup did not settle") &&
        CheckValues(recorder.Values(), {"start:dependency", "start:blocking"},
                    "Async dependency order was incorrect") && result;
    graph.StopAll();
    return result;
}

bool TestAsyncFailureIsObservableAndRollsBack() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    std::atomic_bool failure_started{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;
    bool release{};

    auto root = Descriptor("root", &recorder);
    auto failure = Descriptor("failure", &recorder);
    failure.startup = anomaly::ServiceStartup::Async;
    failure.required_dependencies.push_back({"root", 1});
    failure.start = [&](std::stop_token stop_token) {
        recorder.Add("start:failure");
        failure_started.store(true);
        std::unique_lock lock(wait_mutex);
        wait_condition.wait(lock, stop_token, [&] { return release; });
        return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_BAD_COMMAND;
    };
    auto blocked = Descriptor("blocked", &recorder);
    blocked.startup = anomaly::ServiceStartup::Async;
    blocked.required_dependencies.push_back({"failure", 1});

    bool result = Check(graph.Register(std::move(blocked)) == ERROR_SUCCESS,
                        "Async blocked registration failed") &&
        Check(graph.Register(std::move(failure)) == ERROR_SUCCESS,
              "Async failure registration failed") &&
        Check(graph.Register(std::move(root)) == ERROR_SUCCESS,
              "Async root registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Async failure graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS,
              "Async failure prevented the blocking gate from completing") &&
        Check(WaitUntil([&] { return failure_started.load(); }),
              "Failing async service did not begin");
    {
        std::scoped_lock lock(wait_mutex);
        release = true;
    }
    wait_condition.notify_all();
    result = Check(graph.WaitForAsync() == ERROR_BAD_COMMAND,
                   "Async failure was not returned by WaitForAsync") && result;

    const auto snapshot = graph.Snapshot();
    const auto* root_snapshot = FindService(snapshot, "root");
    const auto* failed_snapshot = FindService(snapshot, "failure");
    const auto* blocked_snapshot = FindService(snapshot, "blocked");
    return Check(snapshot.async_startup_complete &&
                     snapshot.async_startup_error == ERROR_BAD_COMMAND,
                 "Async failure snapshot was not settled") &&
        Check(root_snapshot != nullptr && root_snapshot->state == anomaly::ServiceState::Stopped,
              "Async failure did not roll back its Ready dependency") &&
        Check(failed_snapshot != nullptr &&
                  failed_snapshot->state == anomaly::ServiceState::Failed &&
                  failed_snapshot->error == ERROR_BAD_COMMAND,
              "Async failure diagnostics were lost") &&
        Check(blocked_snapshot != nullptr &&
                  blocked_snapshot->failure_chain ==
                      std::vector<std::string>{"failure", "blocked"},
              "Async dependent failure chain was lost") &&
        CheckValues(recorder.Values(),
                    {"start:root", "start:failure", "stop:root"},
                    "Async failure rollback order was incorrect") && result;
}

bool TestStopCancelsAndJoinsAsyncBeforeReverseStop() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    std::atomic_bool async_started{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;

    auto root = Descriptor("root", &recorder);
    auto async = Descriptor("async", &recorder);
    async.startup = anomaly::ServiceStartup::Async;
    async.required_dependencies.push_back({"root", 1});
    async.start = [&](std::stop_token stop_token) {
        recorder.Add("start:async");
        async_started.store(true);
        std::unique_lock lock(wait_mutex);
        wait_condition.wait(lock, stop_token, [] { return false; });
        recorder.Add("exit:async");
        return ERROR_SUCCESS;
    };

    bool result = Check(graph.Register(std::move(async)) == ERROR_SUCCESS,
                        "Cancellable async registration failed") &&
        Check(graph.Register(std::move(root)) == ERROR_SUCCESS,
              "Cancellable async root registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Cancellable async graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS,
              "Cancellable async blocking gate failed") &&
        Check(WaitUntil([&] { return async_started.load(); }),
              "Cancellable async service did not begin");
    graph.StopAll();
    return CheckValues(
               recorder.Values(),
               {"start:root", "start:async", "exit:async", "stop:async", "stop:root"},
               "StopAll did not join async startup before reverse stop") && result;
}

bool TestCancelStartupDefersRollbackUntilStopAll() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    std::atomic_bool async_started{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;

    auto root = Descriptor("root", &recorder);
    auto async = Descriptor("async", &recorder);
    async.startup = anomaly::ServiceStartup::Async;
    async.required_dependencies.push_back({"root", 1});
    async.start = [&](std::stop_token stop_token) {
        recorder.Add("start:async");
        async_started.store(true);
        std::unique_lock lock(wait_mutex);
        wait_condition.wait(lock, stop_token, [] { return false; });
        recorder.Add("exit:async");
        return ERROR_SUCCESS;
    };

    bool result = Check(graph.Register(std::move(async)) == ERROR_SUCCESS,
                        "Deferred-cancel async registration failed") &&
        Check(graph.Register(std::move(root)) == ERROR_SUCCESS,
              "Deferred-cancel root registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS,
              "Deferred-cancel graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS,
              "Deferred-cancel blocking gate failed") &&
        Check(WaitUntil([&] { return async_started.load(); }),
              "Deferred-cancel async service did not begin");

    graph.CancelStartup();
    result = Check(graph.WaitForAsync() == ERROR_CANCELLED,
                   "CancelStartup did not settle async startup as cancelled") && result;
    const auto cancelled = graph.Snapshot();
    const auto* root_snapshot = FindService(cancelled, "root");
    const auto* async_snapshot = FindService(cancelled, "async");
    result = Check(cancelled.async_startup_complete &&
                       cancelled.async_startup_error == ERROR_CANCELLED,
                   "CancelStartup snapshot did not settle") &&
        Check(root_snapshot != nullptr &&
                  root_snapshot->state == anomaly::ServiceState::Ready,
              "CancelStartup stopped the Ready root service") &&
        Check(async_snapshot != nullptr &&
                  async_snapshot->state == anomaly::ServiceState::Ready,
              "CancelStartup did not retain the completed async service") &&
        CheckValues(recorder.Values(), {"start:root", "start:async", "exit:async"},
                    "CancelStartup rolled services back before StopAll") && result;

    graph.StopAll();
    result = CheckValues(
                 recorder.Values(),
                 {"start:root", "start:async", "exit:async", "stop:async", "stop:root"},
                 "StopAll did not stop deferred services in reverse order") && result;
    graph.StopAll();
    return CheckValues(
               recorder.Values(),
               {"start:root", "start:async", "exit:async", "stop:async", "stop:root"},
               "Deferred services were stopped more than once") && result;
}

bool TestCancelStartupReleasesAsyncWaiter() {
    anomaly::ServiceGraph graph;
    std::atomic_bool async_started{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;

    auto async = Descriptor("waited-async");
    async.startup = anomaly::ServiceStartup::Async;
    async.start = [&](std::stop_token stop_token) {
        async_started.store(true);
        std::unique_lock lock(wait_mutex);
        wait_condition.wait(lock, stop_token, [] { return false; });
        return ERROR_SUCCESS;
    };
    bool result = Check(graph.Register(std::move(async)) == ERROR_SUCCESS,
                        "Waiter async registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Waiter graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS,
              "Waiter graph did not release its empty blocking gate") &&
        Check(WaitUntil([&] { return async_started.load(); }),
              "Waiter async callback did not begin");

    std::atomic_bool waiter_entered{};
    std::atomic_bool waiter_returned{};
    std::atomic<DWORD> waiter_result{ERROR_GEN_FAILURE};
    std::jthread waiter([&] {
        waiter_entered.store(true);
        waiter_result.store(graph.WaitForAsync());
        waiter_returned.store(true);
    });
    result = Check(WaitUntil([&] { return waiter_entered.load(); }),
                   "WaitForAsync worker did not begin") && result;

    graph.CancelStartup();
    result = Check(WaitUntil([&] { return waiter_returned.load(); }),
                   "CancelStartup did not release the WaitForAsync worker") && result;
    waiter.join();
    result = Check(waiter_result.load() == ERROR_CANCELLED,
                   "WaitForAsync worker observed the wrong cancellation result") && result;
    graph.StopAll();
    return result;
}

bool TestRootStartSkipsPluginScopedServices() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    auto scoped = Descriptor("scoped", &recorder);
    scoped.lifetime = anomaly::ServiceLifetime::PluginScoped;
    bool result = Check(graph.Register(std::move(scoped)) == ERROR_SUCCESS,
                        "Plugin-scoped registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Plugin-scoped graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS, "Root startup failed") &&
        Check(graph.WaitForAsync() == ERROR_SUCCESS, "Root startup did not settle") &&
        Check(recorder.Values().empty(),
              "Root startup instantiated a PluginScoped descriptor");
    const auto root_snapshot = graph.Snapshot();
    const auto* scoped_snapshot = FindService(root_snapshot, "scoped");
    result = Check(scoped_snapshot != nullptr &&
                       scoped_snapshot->state == anomaly::ServiceState::Registered,
                   "Skipped PluginScoped service did not remain Registered") &&
        Check(graph.StartService("scoped") == ERROR_SUCCESS,
              "Explicit PluginScoped startup failed") &&
        Check(graph.WaitForAsync() == ERROR_SUCCESS,
              "Explicit PluginScoped startup did not settle") &&
        CheckValues(recorder.Values(), {"start:scoped"},
                    "Explicit PluginScoped startup was not invoked") && result;
    graph.StopAll();
    return CheckValues(recorder.Values(), {"start:scoped", "stop:scoped"},
                       "Explicit PluginScoped service was not stopped") && result;
}

bool TestPreCancelledAsyncStartupDoesNotReleaseSuccessGate() {
    Recorder recorder;
    anomaly::ServiceGraph graph;
    auto service = Descriptor("async", &recorder);
    service.startup = anomaly::ServiceStartup::Async;
    std::stop_source stop;
    stop.request_stop();
    bool result = Check(graph.Register(std::move(service)) == ERROR_SUCCESS,
                        "Pre-cancelled async registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Pre-cancelled async graph did not build") &&
        Check(graph.StartAll(stop.get_token()) == ERROR_CANCELLED,
              "Pre-cancelled async startup released a successful blocking gate") &&
        Check(graph.WaitForAsync() == ERROR_CANCELLED,
              "Pre-cancelled async result was not awaitable") &&
        Check(recorder.Values().empty(),
              "Pre-cancelled async startup invoked its callback");
    const auto snapshot = graph.Snapshot();
    return Check(snapshot.stop_requested && snapshot.blocking_startup_complete &&
                     snapshot.async_startup_complete &&
                     snapshot.async_startup_error == ERROR_CANCELLED,
                 "Pre-cancelled async snapshot was inconsistent") && result;
}

bool TestAsyncFailureCancelsConcurrentBlockingStart() {
    anomaly::ServiceGraph graph;
    std::atomic_bool blocking_observed_stop{};
    anomaly::ServiceDescriptor failure = Descriptor("failure");
    failure.startup = anomaly::ServiceStartup::Async;
    failure.start = [](std::stop_token) { return ERROR_BAD_COMMAND; };
    anomaly::ServiceDescriptor blocking = Descriptor("blocking");
    blocking.start = [&](std::stop_token stop_token) {
        std::mutex wait_mutex;
        std::condition_variable_any wait_condition;
        std::unique_lock lock(wait_mutex);
        wait_condition.wait(lock, stop_token, [] { return false; });
        blocking_observed_stop.store(stop_token.stop_requested());
        return ERROR_CANCELLED;
    };

    bool result = Check(graph.Register(std::move(failure)) == ERROR_SUCCESS,
                        "Concurrent async failure registration failed") &&
        Check(graph.Register(std::move(blocking)) == ERROR_SUCCESS,
              "Concurrent blocking registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS,
              "Concurrent async failure graph did not build") &&
        Check(graph.StartAll() == ERROR_BAD_COMMAND,
              "Concurrent async root error was hidden by blocking cancellation") &&
        Check(graph.WaitForAsync() == ERROR_BAD_COMMAND,
              "Concurrent async root error was not awaitable") &&
        Check(blocking_observed_stop.load(),
              "Async failure did not promptly cancel a concurrent blocking start");
    const auto snapshot = graph.Snapshot();
    const auto* failure_snapshot = FindService(snapshot, "failure");
    return Check(snapshot.error == ERROR_BAD_COMMAND,
                 "Concurrent async root error was not retained by the graph") &&
        Check(failure_snapshot != nullptr &&
                     failure_snapshot->error == ERROR_BAD_COMMAND,
                 "Concurrent async failure diagnostics were lost") && result;
}

bool TestAsyncCallbackCanRequestStopWithoutDeadlock() {
    anomaly::ServiceGraph graph;
    std::atomic_bool callback_returned{};
    anomaly::ServiceDescriptor service = Descriptor("self-stop");
    service.startup = anomaly::ServiceStartup::Async;
    service.start = [&](std::stop_token stop_token) {
        graph.StopAll();
        callback_returned.store(true);
        return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_GEN_FAILURE;
    };
    bool result = Check(graph.Register(std::move(service)) == ERROR_SUCCESS,
                        "Self-stopping async registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Self-stopping async graph did not build") &&
        Check(graph.StartAll() == ERROR_SUCCESS,
              "Self-stopping async service blocked the empty blocking gate") &&
        Check(graph.WaitForAsync() == ERROR_CANCELLED,
              "Self-stopping async result was not reported") &&
        Check(callback_returned.load(),
              "StopAll deadlocked inside an async start callback");
    return result;
}

bool TestServiceGraphJsonFailedStartupSnapshot() {
    anomaly::ServiceGraph graph;
    auto blocked = Descriptor("blocked");
    blocked.required_dependencies.push_back({"failure", 1});
    auto failure = Descriptor("failure", nullptr, ERROR_BAD_COMMAND);
    bool result = Check(graph.Register(std::move(blocked)) == ERROR_SUCCESS &&
                            graph.Register(std::move(failure)) == ERROR_SUCCESS,
                        "Failed JSON graph registration failed") &&
        Check(graph.Build() == ERROR_SUCCESS, "Failed JSON graph did not build") &&
        Check(graph.StartAll() == ERROR_BAD_COMMAND,
              "Failed JSON graph returned the wrong startup error");

    const std::string output =
        anomaly::SerializeServiceGraphSnapshotJson(graph.Snapshot());
    const std::string error_field = "\"error\":" + std::to_string(ERROR_BAD_COMMAND);
    const std::string async_error_field =
        "\"async_startup_error\":" + std::to_string(ERROR_BAD_COMMAND);
    const std::string root_failure =
        "{\"service_id\":\"failure\",\"error\":" +
        std::to_string(ERROR_BAD_COMMAND) + ",\"caused_by\":\"\"}";
    return Check(output.find("\"startup_active\":false") != std::string::npos,
                 "Failed graph JSON reported active startup") &&
        Check(output.find("\"blocking_startup_complete\":true") != std::string::npos &&
                  output.find("\"async_startup_complete\":true") != std::string::npos,
              "Failed graph JSON omitted completion state") &&
        Check(output.find(error_field) != std::string::npos &&
                  output.find(async_error_field) != std::string::npos,
              "Failed graph JSON omitted startup errors") &&
        Check(output.find(root_failure) != std::string::npos,
              "Failed graph JSON omitted the root failure") &&
        Check(output.find("\"failure_chain\":[\"failure\",\"blocked\"]") !=
                  std::string::npos,
              "Failed graph JSON omitted the dependent failure chain") &&
        result;
}

bool TestServiceGraphJsonEscapesAllDynamicStrings() {
    anomaly::ServiceGraphSnapshot snapshot;
    snapshot.failures.push_back(
        {"failure\"\\\n", ERROR_INVALID_DATA, std::string("cause\t") + '\x01'});

    anomaly::ServiceSnapshot service;
    service.id = "service\r\n";
    service.dependencies.push_back(
        {"dep\\\"", 1, true, false, 0, anomaly::ServiceState::Registered});
    service.failure_chain = {"chain\b", "tail\f"};
    snapshot.services.push_back(std::move(service));

    const std::string output = anomaly::SerializeServiceGraphSnapshotJson(snapshot);
    return Check(
               output.find(R"json("service_id":"failure\"\\\n")json") !=
                   std::string::npos,
               "Failure service ID was not JSON escaped") &&
        Check(output.find(R"json("caused_by":"cause\t\u0001")json") !=
                  std::string::npos,
              "Failure cause was not JSON escaped") &&
        Check(output.find(R"json("id":"service\r\n")json") != std::string::npos,
              "Service ID was not JSON escaped") &&
        Check(output.find(R"json("id":"dep\\\"")json") != std::string::npos,
              "Dependency ID was not JSON escaped") &&
        Check(output.find(R"json("failure_chain":["chain\b","tail\f"])json") !=
                  std::string::npos,
              "Failure chain was not JSON escaped") &&
        Check(output.find('\n') == std::string::npos &&
                  output.find('\r') == std::string::npos &&
                  output.find('\t') == std::string::npos &&
                  output.find('\x01') == std::string::npos,
              "Graph JSON contains an unescaped control character");
}

}  // namespace

int main() {
    if (!TestDuplicateId()) return 1;
    if (!TestMissingRequiredDependency()) return 2;
    if (!TestDependencyVersion()) return 3;
    if (!TestCycle()) return 4;
    if (!TestTopologyReverseStopAndSnapshot()) return 5;
    if (!TestProvidedServiceIsNotOwned()) return 6;
    if (!TestPartialFailureAndFailureChain()) return 7;
    if (!TestCancellationRollsBackReadyServices()) return 8;
    if (!TestOptionalMissingAndLazyStart()) return 9;
    if (!TestStopIsIdempotentAndCallbacksAreUnlocked()) return 10;
    if (!TestAsyncReadyWaveRunsConcurrently()) return 11;
    if (!TestBlockingWaitsForAsyncDependency()) return 12;
    if (!TestAsyncFailureIsObservableAndRollsBack()) return 13;
    if (!TestStopCancelsAndJoinsAsyncBeforeReverseStop()) return 14;
    if (!TestCancelStartupDefersRollbackUntilStopAll()) return 15;
    if (!TestCancelStartupReleasesAsyncWaiter()) return 16;
    if (!TestRootStartSkipsPluginScopedServices()) return 17;
    if (!TestPreCancelledAsyncStartupDoesNotReleaseSuccessGate()) return 18;
    if (!TestAsyncFailureCancelsConcurrentBlockingStart()) return 19;
    if (!TestAsyncCallbackCanRequestStopWithoutDeadlock()) return 20;
    if (!TestServiceGraphJsonFailedStartupSnapshot()) return 22;
    if (!TestServiceGraphJsonEscapesAllDynamicStrings()) return 23;
    return 0;
}
