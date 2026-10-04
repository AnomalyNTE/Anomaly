#include <anomaly/sdk/cpp.hpp>
#include "cosmetics.hpp"
#include <chrono>

namespace {
using namespace anomaly::sdk;
using Clock = std::chrono::steady_clock;

int category = 1;
Clock::time_point next_update{};
bool cosmetics_started{};
const AnomalyWindowServiceV1* window_service{};
AnomalyGenerationHandleV1 window_handle{};
int fallback_open{1};

bool EnsureWindow() noexcept {
    if (window_handle.id != 0) return true;
    if (!window_service || window_service->struct_size <
            offsetof(AnomalyWindowServiceV1, end) +
                sizeof(window_service->end) ||
        !window_service->register_window ||
        !window_service->release_window || !window_service->state ||
        !window_service->begin || !window_service->end) return false;
    AnomalyWindowSpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.id = StringView("skin-changer-lite-main");
    spec.title = StringView("换肤器 Lite");
    spec.initial_width = 430.0F;
    spec.initial_height = 540.0F;
    spec.minimum_width = 300.0F;
    spec.minimum_height = 300.0F;
    spec.maximum_width = 850.0F;
    spec.maximum_height = 900.0F;
    spec.default_open = 1;
    return window_service->register_window(window_service->user, &spec,
        &window_handle).code == ANOMALY_STATUS_V1_OK && window_handle.id != 0;
}

void ReleaseWindow() noexcept {
    if (window_handle.id == 0 || !window_service) return;
    if (window_service->release_window(window_service->user,
            window_handle).code == ANOMALY_STATUS_V1_OK)
        window_handle = {};
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) noexcept {
    window_service = Host(host).Query<AnomalyWindowServiceV1>(
        ANOMALY_WINDOW_SERVICE_V1_ID, ANOMALY_WINDOW_SERVICE_V1_VERSION).get();
    window_handle = {};
    fallback_open = 1;
    return CosmeticsLoad(host, context);
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) noexcept {
    category = 1;
    next_update = {};
    cosmetics_started = CosmeticsStart(context).code == ANOMALY_STATUS_V1_OK;
    static_cast<void>(EnsureWindow());
    return Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, uint32_t reason) noexcept {
    ReleaseWindow();
    if (!cosmetics_started) return Ok();
    const auto result = CosmeticsStop(context, reason);
    if (result.code == ANOMALY_STATUS_V1_OK) cosmetics_started = false;
    return result;
}

void ANOMALY_CALL Unload(void* context) noexcept {
    ReleaseWindow();
    CosmeticsUnload(context);
    window_service = nullptr;
}

void ANOMALY_CALL Update(void* context, double delta) noexcept {
    const auto now = Clock::now();
    if (cosmetics_started && now >= next_update) {
        next_update = now + std::chrono::milliseconds(100);
        CosmeticsUpdate(context, delta);
    }
}

void DrawContent(void* context, const AnomalyUiServiceV1* ui) {
    if (ui->button(ui->user, StringView("配饰##category"), 0, 0)) category = 1;
    ui->same_line(ui->user, 0, -1);
    if (ui->button(ui->user, StringView("滑翔翼##category"), 0, 0)) category = 2;
    if (cosmetics_started) CosmeticsDraw(context, ui, category);
    else ui->text(ui->user, StringView("当前游戏版本的配饰/滑翔翼接口不可用。"));
}

void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui) noexcept {
    try {
        if (!ui || !ui->begin_window || !ui->end_window || !ui->button ||
            !ui->same_line || !ui->text) return;
        if (EnsureWindow()) {
            AnomalyWindowStateV1 state{sizeof(state)};
            if (window_service->state(window_service->user, window_handle,
                    &state).code != ANOMALY_STATUS_V1_OK || state.open == 0) return;
            std::int32_t visible{};
            if (window_service->begin(window_service->user, window_handle,
                    0, &visible).code != ANOMALY_STATUS_V1_OK) return;
            if (visible != 0) DrawContent(context, ui);
            static_cast<void>(window_service->end(window_service->user, window_handle));
            return;
        }
        if (fallback_open == 0) return;
        UiWindow window(ui, "换肤器 Lite", &fallback_open);
        if (window) DrawContent(context, ui);
    } catch (...) {}
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("b1ank.skin-changer"), anomaly::sdk::StringView("换肤器 Lite"),
        anomaly::sdk::StringView("b1ank"), anomaly::sdk::StringView("0.1.9"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
