// NTE 伤害仪表盘 (Damage Dashboard)
// ---------------------------------------------------------------------------
// 数据来源：anomaly.nte.combat 的「统一战斗事件流」（latest_event_sequence /
//           next_event），事件 kind 为 DAMAGE / HEAL / BUFF_ADD / BUFF_REMOVE。
// 实现基准：sdk/share/anomaly/examples/nte_combat_demo/plugin.cpp。
//
// 关键坑：宿主在插件 on_load 阶段**不会**交出 anomaly.nte.combat 服务表
// （profile 未校验 / 未进世界时 query_service 直接失败）。官方 Demo 的做法是
// 在每次 on_update 里重新 query，所以这里也每帧重查（见 RefreshServices）。
//
// 统计口径（v2，单界面）：
//   1. 本场战斗 —— 从「进入战斗」到「战斗结束」的累计值。战斗开始 = 闲置
//      idleSeconds 秒后出现第一发伤害；战斗结束 = 再闲置 idleSeconds 秒。
//      战斗结束后面板保留最后一场的数字，直到下一场开始。
//   2. 队伍条形图 —— 本地角色 + 队友（≤4 人）各自造成的伤害与占队伍百分比。
//      队友由事件证据自动判定（治疗/被治疗/被上增益/共同攻击同一敌人）。
//   3. 技能来源 —— 本场战斗内每个 name_id（技能/伤害源）的次数与总量。
//   4. 增益倒计时 —— BUFF_ADD 的 duration_seconds 起算，实时倒数到结束。
//   5. 最近命中 —— 最近若干条事件的时间线。
//
// 线程域：on_update=Game 线程（唯一数据变更点），on_draw=Render 线程（只读加锁拷贝）。
// ---------------------------------------------------------------------------

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/plugin.h"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/json.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>

// 宿主用 /permissive- 构建插件；windows.h 的 min/max 宏会与 std::min/std::max
// 冲突（C2589），所以先抑制这两个宏再引入。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using anomaly::sdk::Host;

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

constexpr std::size_t kMaximumResolvedNameBytes = 512;
constexpr std::size_t kMaximumDrainPerUpdate = 256;
constexpr std::size_t kMaximumNameQueriesPerUpdate = 6;
constexpr std::size_t kMaximumNameRetries = 32;
constexpr std::uint64_t kNameRetryDelayUpdates = 30;
constexpr std::size_t kMaximumPendingNames = 512;

constexpr std::size_t kMaximumRecentHits = 16;      // 最近命中展示条数
constexpr std::size_t kMaximumTrackedHandles = 32;  // 每个角色记住的交手对象上限
constexpr std::size_t kMaximumTeamRows = 6;         // 队伍条形图行数上限
constexpr std::size_t kMaximumSkillRows = 8;        // 技能表行数上限
constexpr std::size_t kMaximumBuffRows = 12;        // 增益倒计时行数上限
constexpr std::size_t kMaximumBattleHistory = 5;    // 历史战斗保留场数
constexpr std::size_t kMaximumBuffTracked = 64;     // 同时跟踪的增益上限

constexpr double kDefaultBattleIdleSeconds = 6.0;   // 闲置这么久判定战斗结束
constexpr double kMaximumFrameDelta = 0.25;         // 单帧 delta 上限，防时间跳变

// 条形行的实际宽度约 66 个 ASCII 字符（名称列 12 + 条形 22 + 百分比 7 + 数值）。
// 默认宽度必须容得下它，否则文字会被窗口右缘裁掉，用户又得去横向滚动。
constexpr float kDashboardDefaultWidth = 620.0f;
constexpr float kDashboardDefaultHeight = 360.0f;
constexpr float kDashboardMinimumWidth = 300.0f;
constexpr float kDashboardMinimumHeight = 220.0f;

// 一队最多 4 人（游戏里最多只能选 4 名角色出战），
// 「我方角色」集合的硬上限，防止把路人/敌人错认成我的队员。
constexpr std::size_t kMaximumPartySize = 4;

// ---------------------------------------------------------------------------
// 通用工具
// ---------------------------------------------------------------------------
AnomalyStringViewV1 Sv(const char* text) {
    return {text, std::strlen(text)};
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool IsSizingStatus(const AnomalyStatusV1 status) noexcept {
    return status.code == ANOMALY_STATUS_V1_OK ||
        status.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL;
}

// 判定两个句柄是不是同一个角色。
//
// **只比 id，不比 generation。** generation 是宿主的「对象注册表世代号」，
// 宿主每次重新扫描对象注册表都会让它自增，而同一个角色 pawn 的 id 是不变的
// —— id 由对象在注册表里的槽位与序列号唯一决定。
// 之前要求 id 和 generation 都相等，结果注册表每变一次，同一个角色就被当成
// 一个新角色：队伍里出现两个同名条目，还会占掉两个队伍名额，
// 导致后面真正的队友没位置。
bool SameHandle(const AnomalyGenerationHandleV1 a,
                const AnomalyGenerationHandleV1 b) noexcept {
    return a.id != 0 && a.id == b.id;
}

std::uint64_t ParticipantCacheKey(const AnomalyGenerationHandleV1 handle) noexcept {
    // 只用 id 做 key，理由同 SameHandle：generation 会随宿主刷新注册表不断变化，
    // 混进来会把同一个角色拆成多个聚合条目（表现为队伍表里同名角色出现两次）。
    return handle.id;
}

std::uint64_t SourceCacheKey(const std::uint64_t world_id, const std::uint64_t source_id) noexcept {
    return source_id ^ (world_id + 0x9E3779B97F4A7C15ULL + (world_id << 6U) + (world_id >> 2U));
}

std::uint64_t BuffCacheKey(const std::uint64_t name_id,
                           const AnomalyGenerationHandleV1 target) noexcept {
    // 同样只用 target.id；否则同一个增益会因为注册表世代变化被拆成多条、重复显示。
    return name_id ^ (target.id + 0x2545F4914F6CDD1DULL + (target.id << 6U) + (target.id >> 2U));
}

// 有界追加，避免重复且不无限增长。
void PushBoundedHandle(std::vector<AnomalyGenerationHandleV1>& list,
                       const AnomalyGenerationHandleV1 handle) {
    if (handle.id == 0) return;
    for (const auto& existing : list) {
        if (SameHandle(existing, handle)) return;
    }
    if (list.size() >= kMaximumTrackedHandles) return;
    list.push_back(handle);
}

bool SharesHandle(const std::vector<AnomalyGenerationHandleV1>& left,
                  const std::vector<AnomalyGenerationHandleV1>& right) noexcept {
    for (const auto& a : left) {
        for (const auto& b : right) {
            if (SameHandle(a, b)) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 数据模型
// ---------------------------------------------------------------------------

enum RoleV1 {
    kRoleUnknown = 0,
    kRoleLocal = 1,
    kRoleParty = 2,
    kRoleEnemy = 3,
};

const char* RoleLabel(const int role) {
    switch (role) {
        case kRoleLocal: return "我";
        case kRoleParty: return "队友";
        case kRoleEnemy: return "敌人";
        default: return "未识别";
    }
}

// 按「技能/伤害来源」聚合：name_id 标识技能或生成期局部 source。
struct SourceAggregate {
    std::uint64_t name_id{};
    std::uint64_t world_id{};
    std::string name;
    bool name_resolved{};

    std::uint64_t hits{};
    std::uint64_t crit_hits{};
    std::uint64_t crit_known{};
    std::uint64_t head_hits{};
    std::int64_t final_total{};
    std::int64_t max_hit{};
    std::uint64_t last_sequence{};

    // 本场战斗口径
    std::uint64_t battle_hits{};
    std::int64_t battle_final_total{};

    // 归并键：宿主给同一技能可能分配不同 name_id（例如带层数/变体），
    // 名字解析出来后统一按名字归并；解析前先按 name_id 分开。
    std::string merge_key;
};

// 按「角色」聚合。out_* = 其造成的伤害，in_* = 其承受的伤害，
// heal_* = 其造成的治疗，buff_* = 其施加/承受的增益。
// battle_* 是「本场战斗」口径，战斗开始时清零。
struct CharacterAggregate {
    AnomalyGenerationHandleV1 handle{};
    std::string name;
    bool name_resolved{};
    bool is_local{};

    // 该角色用过的技能：key = SourceCacheKey(world_id, name_id)，
    // 名字解析后按名字归并（同名技能合并成一行）。
    std::unordered_map<std::uint64_t, SourceAggregate> sources;

    std::uint64_t out_hits{};
    std::uint64_t out_crit{};
    std::uint64_t out_crit_known{};
    std::uint64_t out_head{};
    std::int64_t out_final{};
    std::int64_t out_max_hit{};
    std::uint64_t in_hits{};
    std::int64_t in_final{};
    std::uint64_t heal_hits{};
    std::int64_t heal_total{};
    std::uint64_t heal_received_hits{};
    std::int64_t heal_received_total{};
    std::uint64_t buff_applied{};
    std::uint64_t buff_removed{};
    std::uint64_t buff_received{};

    // 本场战斗口径
    std::uint64_t battle_out_hits{};
    std::int64_t battle_out_final{};
    std::int64_t battle_out_max_hit{};
    std::uint64_t battle_in_hits{};
    std::int64_t battle_in_final{};
    std::uint64_t battle_heal_hits{};
    std::int64_t battle_heal_total{};

    // 角色判定证据
    std::vector<AnomalyGenerationHandleV1> attacked;      // 我打过谁
    std::vector<AnomalyGenerationHandleV1> attacked_by;   // 谁打过我
    bool damaged_local{};        // 我打过本地角色
    bool damaged_by_local{};     // 本地角色打过我
    bool healed_anyone{};        // 我治疗过别人
    bool healed_by_local{};      // 我被本地角色治疗
    bool buffed_by_local{};      // 我被本地角色上过增益

    std::uint64_t last_sequence{};
};

// 生效中的增益（用于倒计时）。
struct ActiveBuff {
    std::uint64_t name_id{};
    std::string name;
    bool name_resolved{};
    AnomalyGenerationHandleV1 target{};
    std::string target_name;
    bool target_resolved{};
    AnomalyGenerationHandleV1 source{};
    std::int64_t stacks{};
    double duration_seconds{};
    double applied_at{};
    // 固定增益：战斗开始前就挂着、或同一个 buff 被重复施加/刷新多次。
    // 这类 buff 没有明确的「结束时刻」，只由 BUFF_REMOVE 移除，不参与
    // duration 过期淘汰，也不显示倒计时。
    bool persistent{};
};

// 一场战斗的存档（战斗结束后保留，用于「上一场」）。
struct BattleRecord {
    bool valid{};
    double started_at{};
    double ended_at{};
    std::uint64_t hits{};
    std::uint64_t crit{};
    std::uint64_t crit_known{};
    std::uint64_t head{};
    std::int64_t out_final{};
    std::int64_t max_hit{};
    std::uint64_t taken_hits{};
    std::int64_t taken_final{};
    std::uint64_t heal_hits{};
    std::int64_t heal_total{};
    std::string top_character;
    std::int64_t top_character_damage{};
};

struct RecentHit {
    double time{};
    std::uint32_t kind{};
    std::string source;
    std::string target;
    std::string name;
    std::int64_t value{};
    bool critical{};
    bool head_hit{};
    bool weak{};
    bool incoming{};
    bool outgoing{};
};

struct PendingNameQuery {
    AnomalyGenerationHandleV1 handle{};
    std::uint64_t retry_at_update{};
    std::uint32_t attempts{};
};

struct PendingEventName {
    std::uint64_t name_id{};
    std::uint64_t retry_at_update{};
    std::uint32_t attempts{};
};

// 宿主持久化配置（on_start 读 / 运行期改内存 / on_stop 写回）。
struct Settings {
    double battle_idle_seconds{kDefaultBattleIdleSeconds};
    bool show_skills{true};
    bool show_buffs{true};
    bool show_recent{true};
    std::uint32_t max_rows{kMaximumSkillRows};

    // 窗口尺寸（用户拖拽 / 点按钮改过之后记下来，下次启动沿用）。
    // 0 表示「没设过」，此时用默认尺寸。
    // 高度字段保留只为兼容旧配置；高度实际永远由内容决定（见 OnDraw），
    // 因为固定高度会逼出滚动条，而滚动会把插件搞崩。
    double window_width{};
    double window_height{};
};

// 唯一跨回调的状态。
struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::sdk::ServiceRef<AnomalyNteCombatServiceV1> combat;
    anomaly::sdk::ServiceRef<AnomalyNteSessionServiceV1> session;
    anomaly::sdk::ServiceRef<AnomalyJsonServiceV1> json;
    anomaly::sdk::ServiceRef<AnomalyConfigServiceV1> config;
    // anomaly.core 的 log() 会把消息送进平台「诊断 → 日志」页，
    // 这是插件在 Anomaly 界面里留下可诊断信息的唯一正确通道。
    anomaly::sdk::ServiceRef<AnomalyCoreServiceV1> core;
    // anomaly.diagnostics 的 register_self_test() 会在开发者资源页
    // 挂一个可运行的「自检」条目，返回 JSON 摘要。
    anomaly::sdk::ServiceRef<AnomalyDiagnosticsServiceV1> diagnostics;
    // anomaly.notifications 用来在游戏里弹提示——这是用户唯一真正「看得见」的
    // 插件自述通道（诊断页的 Logs 只显示文字记录，需要主动去点开）。
    anomaly::sdk::ServiceRef<AnomalyNotificationsServiceV1> notifications;
    bool self_test_registered{};
    AnomalyGenerationHandleV1 self_test_handle{};
    // core 在 on_load 阶段可能还没就绪；标记下来在 on_update 里重试。
    bool core_deferred{};
    bool load_banner_logged{};
    // 服务可用性只在状态翻转时提醒一次，避免每帧刷屏。
    bool combat_service_ready{};
    bool combatant_invalid_reported{};

    // 生命周期
    bool started{};
    int window_open{1};
    bool config_schema_registered{};
    AnomalyGenerationHandleV1 config_schema{};
    bool settings_dirty{};
    bool settings_loaded{};

    // 窗口尺寸运行期状态：每帧读回宿主真实尺寸，只有用户真的拖动过才写回配置，
    // 避免把宿主自己的贴合结果当成用户意图反复覆盖。
    float last_window_width{};
    float last_window_height{};
    bool window_size_observed{};
    // 用户亲手拖过尺寸后置位：此后插件不再调用 set_next_window_size，
    // 否则每帧都会把用户拖出来的尺寸抢回去。
    bool window_user_resized{};

    // 插件时钟（on_update 的 delta 累加，事件时间戳都基于它）
    double session_time{};
    std::uint64_t drain_update_counter{};

    // 时钟校准：宿主 tick_sequence 是权威帧序号，用它测出「每 tick 多少秒」，
    // 再反过来校正插件时钟，避免累计误差导致增益倒计时越走越偏。
    std::uint64_t clock_last_tick{};
    double clock_last_tick_time{};
    double clock_tick_rate{0.0};     // 每 tick 的秒数估计，0=未校准
    bool clock_calibrated{};
    std::uint64_t last_drained_tick{};   // 最近一次 drain 到的事件 tick_sequence

    // 游标
    std::uint64_t event_cursor{};
    bool cursor_primed{};
    bool stream_partial{};

    // 权威统计（宿主 statistics()）
    bool authority_valid{};
    AnomalyNteCombatStatisticsV1 outgoing_stats{};
    AnomalyNteCombatStatisticsV1 incoming_stats{};

    // 本地角色
    bool combatant_valid{};
    AnomalyGenerationHandleV1 local_character{};
    AnomalyGenerationHandleV1 world{};
    double local_hp{};
    double local_max_hp{};
    double local_shield{};
    bool local_dead{};
    std::vector<AnomalyGenerationHandleV1> local_attackers;   // 打过本地角色的对象
    // 我操控过的全部角色（队长 + 另三名队员）。四名角色并非同时在场，
    // 但同一个战斗周期内他们的伤害必须一起累加，所以「我」是一个集合。
    std::vector<AnomalyGenerationHandleV1> party_handles;

    // 会话状态感知（进入/脱离战斗场景）
    bool session_world_ready{};
    std::uint64_t session_event_cursor{};

    bool IsMine(const AnomalyGenerationHandleV1& handle) const {
        if (handle.id == 0) return false;
        if (SameHandle(handle, local_character)) return true;
        for (const auto& known : party_handles) {
            if (SameHandle(known, handle)) return true;
        }
        return false;
    }

    void RememberPartyHandle(const AnomalyGenerationHandleV1& handle) {
        if (handle.id == 0 || IsMine(handle)) return;
        // 一队最多 4 人，满员后不再收录新角色。
        // 之前上限是 8 且满了就整体清空重记，结果会出现第 5、第 6 个「我方角色」，
        // 把他们打出的伤害也算进了我的统计里。
        if (party_handles.size() >= kMaximumPartySize) return;
        party_handles.push_back(handle);
    }

    // 本场战斗
    bool battle_active{};
    double battle_started_at{};
    double battle_last_event_at{};
    std::uint64_t battle_hits{};
    std::uint64_t battle_crit{};
    std::uint64_t battle_crit_known{};
    std::uint64_t battle_head{};
    std::int64_t battle_out_final{};
    std::int64_t battle_max_hit{};
    std::uint64_t battle_taken_hits{};
    std::int64_t battle_taken_final{};
    std::uint64_t battle_heal_hits{};
    std::int64_t battle_heal_total{};
    std::vector<BattleRecord> battle_history;

    // 全程（本次进入世界以来）
    std::uint64_t total_hits{};
    std::uint64_t total_crit{};
    std::uint64_t total_crit_known{};
    std::uint64_t total_head{};
    std::int64_t total_final{};
    std::int64_t total_max_hit{};
    std::uint64_t taken_hits{};
    std::int64_t taken_final{};
    std::uint64_t heal_out_hits{};
    std::int64_t heal_out_total{};
    std::uint64_t heal_in_hits{};
    std::int64_t heal_in_total{};
    std::uint64_t buff_applied_total{};
    std::uint64_t buff_received_total{};
    std::uint64_t buff_removed_total{};
    std::uint64_t damage_event_count{};
    std::uint64_t heal_event_count{};
    std::uint64_t buff_event_count{};
    std::uint64_t total_event_count{};
    std::uint64_t battle_count{};

    // 聚合表
    std::unordered_map<std::uint64_t, SourceAggregate> sources;
    std::unordered_map<std::uint64_t, CharacterAggregate> characters;
    std::map<std::uint64_t, ActiveBuff> active_buffs;
    std::vector<RecentHit> recent_hits;

    // 名称解析挂起队列与缓存
    std::vector<PendingNameQuery> pending_participants;
    std::unordered_map<std::uint64_t, std::string> participant_names;
    std::unordered_map<std::uint64_t, std::string> source_names;
    std::vector<PendingEventName> pending_event_names;
    std::unordered_map<std::uint64_t, std::string> combat_name_cache;

    Settings settings;
    // mutex 单独放最后并用 unique_ptr：Context 含 std::mutex 就无法赋值，
    // 而 on_load / on_unload 需要把整个上下文恢复成初始状态。
    std::unique_ptr<std::mutex> mutex{std::make_unique<std::mutex>()};

    void Reset() {
        host = nullptr;
        combat = {};
        session = {};
        json = {};
        config = {};
        started = false;
        window_open = 1;
        config_schema_registered = false;
        config_schema = {};
        settings_dirty = false;
        settings_loaded = false;
        last_window_width = 0.0f;
        last_window_height = 0.0f;
        window_size_observed = false;
        window_user_resized = false;
        session_time = 0.0;
        drain_update_counter = 0;
        clock_last_tick = 0;
        clock_last_tick_time = 0.0;
        clock_tick_rate = 0.0;
        clock_calibrated = false;
        last_drained_tick = 0;
        event_cursor = 0;
        cursor_primed = false;
        stream_partial = false;
        authority_valid = false;
        outgoing_stats = {};
        incoming_stats = {};
        combatant_valid = false;
        local_character = {};
        world = {};
        local_hp = 0.0;
        local_max_hp = 0.0;
        local_shield = 0.0;
        local_dead = false;
        local_attackers.clear();
        party_handles.clear();
        session_world_ready = false;
        session_event_cursor = 0;
        ResetBattle();
        battle_history.clear();
        total_hits = 0;
        total_crit = 0;
        total_crit_known = 0;
        total_head = 0;
        total_final = 0;
        total_max_hit = 0;
        taken_hits = 0;
        taken_final = 0;
        heal_out_hits = 0;
        heal_out_total = 0;
        heal_in_hits = 0;
        heal_in_total = 0;
        buff_applied_total = 0;
        buff_received_total = 0;
        buff_removed_total = 0;
        damage_event_count = 0;
        heal_event_count = 0;
        buff_event_count = 0;
        total_event_count = 0;
        battle_count = 0;
        sources.clear();
        characters.clear();
        active_buffs.clear();
        recent_hits.clear();
        pending_participants.clear();
        participant_names.clear();
        source_names.clear();
        pending_event_names.clear();
        combat_name_cache.clear();
        settings = Settings{};
    }

    // 只清「本场战斗」口径，不动全程与角色证据。
    void ResetBattleScope() {
        battle_active = false;
        battle_started_at = 0.0;
        battle_last_event_at = 0.0;
        battle_hits = 0;
        battle_crit = 0;
        battle_crit_known = 0;
        battle_head = 0;
        battle_out_final = 0;
        battle_max_hit = 0;
        battle_taken_hits = 0;
        battle_taken_final = 0;
        battle_heal_hits = 0;
        battle_heal_total = 0;
        for (auto& entry : sources) {
            entry.second.battle_hits = 0;
            entry.second.battle_final_total = 0;
        }
        for (auto& entry : characters) {
            auto& character = entry.second;
            character.battle_out_hits = 0;
            character.battle_out_final = 0;
            character.battle_out_max_hit = 0;
            character.battle_in_hits = 0;
            character.battle_in_final = 0;
            character.battle_heal_hits = 0;
            character.battle_heal_total = 0;
            for (auto& source_entry : character.sources) {
                source_entry.second.battle_hits = 0;
                source_entry.second.battle_final_total = 0;
            }
        }
    }

    void ResetBattle() { ResetBattleScope(); }
};

Context g;

// ---------------------------------------------------------------------------
// 名称解析
// ---------------------------------------------------------------------------
std::string TwoCallString(const std::function<AnomalyStatusV1(char*, std::size_t*)>& resolver) {
    std::size_t size{};
    if (!IsSizingStatus(resolver(nullptr, &size)) || size <= 1 ||
        size > kMaximumResolvedNameBytes) {
        return {};
    }
    std::string value(size, '\0');
    if (resolver(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value;
}

bool LooksLikeObjectPath(const std::string& value) {
    return value.rfind("/Game/", 0) == 0 || value.rfind("/Script/", 0) == 0;
}

// 解析参与者显示名。返回 {} 表示暂时解析失败（Runtime FName/FText 查询可能延后）。
std::string ResolveParticipantName(const AnomalyGenerationHandleV1 handle, bool& resolved) {
    resolved = handle.id == 0;
    if (handle.id == 0) return {};
    const auto cache_key = ParticipantCacheKey(handle);
    const auto cached = g.participant_names.find(cache_key);
    if (cached != g.participant_names.end()) {
        resolved = true;
        return cached->second;
    }
    const auto* service = g.combat.get();
    if (service == nullptr) return {};

    if (service->participant_display_name_utf8 != nullptr) {
        const auto display = TwoCallString([&](char* destination, std::size_t* size) {
            return service->participant_display_name_utf8(service->user, handle, destination, size);
        });
        if (!display.empty() && !LooksLikeObjectPath(display)) {
            g.participant_names.emplace(cache_key, display);
            resolved = true;
            return display;
        }
    }
    if (service->participant_path_utf8 != nullptr) {
        const auto path = TwoCallString([&](char* destination, std::size_t* size) {
            return service->participant_path_utf8(service->user, handle, destination, size);
        });
        if (!path.empty()) {
            // 路径只是临时标签，不入缓存；等真正的显示名可用时再替换。
            return path;
        }
    }
    return {};
}

std::string ResolveSourceName(const std::uint64_t world_id, const std::uint64_t source_id,
                              bool& resolved) {
    resolved = source_id == 0;
    if (source_id == 0) return {};
    const auto key = SourceCacheKey(world_id, source_id);
    const auto cached = g.source_names.find(key);
    if (cached != g.source_names.end()) {
        resolved = true;
        return cached->second;
    }
    const auto* service = g.combat.get();
    if (service == nullptr || service->source_name_utf8 == nullptr) return {};
    const auto name = TwoCallString([&](char* destination, std::size_t* size) {
        return service->source_name_utf8(service->user, source_id, destination, size);
    });
    if (name.empty() || LooksLikeObjectPath(name)) return {};
    g.source_names.emplace(key, name);
    resolved = true;
    return name;
}

// 解析统一战斗流事件的 name_id（技能/来源名）。宿主只用 event->name_id 查表，
// 所以只填 name_id 的 probe 在 ABI 层是合法的；解析不出说明宿主侧缓存还没填好。
std::string ResolveCombatEventName(const std::uint64_t name_id, bool& resolved) {
    resolved = name_id == 0;
    if (name_id == 0) return {};
    const auto cached = g.combat_name_cache.find(name_id);
    if (cached != g.combat_name_cache.end()) {
        resolved = true;
        return cached->second;
    }
    const auto* service = g.combat.get();
    if (service == nullptr || service->event_name_utf8 == nullptr) return {};
    AnomalyNteCombatEventV1 probe{};
    probe.struct_size = sizeof(probe);
    probe.name_id = name_id;
    const auto name = TwoCallString([&](char* destination, std::size_t* size) {
        return service->event_name_utf8(service->user, &probe, destination, size);
    });
    if (!name.empty() && !LooksLikeObjectPath(name)) {
        g.combat_name_cache.emplace(name_id, name);
        resolved = true;
        return name;
    }
    return {};
}

// 名字一旦解析出来，就立刻回填到所有还在等名字的聚合上，并按名字合并。
// 宿主对同一个 name_id 的解析是异步的（每 tick 只解一个、失败要隔几十 tick
// 重试），如果只在下一次遇到同一 name_id 的事件时才回填，那些已经打完、
// 不再出现的事件就永远停在「技能#<数字>」上——这就是乱码行的来源。
//
// 同一个技能在宿主里会有多个 name_id（GE 对象 id / 浮字 InjurySourceName /
// 变体），它们解析出来是同一个名字。名字到手后必须把各自独立的聚合行
// 累加进「按名字」的那一行，否则同一技能会被拆成好几行。
void BackfillSkillNames() {
    if (g.combat_name_cache.empty()) return;
    for (auto& character_entry : g.characters) {
        auto& sources = character_entry.second.sources;
        bool changed = false;
        for (auto& source_entry : sources) {
            auto& source = source_entry.second;
            if (source.name_resolved) continue;
            const auto cached = g.combat_name_cache.find(source.name_id);
            if (cached == g.combat_name_cache.end()) continue;
            source.name = cached->second;
            source.name_resolved = true;
            changed = true;
        }
        if (!changed) continue;

        // 按名字归并：把这批刚解析出名字的行并到第一个同名行上。
        // key = 解析后的名字；没有名字的行保持各自独立（它们本来就不显示）。
        std::unordered_map<std::string, std::uint64_t> canonical;
        std::vector<std::uint64_t> drop;
        for (auto& source_entry : sources) {
            auto& source = source_entry.second;
            if (!source.name_resolved || source.name.empty()) continue;
            const auto inserted = canonical.emplace(source.name, source_entry.first);
            if (inserted.second) continue;  // 第一个同名的保留为归并目标
            auto& target = sources[inserted.first->second];
            target.hits += source.hits;
            target.crit_hits += source.crit_hits;
            target.crit_known += source.crit_known;
            target.head_hits += source.head_hits;
            target.final_total += source.final_total;
            target.max_hit = std::max(target.max_hit, source.max_hit);
            target.last_sequence = std::max(target.last_sequence, source.last_sequence);
            target.battle_hits += source.battle_hits;
            target.battle_final_total += source.battle_final_total;
            drop.push_back(source_entry.first);
        }
        for (const auto key : drop) sources.erase(key);
    }
}

void QueueParticipantName(const AnomalyGenerationHandleV1 handle) {
    if (handle.id == 0) return;
    if (g.participant_names.count(ParticipantCacheKey(handle)) != 0) return;
    for (const auto& pending : g.pending_participants) {
        if (SameHandle(pending.handle, handle)) return;
    }
    if (g.pending_participants.size() >= kMaximumPendingNames) return;
    g.pending_participants.push_back({handle, 0, 0});
}

void QueueEventName(const std::uint64_t name_id) {
    if (name_id == 0) return;
    if (g.combat_name_cache.count(name_id) != 0) return;
    for (const auto& pending : g.pending_event_names) {
        if (pending.name_id == name_id) return;
    }
    if (g.pending_event_names.size() >= kMaximumPendingNames) return;
    g.pending_event_names.push_back({name_id, 0, 0});
}

// 每帧有预算地推进挂起的名称查询，避免在 Game 线程上做无界反射查找。
void ServicePendingNames() {
    const auto* service = g.combat.get();
    if (service == nullptr) return;
    std::size_t budget = kMaximumNameQueriesPerUpdate;

    for (std::size_t index = 0; index < g.pending_participants.size() && budget > 0;) {
        auto& pending = g.pending_participants[index];
        if (pending.retry_at_update > g.drain_update_counter) {
            ++index;
            continue;
        }
        bool resolved = false;
        // 用聚合里保存的**最新**句柄去查：挂起队列里的句柄可能是旧的 generation，
        // 而宿主的对象查询会因为 generation 不匹配返回 not-found，
        // 导致名字永远解析不出来、角色显示成「?」。
        const auto latest = g.characters.find(ParticipantCacheKey(pending.handle));
        const AnomalyGenerationHandleV1 query_handle =
            latest != g.characters.end() ? latest->second.handle : pending.handle;
        (void)ResolveParticipantName(query_handle, resolved);
        --budget;
        if (resolved) {
            g.pending_participants.erase(g.pending_participants.begin() +
                                         static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++pending.attempts;
        if (pending.attempts >= kMaximumNameRetries) {
            g.pending_participants.erase(g.pending_participants.begin() +
                                         static_cast<std::ptrdiff_t>(index));
            continue;
        }
        pending.retry_at_update = g.drain_update_counter + kNameRetryDelayUpdates;
        ++index;
    }

    // 事件名（技能名）延后可用：失败就按间隔重试，不丢队列。
    for (std::size_t index = 0; index < g.pending_event_names.size() && budget > 0;) {
        auto& pending = g.pending_event_names[index];
        if (pending.retry_at_update > g.drain_update_counter) {
            ++index;
            continue;
        }
        bool resolved = false;
        (void)ResolveCombatEventName(pending.name_id, resolved);
        --budget;
        if (resolved) {
            g.pending_event_names.erase(g.pending_event_names.begin() +
                                        static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++pending.attempts;
        if (pending.attempts >= kMaximumNameRetries) {
            g.pending_event_names.erase(g.pending_event_names.begin() +
                                        static_cast<std::ptrdiff_t>(index));
            continue;
        }
        pending.retry_at_update = g.drain_update_counter + kNameRetryDelayUpdates;
        ++index;
    }
    // 名字解析成功后立刻回填到已经统计过的技能行，避免它们永远停在
    // 「技能#<数字>」占位名上。
    BackfillSkillNames();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
constexpr const char* kConfigSchemaId = "damage-dashboard.settings";
// 插件身份。必须与 manifest.json 的 id / author / version 完全一致；
// 所有需要 ID 的地方都从这里取值，避免多处字面量漂移。
constexpr const char* kPluginId = "anomaly.local.damage-dashboard";
constexpr const char* kPluginAuthor = "Haaland-a";
// 版本号在日志里输出，升版本时这里和 manifest.json 一起改。
constexpr const char* kPluginVersion = "2.7.0";
// v4：删掉 windowFitContent（高度改由内容决定），所以升版本让旧配置走迁移。
constexpr std::uint32_t kConfigSchemaVersion = 4;

const char* kConfigSchemaJson = R"JSON({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "battleIdleSeconds": {"type": "number", "minimum": 3, "maximum": 30},
    "showSkills": {"type": "boolean"},
    "showBuffs": {"type": "boolean"},
    "showRecent": {"type": "boolean"},
    "maxRows": {"type": "integer", "minimum": 4, "maximum": 24},
    "windowWidth": {"type": "number", "minimum": 0, "maximum": 4000},
    "windowHeight": {"type": "number", "minimum": 0, "maximum": 4000}
  }
})JSON";

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04X",
                              static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buffer;
            } else {
                out += ch;
            }
        }
    }
    return out;
}

std::string SerializeSettings() {
    char number[48]{};
    std::string out;
    out.reserve(256);
    std::snprintf(number, sizeof(number), "{\"battleIdleSeconds\":%.1f",
                  g.settings.battle_idle_seconds);
    out += number;
    out += ",\"showSkills\":";
    out += g.settings.show_skills ? "true" : "false";
    out += ",\"showBuffs\":";
    out += g.settings.show_buffs ? "true" : "false";
    out += ",\"showRecent\":";
    out += g.settings.show_recent ? "true" : "false";
    std::snprintf(number, sizeof(number), ",\"maxRows\":%u", g.settings.max_rows);
    out += number;
    std::snprintf(number, sizeof(number), ",\"windowWidth\":%.0f", g.settings.window_width);
    out += number;
    std::snprintf(number, sizeof(number), ",\"windowHeight\":%.0f", g.settings.window_height);
    out += number;
    out += '}';
    return out;
}

bool ParseSettings(const std::string& document) {
    const auto* service = g.json.get();
    if (service == nullptr) return false;
    AnomalyGenerationHandleV1 root{};
    if (service->parse(service->user, {document.data(), document.size()}, &root).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    const auto release = [&](AnomalyGenerationHandleV1 handle) {
        if (handle.id != 0) service->release(service->user, handle);
    };
    const auto find = [&](AnomalyGenerationHandleV1 parent, const char* key,
                          AnomalyGenerationHandleV1* child) -> bool {
        const auto status = service->object_find(service->user, parent,
                                                 {key, std::strlen(key)}, child);
        return status.code == ANOMALY_STATUS_V1_OK;
    };
    const auto read_bool = [&](const char* key, bool& target) {
        AnomalyGenerationHandleV1 node{};
        if (!find(root, key, &node)) return;
        int32_t value{};
        if (service->boolean_value(service->user, node, &value).code == ANOMALY_STATUS_V1_OK) {
            target = value != 0;
        }
        release(node);
    };
    const auto read_number = [&](const char* key, double& target, double low, double high) {
        AnomalyGenerationHandleV1 node{};
        if (!find(root, key, &node)) return;
        double value{};
        if (service->number_value(service->user, node, &value).code == ANOMALY_STATUS_V1_OK) {
            target = std::clamp(value, low, high);
        }
        release(node);
    };
    const auto read_int = [&](const char* key, std::uint32_t& target, std::uint32_t low,
                              std::uint32_t high) {
        AnomalyGenerationHandleV1 node{};
        if (!find(root, key, &node)) return;
        double value{};
        if (service->number_value(service->user, node, &value).code == ANOMALY_STATUS_V1_OK) {
            if (value < static_cast<double>(low)) value = static_cast<double>(low);
            if (value > static_cast<double>(high)) value = static_cast<double>(high);
            target = static_cast<std::uint32_t>(value + 0.5);
        }
        release(node);
    };

    read_number("battleIdleSeconds", g.settings.battle_idle_seconds, 3.0, 30.0);
    read_bool("showSkills", g.settings.show_skills);
    read_bool("showBuffs", g.settings.show_buffs);
    read_bool("showRecent", g.settings.show_recent);
    read_int("maxRows", g.settings.max_rows, 4u, 24u);
    read_number("windowWidth", g.settings.window_width, 0.0, 4000.0);
    read_number("windowHeight", g.settings.window_height, 0.0, 4000.0);
    // 小于最小可用尺寸的值当没设过，免得老配置把窗口锁成一个挤不下内容的小窗。
    if (g.settings.window_width < static_cast<double>(kDashboardMinimumWidth)) {
        g.settings.window_width = 0.0;
    }
    if (g.settings.window_height < static_cast<double>(kDashboardMinimumHeight)) {
        g.settings.window_height = 0.0;
    }
    release(root);
    return true;
}

void SaveSettings() {
    const auto* service = g.config.get();
    if (service == nullptr || !g.config_schema_registered) return;
    const auto document = SerializeSettings();
    service->write_atomic(service->user, {kConfigSchemaId, std::strlen(kConfigSchemaId)},
                          kConfigSchemaVersion, {reinterpret_cast<const std::uint8_t*>(document.data()),
                                                 document.size()});
    g.settings_dirty = false;
}

// ---------------------------------------------------------------------------
// 战斗会话
// ---------------------------------------------------------------------------
void PushBattleHistory() {
    BattleRecord record;
    record.valid = true;
    record.started_at = g.battle_started_at;
    record.ended_at = g.battle_last_event_at;
    record.hits = g.battle_hits;
    record.crit = g.battle_crit;
    record.crit_known = g.battle_crit_known;
    record.head = g.battle_head;
    record.out_final = g.battle_out_final;
    record.max_hit = g.battle_max_hit;
    record.taken_hits = g.battle_taken_hits;
    record.taken_final = g.battle_taken_final;
    record.heal_hits = g.battle_heal_hits;
    record.heal_total = g.battle_heal_total;
    // 找出本场伤害最高的角色，作为「上一场」的摘要。
    std::int64_t best = 0;
    for (const auto& entry : g.characters) {
        const auto& character = entry.second;
        if (character.battle_out_final <= best) continue;
        best = character.battle_out_final;
        record.top_character = character.name_resolved && !character.name.empty()
                                   ? character.name
                                   : (character.is_local ? std::string("我")
                                                        : std::string("未识别角色"));
        record.top_character_damage = best;
    }
    g.battle_history.insert(g.battle_history.begin(), record);
    if (g.battle_history.size() > kMaximumBattleHistory) {
        g.battle_history.resize(kMaximumBattleHistory);
    }
    ++g.battle_count;
}

// 战斗开始：清零「本场战斗」口径。
void BeginBattle(const double now) {
    g.ResetBattleScope();
    g.battle_active = true;
    g.battle_started_at = now;
    g.battle_last_event_at = now;
}

// 战斗结束：存档并清零。
void EndBattle() {
    if (!g.battle_active) return;
    PushBattleHistory();
    g.ResetBattleScope();
}

// 每 tick 检查战斗是否结束（闲置超过设定秒数）。
void UpdateBattleState(const double now) {
    if (!g.battle_active) return;
    const double idle = g.settings.battle_idle_seconds;
    if (idle <= 0.0) return;
    if (now - g.battle_last_event_at >= idle) EndBattle();
}

// ---------------------------------------------------------------------------
// 会话状态感知（用户需求 5：进入/脱离战斗要能感知到）
// ---------------------------------------------------------------------------
// 宿主 session 服务给出世界状态（LOADING / WORLD_READY）与世界切换事件，
// 用它识别「刚进入战斗场景」和「脱离战斗场景」，比单纯靠闲置秒数更准确。
void UpdateSessionState() {
    auto* service = g.session.get();
    if (service == nullptr) return;

    if (service->snapshot != nullptr) {
        AnomalyNteSessionSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        const auto status = service->snapshot(service->user, &snapshot);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            const bool world_ready = snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY;
            // 世界从未就绪 → 就绪：算「进入战斗场景」。
            if (world_ready && !g.session_world_ready) {
                g.session_world_ready = true;
                // 进图时把游标对齐到当前水位，避免回放加载期间的历史。
                if (g.combat && g.combat->latest_event_sequence != nullptr) {
                    g.event_cursor = g.combat->latest_event_sequence(g.combat->user);
                    g.cursor_primed = true;
                }
            } else if (!world_ready && g.session_world_ready) {
                // 世界就绪 → 不可用：脱离战斗场景，把当前这场结算掉。
                g.session_world_ready = false;
                if (g.battle_active) EndBattle();
            }
        }
    }

    // 世界切换事件：换图/换关卡时上一场战斗一定已经结束。
    if (service->next_event != nullptr && service->latest_event_sequence != nullptr) {
        const std::uint64_t latest = service->latest_event_sequence(service->user);
        while (g.session_event_cursor < latest) {
            AnomalyNteSessionEventV1 event{};
            event.struct_size = sizeof(event);
            const auto status =
                service->next_event(service->user, g.session_event_cursor, &event);
            if (status.code != ANOMALY_STATUS_V1_OK) break;
            g.session_event_cursor = event.sequence;
            if (event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED ||
                event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE) {
                if (g.battle_active) EndBattle();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 权威统计（宿主 statistics()）
// ---------------------------------------------------------------------------
void UpdateAuthority(const AnomalyGenerationHandleV1 character,
                     const AnomalyGenerationHandleV1 world) {
    const auto* service = g.combat.get();
    if (service == nullptr || service->statistics == nullptr) {
        g.authority_valid = false;
        return;
    }
    if (character.id == 0 || world.id == 0) {
        g.authority_valid = false;
        return;
    }
    const auto query = [&](const std::uint32_t direction, AnomalyNteCombatStatisticsV1& target) {
        AnomalyNteCombatStatisticsRequestV1 request{};
        request.struct_size = sizeof(request);
        request.world = world;
        request.character = character;
        request.source_id = 0;
        request.direction = direction;
        target.struct_size = sizeof(target);
        const auto status = service->statistics(service->user, &request, &target);
        return status.code == ANOMALY_STATUS_V1_OK;
    };
    const bool ok_out = query(ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_ATTACKER, g.outgoing_stats);
    const bool ok_in = query(ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_VICTIM, g.incoming_stats);
    g.authority_valid = ok_out || ok_in;
}

// ---------------------------------------------------------------------------
// 事件入口
// ---------------------------------------------------------------------------
void OnCombatEvent(const AnomalyNteCombatEventV1& event, const double now_seconds) {
    ++g.total_event_count;

    std::string event_name;
    if (event.name_id != 0) {
        const auto cached = g.combat_name_cache.find(event.name_id);
        if (cached != g.combat_name_cache.end()) {
            event_name = cached->second;
        } else {
            // 先立刻试一次同步解析：宿主的名字解析是「首次请求 + 后续 tick
            // 异步填充」，早一帧请求就能早一帧拿到名字。
            bool resolved = false;
            event_name = ResolveCombatEventName(event.name_id, resolved);
            if (!resolved) QueueEventName(event.name_id);
        }
    }

    const bool source_is_mine = g.IsMine(event.source);
    const bool target_is_mine = g.IsMine(event.target);

    const auto touch_character = [&](const AnomalyGenerationHandleV1 handle) -> CharacterAggregate& {
        auto& aggregate = g.characters[ParticipantCacheKey(handle)];
        aggregate.handle = handle;
        aggregate.is_local = g.IsMine(handle);
        if (!aggregate.name_resolved && aggregate.name.empty()) QueueParticipantName(handle);
        return aggregate;
    };
    const auto sync_name = [&](CharacterAggregate& aggregate) {
        if (aggregate.name_resolved) return;
        const auto cached = g.participant_names.find(ParticipantCacheKey(aggregate.handle));
        if (cached != g.participant_names.end()) {
            aggregate.name = cached->second;
            aggregate.name_resolved = true;
        }
    };

    switch (event.kind) {
        case ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE: {
            const bool critical = (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL) != 0;
            const bool crit_known = (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID) != 0;
            const bool head_hit = (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT) != 0;
            const bool weak = (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE) != 0;
            const std::int64_t damage = event.final_value;
            ++g.damage_event_count;

            // 战斗开始/继续：任何伤害都算战斗活动。
            if (!g.battle_active) {
                BeginBattle(now_seconds);
            }
            g.battle_last_event_at = now_seconds;

            // ---- 按技能/来源（只记在该攻击者自己的角色表下）----
            // 只用 event.source 归属，绝不写全局 g.sources：宿主的 name_id 是
            // DamageEvent.DamageGEDef 的 source 对象 id（kDamageSourceBase 基址），
            // 同一个 GE 定义对象会被不同角色复用，写全局表会把敌人打我的伤害
            // 混进我方角色的技能统计。
            if (event.source.id != 0) {
                const std::uint64_t source_key = SourceCacheKey(event.world.id, event.name_id);
                auto& aggregate = touch_character(event.source).sources[source_key];
                aggregate.name_id = event.name_id;
                aggregate.world_id = event.world.id;
                aggregate.hits++;
                if (critical) aggregate.crit_hits++;
                if (crit_known) aggregate.crit_known++;
                if (head_hit) aggregate.head_hits++;
                aggregate.final_total += damage;
                aggregate.max_hit = std::max(aggregate.max_hit, damage);
                aggregate.last_sequence = event.sequence;
                aggregate.battle_hits++;
                aggregate.battle_final_total += damage;
                if (!aggregate.name_resolved && event.name_id != 0) {
                    if (const auto cached = g.combat_name_cache.find(event.name_id);
                        cached != g.combat_name_cache.end()) {
                        aggregate.name = cached->second;
                        aggregate.name_resolved = true;
                    }
                }
            }

            // ---- 按角色 ----
            if (event.source.id != 0) {
                auto& source = touch_character(event.source);
                PushBoundedHandle(source.attacked, event.target);
                if (target_is_mine) source.damaged_local = true;
                source.out_hits++;
                if (critical) source.out_crit++;
                if (crit_known) source.out_crit_known++;
                if (head_hit) source.out_head++;
                source.out_final += damage;
                source.out_max_hit = std::max(source.out_max_hit, damage);
                source.battle_out_hits++;
                source.battle_out_final += damage;
                source.battle_out_max_hit = std::max(source.battle_out_max_hit, damage);
                source.last_sequence = event.sequence;
                sync_name(source);
            }
            if (event.target.id != 0) {
                auto& target = touch_character(event.target);
                PushBoundedHandle(target.attacked_by, event.source);
                if (source_is_mine) target.damaged_by_local = true;
                target.in_hits++;
                target.in_final += damage;
                target.battle_in_hits++;
                target.battle_in_final += damage;
                target.last_sequence = event.sequence;
                sync_name(target);
            }
            if (target_is_mine && event.source.id != 0) {
                PushBoundedHandle(g.local_attackers, event.source);
            }

            // ---- 本场战斗汇总 ----
            // 口径：只统计「我操控的角色」造成的伤害与承受的伤害。
            // 敌人之间互相打、或怪物打NPC的伤害一律不计入本场总伤害/最高单次。
            if (source_is_mine) {
                g.battle_hits++;
                if (critical) g.battle_crit++;
                if (crit_known) g.battle_crit_known++;
                if (head_hit) g.battle_head++;
                g.battle_out_final += damage;
                g.battle_max_hit = std::max(g.battle_max_hit, damage);
            }
            if (target_is_mine) {
                g.battle_taken_hits++;
                g.battle_taken_final += damage;
            }

            // ---- 全程汇总（同样只算我操控的角色）----
            if (source_is_mine) {
                g.total_hits++;
                if (critical) g.total_crit++;
                if (crit_known) g.total_crit_known++;
                if (head_hit) g.total_head++;
                g.total_final += damage;
                g.total_max_hit = std::max(g.total_max_hit, damage);
            }
            if (target_is_mine) {
                g.taken_hits++;
                g.taken_final += damage;
            }
            (void)weak;
            break;
        }

        case ANOMALY_NTE_COMBAT_EVENT_V1_HEAL: {
            const std::int64_t healed = event.final_value;
            ++g.heal_event_count;
            if (!g.battle_active) BeginBattle(now_seconds);
            g.battle_last_event_at = now_seconds;

            if (event.source.id != 0) {
                auto& source = touch_character(event.source);
                source.healed_anyone = true;
                source.heal_hits++;
                source.heal_total += healed;
                source.battle_heal_hits++;
                source.battle_heal_total += healed;
                source.last_sequence = event.sequence;
                sync_name(source);
            }
            if (event.target.id != 0) {
                auto& target = touch_character(event.target);
                if (source_is_mine) target.healed_by_local = true;
                target.heal_received_hits++;
                target.heal_received_total += healed;
                target.last_sequence = event.sequence;
                sync_name(target);
            }
            if (source_is_mine) {
                g.heal_out_hits++;
                g.heal_out_total += healed;
                g.battle_heal_hits++;
                g.battle_heal_total += healed;
            }
            if (target_is_mine) {
                g.heal_in_hits++;
                g.heal_in_total += healed;
            }
            break;
        }

        case ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD:
        case ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE: {
            const bool added = event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD;
            ++g.buff_event_count;

            if (event.source.id != 0) {
                auto& source = touch_character(event.source);
                if (added) source.buff_applied++; else source.buff_removed++;
                source.last_sequence = event.sequence;
                sync_name(source);
            }
            if (event.target.id != 0) {
                auto& target = touch_character(event.target);
                if (added && source_is_mine) target.buffed_by_local = true;
                if (added) target.buff_received++;
                target.last_sequence = event.sequence;
                sync_name(target);
            }
            if (added) {
                if (source_is_mine) g.buff_applied_total++;
                if (target_is_mine) g.buff_received_total++;
            } else if (source_is_mine) {
                g.buff_removed_total++;
            }

            // ---- 增益倒计时跟踪 ----
            if (added && event.target.id != 0 && g.active_buffs.size() < kMaximumBuffTracked) {
                ActiveBuff buff;
                buff.name_id = event.name_id;
                buff.target = event.target;
                buff.source = event.source;
                buff.stacks = event.stack_count;
                buff.duration_seconds = event.duration_seconds;
                buff.applied_at = now_seconds;
                if (event.name_id != 0) {
                    const auto cached = g.combat_name_cache.find(event.name_id);
                    if (cached != g.combat_name_cache.end()) {
                        buff.name = cached->second;
                        buff.name_resolved = true;
                    } else {
                        QueueEventName(event.name_id);
                    }
                }
                const auto key = BuffCacheKey(event.name_id, event.target);
                const auto existing = g.active_buffs.find(key);
                if (existing != g.active_buffs.end()) {
                    // 同一个 buff 再次施加 = 刷新。已经存在说明它是持续型
                    // （战斗前就挂着 / 反复刷新），标为固定增益。
                    buff.persistent = true;
                    // 刷新时保留已有名字解析结果，避免闪回「增益#N」。
                    if (!buff.name_resolved && existing->second.name_resolved) {
                        buff.name = existing->second.name;
                        buff.name_resolved = true;
                    }
                    if (!buff.target_resolved && existing->second.target_resolved) {
                        buff.target_name = existing->second.target_name;
                        buff.target_resolved = true;
                    }
                } else if (!g.battle_active) {
                    // 战斗尚未开始就挂上的 buff = 战斗前就有的固定增益。
                    buff.persistent = true;
                }
                auto& slot = g.active_buffs[key];
                slot = buff;
            } else if (!added && event.target.id != 0) {
                g.active_buffs.erase(BuffCacheKey(event.name_id, event.target));
            }
            break;
        }

        default:
            break;
    }

    // ---- 最近命中 ----
    // 只记录伤害/治疗，增益事件不进入「最近命中」列表。
    if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD ||
        event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE) {
        return;
    }
    RecentHit hit;
    hit.time = now_seconds;
    hit.kind = event.kind;
    hit.value = event.final_value;
    hit.critical = (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL) != 0;
    hit.head_hit = (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT) != 0;
    hit.weak = (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE) != 0;
    hit.incoming = target_is_mine;
    hit.outgoing = source_is_mine;
    if (event.source.id == 0) {
        hit.source = "环境";
    } else {
        const auto found = g.characters.find(ParticipantCacheKey(event.source));
        hit.source = found != g.characters.end() && !found->second.name.empty()
                         ? found->second.name
                         : std::string("未知");
    }
    if (event.target.id == 0) {
        hit.target = "环境";
    } else {
        const auto found = g.characters.find(ParticipantCacheKey(event.target));
        hit.target = found != g.characters.end() && !found->second.name.empty()
                         ? found->second.name
                         : std::string("未知");
    }
    hit.name = event_name;
    g.recent_hits.push_back(std::move(hit));
    if (g.recent_hits.size() > kMaximumRecentHits) {
        g.recent_hits.erase(g.recent_hits.begin());
    }
}

// 时钟校准：用宿主 combat event 的 tick_sequence（权威帧序号）测出每 tick 的
// 真实秒数，再把插件时钟拉回真实时间（定义在 DrainDamage 之后）。
void CalibrateClock(const std::uint64_t tick, const double now);

void DrainDamage() {
    auto* service = g.combat.get();
    if (service == nullptr || service->next_event == nullptr ||
        service->latest_event_sequence == nullptr) {
        return;
    }
    const auto latest = service->latest_event_sequence(service->user);
    if (latest == g.event_cursor) return;

    std::size_t drained = 0;
    bool rebased = false;
    while (drained < kMaximumDrainPerUpdate) {
        AnomalyNteCombatEventV1 event{};
        event.struct_size = sizeof(event);
        const auto status = service->next_event(service->user, g.event_cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            if (!rebased && status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                g.event_cursor != 0 && latest > g.event_cursor) {
                // 生产者 ring 把游标甩掉了，从 0 重新问 ABI 要最旧保留事件。
                g.event_cursor = 0;
                rebased = true;
                g.stream_partial = true;
                continue;
            }
            break;
        }
        if (event.sequence <= g.event_cursor) break;   // 防御：游标必须单调前进
        g.event_cursor = event.sequence;
        g.last_drained_tick = event.tick_sequence;
        CalibrateClock(event.tick_sequence, g.session_time);
        OnCombatEvent(event, g.session_time);
        ++drained;
    }
}

// 时钟校准：宿主 combat event 的 tick_sequence 是权威帧序号。每拿到一个带
// tick_sequence 的事件，就记录 (tick, session_time) 对应点；两次采样的间隔
// 越大，估算出的「每 tick 秒数」越准。用这个比率把插件时钟往真实时间上拉，
// 否则 delta 累加的累计误差会让增益倒计时越走越偏。
void CalibrateClock(const std::uint64_t tick, const double now) {
    if (tick == 0) return;
    if (g.clock_calibrated && tick > g.clock_last_tick) {
        const auto tick_delta = tick - g.clock_last_tick;
        const double time_delta = now - g.clock_last_tick_time;
        if (tick_delta > 0 && time_delta > 1e-6) {
            const double measured = time_delta / static_cast<double>(tick_delta);
            // 只接受合理范围内的帧间隔（1μs ~ 1s），剔除卡顿造成的野值。
            if (measured > 1e-6 && measured < 1.0) {
                if (g.clock_tick_rate > 0.0) {
                    // 指数平滑，避免单次抖动把校准值打飞。
                    g.clock_tick_rate = g.clock_tick_rate * 0.9 + measured * 0.1;
                } else {
                    g.clock_tick_rate = measured;
                }
            }
        }
    }
    g.clock_last_tick = tick;
    g.clock_last_tick_time = now;
    g.clock_calibrated = true;
}

// 过期的增益（没有 BUFF_REMOVE）自动退出倒计时列表。
// 固定增益（persistent）不参与过期淘汰，只由 BUFF_REMOVE 移除。
void ExpireBuffs(const double now) {
    for (auto it = g.active_buffs.begin(); it != g.active_buffs.end();) {
        const auto& buff = it->second;
        const bool expired = !buff.persistent && buff.duration_seconds > 0.0f &&
                             now - buff.applied_at >= static_cast<double>(buff.duration_seconds);
        it = expired ? g.active_buffs.erase(it) : std::next(it);
    }
}

// 延迟解析到的名字回填到增益表。
void RefreshBuffNames() {
    for (auto& entry : g.active_buffs) {
        auto& buff = entry.second;
        if (!buff.name_resolved && buff.name_id != 0) {
            const auto cached = g.combat_name_cache.find(buff.name_id);
            if (cached != g.combat_name_cache.end()) {
                buff.name = cached->second;
                buff.name_resolved = true;
            }
        }
        if (!buff.target_resolved) {
            const auto cached = g.participant_names.find(ParticipantCacheKey(buff.target));
            if (cached != g.participant_names.end()) {
                buff.target_name = cached->second;
                buff.target_resolved = true;
            } else {
                buff.target_name = RoleLabel(kRoleUnknown);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// on_update：Game 线程，唯一的数据变更点
// ---------------------------------------------------------------------------

// 换角色/换世界时清掉全部本地统计（定义在文末，这里先声明）。
void ResetStatisticsAll();

// 宿主在插件 on_load 时不会把游戏世界相关的 service 表交出来（profile 未校验 /
// ---------------------------------------------------------------------------
// 诊断日志：写进 Anomaly「诊断 → 日志」页
// ---------------------------------------------------------------------------
// 之前这个插件完全静默：出问题时平台日志里搜不到任何本插件相关记录，
// 用户只能看到界面行为异常却无从排查。这里接上 anomaly.core 的 log()。
void LogMessage(const std::uint32_t level, const char* message) {
    if (!g.core || g.core->log == nullptr || message == nullptr) return;
    g.core->log(g.core->user, level, {message, std::strlen(message)});
}

void LogInfo(const char* message) {
    LogMessage(ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
}

void LogWarning(const char* message) {
    LogMessage(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, message);
}

void LogError(const char* message) {
    LogMessage(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, message);
}

void LogInfoF(const char* format, ...) {
    char buffer[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogInfo(buffer);
}

void LogWarningF(const char* format, ...) {
    char buffer[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogWarning(buffer);
}

void LogErrorF(const char* format, ...) {
    char buffer[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogError(buffer);
}

// 游戏内可见提示。用户「看不见诊断信息」就是因为插件从来不发通知，
// 也不写日志——所有状态只闷在窗口里。
void Notify(const AnomalyNotificationSeverityV1 severity, const char* title,
            const char* body) {
    if (!g.notifications || g.notifications->post == nullptr) return;
    AnomalyGenerationHandleV1 handle{};
    g.notifications->post(g.notifications->user, severity,
                          {title, std::strlen(title)}, {body, std::strlen(body)}, 6000,
                          &handle);
}

// 未进世界时的常态），只有真正进入战斗场景后 query_service 才返回有效指针。
// 所以每次 on_update 都重新查一遍，和官方 NteCombatDemo 的做法一致。
const AnomalyNteCombatServiceV1* RefreshServices() {
    if (g.host == nullptr) return nullptr;
    if (!g.combat) {
        g.combat = anomaly::sdk::Host(g.host).Query<AnomalyNteCombatServiceV1>(
            ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);
    }
    if (!g.session) {
        g.session = anomaly::sdk::Host(g.host).Query<AnomalyNteSessionServiceV1>(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    }
    if (!g.json) {
        g.json = anomaly::sdk::Host(g.host).Query<AnomalyJsonServiceV1>(
            ANOMALY_JSON_SERVICE_V1_ID, ANOMALY_JSON_SERVICE_V1_VERSION);
    }
    if (!g.config) {
        g.config = anomaly::sdk::Host(g.host).Query<AnomalyConfigServiceV1>(
            ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    }
    // core / diagnostics 是常驻服务，一次拿到就不再重查。
    if (!g.core) {
        g.core = anomaly::sdk::Host(g.host).Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    }
    if (!g.diagnostics) {
        g.diagnostics = anomaly::sdk::Host(g.host).Query<AnomalyDiagnosticsServiceV1>(
            ANOMALY_DIAGNOSTICS_SERVICE_V1_ID, ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION);
    }
    if (!g.notifications) {
        g.notifications = anomaly::sdk::Host(g.host).Query<AnomalyNotificationsServiceV1>(
            ANOMALY_NOTIFICATIONS_SERVICE_V1_ID, ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION);
    }
    return g.combat.get();
}

// 换角色/换世界：不丢统计，只把「当前本地角色」重新指向新角色。
// 队伍里四名角色并非同时在场，同一个战斗周期内的伤害必须跨角色累加，
// 所以这里只切换 is_local 归属，绝不清空任何聚合数据。
void UpdateCombatant() {
    auto* service = g.combat.get();
    if (service == nullptr || service->current_combatant == nullptr) {
        // 战斗服务不可用：只在「刚变成不可用」时提醒一次，避免每帧刷屏。
        if (g.combat_service_ready) {
            g.combat_service_ready = false;
            LogWarning("nte.combat 服务丢失，统计暂停（通常表示已离开战斗场景）");
            Notify(ANOMALY_NOTIFICATION_V1_WARNING, "伤害仪表盘",
                   "战斗服务不可用，统计暂停。进入关卡后会自动恢复。");
        }
        return;
    }
    if (!g.combat_service_ready) {
        g.combat_service_ready = true;
        LogInfo("nte.combat 服务已就绪，开始采集战斗事件");
    }

    AnomalyNteCombatantSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const auto status = service->current_combatant(service->user, &snapshot);
    const bool valid = status.code == ANOMALY_STATUS_V1_OK &&
                       (snapshot.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0;
    if (!valid) {
        g.combatant_valid = false;
        if (g.combatant_invalid_reported) return;
        g.combatant_invalid_reported = true;
        LogWarningF("current_combatant 返回无效（status=%u），可能尚未进入关卡",
                    static_cast<unsigned>(status.code));
        return;
    }
    g.combatant_invalid_reported = false;

    const bool character_changed = !SameHandle(snapshot.character, g.local_character);
    // 世界切换不重置统计：宿主的 ring 在世界切换时会被 ResetDamageEvents 清空并
    // 重新计数，游标 rebase 逻辑会处理；累计口径保持连续，符合「同一战斗周期
    // 内四名角色伤害累加」的需求。
    (void)sizeof(snapshot.world);

    g.combatant_valid = true;
    g.local_hp = snapshot.hp;
    g.local_max_hp = snapshot.max_hp;
    g.local_shield = snapshot.shield;
    g.local_dead = (snapshot.flags & ANOMALY_NTE_COMBATANT_V1_DEAD) != 0;

    // current_combatant 返回的永远是我当前操控的角色。四名角色轮换出场，
    // 所以每看到一个新的 local_character 就登记进 party_handles —— 之后这些
    // 角色都算「我操控的」，他们的伤害全部累加进同一份统计。
    g.RememberPartyHandle(snapshot.character);

    // 角色切换只重定向「我」，统计口径（本场 + 全程）全部保留。
    // 注意：local_character 会同时作为 touched 聚合并的 is_local 标记来源，
    // 所以需要在写入前把旧的「我」降级、新的「我」升级。
    if (character_changed) {
        for (auto& entry : g.characters) {
            entry.second.is_local = g.IsMine(entry.second.handle);
        }
    }
    g.local_character = snapshot.character;
    g.world = snapshot.world;

    UpdateAuthority(snapshot.character, snapshot.world);
}

void OnUpdate(void* plugin_context, const double delta_seconds) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || !context->started) return;
    std::scoped_lock lock(*g.mutex);

    ++g.drain_update_counter;
    // 单帧 delta 钳制，避免切窗口/暂停造成时间跳变把战斗误判为结束。
    const double delta = std::clamp(delta_seconds, 0.0, kMaximumFrameDelta);
    g.session_time += delta;

    // on_load 时 core 可能还没就绪，这里补查一次；拿到后补写装载日志。
    if (g.core_deferred && g.host != nullptr && !g.core) {
        g.core = anomaly::sdk::Host(g.host).Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    }
    if (!g.load_banner_logged && g.core) {
        g.load_banner_logged = true;
        g.core_deferred = false;
        LogInfoF("NTE 伤害仪表盘 %s 已装载完成（core 就绪，日志开始记录）",
                 kPluginVersion);
    }

    const auto combat = RefreshServices();
    if (!combat) return;
    UpdateCombatant();
    UpdateSessionState();
    // DrainDamage 内部会用事件 tick_sequence 校准插件时钟（CalibrateClock）。
    DrainDamage();
    ServicePendingNames();
    // 若已用 tick_sequence 校准出帧率，且插件时钟与宿主帧序号明显脱节，
    // 就用校准值把时钟拉回真实时间（解决「插件 10 秒 ≈ 现实 12 秒」的漂移）。
    if (g.clock_calibrated && g.clock_tick_rate > 0.0 && g.clock_last_tick > 0) {
        const double expected = g.clock_last_tick_time +
            (static_cast<double>(g.last_drained_tick - g.clock_last_tick) *
             g.clock_tick_rate);
        const double drift = expected - g.session_time;
        // 只修超过 100ms 的漂移，且每次最多修正 50%，避免抖动自我强化。
        if (std::abs(drift) > 0.1) {
            g.session_time += drift * 0.5;
        }
    }
    ExpireBuffs(g.session_time);
    RefreshBuffNames();
    UpdateBattleState(g.session_time);

    // 首次拿到 combat service 时把游标对齐到当前水位，不回放插件加载前的历史。
    if (!g.cursor_primed && g.combat->latest_event_sequence != nullptr) {
        g.event_cursor = g.combat->latest_event_sequence(g.combat->user);
        g.cursor_primed = true;
    }
}

// ---------------------------------------------------------------------------
// on_draw：Render 线程，只读。所有聚合表在锁内拷贝成定长渲染快照。
// ---------------------------------------------------------------------------

struct RenderSnapshot {
    struct Row {
        std::string name;
        std::string role;
        std::uint64_t hits{};
        std::uint64_t crit{};
        std::uint64_t crit_known{};
        std::int64_t total{};
        std::int64_t max_hit{};
        double share{};      // 0..1
        bool is_local{};
        bool resolved{};
    };

    struct BuffRow {
        std::string name;
        std::string owner;
        std::string owner_role;
        std::int64_t stacks{};
        double remaining{};
        double duration{};
        double ratio{};
        bool resolved{};
        bool persistent{};   // 固定增益：不显示倒计时，只由 BUFF_REMOVE 移除
    };

    // 一个角色的技能明细（用于「角色 → 技能」层级展示）。
    struct SkillGroup {
        std::string character;      // 角色名
        std::vector<Row> skills;    // 该角色的技能，按伤害降序
        std::int64_t total{};       // 该角色合计
        double share{};             // 占全队比例
    };

    struct HitRow {
        std::uint32_t kind{};
        std::string source;
        std::string target;
        std::string name;
        std::int64_t value{};
        bool critical{};
        bool head_hit{};
        bool incoming{};
        bool outgoing{};
    };

    struct BattleSummary {
        bool active{};
        double started_at{};
        double duration{};
        std::uint64_t hits{};
        std::uint64_t crit{};
        std::uint64_t crit_known{};
        std::uint64_t head{};
        std::int64_t out_final{};
        std::int64_t max_hit{};
        std::uint64_t taken_hits{};
        std::int64_t taken_final{};
        std::uint64_t heal_hits{};
        std::int64_t heal_total{};
    };

    bool combat_available{};
    bool combatant_valid{};
    bool session_ready{};      // 会话服务报告 WORLD_READY
    bool stream_partial{};
    double now_seconds{};
    double battle_idle_seconds{};
    bool local_dead{};
    double hp{};
    double max_hp{};
    double shield{};

    BattleSummary battle;
    bool has_previous_battle{};
    std::vector<RenderSnapshot::Row> previous_battles;

    // 全程口径
    std::uint64_t total_hits{};
    std::uint64_t total_crit{};
    std::uint64_t total_crit_known{};
    std::uint64_t total_head{};
    std::int64_t total_final{};
    std::int64_t total_max_hit{};
    std::uint64_t taken_hits{};
    std::int64_t taken_final{};
    std::uint64_t heal_out_hits{};
    std::int64_t heal_out_total{};
    std::uint64_t total_event_count{};
    std::uint64_t battle_count{};

    bool authority_valid{};
    AnomalyNteCombatStatisticsV1 outgoing{};
    AnomalyNteCombatStatisticsV1 incoming{};

    std::vector<Row> team;        // 我 + 队友（条形图）
    std::int64_t team_total{};
    std::vector<Row> enemies;      // 来自敌人的承受伤害明细（按承受量排序）
    std::int64_t enemy_total{};    // 敌人对我造成的伤害合计（承受口径）
    std::vector<Row> skills;        // 全部技能汇总（按总伤害排序）
    std::int64_t skills_total{};
    std::vector<SkillGroup> skill_groups;  // 按角色分组的技能明细
    std::vector<BuffRow> buffs;           // 有明确时长的增益（带倒计时）
    std::vector<BuffRow> persistent_buffs; // 固定增益（战斗前就有 / 持续刷新，无倒计时）
    bool clock_calibrated{};              // 是否已用宿主 tick_sequence 校准时钟
    double clock_tick_rate{};             // 每 tick 秒数估计
    std::vector<HitRow> recent;
    std::uint64_t unknown_participants{};
    std::int64_t unknown_damage{};
};

void BuildRows(std::vector<RenderSnapshot::Row>& rows, const std::int64_t denominator,
               const std::size_t max_rows) {
    for (auto& row : rows) {
        row.share = denominator > 0 ? static_cast<double>(row.total) /
                                          static_cast<double>(denominator)
                                    : 0.0;
    }
    std::sort(rows.begin(), rows.end(),
              [](const RenderSnapshot::Row& left, const RenderSnapshot::Row& right) {
                  return left.total > right.total;
              });
    if (rows.size() > max_rows) rows.resize(max_rows);
}

RenderSnapshot CaptureSnapshot() {
    RenderSnapshot snapshot;
    snapshot.combat_available = static_cast<bool>(g.combat);
    snapshot.combatant_valid = g.combatant_valid;
    snapshot.session_ready = g.session_world_ready;
    snapshot.stream_partial = g.stream_partial;
    snapshot.now_seconds = g.session_time;
    snapshot.battle_idle_seconds = g.settings.battle_idle_seconds;
    snapshot.local_dead = g.local_dead;
    snapshot.hp = g.local_hp;
    snapshot.max_hp = g.local_max_hp;
    snapshot.shield = g.local_shield;

    // ---- 本场战斗（进行中则取实时值，否则取最后一场存档）----
    if (g.battle_active) {
        snapshot.battle.active = true;
        snapshot.battle.started_at = g.battle_started_at;
        snapshot.battle.duration = g.session_time - g.battle_started_at;
        snapshot.battle.hits = g.battle_hits;
        snapshot.battle.crit = g.battle_crit;
        snapshot.battle.crit_known = g.battle_crit_known;
        snapshot.battle.head = g.battle_head;
        snapshot.battle.out_final = g.battle_out_final;
        snapshot.battle.max_hit = g.battle_max_hit;
        snapshot.battle.taken_hits = g.battle_taken_hits;
        snapshot.battle.taken_final = g.battle_taken_final;
        snapshot.battle.heal_hits = g.battle_heal_hits;
        snapshot.battle.heal_total = g.battle_heal_total;
    } else if (!g.battle_history.empty() && g.battle_history.front().valid) {
        const auto& record = g.battle_history.front();
        snapshot.battle.active = false;
        snapshot.battle.started_at = record.started_at;
        snapshot.battle.duration = record.ended_at - record.started_at;
        snapshot.battle.hits = record.hits;
        snapshot.battle.crit = record.crit;
        snapshot.battle.crit_known = record.crit_known;
        snapshot.battle.head = record.head;
        snapshot.battle.out_final = record.out_final;
        snapshot.battle.max_hit = record.max_hit;
        snapshot.battle.taken_hits = record.taken_hits;
        snapshot.battle.taken_final = record.taken_final;
        snapshot.battle.heal_hits = record.heal_hits;
        snapshot.battle.heal_total = record.heal_total;
        snapshot.has_previous_battle = true;
    }

    // ---- 历史战斗摘要 ----
    for (const auto& record : g.battle_history) {
        if (!record.valid) continue;
        RenderSnapshot::Row row;
        row.name = record.top_character.empty() ? std::string("未识别角色") : record.top_character;
        row.total = record.top_character_damage;
        row.hits = record.hits;
        row.max_hit = record.out_final;
        snapshot.previous_battles.push_back(std::move(row));
    }

    // ---- 全程 ----
    snapshot.total_hits = g.total_hits;
    snapshot.total_crit = g.total_crit;
    snapshot.total_crit_known = g.total_crit_known;
    snapshot.total_head = g.total_head;
    snapshot.total_final = g.total_final;
    snapshot.total_max_hit = g.total_max_hit;
    snapshot.taken_hits = g.taken_hits;
    snapshot.taken_final = g.taken_final;
    snapshot.heal_out_hits = g.heal_out_hits;
    snapshot.heal_out_total = g.heal_out_total;
    snapshot.total_event_count = g.total_event_count;
    snapshot.battle_count = g.battle_count;
    snapshot.authority_valid = g.authority_valid;
    snapshot.outgoing = g.outgoing_stats;
    snapshot.incoming = g.incoming_stats;

    // ---- 角色分类 + 队伍条形图 ----
    std::vector<RenderSnapshot::Row> team_rows;
    std::vector<RenderSnapshot::Row> enemy_rows;
    std::int64_t team_total = 0;
    std::int64_t enemy_total = 0;

    for (const auto& entry : g.characters) {
        const auto& character = entry.second;
        // 这个游戏里四名角色全部由我操控（轮换出场，不是队友 NPC），
        // 所以只有两种身份：我操控的角色，和敌人。
        const int role = character.is_local ? kRoleLocal : kRoleEnemy;

        const std::int64_t damage = g.battle_active ? character.battle_out_final
                                                    : character.out_final;
        // 承受口径：敌人打了我多少。这是「来自敌人的伤害」，不是敌人挨了多少。
        const std::int64_t taken = g.battle_active ? character.battle_in_final
                                                   : character.in_final;
        if (role == kRoleEnemy) {
            // 敌人：只统计「他打了我多少」（承受口径），不计他挨了多少。
            enemy_total += taken;
            if (taken > 0) {
                RenderSnapshot::Row row;
                row.name = character.name_resolved && !character.name.empty()
                               ? character.name
                               : std::string("未识别敌人");
                row.role = RoleLabel(role);
                row.hits = g.battle_active ? character.battle_in_hits : character.in_hits;
                row.total = taken;
                row.max_hit = character.battle_in_final;
                row.resolved = character.name_resolved;
                enemy_rows.push_back(std::move(row));
            }
            continue;
        }

        RenderSnapshot::Row row;
        row.name = character.name_resolved && !character.name.empty()
                       ? character.name
                       : std::string("我");
        row.role = RoleLabel(role);
        row.hits = g.battle_active ? character.battle_out_hits : character.out_hits;
        row.crit = character.out_crit;
        row.crit_known = character.out_crit_known;
        row.total = damage;
        row.max_hit = g.battle_active ? character.battle_out_max_hit : character.out_max_hit;
        row.is_local = character.is_local;
        row.resolved = character.name_resolved;
        team_rows.push_back(std::move(row));
        team_total += damage;
    }
    BuildRows(team_rows, team_total, kMaximumTeamRows);
    snapshot.team = std::move(team_rows);
    snapshot.team_total = team_total;

    // 敌人承受口径：按「打了我多少」排序，分母取本场承受总量。
    {
        const std::int64_t denominator = g.battle_active ? g.battle_taken_final
                                                         : g.taken_final;
        BuildRows(enemy_rows, denominator, kMaximumTeamRows);
        snapshot.enemies = std::move(enemy_rows);
        snapshot.enemy_total = enemy_total;
    }
    snapshot.unknown_participants = 0;
    snapshot.unknown_damage = 0;

    // ---- 技能/来源 ----
    // 两个维度：① 全队技能汇总（同名技能归并成一行）② 每个角色各自的技能明细。
    // 宿主给同一个技能可能分配多个 name_id（层数/变体），所以名字解析出来后
    // 必须按「名字」归并，否则同一技能会被拆成多行。
    {
        // ① 全队汇总：key = 解析后的名字。名字还没解析出来的行不进这个表，
        // 而是汇进一条「未知技能」汇总行——绝不允许出现「技能#<数字>」这种
        // 把宿主内部 id 当名字显示的乱码行。
        //
        // 只遍历「我操控的角色」自己的技能表，绝不读全局 g.sources ——
        // 宿主的 name_id 是 DamageEvent.DamageGEDef 的 source 对象 id，
        // 同一个 GE 定义对象会被不同角色复用，全局表会把敌人打我的伤害
        // 混进我方技能统计。
        std::unordered_map<std::string, RenderSnapshot::Row> merged;
        RenderSnapshot::Row unresolved_total;
        for (const auto& character_entry : g.characters) {
            if (!character_entry.second.is_local) continue;
            for (const auto& source_entry : character_entry.second.sources) {
                const auto& source = source_entry.second;
                const std::int64_t total = g.battle_active ? source.battle_final_total
                                                           : source.final_total;
                if (total == 0 && source.hits == 0) continue;
                if (!source.name_resolved || source.name.empty()) {
                    unresolved_total.hits += g.battle_active ? source.battle_hits : source.hits;
                    unresolved_total.crit += source.crit_hits;
                    unresolved_total.crit_known += source.crit_known;
                    unresolved_total.max_hit = std::max(unresolved_total.max_hit,
                                                        source.max_hit);
                    unresolved_total.total += total;
                    continue;
                }
                auto& row = merged[source.name];
                if (row.name.empty()) {
                    row.name = source.name;
                    row.resolved = true;
                    row.max_hit = source.max_hit;
                } else {
                    row.max_hit = std::max(row.max_hit, source.max_hit);
                }
                row.hits += g.battle_active ? source.battle_hits : source.hits;
                row.crit += source.crit_hits;
                row.crit_known += source.crit_known;
                row.total += total;
            }
        }
        if (unresolved_total.hits != 0 || unresolved_total.total != 0) {
            unresolved_total.name = "未知技能";
            unresolved_total.resolved = false;
            merged["\x01unresolved"] = std::move(unresolved_total);
        }
        std::vector<RenderSnapshot::Row> rows;
        rows.reserve(merged.size());
        std::int64_t skills_total = 0;
        for (auto& entry : merged) {
            skills_total += entry.second.total;
            rows.push_back(std::move(entry.second));
        }
        const std::int64_t denominator = g.battle_active ? g.battle_out_final : g.total_final;
        BuildRows(rows, denominator, g.settings.max_rows);
        snapshot.skills = std::move(rows);
        snapshot.skills_total = skills_total;

        // ② 按角色分组：只列我操控的角色，每个角色下面挂他的技能。
        std::vector<RenderSnapshot::SkillGroup> groups;
        for (const auto& entry : g.characters) {
            const auto& character = entry.second;
            if (!character.is_local) continue;
            std::unordered_map<std::string, RenderSnapshot::Row> character_merged;
            RenderSnapshot::Row character_unresolved;
            for (const auto& source_entry : character.sources) {
                const auto& source = source_entry.second;
                const std::int64_t total = g.battle_active ? source.battle_final_total
                                                           : source.final_total;
                if (total == 0 && source.hits == 0) continue;
                if (!source.name_resolved || source.name.empty()) {
                    character_unresolved.hits += g.battle_active ? source.battle_hits
                                                                 : source.hits;
                    character_unresolved.crit += source.crit_hits;
                    character_unresolved.crit_known += source.crit_known;
                    character_unresolved.max_hit = std::max(character_unresolved.max_hit,
                                                            source.max_hit);
                    character_unresolved.total += total;
                    continue;
                }
                auto& row = character_merged[source.name];
                if (row.name.empty()) {
                    row.name = source.name;
                    row.resolved = true;
                    row.max_hit = source.max_hit;
                } else {
                    row.max_hit = std::max(row.max_hit, source.max_hit);
                }
                row.hits += g.battle_active ? source.battle_hits : source.hits;
                row.crit += source.crit_hits;
                row.crit_known += source.crit_known;
                row.total += total;
            }
            if (character_unresolved.hits != 0 || character_unresolved.total != 0) {
                character_unresolved.name = "未知技能";
                character_unresolved.resolved = false;
                character_merged["\x01unresolved"] = std::move(character_unresolved);
            }
            if (character_merged.empty()) continue;
            RenderSnapshot::SkillGroup group;
            group.character = character.name_resolved && !character.name.empty()
                                  ? character.name
                                  : std::string("我");
            group.skills.reserve(character_merged.size());
            for (auto& item : character_merged) {
                group.total += item.second.total;
                group.skills.push_back(std::move(item.second));
            }
            BuildRows(group.skills, group.total, kMaximumSkillRows);
            groups.push_back(std::move(group));
        }
        std::sort(groups.begin(), groups.end(),
                  [](const RenderSnapshot::SkillGroup& left,
                     const RenderSnapshot::SkillGroup& right) {
                      return left.total > right.total;
                  });
        const std::int64_t group_denominator = skills_total > 0 ? skills_total : 1;
        for (auto& group : groups) {
            group.share = static_cast<double>(group.total) /
                          static_cast<double>(group_denominator);
        }
        snapshot.skill_groups = std::move(groups);
    }

    // ---- 增益倒计时 ----
    // 固定增益（战斗前就有 / 被反复刷新）单独一组，不参与倒计时排序。
    std::vector<RenderSnapshot::BuffRow> timed_buffs;
    std::vector<RenderSnapshot::BuffRow> persistent_buffs;
    // 名字还没解析出来的增益整行跳过——绝不用「增益#<数字>」占住列表。
    // 名字一旦解析成功，下一帧这里就会显示真名。
    for (const auto& entry : g.active_buffs) {
        const auto& buff = entry.second;
        if (!buff.name_resolved || buff.name.empty() || LooksLikeObjectPath(buff.name)) continue;
        RenderSnapshot::BuffRow row;
        row.name = buff.name;
        row.owner = buff.target_resolved && !buff.target_name.empty() ? buff.target_name
                                                                     : std::string("未知");
        row.owner_role = RoleLabel(g.IsMine(buff.target) ? kRoleLocal : kRoleEnemy);
        row.stacks = buff.stacks;
        row.persistent = buff.persistent;
        row.remaining = row.duration > 0.0
                            ? std::max(0.0, row.duration - (g.session_time - buff.applied_at))
                            : 0.0;
        row.ratio = row.duration > 0.0 ? row.remaining / row.duration : 0.0;
        row.resolved = buff.name_resolved;
        if (buff.persistent) {
            persistent_buffs.push_back(std::move(row));
        } else {
            timed_buffs.push_back(std::move(row));
        }
    }
    std::sort(timed_buffs.begin(), timed_buffs.end(),
              [](const RenderSnapshot::BuffRow& left, const RenderSnapshot::BuffRow& right) {
                  if (left.duration <= 0.0) return false;
                  if (right.duration <= 0.0) return true;
                  return left.remaining < right.remaining;
              });
    snapshot.buffs = std::move(timed_buffs);
    snapshot.persistent_buffs = std::move(persistent_buffs);
    if (snapshot.buffs.size() > kMaximumBuffRows) snapshot.buffs.resize(kMaximumBuffRows);
    if (snapshot.persistent_buffs.size() > kMaximumBuffRows) {
        snapshot.persistent_buffs.resize(kMaximumBuffRows);
    }
    snapshot.clock_calibrated = g.clock_calibrated && g.clock_tick_rate > 0.0;
    snapshot.clock_tick_rate = g.clock_tick_rate;

    // ---- 最近命中 ----
    snapshot.recent.reserve(g.recent_hits.size());
    for (auto it = g.recent_hits.rbegin(); it != g.recent_hits.rend(); ++it) {
        const auto& hit = *it;
        RenderSnapshot::HitRow row;
        row.kind = hit.kind;
        row.source = hit.source;
        row.target = hit.target;
        row.name = hit.name;
        row.value = hit.value;
        row.critical = hit.critical;
        row.head_hit = hit.head_hit;
        row.incoming = hit.incoming;
        row.outgoing = hit.outgoing;
        snapshot.recent.push_back(std::move(row));
    }
    return snapshot;
}

// ---------------------------------------------------------------------------
// 绘制辅助
// ---------------------------------------------------------------------------
void Text(const AnomalyUiServiceV1* ui, const std::string& value) {
    ui->text(ui->user, {value.data(), value.size()});
}

void TextF(const AnomalyUiServiceV1* ui, const char* format, ...) {
    char buffer[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    ui->text(ui->user, {buffer, std::strlen(buffer)});
}

// 返回格式化后的字符串（供需要在同一行拼装的场合使用）。
template <typename... Args>
std::string TextFmt(const char* format, Args... args) {
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer), format, args...);
    return buffer;
}

std::string FormatNumber(const std::int64_t value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    if (value < 0) out += '-';
    const auto remainder = digits.size() % 3;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (index - remainder) % 3 == 0) out += ',';
        out += digits[index];
    }
    return out;
}

std::string FormatCompact(const std::int64_t value) {
    const auto magnitude = value < 0 ? -value : value;
    char buffer[64]{};
    if (magnitude >= 100000000) {
        std::snprintf(buffer, sizeof(buffer), "%.2f亿",
                      static_cast<double>(value) / 100000000.0);
    } else if (magnitude >= 10000) {
        std::snprintf(buffer, sizeof(buffer), "%.2f万",
                      static_cast<double>(value) / 10000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    }
    return buffer;
}

std::string FormatPercent(const double ratio) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f%%", ratio * 100.0);
    return buffer;
}

std::string FormatDuration(const double seconds) {
    char buffer[48]{};
    if (seconds < 60.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1f 秒", seconds);
    } else {
        const auto total = static_cast<long long>(seconds);
        std::snprintf(buffer, sizeof(buffer), "%lld 分 %lld 秒", total / 60, total % 60);
    }
    return buffer;
}

// 图形条的字形选择（重要）：
//   宿主的平台字体图集只烘焙了 GB2312 全部汉字 + General Punctuation
//   (0x2000-0x206F)，**没有烘焙** Block Elements(0x2580-0x259F) 和
//   Box Drawing(0x2500-0x257F)。所以 █ ░ ━ │ ┌ 这类字符一律会渲染成
//   缺字方框——界面上出现的一串 ?????? 就是这么来的。这里只用确定有字形的
//   字符：实心段 ASCII '#'，空心段 ASCII '.'，两端用 '|' 包裹，
//   以 2 字符为一个「格」，读数更粗壮。
// 进度条本身仍然是文本：UI 服务没有 ProgressBar，也没有导出 draw list，
// 插件无法真正画矩形。
std::string Bar(const double ratio, const std::size_t width) {
    const auto filled = static_cast<std::size_t>(
        std::clamp(ratio, 0.0, 1.0) * static_cast<double>(width) + 0.5);
    std::string out;
    out.reserve(width * 2 + 4);
    out += '|';
    for (std::size_t index = 0; index < width; ++index) {
        out += index < filled ? '#' : '.';
    }
    out += '|';
    return out;
}

// ---------------------------------------------------------------------------
// 图形化文本工具（在宿主字体能力范围内做出「图形」观感）
// ---------------------------------------------------------------------------

// UTF-8 下一个码点的字节数。
std::size_t Utf8SequenceLength(const unsigned char lead) {
    if ((lead & 0x80U) == 0U) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;
}

// UTF-8 字符数（不是字节数）。界面对齐必须按字符算，否则中文会错位。
std::size_t Utf8Length(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        index += Utf8SequenceLength(lead);
        ++count;
    }
    return count;
}

// 把字符串按字符数截断（超出加省略号）。
std::string Utf8Truncate(const std::string& text, const std::size_t limit) {
    if (Utf8Length(text) <= limit) return text;
    std::string out;
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size() && count + 1 < limit;) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const std::size_t step = Utf8SequenceLength(lead);
        out.append(text, index, step);
        index += step;
        ++count;
    }
    while (Utf8Length(out) < limit) out += ' ';
    return out;
}

// 右侧补空格到指定字符数（用于列对齐）。超过了就截断。
std::string Utf8Pad(const std::string& text, const std::size_t width) {
    const std::size_t length = Utf8Length(text);
    if (length >= width) return Utf8Truncate(text, width);
    return text + std::string(width - length, ' ');
}

// 分区标题：用 GB2312 里一定存在的制表符号做装饰。
// 「◆」「■」在 GB2312 符号区内，但为稳妥起见只使用破折号与汉字。
void SectionTitle(const AnomalyUiServiceV1* ui, const char* title) {
    if (ui->separator != nullptr) ui->separator(ui->user);
    std::string line = "— ";
    line += title;
    Text(ui, line);
}

// 大数字块：把关键指标做成「卡片」式的粗体标题 + 大号数字。
// 高度**必须给具体数值**：传 0 时 ImGui 子窗口会按剩余的可用空间拉伸，
// 于是整块卡片被撑成一列贯穿全屏的高墙（用户实测截图就是这个现象）。
// 指标行：纯文本，一行一个，不再用子窗口做「卡片」。
// 用户明确要求「把框框去了」：子窗口既带来高度被撑满的渲染问题，
// 又让内容被塞进滚动区域，滚动时直接崩溃。这里只输出普通文本行。
void MetricBlock(const AnomalyUiServiceV1* ui, const char* label, const std::string& value,
                 const char* suffix, const float /*width*/) {
    std::string line = label;
    line += "  ";
    line += value;
    if (suffix != nullptr && *suffix != '\0') {
        line += ' ';
        line += suffix;
    }
    Text(ui, line);
}

// 带条形的指标行：名称列对齐 + 图形条 + 百分比 + 数值。
void BarRow(const AnomalyUiServiceV1* ui, const std::string& name, const std::size_t name_width,
            const double ratio, const std::size_t bar_width, const std::string& percent,
            const std::string& value) {
    std::string line = Utf8Pad(name, name_width);
    line += ' ';
    line += Bar(ratio, bar_width);
    line += ' ';
    line += Utf8Pad(percent, 7);
    line += ' ';
    line += value;
    Text(ui, line);
}

const char* KindLabel(const std::uint32_t kind) {
    switch (kind) {
        case ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE: return "伤害";
        case ANOMALY_NTE_COMBAT_EVENT_V1_HEAL: return "治疗";
        case ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD: return "增益";
        case ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE: return "增益结束";
        default: return "?";
    }
}

void OnDraw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || ui == nullptr) return;

    RenderSnapshot snapshot;
    Settings settings;
    {
        std::scoped_lock lock(*g.mutex);
        snapshot = CaptureSnapshot();
        settings = g.settings;
    }

    // 窗口尺寸策略：
    //   SDK 的 set_next_window_size 只收一个裸 uint32_t condition，且没有导出
    //   具名常量，所以这里用 ImGui 的公开枚举值 ImGuiCond_FirstUseEver = 4。
    //   只在用户没有亲手拖过尺寸时设置一次，之后完全交给用户。
    constexpr std::uint32_t kCondFirstUseEver = 4U;
    const bool has_saved_size = settings.window_width > 0.0 && settings.window_height > 0.0;
    float applied_width = kDashboardDefaultWidth;
    float applied_height = kDashboardDefaultHeight;
    if (has_saved_size) {
        applied_width = static_cast<float>(settings.window_width);
        applied_height = static_cast<float>(settings.window_height);
    }
    // 高度**每帧都传 0**，由宿主按内容自动贴合。
    // 原因：宿主对插件窗口强制了 NoScrollbar | NoScrollWithMouse，却又给窗口
    // 加了内部 BeginChild 滚动区域——高度一旦被固定住，内容多出来就得滚动，
    // 而那条滚动路径会直接把插件搞崩（用户实测：往下拉就退出并报错）。
    // 让高度永远贴合内容，滚动条根本不会出现，也就没有崩溃路径。
    // 宽度保留用户拖拽的结果，用户仍可自由调整。
    ui->set_next_window_size(ui->user, applied_width, 0.0f, kCondFirstUseEver);
    // 上下限：只挡住「小到看不见」的极端情况。
    if (ui->set_next_window_size_constraints != nullptr) {
        ui->set_next_window_size_constraints(ui->user, kDashboardMinimumWidth,
                                             kDashboardMinimumHeight, 0.0f, 0.0f);
    }
    anomaly::sdk::UiWindow window(ui, "NTE 伤害仪表盘", &context->window_open);
    if (!window) return;

    // 读回宿主真实尺寸，只用来记住用户拖出来的**宽度**。
    // 高度由内容决定，不保存——保存了就会在下次启动时把内容切成滚动区域。
    if (ui->get_window_size != nullptr) {
        float width = 0.0f;
        float height = 0.0f;
        ui->get_window_size(ui->user, &width, &height);
        if (width > 0.0f && height > 0.0f) {
            const bool changed = context->window_size_observed &&
                                 std::abs(width - context->last_window_width) > 0.5f;
            // ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE (1u<<3)：ImGui 在拖动
            // 窗口边缘/标题栏时必然 WantCaptureMouse；自动贴合与折叠恢复
            // 都发生在鼠标空闲状态，不会误判成用户拖拽。
            const bool mouse_active =
                ui->frame_state != nullptr &&
                (ui->frame_state(ui->user) & ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE) != 0;
            if (changed && mouse_active && width >= kDashboardMinimumWidth) {
                context->window_user_resized = true;
                std::scoped_lock lock(*g.mutex);
                const double new_width = static_cast<double>(width);
                if (std::abs(new_width - g.settings.window_width) >= 0.5) {
                    g.settings.window_width = new_width;
                    g.settings_dirty = true;
                }
            }
            context->last_window_width = width;
            context->last_window_height = height;
            context->window_size_observed = true;
        }
    }

    // ---------------------------------------------------------------------
    // 1. 状态行 + 本场战斗汇总
    // ---------------------------------------------------------------------
    // 拿不到战斗数据时不能只显示「不可用」——那等于让用户对着一句废话干瞪眼。
    // 这里逐条列出到底哪一环断了，用户可以直接把这段读给我或贴进反馈里。
    if (!snapshot.combat_available || !snapshot.combatant_valid) {
        SectionTitle(ui, "尚未取到战斗数据");
        TextF(ui, "宿主的 nte.combat 服务：%s",
              snapshot.combat_available ? "已就绪" : "不可用");
        TextF(ui, "当前角色快照：%s", snapshot.combatant_valid ? "有效" : "无效");
        if (!snapshot.combat_available) {
            Text(ui, "原因：宿主 profile 还在 degraded，或尚未进入关卡世界。");
        } else {
            Text(ui, "原因：已连上战斗服务，但 readable 角色快照还没准备好。");
        }
        Text(ui, "进入关卡并打出第一下伤害后会自动开始统计。");
        Text(ui, "详细日志见 Anomaly 诊断页的「日志」标签。");
        TextF(ui, "插件版本 %s", kPluginVersion);
        return;
    }

    const auto& battle = snapshot.battle;
    const bool battle_has_data = battle.hits > 0 || battle.taken_hits > 0 ||
                                 battle.heal_hits > 0;
    if (!battle_has_data && snapshot.total_event_count == 0) {
        SectionTitle(ui, "等待第一次伤害");
        TextF(ui, "已连接战斗场景：生命 %.0f / %.0f", snapshot.hp, snapshot.max_hp);
        Text(ui, "把这一行血量打出来，面板就会开始计数。");
        TextF(ui, "插件版本 %s", kPluginVersion);
        return;
    }

    if (battle_has_data) {
        // 状态徽标：用方括号 + 汉字做「标签」观感（宿主无彩色文字能力）。
        {
            std::string state = battle.active ? "战斗进行中" : "上一场已结束";
            state += "  ";
            state += FormatDuration(battle.duration);
            SectionTitle(ui, battle.active ? "战斗中 · 实时统计" : "战斗已结束 · 最终统计");
            Text(ui, state);
        }

        const auto crit_rate = battle.crit_known > 0
                                   ? static_cast<double>(battle.crit) /
                                         static_cast<double>(battle.crit_known)
                                   : 0.0;

        // 核心指标改成普通文本行（不再用子窗口）。
        // 用 same_line 排成一行，窗口才不会被撑高。
        if (ui->same_line != nullptr) {
            MetricBlock(ui, "总伤害", FormatNumber(battle.out_final), "", 0.0f);
            ui->same_line(ui->user, 0.0f, 14.0f);
            MetricBlock(ui, "命中", std::to_string(battle.hits), "次", 0.0f);
            ui->same_line(ui->user, 0.0f, 14.0f);
            MetricBlock(ui, "最高单次", FormatCompact(battle.max_hit), "", 0.0f);
            if (battle.crit_known > 0) {
                ui->same_line(ui->user, 0.0f, 14.0f);
                MetricBlock(ui, "暴击率", FormatPercent(crit_rate).c_str(), "", 0.0f);
            }
        } else {
            TextF(ui, "本场总伤害 %s    命中 %llu    最高单次 %s",
                  FormatNumber(battle.out_final).c_str(),
                  static_cast<unsigned long long>(battle.hits),
                  FormatCompact(battle.max_hit).c_str());
            if (battle.crit_known > 0) {
                TextF(ui, "暴击 %llu / %llu（%s）",
                      static_cast<unsigned long long>(battle.crit),
                      static_cast<unsigned long long>(battle.crit_known),
                      FormatPercent(crit_rate).c_str());
            }
        }

        // 生命 / 护盾 / 承受 / 治疗全部用条形表示。
        if (snapshot.max_hp > 0.0) {
            const double hp_ratio = snapshot.hp / snapshot.max_hp;
            BarRow(ui, "生命", 8, hp_ratio, 20, FormatPercent(hp_ratio),
                   TextFmt("%.0f / %.0f", snapshot.hp, snapshot.max_hp));
        }
        if (snapshot.shield > 0.0) {
            const double shield_ratio =
                snapshot.max_hp > 0.0 ? snapshot.shield / snapshot.max_hp : 0.0;
            BarRow(ui, "护盾", 8, shield_ratio, 20, FormatPercent(shield_ratio),
                   TextFmt("%.0f", snapshot.shield));
        }
        if (battle.crit_known > 0) {
            BarRow(ui, "暴击率", 8, crit_rate, 20, FormatPercent(crit_rate),
                   TextFmt("%llu / %llu", static_cast<unsigned long long>(battle.crit),
                           static_cast<unsigned long long>(battle.crit_known)));
        }
        if (battle.taken_final > 0) {
            // 承受条以「本场伤害」为满刻度，直观看出挨打占输出的比例。
            const double taken_ratio =
                battle.out_final > 0
                    ? static_cast<double>(battle.taken_final) /
                          static_cast<double>(battle.out_final)
                    : 0.0;
            BarRow(ui, "承受", 8, std::clamp(taken_ratio, 0.0, 1.0), 20,
                   FormatPercent(std::clamp(taken_ratio, 0.0, 1.0)),
                   TextFmt("%s（%llu 次）", FormatNumber(battle.taken_final).c_str(),
                           static_cast<unsigned long long>(battle.taken_hits)));
        }
        if (battle.heal_total > 0) {
            const double heal_ratio =
                battle.out_final > 0
                    ? static_cast<double>(battle.heal_total) /
                          static_cast<double>(battle.out_final)
                    : 0.0;
            BarRow(ui, "治疗", 8, std::clamp(heal_ratio, 0.0, 1.0), 20,
                   FormatPercent(std::clamp(heal_ratio, 0.0, 1.0)),
                   TextFmt("%s（%llu 次）", FormatNumber(battle.heal_total).c_str(),
                           static_cast<unsigned long long>(battle.heal_hits)));
        }

        if (snapshot.local_dead) Text(ui, "状态：已倒下");

        // 手动结束本次战斗：只清零面板，不写历史记录。
        if (battle.active && ui->button != nullptr) {
            if (ui->button(ui->user, Sv("结束本次战斗统计并清零"), 220.0f, 0.0f) != 0) {
                std::scoped_lock lock(*g.mutex);
                // 直接置为非活动 + ResetBattleScope()，不调 EndBattle()（不存档）。
                g.battle_active = false;
                g.ResetBattleScope();
            }
        }
    }

    {
        std::string footer = snapshot.session_ready ? "场景：战斗场景内" : "场景：场景外";
        footer += "    全程累计 伤害 ";
        footer += FormatCompact(snapshot.total_final);
        footer += " · 承受 ";
        footer += FormatCompact(snapshot.taken_final);
        footer += " · 战斗 ";
        footer += std::to_string(snapshot.battle_count);
        footer += " 场 · 事件 ";
        footer += std::to_string(snapshot.total_event_count);
        footer += " 条";
        Text(ui, footer);
    }
    if (snapshot.stream_partial) {
        Text(ui, "* 事件环曾被甩掉，本场统计可能缺失早期数据。");
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    // ---------------------------------------------------------------------
    // 2. 队伍伤害条形图（四名我操控的角色，各自占比）
    // ---------------------------------------------------------------------
    SectionTitle(ui, "队伍伤害分布");
    if (snapshot.team.empty()) {
        Text(ui, "  （本场还没有可归属到我的角色的伤害）");
    } else {
        // 名称列宽按最长名字自适应，保证条形左端对齐成一条竖线。
        std::size_t name_width = 4;
        for (const auto& row : snapshot.team) {
            name_width = std::max(name_width, Utf8Length(row.name) + (row.resolved ? 0 : 2));
        }
        name_width = std::min<std::size_t>(name_width, 12);
        for (const auto& row : snapshot.team) {
            std::string label = row.name;
            if (!row.resolved) label += " ?";
            BarRow(ui, label, name_width, row.share, 20, FormatPercent(row.share),
                   TextFmt("%s  %llu 次 / 最高 %s", FormatNumber(row.total).c_str(),
                           static_cast<unsigned long long>(row.hits),
                           FormatCompact(row.max_hit).c_str()));
        }
        if (snapshot.team_total > 0) {
            TextF(ui, "合计 %s", FormatNumber(snapshot.team_total).c_str());
        }
    }

    // ---------------------------------------------------------------------
    // 3. 技能与来源：先按角色分组，每组下面列该角色的技能（同名技能已归并）
    // ---------------------------------------------------------------------
    if (settings.show_skills) {
        SectionTitle(ui, "技能占比");
        if (snapshot.skill_groups.empty()) {
            Text(ui, "  （暂无数据）");
        } else {
            for (const auto& group : snapshot.skill_groups) {
                // 角色分组标题：角色名 + 合计 + 占全队比例。
                std::string header = group.character;
                header += "   ";
                header += FormatNumber(group.total);
                header += "   占全队 ";
                header += FormatPercent(group.share);
                Text(ui, header);
                std::size_t name_width = 4;
                for (const auto& row : group.skills) {
                    name_width = std::max(name_width, Utf8Length(row.name));
                }
                name_width = std::min<std::size_t>(name_width, 12);
                for (const auto& row : group.skills) {
                    std::string label = row.name;
                    if (!row.resolved) label += " ?";
                    // 组内占比：相对该角色自己的合计。
                    const double local_share =
                        group.total > 0
                            ? static_cast<double>(row.total) /
                                  static_cast<double>(group.total)
                            : 0.0;
                    // 技能行不放进度条：条太长、信息密度低，纯数字列反而好读。
                    std::string line = Utf8Pad(label, name_width);
                    line += "  ";
                    line += Utf8Pad(FormatPercent(local_share), 7);
                    line += ' ';
                    line += FormatNumber(row.total);
                    line += "  ";
                    line += std::to_string(static_cast<unsigned long long>(row.hits));
                    line += " 次 / 最高 ";
                    line += FormatCompact(row.max_hit);
                    Text(ui, line);
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 4. 增益倒计时 / 固定增益
    // ---------------------------------------------------------------------
    if (settings.show_buffs) {
        SectionTitle(ui, "增益状态");
        if (snapshot.buffs.empty()) {
            Text(ui, "  （当前没有生效中的限时增益）");
        } else {
            std::size_t name_width = 4;
            for (const auto& row : snapshot.buffs) {
                name_width = std::max(name_width, Utf8Length(row.name));
            }
            name_width = std::min<std::size_t>(name_width, 12);
            for (const auto& row : snapshot.buffs) {
                std::string label = row.name;
                if (row.stacks > 1) label += " x" + std::to_string(row.stacks);
                if (row.duration > 0.0) {
                    BarRow(ui, label, name_width, row.ratio, 20,
                           FormatPercent(row.ratio),
                           TextFmt("剩余 %.1f 秒", row.remaining));
                } else {
                    BarRow(ui, label, name_width, 0.0, 20, "—",
                           std::string("持续时间未知"));
                }
            }
        }
        // 固定增益：战斗前就挂着、或持续被刷新的同一种 buff，只列出不倒数，
        // 由宿主的 BUFF_REMOVE 事件移除。
        if (!snapshot.persistent_buffs.empty()) {
            Text(ui, "固定增益");
            for (const auto& row : snapshot.persistent_buffs) {
                std::string label = row.name;
                if (!row.resolved) label += " ?";
                if (row.stacks > 1) label += " x" + std::to_string(row.stacks);
                TextF(ui, "%s  于 %s", label.c_str(), row.owner.c_str());
            }
        }
    }

    // ---------------------------------------------------------------------
    // 5. 最近命中
    // ---------------------------------------------------------------------
    if (settings.show_recent && !snapshot.recent.empty()) {
        SectionTitle(ui, "最近命中");
        // 文本对齐版本：来源列与目标列按字符数补空格，比表格更紧凑也更像「日志流」。
        std::size_t source_width = 4;
        std::size_t target_width = 4;
        for (const auto& hit : snapshot.recent) {
            source_width = std::max(source_width, Utf8Length(hit.source));
            target_width = std::max(target_width, Utf8Length(hit.target));
        }
        source_width = std::min<std::size_t>(source_width, 10);
        target_width = std::min<std::size_t>(target_width, 10);
        for (const auto& hit : snapshot.recent) {
            std::string detail = hit.name.empty() ? std::string("-") : hit.name;
            if (hit.critical) detail += " 暴击";
            std::string line = Utf8Pad(hit.source, source_width);
            line += "  ";
            line += Utf8Pad(hit.target, target_width);
            line += "  ";
            line += KindLabel(hit.kind);
            line += ' ';
            line += Utf8Pad(FormatNumber(hit.value), 10);
            line += detail;
            Text(ui, line);
        }
    }

    // ---------------------------------------------------------------------
    // 6. 设置
    // ---------------------------------------------------------------------
    if (ui->separator != nullptr) ui->separator(ui->user);
    Text(ui, "设置");
    if (ui->checkbox != nullptr && ui->slider_float != nullptr) {
        bool changed = false;
        int show_skills = settings.show_skills ? 1 : 0;
        changed |= ui->checkbox(ui->user, Sv("显示技能表"), &show_skills) != 0;
        settings.show_skills = show_skills != 0;
        int show_buffs = settings.show_buffs ? 1 : 0;
        changed |= ui->checkbox(ui->user, Sv("显示增益倒计时"), &show_buffs) != 0;
        settings.show_buffs = show_buffs != 0;
        int show_recent = settings.show_recent ? 1 : 0;
        changed |= ui->checkbox(ui->user, Sv("显示最近命中"), &show_recent) != 0;
        settings.show_recent = show_recent != 0;
        auto idle = static_cast<float>(settings.battle_idle_seconds);
        if (ui->slider_float(ui->user, Sv("战斗结束判定（闲置秒数）"), &idle, 3.0f, 30.0f) != 0) {
            settings.battle_idle_seconds = std::clamp(static_cast<double>(idle), 3.0, 30.0);
            changed = true;
        }
        auto rows = static_cast<float>(settings.max_rows);
        if (ui->slider_float(ui->user, Sv("技能表最多行数"), &rows, 4.0f, 24.0f) != 0) {
            settings.max_rows = static_cast<std::uint32_t>(rows + 0.5f);
            changed = true;
        }
        if (ui->button(ui->user, Sv("恢复默认宽度"), 130.0f, 0.0f) != 0) {
            std::scoped_lock lock(*g.mutex);
            g.settings.window_width = 0.0;
            g.settings.window_height = 0.0;
            g.settings_dirty = true;
            // 清掉「用户拖过」标记，让下一帧重新按默认宽度设置。
            context->window_user_resized = false;
            context->window_size_observed = false;
        }
        if (changed) {
            std::scoped_lock lock(*g.mutex);
            g.settings = settings;
            g.settings_dirty = true;
        }
    }

    // ---------------------------------------------------------------------
    // 7. 承受伤害 · 来自敌人（放在整个面板最下面，不夹在队伍表中间）
    // ---------------------------------------------------------------------
    if (!snapshot.enemies.empty()) {
        SectionTitle(ui, "承受伤害 · 来自敌人");
        std::size_t enemy_name_width = 4;
        for (const auto& row : snapshot.enemies) {
            enemy_name_width = std::max(enemy_name_width, Utf8Length(row.name));
        }
        enemy_name_width = std::min<std::size_t>(enemy_name_width, 12);
        for (const auto& row : snapshot.enemies) {
            BarRow(ui, row.name, enemy_name_width, row.share, 20, FormatPercent(row.share),
                   TextFmt("%s  %llu 次", FormatNumber(row.total).c_str(),
                           static_cast<unsigned long long>(row.hits)));
        }
        if (snapshot.enemy_total > 0) {
            TextF(ui, "承受合计 %s", FormatNumber(snapshot.enemy_total).c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void ResetStatisticsAll() {
    g.ResetBattleScope();
    g.battle_history.clear();
    g.battle_count = 0;
    g.sources.clear();
    g.characters.clear();
    g.active_buffs.clear();
    g.recent_hits.clear();
    g.local_attackers.clear();
    g.party_handles.clear();
    g.pending_participants.clear();
    g.pending_event_names.clear();
    g.total_hits = 0;
    g.total_crit = 0;
    g.total_crit_known = 0;
    g.total_head = 0;
    g.total_final = 0;
    g.total_max_hit = 0;
    g.taken_hits = 0;
    g.taken_final = 0;
    g.heal_out_hits = 0;
    g.heal_out_total = 0;
    g.heal_in_hits = 0;
    g.heal_in_total = 0;
    g.buff_applied_total = 0;
    g.buff_received_total = 0;
    g.buff_removed_total = 0;
    g.damage_event_count = 0;
    g.heal_event_count = 0;
    g.buff_event_count = 0;
    g.total_event_count = 0;
    g.stream_partial = false;
}

// ---------------------------------------------------------------------------
// 诊断自检：出现在 Anomaly「诊断 → 开发者资源」里，返回一段 JSON 摘要
// ---------------------------------------------------------------------------
// 标准的两次调用协议：第一次 destination 为空、返回需要的字节数，
// 第二次把 JSON 写进去。这里如实报告统计口径与数据链路状态。
AnomalyStatusV1 ANOMALY_CALL SelfTestCallback(
    void* /*callback_user*/, AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    if (inout_size == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }

    std::string json;
    {
        std::scoped_lock lock(*g.mutex);
        json += "{\"plugin\":\"";
        json += kPluginId;
        json += "\",";
        json += "\"version\":\"";
        json += kPluginVersion;
        json += "\",\"services\":{\"core\":";
        json += g.core ? "true" : "false";
        json += ",\"combat\":";
        json += g.combat ? "true" : "false";
        json += ",\"session\":";
        json += g.session ? "true" : "false";
        json += ",\"config\":";
        json += g.config ? "true" : "false";
        json += ",\"diagnostics\":";
        json += g.diagnostics ? "true" : "false";
        json += "},";
        json += "\"battle\":{\"active\":";
        json += g.battle_active ? "true" : "false";
        json += ",\"hits\":";
        json += std::to_string(g.battle_hits);
        json += ",\"out_final\":";
        json += std::to_string(g.battle_out_final);
        json += ",\"taken_final\":";
        json += std::to_string(g.battle_taken_final);
        json += "},";
        json += "\"totals\":{\"damage_events\":";
        json += std::to_string(g.damage_event_count);
        json += ",\"heal_events\":";
        json += std::to_string(g.heal_event_count);
        json += ",\"buff_events\":";
        json += std::to_string(g.buff_event_count);
        json += ",\"all_events\":";
        json += std::to_string(g.total_event_count);
        json += "},";
        json += "\"diagnostics\":{\"event_stream_partial\":";
        json += g.stream_partial ? "true" : "false";
        json += ",\"session_world_ready\":";
        json += g.session_world_ready ? "true" : "false";
        json += ",\"clock_calibrated\":";
        json += g.clock_calibrated ? "true" : "false";
        json += ",\"name_cache\":";
        json += std::to_string(g.combat_name_cache.size());
        json += ",\"pending_names\":";
        json += std::to_string(g.pending_event_names.size());
        json += ",\"tracked_characters\":";
        json += std::to_string(g.characters.size());
        json += ",\"tracked_sources\":";
        json += std::to_string(g.sources.size());
        json += "}}";
    }

    const std::size_t required = json.size() + 1;
    if (destination.data == nullptr || destination.size < required) {
        *inout_size = required;
        return {ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, 0,
                {"self test needs a larger buffer", 31}};
    }
    std::memcpy(destination.data, json.data(), json.size());
    destination.data[json.size()] = '\0';
    *inout_size = required;
    return {ANOMALY_STATUS_V1_OK, 0, {nullptr, 0}};
}

AnomalyStatusV1 OnLoad(const AnomalyHostApiV1* host, void** plugin_context) {
    g.Reset();
    g.host = host;
    *plugin_context = &g;

    const Host facade(host);
    g.combat = facade.Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID, 1);
    g.session = facade.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID, 1);
    g.json = facade.Query<AnomalyJsonServiceV1>(ANOMALY_JSON_SERVICE_V1_ID, 1);
    g.config = facade.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID, 1);
    g.core = facade.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
                                                ANOMALY_CORE_SERVICE_V1_VERSION);
    g.diagnostics = facade.Query<AnomalyDiagnosticsServiceV1>(
        ANOMALY_DIAGNOSTICS_SERVICE_V1_ID, ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION);
    g.notifications = facade.Query<AnomalyNotificationsServiceV1>(
        ANOMALY_NOTIFICATIONS_SERVICE_V1_ID, ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION);

    // 首次拿到 core 后立刻留一条装载记录，方便在「诊断 → 日志」里定位版本。
    LogInfoF("NTE 伤害仪表盘 %s 装载中（core=%s config=%s diagnostics=%s）",
             kPluginVersion,
             g.core ? "ok" : "missing",
             g.config ? "ok" : "missing",
             g.diagnostics ? "ok" : "missing");
    // 日志/通知/自检全是**可选**的诊断增强，任何一个取不到都必须继续加载。
    // 之前这里写成 `if (!g.core) return UNAVAILABLE`，结果 game-ready 阶段
    // core 还没就绪时整个插件拒绝启动，平台界面直接标红「故障」——
    // 一个日志服务不该有权决定插件能不能用。
    if (!g.core) {
        g.core_deferred = true;  // 之后在 on_update 里重试，拿到了再补写日志。
    }

    if (g.config) {
        const auto status = g.config->register_schema(
            g.config->user, {kConfigSchemaId, std::strlen(kConfigSchemaId)},
            kConfigSchemaVersion,
            {reinterpret_cast<const std::uint8_t*>(kConfigSchemaJson),
             std::strlen(kConfigSchemaJson)},
            &g.config_schema);
        g.config_schema_registered = status.code == ANOMALY_STATUS_V1_OK;
        if (!g.config_schema_registered) {
            LogErrorF("配置 schema 注册失败（status=%u），设置将无法持久化",
                      static_cast<unsigned>(status.code));
        }
    } else {
        LogWarning("anomaly.config 不可用，设置不会保存");
    }
    if (!g.diagnostics) {
        LogWarning("anomaly.diagnostics 不可用，自检条目不会出现在诊断页");
    }
    if (!g.notifications) {
        LogWarning("anomaly.notifications 不可用，插件无法在游戏内弹提示");
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 OnStart(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    context->started = true;

    if (context->config && context->config_schema_registered) {
        std::size_t size{};
        auto status = context->config->read(context->config->user,
                                            {kConfigSchemaId, std::strlen(kConfigSchemaId)},
                                            nullptr, {nullptr, 0}, &size);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            context->settings_loaded = true;
        } else if (IsSizingStatus(status) && size > 1) {
            std::string document(size, '\0');
            std::uint32_t stored_version = 0;
            status = context->config->read(context->config->user,
                                           {kConfigSchemaId, std::strlen(kConfigSchemaId)},
                                           &stored_version,
                                           {reinterpret_cast<std::uint8_t*>(document.data()),
                                            document.size()},
                                           &size);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                if (const auto end = document.find('\0'); end != std::string::npos) {
                    document.resize(end);
                }
                ParseSettings(document);
                context->settings_loaded = true;
            }
        }
    }
    if (context->combat && context->combat->latest_event_sequence != nullptr) {
        // 从当前水位开始计数，不回放插件加载前的历史。
        context->event_cursor =
            context->combat->latest_event_sequence(context->combat->user);
    }

    // 注册诊断自检：Anomaly「诊断 → 开发者资源」里会出现一条可运行的检查项，
    // 点它就能拿到本插件当前的服务状态、统计口径与事件计数。
    if (context->diagnostics && context->diagnostics->register_self_test != nullptr &&
        !context->self_test_registered) {
        const std::string self_test_id = std::string(kPluginId) + ".selftest";
        const auto status = context->diagnostics->register_self_test(
            context->diagnostics->user, {self_test_id.c_str(), self_test_id.size()},
            &SelfTestCallback, nullptr, &context->self_test_handle);
        context->self_test_registered = status.code == ANOMALY_STATUS_V1_OK;
        if (context->self_test_registered) {
            LogInfoF("诊断自检已注册：%s", self_test_id.c_str());
        } else {
            LogWarningF("诊断自检注册失败（status=%u）", static_cast<unsigned>(status.code));
        }
    }

    LogInfoF("伤害仪表盘已启动：combat=%s session=%s（未进战斗场景时 combat 为缺失属正常）",
             context->combat ? "ok" : "missing",
             context->session ? "ok" : "missing");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 OnStop(void* plugin_context, std::uint32_t /*deadline_milliseconds*/) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    std::scoped_lock lock(*g.mutex);
    if (context->settings_dirty) SaveSettings();
    if (context->config && context->config_schema_registered &&
        context->config_schema.id != 0) {
        context->config->unregister_schema(context->config->user, context->config_schema);
        context->config_schema = {};
        context->config_schema_registered = false;
    }
    context->started = false;
    return anomaly::sdk::Ok();
}

void OnUnload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    if (context->self_test_registered && context->diagnostics &&
        context->diagnostics->unregister_self_test != nullptr) {
        // 显式注销，重载后立刻让自检条目消失，不留下指向旧镜像的回调。
        context->diagnostics->unregister_self_test(context->diagnostics->user,
                                                   context->self_test_handle);
        context->self_test_registered = false;
        context->self_test_handle = {};
    }
    std::scoped_lock lock(*g.mutex);
    context->combat = {};
    context->session = {};
    context->json = {};
    context->config = {};
    context->core = {};
    context->diagnostics = {};
    context->notifications = {};
    g.Reset();
}

}  // namespace

// ---------------------------------------------------------------------------
// C++ 异常桥接
// ---------------------------------------------------------------------------
// Anomaly 2.2.0 宿主对每个「导入 _CxxThrowException 的插件镜像」强制要求额外
// 导出一个异常桥接符号；缺失时报：
//   plugin native dependency denied: ... code=missing-cxx-throw-bridge
//   C++ plugin image does not export the exception bridge; rebuild with the Anomaly SDK
//
// 宿主用自带 CRT 的映像替换插件的 VCRUNTIME140 导入表，使插件抛出的 C++ 异常
// 由宿主运行时接管。宿主替换的目标就是本符号。
// 官方 MSVC 样例（NteCombatDemo）导出两个符号：AnomalyPluginEntryV1 +
// AnomalyPluginCxxThrowV1；SDK 头文件里没有它的声明，属于约定导出。
//
// 语义等价于 MSVC 运行时向 UnhandledExceptionFilter 投递一个 C++ 异常记录：
//   RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE,
//                  4, {0x19930520, exception_object, throw_info, image_base})
// 宿主接管后正常路径不会走到这里；未接管时维持默认的 fail-fast 行为。
#if defined(_MSC_VER) && defined(_M_X64)
extern "C" __declspec(dllexport) void __cdecl AnomalyPluginCxxThrowV1(
    void* exception_object, const void* throw_info) {
    // RaiseException 的第 4 个参数是 ULONG_PTR 数组；MSVC 运行时投递的
    // C++ 异常记录正是这四项。image_base 取本模块加载基址（64K 对齐）。
    const auto image_base = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(&AnomalyPluginCxxThrowV1) &
        ~static_cast<std::uintptr_t>(0xFFFF));

    const ULONG_PTR arguments[4] = {
        0x19930520UL,
        reinterpret_cast<ULONG_PTR>(exception_object),
        reinterpret_cast<ULONG_PTR>(throw_info),
        reinterpret_cast<ULONG_PTR>(image_base)};

    constexpr DWORD kCxxExceptionCode = 0xE06D7363UL;
    constexpr DWORD kNonContinuable = 1UL;
    RaiseException(kCxxExceptionCode, kNonContinuable, 4, arguments);

    // RaiseException 只有在接管方把它标记为可继续时才返回；此时按 fail-fast 处理。
    __fastfail(7);
}
#endif

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    constexpr const char* kName = "NTE \xE4\xBC\xA4\xE5\xAE\xB3\xE4\xBB\xAA\xE8\xA1\xA8\xE7\x9B\x98";
    descriptor->api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    descriptor->api_minor = ANOMALY_PLUGIN_API_V1_MINOR;
    descriptor->id = {kPluginId, std::strlen(kPluginId)};
    descriptor->name = {kName, std::strlen(kName)};
    descriptor->author = {kPluginAuthor, std::strlen(kPluginAuthor)};
    descriptor->version = {kPluginVersion, std::strlen(kPluginVersion)};
    descriptor->on_load = &OnLoad;
    descriptor->on_start = &OnStart;
    descriptor->on_stop = &OnStop;
    descriptor->on_unload = &OnUnload;
    descriptor->on_update = &OnUpdate;
    descriptor->on_draw = &OnDraw;
    return {ANOMALY_STATUS_V1_OK, 0, {nullptr, 0}};
}
