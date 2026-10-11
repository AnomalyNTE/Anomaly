#pragma once
// 自动战斗：复用 plugins/common/combat 的共享状态机（与 CloneEnter / 打怪资源点同一份源码，
// 由静态库 anomaly_plugin_combat 提供），本插件只负责接线与策略选择。
//
// 与那两个调用方的差别只在攻击策略：**技能优先**——先试大招、再试小技能（都要求冷却已走完），
// 没有可放的技能才平A；平A / 小技能 / 大招 三项各由一个勾选框控制
// （见 combat::Host 的 policy_custom 一组字段；为假时保持模块原有行为）。
//
// 结束判据（沿用打怪资源点/CloneEnter 的成熟做法）：
//   ① 模块报 cleared —— 见过怪之后连续 5 秒没有怪；
//   ② 见过怪，且「最近一次伤害 / 最近一次换目标 / 开打时刻」里最晚的那个到现在已超过设定秒数
//      ⇒ 判定打完。这是伤害流读不到时的主判据（日志实测过打死了也收不到伤害事件）；
//   ③ 模块报 attack_failed —— 普攻/技能调用失败，打不动。

namespace {

using anomaly::plugins::combat::Host;
using anomaly::plugins::combat::Result;
using anomaly::plugins::combat::State;

// 技能输入号与 NTE 战斗示例一致：2 = 小技能，3 = 大招。
constexpr std::int32_t kCombatSmallInputId = 2;
constexpr std::int32_t kCombatUltimateInputId = 3;
constexpr std::uint32_t kCombatSearchRadiusMeters = 50;
constexpr double kCombatNoDamageDefaultSeconds = 20.0;
constexpr double kCombatNoDamageMaximumSeconds = 120.0;
constexpr std::size_t kCombatSecondsTextCapacity = 16;

// on_draw 置位、on_update 消费。
std::atomic_bool g_combat_start_requested{};
std::atomic_bool g_combat_stop_requested{};
std::atomic_bool g_combat_active{};
// 三个勾选框：自动战斗中是否使用 平A / 小技能 / 大招。
std::atomic_bool g_combat_use_attack{true};
std::atomic_bool g_combat_use_small{true};
std::atomic_bool g_combat_use_ultimate{true};
// 结束原因回显（打完后由调用方读取）。
std::atomic_bool g_combat_cleared{};
std::atomic_bool g_combat_attack_failed{};
std::atomic_bool g_combat_running{};

State g_combat_state;
std::mutex g_combat_mutex;
std::string g_combat_status;

// 兜底计时的参考点（打怪资源点 NoDamageForTooLong 的同款做法）。
std::chrono::steady_clock::time_point g_combat_started_at{};
std::chrono::steady_clock::time_point g_combat_target_at{};
bool g_combat_had_target{};

// 「多久没伤害算打完」的输入缓冲（on_draw 线程独占，ImGui 文本存储必须跨帧保留）。
std::array<char, kCombatSecondsTextCapacity> g_combat_no_damage_text{"20"};

void CombatPublish(std::string text) {
    std::scoped_lock lock(g_combat_mutex);
    g_combat_status = std::move(text);
}

std::string CombatStatusText() {
    std::scoped_lock lock(g_combat_mutex);
    return g_combat_status;
}

// 模块的状态回显入口（模块不直接写面板）。
void CombatSetStatus(void*, const std::string& text) noexcept {
    try {
        CombatPublish(text);
    } catch (...) {
    }
}

double CombatNoDamageSeconds() {
    const char* text = g_combat_no_damage_text.data();
    if (text == nullptr || text[0] == '\0') return kCombatNoDamageDefaultSeconds;
    char* end{};
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value <= 0.0) {
        return kCombatNoDamageDefaultSeconds;
    }
    return std::min(value, kCombatNoDamageMaximumSeconds);
}

// actors / entities / combat / skills / skill_invocation / pickup 会在世界加载后才发布，
// 本插件 Load 时查询可能静默拿到空指针。这里只补空的，非空不重查。
void CombatRequireServices() {
    if (g_context.actors != nullptr && g_context.entities != nullptr &&
        g_context.combat != nullptr && g_context.skills != nullptr &&
        g_context.skill_invocation != nullptr && g_context.pickup != nullptr) {
        return;
    }
    const auto view = anomaly::sdk::Host(g_context.host);
    if (g_context.actors == nullptr) {
        g_context.actors = view.Query<AnomalyNteActorsServiceV1>(
            ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).get();
    }
    if (g_context.entities == nullptr) {
        g_context.entities = view.Query<AnomalyNteEntitiesServiceV1>(
            ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION).get();
    }
    if (g_context.combat == nullptr) {
        g_context.combat = view.Query<AnomalyNteCombatServiceV1>(
            ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    }
    if (g_context.skills == nullptr) {
        g_context.skills = view.Query<AnomalyNteSkillsServiceV1>(
            ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
    }
    if (g_context.skill_invocation == nullptr) {
        g_context.skill_invocation = view.Query<AnomalyNteSkillInvocationServiceV1>(
            ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
            ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).get();
    }
    if (g_context.pickup == nullptr) {
        g_context.pickup = view.Query<AnomalyNtePickupServiceV1>(
            ANOMALY_NTE_PICKUP_SERVICE_V1_ID, ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION).get();
    }
}

Host CombatMakeHost() {
    CombatRequireServices();
    Host host;
    host.combat = g_context.combat;
    host.navigation = g_context.navigation;
    host.actors = g_context.actors;
    host.entities = g_context.entities;
    host.names = g_context.names;
    host.objects = g_context.objects;
    host.player = g_context.player;
    host.teleport = g_context.teleport;
    host.session = g_context.session;
    host.signature = g_context.signature;
    host.skills = g_context.skills;
    host.skill_invocation = g_context.skill_invocation;
    host.pickup = g_context.pickup;
    host.search_radius_m = kCombatSearchRadiusMeters;
    host.developer_mode = false;
    // 清空那一刻由模块自己发一次范围拾取（与打怪资源点一致）。
    host.loot_after_kill = true;
    // ★ 本插件的策略：技能优先、平A 兜底。
    host.policy_custom = true;
    host.use_normal_attack = g_combat_use_attack.load();
    host.use_skill_small = g_combat_use_small.load();
    host.use_skill_ultimate = g_combat_use_ultimate.load();
    host.skill_small_input_id = kCombatSmallInputId;
    host.skill_ultimate_input_id = kCombatUltimateInputId;
    host.set_status = &CombatSetStatus;
    host.status_user = nullptr;
    return host;
}

void CombatResetState() {
    Host host = CombatMakeHost();
    anomaly::plugins::combat::Reset(host, g_combat_state);
    g_combat_had_target = false;
    g_combat_started_at = {};
    g_combat_target_at = {};
}

// 供生命周期调用：只清状态，不打日志、不改状态行。
void CombatShutdown() {
    g_combat_start_requested.store(false);
    g_combat_stop_requested.store(false);
    g_combat_active.store(false);
    g_combat_cleared.store(false);
    g_combat_attack_failed.store(false);
    CombatResetState();
}

void CombatFinish(const std::string& reason) {
    g_combat_active.store(false);
    CombatResetState();
    CombatPublish(reason);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 combat: " + reason);
}

void AdvanceCombat() {
    if (g_combat_start_requested.exchange(false)) {
        g_combat_cleared.store(false);
        g_combat_attack_failed.store(false);
        g_combat_had_target = false;
        g_combat_target_at = {};
        g_combat_started_at = std::chrono::steady_clock::now();
        CombatResetState();
        g_combat_active.store(true);
        CombatPublish("自动战斗：开始（技能优先，平A 兜底）");
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 combat: started");
    }
    if (g_combat_stop_requested.exchange(false)) {
        CombatFinish("自动战斗：已停止");
        return;
    }
    if (!g_combat_active.load()) return;

    const auto now = std::chrono::steady_clock::now();
    Host host = CombatMakeHost();
    const Result result = anomaly::plugins::combat::Tick(host, g_combat_state);

    // 只在「从没目标变成有目标」时重置兜底计时。不能用「换目标」做条件：目标会在几具
    // 尸体之间每秒来回切（打怪资源点的日志实测），那样计时会被反复刷新、永远攒不满。
    if (g_combat_state.target_valid && !g_combat_had_target) g_combat_target_at = now;
    g_combat_had_target = g_combat_state.target_valid;

    if (result == Result::cleared) {
        g_combat_cleared.store(true);
        CombatFinish("自动战斗：已清空（见过怪后连续 5 秒无怪）");
        return;
    }
    if (result == Result::unavailable && g_combat_state.attack_failed) {
        g_combat_attack_failed.store(true);
        CombatFinish("自动战斗：打不动目标，已停止");
        return;
    }
    // 兜底判据：见过怪，但「最近伤害 / 最近换目标 / 开打」里最晚的那个已过去太久。
    // 伤害流读不到时这条就是主要判据。
    if (g_combat_state.met_monster) {
        auto reference = g_combat_started_at;
        const auto last_hit = g_combat_state.last_player_hit_at;
        if (last_hit.time_since_epoch().count() != 0 && last_hit > reference) {
            reference = last_hit;
        }
        if (g_combat_target_at.time_since_epoch().count() != 0 &&
            g_combat_target_at > reference) {
            reference = g_combat_target_at;
        }
        const double seconds = CombatNoDamageSeconds();
        if (now - reference >
            std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0))) {
            g_combat_cleared.store(true);
            char reason[96]{};
            std::snprintf(reason, sizeof(reason), "自动战斗：已清空（%.0f 秒没有伤害）", seconds);
            CombatFinish(reason);
        }
    }
}

void CombatContent(const AnomalyUiServiceV1* ui) {
    DrawText(ui, g_context.localizer.Text("combat.title", "自动战斗（技能优先，平A 兜底）"));

    if (ui->checkbox != nullptr) {
        int use_attack = g_combat_use_attack.load() ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("平A（普攻）"), &use_attack) != 0) {
            g_combat_use_attack.store(use_attack != 0);
        }
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 14.0F);
        int use_small = g_combat_use_small.load() ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("小技能（input 2）"), &use_small) != 0) {
            g_combat_use_small.store(use_small != 0);
        }
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 14.0F);
        int use_ultimate = g_combat_use_ultimate.load() ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("大招（input 3）"), &use_ultimate) != 0) {
            g_combat_use_ultimate.store(use_ultimate != 0);
        }
    }

    ui->text(ui->user, StringView("多久没伤害算打完"));
    if (ui->same_line != nullptr) ui->same_line(ui->user, 150.0F, 8.0F);
    if (ui->input_text != nullptr) {
        ui->input_text(ui->user, StringView("##night999-combat-nodmg"),
                       g_combat_no_damage_text.data(), g_combat_no_damage_text.size(),
                       ANOMALY_UI_TEXT_INPUT_V1_NONE);
    } else {
        DrawText(ui, g_combat_no_damage_text.data());
    }
    if (ui->same_line != nullptr) {
        ui->same_line(ui->user, 220.0F, 8.0F);
        DrawText(ui, "（秒）");
    }

    if (ui->button != nullptr) {
        const bool active = g_combat_active.load();
        const int start = ui->button_enabled != nullptr
            ? ui->button_enabled(ui->user, StringView("开始自动战斗"), 0.0F, 0.0F, active ? 0 : 1)
            : ui->button(ui->user, StringView("开始自动战斗"), 0.0F, 0.0F);
        if (start != 0) g_combat_start_requested.store(true);
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const int stop = ui->button_enabled != nullptr
            ? ui->button_enabled(ui->user, StringView("停止自动战斗"), 0.0F, 0.0F, active ? 1 : 0)
            : ui->button(ui->user, StringView("停止自动战斗"), 0.0F, 0.0F);
        if (stop != 0) g_combat_stop_requested.store(true);
    }

    const std::string status = CombatStatusText();
    if (!status.empty()) DrawText(ui, status);
}

}  // namespace
