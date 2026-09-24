// 自动战斗共享模块（plugins/common/combat/auto_combat.cpp）的真实夹具。
//
// 模块只通过 `Host` 里的纯 C 函数指针表接触游戏：签名服务给 GWorld，player / entities /
// actors 服务给快照，combat 流给伤害，pickup 服务收掉落请求。这里把整张表换成进程内的假实现
// （一段按模块常量摆好的假内存 + 假服务结构），于是「怪物出现 → 被看到 → 消失 → 5 秒后
// cleared」这条完整路径可以在没有游戏进程的情况下用真实时钟跑出来。
//
// 模块内部读 std::chrono::steady_clock，时间无法快进，所以等待都是真实等待；每个等待循环
// 都有时间上限，测试不会挂死。四个互不相干的场景各自持有夹具与状态机，并行执行，总时长由
// 最长的那个场景（10 秒无怪运行）决定。
#include "../auto_combat.hpp"

#include "anomaly/sdk/cpp.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using anomaly::plugins::combat::Host;
using anomaly::plugins::combat::Reset;
using anomaly::plugins::combat::Result;
using anomaly::plugins::combat::State;
using anomaly::plugins::combat::Tick;

constexpr std::chrono::milliseconds kTickInterval{25};
// 一次「怪物消失 → cleared」的硬上限：判据是 5 秒，多给 3 秒余量。
constexpr std::chrono::seconds kClearCap{8};

void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

AnomalyStatusV1 Status(std::uint32_t code) {
    return AnomalyStatusV1{code, 0, AnomalyStringViewV1{nullptr, 0}};
}

void TickSleep() { std::this_thread::sleep_for(kTickInterval); }

// 假内存布局。模块用固定偏移读 GWorld → GameInstance → LocalPlayer → Controller →
// PlayerState，夹具按同样的偏移摆出一棵真实可读的对象树。
enum ArenaOffset : std::size_t {
    kWorldObject = 0x000,
    kGameInstance = 0x400,
    kPlayersArray = 0x800,
    kLocalPlayer = 0x840,
    kController = 0x900,
    kPlayerState = 0xC00,
    kInstruction = 0x1000,  // 假的 RIP 相对指令，+3 处放位移
    kGWorldSlot = 0x1100,   // 位移解析出的地址，里面放 GWorld 指针
    kArenaBytes = 0x1200,
};

// 模块侧的对象偏移（见 auto_combat.cpp 顶部常量）。
constexpr std::size_t kWorldGameInstanceOffset = 560;
constexpr std::size_t kGameInstanceLocalPlayersOffset = 56;
constexpr std::size_t kLocalPlayerControllerOffset = 48;
constexpr std::size_t kControllerPlayerStateOffset = 720;

struct Arena {
    alignas(8) std::array<std::uint8_t, kArenaBytes> bytes{};
    template <typename T>
    void Store(std::size_t at, T value) {
        std::memcpy(bytes.data() + at, &value, sizeof(T));
    }
    std::uintptr_t Base() const {
        return reinterpret_cast<std::uintptr_t>(bytes.data());
    }
};

struct Fixture {
    // 一条实体 / actor 快照。
    struct Row {
        std::uint32_t flags{};
        std::uint64_t class_id{};
        std::uint32_t class_name_id{};
        double center[3]{};
        double extent[3]{};
        std::uint64_t handle_id{};
    };

    // entities 与 actors 两个服务的读取接口完全一致，各持一份来源。
    struct Source {
        std::vector<Row> rows;
        std::vector<std::pair<std::uint64_t, std::string>> names;
    };

    Arena arena;
    Source entity_source;
    Source actor_source;
    AnomalySignatureServiceV1 signature{};
    AnomalyNtePlayerServiceV1 player{};
    AnomalyNteNavigationServiceV1 navigation{};
    AnomalyNteEntitiesServiceV1 entities{};
    AnomalyNteActorsServiceV1 actors{};
    AnomalyNteCombatServiceV1 combat{};
    AnomalyNtePickupServiceV1 pickup{};
    Host host{};
    State state{};
    double player_position[3]{0.0, 0.0, 0.0};
    std::uint32_t navigation_moves{};
    std::uint32_t pickup_requests{};
    std::uint32_t status_calls{};

    Fixture();

    void AddMonster(double x, double y, double z) {
        Row row;
        row.class_id = 1;
        row.class_name_id = 100;
        row.center[0] = x;
        row.center[1] = y;
        row.center[2] = z;
        row.extent[0] = row.extent[1] = row.extent[2] = 100.0;
        row.handle_id = 7;
        entity_source.rows.push_back(row);
    }
    void ClearMonsters() { entity_source.rows.clear(); }
};

AnomalyStatusV1 ANOMALY_CALL ResolveSignature(void* user, AnomalyStringViewV1,
                                              AnomalyStringViewV1, AnomalyStringViewV1,
                                              std::uintptr_t* address) {
    // 模块把返回的地址当指令，读 +3 的位移后算出 GWorld 槽位。
    *address = static_cast<Fixture*>(user)->arena.Base() + kInstruction;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL PlayerSnapshot(void* user, AnomalyNtePlayerSnapshotV1* snapshot) {
    const auto& fx = *static_cast<Fixture*>(user);
    snapshot->flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
    snapshot->handle = AnomalyGenerationHandleV1{1, 1};
    snapshot->sequence = 1;
    snapshot->position[0] = fx.player_position[0];
    snapshot->position[1] = fx.player_position[1];
    snapshot->position[2] = fx.player_position[2];
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL MoveToLocation(void* user, const double[3]) {
    ++static_cast<Fixture*>(user)->navigation_moves;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL StopMovement(void*) { return anomaly::sdk::Ok(); }

AnomalyStatusV1 ANOMALY_CALL RequestNearby(void* user, const AnomalyNtePickupRequestV1*) {
    ++static_cast<Fixture*>(user)->pickup_requests;
    return anomaly::sdk::Ok();
}

void SetStatusCallback(void* user, const std::string&) noexcept {
    ++static_cast<Fixture*>(user)->status_calls;
}

AnomalyStatusV1 ANOMALY_CALL EntityFrame(void* user, AnomalyNteEntityFrameV1* frame) {
    const auto& source = *static_cast<Fixture::Source*>(user);
    frame->flags = 0;
    frame->generation = 1;
    frame->sequence = 1;
    frame->entity_count = static_cast<std::uint32_t>(source.rows.size());
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL ClassNameUtf8(void* user, std::uint64_t class_id, char* destination,
                                           std::size_t* inout_size) {
    const auto& source = *static_cast<Fixture::Source*>(user);
    for (const auto& [id, name] : source.names) {
        if (id != class_id) continue;
        const std::size_t needed = name.size() + 1;
        if (destination == nullptr) {
            *inout_size = needed;
            return anomaly::sdk::Ok();
        }
        if (*inout_size < needed) {
            *inout_size = needed;
            return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
        }
        std::memcpy(destination, name.c_str(), needed);
        *inout_size = needed;
        return anomaly::sdk::Ok();
    }
    return Status(ANOMALY_STATUS_V1_NOT_FOUND);
}

AnomalyStatusV1 ANOMALY_CALL EntityPage(void* user, const AnomalyNteEntityPageRequestV1* request,
                                        AnomalyNteEntitySnapshotV1* destination,
                                        AnomalyNteEntityPageResultV1* result) {
    const auto& source = *static_cast<Fixture::Source*>(user);
    // class_name_id 为零是通配（类名收集），否则按类名过滤（选靶）。
    std::vector<const Fixture::Row*> matches;
    for (const auto& row : source.rows) {
        if (request->class_name_id != 0 && row.class_name_id != request->class_name_id) continue;
        if ((row.flags & request->excluded_flags) != 0) continue;
        matches.push_back(&row);
    }
    const std::uint32_t offset = request->offset;
    std::uint32_t returned = 0;
    while (returned < request->capacity &&
           static_cast<std::size_t>(offset) + returned < matches.size()) {
        const auto& row = *matches[static_cast<std::size_t>(offset) + returned];
        auto& snapshot = destination[returned];
        snapshot.struct_size = static_cast<std::uint32_t>(sizeof(snapshot));
        snapshot.flags = row.flags;
        snapshot.handle = AnomalyGenerationHandleV1{row.handle_id, 1};
        snapshot.entity_id = row.handle_id;
        snapshot.class_id = row.class_id;
        snapshot.entity_name_id = 0;
        snapshot.class_name_id = row.class_name_id;
        for (int axis = 0; axis < 3; ++axis) {
            snapshot.bounds_center[axis] = row.center[axis];
            snapshot.bounds_extent[axis] = row.extent[axis];
        }
        ++returned;
    }
    result->flags = 0;
    result->generation = request->generation;
    result->sequence = 1;
    result->total_matches = static_cast<std::uint32_t>(matches.size());
    result->returned = returned;
    result->next_offset = offset + returned;
    return anomaly::sdk::Ok();
}

Fixture::Fixture() {
    const std::uintptr_t base = arena.Base();
    // 让 kGWorldPattern 的 RIP 解析落在 kGWorldSlot：resolved = instruction + 7 + displacement。
    constexpr std::int32_t displacement =
        static_cast<std::int32_t>(kGWorldSlot - kInstruction - 7);
    arena.Store<std::int32_t>(kInstruction + 3, displacement);
    arena.Store<std::uintptr_t>(kGWorldSlot, base + kWorldObject);
    arena.Store<std::uintptr_t>(kWorldObject + kWorldGameInstanceOffset, base + kGameInstance);
    arena.Store<std::uintptr_t>(kGameInstance + kGameInstanceLocalPlayersOffset,
                                base + kPlayersArray);
    arena.Store<std::int32_t>(kGameInstance + kGameInstanceLocalPlayersOffset + 8, 1);
    arena.Store<std::uintptr_t>(kPlayersArray, base + kLocalPlayer);
    arena.Store<std::uintptr_t>(kLocalPlayer + kLocalPlayerControllerOffset, base + kController);
    arena.Store<std::uintptr_t>(kController + kControllerPlayerStateOffset, base + kPlayerState);

    entity_source.names.emplace_back(1, "mon_001_BP_C");

    signature.struct_size = sizeof(signature);
    signature.service_version = 1;
    signature.user = this;
    signature.resolve = &ResolveSignature;

    player.struct_size = sizeof(player);
    player.service_version = 1;
    player.user = this;
    player.snapshot = &PlayerSnapshot;

    navigation.struct_size = sizeof(navigation);
    navigation.service_version = 1;
    navigation.user = this;
    navigation.move_to_location = &MoveToLocation;
    navigation.stop_movement = &StopMovement;

    entities.struct_size = sizeof(entities);
    entities.service_version = 1;
    entities.user = &entity_source;
    entities.frame = &EntityFrame;
    entities.class_name_utf8 = &ClassNameUtf8;
    entities.page = &EntityPage;

    actors.struct_size = sizeof(actors);
    actors.service_version = 1;
    actors.user = &actor_source;
    actors.frame = &EntityFrame;
    actors.class_name_utf8 = &ClassNameUtf8;
    actors.page = &EntityPage;

    // 战斗服务在线但没有伤害流：模块因此不做「打不出伤害就拉黑」的判定，本夹具的目标
    // 始终待在攻击距离外，选靶结果稳定。
    combat.struct_size = sizeof(combat);
    combat.service_version = 1;
    combat.user = this;

    pickup.struct_size = sizeof(pickup);
    pickup.service_version = 1;
    pickup.user = this;
    pickup.request_nearby = &RequestNearby;

    host.navigation = &navigation;
    host.signature = &signature;
    host.player = &player;
    host.entities = &entities;
    host.actors = &actors;
    host.combat = &combat;
    host.pickup = &pickup;
    host.search_radius_m = 50;
    host.loot_after_kill = true;
    host.set_status = &SetStatusCallback;
    host.status_user = this;
}

// 一次完整循环的观测结果。
struct CycleReport {
    bool saw_target{};
    bool cleared{};
    std::uint32_t pickups_at_clear{};
    double seconds_to_clear{-1.0};
    bool probed_at_four_seconds{};
    double four_second_probe_at{-1.0};
    bool working_at_four_seconds{};
};

// 走完一次「怪物出现 → 被看到 → 消失 → 5 秒后 cleared」。怪物放在玩家正东 20 米
// （攻击距离 6 米之外），所以模块只寻路、不出手，选靶结果不受伤害流影响。
CycleReport DriveCycle(Fixture& fx) {
    CycleReport report;
    fx.AddMonster(2000.0, 0.0, 0.0);
    Clock::time_point last_seen{};
    // 1) 等状态机看见怪（选靶按秒节流，给 3 秒上限）。
    const auto appear_deadline = Clock::now() + std::chrono::seconds(3);
    while (!fx.state.target_valid) {
        Check(Clock::now() < appear_deadline, "a monster inside the radius was never picked up");
        const auto tick_started = Clock::now();
        Check(Tick(fx.host, fx.state) == Result::working,
              "a visible monster must not end the run");
        if (fx.state.target_valid) last_seen = tick_started;
        TickSleep();
    }
    report.saw_target = true;
    Check(fx.state.met_monster, "seeing a monster must set met_monster");

    // 2) 怪物消失，等状态机自己发现（缓存的目标要等下一次选靶才失效）。
    fx.ClearMonsters();
    const auto gone_deadline = Clock::now() + std::chrono::seconds(3);
    while (fx.state.target_valid) {
        Check(Clock::now() < gone_deadline, "the state machine never dropped a removed monster");
        const auto tick_started = Clock::now();
        Check(Tick(fx.host, fx.state) == Result::working,
              "the run ended while the monster was still cached");
        if (fx.state.target_valid) last_seen = tick_started;
        TickSleep();
    }
    Check(last_seen.time_since_epoch().count() != 0, "no tick ever saw the monster");

    // 3) 一路 tick 到 cleared。last_seen 记的是「最后一帧仍然看得见」那一帧的开始时刻，
    //    比模块内部的取样时刻更早，所以下面的 5 秒下界是严格的。
    const auto clear_deadline = last_seen + kClearCap;
    while (true) {
        Check(Clock::now() < clear_deadline, "cleared never arrived after the monster left");
        const Result result = Tick(fx.host, fx.state);
        const double since_last_seen =
            std::chrono::duration<double>(Clock::now() - last_seen).count();
        if (!report.probed_at_four_seconds && since_last_seen >= 4.0) {
            report.probed_at_four_seconds = true;
            report.four_second_probe_at = since_last_seen;
            report.working_at_four_seconds = result == Result::working;
        }
        if (result == Result::cleared) {
            report.cleared = true;
            report.pickups_at_clear = fx.pickup_requests;
            report.seconds_to_clear = since_last_seen;
            break;
        }
        Check(result == Result::working, "an unexpected result arrived before cleared");
        TickSleep();
    }
    Check(report.probed_at_four_seconds, "the four-second probe never ran");
    return report;
}

// 断言 2：一个从头到尾没有怪的点必须一直 working，永远不会 cleared。
void ScenarioWithoutMonster() {
    Fixture fx;
    const auto started = Clock::now();
    std::uint32_t ticks = 0;
    while (true) {
        const Result result = Tick(fx.host, fx.state);
        Check(result == Result::working,
              "a point with no monster must never report cleared or unavailable");
        ++ticks;
        if (Clock::now() - started >= std::chrono::seconds(10)) break;
        Check(ticks < 5000, "the monster-free run did not converge");
        TickSleep();
    }
    Check(ticks >= 100, "the monster-free run was not ticked continuously");
    Check(!fx.state.met_monster, "a run that never saw a monster must not set met_monster");
    Check(!fx.state.cleared_reported, "a run that never saw a monster must not report cleared");
    Check(Clock::now() - started >= std::chrono::seconds(10),
          "the monster-free run must span at least ten seconds");
}

// 断言 3、4、5、6a、7、8。
void ScenarioClearCycle() {
    Fixture fx;
    const CycleReport report = DriveCycle(fx);
    Check(report.saw_target, "the monster was never seen");
    Check(report.cleared, "cleared never arrived after the monster left");

    // 断言 4：约 4 秒仍 working，5 秒后 cleared。
    Check(report.working_at_four_seconds,
          "the run was not still working four seconds after the monster left");
    Check(report.four_second_probe_at >= 4.0 && report.four_second_probe_at < 4.5,
          "the four-second probe did not land near four seconds");
    Check(report.seconds_to_clear >= 5.0, "cleared arrived before five monster-free seconds");
    Check(report.seconds_to_clear <= 6.5, "cleared arrived too late after the monster left");
    Check(fx.navigation_moves > 0, "a distant monster must drive the navigation service");
    Check(fx.status_calls > 0, "the module must report through the injected status callback");

    // 断言 6a：整轮下来掉落请求恰好一次。
    Check(report.pickups_at_clear == 1, "cleared must request loot exactly once");

    // 断言 5：cleared 只报一次。
    const auto tail_deadline = Clock::now() + std::chrono::milliseconds(2500);
    while (Clock::now() < tail_deadline) {
        Check(Tick(fx.host, fx.state) == Result::working, "cleared must be reported exactly once");
        TickSleep();
    }
    Check(fx.pickup_requests == 1, "loot must be requested exactly once across the whole run");

    // 断言 7：Reset 结束这一轮，之后再 tick 不会又报 cleared。
    Reset(fx.host, fx.state);
    const auto reset_deadline = Clock::now() + std::chrono::milliseconds(1500);
    while (Clock::now() < reset_deadline) {
        Check(Tick(fx.host, fx.state) == Result::working,
              "a reset run must not report cleared again");
        TickSleep();
    }
    Check(!fx.state.met_monster, "Reset must end the completed run");

    // 断言 8：先把状态重新弄脏，再验证 Reset 清空了它。
    fx.AddMonster(2000.0, 0.0, 0.0);
    const auto dirty_deadline = Clock::now() + std::chrono::seconds(3);
    while (!fx.state.target_valid) {
        Check(Clock::now() < dirty_deadline, "the monster was not re-acquired after Reset");
        Check(Tick(fx.host, fx.state) == Result::working, "re-acquiring a monster must stay working");
        TickSleep();
    }
    Check(!fx.state.monster_class_name_ids.empty(),
          "the class-name cache was empty before Reset");
    Check(fx.state.next_class_rescan.time_since_epoch().count() != 0,
          "the class rescan point was never set");
    Check(fx.state.met_monster, "met_monster was not set before Reset");
    Reset(fx.host, fx.state);
    Check(!fx.state.target_valid, "Reset must clear target_valid");
    Check(fx.state.monster_class_name_ids.empty(), "Reset must clear the class-name cache");
    Check(fx.state.next_class_rescan.time_since_epoch().count() == 0,
          "Reset must zero next_class_rescan");
    Check(!fx.state.met_monster, "Reset must clear met_monster");
    Check(fx.state.no_monster_seconds == 0, "Reset must clear no_monster_seconds");
}

// 断言 6b：loot_after_kill = false 时不发掉落请求。
void ScenarioLootDisabled() {
    Fixture fx;
    fx.host.loot_after_kill = false;
    const CycleReport report = DriveCycle(fx);
    Check(report.cleared, "the run must still clear with looting disabled");
    Check(fx.pickup_requests == 0, "loot_after_kill=false must not request any pickup");
}

// 断言 6c：pickup 为空时既不能崩，也不能发掉落请求。
void ScenarioPickupUnavailable() {
    Fixture fx;
    fx.host.loot_after_kill = true;
    fx.host.pickup = nullptr;
    const CycleReport report = DriveCycle(fx);
    Check(report.cleared, "the run must still clear without a pickup service");
    Check(fx.pickup_requests == 0, "a missing pickup service must not be called");
}

// 断言 9：`attack_failed` 只标记「打不动」，其余 unavailable 都是等待。
// 四种 unavailable 的来源在夹具里都能分别驱动到，所以这里逐条比对，而不是只测一条。
void ScenarioAttackFailedFlag() {
    // 9a：等待战斗服务（navigation 缺失）。
    {
        Fixture fx;
        fx.host.navigation = nullptr;
        Check(Tick(fx.host, fx.state) == Result::unavailable,
              "a host without navigation must report unavailable");
        Check(!fx.state.attack_failed,
              "waiting for combat services must not be reported as a failed attack");
    }
    // 9b：等待玩家位置（player 服务缺失）。
    {
        Fixture fx;
        fx.host.player = nullptr;
        Check(Tick(fx.host, fx.state) == Result::unavailable,
              "a host without a player service must report unavailable");
        Check(!fx.state.attack_failed,
              "waiting for the player position must not be reported as a failed attack");
    }
    // 9c：等待目标数据（entities 与 actors 都缺失，选靶拿不到数据）。
    {
        Fixture fx;
        fx.host.entities = nullptr;
        fx.host.actors = nullptr;
        Check(Tick(fx.host, fx.state) == Result::unavailable,
              "a host without entity sources must report unavailable");
        Check(!fx.state.attack_failed,
              "waiting for target data must not be reported as a failed attack");
    }
    // 9d：打不动 —— 怪在 6 米攻击范围内，夹具不提供任何能力输入绑定，普攻必然解析失败。
    {
        Fixture fx;
        fx.host.melee_mode = true;
        fx.AddMonster(300.0, 0.0, 0.0);
        Check(Tick(fx.host, fx.state) == Result::unavailable,
              "a normal attack that cannot be resolved must report unavailable");
        Check(fx.state.attack_failed, "a failed normal attack must set attack_failed");
        // 独立证据：这一帧确实走了攻击分支而不是寻路分支（20 米外的场景会调导航）。
        Check(fx.state.met_monster, "the in-range monster was never seen");
        Check(fx.navigation_moves == 0, "an in-range monster must not start navigation");
        // 标志每帧重算：下一帧不再打不动就必须归零。
        fx.ClearMonsters();
        Check(Tick(fx.host, fx.state) == Result::working,
              "a monster-free frame after a failed attack must keep working");
        Check(!fx.state.attack_failed, "attack_failed must be recomputed every frame");
    }
}

}  // namespace

int main() {
    // 断言 1：服务缺失 → unavailable。
    {
        Host missing{};
        State missing_state{};
        Check(Tick(missing, missing_state) == Result::unavailable,
              "a host without services must report unavailable");
    }

    // 断言 9：快速，不占等待时间，直接在主线程跑。
    ScenarioAttackFailedFlag();

    // 长场景互不相干，并行跑；各自持有夹具与状态机。
    std::thread no_monster(ScenarioWithoutMonster);
    std::thread loot_disabled(ScenarioLootDisabled);
    std::thread pickup_missing(ScenarioPickupUnavailable);
    ScenarioClearCycle();
    no_monster.join();
    loot_disabled.join();
    pickup_missing.join();

    std::cout << "PASS unavailable host, monster-free run, target acquisition, five-second clear, "
                 "single clear, single loot request, reset and reset bookkeeping, "
                 "attack_failed only on a failed attack\n";
    return 0;
}
