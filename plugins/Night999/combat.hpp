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
// 搜索半径（米）：模块只认这个范围内的怪，与打怪资源点的 search_radius_m 同一路数。
constexpr double kCombatRadiusDefaultMeters = 15.0;
constexpr double kCombatRadiusMinimumMeters = 5.0;
constexpr double kCombatRadiusMaximumMeters = 300.0;
// 首次接敌超时（秒）：开打后一次怪都没遇到过超过它 ⇒ 判定这个点没怪，直接进下一步。
// 与打怪资源点的 first_contact_timeout_seconds 同一路数。
constexpr double kCombatFirstContactDefaultSeconds = 15.0;
constexpr double kCombatFirstContactMinimumSeconds = 1.0;
constexpr double kCombatFirstContactMaximumSeconds = 120.0;
constexpr double kCombatNoDamageDefaultSeconds = 10.0;
constexpr double kCombatNoDamageMinimumSeconds = 1.0;
constexpr double kCombatNoDamageMaximumSeconds = 300.0;
constexpr std::size_t kCombatSecondsTextCapacity = 16;
constexpr std::size_t kCombatRadiusTextCapacity = 8;

// 自动战斗的收尾原因。脚本循环据此决定下一步（cleared/timeout/attack_failed 都可走下一步）。
enum class CombatResult : std::int32_t {
    none = 0,
    cleared = 1,
    first_contact_timeout = 2,
    attack_failed = 3,
    stopped = 4,
};

// on_draw 置位、on_update 消费。
std::atomic_bool g_combat_start_requested{};
std::atomic_bool g_combat_stop_requested{};
std::atomic_bool g_combat_active{};
// 三个勾选框：自动战斗中是否使用 平A / 小技能 / 大招。
std::atomic_bool g_combat_use_attack{true};
std::atomic_bool g_combat_use_small{true};
std::atomic_bool g_combat_use_ultimate{true};
// 结束原因（打完后由调用方读取；CombatTakeResult 取走并清零）。
std::atomic<std::int32_t> g_combat_result{static_cast<std::int32_t>(CombatResult::none)};
// 面板参数：on_draw 解析后写进原子量，on_update 只读原子量，避免跨线程读 char 缓冲。
std::atomic<double> g_combat_no_damage_seconds{kCombatNoDamageDefaultSeconds};
std::atomic<double> g_combat_first_contact_seconds{kCombatFirstContactDefaultSeconds};
std::atomic<std::uint32_t> g_combat_radius_meters{
    static_cast<std::uint32_t>(kCombatRadiusDefaultMeters)};

State g_combat_state;
std::mutex g_combat_mutex;
std::string g_combat_status;

// 兜底计时的参考点（打怪资源点 NoDamageForTooLong 的同款做法）。
std::chrono::steady_clock::time_point g_combat_started_at{};
std::chrono::steady_clock::time_point g_combat_target_at{};
bool g_combat_had_target{};

// 三个参数的输入缓冲（on_draw 线程独占，ImGui 文本存储必须跨帧保留）。
std::array<char, kCombatSecondsTextCapacity> g_combat_no_damage_text{"10"};
std::array<char, kCombatSecondsTextCapacity> g_combat_first_contact_text{"15"};
std::array<char, kCombatRadiusTextCapacity> g_combat_radius_text{"15"};

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

// 解析一个数值输入框；非法输入回落到默认值，再夹到 [minimum, maximum]。
double CombatParseNumber(const char* text, const double fallback, const double minimum,
                         const double maximum) {
    if (text == nullptr || text[0] == '\0') return fallback;
    char* end{};
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value)) return fallback;
    return std::min(std::max(value, minimum), maximum);
}

// 面板 → 原子量（在 on_draw 里调用；on_update 只读原子量）。
void CombatSyncParameters() {
    g_combat_no_damage_seconds.store(CombatParseNumber(
        g_combat_no_damage_text.data(), kCombatNoDamageDefaultSeconds,
        kCombatNoDamageMinimumSeconds, kCombatNoDamageMaximumSeconds));
    g_combat_first_contact_seconds.store(CombatParseNumber(
        g_combat_first_contact_text.data(), kCombatFirstContactDefaultSeconds,
        kCombatFirstContactMinimumSeconds, kCombatFirstContactMaximumSeconds));
    g_combat_radius_meters.store(static_cast<std::uint32_t>(CombatParseNumber(
        g_combat_radius_text.data(), kCombatRadiusDefaultMeters, kCombatRadiusMinimumMeters,
        kCombatRadiusMaximumMeters)));
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
    host.search_radius_m = g_combat_radius_meters.load();
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
    g_combat_result.store(static_cast<std::int32_t>(CombatResult::none));
    CombatResetState();
}

void CombatFinish(const CombatResult result, const std::string& reason) {
    g_combat_active.store(false);
    g_combat_result.store(static_cast<std::int32_t>(result));
    CombatResetState();
    CombatPublish(reason);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 combat: " + reason);
}

// 取走并清零本次结果（脚本循环用它决定下一步）。
CombatResult CombatTakeResult() {
    return static_cast<CombatResult>(g_combat_result.exchange(
        static_cast<std::int32_t>(CombatResult::none)));
}

const char* CombatResultName(const CombatResult result) {
    switch (result) {
    case CombatResult::cleared: return "已清空";
    case CombatResult::first_contact_timeout: return "首次接敌超时";
    case CombatResult::attack_failed: return "打不动目标";
    case CombatResult::stopped: return "已停止";
    default: return "";
    }
}

void AdvanceCombat() {
    if (g_combat_start_requested.exchange(false)) {
        g_combat_result.store(static_cast<std::int32_t>(CombatResult::none));
        g_combat_had_target = false;
        g_combat_target_at = {};
        g_combat_started_at = std::chrono::steady_clock::now();
        CombatResetState();
        g_combat_active.store(true);
        CombatPublish("自动战斗：开始（技能优先，平A 兜底）");
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 combat: started");
    }
    if (g_combat_stop_requested.exchange(false)) {
        CombatFinish(CombatResult::stopped, "自动战斗：已停止");
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
        CombatFinish(CombatResult::cleared, "自动战斗：已清空（见过怪后连续 5 秒无怪）");
        return;
    }
    if (result == Result::unavailable && g_combat_state.attack_failed) {
        CombatFinish(CombatResult::attack_failed, "自动战斗：打不动目标，已停止");
        return;
    }
    // 首次接敌超时：开打至今一次怪都没遇到过（搜索半径内始终没目标）⇒ 判定这个点没怪，
    // 把结果交出去，脚本据此进入下一步。与打怪资源点的 first_contact_timeout 一致。
    const double first_contact = g_combat_first_contact_seconds.load();
    if (!g_combat_state.met_monster &&
        now - g_combat_started_at >
            std::chrono::milliseconds(static_cast<long long>(first_contact * 1000.0))) {
        char reason[128]{};
        std::snprintf(reason, sizeof(reason),
                      "自动战斗：首次接敌超时（%.0f 秒没遇到怪），进入下一步", first_contact);
        CombatFinish(CombatResult::first_contact_timeout, reason);
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
        const double seconds = g_combat_no_damage_seconds.load();
        if (now - reference >
            std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0))) {
            char reason[128]{};
            std::snprintf(reason, sizeof(reason), "自动战斗：已清空（%.0f 秒没有伤害）", seconds);
            CombatFinish(CombatResult::cleared, reason);
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

    // 三个数值参数：首次接敌超时 / 多久没伤害算打完 / 搜索半径。单位直接写进标签，
    // 因为输入框本身占掉窗口宽度的 65%，后面再挂说明文字会被挤出窗口。
    LabeledField(ui, "首次接敌超时(秒)", "##night999-combat-first-contact",
                 g_combat_first_contact_text.data(), g_combat_first_contact_text.size());
    LabeledField(ui, "多久没伤害算打完(秒)", "##night999-combat-nodmg",
                 g_combat_no_damage_text.data(), g_combat_no_damage_text.size());
    LabeledField(ui, "搜索半径(米)", "##night999-combat-radius", g_combat_radius_text.data(),
                 g_combat_radius_text.size());
    // 解析进原子量：on_update 只读原子量，不跨线程读这些 char 缓冲。
    CombatSyncParameters();

    // 自动战斗由脚本循环驱动，面板不再提供独立的开始/停止按钮。
    const std::string status = CombatStatusText();
    if (!status.empty()) DrawText(ui, status);
}

}  // namespace
