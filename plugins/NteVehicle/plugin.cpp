#include "backend.hpp"
#include "durability_guard.hpp"
#include "runtime.hpp"
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ui_resources.h"
#include "anomaly/sdk/services/plugin_state.h"
#include "anomaly/sdk/localization.hpp"
#include "../common/config_save_queue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyInputServiceV1* input{};
    const AnomalyWindowServiceV1* window_service{};
    const AnomalyPluginStateServiceV1* plugin_state{};
    const AnomalyConfigServiceV1* config{};
    AnomalyGenerationHandleV1 settings_schema{};
    anomaly::sdk::Localizer localizer;
    AnomalyGenerationHandleV1 menu_window_handle{};
    AnomalyGenerationHandleV1 menu_hotkey_handle{};
    std::atomic_bool toggle_menu_requested{};
    AnomalyNtePlayerSnapshotV1 player_snapshot{};
    bool player_position_valid{};
    std::mutex mutex;
    Snapshot snapshot{};
    bool local_world_allowed{};
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
    std::atomic_bool apply_speed_target_requested{};
    std::atomic_bool apply_speed{};
    std::atomic_bool apply_torque{};
    std::atomic_bool apply_other_speed{};
    std::atomic_bool apply_other_torque{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool summon_customized{};
    std::atomic_bool friction_toggle{};
    // Game owns the guard switch; settings snapshots are written on Worker/Lifecycle.
    bool durability_guard_enabled{true};
    bool durability_guard_installed{};
    std::string durability_guard_detail;
    std::atomic_bool durability_guard_toggle{};
    std::atomic_bool settings_dirty{};
    // 玩家服务在游戏世界就绪后才发布，未就绪时每 60 个 Game tick 重查一次。
    std::uint32_t service_retry_ticks{};
    std::atomic_bool started{};
    std::atomic_uint32_t durability_last_status_code{ANOMALY_STATUS_V1_NOT_FOUND};
    float speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    float other_speed_ratio{1.0F};
    float other_speed_floor_ratio{0.6024F};
    float other_torque_ratio{1.0F};
    bool friction_enabled{true};

    // ==== Local feature (community port): per-vehicle memory / live tuning ====
    // Per-vehicle customization memory: skin + module overrides recorded per
    // catalog ID, restored when the customization catalog loads and when the
    // vehicle is summoned. Persisted as customization-memory.cfg (V/M/H lines).
    struct SavedCustomization {
        std::string skin_id;
        std::vector<std::pair<std::uint32_t, std::string>> module_overrides;
    };
    std::map<std::string, SavedCustomization> saved_customizations;
    // Per-vehicle summon hotkeys (virtual key -> catalog ID), shared file with
    // the customization memory (H lines).
    std::map<std::uint32_t, std::string> hotkey_vehicle_bindings;
    bool hotkey_binding_mode{};
    std::string hotkey_summon_vehicle_id;
    std::array<std::uint8_t, 256> prev_key_state{};
    std::atomic_bool hotkey_summon_requested{};
    // Per-vehicle advanced tuning memory: 16 floats per catalog ID, persisted
    // as tuning-memory.cfg (T2 lines). See LoadTuningMemory for the layout.
    std::map<std::string, std::array<float, 16>> saved_tunes;
    // Per-vehicle factory transmission baselines (maintainer review: baselines
    // must never cross vehicles). [0..4] normal up/down/time/ratio/efficiency,
    // [5..9] comfort group; a negative [0] means "not captured yet".
    std::map<std::string, std::array<float, 10>> tuning_baselines;
    // Maintainer review fix: file writes happen on a plugin-owned worker
    // thread; Game/Render callbacks only queue payloads and mark dirty flags.
    std::thread save_worker;
    std::atomic_bool save_worker_exit{};
    std::mutex pending_writes_mutex;
    std::vector<std::pair<std::string, std::string>> pending_file_writes;
    std::atomic_bool pending_writes_present{};
    std::atomic_bool export_modules_requested{};
    // Dirty flags: config/memory file writes only happen in the lifecycle
    // callbacks (Load/Stop/Unload), never inside Game/Render callbacks.
    std::atomic_bool memory_dirty{};
    std::atomic_bool tuning_dirty{};
    std::atomic_bool drift_dirty{};
    std::atomic_bool extras_dirty{};
    std::atomic_bool durability_auto_full_requested{};
    bool durability_auto_full{true};
    // Live (driven-vehicle) customization.
    std::atomic_bool apply_current_modules_requested{};
    std::atomic_bool apply_skin_requested{};
    std::atomic_bool read_modules_requested{};
    std::atomic_bool realtime_sync_requested{};
    std::atomic_bool save_modules_to_memory_requested{};
    std::atomic_bool capture_effects_requested{};
    std::string realtime_vehicle_id;
    std::string realtime_skin_id;
    std::vector<std::pair<std::uint32_t, std::string>> current_vehicle_modules;
    bool current_vehicle_modules_valid{};
    std::vector<std::pair<std::uint32_t, std::string>> allowed_extra_modules;
    // Advanced powertrain tuning. The shared MaxLinearVelocity write is kept
    // disabled (-1 = skip): the game clamps it server-side around 349 km/h, so
    // the ratio path covers the real cap. Memory entries may still carry it.
    std::uintptr_t adv_synced_movement{};
    float adv_stock_top_speed_kmh{};
    bool adv_stock_top_speed_valid{};
    float adv_speed_target_kmh{0.0F};
    float adv_linear_top_speed{-1.0F};
    float adv_reverse_top_speed{-1.0F};
    float adv_change_up_rpm{-1.0F};
    float adv_down_rpm{-1.0F};
    float adv_gear_change_time{-1.0F};
    float adv_final_ratio{-1.0F};
    float adv_transmission_efficiency{-1.0F};
    float adv_c_up_rpm{-1.0F};
    float adv_c_down_rpm{-1.0F};
    float adv_c_gear_change_time{-1.0F};
    float adv_c_final_ratio{-1.0F};
    float adv_c_transmission_efficiency{-1.0F};
    bool adv_comfort_mode{};
    float adv_live_linear_kmh{};
    float adv_live_reverse_kmh{};
    float adv_live_up_rpm{};
    float adv_live_down_rpm{};
    float adv_live_gear_time{};
    float adv_live_final_ratio{};
    float adv_live_efficiency{};
    float adv_live_c_up_rpm{};
    float adv_live_c_down_rpm{};
    float adv_live_c_gear_time{};
    float adv_live_c_final_ratio{};
    float adv_live_c_efficiency{};
    float adv_baseline_up_rpm{-1.0F};
    float adv_baseline_down_rpm{-1.0F};
    float adv_baseline_gear_time{-1.0F};
    float adv_baseline_final_ratio{-1.0F};
    float adv_baseline_efficiency{-1.0F};
    float adv_baseline_c_up_rpm{-1.0F};
    float adv_baseline_c_down_rpm{-1.0F};
    float adv_baseline_c_gear_time{-1.0F};
    float adv_baseline_c_final_ratio{-1.0F};
    float adv_baseline_c_efficiency{-1.0F};
    bool adv_baseline_valid{};
    std::atomic_bool adv_apply_requested{};
    std::atomic_bool adv_read_requested{};
    std::atomic_bool save_tune_requested{};
    std::atomic_bool reset_tune_requested{};
    // Drift smoke/track rotation, global across vehicles (press Left Shift).
    bool drift_smoke_on{};
    bool drift_mark_on{};
    std::vector<std::string> drift_smoke_scope;
    std::vector<std::string> drift_mark_scope;
    std::size_t drift_smoke_index{};
    std::size_t drift_mark_index{};
    std::uint8_t drift_prev_shift{};
    bool drift_smoke_random{};
    bool drift_mark_random{};
    bool drift_smoke_ui_open{};
    bool drift_mark_ui_open{};
    // Diagnostics log ring + export.
    std::atomic_bool export_log_requested{};
    std::vector<std::string> log_lines;
    std::atomic_bool probe_requested{};
    std::atomic_bool sweep_requested{};
} g_context;

anomaly::plugins::ConfigSaveQueue g_settings_save;

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

std::string HexAddress(std::uintptr_t value) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text("0x");
    std::string digits;
    do {
        digits.push_back(kDigits[value & 0xFU]);
        value >>= 4U;
    } while (value != 0);
    text.append(digits.rbegin(), digits.rend());
    return text;
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

// ---- Local feature (community port): plugin-state files --------------------
// Per-vehicle memory and feature configuration live in the plugin state
// directory next to vehicle-selection.cfg. All writes happen in the lifecycle
// callbacks (Load/Stop/Unload) behind dirty flags — Game/Render callbacks only
// mutate the in-memory state and mark the matching flag.
std::string StatePath(const std::string_view file_name) {
    if (g_context.state_directory.empty()) return {};
    std::string path = g_context.state_directory;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path.append(file_name);
    return path;
}

std::string CustomizationMemoryPath() { return StatePath("customization-memory.cfg"); }
std::string TuningMemoryPath() { return StatePath("tuning-memory.cfg"); }
std::string DriftConfigPath() { return StatePath("drift-config.cfg"); }
std::string AllowedExtraModulesPath() { return StatePath("allowed-extra-modules.cfg"); }

std::string TrimLine(std::string line) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' ||
            line.back() == '\t')) {
        line.pop_back();
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.erase(line.begin());
    }
    return line;
}

std::vector<std::string> SplitLine(const std::string& line, const char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t pos = line.find(separator, begin);
        if (pos == std::string::npos) {
            parts.push_back(line.substr(begin));
            break;
        }
        parts.push_back(line.substr(begin, pos - begin));
        begin = pos + 1;
    }
    return parts;
}

// customization-memory.cfg lines:
//   V|<vehicle_id>|<skin_id or empty>            remembered skin
//   M|<vehicle_id>|<module_type>:<module_id>     remembered module override
//   H|<vehicle_id>|<virtual_key>                 per-vehicle summon hotkey
// module_id may carry an explicit "#N" FName-number suffix.
void LoadCustomizationMemory() {
    const std::string path = CustomizationMemoryPath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = TrimLine(std::move(line));
        if (line.size() < 4) continue;
        const auto parts = SplitLine(line, '|');
        if (parts.size() < 3 || parts[1].empty()) continue;
        if (parts[0] == "V") {
            g_context.saved_customizations[parts[1]].skin_id = parts[2];
        } else if (parts[0] == "M") {
            const auto entry = SplitLine(parts[2], ':');
            if (entry.size() != 2 || entry[0].empty() || entry[1].empty()) continue;
            std::uint32_t module_type = 0;
            try {
                module_type = static_cast<std::uint32_t>(std::stoul(entry[0]));
            } catch (...) { continue; }
            if (module_type == 0 || module_type >= 28) continue;
            g_context.saved_customizations[parts[1]].module_overrides.emplace_back(
                module_type, entry[1]);
        } else if (parts[0] == "H") {
            std::uint32_t key = 0;
            try {
                key = static_cast<std::uint32_t>(std::stoul(parts[2]));
            } catch (...) { continue; }
            if (key != 0) g_context.hotkey_vehicle_bindings[key] = parts[1];
        }
    }
}

void SaveCustomizationMemory() {
    const std::string path = CustomizationMemoryPath();
    if (path.empty()) return;
    // Worker thread: snapshot the shared state under the plugin mutex first,
    // then write the file without holding any lock.
    std::map<std::string, Context::SavedCustomization> saved;
    std::map<std::uint32_t, std::string> hotkeys;
    {
        std::scoped_lock lock(g_context.mutex);
        saved = g_context.saved_customizations;
        hotkeys = g_context.hotkey_vehicle_bindings;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& [vehicle_id, saved_entry] : saved) {
        if (!saved_entry.skin_id.empty()) out << "V|" << vehicle_id << "|" << saved_entry.skin_id << "\n";
        for (const auto& entry : saved_entry.module_overrides) {
            out << "M|" << vehicle_id << "|" << entry.first << ":" << entry.second << "\n";
        }
    }
    for (const auto& [key, vehicle_id] : hotkeys) {
        out << "H|" << vehicle_id << "|" << key << "\n";
    }
}

// tuning-memory.cfg lines (T2): 16 floats per vehicle.
//   [0] MaxLinearVelocity km/h   [1] normal shift-up RPM  [2] normal shift-down RPM
//   [3] normal gear time         [4] normal final ratio   [5] normal efficiency
//   [6] comfort shift-up RPM     [7] comfort shift-down   [8] comfort gear time
//   [9] comfort final ratio      [10] comfort efficiency  [11] speed ratio
//   [12] torque ratio            [13] other speed ratio   [14] other torque ratio
//   [15] reverse top speed km/h
// A value of -1 means "not set / leave unchanged". Older 5- and 13-float
// layouts (T| and short T2|) are still accepted.
void LoadTuningMemory() {
    const std::string path = TuningMemoryPath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = TrimLine(std::move(line));
        if (line.size() < 6) continue;
        const auto parts = SplitLine(line, '|');
        if (parts.size() < 3 || parts[1].empty()) continue;
        const bool legacy_t = parts[0] == "T";
        if (!legacy_t && parts[0] != "T2") continue;
        std::array<float, 16> tune{};
        tune.fill(-1.0F);
        std::size_t count = 0;
        bool valid = true;
        for (std::size_t i = 2; i < parts.size() && count < tune.size(); ++i) {
            if (parts[i].empty()) continue;
            try {
                tune[count++] = std::stof(parts[i]);
            } catch (...) { valid = false; break; }
        }
        if (!valid || count == 0) continue;
        if (legacy_t && count == 5) {
            // v1 layout: normal transmission group only; shift the slots into
            // the T2 positions ([1]..[5]).
            std::array<float, 16> mapped{};
            mapped.fill(-1.0F);
            for (std::size_t i = 0; i < 5; ++i) mapped[1 + i] = tune[i];
            tune = mapped;
        } else if (!legacy_t && count == 13) {
            // Short T2 layout without the legacy speed/torque slots.
            std::array<float, 16> mapped{};
            mapped.fill(-1.0F);
            for (std::size_t i = 0; i < 11; ++i) mapped[i] = tune[i];
            mapped[15] = tune[11];
            tune = mapped;
        }
        g_context.saved_tunes[parts[1]] = tune;
    }
}

void SaveTuningMemory() {
    const std::string path = TuningMemoryPath();
    if (path.empty()) return;
    std::map<std::string, std::array<float, 16>> saved;
    {
        std::scoped_lock lock(g_context.mutex);
        saved = g_context.saved_tunes;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& [vehicle_id, tune] : saved) {
        out << "T2|" << vehicle_id;
        for (const float value : tune) out << "|" << value;
        out << "\n";
    }
}

// drift-config.cfg lines:
//   S|<on 0/1>|<random 0/1>|<tier1,tier2,...>   tyre smoke rotation
//   M|<on 0/1>|<random 0/1>|<tier1,tier2,...>   tyre track rotation
// An empty tier list means "all tiers participate". Older files without the
// random column are accepted.
void LoadDriftConfig() {
    const std::string path = DriftConfigPath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = TrimLine(std::move(line));
        if (line.size() < 5) continue;
        const auto parts = SplitLine(line, '|');
        if (parts.empty()) continue;
        const bool smoke = parts[0] == "S";
        const bool mark = parts[0] == "M";
        if (!smoke && !mark) continue;
        if (parts.size() < 2) continue;
        const bool enabled = parts[1] == "1";
        const bool random_mode = parts.size() >= 3 && parts[2] == "1";
        std::vector<std::string> tiers;
        if (parts.size() >= 4 && !parts[3].empty()) {
            for (auto& tier : SplitLine(parts[3], ',')) {
                tier = TrimLine(std::move(tier));
                if (!tier.empty()) tiers.push_back(std::move(tier));
            }
        }
        if (smoke) {
            g_context.drift_smoke_on = enabled;
            g_context.drift_smoke_random = random_mode;
            g_context.drift_smoke_scope = std::move(tiers);
        } else {
            g_context.drift_mark_on = enabled;
            g_context.drift_mark_random = random_mode;
            g_context.drift_mark_scope = std::move(tiers);
        }
    }
}

void SaveDriftConfig() {
    const std::string path = DriftConfigPath();
    if (path.empty()) return;
    bool smoke_on, mark_on, smoke_random, mark_random;
    std::vector<std::string> smoke_scope, mark_scope;
    {
        std::scoped_lock lock(g_context.mutex);
        smoke_on = g_context.drift_smoke_on;
        mark_on = g_context.drift_mark_on;
        smoke_random = g_context.drift_smoke_random;
        mark_random = g_context.drift_mark_random;
        smoke_scope = g_context.drift_smoke_scope;
        mark_scope = g_context.drift_mark_scope;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    const auto write_line = [&](const char kind, const bool enabled,
                                const bool random_mode,
                                const std::vector<std::string>& tiers) {
        out << kind << "|" << (enabled ? 1 : 0) << "|" << (random_mode ? 1 : 0) << "|";
        for (std::size_t i = 0; i < tiers.size(); ++i) {
            if (i != 0) out << ",";
            out << tiers[i];
        }
        out << "\n";
    };
    write_line('S', smoke_on, smoke_random, smoke_scope);
    write_line('M', mark_on, mark_random, mark_scope);
}

// allowed-extra-modules.cfg lines: <module_type>|<module_id[#N]>.
// Whitelisted effect parts (e.g. cloud-applied premium wheel effects) can be
// selected, rotated and summoned even though the local catalog lacks them.
void LoadAllowedExtraModules() {
    const std::string path = AllowedExtraModulesPath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::vector<std::pair<std::uint32_t, std::string>> entries;
    std::string line;
    while (std::getline(in, line)) {
        line = TrimLine(std::move(line));
        if (line.size() < 3) continue;
        const auto parts = SplitLine(line, '|');
        if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) continue;
        std::uint32_t module_type = 0;
        try {
            module_type = static_cast<std::uint32_t>(std::stoul(parts[0]));
        } catch (...) { continue; }
        if (module_type == 0 || module_type >= 28) continue;
        entries.emplace_back(module_type, parts[1]);
    }
    g_context.allowed_extra_modules = std::move(entries);
    if (g_context.vehicle != nullptr) {
        g_context.vehicle->SetAllowedExtraModules(g_context.allowed_extra_modules);
    }
}

void SaveAllowedExtraModules() {
    const std::string path = AllowedExtraModulesPath();
    if (path.empty()) return;
    std::vector<std::pair<std::uint32_t, std::string>> entries;
    {
        std::scoped_lock lock(g_context.mutex);
        entries = g_context.allowed_extra_modules;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& entry : entries) {
        out << entry.first << "|" << entry.second << "\n";
    }
}

// Persist every dirty plugin-state file. Lifecycle callbacks only.
void SaveAllPluginState() {
    if (g_context.memory_dirty.exchange(false, std::memory_order_acq_rel)) {
        SaveCustomizationMemory();
    }
    if (g_context.tuning_dirty.exchange(false, std::memory_order_acq_rel)) {
        SaveTuningMemory();
    }
    if (g_context.drift_dirty.exchange(false, std::memory_order_acq_rel)) {
        SaveDriftConfig();
    }
    if (g_context.extras_dirty.exchange(false, std::memory_order_acq_rel)) {
        SaveAllowedExtraModules();
    }
}

// Maintainer review fix: Game/Render callbacks never touch the filesystem.
// They queue (path, content) pairs and mark dirty flags; the plugin-owned
// worker thread drains the queue and persists the state files.
bool WriteTextFileAtomic(const std::string& path, const std::string& content);

void QueueFileWrite(std::string path, std::string content) {
    if (path.empty()) return;
    {
        std::scoped_lock lock(g_context.pending_writes_mutex);
        g_context.pending_file_writes.emplace_back(std::move(path), std::move(content));
    }
    g_context.pending_writes_present.store(true, std::memory_order_release);
}

bool AnyStateDirty() {
    return g_context.memory_dirty.load(std::memory_order_acquire) ||
        g_context.tuning_dirty.load(std::memory_order_acquire) ||
        g_context.drift_dirty.load(std::memory_order_acquire) ||
        g_context.extras_dirty.load(std::memory_order_acquire);
}

void SaveWorkerLoop() {
    for (;;) {
        if (g_context.save_worker_exit.load(std::memory_order_acquire)) return;
        const bool has_pending = g_context.pending_writes_present.exchange(
            false, std::memory_order_acq_rel);
        if (AnyStateDirty()) SaveAllPluginState();
        if (has_pending) {
            std::vector<std::pair<std::string, std::string>> batch;
            {
                std::scoped_lock lock(g_context.pending_writes_mutex);
                batch = std::move(g_context.pending_file_writes);
                g_context.pending_file_writes.clear();
            }
            for (const auto& [path, content] : batch) {
                WriteTextFileAtomic(path, content);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

std::string LocalStamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now) != 0) return "unknown-time";
    std::array<char, 32> buffer{};
    const std::size_t size = std::strftime(buffer.data(), buffer.size(),
        "%Y%m%d-%H%M%S", &local);
    return size != 0 ? std::string(buffer.data(), size) : std::string("unknown-time");
}

std::string StampSidecarPath(const std::string_view stem) {
    const std::string base = StatePath(stem);
    if (base.empty()) return {};
    return base + "-" + LocalStamp() + ".txt";
}

bool WriteTextFileAtomic(const std::string& path, const std::string& content) {
    if (path.empty()) return false;
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(std::filesystem::path(temporary),
        std::filesystem::path(path), error);
    if (error) {
        std::filesystem::remove(std::filesystem::path(temporary), error);
        return false;
    }
    return true;
}

// Guard settings use anomaly.config, with snapshots queued from Game and written on Worker.
constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 512;
constexpr std::string_view kSettingsSchema = R"json(
{"type":"object","additionalProperties":false,"required":["durabilityGuard"],
 "properties":{"durabilityGuard":{"type":"boolean"},
               "durabilityAutoFull":{"type":"boolean"}}}
)json";

bool ConfigReady() noexcept {
    const auto* config = g_context.config;
    return HasField<AnomalyConfigServiceV1,
               decltype(AnomalyConfigServiceV1::write_atomic)>(
               config, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        config->service_version >= ANOMALY_CONFIG_SERVICE_V1_VERSION &&
        config->register_schema != nullptr && config->read != nullptr &&
        config->write_atomic != nullptr;
}

bool ParseSettingsFlag(
    const std::string_view document, const std::string_view key_name, bool& value) noexcept {
    std::string needle = "\"" + std::string(key_name) + "\"";
    const auto key = document.find(needle);
    if (key == std::string_view::npos) return false;
    const auto colon = document.find(':', key);
    if (colon == std::string_view::npos) return false;
    std::size_t at = colon + 1;
    while (at < document.size() &&
           (document[at] == ' ' || document[at] == '\n' || document[at] == '\r' ||
            document[at] == '\t')) {
        ++at;
    }
    if (document.substr(at, 4) == "true") {
        value = true;
        return true;
    }
    if (document.substr(at, 5) == "false") {
        value = false;
        return true;
    }
    return false;
}

void LoadSettings() {
    g_context.durability_guard_enabled = true;
    g_context.durability_auto_full = true;
    g_context.settings_dirty.store(false, std::memory_order_release);
    if (!ConfigReady()) return;
    std::uint32_t schema_version{};
    std::size_t size{};
    const auto size_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    // NOT_FOUND 表示还没有保存过的文档；保持默认值即可。
    if (!anomaly::sdk::Succeeded(size_status) || size == 0 || size > kMaximumSettingsBytes) return;
    std::vector<std::uint8_t> document(size);
    std::size_t copied = document.size();
    const auto read_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {document.data(), document.size()}, &copied);
    if (!anomaly::sdk::Succeeded(read_status) || schema_version != kSettingsSchemaVersion ||
        copied == 0 || copied > document.size()) {
        return;
    }
    const std::string_view text{reinterpret_cast<const char*>(document.data()), copied};
    bool enabled = true;
    if (ParseSettingsFlag(text, "durabilityGuard", enabled)) {
        g_context.durability_guard_enabled = enabled;
    }
    bool auto_full = true;
    if (ParseSettingsFlag(text, "durabilityAutoFull", auto_full)) {
        g_context.durability_auto_full = auto_full;
    }
}

void QueueSettingsSave() {
    if (!ConfigReady() || !g_context.settings_dirty.load(std::memory_order_acquire))
        return;
    std::scoped_lock lock(g_context.mutex);
    const std::string document = std::string("{\"durabilityGuard\":") +
        (g_context.durability_guard_enabled ? "true" : "false") +
        ",\"durabilityAutoFull\":" +
        (g_context.durability_auto_full ? "true" : "false") + "}";
    if (g_settings_save.Queue(document)) g_context.settings_dirty.store(false);
}

bool RegisterSettingsSchema() {
    if (!ConfigReady()) return false;
    const auto status = g_context.config->register_schema(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion,
        {reinterpret_cast<const std::uint8_t*>(kSettingsSchema.data()), kSettingsSchema.size()},
        &g_context.settings_schema);
    return anomaly::sdk::Succeeded(status) && g_context.settings_schema.id != 0;
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

void SetStatus(std::string text);

// 安装结果写进宿主日志：界面只显示「不可用」，具体原因与地址看日志。
void LogCore(std::uint32_t level, std::string_view message) {
    const auto* core = g_context.core;
    if (core == nullptr || core->log == nullptr) return;
    core->log(core->user, level, anomaly::sdk::StringView(message));
}

// 两个入口地址由 profile 签名扫描给出，缺项时停用该功能，不回退硬编码地址。
// 安装状态与说明文本跨线程读写，统一在 mutex 下。
void InstallDurabilityGuardOnStart() {
    bool installed = false;
    std::string detail;
    std::uintptr_t take_damage = 0;
    std::uintptr_t impact = 0;
    if (g_context.vehicle == nullptr || g_context.host == nullptr) {
        detail = g_context.localizer.Text("status.durability_guard_unavailable",
            "Vehicle durability guard is unavailable: the vehicle backend is not loaded.");
        LogCore(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "durability guard: the vehicle backend is not loaded");
    } else if (!g_context.vehicle->DurabilityGuardEntries(take_damage, impact)) {
        detail = g_context.localizer.Text("status.durability_guard_unavailable",
            "Vehicle durability guard is unavailable: the compatibility profile did not resolve "
            "the collision damage entries.");
        LogCore(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "durability guard: profile entries unavailable, take_damage=" + HexAddress(take_damage) +
                ", impact=" + HexAddress(impact));
    } else {
        std::string error;
        if (nte_vehicle::InstallDurabilityGuard(g_context.host, take_damage, impact, error)) {
            installed = true;
            LogCore(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "durability guard: hooks installed, take_damage=" + HexAddress(take_damage) +
                    ", impact=" + HexAddress(impact));
        } else {
            const std::array arguments{std::string_view(error)};
            detail = g_context.localizer.Format("status.durability_guard_hook_failed",
                "Durability guard unavailable: installing the damage hook failed ({0}).",
                arguments);
            LogCore(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "durability guard: hook installation failed: " + error + ", take_damage=" +
                    HexAddress(take_damage) + ", impact=" + HexAddress(impact));
        }
    }
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.durability_guard_installed = installed;
        g_context.durability_guard_detail = detail;
    }
    nte_vehicle::SetDurabilityGuardEnabled(installed && g_context.durability_guard_enabled);
    if (!installed) SetStatus(std::move(detail));
}

void SetStatus(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.status = text;
    // Local feature: every status message also lands in a bounded log ring so
    // the diagnostics page can export the recent history for bug reports.
    std::time_t now = std::time(nullptr);
    std::tm local{};
    std::array<char, 16> stamp{};
    if (localtime_s(&local, &now) == 0 &&
        std::strftime(stamp.data(), stamp.size(), "%H:%M:%S", &local) != 0) {
        g_context.log_lines.emplace_back(std::string(stamp.data()) + "] " + text);
    } else {
        g_context.log_lines.emplace_back(std::move(text));
    }
    constexpr std::size_t kMaximumLogLines = 500;
    while (g_context.log_lines.size() > kMaximumLogLines) {
        g_context.log_lines.erase(g_context.log_lines.begin());
    }
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
             (native_option.module_type == 0 || native_option.module_type >= 28)) ||
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
        // Local feature: restore this vehicle's saved customization instead of
        // clearing the selection. Skin and module entries only restore when
        // they exist in the freshly loaded catalog or in the effects whitelist;
        // entries recorded with an explicit "#N" FName suffix match by base ID.
        const auto saved = g_context.saved_customizations.find(selected_id);
        if (saved != g_context.saved_customizations.end()) {
            const std::string& remembered_skin = saved->second.skin_id;
            if (!remembered_skin.empty() &&
                std::ranges::any_of(g_context.customization_options,
                    [&](const CustomizationOption& option) {
                        return option.kind == nte_vehicle::kCustomizationSkin &&
                            option.id == remembered_skin;
                    })) {
                g_context.customization_skin_id = remembered_skin;
            }
            std::vector<std::pair<std::uint32_t, std::string>> restored;
            for (const auto& entry : saved->second.module_overrides) {
                if (entry.first == 0 || entry.first >= 28 || entry.second.empty()) continue;
                std::string base = entry.second;
                const auto hash = base.find('#');
                if (hash != std::string::npos) base.resize(hash);
                const bool known = std::ranges::any_of(
                    g_context.customization_options,
                    [&](const CustomizationOption& option) {
                        return option.kind == nte_vehicle::kCustomizationModule &&
                            option.module_type == entry.first &&
                            (option.id == entry.second || option.id == base);
                    }) || std::ranges::any_of(
                    g_context.allowed_extra_modules,
                    [&](const std::pair<std::uint32_t, std::string>& extra) {
                        std::string extra_base = extra.second;
                        const auto extra_hash = extra_base.find('#');
                        if (extra_hash != std::string::npos) extra_base.resize(extra_hash);
                        return extra.first == entry.first &&
                            (extra.second == entry.second || extra_base == base);
                    });
                if (known) restored.emplace_back(entry);
            }
            g_context.selected_module_overrides = std::move(restored);
        } else {
            g_context.selected_module_overrides.clear();
        }
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

// ---- Local feature (community port): live helpers --------------------------

// Module IDs carry the vehicle prefix (V026_* belongs to Vehicle026). Derive
// the prefix from a vehicle id so whitelist entries of other vehicles never
// render or apply on the wrong car.
std::string VehicleModulePrefixFromId(const std::string& vehicle_id) {
    const std::string_view id{vehicle_id};
    if (id.size() <= 7) return {};
    static constexpr std::string_view kVehicleTag{"vehicle"};
    const auto lower = [](const char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    for (std::size_t i = 0; i < kVehicleTag.size(); ++i) {
        if (lower(id[i]) != kVehicleTag[i]) return {};
    }
    std::string prefix = "V";
    prefix.append(id.substr(kVehicleTag.size()));
    return prefix;
}

bool WhitelistExtraForVehicle(const std::string& extra_id, const std::string& prefix) {
    if (prefix.empty()) return true;
    return extra_id.size() > prefix.size() + 1 &&
        extra_id.compare(0, prefix.size(), prefix) == 0 &&
        extra_id[prefix.size()] == '_';
}

// Only premium-effect whitelist entries render as named buttons; ordinary
// whitelisted parts are memory-backfill-only and never clutter option lists.
std::string WhitelistExtraEffectDisplay(const std::string& extra_id) {
    std::string base = extra_id;
    const auto hash_pos = base.find('#');
    if (hash_pos != std::string::npos) base.resize(hash_pos);
    if (base.find("WheelSmoke_10") != std::string::npos) {
        return g_context.localizer.Text("label.effect_neon_smoke", "Neon Realm smoke");
    }
    if (base.find("WheelMark_10") != std::string::npos) {
        return g_context.localizer.Text("label.effect_flame_mark", "Realistic Flame mark");
    }
    return {};
}

// Local feature: convert a virtual-key code to the localized key name the user
// sees on the keyboard (e.g. 0x48 -> "H"), with extended-key handling.
std::string VirtualKeyDisplayName(std::uint32_t vk) {
    const UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    if (scan == 0) return std::to_string(vk);
    LONG parameter = static_cast<LONG>(scan) << 16;
    switch (vk) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT:
            parameter |= 1L << 24;
            break;
        default:
            break;
    }
    wchar_t name[64]{};
    if (GetKeyNameTextW(parameter, name, 63) == 0) return std::to_string(vk);
    const int utf8_size = WideCharToMultiByte(
        CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 1) return std::to_string(vk);
    std::string text(static_cast<std::size_t>(utf8_size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, -1, text.data(), utf8_size, nullptr, nullptr);
    return text;
}

// Candidate display name: catalog label first, official effect display name for
// whitelisted premiums, raw ID (without the #number) as the last resort.
std::string CandidateDisplayName(const std::string& id) {
    std::string base = id;
    const auto hash_pos = base.find('#');
    if (hash_pos != std::string::npos) base.resize(hash_pos);
    const std::string effect = WhitelistExtraEffectDisplay(id);
    if (!effect.empty()) return effect;
    std::scoped_lock lock(g_context.mutex);
    for (const auto& option : g_context.customization_options) {
        if (option.kind == nte_vehicle::kCustomizationModule &&
            (option.id == id || option.id == base)) {
            return option.label.empty() ? option.id : option.label;
        }
    }
    return base;
}

// Candidate tier pattern: the tier segment starting at "Wheel" (e.g.
// "WheelSmoke_10"), shared across vehicles so one global scope selection
// fits every car's own option IDs.
std::string CandidateTierPattern(const std::string& id) {
    std::string base = id;
    const auto hash_pos = base.find('#');
    if (hash_pos != std::string::npos) base.resize(hash_pos);
    const auto wheel_pos = base.find("Wheel");
    if (wheel_pos == std::string::npos) return base;
    return base.substr(wheel_pos);
}

// Per-vehicle summon hotkeys: scan rising edges of the bound virtual keys.
// The binding mode captures the next key press for the currently selected
// vehicle. Runs on the Game thread inside Update.
void CheckHotkeyKeys() {
    if (!g_context.started.load(std::memory_order_acquire)) return;
    for (std::uint32_t vk = 0x08U; vk < 0xFEU; ++vk) {
        const SHORT state = GetAsyncKeyState(static_cast<int>(vk));
        const bool pressed = (state & 0x8000) != 0;
        const bool was = g_context.prev_key_state[vk & 0xFFU] != 0;
        g_context.prev_key_state[vk & 0xFFU] = pressed ? 1U : 0U;
        if (!pressed || was) continue;
        if (g_context.hotkey_binding_mode) {
            // Bind whatever vehicle is currently selected to this key.
            std::string selected;
            {
                std::scoped_lock lock(g_context.mutex);
                selected = g_context.selected_vehicle_id;
            }
            if (!selected.empty()) {
                std::scoped_lock lock(g_context.mutex);
                g_context.hotkey_vehicle_bindings[vk] = selected;
                const std::string key_text = std::to_string(vk);
                const std::array arguments{std::string_view(key_text)};
                g_context.status = g_context.localizer.Format("status.hotkey_bound",
                    "Bound summon hotkey (key {0}) to this vehicle.", arguments);
            }
            g_context.hotkey_binding_mode = false;
            g_context.memory_dirty.store(true, std::memory_order_release);
            continue;
        }
        std::string bound;
        {
            std::scoped_lock lock(g_context.mutex);
            const auto found = g_context.hotkey_vehicle_bindings.find(vk);
            if (found != g_context.hotkey_vehicle_bindings.end()) bound = found->second;
        }
        if (bound.empty()) continue;
        std::string selected;
        {
            std::scoped_lock lock(g_context.mutex);
            selected = g_context.selected_vehicle_id;
        }
        if (bound == selected) {
            g_context.hotkey_summon_vehicle_id = std::move(bound);
            g_context.hotkey_summon_requested.store(true, std::memory_order_release);
        } else {
            // Different vehicle: switch the selection first; press again to summon.
            std::scoped_lock lock(g_context.mutex);
            g_context.pending_vehicle_id = std::move(bound);
        }
    }
}

// Drift smoke/track rotation: Left Shift rising edge rotates the whitelisted
// or catalog tyre-effect slots and applies them to the driven vehicle.
void CheckDriftRotation() {
    if (!g_context.started.load(std::memory_order_acquire)) return;
    const SHORT shift_state = GetAsyncKeyState(VK_SHIFT);
    const bool pressed = (shift_state & 0x8000) != 0;
    const bool was = (g_context.drift_prev_shift & 0x80) != 0;
    g_context.drift_prev_shift = pressed ? 0x80 : 0x00;
    if (!pressed || was || (!g_context.drift_smoke_on && !g_context.drift_mark_on)) return;
    std::string applied_smoke;
    std::string applied_mark;
    {
        std::scoped_lock lock(g_context.mutex);
        const auto build_candidates = [&](const std::uint32_t type,
                                          std::vector<std::string>& out) {
            for (const auto& option : g_context.customization_options) {
                if (option.kind == nte_vehicle::kCustomizationModule &&
                    option.module_type == type) {
                    out.push_back(option.id);
                }
            }
            const std::string key = !g_context.realtime_vehicle_id.empty()
                ? g_context.realtime_vehicle_id
                : g_context.selected_vehicle_id;
            const std::string prefix = VehicleModulePrefixFromId(key);
            for (const auto& extra : g_context.allowed_extra_modules) {
                if (extra.first != type) continue;
                if (!WhitelistExtraForVehicle(extra.second, prefix)) continue;
                std::string base = extra.second;
                const auto hash_pos = base.find('#');
                if (hash_pos != std::string::npos) base.resize(hash_pos);
                bool listed = false;
                for (const auto& id : out) {
                    if (id == base) {
                        listed = true;
                        break;
                    }
                }
                if (!listed) out.push_back(extra.second);
            }
        };
        // Global scope: checked tier patterns fit every car's own IDs; an empty
        // scope means every candidate participates.
        const auto rotate_type = [&](const std::uint32_t type, const bool enabled,
                                     const bool random_mode,
                                     const std::vector<std::string>& scope,
                                     std::size_t& index, std::string& applied) {
            if (!enabled) return;
            std::vector<std::string> candidates;
            build_candidates(type, candidates);
            if (candidates.empty()) return;
            std::vector<std::string> rotation;
            for (const auto& id : candidates) {
                if (scope.empty()) {
                    rotation.push_back(id);
                    continue;
                }
                const std::string pattern = CandidateTierPattern(id);
                for (const auto& checked : scope) {
                    if (checked == pattern) {
                        rotation.push_back(id);
                        break;
                    }
                }
            }
            if (rotation.empty()) return;
            std::string current;
            for (const auto& entry : g_context.selected_module_overrides) {
                if (entry.first == type) {
                    current = entry.second;
                    break;
                }
            }
            // Rotation mode: sequential (loop from the equipped item) or pure
            // random (uniform, avoiding immediate repeats when possible).
            std::size_t pick = 0;
            if (random_mode) {
                static std::mt19937 random_engine{std::random_device{}()};
                std::uniform_int_distribution<std::size_t> pick_distribution(
                    0, rotation.size() - 1);
                pick = pick_distribution(random_engine);
                if (rotation.size() > 1 && rotation[pick] == current) {
                    pick = pick_distribution(random_engine);
                }
            } else {
                std::size_t pos = index % rotation.size();
                if (!current.empty()) {
                    for (std::size_t k = 0; k < rotation.size(); ++k) {
                        if (rotation[k] == current) {
                            pos = (k + 1) % rotation.size();
                            break;
                        }
                    }
                }
                pick = pos;
                index = (pos + 1) % rotation.size();
            }
            const std::string next_id = rotation[pick];
            auto& overrides = g_context.selected_module_overrides;
            const auto found = std::ranges::find_if(overrides,
                [type](const auto& entry) { return entry.first == type; });
            if (found == overrides.end()) {
                overrides.emplace_back(type, next_id);
            } else {
                found->second = next_id;
            }
            applied = next_id;
        };
        rotate_type(23, g_context.drift_smoke_on, g_context.drift_smoke_random,
            g_context.drift_smoke_scope, g_context.drift_smoke_index, applied_smoke);
        rotate_type(24, g_context.drift_mark_on, g_context.drift_mark_random,
            g_context.drift_mark_scope, g_context.drift_mark_index, applied_mark);
        g_context.memory_dirty.store(true, std::memory_order_release);
    }
    if (!applied_smoke.empty() || !applied_mark.empty()) {
        g_context.apply_current_modules_requested.store(true, std::memory_order_release);
        const std::string smoke_name = applied_smoke.empty()
            ? std::string("-") : CandidateDisplayName(applied_smoke);
        const std::string mark_name = applied_mark.empty()
            ? std::string("-") : CandidateDisplayName(applied_mark);
        const std::array arguments{
            std::string_view(smoke_name), std::string_view(mark_name)};
        SetStatus(g_context.localizer.Format("status.drift_applied",
            "Drift rotation -> smoke: {0} / mark: {1}", arguments));
    }
}

// Per-tick driven-vehicle adoption: read the advanced tuning (live slider
// values), detect boarding a different vehicle by its movement component, and
// on change capture the factory top speed, sync the plugin vehicle selection
// and queue the live module list read.
void AdoptDrivenVehicle() {
    if (!g_context.started.load(std::memory_order_acquire)) return;
    auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr) return;

    nte_vehicle::Runtime::VehicleAdvancedTuning live{};
    const auto read_status = vehicle->VehicleReadAdvancedTuning(&live);
    if (read_status.code == ANOMALY_STATUS_V1_OK && live.valid) {
        std::scoped_lock lock(g_context.mutex);
        g_context.adv_live_linear_kmh = live.max_linear_velocity_kmh;
        g_context.adv_live_reverse_kmh = live.reverse_top_speed_kmh;
        g_context.adv_live_up_rpm = live.change_up_rpm;
        g_context.adv_live_down_rpm = live.change_down_rpm;
        g_context.adv_live_gear_time = live.gear_change_time;
        g_context.adv_live_final_ratio = live.final_ratio;
        g_context.adv_live_efficiency = live.transmission_efficiency;
        g_context.adv_live_c_up_rpm = live.comfort_change_up_rpm;
        g_context.adv_live_c_down_rpm = live.comfort_change_down_rpm;
        g_context.adv_live_c_gear_time = live.comfort_gear_change_time;
        g_context.adv_live_c_final_ratio = live.comfort_final_ratio;
        g_context.adv_live_c_efficiency = live.comfort_transmission_efficiency;
        if (g_context.adv_baseline_up_rpm < 0.0F && !g_context.adv_baseline_valid) {
            // Handled per vehicle in the boarding branch below (maintainer
            // review: baselines must not cross vehicles).
        }
    } else {
        std::scoped_lock lock(g_context.mutex);
        g_context.adv_live_linear_kmh = 0.0F;
        g_context.adv_live_reverse_kmh = 0.0F;
    }

    // Boarding / vehicle change detection: the movement component pointer
    // changes whenever the player enters a different vehicle.
    if (live.valid && live.movement != 0 && live.movement != g_context.adv_synced_movement) {
        g_context.adv_synced_movement = live.movement;
        // Factory top speed anchor: capture before any plugin write.
        if (vehicle->VehicleCaptureStockTopSpeed()) {
            float stock = 0.0F;
            if (vehicle->VehicleGetStockTopSpeedKmh(&stock)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.adv_stock_top_speed_kmh = stock;
                g_context.adv_stock_top_speed_valid = true;
                if (g_context.adv_speed_target_kmh <= 0.0F) {
                    g_context.adv_speed_target_kmh = stock;
                }
            }
        } else {
            std::scoped_lock lock(g_context.mutex);
            g_context.adv_stock_top_speed_valid = false;
        }
        // Queue the live module read and the vehicle-selection sync; the
        // customization catalog reload restores this car's saved memory.
        g_context.read_modules_requested.store(true, std::memory_order_release);
        g_context.realtime_sync_requested.store(true, std::memory_order_release);
        // Auto-load this vehicle's saved tuning, per-vehicle factory baseline,
        // and actually write the saved transmission parameters back.
        std::string vehicle_key;
        {
            std::string catalog_id;
            if (vehicle->VehicleReadCurrentCatalogId(&catalog_id).code ==
                ANOMALY_STATUS_V1_OK && !catalog_id.empty()) {
                vehicle_key = catalog_id;
            } else {
                std::scoped_lock lock(g_context.mutex);
                vehicle_key = g_context.selected_vehicle_id;
            }
        }
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.realtime_vehicle_id = vehicle_key;
            // Maintainer review fix: the factory transmission baseline is kept
            // per vehicle — captured from the first read after boarding, which
            // happens before any plugin write, so it is the factory tuning.
            if (!vehicle_key.empty()) {
                auto& baseline = g_context.tuning_baselines[vehicle_key];
                if (baseline[0] < 0.0F) {
                    baseline[0] = live.change_up_rpm;
                    baseline[1] = live.change_down_rpm;
                    baseline[2] = live.gear_change_time;
                    baseline[3] = live.final_ratio;
                    baseline[4] = live.transmission_efficiency;
                    baseline[5] = live.comfort_change_up_rpm;
                    baseline[6] = live.comfort_change_down_rpm;
                    baseline[7] = live.comfort_gear_change_time;
                    baseline[8] = live.comfort_final_ratio;
                    baseline[9] = live.comfort_transmission_efficiency;
                }
                g_context.adv_baseline_up_rpm = baseline[0];
                g_context.adv_baseline_down_rpm = baseline[1];
                g_context.adv_baseline_gear_time = baseline[2];
                g_context.adv_baseline_final_ratio = baseline[3];
                g_context.adv_baseline_efficiency = baseline[4];
                g_context.adv_baseline_c_up_rpm = baseline[5];
                g_context.adv_baseline_c_down_rpm = baseline[6];
                g_context.adv_baseline_c_gear_time = baseline[7];
                g_context.adv_baseline_c_final_ratio = baseline[8];
                g_context.adv_baseline_c_efficiency = baseline[9];
                g_context.adv_baseline_valid = true;
            }
        }
        bool has_saved_tune = false;
        nte_vehicle::Runtime::VehicleAdvancedTuning restored{};
        {
            std::scoped_lock lock(g_context.mutex);
            const auto tune = g_context.saved_tunes.find(vehicle_key);
            if (tune != g_context.saved_tunes.end()) {
                const auto& t = tune->second;
                g_context.adv_linear_top_speed = t[0];
                g_context.adv_change_up_rpm = t[1];
                g_context.adv_down_rpm = t[2];
                g_context.adv_gear_change_time = t[3];
                g_context.adv_final_ratio = t[4];
                g_context.adv_transmission_efficiency = t[5];
                g_context.adv_c_up_rpm = t[6];
                g_context.adv_c_down_rpm = t[7];
                g_context.adv_c_gear_change_time = t[8];
                g_context.adv_c_final_ratio = t[9];
                g_context.adv_c_transmission_efficiency = t[10];
                if (t[11] > 0.0F) g_context.speed_ratio = std::clamp(t[11],
                    kMinimumSpeedRatio, kMaximumSpeedRatio);
                if (t[12] > 0.0F) g_context.engine_torque_ratio = t[12];
                if (t[13] > 0.0F) g_context.other_speed_ratio = t[13];
                if (t[14] > 0.0F) g_context.other_torque_ratio = t[14];
                g_context.adv_reverse_top_speed = t[15];
                // The saved parameters are written immediately (maintainer
                // review: boarding must apply them, not only refresh editors).
                restored.max_linear_velocity_kmh = t[0];
                restored.reverse_top_speed_kmh = t[15];
                restored.change_up_rpm = t[1];
                restored.change_down_rpm = t[2];
                restored.gear_change_time = t[3];
                restored.final_ratio = t[4];
                restored.transmission_efficiency = t[5];
                restored.comfort_change_up_rpm = t[6];
                restored.comfort_change_down_rpm = t[7];
                restored.comfort_gear_change_time = t[8];
                restored.comfort_final_ratio = t[9];
                restored.comfort_transmission_efficiency = t[10];
                has_saved_tune = true;
            }
        }
        if (has_saved_tune) {
            // Both setups receive their own field set; -1 fields are skipped
            // inside the writer, so unset entries never touch the vehicle.
            (void)vehicle->VehicleWriteAdvancedTuning(restored, false);
            (void)vehicle->VehicleWriteAdvancedTuning(restored, true);
        }
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.apply_speed.store(true, std::memory_order_release);
            g_context.apply_torque.store(true, std::memory_order_release);
            g_context.apply_other_speed.store(true, std::memory_order_release);
            g_context.apply_other_torque.store(true, std::memory_order_release);
        }
    } else if (!live.valid && g_context.adv_synced_movement != 0) {
        // Left the vehicle.
        g_context.adv_synced_movement = 0;
        std::scoped_lock lock(g_context.mutex);
        g_context.adv_stock_top_speed_valid = false;
    }
}

// Consume every live-feature request flag. Runs on the Game thread inside
// Update, after AdoptDrivenVehicle.
void TickLiveFeatures() {
    auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr) return;

    // Sync the plugin vehicle selection to the driven vehicle.
    if (g_context.realtime_sync_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string catalog_id;
        const auto id_status = vehicle->VehicleReadCurrentCatalogId(&catalog_id);
        if (id_status.code == ANOMALY_STATUS_V1_OK && !catalog_id.empty()) {
            bool in_catalog = false;
            {
                std::scoped_lock lock(g_context.mutex);
                in_catalog = std::ranges::find(g_context.vehicle_ids, catalog_id) !=
                    g_context.vehicle_ids.end();
            }
            if (in_catalog) {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.pending_vehicle_id = catalog_id;
                    g_context.realtime_vehicle_id = catalog_id;
                }
                const std::array arguments{std::string_view(catalog_id)};
                SetStatus(g_context.localizer.Format("status.synced_to_vehicle",
                    "Synced selection to the driven vehicle: {0}", arguments));
            } else {
                SetStatus(g_context.localizer.Text("status.sync_vehicle_unknown",
                    "The driven vehicle is not in the catalog; open its customization shop once in game first."));
            }
        } else if (id_status.code != ANOMALY_STATUS_V1_OK &&
                   id_status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.sync_vehicle_failed",
                    "Reading the driven vehicle's VehicleID failed"),
                id_status));
        }
    }

    // Read the driven vehicle's applied modules (and optionally store them as
    // this vehicle's plugin memory or export them via the worker thread).
    const bool read_requested =
        g_context.read_modules_requested.exchange(false, std::memory_order_acq_rel);
    const bool save_requested =
        g_context.save_modules_to_memory_requested.exchange(false, std::memory_order_acq_rel);
    const bool export_requested =
        g_context.export_modules_requested.exchange(false, std::memory_order_acq_rel);
    if (read_requested || save_requested) {
        std::vector<std::pair<std::uint32_t, std::string>> pairs;
        const auto status = vehicle->VehicleReadCurrentModules(&pairs);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.current_vehicle_modules = pairs;
                g_context.current_vehicle_modules_valid = true;
            }
            if (save_requested) {
                std::string vehicle_key;
                {
                    std::scoped_lock lock(g_context.mutex);
                    vehicle_key = !g_context.realtime_vehicle_id.empty()
                        ? g_context.realtime_vehicle_id
                        : g_context.selected_vehicle_id;
                }
                if (!vehicle_key.empty()) {
                    std::scoped_lock lock(g_context.mutex);
                    auto& saved = g_context.saved_customizations[vehicle_key];
                    saved.module_overrides.clear();
                    for (const auto& entry : pairs) {
                        if (entry.first == 0) continue;
                        saved.module_overrides.emplace_back(entry);
                    }
                    g_context.memory_dirty.store(true, std::memory_order_release);
                }
                SetStatus(g_context.localizer.Text("status.modules_saved",
                    "Saved the current vehicle's module list to its memory."));
            } else {
                const std::string count = std::to_string(pairs.size());
                const std::array arguments{std::string_view(count)};
                SetStatus(g_context.localizer.Format("status.modules_read",
                    "Read the driven vehicle's modules: {0}", arguments));
                // Maintainer review fix: the read snapshot is handed to the
                // worker thread for writing; only an explicit export request
                // produces a file (no automatic write on boarding).
                if (export_requested) {
                    std::string content = "modules=" + std::to_string(pairs.size()) + "\n";
                    for (const auto& entry : pairs) {
                        content += "  " + entry.second + "\n";
                    }
                    const std::string path = StampSidecarPath("current-vehicle-modules");
                    QueueFileWrite(path, content);
                    const std::array export_arguments{std::string_view(path)};
                    SetStatus(g_context.localizer.Format("status.modules_exported",
                        "Module list exported: {0}", export_arguments));
                }
            }
        } else {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.modules_read_failed",
                    "Reading the driven vehicle's modules failed"),
                status));
        }
    }

    // Capture the driven vehicle's premium-effect FNames into the whitelist.
    if (g_context.capture_effects_requested.exchange(false, std::memory_order_acq_rel)) {
        std::vector<nte_vehicle::Runtime::VehicleEffectEntry> entries;
        const auto status = vehicle->VehicleCaptureEffects(&entries);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::size_t added = 0;
            {
                std::scoped_lock lock(g_context.mutex);
                for (const auto& entry : entries) {
                    // Pair the smoke and mark variants: the same FName base with
                    // WheelSmoke/WheelMark swapped shares the tier.
                    std::string partner_base = entry.base;
                    std::uint32_t partner_type = entry.type;
                    if (entry.type == 23) {
                        const auto pos = partner_base.find("WheelSmoke");
                        if (pos != std::string::npos)
                            partner_base.replace(pos, 10, "WheelMark");
                        partner_type = 24;
                    } else if (entry.type == 24) {
                        const auto pos = partner_base.find("WheelMark");
                        if (pos != std::string::npos)
                            partner_base.replace(pos, 9, "WheelSmoke");
                        partner_type = 23;
                    }
                    const auto ensure_entry = [&](const std::uint32_t type,
                                                  const std::string& text) {
                        for (const auto& existing : g_context.allowed_extra_modules) {
                            if (existing.first == type &&
                                existing.second.substr(0, existing.second.find('#')) == text) {
                                return;
                            }
                        }
                        g_context.allowed_extra_modules.emplace_back(type, text);
                        ++added;
                    };
                    const std::string text = entry.number != 0
                        ? entry.base + "#" + std::to_string(entry.number - 1U)
                        : entry.base;
                    const std::string partner_text = entry.number != 0
                        ? partner_base + "#" + std::to_string(entry.number - 1U)
                        : partner_base;
                    ensure_entry(entry.type, text);
                    ensure_entry(partner_type, partner_text);
                }
                if (g_context.vehicle != nullptr) {
                    g_context.vehicle->SetAllowedExtraModules(g_context.allowed_extra_modules);
                }
                g_context.extras_dirty.store(true, std::memory_order_release);
            }
            SetStatus(added != 0
                ? g_context.localizer.Text("status.effects_captured",
                      "Captured effect FName entries and updated the whitelist.")
                : g_context.localizer.Text("status.effects_none",
                      "No capturable premium effects on the current vehicle."));
        } else {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.effects_capture_failed",
                    "Capturing the current vehicle's effects failed"),
                status));
        }
    }

    // Absolute top-speed target (km/h): the official backend derives the native
    // ratio from the vehicle's factory top speed and maintains the cap.
    if (g_context.apply_speed_target_requested.exchange(false, std::memory_order_acq_rel)) {
        float kmh = 0.0F;
        {
            std::scoped_lock lock(g_context.mutex);
            kmh = g_context.adv_speed_target_kmh;
        }
        if (kmh > 0.0F) {
            nte_vehicle::SpeedTarget request{};
            request.scope = nte_vehicle::SpeedScope::Current;
            request.top_speed_kmh = std::clamp(kmh, 1.0F, 350.0F);
            request.minimum_speed_kmh = 0.0F;
            const auto status = vehicle->VehicleSetSpeedTarget(&request);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                const std::string target_text = std::to_string(
                    static_cast<std::uint32_t>(request.top_speed_kmh));
                const std::array arguments{std::string_view(target_text)};
                SetStatus(g_context.localizer.Format("status.speed_target_applied",
                    "Top speed target applied: {0} km/h", arguments));
            } else {
                SetStatus(StatusDescription(
                    g_context.localizer.Text("status.speed_target_failed",
                        "Applying the top speed target failed"),
                    status));
            }
        }
    }

    // Apply the typed module overrides to the driven vehicle (implicit: drift
    // rotation and the live selector both set this flag).
    if (g_context.apply_current_modules_requested.exchange(false, std::memory_order_acq_rel)) {
        std::vector<std::pair<std::uint32_t, std::string>> overrides;
        std::string fallback_id;
        std::string skin_id;
        {
            std::scoped_lock lock(g_context.mutex);
            overrides = g_context.selected_module_overrides;
            fallback_id = !g_context.realtime_vehicle_id.empty()
                ? g_context.realtime_vehicle_id
                : g_context.selected_vehicle_id;
            skin_id = g_context.realtime_skin_id;
        }
        std::size_t applied_count = 0;
        const auto status = vehicle->VehicleApplyCustomizationToCurrent(
            &overrides, fallback_id, skin_id, &applied_count);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            const std::string count = std::to_string(applied_count);
            const std::array arguments{std::string_view(count)};
            SetStatus(g_context.localizer.Format("status.live_applied",
                "Applied {0} modules to the current vehicle.", arguments));
        } else {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.live_apply_failed",
                    "Applying to the current vehicle failed"),
                status));
        }
    }

    // Apply a skin to the driven vehicle (paint area click).
    if (g_context.apply_skin_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string skin_id;
        {
            std::scoped_lock lock(g_context.mutex);
            skin_id = g_context.realtime_skin_id;
        }
        if (!skin_id.empty()) {
            const auto status = vehicle->VehicleApplySkinToCurrent(skin_id);
            SetStatus(status.code == ANOMALY_STATUS_V1_OK
                ? g_context.localizer.Text("status.skin_live_applied",
                      "Skin applied to the current vehicle.")
                : StatusDescription(
                      g_context.localizer.Text("status.live_apply_failed",
                          "Applying to the current vehicle failed"),
                      status));
        }
    }

    // Advanced tuning: apply the edited values (sliders write on drag).
    if (g_context.adv_apply_requested.exchange(false, std::memory_order_acq_rel)) {
        nte_vehicle::Runtime::VehicleAdvancedTuning value{};
        bool comfort = false;
        {
            std::scoped_lock lock(g_context.mutex);
            value.max_linear_velocity_kmh = g_context.adv_linear_top_speed;
            value.reverse_top_speed_kmh = g_context.adv_reverse_top_speed;
            value.change_up_rpm = g_context.adv_change_up_rpm;
            value.change_down_rpm = g_context.adv_down_rpm;
            value.gear_change_time = g_context.adv_gear_change_time;
            value.final_ratio = g_context.adv_final_ratio;
            value.transmission_efficiency = g_context.adv_transmission_efficiency;
            value.comfort_change_up_rpm = g_context.adv_c_up_rpm;
            value.comfort_change_down_rpm = g_context.adv_c_down_rpm;
            value.comfort_gear_change_time = g_context.adv_c_gear_change_time;
            value.comfort_final_ratio = g_context.adv_c_final_ratio;
            value.comfort_transmission_efficiency = g_context.adv_c_transmission_efficiency;
            comfort = g_context.adv_comfort_mode;
        }
        const auto status = vehicle->VehicleWriteAdvancedTuning(value, comfort);
        if (status.code != ANOMALY_STATUS_V1_OK &&
            status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.tune_apply_failed",
                    "Applying the advanced tuning failed"),
                status));
        }
    }

    // Save the current tuning values as this vehicle's memory.
    if (g_context.save_tune_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string vehicle_key;
        std::array<float, 16> tune{};
        tune.fill(-1.0F);
        {
            std::scoped_lock lock(g_context.mutex);
            vehicle_key = !g_context.realtime_vehicle_id.empty()
                ? g_context.realtime_vehicle_id
                : g_context.selected_vehicle_id;
            tune[0] = g_context.adv_linear_top_speed;
            tune[1] = g_context.adv_live_up_rpm;
            tune[2] = g_context.adv_live_down_rpm;
            tune[3] = g_context.adv_live_gear_time;
            tune[4] = g_context.adv_live_final_ratio;
            tune[5] = g_context.adv_live_efficiency;
            tune[6] = g_context.adv_live_c_up_rpm;
            tune[7] = g_context.adv_live_c_down_rpm;
            tune[8] = g_context.adv_live_c_gear_time;
            tune[9] = g_context.adv_live_c_final_ratio;
            tune[10] = g_context.adv_live_c_efficiency;
            tune[11] = g_context.speed_ratio;
            tune[12] = g_context.engine_torque_ratio;
            tune[13] = g_context.other_speed_ratio;
            tune[14] = g_context.other_torque_ratio;
            tune[15] = g_context.adv_live_reverse_kmh > 0.0F
                ? g_context.adv_live_reverse_kmh : g_context.adv_reverse_top_speed;
            if (tune[11] <= 1.001F && tune[11] >= 0.999F) tune[11] = -1.0F;
            if (tune[12] <= 1.001F && tune[12] >= 0.999F) tune[12] = -1.0F;
            if (tune[13] <= 1.001F && tune[13] >= 0.999F) tune[13] = -1.0F;
            if (tune[14] <= 1.001F && tune[14] >= 0.999F) tune[14] = -1.0F;
        }
        if (!vehicle_key.empty()) {
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.saved_tunes[vehicle_key] = tune;
                g_context.tuning_dirty.store(true, std::memory_order_release);
            }
            SetStatus(g_context.localizer.Text("status.tune_saved",
                "Saved the current tuning to this vehicle's memory."));
        }
    }

    // Restore this vehicle's tuning baseline (the first-read factory values).
    if (g_context.reset_tune_requested.exchange(false, std::memory_order_acq_rel)) {
        nte_vehicle::Runtime::VehicleAdvancedTuning value{};
        bool has_baseline = false;
        {
            std::scoped_lock lock(g_context.mutex);
            has_baseline = g_context.adv_baseline_valid;
            value.max_linear_velocity_kmh = g_context.adv_linear_top_speed;
            value.reverse_top_speed_kmh = -1.0F;
            value.change_up_rpm = g_context.adv_baseline_up_rpm;
            value.change_down_rpm = g_context.adv_baseline_down_rpm;
            value.gear_change_time = g_context.adv_baseline_gear_time;
            value.final_ratio = g_context.adv_baseline_final_ratio;
            value.transmission_efficiency = g_context.adv_baseline_efficiency;
            value.comfort_change_up_rpm = g_context.adv_baseline_c_up_rpm;
            value.comfort_change_down_rpm = g_context.adv_baseline_c_down_rpm;
            value.comfort_gear_change_time = g_context.adv_baseline_c_gear_time;
            value.comfort_final_ratio = g_context.adv_baseline_c_final_ratio;
            value.comfort_transmission_efficiency = g_context.adv_baseline_c_efficiency;
        }
        if (!has_baseline) {
            SetStatus(g_context.localizer.Text("status.tune_none",
                "No saved tuning for this vehicle yet."));
        } else {
            const auto normal = vehicle->VehicleWriteAdvancedTuning(value, false);
            const auto comfort = vehicle->VehicleWriteAdvancedTuning(value, true);
            if (normal.code == ANOMALY_STATUS_V1_OK && comfort.code == ANOMALY_STATUS_V1_OK) {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.speed_ratio = kDefaultSpeedRatio;
                    g_context.engine_torque_ratio = 1.0F;
                    g_context.other_speed_ratio = 1.0F;
                    g_context.other_torque_ratio = 1.0F;
                    g_context.other_speed_floor_ratio = 0.6024F;
                    g_context.adv_change_up_rpm = -1.0F;
                    g_context.adv_down_rpm = -1.0F;
                    g_context.adv_gear_change_time = -1.0F;
                    g_context.adv_final_ratio = -1.0F;
                    g_context.adv_transmission_efficiency = -1.0F;
                    g_context.adv_c_up_rpm = -1.0F;
                    g_context.adv_c_down_rpm = -1.0F;
                    g_context.adv_c_gear_change_time = -1.0F;
                    g_context.adv_c_final_ratio = -1.0F;
                    g_context.adv_c_transmission_efficiency = -1.0F;
                }
                g_context.apply_speed.store(true, std::memory_order_release);
                g_context.apply_torque.store(true, std::memory_order_release);
                g_context.apply_other_speed.store(true, std::memory_order_release);
                g_context.apply_other_torque.store(true, std::memory_order_release);
                SetStatus(g_context.localizer.Text("status.tune_restored",
                    "Restored this vehicle's tuning baseline."));
            } else {
                SetStatus(StatusDescription(
                    g_context.localizer.Text("status.tune_apply_failed",
                        "Applying the advanced tuning failed"),
                    normal.code != ANOMALY_STATUS_V1_OK ? normal : comfort));
            }
        }
    }

    // Per-vehicle summon hotkey: restore the bound vehicle's memory and summon.
    if (g_context.hotkey_summon_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string vehicle_key;
        {
            std::scoped_lock lock(g_context.mutex);
            vehicle_key = std::move(g_context.hotkey_summon_vehicle_id);
            g_context.hotkey_summon_vehicle_id.clear();
        }
        if (!vehicle_key.empty()) {
            const auto saved = [&]() -> bool {
                std::scoped_lock lock(g_context.mutex);
                const auto found = g_context.saved_customizations.find(vehicle_key);
                if (found == g_context.saved_customizations.end()) return false;
                g_context.customization_skin_id = found->second.skin_id.empty()
                    ? g_context.customization_skin_id : found->second.skin_id;
                g_context.selected_module_overrides = found->second.module_overrides;
                return true;
            }();
            if (saved) {
                std::scoped_lock lock(g_context.mutex);
                g_context.customization_vehicle_id = vehicle_key;
                g_context.summon_customized.store(true, std::memory_order_release);
            } else {
                std::scoped_lock lock(g_context.mutex);
                g_context.selected_module_overrides.clear();
                g_context.summon.store(true, std::memory_order_release);
            }
        }
    }

    // Diagnostics: export the status log ring.
    if (g_context.export_log_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string content = "\xEF\xBB\xBF";
        content += "plugin-log " + LocalStamp() + " lines=" +
            std::to_string(g_context.log_lines.size()) + "\n";
        {
            std::scoped_lock lock(g_context.mutex);
            for (const auto& line : g_context.log_lines) {
                content += line + "\n";
            }
        }
        const std::string path = StampSidecarPath("plugin-log");
        QueueFileWrite(path, content);
        const std::array arguments{std::string_view(path)};
        SetStatus(g_context.localizer.Format("status.log_exported",
            "Log exported: {0}", arguments));
    }

    // Diagnostics: probe candidate premium-effect IDs against the FName pool.
    if (g_context.probe_requested.exchange(false, std::memory_order_acq_rel)) {
        static constexpr std::array<std::string_view, 18> kProbeIds{
            "V016_WheelSmoke_10", "V017_WheelSmoke_10", "V021_WheelSmoke_10",
            "V026_WheelSmoke_10", "V037_WheelSmoke_10", "V038_WheelSmoke_10",
            "V016_WheelMark_10", "V017_WheelMark_10", "V021_WheelMark_10",
            "V026_WheelMark_10", "V037_WheelMark_10", "V038_WheelMark_10",
            "Vehicle_Common_WheelSmoke_09", "Vehicle_Common_WheelSmoke_10",
            "Vehicle_Common_WheelMark_09", "Vehicle_Common_WheelMark_10",
            "V060a_WheelSmoke_10", "V060a_WheelMark_10"};
        std::string content = "fname-probe " + LocalStamp() + "\n";
        std::size_t hits = 0;
        for (const auto& id : kProbeIds) {
            std::uint32_t name_id = 0;
            const std::string candidate(id);
            // The names service pointer lives inside the backend; probe through
            // a catalog-free find via the runtime's resolve path.
            const bool found = vehicle->ProbeFNameText(candidate, &name_id);
            content += candidate + " -> " + (found ? ("id=" + std::to_string(name_id)) : "not found") + "\n";
            if (found) ++hits;
        }
        content += "hits=" + std::to_string(hits) + "/" + std::to_string(kProbeIds.size()) + "\n";
        const std::string path = StampSidecarPath("fname-probe");
        QueueFileWrite(path, content);
        const std::array arguments{std::string_view(path)};
        SetStatus(g_context.localizer.Format("status.probe_exported",
            "FName probe written: {0}", arguments));
    }

    // Diagnostics: sweep every DataTable in the registry.
    if (g_context.sweep_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string content;
        const auto status = vehicle->SweepAllDataTables(&content);
        if (status.code == ANOMALY_STATUS_V1_OK && !content.empty()) {
            const std::string path = StampSidecarPath("datatables-inventory");
            QueueFileWrite(path, content);
            const std::array arguments{std::string_view(path)};
            SetStatus(g_context.localizer.Format("status.sweep_exported",
                "DataTable sweep written: {0}", arguments));
        } else {
            SetStatus(StatusDescription(
                g_context.localizer.Text("status.sweep_failed",
                    "The DataTable sweep failed"),
                status));
        }
    }

    // Durability auto-full toggle.
    if (g_context.durability_auto_full_requested.exchange(false, std::memory_order_acq_rel)) {
        bool enabled;
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.durability_auto_full = !g_context.durability_auto_full;
            enabled = g_context.durability_auto_full;
        }
        SetStatus(enabled
            ? g_context.localizer.Text("status.durability_auto_full_on",
                  "Automatic durability repair enabled.")
            : g_context.localizer.Text("status.durability_auto_full_off",
                  "Automatic durability repair disabled."));
        g_context.settings_dirty.store(true, std::memory_order_release);
    }
}

void DiscardWorldRequests() {
    for (auto* flag : {&g_context.refresh_customization_requested, &g_context.refresh_catalog_requested,
             &g_context.apply_speed_target_requested, &g_context.apply_speed, &g_context.apply_torque,
             &g_context.apply_other_speed, &g_context.apply_other_torque, &g_context.reset,
             &g_context.summon, &g_context.summon_customized, &g_context.friction_toggle,
             &g_context.durability_guard_toggle, &g_context.hotkey_summon_requested,
             &g_context.export_modules_requested, &g_context.durability_auto_full_requested,
             &g_context.apply_current_modules_requested, &g_context.apply_skin_requested,
             &g_context.read_modules_requested, &g_context.realtime_sync_requested,
             &g_context.save_modules_to_memory_requested, &g_context.capture_effects_requested,
             &g_context.adv_apply_requested, &g_context.adv_read_requested,
             &g_context.save_tune_requested, &g_context.reset_tune_requested,
             &g_context.probe_requested, &g_context.sweep_requested}) {
        flag->store(false, std::memory_order_release);
    }
    std::scoped_lock lock(g_context.mutex);
    g_context.pending_vehicle_id.clear();
    g_context.hotkey_summon_vehicle_id.clear();
    g_context.hotkey_binding_mode = false;
}

void Update() {
    if (!g_context.vehicle || !g_context.started.load(std::memory_order_acquire)) return;

    // 玩家服务由 NTE 桥接层在本地玩家存在后才发布，这里按 tick 节流补绑。
    if (g_context.player == nullptr) {
        if (g_context.service_retry_ticks == 0) {
            g_context.service_retry_ticks = 60;
            g_context.player = QueryService<AnomalyNtePlayerServiceV1>(
                g_context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
        } else {
            --g_context.service_retry_ticks;
        }
    }

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

    const bool allowed = g_context.vehicle->LocalWorldAllowed();
    bool previously_allowed{};
    {
        std::scoped_lock lock(g_context.mutex);
        previously_allowed = g_context.local_world_allowed;
        g_context.local_world_allowed = allowed;
        if (previously_allowed != allowed) {
            g_context.snapshot = {};
            g_context.player_position_valid = false;
            g_context.adv_synced_movement = 0;
            g_context.adv_stock_top_speed_valid = false;
            g_context.adv_baseline_valid = false;
            g_context.current_vehicle_modules_valid = false;
            g_context.realtime_vehicle_id.clear();
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            g_context.customization_options.clear();
            g_context.customization_vehicle_id.clear();
            g_context.tuning_baselines.clear();
        }
    }
    if (!allowed || !previously_allowed) DiscardWorldRequests();
    if (!allowed) {
        // Consume key edges here so holding a key through PVP/travel cannot
        // replay a summon or drift action when the local world becomes ready.
        for (std::uint32_t vk = 0x08; vk < 0xFE; ++vk)
            g_context.prev_key_state[vk] = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) ? 1 : 0;
        g_context.drift_prev_shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x80 : 0;
        return;
    }
    if (!previously_allowed) {
        g_context.refresh_catalog_requested.store(true, std::memory_order_release);
    }

    // 钩子装好时碰撞伤害已在扣耐久前被中止；Local feature:耐久自动保持开关
    // 打开时每帧补满（钩子环境下通常无事可做，未装钩子时是经典回退路径）。
    // 开关关闭且钩子未装时不做任何 RepairSelf 写入。
    bool durability_guard_installed = false;
    bool durability_auto_full = false;
    {
        std::scoped_lock lock(g_context.mutex);
        durability_guard_installed = g_context.durability_guard_installed;
        durability_auto_full = g_context.durability_auto_full;
    }
    if (durability_auto_full) {
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
                    // Local feature: remember this configuration as the vehicle's
                    // summon memory so per-vehicle hotkeys and later catalog
                    // loads restore it automatically.
                    {
                        std::scoped_lock lock(g_context.mutex);
                        auto& saved = g_context.saved_customizations[selected_id];
                        saved.skin_id = selected_skin_id;
                        saved.module_overrides = module_overrides;
                        g_context.memory_dirty.store(true, std::memory_order_release);
                    }
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

    if (g_context.durability_guard_toggle.exchange(false, std::memory_order_acq_rel)) {
        const bool enabled = !nte_vehicle::DurabilityGuardEnabled();
        nte_vehicle::SetDurabilityGuardEnabled(enabled);
        const bool applied = nte_vehicle::DurabilityGuardEnabled();
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.durability_guard_enabled = applied;
            g_context.status = applied
                ? g_context.localizer.Text("status.durability_guard_enabled",
                      "Vehicle durability guard enabled: collision damage no longer reduces "
                      "durability.")
                : g_context.localizer.Text("status.durability_guard_disabled",
                      "Vehicle durability guard disabled: collision damage is applied again.");
        }
        g_context.settings_dirty.store(true, std::memory_order_release);
    }

    // Local feature: per-tick live-feature processing. Boarding detection and
    // the request handlers run here, on the Game thread.
    AdoptDrivenVehicle();
    CheckHotkeyKeys();
    CheckDriftRotation();
    TickLiveFeatures();

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

    static constexpr std::array<std::string_view, 28> module_type_keys{
        "label.module.none", "label.module.engine", "label.module.gearbox",
        "label.module.brake", "label.module.suspension", "label.module.body_color",
        "label.module.front_bumper", "label.module.side_skirt", "label.module.rear_bumper",
        "label.module.rear_wing", "label.module.decal", "label.module.wheel",
        "label.module.hood", "label.module.plate", "label.module.roof",
        "label.module.tire", "label.module.accessory", "label.module.lsd",
        "label.module.stiffness", "label.module.skin", "label.module.interior_leather",
        "label.module.interior_stitch", "label.module.vehicle_badge",
        "label.module.tire_smoke", "label.module.tire_track", "label.module.tow_hook",
        "label.module.ecu", "label.module.exhaust_smoke"
    };
    static constexpr std::array<std::string_view, 28> module_type_english{
        "None", "Engine", "Gearbox", "Brakes", "Suspension", "Body color module",
        "Front bumper", "Side skirt", "Rear bumper", "Rear wing", "Decal", "Wheels",
        "Hood", "License plate", "Roof", "Tires", "Accessories",
        "Limited-slip differential LSD", "Body rigidity", "Skin module",
        "Interior leather color", "Interior stitching color", "Vehicle badge",
        "Tire smoke", "Tire tracks", "Tow hook", "ECU", "Exhaust smoke"
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
    // Some shipped skins carry an empty FText name; exposing the raw SkinID there is
    // unreadable, so fall back to the localized ordinal of that skin within the vehicle.
    const auto skin_display_label = [&](const CustomizationOption& option) -> std::string {
        if (!option.label.empty()) return option.label;
        std::uint32_t ordinal = 1;
        for (const auto& candidate : options) {
            if (candidate.kind != nte_vehicle::kCustomizationSkin) continue;
            if (candidate.id == option.id) break;
            ++ordinal;
        }
        const std::string ordinal_text = std::to_string(ordinal);
        const std::array arguments{std::string_view(ordinal_text)};
        return g_context.localizer.Format("label.skin_fallback", "Skin {0}", arguments);
    };
    const auto draw_skin_options = [&]() {
        DrawText(ui, g_context.localizer.Text("section.skins", "Choose a skin"));
        bool has_default_skin = false;
        for (const auto& option : options) {
            if (option.kind != nte_vehicle::kCustomizationSkin) continue;
            has_default_skin = has_default_skin || option.is_default;
            const bool current = skin_id == option.id;
            std::string label = skin_display_label(option);
            if (current) {
                label = "[" + g_context.localizer.Text("label.selected_mark", "Selected") + "] " + label;
            }
            label += "##custom-skin-" + option.id;
            if (ui->button(ui->user, anomaly::sdk::StringView(label), list_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.customization_skin_id = option.id;
                const std::string skin_label = skin_display_label(option);
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
                    const std::string skin_label = skin_display_label(*found);
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
            // Local feature: mark the equipped slot and the slot that keeps
            // the game default, matching the community build's customization list.
            if (explicit_selected) {
                label = "[" + g_context.localizer.Text("label.selected_mark", "Selected") + "] " + label;
            } else if (default_selected) {
                label = "[" + g_context.localizer.Text("label.default_mark", "Default") + "] " + label;
            }
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
        // Local feature: whitelisted premium-effect entries (captured from a
        // driven vehicle's cloud-applied modules) render as extra buttons for
        // the tyre smoke / track slots and apply to customized summons too.
        if (type == 23 || type == 24) {
            std::vector<std::pair<std::uint32_t, std::string>> extras;
            {
                std::scoped_lock lock(g_context.mutex);
                extras = g_context.allowed_extra_modules;
            }
            // Only this vehicle's own whitelist entries render, deduplicated
            // against catalog rows of the same base ID (the catalog carries the
            // effect once it is owned, so the whitelist entry would duplicate it).
            const std::string extra_prefix = VehicleModulePrefixFromId(selected);
            for (const auto& extra : extras) {
                if (extra.first != type) continue;
                if (!WhitelistExtraForVehicle(extra.second, extra_prefix)) continue;
                const std::string display = WhitelistExtraEffectDisplay(extra.second);
                if (display.empty()) continue;
                std::string extra_base = extra.second;
                const auto extra_hash = extra_base.find('#');
                if (extra_hash != std::string::npos) extra_base.resize(extra_hash);
                const bool duplicated = std::ranges::any_of(options,
                    [&](const CustomizationOption& option) {
                        return option.kind == nte_vehicle::kCustomizationModule &&
                            option.module_type == type && option.id == extra_base;
                    });
                if (duplicated) continue;
                const bool selected_extra = override_for_type(type) == extra.second;
                std::string label = (selected_extra ? "[x] " : "") + display +
                    "##whitelist-extra-" + extra.second;
                if (ui->button(ui->user, anomaly::sdk::StringView(label), list_width, 0.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    auto& overrides = g_context.selected_module_overrides;
                    const auto found = std::ranges::find_if(overrides,
                        [type](const auto& entry) { return entry.first == type; });
                    if (found == overrides.end()) overrides.emplace_back(type, extra.second);
                    else found->second = extra.second;
                    const std::string category = g_context.localizer.Text(
                        module_type_keys[type], module_type_english[type]);
                    const std::array arguments{std::string_view(category)};
                    g_context.customization_status = g_context.localizer.Format(
                        "status.module_updated", "Updated the {0} selection", arguments);
                    g_context.customization_status_failure = false;
                }
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

    // Local feature: save the driven vehicle's current module list as this
    // vehicle's plugin memory (restored on catalog reload and summon). The
    // currently bound summon hotkey is shown next to the button instead of a
    // long binding list.
    {
        const auto save_label =
            g_context.localizer.Text("action.save_current_modules", "Save current vehicle modules") +
            "##save-current-modules";
        if (DrawButtonEnabled(ui, save_label, true)) {
            g_context.read_modules_requested.store(true, std::memory_order_release);
            g_context.save_modules_to_memory_requested.store(true, std::memory_order_release);
            SetStatus(g_context.localizer.Text("status.modules_save_requested",
                "Saving the current vehicle's module list to its memory..."));
        }
        DrawInlineGap(ui);
        std::uint32_t bound = 0;
        bool binding_mode = false;
        {
            std::scoped_lock lock(g_context.mutex);
            for (const auto& [key, vehicle_id] : g_context.hotkey_vehicle_bindings) {
                if (vehicle_id == selected) {
                    bound = key;
                    break;
                }
            }
            binding_mode = g_context.hotkey_binding_mode;
        }
        const std::string bound_text = bound != 0 ? VirtualKeyDisplayName(bound)
            : g_context.localizer.Text("text.hotkey_none", "none");
        const std::array bound_arguments{std::string_view(bound_text)};
        DrawText(ui, g_context.localizer.Format("text.current_hotkey",
            "Hotkey: {0}", bound_arguments) +
            std::string(binding_mode ? "  <<" +
                g_context.localizer.Text("action.binding_wait", "press any key to bind...") + ">>"
                : std::string()));
    }
    // Local feature: per-vehicle summon hotkey binding.
    {
        bool binding_mode = false;
        {
            std::scoped_lock lock(g_context.mutex);
            binding_mode = g_context.hotkey_binding_mode;
        }
        if (binding_mode) {
            const auto wait_label = g_context.localizer.Text(
                "action.binding_wait", ">> press any key to bind... <<");
            if (DrawButtonEnabled(ui, wait_label + "##bind-hotkey", true)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.hotkey_binding_mode = false;
            }
        } else {
            const auto bind_label = g_context.localizer.Text(
                "action.bind_summon_hotkey", "Bind summon hotkey") + "##bind-hotkey";
            if (DrawButtonEnabled(ui, bind_label, true)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.hotkey_binding_mode = true;
            }
        }
        DrawInlineGap(ui);
        const auto clear_label = g_context.localizer.Text(
            "action.clear_summon_hotkey", "Clear this vehicle's hotkey") + "##clear-hotkey";
        if (DrawButtonEnabled(ui, clear_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            std::erase_if(g_context.hotkey_vehicle_bindings,
                [&selected](const auto& entry) { return entry.second == selected; });
            g_context.memory_dirty.store(true, std::memory_order_release);
            g_context.status = g_context.localizer.Text("status.hotkey_cleared",
                "Cleared this vehicle's summon hotkey.");
        }
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
    float other_speed_floor_ratio, float other_torque_ratio,
    bool friction, bool durability_guard, bool durability_guard_installed,
    const std::string& durability_guard_detail, bool durability_auto_full) {
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

    // ==== Local feature: live customization of the driven vehicle ====
    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("section.live_customization",
        "Live customization (driven vehicle)"));
    DrawText(ui, g_context.localizer.Text("hint.live_customization",
        "Read the driven vehicle, then pick options below; every click applies to "
        "the current vehicle immediately (best while stationary)."));
    {
        const auto read_label =
            g_context.localizer.Text("action.read_live_modules",
                "Read driven vehicle (sync selection)") + "##read-live-modules";
        if (DrawButtonEnabled(ui, read_label, true)) {
            g_context.read_modules_requested.store(true, std::memory_order_release);
            g_context.realtime_sync_requested.store(true, std::memory_order_release);
        }
    }
    // Drift rotation buttons (global, Left Shift applies the rotation in game).
    {
        std::string realtime_id;
        std::string selected_id;
        bool smoke_on, mark_on, smoke_random, mark_random, smoke_ui, mark_ui;
        std::size_t smoke_count, mark_count;
        std::vector<std::pair<std::uint32_t, std::string>> extras;
        {
            std::scoped_lock lock(g_context.mutex);
            realtime_id = g_context.realtime_vehicle_id;
            selected_id = g_context.selected_vehicle_id;
            smoke_on = g_context.drift_smoke_on;
            mark_on = g_context.drift_mark_on;
            smoke_random = g_context.drift_smoke_random;
            mark_random = g_context.drift_mark_random;
            smoke_ui = g_context.drift_smoke_ui_open;
            mark_ui = g_context.drift_mark_ui_open;
            smoke_count = g_context.drift_smoke_scope.size();
            mark_count = g_context.drift_mark_scope.size();
            extras = g_context.allowed_extra_modules;
        }
        const std::string key = !realtime_id.empty() ? realtime_id : selected_id;
        const std::string prefix = VehicleModulePrefixFromId(key);
        const auto on_off = [&](const bool value) {
            return std::string(value ? "On" : "Off");
        };
        const std::string smoke_label =
            g_context.localizer.Format("action.drift_smoke", "Tyre smoke rotation: {0}",
                std::array<std::string_view, 1>{on_off(smoke_on)}) + "##drift-smoke";
        if (DrawButtonEnabled(ui, smoke_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_smoke_on = !g_context.drift_smoke_on;
            g_context.drift_dirty.store(true, std::memory_order_release);
        }
        DrawInlineGap(ui);
        const std::string smoke_scope_label =
            g_context.localizer.Format("action.drift_smoke_scope", "Smoke scope ({0})",
                std::array<std::string_view, 1>{std::to_string(smoke_count)}) +
            "##drift-smoke-scope";
        if (DrawButtonEnabled(ui, smoke_scope_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_smoke_ui_open = !g_context.drift_smoke_ui_open;
        }
        DrawInlineGap(ui);
        const std::string smoke_mode_label =
            g_context.localizer.Format("action.drift_smoke_mode", "Smoke mode: {0}",
                std::array<std::string_view, 1>{
                    std::string(smoke_random ? "Random" : "Sequential")}) +
            "##drift-smoke-mode";
        if (DrawButtonEnabled(ui, smoke_mode_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_smoke_random = !g_context.drift_smoke_random;
            g_context.drift_dirty.store(true, std::memory_order_release);
        }
        DrawInlineGap(ui);
        const std::string mark_label =
            g_context.localizer.Format("action.drift_mark", "Tyre track rotation: {0}",
                std::array<std::string_view, 1>{on_off(mark_on)}) + "##drift-mark";
        if (DrawButtonEnabled(ui, mark_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_mark_on = !g_context.drift_mark_on;
            g_context.drift_dirty.store(true, std::memory_order_release);
        }
        DrawInlineGap(ui);
        const std::string mark_scope_label =
            g_context.localizer.Format("action.drift_mark_scope", "Track scope ({0})",
                std::array<std::string_view, 1>{std::to_string(mark_count)}) +
            "##drift-mark-scope";
        if (DrawButtonEnabled(ui, mark_scope_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_mark_ui_open = !g_context.drift_mark_ui_open;
        }
        DrawInlineGap(ui);
        const std::string mark_mode_label =
            g_context.localizer.Format("action.drift_mark_mode", "Track mode: {0}",
                std::array<std::string_view, 1>{
                    std::string(mark_random ? "Random" : "Sequential")}) +
            "##drift-mark-mode";
        if (DrawButtonEnabled(ui, mark_mode_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_mark_random = !g_context.drift_mark_random;
            g_context.drift_dirty.store(true, std::memory_order_release);
        }
        DrawInlineGap(ui);
        const auto capture_label =
            g_context.localizer.Text("action.capture_effects",
                "Add current vehicle's effects to whitelist") + "##capture-effects";
        if (DrawButtonEnabled(ui, capture_label, true)) {
            g_context.capture_effects_requested.store(true, std::memory_order_release);
        }
        DrawText(ui, g_context.localizer.Text("hint.drift_rotation",
            "Enable a rotation, then press Left Shift in game to cycle the tyre "
            "smoke / track effects; scope narrows the tiers that participate."));
        // Scope tick lists (collapsed by default).
        const auto draw_scope_list = [&](const bool open, const std::uint32_t type,
                                         const std::vector<std::string>& scope,
                                         const char* child_id) {
            if (!open) return;
            std::vector<std::string> candidates;
            {
                std::scoped_lock lock(g_context.mutex);
                for (const auto& option : g_context.customization_options) {
                    if (option.kind == nte_vehicle::kCustomizationModule &&
                        option.module_type == type) {
                        candidates.push_back(option.id);
                    }
                }
                for (const auto& extra : extras) {
                    if (extra.first != type) continue;
                    if (!WhitelistExtraForVehicle(extra.second, prefix)) continue;
                    std::string base = extra.second;
                    const auto hash_pos = base.find('#');
                    if (hash_pos != std::string::npos) base.resize(hash_pos);
                    bool listed = false;
                    for (const auto& id : candidates) {
                        if (id == base) { listed = true; break; }
                    }
                    if (!listed) candidates.push_back(extra.second);
                }
            }
            std::vector<std::string> patterns;
            for (const auto& id : candidates) {
                const std::string pattern = CandidateTierPattern(id);
                bool listed = false;
                for (const auto& existing : patterns) {
                    if (existing == pattern) { listed = true; break; }
                }
                if (!listed) patterns.push_back(pattern);
            }
            std::sort(patterns.begin(), patterns.end());
            // Display each tier pattern with its Chinese effect name instead of
            // the raw ID token.
            std::map<std::string, std::string> pattern_names;
            for (const auto& id : candidates) {
                const std::string pattern = CandidateTierPattern(id);
                if (pattern_names.find(pattern) == pattern_names.end()) {
                    pattern_names.emplace(pattern, CandidateDisplayName(id));
                }
            }
            const bool child_available =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
                    sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
                ui->begin_child && ui->end_child;
            bool visible = true;
            if (child_available) {
                visible = ui->begin_child(ui->user,
                    anomaly::sdk::StringView(child_id), 0.0F, 120.0F, 1U) != 0;
            }
            if (visible) {
                for (const auto& pattern : patterns) {
                    bool checked = false;
                    for (const auto& tier : scope) {
                        if (tier == pattern) { checked = true; break; }
                    }
                    int flag = checked ? 1 : 0;
                    const std::string display = pattern_names.count(pattern) != 0
                        ? pattern_names[pattern] : pattern;
                    const std::string checkbox_label =
                        "[" + std::string(checked ? "x" : " ") + "] " + display +
                        "##drift-scope-" + std::to_string(type) + "-" + pattern;
                    if (ui->checkbox(ui->user,
                            anomaly::sdk::StringView(checkbox_label), &flag) != 0) {
                        std::scoped_lock lock(g_context.mutex);
                        auto& target = type == 23
                            ? g_context.drift_smoke_scope : g_context.drift_mark_scope;
                        if (flag != 0) {
                            if (std::ranges::find(target, pattern) == target.end())
                                target.push_back(pattern);
                        } else {
                            std::erase(target, pattern);
                        }
                        g_context.drift_dirty.store(true, std::memory_order_release);
                    }
                }
            }
            if (child_available) ui->end_child(ui->user);
        };
        draw_scope_list(smoke_ui, 23, [&]{
            std::scoped_lock lock(g_context.mutex);
            return g_context.drift_smoke_scope; }(), "##drift-smoke-scope-list");
        draw_scope_list(mark_ui, 24, [&]{
            std::scoped_lock lock(g_context.mutex);
            return g_context.drift_mark_scope; }(), "##drift-mark-scope-list");
    }
    // Live option picker: category cards first (like the customization tab);
    // clicking a card shows only that category's list below. Every option click
    // applies to the driven vehicle immediately.
    {
        std::string realtime_id;
        std::string customization_vehicle;
        std::string skin_id;
        std::vector<CustomizationOption> options;
        std::vector<std::pair<std::uint32_t, std::string>> overrides;
        std::vector<std::pair<std::uint32_t, std::string>> extras;
        {
            std::scoped_lock lock(g_context.mutex);
            realtime_id = g_context.realtime_vehicle_id;
            customization_vehicle = g_context.customization_vehicle_id;
            skin_id = g_context.realtime_skin_id;
            options = g_context.customization_options;
            overrides = g_context.selected_module_overrides;
            extras = g_context.allowed_extra_modules;
        }
        if (realtime_id.empty() || customization_vehicle != realtime_id) {
            DrawText(ui, g_context.localizer.Text("hint.live_read_first",
                "Click \"Read driven vehicle\" first; the picker appears once this "
                "car's customization catalog is loaded."));
        } else {
            static constexpr std::array<std::string_view, 28> live_type_keys{
                "label.module.none", "label.module.engine", "label.module.gearbox",
                "label.module.brake", "label.module.suspension", "label.module.body_color",
                "label.module.front_bumper", "label.module.side_skirt",
                "label.module.rear_bumper", "label.module.rear_wing", "label.module.decal",
                "label.module.wheel", "label.module.hood", "label.module.plate",
                "label.module.roof", "label.module.tire", "label.module.accessory",
                "label.module.lsd", "label.module.stiffness", "label.module.skin",
                "label.module.interior_leather", "label.module.interior_stitch",
                "label.module.vehicle_badge", "label.module.tire_smoke",
                "label.module.tire_track", "label.module.tow_hook", "label.module.ecu",
                "label.module.exhaust_smoke"};
            static constexpr std::array<std::string_view, 28> live_type_english{
                "None", "Engine", "Gearbox", "Brakes", "Suspension", "Body color module",
                "Front bumper", "Side skirt", "Rear bumper", "Rear wing", "Decal",
                "Wheels", "Hood", "License plate", "Roof", "Tires", "Accessories",
                "Limited-slip differential LSD", "Body rigidity", "Skin module",
                "Interior leather color", "Interior stitching color", "Vehicle badge",
                "Tire smoke", "Tire tracks", "Tow hook", "ECU", "Exhaust smoke"};
            const std::string prefix = VehicleModulePrefixFromId(realtime_id);
            // Category list: paint first, then every module type that has
            // catalog options or whitelisted effect entries for this vehicle.
            std::vector<std::pair<std::uint32_t, std::string>> live_categories;
            live_categories.emplace_back(0U,
                g_context.localizer.Text("section.paint", "Paint"));
            std::vector<std::uint32_t> live_types;
            for (const auto& option : options) {
                if (option.kind != nte_vehicle::kCustomizationModule ||
                    option.module_type == 0 || option.module_type == 19 ||
                    option.module_type >= 28) continue;
                if (std::find(live_types.begin(), live_types.end(),
                        option.module_type) == live_types.end()) {
                    live_types.push_back(option.module_type);
                }
            }
            for (const auto& extra : extras) {
                if (extra.first == 0 || extra.first == 19 || extra.first >= 28) continue;
                if (!WhitelistExtraForVehicle(extra.second, prefix)) continue;
                if (WhitelistExtraEffectDisplay(extra.second).empty()) continue;
                if (std::find(live_types.begin(), live_types.end(),
                        extra.first) == live_types.end()) {
                    live_types.push_back(extra.first);
                }
            }
            std::sort(live_types.begin(), live_types.end());
            for (const auto type : live_types) {
                live_categories.emplace_back(type, g_context.localizer.Text(
                    live_type_keys[type], live_type_english[type]));
            }
            static std::size_t live_active = 0;
            if (live_active >= live_categories.size()) live_active = 0;
            // Category cards: up to five per row, the active one bracketed.
            constexpr std::size_t kLiveColumns = 5;
            const bool has_same_line =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, same_line),
                    sizeof(decltype(AnomalyUiServiceV1::same_line))) && ui->same_line;
            for (std::size_t row_begin = 0; row_begin < live_categories.size();
                 row_begin += kLiveColumns) {
                const std::size_t row_end =
                    (std::min)(live_categories.size(), row_begin + kLiveColumns);
                for (std::size_t i = row_begin; i < row_end; ++i) {
                    const auto& category = live_categories[i];
                    const std::string card_label =
                        (live_active == i ? "[" + category.second + "]" : category.second) +
                        "##live-cat-" + std::to_string(category.first);
                    if (ui->button(ui->user, anomaly::sdk::StringView(card_label),
                            96.0F, 0.0F)) {
                        live_active = i;
                    }
                    if (i + 1 < row_end) {
                        if (has_same_line) ui->same_line(ui->user, 0.0F, 8.0F);
                        else DrawInlineGap(ui);
                    }
                }
            }
            // Only the active category's list renders, in a bounded child.
            const bool child_available =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
                    sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
                ui->begin_child && ui->end_child;
            bool list_visible = true;
            if (child_available) {
                list_visible = ui->begin_child(ui->user,
                    anomaly::sdk::StringView("##live-options-child"), 0.0F, 300.0F, 1U) != 0;
            }
            if (list_visible) {
                const std::uint32_t active_type = live_categories[live_active].first;
                const auto current_override = [&](const std::uint32_t type) {
                    std::string current;
                    for (const auto& entry : overrides) {
                        if (entry.first == type) { current = entry.second; break; }
                    }
                    return current;
                };
                if (active_type == 0) {
                    // Paint: clicking a skin applies it to the driven vehicle.
                    for (const auto& option : options) {
                        if (option.kind != nte_vehicle::kCustomizationSkin) continue;
                        const bool chosen = skin_id == option.id;
                        std::string label =
                            (chosen ? "[" + g_context.localizer.Text("label.selected_mark", "Selected") + "] " : "") +
                            (option.label.empty() ? option.id : option.label) +
                            "##live-skin-" + option.id;
                        if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F)) {
                            {
                                std::scoped_lock lock(g_context.mutex);
                                g_context.realtime_skin_id = option.id;
                            }
                            g_context.apply_skin_requested.store(true, std::memory_order_release);
                        }
                    }
                } else {
                    const std::string current = current_override(active_type);
                    for (const auto& option : options) {
                        if (option.kind != nte_vehicle::kCustomizationModule ||
                            option.module_type != active_type) continue;
                        const bool chosen = current == option.id;
                        std::string label =
                            (chosen ? "[" + g_context.localizer.Text("label.selected_mark", "Selected") + "] " : "") +
                            (option.label.empty() ? option.id : option.label) +
                            "##live-" + std::to_string(active_type) + "-" + option.id;
                        if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F)) {
                            {
                                std::scoped_lock lock(g_context.mutex);
                                auto& target = g_context.selected_module_overrides;
                                const auto found = std::ranges::find_if(target,
                                    [active_type](const auto& entry) { return entry.first == active_type; });
                                if (found == target.end()) target.emplace_back(active_type, option.id);
                                else found->second = option.id;
                            }
                            g_context.apply_current_modules_requested.store(
                                true, std::memory_order_release);
                        }
                    }
                    // Whitelisted premium effects for the tyre slots: filtered to
                    // this vehicle's prefix and deduplicated against catalog rows.
                    if (active_type == 23 || active_type == 24) {
                        for (const auto& extra : extras) {
                            if (extra.first != active_type) continue;
                            if (!WhitelistExtraForVehicle(extra.second, prefix)) continue;
                            const std::string display =
                                WhitelistExtraEffectDisplay(extra.second);
                            if (display.empty()) continue;
                            std::string base = extra.second;
                            const auto hash_pos = base.find('#');
                            if (hash_pos != std::string::npos) base.resize(hash_pos);
                            const bool duplicated = std::ranges::any_of(options,
                                [&](const CustomizationOption& option) {
                                    return option.kind == nte_vehicle::kCustomizationModule &&
                                        option.module_type == active_type &&
                                        option.id == base;
                                });
                            if (duplicated) continue;
                            const bool chosen = current == extra.second;
                            const std::string label =
                                (chosen ? "[" + g_context.localizer.Text("label.selected_mark", "Selected") + "] " : "") +
                                display + "##live-wl-" + std::to_string(active_type) + "-" + extra.second;
                            if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F)) {
                                {
                                    std::scoped_lock lock(g_context.mutex);
                                    auto& target = g_context.selected_module_overrides;
                                    const auto found = std::ranges::find_if(target,
                                        [active_type](const auto& entry) { return entry.first == active_type; });
                                    if (found == target.end())
                                        target.emplace_back(active_type, extra.second);
                                    else found->second = extra.second;
                                }
                                g_context.apply_current_modules_requested.store(
                                    true, std::memory_order_release);
                            }
                        }
                    }
                }
            }
            if (child_available) ui->end_child(ui->user);
        }
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

    // ==== Local feature: absolute km/h target + advanced powertrain tuning ====
    {
        float stock = 0.0F;
        bool stock_valid = false;
        float target_kmh = 0.0F;
        {
            std::scoped_lock lock(g_context.mutex);
            stock = g_context.adv_stock_top_speed_kmh;
            stock_valid = g_context.adv_stock_top_speed_valid;
            target_kmh = g_context.adv_speed_target_kmh;
        }
        if (stock_valid && stock > 0.0F) {
            DrawText(ui, g_context.localizer.Text("hint.speed_target",
                "Absolute top speed target, anchored at the vehicle's factory top "
                "speed (applied while dragging)."));
            const std::string stock_text = std::to_string(
                static_cast<std::uint32_t>(stock));
            const std::array stock_arguments{std::string_view(stock_text)};
            DrawText(ui, g_context.localizer.Format("text.stock_speed_anchor",
                "Factory top speed anchor: {0} km/h", stock_arguments));
            if (ui->slider_float(ui->user,
                    anomaly::sdk::StringView(g_context.localizer.Text(
                        "label.speed_target", "Top speed target (km/h)")),
                    &target_kmh, 0.0F, 350.0F)) {
                // Apply only when the slider value actually changed (drag),
                // never on every rendered frame.
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.adv_speed_target_kmh = target_kmh;
                }
                if (target_kmh > 0.0F) {
                    g_context.apply_speed_target_requested.store(
                        true, std::memory_order_release);
                }
            }
        }
    }
    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("section.advanced_tuning",
        "Advanced powertrain tuning (per vehicle)"));
    {
        float reverse = -1.0F;
        {
            std::scoped_lock lock(g_context.mutex);
            reverse = g_context.adv_reverse_top_speed < 0.0F
                ? 0.0F : g_context.adv_reverse_top_speed;
        }
        const std::string reverse_label =
            g_context.localizer.Text("label.reverse_speed", "Reverse top speed (km/h)") +
            "##reverse-speed";
        if (ui->slider_float(ui->user, anomaly::sdk::StringView(reverse_label),
                &reverse, 0.0F, 2000.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.adv_reverse_top_speed = reverse;
            g_context.adv_apply_requested.store(true, std::memory_order_release);
        }
        DrawText(ui, g_context.localizer.Text("hint.reverse_speed",
            "Writes ReverseTopSpeedInKmH; takes effect after re-entering the vehicle "
            "(the stock value may not equal the real reverse limit)."));
    }
    {
        bool comfort = false;
        float up = -1.0F, down = -1.0F, gear_time = -1.0F, ratio = -1.0F, eff = -1.0F;
        float live_up = 0, live_down = 0, live_time = 0, live_ratio = 0, live_eff = 0;
        {
            std::scoped_lock lock(g_context.mutex);
            comfort = g_context.adv_comfort_mode;
            up = g_context.adv_change_up_rpm;
            down = g_context.adv_down_rpm;
            gear_time = g_context.adv_gear_change_time;
            ratio = g_context.adv_final_ratio;
            eff = g_context.adv_transmission_efficiency;
            live_up = g_context.adv_live_up_rpm;
            live_down = g_context.adv_live_down_rpm;
            live_time = g_context.adv_live_gear_time;
            live_ratio = g_context.adv_live_final_ratio;
            live_eff = g_context.adv_live_efficiency;
            if (comfort) {
                up = g_context.adv_c_up_rpm;
                down = g_context.adv_c_down_rpm;
                gear_time = g_context.adv_c_gear_change_time;
                ratio = g_context.adv_c_final_ratio;
                eff = g_context.adv_c_transmission_efficiency;
                live_up = g_context.adv_live_c_up_rpm;
                live_down = g_context.adv_live_c_down_rpm;
                live_time = g_context.adv_live_c_gear_time;
                live_ratio = g_context.adv_live_c_final_ratio;
                live_eff = g_context.adv_live_c_efficiency;
            }
        }
        const std::string mode_label =
            comfort
                ? g_context.localizer.Text("label.transmission_mode_comfort",
                      "Editing: comfort transmission (click to switch to normal)")
                : g_context.localizer.Text("label.transmission_mode_normal",
                      "Editing: normal transmission (click to switch to comfort)");
        if (DrawButtonEnabled(ui, mode_label + "##tune-mode", true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.adv_comfort_mode = !g_context.adv_comfort_mode;
        }
        DrawInlineGap(ui);
        const auto save_label =
            g_context.localizer.Text("action.save_tune", "Save current tuning") +
            "##save-tune";
        if (DrawButtonEnabled(ui, save_label, true)) {
            g_context.save_tune_requested.store(true, std::memory_order_release);
        }
        DrawInlineGap(ui);
        const auto reset_label =
            g_context.localizer.Text("action.reset_tune", "Restore tuning baseline") +
            "##reset-tune";
        if (DrawButtonEnabled(ui, reset_label, true)) {
            g_context.reset_tune_requested.store(true, std::memory_order_release);
        }
        // Negative edit values mean "not edited this session": seed the slider
        // with the live value so dragging starts from the current state.
        const auto locked_slider = [&](const char* key, const char* english,
                                       float* edited, const float live,
                                       const float minimum, const float maximum) {
            float value = 0.0F;
            {
                std::scoped_lock lock(g_context.mutex);
                value = *edited >= 0.0F ? *edited : (live > 0.0F ? live : 0.0F);
            }
            const std::string label = std::string(g_context.localizer.Text(key, english)) +
                "##adv-" + key;
            if (ui->slider_float(ui->user, anomaly::sdk::StringView(label),
                    &value, minimum, maximum)) {
                std::scoped_lock lock(g_context.mutex);
                *edited = value;
                g_context.adv_apply_requested.store(true, std::memory_order_release);
            }
        };
        locked_slider("label.change_up_rpm", "Shift-up RPM", comfort
            ? &g_context.adv_c_up_rpm : &g_context.adv_change_up_rpm,
            live_up, 500.0F, 12000.0F);
        locked_slider("label.change_down_rpm", "Shift-down RPM", comfort
            ? &g_context.adv_c_down_rpm : &g_context.adv_down_rpm,
            live_down, 0.0F, 12000.0F);
        locked_slider("label.gear_change_time", "Gear change time (s)", comfort
            ? &g_context.adv_c_gear_change_time : &g_context.adv_gear_change_time,
            live_time, 0.0F, 2.0F);
        locked_slider("label.final_ratio", "Final drive ratio", comfort
            ? &g_context.adv_c_final_ratio : &g_context.adv_final_ratio,
            live_ratio, 0.5F, 10.0F);
        locked_slider("label.transmission_efficiency", "Transmission efficiency", comfort
            ? &g_context.adv_c_transmission_efficiency
            : &g_context.adv_transmission_efficiency,
            live_eff, 0.1F, 3.0F);
    }

    DrawDivider(ui);
    DrawText(ui, g_context.localizer.Text("text.durability_note",
        "Current land vehicle durability: the collision damage guard stops the damage event, so "
        "durability is never reduced. The value itself stays server-owned."));
    DrawText(ui, g_context.localizer.Text("text.durability_detail",
        "The guard blocks the dump-verified vehicle damage entry; without it the plugin falls "
        "back to RepairSelf(true). Water vehicles have no verified equivalent."));
    // Local feature (community layout): the wheel friction switch and the
    // automatic durability repair toggle live on this page, next to the
    // durability explanation.
    DrawInlineGap(ui);
    int guard_flag = durability_guard ? 1 : 0;
    if (ui->checkbox(ui->user,
            anomaly::sdk::StringView(g_context.localizer.Text(
                "label.durability_guard", "Durability guard") + "##perf-guard"),
            &guard_flag) != 0) {
        g_context.durability_guard_toggle.store(true, std::memory_order_release);
    }
    if (!durability_guard_installed) {
        DrawInlineGap(ui);
        DrawText(ui, durability_guard_detail.empty()
            ? g_context.localizer.Text("status.durability_guard_unavailable",
                  "Vehicle durability guard is unavailable.")
            : durability_guard_detail);
    }
    DrawInlineGap(ui);
    {
        const std::string auto_full_label =
            g_context.localizer.Format("action.durability_auto_full_toggle",
                "Keep durability full: {0}",
                std::array<std::string_view, 1>{std::string(
                    durability_auto_full
                        ? g_context.localizer.Text("text.on", "On")
                        : g_context.localizer.Text("text.off", "Off"))});
        if (DrawButtonEnabled(ui, auto_full_label + "##perf-auto-full", true)) {
            g_context.durability_auto_full_requested.store(true, std::memory_order_release);
        }
    }

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
    // Local feature (community layout): wheel friction sits at the far right
    // of the reset row.
    {
        float window_width = 0.0F;
        float window_height = 0.0F;
        const bool has_size =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, get_window_size),
                sizeof(decltype(AnomalyUiServiceV1::get_window_size))) &&
            ui->get_window_size != nullptr;
        if (has_size) ui->get_window_size(ui->user, &window_width, &window_height);
        const bool has_cursor =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, set_cursor_pos_x),
                sizeof(decltype(AnomalyUiServiceV1::set_cursor_pos_x))) &&
            ui->set_cursor_pos_x != nullptr;
        if (has_cursor && window_width > 240.0F) {
            ui->set_cursor_pos_x(ui->user, window_width - 150.0F);
        } else {
            DrawInlineGap(ui);
        }
        const std::string friction_label =
            g_context.localizer.Format("action.friction_toggle", "Wheel friction: {0}",
                std::array<std::string_view, 1>{std::string(
                    friction ? g_context.localizer.Text("text.on", "On")
                             : g_context.localizer.Text("text.off", "Off"))});
        if (DrawButtonEnabled(ui, friction_label + "##perf-friction", true)) {
            g_context.friction_toggle.store(true, std::memory_order_release);
        }
    }
}

void DrawDebugPanel(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    const std::string& status, const std::string& selected,
    std::size_t catalog_size, bool player_position_valid) {
    DrawText(ui, g_context.localizer.Text("detail.build_marker",
        "Build marker: NTE Vehicle 0.14.2"));
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

    // Local feature: diagnostic actions writing timestamped evidence files into
    // the plugin state directory (for bug reports). The catalog refresh button
    // lives here too (community layout).
    DrawDivider(ui);
    const auto refresh_label =
        g_context.localizer.Text("action.refresh_catalog", "Refresh vehicle list") +
        "##diag-refresh";
    if (ui->button(ui->user, anomaly::sdk::StringView(refresh_label), 0.0F, 0.0F)) {
        g_context.refresh_catalog_requested.store(true, std::memory_order_release);
    }
    const auto read_label =
        g_context.localizer.Text("action.read_live_modules",
            "Read driven vehicle (sync selection)") + "##diag-read-modules";
    if (ui->button(ui->user, anomaly::sdk::StringView(read_label), 0.0F, 0.0F)) {
        g_context.read_modules_requested.store(true, std::memory_order_release);
        g_context.export_modules_requested.store(true, std::memory_order_release);
    }
    const auto probe_label =
        g_context.localizer.Text("action.probe_effects",
            "Probe candidate effect IDs (read-only)") + "##diag-probe";
    if (ui->button(ui->user, anomaly::sdk::StringView(probe_label), 0.0F, 0.0F)) {
        g_context.probe_requested.store(true, std::memory_order_release);
    }
    const auto sweep_label =
        g_context.localizer.Text("action.sweep_tables",
            "Sweep all DataTables (forensics)") + "##diag-sweep";
    if (ui->button(ui->user, anomaly::sdk::StringView(sweep_label), 0.0F, 0.0F)) {
        g_context.sweep_requested.store(true, std::memory_order_release);
    }
    const auto export_label =
        g_context.localizer.Text("action.export_log",
            "Export log (attach when reporting)") + "##diag-export";
    if (ui->button(ui->user, anomaly::sdk::StringView(export_label), 0.0F, 0.0F)) {
        g_context.export_log_requested.store(true, std::memory_order_release);
    }

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
    bool durability_guard;
    bool durability_guard_installed;
    bool durability_auto_full{};
    std::string durability_guard_detail;
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
        durability_guard = g_context.durability_guard_enabled;
        durability_guard_installed = g_context.durability_guard_installed;
        durability_guard_detail = g_context.durability_guard_detail;
        durability_auto_full = g_context.durability_auto_full;
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
    nav_button(g_context.localizer.Text("action.tab_diagnostics", "Diagnostics"), 2);
    DrawDivider(ui);

    if (active_tab == 0) {
        DrawCustomizationTab(ui, selected, ids, labels, customization_options,
            module_overrides, customization_vehicle_id, customization_skin_id,
            customization_status, customization_status_failure, last_summon,
            player_position_valid);
    } else if (active_tab == 1) {
        DrawPerformanceTab(ui, snap, speed_ratio, torque_ratio, other_speed_ratio,
            other_speed_floor_ratio, other_torque_ratio, friction, durability_guard,
            durability_guard_installed, durability_guard_detail, durability_auto_full);
    } else if (active_tab == 2) {
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
    const auto* config = QueryService<AnomalyConfigServiceV1>(
        host, ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    if (!vehicle || !InputReady(input) || !WindowReady(window_service)) {
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
    g_context.host = host;
    g_context.player = player;
    g_context.input = input;
    g_context.window_service = window_service;
    g_context.plugin_state = plugin_state;
    g_context.config = config;
    g_settings_save.Bind(host, config, kSettingsSchemaId, kSettingsSchemaVersion);
    g_context.core = QueryService<AnomalyCoreServiceV1>(
        host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    g_context.settings_schema = {};
    g_context.durability_guard_toggle.store(false, std::memory_order_release);
    static_cast<void>(RegisterSettingsSchema());
    LoadSettings();
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
    g_context.local_world_allowed = false;
    g_context.snapshot.vehicle_snapshot_status = g_context.localizer.Text(
        "status.snapshot_pending",
        "The current driving vehicle state has not been read yet.");
    g_context.selected_vehicle_id = "Vehicle007";
    g_context.saved_vehicle_id = LoadSavedSelection();
    g_context.saved_selection_pending = !g_context.saved_vehicle_id.empty();
    if (!g_context.saved_vehicle_id.empty()) {
        g_context.selected_vehicle_id = g_context.saved_vehicle_id;
    }
    // Local feature: load the per-vehicle memory and feature configuration
    // (plugin-state directory, lifecycle callback — no I/O in Game/Render).
    LoadCustomizationMemory();
    LoadTuningMemory();
    LoadDriftConfig();
    LoadAllowedExtraModules();
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
    // Maintainer review fix: launch the file-write worker (callbacks only
    // queue payloads; this thread persists them and the dirty state files).
    g_context.save_worker_exit.store(false, std::memory_order_release);
    g_context.save_worker = std::thread(SaveWorkerLoop);
    InstallDurabilityGuardOnStart();
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
    nte_vehicle::ReleaseDurabilityGuard();
    nte_vehicle::RestoreWheelFrictionBackend();
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.durability_guard_installed = false;
        g_context.local_world_allowed = false;
    }
    nte_vehicle::StopBackend();
    // Maintainer review fix: stop the worker before the final save so nothing
    // writes concurrently with the lifecycle finalization.
    g_context.save_worker_exit.store(true, std::memory_order_release);
    if (g_context.save_worker.joinable()) g_context.save_worker.join();
    SaveSelection(g_context.saved_vehicle_id);
    SaveAllPluginState();
    QueueSettingsSave();
    const bool saved = g_settings_save.Flush();
    ReleaseMenuHotkey(g_context);
    if (g_context.window_service && g_context.menu_window_handle.id != 0) {
        static_cast<void>(g_context.window_service->set_open(
            g_context.window_service->user, g_context.menu_window_handle, 0));
    }
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.speed_ratio = kDefaultSpeedRatio;
    }
    return saved ? anomaly::sdk::Ok() : Status(ANOMALY_STATUS_V1_FAILED);
} catch (...) { return Status(ANOMALY_STATUS_V1_FAILED); }

void ANOMALY_CALL Unload(void* plugin_context) try {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    g_context.toggle_menu_requested.store(false, std::memory_order_release);
    nte_vehicle::ReleaseDurabilityGuard();
    nte_vehicle::RestoreWheelFrictionBackend();
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.durability_guard_installed = false;
        g_context.local_world_allowed = false;
    }
    nte_vehicle::StopBackend();
    g_context.save_worker_exit.store(true, std::memory_order_release);
    if (g_context.save_worker.joinable()) g_context.save_worker.join();
    SaveSelection(g_context.saved_vehicle_id);
    SaveAllPluginState();
    QueueSettingsSave();
    static_cast<void>(g_settings_save.Flush());
    ReleaseMenuHotkey(g_context);
    if (g_context.window_service && g_context.menu_window_handle.id != 0) {
        static_cast<void>(g_context.window_service->release_window(
            g_context.window_service->user, g_context.menu_window_handle));
    }
    g_context.menu_window_handle = {};
    nte_vehicle::UnloadBackend();
    g_context.vehicle = nullptr;
    g_context.host = nullptr;
    g_context.player = nullptr;
    g_context.input = nullptr;
    g_context.window_service = nullptr;
    g_context.config = nullptr;
    g_context.core = nullptr;
    g_context.settings_schema = {};
    g_settings_save.Reset();
} catch (...) {  }

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) try {
    if (plugin_context == &g_context && g_context.started.load(std::memory_order_acquire)) {
        nte_vehicle::TickBackend();
        Update();
        QueueSettingsSave();
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
        anomaly::sdk::StringView("0.14.2"), Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}
