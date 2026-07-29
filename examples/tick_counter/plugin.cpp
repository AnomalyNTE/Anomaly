#include "anomaly/sdk/cpp.hpp"

#include <cstdio>

namespace {

const AnomalyUiServiceV1* g_ui{};
unsigned long long g_ticks{};

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    const auto ui = anomaly::sdk::Host(host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();
    *context = &g_ticks;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }

void ANOMALY_CALL Unload(void*) {
    g_ui = nullptr;
    g_ticks = 0;
}

void ANOMALY_CALL Update(void*, double) { ++g_ticks; }

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;
    int open = 1;
    anomaly::sdk::UiWindow window(ui, "Game Tick Counter", &open);
    if (!window) return;
    char text[96]{};
    std::snprintf(text, sizeof(text), "Validated game-thread ticks: %llu", g_ticks);
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.example.tick-counter"),
        anomaly::sdk::StringView("Game Tick Counter"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
