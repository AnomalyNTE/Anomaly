#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace {

struct Snapshot {
    std::uint32_t flags{};
    double speed_kmh{};
    float top_speed_ratio{1.0F};
    bool friction{true};
};

struct Context {
    const AnomalyNteVehicleServiceV1* vehicle{};
    const AnomalyUiServiceV1* ui{};
    std::mutex mutex;
    Snapshot snapshot{};
    std::string status{"等待载具"};
    std::atomic_bool apply_speed{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool friction_toggle{};
    std::atomic_bool started{};
    float speed_ratio{1.0F};
    bool friction_enabled{true};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

constexpr AnomalyStatusV1 Status(std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
const Service* QueryService(const AnomalyHostApiV1* host,
                            std::string_view id,
                            std::uint32_t version) noexcept {
    if (!host || !host->query_service) return nullptr;
    const void* table{};
    const auto result = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), version, &table);
    if (result.code != ANOMALY_STATUS_V1_OK || !table) return nullptr;
    return static_cast<const Service*>(table);
}

void SetStatus(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.status = std::move(text);
}

void DrawText(std::string_view text) {
    if (g_context.ui && g_context.ui->text) {
        g_context.ui->text(g_context.ui->user, anomaly::sdk::StringView(text));
    }
}

void Draw() {
    const auto* ui = g_context.ui;
    if (!ui || !ui->begin_window || !ui->end_window || !ui->text ||
        !ui->button || !ui->slider_float) return;

    int open = 1;
    if (!ui->begin_window(ui->user, anomaly::sdk::StringView("NTE Vehicle"), &open, 0)) return;

    Snapshot snap;
    std::string status;
    float ratio;
    bool friction;
    {
        std::scoped_lock lock(g_context.mutex);
        snap = g_context.snapshot;
        status = g_context.status;
        ratio = g_context.speed_ratio;
        friction = g_context.friction_enabled;
    }

    DrawText("NTE 载具控制");
    DrawText(status);

    if ((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) == 0) {
        DrawText("当前没有检测到正在驾驶的载具。");
    } else {
        const std::string speed = "当前速度：" + std::to_string(snap.speed_kmh) + " km/h";
        DrawText(speed);

        const std::string ratio_text = "最高速度倍率：" + std::to_string(snap.top_speed_ratio) + "x";
        DrawText(ratio_text);
        if (ui->slider_float(ui->user, anomaly::sdk::StringView("倍率"),
                             &ratio, 0.05F, 20.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.speed_ratio = ratio;
        }
        if (ui->button(ui->user, anomaly::sdk::StringView("应用速度倍率"), 0.0F, 0.0F)) {
            g_context.apply_speed.store(true, std::memory_order_release);
        }
        if (ui->button(ui->user, anomaly::sdk::StringView("恢复 1.0x"), 0.0F, 0.0F)) {
            g_context.reset.store(true, std::memory_order_release);
        }
        if (ui->button(ui->user, anomaly::sdk::StringView("召唤载具"), 0.0F, 0.0F)) {
            g_context.summon.store(true, std::memory_order_release);
        }

        const std::string friction_text = std::string("车轮摩擦：") + (friction ? "开启" : "关闭");
        DrawText(friction_text);
        if (ui->button(ui->user, anomaly::sdk::StringView("切换车轮摩擦"), 0.0F, 0.0F)) {
            g_context.friction_toggle.store(true, std::memory_order_release);
        }
    }

    ui->end_window(ui->user);
}

void Update() {
    if (!g_context.vehicle || !g_context.started.load(std::memory_order_acquire)) return;

    if (g_context.reset.exchange(false, std::memory_order_acq_rel)) {
        const auto status = g_context.vehicle->reset(g_context.vehicle->user);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK ? "已恢复默认载具设置" : "恢复失败");
    }

    if (g_context.apply_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.speed_ratio;
        }
        const auto status = g_context.vehicle->set_top_speed_ratio(
            g_context.vehicle->user, ratio);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK ? "速度倍率已应用" : "速度倍率应用失败");
    }

    if (g_context.summon.exchange(false, std::memory_order_acq_rel)) {
        const auto status = g_context.vehicle->summon_vehicle(g_context.vehicle->user);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK ? "召唤载具请求已发送" : "召唤载具不可用");
    }

    if (g_context.friction_toggle.exchange(false, std::memory_order_acq_rel)) {
        bool enabled;
        {
            std::scoped_lock lock(g_context.mutex);
            enabled = !g_context.friction_enabled;
        }
        const auto status = g_context.vehicle->set_wheel_friction_enabled(
            g_context.vehicle->user, enabled ? 1u : 0u);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.friction_enabled = enabled;
            g_context.status = enabled ? "车轮摩擦已开启" : "车轮摩擦已关闭";
        } else {
            SetStatus("车轮摩擦切换失败");
        }
    }

    AnomalyNteVehicleSnapshotV1 snapshot{sizeof(snapshot)};
    const auto status = g_context.vehicle->snapshot(g_context.vehicle->user, &snapshot);
    std::scoped_lock lock(g_context.mutex);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        g_context.snapshot.flags = snapshot.flags;
        g_context.snapshot.speed_kmh = snapshot.speed_kmh;
        g_context.snapshot.top_speed_ratio = snapshot.top_speed_ratio;
        g_context.snapshot.friction = snapshot.wheel_friction_enabled != 0;
    } else {
        g_context.snapshot.flags = 0;
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!host || !plugin_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto* vehicle = QueryService<AnomalyNteVehicleServiceV1>(
        host, ANOMALY_NTE_VEHICLE_SERVICE_V1_ID, ANOMALY_NTE_VEHICLE_SERVICE_V1_VERSION);
    const auto* ui = QueryService<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!vehicle || !ui) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    if (!vehicle->snapshot || !vehicle->set_top_speed_ratio || !vehicle->summon_vehicle ||
        !vehicle->set_wheel_friction_enabled || !vehicle->reset ||
        !ui->begin_window || !ui->end_window || !ui->text ||
        !ui->button || !ui->slider_float) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_context.vehicle = vehicle;
    g_context.ui = ui;
    g_context.started.store(false, std::memory_order_release);
    g_context.apply_speed.store(false, std::memory_order_release);
    g_context.reset.store(false, std::memory_order_release);
    g_context.summon.store(false, std::memory_order_release);
    g_context.friction_toggle.store(false, std::memory_order_release);
    g_context.speed_ratio = 1.0F;
    g_context.friction_enabled = true;
    g_context.snapshot = {};
    g_context.status = "等待载具";
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(false, std::memory_order_release);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    g_context.vehicle = nullptr;
    g_context.ui = nullptr;
}

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) {
    if (plugin_context == &g_context) Update();
}

void ANOMALY_CALL DrawCallback(void* plugin_context, const AnomalyUiServiceV1*) {
    if (plugin_context == &g_context) Draw();
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-vehicle"),
        anomaly::sdk::StringView("NTE Vehicle"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("0.5.0"),
        Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}