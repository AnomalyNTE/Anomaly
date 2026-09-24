#pragma once
// 共享自动战斗状态机。
//
// 来源：`plugins/CloneEnter/plugin.cpp` 4216–4641 的搬移（含怪物识别、选靶、攻击调用、
// 清空后的掉落拾取）。插件是独立 DLL，**不能互相调用**，所以复用只能靠共享源码——这就是
// 这个模块存在的原因；两份拷贝迟早会漂移。
//
// 模块拥有：从战斗流选靶、接近与攻击、「见过怪之后连续 5 秒无怪」的完成判据、清空后的
//           掉落拾取。
// 模块不拥有：点位列表、点与点之间的传送/寻路、标记、面板、每点策略（超时/跳过）——
//           调用方决定何时 tick 它，以及拿结果做什么。
//
// 模块不认识副本：原实现里 `GetCurrentCloneId` 只用于「副本实例变了就重置」的一次性簿记，
// 那件事由调用方在切换实例时调一次 `Reset` 完成。
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ue5.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace anomaly::plugins::combat {

// 原 `AutoCombatDeadTarget`：已死/打不动而被拉黑的目标，带过期时间。
struct DeadTarget final {
    AnomalyGenerationHandleV1 handle{};
    double pos[3]{};
    std::chrono::steady_clock::time_point until{};
};

// 原 CloneEnter `NormalAttackBinding`：普攻能力输入绑定的惰性解析结果（按 world/controller 失效）。
// 它必须能跨 tick 存活，所以放在 `State` 里而不是函数局部。
struct NormalAttackBinding final {
    std::uintptr_t world{};
    std::uintptr_t controller{};
    std::uintptr_t triggered{};
    std::uintptr_t completed{};
    std::uint8_t input_id{};
    std::int32_t input_param{};
    std::array<std::uint8_t, 8> pressed_value{};
    std::array<std::uint8_t, 8> released_value{};
};

// 原 CloneEnter `Context` 里的 `auto_combat_*` 等字段，搬到这里由调用方持有一个实例。
// 只放战斗自己的状态：`auto_attack`（总开关）、`last_clone_id`/`clone_check_done`（副本簿记）
// 留在各自插件。
struct State final {
    double target_pos[3]{};
    bool target_valid{false};
    bool scan_valid{false};
    bool moving{false};
    bool target_hit{false};
    std::uint64_t player_handle{0};
    std::uint64_t damage_cursor{0};
    AnomalyGenerationHandleV1 target_handle{};
    std::chrono::steady_clock::time_point nav_retry_at{};
    std::chrono::steady_clock::time_point last_player_hit_at{};
    std::chrono::steady_clock::time_point attack_since{};
    std::vector<DeadTarget> dead_targets;
    std::uint64_t monster_class_id{0};
    std::vector<std::uint32_t> monster_class_name_ids;
    std::chrono::steady_clock::time_point next_class_rescan{};
    // 选靶节流点（原 `next_target_update`）：目标扫描按秒做，不是每帧。
    std::chrono::steady_clock::time_point next_target_update{};
    // 普攻输入绑定缓存（原 `normal_attack`）与两种攻击模式的轮换（原 `attack_is_melee`、
    // `melee_attack_counter`）。
    NormalAttackBinding normal_attack{};
    bool attack_is_melee{true};
    std::uint32_t melee_attack_counter{0};
    // 玩家状态缓存（原 `g_world_address` / `player_state` / `controller` / `cached_world`）：
    // `GetPlayerState` 靠它们判断世界有没有换，换世界时走一次 `Reset`。
    std::uintptr_t g_world_address{0};
    std::uintptr_t player_state{0};
    std::uintptr_t controller{0};
    std::uintptr_t cached_world{0};
    // 原 `g_objects_address`：GObjects 签名解析缓存（`EnsureGObjects` 用）。
    std::uintptr_t g_objects_address{0};
    // 完成判据，对应副本流程里的 `one_key_met_monster` / `one_key_no_monster_seconds`：
    // 必须先见过怪才开始计秒，所以「本来就没怪的点」不会被误判为完成。
    bool met_monster{false};
    std::uint32_t no_monster_seconds{0};
    std::chrono::steady_clock::time_point no_monster_tick_at{};
    // 本点是否已经报过 `cleared`。进入 `cleared` 的那一帧置真，`Reset` 清零；
    // `cleared` 与随之发出的那一次掉落拾取都由它保证「只发生一次」。
    bool cleared_reported{false};
    // 本帧是否因为「打不动」而 `unavailable`（普攻/技能调用失败），每帧重算。
    // 调用方靠它区分「打不动」与「等服务/等位置/等目标数据」：原实现只在打不动时关掉总开关
    // 并结束副本流程，其余几种只是等待。
    bool attack_failed{false};
};

// 调用方注入的宿主：服务指针 + 策略 + 状态回显。
struct Host final {
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteNavigationServiceV1* navigation{};
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyNteEntitiesServiceV1* entities{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyNtePlayerServiceV1* player{};
    // 开发者模式下「寻路到不了就传送接近」用；为空则只走导航。
    const AnomalyNtePlayerTeleportServiceV1* teleport{};
    // 传送请求要求当前 world 句柄，只能从 session 快照取。
    const AnomalyNteSessionServiceV1* session{};
    // 签名服务：`GetPlayerState` / `EnsureGObjects` 靠它解析 GWorld / GObjects。
    const AnomalySignatureServiceV1* signature{};
    // 技能流：`ActivateSkillByInputId` 用（原实现每次调用时现查这三个服务）。
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* skill_invocation{};
    // 为空则不拾取。
    const AnomalyNtePickupServiceV1* pickup{};
    std::uint32_t search_radius_m{50};
    // 攻击目标多久没有造成伤害就当作「打不动」（尸体/道具）：模块据此拉黑并换靶。
    // 注意这条判定不要求伤害流可用——伤害流不可用时它退化成「打了这么久就换靶」。
    double no_damage_grace_seconds{3.0};
    bool developer_mode{false};
    bool loot_after_kill{true};
    // 攻击策略（原 `melee_mode` / `test_input_id`）：近战模式走普攻，否则轮流放技能。
    bool melee_mode{false};
    std::uint32_t test_input_id{15};
    // 模块不直接写面板：状态字符串经这里回到调用方。
    void (*set_status)(void* user, const std::string& text) noexcept{};
    void* status_user{};
};

enum class Result : std::uint8_t {
    // 仍在选靶/接近/交战，或见过怪但无怪时长还没满 5 秒。
    working,
    // 见过怪，且连续 5 秒无怪：本点完成（拾取已在内部发出）。
    cleared,
    // 战斗流或导航不可用，或本帧拿不到目标数据 / 出不了手：调用方决定等待还是放弃
    // （本模块不做超时）。
    unavailable,
};

// 每帧调用一次。`cleared` 只会在进入该状态的那一帧返回一次。
Result Tick(Host& host, State& state) noexcept;

// 清空战斗状态（换点、换副本实例、关闭自动战斗时调用）。
void Reset(Host& host, State& state) noexcept;

}  // namespace anomaly::plugins::combat
