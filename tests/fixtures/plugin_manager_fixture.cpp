#include "anomaly/sdk/anomaly_sdk.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

constexpr const char* kPluginId = "anomaly.test.plugin-manager-fixture";

constexpr AnomalyStringViewV1 View(const char* text, std::size_t size) {
    return {text, size};
}

constexpr AnomalyStatusV1 OkV1() { return {ANOMALY_STATUS_V1_OK, 0, {nullptr, 0}}; }

struct SampleV1Context {
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyUe5AhudServiceV1* ahud{};
    AnomalyGenerationHandleV1 ahud_explicit{};
    AnomalyGenerationHandleV1 ahud_scoped{};
    AnomalyGenerationHandleV1 ahud_self{};
};

SampleV1Context g_v1;

void Log(const char* message, const std::size_t size) {
    if (g_v1.core != nullptr && g_v1.core->log != nullptr) {
        g_v1.core->log(
            g_v1.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            View(message, size));
    }
}

void ANOMALY_CALL OnAhudDraw(
    void* user, const AnomalyUe5AhudFrameV1* frame) {
    Log("AHUD fixture callback", sizeof("AHUD fixture callback") - 1);
    if (user == &g_v1.ahud_self && g_v1.ahud != nullptr &&
        g_v1.ahud->unsubscribe != nullptr && g_v1.ahud_self.id != 0) {
        const AnomalyStatusV1 status = g_v1.ahud->unsubscribe(
            g_v1.ahud->user, g_v1.ahud_self);
        g_v1.ahud_self = {};
        char message[96]{};
        const int length = std::snprintf(
            message, sizeof(message),
            "AHUD self-unsubscribe status=%u", status.code);
        if (length > 0) {
            Log(
                message,
                static_cast<std::size_t>((std::min)(
                    length, static_cast<int>(sizeof(message) - 1))));
        }
        throw 1;
    }
    if (frame == nullptr || frame->project == nullptr) return;
    const double world[3]{};
    float screen[2]{};
    double depth{};
    static_cast<void>(frame->project(frame->user, world, screen, &depth));
}

AnomalyStatusV1 ANOMALY_CALL OnLoadV1(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr || host->query_service == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    const void* core{};
    const AnomalyStatusV1 core_status = host->query_service(
        host->host_context,
        View(ANOMALY_CORE_SERVICE_V1_ID, sizeof(ANOMALY_CORE_SERVICE_V1_ID) - 1),
        ANOMALY_CORE_SERVICE_V1_VERSION, &core);
    if (core_status.code != ANOMALY_STATUS_V1_OK || core == nullptr) return core_status;
    g_v1.core = static_cast<const AnomalyCoreServiceV1*>(core);
    const void* ui{};
    const AnomalyStatusV1 status = host->query_service(
        host->host_context,
        View(ANOMALY_UI_SERVICE_V1_ID, sizeof(ANOMALY_UI_SERVICE_V1_ID) - 1),
        ANOMALY_UI_SERVICE_V1_VERSION, &ui);
    if (status.code != ANOMALY_STATUS_V1_OK || ui == nullptr) return status;
    g_v1.ui = static_cast<const AnomalyUiServiceV1*>(ui);
    const void* player{};
    const AnomalyStatusV1 player_status = host->query_service(
        host->host_context,
        View(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, sizeof(ANOMALY_NTE_PLAYER_SERVICE_V1_ID) - 1),
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION, &player);
    g_v1.player = player_status.code == ANOMALY_STATUS_V1_OK
        ? static_cast<const AnomalyNtePlayerServiceV1*>(player)
        : nullptr;
    const void* ahud{};
    const AnomalyStatusV1 ahud_status = host->query_service(
        host->host_context,
        View(ANOMALY_UE5_AHUD_SERVICE_V1_ID, sizeof(ANOMALY_UE5_AHUD_SERVICE_V1_ID) - 1),
        ANOMALY_UE5_AHUD_SERVICE_V1_VERSION, &ahud);
    char ahud_status_message[64]{};
    const int ahud_status_length = std::snprintf(
        ahud_status_message, sizeof(ahud_status_message),
        "AHUD query status=%u", ahud_status.code);
    if (ahud_status_length > 0) {
        Log(
            ahud_status_message,
            static_cast<std::size_t>((std::min)(
                ahud_status_length,
                static_cast<int>(sizeof(ahud_status_message) - 1))));
    }
    g_v1.ahud = ahud_status.code == ANOMALY_STATUS_V1_OK
        ? static_cast<const AnomalyUe5AhudServiceV1*>(ahud)
        : nullptr;
    *plugin_context = &g_v1;
    return OkV1();
}

AnomalyStatusV1 ANOMALY_CALL OnStartV1(void*) {
    if (g_v1.ahud == nullptr || g_v1.ahud->subscribe == nullptr) return OkV1();
    AnomalyStatusV1 status = g_v1.ahud->subscribe(
        g_v1.ahud->user, OnAhudDraw, nullptr, &g_v1.ahud_explicit);
    if (status.code != ANOMALY_STATUS_V1_OK) return status;
    status = g_v1.ahud->subscribe(
        g_v1.ahud->user, OnAhudDraw, nullptr, &g_v1.ahud_scoped);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        if (g_v1.ahud->unsubscribe != nullptr) {
            static_cast<void>(g_v1.ahud->unsubscribe(
                g_v1.ahud->user, g_v1.ahud_explicit));
        }
        g_v1.ahud_explicit = {};
        return status;
    }
    status = g_v1.ahud->subscribe(
        g_v1.ahud->user, OnAhudDraw, &g_v1.ahud_self, &g_v1.ahud_self);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        if (g_v1.ahud->unsubscribe != nullptr) {
            static_cast<void>(g_v1.ahud->unsubscribe(
                g_v1.ahud->user, g_v1.ahud_explicit));
            static_cast<void>(g_v1.ahud->unsubscribe(
                g_v1.ahud->user, g_v1.ahud_scoped));
        }
        g_v1.ahud_explicit = {};
        g_v1.ahud_scoped = {};
    }
    return status;
}

AnomalyStatusV1 ANOMALY_CALL OnStopV1(void*, std::uint32_t) {
    g_v1.ahud_explicit = {};
    g_v1.ahud_scoped = {};
    g_v1.ahud_self = {};
    return OkV1();
}
void ANOMALY_CALL OnUnloadV1(void*) { g_v1 = {}; }
void ANOMALY_CALL OnUpdateV1(void*, double) {
    if (g_v1.ahud == nullptr || g_v1.ahud->unsubscribe == nullptr ||
        g_v1.ahud_explicit.id == 0) {
        return;
    }
    const AnomalyStatusV1 status = g_v1.ahud->unsubscribe(
        g_v1.ahud->user, g_v1.ahud_explicit);
    g_v1.ahud_explicit = {};
    char message[80]{};
    const int length = std::snprintf(
        message, sizeof(message), "AHUD explicit unsubscribe status=%u", status.code);
    if (length > 0) {
        Log(
            message,
            static_cast<std::size_t>((std::min)(
                length, static_cast<int>(sizeof(message) - 1))));
    }
}

void ANOMALY_CALL OnDrawV1(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<SampleV1Context*>(plugin_context);
    if (ui == nullptr && context != nullptr) ui = context->ui;
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr) return;
    static int open = 1;
    if (ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 360.0F, 210.0F, 4u);
    }
    if (ui->begin_window(
            ui->user, View("Plugin Manager Fixture", sizeof("Plugin Manager Fixture") - 1),
            &open, 0) != 0) {
        ui->text(
            ui->user,
            View("ABI v1 host-owned UI service", sizeof("ABI v1 host-owned UI service") - 1));
        if (context != nullptr && context->player != nullptr &&
            context->player->snapshot != nullptr) {
            AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
            if (context->player->snapshot(context->player->user, &player).code ==
                ANOMALY_STATUS_V1_OK) {
                char position[160]{};
                const int length = std::snprintf(
                    position, sizeof(position), "NTE position: %.3f, %.3f, %.3f",
                    player.position[0], player.position[1], player.position[2]);
                if (length > 0) {
                    ui->text(
                        ui->user,
                        View(position, static_cast<std::size_t>((std::min)(
                            length, static_cast<int>(sizeof(position) - 1)))));
                }
            } else {
                ui->text(
                    ui->user,
                    View("NTE player snapshot is loading",
                         sizeof("NTE player snapshot is loading") - 1));
            }
        } else {
            ui->text(
                ui->user,
                View("NTE player service unavailable",
                     sizeof("NTE player service unavailable") - 1));
        }
    }
    ui->end_window(ui->user);
}

}  // namespace
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(AnomalyPluginDescriptorV1)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    descriptor->struct_size = sizeof(AnomalyPluginDescriptorV1);
    descriptor->api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    descriptor->api_minor = ANOMALY_PLUGIN_API_V1_MINOR;
    descriptor->id = View(kPluginId, sizeof("anomaly.test.plugin-manager-fixture") - 1);
    descriptor->name = View("Plugin Manager Fixture", sizeof("Plugin Manager Fixture") - 1);
    descriptor->author = View("Anomaly", sizeof("Anomaly") - 1);
    descriptor->version = View("1.0.0", sizeof("1.0.0") - 1);
    descriptor->on_load = OnLoadV1;
    descriptor->on_start = OnStartV1;
    descriptor->on_stop = OnStopV1;
    descriptor->on_unload = OnUnloadV1;
    descriptor->on_update = OnUpdateV1;
    descriptor->on_draw = OnDrawV1;
    return OkV1();
}
