#pragma once
// 脚本主循环：营地 → 刷新怪物 → 1 号战斗点 → 自动战斗 → 2 号战斗点 → 自动战斗 → 回到营地。
// 只在 Game 回调域（on_update）推进；界面线程只读写几个原子量。
// 传送、刷新、战斗都是异步的，每步配「完成判据 + 超时兜底」：超时记一条警告再进下一步。

namespace {

// 到达判据只比水平距离：传送点给的 Z 往往高于地面，用三维距离会永远判不到达。
constexpr double kScriptArriveRadiusMeters = 40.0;
// 单次传送：发出请求最多等这么久；发出之后再等这么久没到就按「已到达」继续。
constexpr std::chrono::seconds kScriptTeleportTimeout{14};
// 每次落地后的稳定等待：流送与区域状态需要一点时间才就绪，紧接着刷新会打空。
constexpr std::chrono::seconds kScriptSettleDuration{2};
constexpr std::chrono::seconds kScriptRefreshTimeout{30};
// 单个战斗点的兜底上限：正常由战斗模块的收尾判据结束，这里只防卡死。
constexpr std::chrono::minutes kScriptCombatTimeout{5};

// 传送点槽位：0 = 营地、1 = 1 号战斗点、2 = 2 号战斗点。
constexpr int kScriptSlotCamp = 0;
constexpr int kScriptSlotFight1 = 1;
constexpr int kScriptSlotFight2 = 2;

enum class ScriptStep {
    idle,
    teleport_camp,
    teleport_fight,
    teleport_fight2,
    settle,
    refresh,
    combat,
};

// 界面线程置位、Game 线程消费。
std::atomic_bool g_script_start_requested{};
std::atomic_bool g_script_stop_requested{};
std::atomic_bool g_script_running{};
std::atomic_int g_script_round{0};
// 循环次数，0 = 无限循环。
std::atomic<std::uint32_t> g_script_loop_count{0};

std::mutex g_script_mutex;
// 静态初始化阶段不能访问本地化服务，所以初始为空，由 ScriptStatusText 兜底。
std::string g_script_status;

// Game 线程独占。
ScriptStep g_script_phase{ScriptStep::idle};
ScriptStep g_script_settle_next{ScriptStep::refresh};
// 当前这一段自动战斗属于哪个战斗点（1 或 2），决定打完后的去向。
int g_script_fight_slot{kScriptSlotFight1};
std::chrono::steady_clock::time_point g_script_step_at{};
std::chrono::steady_clock::time_point g_script_deadline{};
double g_script_target[3]{};
bool g_script_step_started{};

void ScriptPublish(std::string text) {
    std::scoped_lock lock(g_script_mutex);
    g_script_status = std::move(text);
}

std::string ScriptStatusText() {
    std::scoped_lock lock(g_script_mutex);
    if (g_script_status.empty()) {
        return g_context.localizer.Text("script.status.idle", "Script: not started");
    }
    return g_script_status;
}

std::string ScriptStepName(const ScriptStep step) {
    switch (step) {
    case ScriptStep::teleport_camp:
        return g_context.localizer.Text("script.step.teleport_camp", "Teleporting to camp");
    case ScriptStep::teleport_fight:
        return g_context.localizer.Text("script.step.teleport_fight1",
                                        "Teleporting to fight point 1");
    case ScriptStep::teleport_fight2:
        return g_context.localizer.Text("script.step.teleport_fight2",
                                        "Teleporting to fight point 2");
    case ScriptStep::settle:
        return g_context.localizer.Text("script.step.settle", "Settling");
    case ScriptStep::refresh:
        return g_context.localizer.Text("script.step.refresh", "Refreshing monsters");
    case ScriptStep::combat:
        return g_context.localizer.Text("script.step.combat", "Auto combat");
    default: return g_context.localizer.Text("script.step.idle", "Idle");
    }
}

std::string ScriptRoundPrefix() {
    const int round = g_script_round.load();
    const std::uint32_t total = g_script_loop_count.load();
    const std::string round_text = std::to_string(round);
    if (total == 0) {
        const std::array<std::string_view, 1> arguments{std::string_view(round_text)};
        return g_context.localizer.Format("script.round", "Round {0}", arguments);
    }
    const std::string total_text = std::to_string(total);
    const std::array<std::string_view, 2> arguments{std::string_view(round_text),
                                                    std::string_view(total_text)};
    return g_context.localizer.Format("script.round_of", "Round {0}/{1}", arguments);
}

void ScriptEnterStep(const ScriptStep step, const std::chrono::steady_clock::time_point now) {
    g_script_phase = step;
    g_script_step_at = now;
    g_script_step_started = false;
    g_script_deadline = now + kScriptTeleportTimeout;
}

// 读取某个槽位的传送点坐标（0 营地 / 1 一号战斗点 / 2 二号战斗点）。
void ScriptReadTarget(const int slot, double target[3]) {
    std::scoped_lock lock(g_context.mutex);
    const double* source = g_context.settings.targets[slot];
    std::copy(source, source + 3, target);
}

bool ScriptArrived(const AnomalyHostApiV1* host) {
    double position[3]{};
    if (!TryReadCurrentPosition(host, position)) return false;
    const double dx = position[0] - g_script_target[0];
    const double dy = position[1] - g_script_target[1];
    return (dx * dx + dy * dy) <= kScriptArriveRadiusMeters * kScriptArriveRadiusMeters;
}

void ScriptShutdown() {
    g_script_start_requested.store(false);
    g_script_stop_requested.store(false);
    g_script_running.store(false);
    g_script_round.store(0);
    g_script_phase = ScriptStep::idle;
    g_script_step_started = false;
    ScriptPublish(g_context.localizer.Text("script.status.idle", "Script: not started"));
}

// 停脚本：连同正在跑的刷新与自动战斗一起收尾，避免留下后台流程。
void ScriptStop(const std::string& reason) {
    g_script_running.store(false);
    g_script_phase = ScriptStep::idle;
    g_script_step_started = false;
    g_script_stop_requested.store(false);
    if (g_ue_refresh_stage.load() != 0) g_ue_refresh_stage.store(0);
    if (g_combat_active.load()) g_combat_stop_requested.store(true);
    ScriptPublish(reason);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 script: " + reason);
}

// 一轮结束：轮次够了就收工，否则回到第一步。
void ScriptFinishRound(const std::string& detail, const std::chrono::steady_clock::time_point now) {
    const int round = g_script_round.load();
    const std::uint32_t total = g_script_loop_count.load();
    if (total != 0 && static_cast<std::uint32_t>(round) >= total) {
        const std::string round_text = std::to_string(round);
        const std::array<std::string_view, 1> arguments{std::string_view(round_text)};
        ScriptStop(g_context.localizer.Format("script.status.completed",
                                              "Script: finished {0} rounds", arguments));
        return;
    }
    g_script_round.store(round + 1);
    ScriptEnterStep(ScriptStep::teleport_camp, now);
    std::string text =
        ScriptRoundPrefix() + g_context.localizer.Text("script.status.to_camp",
                                                       " · Teleporting to camp");
    if (!detail.empty()) {
        const std::array<std::string_view, 1> arguments{std::string_view(detail)};
        text += g_context.localizer.Format("script.status.previous_round",
                                           " (previous round: {0})", arguments);
    }
    ScriptPublish(text);
}

void AdvanceScript(const AnomalyHostApiV1* host) {
    if (host == nullptr) return;
    const auto now = std::chrono::steady_clock::now();

    if (g_script_start_requested.exchange(false) && !g_script_running.load()) {
        g_script_round.store(1);
        g_script_running.store(true);
        ScriptEnterStep(ScriptStep::teleport_camp, now);
        ScriptPublish(ScriptRoundPrefix() + g_context.localizer.Text(
                                                 "script.status.to_camp",
                                                 " · Teleporting to camp"));
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "night999 script: started, rounds=" + std::to_string(g_script_loop_count.load()));
    }
    if (g_script_stop_requested.exchange(false)) {
        ScriptStop(g_context.localizer.Text("script.status.stopped", "Script: stopped"));
        return;
    }
    if (!g_script_running.load()) return;

    switch (g_script_phase) {
    case ScriptStep::teleport_camp:
    case ScriptStep::teleport_fight:
    case ScriptStep::teleport_fight2: {
        int slot = kScriptSlotCamp;
        if (g_script_phase == ScriptStep::teleport_fight) slot = kScriptSlotFight1;
        else if (g_script_phase == ScriptStep::teleport_fight2) slot = kScriptSlotFight2;
        ScriptReadTarget(slot, g_script_target);
        const auto go_settle = [&]() {
            if (slot == kScriptSlotCamp) {
                g_script_settle_next = ScriptStep::refresh;
            } else {
                g_script_settle_next = ScriptStep::combat;
                g_script_fight_slot = slot;
            }
            ScriptEnterStep(ScriptStep::settle, now);
        };
        const std::string arrived_text = slot == kScriptSlotCamp
            ? g_context.localizer.Text("script.status.arrived_camp", "Arrived at camp")
            : (slot == kScriptSlotFight1
                   ? g_context.localizer.Text("script.status.arrived_fight1",
                                              "Arrived at fight point 1")
                   : g_context.localizer.Text("script.status.arrived_fight2",
                                              "Arrived at fight point 2"));
        if (!g_script_step_started) {
            // 只发一次传送；发不出去就等到超时。每帧重发会反复打断流送。
            if (ScriptArrived(host)) {
                go_settle();
                ScriptPublish(ScriptRoundPrefix() + " · " + arrived_text);
                return;
            }
            if (TeleportToPosition(host, g_script_target)) {
                g_script_step_started = true;
                g_script_deadline = now + kScriptTeleportTimeout;
                return;
            }
            if (now >= g_script_deadline) {
                Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    "night999 script: teleport request refused; skipping step");
                go_settle();
                ScriptPublish(ScriptRoundPrefix() + g_context.localizer.Text(
                                                         "script.status.teleport_refused",
                                                         " · teleport refused, moving on"));
            }
            return;
        }
        if (ScriptArrived(host)) {
            go_settle();
            ScriptPublish(ScriptRoundPrefix() + " · " + arrived_text);
            return;
        }
        if (now >= g_script_deadline) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 script: teleport timeout");
            go_settle();
            ScriptPublish(ScriptRoundPrefix() + g_context.localizer.Text(
                                                     "script.status.teleport_timeout",
                                                     " · teleport timed out, moving on"));
        }
        return;
    }
    case ScriptStep::settle: {
        if (now - g_script_step_at < kScriptSettleDuration) return;
        ScriptEnterStep(g_script_settle_next, now);
        ScriptPublish(ScriptRoundPrefix() + " · " + ScriptStepName(g_script_settle_next));
        return;
    }
    case ScriptStep::refresh: {
        if (!g_script_step_started) {
            g_script_step_started = true;
            g_script_deadline = now + kScriptRefreshTimeout;
            g_ue_refresh_done.store(0);
            g_ue_refresh_ok.store(0);
            g_ue_refresh_requested.store(true);
            return;
        }
        if (g_ue_refresh_done.load() != 0) {
            const bool ok = g_ue_refresh_ok.load() != 0;
            if (!ok) Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 script: refresh failed");
            ScriptEnterStep(ScriptStep::teleport_fight, now);
            ScriptPublish(ScriptRoundPrefix() + g_context.localizer.Text(
                                                     "script.status.to_fight1",
                                                     " · Teleporting to fight point 1"));
            return;
        }
        if (now >= g_script_deadline) {
            g_ue_refresh_stage.store(0);
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 script: refresh timeout");
            ScriptEnterStep(ScriptStep::teleport_fight, now);
            ScriptPublish(ScriptRoundPrefix() + g_context.localizer.Text(
                                                     "script.status.refresh_timeout",
                                                     " · refresh timed out, moving on"));
        }
        return;
    }
    case ScriptStep::combat: {
        if (!g_script_step_started) {
            g_script_step_started = true;
            g_script_deadline = now + kScriptCombatTimeout;
            g_combat_result.store(static_cast<std::int32_t>(CombatResult::none));
            g_combat_start_requested.store(true);
            return;
        }
        const CombatResult result = CombatTakeResult();
        if (result != CombatResult::none) {
            if (g_script_fight_slot == kScriptSlotFight1) {
                // 1 号战斗点打完 → 去 2 号战斗点，再来一轮自动战斗。
                ScriptEnterStep(ScriptStep::teleport_fight2, now);
                const std::string fight1_result = CombatResultName(result);
                const std::array<std::string_view, 1> arguments{
                    std::string_view(fight1_result)};
                ScriptPublish(ScriptRoundPrefix() + g_context.localizer.Format(
                                                         "script.status.to_fight2_after_fight1",
                                                         " · now to fight point 2 (1st: {0})",
                                                         arguments));
                return;
            }
            ScriptFinishRound(CombatResultName(result), now);
            return;
        }
        if (now >= g_script_deadline) {
            if (g_combat_active.load()) g_combat_stop_requested.store(true);
            static_cast<void>(CombatTakeResult());
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 script: combat timeout");
            if (g_script_fight_slot == kScriptSlotFight1) {
                ScriptEnterStep(ScriptStep::teleport_fight2, now);
                ScriptPublish(ScriptRoundPrefix() + g_context.localizer.Text(
                                                         "script.status.to_fight2_timeout",
                                                         " · Teleporting to fight point 2 "
                                                         "(fight point 1 timed out)"));
                return;
            }
            ScriptFinishRound(
                g_context.localizer.Text("script.detail.combat_timeout", "Combat timed out"),
                now);
        }
        return;
    }
    default:
        return;
    }
}

}  // namespace
