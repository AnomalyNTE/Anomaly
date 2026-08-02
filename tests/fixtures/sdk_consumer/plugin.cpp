#include "anomaly/sdk/cpp.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct Context final {
    const AnomalyHookServiceV1* hook{};
};

std::atomic_bool g_task_completed{};
std::atomic_uint32_t g_hook_calls{};
std::atomic_uint32_t g_second_hook_calls{};
const AnomalyHookServiceV1* g_hook_service{};
AnomalyGenerationHandleV1 g_hook_handle{};
AnomalyGenerationHandleV1 g_second_hook_handle{};
using HookTargetFn = int (ANOMALY_CALL *)(int);
HookTargetFn g_hook_original{};
HookTargetFn g_second_hook_original{};
std::array<std::uint8_t, 2> g_patch_target{{0x24, 0x42}};

constexpr AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {nullptr, 0}};
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const char* id, const std::uint32_t version = 1) {
    return anomaly::sdk::Host(host).Query<Service>(id, version).get();
}

bool WaitFor(const std::atomic_bool& value) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(5ms);
    }
    return value.load(std::memory_order_acquire);
}

AnomalyStatusV1 ANOMALY_CALL SelfTest(
    void*, const AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    constexpr std::string_view result = "ok";
    if (inout_size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (destination.data == nullptr || destination.size < result.size() || *inout_size < result.size()) {
        *inout_size = result.size();
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    for (std::size_t index = 0; index < result.size(); ++index) destination.data[index] = result[index];
    *inout_size = result.size();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Task(void*, AnomalyGenerationHandleV1) {
    g_task_completed.store(true, std::memory_order_release);
}

AnomalyStatusV1 ANOMALY_CALL Command(
    void*, AnomalyStringViewV1 arguments,
    AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    constexpr std::string_view result = "pong";
    if (arguments.size != 4 || std::string_view(arguments.data, arguments.size) != "ping" ||
        inout_size == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    if (destination.data == nullptr || destination.size < result.size() || *inout_size < result.size()) {
        *inout_size = result.size();
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    for (std::size_t index = 0; index < result.size(); ++index) destination.data[index] = result[index];
    *inout_size = result.size();
    return anomaly::sdk::Ok();
}

__declspec(noinline) int ANOMALY_CALL HookTarget(const int value) {
    return value + 1;
}

__declspec(noinline) int ANOMALY_CALL SecondHookTarget(const int value) {
    return value + 2;
}

__declspec(noinline) int ANOMALY_CALL HookDetour(const int value) {
    AnomalyGenerationHandleV1 callback_lease{};
    if (g_hook_service != nullptr &&
        g_hook_service->begin_callback(
            g_hook_service->user, g_hook_handle, &callback_lease).code == ANOMALY_STATUS_V1_OK) {
        ++g_hook_calls;
        static_cast<void>(g_hook_service->end_callback(g_hook_service->user, callback_lease));
    }
    return g_hook_original == nullptr ? value : g_hook_original(value) + 1;
}

__declspec(noinline) int ANOMALY_CALL SecondHookDetour(const int value) {
    AnomalyGenerationHandleV1 callback_lease{};
    if (g_hook_service != nullptr &&
        g_hook_service->begin_callback(
            g_hook_service->user, g_second_hook_handle, &callback_lease).code ==
            ANOMALY_STATUS_V1_OK) {
        ++g_second_hook_calls;
        static_cast<void>(g_hook_service->end_callback(g_hook_service->user, callback_lease));
    }
    return g_second_hook_original == nullptr ? value : g_second_hook_original(value) + 2;
}

bool ExerciseConfig(const AnomalyConfigServiceV1& service) {
    constexpr std::string_view schema_id = "settings";
    constexpr std::string_view schema =
        "{\"type\":\"object\",\"required\":[\"enabled\"],\"properties\":{\"enabled\":{\"type\":\"boolean\"}}}";
    constexpr std::string_view document = "{\"enabled\":true}";
    AnomalyGenerationHandleV1 handle{};
    if (service.register_schema(
            service.user, anomaly::sdk::StringView(schema_id), 1,
            {reinterpret_cast<const std::uint8_t*>(schema.data()), schema.size()}, &handle).code !=
        ANOMALY_STATUS_V1_OK ||
        service.write_atomic(
            service.user, anomaly::sdk::StringView(schema_id), 1,
            {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()}).code !=
            ANOMALY_STATUS_V1_OK) {
        return false;
    }
    std::uint32_t version{};
    std::size_t size{};
    if (service.read(
            service.user, anomaly::sdk::StringView(schema_id), &version, {nullptr, 0}, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        version != 1 || size == 0 || size > 1024) {
        return false;
    }
    std::array<std::uint8_t, 1024> output{};
    return service.read(
               service.user, anomaly::sdk::StringView(schema_id), &version,
               {output.data(), output.size()}, &size).code == ANOMALY_STATUS_V1_OK &&
        version == 1 && size != 0;
}

bool ExerciseStorage(const AnomalyStorageServiceV1& service) {
    constexpr std::array<std::uint8_t, 3> source{{1, 2, 3}};
    if (service.write_atomic(
            service.user, anomaly::sdk::StringView("cache.bin"),
            {source.data(), source.size()}).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    std::array<std::uint8_t, 3> destination{};
    std::size_t size = destination.size();
    return service.read(
               service.user, anomaly::sdk::StringView("cache.bin"),
               {destination.data(), destination.size()}, &size).code == ANOMALY_STATUS_V1_OK &&
        size == source.size() && destination == source;
}

bool ExerciseDiagnostics(const AnomalyDiagnosticsServiceV1& service) {
    AnomalyGenerationHandleV1 handle{};
    if (service.register_self_test(
            service.user, anomaly::sdk::StringView("health"), SelfTest, nullptr, &handle).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    std::array<std::uint8_t, 8> output{};
    std::size_t size = output.size();
    return service.run_self_test(
               service.user, anomaly::sdk::StringView("health"),
               {output.data(), output.size()}, &size).code == ANOMALY_STATUS_V1_OK &&
        size == 2 && output[0] == 'o' && output[1] == 'k';
}

bool ExerciseCollaboration(
    const AnomalySchedulerServiceV1& scheduler,
    const AnomalyCommandsServiceV1& commands,
    const AnomalyNotificationsServiceV1& notifications) {
    g_task_completed.store(false, std::memory_order_release);
    AnomalyGenerationHandleV1 task{};
    AnomalyGenerationHandleV1 command{};
    AnomalyGenerationHandleV1 notification{};
    if (scheduler.schedule(scheduler.user, 0, Task, nullptr, &task).code != ANOMALY_STATUS_V1_OK ||
        commands.register_command(
            commands.user, anomaly::sdk::StringView("ping"), anomaly::sdk::StringView("test"),
            Command, nullptr, &command).code != ANOMALY_STATUS_V1_OK ||
        notifications.post(
            notifications.user, ANOMALY_NOTIFICATION_V1_INFO,
            anomaly::sdk::StringView("External SDK"), anomaly::sdk::StringView("active"), 0,
            &notification).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    std::array<std::uint8_t, 8> output{};
    std::size_t size = output.size();
    if (commands.invoke(
            commands.user, anomaly::sdk::StringView("ping"), anomaly::sdk::StringView("ping"),
            {output.data(), output.size()}, &size).code != ANOMALY_STATUS_V1_OK ||
        size != 4 || std::string_view(reinterpret_cast<const char*>(output.data()), size) != "pong" ||
        !WaitFor(g_task_completed)) {
        return false;
    }
    return notifications.dismiss(notifications.user, notification).code == ANOMALY_STATUS_V1_OK;
}

bool ExercisePatch(const AnomalyPatchServiceV1& service) {
    constexpr std::array<std::uint8_t, 2> replacement{{0x99, 0x66}};
    AnomalyGenerationHandleV1 handle{};
    if (service.apply(
            service.user, reinterpret_cast<std::uintptr_t>(g_patch_target.data()),
            {replacement.data(), replacement.size()}, anomaly::sdk::StringView("external-patch"),
            &handle).code != ANOMALY_STATUS_V1_OK ||
        g_patch_target != replacement) {
        return false;
    }
    return service.release(service.user, handle).code == ANOMALY_STATUS_V1_OK &&
        g_patch_target == std::array<std::uint8_t, 2>{{0x24, 0x42}};
}

bool ExerciseHook(const AnomalyHookServiceV1& service) {
    g_hook_service = &service;
    g_hook_original = nullptr;
    g_second_hook_original = nullptr;
    g_hook_handle = {};
    g_second_hook_handle = {};
    g_hook_calls.store(0, std::memory_order_release);
    g_second_hook_calls.store(0, std::memory_order_release);
    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = reinterpret_cast<std::uintptr_t>(&HookTarget);
    request.detour = reinterpret_cast<void*>(&HookDetour);
    request.label = anomaly::sdk::StringView("external-hook-primary");
    std::uintptr_t original{};
    if (service.create(service.user, &request, &original, &g_hook_handle).code != ANOMALY_STATUS_V1_OK ||
        original == 0) {
        g_hook_service = nullptr;
        return false;
    }
    g_hook_original = reinterpret_cast<HookTargetFn>(original);
    AnomalyHookRequestV1 second_request = request;
    second_request.target = reinterpret_cast<std::uintptr_t>(&SecondHookTarget);
    second_request.detour = reinterpret_cast<void*>(&SecondHookDetour);
    second_request.label = anomaly::sdk::StringView("external-hook-secondary");
    std::uintptr_t second_original{};
    if (service.create(service.user, &second_request, &second_original, &g_second_hook_handle).code !=
            ANOMALY_STATUS_V1_OK ||
        second_original == 0 || g_second_hook_handle.id == g_hook_handle.id) {
        static_cast<void>(service.release(service.user, g_hook_handle));
        g_hook_original = nullptr;
        g_hook_service = nullptr;
        return false;
    }
    g_second_hook_original = reinterpret_cast<HookTargetFn>(second_original);
    HookTargetFn target = &HookTarget;
    HookTargetFn second_target = &SecondHookTarget;
    const bool both_detours_invoked =
        target(4) == 6 && second_target(4) == 8 &&
        g_hook_calls.load(std::memory_order_acquire) == 1 &&
        g_second_hook_calls.load(std::memory_order_acquire) == 1;
    const bool first_released =
        service.release(service.user, g_hook_handle).code == ANOMALY_STATUS_V1_OK &&
        target(4) == 5 && second_target(4) == 8 &&
        g_hook_calls.load(std::memory_order_acquire) == 1 &&
        g_second_hook_calls.load(std::memory_order_acquire) == 2;
    const bool second_released =
        service.release(service.user, g_second_hook_handle).code == ANOMALY_STATUS_V1_OK &&
        second_target(4) == 6;
    g_hook_original = nullptr;
    g_second_hook_original = nullptr;
    g_hook_service = nullptr;
    return both_detours_invoked && first_released && second_released;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto* config = Query<AnomalyConfigServiceV1>(host, ANOMALY_CONFIG_SERVICE_V1_ID);
    const auto* storage = Query<AnomalyStorageServiceV1>(host, ANOMALY_STORAGE_SERVICE_V1_ID);
    const auto* runtime = Query<AnomalyRuntimeInfoServiceV1>(host, ANOMALY_RUNTIME_INFO_SERVICE_V1_ID);
    const auto* diagnostics = Query<AnomalyDiagnosticsServiceV1>(host, ANOMALY_DIAGNOSTICS_SERVICE_V1_ID);
    const auto* scheduler = Query<AnomalySchedulerServiceV1>(host, ANOMALY_SCHEDULER_SERVICE_V1_ID);
    const auto* ipc = Query<AnomalyIpcServiceV1>(host, ANOMALY_IPC_SERVICE_V1_ID);
    const auto* commands = Query<AnomalyCommandsServiceV1>(host, ANOMALY_COMMANDS_SERVICE_V1_ID);
    const auto* notifications = Query<AnomalyNotificationsServiceV1>(host, ANOMALY_NOTIFICATIONS_SERVICE_V1_ID);
    const auto* hook = Query<AnomalyHookServiceV1>(
        host, ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION);
    const auto* patch = Query<AnomalyPatchServiceV1>(host, ANOMALY_PATCH_SERVICE_V1_ID);
    if (config == nullptr || storage == nullptr || runtime == nullptr || diagnostics == nullptr ||
        scheduler == nullptr || ipc == nullptr || commands == nullptr || notifications == nullptr ||
        hook == nullptr || patch == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    AnomalyRuntimeInfoV1 runtime_info{};
    runtime_info.struct_size = sizeof(runtime_info);
    if (runtime->snapshot(runtime->user, &runtime_info).code != ANOMALY_STATUS_V1_OK ||
        runtime_info.plugin_generation == 0 || !ExerciseConfig(*config) || !ExerciseStorage(*storage) ||
        !ExerciseDiagnostics(*diagnostics) ||
        !ExerciseCollaboration(*scheduler, *commands, *notifications) ||
        !ExercisePatch(*patch) || !ExerciseHook(*hook)) {
        return Status(ANOMALY_STATUS_V1_FAILED);
    }

    auto context = std::make_unique<Context>();
    context->hook = hook;
    *plugin_context = context.release();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    g_hook_original = nullptr;
    g_second_hook_original = nullptr;
    g_hook_service = nullptr;
    delete static_cast<Context*>(plugin_context);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("example.external-sdk"),
        anomaly::sdk::StringView("External SDK"),
        anomaly::sdk::StringView("Fixture"),
        anomaly::sdk::StringView("1.0.0"),
        Load,
        nullptr,
        nullptr,
        Unload,
        nullptr,
        nullptr,
    };
    return anomaly::sdk::Ok();
}
