#include "backend.hpp"
#include "runtime.hpp"
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ui_resources.h"
#include "anomaly/sdk/services/plugin_state.h"
#include "anomaly/sdk/localization.hpp"

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

// 车速倍率直接调节，无需额外启用/关闭；范围 0.05x–100x。
constexpr float kDefaultSpeedRatio = 1.0F;
constexpr float kMinimumSpeedRatio = 0.05F;
constexpr float kMaximumSpeedRatio = 100.0F;

struct Snapshot {
    std::uint32_t flags{};
    double speed_kmh{};
    float top_speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    bool friction{true};
    std::uint64_t vehicle_entity_id{};
    std::string vehicle_class_name;
    std::string vehicle_snapshot_status;
};

struct CustomizationOption {
    std::string id;
    std::string label;
    std::uint32_t kind{};
    std::uint32_t module_type{};
    bool is_default{};
};

struct Context {
    nte_vehicle::Runtime* vehicle{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyInputServiceV1* input{};
    const AnomalyWindowServiceV1* window_service{};
    const AnomalyPluginStateServiceV1* plugin_state{};
    anomaly::sdk::Localizer localizer;
    AnomalyGenerationHandleV1 menu_window_handle{};
    AnomalyGenerationHandleV1 menu_hotkey_handle{};
    std::atomic_bool toggle_menu_requested{};
    AnomalyNtePlayerSnapshotV1 player_snapshot{};
    bool player_position_valid{};
    std::mutex mutex;
    Snapshot snapshot{};
    std::string status;
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
    std::string customization_status;
    bool customization_status_failure{};
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

// 本地化后的失败消息要活到 Host 读完返回值，所以缓存在插件自己的缓冲里。
std::string g_failure_message;

AnomalyStatusV1 FailureStatus(
    std::uint32_t code, std::string_view key, std::string_view english) {
    g_failure_message = g_context.localizer.Text(key, english);
    return Status(code, g_failure_message);
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
    if (status.message.data != nullptr && status.message.size != 0) {
        const std::array arguments{
            prefix, std::string_view(status.message.data, status.message.size)};
        return g_context.localizer.Format(
            "text.status_with_message", "{0}: {1}", arguments);
    }
    const std::string code = std::to_string(status.code);
    const std::array arguments{prefix, std::string_view(code)};
    return g_context.localizer.Format(
        "text.status_with_code", "{0} (status code {1})", arguments);
}

void SetLastSummon(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.last_summon = std::move(text);
}

// 目录由插件读取实时 DataTable；读取前后序号相同才发布完整目录。
void RefreshCatalogOnGameThread() {
    auto* vehicle = g_context.vehicle;
    if (!vehicle) {
        SetStatus(g_context.localizer.Text("status.catalog_interface_missing",
            "The vehicle catalog backend is unavailable."));
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids.clear();
        g_context.vehicle_labels.clear();
        return;
    }

    nte_vehicle::CatalogSnapshot before{};

    const auto before_status = vehicle->VehicleCatalogSnapshot(&before);
    if (before_status.code != ANOMALY_STATUS_V1_OK ||
        (before.flags & nte_vehicle::kCatalogValid) == 0 ||
        before.entry_count > 8192U) {
        SetStatus(StatusDescription(
            g_context.localizer.Text("status.catalog_read_failed", "Catalog read failed"),
            before_status));
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
        std::array<char, nte_vehicle::kVehicleIdMaxBytes + 1U> id_buffer{};
        std::array<char, nte_vehicle::kVehicleIdMaxBytes * 2U + 8U> label_buffer{};
        std::size_t id_size = id_buffer.size();
        const auto id_status = vehicle->VehicleIdAt(i, id_buffer.data(), &id_size);
        if (id_status.code != ANOMALY_STATUS_V1_OK || id_size <= 1U || id_size > id_buffer.size()) {
            const std::string index = std::to_string(i);
            const std::array arguments{std::string_view(index)};
            SetStatus(StatusDescription(
                g_context.localizer.Format("status.row_name_read_failed",
                    "Row name read failed (index {0})", arguments),
                id_status));
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }

        std::size_t label_size = label_buffer.size();
        const auto label_status = vehicle->VehicleCatalogLabelAt(i, label_buffer.data(), &label_size);
        if (label_status.code != ANOMALY_STATUS_V1_OK ||
            label_size <= 1U || label_size > label_buffer.size()) {
            const std::string index = std::to_string(i);
            const std::array arguments{std::string_view(index)};
            SetStatus(StatusDescription(
                g_context.localizer.Format("status.catalog_label_read_failed",
                    "Display name read failed (index {0})", arguments),
                label_status));
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }
        ids.emplace_back(id_buffer.data(), id_size - 1U);
        labels.emplace_back(label_buffer.data(), label_size - 1U);
        if (ids.back().empty() || labels.back().empty()) {
            SetStatus(g_context.localizer.Text("status.catalog_empty_entry",
                "Catalog read failed: an empty RowName or display label was returned."));
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }
    }

    nte_vehicle::CatalogSnapshot after{};

    const auto after_status = vehicle->VehicleCatalogSnapshot(&after);
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & nte_vehicle::kCatalogValid) == 0 ||
        after.sequence != before.sequence || after.entry_count != ids.size()) {
        SetStatus(StatusDescription(
            g_context.localizer.Text("status.catalog_changed",
                "The DataTable changed while the catalog was being read"),
            after_status));
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
        const std::string count = std::to_string(g_context.vehicle_ids.size());
        const std::array arguments{std::string_view(count)};
        g_context.status = g_context.localizer.Format(
            "status.catalog_loaded", "Loaded {0} DT_VehicleData records.", arguments);
    }

    // Always synchronize the current UI selection with the plugin runtime. Otherwise the
    // default item can look selected while selected_summon_vehicle_id is still empty.
    if (!selected_id.empty()) {
        const auto selected_status = vehicle->VehicleSetSummonVehicleId(anomaly::sdk::StringView(selected_id));
        if (selected_status.code != ANOMALY_STATUS_V1_OK)
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.catalog_selection_rejected",
                    "The catalog was read, but the selected RowName was not accepted"),
                selected_status));
    }
}

void RefreshCustomizationCatalogOnGameThread() {
    auto* vehicle = g_context.vehicle;
    std::string selected_id;
    {
        std::scoped_lock lock(g_context.mutex);
        selected_id = g_context.selected_vehicle_id;
    }
    if (!vehicle || selected_id.empty()) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id.clear();
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status = selected_id.empty()
            ? g_context.localizer.Text("status.customization_pick_vehicle",
                "Select a vehicle from the summon list first.")
            : g_context.localizer.Text("status.customization_interface_missing",
                "The vehicle customization backend is unavailable.");
        g_context.customization_status_failure = false;
        return;
    }

    nte_vehicle::CatalogSnapshot before{};

    const auto before_status = vehicle->VehicleCustomizationCatalogSnapshot(anomaly::sdk::StringView(selected_id), &before);
    if (before_status.code != ANOMALY_STATUS_V1_OK ||
        (before.flags & nte_vehicle::kCatalogValid) == 0 ||
        before.entry_count > 8192U) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status = StatusDescription(
            g_context.localizer.Text("status.customization_catalog_read_failed",
                "Could not read the customization catalog for this vehicle"),
            before_status);
        g_context.customization_status_failure = true;
        return;
    }

    std::vector<CustomizationOption> options;
    options.reserve(before.entry_count);
    for (std::uint32_t index = 0; index < before.entry_count; ++index) {
        nte_vehicle::CustomizationEntry native_option{};

        const auto option_status = vehicle->VehicleCustomizationOptionAt(anomaly::sdk::StringView(selected_id), index, &native_option);
        if (option_status.code != ANOMALY_STATUS_V1_OK ||
            (native_option.flags & nte_vehicle::kOptionValid) == 0) {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_options.clear();
            g_context.customization_vehicle_id = selected_id;
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            const std::string index_text = std::to_string(index);
            const std::array arguments{std::string_view(index_text)};
            g_context.customization_status = StatusDescription(
                g_context.localizer.Format("status.customization_option_read_failed",
                    "Could not read customization catalog option (index {0})", arguments),
                option_status);
            g_context.customization_status_failure = true;
            return;
        }
        if (native_option.option_id.empty() ||
            (native_option.kind != nte_vehicle::kCustomizationModule &&
             native_option.kind != nte_vehicle::kCustomizationSkin) ||
            (native_option.kind == nte_vehicle::kCustomizationModule &&
             (native_option.module_type == 0 || native_option.module_type >= 27)) ||
            (native_option.kind == nte_vehicle::kCustomizationSkin &&
             native_option.module_type != 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_options.clear();
            g_context.customization_vehicle_id = selected_id;
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = g_context.localizer.Text(
                "status.customization_option_invalid",
                "The customization catalog contains an invalid ID or module type.");
            g_context.customization_status_failure = true;
            return;
        }
        CustomizationOption option{};
        option.id = std::move(native_option.option_id);
        option.label = std::move(native_option.label);
        option.kind = native_option.kind;
        option.module_type = native_option.module_type;
        option.is_default =
            (native_option.flags & nte_vehicle::kOptionDefault) != 0;
        options.push_back(std::move(option));
    }

    nte_vehicle::CatalogSnapshot after{};

    const auto after_status = vehicle->VehicleCustomizationCatalogSnapshot(anomaly::sdk::StringView(selected_id), &after);
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & nte_vehicle::kCatalogValid) == 0 ||
        after.sequence != before.sequence || after.entry_count != options.size()) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status = g_context.localizer.Text(
            "status.customization_catalog_changed",
            "The vehicle data changed while the customization catalog was being read; reopen the customization list.");
        g_context.customization_status_failure = false;
        return;
    }

    std::string default_skin;
    std::string fallback_skin;
    for (const auto& option : options) {
        if (option.kind != nte_vehicle::kCustomizationSkin) continue;
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
                return option.kind == nte_vehicle::kCustomizationModule;
            });
        const auto skin_count = std::ranges::count_if(
            g_context.customization_options, [](const CustomizationOption& option) {
                return option.kind == nte_vehicle::kCustomizationSkin;
            });
        const std::string module_text = std::to_string(module_count);
        const std::string skin_text = std::to_string(skin_count);
        const std::array arguments{
            std::string_view(module_text), std::string_view(skin_text)};
        g_context.customization_status = g_context.localizer.Format(
            "status.customization_loaded",
            "Loaded live customization options for this vehicle: {0} modules, {1} skins; "
            "unselected module slots keep the game data table defaults.",
            arguments);
        g_context.customization_status_failure = false;
        if (used_fallback_skin) {
            g_context.customization_status += g_context.localizer.Text(
                "status.customization_skin_fallback",
                "; this vehicle has no default skin, so the first valid skin is preselected and can be changed below");
        } else if (g_context.customization_skin_id.empty() && skin_count == 0) {
            g_context.customization_status += g_context.localizer.Text(
                "status.customization_no_skins",
                "; this vehicle has no skin options, so customized summoning uses the game default materials");
        } else if (g_context.customization_skin_id.empty()) {
            g_context.customization_status += g_context.localizer.Text(
                "status.customization_choose_skin",
                "; this vehicle has no default skin, choose one below");
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
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.menu_toggle_failed",
                    "The K hotkey could not toggle the vehicle menu"),
                toggle_status));
        }
    }

    // Keep the current wheeled vehicle at maximum durability by calling Dump-validated
    // RepairSelf(true) on every Game Update. The plugin resolves and validates the current vehicle
    // each time; the plugin does not keep a stale actor pointer.
    {
        const auto durability_status = g_context.vehicle->VehicleRepairCurrentDurability();
        const auto previous_code = g_context.durability_last_status_code.exchange(
            durability_status.code, std::memory_order_acq_rel);
        if (durability_status.code != ANOMALY_STATUS_V1_OK &&
            durability_status.code != ANOMALY_STATUS_V1_NOT_FOUND &&
            previous_code != durability_status.code) {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.durability_hold_failed",
                    "Keeping the current vehicle durability full failed"),
                durability_status));
        } else if (durability_status.code == ANOMALY_STATUS_V1_OK &&
                   previous_code != ANOMALY_STATUS_V1_OK &&
                   previous_code != ANOMALY_STATUS_V1_NOT_FOUND) {
            SetStatus(g_context.localizer.Text("status.durability_restored",
                "Durability protection for the current land vehicle recovered."));
        }
    }

    // Explicit refresh runs on Game thread; a failed scan waits for another user request
    // instead of rescanning UObject/DataTable every frame.
    if (g_context.refresh_catalog_requested.exchange(false, std::memory_order_acq_rel)) {
        const auto refresh_status = g_context.vehicle->VehicleCatalogRefresh();
        if (refresh_status.code == ANOMALY_STATUS_V1_OK) {
            RefreshCatalogOnGameThread();
        } else {
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.vehicle_ids.clear();
                g_context.vehicle_labels.clear();
            }
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.catalog_refresh_failed",
                    "Catalog refresh failed"),
                refresh_status));
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
        const auto status = g_context.vehicle->VehicleSetSummonVehicleId(anomaly::sdk::StringView(pending_vehicle_id));
        if (status.code == ANOMALY_STATUS_V1_OK) {
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.selected_vehicle_id = pending_vehicle_id;
                const std::array arguments{std::string_view(pending_vehicle_id)};
                g_context.status = g_context.localizer.Format(
                    "status.vehicle_selected", "Selected vehicle: {0}", arguments);
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
            g_context.saved_vehicle_id = pending_vehicle_id;
        } else {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.vehicle_select_failed",
                    "Vehicle selection failed"),
                status));
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
    if (catalog_loaded) {
        nte_vehicle::CatalogSnapshot catalog{};

        const auto catalog_status = g_context.vehicle->VehicleCatalogSnapshot(&catalog);
        if (catalog_status.code == ANOMALY_STATUS_V1_OK &&
            (catalog.flags & nte_vehicle::kCatalogValid) != 0) {
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
            const std::string invalid_text = StatusDescription(
                g_context.localizer.Text("status.catalog_invalid",
                    "The vehicle catalog is invalid"),
                catalog_status);
            const std::array invalid_arguments{std::string_view(invalid_text)};
            SetStatus(g_context.localizer.Format("status.catalog_invalid_retry",
                "{0}; click \"Refresh\" to retry", invalid_arguments));
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
        const auto status = g_context.vehicle->VehicleReset();
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.speed_ratio = kDefaultSpeedRatio;
        }
        g_context.apply_speed.store(true, std::memory_order_release);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK
            ? g_context.localizer.Text("status.reset_applied",
                  "Default vehicle settings restored; the speed ratio is back to 1.0x.")
            : g_context.localizer.Text("status.reset_failed_reapplied",
                  "Reset failed; the default 1.0x speed ratio is still reapplied."));
    }

    if (g_context.apply_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio{};
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = std::clamp(g_context.speed_ratio,
                kMinimumSpeedRatio, kMaximumSpeedRatio);
            g_context.speed_ratio = ratio;
        }
        const auto status = g_context.vehicle->VehicleSetTopSpeedRatio(ratio);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            const std::string ratio_text = std::to_string(ratio);
            const std::array arguments{std::string_view(ratio_text)};
            SetStatus(g_context.localizer.Format("status.speed_applied",
                "Current speed and top speed cap applied: {0}x", arguments));
        } else if (status.message.data != nullptr && status.message.size != 0) {
            const std::array arguments{
                std::string_view(status.message.data, status.message.size)};
            SetStatus(g_context.localizer.Format("status.speed_apply_failed",
                "Applying the speed ratio failed: {0}", arguments));
        } else {
            const std::string code = std::to_string(status.code);
            const std::array arguments{std::string_view(code)};
            SetStatus(g_context.localizer.Format("status.speed_apply_failed_code",
                "Applying the speed ratio failed, status code {0}", arguments));
        }
    }

    if (g_context.apply_torque.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.engine_torque_ratio;
        }
        const auto status = g_context.vehicle->VehicleSetEngineTorqueRatio(ratio);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            const std::string ratio_text = std::to_string(ratio);
            const std::array arguments{std::string_view(ratio_text)};
            SetStatus(g_context.localizer.Format("status.torque_applied",
                "Engine torque ratio applied: {0}x", arguments));
        } else if (status.message.data != nullptr && status.message.size != 0) {
            const std::array arguments{
                std::string_view(status.message.data, status.message.size)};
            SetStatus(g_context.localizer.Format("status.torque_apply_failed",
                "Applying the engine torque ratio failed: {0}", arguments));
        } else {
            const std::string code = std::to_string(status.code);
            const std::array arguments{std::string_view(code)};
            SetStatus(g_context.localizer.Format("status.torque_apply_failed_code",
                "Applying the engine torque ratio failed, status code {0}", arguments));
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
        const auto floor_status = g_context.vehicle->VehicleSetOtherVehiclesMinSpeedRatio(floor_ratio);
        if (floor_status.code != ANOMALY_STATUS_V1_OK) {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.other_min_speed_failed",
                    "Setting the non-player vehicle minimum speed ratio failed"),
                floor_status));
        } else {
            const auto status = g_context.vehicle->VehicleSetOtherVehiclesTopSpeedRatio(ratio);
            SetStatus(status.code == ANOMALY_STATUS_V1_OK
                ? StatusDescription(
                      g_context.localizer.Text("status.other_speed_applied",
                          "Non-player vehicle speed and floor updated"),
                      status)
                : StatusDescription(
                      g_context.localizer.Text("status.other_speed_partial",
                          "Non-player vehicle speed update did not fully succeed"),
                      status));
        }
    }

    if (g_context.apply_other_torque.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.other_torque_ratio;
        }
        const auto status = g_context.vehicle->VehicleSetOtherVehiclesEngineTorqueRatio(ratio);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK
            ? StatusDescription(
                  g_context.localizer.Text("status.other_torque_applied",
                      "Non-player vehicle torque updated"),
                  status)
            : StatusDescription(
                  g_context.localizer.Text("status.other_torque_partial",
                      "Non-player vehicle torque update did not fully succeed"),
                  status));
    }

    const bool summon_requested = g_context.summon.exchange(false, std::memory_order_acq_rel);
    const bool summon_customized_requested = g_context.summon_customized.exchange(false, std::memory_order_acq_rel);
    if (summon_requested || summon_customized_requested) {
        nte_vehicle::SummonRequest request{};

        request.flags = nte_vehicle::kSummonHasPosition |
            nte_vehicle::kSummonSetOwnerToPlayer;

        std::string selected_id;
        std::string selected_label;
        std::string selected_skin_id;
        std::string customization_vehicle_id;
        std::vector<std::pair<std::uint32_t, std::string>> module_overrides;
        std::vector<nte_vehicle::ModuleSelection> module_selections;
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
                validation_error = g_context.localizer.Text(
                    "status.customization_vehicle_mismatch",
                    "Customized summon failed: the customization list does not match the selected vehicle, reopen the list.");
            } else {
                request.flags |= nte_vehicle::kSummonCustomConfiguration;
                if (!selected_skin_id.empty()) {
                    request.skin_id = anomaly::sdk::StringView(std::string_view(
                        selected_skin_id.data(), selected_skin_id.size()));
                }
                module_selections.reserve(module_overrides.size());
                for (const auto& override_entry : module_overrides) {
                    nte_vehicle::ModuleSelection selection{};

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
                ? g_context.localizer.Text("status.summon_position_invalid",
                      "Summon failed: the player position is currently invalid.")
                : g_context.localizer.Text("status.summon_selection_unsynced",
                      "Summon failed: the current selection is not synchronized with the vehicle catalog.");
            SetStatus(reason);
            SetLastSummon(reason);
        } else {
            const auto status = g_context.vehicle->VehicleSummon(&request);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                const std::string detail = status.message.data != nullptr && status.message.size != 0
                    ? std::string(status.message.data, status.message.size)
                    : std::string{};
                std::string result;
                if (summon_customized_requested) {
                    const std::array arguments{std::string_view(detail)};
                    result = g_context.localizer.Format("status.summon_customized_ok",
                        "Customized vehicle summoned: {0}", arguments);
                    SetStatus(result);
                    SetLastSummon(result);
                } else {
                    SetStatus(detail);
                    SetLastSummon(detail);
                }
            } else {
                const auto reason = StatusDescription(
                    summon_customized_requested
                        ? g_context.localizer.Text("status.summon_customized_failed",
                              "Customized vehicle summon failed")
                        : g_context.localizer.Text("status.summon_failed", "Summon failed"),
                    status);
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
        const auto status = g_context.vehicle->VehicleSetWheelFriction(enabled);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.friction_enabled = enabled;
            g_context.status = enabled
                ? g_context.localizer.Text("status.friction_enabled", "Wheel friction enabled.")
                : g_context.localizer.Text("status.friction_disabled", "Wheel friction disabled.");
        } else {
            SetStatus(g_context.localizer.Text("status.friction_toggle_failed",
                "Toggling wheel friction failed."));
        }
    }

    nte_vehicle::StateSnapshot snapshot{};
    const auto status = g_context.vehicle->VehicleSnapshot(&snapshot);
    std::scoped_lock lock(g_context.mutex);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        g_context.snapshot.flags = snapshot.flags;
        g_context.snapshot.speed_kmh = snapshot.speed_kmh;
        g_context.snapshot.top_speed_ratio = snapshot.top_speed_ratio;
        g_context.snapshot.engine_torque_ratio = snapshot.engine_torque_ratio;
        g_context.snapshot.friction = snapshot.wheel_friction_enabled != 0;
        g_context.snapshot.vehicle_entity_id = snapshot.vehicle_entity_id;
        g_context.snapshot.vehicle_class_name = snapshot.vehicle_class_name;
        g_context.snapshot.vehicle_snapshot_status.clear();
    } else {
        g_context.snapshot.flags = 0;
        g_context.snapshot.vehicle_entity_id = 0;
        g_context.snapshot.vehicle_class_name.clear();
        if (status.message.data != nullptr && status.message.size != 0) {
            g_context.snapshot.vehicle_snapshot_status =
                std::string(status.message.data, status.message.size);
        } else {
            const std::string code = std::to_string(status.code);
            const std::array arguments{std::string_view(code)};
            g_context.snapshot.vehicle_snapshot_status = g_context.localizer.Format(
                "status.snapshot_status_code", "Vehicle snapshot status code {0}", arguments);
        }
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
struct VehicleNameRow {
    std::string_view id;
    std::string_view key;
    std::string_view english;
};

std::string VehicleDisplayName(std::string_view vehicle_id) {
    static constexpr VehicleNameRow kNames[] = {
        {"Vehicle002Combat", "vehicle.name.griffin", "Griffin"},
        {"Vehicle002_q110116", "vehicle.name.ae86_fujiwara", "Driving school AE86 (Fujiwara job)"},
        {"Vehicle003Combat", "vehicle.name.g3_stock", "G3 stock"},
        {"Vehicle003Scaled", "vehicle.name.g3_stock", "G3 stock"},
        {"Vehicle003_oneoff", "vehicle.name.g3_khaki", "G3 khaki"},
        {"Vehicle004_oneoff", "vehicle.name.griffin_volante", "GriffinVolante"},
        {"Vehicle004_q110320", "vehicle.name.ae86_fujiwara", "Driving school AE86 (Fujiwara job)"},
        {"Vehicle005", "vehicle.name.vehicle005_untested", "Vehicle005 (error, untested)"},
        {"Vehicle005_Player", "vehicle.name.city_bus", "City bus"},
        {"Vehicle005a", "vehicle.name.vehicle005a_untested", "Vehicle005a (error, untested)"},
        {"Vehicle006Combat", "vehicle.name.van", "Van"},
        {"Vehicle007", "vehicle.name.c2000", "C2000"},
        {"Vehicle007Combat", "vehicle.name.c2000", "C2000"},
        {"Vehicle007_drivingschool", "vehicle.name.driving_school", "Driving school car"},
        {"Vehicle007_oneoff", "vehicle.name.c2000_white", "C2000 white"},
        {"Vehicle007_s111601", "vehicle.name.c2000_white", "C2000 white"},
        {"Vehicle007_s111601_race", "vehicle.name.c2000_npc", "C2000 (NPC driver)"},
        {"Vehicle007_v2", "vehicle.name.c2000_white", "C2000 white"},
        {"Vehicle007a", "vehicle.name.c2000_gold", "C2000 gold"},
        {"Vehicle007c", "vehicle.name.taxi", "Taxi"},
        {"Vehicle008Combat", "vehicle.name.m1000", "M1000"},
        {"Vehicle008_K111801", "vehicle.name.m1000", "M1000"},
        {"Vehicle008_oneoff", "vehicle.name.m1000_red", "M1000 red"},
        {"Vehicle008fish", "vehicle.name.m1000_gold", "M1000 gold"},
        {"Vehicle009", "vehicle.name.pickup_truck", "Pickup truck"},
        {"Vehicle009a", "vehicle.name.container_truck", "Container truck"},
        {"Vehicle009a_toq", "vehicle.name.container_truck", "Container truck"},
        {"Vehicle009b", "vehicle.name.blue_pickup", "Blue transport pickup"},
        {"Vehicle009c", "vehicle.name.crew_pickup", "Work crew transport pickup"},
        {"Vehicle010_q110328", "vehicle.name.st_x950", "ST-X950 bike"},
        {"Vehicle012", "vehicle.name.dominus_s", "Dominus-S"},
        {"Vehicle014", "vehicle.name.small_yacht", "Small yacht"},
        {"Vehicle015", "vehicle.name.e_scooter", "E-scooter"},
        {"Vehicle015A", "vehicle.name.e_scooter_green", "Green e-scooter"},
        {"Vehicle016Combat", "vehicle.name.lavelox", "LaVelox"},
        {"Vehicle017Combat", "vehicle.name.pursuit_v8", "Pursuit-V8"},
        {"Vehicle018Combat", "vehicle.name.st79", "ST79(AE86)"},
        {"Vehicle018_K110000", "vehicle.name.st79_tofu", "ST79(AE86) tofu shop car"},
        {"Vehicle018_K110027", "vehicle.name.st79_legend", "ST79(AE86) racing legend job"},
        {"Vehicle018_K110045", "vehicle.name.st79_legend_trailing",
            "ST79(AE86) racing legend job - trailing"},
        {"Vehicle018_Quest_K110000", "vehicle.name.st79_tofu", "ST79(AE86) tofu shop car"},
        {"Vehicle018ghost", "vehicle.name.ghost_ae86", "Ghost AE86"},
        {"Vehicle020Combat", "vehicle.name.enforcer", "Enforcer"},
        {"Vehicle020Police", "vehicle.name.c2000", "C2000"},
        {"Vehicle021Combat", "vehicle.name.pendragon", "Pendragon"},
        {"Vehicle024_Mods", "vehicle.name.blizzard_v4", "Blizzard V4 (bike)"},
        {"Vehicle027", "vehicle.name.business_van", "Business van"},
        {"Vehicle031_toq", "vehicle.name.dayun_pickup", "Dayun pickup"},
        {"Vehicle033", "vehicle.name.road_roller", "Road roller"},
        {"Vehicle034", "vehicle.name.forklift", "Forklift"},
        {"Vehicle035", "vehicle.name.wheel_loader", "Wheel loader"},
        {"Vehicle036", "vehicle.name.excavator", "Excavator"},
        {"Vehicle040", "vehicle.name.retail_truck", "Retail truck"},
        {"Vehicle041", "vehicle.name.retail_truck_2", "Retail truck 2"},
        {"Vehicle044", "vehicle.name.surf_boat", "Surf boat"},
        {"Vehicle044Collect", "vehicle.name.motorboat", "Motorboat"},
        {"Vehicle044Motorboatrace", "vehicle.name.motorboat", "Motorboat"},
        {"Vehicle044TestDrive", "vehicle.name.motorboat_purple", "Motorboat (purple)"},
        {"Vehicle054", "vehicle.name.baby_bus", "Baby bus"},
        {"Vehicle055", "vehicle.name.row_to_tomorrow", "Row to Tomorrow"},
        {"Vehicle066", "vehicle.name.titanic", "Titanic"},
        {"Vehicle_010_Mods", "vehicle.name.st_x950", "ST-X950 bike"},
        {"Vehicle_013_Hight_WJ106501", "vehicle.name.old_times", "Old Times"},
        {"Vehicle_015_Hight_WJ100105", "vehicle.name.e_scooter", "E-scooter"},
        {"vehicle002", "vehicle.name.griffin", "Griffin"},
        {"vehicle002_oneoff", "vehicle.name.orange_griffin", "Orange Griffin"},
        {"vehicle003", "vehicle.name.g3_green", "G3 green"},
        {"vehicle004", "vehicle.name.griffin_volante_same", "GriffinVolante (same as Griffin)"},
        {"vehicle006", "vehicle.name.b100_rose", "B100 rose red"},
        {"vehicle008", "vehicle.name.m1000", "M1000"},
        {"vehicle010", "vehicle.name.st_x950", "ST-X950 bike"},
        {"vehicle013", "vehicle.name.old_times", "Old Times"},
        {"vehicle016", "vehicle.name.lavelox", "LaVelox"},
        {"vehicle017", "vehicle.name.pursuit_v8", "Pursuit-V8"},
        {"vehicle018", "vehicle.name.st79_plain", "ST79"},
        {"vehicle020", "vehicle.name.enforcer", "Enforcer"},
        {"vehicle021", "vehicle.name.pendragon", "Pendragon"},
        {"vehicle023", "vehicle.name.k01_kart", "K01 kart"},
        {"vehicle024", "vehicle.name.blizzard_v4", "Blizzard V4 (bike)"},
        {"vehicle026", "vehicle.name.porsche_918", "Porsche918Spyder (Porsche 918)"},
        {"vehicle030", "vehicle.name.wheelchair", "Wheelchair"},
        {"vehicle031", "vehicle.name.dayun_pickup", "Dayun pickup"},
        {"vehicle032", "vehicle.name.cement_truck", "Cement truck"},
        {"vehicle037", "vehicle.name.porsche_911", "Porsche911Turbo (Porsche 911)"},
        {"vehicle038", "vehicle.name.taycan_gt", "PorscheTaycanTurboGT (electric Porsche)"},
        {"vehicle039", "vehicle.name.draco", "Draco (transforming bike)"},
        {"vehicle043", "vehicle.name.offroader", "Small off-roader"},
    };
    for (const auto& entry : kNames) {
        if (entry.id == vehicle_id) {
            return g_context.localizer.Text(entry.key, entry.english);
        }
    }
    return {};
}

// 优先使用对照表里的名称；缺失时退回目录标签并去掉 " (VehicleID)"。
std::string VehicleShownName(const std::string& vehicle_id, const std::string& catalog_label) {
    const std::string mapped = VehicleDisplayName(vehicle_id);
    if (!mapped.empty()) return mapped;
    std::string label = catalog_label.empty() ? vehicle_id : catalog_label;
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
    // 匹配目录标签里的中文备注/配色后缀，以及本地化目录里的英文等价写法。
    static constexpr std::string_view kSuffixes[] = {
        "\uff08\u548cgriffin\u76f8\u540c\uff09", "\uff08\u7d2b\u914d\u8272\uff09",
        "\uff08\u76f8\u540c\uff09", "(\u76f8\u540c)",
        " (same as griffin)", " (purple)", " (same)"};
    for (const auto suffix : kSuffixes) {
        if (key.size() >= suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            key.resize(key.size() - suffix.size());
        }
    }
    static constexpr std::string_view kColorTokens[] = {
        "\u73ab\u7ea2\u8272", "\u5361\u5176\u8272", "\u7d2b\u914d\u8272", "\u539f\u8272",
        "\u7eff\u8272", "\u7ea2\u8272", "\u91d1\u8272", "\u767d\u8272", "\u6a59\u8272",
        "\u84dd\u8272", "\u7d2b\u8272", "\u9ed1\u8272", "\u94f6\u8272", "\u7070\u8272",
        "rose red", "khaki", "stock", "green", "red", "gold", "white", "orange",
        "blue", "black", "silver", "gray"};
    for (const auto token : kColorTokens) {
        std::size_t pos = 0;
        while ((pos = key.find(token, pos)) != std::string::npos) {
            key.erase(pos, token.size());
        }
    }
    while (true) {
        const std::size_t pos = key.find("\uff08\uff09");
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
        const std::string catalog_label = i < labels.size() ? labels[i] : std::string{};
        return VehicleShownName(ids[i], catalog_label);
    };
    const bool has_selected = std::ranges::find(ids, selected) != ids.end();
    if (!last_summon.empty()) {
        const std::array arguments{std::string_view(last_summon)};
        DrawText(ui, g_context.localizer.Format(
            "text.last_summon", "Last summon: {0}", arguments));
    }

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
        DrawText(ui, g_context.localizer.Text("text.catalog_empty",
            "The vehicle catalog is empty."));
    } else if (filtered.empty()) {
        DrawText(ui, g_context.localizer.Text("text.no_matches", "No matching entries."));
    } else {
        // 每辆载具本身就是可点击的按钮，不再单独渲染“选择”列。
        for (const std::size_t index : visible_indices) {
            const std::string& id = ids[index];
            const bool current = id == selected;
            std::string display_label(label_at(index));

            // Catalog labels may append " (VehicleID)" after the user-facing
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
        DrawText(ui, g_context.localizer.Text("label.filter", "Filter"));
        const auto search_hint = g_context.localizer.Text(
            "hint.search_placeholder", "Search vehicle name or ID");
        if (ui->input_text(ui->user, anomaly::sdk::StringView(search_hint),
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
    const std::string match_count = std::to_string(filtered.size());
    const std::string total_count = std::to_string(ids.size());
    const std::array match_arguments{
        std::string_view(match_count), std::string_view(total_count)};
    DrawText(ui, g_context.localizer.Format(
        "text.match_summary", "{0} matches / {1} vehicles", match_arguments));
    const auto previous_label =
        g_context.localizer.Text("action.previous_page", "Previous page");
    if (DrawButtonEnabled(ui, previous_label, current_page > 0U)) --current_page;
    DrawInlineGap(ui);
    const auto next_label = g_context.localizer.Text("action.next_page", "Next page");
    if (DrawButtonEnabled(ui, next_label, current_page + 1U < page_count)) ++current_page;
    const std::string page_text = std::to_string(current_page + 1U);
    const std::string page_total = std::to_string(page_count);
    const std::array page_arguments{
        std::string_view(page_text), std::string_view(page_total)};
    DrawText(ui, g_context.localizer.Format("text.page_of", "Page {0} / {1}", page_arguments));

    const bool can_summon = player_position_valid && has_selected;
    const auto summon_label = g_context.localizer.Text("action.summon", "Summon");
    if (DrawButtonEnabled(ui, summon_label, can_summon)) {
        g_context.summon.store(true, std::memory_order_release);
    }
    DrawInlineGap(ui);
    const auto customization_label =
        g_context.localizer.Text("action.customization_list", "Customize vehicles");
    if (DrawButtonEnabled(ui, customization_label, has_selected)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_page = true;
            g_context.customization_vehicle_id.clear();
            g_context.customization_options.clear();
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            const std::array arguments{std::string_view(selected)};
            g_context.customization_status = g_context.localizer.Format(
                "status.customization_reading",
                "Reading the live customization catalog for {0}", arguments);
            g_context.customization_status_failure = false;
        }
        g_context.refresh_customization_requested.store(true, std::memory_order_release);
    }
    DrawText(ui, g_context.localizer.Text("hint.customization_usage",
        "Pick modules and skins by category in the customization list; slots you do not "
        "override keep the game defaults."));
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
    bool customization_status_failure,
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
    const std::array title_arguments{std::string_view(selected_label)};
    DrawText(ui, g_context.localizer.Format(
        "text.customization_title", "Customize / {0}", title_arguments));
    if (!last_summon.empty()) {
        const std::array arguments{std::string_view(last_summon)};
        DrawText(ui, g_context.localizer.Format(
            "text.last_summon", "Last summon: {0}", arguments));
    }
    const auto summon_label = g_context.localizer.Text("action.summon", "Summon");
    if (DrawButtonEnabled(ui, summon_label, has_selected && player_position_valid)) {
        SetLastSummon(g_context.localizer.Text("status.summon_submitted", "Summon request submitted."));
        g_context.summon.store(true, std::memory_order_release);
    }
    const auto reload_label =
        g_context.localizer.Text("action.reload_customization", "Reload customization data");
    if (DrawButtonEnabled(ui, reload_label, true)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_vehicle_id.clear();
            g_context.customization_options.clear();
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = g_context.localizer.Text(
                "status.customization_rereading",
                "Re-reading the customization catalog of the selected vehicle.");
            g_context.customization_status_failure = false;
        }
        g_context.refresh_customization_requested.store(true, std::memory_order_release);
    }
    // 只在出错时显示状态文本；正常流程不占用界面。
    if (customization_status_failure) {
        DrawText(ui, customization_status);
    }
    if (!has_selected) {
        DrawText(ui, g_context.localizer.Text("text.customization_vehicle_missing",
            "The current vehicle is not in the loaded vehicle catalog, so customization "
            "cannot be applied."));
        return;
    }
    if (customization_vehicle_id != selected) {
        return;
    }
    if (options.empty()) {
        DrawText(ui, g_context.localizer.Text("text.customization_no_options",
            "No usable customization options were returned. Read again; if it still fails, "
            "check the vehicle catalog validation diagnostics."));
        return;
    }

    static constexpr std::array<std::string_view, 27> module_type_keys{
        "label.module.none", "label.module.engine", "label.module.gearbox",
        "label.module.brake", "label.module.suspension", "label.module.body_color",
        "label.module.front_bumper", "label.module.side_skirt", "label.module.rear_bumper",
        "label.module.rear_wing", "label.module.decal", "label.module.wheel",
        "label.module.hood", "label.module.plate", "label.module.roof",
        "label.module.tire", "label.module.accessory", "label.module.lsd",
        "label.module.stiffness", "label.module.skin", "label.module.interior_leather",
        "label.module.interior_stitch", "label.module.vehicle_badge",
        "label.module.tire_smoke", "label.module.tire_track", "label.module.tow_hook",
        "label.module.ecu"
    };
    static constexpr std::array<std::string_view, 27> module_type_english{
        "None", "Engine", "Gearbox", "Brakes", "Suspension", "Body color module",
        "Front bumper", "Side skirt", "Rear bumper", "Rear wing", "Decal", "Wheels",
        "Hood", "License plate", "Roof", "Tires", "Accessories",
        "Limited-slip differential LSD", "Body rigidity", "Skin module",
        "Interior leather color", "Interior stitching color", "Vehicle badge",
        "Tire smoke", "Tire tracks", "Tow hook", "ECU"
    };
    const auto override_for_type = [&](const std::uint32_t module_type) -> std::string_view {
        const auto found = std::ranges::find_if(module_overrides,
            [module_type](const auto& entry) { return entry.first == module_type; });
        return found == module_overrides.end()
            ? std::string_view{} : std::string_view(found->second);
    };
    const auto has_options_for_type = [&](const std::uint32_t type) {
        return std::ranges::any_of(options, [type](const CustomizationOption& option) {
            return option.kind == nte_vehicle::kCustomizationModule &&
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
        DrawText(ui, g_context.localizer.Text("section.skins", "Choose a skin"));
        bool has_default_skin = false;
        for (const auto& option : options) {
            if (option.kind != nte_vehicle::kCustomizationSkin) continue;
            has_default_skin = has_default_skin || option.is_default;
            const bool current = skin_id == option.id;
            std::string label = option.label.empty() ? option.id : option.label;
            label += "##custom-skin-" + option.id;
            if (ui->button(ui->user, anomaly::sdk::StringView(label), list_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.customization_skin_id = option.id;
                const std::string& skin_label = option.label.empty() ? option.id : option.label;
                const std::array arguments{
                    std::string_view(skin_label), std::string_view(option.id)};
                g_context.customization_status = g_context.localizer.Format(
                    "status.skin_selected", "Skin selected: {0} ({1})", arguments);
                g_context.customization_status_failure = false;
            }
        }
        if (has_default_skin && !skin_id.empty()) {
            const std::string reset_skin_label =
                g_context.localizer.Text("action.reset_skin", "Restore default skin") +
                "##custom-reset-skin";
            if (DrawButtonEnabled(ui, reset_skin_label, true)) {
                const auto found = std::ranges::find_if(options, [](const CustomizationOption& option) {
                    return option.kind == nte_vehicle::kCustomizationSkin &&
                        option.is_default;
                });
                if (found != options.end()) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.customization_skin_id = found->id;
                    const std::string& skin_label =
                        found->label.empty() ? found->id : found->label;
                    const std::array arguments{std::string_view(skin_label)};
                    g_context.customization_status = g_context.localizer.Format(
                        "status.skin_reset", "Restored the game default skin: {0}", arguments);
                    g_context.customization_status_failure = false;
                }
            }
        }
    };
    const auto draw_module_options = [&](const std::uint32_t type) {
        DrawText(ui, g_context.localizer.Text("section.options", "Available options"));
        bool has_default = false;
        for (const auto& option : options) {
            if (option.kind != nte_vehicle::kCustomizationModule ||
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
                const std::string category = g_context.localizer.Text(
                    module_type_keys[type], module_type_english[type]);
                const std::array arguments{std::string_view(category)};
                g_context.customization_status = g_context.localizer.Format(
                    "status.module_updated", "Updated the {0} selection", arguments);
                g_context.customization_status_failure = false;
            }
        }
        const std::string_view selected_option_id = override_for_type(type);
        if (has_default && !selected_option_id.empty()) {
            const std::string category = g_context.localizer.Text(
                module_type_keys[type], module_type_english[type]);
            const std::string reset_label =
                g_context.localizer.Text("action.reset_module",
                    "Restore this category's default") +
                "##custom-reset-module-" + std::to_string(type);
            if (DrawButtonEnabled(ui, reset_label, true)) {
                std::scoped_lock lock(g_context.mutex);
                std::erase_if(g_context.selected_module_overrides, [type](const auto& entry) {
                    return entry.first == type;
                });
                const std::array arguments{std::string_view(category)};
                g_context.customization_status = g_context.localizer.Format(
                    "status.module_reset", "Restored the game default of {0}", arguments);
                g_context.customization_status_failure = false;
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
    for (std::uint32_t type = 1; type < module_type_keys.size(); ++type) {
        if (has_options_for_type(type)) categories[category_count++] = type;
    }
    static std::size_t active_category = 0;
    if (active_category >= category_count) active_category = 0;

    const auto category_label = [&](const std::uint32_t type) -> std::string {
        return type == 0
            ? g_context.localizer.Text("section.paint", "Paint")
            : g_context.localizer.Text(module_type_keys[type], module_type_english[type]);
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

    const bool can_summon_customized = has_selected && player_position_valid &&
        customization_vehicle_id == selected && !options.empty();
    const auto summon_customized_label =
        g_context.localizer.Text("action.summon_customized", "Summon customized vehicle");
    if (draw_wide_enabled_button(summon_customized_label, can_summon_customized, section_width)) {
        SetStatus(g_context.localizer.Text("status.customized_submitted",
            "Customized summon request submitted."));
        SetLastSummon(g_context.localizer.Text("text.customized_submitted",
            "Customized summon request submitted"));
        g_context.summon_customized.store(true, std::memory_order_release);
    } else if (customization_vehicle_id != selected) {
        DrawText(ui, g_context.localizer.Text("text.customization_not_ready",
            "The customization catalog for this vehicle is not ready; reload the "
            "customization data."));
    }

    DrawDivider(ui);
    const std::string heading = category_label(categories[active_category]);
    const std::array heading_arguments{std::string_view(heading)};
    DrawText(ui, g_context.localizer.Format(
        "text.customization_heading", "Customization / {0}", heading_arguments));
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
    DrawText(ui, g_context.localizer.Text("section.vehicle_list", "Vehicle list"));
    DrawText(ui, g_context.localizer.Text("hint.customization_column",
        "Select a vehicle to show its customization options on the right."));
    if (ids.empty()) {
        DrawText(ui, g_context.localizer.Text("text.catalog_empty",
            "The vehicle catalog is empty."));
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
    bool customization_status_failure,
    const std::string& last_summon,
    bool player_position_valid) {
    const bool child_api_available =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
            sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
        ui->begin_child && ui->end_child;
    if (!child_api_available) {
        DrawCustomizationDetail(ui, selected, ids, labels, options, module_overrides,
            customization_vehicle_id, skin_id, customization_status,
            customization_status_failure, last_summon, player_position_valid);
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
            customization_vehicle_id, skin_id, customization_status,
            customization_status_failure, last_summon, player_position_valid);
    }
    ui->end_child(ui->user);

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("hint.module_override",
        "Only module slots you pick by hand override the defaults; other slots keep the "
        "game data table default modules."));
    if (!player_position_valid) {
        DrawText(ui, g_context.localizer.Text("hint.position_invalid",
            "The current player position snapshot is invalid; you can still submit the "
            "request, but the plugin rejects summons with unverified coordinates."));
    }
}


void DrawPerformanceTab(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    float speed_ratio, float torque_ratio, float other_speed_ratio,
    float other_speed_floor_ratio, float other_torque_ratio) {
    if ((snap.flags & nte_vehicle::kSnapshotValid) == 0) {
        DrawText(ui, g_context.localizer.Text("text.current_vehicle_none",
            "Current vehicle: not identified"));
    } else {
        const std::string class_name = snap.vehicle_class_name.empty()
            ? g_context.localizer.Text("text.unknown_class", "Unknown class name")
            : snap.vehicle_class_name;
        const std::string entity_id = snap.vehicle_entity_id != 0
            ? "#" + std::to_string(snap.vehicle_entity_id)
            : g_context.localizer.Text("text.unknown_entity_id", "#ID unknown");
        const std::array vehicle_arguments{
            std::string_view(class_name), std::string_view(entity_id)};
        DrawText(ui, g_context.localizer.Format(
            "text.current_vehicle", "Current vehicle / {0} ({1})", vehicle_arguments));
        const double display_speed = std::isfinite(snap.speed_kmh)
            ? std::floor(std::fabs(snap.speed_kmh)) : 0.0;
        const std::string speed_text =
            std::to_string(static_cast<std::uint64_t>(display_speed));
        const std::array speed_arguments{std::string_view(speed_text)};
        DrawText(ui, g_context.localizer.Format(
            "text.current_speed", "Current speed / {0} km/h", speed_arguments));
        const std::string ratio_text = std::to_string(snap.top_speed_ratio);
        const std::array ratio_arguments{std::string_view(ratio_text)};
        DrawText(ui, g_context.localizer.Format(
            "text.current_top_speed_ratio", "Current top speed ratio / {0}x", ratio_arguments));
    }

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("hint.speed_slider",
        "Speed ratio of the current vehicle (applied right after dragging, 0.05x-100.0x)"));
    const auto speed_ratio_label =
        g_context.localizer.Text("label.speed_ratio", "Speed ratio");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView(speed_ratio_label),
                         &speed_ratio, kMinimumSpeedRatio, kMaximumSpeedRatio)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.speed_ratio = std::clamp(
                speed_ratio, kMinimumSpeedRatio, kMaximumSpeedRatio);
        }
        g_context.apply_speed.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("text.durability_note",
        "Current land vehicle durability: kept full automatically (enabled by default)"));
    DrawText(ui, g_context.localizer.Text("text.durability_detail",
        "Relies on the RepairSelf(true) already verified by the dump; the current dump does "
        "not confirm an equivalent repair entry point for water vehicles."));

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("text.torque_heading",
        "Engine external torque ratio"));
    const auto torque_ratio_label =
        g_context.localizer.Text("label.torque_ratio", "Engine torque ratio");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView(torque_ratio_label),
                         &torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.engine_torque_ratio = torque_ratio;
    }
    const auto apply_torque_label =
        g_context.localizer.Text("action.apply_torque", "Apply torque ratio");
    if (ui->button(ui->user, anomaly::sdk::StringView(apply_torque_label), 0.0F, 0.0F)) {
        g_context.apply_torque.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("section.other_vehicles", "Other vehicle instances"));
    DrawText(ui, g_context.localizer.Text("text.other_speed_formula",
        "Target top speed = 70 km/h x speed factor"));
    const auto other_speed_label =
        g_context.localizer.Text("label.other_speed_ratio", "Other vehicle speed ratio");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView(other_speed_label),
                         &other_speed_ratio, 0.05F, 10.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_speed_ratio = other_speed_ratio;
    }
    const auto other_speed_floor_label = g_context.localizer.Text(
        "label.other_speed_floor", "Other vehicle minimum speed ratio");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView(other_speed_floor_label),
                         &other_speed_floor_ratio, 0.05F, 10.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_speed_floor_ratio = other_speed_floor_ratio;
    }
    const auto apply_other_speed_label = g_context.localizer.Text(
        "action.apply_other_speed", "Apply other vehicle ratio and floor");
    if (ui->button(ui->user, anomaly::sdk::StringView(apply_other_speed_label), 0.0F, 0.0F)) {
        g_context.apply_other_speed.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("text.other_torque_heading",
        "Non-player vehicle engine external torque ratio"));
    const auto other_torque_label = g_context.localizer.Text(
        "label.other_torque_ratio", "Other vehicle torque ratio");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView(other_torque_label),
                         &other_torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_torque_ratio = other_torque_ratio;
    }
    const auto apply_other_torque_label = g_context.localizer.Text(
        "action.apply_other_torque", "Apply other vehicle torque ratio");
    if (ui->button(ui->user, anomaly::sdk::StringView(apply_other_torque_label), 0.0F, 0.0F)) {
        g_context.apply_other_torque.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    const auto reset_label = g_context.localizer.Text(
        "action.reset_defaults", "Restore defaults / speed ratio 1.0x");
    if (ui->button(ui->user, anomaly::sdk::StringView(reset_label), 0.0F, 0.0F)) {
        g_context.reset.store(true, std::memory_order_release);
    }
}

void DrawChassisTab(const AnomalyUiServiceV1* ui, bool friction) {
    DrawText(ui, g_context.localizer.Text("section.wheel_friction", "Wheel friction"));
    DrawInlineGap(ui);
    const auto friction_label = friction
        ? g_context.localizer.Text("action.disable", "Disable")
        : g_context.localizer.Text("action.enable", "Enable");
    if (ui->button(ui->user, anomaly::sdk::StringView(friction_label), 0.0F, 0.0F)) {
        g_context.friction_toggle.store(true, std::memory_order_release);
    }
}

void DrawDebugPanel(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    const std::string& status, const std::string& selected,
    std::size_t catalog_size, bool player_position_valid) {
    DrawText(ui, g_context.localizer.Text("detail.build_marker",
        "Build marker: NTE Vehicle 0.11.1"));
    DrawText(ui, g_context.localizer.Text("detail.build_marker_abi",
        "Build marker: manifest schema v2 / Plugin API v1"));
    const std::array plugin_id_arguments{
        std::string_view("anomaly.nte-vehicle")};
    DrawText(ui, g_context.localizer.Format("detail.plugin_id", "Plugin ID: {0}",
        plugin_id_arguments));
    DrawText(ui, g_context.localizer.Text("detail.manifest",
        "Manifest: schemaVersion=2 / entry=plugin.dll / loadPhase=game-ready / games=nte "
        "/ builds=nte-*"));
    DrawText(ui, g_context.localizer.Text("detail.required_services",
        "Vehicle execution: plugin-owned profile, reflection and Game-thread operations"));
    DrawText(ui, g_context.localizer.Text("detail.capabilities",
        "Capabilities: vehicle catalog, customized summons, speed, torque and durability"));
    DrawText(ui, g_context.localizer.Text("detail.created", "Created: 2026-10-14"));
    DrawText(ui, g_context.localizer.Text("detail.status_note",
        "Modification state: standalone K menu, customized vehicle summon, 0.05-100x speed "
        "sliders and automatic full durability for the current land vehicle; in-game "
        "results still pending."));
    const std::string status_text = status.empty()
        ? g_context.localizer.Text("text.no_status_message", "No status message yet")
        : status;
    const std::array status_arguments{std::string_view(status_text)};
    DrawText(ui, g_context.localizer.Format("detail.host_status", "Vehicle status: {0}",
        status_arguments));
    // An empty diagnostic string means the plugin runtime snapshot returned OK.
    // Do not label this success path as a missing snapshot status; use the validity flag
    // to distinguish a valid current vehicle from a successful-but-empty snapshot.
    const std::string snapshot_status = !snap.vehicle_snapshot_status.empty()
        ? snap.vehicle_snapshot_status
        : (((snap.flags & nte_vehicle::kSnapshotValid) != 0)
            ? g_context.localizer.Text("text.snapshot_ok_identified",
                  "Vehicle snapshot OK: the current vehicle was identified")
            : g_context.localizer.Text("text.snapshot_ok_unidentified",
                  "Vehicle snapshot call succeeded, but no vehicle is identified right now"));
    const std::array snapshot_arguments{std::string_view(snapshot_status)};
    DrawText(ui, g_context.localizer.Format("detail.snapshot_status",
        "Vehicle snapshot status: {0}", snapshot_arguments));
    const std::string driving_object =
        ((snap.flags & nte_vehicle::kSnapshotValid) != 0)
            ? (snap.vehicle_class_name.empty()
                ? g_context.localizer.Text("text.identified_unknown_class",
                      "Identified, class name unknown")
                : snap.vehicle_class_name)
            : g_context.localizer.Text("text.not_identified", "Not identified");
    const std::array object_arguments{std::string_view(driving_object)};
    DrawText(ui, g_context.localizer.Format("detail.driving_object",
        "Driving vehicle object: {0}", object_arguments));
    const std::string entity_text = snap.vehicle_entity_id != 0
        ? std::to_string(snap.vehicle_entity_id)
        : g_context.localizer.Text("text.unavailable", "Unavailable");
    const std::array entity_arguments{std::string_view(entity_text)};
    DrawText(ui, g_context.localizer.Format("detail.entity_id",
        "Driving vehicle entity ID: {0}", entity_arguments));
    const std::string selected_text = selected.empty()
        ? g_context.localizer.Text("text.unavailable", "Unavailable")
        : selected;
    const std::array selected_arguments{std::string_view(selected_text)};
    DrawText(ui, g_context.localizer.Format("detail.vehicle_id", "VehicleID: {0}",
        selected_arguments));
    const std::string top_speed_text =
        ((snap.flags & nte_vehicle::kSnapshotValid) != 0)
            ? std::to_string(snap.top_speed_ratio)
            : g_context.localizer.Text("text.unavailable", "Unavailable");
    const std::array top_speed_arguments{std::string_view(top_speed_text)};
    DrawText(ui, g_context.localizer.Format("detail.top_speed_ratio",
        "TopSpeedRatio: {0}", top_speed_arguments));
    const std::array position_arguments{
        std::string_view(player_position_valid ? "VALID" : "INVALID")};
    DrawText(ui, g_context.localizer.Format("detail.player_position",
        "Player position snapshot: {0}", position_arguments));
    const std::string catalog_count = std::to_string(catalog_size);
    const std::string catalog_selection = selected.empty()
        ? g_context.localizer.Text("text.none", "None")
        : selected;
    const std::array catalog_arguments{
        std::string_view(catalog_count), std::string_view(catalog_selection)};
    DrawText(ui, g_context.localizer.Format("detail.catalog",
        "Vehicle catalog: {0} entries; current selection: {1}", catalog_arguments));

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("detail.speed_ratio_note",
        "Speed ratio: 0.05x-100.0x; drag the slider to apply it automatically"));
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
    bool customization_status_failure{};
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
        customization_status_failure = g_context.customization_status_failure;
    }

    static int active_tab = 0;

    DrawText(ui, g_context.localizer.Text("hint.menu_keys",
        "K: open or close the vehicle menu; hold Alt to show the native game cursor."));

    const auto nav_button = [&](std::string_view label, int tab) {
        const std::string shown = active_tab == tab
            ? "[" + std::string(label) + "]" : std::string(label);
        if (ui->button(ui->user, anomaly::sdk::StringView(shown), 0.0F, 0.0F))
            active_tab = tab;
        DrawInlineGap(ui);
    };
    nav_button(g_context.localizer.Text("action.tab_customization", "Customize vehicles"), 0);
    nav_button(g_context.localizer.Text("action.tab_performance", "Performance tuning"), 1);
    nav_button(g_context.localizer.Text("action.tab_chassis", "Wheel friction"), 2);

    nav_button(g_context.localizer.Text("action.tab_diagnostics", "Diagnostics"), 3);
    const auto refresh_label =
        g_context.localizer.Text("action.refresh_catalog", "Refresh vehicle list");
    if (ui->button(ui->user, anomaly::sdk::StringView(refresh_label), 0.0F, 0.0F)) {
        g_context.refresh_catalog_requested.store(true, std::memory_order_release);
        SetStatus(g_context.localizer.Text("status.catalog_refresh_requested",
            "Vehicle catalog refresh requested."));
    }
    DrawDivider(ui);

    if (active_tab == 0) {
        DrawCustomizationTab(ui, selected, ids, labels, customization_options,
            module_overrides, customization_vehicle_id, customization_skin_id,
            customization_status, customization_status_failure, last_summon,
            player_position_valid);
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
        SetStatus(StatusDescription(
            g_context.localizer.Text("status.menu_window_open_failed",
                "Opening the vehicle menu window failed"),
            begin_status));
        return;
    }
    if (visible != 0) DrawContents(ui);
    const auto end_status = g_context.window_service->end(
        g_context.window_service->user, g_context.menu_window_handle);
    if (end_status.code != ANOMALY_STATUS_V1_OK) {
        SetStatus(StatusDescription(
            g_context.localizer.Text("status.menu_window_close_failed",
                "Finishing the vehicle menu window failed"),
            end_status));
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) try {
    if (!host || !plugin_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.localizer = anomaly::sdk::Localizer(host);
    if (!nte_vehicle::LoadBackend(host, g_context.localizer))
        return FailureStatus(ANOMALY_STATUS_V1_UNAVAILABLE, "status.backend_unavailable",
            "Vehicle compatibility profile or required engine services are unavailable.");
    auto* vehicle = nte_vehicle::Backend();
    const auto* player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    const auto* input = QueryService<AnomalyInputServiceV1>(
        host, ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    const auto* window_service = QueryService<AnomalyWindowServiceV1>(
        host, ANOMALY_WINDOW_SERVICE_V1_ID, ANOMALY_WINDOW_SERVICE_V1_VERSION);
    const auto* plugin_state = QueryService<AnomalyPluginStateServiceV1>(
        host, ANOMALY_PLUGIN_STATE_SERVICE_V1_ID, ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION);
    if (!vehicle || !player || !InputReady(input) || !WindowReady(window_service)) {
        return FailureStatus(ANOMALY_STATUS_V1_UNAVAILABLE, "status.services_missing",
            "The vehicle menu requires the anomaly.input and anomaly.window services.");
    }

    AnomalyWindowSpecV1 window_spec{};
    window_spec.struct_size = sizeof(window_spec);
    window_spec.flags = ANOMALY_WINDOW_V1_NO_COLLAPSE;
    window_spec.id = anomaly::sdk::StringView("nte-vehicle-independent-menu");
    const std::string window_title =
        g_context.localizer.Text("window.title", "Vehicle Control");
    window_spec.title = anomaly::sdk::StringView(window_title);
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
        return FailureStatus(ANOMALY_STATUS_V1_UNAVAILABLE, "status.window_register_failed",
            "Could not register the standalone vehicle menu window.");
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
    g_context.snapshot.vehicle_snapshot_status = g_context.localizer.Text(
        "status.snapshot_pending",
        "The current driving vehicle state has not been read yet.");
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
    g_context.customization_status = g_context.localizer.Text(
        "status.customization_not_loaded",
        "The customization data of the current vehicle has not been read yet.");
    g_context.customization_status_failure = false;
    g_context.customization_options.clear();
    g_context.selected_module_overrides.clear();
    g_context.customization_catalog_tick = 0;
    g_context.last_summon.clear();
    g_context.status = g_context.localizer.Text("status.catalog_reading",
        "Reading and validating the runtime vehicle catalog.");
    g_context.player_position_valid = false;
    g_context.player_snapshot = {};
    g_context.catalog_tick = 0;
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
} catch (...) { return Status(ANOMALY_STATUS_V1_FAILED); }

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) try {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (!InputReady(g_context.input) || !WindowReady(g_context.window_service) ||
        g_context.menu_window_handle.id == 0) {
        return FailureStatus(ANOMALY_STATUS_V1_UNAVAILABLE, "status.window_unavailable",
            "The vehicle menu window or the input service is unavailable.");
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
        return FailureStatus(hotkey_status.code == ANOMALY_STATUS_V1_OK
                ? ANOMALY_STATUS_V1_FAILED : hotkey_status.code,
            "status.hotkey_register_failed",
            "Could not register the K menu hotkey.");
    }

    g_context.toggle_menu_requested.store(false, std::memory_order_release);
    static_cast<void>(g_context.window_service->set_open(
        g_context.window_service->user, g_context.menu_window_handle, 0));
    g_context.refresh_catalog_requested.store(true, std::memory_order_release);
    g_context.apply_speed.store(true, std::memory_order_release);
    g_context.started.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
} catch (...) { return Status(ANOMALY_STATUS_V1_FAILED); }

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) try {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(false, std::memory_order_release);
    g_context.toggle_menu_requested.store(false, std::memory_order_release);
    nte_vehicle::StopBackend();
    SaveSelection(g_context.saved_vehicle_id);
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
} catch (...) { return Status(ANOMALY_STATUS_V1_FAILED); }

void ANOMALY_CALL Unload(void* plugin_context) try {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    g_context.toggle_menu_requested.store(false, std::memory_order_release);
    nte_vehicle::StopBackend();
    SaveSelection(g_context.saved_vehicle_id);
    ReleaseMenuHotkey(g_context);
    if (g_context.window_service && g_context.menu_window_handle.id != 0) {
        static_cast<void>(g_context.window_service->release_window(
            g_context.window_service->user, g_context.menu_window_handle));
    }
    g_context.menu_window_handle = {};
    nte_vehicle::UnloadBackend();
    g_context.vehicle = nullptr;
    g_context.player = nullptr;
    g_context.input = nullptr;
    g_context.window_service = nullptr;
} catch (...) {  }

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) try {
    if (plugin_context == &g_context && g_context.started.load(std::memory_order_acquire)) {
        nte_vehicle::TickBackend();
        Update();
    }
} catch (...) {  }

void ANOMALY_CALL DrawCallback(void* plugin_context, const AnomalyUiServiceV1* ui) try {
    if (plugin_context == &g_context) Draw(ui);
} catch (...) {  }

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.nte-vehicle"),
        anomaly::sdk::StringView("Vehicle Control"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.11.1"), Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}
