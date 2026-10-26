#pragma once
// 自动战斗：复用 plugins/common/combat 的共享状态机（静态库 anomaly_plugin_combat）。
// 策略：技能优先（大招 → 小技能 → 平A），三项各由一个勾选框控制。
// 结束判据只有两条，都不依赖伤害流：模块报 cleared（搜索半径内连续 5 秒无怪）或 attack_failed
// （普攻/技能调用失败）。「多久没伤害」不能当判据——伤害流读不到时它在怪还活着时就成立。

namespace {

using anomaly::plugins::combat::Host;
using anomaly::plugins::combat::Result;
using anomaly::plugins::combat::State;

// 技能输入号与 NTE 战斗示例一致：2 = 小技能，3 = 大招。
constexpr std::int32_t kCombatSmallInputId = 2;
constexpr std::int32_t kCombatUltimateInputId = 3;
// 搜索半径（米）：模块只认这个范围内的怪。
constexpr double kCombatRadiusDefaultMeters = 15.0;
constexpr double kCombatRadiusMinimumMeters = 5.0;
constexpr double kCombatRadiusMaximumMeters = 300.0;
// 首次接敌超时（秒）：开打后一次怪都没遇到过超过它 ⇒ 判定这个点没怪，直接进下一步。
constexpr double kCombatFirstContactDefaultSeconds = 15.0;
constexpr double kCombatFirstContactMinimumSeconds = 1.0;
constexpr double kCombatFirstContactMaximumSeconds = 120.0;
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
std::atomic<double> g_combat_first_contact_seconds{kCombatFirstContactDefaultSeconds};
std::atomic<std::uint32_t> g_combat_radius_meters{
    static_cast<std::uint32_t>(kCombatRadiusDefaultMeters)};

State g_combat_state;
std::mutex g_combat_mutex;
std::string g_combat_status;

// 本次自动战斗的开始时刻：首次接敌超时以它为基准。
std::chrono::steady_clock::time_point g_combat_started_at{};
// 开打那一刻模块记下的「最近一次玩家命中」：只有本次战斗内被刷新，才说明伤害流真的在工作。
std::chrono::steady_clock::time_point g_combat_hit_baseline{};

// 两个参数的输入缓冲（on_draw 线程独占，ImGui 文本存储必须跨帧保留）。
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
    // 清空那一刻由模块发一次范围拾取。
    host.loot_after_kill = true;
    // 本插件的策略：技能优先、平A 兜底。
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
    g_combat_started_at = {};
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
    // 本次战斗内伤害流是否报过玩家命中（只做诊断，不参与判定）。
    const bool saw_player_hit = g_combat_state.last_player_hit_at > g_combat_hit_baseline;
    g_combat_active.store(false);
    g_combat_result.store(static_cast<std::int32_t>(result));
    CombatResetState();
    CombatPublish(reason);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "night999 combat: " + reason +
            (saw_player_hit ? " [damage observed]" : " [damage never observed]"));
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
        g_combat_started_at = std::chrono::steady_clock::now();
        g_combat_hit_baseline = g_combat_state.last_player_hit_at;
        CombatResetState();
        g_combat_active.store(true);
        CombatPublish("自动战斗：开始（技能优先，平A 兜底）");
        // 伤害流入口是否随服务发布：缺失是能力问题，齐全却收不到事件才是偏移或句柄问题。
        const bool stream_api = g_context.combat != nullptr &&
            g_context.combat->latest_damage_sequence != nullptr &&
            g_context.combat->next_damage_event != nullptr;
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            std::string("night999 combat: started, damage stream ") +
                (stream_api ? "present" : "absent"));
    }
    if (g_combat_stop_requested.exchange(false)) {
        CombatFinish(CombatResult::stopped, "自动战斗：已停止");
        return;
    }
    if (!g_combat_active.load()) return;

    const auto now = std::chrono::steady_clock::now();
    Host host = CombatMakeHost();
    const Result result = anomaly::plugins::combat::Tick(host, g_combat_state);

    if (result == Result::cleared) {
        CombatFinish(CombatResult::cleared, "自动战斗：已清空（见过怪后连续 5 秒无怪）");
        return;
    }
    if (result == Result::unavailable && g_combat_state.attack_failed) {
        CombatFinish(CombatResult::attack_failed, "自动战斗：打不动目标，已停止");
        return;
    }
    // 首次接敌超时：开打至今没遇到过怪 ⇒ 判定这个点没怪。
    const double first_contact = g_combat_first_contact_seconds.load();
    if (!g_combat_state.met_monster &&
        now - g_combat_started_at >
            std::chrono::milliseconds(static_cast<long long>(first_contact * 1000.0))) {
        char reason[128]{};
        std::snprintf(reason, sizeof(reason),
                      "自动战斗：首次接敌超时（%.0f 秒没遇到怪），进入下一步", first_contact);
        CombatFinish(CombatResult::first_contact_timeout, reason);
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

    // 单位写进标签：输入框已占掉窗口宽度的 65%，后面挂说明会被挤出窗口。
    LabeledField(ui, "首次接敌超时(秒)", "##night999-combat-first-contact",
                 g_combat_first_contact_text.data(), g_combat_first_contact_text.size());
    // 半径就是「打完」的定义：半径内连续 5 秒无怪才收工。
    LabeledField(ui, "搜索半径(米)", "##night999-combat-radius", g_combat_radius_text.data(),
                 g_combat_radius_text.size());
    // 解析进原子量：on_update 只读原子量，不跨线程读这些 char 缓冲。
    CombatSyncParameters();

    // 自动战斗由脚本循环驱动，面板不再提供独立的开始/停止按钮。
    const std::string status = CombatStatusText();
    if (!status.empty()) DrawText(ui, status);
}

}  // namespace
