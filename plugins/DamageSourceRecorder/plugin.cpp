// 伤害来源记录（Damage Source Recorder）—— Anomaly 插件
//
// 记录
//   `anomaly.nte.combat` 的伤害事件流（AHTAbilityCharacter::CharacterOnDamaged，
//   并由 FHTDamageTextInfo 补全）：伤害类型（EExecutionDamageTypeEX）、显示类型
//   （EDamageDisPlayType）、反应类型（EReactionResultType）、基础 / 最终 / 客户端展示数值、
//   暴击 / 头部 / 失衡标志、伤害来源（source_id → 技能名或 GameplayEffect 路径）与目标。
//
// 分段
//   同一来源、相邻两次命中间隔不超过「记录间隔」的连续命中合并为“一段伤害”。
//   每段记录来源、关联技能、命中次数、总伤害、类型分布与逐次命中明细。
//
// 重放
//   用 `anomaly.nte.skill-invocation` 的 `activate` 重新请求产生该段伤害的技能
//   （宿主只经已校验的 HTTryActivateAbilityByClass 入口激活，不向插件暴露 UFunction、
//   对象指针或 Profile 偏移）。伤害数值由游戏重新计算，插件不伪造任何数字。
//
// 线程域
//   on_update（Game 域）：消费事件流、推进 cursor、解析名称、调用技能激活。
//   on_draw（Render 域）：只在短锁内复制插件自己的不可变展示模型，锁外绘制 UI；
//   不查询 combat / skills / skill-invocation，也不推进 cursor。

#include "anomaly/sdk/cpp.hpp"

#include "damage_apply.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using anomaly::sdk::StringView;
using Clock = std::chrono::steady_clock;

// ABI v1 服务表按 struct_size 尾部扩展：访问每个字段前确认宿主实现到了它。
#define HAS_FIELD(service, type, field)                                           \
    ((service) != nullptr &&                                                      \
     (service)->struct_size >=                                                    \
         offsetof(type, field) + sizeof(decltype(type::field)) &&                 \
     (service)->field != nullptr)

constexpr std::size_t kMaxSegments = 32;
constexpr std::size_t kMaxHitsPerSegment = 64;
constexpr std::size_t kMaxDrainPerUpdate = 64;
constexpr std::size_t kSkillPageCapacity = 64;
constexpr std::size_t kMaxSkills = 128;
constexpr std::size_t kMaxCachedNames = 512;
constexpr std::size_t kMaxCachedIdentities = 256;
constexpr std::size_t kMaxReplayTargets = 8;
constexpr double kDamageBridgeRetrySeconds = 3.0;
constexpr float kMinGapSeconds = 0.3F;
constexpr float kMaxGapSeconds = 3.0F;
// 一次重放后继续观测伤害的窗口：技能激活到伤害结算可能跨几个 tick。
constexpr double kReplayWatchSeconds = 1.0;
// 技能失效后仍把它当作“刚放过的技能”的宽限时间：伤害事件通常晚于激活几个 tick。
constexpr double kActiveSkillHoldSeconds = 0.8;
constexpr std::uint32_t kDamageTypeCount = 15;  // EExecutionDamageTypeEX: 0..14
constexpr std::uint32_t kCondFirstUseEver = 4U;  // ImGuiCond_FirstUseEver
// 宿主只接受 AnomalyUiTableFlagsV1 里的位：未知位会让 begin_table 直接返回 0。
constexpr std::uint32_t kTableFlags = ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT;

// HTGame.EExecutionDamageTypeEX（见 sdk_dump HTGame_structs.hpp）。
constexpr std::array<const char*, kDamageTypeCount> kDamageTypeNames{
    "普通", "宇宙", "自然", "咒术", "混沌", "心灵", "业相", "精神",
    "真实", "Fons", "龙焰", "圣焰", "暗焰", "黑鸟", "未知"};

const char* TypeName(const std::uint32_t type) noexcept {
    return type < kDamageTypeCount ? kDamageTypeNames[type] : kDamageTypeNames.back();
}

const char* StatusName(const std::uint32_t code) noexcept {
    switch (code) {
    case ANOMALY_STATUS_V1_OK: return "OK";
    case ANOMALY_STATUS_V1_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "UNAVAILABLE";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "NOT_FOUND";
    case ANOMALY_STATUS_V1_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case ANOMALY_STATUS_V1_FAILED: return "FAILED";
    case ANOMALY_STATUS_V1_TIMEOUT: return "TIMEOUT";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ANOMALY_STATUS_V1_CONFLICT: return "CONFLICT";
    case ANOMALY_STATUS_V1_CANCELLED: return "CANCELLED";
    default: return "UNKNOWN";
    }
}

bool SameHandle(const AnomalyGenerationHandleV1 left,
                const AnomalyGenerationHandleV1 right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

// 字符串读取：先问长度，再读内容。返回去掉了结尾 NUL 的 UTF-8 文本。
template <typename Resolver>
std::string ReadNameText(Resolver&& resolver) {
    std::size_t size{};
    AnomalyStatusV1 status = resolver(nullptr, &size);
    if (status.code != ANOMALY_STATUS_V1_OK &&
        status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) {
        return {};
    }
    if (size < 2 || size > 1024) return {};
    std::string text(size, '\0');
    status = resolver(text.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size > text.size()) return {};
    text.resize(size);
    if (const auto end = text.find('\0'); end != std::string::npos) text.resize(end);
    return text;
}

// 对象路径只保留末段类名并去掉 Default__ / _C 这类生成器后缀，表格里更好读；
// 普通名称原样返回。
std::string ShortLabel(const std::string& text) {
    if (text.empty()) return text;
    std::string tail = text;
    if (const auto slash = text.find_last_of('/'); slash != std::string::npos) {
        tail = text.substr(slash + 1);
    }
    if (const auto dot = tail.find_last_of('.'); dot != std::string::npos && dot + 1 < tail.size()) {
        tail = tail.substr(dot + 1);
    }
    if (tail.rfind("Default__", 0) == 0) tail.erase(0, 9);
    if (tail.size() > 2 && tail.compare(tail.size() - 2, 2, "_C") == 0) {
        tail.resize(tail.size() - 2);
    }
    return tail.empty() ? text : tail;
}

std::string HexText(const std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

// 把伤害来源名（GameplayEffect）与技能类名归一成可比较的标识：
// 去掉路径、Default__、_C、GE_/GA_ 前缀和 _Damage 一类后缀。
// 例：Default__GE_Player_Akane_Melee4_Damage_C → Player_Akane_Melee4
//     /Game/…/GA_Player_Akane_Melee4.GA_Player_Akane_Melee4_C → 同一个标识
std::string SkillToken(const std::string& text) {
    std::string token = ShortLabel(text);
    constexpr std::string_view kPrefixes[] = {"GE_", "GA_", "Player_"};
    for (const auto prefix : kPrefixes) {
        if (std::string_view(token).rfind(prefix, 0) == 0) token.erase(0, prefix.size());
    }
    constexpr std::string_view kSuffixes[] = {"_DamageBase", "_Damage", "_Base", "_C"};
    for (const auto suffix : kSuffixes) {
        if (token.size() > suffix.size() &&
            std::string_view(token).compare(
                token.size() - suffix.size(), suffix.size(), suffix) == 0) {
            token.resize(token.size() - suffix.size());
        }
    }
    return token;
}

// ---------------------------------------------------------------------------
// 运行期状态
// ---------------------------------------------------------------------------

Clock::time_point g_started{};
const AnomalyHostApiV1* g_host{};
int g_state_token{};

double Now() noexcept {
    return std::chrono::duration<double>(Clock::now() - g_started).count();
}

struct Services {
    const AnomalyUiServiceV1* ui{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* invocation{};
    // 直接施加伤害用的底层服务（签名 / 对象注册表 / 名称）。
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalySignatureServiceV1* signature{};
};
Services g_services{};

damage_apply::Bridge g_damage_bridge;
double g_damage_bridge_next_try{};

std::atomic<bool> g_recording{true};
std::atomic<bool> g_include_incoming{false};
std::atomic<bool> g_include_other{false};
std::atomic<float> g_gap_seconds{1.0F};

struct SkillIdentity {
    std::string name;
    std::string path;
};

std::string AbilityToken(const SkillIdentity& identity) {
    return SkillToken(identity.path.empty() ? identity.name : identity.path);
}

struct CachedIdentity {
    SkillIdentity identity;
    std::uint64_t generation{};
    double next_try{};
};

// 记录时对“产生这段伤害的技能”的关联结果；重放时用来在当前技能表里找回它。
struct SkillRef {
    std::int32_t input_id{-1};
    std::uint64_t class_id{};
    std::uint64_t class_generation{};
    std::string name;
    std::string path;
    // 关联依据，如实显示给用户：来源名 / 来源名推断 / 唯一激活 / 最近激活。
    std::string basis;

    [[nodiscard]] bool HasHint() const noexcept {
        return class_id != 0 || input_id >= 0 || !name.empty() || !path.empty();
    }
};

struct HitRecord {
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t damage_type{};
    std::uint32_t display_type{};
    std::uint32_t reaction_type{};
    std::uint32_t flags{};
    std::int64_t display_damage{};
    std::int64_t basic_damage{};
    std::int64_t final_damage{};
    double at{};
    std::string victim;
};

struct SegmentRecord {
    std::uint32_t id{};
    std::uint64_t source_id{};
    std::string source_name;
    SkillRef skill;
    double start{};
    double last{};
    std::int64_t total_damage{};
    std::uint32_t hit_count{};
    std::uint32_t critical_count{};
    std::array<std::uint32_t, kDamageTypeCount> type_counts{};
    // 这一段是“角色受到的伤害”：重放时用记录到的攻击者当施加方、角色自己当目标。
    bool incoming{};
    AnomalyGenerationHandleV1 attacker{};
    // 受击目标（去重，最多 8 个）：直接重放伤害时对它们施加同一个 GE。
    std::vector<AnomalyGenerationHandleV1> victims;
    std::deque<HitRecord> hits;
};

struct ReplayState {
    bool watching{};
    bool incoming{};
    double watch_until{};
    std::uint32_t watch_hits{};
    std::int64_t watch_damage{};
    std::string message;
};

// 最近一次被观察到“激活/按下”的技能。伤害事件通常晚于技能激活若干 tick 才被消费，
// 名称解析又可能延后补全，所以记录时用它兜底关联。
struct ActiveMark {
    AnomalyGenerationHandleV1 ability_class{};
    std::int32_t input_id{-1};
    double at{};
};

struct Recorder {
    std::deque<SegmentRecord> segments;
    std::uint32_t next_id{1};
    std::uint32_t selected_id{};
    bool pinned{};
    std::uint64_t cursor{};
    bool cursor_ready{};
    std::uint64_t total_hits{};
    std::array<std::uint32_t, kDamageTypeCount> type_totals{};
    AnomalyGenerationHandleV1 world{};
    bool world_known{};
    ActiveMark active_skill;
    // 技能枚举复用缓冲：每 tick 都会读一次技能帧，避免反复分配。
    std::vector<AnomalyNteSkillSnapshotV1> skills;
    std::unordered_map<std::uint64_t, std::string> source_names;
    std::unordered_map<std::uint64_t, std::string> participant_names;
    std::unordered_map<std::uint64_t, CachedIdentity> identities;
    ReplayState replay;
};
Recorder g_recorder;

bool g_dirty{true};

struct UiModel;

// Render 域只读这一份不可变模型；Game 域每帧按需重建后整体替换。
std::mutex g_model_mutex;
std::shared_ptr<const UiModel> g_model;

// Render 域提交的用户意图，Game 域在下一 tick 取出执行。
struct Actions {
    bool refresh{};
    bool clear{};
    bool select{};
    std::uint32_t select_id{};
    bool replay{};
    std::uint32_t replay_id{};
};
std::mutex g_action_mutex;
Actions g_actions;

// ---------------------------------------------------------------------------
// 展示模型（插件自有数据，不跨 ABI）
// ---------------------------------------------------------------------------

struct SegmentView {
    std::uint32_t id{};
    std::string title;
    std::string skill;
    std::string type;
    std::string hits;
    std::string damage;
};

struct HitView {
    std::string time;
    std::string damage;
    std::string type;
    std::string detail;
};

struct UiModel {
    bool recording{};
    bool include_incoming{};
    bool include_other{};
    float gap_seconds{};
    bool invocation_ready{};
    bool damage_bridge_ready{};
    bool replay_watching{};
    bool has_selection{};
    bool selection_replayable{};
    std::uint32_t selected_id{};
    std::uint32_t total_hits{};
    std::uint32_t segment_count{};
    std::string service_line;
    std::string totals_line;
    std::string replay_status;
    std::string watch_line;
    std::string selection_meta;
    std::string selection_types;
    std::vector<SegmentView> segments;
    std::vector<HitView> hits;
};

// ---------------------------------------------------------------------------
// 服务可用性
// ---------------------------------------------------------------------------

void LogInfo(const std::string& text) {
    if (HAS_FIELD(g_services.core, AnomalyCoreServiceV1, log)) {
        g_services.core->log(
            g_services.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
    }
}

void EnsureServices(const AnomalyHostApiV1* host) {
    if (host == nullptr) return;
    const anomaly::sdk::Host view(host);
    if (g_services.ui == nullptr) {
        g_services.ui = view.Query<AnomalyUiServiceV1>(
            ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();
    }
    if (g_services.core == nullptr) {
        g_services.core = view.Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
    }
    // combat / skills / skill-invocation 只在进入世界、Profile 校验通过后才发布：
    // 查询为空时下一 tick 再试一次，非空就不重复查询。
    if (g_services.combat == nullptr) {
        g_services.combat = view.Query<AnomalyNteCombatServiceV1>(
            ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    }
    if (g_services.skills == nullptr) {
        g_services.skills = view.Query<AnomalyNteSkillsServiceV1>(
            ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
    }
    if (g_services.invocation == nullptr) {
        g_services.invocation = view.Query<AnomalyNteSkillInvocationServiceV1>(
            ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
            ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).get();
    }
    // 直接施加伤害需要的底层服务；缺少任何一个就退回到“按技能重放”。
    if (g_services.objects == nullptr) {
        g_services.objects = view.Query<AnomalyUe5ObjectsServiceV1>(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    }
    if (g_services.names == nullptr) {
        g_services.names = view.Query<AnomalyUe5NamesServiceV1>(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    }
    if (g_services.signature == nullptr) {
        g_services.signature = view.Query<AnomalySignatureServiceV1>(
            ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
    }
}

// 对象表 / 伤害结算函数只在第一次使用时解析，失败后每 3 秒重试一次。
bool DamageBridgeReady(const double now) {
    if (g_damage_bridge.ready) return true;
    if (now < g_damage_bridge_next_try) return false;
    g_damage_bridge_next_try = now + kDamageBridgeRetrySeconds;
    g_damage_bridge.core = g_services.core;
    g_damage_bridge.names = g_services.names;
    g_damage_bridge.objects = g_services.objects;
    g_damage_bridge.signature = g_services.signature;
    if (!damage_apply::Initialize(g_damage_bridge)) {
        LogInfo("伤害施加桥不可用：" + g_damage_bridge.status);
        return false;
    }
    LogInfo("伤害施加桥已就绪（GObjects / ProcessEvent / K2_ApplyGameplayEffectToTargetWithClass）");
    return true;
}

bool CombatReady() {
    const auto* combat = g_services.combat;
    return HAS_FIELD(combat, AnomalyNteCombatServiceV1, latest_damage_sequence) &&
        HAS_FIELD(combat, AnomalyNteCombatServiceV1, next_damage_event) &&
        HAS_FIELD(combat, AnomalyNteCombatServiceV1, current_combatant);
}

bool SkillsReady() {
    const auto* skills = g_services.skills;
    return HAS_FIELD(skills, AnomalyNteSkillsServiceV1, frame) &&
        HAS_FIELD(skills, AnomalyNteSkillsServiceV1, page);
}

bool InvocationReady() {
    return HAS_FIELD(g_services.invocation, AnomalyNteSkillInvocationServiceV1, activate);
}

// ---------------------------------------------------------------------------
// 技能表与名称缓存
// ---------------------------------------------------------------------------

bool EnumerateSkills(const AnomalyNteSkillFrameV1& frame,
                     std::vector<AnomalyNteSkillSnapshotV1>& destination) {
    const auto* skills = g_services.skills;
    destination.clear();
    if (!SkillsReady()) return false;
    std::array<AnomalyNteSkillSnapshotV1, kSkillPageCapacity> page_buffer{};
    std::uint32_t offset{};
    while (offset < frame.skill_count && destination.size() < kMaxSkills) {
        for (auto& slot : page_buffer) slot.struct_size = sizeof(slot);
        AnomalyNteSkillPageRequestV1 request{sizeof(request)};
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = static_cast<std::uint32_t>(page_buffer.size());
        AnomalyNteSkillPageResultV1 result{sizeof(result)};
        if (skills->page(skills->user, &request, page_buffer.data(), &result).code !=
            ANOMALY_STATUS_V1_OK) {
            destination.clear();
            return false;
        }
        if (result.generation != frame.generation ||
            result.returned > page_buffer.size()) {
            destination.clear();
            return false;
        }
        for (std::uint32_t index = 0; index < result.returned; ++index) {
            destination.push_back(page_buffer[index]);
        }
        if (result.next_offset <= offset) break;
        offset = result.next_offset;
    }
    return true;
}

// 技能类的显示名与路径：按类缓存；读取失败时 1 秒内不重复查询。
const SkillIdentity& IdentityFor(const AnomalyGenerationHandleV1 ability_class) {
    static const SkillIdentity kEmpty{};
    const auto* skills = g_services.skills;
    if (ability_class.id == 0 || !SkillsReady()) return kEmpty;
    const bool can_name = HAS_FIELD(skills, AnomalyNteSkillsServiceV1, ability_display_name_utf8);
    const bool can_path = HAS_FIELD(skills, AnomalyNteSkillsServiceV1, ability_path_utf8);
    if (!can_name && !can_path) return kEmpty;

    if (g_recorder.identities.size() > kMaxCachedIdentities) g_recorder.identities.clear();
    auto& entry = g_recorder.identities[ability_class.id];
    if (entry.generation != ability_class.generation) {
        entry = CachedIdentity{};
        entry.generation = ability_class.generation;
    }
    const double now = Now();
    if (now < entry.next_try) return entry.identity;

    if (can_name && entry.identity.name.empty()) {
        entry.identity.name = ReadNameText([&](char* text, std::size_t* size) {
            return skills->ability_display_name_utf8(
                skills->user, ability_class, text, size);
        });
    }
    if (can_path && entry.identity.path.empty()) {
        entry.identity.path = ReadNameText([&](char* text, std::size_t* size) {
            return skills->ability_path_utf8(skills->user, ability_class, text, size);
        });
    }
    entry.next_try = (entry.identity.name.empty() && entry.identity.path.empty())
        ? now + 1.0
        : now + 3600.0;
    return entry.identity;
}

// 每 tick 读一次技能帧，记住最近处于激活/按下的技能，供开段时兜底关联。
void TrackActiveSkill(const double now,
                      const bool combatant_valid,
                      const AnomalyNteCombatantSnapshotV1& combatant) {
    if (!combatant_valid || !SkillsReady()) return;
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (g_services.skills->frame(g_services.skills->user, &frame).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }
    if ((frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0 || frame.generation == 0 ||
        !SameHandle(frame.character, combatant.character)) {
        return;
    }
    if (!EnumerateSkills(frame, g_recorder.skills)) return;

    const AnomalyNteSkillSnapshotV1* chosen = nullptr;
    const AnomalyNteSkillSnapshotV1* first_active = nullptr;
    for (const auto& skill : g_recorder.skills) {
        if ((skill.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) continue;
        if ((skill.flags & ANOMALY_NTE_SKILL_V1_PENDING_REMOVE) != 0) continue;
        if ((skill.flags & (ANOMALY_NTE_SKILL_V1_ACTIVE |
                            ANOMALY_NTE_SKILL_V1_INPUT_PRESSED)) == 0) {
            continue;
        }
        if (first_active == nullptr) first_active = &skill;
        // 已经在跟踪的那个技能仍然激活时保持不换，避免多技能同时激活时来回跳。
        if (SameHandle(skill.ability_class, g_recorder.active_skill.ability_class)) {
            chosen = &skill;
            break;
        }
    }
    if (chosen == nullptr) chosen = first_active;
    if (chosen == nullptr) return;
    g_recorder.active_skill.ability_class = chosen->ability_class;
    g_recorder.active_skill.input_id = chosen->input_id;
    g_recorder.active_skill.at = now;
}

SkillRef MakeSkillRef(const AnomalyNteSkillSnapshotV1& skill,
                      const SkillIdentity& identity, const char* basis) {
    SkillRef ref;
    ref.input_id = skill.input_id;
    ref.class_id = skill.ability_class.id;
    ref.class_generation = skill.ability_class.generation;
    ref.name = identity.name;
    ref.path = identity.path;
    ref.basis = basis;
    return ref;
}

// 记录一段新伤害时，尽力把伤害来源名称对回当前技能表里的技能。
SkillRef ResolveSkillForSource(const std::string& source_name,
                               const AnomalyNteCombatantSnapshotV1& combatant) {
    SkillRef result;
    if (!SkillsReady()) return result;
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (g_services.skills->frame(g_services.skills->user, &frame).code !=
        ANOMALY_STATUS_V1_OK) {
        return result;
    }
    if ((frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0 || frame.generation == 0) {
        return result;
    }
    if (!SameHandle(frame.character, combatant.character)) return result;
    if (!EnumerateSkills(frame, g_recorder.skills)) return result;

    SkillRef active;
    std::uint32_t active_count{};
    const std::string source_token = source_name.empty() ? std::string{} : SkillToken(source_name);
    for (const auto& skill : g_recorder.skills) {
        if ((skill.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) continue;
        if ((skill.flags & ANOMALY_NTE_SKILL_V1_PENDING_REMOVE) != 0) continue;
        const SkillIdentity& identity = IdentityFor(skill.ability_class);
        // 宿主把伤害来源名称解析成的就是技能显示名，两者相等时可以直接锁定技能。
        if (!source_name.empty() && !identity.name.empty() &&
            identity.name == source_name) {
            return MakeSkillRef(skill, identity, "伤害来源");
        }
        // 名称解析还没就绪时，用归一化后的来源名/技能类名比较（GE_X_Damage ↔ GA_X）。
        if (!source_token.empty() && AbilityToken(identity) == source_token) {
            return MakeSkillRef(skill, identity, "来源名推断");
        }
        if ((skill.flags & (ANOMALY_NTE_SKILL_V1_ACTIVE |
                            ANOMALY_NTE_SKILL_V1_INPUT_PRESSED)) != 0) {
            ++active_count;
            active = MakeSkillRef(skill, identity, "唯一激活");
        }
    }
    // 没有名称匹配时，只有“恰好一个技能处于激活/按下”才敢认定它是来源。
    if (active_count == 1) return active;

    // 最后兜底：伤害事件通常晚于技能激活几个 tick 才被消费，用最近跟踪到的技能。
    // 这一层只是“最近放过什么”，不保证就是这段伤害的来源，界面会标注出来。
    const ActiveMark& mark = g_recorder.active_skill;
    if (mark.ability_class.id == 0 || Now() - mark.at > kActiveSkillHoldSeconds) {
        return result;
    }
    for (const auto& skill : g_recorder.skills) {
        if (SameHandle(skill.ability_class, mark.ability_class)) {
            return MakeSkillRef(skill, IdentityFor(skill.ability_class), "最近激活");
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// 名称解析（带缓存）
// ---------------------------------------------------------------------------

std::string SourceName(const std::uint64_t source_id) {
    if (source_id == 0) return {};
    if (const auto cached = g_recorder.source_names.find(source_id);
        cached != g_recorder.source_names.end()) {
        return cached->second;
    }
    const auto* combat = g_services.combat;
    if (!HAS_FIELD(combat, AnomalyNteCombatServiceV1, source_name_utf8)) return {};
    std::string text = ReadNameText([&](char* destination, std::size_t* size) -> AnomalyStatusV1 {
        return combat->source_name_utf8(combat->user, source_id, destination, size);
    });
    // 名称由 Game 更新延后补全：失败结果不缓存，下次命中再试。
    if (text.empty()) return {};
    if (g_recorder.source_names.size() >= kMaxCachedNames) g_recorder.source_names.clear();
    g_recorder.source_names.emplace(source_id, text);
    return text;
}

std::string ParticipantName(const AnomalyGenerationHandleV1 handle) {
    if (handle.id == 0) return {};
    const auto* combat = g_services.combat;
    if (!HAS_FIELD(combat, AnomalyNteCombatServiceV1, participant_path_utf8)) return {};

    const std::uint64_t key = handle.id ^ (handle.generation * 0x9E3779B97F4A7C15ULL);
    if (const auto cached = g_recorder.participant_names.find(key);
        cached != g_recorder.participant_names.end()) {
        return cached->second;
    }
    std::string text;
    if (HAS_FIELD(combat, AnomalyNteCombatServiceV1, participant_display_name_utf8)) {
        text = ReadNameText([&](char* destination, std::size_t* size) -> AnomalyStatusV1 {
            return combat->participant_display_name_utf8(
                combat->user, handle, destination, size);
        });
    }
    if (text.empty()) {
        text = ReadNameText([&](char* destination, std::size_t* size) -> AnomalyStatusV1 {
            return combat->participant_path_utf8(
                combat->user, handle, destination, size);
        });
    }
    if (text.empty()) return {};
    if (g_recorder.participant_names.size() >= kMaxCachedNames) {
        g_recorder.participant_names.clear();
    }
    g_recorder.participant_names.emplace(key, text);
    return text;
}

// ---------------------------------------------------------------------------
// 记录
// ---------------------------------------------------------------------------

SegmentRecord* FindSegment(const std::uint32_t id) {
    if (id == 0) return nullptr;
    for (auto& segment : g_recorder.segments) {
        if (segment.id == id) return &segment;
    }
    return nullptr;
}

void AcceptDamage(const double now,
                  const AnomalyNteDamageEventV1& event,
                  const AnomalyNteCombatantSnapshotV1& combatant) {
    const bool outgoing = SameHandle(event.attacker, combatant.character);
    const bool incoming = SameHandle(event.victim, combatant.character);

    // 重放观测：只统计与正在重放的那一段方向相同的命中（出伤 / 受伤各自核对）。
    if (g_recorder.replay.watching && incoming == g_recorder.replay.incoming) {
        ++g_recorder.replay.watch_hits;
        g_recorder.replay.watch_damage += event.final_damage;
        g_dirty = true;
    }

    if (incoming) {
        if (!g_include_incoming.load(std::memory_order_relaxed)) return;
    } else if (!outgoing && !g_include_other.load(std::memory_order_relaxed)) {
        // 攻击者不是本地角色（召唤物、联动效果等）时默认不记；需要时单独打开。
        return;
    }
    if (!g_recording.load(std::memory_order_relaxed)) return;

    const std::string source_name = SourceName(event.source_id);
    SegmentRecord* segment = nullptr;
    if (!g_recorder.segments.empty()) {
        auto& back = g_recorder.segments.back();
        const bool same_source = back.source_id == event.source_id;
        const bool gap_ok =
            (now - back.last) <= static_cast<double>(g_gap_seconds.load(std::memory_order_relaxed));
        if (same_source && gap_ok) segment = &back;
    }
    if (segment == nullptr) {
        SegmentRecord record;
        record.id = g_recorder.next_id++;
        record.source_id = event.source_id;
        record.source_name = source_name;
        record.start = now;
        record.last = now;
        record.incoming = incoming;
        record.attacker = event.attacker;
        record.skill = ResolveSkillForSource(source_name, combatant);
        g_recorder.segments.push_back(std::move(record));
        if (g_recorder.segments.size() > kMaxSegments) {
            const std::uint32_t dropped = g_recorder.segments.front().id;
            g_recorder.segments.pop_front();
            if (g_recorder.selected_id == dropped) g_recorder.selected_id = 0;
        }
        segment = &g_recorder.segments.back();
        // 没有手动选中旧片段时，自动跟随最新的一段。
        if (!g_recorder.pinned) g_recorder.selected_id = segment->id;
    }
    HitRecord hit;
    hit.sequence = event.sequence;
    hit.tick_sequence = event.tick_sequence;
    hit.damage_type =
        event.damage_type < kDamageTypeCount ? event.damage_type : 0;
    hit.display_type = event.display_type;
    hit.reaction_type = event.reaction_type;
    hit.flags = event.flags;
    hit.display_damage = event.display_damage;
    hit.basic_damage = event.basic_damage;
    hit.final_damage = event.final_damage;
    hit.at = now;
    hit.victim = ParticipantName(event.victim);

    segment->last = now;
    segment->total_damage += event.final_damage;
    ++segment->hit_count;
    ++g_recorder.total_hits;
    if ((event.flags & ANOMALY_NTE_DAMAGE_V1_CRITICAL) != 0) ++segment->critical_count;
    ++segment->type_counts[hit.damage_type];
    ++g_recorder.type_totals[hit.damage_type];
    if (segment->hits.size() >= kMaxHitsPerSegment) segment->hits.pop_front();
    segment->hits.push_back(std::move(hit));
    if (segment->source_name.empty() && !source_name.empty()) {
        segment->source_name = source_name;
    }
    // 记下受击目标：重放时对它们施加同一个伤害效果，而不是重新施放技能。
    // 受伤片段的目标就是角色自己，出伤片段的目标是怪物，两种都要留下。
    if (event.victim.id != 0 &&
        segment->victims.size() < kMaxReplayTargets &&
        std::none_of(segment->victims.begin(), segment->victims.end(),
            [&](const AnomalyGenerationHandleV1 handle) {
                return SameHandle(handle, event.victim);
            })) {
        segment->victims.push_back(event.victim);
    }
    // 技能关联只在开段时解析一次：名称与技能表都可能延后补全，命中路径上不重复解析。
    g_dirty = true;
}

void DrainDamage(const double now,
                 const bool combatant_valid,
                 const AnomalyNteCombatantSnapshotV1& combatant) {
    const auto* combat = g_services.combat;
    if (!CombatReady()) return;
    const std::uint64_t latest = combat->latest_damage_sequence(combat->user);
    if (!g_recorder.cursor_ready) {
        // 从加载这一刻开始记录：跳过宿主保留窗口里已经存在的旧命中。
        g_recorder.cursor_ready = true;
        g_recorder.cursor = latest;
        return;
    }
    if (latest == g_recorder.cursor) return;
    bool rebased = false;
    for (std::size_t drained = 0; drained < kMaxDrainPerUpdate;) {
        AnomalyNteDamageEventV1 event{sizeof(event)};
        const AnomalyStatusV1 status =
            combat->next_damage_event(combat->user, g_recorder.cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            if (!rebased && status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                g_recorder.cursor != 0 && latest > g_recorder.cursor) {
                // 生产者的 ring 已经越过本消费端：只接最新一条，避免把保留窗口里的
                // 旧伤害整批灌进记录。
                g_recorder.cursor = latest - 1U;
                rebased = true;
                continue;
            }
            break;
        }
        g_recorder.cursor = event.sequence;
        ++drained;
        if (combatant_valid) AcceptDamage(now, event, combatant);
    }
}

// ---------------------------------------------------------------------------
// 重放
// ---------------------------------------------------------------------------

struct ReplayMatch {
    AnomalyGenerationHandleV1 skill{};
    std::string label;
    std::string basis;
    int score{};
};

bool ResolveReplaySkill(const SegmentRecord& segment,
                        const AnomalyNteCombatantSnapshotV1& combatant,
                        ReplayMatch& match,
                        std::string& reason) {
    if (!SkillsReady()) {
        reason = "技能服务不可用（需要进入世界）";
        return false;
    }
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (g_services.skills->frame(g_services.skills->user, &frame).code !=
        ANOMALY_STATUS_V1_OK) {
        reason = "技能快照不可读";
        return false;
    }
    if ((frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0 || frame.generation == 0) {
        reason = "技能快照尚未就绪";
        return false;
    }
    if (!SameHandle(frame.character, combatant.character)) {
        reason = "技能快照与当前角色不一致";
        return false;
    }
    if (!EnumerateSkills(frame, g_recorder.skills)) {
        reason = "技能列表读取失败";
        return false;
    }

    const bool has_path = !segment.skill.path.empty();
    const bool has_name = !segment.skill.name.empty() || !segment.source_name.empty();
    const std::string source_token =
        segment.source_name.empty() ? std::string{} : SkillToken(segment.source_name);
    ReplayMatch best;
    for (const auto& skill : g_recorder.skills) {
        if ((skill.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) continue;
        if ((skill.flags & ANOMALY_NTE_SKILL_V1_PENDING_REMOVE) != 0) continue;
        const SkillIdentity& identity = IdentityFor(skill.ability_class);
        int score{};
        const char* basis{};
        if (has_path && !identity.path.empty() && identity.path == segment.skill.path) {
            score = 100;
            basis = "技能路径";
        } else if (!segment.source_name.empty() && !identity.name.empty() &&
                   identity.name == segment.source_name) {
            score = 80;
            basis = "伤害来源";
        } else if (!source_token.empty() && AbilityToken(identity) == source_token) {
            score = 70;
            basis = "来源名推断";
        } else if (!segment.skill.name.empty() && !identity.name.empty() &&
                   identity.name == segment.skill.name) {
            score = 60;
            basis = "技能名称";
        } else if (!has_path && !has_name && segment.skill.input_id >= 0 &&
                   skill.input_id == segment.skill.input_id) {
            // 只记录到输入 ID 时按输入 ID 匹配：可能不是同一段，如实报告依据。
            score = 20;
            basis = "输入 ID";
        }
        if (score <= best.score) continue;
        best.score = score;
        best.skill = skill.handle;
        best.basis = basis;
        best.label = !identity.name.empty()
            ? ShortLabel(identity.name)
            : (!identity.path.empty() ? ShortLabel(identity.path)
                                      : "技能#" + std::to_string(skill.handle.id));
    }
    if (best.score == 0) {
        reason = "当前角色没有与记录匹配的技能（换角色或技能未授予时无法重放）";
        return false;
    }
    match = best;
    return true;
}

// 重放失败：清掉上一次的观测状态，只留下原因。
void FailReplay(std::string message) {
    g_recorder.replay = ReplayState{};
    g_recorder.replay.message = std::move(message);
    LogInfo(g_recorder.replay.message);
    g_dirty = true;
}

// 直接重放：把记录到的那一个伤害效果（GameplayEffect）重新施加到记录中的目标身上。
// 这是 b1anktool 自动攻击用的同一条路径：同一 GE、同一目标，数值由游戏重新结算。
bool TryApplyRecordedDamage(const SegmentRecord& segment,
                            const AnomalyNteCombatantSnapshotV1& combatant,
                            const double now,
                            std::string& reason) {
    if (!DamageBridgeReady(now)) {
        reason = g_damage_bridge.status.empty() ? "伤害施加桥未就绪" : g_damage_bridge.status;
        return false;
    }
    std::uintptr_t effect_class{};
    if (!damage_apply::ResolveEffectClass(g_damage_bridge, segment.source_name, effect_class,
            reason)) {
        return false;
    }

    // 施加方：优先用记录到的攻击者（受伤片段靠它复现怪物的那一击）；
    // 它不在了时，出伤片段退回当前角色，受伤片段则无法用别人顶替。
    std::uintptr_t source_character{};
    if (!damage_apply::ResolveHandle(g_damage_bridge, segment.attacker, source_character)) {
        if (segment.incoming) {
            reason = "记录的攻击者已不在了，无法复现这一段受到的伤害";
            return false;
        }
        if (!damage_apply::ResolveHandle(g_damage_bridge, combatant.character,
                source_character)) {
            reason = "本地角色地址不可用";
            return false;
        }
    }

    // 记录到的目标优先；出伤片段都消失时退到当前攻击目标，受伤片段退到当前角色。
    std::vector<std::uintptr_t> targets;
    for (const auto& victim : segment.victims) {
        std::uintptr_t address{};
        if (damage_apply::ResolveHandle(g_damage_bridge, victim, address) && address != 0) {
            targets.push_back(address);
        }
        if (targets.size() >= kMaxReplayTargets) break;
    }
    if (targets.empty()) {
        const AnomalyGenerationHandleV1 fallback =
            segment.incoming ? combatant.character : combatant.target;
        std::uintptr_t address{};
        if (fallback.id != 0 &&
            damage_apply::ResolveHandle(g_damage_bridge, fallback, address) &&
            address != 0) {
            targets.push_back(address);
        }
    }
    if (targets.empty()) {
        reason = segment.incoming ? "记录的角色已不在，无法施加" : "记录的目标都已消失，当前也没有攻击目标";
        return false;
    }

    std::uint32_t applied{};
    std::int32_t last_result{};
    std::string last_error;
    for (const auto address : targets) {
        // 跳过已死亡的目标：对尸体施加即时伤害没有意义。
        float hp{};
        if (damage_apply::Health(g_damage_bridge, address, hp) && hp <= 0.0F) continue;
        std::int32_t result{};
        std::string error;
        if (damage_apply::Apply(g_damage_bridge, source_character, address, effect_class,
                result, error)) {
            ++applied;
            last_result = result;
        } else {
            last_error = std::move(error);
        }
    }
    if (applied == 0) {
        reason = last_error.empty() ? "目标都已死亡" : last_error;
        return false;
    }

    ReplayState state;
    state.watching = true;
    state.incoming = segment.incoming;
    state.watch_until = now + kReplayWatchSeconds;
    char text[384];
    std::snprintf(text, sizeof(text), "已对 %u 个目标重新施加 %s · 效果句柄 %d",
                  applied, ShortLabel(segment.source_name).c_str(), last_result);
    state.message = text;
    g_recorder.replay = std::move(state);
    LogInfo(g_recorder.replay.message);
    g_dirty = true;
    return true;
}

// 点一次重放一次：优先直接施加记录到的伤害效果，不行再退回到激活技能。
void ReplaySelected(const std::uint32_t segment_id,
                    const double now,
                    const bool combatant_valid,
                    const AnomalyNteCombatantSnapshotV1& combatant) {
    ReplayState state;
    const SegmentRecord* segment = FindSegment(segment_id);
    if (segment == nullptr) {
        FailReplay("重放失败：片段不在记录中（可能已被挤出缓存）");
        return;
    }
    g_recorder.selected_id = segment_id;
    g_recorder.pinned = true;
    if (!combatant_valid) {
        FailReplay("重放失败：等待角色战斗快照");
        return;
    }

    // 1) 直接施加记录到的伤害效果：数值与原来最接近，也不受技能冷却影响。
    std::string direct_reason;
    if (TryApplyRecordedDamage(*segment, combatant, now, direct_reason)) return;

    // 2) 退回技能激活：只对“角色打出去”的片段有意义。
    //    受伤片段是怪物的技能打的，用角色的技能顶替只会打出另一段伤害。
    if (segment->incoming) {
        FailReplay("重放失败：" + direct_reason);
        return;
    }
    if (!InvocationReady()) {
        FailReplay("重放失败：" + direct_reason + "；技能激活服务也不可用");
        return;
    }

    ReplayMatch match;
    std::string reason;
    if (!ResolveReplaySkill(*segment, combatant, match, reason)) {
        FailReplay("重放失败：" + direct_reason + "；按技能重放也不可用：" + reason);
        return;
    }

    AnomalyNteSkillInvocationRequestV1 request{sizeof(request)};
    request.world = combatant.world;
    request.character = combatant.character;
    request.skill = match.skill;
    AnomalyNteSkillInvocationResultV1 result{sizeof(result)};
    const AnomalyStatusV1 status = g_services.invocation->activate(
        g_services.invocation->user, &request, &result);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        FailReplay(std::string("重放调用失败：") + StatusName(status.code));
        return;
    }

    state.watching = true;
    state.watch_until = now + kReplayWatchSeconds;
    char text[512];
    std::snprintf(text, sizeof(text),
                  "按技能重放 %s（%s）· 游戏接受=%s%s · 直接施加不可用：%s",
                  match.label.c_str(), match.basis.c_str(),
                  result.accepted != 0 ? "是" : "否",
                  result.accepted != 0 ? "" : "（技能仍在动作中，可再点一次）",
                  direct_reason.c_str());
    state.message = text;
    g_recorder.replay = std::move(state);
    LogInfo(g_recorder.replay.message);
    g_dirty = true;
}

// 观测窗口结束时汇报这次重放实际打出的伤害。
void TickReplayWatch(const double now) {
    ReplayState& replay = g_recorder.replay;
    if (!replay.watching || now < replay.watch_until) return;
    replay.watching = false;
    replay.message += " · 观测到 ";
    replay.message += std::to_string(replay.watch_hits);
    replay.message += " 次命中、伤害 ";
    replay.message += std::to_string(static_cast<long long>(replay.watch_damage));
    LogInfo(replay.message);
    g_dirty = true;
}

// ---------------------------------------------------------------------------
// 用户意图与配置
// ---------------------------------------------------------------------------

void QueueRefresh() {
    std::lock_guard<std::mutex> lock(g_action_mutex);
    g_actions.refresh = true;
}

void QueueClear() {
    std::lock_guard<std::mutex> lock(g_action_mutex);
    g_actions.clear = true;
}

void QueueSelect(const std::uint32_t id) {
    std::lock_guard<std::mutex> lock(g_action_mutex);
    g_actions.select = true;
    g_actions.select_id = id;
}

void QueueReplay(const std::uint32_t id) {
    std::lock_guard<std::mutex> lock(g_action_mutex);
    g_actions.replay = true;
    g_actions.replay_id = id;
}

void ConsumeActions(const double now,
                    const bool combatant_valid,
                    const AnomalyNteCombatantSnapshotV1& combatant) {
    Actions actions;
    {
        std::lock_guard<std::mutex> lock(g_action_mutex);
        actions = std::move(g_actions);
        g_actions = Actions{};
    }
    if (actions.clear) {
        g_recorder.segments.clear();
        g_recorder.selected_id = 0;
        g_recorder.pinned = false;
        g_recorder.total_hits = 0;
        g_recorder.type_totals.fill(0);
        g_recorder.next_id = 1;
        g_recorder.replay = ReplayState{};
        g_dirty = true;
    }
    if (actions.select) {
        g_recorder.selected_id = actions.select_id;
        g_recorder.pinned = true;
        g_dirty = true;
    }
    if (actions.replay) ReplaySelected(actions.replay_id, now, combatant_valid, combatant);
    if (actions.refresh) g_dirty = true;
}

// ---------------------------------------------------------------------------
// 展示模型构建
// ---------------------------------------------------------------------------

std::uint32_t DominantType(const SegmentRecord& segment) {
    std::uint32_t best{};
    std::uint32_t best_count{};
    for (std::uint32_t index = 0; index < kDamageTypeCount; ++index) {
        if (segment.type_counts[index] > best_count) {
            best_count = segment.type_counts[index];
            best = index;
        }
    }
    return best;
}

std::string TypeBreakdown(const SegmentRecord& segment) {
    std::string text;
    for (std::uint32_t index = 0; index < kDamageTypeCount; ++index) {
        const std::uint32_t count = segment.type_counts[index];
        if (count == 0) continue;
        if (!text.empty()) text += "  ";
        text += TypeName(index);
        text += "×";
        text += std::to_string(count);
    }
    return text.empty() ? std::string("无") : text;
}

std::string TypeTotals(const std::array<std::uint32_t, kDamageTypeCount>& totals) {
    std::string text;
    for (std::uint32_t index = 0; index < kDamageTypeCount; ++index) {
        if (totals[index] == 0) continue;
        if (!text.empty()) text += " · ";
        text += TypeName(index);
        text += " ";
        text += std::to_string(totals[index]);
    }
    return text;
}

std::string HitFlags(const HitRecord& hit) {
    std::string text;
    const auto append = [&text](const char* value) {
        if (!text.empty()) text += " ";
        text += value;
    };
    if ((hit.flags & ANOMALY_NTE_DAMAGE_V1_CRITICAL) != 0) append("暴击");
    if ((hit.flags & ANOMALY_NTE_DAMAGE_V1_HEAD_HIT) != 0) append("头部");
    if ((hit.flags & ANOMALY_NTE_DAMAGE_V1_WEAK_UNBALANCE) != 0) append("失衡");
    if (text.empty()) text = "-";
    if (!hit.victim.empty()) {
        text += " · ";
        text += ShortLabel(hit.victim);
    }
    return text;
}

void PublishModel(const bool combatant_valid) {
    auto model = std::make_shared<UiModel>();
    model->recording = g_recording.load(std::memory_order_relaxed);
    model->include_incoming = g_include_incoming.load(std::memory_order_relaxed);
    model->include_other = g_include_other.load(std::memory_order_relaxed);
    model->gap_seconds = g_gap_seconds.load(std::memory_order_relaxed);
    model->invocation_ready = InvocationReady();
    model->damage_bridge_ready = g_damage_bridge.ready;
    model->replay_watching = g_recorder.replay.watching;
    model->selected_id = g_recorder.selected_id;
    model->total_hits = static_cast<std::uint32_t>(g_recorder.total_hits);
    model->segment_count = static_cast<std::uint32_t>(g_recorder.segments.size());
    model->replay_status = g_recorder.replay.message;

    {
        char line[384];
        std::snprintf(line, sizeof(line),
                      "战斗 %s · 技能 %s · 施伤 %s · 角色 %s · 记录%s · 片段 %u · 命中 %u",
                      CombatReady() ? "可用" : "不可用",
                      SkillsReady() ? "可用" : "不可用",
                      g_damage_bridge.ready ? "可用" : "不可用",
                      combatant_valid ? "就绪" : "等待中",
                      model->recording ? "中" : "暂停",
                      model->segment_count, model->total_hits);
        model->service_line = line;
    }
    if (const std::string totals = TypeTotals(g_recorder.type_totals); !totals.empty()) {
        model->totals_line = "类型总计：" + totals;
    }
    if (g_recorder.replay.watching) {
        char line[192];
        std::snprintf(line, sizeof(line), "观测中：%u 次命中 / 伤害 %lld",
                      g_recorder.replay.watch_hits,
                      static_cast<long long>(g_recorder.replay.watch_damage));
        model->watch_line = line;
    }

    // 新记录在最上面。
    for (auto iterator = g_recorder.segments.rbegin();
         iterator != g_recorder.segments.rend(); ++iterator) {
        const SegmentRecord& segment = *iterator;
        SegmentView view;
        view.id = segment.id;
        char title[64];
        const unsigned total_seconds = static_cast<unsigned>(segment.start);
        std::snprintf(title, sizeof(title), "#%u  %u:%02u", segment.id,
                      total_seconds / 60U, total_seconds % 60U);
        view.title = title;
        if (!segment.skill.name.empty()) {
            view.skill = ShortLabel(segment.skill.name);
        } else if (!segment.skill.path.empty()) {
            view.skill = ShortLabel(segment.skill.path);
        } else if (!segment.source_name.empty()) {
            view.skill = ShortLabel(segment.source_name);
        } else {
            view.skill = "未识别";
        }
        // 受伤片段在列表里加个前缀，免得和同类来源的出伤记录混在一起。
        if (segment.incoming) view.skill = "受击 · " + view.skill;
        view.type = TypeName(DominantType(segment));
        view.hits = std::to_string(segment.hit_count);
        view.damage = std::to_string(static_cast<long long>(segment.total_damage));
        model->segments.push_back(std::move(view));
    }

    if (const SegmentRecord* selected = FindSegment(g_recorder.selected_id);
        selected != nullptr) {
        model->has_selection = true;
        model->selection_replayable =
            selected->skill.HasHint() || !selected->source_name.empty();
        std::string meta;
        meta += "方向：";
        meta += selected->incoming ? "角色受到" : "角色造成";
        meta += "\n技能：";
        if (!selected->skill.name.empty()) {
            meta += ShortLabel(selected->skill.name);
        } else if (!selected->skill.path.empty()) {
            meta += ShortLabel(selected->skill.path);
        } else {
            meta += "未识别";
        }
        if (!selected->skill.basis.empty()) {
            meta += "（";
            meta += selected->skill.basis;
            meta += "）";
        }
        if (selected->skill.input_id >= 0) {
            meta += " 输入 ID ";
            meta += std::to_string(selected->skill.input_id);
        }
        meta += "\n路径：";
        meta += selected->skill.path.empty() ? "-" : ShortLabel(selected->skill.path);
        meta += "\n来源：";
        meta += selected->source_name.empty() ? "-" : ShortLabel(selected->source_name);
        meta += "\n来源 ID：";
        meta += HexText(selected->source_id);
        meta += " · 目标 ";
        meta += std::to_string(selected->victims.size());
        meta += " 个";
        meta += "\n命中 ";
        meta += std::to_string(selected->hit_count);
        meta += " 次 · 暴击 ";
        meta += std::to_string(selected->critical_count);
        meta += " 次 · 总伤害 ";
        meta += std::to_string(static_cast<long long>(selected->total_damage));
        model->selection_meta = meta;
        model->selection_types = "类型分布：" + TypeBreakdown(*selected);

        for (const HitRecord& hit : selected->hits) {
            HitView row;
            char time[32];
            std::snprintf(time, sizeof(time), "+%.2fs", hit.at - selected->start);
            row.time = time;
            char damage[96];
            if (hit.basic_damage != 0 && hit.basic_damage != hit.final_damage) {
                std::snprintf(damage, sizeof(damage), "%lld（基础 %lld）",
                              static_cast<long long>(hit.final_damage),
                              static_cast<long long>(hit.basic_damage));
            } else {
                std::snprintf(damage, sizeof(damage), "%lld",
                              static_cast<long long>(hit.final_damage));
            }
            row.damage = damage;
            row.type = TypeName(hit.damage_type);
            row.detail = HitFlags(hit);
            model->hits.push_back(std::move(row));
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_model_mutex);
        g_model = std::move(model);
    }
    g_dirty = false;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void UiText(const AnomalyUiServiceV1* ui, const char* text) {
    if (HAS_FIELD(ui, AnomalyUiServiceV1, text)) ui->text(ui->user, StringView(text));
}

void UiTextWrapped(const AnomalyUiServiceV1* ui, const char* text) {
    if (HAS_FIELD(ui, AnomalyUiServiceV1, text_wrapped)) {
        ui->text_wrapped(ui->user, StringView(text));
        return;
    }
    UiText(ui, text);
}

void UiSameLine(const AnomalyUiServiceV1* ui, float spacing) {
    if (HAS_FIELD(ui, AnomalyUiServiceV1, same_line)) {
        ui->same_line(ui->user, 0.0F, spacing);
    }
}

void UiSeparator(const AnomalyUiServiceV1* ui) {
    if (HAS_FIELD(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);
}

bool UiButton(const AnomalyUiServiceV1* ui, const char* label, const bool enabled) {
    if (HAS_FIELD(ui, AnomalyUiServiceV1, button_enabled)) {
        return ui->button_enabled(ui->user, StringView(label), 0.0F, 0.0F,
                                  enabled ? 1 : 0) != 0;
    }
    return enabled && HAS_FIELD(ui, AnomalyUiServiceV1, button) &&
        ui->button(ui->user, StringView(label), 0.0F, 0.0F) != 0;
}

void DrawSegmentTable(const AnomalyUiServiceV1* ui, const UiModel& model) {
    if (model.segments.empty()) {
        UiText(ui, "还没有记录。进入战斗后，角色造成的伤害会自动出现在这里。");
        return;
    }
    const bool can_table = HAS_FIELD(ui, AnomalyUiServiceV1, begin_table) &&
        HAS_FIELD(ui, AnomalyUiServiceV1, table_next_row) &&
        HAS_FIELD(ui, AnomalyUiServiceV1, table_next_column) &&
        HAS_FIELD(ui, AnomalyUiServiceV1, end_table);
    if (!can_table) {
        for (const SegmentView& row : model.segments) {
            std::string line = row.title + "  " + row.skill + "  " + row.type + "  " +
                row.hits + " 段 / " + row.damage;
            UiText(ui, line.c_str());
        }
        return;
    }
    if (!ui->begin_table(ui->user, StringView("##dsr-segments"), 6, kTableFlags,
                         0.0F, 190.0F)) {
        return;
    }
    ui->table_next_row(ui->user);
    ui->table_next_column(ui->user);
    UiText(ui, "序号");
    ui->table_next_column(ui->user);
    UiText(ui, "技能 / 来源");
    ui->table_next_column(ui->user);
    UiText(ui, "类型");
    ui->table_next_column(ui->user);
    UiText(ui, "段数");
    ui->table_next_column(ui->user);
    UiText(ui, "总伤害");
    ui->table_next_column(ui->user);
    UiText(ui, "操作");

    for (const SegmentView& row : model.segments) {
        ui->table_next_row(ui->user);
        ui->table_next_column(ui->user);
        UiText(ui, row.title.c_str());
        ui->table_next_column(ui->user);
        UiText(ui, row.skill.c_str());
        ui->table_next_column(ui->user);
        UiText(ui, row.type.c_str());
        ui->table_next_column(ui->user);
        UiText(ui, row.hits.c_str());
        ui->table_next_column(ui->user);
        UiText(ui, row.damage.c_str());
        ui->table_next_column(ui->user);
        char label[64];
        std::snprintf(label, sizeof(label), "选择##dsr-select-%u", row.id);
        const bool selected = row.id == model.selected_id;
        if (UiButton(ui, label, !selected)) QueueSelect(row.id);
    }
    ui->end_table(ui->user);
}

void DrawDetail(const AnomalyUiServiceV1* ui, const UiModel& model) {
    if (!model.has_selection) {
        UiText(ui, "在下方记录里点「选择」，这里显示该段的逐次命中。");
        return;
    }
    const bool child = HAS_FIELD(ui, AnomalyUiServiceV1, begin_child) &&
        HAS_FIELD(ui, AnomalyUiServiceV1, end_child);
    if (child) ui->begin_child(ui->user, StringView("##dsr-detail"), 0.0F, 210.0F, 0U);
    UiTextWrapped(ui, model.selection_meta.c_str());
    UiTextWrapped(ui, model.selection_types.c_str());

    const bool can_table = HAS_FIELD(ui, AnomalyUiServiceV1, begin_table) &&
        HAS_FIELD(ui, AnomalyUiServiceV1, table_next_row) &&
        HAS_FIELD(ui, AnomalyUiServiceV1, table_next_column) &&
        HAS_FIELD(ui, AnomalyUiServiceV1, end_table);
    if (can_table && !model.hits.empty() &&
        ui->begin_table(ui->user, StringView("##dsr-hits"), 4, kTableFlags, 0.0F,
                        0.0F)) {
        ui->table_next_row(ui->user);
        ui->table_next_column(ui->user);
        UiText(ui, "时间");
        ui->table_next_column(ui->user);
        UiText(ui, "伤害");
        ui->table_next_column(ui->user);
        UiText(ui, "类型");
        ui->table_next_column(ui->user);
        UiText(ui, "标志 / 目标");
        for (const HitView& row : model.hits) {
            ui->table_next_row(ui->user);
            ui->table_next_column(ui->user);
            UiText(ui, row.time.c_str());
            ui->table_next_column(ui->user);
            UiText(ui, row.damage.c_str());
            ui->table_next_column(ui->user);
            UiText(ui, row.type.c_str());
            ui->table_next_column(ui->user);
            UiText(ui, row.detail.c_str());
        }
        ui->end_table(ui->user);
    } else {
        for (const HitView& row : model.hits) {
            std::string line = row.time + "  " + row.damage + "  " + row.type + "  " +
                row.detail;
            UiText(ui, line.c_str());
        }
    }
    if (child) ui->end_child(ui->user);
}

void DrawControls(const AnomalyUiServiceV1* ui, const UiModel& model) {
    if (HAS_FIELD(ui, AnomalyUiServiceV1, checkbox)) {
        int recording = model.recording ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("记录伤害"), &recording) &&
            recording != (model.recording ? 1 : 0)) {
            g_recording.store(recording != 0);
            QueueRefresh();
        }
        UiSameLine(ui, 10.0F);
        int incoming = model.include_incoming ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("包含受到的伤害"), &incoming) &&
            incoming != (model.include_incoming ? 1 : 0)) {
            g_include_incoming.store(incoming != 0);
            QueueRefresh();
        }
        UiSameLine(ui, 10.0F);
        int other = model.include_other ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("含召唤物伤害"), &other) &&
            other != (model.include_other ? 1 : 0)) {
            g_include_other.store(other != 0);
            QueueRefresh();
        }
    }
    if (HAS_FIELD(ui, AnomalyUiServiceV1, slider_float)) {
        float gap = model.gap_seconds;
        if (ui->slider_float(ui->user, StringView("记录间隔（秒）"), &gap,
                             kMinGapSeconds, kMaxGapSeconds) &&
            gap != model.gap_seconds) {
            g_gap_seconds.store(gap);
            QueueRefresh();
        }
    }
    if (UiButton(ui, "清空记录", true)) QueueClear();
}

// “重放已选的伤害”：点一次重放一次。按钮紧跟开关，选中片段的详情和记录列表都排在它下面，
// 这样记录再多也不会把按钮和当前片段的信息挤出视野。
void DrawReplayButton(const AnomalyUiServiceV1* ui, const UiModel& model) {
    std::string hint;
    if (!model.has_selection) {
        hint = "先在下方记录里选择一段伤害";
    } else if (!model.selection_replayable) {
        hint = "这一段没有记录到来源或技能，无法重放";
    } else if (!model.damage_bridge_ready && !model.invocation_ready) {
        hint = "等待对象表与技能服务（进入世界后可用）";
    }
    const bool can_replay = model.has_selection && model.selection_replayable;
    if (UiButton(ui, "重放已选的伤害", can_replay)) QueueReplay(model.selected_id);
    if (!hint.empty()) {
        UiSameLine(ui, 12.0F);
        UiText(ui, hint.c_str());
    }
    if (!model.replay_status.empty()) UiTextWrapped(ui, model.replay_status.c_str());
    if (!model.watch_line.empty()) UiText(ui, model.watch_line.c_str());
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void ResetAll() {
    g_started = Clock::now();
    g_recorder = Recorder{};
    g_recording.store(true);
    g_include_incoming.store(false);
    g_include_other.store(false);
    g_gap_seconds.store(1.0F);
    {
        std::lock_guard<std::mutex> lock(g_action_mutex);
        g_actions = Actions{};
    }
    {
        std::lock_guard<std::mutex> lock(g_model_mutex);
        g_model.reset();
    }
    g_dirty = true;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;
    g_host = host;
    EnsureServices(host);
    if (!HAS_FIELD(g_services.ui, AnomalyUiServiceV1, begin_window) ||
        !HAS_FIELD(g_services.ui, AnomalyUiServiceV1, end_window) ||
        !HAS_FIELD(g_services.ui, AnomalyUiServiceV1, text)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    ResetAll();
    *context = &g_state_token;
    LogInfo("伤害来源记录已加载（v1.0.3）");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g_state_token) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    ResetAll();
    LogInfo("伤害来源记录已启用");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    g_recorder = Recorder{};
    {
        std::lock_guard<std::mutex> lock(g_model_mutex);
        g_model.reset();
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    g_recorder = Recorder{};
    {
        std::lock_guard<std::mutex> lock(g_model_mutex);
        g_model.reset();
    }
    g_damage_bridge = damage_apply::Bridge{};
    g_damage_bridge_next_try = 0.0;
    g_services = Services{};
    g_host = nullptr;
}

void ANOMALY_CALL Update(void* context, double) {
    if (context != &g_state_token) return;
    EnsureServices(g_host);
    const double now = Now();

    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    bool combatant_valid = false;
    if (CombatReady()) {
        const AnomalyStatusV1 status =
            g_services.combat->current_combatant(g_services.combat->user, &combatant);
        combatant_valid = status.code == ANOMALY_STATUS_V1_OK &&
            (combatant.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 &&
            combatant.character.id != 0;
    }
    if (combatant_valid) {
        if (!g_recorder.world_known || !SameHandle(g_recorder.world, combatant.world)) {
            g_recorder.world = combatant.world;
            g_recorder.world_known = true;
            if (g_recorder.replay.watching) {
                g_recorder.replay.watching = false;
                g_recorder.replay.message = "世界已切换，观测已停止";
                g_dirty = true;
            }
        }
    }

    // 技能状态先记下来：伤害事件通常晚于技能激活几个 tick 才被消费。
    TrackActiveSkill(now, combatant_valid, combatant);
    // 进入世界后就把伤害施加桥准备好，界面顶栏能直接反映它是否可用。
    if (combatant_valid) static_cast<void>(DamageBridgeReady(now));
    ConsumeActions(now, combatant_valid, combatant);
    DrainDamage(now, combatant_valid, combatant);
    TickReplayWatch(now);

    if (g_dirty || g_recorder.replay.watching) PublishModel(combatant_valid);
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui_argument) {
    const AnomalyUiServiceV1* ui =
        ui_argument != nullptr ? ui_argument : g_services.ui;
    if (!HAS_FIELD(ui, AnomalyUiServiceV1, begin_window) ||
        !HAS_FIELD(ui, AnomalyUiServiceV1, end_window) ||
        !HAS_FIELD(ui, AnomalyUiServiceV1, text)) {
        return;
    }
    std::shared_ptr<const UiModel> model;
    {
        std::lock_guard<std::mutex> lock(g_model_mutex);
        model = g_model;
    }
    if (!model) return;

    if (HAS_FIELD(ui, AnomalyUiServiceV1, set_next_window_size_constraints)) {
        ui->set_next_window_size_constraints(
            ui->user, 560.0F, 420.0F, 100000.0F, 100000.0F);
    }
    if (HAS_FIELD(ui, AnomalyUiServiceV1, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 660.0F, 720.0F, kCondFirstUseEver);
    }
    anomaly::sdk::UiWindow window(ui, "伤害来源记录");
    if (!window) return;

    UiTextWrapped(ui, model->service_line.c_str());
    DrawControls(ui, *model);
    UiSeparator(ui);
    DrawReplayButton(ui, *model);
    UiSeparator(ui);
    if (!model->totals_line.empty()) UiText(ui, model->totals_line.c_str());
    UiText(ui, "选中片段");
    DrawDetail(ui, *model);
    UiSeparator(ui);
    UiText(ui, "伤害记录（点「选择」切换要重放的片段）");
    DrawSegmentTable(ui, *model);
    UiSeparator(ui);
    UiTextWrapped(ui, "记录来自战斗事件流（CharacterOnDamaged）。");
    UiTextWrapped(ui, "连续命中按「同一来源 + 记录间隔」合并为一段。");
    UiTextWrapped(ui, "重放会把记录到的伤害效果重新施加到记录的目标上。");
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("com.yu.damage-source-recorder"),
        StringView("伤害来源记录"),
        StringView("YU-1021"), StringView("1.0.3"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
