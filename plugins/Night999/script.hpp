#pragma once
// 脚本主循环：传送至营地 → 刷新怪物 → 传送至 1 号战斗点 → 自动战斗
//              → 传送至 2 号战斗点 → 自动战斗 → 回到第一步。
//
// 每一步都复用已有的成熟模块，本身只做「排序 + 等待」：
//   · 传送：TeleportToPosition（与界面按钮同一条已验证路径）；
//   · 刷新：置 g_ue_refresh_requested，等 ue_direct 的 g_ue_refresh_done 置位；
//   · 战斗：置 g_combat_start_requested，等 CombatTakeResult 返回非 none。
//
// 线程约束：整个状态机只在 Game 回调域（on_update）推进；界面线程只读写几个原子量。
// 传送、刷新、战斗三者都是异步的，所以每一步都用「完成判据 + 超时兜底」两把尺子：
// 超时不会卡死循环，只会在日志里留一条警告，然后进入下一步。

namespace {

// 到达判据只比水平距离：传送点给的 Z 往往高于地面，用三维距离会永远判不到达。
constexpr double kScriptArriveRadiusMeters = 40.0;
// 单次传送：发出请求最多等这么久；发出之后再等这么久没到就按「已到达」继续。
constexpr std::chrono::seconds kScriptTeleportTimeout{14};
// 每次落地后的稳定等待：流送与区域状态需要一点时间才就绪，紧接着刷新会打空。
constexpr std::chrono::seconds kScriptSettleDuration{2};
constexpr std::chrono::seconds kScriptRefreshTimeout{30};
constexpr std::chrono::minutes kScriptCombatTimeout{15};

// 传送点槽位：0 = 营地、1 = 1 号战斗点、2 = 2 号战斗点。
constexpr int kScriptSlotCamp = 0;
constexpr int kScriptSlotFight1 = 1;
constexpr int kScriptSlotFight2 = 2;

enum class ScriptStep : std::int32_t {
    idle = 0,
    teleport_camp = 1,
    teleport_fight = 2,
    teleport_fight2 = 3,
    settle = 4,
    refresh = 5,
    combat = 6,
};

// 界面线程置位、Game 线程消费。
std::atomic_bool g_script_start_requested{};
std::atomic_bool g_script_stop_requested{};
std::atomic_bool g_script_running{};
std::atomic_int g_script_step{0};
std::atomic_int g_script_round{0};
// 循环次数，0 = 无限循环。
std::atomic<std::uint32_t> g_script_loop_count{0};

std::mutex g_script_mutex;
std::string g_script_status{"脚本：未开始"};

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
    return g_script_status;
}

const char* ScriptStepName(const ScriptStep step) {
    switch (step) {
    case ScriptStep::teleport_camp: return "传送至营地";
    case ScriptStep::teleport_fight: return "传送至1号战斗点";
    case ScriptStep::teleport_fight2: return "传送至2号战斗点";
    case ScriptStep::settle: return "落地稳定";
    case ScriptStep::refresh: return "刷新怪物";
    case ScriptStep::combat: return "自动战斗";
    default: return "空闲";
    }
}

std::string ScriptRoundPrefix() {
    const int round = g_script_round.load();
    const std::uint32_t total = g_script_loop_count.load();
    if (total == 0) return "第 " + std::to_string(round) + " 轮";
    return "第 " + std::to_string(round) + "/" + std::to_string(total) + " 轮";
}

void ScriptEnterStep(const ScriptStep step, const std::chrono::steady_clock::time_point now) {
    g_script_phase = step;
    g_script_step.store(static_cast<int>(step));
    g_script_step_at = now;
    g_script_step_started = false;
    g_script_deadline = now + kScriptTeleportTimeout;
}

// 读取某个槽位的传送点坐标（0 营地 / 1 一号战斗点 / 2 二号战斗点）。
void ScriptReadTarget(const int slot, double target[3]) {
    const int clamped = slot < 0 ? 0 : (slot > kScriptSlotFight2 ? kScriptSlotFight2 : slot);
    std::scoped_lock lock(g_context.mutex);
    const double* source = g_context.settings.targets[clamped];
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
    g_script_step.store(0);
    g_script_round.store(0);
    g_script_phase = ScriptStep::idle;
    g_script_step_started = false;
    ScriptPublish("脚本：未开始");
}

// 停脚本：连同正在跑的刷新与自动战斗一起收尾，避免留下后台流程。
void ScriptStop(const std::string& reason) {
    g_script_running.store(false);
    g_script_step.store(0);
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
        ScriptStop("脚本：已完成 " + std::to_string(round) + " 轮");
        return;
    }
    g_script_round.store(round + 1);
    ScriptEnterStep(ScriptStep::teleport_camp, now);
    ScriptPublish(ScriptRoundPrefix() + " · 传送至营地" +
                  (detail.empty() ? std::string() : "（上轮 " + detail + "）"));
}

void AdvanceScript(const AnomalyHostApiV1* host) {
    if (host == nullptr) return;
    const auto now = std::chrono::steady_clock::now();

    if (g_script_start_requested.exchange(false) && !g_script_running.load()) {
        g_script_round.store(1);
        g_script_running.store(true);
        ScriptEnterStep(ScriptStep::teleport_camp, now);
        ScriptPublish(ScriptRoundPrefix() + " · 传送至营地");
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "night999 script: started, rounds=" + std::to_string(g_script_loop_count.load()));
    }
    if (g_script_stop_requested.exchange(false)) {
        ScriptStop("脚本：已停止");
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
        const char* arrived_text = slot == kScriptSlotCamp
            ? "已到营地"
            : (slot == kScriptSlotFight1 ? "已到1号战斗点" : "已到2号战斗点");
        if (!g_script_step_started) {
            // 第一拍：已经在目标点就直接进下一步，否则发一次传送；发不出去（世界还没就绪）
            // 就继续等，直到超时 —— 只发一次，避免每帧重发把流送打断。
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
                ScriptPublish(ScriptRoundPrefix() + " · 传送失败，继续下一步");
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
            ScriptPublish(ScriptRoundPrefix() + " · 传送超时，继续下一步");
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
            ScriptPublish(ScriptRoundPrefix() + " · 传送至1号战斗点");
            return;
        }
        if (now >= g_script_deadline) {
            g_ue_refresh_stage.store(0);
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 script: refresh timeout");
            ScriptEnterStep(ScriptStep::teleport_fight, now);
            ScriptPublish(ScriptRoundPrefix() + " · 刷新超时，继续下一步");
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
                ScriptPublish(ScriptRoundPrefix() + " · 传送至2号战斗点（1号 " +
                              CombatResultName(result) + "）");
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
                ScriptPublish(ScriptRoundPrefix() + " · 传送至2号战斗点（1号战斗超时）");
                return;
            }
            ScriptFinishRound("战斗超时", now);
        }
        return;
    }
    default:
        return;
    }
}

}  // namespace
