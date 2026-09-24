// 共享自动战斗实现。搬移自 `plugins/CloneEnter/plugin.cpp`（各函数注释里标了原始行号），
// 逻辑逐行保持原样，只做接口要求的那几处替换：`context.auto_combat_*` → `state.*`、
// `context.<service>` → `host.<service>`、`context.combat_status = X` → `SetStatus(host, X)`。
//
// 与原实现的两处结构性差异（都不是逻辑改动）：
//   * `clone_check_done` 那段副本簿记没有搬进来，等价动作由调用方调一次 `Reset` 完成；
//   * 原来每次调用现查 combat/skills/skill_invocation 三个服务，这里直接用 `host` 里的指针。
#include "auto_combat.hpp"

#include "anomaly/sdk/cpp.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anomaly::plugins::combat {
namespace {

// —— 搬移过来的游戏布局常量（CloneEnter/plugin.cpp:27–83）——
constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
constexpr std::ptrdiff_t kGObjectsAddend = -16;
constexpr std::uint32_t kRipDisplacementOffset = 3;
constexpr std::uint32_t kRipInstructionSize = 7;

constexpr std::ptrdiff_t kObjectClassOffset = 16;
constexpr std::ptrdiff_t kObjectNameOffset = 24;
constexpr std::ptrdiff_t kUStructChildrenOffset = 72;
constexpr std::ptrdiff_t kUStructSuperStructOffset = 64;
constexpr std::ptrdiff_t kUFieldNextOffset = 40;
constexpr std::ptrdiff_t kUFunctionNumParmsOffset = 180;
constexpr std::ptrdiff_t kUFunctionParmsSizeOffset = 182;

constexpr std::ptrdiff_t kWorldGameInstanceOffset = 560;
constexpr std::ptrdiff_t kGameInstanceLocalPlayersOffset = 56;
constexpr std::ptrdiff_t kLocalPlayerControllerOffset = 48;
constexpr std::ptrdiff_t kControllerPlayerStateOffset = 720;
constexpr std::size_t kProcessEventVtableIndex = 0x4C;
constexpr std::size_t kMaximumNameBytes = 1024;

// DataTable RowMap 布局
constexpr std::ptrdiff_t kDataTableRowMapOffset = 48;
constexpr std::size_t kDataTableRowStride = 24;

constexpr std::ptrdiff_t kUStructPropertyLinkOffset = 112;
constexpr std::ptrdiff_t kFFieldNameOffset = 32;
constexpr std::ptrdiff_t kFFieldClassOffset = 8;
constexpr std::ptrdiff_t kFPropertyElementSizeOffset = 52;
constexpr std::ptrdiff_t kFPropertyOffsetInternalOffset = 68;
constexpr std::ptrdiff_t kFPropertyPropertyLinkNextOffset = 72;

// 传送落点额外抬高的厘米数（CloneEnter/plugin.cpp:6544）。
constexpr double kTeleportChestZOffset = 200.0;

// —— 战斗常量（CloneEnter/plugin.cpp:4242–4255）——
// 进入攻击距离后，玩家在这么长时间里一次伤害都没打出来，就认定目标无效。
constexpr auto kAutoCombatNoDamageGrace = std::chrono::seconds(3);
// 打死之后的尸体：拉黑到它从快照消失即可。
constexpr auto kAutoCombatDeadTargetTtl = std::chrono::seconds(45);
// 从头到尾一次都没打中过的目标（雨人的湖面/底座这类道具）：拉黑久一些，
// 否则它会一直是最"近"的目标，让你反复对着空气挥。
constexpr auto kAutoCombatNeverHitTargetTtl = std::chrono::seconds(120);
// 大世界里带 RainMan/mon_ 字样却不是怪的道具（湖面、底座、贴花）实测最大边只有约 42cm，
// 真正的怪都在 74cm 以上，因此按尺寸做一道物理预筛，三条边都小于阈值就不算怪物候选。
constexpr double kAutoCombatMinimumExtentCm = 60.0;
// 判定"同一具尸体"的位置容差（厘米）。
constexpr double kAutoCombatDeadPosTolerance = 150.0;
// 攻击分支使用的距离阈值（厘米）。
constexpr double kAutoCombatAttackRangeCm = 600.0;
// 离开这个距离才算「不再打它」（迟滞，避免在攻击距离边界抖动时反复清零计时）。
constexpr double kAutoCombatLeaveRangeCm = 1200.0;
// 见过怪之后连续这么多秒无怪，本点就算打完（CloneEnter/plugin.cpp:6875 的同一个 5）。
constexpr std::uint32_t kAutoCombatNoMonsterSeconds = 5;

struct FNamePair {
    std::uint32_t cmp{};
    std::uint32_t number{};
};

// 模块不直接写面板：状态字符串经 `host.set_status` 回到调用方。
void SetStatus(Host& host, const std::string& text) noexcept {
    if (host.set_status != nullptr) host.set_status(host.status_user, text);
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool SignatureReady(const AnomalySignatureServiceV1* s) noexcept {
    return HasField<AnomalySignatureServiceV1,
               decltype(AnomalySignatureServiceV1::resolve)>(
               s, offsetof(AnomalySignatureServiceV1, resolve)) &&
        s->resolve != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* s) noexcept {
    return HasField<AnomalyUe5NamesServiceV1,
               decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               s, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
        s->resolve_utf8 != nullptr;
}

bool PlayerReady(const AnomalyNtePlayerServiceV1* s) noexcept {
    return HasField<AnomalyNtePlayerServiceV1,
               decltype(AnomalyNtePlayerServiceV1::snapshot)>(
               s, offsetof(AnomalyNtePlayerServiceV1, snapshot)) &&
        s->snapshot != nullptr;
}

template <typename T>
bool Read(const void* address, T& value) noexcept {
    if (address == nullptr) return false;
    __try {
        std::memcpy(&value, address, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* ReadPointer(const void* address) noexcept {
    std::uintptr_t value{};
    return Read(address, value) ? reinterpret_cast<void*>(value) : nullptr;
}

bool ResolveRipRelative(const AnomalySignatureServiceV1* signature,
                        const std::string_view pattern,
                        const std::ptrdiff_t addend,
                        std::uintptr_t& address) noexcept {
    address = 0;
    if (!SignatureReady(signature)) return false;
    std::uintptr_t instruction{};
    if (signature->resolve(signature->user, anomaly::sdk::StringView("HTGame.exe"),
                           anomaly::sdk::StringView(".text"),
                           anomaly::sdk::StringView(pattern), &instruction)
            .code != ANOMALY_STATUS_V1_OK ||
        instruction == 0) {
        return false;
    }
    std::int32_t displacement{};
    if (!Read(reinterpret_cast<const void*>(instruction + kRipDisplacementOffset),
              displacement)) {
        return false;
    }
    const auto resolved = static_cast<std::intptr_t>(instruction) +
        static_cast<std::intptr_t>(kRipInstructionSize) + displacement;
    if (resolved <= 0) return false;
    if (addend < 0) {
        const auto magnitude = static_cast<std::uintptr_t>(-(addend + 1)) + 1U;
        if (static_cast<std::uintptr_t>(resolved) <= magnitude) return false;
        address = static_cast<std::uintptr_t>(resolved) - magnitude;
    } else {
        address = static_cast<std::uintptr_t>(resolved) +
            static_cast<std::uintptr_t>(addend);
    }
    return address != 0;
}

std::string ResolveName(const AnomalyUe5NamesServiceV1* names,
                        const std::uint32_t name_id) {
    if (!NamesReady(names) || name_id == 0) return {};
    std::size_t size{};
    if (names->resolve_utf8(names->user, name_id, nullptr, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > kMaximumNameBytes) {
        return {};
    }
    std::string value(size, '\0');
    if (names->resolve_utf8(names->user, name_id, value.data(), &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > value.size()) {
        return {};
    }
    value.resize(size - 1U);
    return value;
}

std::string ObjectName(const AnomalyUe5NamesServiceV1* names,
                       const std::uintptr_t object) {
    std::uint32_t name_id{};
    if (!Read(reinterpret_cast<const void*>(object + kObjectNameOffset), name_id)) {
        return {};
    }
    return ResolveName(names, name_id);
}

std::string RenderFName(const Host& host, const FNamePair& f) {
    std::string s = ResolveName(host.names, f.cmp);
    if (f.number != 0) {
        s += "_";
        s += std::to_string(f.number - 1);
    }
    return s;
}

bool FindFunction(const AnomalyUe5NamesServiceV1* names, const std::uintptr_t cls,
                  const std::string_view target, const std::uint8_t num_parms,
                  const std::uint16_t parms_size, std::uintptr_t& result) noexcept {
    std::uintptr_t owner = cls;
    for (std::uint32_t depth{}; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t count{}; field != 0 && count < 4096; ++count) {
            std::uintptr_t next{};
            std::uintptr_t field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) {
                break;
            }
            if (ObjectName(names, field_class) == "Function" &&
                ObjectName(names, field) == target) {
                std::uint8_t np{};
                std::uint16_t ps{};
                Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                if (np == num_parms && ps == parms_size) {
                    result = field;
                    return true;
                }
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) {
            break;
        }
        owner = super;
    }
    return false;
}

bool Invoke(void* object, void* function, void* parameters) noexcept {
    if (!object || !function) return false;
    using ProcessEvent = void(__fastcall*)(void*, void*, void*);
    auto vtable = ReadPointer(object);
    if (!vtable) return false;
    auto process_event = ReadPointer(reinterpret_cast<const std::uint8_t*>(vtable) +
        kProcessEventVtableIndex * sizeof(void*));
    if (!process_event) return false;
    __try {
        reinterpret_cast<ProcessEvent>(process_event)(object, function, parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void StopAutoCombatMovement(Host& host, State& state) noexcept;
void ResetAutoCombatTarget(Host& host, State& state) noexcept;

bool GetPlayerState(Host& host, State& state) noexcept {
    if (state.g_world_address == 0 &&
        !ResolveRipRelative(host.signature, kGWorldPattern, 0,
                            state.g_world_address)) {
        return false;
    }
    std::uintptr_t world{};
    if (!Read(reinterpret_cast<const void*>(state.g_world_address), world) ||
        world == 0) {
        return false;
    }
    if (state.player_state != 0 && state.controller != 0 &&
        world == state.cached_world) {
        return true;
    }
    std::uintptr_t game_instance{};
    if (!Read(reinterpret_cast<const void*>(world + kWorldGameInstanceOffset),
              game_instance) || game_instance == 0) {
        return false;
    }
    std::uintptr_t players_array{};
    std::int32_t players_count{};
    if (!Read(reinterpret_cast<const void*>(
                  game_instance + kGameInstanceLocalPlayersOffset), players_array) ||
        players_array == 0 ||
        !Read(reinterpret_cast<const void*>(
                  game_instance + kGameInstanceLocalPlayersOffset + 8), players_count) ||
        players_count < 1) {
        return false;
    }
    std::uintptr_t local_player{};
    if (!Read(reinterpret_cast<const void*>(players_array), local_player) ||
        local_player == 0) {
        return false;
    }
    std::uintptr_t controller{};
    if (!Read(reinterpret_cast<const void*>(
                  local_player + kLocalPlayerControllerOffset), controller) ||
        controller == 0) {
        return false;
    }
    std::uintptr_t ps{};
    if (!Read(reinterpret_cast<const void*>(
                  controller + kControllerPlayerStateOffset), ps) ||
        ps == 0) {
        return false;
    }
    if (state.cached_world != world) {
        // 换世界等价于换副本实例：目标与怪物类名缓存一起清掉（原实现这里是
        // ResetAutoCombatTarget + 清 monster_class_name_ids + 清 next_class_rescan）。
        Reset(host, state);
    }
    state.controller = controller;
    state.player_state = ps;
    state.cached_world = world;
    return true;
}

bool SnapshotPlayerPosition(Host& host, double (&position)[3]) noexcept {
    if (!PlayerReady(host.player)) return false;
    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    if (host.player->snapshot(host.player->user, &snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) {
        return false;
    }
    position[0] = snapshot.position[0];
    position[1] = snapshot.position[1];
    position[2] = snapshot.position[2];
    return true;
}

bool ReadDataTable(const std::uintptr_t dt,
                   std::vector<std::pair<FNamePair, std::uintptr_t>>& rows) noexcept {
    std::uintptr_t data{};
    std::int32_t num{}, max{};
    if (!Read(reinterpret_cast<const void*>(dt + kDataTableRowMapOffset + 0), data) ||
        !Read(reinterpret_cast<const void*>(dt + kDataTableRowMapOffset + 8), num) ||
        !Read(reinterpret_cast<const void*>(dt + kDataTableRowMapOffset + 12), max) ||
        data == 0 || num <= 0 || num > 4096 || max < num) {
        return false;
    }
    rows.clear();
    rows.reserve(static_cast<std::size_t>(num));
    for (std::int32_t i = 0; i < num; ++i) {
        const std::size_t elem = static_cast<std::size_t>(i) * kDataTableRowStride;
        FNamePair key{};
        std::uintptr_t row{};
        if (!Read(reinterpret_cast<const void*>(data + elem), key.cmp) ||
            !Read(reinterpret_cast<const void*>(data + elem + 4), key.number) ||
            !Read(reinterpret_cast<const void*>(data + elem + 8), row) ||
            key.cmp == 0) {
            continue;
        }
        rows.emplace_back(key, row);
    }
    return !rows.empty();
}

bool InputFieldOffset(const Host& host, std::uintptr_t owner,
                      std::string_view name, std::string_view type,
                      std::int32_t size, std::int32_t& offset,
                      std::uint16_t buffer_size = 0) {
    std::uintptr_t property{};
    Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), property);
    for (unsigned i = 0; property != 0 && i < 32; ++i) {
        std::uint32_t name_id{};
        Read(reinterpret_cast<const void*>(property + kFFieldNameOffset), name_id);
        if (ResolveName(host.names, name_id) == name) {
            std::uintptr_t field_class{};
            std::uint32_t type_id{};
            std::int32_t actual_size{};
            if (!Read(reinterpret_cast<const void*>(property + kFFieldClassOffset), field_class) ||
                !Read(reinterpret_cast<const void*>(field_class), type_id) ||
                ResolveName(host.names, type_id) != type ||
                !Read(reinterpret_cast<const void*>(property + kFPropertyElementSizeOffset), actual_size) ||
                actual_size != size ||
                !Read(reinterpret_cast<const void*>(property + kFPropertyOffsetInternalOffset), offset) ||
                offset < 0 || (buffer_size != 0 && (size > buffer_size || offset > buffer_size - size))) return false;
            return true;
        }
        std::uintptr_t next{};
        if (!Read(reinterpret_cast<const void*>(property + kFPropertyPropertyLinkNextOffset), next) || next == property) break;
        property = next;
    }
    return false;
}

std::uintptr_t FindWidgetProperty(const Host& host, std::uintptr_t object,
                                  std::string_view property_name) noexcept {
    std::uintptr_t cls{};
    Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
    for (unsigned depth = 0; cls != 0 && depth < 64; ++depth) {
        std::uintptr_t property{};
        Read(reinterpret_cast<const void*>(cls + kUStructPropertyLinkOffset), property);
        for (unsigned count = 0; property != 0 && count < 4096; ++count) {
            std::uint32_t name_id{};
            if (!Read(reinterpret_cast<const void*>(property + kFFieldNameOffset), name_id)) return 0;
            if (ResolveName(host.names, name_id) == property_name) {
                std::uintptr_t field_class{}, value{};
                std::uint32_t type_id{};
                std::int32_t size{}, offset{};
                if (!Read(reinterpret_cast<const void*>(property + kFFieldClassOffset), field_class) ||
                    !Read(reinterpret_cast<const void*>(field_class), type_id) ||
                    ResolveName(host.names, type_id) != "ObjectProperty" ||
                    !Read(reinterpret_cast<const void*>(property + kFPropertyElementSizeOffset), size) ||
                    size != sizeof(std::uintptr_t) ||
                    !Read(reinterpret_cast<const void*>(property + kFPropertyOffsetInternalOffset), offset) ||
                    offset < 0 ||
                    !Read(reinterpret_cast<const void*>(object + offset), value)) return 0;
                return value;
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(property + kFPropertyPropertyLinkNextOffset), next) ||
                next == property) break;
            property = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(cls + kUStructSuperStructOffset), super) ||
            super == cls) break;
        cls = super;
    }
    return 0;
}

bool ResolveNormalAttackInput(Host& host, State& state) {
    if (!GetPlayerState(host, state)) {
        SetStatus(host, "普攻输入：等待玩家");
        return false;
    }
    auto& binding = state.normal_attack;
    if (binding.controller == state.controller && binding.world == state.cached_world &&
        binding.triggered != 0 && binding.completed != 0) return true;
    binding = {};
    NormalAttackBinding next;
    std::uintptr_t cls{};
    Read(reinterpret_cast<const void*>(state.controller + kObjectClassOffset), cls);
    if (!FindFunction(host.names, cls, "ActivateAbilityFromID", 2, 8, next.triggered) ||
        !FindFunction(host.names, cls, "ReleaseAbilityFromID", 2, 8, next.completed)) {
        SetStatus(host, "普攻输入：未找到配对的能力输入函数");
        return false;
    }
    const auto table = FindWidgetProperty(host, state.controller, "DT_AbilityInput");
    const auto row_struct = FindWidgetProperty(host, table, "RowStruct");
    std::int32_t id_offset{}, param_offset{}, action_offset{};
    std::vector<std::pair<FNamePair, std::uintptr_t>> rows;
    if (table == 0 || ObjectName(host.names, row_struct) != "HTAbilityInputRow" ||
        !InputFieldOffset(host, row_struct, "InputID", "ByteProperty", 1, id_offset) ||
        !InputFieldOffset(host, row_struct, "Param", "IntProperty", 4, param_offset) ||
        !InputFieldOffset(host, row_struct, "InputAction", "ObjectProperty", 8, action_offset) ||
        !ReadDataTable(table, rows)) {
        SetStatus(host, "普攻输入：角色输入绑定表不可用");
        return false;
    }
    bool found{};
    for (const auto& [name, row] : rows) {
        if (RenderFName(host, name) != "MeleeAtack") continue;
        std::uintptr_t action{};
        if (found || !Read(reinterpret_cast<const void*>(row + action_offset), action) ||
            ObjectName(host.names, action) != "IA_MeleeAttack" ||
            !Read(reinterpret_cast<const void*>(row + id_offset), next.input_id) ||
            !Read(reinterpret_cast<const void*>(row + param_offset), next.input_param)) {
            SetStatus(host, "普攻输入：普通攻击绑定不兼容");
            return false;
        }
        found = true;
    }
    if (!found) {
        SetStatus(host, "普攻输入：未找到普通攻击绑定");
        return false;
    }
    for (const bool pressed : {false, true}) {
        const auto fn = pressed ? next.triggered : next.completed;
        auto& value = pressed ? next.pressed_value : next.released_value;
        if (!InputFieldOffset(host, fn, "InputID", "ByteProperty", 1, id_offset, 8) ||
            !InputFieldOffset(host, fn, "Param", "IntProperty", 4, param_offset, 8)) {
            SetStatus(host, "普攻输入：能力输入参数不兼容");
            return false;
        }
        value[id_offset] = next.input_id;
        std::memcpy(value.data() + param_offset, &next.input_param, sizeof(next.input_param));
    }
    next.world = state.cached_world;
    next.controller = state.controller;
    binding = next;
    return true;
}

bool InvokeNormalAttack(Host& host, State& state) {
    if (!ResolveNormalAttackInput(host, state)) return false;
    const auto& binding = state.normal_attack;
    alignas(8) auto pressed_value = binding.pressed_value;
    alignas(8) auto released_value = binding.released_value;
    // A tap completes in this Game callback, so unload cannot strand a held input.
    const bool pressed = Invoke(reinterpret_cast<void*>(binding.controller),
        reinterpret_cast<void*>(binding.triggered), pressed_value.data());
    const bool released = Invoke(reinterpret_cast<void*>(binding.controller),
        reinterpret_cast<void*>(binding.completed), released_value.data());
    if (!pressed || !released) SetStatus(host, "普攻输入调用异常，已停止");
    return pressed && released;
}

bool ActivateSkillByInputId(Host& host, std::int32_t input_id) noexcept {
    // 原实现在这里用 `context.host` 现查 combat / skills / skill_invocation 三个服务；
    // 模块由 `Host` 注入，指针为空即视为服务不可用（`HasField` 对空指针返回 false）。
    if (!HasField<AnomalyNteCombatServiceV1, decltype(AnomalyNteCombatServiceV1::current_combatant)>(
            host.combat, offsetof(AnomalyNteCombatServiceV1, current_combatant)) ||
        host.combat->current_combatant == nullptr ||
        !HasField<AnomalyNteSkillsServiceV1, decltype(AnomalyNteSkillsServiceV1::page)>(
            host.skills, offsetof(AnomalyNteSkillsServiceV1, page)) ||
        host.skills->frame == nullptr || host.skills->page == nullptr ||
        !HasField<AnomalyNteSkillInvocationServiceV1, decltype(AnomalyNteSkillInvocationServiceV1::activate)>(
            host.skill_invocation, offsetof(AnomalyNteSkillInvocationServiceV1, activate)) ||
        host.skill_invocation->activate == nullptr) {
        SetStatus(host, "等待框架技能服务");
        return false;
    }
    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    const auto combat_status = host.combat->current_combatant(host.combat->user, &combatant);
    const auto skill_status = host.skills->frame(host.skills->user, &frame);
    if (combat_status.code != ANOMALY_STATUS_V1_OK || skill_status.code != ANOMALY_STATUS_V1_OK ||
        (combatant.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0 ||
        (frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0 ||
        combatant.character.id != frame.character.id ||
        combatant.character.generation != frame.character.generation) {
        SetStatus(host, "等待框架角色技能快照");
        return false;
    }
    std::array<AnomalyNteSkillSnapshotV1, 64> skills{};
    for (auto& skill : skills) skill.struct_size = sizeof(skill);
    std::uint32_t offset{};
    bool matched{};
    while (offset < frame.skill_count) {
        AnomalyNteSkillPageRequestV1 page_request{sizeof(page_request)};
        page_request.generation = frame.generation;
        page_request.offset = offset;
        page_request.capacity = static_cast<std::uint32_t>(skills.size());
        AnomalyNteSkillPageResultV1 page{sizeof(page)};
        if (host.skills->page(host.skills->user, &page_request, skills.data(), &page).code != ANOMALY_STATUS_V1_OK ||
            page.generation != frame.generation || page.returned > skills.size()) {
            SetStatus(host, "框架技能快照已变化，等待刷新");
            return false;
        }
        for (std::uint32_t i = 0; i < page.returned; ++i) {
            const auto& skill = skills[i];
            // STALE describes sample age; activate revalidates live identities in the host.
            if (skill.input_id != input_id || (skill.flags & ANOMALY_NTE_SKILL_V1_VALID) == 0 ||
                (skill.flags & ANOMALY_NTE_SKILL_V1_PENDING_REMOVE) != 0) continue;
            matched = true;
            AnomalyNteSkillInvocationRequestV1 request{sizeof(request)};
            request.world = combatant.world;
            request.character = frame.character;
            request.skill = skill.handle;
            AnomalyNteSkillInvocationResultV1 result{sizeof(result)};
            const auto status = host.skill_invocation->activate(host.skill_invocation->user, &request, &result);
            if (status.code != ANOMALY_STATUS_V1_OK) {
                SetStatus(host, "框架技能调用失败：" + std::to_string(status.code));
                return false;
            }
            if (result.accepted != 0) return true;
        }
        if (page.next_offset <= offset) break;
        offset = page.next_offset;
    }
    SetStatus(host, matched ? std::string("框架技能未接受本次施放") :
        "框架未找到技能 ID " + std::to_string(input_id));
    return false;
}

bool IsMonsterClassName(const std::string& name) noexcept {
    // 以怪物前缀开头是最强信号，直接认定，不参与下面的辅助对象排除：
    // 例如 mon_038_BP_World_CityEvent_Passive_01_C 名字里带 World/Passive，
    // 但它确实是怪物，按关键词+排除词会被误杀。
    if (name.rfind("mon_", 0) == 0 || name.rfind("boss", 0) == 0 ||
        name.rfind("Boss_", 0) == 0) {
        return true;
    }
    // 其余命名（大世界/事件怪等）按关键词命中识别，再排除同名族里的辅助对象。
    // "mon_" 必须落在名字段边界上：Common_ 里也含 "mon_"，但 BP_MB_Graffiti_Decal_Common_C
    // 是涂鸦贴花而不是怪，直接 find 会把它当成怪物。
    bool candidate = false;
    for (std::size_t at = name.find("mon_"); at != std::string::npos;
         at = name.find("mon_", at + 1)) {
        if (at == 0 || name[at - 1] == '_') {
            candidate = true;
            break;
        }
    }
    if (!candidate) {
        for (const char* kw : {"Monster", "monster", "boss", "Boss",
                               "RainMan", "Enemy", "enemy"}) {
            if (name.find(kw) != std::string::npos) {
                candidate = true;
                break;
            }
        }
    }
    if (!candidate) return false;
    for (const char* kw : {"Controller", "bullet", "World", "Vision", "FX",
                           "Child", "summon", "Body", "anim", "back", "begin",
                           "Dead", "Play", "Hide", "Open", "Passive", "Skin",
                           "Weapon", "Montage", "Material", "Texture",
                           "LogicBox", "Spawn", "Manager"}) {
        if (name.find(kw) != std::string::npos) return false;
    }
    return true;
}

// entities 与 actors 两个服务的读取接口完全一致（frame/page/class_name_utf8），
// 因此用模板统一处理：同一份逻辑同时覆盖两个实体来源。
// 二者覆盖面不同——例如 boss18_* 只出现在 actors 服务，雨人只出现在 entities 服务。
struct CombatTargetPick {
    bool valid{};
    double pos[3]{};
    double best_distance_squared{};
    AnomalyGenerationHandleV1 handle{};
    std::uint32_t class_name_id{};
};

double CombatDistanceSquared(const double* from, const double* to) noexcept {
    const double dx = to[0] - from[0];
    const double dy = to[1] - from[1];
    const double dz = to[2] - from[2];
    return dx * dx + dy * dy + dz * dz;
}

template <typename Service>
bool CollectMonsterClassIdsFrom(
    Service* service,
    std::vector<std::uint32_t>& ids) {
    if (service == nullptr || service->frame == nullptr || service->page == nullptr ||
        service->class_name_utf8 == nullptr) {
        return false;
    }
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if (service->frame(service->user, &frame).code != ANOMALY_STATUS_V1_OK) return false;
    std::array<AnomalyNteEntitySnapshotV1, 256> buf{};
    for (auto& s : buf) s.struct_size = sizeof(s);
    std::uint32_t offset = 0;
    while (true) {
        AnomalyNteEntityPageRequestV1 req{sizeof(req)};
        req.generation = frame.generation;
        req.offset = offset;
        req.capacity = 256;
        AnomalyNteEntityPageResultV1 res{sizeof(res)};
        if (service->page(service->user, &req, buf.data(), &res).code !=
            ANOMALY_STATUS_V1_OK) {
            return false;
        }
        for (std::uint32_t j = 0; j < res.returned; ++j) {
            const auto& snap = buf[j];
            std::size_t sz = 0;
            if (service->class_name_utf8(service->user, snap.class_id, nullptr, &sz).code !=
                    ANOMALY_STATUS_V1_OK || sz == 0) {
                continue;
            }
            std::string cn(sz, '\0');
            if (service->class_name_utf8(service->user, snap.class_id, cn.data(), &sz).code !=
                ANOMALY_STATUS_V1_OK) {
                continue;
            }
            cn.resize(sz - 1);
            if (!IsMonsterClassName(cn)) continue;
            bool exists = false;
            for (const std::uint32_t id : ids) {
                if (id == snap.class_name_id) { exists = true; break; }
            }
            if (!exists) ids.push_back(snap.class_name_id);
        }
        if (res.next_offset == 0 || res.next_offset >= res.total_matches) break;
        offset = res.next_offset;
    }
    return true;
}

template <typename Service, typename Skip>
bool PickNearestMonsterFrom(
    Service* service,
    const std::vector<std::uint32_t>& ids,
    const double* player_pos,
    const double radius_squared,
    Skip&& skip,
    CombatTargetPick& pick) {
    if (service == nullptr || service->frame == nullptr || service->page == nullptr ||
        service->class_name_utf8 == nullptr) {
        return false;
    }
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if (service->frame(service->user, &frame).code != ANOMALY_STATUS_V1_OK) return false;
    // 缓冲区在类名循环外复用：一次调用只初始化一份，而不是每个类名各一份
    // （256 × sizeof(snapshot) ≈ 24KB，按类名数翻倍放大）。
    std::array<AnomalyNteEntitySnapshotV1, 256> buf{};
    for (auto& s : buf) s.struct_size = sizeof(s);
    for (const std::uint32_t cid : ids) {
        std::uint32_t offset = 0;
        while (true) {
            AnomalyNteEntityPageRequestV1 req{sizeof(req)};
            req.generation = frame.generation;
            req.offset = offset;
            req.capacity = 256;
            req.class_name_id = cid;
            req.excluded_flags = ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER;
            AnomalyNteEntityPageResultV1 res{sizeof(res)};
            if (service->page(service->user, &req, buf.data(), &res).code !=
                ANOMALY_STATUS_V1_OK) {
                return false;
            }
            for (std::uint32_t j = 0; j < res.returned; ++j) {
                const auto& snap = buf[j];
                if (skip(snap)) continue;
                // 物理预筛：三条边都小于阈值的不是怪物体型（雨人湖面/底座这类道具）。
                const double largest_extent = (std::max)({snap.bounds_extent[0],
                    snap.bounds_extent[1], snap.bounds_extent[2]});
                if (!(largest_extent >= kAutoCombatMinimumExtentCm)) continue;
                const double d2 = CombatDistanceSquared(player_pos, snap.bounds_center);
                if (!std::isfinite(d2) || d2 > radius_squared) continue;
                if (!pick.valid || d2 < pick.best_distance_squared) {
                    pick.best_distance_squared = d2;
                    pick.pos[0] = snap.bounds_center[0];
                    pick.pos[1] = snap.bounds_center[1];
                    pick.pos[2] = snap.bounds_center[2];
                    pick.valid = true;
                    pick.handle = snap.handle;
                    pick.class_name_id = cid;
                }
            }
            if (res.next_offset == 0 || res.next_offset >= res.total_matches) break;
            offset = res.next_offset;
        }
    }
    return true;
}

// 同时扫描 entities 与 actors 两个来源。二者覆盖面确实不同：entities 只覆盖
// world.persistentLevel，包含大世界怪物的 actors 只在全部关卡的扫描里出现。
bool FindMonsterClassIds(Host& host, State& state) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (!state.monster_class_name_ids.empty() && now < state.next_class_rescan) return true;
    std::vector<std::uint32_t> next_ids;
    const bool entities_ok = CollectMonsterClassIdsFrom(host.entities, next_ids);
    const bool actors_ok = CollectMonsterClassIdsFrom(host.actors, next_ids);
    if (!entities_ok && !actors_ok) return false;
    state.monster_class_name_ids = std::move(next_ids);
    state.next_class_rescan = now + std::chrono::seconds(5);
    return true;
}

void StopAutoCombatMovement(Host& host, State& state) noexcept {
    if (!state.moving) return;
    if (host.navigation != nullptr && host.navigation->stop_movement != nullptr) {
        static_cast<void>(host.navigation->stop_movement(host.navigation->user));
    }
    state.moving = false;
}

void ResetAutoCombatTarget(Host& host, State& state) noexcept {
    StopAutoCombatMovement(host, state);
    state.target_valid = false;
    state.scan_valid = false;
    state.next_target_update = {};
    // 计时必须跟着目标一起清掉：否则同一只怪离开半径后再回来时，
    // 会继承上一轮的进入时刻，一锁定就被判成尸体。
    state.attack_since = {};
    state.target_hit = false;
}

// 实体快照句柄与伤害参与者句柄不在同一 ID 空间，句柄和位置任一命中都算同一具尸体。
bool IsDeadAutoCombatTarget(
    const State& state, const AnomalyNteEntitySnapshotV1& snap,
    const std::chrono::steady_clock::time_point now) noexcept {
    for (const auto& dead : state.dead_targets) {
        if (now >= dead.until) continue;
        if (dead.handle.id != 0 && dead.handle.id == snap.handle.id &&
            dead.handle.generation == snap.handle.generation) {
            return true;
        }
        if (CombatDistanceSquared(dead.pos, snap.bounds_center) <=
            kAutoCombatDeadPosTolerance * kAutoCombatDeadPosTolerance) {
            return true;
        }
    }
    return false;
}

void BlacklistAutoCombatTarget(
    State& state, const AnomalyGenerationHandleV1& handle, const double* pos,
    const std::chrono::steady_clock::time_point now, bool ever_hit) noexcept {
    std::erase_if(state.dead_targets,
        [now](const DeadTarget& dead) { return now >= dead.until; });
    DeadTarget dead;
    dead.handle = handle;
    dead.pos[0] = pos[0];
    dead.pos[1] = pos[1];
    dead.pos[2] = pos[2];
    dead.until = now +
        (ever_hit ? kAutoCombatDeadTargetTtl : kAutoCombatNeverHitTargetTtl);
    state.dead_targets.push_back(dead);
}

// 伤害流是否可用。不可用时不能做尸体判定，否则会误把所有目标判成尸体。
bool CombatStreamAvailable(const Host& host) noexcept {
    return host.combat != nullptr &&
        host.combat->latest_damage_sequence != nullptr &&
        host.combat->next_damage_event != nullptr;
}

// 消费战斗伤害流，记录"玩家最近一次打出了伤害"的时刻。
// 该时刻是判断当前目标是否还能打的依据：尸体和道具类目标仍在快照里，但打不出任何伤害。
void PumpAutoCombatCombatStream(Host& host, State& state) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (host.combat == nullptr) return;
    if (host.combat->current_combatant != nullptr) {
        AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
        if (host.combat->current_combatant(host.combat->user, &combatant).code ==
            ANOMALY_STATUS_V1_OK) {
            state.player_handle = combatant.character.id;
        }
    }
    if (!CombatStreamAvailable(host)) return;
    const std::uint64_t latest =
        host.combat->latest_damage_sequence(host.combat->user);
    // 首次进入或序列被重置时，从当前序列起步，避免把历史伤害当成刚刚命中。
    if (state.damage_cursor == 0 ||
        state.damage_cursor > latest) {
        state.damage_cursor = latest;
        return;
    }
    int drained = 0;
    while (state.damage_cursor < latest && drained < 64) {
        AnomalyNteDamageEventV1 event{sizeof(event)};
        if (host.combat->next_damage_event(
                host.combat->user, state.damage_cursor, &event).code !=
            ANOMALY_STATUS_V1_OK) {
            break;
        }
        if (event.sequence <= state.damage_cursor) break;
        state.damage_cursor = event.sequence;
        ++drained;
        if (state.player_handle != 0 &&
            event.attacker.id == state.player_handle) {
            state.last_player_hit_at = now;
        }
    }
}

// 传送请求要当前 world 句柄，只能从 session 快照取（原实现在这里还会惰性重查 session 服务，
// 模块改为由 `host.session` 注入）。
bool TeleportToPosition(Host& host, const double (&position)[3]) noexcept {
    if (host.session == nullptr || host.player == nullptr ||
        host.teleport == nullptr || host.teleport->teleport == nullptr) {
        return false;
    }
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    if (host.session->snapshot(host.session->user, &session_snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        host.player->snapshot(host.player->user, &player_snapshot).code !=
            ANOMALY_STATUS_V1_OK) {
        return false;
    }
    if (session_snapshot.world.id == 0 || player_snapshot.handle.id == 0) return false;
    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    request.flags = 0;
    request.world = session_snapshot.world;
    request.player = player_snapshot.handle;
    request.position[0] = position[0];
    request.position[1] = position[1];
    request.position[2] = position[2] + kTeleportChestZOffset;
    return host.teleport->teleport(host.teleport->user, &request).code ==
        ANOMALY_STATUS_V1_OK;
}

}  // namespace

Result Tick(Host& host, State& state) noexcept {
    // 每帧重算：只有「打不动」（普攻/技能调用失败）才置真。调用方要区分它与「等服务/等位置/
    // 等目标数据」那几种等待——原实现只在打不动时关掉总开关并结束流程。
    state.attack_failed = false;
    if (host.navigation == nullptr || host.navigation->move_to_location == nullptr ||
        !GetPlayerState(host, state)) {
        ResetAutoCombatTarget(host, state);
        SetStatus(host, "等待战斗服务");
        return Result::unavailable;
    }
    double player_pos[3]{};
    if (!SnapshotPlayerPosition(host, player_pos) ||
        !std::isfinite(player_pos[0]) || !std::isfinite(player_pos[1]) ||
        !std::isfinite(player_pos[2])) {
        ResetAutoCombatTarget(host, state);
        SetStatus(host, "等待玩家位置");
        return Result::unavailable;
    }
    const auto now = std::chrono::steady_clock::now();
    PumpAutoCombatCombatStream(host, state);
    const double radius_cm = static_cast<double>(host.search_radius_m) * 100.0;
    const double radius_squared = radius_cm * radius_cm;
    if (state.target_valid) {
        const double cached_distance = CombatDistanceSquared(player_pos, state.target_pos);
        if (!std::isfinite(cached_distance) || cached_distance > radius_squared) {
            ResetAutoCombatTarget(host, state);
        } else if (cached_distance <=
                   kAutoCombatAttackRangeCm * kAutoCombatAttackRangeCm) {
            // 已经在打它了。若连着 kAutoCombatNoDamageGrace 一点伤害都没打出来，
            // 说明这个目标打不动：可能是还留在快照里的尸体，也可能是类名像怪、
            // 实际是道具的对象。拉黑并重新选靶。
            if (state.attack_since.time_since_epoch().count() == 0) {
                state.attack_since = now;
            }
            const bool hit_recent =
                state.last_player_hit_at.time_since_epoch().count() != 0 &&
                now - state.last_player_hit_at <=
                    std::chrono::milliseconds(static_cast<long long>(
                        host.no_damage_grace_seconds * 1000.0));
            if (hit_recent) state.target_hit = true;
            // 这里不再要求 `CombatStreamAvailable(host)`：伤害流不可用时那条判定整条失效，
            // 目标永远不换（尸体留在快照里，连旁边的活怪都不打）。代价是伤害流不可用时
            // 「没有伤害」对每个目标都成立，于是每打 grace 秒就换一次靶——由调用方按点位
            // 调 grace 来权衡（打硬骨头就调大）。
            if (!hit_recent &&
                now - state.attack_since > std::chrono::milliseconds(
                    static_cast<long long>(host.no_damage_grace_seconds * 1000.0))) {
                BlacklistAutoCombatTarget(state, state.target_handle,
                    state.target_pos, now, state.target_hit);
                ResetAutoCombatTarget(host, state);
            }
        } else {
            // 迟滞：只有明显离开攻击距离才清计时。原来只要越过攻击距离就清零，而目标距离
            // 在 5~6 m 附近抖动（日志实测 4~5 m 来回跳），于是「打不动」的计时永远攒不满，
            // 尸体既不会被拉黑、也不会被换掉。
            const double leave_squared =
                kAutoCombatLeaveRangeCm * kAutoCombatLeaveRangeCm;
            if (cached_distance > leave_squared) state.attack_since = {};
        }
    }
    // 每秒重新找一次最近的怪。这里不能再用 !target_valid 做条件：没有目标时它会让
    // 整段扫描每帧都跑（两个服务 × 每个类名一次分页，每帧几十次分页调用），
    // 这正是"开了自动战斗就掉帧"的来源。目标释放一律走 ResetAutoCombatTarget，
    // 而它会清空 next_target_update，所以"释放后立刻重新选靶"的行为不受影响。
    if (now >= state.next_target_update) {
        state.target_valid = false;
        state.scan_valid = false;
        if (FindMonsterClassIds(host, state)) {
            CombatTargetPick pick;
            pick.best_distance_squared = radius_squared;
            // 同一份逻辑扫两个来源，取二者中更近的那个。
            const auto skip_dead =
                [&state, now](const AnomalyNteEntitySnapshotV1& snap) {
                    return IsDeadAutoCombatTarget(state, snap, now);
                };
            const bool entities_ok = PickNearestMonsterFrom(
                host.entities, state.monster_class_name_ids, player_pos,
                radius_squared, skip_dead, pick);
            const bool actors_ok = PickNearestMonsterFrom(
                host.actors, state.monster_class_name_ids, player_pos,
                radius_squared, skip_dead, pick);
            state.scan_valid = entities_ok || actors_ok;
            if (pick.valid) {
                state.target_pos[0] = pick.pos[0];
                state.target_pos[1] = pick.pos[1];
                state.target_pos[2] = pick.pos[2];
                state.target_valid = true;
                if (state.target_handle.id != pick.handle.id ||
                    state.target_handle.generation !=
                        pick.handle.generation) {
                    state.target_handle = pick.handle;
                    state.attack_since = {};
                    state.target_hit = false;
                }
            }
        }
        state.next_target_update = now + std::chrono::milliseconds(1000);
        if (!state.scan_valid) {
            ResetAutoCombatTarget(host, state);
            SetStatus(host, "等待目标数据");
            return Result::unavailable;
        }
        if (!state.target_valid) {
            StopAutoCombatMovement(host, state);
            SetStatus(host, std::to_string(host.search_radius_m) + "米内无怪");
        }
    }
    // 完成判据（原来在副本流程里，CloneEnter/plugin.cpp:6868–6887）：必须先见过怪才开始
    // 计秒，所以「本来就没怪的点」不会被误判为完成——那种点由调用方自己的首次接敌超时收场。
    // 计数是秒级的：no_monster_tick_at 是下一次允许 +1 的时刻。
    if (state.target_valid) {
        state.met_monster = true;
        state.no_monster_seconds = 0;
        // 丢目标后要过满一秒才 +1：置 {} 会让第一帧立刻计数，「连续 5 秒」实际只剩约 4 秒。
        state.no_monster_tick_at = now + std::chrono::seconds(1);
    } else if (state.met_monster) {
        if (now >= state.no_monster_tick_at) {
            ++state.no_monster_seconds;
            state.no_monster_tick_at = now + std::chrono::seconds(1);
        }
        if (state.no_monster_seconds >= kAutoCombatNoMonsterSeconds) {
            // cleared 只在进入该状态的那一帧报一次；清空后的掉落拾取（原实现每秒都发一次，
            // CloneEnter/plugin.cpp:4571–4576）挂在这同一次跳变上，因此也只会发一次。
            if (!state.cleared_reported) {
                state.cleared_reported = true;
                if (host.loot_after_kill && host.pickup != nullptr) {
                    AnomalyNtePickupRequestV1 req{sizeof(req)};
                    req.radius = 2000.0;
                    req.maximum_items = 10;
                    static_cast<void>(host.pickup->request_nearby(host.pickup->user, &req));
                }
                return Result::cleared;
            }
        }
    }
    if (!state.target_valid) {
        state.attack_since = {};
        return Result::working;
    }
    const double dx = state.target_pos[0] - player_pos[0];
    const double dy = state.target_pos[1] - player_pos[1];
    const double dz = state.target_pos[2] - player_pos[2];
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    char status[128]{};
    std::snprintf(status, sizeof(status), "目标 %.1f米", dist / 100.0);
    SetStatus(host, status);
    if (dist > kAutoCombatAttackRangeCm) {
        // 怪物包围盒中心常常无法直接寻路抵达（悬空/湖面/特殊地形），游戏导航会原地不动。
        // 开发者模式下改用传送接近，绕过不可达的寻路；抬高 200cm 避免落进地面。
        if (host.developer_mode) {
            double tp_target[3] = {
                state.target_pos[0], state.target_pos[1],
                state.target_pos[2] + 200.0};
            if (TeleportToPosition(host, tp_target)) {
                StopAutoCombatMovement(host, state);
                return Result::working;
            }
        }
        // 寻路只取水平位置，高度用玩家当前高度。
        double nav_target[3] = {
            state.target_pos[0], state.target_pos[1], player_pos[2]};
        // 游戏原生寻路被每帧重复下发会反复重置（角色原地不动），
        // 因此按间隔先 stop 再下发，与 BoxAuto 的成熟做法一致。
        if (now >= state.nav_retry_at) {
            state.nav_retry_at = now + std::chrono::seconds(2);
            StopAutoCombatMovement(host, state);
            if (host.navigation->move_to_location(
                    host.navigation->user, nav_target).code == ANOMALY_STATUS_V1_OK) {
                state.moving = true;
            }
        }
    } else {
        StopAutoCombatMovement(host, state);
        if (host.melee_mode) {
            ++state.melee_attack_counter;
            bool do_normal_attack = true;
            if (state.melee_attack_counter >= 4) {
                state.melee_attack_counter = 0;
                if (ActivateSkillByInputId(host, 2)) {
                    do_normal_attack = false;
                }
            }
            if (do_normal_attack && !InvokeNormalAttack(host, state)) {
                // 原实现在这里关掉总开关（`auto_attack`）并退出副本流程（`one_key_active`）——
                // 那两项属于调用方，模块用 `state.attack_failed` 把"打不动"报回去（配合
                // `Result::unavailable`），调用方据此复现原来的动作。
                state.attack_failed = true;
                ResetAutoCombatTarget(host, state);
                return Result::unavailable;
            }
        } else {
            if (state.attack_is_melee) {
                static_cast<void>(ActivateSkillByInputId(host, 2));
            } else {
                static_cast<void>(ActivateSkillByInputId(
                    host, static_cast<std::int32_t>(host.test_input_id)));
            }
            state.attack_is_melee = !state.attack_is_melee;
        }
    }
    return Result::working;
}

void Reset(Host& host, State& state) noexcept {
    ResetAutoCombatTarget(host, state);
    state.monster_class_name_ids.clear();
    state.next_class_rescan = {};
    // 完成判据的簿记必须一起清：否则换到下一个点时，上一轮留下的 met_monster /
    // no_monster_seconds 会让一个本来就没怪的点在几秒内被判成 cleared。
    state.met_monster = false;
    state.no_monster_seconds = 0;
    state.no_monster_tick_at = {};
    state.cleared_reported = false;
}

}  // namespace anomaly::plugins::combat
