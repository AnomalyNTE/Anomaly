#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/interop.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <new>
#include <thread>

namespace {

struct Context final {
    const AnomalyHookServiceV1* hook{};
    AnomalyGenerationHandleV1 handle{};
    std::thread hold_thread;
    std::atomic_bool hold_started{};
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
    wchar_t hold_text[32]{};
    const DWORD hold_length = GetEnvironmentVariableW(
        L"ANOMALY_HOOK_FIXTURE_HOLD_MS", hold_text,
        static_cast<DWORD>(std::size(hold_text)));
    const long hold_value =
        hold_length == 0 || hold_length >= std::size(hold_text)
            ? 0
            : std::wcstol(hold_text, nullptr, 10);
    const int hold_ms =
        hold_value > 0 && hold_value <= 60000
            ? static_cast<int>(hold_value)
            : 0;
    if (hold_ms > 0) {
        context->hold_thread = std::thread([context, hold_ms] {
            AnomalyGenerationHandleV1 callback_lease{};
            const bool leased = context->hook->begin_callback(
                context->hook->user, context->handle,
                &callback_lease).code == ANOMALY_STATUS_V1_OK;
            SetEnvironmentVariableW(
                L"ANOMALY_HOOK_FIXTURE_STAGE",
                leased ? L"begin-ok" : L"begin-failed");
            context->hold_started.store(true, std::memory_order_release);
            if (leased) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(hold_ms));
                SetEnvironmentVariableW(
                    L"ANOMALY_HOOK_FIXTURE_STAGE", L"sleep-done");
                const AnomalyStatusV1 status = context->hook->end_callback(
                    context->hook->user, callback_lease);
                wchar_t stage[32]{};
                if (status.code == ANOMALY_STATUS_V1_OK) {
                    std::swprintf(stage, std::size(stage), L"end-ok");
                } else {
                    std::swprintf(
                        stage, std::size(stage), L"end-%u",
                        status.code);
                }
                SetEnvironmentVariableW(
                    L"ANOMALY_HOOK_FIXTURE_STAGE", stage);
            }
        });
        while (!context->hold_started.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(
    void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (context->hold_thread.joinable())
        context->hold_thread.join();
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
    if (context->hold_thread.joinable())
        context->hold_thread.join();
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
        anomaly::sdk::StringView("anomaly.hook-lifecycle"),
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
