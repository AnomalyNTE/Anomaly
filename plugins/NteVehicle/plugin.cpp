#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ui_resources.h"
#include "anomaly/sdk/services/plugin_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <cctype>
#include <string_view>
#include <vector>
#include <utility>
#include <fstream>
#include <chrono>

namespace {

// 车速倍率直接调节，无需额外启用/关闭；范围 0x–100x。
constexpr float kDefaultSpeedRatio = 1.0F;
constexpr float kMinimumSpeedRatio = 0.0F;
constexpr float kMaximumSpeedRatio = 100.0F;

struct Snapshot {
    std::uint32_t flags{};
    double speed_kmh{};
    float top_speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    bool friction{true};
    std::uint64_t vehicle_entity_id{};
    std::string vehicle_class_name;
    std::string vehicle_snapshot_status{"Host 尚未返回当前驾驶载具状态"};
};

struct CustomizationOption {
    std::string id;
    std::string label;
    std::uint32_t kind{};
    std::uint32_t module_type{};
    bool is_default{};
};

struct Context {
    const AnomalyNteVehicleServiceV1* vehicle{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyInputServiceV1* input{};
    const AnomalyWindowServiceV1* window_service{};
    const AnomalyPluginStateServiceV1* plugin_state{};
    AnomalyGenerationHandleV1 menu_window_handle{};
    AnomalyGenerationHandleV1 menu_hotkey_handle{};
    std::atomic_bool toggle_menu_requested{};
    AnomalyNtePlayerSnapshotV1 player_snapshot{};
    bool player_position_valid{};
    std::mutex mutex;
    Snapshot snapshot{};
    std::string status{"正在读取游戏进程中的 Vehicle 载具数据"};
    std::string selected_vehicle_id{"Vehicle007"};
    std::string pending_vehicle_id;
    // 上次选择持久化：启动/热重载后恢复。
    std::string saved_vehicle_id;
    bool saved_selection_pending{};
    std::string state_directory;
    std::string catalog_filter;
    std::string last_summon{};
    std::vector<std::string> vehicle_ids;
    std::vector<std::string> vehicle_labels;
    std::uint64_t catalog_tick{};
    bool customization_page{};
    std::string customization_vehicle_id;
    std::string customization_skin_id;
    std::string customization_status{"尚未读取当前车型的改装数据"};
    std::vector<CustomizationOption> customization_options;
    std::vector<std::pair<std::uint32_t, std::string>> selected_module_overrides;
    std::uint64_t customization_catalog_tick{};
    std::atomic_bool refresh_customization_requested{};
    std::atomic_bool refresh_catalog_requested{};
    std::atomic_bool apply_speed{};
    std::atomic_bool apply_torque{};
    std::atomic_bool apply_other_speed{};
    std::atomic_bool apply_other_torque{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool summon_customized{};
    std::atomic_bool friction_toggle{};
    std::atomic_bool started{};
    std::atomic_uint32_t durability_last_status_code{ANOMALY_STATUS_V1_NOT_FOUND};
    float speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    float other_speed_ratio{1.0F};
    float other_speed_floor_ratio{0.6024F};
    float other_torque_ratio{1.0F};
    bool friction_enabled{true};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

constexpr AnomalyStatusV1 Status(
    std::uint32_t code, std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
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

// 载具选择持久化：保存到插件自己的 state 目录下的 cfg 文件。
std::string SavedSelectionPath() {
    if (g_context.state_directory.empty()) return {};
    std::string path = g_context.state_directory;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "vehicle-selection.cfg";
    return path;
}

std::string LoadSavedSelection() {
    const std::string path = SavedSelectionPath();
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string id;
    std::getline(in, id);
    while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back()))) id.pop_back();
    while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front())))
        id.erase(id.begin());
    return id;
}

void SaveSelection(const std::string& vehicle_id) {
    if (vehicle_id.empty()) return;
    const std::string path = SavedSelectionPath();
    if (path.empty()) return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) out << vehicle_id << "\n";
}

bool InputReady(const AnomalyInputServiceV1* input) noexcept {
    return HasField<AnomalyInputServiceV1,
        decltype(AnomalyInputServiceV1::register_hotkey)>(
            input, offsetof(AnomalyInputServiceV1, register_hotkey)) &&
        input->register_hotkey != nullptr &&
        HasField<AnomalyInputServiceV1,
            decltype(AnomalyInputServiceV1::release_hotkey)>(
                input, offsetof(AnomalyInputServiceV1, release_hotkey)) &&
        input->release_hotkey != nullptr;
}

bool WindowReady(const AnomalyWindowServiceV1* window) noexcept {
    return HasField<AnomalyWindowServiceV1,
        decltype(AnomalyWindowServiceV1::end)>(
            window, offsetof(AnomalyWindowServiceV1, end)) &&
        window->register_window != nullptr &&
        window->release_window != nullptr &&
        window->set_open != nullptr &&
        window->toggle != nullptr &&
        window->state != nullptr &&
        window->begin != nullptr &&
        window->end != nullptr;
}

void ANOMALY_CALL ToggleMenuHotkey(
    void* user, AnomalyGenerationHandleV1,
    const AnomalyInputSnapshotV1* snapshot) noexcept {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr || !context->started.load(std::memory_order_acquire)) return;
    // K remains usable while the menu is open, but typing K into a focused text field
    // (such as the catalog search box) must not toggle the window.
    if (snapshot != nullptr &&
        snapshot->struct_size >= offsetof(AnomalyInputSnapshotV1, capture_flags) +
            sizeof(snapshot->capture_flags) &&
        (snapshot->capture_flags & ANOMALY_INPUT_CAPTURE_V1_TEXT) != 0) {
        return;
    }
    context->toggle_menu_requested.store(true, std::memory_order_release);
}

void ReleaseMenuHotkey(Context& context) noexcept {
    if (context.menu_hotkey_handle.id != 0 && InputReady(context.input)) {
        static_cast<void>(context.input->release_hotkey(
            context.input->user, context.menu_hotkey_handle));
    }
    context.menu_hotkey_handle = {};
}



void SetStatus(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.status = std::move(text);
}

void DrawText(const AnomalyUiServiceV1* ui, std::string_view text) {
    if (ui == nullptr) return;
    const auto wrapped_offset = offsetof(AnomalyUiServiceV1, text_wrapped);
    if (ui->struct_size >= wrapped_offset +
            sizeof(decltype(AnomalyUiServiceV1::text_wrapped)) &&
        ui->text_wrapped != nullptr) {
        ui->text_wrapped(ui->user, anomaly::sdk::StringView(text));
    } else if (ui->text != nullptr) {
        ui->text(ui->user, anomaly::sdk::StringView(text));
    }
}

std::string StatusDescription(std::string_view prefix, const AnomalyStatusV1& status) {
    std::string result(prefix);
    if (status.message.data != nullptr && status.message.size != 0) {
        result += "：";
        result.append(status.message.data, status.message.size);
    } else {
        result += "（Host 状态码 ";
        result += std::to_string(status.code);
        result += "）";
    }
    return result;
}

void SetLastSummon(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.last_summon = std::move(text);
}

// 目录内容来自当前 Host 已核验的实时 FNamePool；读取前后序号相同才发布完整目录。
void RefreshCatalogOnGameThread() {
    const auto* vehicle = g_context.vehicle;
    if (!vehicle || !vehicle->catalog_snapshot || !vehicle->vehicle_id_at ||
        !vehicle->catalog_label_at) {
        SetStatus("目录读取失败：当前 Host 没有发布 Vehicle V3 的目录标签接口");
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids.clear();
        g_context.vehicle_labels.clear();
        return;
    }

    AnomalyNteVehicleCatalogSnapshotV1 before{};
    before.struct_size = sizeof(before);
    const auto before_status = vehicle->catalog_snapshot(vehicle->user, &before);
    if (before_status.code != ANOMALY_STATUS_V1_OK ||
        (before.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        before.entry_count > 8192U) {
        SetStatus(StatusDescription("目录读取失败", before_status));
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids.clear();
        g_context.vehicle_labels.clear();
        return;
    }

    std::vector<std::string> ids;
    std::vector<std::string> labels;
    ids.reserve(before.entry_count);
    labels.reserve(before.entry_count);
    for (std::uint32_t i = 0; i < before.entry_count; ++i) {
        std::array<char, ANOMALY_NTE_VEHICLE_ID_MAX_UTF8_BYTES + 1U> id_buffer{};
        std::array<char, ANOMALY_NTE_VEHICLE_ID_MAX_UTF8_BYTES * 2U + 8U> label_buffer{};
        std::size_t id_size = id_buffer.size();
        const auto id_status = vehicle->vehicle_id_at(vehicle->user, i, id_buffer.data(), &id_size);
        if (id_status.code != ANOMALY_STATUS_V1_OK || id_size <= 1U || id_size > id_buffer.size()) {
            SetStatus(StatusDescription("读取 RowName 失败（索引 " + std::to_string(i) + "）", id_status));
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }

        std::size_t label_size = label_buffer.size();
        const auto label_status = vehicle->catalog_label_at(
            vehicle->user, i, label_buffer.data(), &label_size);
        if (label_status.code != ANOMALY_STATUS_V1_OK ||
            label_size <= 1U || label_size > label_buffer.size()) {
            SetStatus(StatusDescription("读取载具显示名称失败（索引 " + std::to_string(i) + "）", label_status));
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }
        ids.emplace_back(id_buffer.data(), id_size - 1U);
        labels.emplace_back(label_buffer.data(), label_size - 1U);
        if (ids.back().empty() || labels.back().empty()) {
            SetStatus("目录读取失败：存在空 RowName 或显示标签");
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }
    }

    AnomalyNteVehicleCatalogSnapshotV1 after{};
    after.struct_size = sizeof(after);
    const auto after_status = vehicle->catalog_snapshot(vehicle->user, &after);
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        after.sequence != before.sequence || after.entry_count != ids.size()) {
        SetStatus(StatusDescription("读取目录期间 DataTable 发生变化", after_status));
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids.clear();
        g_context.vehicle_labels.clear();
        return;
    }

    std::string selected_id;
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids = std::move(ids);
        g_context.vehicle_labels = std::move(labels);
        g_context.catalog_tick = after.sequence;
        if (std::ranges::find(g_context.vehicle_ids, g_context.selected_vehicle_id) ==
            g_context.vehicle_ids.end()) {
            g_context.selected_vehicle_id = g_context.vehicle_ids.empty()
                ? std::string{} : g_context.vehicle_ids.front();
        }
        selected_id = g_context.selected_vehicle_id;
        g_context.status = "已读取 " + std::to_string(g_context.vehicle_ids.size()) +
            " 条 DT_VehicleData 记录";
    }

    // Always synchronize the current UI selection with Host state. Otherwise the
    // default item can look selected while selected_summon_vehicle_id is still empty.
    if (!selected_id.empty()) {
        const auto selected_status = vehicle->set_summon_vehicle_id(
            vehicle->user, anomaly::sdk::StringView(selected_id));
        if (selected_status.code != ANOMALY_STATUS_V1_OK)
            SetStatus(StatusDescription("目录已读取，但 Host 未接受当前 RowName", selected_status));
    }
}

void RefreshCustomizationCatalogOnGameThread() {
    const auto* vehicle = g_context.vehicle;
    std::string selected_id;
    {
        std::scoped_lock lock(g_context.mutex);
        selected_id = g_context.selected_vehicle_id;
    }
    const bool callbacks_available =
        vehicle != nullptr &&
        HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_catalog_snapshot)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_catalog_snapshot)) &&
        vehicle->customization_catalog_snapshot != nullptr &&
        HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_option_at)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_option_at)) &&
        vehicle->customization_option_at != nullptr;
    if (!callbacks_available || selected_id.empty()) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id.clear();
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status = selected_id.empty()
            ? "请先从召唤清单中选择车型"
            : "当前 Core 未发布 V7 改装目录接口；请更新 Runtime";
        return;
    }

    AnomalyNteVehicleCustomizationCatalogSnapshotV1 before{};
    before.struct_size = sizeof(before);
    const auto before_status = vehicle->customization_catalog_snapshot(
        vehicle->user, anomaly::sdk::StringView(selected_id), &before);
    if (before_status.code != ANOMALY_STATUS_V1_OK ||
        (before.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        before.entry_count > 8192U) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status = StatusDescription("读取当前车型改装目录失败", before_status);
        return;
    }

    std::vector<CustomizationOption> options;
    options.reserve(before.entry_count);
    for (std::uint32_t index = 0; index < before.entry_count; ++index) {
        AnomalyNteVehicleCustomizationOptionV1 native_option{};
        native_option.struct_size = sizeof(native_option);
        const auto option_status = vehicle->customization_option_at(
            vehicle->user, anomaly::sdk::StringView(selected_id), index, &native_option);
        if (option_status.code != ANOMALY_STATUS_V1_OK ||
            (native_option.flags & ANOMALY_NTE_VEHICLE_CUSTOMIZATION_OPTION_V1_VALID) == 0) {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_options.clear();
            g_context.customization_vehicle_id = selected_id;
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = StatusDescription(
                "读取改装目录选项失败（索引 " + std::to_string(index) + "）", option_status);
            return;
        }
        const auto id_end = std::find(
            std::begin(native_option.option_id), std::end(native_option.option_id), '\0');
        const auto label_end = std::find(
            std::begin(native_option.label), std::end(native_option.label), '\0');
        if (id_end == std::end(native_option.option_id) ||
            label_end == std::end(native_option.label) ||
            id_end == std::begin(native_option.option_id) ||
            (native_option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE &&
             native_option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN) ||
            (native_option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE &&
             (native_option.module_type == 0 || native_option.module_type >= 27)) ||
            (native_option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN &&
             native_option.module_type != 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_options.clear();
            g_context.customization_vehicle_id = selected_id;
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = "改装目录包含无效 ID、类型或未终止的 ABI 字符串";
            return;
        }
        CustomizationOption option{};
        option.id.assign(native_option.option_id, static_cast<std::size_t>(
            id_end - std::begin(native_option.option_id)));
        option.label.assign(native_option.label, static_cast<std::size_t>(
            label_end - std::begin(native_option.label)));
        option.kind = native_option.kind;
        option.module_type = native_option.module_type;
        option.is_default =
            (native_option.flags & ANOMALY_NTE_VEHICLE_CUSTOMIZATION_OPTION_V1_DEFAULT) != 0;
        options.push_back(std::move(option));
    }

    AnomalyNteVehicleCustomizationCatalogSnapshotV1 after{};
    after.struct_size = sizeof(after);
    const auto after_status = vehicle->customization_catalog_snapshot(
        vehicle->user, anomaly::sdk::StringView(selected_id), &after);
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        after.sequence != before.sequence || after.entry_count != options.size()) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status =
            "读取改装目录期间车型数据发生变化；请重新打开「改装载具清单」";
        return;
    }

    std::string default_skin;
    std::string fallback_skin;
    for (const auto& option : options) {
        if (option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN) continue;
        if (fallback_skin.empty()) fallback_skin = option.id;
        if (option.is_default) {
            default_skin = option.id;
            break;
        }
    }
    const bool used_fallback_skin = default_skin.empty() && !fallback_skin.empty();
    if (default_skin.empty()) default_skin = std::move(fallback_skin);
    {
        std::scoped_lock lock(g_context.mutex);
        if (selected_id != g_context.selected_vehicle_id) return;
        g_context.customization_options = std::move(options);
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id = std::move(default_skin);
        g_context.selected_module_overrides.clear();
        g_context.customization_catalog_tick = after.sequence;
        const auto module_count = std::ranges::count_if(
            g_context.customization_options, [](const CustomizationOption& option) {
                return option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE;
            });
        const auto skin_count = std::ranges::count_if(
            g_context.customization_options, [](const CustomizationOption& option) {
                return option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN;
            });
        g_context.customization_status = "已读取该车型实时改装选项：模块 " +
            std::to_string(module_count) + " 项，涂装 " + std::to_string(skin_count) +
            " 项；未选择的模块槽位沿用游戏数据表默认配置";
        if (used_fallback_skin) {
            g_context.customization_status += "；该车型没有标记默认涂装，已暂选首个有效涂装，可在下方更改";
        } else if (g_context.customization_skin_id.empty() && skin_count == 0) {
            g_context.customization_status += "；该车型没有可用涂装选项，改装召唤将保留游戏生成车辆原涂装";
        } else if (g_context.customization_skin_id.empty()) {
            g_context.customization_status += "；该车型没有标记默认涂装，请在下方选择涂装";
        }
    }
}

void Update() {
    if (!g_context.vehicle || !g_context.started.load(std::memory_order_acquire)) return;

    if (g_context.toggle_menu_requested.exchange(false, std::memory_order_acq_rel) &&
        g_context.window_service != nullptr && g_context.menu_window_handle.id != 0) {
        const auto toggle_status = g_context.window_service->toggle(
            g_context.window_service->user, g_context.menu_window_handle);
        if (toggle_status.code != ANOMALY_STATUS_V1_OK) {
            SetStatus(StatusDescription("K 快捷键切换载具菜单失败", toggle_status));
        }
    }

    // Keep the current wheeled vehicle at maximum durability by calling Dump-validated
    // RepairSelf(true) on every Game Update. Core resolves and validates the current vehicle
    // each time; the plugin does not keep a stale actor pointer.
    if (HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::repair_current_vehicle)>(
                g_context.vehicle,
                offsetof(AnomalyNteVehicleServiceV1, repair_current_vehicle)) &&
        g_context.vehicle->repair_current_vehicle != nullptr) {
        const auto durability_status = g_context.vehicle->repair_current_vehicle(
            g_context.vehicle->user);
        const auto previous_code = g_context.durability_last_status_code.exchange(
            durability_status.code, std::memory_order_acq_rel);
        if (durability_status.code != ANOMALY_STATUS_V1_OK &&
            durability_status.code != ANOMALY_STATUS_V1_NOT_FOUND &&
            previous_code != durability_status.code) {
            SetStatus(StatusDescription("当前载具耐久保持满值失败", durability_status));
        } else if (durability_status.code == ANOMALY_STATUS_V1_OK &&
                   previous_code != ANOMALY_STATUS_V1_OK &&
                   previous_code != ANOMALY_STATUS_V1_NOT_FOUND) {
            SetStatus("当前陆地载具耐久保护已恢复");
        }
    }

    // Explicit refresh runs on Game thread; a failed scan waits for another user request
    // instead of rescanning UObject/DataTable every frame.
    if (g_context.refresh_catalog_requested.exchange(false, std::memory_order_acq_rel)) {
        if (!g_context.vehicle->refresh_catalog) {
            SetStatus("目录刷新失败：Host 没有发布 Vehicle V3 refresh_catalog 接口");
        } else {
            const auto refresh_status = g_context.vehicle->refresh_catalog(g_context.vehicle->user);
            if (refresh_status.code == ANOMALY_STATUS_V1_OK) {
                RefreshCatalogOnGameThread();
            } else {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.vehicle_ids.clear();
                    g_context.vehicle_labels.clear();
                }
                SetStatus(StatusDescription("目录刷新失败", refresh_status));
            }
        }
    }

    // 一次性目录转储：顺序与游戏目录一致，用于名称对照（写在工作目录）。
    static std::uint64_t dumped_catalog_tick = 0;
    {
        std::scoped_lock lock(g_context.mutex);
        if (dumped_catalog_tick != g_context.catalog_tick && !g_context.vehicle_ids.empty()) {
            dumped_catalog_tick = g_context.catalog_tick;
            std::ofstream out("ntvehicle_catalog.txt", std::ios::trunc);
            if (out) {
                for (std::size_t i = 0; i < g_context.vehicle_ids.size(); ++i) {
                    const std::string& label =
                        i < g_context.vehicle_labels.size() && !g_context.vehicle_labels[i].empty()
                            ? g_context.vehicle_labels[i] : g_context.vehicle_ids[i];
                    out << i << "\t" << label << "\t" << g_context.vehicle_ids[i] << "\n";
                }
            }
        }
    }

    // Render only writes pending_vehicle_id under mutex; consume it under the same mutex.
    std::string pending_vehicle_id;
    {
        std::scoped_lock lock(g_context.mutex);
        pending_vehicle_id = std::move(g_context.pending_vehicle_id);
        g_context.pending_vehicle_id.clear();
    }
    if (!pending_vehicle_id.empty()) {
        const auto status = g_context.vehicle->set_summon_vehicle_id(
            g_context.vehicle->user, anomaly::sdk::StringView(pending_vehicle_id));
        if (status.code == ANOMALY_STATUS_V1_OK) {
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.selected_vehicle_id = pending_vehicle_id;
                g_context.status = "已选择载具：" + pending_vehicle_id;
                if (g_context.customization_page) {
                    g_context.customization_vehicle_id.clear();
                    g_context.customization_options.clear();
                    g_context.customization_skin_id.clear();
                    g_context.selected_module_overrides.clear();
                    g_context.refresh_customization_requested.store(
                        true, std::memory_order_release);
                }
            }
            // 记住当前选择，下次启动/热重载后恢复。
            SaveSelection(pending_vehicle_id);
        } else {
            SetStatus(StatusDescription("载具选择失败", status));
        }
    }

    if (g_context.refresh_customization_requested.exchange(false, std::memory_order_acq_rel)) {
        RefreshCustomizationCatalogOnGameThread();
    }

    bool catalog_loaded{};
    {
        std::scoped_lock lock(g_context.mutex);
        catalog_loaded = !g_context.vehicle_ids.empty();
    }
    // 载具目录为空且已进入主世界时，每 5 秒自动重读一次（初始化未进主世界前不做）。
    if (!catalog_loaded && g_context.player_position_valid) {
        static std::chrono::steady_clock::time_point last_catalog_retry{};
        const auto now = std::chrono::steady_clock::now();
        if (last_catalog_retry.time_since_epoch().count() == 0 ||
            now - last_catalog_retry >= std::chrono::seconds(5)) {
            last_catalog_retry = now;
            g_context.refresh_catalog_requested.store(true, std::memory_order_release);
        }
    }
    // 上一次的选择在载具目录就绪后自动应用。
    if (g_context.saved_selection_pending && catalog_loaded) {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.pending_vehicle_id.empty()) {
            g_context.pending_vehicle_id = g_context.saved_vehicle_id;
        }
        g_context.saved_selection_pending = false;
    }
    if (catalog_loaded && g_context.vehicle->catalog_snapshot) {
        AnomalyNteVehicleCatalogSnapshotV1 catalog{};
        catalog.struct_size = sizeof(catalog);
        const auto catalog_status = g_context.vehicle->catalog_snapshot(
            g_context.vehicle->user, &catalog);
        if (catalog_status.code == ANOMALY_STATUS_V1_OK &&
            (catalog.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) != 0) {
            bool need_refresh{};
            {
                std::scoped_lock lock(g_context.mutex);
                need_refresh = catalog.sequence != g_context.catalog_tick;
            }
            if (need_refresh) RefreshCatalogOnGameThread();
        } else {
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.vehicle_ids.clear();
                g_context.vehicle_labels.clear();
            }
            SetStatus(StatusDescription("载具目录失效", catalog_status) +
                "；请点击「重新整理」重试");
        }
    }

    // Refresh the live player snapshot every Game update. Summon must not reuse stale coordinates.
    if (g_context.player && g_context.player->snapshot) {
        AnomalyNtePlayerSnapshotV1 player_snapshot{};
        player_snapshot.struct_size = sizeof(player_snapshot);
        const auto player_status = g_context.player->snapshot(
            g_context.player->user, &player_snapshot);
        const bool valid = player_status.code == ANOMALY_STATUS_V1_OK &&
            (player_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
            std::ranges::all_of(player_snapshot.position,
                [](double value) { return std::isfinite(value); });
        std::scoped_lock lock(g_context.mutex);
        g_context.player_position_valid = valid;
        if (valid) g_context.player_snapshot = player_snapshot;
    }

    if (g_context.reset.exchange(false, std::memory_order_acq_rel)) {
        const auto status = g_context.vehicle->reset(g_context.vehicle->user);
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.speed_ratio = kDefaultSpeedRatio;
        }
        g_context.apply_speed.store(true, std::memory_order_release);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK
            ? "已恢复默认载具设置；车速倍率已恢复为 1.0x"
            : "恢复失败；仍将重新应用默认车速倍率 1.0x");
    }

    if (g_context.apply_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio{};
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = std::clamp(g_context.speed_ratio,
                kMinimumSpeedRatio, kMaximumSpeedRatio);
            g_context.speed_ratio = ratio;
        }
        const auto status = g_context.vehicle->set_top_speed_ratio(
            g_context.vehicle->user, ratio);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            SetStatus("当前车速与最高车速上限均已应用：" + std::to_string(ratio) + "x");
        } else if (status.message.data != nullptr && status.message.size != 0) {
            SetStatus("车速倍率应用失败：" + std::string(status.message.data, status.message.size));
        } else {
            SetStatus("车速倍率应用失败，Host 状态码 " + std::to_string(status.code));
        }
    }

    if (g_context.apply_torque.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.engine_torque_ratio;
        }
        const bool has_torque_ratio_service =
            HasField<AnomalyNteVehicleServiceV1,
                decltype(AnomalyNteVehicleServiceV1::set_engine_torque_ratio)>(
                    g_context.vehicle,
                    offsetof(AnomalyNteVehicleServiceV1, set_engine_torque_ratio)) &&
            g_context.vehicle->set_engine_torque_ratio != nullptr;
        if (!has_torque_ratio_service) {
            SetStatus("发动机扭矩倍率不可用：当前 Host Runtime 未发布该扩展接口");
        } else {
            const auto status = g_context.vehicle->set_engine_torque_ratio(
                g_context.vehicle->user, ratio);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                SetStatus("发动机扭矩倍率已应用：" + std::to_string(ratio) + "x");
            } else if (status.message.data != nullptr && status.message.size != 0) {
                SetStatus("发动机扭矩倍率应用失败：" +
                    std::string(status.message.data, status.message.size));
            } else {
                SetStatus("发动机扭矩倍率应用失败，Host 状态码 " +
                    std::to_string(status.code));
            }
        }
    }

    if (g_context.apply_other_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        float floor_ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.other_speed_ratio;
            floor_ratio = g_context.other_speed_floor_ratio;
        }
        const bool floor_available =
            HasField<AnomalyNteVehicleServiceV1,
                decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_min_speed_ratio)>(
                    g_context.vehicle,
                    offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_min_speed_ratio)) &&
            g_context.vehicle->set_other_vehicles_min_speed_ratio != nullptr;
        if (!floor_available) {
            SetStatus("非玩家车辆最低车速倍率不可用：Host Runtime 未发布扩展接口");
        } else {
            const auto floor_status = g_context.vehicle->set_other_vehicles_min_speed_ratio(
                g_context.vehicle->user, floor_ratio);
            if (floor_status.code != ANOMALY_STATUS_V1_OK) {
                SetStatus(StatusDescription("非玩家车辆最低车速倍率设置失败", floor_status));
            } else {
                const bool available =
                    HasField<AnomalyNteVehicleServiceV1,
                        decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_top_speed_ratio)>(
                            g_context.vehicle,
                            offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_top_speed_ratio)) &&
                    g_context.vehicle->set_other_vehicles_top_speed_ratio != nullptr;
                if (!available) {
                    SetStatus("非玩家车辆速率倍率不可用：Host Runtime 未发布 Vehicle V4 接口");
                } else {
                    const auto status = g_context.vehicle->set_other_vehicles_top_speed_ratio(
                        g_context.vehicle->user, ratio);
                    SetStatus(status.code == ANOMALY_STATUS_V1_OK
                        ? StatusDescription("非玩家车辆速率与下限修改完成", status)
                        : StatusDescription("非玩家车辆速率修改未完全成功", status));
                }
            }
        }
    }

    if (g_context.apply_other_torque.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.other_torque_ratio;
        }
        const bool available =
            HasField<AnomalyNteVehicleServiceV1,
                decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_engine_torque_ratio)>(
                    g_context.vehicle,
                    offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_engine_torque_ratio)) &&
            g_context.vehicle->set_other_vehicles_engine_torque_ratio != nullptr;
        if (!available) {
            SetStatus("非玩家车辆扭矩倍率不可用：Host Runtime 未发布 Vehicle V4 接口");
        } else {
            const auto status = g_context.vehicle->set_other_vehicles_engine_torque_ratio(
                g_context.vehicle->user, ratio);
            SetStatus(status.code == ANOMALY_STATUS_V1_OK
                ? StatusDescription("非玩家车辆扭矩修改完成", status)
                : StatusDescription("非玩家车辆扭矩修改未完全成功", status));
        }
    }

    const bool summon_requested = g_context.summon.exchange(false, std::memory_order_acq_rel);
    const bool summon_customized_requested = g_context.summon_customized.exchange(false, std::memory_order_acq_rel);
    if (summon_requested || summon_customized_requested) {
        AnomalyNteVehicleSummonRequestV1 request{};
        request.struct_size = sizeof(request);
        request.flags = ANOMALY_NTE_VEHICLE_SUMMON_V1_HAS_POSITION |
            ANOMALY_NTE_VEHICLE_SUMMON_V1_SET_OWNER_TO_PLAYER;

        std::string selected_id;
        std::string selected_label;
        std::string selected_skin_id;
        std::string customization_vehicle_id;
        std::vector<std::pair<std::uint32_t, std::string>> module_overrides;
        std::vector<AnomalyNteVehicleModuleSelectionV1> module_selections;
        bool position_valid{};
        {
            std::scoped_lock lock(g_context.mutex);
            selected_id = g_context.selected_vehicle_id;
            selected_skin_id = g_context.customization_skin_id;
            customization_vehicle_id = g_context.customization_vehicle_id;
            module_overrides = g_context.selected_module_overrides;
            position_valid = g_context.player_position_valid;
            const auto selected_index = std::ranges::find(g_context.vehicle_ids, selected_id);
            if (selected_index != g_context.vehicle_ids.end()) {
                const auto index = static_cast<std::size_t>(selected_index - g_context.vehicle_ids.begin());
                selected_label = index < g_context.vehicle_labels.size()
                    ? g_context.vehicle_labels[index] : selected_id;
            }
            if (position_valid) {
                // Keep the existing verified spawn position unchanged.
                request.world_position[0] = g_context.player_snapshot.position[0] - 500.0;
                request.world_position[1] = g_context.player_snapshot.position[1] + 500.0;
                request.world_position[2] = g_context.player_snapshot.position[2] + 500.0;
            }
        }

        bool catalog_has_selection{};
        {
            std::scoped_lock lock(g_context.mutex);
            catalog_has_selection = !selected_id.empty() &&
                std::ranges::find(g_context.vehicle_ids, selected_id) != g_context.vehicle_ids.end();
        }

        std::string validation_error;
        if (summon_customized_requested) {
            if (customization_vehicle_id != selected_id) {
                validation_error = "改装召唤失败：改造清单与当前选择的车型不一致，请重新打开清单";
            } else {
                request.flags |= ANOMALY_NTE_VEHICLE_SUMMON_V1_APPLY_CUSTOM_CONFIGURATION;
                if (!selected_skin_id.empty()) {
                    request.skin_id = anomaly::sdk::StringView(std::string_view(
                        selected_skin_id.data(), selected_skin_id.size()));
                }
                module_selections.reserve(module_overrides.size());
                for (const auto& override_entry : module_overrides) {
                    AnomalyNteVehicleModuleSelectionV1 selection{};
                    selection.struct_size = sizeof(selection);
                    selection.module_type = override_entry.first;
                    selection.module_id = anomaly::sdk::StringView(std::string_view(
                        override_entry.second.data(), override_entry.second.size()));
                    module_selections.push_back(selection);
                }
                request.module_selections = module_selections.empty()
                    ? nullptr : module_selections.data();
                request.module_selection_count =
                    static_cast<std::uint32_t>(module_selections.size());
            }
        }

        if (!validation_error.empty()) {
            SetStatus(validation_error);
            SetLastSummon(validation_error);
        } else if (!position_valid || !catalog_has_selection) {
            const std::string reason = !position_valid
                ? "召唤失败：玩家坐标当前无效"
                : "召唤失败：当前选择未同步到 Host 载具目录";
            SetStatus(reason);
            SetLastSummon(reason);
        } else {
            const auto status = g_context.vehicle->summon_vehicle(
                g_context.vehicle->user, &request);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                const std::string label = selected_label.empty() ? selected_id : selected_label;
                const std::string detail = status.message.data != nullptr && status.message.size != 0
                    ? std::string(status.message.data, status.message.size)
                    : std::string{};
                constexpr std::string_view verified_prefix = "召唤已实际生成并通过后置验证：";
                const bool postcondition_verified =
                    !detail.empty() && detail.find(verified_prefix) == 0;
                std::string result;
                if (postcondition_verified && summon_customized_requested) {
                    result = "改装载具召唤成功：" + detail;
                    SetStatus(result);
                    SetLastSummon(result);
                } else if (postcondition_verified) {
                    SetStatus({});
                    SetLastSummon({});
                } else {
                    result = "召唤状态未验证：" + label + " (" + selected_id +
                        ")；Host 返回 OK，但没有明确的实际生成/后置验证标记";
                    if (!detail.empty()) result += "；Host 信息：" + detail;
                    SetStatus(result);
                    SetLastSummon(result);
                }
            } else {
                const auto reason = StatusDescription(
                    summon_customized_requested ? "改装载具召唤失败" : "召唤失败", status);
                SetStatus(reason);
                SetLastSummon(reason);
            }
        }
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
        g_context.snapshot.engine_torque_ratio = snapshot.engine_torque_ratio;
        g_context.snapshot.friction = snapshot.wheel_friction_enabled != 0;
        if (snapshot.struct_size >= offsetof(AnomalyNteVehicleSnapshotV1, vehicle_entity_id) +
                sizeof(snapshot.vehicle_entity_id))
            g_context.snapshot.vehicle_entity_id = snapshot.vehicle_entity_id;
        else
            g_context.snapshot.vehicle_entity_id = 0;
        if (snapshot.struct_size >= offsetof(AnomalyNteVehicleSnapshotV1, vehicle_class_name) +
                sizeof(snapshot.vehicle_class_name)) {
            g_context.snapshot.vehicle_class_name.assign(
                snapshot.vehicle_class_name,
                std::find(snapshot.vehicle_class_name,
                    snapshot.vehicle_class_name + sizeof(snapshot.vehicle_class_name), '\0'));
        } else {
            g_context.snapshot.vehicle_class_name.clear();
        }
        g_context.snapshot.vehicle_snapshot_status.clear();
    } else {
        g_context.snapshot.flags = 0;
        g_context.snapshot.vehicle_entity_id = 0;
        g_context.snapshot.vehicle_class_name.clear();
        g_context.snapshot.vehicle_snapshot_status =
            status.message.data != nullptr && status.message.size != 0
                ? std::string(status.message.data, status.message.size)
                : "Host 载具快照状态码 " + std::to_string(status.code);
    }
}

bool AsciiContainsInsensitive(std::string_view text, std::string_view filter) noexcept {
    if (filter.empty()) return true;
    if (filter.size() > text.size()) return false;
    const auto equal_at = [&](std::size_t offset) {
        for (std::size_t i = 0; i < filter.size(); ++i) {
            const auto left = static_cast<unsigned char>(text[offset + i]);
            const auto right = static_cast<unsigned char>(filter[i]);
            if (std::tolower(left) != std::tolower(right)) return false;
        }
        return true;
    };
    for (std::size_t i = 0; i + filter.size() <= text.size(); ++i) {
        if (equal_at(i)) return true;
    }
    return false;
}

void DrawDivider(const AnomalyUiServiceV1* ui) {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::separator)>(
            ui, offsetof(AnomalyUiServiceV1, separator)) && ui->separator) {
        ui->separator(ui->user);
    }
}

void DrawInlineGap(const AnomalyUiServiceV1* ui) {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::same_line)>(
            ui, offsetof(AnomalyUiServiceV1, same_line)) && ui->same_line) {
        ui->same_line(ui->user, 0.0F, 12.0F);
    }
}

bool DrawButtonEnabled(
    const AnomalyUiServiceV1* ui, std::string_view label, bool enabled) {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, button_enabled)) && ui->button_enabled) {
        return ui->button_enabled(ui->user, anomaly::sdk::StringView(label),
            0.0F, 0.0F, enabled ? 1 : 0) != 0;
    }
    return enabled && ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0;
}

bool UiFieldAvailable(const AnomalyUiServiceV1* ui, std::size_t offset, std::size_t field_size) {
    return ui != nullptr && ui->struct_size >= offset + field_size;
}

// 载具显示名对照表：键为内部 VehicleID，顺序与游戏目录一致。
std::string_view VehicleDisplayName(std::string_view vehicle_id) {
    static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
{"Vehicle002Combat", "Griffin"},
        {"Vehicle002_q110116", "教练车AE86（藤原任务）"},
        {"Vehicle003Combat", "G3原色"},
        {"Vehicle003Scaled", "G3原色"},
        {"Vehicle003_oneoff", "G3卡其色"},
        {"Vehicle004_oneoff", "GriffinVolante"},
        {"Vehicle004_q110320", "教练车AE86（藤原任务）"},
        {"Vehicle005", "Vehicle005(报错未测试成功）"},
        {"Vehicle005_Player", "公交车"},
        {"Vehicle005a", "Vehicle005a(报错未测试成功）"},
        {"Vehicle006Combat", "面包车"},
        {"Vehicle007", "C2000"},
        {"Vehicle007Combat", "C2000"},
        {"Vehicle007_drivingschool", "教练车（驾校）"},
        {"Vehicle007_oneoff", "C2000白色"},
        {"Vehicle007_s111601", "C2000白色"},
        {"Vehicle007_s111601_race", "C2000（NPC代驾）"},
        {"Vehicle007_v2", "C2000白色"},
        {"Vehicle007a", "C2000金色"},
        {"Vehicle007c", "出租TAXI"},
        {"Vehicle008Combat", "M1000"},
        {"Vehicle008_K111801", "M1000"},
        {"Vehicle008_oneoff", "M1000红色"},
        {"Vehicle008fish", "M1000金色"},
        {"Vehicle009", "皮卡货车"},
        {"Vehicle009a", "集装箱货车"},
        {"Vehicle009a_toq", "集装箱货车"},
        {"Vehicle009b", "蓝色运输皮卡"},
        {"Vehicle009c", "工程队运输皮卡"},
        {"Vehicle010_q110328", "ST-X950摩托"},
        {"Vehicle012", "Dominus-S"},
        {"Vehicle014", "小游艇"},
        {"Vehicle015", "电瓶车"},
        {"Vehicle015A", "电瓶车绿色"},
        {"Vehicle016Combat", "LaVelox"},
        {"Vehicle017Combat", "Pursuit-V8"},
        {"Vehicle018Combat", "ST79(AE86)"},
        {"Vehicle018_K110000", "ST79(AE86)豆腐店车"},
        {"Vehicle018_K110027", "ST79(AE86)车神任务"},
        {"Vehicle018_K110045", "ST79(AE86)车神任务-落后"},
        {"Vehicle018_Quest_K110000", "ST79(AE86)豆腐店车"},
        {"Vehicle018ghost", "幽灵AE86"},
        {"Vehicle020Combat", "Enforcer"},
        {"Vehicle020Police", "C2000"},
        {"Vehicle021Combat", "Pendragon"},
        {"Vehicle024_Mods", "暴雪V4（机车）"},
        {"Vehicle027", "商务车"},
        {"Vehicle031_toq", "大运皮卡"},
        {"Vehicle033", "压路机"},
        {"Vehicle034", "叉车"},
        {"Vehicle035", "铲车"},
        {"Vehicle036", "挖掘机"},
        {"Vehicle040", "零售车"},
        {"Vehicle041", "零售车2"},
        {"Vehicle044", "冲浪艇"},
        {"Vehicle044Collect", "摩托艇"},
        {"Vehicle044Motorboatrace", "摩托艇"},
        {"Vehicle044TestDrive", "摩托艇（紫配色）"},
        {"Vehicle054", "宝宝巴士"},
        {"Vehicle055", "划向明天"},
        {"Vehicle066", "泰坦尼克"},
        {"Vehicle_010_Mods", "ST-X950摩托"},
        {"Vehicle_013_Hight_WJ106501", "旧时光"},
        {"Vehicle_015_Hight_WJ100105", "电瓶车"},
        {"vehicle002", "Griffin"},
        {"vehicle002_oneoff", "橙色griffin"},
        {"vehicle003", "G3绿色"},
        {"vehicle004", "GriffinVolante（和Griffin相同）"},
        {"vehicle006", "B100玫红色"},
        {"vehicle008", "M1000"},
        {"vehicle010", "ST-X950摩托"},
        {"vehicle013", "旧时光"},
        {"vehicle016", "LaVelox"},
        {"vehicle017", "Pursuit-V8"},
        {"vehicle018", "ST79"},
        {"vehicle020", "Enforcer"},
        {"vehicle021", "Pendragon"},
        {"vehicle023", "K01卡丁车"},
        {"vehicle024", "暴雪V4（机车）"},
        {"vehicle026", "Porsche918Spyder（保时捷918）"},
        {"vehicle030", "轮椅"},
        {"vehicle031", "大运皮卡"},
        {"vehicle032", "水泥车"},
        {"vehicle037", "Porsche911Turbo（保时捷911）"},
        {"vehicle038", "PorscheTaycanTurboGT(电车保时捷）"},
        {"vehicle039", "Draco（变形摩托）"},
        {"vehicle043", "小越野车"},
    };
    for (const auto& entry : kNames) {
        if (entry.first == vehicle_id) return entry.second;
    }
    return {};
}

// 优先使用对照表里的名称；缺失时退回 Host 标签并去掉 " (VehicleID)"。
std::string VehicleShownName(const std::string& vehicle_id, const std::string& host_label) {
    const std::string_view mapped = VehicleDisplayName(vehicle_id);
    if (!mapped.empty()) return std::string(mapped);
    std::string label = host_label.empty() ? vehicle_id : host_label;
    if (!label.empty() && label.back() == ')') {
        const std::size_t suffix = label.rfind(" (");
        if (suffix != std::string::npos && suffix > 0U) label.resize(suffix);
    }
    return label;
}

// 去重键：忽略颜色描述与“相同”备注；出租TAXI、NPC代驾等角色修饰保留。
std::string VehicleDedupeKey(const std::string& name) {
    std::string key;
    key.reserve(name.size());
    for (char c : name) {
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    static constexpr std::string_view kSuffixes[] = {
        "（和griffin相同）", "（紫配色）", "（相同）", "(相同)"};
    for (const auto suffix : kSuffixes) {
        if (key.size() >= suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            key.resize(key.size() - suffix.size());
        }
    }
    static constexpr std::string_view kColorTokens[] = {
        "玫红色", "卡其色", "紫配色", "原色", "绿色", "红色", "金色", "白色",
        "橙色", "蓝色", "紫色", "黑色", "银色", "灰色"};
    for (const auto token : kColorTokens) {
        std::size_t pos = 0;
        while ((pos = key.find(token, pos)) != std::string::npos) {
            key.erase(pos, token.size());
        }
    }
    while (true) {
        const std::size_t pos = key.find("（）");
        if (pos == std::string::npos) break;
        key.erase(pos, 6);
    }
    key.erase(std::remove_if(key.begin(), key.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }), key.end());
    return key;
}

void DrawSummonTab(
    const AnomalyUiServiceV1* ui,
    const std::string& selected,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& labels,
    const std::string& last_summon,
    bool player_position_valid) {
    const auto label_at = [&labels, &ids](std::size_t i) -> std::string {
        const std::string host_label = i < labels.size() ? labels[i] : std::string{};
        return VehicleShownName(ids[i], host_label);
    };
    const bool has_selected = std::ranges::find(ids, selected) != ids.end();
    if (!last_summon.empty()) DrawText(ui, "上次召唤：" + last_summon);

    std::string current_filter;
    {
        std::scoped_lock lock(g_context.mutex);
        current_filter = g_context.catalog_filter;
    }
    const auto filter_ids = [&](std::string_view filter) {
        std::vector<std::size_t> matches_out;
        matches_out.reserve(ids.size());
        const bool can_host_filter =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, filter_match),
                sizeof(decltype(AnomalyUiServiceV1::filter_match))) && ui->filter_match;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto label = label_at(i);
            bool matches = filter.empty();
            if (!matches && can_host_filter) {
                matches = ui->filter_match(ui->user, anomaly::sdk::StringView(filter),
                    anomaly::sdk::StringView(label)) != 0;
                if (!matches) {
                    matches = ui->filter_match(ui->user, anomaly::sdk::StringView(filter),
                        anomaly::sdk::StringView(ids[i])) != 0;
                }
            } else if (!matches) {
                matches = AsciiContainsInsensitive(label, filter) ||
                    AsciiContainsInsensitive(ids[i], filter);
            }
            if (matches) matches_out.push_back(i);
        }
        return matches_out;
    };

    // 列表不创建可滚动子窗口；分页在渲染阶段限制为最多五行，因此不会显示旧版滚动条。
    constexpr std::size_t kVehiclesPerPage = 5U;
    static std::size_t current_page = 0U;
    auto filtered = filter_ids(current_filter);
    std::size_t page_count = filtered.empty()
        ? 1U : (filtered.size() + kVehiclesPerPage - 1U) / kVehiclesPerPage;
    if (current_page >= page_count) current_page = page_count - 1U;
    const std::size_t page_begin = current_page * kVehiclesPerPage;
    const std::size_t page_end = (std::min)(filtered.size(), page_begin + kVehiclesPerPage);
    std::vector<std::size_t> visible_indices;
    if (page_begin < page_end) {
        visible_indices.assign(filtered.begin() + static_cast<std::ptrdiff_t>(page_begin),
            filtered.begin() + static_cast<std::ptrdiff_t>(page_end));
    }

    if (ids.empty()) {
        DrawText(ui, "载具清单为空。");
    } else if (filtered.empty()) {
        DrawText(ui, "没有匹配项。");
    } else {
        // 每辆载具本身就是可点击的按钮，不再单独渲染“选择”列。
        for (const std::size_t index : visible_indices) {
            const std::string& id = ids[index];
            const bool current = id == selected;
            std::string display_label(label_at(index));

            // Host label format may append " (VehicleID)" after the user-facing
            // VehicleData.Name text. Keep the game display name as the visible label.
            if (!display_label.empty() && display_label.back() == ')') {
                const std::size_t suffix = display_label.rfind(" (");
                if (suffix != std::string::npos && suffix > 0U) {
                    display_label.resize(suffix);
                }
            }
            if (display_label.empty()) display_label = id;

            const std::string button_label =
                "[" + std::string(current ? "✓ " : "") + display_label +
                "]##vehicle-select-" + id + "-" + std::to_string(index);
            if (ui->button(ui->user, anomaly::sdk::StringView(button_label), 0.0F, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.pending_vehicle_id = id;
            }
        }
    }

    // 筛选标签与搜索框放在载具选项列表下方、分页控件上方；这里不创建滚动容器。
    DrawDivider(ui);
    const bool has_input_text =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, input_text),
            sizeof(decltype(AnomalyUiServiceV1::input_text))) && ui->input_text;
    std::array<char, 128> filter_buffer{};
    const std::size_t filter_length = (std::min)(current_filter.size(), filter_buffer.size() - 1U);
    std::copy_n(current_filter.data(), filter_length, filter_buffer.data());
    if (has_input_text) {
        DrawText(ui, "筛选");
        if (ui->input_text(ui->user, anomaly::sdk::StringView("搜索载具名称或编号"),
                filter_buffer.data(), filter_buffer.size(), 0U)) {
            std::string updated_filter(filter_buffer.data());
            if (updated_filter != current_filter) {
                current_filter = std::move(updated_filter);
                std::scoped_lock lock(g_context.mutex);
                g_context.catalog_filter = current_filter;
                current_page = 0U;
            }
        }
    }

    // 输入变化后立即重算匹配数量和分页；列表行在下一次绘制时同步更新。
    filtered = filter_ids(current_filter);
    page_count = filtered.empty()
        ? 1U : (filtered.size() + kVehiclesPerPage - 1U) / kVehiclesPerPage;
    if (current_page >= page_count) current_page = page_count - 1U;
    DrawText(ui, "匹配 " + std::to_string(filtered.size()) + " 项 / 共 " +
        std::to_string(ids.size()) + " 辆");
    if (DrawButtonEnabled(ui, "上一页", current_page > 0U)) --current_page;
    DrawInlineGap(ui);
    if (DrawButtonEnabled(ui, "下一页", current_page + 1U < page_count)) ++current_page;
    DrawText(ui, "第 " + std::to_string(current_page + 1U) + " / " +
        std::to_string(page_count) + " 页");

    const bool can_summon = player_position_valid && has_selected;
    if (DrawButtonEnabled(ui, "召唤", can_summon)) {
        g_context.summon.store(true, std::memory_order_release);
    }
    DrawInlineGap(ui);
    if (DrawButtonEnabled(ui, "改装载具清单", has_selected)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_page = true;
            g_context.customization_vehicle_id.clear();
            g_context.customization_options.clear();
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = "正在读取 " + selected + " 的实时改装目录";
        }
        g_context.refresh_customization_requested.store(true, std::memory_order_release);
    }
    DrawText(ui, "进入改装载具清单后，可按类别选择模块和涂装；未覆盖的模块槽位保留游戏默认配置。");
}

void DrawCustomizationDetail(
    const AnomalyUiServiceV1* ui,
    const std::string& selected,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& labels,
    const std::vector<CustomizationOption>& options,
    const std::vector<std::pair<std::uint32_t, std::string>>& module_overrides,
    const std::string& customization_vehicle_id,
    const std::string& skin_id,
    const std::string& customization_status,
    const std::string& last_summon,
    bool player_position_valid) {
    const auto selected_index = std::ranges::find(ids, selected);
    const bool has_selected = selected_index != ids.end();
    std::string selected_label = selected;
    if (has_selected) {
        const auto index = static_cast<std::size_t>(selected_index - ids.begin());
        selected_label = VehicleShownName(selected,
            index < labels.size() ? labels[index] : std::string{});
    }
    DrawText(ui, "改装载具清单 / " + selected_label);
    if (DrawButtonEnabled(ui, "重新读取改装数据", true)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_vehicle_id.clear();
            g_context.customization_options.clear();
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = "正在重新读取所选车型的改装目录";
        }
        g_context.refresh_customization_requested.store(true, std::memory_order_release);
    }
    // 只在出错时显示状态文本；正常流程不占用界面。
    if (customization_status.find("失败") != std::string::npos ||
        customization_status.find("无效") != std::string::npos) {
        DrawText(ui, customization_status);
    }
    if (!has_selected) {
        DrawText(ui, "当前车型不在已读取的载具目录中，无法应用改装。");
        return;
    }
    if (customization_vehicle_id != selected) {
        return;
    }
    if (options.empty()) {
        DrawText(ui, "没有取得可用的改装选项。请重新读取；若仍失败，请查看 Core 返回的目录校验诊断。");
        return;
    }

    static constexpr std::array<std::string_view, 27> kModuleTypeNames{
        "无", "发动机", "变速箱", "制动", "悬挂", "车身颜色模块",
        "前包围", "侧裙", "后包围", "尾翼", "贴花", "轮毂",
        "引擎盖", "车牌", "车顶", "轮胎", "附件", "限滑差速器 LSD",
        "车身刚度", "Skin 模块", "内饰皮革颜色", "内饰缝线颜色",
        "车辆铭牌", "轮胎烟雾", "轮胎痕迹", "拖钩", "ECU"
    };
    const auto override_for_type = [&](const std::uint32_t module_type) -> std::string_view {
        const auto found = std::ranges::find_if(module_overrides,
            [module_type](const auto& entry) { return entry.first == module_type; });
        return found == module_overrides.end()
            ? std::string_view{} : std::string_view(found->second);
    };
    const auto has_options_for_type = [&](const std::uint32_t type) {
        return std::ranges::any_of(options, [type](const CustomizationOption& option) {
            return option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE &&
                option.module_type == type;
        });
    };

    // 布局度量：标签卡按窗口宽度均分（最多 5 列）并整行居中；选项列表用整行宽度。
    float window_width = 0.0F;
    float window_height = 0.0F;
    const bool has_window_size =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, get_window_size),
            sizeof(decltype(AnomalyUiServiceV1::get_window_size))) &&
        ui->get_window_size != nullptr;
    if (has_window_size) ui->get_window_size(ui->user, &window_width, &window_height);
    const bool has_cursor_x =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, set_cursor_pos_x),
            sizeof(decltype(AnomalyUiServiceV1::set_cursor_pos_x))) &&
        ui->set_cursor_pos_x != nullptr;
    const bool has_same_line =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, same_line),
            sizeof(decltype(AnomalyUiServiceV1::same_line))) &&
        ui->same_line != nullptr;
    constexpr std::size_t kCategoryColumns = 5;
    constexpr float kGridGap = 8.0F;
    // 内容宽度上限：窗口再宽也维持图 1 的体量，整体贴窗口左边缘对齐。
    constexpr float kMaxSectionWidth = 540.0F;
    const float available_width = window_width > 96.0F ? window_width - 32.0F : 0.0F;
    const float section_width = available_width > 0.0F
        ? (std::min)(available_width, kMaxSectionWidth) : 0.0F;
    const float list_width = section_width > 0.0F ? section_width - 16.0F : 0.0F;
    const float card_width = section_width > 0.0F
        ? (section_width - kGridGap * static_cast<float>(kCategoryColumns - 1)) /
            static_cast<float>(kCategoryColumns)
        : 0.0F;
    const auto draw_wide_enabled_button = [&](const std::string& label,
                                              const bool enabled,
                                              const float width) -> bool {
        const auto view = anomaly::sdk::StringView(label);
        if (UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, button_enabled),
                sizeof(decltype(AnomalyUiServiceV1::button_enabled))) &&
            ui->button_enabled != nullptr) {
            return ui->button_enabled(ui->user, view, width, 0.0F,
                enabled ? 1 : 0) != 0;
        }
        return enabled && ui->button(ui->user, view, width, 0.0F) != 0;
    };
    const auto draw_skin_options = [&]() {
        DrawText(ui, "选择涂装");
        bool has_default_skin = false;
        for (const auto& option : options) {
            if (option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN) continue;
            has_default_skin = has_default_skin || option.is_default;
            const bool current = skin_id == option.id;
            std::string label = option.label.empty() ? option.id : option.label;
            label += "##custom-skin-" + option.id;
            if (ui->button(ui->user, anomaly::sdk::StringView(label), list_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.customization_skin_id = option.id;
                g_context.customization_status = "已选择涂装：" +
                    (option.label.empty() ? option.id : option.label) + " (" + option.id + ")";
            }
        }
        if (has_default_skin && !skin_id.empty()) {
            if (DrawButtonEnabled(ui, "恢复默认涂装##custom-reset-skin", true)) {
                const auto found = std::ranges::find_if(options, [](const CustomizationOption& option) {
                    return option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN &&
                        option.is_default;
                });
                if (found != options.end()) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.customization_skin_id = found->id;
                    g_context.customization_status = "已恢复游戏默认涂装：" +
                        (found->label.empty() ? found->id : found->label);
                }
            }
        }
    };
    const auto draw_module_options = [&](const std::uint32_t type) {
        DrawText(ui, "可选项目");
        bool has_default = false;
        for (const auto& option : options) {
            if (option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE ||
                option.module_type != type) continue;
            has_default = has_default || option.is_default;
            const std::string_view selected_option_id = override_for_type(type);
            const bool explicit_selected = !selected_option_id.empty() &&
                selected_option_id == option.id;
            const bool default_selected = selected_option_id.empty() && option.is_default;
            std::string label = option.label.empty() ? option.id : option.label;
            label += "##custom-module-" + std::to_string(type) + "-" + option.id;
            if (ui->button(ui->user, anomaly::sdk::StringView(label), list_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                auto& overrides = g_context.selected_module_overrides;
                if (option.is_default) {
                    std::erase_if(overrides, [type](const auto& entry) {
                        return entry.first == type;
                    });
                } else {
                    const auto found = std::ranges::find_if(overrides,
                        [type](const auto& entry) { return entry.first == type; });
                    if (found == overrides.end()) overrides.emplace_back(type, option.id);
                    else found->second = option.id;
                }
                g_context.customization_status = "已更新 " +
                    std::string(kModuleTypeNames[type]) + " 的选择";
            }
        }
        const std::string_view selected_option_id = override_for_type(type);
        if (has_default && !selected_option_id.empty()) {
            const std::string reset_label =
                "恢复此类别默认配置##custom-reset-module-" + std::to_string(type);
            if (DrawButtonEnabled(ui, reset_label, true)) {
                std::scoped_lock lock(g_context.mutex);
                std::erase_if(g_context.selected_module_overrides, [type](const auto& entry) {
                    return entry.first == type;
                });
                g_context.customization_status = "已恢复 " +
                    std::string(kModuleTypeNames[type]) + " 的游戏默认配置";
            }
        }
    };

    const bool child_api_available =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
            sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
        ui->begin_child && ui->end_child;
    // 类别标签卡：整行居中，一行最多 5 个；召唤按钮在标签卡下方整行显示。
    std::array<std::uint32_t, 28> categories{};
    std::size_t category_count = 0;
    categories[category_count++] = 0;   // 涂装
    for (std::uint32_t type = 1; type < kModuleTypeNames.size(); ++type) {
        if (has_options_for_type(type)) categories[category_count++] = type;
    }
    static std::size_t active_category = 0;
    if (active_category >= category_count) active_category = 0;

    const auto category_label = [&](const std::uint32_t type) -> std::string {
        return type == 0 ? std::string("涂装") : std::string(kModuleTypeNames[type]);
    };
    for (std::size_t row_begin = 0; row_begin < category_count; row_begin += kCategoryColumns) {
        const std::size_t row_end = (std::min)(category_count, row_begin + kCategoryColumns);
        const std::size_t row_count = row_end - row_begin;
        // 整体块贴窗口左边缘；块内每一行（含不足 5 个的最后一行）水平居中。
        if (has_cursor_x && section_width > 0.0F) {
            const float row_width = static_cast<float>(row_count) * card_width +
                static_cast<float>(row_count - 1) * kGridGap;
            const float start_x = (section_width - row_width) * 0.5F;
            ui->set_cursor_pos_x(ui->user, start_x > 0.0F ? start_x : 0.0F);
        }
        for (std::size_t i = row_begin; i < row_end; ++i) {
            const std::string label = category_label(categories[i]);
            if (ui->button(ui->user, anomaly::sdk::StringView(label), card_width, 0.0F)) {
                active_category = i;
            }
            if (i + 1 < row_end) {
                if (has_same_line) ui->same_line(ui->user, 0.0F, kGridGap);
                else DrawInlineGap(ui);
            }
        }
    }

    const bool can_summon_customized = has_selected &&
        customization_vehicle_id == selected && !options.empty();
    if (draw_wide_enabled_button("召唤改装车辆", can_summon_customized, section_width)) {
        SetStatus("已提交改装召唤请求");
        SetLastSummon("改装召唤请求已提交");
        g_context.summon_customized.store(true, std::memory_order_release);
    } else if (customization_vehicle_id != selected) {
        DrawText(ui, "当前车型的改装目录尚未就绪；请重新读取改装数据。");
    }

    DrawDivider(ui);
    DrawText(ui, "改装清单 / " + category_label(categories[active_category]));
    bool child_visible = true;
    if (child_api_available) {
        child_visible = ui->begin_child(ui->user,
            anomaly::sdk::StringView("##vehicle-customization-options"),
            section_width, 320.0F, 0U) != 0;
    }
    if (child_visible) {
        if (categories[active_category] == 0) {
            draw_skin_options();
        } else {
            draw_module_options(categories[active_category]);
        }
    }
    if (child_api_available) ui->end_child(ui->user);
}


// 右侧载具清单：一行一辆，行内居中；选择后左侧切换到该车型的改装数据。
void DrawVehicleListColumn(
    const AnomalyUiServiceV1* ui,
    const std::string& selected,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& labels) {
    DrawText(ui, "载具清单");
    DrawText(ui, "选择车型后，右侧显示该车型的改装选项");
    if (ids.empty()) {
        DrawText(ui, "载具清单为空。");
        return;
    }

    float column_width = 0.0F;
    float column_height = 0.0F;
    const bool has_window_size =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, get_window_size),
            sizeof(decltype(AnomalyUiServiceV1::get_window_size))) &&
        ui->get_window_size != nullptr;
    if (has_window_size) ui->get_window_size(ui->user, &column_width, &column_height);
    const bool has_cursor_x =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, set_cursor_pos_x),
            sizeof(decltype(AnomalyUiServiceV1::set_cursor_pos_x))) &&
        ui->set_cursor_pos_x != nullptr;
    const float row_width = column_width > 64.0F
        ? (std::min)(column_width - 24.0F, 340.0F) : 0.0F;

    // 对照表名称；颜色差异合并到基础车型，只保留第一条作为召唤入口。
    std::vector<std::string> shown_names;
    std::vector<std::string> dedupe_keys;
    shown_names.reserve(ids.size());
    dedupe_keys.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        shown_names.push_back(VehicleShownName(
            ids[i], i < labels.size() ? labels[i] : std::string{}));
        dedupe_keys.push_back(VehicleDedupeKey(shown_names.back()));
    }
    std::vector<std::size_t> entries;
    entries.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        bool duplicate = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (dedupe_keys[j] == dedupe_keys[i]) { duplicate = true; break; }
        }
        if (!duplicate) entries.push_back(i);
    }

    // 载具清单放在带边框、可滚动的子窗口里。高度取行高的整数倍，
    // 静止时底部正好停在整行边界，不会露出半行文字。
    constexpr float kListRowPitch = 40.0F;
    const float list_available = column_height > 160.0F ? column_height - 60.0F : 420.0F;
    const float list_rows = (std::max)(1.0F, std::floor(list_available / kListRowPitch));
    const float list_box_height = list_rows * kListRowPitch;
    const bool child_api_available =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
            sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
        ui->begin_child && ui->end_child;
    bool box_visible = true;
    if (child_api_available) {
        box_visible = ui->begin_child(ui->user,
            anomaly::sdk::StringView("##vehicle-list-box"),
            0.0F, list_box_height, 1U) != 0;
    }
    if (box_visible) {
        for (const std::size_t i : entries) {
            std::string label = shown_names[i];
            const std::string button_label = label + "##vehicle-pick-" + ids[i];
            if (row_width > 0.0F && has_cursor_x) {
                const float start_x = (column_width - row_width) * 0.5F;
                ui->set_cursor_pos_x(ui->user, start_x > 0.0F ? start_x : 0.0F);
            }
            if (ui->button(ui->user, anomaly::sdk::StringView(button_label),
                    row_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.pending_vehicle_id = ids[i];
            }
        }
    }
    if (child_api_available) ui->end_child(ui->user);
}

// 改装主页面：左侧改装详情（固定 540），右侧载具清单。
void DrawCustomizationTab(
    const AnomalyUiServiceV1* ui,
    const std::string& selected,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& labels,
    const std::vector<CustomizationOption>& options,
    const std::vector<std::pair<std::uint32_t, std::string>>& module_overrides,
    const std::string& customization_vehicle_id,
    const std::string& skin_id,
    const std::string& customization_status,
    const std::string& last_summon,
    bool player_position_valid) {
    const bool child_api_available =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
            sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
        ui->begin_child && ui->end_child;
    if (!child_api_available) {
        DrawCustomizationDetail(ui, selected, ids, labels, options, module_overrides,
            customization_vehicle_id, skin_id, customization_status, last_summon,
            player_position_valid);
        return;
    }
    float window_width = 0.0F;
    float window_height = 0.0F;
    const bool has_window_size =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, get_window_size),
            sizeof(decltype(AnomalyUiServiceV1::get_window_size))) &&
        ui->get_window_size != nullptr;
    if (has_window_size) ui->get_window_size(ui->user, &window_width, &window_height);
    const bool has_same_line =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, same_line),
            sizeof(decltype(AnomalyUiServiceV1::same_line))) &&
        ui->same_line != nullptr;
    constexpr float kDetailWidth = 540.0F;
    constexpr float kColumnGap = 12.0F;
    const float detail_width = window_width > 0.0F
        ? (std::min)(kDetailWidth, window_width) : kDetailWidth;
    const float list_width = window_width > detail_width + kColumnGap + 240.0F
        ? window_width - detail_width - kColumnGap - 16.0F : 0.0F;
    const float column_height = window_height > 160.0F
        ? (std::min)(580.0F, window_height - 48.0F) : 520.0F;

    if (list_width > 0.0F) {
        const bool list_visible = ui->begin_child(ui->user,
            anomaly::sdk::StringView("##customization-vehicle-list"),
            list_width, column_height, 0U) != 0;
        if (list_visible) DrawVehicleListColumn(ui, selected, ids, labels);
        ui->end_child(ui->user);
        if (has_same_line) ui->same_line(ui->user, 0.0F, kColumnGap);
        else DrawInlineGap(ui);
    }

    const bool detail_visible = ui->begin_child(ui->user,
        anomaly::sdk::StringView("##customization-detail"),
        detail_width, column_height, 0U) != 0;
    if (detail_visible) {
        DrawCustomizationDetail(ui, selected, ids, labels, options, module_overrides,
            customization_vehicle_id, skin_id, customization_status, last_summon,
            player_position_valid);
    }
    ui->end_child(ui->user);

    DrawDivider(ui);
    DrawText(ui, "仅手动选择的模块槽位会覆盖默认值；其他槽位使用游戏数据表的默认模块。");
    if (!player_position_valid) {
        DrawText(ui, "当前玩家位置快照无效；可以提交请求，但 Host 会拒绝未验证坐标的召唤。");
    }
}


void DrawPerformanceTab(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    float speed_ratio, float torque_ratio, float other_speed_ratio,
    float other_speed_floor_ratio, float other_torque_ratio) {
    if ((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) == 0) {
        DrawText(ui, "当前驾驶载具：未识别");
    } else {
        DrawText(ui, "当前驾驶载具 / " +
            (snap.vehicle_class_name.empty() ? std::string("未知类名") : snap.vehicle_class_name) +
            " (" + (snap.vehicle_entity_id != 0
                ? "#" + std::to_string(snap.vehicle_entity_id) : std::string("#编号未知")) + ")");
        const double display_speed = std::isfinite(snap.speed_kmh)
            ? std::floor(std::fabs(snap.speed_kmh)) : 0.0;
        DrawText(ui, "当前速度 / " +
            std::to_string(static_cast<std::uint64_t>(display_speed)) + " km/h");
        DrawText(ui, "当前最高速倍率 / " + std::to_string(snap.top_speed_ratio) + "x");
    }

    DrawDivider(ui);
    DrawText(ui, "当前载具车速倍率（拖动后立即应用，范围 0.0x–100.0x）");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("车速倍率"),
                         &speed_ratio, kMinimumSpeedRatio, kMaximumSpeedRatio)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.speed_ratio = std::clamp(
                speed_ratio, kMinimumSpeedRatio, kMaximumSpeedRatio);
        }
        g_context.apply_speed.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, "当前陆地载具耐久：自动保持满值（默认启用）");
    DrawText(ui, "依赖 Dump 已校验的 RepairSelf(true)；当前 Dump 未确认水上载具的等价修复入口。");

    DrawDivider(ui);
    DrawText(ui, "发动机外部扭矩倍率");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("发动机扭矩倍率"),
                         &torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.engine_torque_ratio = torque_ratio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用扭矩倍率"), 0.0F, 0.0F)) {
        g_context.apply_torque.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, "其他载具实例");
    DrawText(ui, "目标最高速 = 70 km/h × 速度因子");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("其他车辆速度倍率"),
                         &other_speed_ratio, 0.05F, 10.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_speed_ratio = other_speed_ratio;
    }
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("其他车辆最低车速倍率"),
                         &other_speed_floor_ratio, 0.05F, 10.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_speed_floor_ratio = other_speed_floor_ratio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用其他车辆倍率与下限"), 0.0F, 0.0F)) {
        g_context.apply_other_speed.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, "非玩家车辆发动机外部扭矩倍率");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("其他车辆扭矩倍率"),
                         &other_torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_torque_ratio = other_torque_ratio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用其他车辆扭矩倍率"), 0.0F, 0.0F)) {
        g_context.apply_other_torque.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    if (ui->button(ui->user,
            anomaly::sdk::StringView("恢复默认设置 / 车速倍率 1.0x"), 0.0F, 0.0F)) {
        g_context.reset.store(true, std::memory_order_release);
    }
}

void DrawChassisTab(const AnomalyUiServiceV1* ui, bool friction) {
    DrawText(ui, "车轮摩擦");
    DrawInlineGap(ui);
    if (ui->button(ui->user, anomaly::sdk::StringView(friction ? "关闭" : "开启"), 0.0F, 0.0F)) {
        g_context.friction_toggle.store(true, std::memory_order_release);
    }
}

void DrawDebugPanel(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    const std::string& status, const std::string& selected,
    std::size_t catalog_size, bool player_position_valid) {
    DrawText(ui, "构建标记：NTE Vehicle 0.10.5 / 2026-10-21-clickable-display-names");
    DrawText(ui, "构建标记：manifest schema v2 / Plugin API v1 / Vehicle service v8");
    DrawText(ui, "Plugin ID：anomaly.local." + std::string("nte-vehicle"));
    DrawText(ui, "Manifest：schemaVersion=2 / entry=plugin.dll / loadPhase=game-ready / games=nte / builds=nte-*");
    DrawText(ui, "Required services：anomaly.ui v1 / anomaly.input v1 / anomaly.window v1 / anomaly.nte.vehicle v8 / anomaly.nte.player v1");
    DrawText(ui, "Capabilities：ui / input / nte-vehicle / nte-player-snapshot / per-vehicle-customization-catalog / selected-modification-summon");
    DrawText(ui, "创建日期：2026-10-14");
    DrawText(ui, "修改状态：独立 K 菜单、改装车辆召唤、0–10 倍车速滑块与当前陆地载具自动满耐久；游戏内结果待实测");
    DrawText(ui, "Host 状态：" + (status.empty() ? std::string("暂无状态消息") : status));
    // An empty diagnostic string means the Host snapshot callback returned OK.
    // Do not label this success path as "Host 未提供快照状态"; use the validity flag
    // to distinguish a valid current vehicle from a successful-but-empty snapshot.
    const std::string snapshot_status = !snap.vehicle_snapshot_status.empty()
        ? snap.vehicle_snapshot_status
        : (((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0)
            ? std::string("Host 快照正常：已识别当前驾驶载具")
            : std::string("Host 快照调用成功，但当前未识别驾驶载具"));
    DrawText(ui, "载具快照状态：" + snapshot_status);
    DrawText(ui, "驾驶载具对象：" +
        (((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0)
            ? (snap.vehicle_class_name.empty() ? std::string("已识别，类名未知") : snap.vehicle_class_name)
            : std::string("未识别")));
    DrawText(ui, "驾驶载具 Entity ID：" + (snap.vehicle_entity_id != 0
        ? std::to_string(snap.vehicle_entity_id) : std::string("不可用")));
    DrawText(ui, "VehicleID：" + (selected.empty() ? std::string("不可用") : selected));
    DrawText(ui, "TopSpeedRatio：" +
        (((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0)
            ? std::to_string(snap.top_speed_ratio) : std::string("不可用")));
    DrawText(ui, "玩家位置快照：" + std::string(player_position_valid ? "VALID" : "INVALID"));
    DrawText(ui, "载具目录：" + std::to_string(catalog_size) + " 项；当前选项：" +
        (selected.empty() ? std::string("无") : selected));

    DrawDivider(ui);
    DrawText(ui, "车速倍率：0.0x–100.0x；拖动滑块后自动应用");
}

void DrawContents(const AnomalyUiServiceV1* ui) {
    if (!ui || !ui->text || !ui->button || !ui->slider_float) return;

    Snapshot snap;
    std::string status;
    std::string selected;
    std::string last_summon;
    std::vector<std::string> ids;
    std::vector<std::string> labels;
    std::vector<CustomizationOption> customization_options;
    std::vector<std::pair<std::uint32_t, std::string>> module_overrides;
    std::string customization_vehicle_id;
    std::string customization_skin_id;
    std::string customization_status;
    float speed_ratio;
    float torque_ratio;
    float other_speed_ratio;
    float other_speed_floor_ratio;
    float other_torque_ratio;
    bool friction;
    bool player_position_valid{};
    {
        std::scoped_lock lock(g_context.mutex);
        snap = g_context.snapshot;
        status = g_context.status;
        selected = g_context.selected_vehicle_id;
        last_summon = g_context.last_summon;
        ids = g_context.vehicle_ids;
        labels = g_context.vehicle_labels;
        customization_options = g_context.customization_options;
        module_overrides = g_context.selected_module_overrides;
        customization_vehicle_id = g_context.customization_vehicle_id;
        customization_skin_id = g_context.customization_skin_id;
        customization_status = g_context.customization_status;
        speed_ratio = g_context.speed_ratio;
        torque_ratio = g_context.engine_torque_ratio;
        other_speed_ratio = g_context.other_speed_ratio;
        other_speed_floor_ratio = g_context.other_speed_floor_ratio;
        other_torque_ratio = g_context.other_torque_ratio;
        friction = g_context.friction_enabled;
        player_position_valid = g_context.player_position_valid;
    }

    static int active_tab = 0;

    DrawText(ui, "K：打开/关闭载具菜单；按住 Alt 呼出游戏原生光标。");

    const auto nav_button = [&](std::string_view label, int tab) {
        const std::string shown = active_tab == tab
            ? "[" + std::string(label) + "]" : std::string(label);
        if (ui->button(ui->user, anomaly::sdk::StringView(shown), 0.0F, 0.0F))
            active_tab = tab;
        DrawInlineGap(ui);
    };
    nav_button("改装载具", 0);
    nav_button("性能调校", 1);
    nav_button("车轮摩擦", 2);

    nav_button("诊断", 3);
    if (ui->button(ui->user, anomaly::sdk::StringView("刷新载具列表"), 0.0F, 0.0F)) {
        g_context.refresh_catalog_requested.store(true, std::memory_order_release);
        SetStatus("已请求重新整理载具目录");
    }
    DrawDivider(ui);

    if (active_tab == 0) {
        DrawCustomizationTab(ui, selected, ids, labels, customization_options,
            module_overrides, customization_vehicle_id, customization_skin_id,
            customization_status, last_summon, player_position_valid);
    } else if (active_tab == 1) {
        DrawPerformanceTab(ui, snap, speed_ratio, torque_ratio, other_speed_ratio,
            other_speed_floor_ratio, other_torque_ratio);
    } else if (active_tab == 2) {
        DrawChassisTab(ui, friction);
    } else if (active_tab == 3) {
        DrawDebugPanel(ui, snap, status, selected, ids.size(), player_position_valid);
    }
}

void Draw(const AnomalyUiServiceV1* ui) {
    if (!ui || !g_context.window_service || g_context.menu_window_handle.id == 0) return;

    AnomalyWindowStateV1 state{};
    state.struct_size = sizeof(state);
    const auto state_status = g_context.window_service->state(
        g_context.window_service->user, g_context.menu_window_handle, &state);
    if (state_status.code != ANOMALY_STATUS_V1_OK || state.open == 0) return;

    std::int32_t visible = 0;
    const auto begin_status = g_context.window_service->begin(
        g_context.window_service->user, g_context.menu_window_handle, 0U, &visible);
    if (begin_status.code != ANOMALY_STATUS_V1_OK) {
        SetStatus(StatusDescription("载具菜单窗口打开失败", begin_status));
        return;
    }
    if (visible != 0) DrawContents(ui);
    const auto end_status = g_context.window_service->end(
        g_context.window_service->user, g_context.menu_window_handle);
    if (end_status.code != ANOMALY_STATUS_V1_OK) {
        SetStatus(StatusDescription("载具菜单窗口收尾失败", end_status));
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!host || !plugin_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto* vehicle = QueryService<AnomalyNteVehicleServiceV1>(
        host, ANOMALY_NTE_VEHICLE_SERVICE_V1_ID, ANOMALY_NTE_VEHICLE_SERVICE_V1_VERSION);
    const auto* player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    const auto* input = QueryService<AnomalyInputServiceV1>(
        host, ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    const auto* window_service = QueryService<AnomalyWindowServiceV1>(
        host, ANOMALY_WINDOW_SERVICE_V1_ID, ANOMALY_WINDOW_SERVICE_V1_VERSION);
    const auto* plugin_state = QueryService<AnomalyPluginStateServiceV1>(
        host, ANOMALY_PLUGIN_STATE_SERVICE_V1_ID, ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION);
    if (!vehicle || !player || !InputReady(input) || !WindowReady(window_service)) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            "载具菜单需要 anomaly.input 与 anomaly.window 服务");
    }

    if (!vehicle->snapshot || !vehicle->set_top_speed_ratio ||
        !vehicle->summon_vehicle || !vehicle->set_wheel_friction_enabled ||
        !vehicle->reset || !vehicle->catalog_snapshot || !vehicle->vehicle_id_at ||
        !vehicle->set_summon_vehicle_id ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::refresh_catalog)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, refresh_catalog)) ||
        !vehicle->refresh_catalog ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::catalog_label_at)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, catalog_label_at)) ||
        !vehicle->catalog_label_at ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_top_speed_ratio)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_top_speed_ratio)) ||
        !vehicle->set_other_vehicles_top_speed_ratio ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_min_speed_ratio)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_min_speed_ratio)) ||
        !vehicle->set_other_vehicles_min_speed_ratio ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_engine_torque_ratio)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_engine_torque_ratio)) ||
        !vehicle->set_other_vehicles_engine_torque_ratio ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_catalog_snapshot)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_catalog_snapshot)) ||
        !vehicle->customization_catalog_snapshot ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_option_at)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_option_at)) ||
        !vehicle->customization_option_at ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::repair_current_vehicle)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, repair_current_vehicle)) ||
        !vehicle->repair_current_vehicle || !player->snapshot) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    AnomalyWindowSpecV1 window_spec{};
    window_spec.struct_size = sizeof(window_spec);
    window_spec.flags = ANOMALY_WINDOW_V1_NO_COLLAPSE;
    window_spec.id = anomaly::sdk::StringView("nte-vehicle-independent-menu");
    window_spec.title = anomaly::sdk::StringView("载具控制");
    window_spec.initial_width = 1000.0F;
    window_spec.initial_height = 780.0F;
    window_spec.minimum_width = 860.0F;
    window_spec.minimum_height = 560.0F;
    window_spec.maximum_width = 1400.0F;
    window_spec.maximum_height = 1000.0F;
    window_spec.default_open = 0;
    const auto window_status = window_service->register_window(
        window_service->user, &window_spec, &g_context.menu_window_handle);
    if (window_status.code != ANOMALY_STATUS_V1_OK || g_context.menu_window_handle.id == 0) {
        g_context.menu_window_handle = {};
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            "无法注册独立载具菜单窗口");
    }

    g_context.vehicle = vehicle;
    g_context.player = player;
    g_context.input = input;
    g_context.window_service = window_service;
    g_context.plugin_state = plugin_state;
    // 生命周期回调里解析 state 目录并缓存；游戏/渲染回调不允许同步 state I/O。
    if (plugin_state != nullptr && plugin_state->directory != nullptr) {
        std::array<char, 512> buffer{};
        std::size_t size = buffer.size();
        if (plugin_state->directory(plugin_state->user, buffer.data(), &size).code ==
                ANOMALY_STATUS_V1_OK &&
            size > 0 && size < buffer.size()) {
            g_context.state_directory.assign(buffer.data(), size);
            while (!g_context.state_directory.empty() &&
                   (g_context.state_directory.back() == '\0' ||
                    g_context.state_directory.back() == '\r' ||
                    g_context.state_directory.back() == '\n')) {
                g_context.state_directory.pop_back();
            }
        }
    }
    g_context.started.store(false, std::memory_order_release);
    g_context.apply_speed.store(false, std::memory_order_release);
    g_context.apply_torque.store(false, std::memory_order_release);
    g_context.apply_other_speed.store(false, std::memory_order_release);
    g_context.apply_other_torque.store(false, std::memory_order_release);
    g_context.reset.store(false, std::memory_order_release);
    g_context.summon.store(false, std::memory_order_release);
    g_context.summon_customized.store(false, std::memory_order_release);
    g_context.friction_toggle.store(false, std::memory_order_release);
    g_context.refresh_catalog_requested.store(false, std::memory_order_release);
    g_context.refresh_customization_requested.store(false, std::memory_order_release);
    g_context.durability_last_status_code.store(
        ANOMALY_STATUS_V1_NOT_FOUND, std::memory_order_release);
    g_context.speed_ratio = kDefaultSpeedRatio;
    g_context.engine_torque_ratio = 1.0F;
    g_context.other_speed_ratio = 1.0F;
    g_context.other_speed_floor_ratio = 0.6024F;
    g_context.other_torque_ratio = 1.0F;
    g_context.friction_enabled = true;

    g_context.snapshot = {};
    g_context.selected_vehicle_id = "Vehicle007";
    g_context.saved_vehicle_id = LoadSavedSelection();
    g_context.saved_selection_pending = !g_context.saved_vehicle_id.empty();
    if (!g_context.saved_vehicle_id.empty()) {
        g_context.selected_vehicle_id = g_context.saved_vehicle_id;
    }
    g_context.pending_vehicle_id.clear();
    g_context.catalog_filter.clear();
    g_context.vehicle_ids.clear();
    g_context.vehicle_labels.clear();
    g_context.customization_page = true;
    g_context.customization_vehicle_id.clear();
    g_context.customization_skin_id.clear();
    g_context.customization_status = "尚未读取当前车型的改装数据";
    g_context.customization_options.clear();
    g_context.selected_module_overrides.clear();
    g_context.customization_catalog_tick = 0;
    g_context.last_summon.clear();
    g_context.status = "正在读取 Host 验证的运行时 Vehicle 目录";
    g_context.player_position_valid = false;
    g_context.player_snapshot = {};
    g_context.catalog_tick = 0;
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (!InputReady(g_context.input) || !WindowReady(g_context.window_service) ||
        g_context.menu_window_handle.id == 0) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "载具菜单窗口或输入服务不可用");
    }

    ReleaseMenuHotkey(g_context);
    AnomalyHotkeySpecV1 hotkey_spec{};
    hotkey_spec.struct_size = sizeof(hotkey_spec);
    hotkey_spec.virtual_key = static_cast<std::uint32_t>('K');
    hotkey_spec.flags = ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED;
    const std::string hotkey_id = "nte-vehicle-toggle-menu-k";
    hotkey_spec.id = anomaly::sdk::StringView(hotkey_id);
    const auto hotkey_status = g_context.input->register_hotkey(
        g_context.input->user, &hotkey_spec, ToggleMenuHotkey,
        &g_context, &g_context.menu_hotkey_handle);
    if (hotkey_status.code != ANOMALY_STATUS_V1_OK ||
        g_context.menu_hotkey_handle.id == 0) {
        g_context.menu_hotkey_handle = {};
        return Status(hotkey_status.code == ANOMALY_STATUS_V1_OK
                ? ANOMALY_STATUS_V1_FAILED : hotkey_status.code,
            "无法注册 K 菜单快捷键");
    }

    g_context.toggle_menu_requested.store(false, std::memory_order_release);
    static_cast<void>(g_context.window_service->set_open(
        g_context.window_service->user, g_context.menu_window_handle, 0));
    g_context.refresh_catalog_requested.store(true, std::memory_order_release);
    g_context.apply_speed.store(true, std::memory_order_release);
    g_context.started.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(false, std::memory_order_release);
    g_context.toggle_menu_requested.store(false, std::memory_order_release);
    ReleaseMenuHotkey(g_context);
    if (g_context.window_service && g_context.menu_window_handle.id != 0) {
        static_cast<void>(g_context.window_service->set_open(
            g_context.window_service->user, g_context.menu_window_handle, 0));
    }
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.speed_ratio = kDefaultSpeedRatio;
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    g_context.toggle_menu_requested.store(false, std::memory_order_release);
    ReleaseMenuHotkey(g_context);
    if (g_context.window_service && g_context.menu_window_handle.id != 0) {
        static_cast<void>(g_context.window_service->release_window(
            g_context.window_service->user, g_context.menu_window_handle));
    }
    g_context.menu_window_handle = {};
    g_context.vehicle = nullptr;
    g_context.player = nullptr;
    g_context.input = nullptr;
    g_context.window_service = nullptr;
}

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) {
    if (plugin_context == &g_context) Update();
}

void ANOMALY_CALL DrawCallback(void* plugin_context, const AnomalyUiServiceV1* ui) {
    if (plugin_context == &g_context) Draw(ui);
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-vehicle"),
        anomaly::sdk::StringView("载具控制"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.10.3"), Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}
