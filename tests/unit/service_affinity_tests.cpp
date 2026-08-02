#include "anomaly/service_graph.hpp"

#include <Windows.h>

#include <atomic>
#include <iostream>
#include <thread>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

}  // namespace

int main() {
    anomaly::ServiceGraph graph;
    std::atomic<DWORD> start_thread{};
    std::atomic<DWORD> stop_thread{};
    std::atomic_int start_calls{};
    std::atomic_int stop_calls{};

    graph.SetAffinityExecutors({
        [&](anomaly::ServiceAffinity affinity,
            std::function<DWORD(std::stop_token)> callback,
            std::stop_token token) -> DWORD {
            if (affinity == anomaly::ServiceAffinity::Worker) {
                std::thread worker([&] {
                    start_thread.store(GetCurrentThreadId());
                    ++start_calls;
                    static_cast<void>(callback(token));
                });
                worker.join();
                return ERROR_SUCCESS;
            }
            return callback(token);
        },
        [&](anomaly::ServiceAffinity affinity, std::function<void()> callback) {
            if (affinity == anomaly::ServiceAffinity::Worker) {
                std::thread worker([&] {
                    stop_thread.store(GetCurrentThreadId());
                    ++stop_calls;
                    callback();
                });
                worker.join();
                return;
            }
            callback();
        }});

    anomaly::ServiceDescriptor service;
    service.id = "fixture.affinity";
    service.affinity = anomaly::ServiceAffinity::Worker;
    service.start = [](std::stop_token) { return ERROR_SUCCESS; };
    service.stop = [] {};
    if (!Check(graph.Register(std::move(service)) == ERROR_SUCCESS, "register failed")) return 1;
    if (!Check(graph.Build() == ERROR_SUCCESS, "build failed")) return 2;
    if (!Check(graph.StartAll() == ERROR_SUCCESS, "start failed")) return 3;

    const auto started = graph.Snapshot().services.front();
    const DWORD caller_thread = GetCurrentThreadId();
    bool result = Check(start_calls.load() == 1, "worker start executor was not called") &&
        Check(start_thread.load() != 0 && start_thread.load() != caller_thread,
              "worker start ran on the caller thread") &&
        Check(started.start_thread_id == start_thread.load(),
              "start thread evidence was not captured") &&
        Check(!started.affinity_bypassed, "start affinity was marked bypassed");

    graph.StopAll();
    const auto stopped = graph.Snapshot().services.front();
    result = Check(stop_calls.load() == 1, "worker stop executor was not called") && result &&
        Check(stop_thread.load() != 0 && stop_thread.load() != caller_thread,
              "worker stop ran on the caller thread") &&
        Check(stopped.stop_thread_id == stop_thread.load(),
              "stop thread evidence was not captured") &&
        Check(!stopped.affinity_bypassed, "stop affinity was marked bypassed");
    return result ? 0 : 4;
}
