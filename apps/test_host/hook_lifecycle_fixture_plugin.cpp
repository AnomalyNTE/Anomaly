#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/interop.h"

#include <cstddef>
#include <new>

namespace {

struct Context final {
    const AnomalyHookServiceV1* hook{};
    AnomalyGenerationHandleV1 handle{};
};

__declspec(noinline) int ANOMALY_CALL FixtureTarget(const int value) noexcept {
    return value + 1;
}

int ANOMALY_CALL FixtureDetour(const int value) noexcept {
    return value + 1;
}

bool HookReady(const AnomalyHookServiceV1* hook) noexcept {
    return hook != nullptr &&
        hook->struct_size >=
            offsetof(AnomalyHookServiceV1, end_callback) +
                sizeof(hook->end_callback) &&
        hook->create != nullptr && hook->release != nullptr &&
        hook->begin_callback != nullptr && hook->end_callback != nullptr;
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *plugin_context = nullptr;
    const auto hook = anomaly::sdk::Host(host).Query<AnomalyHookServiceV1>(
        ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION);
    if (!HookReady(hook.get()))
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    auto* context = new (std::nothrow) Context();
    if (context == nullptr)
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    context->hook = hook.get();
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target =
        reinterpret_cast<std::uintptr_t>(&FixtureTarget);
    request.detour = reinterpret_cast<void*>(&FixtureDetour);
    request.label = anomaly::sdk::StringView("lifecycle-fixture");
    std::uintptr_t original{};
    const AnomalyStatusV1 status = context->hook->create(
        context->hook->user, &request, &original, &context->handle);
    if (status.code != ANOMALY_STATUS_V1_OK ||
        context->handle.id == 0 || original == 0) {
        context->handle = {};
        return status.code == ANOMALY_STATUS_V1_OK
            ? AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}}
            : status;
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(
    void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (context->handle.id == 0)
        return anomaly::sdk::Ok();
    const AnomalyStatusV1 status = context->hook->release(
        context->hook->user, context->handle);
    if (status.code == ANOMALY_STATUS_V1_OK)
        context->handle = {};
    return status;
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr)
        return;
    if (context->handle.id != 0)
        static_cast<void>(context->hook->release(
            context->hook->user, context->handle));
    delete context;
}

void ANOMALY_CALL Update(void*, double) {}
void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1*) {}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr ||
        descriptor->struct_size <
            offsetof(AnomalyPluginDescriptorV1, on_draw) +
                sizeof(descriptor->on_draw)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.test.hook-lifecycle"),
        anomaly::sdk::StringView("Hook Lifecycle Fixture"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.0.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw};
    return anomaly::sdk::Ok();
}
