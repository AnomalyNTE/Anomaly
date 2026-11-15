// =============================================================================
//  Anomaly 插件：自动日常（yu-1021.daily）
// =============================================================================
//
//  一条龙：进游戏 → 自动打副本（交给 CloneEnter 插件）→ 领取一咖舍 → 领取奖励。
//  首次使用需要在面板里确认一次设置；之后勾「启动后就下一次」即可自动跑。
//  游戏内 Ctrl+S（可在配置文件里改）随时强制停止。
//
//  约定：
//    * 打副本不重复造轮子：经 anomaly.ipc 调 ccyellowstar.clone-enter 的 onekey 端点；
//    * 偏移与签名只来自 profile/compatibility.profile.json，缺失即停用相关功能；
//    * 反射调用走 UObject::ProcessEvent，__try/__except 保护，只读内存；
//    * Game/Render 回调里不做文件 I/O：配置读写都在 Worker 线程（anomaly.scheduler）；
//    * 用户可见文案全部经 anomaly.localization，中文只在 locales/zh-CN.json。
//
//  =============================================================================

#include "anomaly/sdk/anomaly_sdk.h"

#include "chain.hpp"
#include "dungeon.hpp"
#include "game.hpp"
#include "i18n.hpp"
#include "profile_layout.hpp"
#include "settings_model.hpp"
#include "state.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace daily {
namespace {

constexpr std::uint32_t kPollIntervalMilliseconds = 2000;
constexpr std::uint32_t kFirstRunWindowSize = 4u;  // ImGuiCond_FirstUseEver
constexpr const char* kStopHotkeyId = "yu-1021.daily.stop";

template <typename T>
const T* Query(const AnomalyHostApiV1* host, const char* id, const std::uint32_t version) {
    if (host == nullptr || host->query_service == nullptr || id == nullptr) return nullptr;
    const void* service = nullptr;
    const AnomalyStatusV1 status = host->query_service(
        host->host_context, Str(id, std::strlen(id)), version, &service);
    if (status.code != ANOMALY_STATUS_V1_OK) return nullptr;
    return static_cast<const T*>(service);
}

void ANOMALY_CALL OnStopHotkey(void*, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*) {
    S().stop_requested.store(true, std::memory_order_release);
}

void ReleaseHotkey() {
    State& state = S();
    if (state.input == nullptr || state.input->release_hotkey == nullptr) return;
    if (state.stop_hotkey.id == 0) return;
    static_cast<void>(state.input->release_hotkey(state.input->user, state.stop_hotkey));
    state.stop_hotkey = {};
}

void RegisterHotkey() {
    State& state = S();
    ReleaseHotkey();
    if (!HAS(state.input, AnomalyInputServiceV1, register_hotkey)) return;
    AnomalyHotkeySpecV1 spec{sizeof(spec)};
    spec.modifiers = state.settings.stop_modifiers;
    spec.virtual_key = state.settings.stop_vk;
    spec.flags = ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS;
    spec.id = Str(kStopHotkeyId, std::strlen(kStopHotkeyId));
    if (state.input->register_hotkey(state.input->user, &spec, &OnStopHotkey, nullptr,
                                     &state.stop_hotkey)
            .code != ANOMALY_STATUS_V1_OK) {
        WinLog("stop hotkey registration failed");
    }
}

// ---- Worker 线程：配置写入与配置文件热更新 ----

void ANOMALY_CALL SaveTask(void*, AnomalyGenerationHandleV1) {
    State& state = S();
    state.save_in_flight.store(false, std::memory_order_release);
    if (!state.config_store.Write(state.settings)) {
        WinLog("configuration save failed");
    }
}

void ScheduleSave() {
    State& state = S();
    if (state.scheduler == nullptr || state.scheduler->schedule == nullptr) return;
    if (state.save_in_flight.exchange(true, std::memory_order_acq_rel)) return;
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status =
        state.scheduler->schedule(state.scheduler->user, 0, &SaveTask, nullptr, &task);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        state.save_in_flight.store(false, std::memory_order_release);
        WinLog("configuration save could not be scheduled");
    }
}

void ScheduleReload();

// 勾上「下次游戏启动时运行」：记下当前会话，等下次真的重启游戏才触发。
void ArmAutoRun(Settings& settings) {
    const SessionIdentity now = CurrentSession();
    settings.auto_run_pending = true;
    settings.auto_run_pid = now.pid;
    settings.auto_run_stamp = now.stamp;
    WinLogf("auto run armed: every game start from now on (session pid=%u)", now.pid);
}

void DisarmAutoRun(Settings& settings) {
    settings.auto_run_pending = false;
    settings.auto_run_pid = 0;
    settings.auto_run_stamp = 0;
}

void ANOMALY_CALL ReloadTask(void*, AnomalyGenerationHandleV1) {
    State& state = S();
    Settings stored{};
    if (state.config_store.Read(stored)) {
        const std::uint32_t previous_vk = state.settings.stop_vk;
        const std::uint32_t previous_modifiers = state.settings.stop_modifiers;
        const bool previous_enabled = state.settings.enabled;
        // 武装状态只在界面里改，配置文件里的值不生效（避免手改文件就地触发一次）。
        stored.auto_run_pending = state.settings.auto_run_pending;
        stored.auto_run_pid = state.settings.auto_run_pid;
        stored.auto_run_stamp = state.settings.auto_run_stamp;
        state.settings = stored;
        if (stored.stop_vk != previous_vk || stored.stop_modifiers != previous_modifiers) {
            state.hotkey_dirty.store(true, std::memory_order_release);
        }
        if (stored.enabled != previous_enabled) {
            WinLogf("configuration reloaded: enabled=%d", stored.enabled ? 1 : 0);
        }
    }
    ScheduleReload();
}

void ScheduleReload() {
    State& state = S();
    if (state.scheduler == nullptr || state.scheduler->schedule == nullptr) return;
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status = state.scheduler->schedule(
        state.scheduler->user, kPollIntervalMilliseconds, &ReloadTask, nullptr, &task);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        state.reload_task = task;
    }
}

void StopWorkerTasks() {
    State& state = S();
    if (state.scheduler == nullptr || state.scheduler->cancel == nullptr) return;
    if (state.reload_task.id != 0) {
        static_cast<void>(state.scheduler->cancel(state.scheduler->user, state.reload_task));
        state.reload_task = {};
    }
}

// ---- 请求位消费 ----

void ConsumeRequests() {
    State& state = S();
    if (state.request_chain_start.exchange(0) != 0) {
        if (state.stage == Stage::Idle) ChainStart(false);
    }
    if (state.request_chain_stop.exchange(0) != 0) {
        ChainStop(StopCause::Manual, "stage.stopped_manual", "Stopped manually");
    }
    if (state.request_claim_all.exchange(0) != 0) {
        static_cast<void>(ClaimDaily(-1));
    }
    if (state.request_fons.exchange(0) != 0) {
        StartFonsChain();
    }
    const int reward_run = state.request_reward_run.exchange(0);
    if (reward_run != 0) {
        ChainStartSingle(reward_run == 2 ? Stage::PeriodicRewards : Stage::DailyRewards);
    }
    if (state.request_dungeon_only.exchange(0) != 0) {
        if (DungeonIpcAvailable()) {
            if (!DungeonStart(state.settings)) {
                SetStatusText(TextLocalized("status.dungeon_rejected", "The dungeon start request was rejected."));
            }
        } else {
            SetStatusText(TextLocalized("status.dungeon_unavailable", "The dungeon plugin is unavailable."));
        }
    }
    if (state.request_save.exchange(0) != 0) {
        state.settings_dirty.store(true, std::memory_order_release);
    }
    if (state.request_placeholders.exchange(0) != 0) {
        const bool configured = state.settings.configured;
        state.settings = Settings{};
        state.settings.configured = configured;
        state.settings_dirty.store(true, std::memory_order_release);
        SetStatusText(TextLocalized("status.defaults_restored", "Defaults restored."));
    }
    if (state.hotkey_dirty.exchange(false, std::memory_order_acq_rel)) {
        RegisterHotkey();
    }
    if (state.settings_dirty.exchange(false, std::memory_order_acq_rel)) {
        ScheduleSave();
    }
}

// ---- 面板 ----

void Text(const AnomalyUiServiceV1* ui, const std::string& text) {
    ui->text(ui->user, Str(text.data(), text.size()));
}

// 面板里的一行本地化文本：key 走同一个目录，渲染进当前 UI 作用域。
void ScopedLabel(const AnomalyUiServiceV1* ui, const char* key, const char* fallback) {
    Text(ui, TextLocalized(key, fallback));
}

bool Checkbox(const AnomalyUiServiceV1* ui, const char* key, const char* fallback, bool& value) {
    if (!HAS(ui, AnomalyUiServiceV1, checkbox)) return false;
    int flag = value ? 1 : 0;
    const std::string label = TextLocalized(key, fallback);
    if (ui->checkbox(ui->user, Str(label.data(), label.size()), &flag) == 0) return false;
    value = flag != 0;
    return true;
}

bool Number(const AnomalyUiServiceV1* ui, const char* key, const char* fallback,
            std::uint32_t& value, const std::uint32_t step) {
    if (!HAS(ui, AnomalyUiServiceV1, input_uint32)) return false;
    const std::string label = TextLocalized(key, fallback);
    std::uint32_t current = value;
    if (ui->input_uint32(ui->user, Str(label.data(), label.size()), &current, step,
                         step * 10u) == 0) {
        return false;
    }
    value = current;
    return true;
}

// 副本菜单：和 CloneEnter 面板一样，点名字选本，切换时子本归零、难度回到该本下限。
bool DungeonMenu(const AnomalyUiServiceV1* ui, std::uint32_t& index) {
    if (!HAS(ui, AnomalyUiServiceV1, begin_menu) || !HAS(ui, AnomalyUiServiceV1, end_menu) ||
        !HAS(ui, AnomalyUiServiceV1, button)) {
        return false;
    }
    bool changed = false;
    const std::uint32_t current = index < kDungeonTableCount ? index : 0;
    const std::string current_name =
        TextLocalized(kDungeonTable[current].name_key, kDungeonTable[current].name);
    char label[160]{};
    std::snprintf(label, sizeof(label), "%s: %s",
                  TextLocalized("settings.menu.dungeon", "Dungeon").c_str(), current_name.c_str());
    if (ui->begin_menu(ui->user, Str(label, std::strlen(label)), 1) != 0) {
        for (std::uint32_t i = 0; i < kDungeonTableCount; ++i) {
            const std::string name = TextLocalized(kDungeonTable[i].name_key, kDungeonTable[i].name);
            if (ui->button(ui->user, Str(name.data(), name.size()), 0.0f, 0.0f) != 0 &&
                i != index) {
                index = i;
                changed = true;
            }
        }
        ui->end_menu(ui->user);
    }
    return changed;
}

// 子副本菜单：只有多个子本的副本才出现（与 CloneEnter 的 begin_menu 行为一致）。
bool SubMenu(const AnomalyUiServiceV1* ui, const DungeonRow& row, std::uint32_t& sub) {
    if (row.sub_count <= 1) return false;
    if (!HAS(ui, AnomalyUiServiceV1, begin_menu) || !HAS(ui, AnomalyUiServiceV1, end_menu) ||
        !HAS(ui, AnomalyUiServiceV1, button)) {
        return false;
    }
    const std::uint32_t current = sub < row.sub_count ? sub : 0;
    const char* current_key = row.sub_keys[current] != nullptr ? row.sub_keys[current] : "";
    const char* current_name = row.sub_names[current] != nullptr ? row.sub_names[current] : "";
    char label[160]{};
    std::snprintf(label, sizeof(label), "%s: %s",
                  TextLocalized("settings.menu.sub", "Sub dungeon").c_str(),
                  TextLocalized(current_key, current_name).c_str());
    bool changed = false;
    if (ui->begin_menu(ui->user, Str(label, std::strlen(label)), 1) != 0) {
        for (std::uint32_t i = 0; i < row.sub_count; ++i) {
            const char* sub_key = row.sub_keys[i] != nullptr ? row.sub_keys[i] : "";
            const char* sub_name = row.sub_names[i] != nullptr ? row.sub_names[i] : "";
            const std::string name = TextLocalized(sub_key, sub_name);
            if (ui->button(ui->user, Str(name.data(), name.size()), 0.0f, 0.0f) != 0 &&
                i != current) {
                sub = i;
                changed = true;
            }
        }
        ui->end_menu(ui->user);
    }
    return changed;
}

// 副本选择区（面板与首次向导共用）。返回 true 表示设置被改动。
bool DrawDungeonPicker(const AnomalyUiServiceV1* ui, Settings& settings) {
    bool changed = false;
    if (DungeonMenu(ui, settings.dungeon.index)) {
        settings.dungeon.sub_index = 0;
        settings.dungeon.level_index = DungeonRowAt(settings.dungeon.index).min_level;
        changed = true;
    }
    const DungeonRow& row = DungeonRowAt(settings.dungeon.index);
    if (SubMenu(ui, row, settings.dungeon.sub_index)) changed = true;
    if (Number(ui, "settings.level_index", "Difficulty level", settings.dungeon.level_index, 1)) {
        changed = true;
    }
    if (Number(ui, "settings.runs", "Runs", settings.dungeon.runs, 1)) changed = true;
    if (Number(ui, "settings.enter_wait", "Enter wait (seconds)", settings.dungeon.enter_wait, 1)) {
        changed = true;
    }
    if (Number(ui, "settings.exit_wait", "Exit wait (seconds)", settings.dungeon.exit_wait, 1)) {
        changed = true;
    }
    if (changed) NormalizeSettings(settings);

    const DungeonRow& normalized = DungeonRowAt(settings.dungeon.index);
    char sub_name[96]{};
    const char* sub_pick = normalized.sub_names[settings.dungeon.sub_index] != nullptr
        ? normalized.sub_names[settings.dungeon.sub_index] : "";
    std::snprintf(sub_name, sizeof(sub_name), "%s", sub_pick);
    char summary[224]{};
    std::snprintf(summary, sizeof(summary),
                  "index=%u entry=%u sub=%u/%u(%s) level=%u-%u runs=%u",
                  static_cast<unsigned>(settings.dungeon.index),
                  static_cast<unsigned>(normalized.entry_index),
                  static_cast<unsigned>(settings.dungeon.sub_index) + 1u,
                  static_cast<unsigned>(normalized.sub_count), sub_name,
                  static_cast<unsigned>(normalized.min_level),
                  static_cast<unsigned>(normalized.max_level),
                  static_cast<unsigned>(settings.dungeon.runs));
    Text(ui, summary);
    return changed;
}

void DrawSettings(const AnomalyUiServiceV1* ui) {
    State& state = S();
    Settings& settings = state.settings;
    bool changed = false;

    if (Checkbox(ui, "settings.enabled", "Master switch", settings.enabled)) changed = true;
    if (Checkbox(ui, "settings.auto_run", "Run automatically on every game start",
                 settings.auto_run_pending)) {
        if (settings.auto_run_pending) {
            ArmAutoRun(settings);
        } else {
            DisarmAutoRun(settings);
        }
        changed = true;
    }
    if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
    ScopedLabel(ui, "settings.stages", "Stages");
    if (Checkbox(ui, "settings.stage_dungeon", "Dungeon", settings.stage_dungeon)) changed = true;
    if (Checkbox(ui, "settings.stage_cafe", "One-shop withdrawal", settings.stage_cafe))
        changed = true;
    // 两个领取分开设置：想只领一种就把另一个关掉。
    if (Checkbox(ui, "settings.stage_daily_rewards", "Daily activity rewards",
                 settings.stage_daily_rewards)) {
        changed = true;
    }
    if (Checkbox(ui, "settings.stage_periodic_rewards", "Periodic quest rewards",
                 settings.stage_periodic_rewards)) {
        changed = true;
    }
    if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);

    ScopedLabel(ui, "settings.dungeon", "Dungeon selection");
    if (DrawDungeonPicker(ui, settings)) changed = true;

    if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
    ScopedLabel(ui, "settings.hotkey", "Stop hotkey: Ctrl+S (configurable)");
    ScopedLabel(ui, "settings.config_path",
            "Config file: Anomaly\\config\\plugins\\yu-1021.daily\\");

    if (changed) {
        NormalizeSettings(settings);
        state.settings_dirty.store(true, std::memory_order_release);
    }
}

void DrawFirstRun(const AnomalyUiServiceV1* ui) {
    State& state = S();
    Settings& settings = state.settings;
    ScopedLabel(ui, "setup.title", "First-time setup");
    ScopedLabel(ui, "setup.intro",
            "Pick a dungeon and whether the chain should run once right after the next start.");
    bool changed = false;
    if (Checkbox(ui, "settings.enabled", "Master switch", settings.enabled)) changed = true;
    if (Checkbox(ui, "settings.auto_run", "Run automatically on every game start",
                 settings.auto_run_pending)) {
        if (settings.auto_run_pending) {
            ArmAutoRun(settings);
        } else {
            DisarmAutoRun(settings);
        }
        changed = true;
    }
    if (Checkbox(ui, "settings.stage_dungeon", "Dungeon", settings.stage_dungeon)) changed = true;
    if (Checkbox(ui, "settings.stage_cafe", "One-shop withdrawal", settings.stage_cafe))
        changed = true;
    if (Checkbox(ui, "settings.stage_daily_rewards", "Daily activity rewards",
                 settings.stage_daily_rewards)) {
        changed = true;
    }
    if (Checkbox(ui, "settings.stage_periodic_rewards", "Periodic quest rewards",
                 settings.stage_periodic_rewards)) {
        changed = true;
    }
    if (DrawDungeonPicker(ui, settings)) changed = true;
    if (changed) NormalizeSettings(settings);

    if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
    ScopedLabel(ui, "setup.table_hint", "Dungeon reference (index / game entry / subs / level):");
    for (std::size_t i = 0; i < kDungeonTableCount; ++i) {
        const DungeonRow& row = kDungeonTable[i];
        char line[192]{};
        std::snprintf(line, sizeof(line), "%u  entry %u  %s  (subs %u, level %u-%u)",
                      static_cast<unsigned>(i), static_cast<unsigned>(row.entry_index), row.name,
                      static_cast<unsigned>(row.sub_count),
                      static_cast<unsigned>(row.min_level),
                      static_cast<unsigned>(row.max_level));
        Text(ui, line);
        if (row.sub_count > 1) {
            char subs[192]{};
            int offset = 0;
            for (std::uint32_t s = 0; s < row.sub_count && offset >= 0; ++s) {
                const char* sub_name = row.sub_names[s] != nullptr ? row.sub_names[s] : "";
                offset += std::snprintf(subs + offset, sizeof(subs) - static_cast<std::size_t>(offset),
                                        "%s%u=%s", s == 0 ? "" : "  ",
                                        static_cast<unsigned>(s), sub_name);
            }
            Text(ui, std::string("    ") + subs);
        }
    }

    if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
    const std::string confirm = TextLocalized("setup.confirm", "Save and start using the plugin");
    if (HAS(ui, AnomalyUiServiceV1, button) &&
        ui->button(ui->user, Str(confirm.data(), confirm.size()), 0.0f, 0.0f) != 0) {
        settings.configured = true;
        state.first_run_popup = false;
        state.settings_dirty.store(true, std::memory_order_release);
        SetStatusText(TextLocalized("status.setup_saved", "Setup saved."));
    }
}

void DrawActions(const AnomalyUiServiceV1* ui) {
    State& state = S();
    if (!HAS(ui, AnomalyUiServiceV1, button)) return;
    const bool idle = state.stage == Stage::Idle;
    const std::string start = TextLocalized("action.start_chain", "Run the chain");
    const std::string stop = TextLocalized("action.stop_chain", "Stop");
    if (HAS(ui, AnomalyUiServiceV1, button_enabled)) {
        if (ui->button_enabled(ui->user, Str(start.data(), start.size()), 0.0f, 0.0f,
                               idle ? 1 : 0) != 0) {
            state.request_chain_start.store(1, std::memory_order_release);
        }
        if (HAS(ui, AnomalyUiServiceV1, same_line)) ui->same_line(ui->user, 0.0f, 8.0f);
        if (ui->button_enabled(ui->user, Str(stop.data(), stop.size()), 0.0f, 0.0f,
                               idle ? 0 : 1) != 0) {
            state.request_chain_stop.store(1, std::memory_order_release);
        }
    }
    const std::string claim = TextLocalized("action.claim", "Claim rewards now");
    if (ui->button(ui->user, Str(claim.data(), claim.size()), 0.0f, 0.0f) != 0) {
        state.request_claim_all.store(1, std::memory_order_release);
    }
    // 两个领取分开执行：各点各的，互不影响。
    const std::string daily = TextLocalized("action.daily_rewards", "Claim daily activity rewards");
    if (ui->button(ui->user, Str(daily.data(), daily.size()), 0.0f, 0.0f) != 0) {
        state.request_reward_run.store(1, std::memory_order_release);
    }
    if (HAS(ui, AnomalyUiServiceV1, same_line)) ui->same_line(ui->user, 0.0f, 8.0f);
    const std::string periodic = TextLocalized("action.periodic_rewards", "Claim periodic quest rewards");
    if (ui->button(ui->user, Str(periodic.data(), periodic.size()), 0.0f, 0.0f) != 0) {
        state.request_reward_run.store(2, std::memory_order_release);
    }
    if (HAS(ui, AnomalyUiServiceV1, same_line)) ui->same_line(ui->user, 0.0f, 8.0f);
    const std::string fons = TextLocalized("action.fons", "Withdraw one-shop now");
    if (ui->button(ui->user, Str(fons.data(), fons.size()), 0.0f, 0.0f) != 0) {
        state.request_fons.store(1, std::memory_order_release);
    }
    const std::string dungeon = TextLocalized("action.dungeon", "Run the dungeon now");
    if (ui->button(ui->user, Str(dungeon.data(), dungeon.size()), 0.0f, 0.0f) != 0) {
        state.request_dungeon_only.store(1, std::memory_order_release);
    }
    if (HAS(ui, AnomalyUiServiceV1, same_line)) ui->same_line(ui->user, 0.0f, 8.0f);
    const std::string save = TextLocalized("action.save", "Save configuration");
    if (ui->button(ui->user, Str(save.data(), save.size()), 0.0f, 0.0f) != 0) {
        state.request_save.store(1, std::memory_order_release);
    }
    const std::string defaults = TextLocalized("action.defaults", "Restore defaults");
    if (ui->button(ui->user, Str(defaults.data(), defaults.size()), 0.0f, 0.0f) != 0) {
        state.request_placeholders.store(1, std::memory_order_release);
    }
}

void DrawStatus(const AnomalyUiServiceV1* ui) {
    const State& state = S();
    char line[256]{};
    const std::string stage_text = TextLocalized(StageKey(state.stage), StageFallback(state.stage));
    const std::string stage_label = TextLocalized("status.stage_label", "Stage");
    std::snprintf(line, sizeof(line), "%s: %s", stage_label.c_str(), stage_text.c_str());
    Text(ui, line);
    if (state.stage_note[0] != '\0') Text(ui, state.stage_note);
    if (state.status[0] != '\0') Text(ui, state.status);
    if (!state.layout_ready) {
        ScopedLabel(ui, "status.profile_missing",
                "Compatibility profile is unavailable: reflection features are disabled.");
        if (state.layout_error[0] != '\0') Text(ui, state.layout_error);
    } else if (state.engine_error[0] != '\0') {
        Text(ui, state.engine_error);
    }
    if (state.settings.auto_run_pending) {
        ScopedLabel(ui, "status.auto_run_armed",
                "Set: the chain runs automatically on every game start (master switch must be on).");
    }
}

void DrawLogs(const AnomalyUiServiceV1* ui) {
    const State& state = S();
    if (!HAS(ui, AnomalyUiServiceV1, begin_child) || !HAS(ui, AnomalyUiServiceV1, end_child)) return;
    const std::string id = TextLocalized("panel.logs", "Logs");
    if (ui->begin_child(ui->user, Str(id.data(), id.size()), 0.0f, 140.0f, 0u) == 0) {
        ui->end_child(ui->user);
        return;
    }
    for (int i = 0; i < state.log_count; ++i) {
        const int index =
            (state.log_head - state.log_count + i + static_cast<int>(kMaxLogLines)) %
            static_cast<int>(kMaxLogLines);
        Text(ui, state.log_lines[index]);
    }
    ui->end_child(ui->user);
}

int g_panel_open = 1;

// ---- 生命周期 ----

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    ResetState();
    State& state = S();
    state.host = host;
    state.core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID, 1);
    if (state.core == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    state.ui = Query<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID, 1);
    state.localization =
        Query<AnomalyLocalizationServiceV1>(host, ANOMALY_LOCALIZATION_SERVICE_V1_ID, 1);
    BindLocalization(state.localization);
    state.config = Query<AnomalyConfigServiceV1>(host, ANOMALY_CONFIG_SERVICE_V1_ID, 1);
    state.json = Query<AnomalyJsonServiceV1>(host, ANOMALY_JSON_SERVICE_V1_ID, 1);
    state.input = Query<AnomalyInputServiceV1>(host, ANOMALY_INPUT_SERVICE_V1_ID, 1);
    state.scheduler = Query<AnomalySchedulerServiceV1>(host, ANOMALY_SCHEDULER_SERVICE_V1_ID, 1);
    state.signature = Query<AnomalySignatureServiceV1>(host, ANOMALY_SIGNATURE_SERVICE_V1_ID, 1);
    state.ipc = Query<AnomalyIpcServiceV1>(host, ANOMALY_IPC_SERVICE_V1_ID, 1);
    state.profile = Query<AnomalyProfileServiceV1>(host, ANOMALY_PROFILE_SERVICE_V1_ID, 1);
    state.names = Query<AnomalyUe5NamesServiceV1>(host, ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1);
    state.objects = Query<AnomalyUe5ObjectsServiceV1>(host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1);
    state.uibuttons =
        Query<AnomalyNteUiButtonsServiceV1>(host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID, 1);
    state.session = Query<AnomalyNteSessionServiceV1>(host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, 1);

    state.config_store.Bind(state.config, state.json);
    if (state.config_store.RegisterSchema()) {
        Settings stored{};
        if (state.config_store.Read(stored)) {
            state.settings = stored;
            WinLog("configuration restored");
        } else {
            state.settings_dirty.store(true, std::memory_order_release);
        }
    } else {
        WinLog("configuration schema registration failed");
    }
    NormalizeSettings(state.settings);
    if (state.settings.auto_run_pending &&
        (state.settings.auto_run_pid == 0 && state.settings.auto_run_stamp == 0)) {
        // 老配置没有会话标记：按「本会话刚勾上」处理，等下次重启游戏再跑，绝不就地触发。
        const SessionIdentity now = CurrentSession();
        state.settings.auto_run_pid = now.pid;
        state.settings.auto_run_stamp = now.stamp;
        state.settings_dirty.store(true, std::memory_order_release);
    }

    if (state.profile != nullptr && state.json != nullptr && state.core != nullptr) {
        char error[kLogLineBytes]{};
        if (LoadCompatibilityProfile(state.profile, state.json, g_profile, error, sizeof(error))) {
            state.layout = g_profile.layout;
            state.layout_ready = true;
            WinLog("compatibility profile loaded");
        } else {
            std::snprintf(state.layout_error, sizeof(state.layout_error), "%s", error);
            WinLogf("compatibility profile rejected: %s", error);
        }
    } else {
        std::snprintf(state.layout_error, sizeof(state.layout_error), "%s",
                      "anomaly.profile is unavailable");
        WinLog("anomaly.profile is unavailable");
    }

    state.first_run_popup = !state.settings.configured;
    *plugin_context = &state;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    RegisterHotkey();
    ScheduleReload();
    SetStatusText(TextLocalized("status.ready", "Ready."));
    WinLog("auto-daily started");
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t /*deadline_milliseconds*/) {
    State& state = S();
    ChainStop(StopCause::Manual, "stage.stopped_manual", "Stopped");
    ReleaseHotkey();
    StopWorkerTasks();
    static_cast<void>(state.config_store.Write(state.settings));
    WinLog("auto-daily stopped");
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

void ANOMALY_CALL Unload(void*) {
    State& state = S();
    ChainStop(StopCause::Manual, "stage.stopped_manual", "Stopped");
    DungeonCancelPending();
    ReleaseHotkey();
    StopWorkerTasks();
    static_cast<void>(state.config_store.Write(state.settings));
    ResetState();
}

void ANOMALY_CALL Update(void*, double delta_seconds) {
    State& state = S();
    ++state.updates;
    ++state.tick;
    ConsumeRequests();
    // 「下次游戏启动时跑一次」：只有当进程换过一次（真的重启了游戏）才触发。
    // 同一个进程里热重载插件不算，勾上之后不会立刻执行。
    if (state.settings.auto_run_pending && state.settings.enabled) {
        const SessionIdentity now = CurrentSession();
        const bool armed_here = state.settings.auto_run_pid == now.pid &&
                                state.settings.auto_run_stamp == now.stamp;
        if (!armed_here && state.stage == Stage::Idle) {
            WinLogf("auto run fired: new game session (pid=%u, armed pid=%u)", now.pid,
                    state.settings.auto_run_pid);
            // 常驻开关：只把会话标记推进到当前会话（同一次启动内不再重复触发），
            // 开关本身保持打开，下次启动继续自动运行。
            state.settings.auto_run_pid = now.pid;
            state.settings.auto_run_stamp = now.stamp;
            state.settings_dirty.store(true, std::memory_order_release);
            ChainStart(true);
        }
    }
    ChainTick(delta_seconds);
    TickClaimAll();
    if (state.tick % 600u == 0u && state.layout_ready && !state.engine_ready) {
        static_cast<void>(EnsureEngine());
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui_service) {
    const AnomalyUiServiceV1* ui = ui_service != nullptr ? ui_service : S().ui;
    if (!HAS(ui, AnomalyUiServiceV1, begin_window) || !HAS(ui, AnomalyUiServiceV1, end_window)) return;
    if (HAS(ui, AnomalyUiServiceV1, set_next_window_size_constraints)) {
        ui->set_next_window_size_constraints(ui->user, 420.0f, 380.0f, 100000.0f, 100000.0f);
    }
    if (HAS(ui, AnomalyUiServiceV1, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 520.0f, 620.0f, kFirstRunWindowSize);
    }
    const std::string title = TextLocalized("window.title", "Auto Daily");
    if (ui->begin_window(ui->user, Str(title.data(), title.size()), &g_panel_open, 0u) ==
        0) {
        ui->end_window(ui->user);
        return;
    }
    State& state = S();
    DrawStatus(ui);
    if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
    if (state.first_run_popup || !state.settings.configured) {
        DrawFirstRun(ui);
    } else {
        DrawActions(ui);
        if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
        DrawSettings(ui);
    }
    if (HAS(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
    DrawLogs(ui);
    ui->end_window(ui->user);
}

}  // namespace
}  // namespace daily

using daily::Str;

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {sizeof(*descriptor),
                   ANOMALY_PLUGIN_API_V1_MAJOR,
                   ANOMALY_PLUGIN_API_V1_MINOR,
                   Str("yu-1021.daily"),
                   Str("Auto Daily"),
                   Str("YU-1021"),
                   Str("4.3.1"),
                   daily::Load,
                   daily::Start,
                   daily::Stop,
                   daily::Unload,
                   daily::Update,
                   daily::Draw};
    return anomaly::sdk::Ok();
}
