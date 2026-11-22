#include "backend.hpp"
#include "runtime.hpp"
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ui_resources.h"
#include "anomaly/sdk/services/plugin_state.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/localization.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <map>
#include <cstdio>
#include <string>
#include <windows.h>
#include <cctype>
#include <string_view>
#include <vector>
#include <utility>
#include <fstream>
#include <chrono>
#include <random>

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

// Local feature (ported from the 2.6 v3.2 plugin): per-vehicle customization
// memory so module/skin choices survive game restarts. The old 2.6 runtime
// wrote this on successful customized summons; the 0.11.1 rewrite dropped it.
struct SavedCustomization {
    std::string skin_id;
    std::vector<std::pair<std::uint32_t, std::string>> module_overrides;
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
    std::map<std::string, SavedCustomization> saved_customizations;
    // Local feature (ported from the 2.6 v3.2 plugin): per-vehicle summon hotkeys.
    std::map<std::uint32_t, std::string> hotkey_vehicle_bindings;
    bool hotkey_binding_mode{};
    std::string hotkey_summon_vehicle_id;
    std::uint8_t prev_key_state[256]{};
    std::atomic_bool hotkey_summon_requested{};
    std::atomic_bool sweep_requested{};
    std::atomic_bool read_modules_requested{};
    std::atomic_bool save_modules_to_memory_requested{};
    std::atomic_bool probe_requested{};
    // Local feature: apply overrides onto the currently driven vehicle (client-
    // side visual rebuild via the modification component's own OnRep handler).
    std::atomic_bool apply_current_modules_requested{};
    // Local feature: capture the driven vehicle's equipped effects (with their
    // exact FName numbers) into the whitelist file, so every car can self-serve
    // its own 霓虹域界/拟真态火焰 entries.
    std::atomic_bool capture_effects_requested{};
    // Local feature: real-time modification — sync the plugin's vehicle selection
    // to the driven car so the 改装调校 page can pick that car's options in place.
    std::atomic_bool realtime_sync_requested{};
    std::string realtime_vehicle_id;
    std::vector<std::pair<std::uint32_t, std::string>> current_vehicle_modules;
    bool current_vehicle_modules_valid{};
    // Local feature: log ring (fed by SetStatus) + export button.
    std::atomic_bool export_log_requested{};
    std::vector<std::string> log_lines;
    // Local feature: advanced powertrain tuning (transmission + mobile top speed).
    std::atomic_bool adv_read_requested{};
    std::atomic_bool adv_apply_requested{};
    bool adv_comfort_mode{};   // which transmission setup the sliders edit
    // km/h target slider (0.11.30): displays the target top speed in km/h and
    // derives the speed ratio from the captured factory top speed — the ratio
    // kernel itself is unchanged.
    float adv_speed_target_kmh{};
    float adv_stock_top_speed_kmh{};
    bool adv_stock_valid{};
    // Shared top-level fields (km/h): MaxLinearVelocity (m/s in game, the real
    // physics cap) and ReverseTopSpeedInKmH.
    float adv_linear_top_speed{};
    float adv_reverse_top_speed{};
    // Normal (普通) transmission set.
    float adv_change_up_rpm{};
    float adv_down_rpm{};
    float adv_gear_change_time{};
    float adv_final_ratio{};
    float adv_transmission_efficiency{};
    // Comfort (舒适) transmission set.
    float adv_c_change_up_rpm{};
    float adv_c_down_rpm{};
    float adv_c_gear_change_time{};
    float adv_c_final_ratio{};
    float adv_c_transmission_efficiency{};
    // Live values + the movement pointer the sliders were last synced from
    // (auto re-sync whenever the driven vehicle changes).
    std::uint64_t adv_synced_movement{};
    float adv_live_linear{};
    float adv_live_reverse{};
    float adv_live_up{};
    float adv_live_down{};
    float adv_live_time{};
    float adv_live_ratio{};
    float adv_live_eff{};
    float adv_live_c_up{};
    float adv_live_c_down{};
    float adv_live_c_time{};
    float adv_live_c_ratio{};
    float adv_live_c_eff{};
    // Baselines (defaults at first sight this session, before any write).
    float adv_baseline_linear{};
    float adv_baseline_reverse{};
    float adv_baseline_up{};
    float adv_baseline_down{};
    float adv_baseline_time{};
    float adv_baseline_ratio{};
    float adv_baseline_eff{};
    float adv_baseline_c_up{};
    float adv_baseline_c_down{};
    float adv_baseline_c_time{};
    float adv_baseline_c_ratio{};
    float adv_baseline_c_eff{};
    bool adv_baseline_valid{};
    // Local feature: per-vehicle tuning memory (separate file from the
    // customization memory, same state directory).
    std::atomic_bool save_tune_requested{};
    std::atomic_bool reset_tune_requested{};
    // Per-vehicle saved tunes, persisted as tuning-memory.cfg in the same state
    // directory as customization-memory.cfg.
    // Array layout (16; -1 = unset for legacy entries): [0] linear km/h,
    // [1] n_up, [2] n_down, [3] n_time, [4] n_ratio, [5] n_eff,
    // [6] c_up, [7] c_down, [8] c_time, [9] c_ratio, [10] c_eff,
    // [11] speed ratio, [12] torque ratio, [13] other speed, [14] other torque,
    // [15] reverse km/h.
    std::map<std::string, std::array<float, 16>> saved_tunes;
    // Local feature: real-time skin application on the driven vehicle.
    std::atomic_bool apply_skin_requested{};
    std::string realtime_skin_id;
    // Local feature: drift rotation (漂移轮换) — Shift cycles the tyre smoke /
    // tyre mark through the user-checked option tiers. GLOBAL settings (shared
    // by all vehicles, user request), persisted as drift-config.cfg in the same
    // state directory. Scope stores tier patterns (e.g. "WheelSmoke_10",
    // "WheelMark_10") matched against each candidate's base after the vehicle
    // prefix — one checked set maps onto every car's own option ids.
    bool drift_smoke_on{};
    bool drift_mark_on{};
    std::vector<std::string> drift_smoke_scope;
    std::vector<std::string> drift_mark_scope;
    std::size_t drift_smoke_index{};
    std::size_t drift_mark_index{};
    std::uint8_t drift_prev_shift{};
    // 轮换模式:false = 顺序(按列表循环,从当前装备件继续),true = 随机
    // (纯随机,尽量避免连续重复)。烟雾/痕迹各自独立,持久化于
    // drift-config.cfg(S/M 行的 random 字段)。
    bool drift_smoke_random{};
    bool drift_mark_random{};
    bool drift_smoke_ui_open{};
    bool drift_mark_ui_open{};
    std::vector<std::pair<std::uint32_t, std::string>> allowed_extra_modules;
    const AnomalyUe5NamesServiceV1* names{};
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
    // 0.11.32: 耐久自动保持开关(默认开,保持既有行为;用户可在性能调校页切换)。
    std::atomic_bool durability_auto_full{true};
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

// Local feature (ported from the 2.6 v3.2 plugin): customization memory.
// Format (one entry per line, same file the 2.6 plugin wrote):
//   V|<vehicle_id>|<skin_id or empty>
//   M|<vehicle_id>|<module_type>:<module_id>
std::string CustomizationMemoryPath() {
    if (g_context.state_directory.empty()) return {};
    std::string path = g_context.state_directory;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "customization-memory.cfg";
    return path;
}

std::string CatalogDumpPath() {
    if (g_context.state_directory.empty()) return {};
    std::string path = g_context.state_directory;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "catalog-dump.txt";
    return path;
}

// Local feature: timestamped snapshot names so before/after experiments (e.g.
// unlocking a premium effect) can be diffed instead of overwritten.
std::string LocalStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04u%02u%02u-%02u%02u%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::string(buffer);
}

std::string StampSidecarPath(const char* base_name) {
    if (g_context.state_directory.empty()) return {};
    std::string path = g_context.state_directory;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += std::string(base_name) + "-" + LocalStamp() + ".txt";
    return path;
}

// Local safety: two-phase evidence writes (tmp file + replace). A crash mid-write
// can at worst lose the .tmp file; the previous snapshot stays intact.
void WriteTextFileAtomic(const std::string& path, const std::string& text) {
    if (path.empty()) return;
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << text;
        out.flush();
        if (!out.good()) return;
    }
    MoveFileExA(tmp_path.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void LoadCustomizationMemory() {
    const std::string path = CustomizationMemoryPath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string line;
    std::scoped_lock lock(g_context.mutex);
    while (std::getline(in, line)) {
        if (line.size() < 3 || line[1] != '|') continue;
        const char type = line[0];
        const std::size_t p1 = line.find('|', 2);
        if (p1 == std::string::npos) continue;
        const std::string vehicle_id = line.substr(2, p1 - 2);
        std::string rest = line.substr(p1 + 1);
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
        if (type == 'V') {
            g_context.saved_customizations[vehicle_id].skin_id = rest;
        } else if (type == 'M') {
            const std::size_t p2 = rest.find(':');
            if (p2 == std::string::npos) continue;
            std::uint32_t mt = 0;
            try { mt = static_cast<std::uint32_t>(std::stoul(rest.substr(0, p2))); }
            catch (...) { continue; }
            g_context.saved_customizations[vehicle_id].module_overrides.push_back(
                {mt, rest.substr(p2 + 1)});
        } else if (type == 'H') {
            // Local feature: per-vehicle summon hotkey bindings (2.6 v3.2 format).
            std::uint32_t vk = 0;
            try { vk = static_cast<std::uint32_t>(std::stoul(rest)); }
            catch (...) { continue; }
            g_context.hotkey_vehicle_bindings[vk] = vehicle_id;
        }
    }
}

// Local feature: per-vehicle tuning memory — tuning-memory.cfg, same state
// directory as customization-memory.cfg.
//   T2|<id>|13 floats: mobile, normal up/time/ratio/eff, comfort
//   up/time/ratio/eff, ratios speed/torque/other_speed/other_torque
//   T|<id>|5 floats (legacy 0.11.21 lines) → normal set only.
std::string TuningMemoryPath() {
    if (g_context.state_directory.empty()) return {};
    std::string path = g_context.state_directory;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "tuning-memory.cfg";
    return path;
}

void LoadTuningMemory() {
    const std::string path = TuningMemoryPath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        const bool v2 = line.size() > 2 && line[0] == 'T' && line[1] == '2' &&
                        line[2] == '|';
        const bool v1 = line.size() > 1 && line[0] == 'T' && line[1] == '|';
        if (!v2 && !v1) continue;
        // v2-new: id + 16 floats; v2-legacy: id + 13; v1: id + 5.
        const std::size_t expect = v2 ? 17U : (v1 ? 6U : 0U);
        const bool v2_legacy = v2;  // distinguished by the parsed field count
        std::vector<std::string> fields;
        std::size_t pos = v2 ? 3 : 2;
        while (fields.size() < expect) {
            const auto next = line.find('|', pos);
            if (next == std::string::npos) {
                fields.push_back(line.substr(pos));
                break;
            }
            fields.push_back(line.substr(pos, next - pos));
            pos = next + 1;
        }
        if (fields.size() < expect || fields[0].empty()) continue;
        std::array<float, 16> tune{};
        tune.fill(-1.0F);
        bool ok = true;
        const std::size_t count = fields.size() - 1;
        for (std::size_t k = 0; k < count; ++k) {
            try {
                tune[k] = std::stof(fields[1 + k]);
            } catch (...) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        std::array<float, 16> mapped{};
        mapped.fill(-1.0F);
        if (v1) {
            // v1: mobile dropped; [1] n_up, [2] n_time, [3] n_ratio, [4] n_eff.
            mapped[1] = tune[1];
            mapped[3] = tune[2];
            mapped[4] = tune[3];
            mapped[5] = tune[4];
        } else if (v2_legacy && count == 13U) {
            // Old T2: [0] mobile (dropped — desktop-irrelevant field),
            // [1-4] n up/time/ratio/eff, [5-8] c up/time/ratio/eff, [9-12] ratios.
            mapped[1] = tune[1];
            mapped[3] = tune[2];
            mapped[4] = tune[3];
            mapped[5] = tune[4];
            mapped[6] = tune[5];
            mapped[8] = tune[6];
            mapped[9] = tune[7];
            mapped[10] = tune[8];
            mapped[11] = tune[9];
            mapped[12] = tune[10];
            mapped[13] = tune[11];
            mapped[14] = tune[12];
        } else {
            mapped = tune;
        }
        g_context.saved_tunes[fields[0]] = mapped;
    }
}

void SaveTuningMemory() {
    const std::string path = TuningMemoryPath();
    if (path.empty()) return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& e : g_context.saved_tunes) {
        out << "T2|" << e.first;
        for (const float v : e.second) {
            out << "|" << v;
        }
        out << "\n";
    }
}

// Local feature: drift rotation config — drift-config.cfg, same state
// directory. Global (all vehicles) settings; scope stores tier patterns.
//   S|<on 0/1>|<pattern,pattern,...>   (empty patterns = all)
//   M|<on 0/1>|<pattern,pattern,...>
std::string DriftConfigPath() {
    if (g_context.state_directory.empty()) return {};
    std::string path = g_context.state_directory;
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path += "drift-config.cfg";
    return path;
}

void LoadDriftConfig() {
    const std::string path = DriftConfigPath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.size() < 3 || line[1] != '|') continue;
        const char kind = line[0];
        const bool on = line[2] == '1';
        const std::string rest = line.size() > 3 ? line.substr(3) : std::string();
        // Format: "<random>|<p1,p2,...>" (0.11.33+); legacy "<p1,p2,...>" loads
        // with random = false.
        bool random_mode = false;
        std::string pattern_text = rest;
        const auto format_pipe = rest.find('|');
        if (format_pipe != std::string::npos) {
            random_mode = format_pipe > 0 && rest[0] == '1';
            pattern_text = rest.substr(format_pipe + 1);
        }
        std::vector<std::string> patterns;
        std::size_t pos = 0;
        while (pos < pattern_text.size()) {
            const auto next = pattern_text.find(',', pos);
            if (next == std::string::npos) {
                if (next > pos) patterns.push_back(pattern_text.substr(pos));
                break;
            }
            if (next > pos) {
                patterns.push_back(pattern_text.substr(pos, next - pos));
            }
            pos = next + 1;
        }
        if (kind == 'S') {
            g_context.drift_smoke_on = on;
            g_context.drift_smoke_random = random_mode;
            g_context.drift_smoke_scope = patterns;
        } else if (kind == 'M') {
            g_context.drift_mark_on = on;
            g_context.drift_mark_random = random_mode;
            g_context.drift_mark_scope = patterns;
        }
    }
}

void SaveDriftConfig() {
    const std::string path = DriftConfigPath();
    if (path.empty()) return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    const auto write_line = [&](const char kind, bool on, bool random_mode,
            const std::vector<std::string>& scope) {
        out << kind << '|' << (on ? 1 : 0) << '|' << (random_mode ? 1 : 0) << '|';
        for (std::size_t i = 0; i < scope.size(); ++i) {
            out << scope[i];
            if (i + 1 < scope.size()) out << ",";
        }
        out << "\n";
    };
    write_line('S', g_context.drift_smoke_on, g_context.drift_smoke_random,
        g_context.drift_smoke_scope);
    write_line('M', g_context.drift_mark_on, g_context.drift_mark_random,
        g_context.drift_mark_scope);
}

void SaveCustomizationMemory() {
    const std::string path = CustomizationMemoryPath();
    if (path.empty()) return;
    std::scoped_lock lock(g_context.mutex);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& e : g_context.saved_customizations) {
        out << "V|" << e.first << "|" << e.second.skin_id << "\n";
        for (const auto& m : e.second.module_overrides)
            out << "M|" << e.first << "|" << m.first << ":" << m.second << "\n";
    }
    for (const auto& h : g_context.hotkey_vehicle_bindings)
        out << "H|" << h.second << "|" << h.first << "\n";
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



// Local feature: every status (and therefore every error path) is appended to an
// in-memory log ring; 「导出日志」 writes it to the state dir so the user can
// share the exact wording of any failure.
void AppendLogLocked(const std::string& text) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u] ",
        static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond));
    std::string line = stamp;
    line += text;
    auto& log = g_context.log_lines;
    if (log.size() >= 500) {
        log.erase(log.begin());
    }
    log.push_back(std::move(line));
}

void SetStatus(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    AppendLogLocked(text);
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

// Local feature: drift rotation (漂移轮换) — Shift (0.3 s delayed) cycles the
// equipped tyre smoke / tyre mark through the user-checked options of the
// current vehicle. Candidates follow the picker's render order (catalog rows
// then vehicle-filtered whitelist extras); the rotation continues from the
// currently equipped item. Fires the standard real-time apply afterwards.
// Runs on the plugin update (Game) thread.
std::string VehicleModulePrefixFromId(const std::string& vehicle_id);
bool WhitelistExtraForVehicle(const std::string& extra_id, const std::string& prefix);
std::string WhitelistExtraEffectDisplay(const std::string& extra_id);
// 候选项的显示名:目录中文标签优先,特效显示官方名,兜底为裸 ID(去 #编号)。
std::string CandidateDisplayName(const std::string& id) {
    std::string base = id;
    const auto hash_pos = base.find('#');
    if (hash_pos != std::string::npos) base.resize(hash_pos);
    const std::string effect = WhitelistExtraEffectDisplay(id);
    if (!effect.empty()) return effect;
    for (const auto& option : g_context.customization_options) {
        if (option.kind == nte_vehicle::kCustomizationModule &&
            (option.id == id || option.id == base)) {
            return option.label.empty() ? option.id : option.label;
        }
    }
    return base;
}
// 候选项的档位模式:去掉车辆前缀后的档位段(如 "WheelSmoke_10")。全局
// 范围勾选按此匹配 — 一套勾选适配所有车的自有选项 ID。
std::string CandidateTierPattern(const std::string& id) {
    std::string base = id;
    const auto hash_pos = base.find('#');
    if (hash_pos != std::string::npos) base.resize(hash_pos);
    const auto wheel_pos = base.find("Wheel");
    if (wheel_pos == std::string::npos) return base;
    return base.substr(wheel_pos);
}
void CheckDriftRotation() {
    if (!g_context.started.load(std::memory_order_acquire)) return;
    const SHORT shift_state = GetAsyncKeyState(VK_SHIFT);
    const bool pressed = (shift_state & 0x8000) != 0;
    const bool was = (g_context.drift_prev_shift & 0x80) != 0;
    g_context.drift_prev_shift = pressed ? 0x80 : 0x00;
    if (pressed && !was &&
        (g_context.drift_smoke_on || g_context.drift_mark_on)) {
        // 0.11.27: 0.3 秒延迟按用户要求移除 — 按 Shift 立即轮换。
        std::string applied_smoke;
        std::string applied_mark;
        {
            std::scoped_lock lock(g_context.mutex);
            const auto build_candidates = [&](std::uint32_t type,
                    std::vector<std::string>& out) {
                for (const auto& option : g_context.customization_options) {
                    if (option.kind == nte_vehicle::kCustomizationModule &&
                        option.module_type == type) {
                        out.push_back(option.id);
                    }
                }
                const std::string key =
                    !g_context.realtime_vehicle_id.empty()
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
            // 全局范围:勾选存档位模式("WheelSmoke_10" 等),一套勾选适配
            // 所有车;空范围 = 全部参与。
            const auto rotate_type = [&](std::uint32_t type, bool enabled,
                    bool random_mode, const std::vector<std::string>& scope,
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
                // 0.11.31: 轮换模式 — 顺序(列表循环,从当前装备件继续)或
                // 纯随机(均匀分布,列表 >1 时尽量避开连续重复)。烟雾/痕迹
                // 各自独立的模式开关(0.11.33)。
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
            rotate_type(23, g_context.drift_smoke_on,
                g_context.drift_smoke_random, g_context.drift_smoke_scope,
                g_context.drift_smoke_index, applied_smoke);
            rotate_type(24, g_context.drift_mark_on,
                g_context.drift_mark_random, g_context.drift_mark_scope,
                g_context.drift_mark_index, applied_mark);
        }
        if (applied_smoke.empty() && applied_mark.empty()) return;
        g_context.apply_current_modules_requested.store(true,
            std::memory_order_release);
        std::string message = "漂移轮换 →";
        if (!applied_smoke.empty()) {
            message += " 烟雾: " + CandidateDisplayName(applied_smoke);
        }
        if (!applied_mark.empty()) {
            message += " 痕迹: " + CandidateDisplayName(applied_mark);
        }
        SetStatus(message);
    }
}

// Local feature (ported from the 2.6 v3.2 plugin): full-keyboard per-vehicle
// summon hotkeys. Behaviour matches 2.6: a bound key summons the bound vehicle
// with its saved customization; binding mode captures the next key press.
// Runs on the plugin update (Game) thread, same as in 2.6.
void CheckHotkeyKeys() {
    if (!g_context.started.load(std::memory_order_acquire)) return;
    for (int vk = 0x08; vk < 0xFE; ++vk) {
        const SHORT state = GetAsyncKeyState(vk);
        const bool pressed = (state & 0x8000) != 0;
        const bool was = (g_context.prev_key_state[vk] & 0x80) != 0;
        g_context.prev_key_state[vk] = pressed ? 0x80 : 0x00;
        if (!pressed || was) continue;

        bool binding = false;
        std::string sel;
        std::string bound;
        {
            std::scoped_lock lock(g_context.mutex);
            binding = g_context.hotkey_binding_mode;
            sel = g_context.selected_vehicle_id;
            const auto it = g_context.hotkey_vehicle_bindings.find(
                static_cast<std::uint32_t>(vk));
            if (it != g_context.hotkey_vehicle_bindings.end()) bound = it->second;
        }
        if (binding) {
            if (!sel.empty()) {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.hotkey_vehicle_bindings[static_cast<std::uint32_t>(vk)] = sel;
                    g_context.hotkey_binding_mode = false;
                }
                SaveCustomizationMemory();
                SetLastSummon("已绑定 [键值 " + std::to_string(vk) + "] -> " + sel);
            }
            return;
        }
        if (!bound.empty()) {
            if (bound == sel) {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.hotkey_summon_vehicle_id = bound;
                }
                g_context.hotkey_summon_requested.store(true, std::memory_order_release);
            } else {
                // First press on a different vehicle: switch the selection to it;
                // the pending-vehicle flow reloads its customization (and memory).
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.pending_vehicle_id = bound;
                }
                SetLastSummon("已选 " + bound + " (再按一次召唤)");
            }
        }
    }
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
    // Local fix: count entries skipped because they are unreadable or invalid, so the
    // stability check below still detects real mid-read table changes.
    std::uint32_t skipped_entries = 0;
    for (std::uint32_t index = 0; index < before.entry_count; ++index) {
        nte_vehicle::CustomizationEntry native_option{};

        const auto option_status = vehicle->VehicleCustomizationOptionAt(anomaly::sdk::StringView(selected_id), index, &native_option);
        if (option_status.code != ANOMALY_STATUS_V1_OK ||
            (native_option.flags & nte_vehicle::kOptionValid) == 0) {
            // Local fix: skip the single unreadable entry instead of dropping the
            // whole catalog; the stability check still guards mid-read changes.
            ++skipped_entries;
            continue;
        }
        if (native_option.option_id.empty() ||
            (native_option.kind != nte_vehicle::kCustomizationModule &&
             native_option.kind != nte_vehicle::kCustomizationSkin) ||
            (native_option.kind == nte_vehicle::kCustomizationModule &&
             (native_option.module_type == 0 || native_option.module_type >= 28)) ||
            (native_option.kind == nte_vehicle::kCustomizationSkin &&
             native_option.module_type != 0)) {
            // Local fix: one broken row used to wipe the whole catalog. Skip the single
            // entry (counted against the stability check) and keep the rest.
            ++skipped_entries;
            continue;
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
    // Local diagnostics: rows the backend dropped because their VehicleID FName did
    // not exactly match this vehicle (comparison index + number). Appended to
    // catalog-dropped-rows.txt — the evidence file for the Pendragon-style
    // "the shop shows more options than the plugin" problem.
    {
        auto dropped = vehicle->TakeVehicleCustomizationDroppedNotes();
        if (!dropped.empty()) {
            const std::string dropped_path = [] {
                if (g_context.state_directory.empty()) return std::string{};
                std::string path = g_context.state_directory;
                if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
                path += "catalog-dropped-rows.txt";
                return path;
            }();
            if (!dropped_path.empty()) {
                std::ofstream dropped_out(dropped_path, std::ios::binary | std::ios::app);
                if (dropped_out) {
                    dropped_out << "# " << selected_id << "\n";
                    for (const auto& note : dropped) dropped_out << note << "\n";
                }
            }
        }
    }
    // Local diagnostics: ExtraModuleIds captured per kept row — the shop may expand
    // these into additional options; stashed and written into catalog-dump.txt below.
    std::vector<std::string> extra_notes =
        vehicle->TakeVehicleCustomizationExtraNotes();
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & nte_vehicle::kCatalogValid) == 0 ||
        after.sequence != before.sequence ||
        after.entry_count != options.size() + skipped_entries) {
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
        // Local feature: restore the saved customization for this vehicle so module
        // and skin choices survive a restart (2.6 v3.2 behaviour). Saved entries are
        // only applied when they still exist in the current catalog.
        auto saved_it = g_context.saved_customizations.find(selected_id);
        if (saved_it != g_context.saved_customizations.end()) {
            const std::string& saved_skin = saved_it->second.skin_id;
            bool skin_known = saved_skin.empty();
            if (!skin_known) {
                for (const auto& o : g_context.customization_options) {
                    if (o.kind == nte_vehicle::kCustomizationSkin && o.id == saved_skin) {
                        skin_known = true;
                        break;
                    }
                }
            }
            if (skin_known) g_context.customization_skin_id = saved_skin;
            for (const auto& m : saved_it->second.module_overrides) {
                // Local fix: backfill memory entries that exist in the catalog OR
                // are whitelisted as extra modules (cloud-applied premiums,
                // legacy upgrades no longer local).
                // Local fix (0.11.22): saved entries may carry a "#N" FName number
                // (read back from CurrentModuleIDList) while catalog option ids are
                // plain text — compare the base name too, otherwise those entries
                // never backfill ("sometimes the memory does not restore").
                bool known = false;
                std::string saved_base = m.second;
                const auto saved_hash = saved_base.find('#');
                if (saved_hash != std::string::npos) saved_base.resize(saved_hash);
                for (const auto& o : g_context.customization_options) {
                    if (o.kind == nte_vehicle::kCustomizationModule &&
                        o.module_type == m.first &&
                        (o.id == m.second || o.id == saved_base)) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    for (const auto& extra : g_context.allowed_extra_modules) {
                        if (extra.first == m.first && extra.second == m.second) {
                            known = true;
                            break;
                        }
                    }
                }
                if (known) g_context.selected_module_overrides.push_back(m);
            }
        }
        // Local feature: dump the full customization catalog after every successful
        // read (same format the 2.6 v3.2 plugin wrote). This is the evidence file for
        // "options missing from the UI" analysis: entries the backend dropped never
        // appear here, entries present here but not in the menu are UI-side.
        {
            const std::string dump_path = CatalogDumpPath();
            const std::string stamp_path =
                StampSidecarPath("catalog-dump");
            std::string dump_text;
            dump_text.append("Vehicle=").append(selected_id)
                .append(" entries=").append(std::to_string(g_context.customization_options.size()))
                .append(" skipped=").append(std::to_string(skipped_entries)).append("\n");
            for (const auto& o : g_context.customization_options) {
                dump_text.append(o.kind == nte_vehicle::kCustomizationSkin ? "SKIN" : "MODULE")
                    .append("|type=").append(std::to_string(o.module_type))
                    .append("|default=").append(o.is_default ? "1" : "0")
                    .append("|").append(o.id).append("|")
                    .append(o.label.empty() ? o.id : o.label).append("\n");
            }
            for (const auto& note : extra_notes) {
                dump_text.append("EXTRA|").append(note).append("\n");
            }
            WriteTextFileAtomic(dump_path, dump_text);
            WriteTextFileAtomic(stamp_path, dump_text);
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
    // 0.11.32: gated by the user-facing toggle (耐久自动保持:开/关)。
    if (g_context.durability_auto_full.load(std::memory_order_acquire)) {
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

    // Local diagnostics: full DataTable inventory sweep. Heavy (registry-wide);
    // runs once per explicit request and may stall the game for a few seconds.
    if (g_context.sweep_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            std::string sweep_text;
            std::string sweep_error;
            SetStatus("正在扫描全部数据表…(游戏可能卡顿数秒)");
            if (g_context.vehicle->SweepAllDataTables(&sweep_text, &sweep_error)) {
                const std::string sweep_path =
                    StampSidecarPath("datatables-inventory");
                if (!sweep_path.empty()) {
                    WriteTextFileAtomic(sweep_path,
                        std::string("\xEF\xBB\xBF") + sweep_text);
                    SetStatus("数据表普查完成:" + std::to_string(sweep_text.size() / 1024) +
                              " KB → " + sweep_path.substr(sweep_path.find_last_of('\\') + 1));
                } else {
                    SetStatus("数据表普查完成,但 state 目录不可用,未能落盘");
                }
            } else {
                SetStatus("数据表普查失败:" + sweep_error);
            }
        } else {
            SetStatus("数据表普查失败:载具后端不可用");
        }
    }

    // Local feature: read the module IDs currently applied on the driven vehicle
    // (server-replicated CurrentModuleIDList — reveals cloud-applied premiums).
    if (g_context.read_modules_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            std::vector<std::pair<std::uint32_t, std::string>> module_pairs;
            std::string modules_error;
            if (g_context.vehicle->ReadCurrentVehicleModuleList(&module_pairs,
                    &modules_error)) {
                std::string modules_text =
                    "modules=" + std::to_string(module_pairs.size()) + "\n";
                for (const auto& entry : module_pairs) {
                    modules_text.append("  ").append(entry.second);
                    if (entry.first != 0) {
                        modules_text.append("  (type ")
                            .append(std::to_string(entry.first))
                            .append(")");
                    }
                    modules_text.append("\n");
                }
                const std::string modules_path =
                    StampSidecarPath("current-vehicle-modules");
                WriteTextFileAtomic(modules_path,
                    std::string("\xEF\xBB\xBF") + modules_text);
                // Keep the pairs for the 改装调校 tab's on-screen display.
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.current_vehicle_modules = module_pairs;
                    g_context.current_vehicle_modules_valid = true;
                }
                std::string flat;
                for (const char c : modules_text) {
                    flat.push_back(c == '\n' ? ' ' : c);
                }
                SetStatus("当前车辆模块: " + flat + " → 已存 " +
                          modules_path.substr(modules_path.find_last_of('\\') + 1));
            } else {
                SetStatus("读取当前车辆模块失败:" + modules_error);
            }
        } else {
            SetStatus("读取当前车辆模块失败:载具后端不可用");
        }
    }

    // Local feature: capture the driven vehicle's current modules as that
    // vehicle's plugin memory, so 改装召唤 can reproduce this exact state.
    if (g_context.save_modules_to_memory_requested.exchange(false,
            std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            std::vector<std::pair<std::uint32_t, std::string>> module_pairs;
            std::string modules_error;
            if (g_context.vehicle->ReadCurrentVehicleModuleList(&module_pairs,
                    &modules_error)) {
                // SetStatus must stay OUTSIDE the mutex (it locks it itself for
                // the log ring — this block was a latent deadlock since 0.11.13).
                std::string status_message;
                {
                    std::scoped_lock lock(g_context.mutex);
                    const std::string target = g_context.selected_vehicle_id;
                    if (!target.empty()) {
                        auto& saved = g_context.saved_customizations[target];
                        saved.module_overrides.clear();
                        std::uint32_t kept = 0;
                        for (const auto& entry : module_pairs) {
                            if (entry.first == 0) continue;
                            saved.module_overrides.push_back(entry);
                            ++kept;
                        }
                        SaveCustomizationMemory();
                        status_message = "已将当前车辆状态(" + std::to_string(kept) +
                                         " 件)保存为 " + target + " 的记忆";
                    } else {
                        status_message = "保存失败:当前没有选中的车辆";
                    }
                }
                SetStatus(status_message);
            } else {
                SetStatus("保存失败:" + modules_error);
            }
        } else {
            SetStatus("保存失败:载具后端不可用");
        }
    }

    // Local feature: apply the selected overrides onto the currently driven
    // vehicle (client-side visual rebuild via OnRep_CurrentModuleIDList). The
    // server loadout is untouched; the server restores its own list on the next
    // replication (relog / shop action / re-summon).
    if (g_context.apply_current_modules_requested.exchange(false,
            std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            std::vector<std::pair<std::uint32_t, std::string>> overrides;
            std::string fallback_vehicle_id;
            {
                std::scoped_lock lock(g_context.mutex);
                overrides = g_context.selected_module_overrides;
                fallback_vehicle_id = g_context.selected_vehicle_id;
            }
            if (overrides.empty()) {
                SetStatus("当前没有已选改装件(先在改装召唤页选件)");
            } else {
                std::string applied_text;
                std::string apply_error;
                if (g_context.vehicle->ApplyOverridesToCurrentVehicle(
                        overrides, fallback_vehicle_id, &applied_text,
                        &apply_error)) {
                    std::string flat;
                    for (const char c : applied_text) {
                        flat.push_back(c == '\n' ? ' ' : c);
                    }
                    SetStatus("已应用到当前载具: " + flat);
                } else {
                    SetStatus("应用到当前载具失败:" + apply_error);
                }
            }
        } else {
            SetStatus("应用失败:载具后端不可用");
        }
    }

    // Local feature: capture the driven vehicle's equipped premium effects into
    // allowed-extra-modules.cfg at the FName level (raw pool base + raw number),
    // then derive the twin effect (smoke <-> mark) for the same car with the same
    // number — the two have always taken effect as a pair on these cars.
    if (g_context.capture_effects_requested.exchange(false,
            std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr && !g_context.state_directory.empty()) {
            std::vector<nte_vehicle::Runtime::VehicleEffectEntry> effects;
            std::string effects_error;
            if (g_context.vehicle->ReadCurrentVehicleEffectEntries(&effects,
                    &effects_error)) {
                std::vector<nte_vehicle::Runtime::VehicleEffectEntry> captured =
                    effects;
                const auto has_type = [&captured](std::uint32_t type) {
                    for (const auto& entry : captured) {
                        if (entry.type == type) return true;
                    }
                    return false;
                };
                // Derive the twin: 霓虹域界 (smoke) <-> 拟真态火焰 (mark), same
                // base pattern and same FName number, per the user's verified
                // experience that the two apply together.
                if (has_type(23) && !has_type(24)) {
                    for (const auto& entry : effects) {
                        if (entry.type != 23) continue;
                        nte_vehicle::Runtime::VehicleEffectEntry twin;
                        twin.type = 24;
                        twin.number = entry.number;
                        const auto smoke_pos = entry.base.find("WheelSmoke");
                        if (smoke_pos == std::string::npos) continue;
                        twin.base = entry.base;
                        twin.base.replace(smoke_pos, 10, "WheelMark");
                        captured.push_back(std::move(twin));
                        break;
                    }
                } else if (has_type(24) && !has_type(23)) {
                    for (const auto& entry : effects) {
                        if (entry.type != 24) continue;
                        nte_vehicle::Runtime::VehicleEffectEntry twin;
                        twin.type = 23;
                        twin.number = entry.number;
                        const auto mark_pos = entry.base.find("WheelMark");
                        if (mark_pos == std::string::npos) continue;
                        twin.base = entry.base;
                        twin.base.replace(mark_pos, 9, "WheelSmoke");
                        captured.push_back(std::move(twin));
                        break;
                    }
                }
                if (captured.empty()) {
                    SetStatus("当前车清单里没有 WheelSmoke_10/WheelMark_10 特效件"
                              "(若云游戏重登后读取仍没有,说明特效已不在服务器清单上)");
                } else {
                    std::string whitelist_path = g_context.state_directory;
                    if (whitelist_path.back() != '\\' &&
                        whitelist_path.back() != '/') {
                        whitelist_path.push_back('\\');
                    }
                    whitelist_path += "allowed-extra-modules.cfg";
                    std::vector<std::string> kept_lines;
                    {
                        std::ifstream in(whitelist_path, std::ios::binary);
                        std::string line;
                        while (in && std::getline(in, line)) {
                            while (!line.empty() &&
                                   (line.back() == '\r' || line.back() == '\n' ||
                                    line.back() == ' ')) {
                                line.pop_back();
                            }
                            if (line.empty()) continue;
                            kept_lines.push_back(line);
                        }
                    }
                    std::uint32_t added = 0;
                    std::uint32_t replaced = 0;
                    for (const auto& cap : captured) {
                        // "type|base" or "type|base#number" — reconstructs the
                        // exact FName the game itself reports for this car.
                        std::string line = std::to_string(cap.type) + "|" + cap.base;
                        if (cap.number != 0) {
                            line += "#" + std::to_string(cap.number);
                        }
                        bool exists = false;
                        for (std::size_t k = 0; k < kept_lines.size(); ++k) {
                            if (kept_lines[k] == line) {
                                exists = true;
                                break;
                            }
                            // Purge stale same-base lines with a different (or
                            // missing) number — keep exactly one per base.
                            const auto pipe = kept_lines[k].find('|');
                            const std::string kept_id = pipe == std::string::npos
                                ? std::string{} : kept_lines[k].substr(pipe + 1);
                            std::string kept_base = kept_id;
                            const auto kept_hash = kept_base.find('#');
                            if (kept_hash != std::string::npos) {
                                kept_base.resize(kept_hash);
                            }
                            if (kept_base == cap.base) {
                                kept_lines[k] = line;
                                exists = true;
                                ++replaced;
                                break;
                            }
                        }
                        if (!exists) {
                            kept_lines.push_back(line);
                            ++added;
                        }
                    }
                    if (added > 0 || replaced > 0) {
                        std::string file_text;
                        for (const auto& kept : kept_lines) {
                            file_text += kept + "\n";
                        }
                        WriteTextFileAtomic(whitelist_path, file_text);
                        // Hot-reload the in-memory whitelist (no restart needed).
                        std::vector<std::pair<std::uint32_t, std::string>> reloaded;
                        for (const auto& kept : kept_lines) {
                            const auto pipe = kept.find('|');
                            if (pipe == std::string::npos || pipe == 0) continue;
                            std::uint32_t type = 0;
                            try {
                                type = static_cast<std::uint32_t>(
                                    std::stoul(kept.substr(0, pipe)));
                            } catch (...) {
                                continue;
                            }
                            reloaded.emplace_back(type, kept.substr(pipe + 1));
                        }
                        {
                            std::scoped_lock lock(g_context.mutex);
                            g_context.allowed_extra_modules = reloaded;
                            g_context.vehicle->SetAllowedExtraModules(
                                std::move(reloaded));
                        }
                    }
                    SetStatus("特效收录完成:捕获 " + std::to_string(effects.size()) +
                              " 条,新增 " + std::to_string(added) + " 条,更新 " +
                              std::to_string(replaced) + " 条(含特效成对推导)"
                              "——每辆车坐进去点一次即可");
                }
            } else {
                SetStatus("特效收录失败:" + effects_error);
            }
        } else {
            SetStatus("特效收录失败:后端未就绪或 state 目录不可用");
        }
    }

    // Local feature: sync the plugin's vehicle selection to the driven vehicle so
    // the real-time picker (改装调校页) shows that car's catalog and effects.
    if (g_context.realtime_sync_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            std::string vehicle_id;
            std::string id_error;
            if (g_context.vehicle->ReadCurrentVehicleCatalogId(&vehicle_id,
                    &id_error)) {
                bool in_catalog = false;
                {
                    std::scoped_lock lock(g_context.mutex);
                    for (const auto& id : g_context.vehicle_ids) {
                        if (id == vehicle_id) {
                            in_catalog = true;
                            break;
                        }
                    }
                }
                if (in_catalog) {
                    {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.realtime_vehicle_id = vehicle_id;
                        if (g_context.selected_vehicle_id != vehicle_id) {
                            // Existing selection machinery: Update() consumes
                            // pending_vehicle_id, selects the vehicle and (with
                            // customization_page set) reloads its catalog.
                            g_context.pending_vehicle_id = vehicle_id;
                            g_context.customization_page = true;
                        } else if (g_context.customization_vehicle_id != vehicle_id) {
                            g_context.customization_vehicle_id.clear();
                            g_context.customization_options.clear();
                            g_context.refresh_customization_requested.store(
                                true, std::memory_order_release);
                        }
                    }
                    SetStatus("已同步到当前载具 " + vehicle_id +
                              " —— 下方即为该车的可改装项,选好后点应用");
                } else {
                    {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.realtime_vehicle_id.clear();
                    }
                    // SetStatus must stay OUTSIDE the mutex (it locks it itself
                    // for the log ring — recursive lock would deadlock).
                    SetStatus("当前载具 " + vehicle_id +
                              " 不在车辆列表里(目录未拉取?先在游戏里开一次它的改装店,再点"
                              "「重新读取改装数据」)");
                }
            } else {
                SetStatus("同步失败:" + id_error);
            }
        } else {
            SetStatus("同步失败:载具后端不可用");
        }
    }

    // Local feature: export the log ring so the user can share every status and
    // error verbatim.
    if (g_context.export_log_requested.exchange(false, std::memory_order_acq_rel)) {
        if (!g_context.state_directory.empty()) {
            std::vector<std::string> log_copy;
            {
                std::scoped_lock lock(g_context.mutex);
                log_copy = g_context.log_lines;
            }
            std::string text = "NteVehicle 改装版 日志导出\n";
            text += "条数: " + std::to_string(log_copy.size()) + "\n\n";
            for (const auto& line : log_copy) {
                text += line + "\n";
            }
            const std::string log_path = StampSidecarPath("plugin-log");
            WriteTextFileAtomic(log_path, std::string("\xEF\xBB\xBF") + text);
            SetStatus("日志已导出(" + std::to_string(log_copy.size()) +
                      " 条)→ 完整路径: " + log_path);
        } else {
            SetStatus("日志导出失败:state 目录不可用");
        }
    }

    // Local feature: advanced powertrain tuning read/apply.
    // Local feature: advanced powertrain tuning — per-tick live read so the
    // sliders track the driven vehicle's real values (auto re-sync on vehicle
    // change; cheap thanks to the runtime's offset cache).
    if (g_context.vehicle != nullptr) {
        nte_vehicle::Runtime::VehicleAdvancedTuning live{};
        std::string live_error;
        if (g_context.vehicle->VehicleReadAdvancedTuning(&live, &live_error) &&
            live.valid) {
            bool adopt = false;
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.adv_live_linear = live.max_linear_velocity_kmh;
                g_context.adv_live_reverse = live.reverse_top_speed_kmh;
                g_context.adv_live_up = live.change_up_rpm;
                g_context.adv_live_down = live.change_down_rpm;
                g_context.adv_live_time = live.gear_change_time;
                g_context.adv_live_ratio = live.final_ratio;
                g_context.adv_live_eff = live.transmission_efficiency;
                g_context.adv_live_c_up = live.comfort_change_up_rpm;
                g_context.adv_live_c_down = live.comfort_change_down_rpm;
                g_context.adv_live_c_time = live.comfort_gear_change_time;
                g_context.adv_live_c_ratio = live.comfort_final_ratio;
                g_context.adv_live_c_eff = live.comfort_transmission_efficiency;
                if (g_context.adv_synced_movement != live.movement) {
                    g_context.adv_synced_movement = live.movement;
                    adopt = true;
                }
            }
            if (adopt) {
                // New (or first-seen) vehicle: sliders jump to its real values
                // (both modes), the values are captured as this session's default
                // baseline, and a saved tune (if any) is auto-loaded — the memory
                // key falls back to the plugin's selected vehicle for
                // plugin-summoned cars (their actors carry no VehicleID).
                std::string vehicle_id;
                std::string id_error;
                if (!g_context.vehicle->ReadCurrentVehicleCatalogId(&vehicle_id,
                        &id_error)) {
                    std::scoped_lock lock(g_context.mutex);
                    vehicle_id = g_context.selected_vehicle_id;
                }
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.adv_linear_top_speed = live.max_linear_velocity_kmh;
                    g_context.adv_reverse_top_speed = live.reverse_top_speed_kmh;
                    g_context.adv_change_up_rpm = live.change_up_rpm;
                    g_context.adv_down_rpm = live.change_down_rpm;
                    g_context.adv_gear_change_time = live.gear_change_time;
                    g_context.adv_final_ratio = live.final_ratio;
                    g_context.adv_transmission_efficiency =
                        live.transmission_efficiency;
                    g_context.adv_c_change_up_rpm = live.comfort_change_up_rpm;
                    g_context.adv_c_down_rpm = live.comfort_change_down_rpm;
                    g_context.adv_c_gear_change_time =
                        live.comfort_gear_change_time;
                    g_context.adv_c_final_ratio = live.comfort_final_ratio;
                    g_context.adv_c_transmission_efficiency =
                        live.comfort_transmission_efficiency;
                    g_context.adv_baseline_linear = live.max_linear_velocity_kmh;
                    g_context.adv_baseline_reverse = live.reverse_top_speed_kmh;
                    g_context.adv_baseline_up = live.change_up_rpm;
                    g_context.adv_baseline_down = live.change_down_rpm;
                    g_context.adv_baseline_time = live.gear_change_time;
                    g_context.adv_baseline_ratio = live.final_ratio;
                    g_context.adv_baseline_eff = live.transmission_efficiency;
                    g_context.adv_baseline_c_up = live.comfort_change_up_rpm;
                    g_context.adv_baseline_c_down = live.comfort_change_down_rpm;
                    g_context.adv_baseline_c_time = live.comfort_gear_change_time;
                    g_context.adv_baseline_c_ratio = live.comfort_final_ratio;
                    g_context.adv_baseline_c_eff =
                        live.comfort_transmission_efficiency;
                    g_context.adv_baseline_valid = true;
                    // 0.11.30: capture the factory top speed at boarding —
                    // BEFORE any plugin write (the saved-tune ratio may rewrite
                    // the field right after) — so the km/h slider has a stable
                    // 1.0x anchor.
                    float stock_kmh = 0;
                    if (g_context.vehicle->VehicleCaptureStockTopSpeed() &&
                        g_context.vehicle->VehicleGetStockTopSpeedKmh(&stock_kmh)) {
                        g_context.adv_stock_top_speed_kmh = stock_kmh;
                        g_context.adv_stock_valid = true;
                    } else {
                        g_context.adv_stock_valid = false;
                    }
                    // 0.11.26: 上车即自动执行「读取当前载具改装(并同步选车)」,
                    // 无需再点按钮(与同步按钮完全相同的两个动作)。
                    g_context.read_modules_requested.store(true,
                        std::memory_order_release);
                    g_context.realtime_sync_requested.store(true,
                        std::memory_order_release);
                }
                nte_vehicle::Runtime::VehicleAdvancedTuning saved{};
                bool have_saved = false;
                float saved_speed_ratio = 0;
                float saved_torque_ratio = 0;
                float saved_other_speed_ratio = 0;
                float saved_other_torque_ratio = 0;
                {
                    std::scoped_lock lock(g_context.mutex);
                    const auto found = g_context.saved_tunes.find(vehicle_id);
                    if (found != g_context.saved_tunes.end()) {
                        // -1 entries (legacy) stay negative → the write skips them.
                        saved.max_linear_velocity_kmh = found->second[0];
                        saved.change_up_rpm = found->second[1];
                        saved.change_down_rpm = found->second[2];
                        saved.gear_change_time = found->second[3];
                        saved.final_ratio = found->second[4];
                        saved.transmission_efficiency = found->second[5];
                        saved.comfort_change_up_rpm = found->second[6];
                        saved.comfort_change_down_rpm = found->second[7];
                        saved.comfort_gear_change_time = found->second[8];
                        saved.comfort_final_ratio = found->second[9];
                        saved.comfort_transmission_efficiency = found->second[10];
                        saved_speed_ratio = found->second[11];
                        saved_torque_ratio = found->second[12];
                        saved_other_speed_ratio = found->second[13];
                        saved_other_torque_ratio = found->second[14];
                        saved.reverse_top_speed_kmh = found->second[15];
                        saved.valid = true;
                        have_saved = true;
                    }
                }
                if (have_saved) {
                    std::string load_error;
                    bool ok = g_context.vehicle->VehicleWriteAdvancedTuning(saved,
                        false, &load_error);
                    if (ok) {
                        ok = g_context.vehicle->VehicleWriteAdvancedTuning(saved,
                            true, &load_error);
                    }
                    if (ok) {
                        {
                            std::scoped_lock lock(g_context.mutex);
                            if (saved.max_linear_velocity_kmh >= 0.0F) {
                                g_context.adv_linear_top_speed =
                                    saved.max_linear_velocity_kmh;
                            }
                            if (saved.reverse_top_speed_kmh >= 0.0F) {
                                g_context.adv_reverse_top_speed =
                                    saved.reverse_top_speed_kmh;
                            }
                            if (saved.change_up_rpm >= 0.0F) {
                                g_context.adv_change_up_rpm = saved.change_up_rpm;
                            }
                            if (saved.change_down_rpm >= 0.0F) {
                                g_context.adv_down_rpm = saved.change_down_rpm;
                            }
                            if (saved.gear_change_time >= 0.0F) {
                                g_context.adv_gear_change_time =
                                    saved.gear_change_time;
                            }
                            if (saved.final_ratio >= 0.0F) {
                                g_context.adv_final_ratio = saved.final_ratio;
                            }
                            if (saved.transmission_efficiency >= 0.0F) {
                                g_context.adv_transmission_efficiency =
                                    saved.transmission_efficiency;
                            }
                            if (saved.comfort_change_up_rpm >= 0.0F) {
                                g_context.adv_c_change_up_rpm =
                                    saved.comfort_change_up_rpm;
                            }
                            if (saved.comfort_change_down_rpm >= 0.0F) {
                                g_context.adv_c_down_rpm =
                                    saved.comfort_change_down_rpm;
                            }
                            if (saved.comfort_gear_change_time >= 0.0F) {
                                g_context.adv_c_gear_change_time =
                                    saved.comfort_gear_change_time;
                            }
                            if (saved.comfort_final_ratio >= 0.0F) {
                                g_context.adv_c_final_ratio =
                                    saved.comfort_final_ratio;
                            }
                            if (saved.comfort_transmission_efficiency >= 0.0F) {
                                g_context.adv_c_transmission_efficiency =
                                    saved.comfort_transmission_efficiency;
                            }
                            if (saved_speed_ratio > 0.0F) {
                                g_context.speed_ratio = saved_speed_ratio;
                                g_context.apply_speed.store(true,
                                    std::memory_order_release);
                            }
                            if (saved_torque_ratio > 0.0F) {
                                g_context.engine_torque_ratio = saved_torque_ratio;
                                g_context.apply_torque.store(true,
                                    std::memory_order_release);
                            }
                            if (saved_other_speed_ratio > 0.0F) {
                                g_context.other_speed_ratio =
                                    saved_other_speed_ratio;
                                g_context.apply_other_speed.store(true,
                                    std::memory_order_release);
                            }
                            if (saved_other_torque_ratio > 0.0F) {
                                g_context.other_torque_ratio =
                                    saved_other_torque_ratio;
                                g_context.apply_other_torque.store(true,
                                    std::memory_order_release);
                            }
                        }
                        SetStatus("已自动加载 " + vehicle_id +
                                  " 的保存调校(tuning-memory.cfg,含双模式与倍率)");
                        } else {
                            SetStatus("自动加载保存调校失败:" + load_error);
                        }
                    }
                }
                // 0.11.30: derive the km/h slider target AFTER any auto-loaded
                // ratio — target = ratio × captured factory top speed.
                {
                    float stock_kmh = 0;
                    if (g_context.vehicle->VehicleGetStockTopSpeedKmh(&stock_kmh)) {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.adv_speed_target_kmh =
                            g_context.speed_ratio * stock_kmh;
                    }
                }
            }
    }
    if (g_context.adv_read_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            nte_vehicle::Runtime::VehicleAdvancedTuning tuning{};
            std::string tuning_error;
            if (g_context.vehicle->VehicleReadAdvancedTuning(&tuning, &tuning_error)) {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.adv_linear_top_speed = tuning.max_linear_velocity_kmh;
                    g_context.adv_reverse_top_speed = tuning.reverse_top_speed_kmh;
                    g_context.adv_change_up_rpm = tuning.change_up_rpm;
                    g_context.adv_down_rpm = tuning.change_down_rpm;
                    g_context.adv_gear_change_time = tuning.gear_change_time;
                    g_context.adv_final_ratio = tuning.final_ratio;
                    g_context.adv_transmission_efficiency =
                        tuning.transmission_efficiency;
                    g_context.adv_c_change_up_rpm = tuning.comfort_change_up_rpm;
                    g_context.adv_c_down_rpm = tuning.comfort_change_down_rpm;
                    g_context.adv_c_gear_change_time =
                        tuning.comfort_gear_change_time;
                    g_context.adv_c_final_ratio = tuning.comfort_final_ratio;
                    g_context.adv_c_transmission_efficiency =
                        tuning.comfort_transmission_efficiency;
                }
                // SetStatus stays outside the mutex (it locks it for the log ring).
                SetStatus("已读取当前载具进阶参数(滑条已同步当前值)");
            } else {
                SetStatus("读取进阶参数失败:" + tuning_error);
            }
        } else {
            SetStatus("读取进阶参数失败:载具后端不可用");
        }
    }
    if (g_context.adv_apply_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            bool comfort_mode = false;
            nte_vehicle::Runtime::VehicleAdvancedTuning tuning{};
            {
                std::scoped_lock lock(g_context.mutex);
                comfort_mode = g_context.adv_comfort_mode;
                // 0.11.26: MaxLinearVelocity 实测无效(移动端字段另有、349 疑似
                // 游戏侧硬顶),停止写入 — -1 = 跳过。
                tuning.max_linear_velocity_kmh = -1.0F;
                tuning.reverse_top_speed_kmh = g_context.adv_reverse_top_speed;
                if (comfort_mode) {
                    tuning.change_up_rpm = g_context.adv_c_change_up_rpm;
                    tuning.change_down_rpm = g_context.adv_c_down_rpm;
                    tuning.gear_change_time = g_context.adv_c_gear_change_time;
                    tuning.final_ratio = g_context.adv_c_final_ratio;
                    tuning.transmission_efficiency =
                        g_context.adv_c_transmission_efficiency;
                } else {
                    tuning.change_up_rpm = g_context.adv_change_up_rpm;
                    tuning.change_down_rpm = g_context.adv_down_rpm;
                    tuning.gear_change_time = g_context.adv_gear_change_time;
                    tuning.final_ratio = g_context.adv_final_ratio;
                    tuning.transmission_efficiency =
                        g_context.adv_transmission_efficiency;
                }
            }
            tuning.valid = true;
            // 拖动即写:成功时静默(实时值行就是反馈),失败才报。
            std::string tuning_error;
            if (!g_context.vehicle->VehicleWriteAdvancedTuning(tuning,
                    comfort_mode, &tuning_error)) {
                SetStatus((comfort_mode ? "写入舒适传动失败:" : "写入普通传动失败:") +
                          tuning_error);
            }
        }
    }

    // Local feature: save the driven vehicle's CURRENT tuning values as that
    // vehicle's memory (tuning-memory.cfg; auto-loaded on every boarding).
    if (g_context.save_tune_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            std::string vehicle_id;
            std::string id_error;
            if (!g_context.vehicle->ReadCurrentVehicleCatalogId(&vehicle_id,
                    &id_error)) {
                // Plugin-summoned cars carry no VehicleID — fall back to the
                // plugin's selected vehicle (that is what was summoned).
                std::scoped_lock lock(g_context.mutex);
                vehicle_id = g_context.selected_vehicle_id;
            }
            std::array<float, 16> tune{};
            bool have_live = false;
            {
                std::scoped_lock lock(g_context.mutex);
                tune = {-1.0F, g_context.adv_live_up,
                    g_context.adv_live_down, g_context.adv_live_time,
                    g_context.adv_live_ratio, g_context.adv_live_eff,
                    g_context.adv_live_c_up, g_context.adv_live_c_down,
                    g_context.adv_live_c_time, g_context.adv_live_c_ratio,
                    g_context.adv_live_c_eff, g_context.speed_ratio,
                    g_context.engine_torque_ratio, g_context.other_speed_ratio,
                    g_context.other_torque_ratio, g_context.adv_live_reverse};
                have_live = g_context.adv_synced_movement != 0;
            }
            if (have_live && !vehicle_id.empty()) {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.saved_tunes[vehicle_id] = tune;
                    SaveTuningMemory();
                }
                SetStatus("已保存当前调校为 " + vehicle_id +
                          " 的记忆(双模式+四个倍率;以后每次上车自动加载;"
                          "文件 tuning-memory.cfg 与改装记忆同目录)");
            } else {
                SetStatus("保存调校失败:还没有读取到当前车的实时值(稍等一秒再试)");
            }
        } else {
            SetStatus("保存调校失败:载具后端不可用");
        }
    }

    // Local feature: restore the driven vehicle's default tuning (the values
    // first read this session, before any plugin write) — both modes, plus the
    // four ratios back to 1.0 and the other-vehicle floor back to its default.
    if (g_context.reset_tune_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            bool have_baseline = false;
            nte_vehicle::Runtime::VehicleAdvancedTuning defaults{};
            {
                std::scoped_lock lock(g_context.mutex);
                have_baseline = g_context.adv_baseline_valid;
                defaults.max_linear_velocity_kmh = g_context.adv_baseline_linear;
                defaults.reverse_top_speed_kmh = g_context.adv_baseline_reverse;
                defaults.change_up_rpm = g_context.adv_baseline_up;
                defaults.change_down_rpm = g_context.adv_baseline_down;
                defaults.gear_change_time = g_context.adv_baseline_time;
                defaults.final_ratio = g_context.adv_baseline_ratio;
                defaults.transmission_efficiency = g_context.adv_baseline_eff;
                defaults.comfort_change_up_rpm = g_context.adv_baseline_c_up;
                defaults.comfort_change_down_rpm = g_context.adv_baseline_c_down;
                defaults.comfort_gear_change_time = g_context.adv_baseline_c_time;
                defaults.comfort_final_ratio = g_context.adv_baseline_c_ratio;
                defaults.comfort_transmission_efficiency =
                    g_context.adv_baseline_c_eff;
            }
            defaults.valid = true;
            if (have_baseline) {
                std::string reset_error;
                bool ok = g_context.vehicle->VehicleWriteAdvancedTuning(defaults,
                    false, &reset_error);
                if (ok) {
                    ok = g_context.vehicle->VehicleWriteAdvancedTuning(defaults,
                        true, &reset_error);
                }
                if (ok) {
                    {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.adv_linear_top_speed =
                            defaults.max_linear_velocity_kmh;
                        g_context.adv_reverse_top_speed =
                            defaults.reverse_top_speed_kmh;
                        g_context.adv_change_up_rpm = defaults.change_up_rpm;
                        g_context.adv_down_rpm = defaults.change_down_rpm;
                        g_context.adv_gear_change_time =
                            defaults.gear_change_time;
                        g_context.adv_final_ratio = defaults.final_ratio;
                        g_context.adv_transmission_efficiency =
                            defaults.transmission_efficiency;
                        g_context.adv_c_change_up_rpm =
                            defaults.comfort_change_up_rpm;
                        g_context.adv_c_down_rpm =
                            defaults.comfort_change_down_rpm;
                        g_context.adv_c_gear_change_time =
                            defaults.comfort_gear_change_time;
                        g_context.adv_c_final_ratio =
                            defaults.comfort_final_ratio;
                        g_context.adv_c_transmission_efficiency =
                            defaults.comfort_transmission_efficiency;
                        g_context.speed_ratio = 1.0F;
                        g_context.engine_torque_ratio = 1.0F;
                        g_context.other_speed_ratio = 1.0F;
                        g_context.other_torque_ratio = 1.0F;
                        g_context.other_speed_floor_ratio = 0.6024F;
                        if (g_context.adv_stock_valid) {
                            // 0.11.30: 恢复默认后限速滑条回到 1.0x(原厂上限)。
                            g_context.adv_speed_target_kmh =
                                g_context.adv_stock_top_speed_kmh;
                        }
                        g_context.apply_speed.store(true,
                            std::memory_order_release);
                        g_context.apply_torque.store(true,
                            std::memory_order_release);
                        g_context.apply_other_speed.store(true,
                            std::memory_order_release);
                        g_context.apply_other_torque.store(true,
                            std::memory_order_release);
                    }
                    SetStatus("已恢复默认调校(双模式+倍率回 1.0+其他车下限回默认,"
                              "以本次启动首次读取的原厂值为准;保存的记忆保留,"
                              "重新上车会再次自动加载)");
                } else {
                    SetStatus("恢复默认调校失败:" + reset_error);
                }
            } else {
                SetStatus("没有默认基线(本次启动还没读取过这辆车的原厂值)"
                          "——重启游戏后上车一次即可建立");
            }
        } else {
            SetStatus("恢复默认调校失败:载具后端不可用");
        }
    }

    // Local feature: real-time skin application on the driven vehicle.
    if (g_context.apply_skin_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.vehicle != nullptr) {
            std::string skin_id;
            {
                std::scoped_lock lock(g_context.mutex);
                skin_id = g_context.realtime_skin_id;
            }
            if (skin_id.empty()) {
                SetStatus("当前没有选择涂装");
            } else {
                std::string skin_error;
                if (g_context.vehicle->ApplySkinToCurrentVehicle(skin_id,
                        &skin_error)) {
                    SetStatus("涂装已应用到当前载具: " + skin_id +
                              "(客户端视觉,重登回服务器状态)");
                } else {
                    SetStatus("应用涂装失败:" + skin_error);
                }
            }
        } else {
            SetStatus("应用涂装失败:载具后端不可用");
        }
    }

    // Local diagnostics: FName pool probe — which candidate effect IDs exist in
    // this client (read-only, no application).
    if (g_context.probe_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_context.names != nullptr && g_context.names->find_utf8 != nullptr) {
            static constexpr const char* kProbeCandidates[] = {
                "V016_WheelSmoke_10", "V016_WheelMark_10",
                "V017_WheelSmoke_10", "V017_WheelMark_10",
                "V021_WheelSmoke_10", "V021_WheelMark_10",
                "V026_WheelSmoke_10", "V026_WheelMark_10",
                "V037_WheelSmoke_10", "V037_WheelMark_10",
                "V038_WheelSmoke_10", "V038_WheelMark_10",
                "V060a_WheelSmoke_10", "V060a_WheelMark_10",
                "Vehicle_Common_WheelSmoke_09", "Vehicle_Common_WheelMark_09",
                "Vehicle_Common_WheelSmoke_10", "Vehicle_Common_WheelMark_10",
            };
            std::string report = "FName 探测(只读):\n";
            std::uint32_t found_count = 0;
            for (const auto* candidate : kProbeCandidates) {
                std::uint32_t name_id = 0;
                const bool exists = g_context.names->find_utf8(
                    g_context.names->user,
                    anomaly::sdk::StringView(std::string_view(candidate)),
                    &name_id).code == ANOMALY_STATUS_V1_OK && name_id != 0;
                report += "  ";
                report += candidate;
                report += exists ? " = 存在\n" : " = 不存在\n";
                if (exists) ++found_count;
            }
            report += "存在 " + std::to_string(found_count) + "/" +
                      std::to_string(std::size(kProbeCandidates)) + "\n";
            WriteTextFileAtomic(StampSidecarPath("fname-probe"),
                std::string("\xEF\xBB\xBF") + report);
            SetStatus("探测完成:" + std::to_string(found_count) + " 个候选 ID 存在 → fname-probe-*.txt");
        } else {
            SetStatus("探测失败:ue5.names 服务不可用");
        }
    }

    // Local feature: per-vehicle summon hotkeys (2.6 v3.2 behaviour).
    CheckHotkeyKeys();
    CheckDriftRotation();
    if (g_context.hotkey_summon_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string hv;
        bool has_custom = false;
        {
            std::scoped_lock lock(g_context.mutex);
            hv = g_context.hotkey_summon_vehicle_id;
            const auto it = g_context.saved_customizations.find(hv);
            if (it != g_context.saved_customizations.end() &&
                (!it->second.skin_id.empty() || !it->second.module_overrides.empty())) {
                has_custom = true;
            }
            if (!hv.empty()) {
                g_context.selected_vehicle_id = hv;
                if (has_custom) {
                    g_context.customization_vehicle_id = hv;
                    g_context.customization_skin_id = it->second.skin_id;
                    g_context.selected_module_overrides = it->second.module_overrides;
                }
            }
        }
        if (!hv.empty()) {
            if (has_custom) {
                g_context.summon_customized.store(true, std::memory_order_release);
            } else {
                g_context.summon.store(true, std::memory_order_release);
            }
        }
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
                    // Local feature: persist this vehicle's customization on a successful
                    // customized summon (2.6 v3.2 behaviour).
                    {
                        std::scoped_lock lock(g_context.mutex);
                        auto& saved = g_context.saved_customizations[selected_id];
                        saved.skin_id = selected_skin_id;
                        saved.module_overrides = module_overrides;
                    }
                    SaveCustomizationMemory();
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

// Local feature: module IDs carry the vehicle prefix (V026_* belongs to
// Vehicle026). Derive the prefix from a vehicle id so whitelist entries of
// other vehicles never render or apply on the wrong car.
std::string VehicleModulePrefixFromId(const std::string& vehicle_id) {
    std::string_view id{vehicle_id};
    if (id.size() <= 7) return {};
    static constexpr std::string_view kVehicleTag{"vehicle"};
    const auto lower = [](char c) {
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

// Only premium-effect whitelist entries render as named buttons
// (catalog evidence, 05 §13: *_WheelSmoke_10 = 霓虹域界,
// *_WheelMark_10 = 拟真态火焰); ordinary whitelisted parts are
// memory-backfill-only and never clutter the option lists.
std::string WhitelistExtraEffectDisplay(const std::string& extra_id) {
    std::string base = extra_id;
    const auto hash_pos = base.find('#');
    if (hash_pos != std::string::npos) base.resize(hash_pos);
    if (base.find("WheelSmoke_10") != std::string::npos) return "霓虹域界";
    if (base.find("WheelMark_10") != std::string::npos) return "拟真态火焰";
    return {};
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
        // Local feature: force the backend to rescan (otherwise a cached catalog
        // would hide unlock-driven data changes, e.g. newly unlocked effects).
        if (g_context.vehicle != nullptr) {
            g_context.vehicle->InvalidateVehicleCustomizationCache();
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
    // Local feature (ported from 2.6 v3.2): per-vehicle summon hotkeys. Placed
    // before the empty-options early returns so ANY selected vehicle can be bound
    // (a vehicle without customization rows still supports hotkey summoning).
    // 0.11.26: 「读取当前车辆已装模块」「探测候选特效 ID(只读)」已迁至诊断页。
    if (DrawButtonEnabled(ui, "保存当前车辆状态为该车记忆", true)) {
        g_context.save_modules_to_memory_requested.store(true, std::memory_order_release);
    }
    {
        bool binding_mode = false;
        std::string sel;
        {
            std::scoped_lock lock(g_context.mutex);
            binding_mode = g_context.hotkey_binding_mode;
            sel = g_context.selected_vehicle_id;
        }
        if (DrawButtonEnabled(ui,
                binding_mode ? ">> 按任意键绑定... <<" : "自定义快捷键召唤", true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.hotkey_binding_mode = !g_context.hotkey_binding_mode;
        }
        if (DrawButtonEnabled(ui, "清空当前车辆快捷键", true)) {
            bool removed = false;
            {
                std::scoped_lock lock(g_context.mutex);
                for (auto it = g_context.hotkey_vehicle_bindings.begin();
                     it != g_context.hotkey_vehicle_bindings.end();) {
                    if (it->second == sel) {
                        it = g_context.hotkey_vehicle_bindings.erase(it);
                        removed = true;
                    } else {
                        ++it;
                    }
                }
            }
            if (removed) {
                SaveCustomizationMemory();
                SetLastSummon("已清空 " + sel + " 的快捷键");
            }
        }
        {
            std::scoped_lock lock(g_context.mutex);
            for (const auto& b : g_context.hotkey_vehicle_bindings) {
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "  [键值 %u] -> %s",
                    b.first, b.second.c_str());
                DrawText(ui, buffer);
            }
        }
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
    // Local feature: whitelist extras are vehicle-specific — derive the prefix of
    // the selected vehicle (shared helpers, also used by the real-time picker).
    const std::string vehicle_module_prefix = VehicleModulePrefixFromId(selected);
    const auto extra_for_this_vehicle = [&vehicle_module_prefix](const std::string& extra_id) {
        return WhitelistExtraForVehicle(extra_id, vehicle_module_prefix);
    };
    // Local feature: snapshot the whitelist under the mutex (it can now be
    // appended at runtime by the effect-capture button while the render thread
    // is drawing).
    std::vector<std::pair<std::uint32_t, std::string>> whitelist_extras;
    {
        std::scoped_lock lock(g_context.mutex);
        whitelist_extras = g_context.allowed_extra_modules;
    }
    const auto extra_effect_display = [](const std::string& extra_id) -> std::string {
        return WhitelistExtraEffectDisplay(extra_id);
    };
    // Local feature: whitelisted extra IDs (cloud-applied premiums etc.) render as
    // selectable options even when their catalog rows are absent (transient rows).
    const auto has_extra_for_type = [&](const std::uint32_t type) {
        for (const auto& extra : whitelist_extras) {
            if (extra.first == type && extra_for_this_vehicle(extra.second) &&
                !extra_effect_display(extra.second).empty()) {
                return true;
            }
        }
        return false;
    };
    const auto has_options_for_type = [&](const std::uint32_t type) {
        return std::ranges::any_of(options, [type](const CustomizationOption& option) {
            return option.kind == nte_vehicle::kCustomizationModule &&
                option.module_type == type;
        }) || has_extra_for_type(type);
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
            // 0.11.22: mark the picked (and stock) option like the realtime page.
            if (explicit_selected) label = "[已选] " + label;
            else if (default_selected) label = "[默认] " + label;
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
        // Local feature: ONLY premium-effect whitelist entries render (named
        // buttons); ordinary whitelisted parts are memory-backfill-only.
        for (const auto& extra : whitelist_extras) {
            if (extra.first != type) continue;
            if (!extra_for_this_vehicle(extra.second)) continue;
            std::string extra_id = extra.second;
            const std::string display = extra_effect_display(extra_id);
            if (display.empty()) continue;
            // Local fix: suffix-aware dedupe — the catalog lists "V026_WheelSmoke_10"
            // while the whitelist entry may carry "#11"; same item, hide the extra
            // button so the two never render twice under one category.
            std::string extra_base = extra_id;
            const auto hash_pos = extra_base.find('#');
            if (hash_pos != std::string::npos) extra_base.resize(hash_pos);
            bool already_listed = false;
            for (const auto& option : options) {
                if (option.kind == nte_vehicle::kCustomizationModule &&
                    option.id == extra_base) {
                    already_listed = true;
                    break;
                }
            }
            if (already_listed) continue;
            const std::string_view extra_selected_id = override_for_type(type);
            std::string label = display;
            if (extra_selected_id == extra_id) label = "[已选] " + label;
            label += "##custom-extra-" + std::to_string(type) + "-" + extra_id;
            if (ui->button(ui->user, anomaly::sdk::StringView(label), list_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                auto& overrides = g_context.selected_module_overrides;
                const auto found = std::ranges::find_if(overrides,
                    [type](const auto& entry) { return entry.first == type; });
                if (found == overrides.end()) overrides.emplace_back(type, extra_id);
                else found->second = extra_id;
                g_context.customization_status = "已选 " +
                    (display.empty() ? extra_id : display + " (" + extra_id + ")") +
                    "（改装召唤时应用）";
                g_context.customization_status_failure = false;
            }
        }
    };

    const bool child_api_available =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
            sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
        ui->begin_child && ui->end_child;
    // 类别标签卡：整行居中，一行最多 5 个；召唤按钮在标签卡下方整行显示。
    std::array<std::uint32_t, 29> categories{};
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
    }

    // Local feature: real-time modification on the currently driven vehicle —
    // read its modules, pick options in place, apply (client-side visual).
    DrawDivider(ui);
    DrawText(ui, "实时改装(读取 → 本页选件 → 应用到当前载具;停车后应用)");
    {
        // 漂移轮换控件行:同步按钮缩宽,右侧为烟雾/痕迹的轮换开关与范围。
        float window_width = 0.0F;
        float window_height = 0.0F;
        const bool has_window =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, get_window_size),
                sizeof(decltype(AnomalyUiServiceV1::get_window_size))) &&
            ui->get_window_size != nullptr;
        if (has_window) {
            ui->get_window_size(ui->user, &window_width, &window_height);
        }
        const float row_width =
            window_width > 96.0F ? window_width - 32.0F : 640.0F;
        const float sync_width = row_width * 0.40F;
        const float drift_width = row_width * 0.142F;
        const bool has_same_line =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, same_line),
                sizeof(decltype(AnomalyUiServiceV1::same_line))) &&
            ui->same_line != nullptr;
        const bool has_button_enabled =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, button_enabled),
                sizeof(decltype(AnomalyUiServiceV1::button_enabled))) &&
            ui->button_enabled != nullptr;
        bool sync_clicked = false;
        if (has_button_enabled) {
            sync_clicked = ui->button_enabled(ui->user,
                anomaly::sdk::StringView("读取当前载具改装(并同步选车)"),
                sync_width, 0.0F, 1) != 0;
        } else {
            sync_clicked = ui->button(ui->user,
                anomaly::sdk::StringView("读取当前载具改装(并同步选车)"),
                sync_width, 0.0F) != 0;
        }
        if (sync_clicked) {
            g_context.read_modules_requested.store(true, std::memory_order_release);
            g_context.realtime_sync_requested.store(true, std::memory_order_release);
        }
        // 漂移轮换状态(全局,所有车通用;范围存档位模式)。
        bool smoke_on = false;
        bool mark_on = false;
        std::size_t smoke_count = 0;
        std::size_t mark_count = 0;
        std::vector<std::string> smoke_candidates;
        std::vector<std::string> mark_candidates;
        {
            std::scoped_lock lock(g_context.mutex);
            smoke_on = g_context.drift_smoke_on;
            mark_on = g_context.drift_mark_on;
            const std::string key =
                !g_context.realtime_vehicle_id.empty()
                    ? g_context.realtime_vehicle_id
                    : g_context.selected_vehicle_id;
            const std::string prefix = VehicleModulePrefixFromId(key);
            const auto build_candidates = [&](std::uint32_t type,
                    std::vector<std::string>& out) {
                for (const auto& option : g_context.customization_options) {
                    if (option.kind == nte_vehicle::kCustomizationModule &&
                        option.module_type == type) {
                        out.push_back(option.id);
                    }
                }
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
            build_candidates(23, smoke_candidates);
            build_candidates(24, mark_candidates);
            const auto count_checked = [&](
                const std::vector<std::string>& scope,
                const std::vector<std::string>& candidates) {
                if (scope.empty()) return candidates.size();
                std::size_t n = 0;
                for (const auto& id : candidates) {
                    const std::string pattern = CandidateTierPattern(id);
                    for (const auto& p : scope) {
                        if (p == pattern) {
                            ++n;
                            break;
                        }
                    }
                }
                return n;
            };
            smoke_count = count_checked(g_context.drift_smoke_scope,
                smoke_candidates);
            mark_count = count_checked(g_context.drift_mark_scope,
                mark_candidates);
        }
        const auto smoke_toggle_label = smoke_on
            ? "烟雾轮换:开" : "烟雾轮换:关";
        const auto mark_toggle_label = mark_on
            ? "痕迹轮换:开" : "痕迹轮换:关";
        const auto smoke_scope_label = "烟雾范围(" +
            std::to_string(smoke_count) + ")";
        const auto mark_scope_label = "痕迹范围(" +
            std::to_string(mark_count) + ")";
        if (has_same_line) ui->same_line(ui->user, 0.0F, 8.0F);
        if (ui->button(ui->user, anomaly::sdk::StringView(smoke_toggle_label),
                drift_width, 0.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_smoke_on = !smoke_on;
            SaveDriftConfig();
        }
        if (has_same_line) ui->same_line(ui->user, 0.0F, 6.0F);
        if (ui->button(ui->user, anomaly::sdk::StringView(smoke_scope_label),
                drift_width, 0.0F)) {
            g_context.drift_smoke_ui_open = !g_context.drift_smoke_ui_open;
        }
        if (has_same_line) ui->same_line(ui->user, 0.0F, 8.0F);
        if (ui->button(ui->user, anomaly::sdk::StringView(mark_toggle_label),
                drift_width, 0.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.drift_mark_on = !mark_on;
            SaveDriftConfig();
        }
        if (has_same_line) ui->same_line(ui->user, 0.0F, 6.0F);
        if (ui->button(ui->user, anomaly::sdk::StringView(mark_scope_label),
                drift_width, 0.0F)) {
            g_context.drift_mark_ui_open = !g_context.drift_mark_ui_open;
        }
        // 轮换模式按钮(0.11.33):分列在烟雾范围/痕迹范围按钮的正下方 —
        // 烟雾列管烟雾、痕迹列管痕迹,各自独立的随机/顺序切换。
        {
            const bool has_cursor_x =
                UiFieldAvailable(ui,
                    offsetof(AnomalyUiServiceV1, set_cursor_pos_x),
                    sizeof(decltype(AnomalyUiServiceV1::set_cursor_pos_x))) &&
                ui->set_cursor_pos_x != nullptr;
            const float smoke_scope_x = sync_width + 8.0F + drift_width + 6.0F;
            const float mark_scope_x =
                smoke_scope_x + drift_width + 8.0F + drift_width + 6.0F;
            bool smoke_random = false;
            bool mark_random = false;
            {
                std::scoped_lock lock(g_context.mutex);
                smoke_random = g_context.drift_smoke_random;
                mark_random = g_context.drift_mark_random;
            }
            const auto smoke_mode_label = smoke_random
                ? "烟雾轮换:随机" : "烟雾轮换:顺序";
            const auto mark_mode_label = mark_random
                ? "痕迹轮换:随机" : "痕迹轮换:顺序";
            if (has_cursor_x) {
                ui->set_cursor_pos_x(ui->user,
                    smoke_scope_x > 0.0F ? smoke_scope_x : 0.0F);
            }
            if (ui->button(ui->user, anomaly::sdk::StringView(smoke_mode_label),
                    drift_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.drift_smoke_random = !smoke_random;
                SaveDriftConfig();
            }
            if (has_same_line) ui->same_line(ui->user, 0.0F, 0.0F);
            if (has_cursor_x) {
                ui->set_cursor_pos_x(ui->user,
                    mark_scope_x > 0.0F ? mark_scope_x : 0.0F);
            }
            if (ui->button(ui->user, anomaly::sdk::StringView(mark_mode_label),
                    drift_width, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.drift_mark_random = !mark_random;
                SaveDriftConfig();
            }
        }
        // 范围勾选列表(点范围按钮展开,?=参与轮换)。
        const auto draw_scope_list = [&](bool is_smoke,
                std::vector<std::string>& candidates) {
            if (candidates.empty()) {
                DrawText(ui, "  (该车暂无可选列表——先同步选车加载目录)");
                return;
            }
            const bool child_ok =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
                    sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
                ui->begin_child && ui->end_child;
            bool visible = true;
            if (child_ok) {
                visible = ui->begin_child(ui->user,
                    anomaly::sdk::StringView(is_smoke
                        ? "##drift-scope-smoke" : "##drift-scope-mark"),
                    0.0F, 150.0F, 1U) != 0;
            }
            if (visible) {
                for (const auto& id : candidates) {
                    const std::string pattern = CandidateTierPattern(id);
                    bool checked = false;
                    {
                        std::scoped_lock lock(g_context.mutex);
                        const auto& scope = is_smoke
                            ? g_context.drift_smoke_scope
                            : g_context.drift_mark_scope;
                        if (scope.empty()) {
                            checked = true;   // 默认全部参与
                        } else {
                            for (const auto& p : scope) {
                                if (p == pattern) {
                                    checked = true;
                                    break;
                                }
                            }
                        }
                    }
                    std::string item_label =
                        (checked ? std::string("[✓] ") : std::string("[ ] ")) +
                        CandidateDisplayName(id);
                    item_label += is_smoke
                        ? "##dss-" + id : "##dsm-" + id;
                    if (ui->button(ui->user, anomaly::sdk::StringView(item_label),
                            0.0F, 0.0F)) {
                        bool changed = false;
                        {
                            std::scoped_lock lock(g_context.mutex);
                            auto& scope = is_smoke
                                ? g_context.drift_smoke_scope
                                : g_context.drift_mark_scope;
                            if (scope.empty()) {
                                // 先固化为全选档位模式,再取消勾选
                                for (const auto& cid : candidates) {
                                    scope.push_back(CandidateTierPattern(cid));
                                }
                            }
                            bool removed = false;
                            for (auto it = scope.begin(); it != scope.end(); ++it) {
                                if (*it == pattern) {
                                    scope.erase(it);
                                    removed = true;
                                    break;
                                }
                            }
                            if (!removed) scope.push_back(pattern);
                            changed = true;
                        }
                        if (changed) SaveDriftConfig();
                    }
                }
            }
            if (child_ok) ui->end_child(ui->user);
        };
        if (g_context.drift_smoke_ui_open) {
            DrawText(ui, "烟雾轮换范围(✓ = 参与 Shift 轮换):");
            draw_scope_list(true, smoke_candidates);
        }
        if (g_context.drift_mark_ui_open) {
            DrawText(ui, "痕迹轮换范围(✓ = 参与 Shift 轮换):");
            draw_scope_list(false, mark_candidates);
        }
    }
    {
        std::vector<CustomizationOption> options;
        std::vector<std::pair<std::uint32_t, std::string>> overrides;
        std::vector<std::pair<std::uint32_t, std::string>> whitelist_extras;
        std::vector<std::pair<std::uint32_t, std::string>> modules;
        bool modules_valid = false;
        std::string realtime_id;
        std::string customized_id;
        {
            std::scoped_lock lock(g_context.mutex);
            options = g_context.customization_options;
            overrides = g_context.selected_module_overrides;
            whitelist_extras = g_context.allowed_extra_modules;
            modules = g_context.current_vehicle_modules;
            modules_valid = g_context.current_vehicle_modules_valid;
            realtime_id = g_context.realtime_vehicle_id;
            customized_id = g_context.customization_vehicle_id;
        }
        if (!modules_valid) {
            DrawText(ui, "尚未读取(先坐进载具再点上面按钮)");
        }
        if (realtime_id.empty()) {
            DrawText(ui, "点上面按钮:插件选车将切到你正驾驶的车并加载它的改装目录");
        } else if (customized_id != realtime_id) {
            DrawText(ui, "目录加载中/需要重新同步(再点一次上面按钮)");
        } else {
            // ===== 实时选件器:本车目录 + 本车特效,点选即改槽位 =====
            struct RealtimeCategory {
                std::uint32_t type;
                const char* key;
                const char* english;
            };
            static constexpr std::array<RealtimeCategory, 26> kRealtimeCategories{{
                {1, "label.module.engine", "Engine"},
                {2, "label.module.gearbox", "Gearbox"},
                {3, "label.module.brake", "Brakes"},
                {4, "label.module.suspension", "Suspension"},
                {5, "label.module.body_color", "Body color module"},
                {6, "label.module.front_bumper", "Front bumper"},
                {7, "label.module.side_skirt", "Side skirt"},
                {8, "label.module.rear_bumper", "Rear bumper"},
                {9, "label.module.rear_wing", "Rear wing"},
                {10, "label.module.decal", "Decal"},
                {11, "label.module.wheel", "Wheels"},
                {12, "label.module.hood", "Hood"},
                {13, "label.module.plate", "License plate"},
                {14, "label.module.roof", "Roof"},
                {15, "label.module.tire", "Tires"},
                {16, "label.module.accessory", "Accessories"},
                {17, "label.module.lsd", "Limited-slip differential LSD"},
                {18, "label.module.stiffness", "Body rigidity"},
                {20, "label.module.interior_leather", "Interior leather color"},
                {21, "label.module.interior_stitch", "Interior stitching color"},
                {22, "label.module.vehicle_badge", "Vehicle badge"},
                {23, "label.module.tire_smoke", "Tire smoke"},
                {24, "label.module.tire_track", "Tire tracks"},
                {25, "label.module.tow_hook", "Tow hook"},
                {26, "label.module.ecu", "ECU"},
                {27, "label.module.exhaust_smoke", "Exhaust smoke"},
            }};
            const std::string prefix = VehicleModulePrefixFromId(realtime_id);
            const bool child_ok =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
                    sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
                ui->begin_child && ui->end_child;
            bool picker_visible = true;
            if (child_ok) {
                picker_visible = ui->begin_child(ui->user,
                    anomaly::sdk::StringView("##realtime-mod-picker"),
                    0.0F, 380.0F, 1U) != 0;
            }
            if (picker_visible) {
                for (const auto& category : kRealtimeCategories) {
                    const std::uint32_t type = category.type;
                    const std::string category_label =
                        g_context.localizer.Text(category.key, category.english);
                    // Whitelisted twin effects of this category (named buttons).
                    std::vector<std::pair<std::uint32_t, std::string>> effects;
                    for (const auto& extra : whitelist_extras) {
                        if (extra.first != type) continue;
                        if (!WhitelistExtraForVehicle(extra.second, prefix)) continue;
                        if (WhitelistExtraEffectDisplay(extra.second).empty()) continue;
                        bool already_listed = false;
                        std::string extra_base = extra.second;
                        const auto hash_pos = extra_base.find('#');
                        if (hash_pos != std::string::npos) {
                            extra_base.resize(hash_pos);
                        }
                        for (const auto& option : options) {
                            if (option.kind == nte_vehicle::kCustomizationModule &&
                                option.module_type == type && option.id == extra_base) {
                                already_listed = true;
                                break;
                            }
                        }
                        if (!already_listed) effects.push_back(extra);
                    }
                    bool any = !effects.empty();
                    for (const auto& option : options) {
                        if (option.kind == nte_vehicle::kCustomizationModule &&
                            option.module_type == type) {
                            any = true;
                            break;
                        }
                    }
                    if (!any) continue;
                    DrawText(ui, category_label);
                    for (const auto& option : options) {
                        if (option.kind != nte_vehicle::kCustomizationModule ||
                            option.module_type != type) {
                            continue;
                        }
                        bool selected_now = false;
                        for (const auto& entry : overrides) {
                            if (entry.first == type && entry.second == option.id) {
                                selected_now = true;
                                break;
                            }
                        }
                        std::string label =
                            option.label.empty() ? option.id : option.label;
                        if (selected_now) label = "[已选] " + label;
                        label += "##rt-" + std::to_string(type) + "-" + option.id;
                        if (ui->button(ui->user, anomaly::sdk::StringView(label),
                                0.0F, 0.0F)) {
                            std::scoped_lock lock(g_context.mutex);
                            auto& mods = g_context.selected_module_overrides;
                            const auto found = std::ranges::find_if(mods,
                                [type](const auto& entry) {
                                    return entry.first == type;
                                });
                            if (found == mods.end()) {
                                mods.emplace_back(type, option.id);
                            } else {
                                found->second = option.id;
                            }
                            // 点选即隐式触发一次实时应用(0.11.24:无需按钮)。
                            g_context.apply_current_modules_requested.store(
                                true, std::memory_order_release);
                        }
                    }
                    for (const auto& effect : effects) {
                        bool selected_now = false;
                        for (const auto& entry : overrides) {
                            if (entry.first == type && entry.second == effect.second) {
                                selected_now = true;
                                break;
                            }
                        }
                        std::string label = WhitelistExtraEffectDisplay(effect.second);
                        if (selected_now) label = "[已选] " + label;
                        label += "##rt-" + std::to_string(type) + "-" + effect.second;
                        if (ui->button(ui->user, anomaly::sdk::StringView(label),
                                0.0F, 0.0F)) {
                            std::scoped_lock lock(g_context.mutex);
                            auto& mods = g_context.selected_module_overrides;
                            const auto found = std::ranges::find_if(mods,
                                [type](const auto& entry) {
                                    return entry.first == type;
                                });
                            if (found == mods.end()) {
                                mods.emplace_back(type, effect.second);
                            } else {
                                found->second = effect.second;
                            }
                            // 点选即隐式触发一次实时应用(0.11.24:无需按钮)。
                            g_context.apply_current_modules_requested.store(
                                true, std::memory_order_release);
                        }
                    }
                }
                // 涂装:点击即应用到当前载具(UpdateVehicleMat,客户端视觉)。
                bool has_skins = false;
                for (const auto& option : options) {
                    if (option.kind == nte_vehicle::kCustomizationSkin) {
                        has_skins = true;
                        break;
                    }
                }
                if (has_skins) {
                    DrawText(ui, "涂装(点击即应用)");
                    std::string current_skin;
                    {
                        std::scoped_lock lock(g_context.mutex);
                        current_skin = g_context.realtime_skin_id;
                    }
                    for (const auto& option : options) {
                        if (option.kind != nte_vehicle::kCustomizationSkin) {
                            continue;
                        }
                        std::string label =
                            option.label.empty() ? option.id : option.label;
                        if (current_skin == option.id) label = "[已选] " + label;
                        label += "##rt-skin-" + option.id;
                        if (ui->button(ui->user, anomaly::sdk::StringView(label),
                                0.0F, 0.0F)) {
                            std::scoped_lock lock(g_context.mutex);
                            g_context.realtime_skin_id = option.id;
                            g_context.apply_skin_requested.store(true,
                                std::memory_order_release);
                        }
                    }
                }
            }
            if (child_ok) ui->end_child(ui->user);
        }
        // 0.11.28: 「把当前车特效加入白名单」已移至「导出日志」右侧。
        if (DrawButtonEnabled(ui, "保存当前车辆状态为该车记忆", true)) {
            g_context.save_modules_to_memory_requested.store(true,
                std::memory_order_release);
        }
    }

    DrawDivider(ui);
    // 0.11.32: 耐久自动保持开关(替代原两行说明文本)。
    {
        bool durability_on = true;
        {
            std::scoped_lock lock(g_context.mutex);
            durability_on = g_context.durability_auto_full.load(
                std::memory_order_acquire);
        }
        const auto durability_label = durability_on
            ? "耐久自动保持:开" : "耐久自动保持:关";
        if (ui->button(ui->user, anomaly::sdk::StringView(durability_label),
                0.0F, 0.0F)) {
            g_context.durability_auto_full.store(!durability_on,
                std::memory_order_release);
        }
    }

    // Local feature: advanced powertrain tuning — per-mode (normal/comfort)
    // direct-value sliders, live on drag, no apply buttons. 车速倍率并入本组
    // (0.11.26:位于发动机扭矩上方)。
    // 0.11.33: 轮换模式按钮改为分列(烟雾/痕迹各自独立),共享按钮删除。
    DrawDivider(ui);
    {
        // 最高限速滑条(0.11.31):0–350 km/h 全区间(可上可下),倍率按
        // 原厂极速换算(内核不变);初始位置 = 当前倍率 × 原厂极速(字段
        // 原值;模块加成的实际极速能通过拖动精确设定)。
        float stock_kmh = 0;
        bool has_stock = false;
        {
            std::scoped_lock lock(g_context.mutex);
            stock_kmh = g_context.adv_stock_top_speed_kmh;
            has_stock = g_context.adv_stock_valid;
        }
        if (has_stock && stock_kmh > 1.0F) {
            float target = 0;
            {
                std::scoped_lock lock(g_context.mutex);
                target = std::clamp(g_context.adv_speed_target_kmh,
                    0.0F, 350.0F);
            }
            if (ui->slider_float(ui->user,
                    anomaly::sdk::StringView("最高限速 (km/h)"),
                    &target, 0.0F, 350.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.adv_speed_target_kmh = target;
                g_context.speed_ratio = std::clamp(
                    target / stock_kmh, kMinimumSpeedRatio, kMaximumSpeedRatio);
                g_context.apply_speed.store(true, std::memory_order_release);
            }
            float ratio_now = 0;
            {
                std::scoped_lock lock(g_context.mutex);
                ratio_now = g_context.speed_ratio;
            }
            char ratio_line[96];
            std::snprintf(ratio_line, sizeof(ratio_line),
                "当前倍率 = %.2fx",
                ratio_now);
            DrawText(ui, ratio_line);
        } else {
            // 原厂极速未建立(尚未上车识别):回退倍率滑条。
            const auto speed_ratio_label =
                g_context.localizer.Text("label.speed_ratio", "Speed ratio");
            if (ui->slider_float(ui->user,
                    anomaly::sdk::StringView(speed_ratio_label),
                    &speed_ratio, kMinimumSpeedRatio, kMaximumSpeedRatio)) {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.speed_ratio = std::clamp(
                        speed_ratio, kMinimumSpeedRatio, kMaximumSpeedRatio);
                }
                g_context.apply_speed.store(true, std::memory_order_release);
            }
            DrawText(ui, "(原厂极速未读取:上车后自动建立,届时切换为限速模式)");
        }
        const auto locked_slider = [&](const char* label, float* context_value,
                float low, float high) {
            float local_value = 0;
            {
                std::scoped_lock lock(g_context.mutex);
                local_value = *context_value;
            }
            if (ui->slider_float(ui->user, anomaly::sdk::StringView(label),
                    &local_value, low, high)) {
                std::scoped_lock lock(g_context.mutex);
                *context_value = local_value;
                // 拖动即写:松手/拖动的每一帧直接生效(实时反馈)。
                g_context.adv_apply_requested.store(true,
                    std::memory_order_release);
            }
        };
        locked_slider("倒车极速 (km/h)",
            &g_context.adv_reverse_top_speed, 0.0F, 2000.0F);
    }
    {
        bool comfort_mode = false;
        {
            std::scoped_lock lock(g_context.mutex);
            comfort_mode = g_context.adv_comfort_mode;
        }
        const auto mode_label = comfort_mode
            ? "当前编辑:舒适传动(点此切回普通)"
            : "当前编辑:普通传动(点此切到舒适)";
        if (DrawButtonEnabled(ui, mode_label, true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.adv_comfort_mode = !g_context.adv_comfort_mode;
        }
    }
    {
        bool comfort_mode = false;
        {
            std::scoped_lock lock(g_context.mutex);
            comfort_mode = g_context.adv_comfort_mode;
        }
        const auto locked_slider = [&](const char* label, float* context_value,
                float low, float high) {
            float local_value = 0;
            {
                std::scoped_lock lock(g_context.mutex);
                local_value = *context_value;
            }
            if (ui->slider_float(ui->user, anomaly::sdk::StringView(label),
                    &local_value, low, high)) {
                std::scoped_lock lock(g_context.mutex);
                *context_value = local_value;
                // 拖动即写:松手/拖动的每一帧直接生效(实时反馈)。
                g_context.adv_apply_requested.store(true,
                    std::memory_order_release);
            }
        };
        if (comfort_mode) {
            locked_slider("升档转速 (RPM) [舒适]", &g_context.adv_c_change_up_rpm,
                500.0F, 12000.0F);
            locked_slider("降档转速 (RPM) [舒适]", &g_context.adv_c_down_rpm,
                0.0F, 12000.0F);
            locked_slider("换挡耗时 (秒) [舒适]", &g_context.adv_c_gear_change_time,
                0.0F, 2.0F);
            locked_slider("齿比总比 (FinalRatio) [舒适]",
                &g_context.adv_c_final_ratio, 0.5F, 10.0F);
            locked_slider("传动效率 [舒适]",
                &g_context.adv_c_transmission_efficiency, 0.1F, 3.0F);
        } else {
            locked_slider("升档转速 (RPM) [普通]", &g_context.adv_change_up_rpm,
                500.0F, 12000.0F);
            locked_slider("降档转速 (RPM) [普通]", &g_context.adv_down_rpm,
                0.0F, 12000.0F);
            locked_slider("换挡耗时 (秒) [普通]", &g_context.adv_gear_change_time,
                0.0F, 2.0F);
            locked_slider("齿比总比 (FinalRatio) [普通]", &g_context.adv_final_ratio,
                0.5F, 10.0F);
            locked_slider("传动效率 [普通]", &g_context.adv_transmission_efficiency,
                0.1F, 3.0F);
        }
        // 四倍率(拖动即生效,0.11.28 并入本组:传动效率下面、实时值上面)。
        const auto torque_ratio_label =
            g_context.localizer.Text("label.torque_ratio", "Engine torque ratio");
        if (ui->slider_float(ui->user, anomaly::sdk::StringView(torque_ratio_label),
                &torque_ratio, 0.05F, 20.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.engine_torque_ratio = torque_ratio;
            g_context.apply_torque.store(true, std::memory_order_release);
        }
        const auto other_speed_label =
            g_context.localizer.Text("label.other_speed_ratio", "Other vehicle speed ratio");
        if (ui->slider_float(ui->user, anomaly::sdk::StringView(other_speed_label),
                &other_speed_ratio, 0.05F, 10.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.other_speed_ratio = other_speed_ratio;
            g_context.apply_other_speed.store(true, std::memory_order_release);
        }
        const auto other_speed_floor_label = g_context.localizer.Text(
            "label.other_speed_floor", "Other vehicle minimum speed ratio");
        if (ui->slider_float(ui->user, anomaly::sdk::StringView(other_speed_floor_label),
                &other_speed_floor_ratio, 0.05F, 10.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.other_speed_floor_ratio = other_speed_floor_ratio;
            g_context.apply_other_speed.store(true, std::memory_order_release);
        }
        const auto other_torque_label = g_context.localizer.Text(
            "label.other_torque_ratio", "Other vehicle torque ratio");
        if (ui->slider_float(ui->user, anomaly::sdk::StringView(other_torque_label),
                &other_torque_ratio, 0.05F, 20.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.other_torque_ratio = other_torque_ratio;
            g_context.apply_other_torque.store(true, std::memory_order_release);
        }
        // 0.11.31: 三条「游戏实时值」行按用户要求删除(滑条即实时反馈)。
        if (DrawButtonEnabled(ui, "保存当前调校为该车记忆", true)) {
            g_context.save_tune_requested.store(true, std::memory_order_release);
        }
        if (DrawButtonEnabled(ui, "恢复默认调校", true)) {
            g_context.reset_tune_requested.store(true, std::memory_order_release);
        }
        // 车轮摩擦(原独立页移植):本行最右,紧挨右边界。
        {
            bool friction_enabled = true;
            {
                std::scoped_lock lock(g_context.mutex);
                friction_enabled = g_context.friction_enabled;
            }
            const auto friction_label = friction_enabled
                ? "车轮摩擦:开" : "车轮摩擦:关";
            float row_width = 640.0F;
            float window_width = 0.0F;
            float window_height = 0.0F;
            const bool has_window =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, get_window_size),
                    sizeof(decltype(AnomalyUiServiceV1::get_window_size))) &&
                ui->get_window_size != nullptr;
            if (has_window) {
                ui->get_window_size(ui->user, &window_width, &window_height);
            }
            if (window_width > 96.0F) row_width = window_width - 32.0F;
            const bool has_same_line =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, same_line),
                    sizeof(decltype(AnomalyUiServiceV1::same_line))) &&
                ui->same_line != nullptr;
            const bool has_cursor_x =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, set_cursor_pos_x),
                    sizeof(decltype(AnomalyUiServiceV1::set_cursor_pos_x))) &&
                ui->set_cursor_pos_x != nullptr;
            if (has_same_line) ui->same_line(ui->user, 0.0F, 8.0F);
            if (has_cursor_x) ui->set_cursor_pos_x(ui->user,
                (row_width - 150.0F) > 0.0F ? (row_width - 150.0F) : 0.0F);
            if (ui->button(ui->user, anomaly::sdk::StringView(friction_label),
                    150.0F, 0.0F)) {
                g_context.friction_toggle.store(true, std::memory_order_release);
            }
        }
    }

    // 0.11.28: 四倍率已并入进阶组(传动效率下方),原独立区删除。
    // Local feature: export the in-memory log ring for bug reports.
    DrawDivider(ui);
    const bool has_same_line_export =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, same_line),
            sizeof(decltype(AnomalyUiServiceV1::same_line))) &&
        ui->same_line != nullptr;
    if (DrawButtonEnabled(ui, "导出日志(把报错发给开发者)", true)) {
        g_context.export_log_requested.store(true, std::memory_order_release);
    }
    // 0.11.28: 「把当前车特效加入白名单」移至导出日志右侧(用户指定)。
    if (has_same_line_export) ui->same_line(ui->user, 0.0F, 10.0F);
    if (DrawButtonEnabled(ui, "把当前车特效加入白名单", true)) {
        g_context.capture_effects_requested.store(true,
            std::memory_order_release);
    }
}

// 0.11.26: 车轮摩擦已并入性能调校页(恢复默认调校行最右),原底盘页删除。

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
    // 0.11.26: 诊断按钮集中到本页(车速倍率显示下方)。
    if (ui->button(ui->user,
            anomaly::sdk::StringView("读取当前车辆已装模块"), 0.0F, 0.0F)) {
        g_context.read_modules_requested.store(true, std::memory_order_release);
    }
    if (ui->button(ui->user,
            anomaly::sdk::StringView("探测候选特效 ID(只读)"), 0.0F, 0.0F)) {
        g_context.probe_requested.store(true, std::memory_order_release);
    }
    const auto refresh_label =
        g_context.localizer.Text("action.refresh_catalog", "Refresh vehicle list");
    if (ui->button(ui->user, anomaly::sdk::StringView(refresh_label), 0.0F, 0.0F)) {
        g_context.refresh_catalog_requested.store(true, std::memory_order_release);
        SetStatus(g_context.localizer.Text("status.catalog_refresh_requested",
            "Vehicle catalog refresh requested."));
    }
    // Local diagnostics: full DataTable inventory sweep (finds tables holding
    // shop-only customization entries, e.g. premium wheel effects).
    if (ui->button(ui->user, anomaly::sdk::StringView("扫描全部数据表(取证)"), 0.0F, 0.0F)) {
        g_context.sweep_requested.store(true, std::memory_order_release);
    }
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

    nav_button(g_context.localizer.Text("action.tab_diagnostics", "Diagnostics"), 2);
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
    // Local feature: ue5.names for the FName pool probe (declared optional).
    g_context.names = QueryService<AnomalyUe5NamesServiceV1>(
        host, ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
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
    // Local fix: load the extra-module whitelist (lines "<type>|<ModuleID>").
    // These IDs are accepted by the summon path even when absent from the local
    // catalog (cloud-applied premiums, legacy upgrades no longer local).
    if (!g_context.state_directory.empty() && vehicle != nullptr) {
        std::string whitelist_path = g_context.state_directory;
        if (whitelist_path.back() != '\\' && whitelist_path.back() != '/')
            whitelist_path.push_back('\\');
        whitelist_path += "allowed-extra-modules.cfg";
        std::ifstream whitelist_in(whitelist_path, std::ios::binary);
        if (whitelist_in) {
            std::vector<std::pair<std::uint32_t, std::string>> extras;
            std::string line;
            while (std::getline(whitelist_in, line)) {
                while (!line.empty() &&
                       (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                    line.pop_back();
                // Local fix: the type may be multi-digit ("23|..."); parse everything
                // before the first '|' as the type instead of only the first char.
                const auto pipe_pos = line.find('|');
                if (pipe_pos == std::string::npos || pipe_pos == 0) continue;
                std::uint32_t type = 0;
                try {
                    type = static_cast<std::uint32_t>(std::stoul(line.substr(0, pipe_pos)));
                } catch (...) {
                    continue;
                }
                extras.emplace_back(type, line.substr(pipe_pos + 1));
            }
            if (!extras.empty()) {
                g_context.allowed_extra_modules = extras;
                vehicle->SetAllowedExtraModules(std::move(extras));
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
    LoadCustomizationMemory();
    LoadTuningMemory();
    LoadDriftConfig();
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
