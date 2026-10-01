#include <anomaly/sdk/cpp.hpp>
#include "cosmetics.hpp"
#include <chrono>

namespace {
using namespace anomaly::sdk;
using Clock = std::chrono::steady_clock;

int category = 1;
Clock::time_point next_update{};
bool cosmetics_started{};

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) noexcept {
    return CosmeticsLoad(host, context);
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) noexcept {
    category = 1;
    next_update = {};
    cosmetics_started = CosmeticsStart(context).code == ANOMALY_STATUS_V1_OK;
    return Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, uint32_t reason) noexcept {
    if (!cosmetics_started) return Ok();
    const auto result = CosmeticsStop(context, reason);
    if (result.code == ANOMALY_STATUS_V1_OK) cosmetics_started = false;
    return result;
}

void ANOMALY_CALL Unload(void* context) noexcept {
    CosmeticsUnload(context);
}

void ANOMALY_CALL Update(void* context, double delta) noexcept {
    const auto now = Clock::now();
    if (cosmetics_started && now >= next_update) {
        next_update = now + std::chrono::milliseconds(100);
        CosmeticsUpdate(context, delta);
    }
}

void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui) noexcept {
    try {
        if (!ui || !ui->begin_window || !ui->end_window || !ui->button || !ui->same_line || !ui->text) return;
        UiWindow window(ui, "换肤器 Lite");
        if (!window) return;
        if (ui->button(ui->user, StringView("配饰##category"), 0, 0)) category = 1;
        ui->same_line(ui->user, 0, -1);
        if (ui->button(ui->user, StringView("滑翔翼##category"), 0, 0)) category = 2;
        if (cosmetics_started) CosmeticsDraw(context, ui, category);
        else ui->text(ui->user, StringView("当前游戏版本的配饰/滑翔翼接口不可用。"));
    } catch (...) {}
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("b1ank.skin-changer"), anomaly::sdk::StringView("换肤器 Lite"),
        anomaly::sdk::StringView("b1ank"), anomaly::sdk::StringView("0.1.8"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
