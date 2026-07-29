#include "anomaly/ipc_registry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool Check(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

AnomalyIpcSchemaHashV1 Hash(const std::uint8_t seed) {
    AnomalyIpcSchemaHashV1 result{};
    for (std::size_t index = 0; index < std::size(result.bytes); ++index) {
        result.bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return result;
}

AnomalyIpcEndpointDescriptorV1 Descriptor() {
    static constexpr std::string_view id = "dev.anomaly.tests.echo";
    AnomalyIpcEndpointDescriptorV1 result{};
    result.struct_size = sizeof(result);
    result.endpoint_id = {id.data(), id.size()};
    result.major_version = 1;
    result.minor_version = 3;
    result.request_schema = Hash(1);
    result.response_schema = Hash(2);
    result.event_schema = Hash(3);
    result.modes = ANOMALY_IPC_MODE_V1_SYNC_REQUEST |
        ANOMALY_IPC_MODE_V1_ASYNC_REQUEST | ANOMALY_IPC_MODE_V1_EVENT;
    result.affinity = ANOMALY_IPC_AFFINITY_V1_WORKER;
    result.timeout_milliseconds = 1000;
    result.reentrancy = ANOMALY_IPC_REENTRANCY_V1_REJECT;
    result.maximum_request_bytes = 64;
    result.maximum_response_bytes = 64;
    result.maximum_event_bytes = 64;
    result.maximum_queue_depth = 8;
    return result;
}

AnomalyIpcEndpointSelectorV1 Selector() {
    const auto descriptor = Descriptor();
    AnomalyIpcEndpointSelectorV1 result{};
    result.struct_size = sizeof(result);
    result.endpoint_id = descriptor.endpoint_id;
    result.major_version = descriptor.major_version;
    result.minimum_minor_version = 2;
    result.request_schema = descriptor.request_schema;
    result.response_schema = descriptor.response_schema;
    result.event_schema = descriptor.event_schema;
    return result;
}

struct Fixture {
    std::shared_ptr<anomaly::ResourceLedger> ledger =
        std::make_shared<anomaly::ResourceLedger>();
    anomaly::IpcRegistry registry;
    anomaly::IpcPluginOwner provider{
        std::make_shared<anomaly::PluginScope>(ledger, "dev.anomaly.provider", 7), {}};
    anomaly::IpcPluginOwner consumer{
        std::make_shared<anomaly::PluginScope>(ledger, "dev.anomaly.consumer", 11),
        {"dev.anomaly.provider"}};
};

AnomalyStatusV1 ANOMALY_CALL Echo(
    void*, const AnomalyIpcRequestContextV1* context, const AnomalyByteSpanV1 request,
    const AnomalyMutableByteSpanV1 response, std::size_t* response_size) {
    if (context == nullptr || context->request_id == 0 || response_size == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    if (response.size < request.size) {
        *response_size = request.size;
        return {ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, 0, {}};
    }
    if (request.size != 0) std::memcpy(response.data, request.data, request.size);
    *response_size = request.size;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

bool TestValidationAndSynchronousCall() {
    Fixture fixture;
    auto descriptor = Descriptor();
    AnomalyGenerationHandleV1 endpoint{};
    if (!Check(fixture.registry.RegisterEndpoint(
            fixture.provider, &descriptor, Echo, nullptr, &endpoint).code ==
            ANOMALY_STATUS_V1_OK, "endpoint registration failed")) return false;

    const std::array<std::uint8_t, 4> request{1, 2, 3, 4};
    std::array<std::uint8_t, 8> response{};
    std::size_t response_size = response.size();
    auto selector = Selector();
    AnomalyStatusV1 status = fixture.registry.Invoke(
        fixture.consumer, anomaly::IpcCallingDomain::Lifecycle, &selector,
        {request.data(), request.size()}, {response.data(), response.size()}, &response_size);
    bool passed = Check(status.code == ANOMALY_STATUS_V1_OK && response_size == request.size() &&
            std::equal(request.begin(), request.end(), response.begin()),
        "synchronous IPC response mismatch");

    selector.minimum_minor_version = 4;
    status = fixture.registry.Invoke(
        fixture.consumer, anomaly::IpcCallingDomain::Lifecycle, &selector,
        {}, {response.data(), response.size()}, &response_size);
    passed = Check(status.code == ANOMALY_STATUS_V1_CONFLICT &&
            status.reserved == ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH,
        "minor version mismatch was not rejected") && passed;

    selector = Selector();
    selector.request_schema = Hash(9);
    status = fixture.registry.Invoke(
        fixture.consumer, anomaly::IpcCallingDomain::Lifecycle, &selector,
        {}, {response.data(), response.size()}, &response_size);
    passed = Check(status.code == ANOMALY_STATUS_V1_CONFLICT &&
            status.reserved == ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH,
        "schema mismatch was not rejected") && passed;

    selector = Selector();
    status = fixture.registry.Invoke(
        fixture.consumer, anomaly::IpcCallingDomain::Game, &selector,
        {}, {response.data(), response.size()}, &response_size);
    passed = Check(status.code == ANOMALY_STATUS_V1_UNAVAILABLE,
        "Game-domain blocking IPC was not rejected") && passed;

    anomaly::IpcPluginOwner undeclared{
        std::make_shared<anomaly::PluginScope>(fixture.ledger, "dev.anomaly.undeclared", 1), {}};
    status = fixture.registry.Invoke(
        undeclared, anomaly::IpcCallingDomain::Lifecycle, &selector,
        {}, {response.data(), response.size()}, &response_size);
    passed = Check(status.code == ANOMALY_STATUS_V1_PERMISSION_DENIED &&
            status.reserved == ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED,
        "undeclared provider dependency was not rejected") && passed;
    return passed;
}

struct AsyncResult {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete{};
    bool event{};
    std::array<std::uint8_t, 4> response{};
};

void ANOMALY_CALL Complete(
    void* user, AnomalyGenerationHandleV1, const AnomalyStatusV1 status,
    const AnomalyByteSpanV1 response) {
    auto& result = *static_cast<AsyncResult*>(user);
    std::scoped_lock lock(result.mutex);
    result.complete = status.code == ANOMALY_STATUS_V1_OK && response.size == result.response.size();
    if (result.complete) std::memcpy(result.response.data(), response.data, response.size);
    result.condition.notify_all();
}

void ANOMALY_CALL Event(
    void* user, AnomalyStringViewV1, const AnomalyByteSpanV1 event) {
    auto& result = *static_cast<AsyncResult*>(user);
    std::scoped_lock lock(result.mutex);
    result.event = event.size == 1 && event.data[0] == 42;
    result.condition.notify_all();
}

bool TestAsyncEventAndGenerationRevoke() {
    Fixture fixture;
    auto descriptor = Descriptor();
    AnomalyGenerationHandleV1 endpoint{};
    if (fixture.registry.RegisterEndpoint(
            fixture.provider, &descriptor, Echo, nullptr, &endpoint).code !=
        ANOMALY_STATUS_V1_OK) return false;
    auto selector = Selector();
    AsyncResult result;
    const std::array<std::uint8_t, 4> request{4, 3, 2, 1};
    AnomalyGenerationHandleV1 pending{};
    AnomalyGenerationHandleV1 subscription{};
    if (fixture.registry.InvokeAsync(
            fixture.consumer, &selector, {request.data(), request.size()},
            Complete, &result, &pending).code != ANOMALY_STATUS_V1_OK ||
        fixture.registry.Subscribe(
            fixture.consumer, &selector, Event, &result, &subscription).code !=
            ANOMALY_STATUS_V1_OK) {
        return false;
    }
    const std::uint8_t event = 42;
    if (fixture.registry.Publish(fixture.provider, endpoint, {&event, 1}).code !=
        ANOMALY_STATUS_V1_OK) return false;
    {
        std::unique_lock lock(result.mutex);
        result.condition.wait_for(lock, 2s, [&] { return result.complete && result.event; });
    }
    bool passed = Check(result.complete && result.event && result.response == request,
        "async IPC or event delivery failed");
    static_cast<void>(fixture.provider.scope->RevokeAll());
    const auto snapshot = fixture.registry.Snapshot();
    passed = Check(snapshot.endpoints.empty(), "revoked provider endpoint remained published") && passed;
    passed = Check(fixture.consumer.scope->Resources().empty(),
        "provider revoke did not clear consumer IPC resources") && passed;
    return passed;
}

struct CycleContext {
    anomaly::IpcRegistry* registry{};
    anomaly::IpcPluginOwner owner;
    AnomalyIpcEndpointSelectorV1 selector{};
};

AnomalyStatusV1 ANOMALY_CALL Cycle(
    void* user, const AnomalyIpcRequestContextV1*, AnomalyByteSpanV1,
    AnomalyMutableByteSpanV1 response, std::size_t* response_size) {
    auto& context = *static_cast<CycleContext*>(user);
    return context.registry->Invoke(
        context.owner, anomaly::IpcCallingDomain::Worker, &context.selector,
        {}, response, response_size);
}

bool TestReentrantCycle() {
    Fixture fixture;
    auto descriptor = Descriptor();
    CycleContext cycle{&fixture.registry, fixture.provider, Selector()};
    AnomalyGenerationHandleV1 endpoint{};
    if (fixture.registry.RegisterEndpoint(
            fixture.provider, &descriptor, Cycle, &cycle, &endpoint).code !=
        ANOMALY_STATUS_V1_OK) return false;
    std::array<std::uint8_t, 4> response{};
    std::size_t response_size = response.size();
    const AnomalyStatusV1 status = fixture.registry.Invoke(
        fixture.consumer, anomaly::IpcCallingDomain::Lifecycle, &cycle.selector,
        {}, {response.data(), response.size()}, &response_size);
    return Check(status.code == ANOMALY_STATUS_V1_CONFLICT &&
            status.reserved == ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE,
        "reentrant IPC cycle was not rejected");
}

AnomalyStatusV1 ANOMALY_CALL Fault(
    void*, const AnomalyIpcRequestContextV1*, AnomalyByteSpanV1,
    AnomalyMutableByteSpanV1, std::size_t*) {
    throw std::runtime_error("provider fault");
}

void ANOMALY_CALL IgnoreCompletion(
    void*, AnomalyGenerationHandleV1, AnomalyStatusV1, AnomalyByteSpanV1) {}

struct TimeoutCompletionResult {
    std::mutex mutex;
    std::condition_variable condition;
    bool timed_out{};
};

void ANOMALY_CALL TimeoutCompletion(
    void* user, AnomalyGenerationHandleV1, const AnomalyStatusV1 status,
    AnomalyByteSpanV1) {
    auto& result = *static_cast<TimeoutCompletionResult*>(user);
    std::scoped_lock lock(result.mutex);
    result.timed_out = status.code == ANOMALY_STATUS_V1_TIMEOUT &&
        status.reserved == ANOMALY_IPC_ERROR_V1_TIMEOUT;
    result.condition.notify_all();
}

struct DeferredDispatcher {
    std::mutex mutex;
    std::vector<std::function<void()>> tasks;

    bool Post(
        std::uint32_t, std::string, std::uint64_t, std::function<void()> callback) {
        std::scoped_lock lock(mutex);
        tasks.push_back(std::move(callback));
        return true;
    }

    void DropAll() {
        std::scoped_lock lock(mutex);
        tasks.clear();
    }
};

bool TestFaultTimeoutAndDroppedQueueCleanup() {
    {
        Fixture fixture;
        auto descriptor = Descriptor();
        descriptor.affinity = ANOMALY_IPC_AFFINITY_V1_CALLER;
        AnomalyGenerationHandleV1 endpoint{};
        if (fixture.registry.RegisterEndpoint(
                fixture.provider, &descriptor, Fault, nullptr, &endpoint).code !=
            ANOMALY_STATUS_V1_OK) return false;
        auto selector = Selector();
        std::array<std::uint8_t, 4> response{};
        std::size_t response_size = response.size();
        const AnomalyStatusV1 status = fixture.registry.Invoke(
            fixture.consumer, anomaly::IpcCallingDomain::Worker, &selector, {},
            {response.data(), response.size()}, &response_size);
        const auto snapshot = fixture.registry.Snapshot();
        if (!Check(status.code == ANOMALY_STATUS_V1_FAILED &&
                snapshot.endpoints.size() == 1 && snapshot.endpoints.front().calls == 1 &&
                snapshot.endpoints.front().failures == 1,
            "provider fault was not isolated and counted")) return false;
    }

    DeferredDispatcher dispatcher;
    auto ledger = std::make_shared<anomaly::ResourceLedger>();
    anomaly::IpcRegistry registry(
        [&dispatcher](auto affinity, auto owner, auto generation, auto callback) {
            return dispatcher.Post(
                affinity, std::move(owner), generation, std::move(callback));
        });
    anomaly::IpcPluginOwner provider{
        std::make_shared<anomaly::PluginScope>(ledger, "dev.anomaly.provider", 3), {}};
    anomaly::IpcPluginOwner consumer{
        std::make_shared<anomaly::PluginScope>(ledger, "dev.anomaly.consumer", 5),
        {"dev.anomaly.provider"}};
    auto descriptor = Descriptor();
    descriptor.affinity = ANOMALY_IPC_AFFINITY_V1_LIFECYCLE;
    descriptor.timeout_milliseconds = 10;
    descriptor.maximum_queue_depth = 1;
    AnomalyGenerationHandleV1 endpoint{};
    if (registry.RegisterEndpoint(provider, &descriptor, Echo, nullptr, &endpoint).code !=
        ANOMALY_STATUS_V1_OK) return false;
    auto selector = Selector();
    std::array<std::uint8_t, 4> response{};
    std::size_t response_size = response.size();
    AnomalyStatusV1 status = registry.Invoke(
        consumer, anomaly::IpcCallingDomain::Worker, &selector, {},
        {response.data(), response.size()}, &response_size);
    if (!Check(status.code == ANOMALY_STATUS_V1_TIMEOUT &&
            status.reserved == ANOMALY_IPC_ERROR_V1_TIMEOUT,
        "deferred synchronous call did not time out")) return false;
    dispatcher.DropAll();

    AnomalyGenerationHandleV1 pending{};
    TimeoutCompletionResult timeout_result;
    status = registry.InvokeAsync(
        consumer, &selector, {}, TimeoutCompletion, &timeout_result, &pending);
    if (!Check(status.code == ANOMALY_STATUS_V1_OK,
        "deferred asynchronous call was not accepted")) return false;
    {
        std::unique_lock lock(timeout_result.mutex);
        timeout_result.condition.wait_for(lock, 1s, [&] { return timeout_result.timed_out; });
    }
    if (!Check(timeout_result.timed_out && consumer.scope->Resources().empty(),
        "asynchronous timeout did not complete and release its pending call")) return false;
    dispatcher.DropAll();

    status = registry.InvokeAsync(
        consumer, &selector, {}, IgnoreCompletion, nullptr, &pending);
    if (!Check(status.code == ANOMALY_STATUS_V1_OK,
        "timeout did not return its queue reservation")) return false;
    if (!Check(registry.Cancel(consumer, pending).code == ANOMALY_STATUS_V1_CANCELLED,
        "queued asynchronous call was not cancelled")) return false;
    dispatcher.DropAll();
    if (!Check(consumer.scope->Resources().empty(),
        "cancelled asynchronous call remained in the consumer ledger")) return false;

    status = registry.InvokeAsync(
        consumer, &selector, {}, IgnoreCompletion, nullptr, &pending);
    if (!Check(status.code == ANOMALY_STATUS_V1_OK,
        "dropped asynchronous call did not return its queue reservation")) return false;
    static_cast<void>(registry.Cancel(consumer, pending));
    dispatcher.DropAll();
    const auto snapshot = registry.Snapshot();
    return Check(snapshot.endpoints.size() == 1 && snapshot.endpoints.front().timeouts == 2 &&
            snapshot.endpoints.front().pending_calls == 0,
        "timeout and pending-call diagnostics were inconsistent");
}

bool TestConcurrentPayloadsAndDiagnostics() {
    Fixture fixture;
    auto descriptor = Descriptor();
    descriptor.affinity = ANOMALY_IPC_AFFINITY_V1_CALLER;
    AnomalyGenerationHandleV1 endpoint{};
    if (fixture.registry.RegisterEndpoint(
            fixture.provider, &descriptor, Echo, nullptr, &endpoint).code !=
        ANOMALY_STATUS_V1_OK) return false;
    const auto selector = Selector();
    std::atomic_uint64_t accepted{};
    std::atomic_bool valid{true};
    std::vector<std::thread> threads;
    for (std::size_t thread_index = 0; thread_index < 8; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            std::array<std::uint8_t, 80> request{};
            std::array<std::uint8_t, 64> response{};
            for (std::size_t iteration = 0; iteration < 100; ++iteration) {
                const std::size_t size = (thread_index * 17U + iteration) % 81U;
                std::fill_n(request.begin(), size, static_cast<std::uint8_t>(iteration));
                std::size_t response_size = response.size();
                const AnomalyStatusV1 status = fixture.registry.Invoke(
                    fixture.consumer, anomaly::IpcCallingDomain::Worker, &selector,
                    {request.data(), size}, {response.data(), response.size()}, &response_size);
                if (size <= descriptor.maximum_request_bytes) {
                    ++accepted;
                    if (status.code != ANOMALY_STATUS_V1_OK || response_size != size ||
                        !std::equal(request.begin(), request.begin() + size, response.begin())) {
                        valid.store(false, std::memory_order_release);
                    }
                } else if (status.code != ANOMALY_STATUS_V1_INVALID_ARGUMENT) {
                    valid.store(false, std::memory_order_release);
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const auto snapshot = fixture.registry.Snapshot();
    return Check(valid.load(std::memory_order_acquire) && snapshot.endpoints.size() == 1 &&
            snapshot.endpoints.front().calls == accepted.load() &&
            snapshot.endpoints.front().consumers ==
                std::vector<std::string>{"dev.anomaly.consumer"} &&
            snapshot.endpoints.front().request_schema_hash.size() == 64 &&
            snapshot.endpoints.front().response_schema_hash.size() == 64 &&
            snapshot.endpoints.front().event_schema_hash.size() == 64,
        "concurrent payload calls or endpoint diagnostics were inconsistent");
}

}  // namespace

int main() {
    return TestValidationAndSynchronousCall() &&
        TestAsyncEventAndGenerationRevoke() && TestReentrantCycle() &&
        TestFaultTimeoutAndDroppedQueueCleanup() &&
        TestConcurrentPayloadsAndDiagnostics() ? 0 : 1;
}
