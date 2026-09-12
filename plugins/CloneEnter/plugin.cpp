#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/nte.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
constexpr std::ptrdiff_t kGObjectsAddend = -16;
constexpr std::uint32_t kRipDisplacementOffset = 3;
constexpr std::uint32_t kRipInstructionSize = 7;

constexpr std::ptrdiff_t kObjectItemsOffset = 16;
constexpr std::ptrdiff_t kObjectCountOffset = 36;
constexpr std::ptrdiff_t kObjectNumChunksOffset = 44;
constexpr std::uint32_t kObjectChunkSize = 65536;
constexpr std::uint32_t kObjectItemStride = 24;

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

// HT_CloneSystemDataAsset 内部偏移
constexpr std::ptrdiff_t kDataAssetCloneSystemDataOffset = 56;
constexpr std::ptrdiff_t kDataAssetCloneEnterOffset = 72;

// CloneSystemContain（DT_CloneEnter 行）字段偏移
constexpr std::ptrdiff_t kContainCloneSystemIDsOffset = 64;

// CloneSystemData（DT_CloneSystemData 行）字段偏移
constexpr std::ptrdiff_t kDataCloneTypeOffset = 73;
constexpr std::ptrdiff_t kDataSubNodesOffset = 88;

// CloneSystemSubNodeData 字段偏移
constexpr std::size_t kSubNodeSize = 160;
constexpr std::ptrdiff_t kSubNodeDifficultyOffset = 124;
constexpr std::ptrdiff_t kSubNodeSpawnOffset = 136;
constexpr std::ptrdiff_t kUStructPropertyLinkOffset = 112;
constexpr std::ptrdiff_t kFFieldNameOffset = 32;
constexpr std::ptrdiff_t kFFieldClassOffset = 8;
constexpr std::ptrdiff_t kFPropertyElementSizeOffset = 52;
constexpr std::ptrdiff_t kFPropertyOffsetInternalOffset = 68;
constexpr std::ptrdiff_t kFPropertyPropertyLinkNextOffset = 72;
constexpr std::ptrdiff_t kFStructPropertyStructOffset = 112;
constexpr std::ptrdiff_t kFArrayPropertyInnerOffset = 112;
constexpr std::ptrdiff_t kSubNodeTeamLevelOffset = 128;

struct FNamePair {
    std::uint32_t cmp{};
    std::uint32_t number{};
};

struct SubInfo {
    FNamePair sub;
    std::uint8_t type;
    std::vector<std::int32_t> levels;
    FNamePair match;
};

struct CloneEntry {
    std::string contain_id;
    FNamePair contain_fname;
    std::vector<FNamePair> sub_ids;
    std::vector<SubInfo> sub_infos;
};

struct SubDisplay {
    const char* name;
    std::uint32_t sub_index;
};

struct EntryDisplay {
    std::uint32_t entry_index;
    const char* name;
    SubDisplay subs[8];
    std::uint32_t sub_count;
    std::uint32_t min_level;
    std::uint32_t max_level;
};

constexpr EntryDisplay kDisplays[] = {
    {1, "资源副本", {{"角色经验",3},{"弧盘经验",4},{"甲硬币",5}}, 3, 1, 6},
    {2, "异能材料副本", {{"小心鸽子",0},{"扑克茶会",1},{"惊喜派对",2},{"心电感应",3},{"越狱艺术",4}}, 5, 1, 7},
    {3, "弧盘突破副本", {{"苹果核",0},{"螺旋乐",1},{"液态梦",2},{"冷甜点",3},{"戏剧芯",4}}, 5, 1, 7},
    {4, "空暮副本", {{"失落光芒和迪亚波",0},{"恶魔之血和街头拳王",1},{"森林萤火和真红",2},{"缇娅和守卫王国",3},{"音速蓝刺猬和静谧山庄",4},{"影之信条和小小大冒险",5}}, 6, 1, 6},
    {5, "周本墨菲克斯", {{"",0}}, 1, 1, 7},
    {6, "周本永不谢幕", {{"",0}}, 1, 1, 7},
    {9, "抢银行", {{"",0}}, 1, 1, 1},
    {13, "周本讨债人", {{"",0}}, 1, 1, 7},
};

constexpr std::size_t kDisplayCount = sizeof(kDisplays) / sizeof(kDisplays[0]);

struct NormalAttackBinding {
    std::uintptr_t world{};
    std::uintptr_t controller{};
    std::uintptr_t triggered{};
    std::uintptr_t completed{};
    std::uint8_t input_id{};
    std::int32_t input_param{};
    std::array<std::uint8_t, 8> pressed_value{};
    std::array<std::uint8_t, 8> released_value{};
};

// 大世界怪死后尸体仍留在实体/角色快照里（位置、类名都不变），会被每轮重新选为
// "最近的怪"，导致对着空气一直打。框架没有暴露死亡位，因此改用唯一可靠的信号：
// 玩家打出的伤害流。在攻击距离内持续打不出伤害，就把该目标判为尸体并拉黑一段时间，
// 让选靶跳过它，直到它真正从快照里消失。
struct AutoCombatDeadTarget {
    AnomalyGenerationHandleV1 handle{};
    double pos[3]{};
    std::chrono::steady_clock::time_point until{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyInputServiceV1* input{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyNteMapLandmarksServiceV1* map_landmarks{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyNteEntitiesServiceV1* entities{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* skill_invocation{};
    const AnomalyNteNavigationServiceV1* navigation{};
    const AnomalyNtePlayerTeleportServiceV1* teleport{};
    const AnomalyNtePickupServiceV1* pickup{};
    std::string cache_path;
    std::uintptr_t g_objects_address{};
    std::uintptr_t g_world_address{};
    std::uintptr_t controller{};
    std::uintptr_t player_state{};
    std::uintptr_t cached_world{0};

    std::uint32_t entry_index{1};
    std::uint32_t display_index{3};
    std::uint32_t sub_choice_index{0};
    std::uint32_t sub_index{1};
    std::uint32_t level_index{1};
    std::uint32_t wait_seconds{4};
    std::uint32_t test_input_id{15};

    std::vector<CloneEntry> entries;
    bool entries_loaded{false};
    bool cache_validated{false};
    std::uintptr_t data_asset{0};
    std::vector<std::pair<FNamePair, std::uintptr_t>> system_rows;
    std::uintptr_t player_state_cls{0};
    std::uintptr_t enter_fn{0};
    std::uintptr_t get_cur_clone_id_fn{0};

    std::atomic_bool enter_pending{};
    std::atomic_bool load_pending{};
    std::atomic_bool exit_pending{};
    std::atomic_bool scan_pending{};
    std::atomic_bool dump_funcs_pending{};
    std::atomic_bool kill_pending{};
    std::atomic_bool params_pending{};
    std::atomic_bool dmg_params_pending{};
    std::atomic_bool ge_scan_pending{};
    std::atomic_bool player_funcs_pending{};
    std::atomic_bool attrset_pending{};
    std::atomic_bool buff_pending{};
    std::atomic_bool modify_pending{};
    std::atomic_bool ge_struct_pending{};
    std::atomic_bool modifier_info_pending{};
    std::atomic_bool modifiers_pending{};
    std::atomic_bool attr_values_pending{};
    std::atomic_bool ht_attr_class_pending{};
    std::atomic_bool damage_attrs_pending{};
    std::atomic_bool modifier_attr_pending{};
    std::atomic_bool boost_damage_pending{};
    std::atomic_bool nettarget_pending{};
    std::atomic_bool charfornet_pending{};
    std::atomic_bool player_damage_func_pending{};
    std::atomic_bool any_damage_pending{};
    std::atomic_bool kill_self_pending{};
    std::atomic_bool death_event_pending{};
    std::atomic_bool nettarget_damage_pending{};
    std::atomic_bool set_hp_one_pending{};
    std::atomic_bool hp_attrs_pending{};
    std::atomic_bool monster_attrset_pending{};
    std::atomic_bool scan_hp_pending{};
    std::atomic_bool monster_hp_values_pending{};
    std::atomic_bool player_state_funcs_pending{};
    std::atomic_bool clone_rpc_params_pending{};
    std::atomic_bool clone_enums_pending{};
    std::atomic_bool enum_values_pending{};
    std::atomic_bool passpermit_award_pending{};
    std::atomic_bool scan_clone_classes_pending{};
    std::atomic_bool clone_manager_funcs_pending{};
    std::atomic_bool activate_skill_pending{};
    std::atomic_bool combat_target_pending{};
    std::atomic_bool monster_classes_pending{};
    std::atomic_bool clone_monster_info_pending{};
    std::atomic_bool monster_assets_pending{};
    std::atomic_bool entity_classes_pending{};
    std::atomic_bool test_skill_pending{};
    std::atomic_bool claim_reward_pending{};
    std::atomic_bool claim_double_pending{};
    std::atomic_bool chest_choices_pending{};
    std::atomic_bool chest_funcs_pending{};
    std::atomic_bool developer_mode{};
    std::atomic_bool reward_params_pending{};
    std::atomic_bool award_funcs_pending{};
    std::atomic_bool award_ui_funcs_pending{};
    std::atomic_bool award_ui_pending{};
    std::atomic_bool award_widgets_pending{};
    std::atomic_bool btn_params_pending{};
    std::atomic_bool controller_click_pending{};
    std::atomic_bool btn_geometry_pending{};
    std::atomic_bool click_params_pending{};
    std::atomic_bool record_mouse_pending{};
    std::atomic_bool simulate_click_pending{};
    bool enter_waiting_landmark{false};
    bool enter_arrived{false};
    double landmark_dest[3]{};
    std::chrono::steady_clock::time_point enter_deadline{};
    std::chrono::steady_clock::time_point enter_transfer_timeout{};
    std::atomic_bool auto_attack{false};
    std::chrono::steady_clock::time_point next_attack{};
    bool attack_is_melee{true};
    std::string combat_status;
    double target_pos[3]{};
    bool target_valid{false};
    std::atomic_uint32_t combat_search_radius_m{50};
    bool combat_scan_valid{false};
    std::chrono::steady_clock::time_point auto_combat_nav_retry_at{};
    bool auto_combat_moving{false};
    std::chrono::steady_clock::time_point next_target_update{};
    // 尸体判定：当前目标句柄、玩家句柄、伤害流游标、最近一次玩家造成伤害的时刻、
    // 进入攻击距离的时刻，以及已拉黑的尸体列表。
    AnomalyGenerationHandleV1 auto_combat_target_handle{};
    std::uint64_t auto_combat_player_handle{0};
    std::uint64_t auto_combat_damage_cursor{0};
    std::chrono::steady_clock::time_point auto_combat_last_player_hit_at{};
    std::chrono::steady_clock::time_point auto_combat_attack_since{};
    // 当前目标锁定期间是否至少命中过一次。命中的目标被打死用短拉黑，
    // 从头到尾打不中的目标（道具）用长拉黑。
    bool auto_combat_target_hit{false};
    std::vector<AutoCombatDeadTarget> auto_combat_dead_targets;
    // 领奖窗口扫描失败是概率性的，重试很密集：这条诊断按 2 秒节流。
    std::chrono::steady_clock::time_point next_reward_scan_diag{};
    // 同上，「打开领奖窗口」失败的诊断也按 2 秒节流。
    std::chrono::steady_clock::time_point next_reward_open_diag{};
    std::uint64_t monster_class_id{0};
    std::vector<std::uint32_t> monster_class_name_ids;
    std::chrono::steady_clock::time_point next_class_rescan{};
    std::uint64_t last_clone_id{0};
    std::chrono::steady_clock::time_point next_clone_check{};
    bool clone_check_done{false};
    std::uint64_t current_clone_fname{0};
    bool award_window_opened{false};
    std::chrono::steady_clock::time_point award_window_time{};
    std::uintptr_t settlement_ui_cache{0};
    std::unordered_map<std::uint32_t, std::uint8_t> reward_class_kinds;
    std::int32_t click_x{0};
    std::int32_t click_y{0};
    std::int32_t click_client_width{0};
    std::int32_t click_client_height{0};
    std::int32_t exit_click_x{0};
    std::int32_t exit_click_y{0};
    std::int32_t weekly_click_x{0};
    std::int32_t weekly_click_y{0};
    bool record_waiting{false};
    bool record_exit_mode{false};
    bool record_weekly_mode{false};
    std::chrono::steady_clock::time_point record_deadline{};
    bool click_waiting{false};
    std::chrono::steady_clock::time_point click_deadline{};
    std::int32_t click_phase{0};
    std::atomic_bool auto_claim_pending{false};
    std::atomic_bool open_reward_pending{false};
    std::atomic_bool settlement_ui_pending{false};
    std::atomic_bool settlement_exit_pending{false};
    std::atomic_bool record_exit_pending{false};
    std::atomic_bool record_weekly_pending{false};
    std::atomic_bool one_key_pending{false};
    bool auto_claim_active{false};
    bool auto_claim_succeeded{false};
    std::uintptr_t auto_claim_world{};
    std::uint64_t auto_claim_clone_id{};
    std::int32_t auto_claim_phase{0};
    std::int32_t auto_claim_retries{0};
    // 领取流程的时限按"采样次数"计，而不是墙钟：状态机靠被调用推进，
    // 游戏 tick 一旦停摆（失焦/加载/卡顿），墙钟 deadline 会在恢复调用的第一刻
    // 立刻判超时，于是变成概率性失败。按次数计则等价于"观察够 N 次才放弃"。
    std::uint32_t auto_claim_polls{0};
    std::uint32_t auto_claim_deadline_polls{0};
    std::uint32_t auto_claim_limit_polls{0};
    // 本次交互用的宝箱 actor：窗口没出现时需要重发交互。
    std::uintptr_t auto_claim_chest{0};
    // 已经为了交互而接近宝箱的轮数（有上限，避免落点不可达时卡在接近阶段）。
    std::uint32_t auto_claim_approach_polls{0};
    std::chrono::steady_clock::time_point auto_claim_poll{};
    std::chrono::steady_clock::time_point auto_claim_nav_issue{};
    std::chrono::steady_clock::time_point auto_claim_nav_deadline{};
    bool one_key_active{false};
    std::int32_t one_key_phase{0};
    std::chrono::steady_clock::time_point one_key_deadline{};
    std::int32_t one_key_no_monster_seconds{0};
    bool one_key_met_monster{false};
    std::uint32_t one_key_enter_wait{8};
    std::uint32_t one_key_loop_count{1};
    std::uint32_t one_key_loop_done{0};
    std::uint32_t one_key_exit_wait{8};
    double one_key_home_pos[3]{};
    bool one_key_home_valid{false};
    double one_key_enter_pos[3]{};
    bool one_key_enter_pos_valid{false};
    bool one_key_direct_enter{false};
    bool auto_claim_is_weekly{false};
    std::atomic_bool test_attack_pending{false};
    bool test_attack_waiting{false};
    std::int32_t test_attack_phase{0};
    std::chrono::steady_clock::time_point test_attack_deadline{};
    bool melee_mode{false};
    NormalAttackBinding normal_attack;
    std::uint32_t melee_attack_counter{0};
    AnomalyGenerationHandleV1 exit_hotkey{};
    std::atomic_bool capturing_exit_hotkey{false};
    std::atomic_uint32_t exit_hotkey_key{VK_F9};
};

void ResetAutoCombatTarget(Context& context) noexcept;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool CoreReady(const AnomalyCoreServiceV1* service) noexcept {
    return HasField<AnomalyCoreServiceV1,
               decltype(AnomalyCoreServiceV1::write_memory)>(
               service, offsetof(AnomalyCoreServiceV1, write_memory)) &&
        service->read_memory != nullptr && service->write_memory != nullptr;
}

bool InputReady(const AnomalyInputServiceV1* s) noexcept {
    return HasField<AnomalyInputServiceV1,
               decltype(AnomalyInputServiceV1::release_hotkey)>(
               s, offsetof(AnomalyInputServiceV1, release_hotkey)) &&
        s->was_pressed != nullptr && s->register_hotkey != nullptr &&
        s->release_hotkey != nullptr;
}

bool DeveloperModeEnabled(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1,
               decltype(AnomalyUiServiceV1::developer_mode_enabled)>(
               ui, offsetof(AnomalyUiServiceV1, developer_mode_enabled)) &&
        ui->developer_mode_enabled != nullptr &&
        ui->developer_mode_enabled(ui->user) != 0;
}

bool ValidHotkey(const std::uint32_t key) noexcept {
    return key == 0 || (key < 256U && key != VK_ESCAPE &&
        !(key >= VK_LBUTTON && key <= VK_XBUTTON2));
}

std::string HotkeyName(const std::uint32_t key) {
    if (key == 0) return "未设置";
    if (key >= '0' && key <= '9') return std::string(1, static_cast<char>(key));
    if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1U);
    switch (key) {
    case VK_SPACE: return "Space";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    default: return "Key" + std::to_string(key);
    }
}

enum class ExitHotkeyResult : std::uint32_t { Registered, Unavailable, Conflict, Failed };

void ANOMALY_CALL ExitHotkey(
    void* user, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*) noexcept {
    auto* context = static_cast<Context*>(user);
    if (context != nullptr &&
        !context->capturing_exit_hotkey.load(std::memory_order_acquire)) {
        context->exit_pending.store(true, std::memory_order_release);
    }
}

ExitHotkeyResult RegisterExitHotkey(
    Context& context, const std::uint32_t key, AnomalyGenerationHandleV1& handle) noexcept {
    handle = {};
    if (key == 0) return ExitHotkeyResult::Registered;
    if (!InputReady(context.input) || !ValidHotkey(key)) {
        return ExitHotkeyResult::Unavailable;
    }
    AnomalyHotkeySpecV1 spec{sizeof(spec)};
    spec.virtual_key = key;
    spec.flags = ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS |
        ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED;
    const std::string id = "clone-enter-exit-" + std::to_string(key);
    spec.id = anomaly::sdk::StringView(id);
    const auto status = context.input->register_hotkey(
        context.input->user, &spec, ExitHotkey, &context, &handle);
    if (status.code == ANOMALY_STATUS_V1_OK && handle.id != 0) {
        return ExitHotkeyResult::Registered;
    }
    handle = {};
    if (status.code == ANOMALY_STATUS_V1_CONFLICT) return ExitHotkeyResult::Conflict;
    if (status.code == ANOMALY_STATUS_V1_UNAVAILABLE) return ExitHotkeyResult::Unavailable;
    return ExitHotkeyResult::Failed;
}

void ReleaseExitHotkey(Context& context) noexcept {
    if (context.exit_hotkey.id != 0 && InputReady(context.input)) {
        static_cast<void>(context.input->release_hotkey(context.input->user, context.exit_hotkey));
    }
    context.exit_hotkey = {};
}

bool ReplaceExitHotkey(Context& context, const std::uint32_t key) noexcept {
    if (key == context.exit_hotkey_key.load(std::memory_order_acquire)) return true;
    AnomalyGenerationHandleV1 replacement{};
    if (RegisterExitHotkey(context, key, replacement) != ExitHotkeyResult::Registered) {
        return false;
    }
    const auto previous = context.exit_hotkey;
    if (previous.id != 0 && context.input->release_hotkey(
            context.input->user, previous).code != ANOMALY_STATUS_V1_OK) {
        static_cast<void>(context.input->release_hotkey(context.input->user, replacement));
        return false;
    }
    context.exit_hotkey = replacement;
    context.exit_hotkey_key.store(key, std::memory_order_release);
    return true;
}

void CaptureExitHotkey(Context& context) noexcept {
    int pressed{};
    if (context.input->was_pressed(context.input->user, VK_ESCAPE, &pressed).code ==
            ANOMALY_STATUS_V1_OK && pressed != 0) {
        context.capturing_exit_hotkey.store(false, std::memory_order_release);
        return;
    }
    for (std::uint32_t key = 1; key < 256U; ++key) {
        if (!ValidHotkey(key)) continue;
        pressed = 0;
        if (context.input->was_pressed(context.input->user, key, &pressed).code ==
                ANOMALY_STATUS_V1_OK && pressed != 0 && ReplaceExitHotkey(context, key)) {
            context.capturing_exit_hotkey.store(false, std::memory_order_release);
            return;
        }
    }
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

bool LandmarksReady(const AnomalyNteMapLandmarksServiceV1* s) noexcept {
    return HasField<AnomalyNteMapLandmarksServiceV1,
               decltype(AnomalyNteMapLandmarksServiceV1::teleport)>(
               s, offsetof(AnomalyNteMapLandmarksServiceV1, teleport)) &&
        s->sequence != nullptr && s->count != nullptr &&
        s->snapshot_at != nullptr && s->teleport != nullptr;
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

bool WriteFloatRaw(const std::uintptr_t address, const float value) noexcept {
    __try {
        *reinterpret_cast<float*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

std::string RenderFName(Context& context, const FNamePair& f) {
    std::string s = ResolveName(context.names, f.cmp);
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

bool GetPlayerState(Context& context) noexcept {
    if (context.g_world_address == 0 &&
        !ResolveRipRelative(context.signature, kGWorldPattern, 0,
                            context.g_world_address)) {
        return false;
    }
    std::uintptr_t world{};
    if (!Read(reinterpret_cast<const void*>(context.g_world_address), world) ||
        world == 0) {
        return false;
    }
    if (context.player_state != 0 && context.controller != 0 &&
        world == context.cached_world) {
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
    if (context.cached_world != world) {
        ResetAutoCombatTarget(context);
        context.monster_class_name_ids.clear();
        context.next_class_rescan = {};
    }
    context.controller = controller;
    context.player_state = ps;
    context.cached_world = world;
    return true;
}

bool SnapshotPlayerPosition(Context& context, double (&position)[3]) noexcept {
    if (!PlayerReady(context.player)) return false;
    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    if (context.player->snapshot(context.player->user, &snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) {
        return false;
    }
    position[0] = snapshot.position[0];
    position[1] = snapshot.position[1];
    position[2] = snapshot.position[2];
    return true;
}

bool EnsureGObjects(Context& context) noexcept {
    if (context.g_objects_address != 0) return true;
    return ResolveRipRelative(context.signature, kGObjectsPattern, kGObjectsAddend,
                              context.g_objects_address);
}

bool ActorsReady(const AnomalyNteActorsServiceV1* s) noexcept {
    return HasField<AnomalyNteActorsServiceV1,
               decltype(AnomalyNteActorsServiceV1::frame)>(
               s, offsetof(AnomalyNteActorsServiceV1, frame)) &&
        s->frame != nullptr && s->snapshot_at != nullptr && s->class_name_utf8 != nullptr;
}

void ScanMonsters(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\monsters.txt", "w");
    if (fp == nullptr) return;
    const auto* s = context.actors;
    if (!ActorsReady(s)) {
        std::fprintf(fp, "actors not ready\n");
        std::fclose(fp);
        return;
    }
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if (s->frame(s->user, &frame).code != ANOMALY_STATUS_V1_OK || frame.entity_count == 0) {
        std::fprintf(fp, "no entities\n");
        std::fclose(fp);
        return;
    }
    std::fprintf(fp, "entities=%u\n", frame.entity_count);
    for (std::uint32_t i = 0; i < frame.entity_count; ++i) {
        AnomalyNteEntitySnapshotV1 snap{sizeof(snap)};
        if (s->snapshot_at(s->user, frame.generation, i, &snap).code != ANOMALY_STATUS_V1_OK) {
            continue;
        }
        std::size_t sz = 0;
        if (s->class_name_utf8(s->user, snap.class_id, nullptr, &sz).code != ANOMALY_STATUS_V1_OK || sz == 0) {
            continue;
        }
        std::string cn(sz, '\0');
        if (s->class_name_utf8(s->user, snap.class_id, cn.data(), &sz).code != ANOMALY_STATUS_V1_OK) {
            continue;
        }
        cn.resize(sz - 1);
        if (cn.find("Monster") == std::string::npos && cn.find("Clone") == std::string::npos) {
            continue;
        }
        std::fprintf(fp, "%u: class=%s\n", i, cn.c_str());
    }
    std::fclose(fp);
}


void* ObjectAt(Context& context, const std::uint32_t index) noexcept {
    if (!EnsureGObjects(context)) return nullptr;
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || index >= static_cast<std::uint32_t>(count) || num_chunks <= 0) {
        return nullptr;
    }
    const auto chunk_index = index / kObjectChunkSize;
    const auto within = index % kObjectChunkSize;
    if (chunk_index >= static_cast<std::uint32_t>(num_chunks)) return nullptr;
    const auto chunk = ReadPointer(reinterpret_cast<const void*>(
        items + static_cast<std::uintptr_t>(chunk_index) * sizeof(void*)));
    if (!chunk) return nullptr;
    return ReadPointer(reinterpret_cast<const std::uint8_t*>(chunk) +
        static_cast<std::uintptr_t>(within) * kObjectItemStride);
}

void KillMonsters(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\kill-monsters.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) {
        std::fprintf(fp, "no gobjects\n");
        std::fclose(fp);
        return;
    }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) {
        std::fprintf(fp, "no items\n");
        std::fclose(fp);
        return;
    }
    // 找 GE_Kill_150_Damage_C 的 CDO（Default__GE_Kill_150_Damage_C）
    std::uintptr_t ge_cdo = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && ge_cdo == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        if (ObjectName(context.names, object) == "Default__GE_Kill_150_Damage_C") { ge_cdo = object; }
    }
    std::fprintf(fp, "ge_cdo=%llx\n", static_cast<unsigned long long>(ge_cdo));
    if (ge_cdo == 0) {
        std::fprintf(fp, "GE CDO not found\n");
        std::fclose(fp);
        return;
    }
    std::int32_t killed = 0;
    cur_chunk = 0xFFFFFFFFu;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uintptr_t fn{};
        if (!FindFunction(context.names, cls, "ServerActivateGameEffectsWithClassByActor", 5, 124, fn)) {
            std::fprintf(fp, "[%u] %s no ServerActivateGameEffectsWithClassByActor\n", i, obj_name.c_str());
            continue;
        }
        std::uint8_t p[124]{};
        std::memcpy(p + 0, &object, sizeof(object));
        std::memcpy(p + 8, &ge_cdo, sizeof(ge_cdo));
        // ModifyData = BufferData (88B, off=16)
        //   nLevel(int32,40) nStackCount(int32,56) fStrengthMult(float,60) fStrengthAdd(float,64)
        const std::int32_t level = 1;
        const std::int32_t stack = 1;
        const float mult = 1.0f;
        const float add = 999999.0f;
        std::memcpy(p + 16 + 40, &level, 4);
        std::memcpy(p + 16 + 56, &stack, 4);
        std::memcpy(p + 16 + 60, &mult, 4);
        std::memcpy(p + 16 + 64, &add, 4);
        const bool ok = Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn), p);
        if (ok) {
            ++killed;
            std::fprintf(fp, "[%u] server GE %s (%s) mult=1 add=999999\n", i, obj_name.c_str(),
                         cls_name.c_str());
        }
    }
    std::fprintf(fp, "total=%d\n", killed);
    std::fclose(fp);
}
void DumpFunctionParams(Context& context, std::uintptr_t fn, std::FILE* fp) noexcept {
    std::uintptr_t prop{};
    if (!Read(reinterpret_cast<const void*>(fn + kUStructPropertyLinkOffset), prop) || prop == 0) {
        std::fprintf(fp, "  (no params)\n");
        return;
    }
    std::uint32_t k = 0;
    while (prop != 0 && k < 16) {
        std::uint32_t name_id{};
        Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
        const std::string pname = ResolveName(context.names, name_id);
        std::uint16_t esz{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
        std::int32_t off{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
        std::uintptr_t prop_class{};
        Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
        std::string tname;
        if (prop_class != 0) {
            std::uint32_t tnid{};
            if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                tname = ResolveName(context.names, tnid);
            }
        }
        std::fprintf(fp, "  [%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                     tname.c_str(), static_cast<unsigned>(esz), off);
        std::uintptr_t next{};
        if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
            next == 0 || next == prop) break;
        prop = next;
        ++k;
    }
}

void ScanGE(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\ge-classes.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "Class" && cls_name != "BlueprintGeneratedClass") continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("GE_") == std::string::npos &&
            obj_name.find("GameplayEffect") == std::string::npos &&
            obj_name.find("Damage") == std::string::npos) continue;
        std::fprintf(fp, "%s | %s\n", obj_name.c_str(), cls_name.c_str());
    }
    std::fclose(fp);
}


void DumpControllerDamageParams(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\controller-damage-params.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) {
        std::fprintf(fp, "no player state\n");
        std::fclose(fp);
        return;
    }
    std::uintptr_t cc{};
    if (!Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc) || cc == 0) {
        std::fprintf(fp, "no controller class\n");
        std::fclose(fp);
        return;
    }
    const char* targets[] = {"ServerApplyGameplayEffectOnNetTarget", "ServerAbilityFunction",
                             "ServerAbilityActorGE", "TestServerAbilityActorGE"};
    std::uintptr_t owner = cc;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
            if (ObjectName(context.names, field_class) == "Function") {
                const std::string fn_name = ObjectName(context.names, field);
                for (const char* t : targets) {
                    if (fn_name == t) {
                        std::uint8_t np{}; std::uint16_t ps{};
                        Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                        Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                        std::fprintf(fp, "\n=== %s np=%u ps=%u ===\n", fn_name.c_str(),
                                     static_cast<unsigned>(np), static_cast<unsigned>(ps));
                        DumpFunctionParams(context, field, fp);
                    }
                }
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpStructFields(Context& context, std::uintptr_t s, std::FILE* fp, int depth) noexcept;
void DumpPlayerStateFuncs(Context& context) noexcept;
void DumpCloneRPCParams(Context& context) noexcept;
void DumpCloneEnums(Context& context) noexcept;
void DumpEnumValues(Context& context) noexcept;
void TriggerPassPermitAward(Context& context) noexcept;
void ScanCloneClasses(Context& context) noexcept;
void DumpCloneManagerFuncs(Context& context) noexcept;
void ActivateSkill(Context& context) noexcept;
bool ActivateSkillByInputId(Context& context, std::int32_t input_id) noexcept;
void AutoCombatTick(Context& context) noexcept;
void DumpCombatTarget(Context& context) noexcept;
void DumpMonsterClasses(Context& context) noexcept;
void DumpCloneMonsterInfo(Context& context) noexcept;
void ScanMonsterAssets(Context& context) noexcept;
void DumpEntityClasses(Context& context) noexcept;
std::uintptr_t FindChestActor(Context& context, double* position = nullptr) noexcept;
bool FindChestPos(Context& context, double (&pos)[3]) noexcept;
void ClaimReward(Context& context, std::int32_t index) noexcept;
void DumpChestChoices(Context& context) noexcept;
void DumpChestFuncs(Context& context) noexcept;
void DumpRewardParams(Context& context) noexcept;
void DumpAwardFuncs(Context& context) noexcept;
void DumpAwardUIFuncs(Context& context) noexcept;
void DumpAwardUI(Context& context) noexcept;
void DumpAwardWidgets(Context& context) noexcept;
void DumpBtnParams(Context& context) noexcept;
void DumpControllerClickFuncs(Context& context) noexcept;
void DumpBtnGeometry(Context& context) noexcept;
void DumpClickParams(Context& context) noexcept;
void RecordMousePos(Context& context) noexcept;
void RecordExitPos(Context& context) noexcept;
void LoadExitConfig(Context& context) noexcept;
void RecordWeeklyPos(Context& context) noexcept;
void LoadWeeklyConfig(Context& context) noexcept;
void SimulateClick(Context& context) noexcept;
void SendKeyF(bool down) noexcept;
void GetClientSize(std::int32_t& w, std::int32_t& h) noexcept;
void MoveSystemCursor(std::int32_t vx, std::int32_t vy) noexcept;
void SendMouseButton(bool down) noexcept;
void DumpSettlementUI(Context& context) noexcept;

void DumpNetTargetStruct(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\nettarget-struct.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) {
        std::fprintf(fp, "no player state\n");
        std::fclose(fp);
        return;
    }
    std::uintptr_t cc{};
    if (!Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc) || cc == 0) {
        std::fprintf(fp, "no controller class\n");
        std::fclose(fp);
        return;
    }
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cc, "ServerApplyGameplayEffectOnNetTarget", 4, 176, fn)) {
        std::fprintf(fp, "fn not found\n");
        std::fclose(fp);
        return;
    }
    std::fprintf(fp, "fn=%llx\n", static_cast<unsigned long long>(fn));
    std::uintptr_t prop{};
    if (!Read(reinterpret_cast<const void*>(fn + kUStructPropertyLinkOffset), prop) || prop == 0) {
        std::fprintf(fp, "no params\n");
        std::fclose(fp);
        return;
    }
    std::uint32_t k = 0;
    while (prop != 0 && k < 16) {
        std::uint32_t name_id{};
        Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
        const std::string pname = ResolveName(context.names, name_id);
        std::uint16_t esz{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
        std::int32_t off{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
        std::uintptr_t prop_class{};
        Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
        std::string tname;
        if (prop_class != 0) {
            std::uint32_t tnid{};
            if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                tname = ResolveName(context.names, tnid);
            }
        }
        std::fprintf(fp, "[%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                     tname.c_str(), static_cast<unsigned>(esz), off);
        if (tname == "StructProperty") {
            std::uintptr_t inner{};
            if (Read(reinterpret_cast<const void*>(prop + kFStructPropertyStructOffset), inner) &&
                inner != 0) {
                std::fprintf(fp, "=== struct %s (%llx) ===\n",
                             ObjectName(context.names, inner).c_str(),
                             static_cast<unsigned long long>(inner));
                DumpStructFields(context, inner, fp, 1);
            }
        }
        std::uintptr_t next{};
        if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
            next == 0 || next == prop) break;
        prop = next;
        ++k;
    }
    std::fclose(fp);
}


void DumpCharForNet(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\charfornet.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t st = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && st == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (ObjectName(context.names, object) == "CharacterForNet" &&
            cls_name == "ScriptStruct") {
            st = object;
        }
    }
    std::fprintf(fp, "struct=%llx\n", static_cast<unsigned long long>(st));
    if (st == 0) { std::fclose(fp); return; }
    std::uintptr_t owner = st;
    for (std::uint32_t depth = 0; owner != 0 && depth < 16; ++depth) {
        std::fprintf(fp, "--- depth %u: %s (%llx) ---\n", depth,
                     ObjectName(context.names, owner).c_str(),
                     static_cast<unsigned long long>(owner));
        // ChildProperties (FField 链表)
        std::uintptr_t prop{};
        Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
        std::uint32_t k = 0;
        while (prop != 0 && k < 64) {
            std::uint32_t name_id{};
            Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
            const std::string pname = ResolveName(context.names, name_id);
            std::uint16_t esz{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
            std::int32_t off{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
            std::uintptr_t prop_class{};
            Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
            std::string tname;
            if (prop_class != 0) {
                std::uint32_t tnid{};
                if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                    tname = ResolveName(context.names, tnid);
                }
            }
            std::fprintf(fp, "  prop[%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                         tname.c_str(), static_cast<unsigned>(esz), off);
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                next == 0 || next == prop) break;
            prop = next;
            ++k;
        }
        // Children (UField 链表)
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        std::uint32_t j = 0;
        while (field != 0 && j < 64) {
            std::fprintf(fp, "  child[%u] %s\n", j, ObjectName(context.names, field).c_str());
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                next == 0 || next == field) break;
            field = next;
            ++j;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpPlayerDamageFunc(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\player-damage-func.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t gpc{};
    if (!FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, gpc)) {
        std::fprintf(fp, "no GetPlayerCharacter\n"); std::fclose(fp); return;
    }
    std::uint8_t pb[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(gpc), pb)) {
        std::fprintf(fp, "GetPlayerCharacter failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t pawn{};
    std::memcpy(&pawn, pb, sizeof(pawn));
    if (pawn == 0) { std::fprintf(fp, "pawn=0\n"); std::fclose(fp); return; }
    std::uintptr_t pawn_cls{};
    Read(reinterpret_cast<const void*>(pawn + kObjectClassOffset), pawn_cls);
    std::uintptr_t owner = pawn_cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
            if (ObjectName(context.names, field_class) == "Function") {
                const std::string fn_name = ObjectName(context.names, field);
                if (fn_name.find("Damage") != std::string::npos ||
                    fn_name.find("TakeDamage") != std::string::npos ||
                    fn_name.find("ApplyDamage") != std::string::npos) {
                    std::uint8_t np{}; std::uint16_t ps{};
                    Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                    Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                    std::fprintf(fp, "\n=== %s np=%u ps=%u ===\n", fn_name.c_str(),
                                 static_cast<unsigned>(np), static_cast<unsigned>(ps));
                    DumpFunctionParams(context, field, fp);
                }
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void KillMonstersViaAnyDamage(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\kill-any-damage.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    // 找玩家 pawn 作为 Instigator
    std::uintptr_t pawn = 0;
    {
        std::uintptr_t cc{};
        Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
        std::uintptr_t gpc{};
        if (FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, gpc)) {
            std::uint8_t pb[8]{};
            if (Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(gpc), pb)) {
                std::memcpy(&pawn, pb, sizeof(pawn));
            }
        }
    }
    std::fprintf(fp, "pawn=%llx\n", static_cast<unsigned long long>(pawn));
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    // 找 HTDamageType CDO
    std::uintptr_t dmg_type = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && dmg_type == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        if (ObjectName(context.names, object) == "Default__HTDamageType") { dmg_type = object; }
    }
    std::fprintf(fp, "dmg_type=%llx\n", static_cast<unsigned long long>(dmg_type));
    std::int32_t killed = 0;
    cur_chunk = 0xFFFFFFFFu;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uintptr_t fn{};
        if (!FindFunction(context.names, cls, "ReceiveAnyDamage", 4, 32, fn)) {
            std::fprintf(fp, "[%u] %s no ReceiveAnyDamage\n", i, obj_name.c_str());
            continue;
        }
        std::uint8_t p[32]{};
        const float dmg = 999999.0f;
        std::memcpy(p + 0, &dmg, 4);
        std::memcpy(p + 8, &dmg_type, sizeof(dmg_type));
        std::memcpy(p + 16, &pawn, sizeof(pawn));
        std::memcpy(p + 24, &pawn, sizeof(pawn));
        const bool ok = Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn), p);
        if (ok) {
            ++killed;
            std::fprintf(fp, "[%u] any damage %s (%s)\n", i, obj_name.c_str(), cls_name.c_str());
        }
    }
    std::fprintf(fp, "total=%d\n", killed);
    std::fclose(fp);
}


void KillMonstersViaKillSelf(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\kill-self.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::int32_t killed = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uintptr_t fn{};
        if (!FindFunction(context.names, cls, "KillSelf", 1, 1, fn)) {
            std::fprintf(fp, "[%u] %s no KillSelf\n", i, obj_name.c_str());
            continue;
        }
        std::uint8_t p[1]{};
        p[0] = 0;  // bDestroy = false
        const bool ok = Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn), p);
        if (ok) {
            ++killed;
            std::fprintf(fp, "[%u] killself %s (%s)\n", i, obj_name.c_str(), cls_name.c_str());
        }
    }
    std::fprintf(fp, "total=%d\n", killed);
    std::fclose(fp);
}


void KillMonstersViaDeathEvent(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\kill-death-event.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::int32_t done = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uintptr_t fn_dead{};
        std::uintptr_t fn_hp{};
        std::uintptr_t fn_evt{};
        const bool has_dead = FindFunction(context.names, cls, "SetIsDead", 1, 1, fn_dead);
        const bool has_hp = FindFunction(context.names, cls, "SetHP", 4, 12, fn_hp);
        const bool has_evt = FindFunction(context.names, cls, "BPOnCharacterDead", 0, 0, fn_evt) ||
                             FindFunction(context.names, cls, "BPOnAICharacterDead", 0, 0, fn_evt);
        if (!has_dead || !has_hp) {
            std::fprintf(fp, "[%u] %s missing SetIsDead/SetHP\n", i, obj_name.c_str());
            continue;
        }
        std::uint8_t dead[1]{};
        dead[0] = 1;
        Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_dead), dead);
        std::uint8_t hp[12]{};
        const float hpv = 0.0f;
        const std::uint8_t reason = 0;
        const std::uint8_t real = 1;
        const float dmg = 999999.0f;
        std::memcpy(hp + 0, &hpv, 4);
        hp[4] = reason;
        hp[5] = real;
        std::memcpy(hp + 8, &dmg, 4);
        Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_hp), hp);
        if (has_evt) {
            Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_evt), nullptr);
        }
        ++done;
        std::fprintf(fp, "[%u] death-event %s (%s) evt=%d\n", i, obj_name.c_str(),
                     cls_name.c_str(), has_evt ? 1 : 0);
    }
    std::fprintf(fp, "total=%d\n", done);
    std::fclose(fp);
}


void ApplyDamageViaNetTarget(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\nettarget-damage.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t gpc{};
    if (!FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, gpc)) {
        std::fprintf(fp, "no GetPlayerCharacter\n"); std::fclose(fp); return;
    }
    std::uint8_t pb[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(gpc), pb)) {
        std::fprintf(fp, "GetPlayerCharacter failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t pawn{};
    std::memcpy(&pawn, pb, sizeof(pawn));
    std::fprintf(fp, "pawn=%llx controller=%llx\n",
                 static_cast<unsigned long long>(pawn),
                 static_cast<unsigned long long>(context.controller));
    if (pawn == 0) { std::fclose(fp); return; }
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cc, "ServerApplyGameplayEffectOnNetTarget", 4, 176, fn)) {
        std::fprintf(fp, "no ServerApplyGameplayEffectOnNetTarget\n"); std::fclose(fp); return;
    }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t ge_cdo = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && ge_cdo == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        if (ObjectName(context.names, object) == "Default__GE_Kill_150_Damage_C") { ge_cdo = object; }
    }
    std::fprintf(fp, "ge_cdo=%llx\n", static_cast<unsigned long long>(ge_cdo));
    if (ge_cdo == 0) { std::fclose(fp); return; }
    std::int32_t done = 0;
    cur_chunk = 0xFFFFFFFFu;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uint8_t p[176]{};
        // NetTarget (CharacterForNet 40B): 猜测第一个字段是 Actor 指针
        std::memcpy(p + 0, &object, sizeof(object));
        // NetSource (CharacterForNet 40B): 第一个字段是玩家 pawn 指针
        std::memcpy(p + 40, &pawn, sizeof(pawn));
        std::memcpy(p + 80, &ge_cdo, sizeof(ge_cdo));
        // ModifyData (BufferData 88B) at off=88
        const std::int32_t level = 1;
        const std::int32_t stack = 1;
        const float mult = 1.0f;
        const float add = 999999.0f;
        std::memcpy(p + 88 + 40, &level, 4);
        std::memcpy(p + 88 + 56, &stack, 4);
        std::memcpy(p + 88 + 60, &mult, 4);
        std::memcpy(p + 88 + 64, &add, 4);
        const bool ok = Invoke(reinterpret_cast<void*>(context.controller),
                               reinterpret_cast<void*>(fn), p);
        ++done;
        std::fprintf(fp, "[%u] nettarget-damage %s ok=%d\n", i, obj_name.c_str(), ok ? 1 : 0);
    }
    std::fprintf(fp, "total=%d\n", done);
    std::fclose(fp);
}


void SetMonstersHPToOne(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\set-hp-one.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::int32_t done = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uintptr_t fn_hp{};
        std::uintptr_t fn_get{};
        if (!FindFunction(context.names, cls, "SetHP", 4, 12, fn_hp)) continue;
        const bool has_get = FindFunction(context.names, cls, "GetHP", 1, 4, fn_get);
        float before = -1.0f;
        if (has_get) {
            std::uint8_t g[4]{};
            Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_get), g);
            std::memcpy(&before, g, 4);
        }
        std::uint8_t p[12]{};
        const float hp = 0.0f;
        const std::uint8_t reason = 0;
        const std::uint8_t real = 1;
        const float dmg = 999999.0f;
        std::memcpy(p + 0, &hp, 4);
        p[4] = reason;
        p[5] = real;
        std::memcpy(p + 8, &dmg, 4);
        Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_hp), p);
        float after = -1.0f;
        if (has_get) {
            std::uint8_t g[4]{};
            Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_get), g);
            std::memcpy(&after, g, 4);
        }
        ++done;
        std::fprintf(fp, "[%u] %s hp %.1f -> %.1f\n", i, obj_name.c_str(), before, after);
    }
    std::fprintf(fp, "total=%d\n", done);
    std::fclose(fp);
}


void DumpMonsterAttrSet(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\monster-attrset.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::int32_t found = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && found < 3; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uintptr_t fn{};
        if (!FindFunction(context.names, cls, "GetHTCharacterAttributeSet", 1, 8, fn)) continue;
        std::uint8_t ab[8]{};
        if (!Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn), ab)) continue;
        std::uintptr_t attrset{};
        std::memcpy(&attrset, ab, sizeof(attrset));
        std::fprintf(fp, "\n=== %s attrset=%llx ===\n", obj_name.c_str(),
                     static_cast<unsigned long long>(attrset));
        if (attrset == 0) continue;
        std::uintptr_t owner{};
        Read(reinterpret_cast<const void*>(attrset + kObjectClassOffset), owner);
        for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
            std::fprintf(fp, "--- depth %u: %s ---\n", depth, ObjectName(context.names, owner).c_str());
            std::uintptr_t prop{};
            Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
            std::uint32_t k = 0;
            while (prop != 0 && k < 512) {
                std::uint32_t name_id{};
                Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
                const std::string pname = ResolveName(context.names, name_id);
                std::uint16_t esz{};
                Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
                std::int32_t off{};
                Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
                std::uintptr_t prop_class{};
                Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
                std::string tname;
                if (prop_class != 0) {
                    std::uint32_t tnid{};
                    if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                        tname = ResolveName(context.names, tnid);
                    }
                }
                if (tname == "FloatProperty") {
                    float v{};
                    Read(reinterpret_cast<const void*>(attrset + static_cast<std::uintptr_t>(off)), v);
                    std::fprintf(fp, "  [%u] %s off=%d = %f\n", k, pname.c_str(), off, v);
                }
                std::uintptr_t next{};
                if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                    next == 0 || next == prop) break;
                prop = next;
                ++k;
            }
            std::uintptr_t super{};
            if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                super == 0 || super == owner) break;
            owner = super;
        }
        ++found;
    }
    std::fclose(fp);
}


void ScanMonsterHP(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\monster-hp-scan.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t monster = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && monster == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        const std::string obj_name = ObjectName(context.names, object);
        if (cls_name.find("BP_Clone_C") != std::string::npos &&
            obj_name.find("Default__") == std::string::npos) {
            monster = object;
        }
    }
    std::fprintf(fp, "monster=%llx\n", static_cast<unsigned long long>(monster));
    if (monster == 0) { std::fclose(fp); return; }
    for (std::uintptr_t off = 0; off < 65536; off += 4) {
        float f{};
        if (Read(reinterpret_cast<const void*>(monster + off), f) &&
            f >= 34000.0f && f <= 34500.0f) {
            std::fprintf(fp, "  +0x%llx float = %f\n",
                         static_cast<unsigned long long>(off), f);
        }
        std::int32_t i32{};
        if (Read(reinterpret_cast<const void*>(monster + off), i32) &&
            i32 >= 34000 && i32 <= 34500) {
            std::fprintf(fp, "  +0x%llx int32 = %d\n",
                         static_cast<unsigned long long>(off), i32);
        }
    }
    std::fclose(fp);
}


void DumpMonsterHPValues(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\monster-hp-values.txt", "w");
    if (fp == nullptr) return;
    static_cast<void>(GetPlayerState(context));
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::int32_t found = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && found < 2; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0) continue;
        const std::string cls_name = ObjectName(context.names, cls);
        if (cls_name.find("BP_Clone_C") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::uintptr_t fn_hp{}, fn_max{};
        const bool has_hp = FindFunction(context.names, cls, "GetHP", 1, 4, fn_hp);
        const bool has_max = FindFunction(context.names, cls, "GetHPMax", 2, 8, fn_max);
        float hp = -1.0f, max0 = -1.0f, max1 = -1.0f;
        if (has_hp) {
            std::uint8_t b[4]{};
            Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_hp), b);
            std::memcpy(&hp, b, 4);
        }
        if (has_max) {
            std::uint8_t b[8]{};
            b[0] = 0;
            Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_max), b);
            std::memcpy(&max0, b + 4, 4);
            std::uint8_t c[8]{};
            c[0] = 1;
            Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn_max), c);
            std::memcpy(&max1, c + 4, 4);
        }
        std::fprintf(fp, "%s: GetHP=%.1f GetHPMax(fix0)=%.1f GetHPMax(fix1)=%.1f\n",
                     obj_name.c_str(), hp, max0, max1);
        ++found;
    }
    std::fclose(fp);
}


void DumpPlayerAttributeSet(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\player-attrset.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t gpc{};
    if (!FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, gpc)) {
        std::fprintf(fp, "no GetPlayerCharacter\n"); std::fclose(fp); return;
    }
    std::uint8_t pb[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(gpc), pb)) {
        std::fprintf(fp, "GetPlayerCharacter failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t pawn{};
    std::memcpy(&pawn, pb, sizeof(pawn));
    if (pawn == 0) { std::fprintf(fp, "pawn=0\n"); std::fclose(fp); return; }
    std::uintptr_t pawn_cls{};
    Read(reinterpret_cast<const void*>(pawn + kObjectClassOffset), pawn_cls);
    std::uintptr_t as_fn{};
    if (!FindFunction(context.names, pawn_cls, "GetAttributeSet", 1, 8, as_fn)) {
        std::fprintf(fp, "no GetHTCharacterAttributeSet\n"); std::fclose(fp); return;
    }
    std::uint8_t ap[8]{};
    if (!Invoke(reinterpret_cast<void*>(pawn), reinterpret_cast<void*>(as_fn), ap)) {
        std::fprintf(fp, "GetHTCharacterAttributeSet failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t attrset{};
    std::memcpy(&attrset, ap, sizeof(attrset));
    std::fprintf(fp, "attrset=%llx\n", static_cast<unsigned long long>(attrset));
    if (attrset == 0) { std::fclose(fp); return; }
    std::uintptr_t owner{};
    Read(reinterpret_cast<const void*>(attrset + kObjectClassOffset), owner);
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::fprintf(fp, "--- depth %u: %s ---\n", depth, ObjectName(context.names, owner).c_str());
        std::uintptr_t prop{};
        Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
        std::uint32_t k = 0;
        while (prop != 0 && k < 512) {
            std::uint32_t name_id{};
            Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
            const std::string pname = ResolveName(context.names, name_id);
            std::uint16_t esz{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
            std::int32_t off{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
            std::uintptr_t prop_class{};
            Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
            std::string tname;
            if (prop_class != 0) {
                std::uint32_t tnid{};
                if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                    tname = ResolveName(context.names, tnid);
                }
            }
            if (tname == "StructProperty") {
                std::fprintf(fp, "  [%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                             tname.c_str(), static_cast<unsigned>(esz), off);
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                next == 0 || next == prop) break;
            prop = next;
            ++k;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpAttributeValues(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\attr-values.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t gpc{};
    if (!FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, gpc)) {
        std::fprintf(fp, "no GetPlayerCharacter\n"); std::fclose(fp); return;
    }
    std::uint8_t pb[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(gpc), pb)) {
        std::fprintf(fp, "GetPlayerCharacter failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t pawn{};
    std::memcpy(&pawn, pb, sizeof(pawn));
    if (pawn == 0) { std::fprintf(fp, "pawn=0\n"); std::fclose(fp); return; }
    std::uintptr_t pawn_cls{};
    Read(reinterpret_cast<const void*>(pawn + kObjectClassOffset), pawn_cls);
    std::uintptr_t as_fn{};
    if (!FindFunction(context.names, pawn_cls, "GetAttributeSet", 1, 8, as_fn)) {
        std::fprintf(fp, "no GetAttributeSet\n"); std::fclose(fp); return;
    }
    std::uint8_t ap[8]{};
    if (!Invoke(reinterpret_cast<void*>(pawn), reinterpret_cast<void*>(as_fn), ap)) {
        std::fprintf(fp, "GetAttributeSet failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t attrset{};
    std::memcpy(&attrset, ap, sizeof(attrset));
    std::fprintf(fp, "attrset=%llx\n", static_cast<unsigned long long>(attrset));
    if (attrset == 0) { std::fclose(fp); return; }
    std::uintptr_t owner{};
    Read(reinterpret_cast<const void*>(attrset + kObjectClassOffset), owner);
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::fprintf(fp, "--- depth %u: %s ---\n", depth, ObjectName(context.names, owner).c_str());
        std::uintptr_t prop{};
        Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
        std::uint32_t k = 0;
        while (prop != 0 && k < 512) {
            std::uint32_t name_id{};
            Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
            const std::string pname = ResolveName(context.names, name_id);
            std::uint16_t esz{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
            std::int32_t off{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
            std::uintptr_t prop_class{};
            Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
            std::string tname;
            if (prop_class != 0) {
                std::uint32_t tnid{};
                if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                    tname = ResolveName(context.names, tnid);
                }
            }
            if (tname == "FloatProperty") {
                float v{};
                Read(reinterpret_cast<const void*>(attrset + static_cast<std::uintptr_t>(off)), v);
                std::fprintf(fp, "  [%u] %s Float off=%d = %f\n", k, pname.c_str(), off, v);
            } else if (tname == "IntProperty") {
                std::int32_t v{};
                Read(reinterpret_cast<const void*>(attrset + static_cast<std::uintptr_t>(off)), v);
                std::fprintf(fp, "  [%u] %s Int off=%d = %d\n", k, pname.c_str(), off, v);
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                next == 0 || next == prop) break;
            prop = next;
            ++k;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpHTAttrClass(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\ht-attr-class.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t cls = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && cls == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t obj_cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), obj_cls);
        const std::string obj_cls_name = obj_cls != 0 ? ObjectName(context.names, obj_cls) : std::string();
        if (ObjectName(context.names, object) == "HTPlayerAttributeSet" &&
            (obj_cls_name == "BlueprintGeneratedClass" || obj_cls_name == "Class")) {
            cls = object;
        }
    }
    std::fprintf(fp, "cls=%llx\n", static_cast<unsigned long long>(cls));
    if (cls == 0) { std::fclose(fp); return; }
    std::uintptr_t owner = cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::fprintf(fp, "--- depth %u: %s ---\n", depth, ObjectName(context.names, owner).c_str());
        std::uintptr_t prop{};
        Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
        std::uint32_t k = 0;
        while (prop != 0 && k < 2048) {
            std::uint32_t name_id{};
            Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
            const std::string pname = ResolveName(context.names, name_id);
            std::uint16_t esz{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
            std::int32_t off{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
            std::uintptr_t prop_class{};
            Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
            std::string tname;
            if (prop_class != 0) {
                std::uint32_t tnid{};
                if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                    tname = ResolveName(context.names, tnid);
                }
            }
            if (tname == "FloatProperty" || tname == "IntProperty" ||
                tname == "Int64Property") {
                std::fprintf(fp, "  [%u] %s %s elem=%u off=%d\n", k, pname.c_str(),
                             tname.c_str(), static_cast<unsigned>(esz), off);
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                next == 0 || next == prop) break;
            prop = next;
            ++k;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpDamageAttrs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\damage-attrs.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t obj_cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), obj_cls);
        const std::string obj_cls_name = obj_cls != 0 ? ObjectName(context.names, obj_cls) : std::string();
        const std::string obj_name = ObjectName(context.names, object);
        if ((obj_cls_name == "BlueprintGeneratedClass" || obj_cls_name == "Class") &&
            obj_name.find("AttributeSet") != std::string::npos) {
            std::uintptr_t owner = object;
            for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
                std::uintptr_t prop{};
                Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
                std::uint32_t k = 0;
                while (prop != 0 && k < 2048) {
                    std::uint32_t name_id{};
                    Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
                    const std::string pname = ResolveName(context.names, name_id);
                    std::uint16_t esz{};
                    Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
                    std::int32_t off{};
                    Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
                    std::uintptr_t prop_class{};
                    Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
                    std::string tname;
                    if (prop_class != 0) {
                        std::uint32_t tnid{};
                        if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                            tname = ResolveName(context.names, tnid);
                        }
                    }
                    if ((tname == "FloatProperty" || tname == "IntProperty" ||
                         tname == "Int64Property") &&
                        (pname.find("Damage") != std::string::npos ||
                         pname.find("Attack") != std::string::npos)) {
                        std::fprintf(fp, "%s :: %s %s off=%d\n", obj_name.c_str(),
                                     pname.c_str(), tname.c_str(), off);
                    }
                    std::uintptr_t next{};
                    if (!Read(reinterpret_cast<const void*>(
                                  prop + kFPropertyPropertyLinkNextOffset), next) ||
                        next == 0 || next == prop) break;
                    prop = next;
                    ++k;
                }
                std::uintptr_t super{};
                if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                    super == 0 || super == owner) break;
                owner = super;
            }
        }
    }
    std::fclose(fp);
}


void DumpHPAttrs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\hp-attrs.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t obj_cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), obj_cls);
        const std::string obj_cls_name = obj_cls != 0 ? ObjectName(context.names, obj_cls) : std::string();
        const std::string obj_name = ObjectName(context.names, object);
        if ((obj_cls_name == "BlueprintGeneratedClass" || obj_cls_name == "Class") &&
            obj_name.find("AttributeSet") != std::string::npos) {
            std::uintptr_t owner = object;
            for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
                std::uintptr_t prop{};
                Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
                std::uint32_t k = 0;
                while (prop != 0 && k < 2048) {
                    std::uint32_t name_id{};
                    Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
                    const std::string pname = ResolveName(context.names, name_id);
                    std::uint16_t esz{};
                    Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
                    std::int32_t off{};
                    Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
                    std::uintptr_t prop_class{};
                    Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
                    std::string tname;
                    if (prop_class != 0) {
                        std::uint32_t tnid{};
                        if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                            tname = ResolveName(context.names, tnid);
                        }
                    }
                    if ((tname == "FloatProperty" || tname == "IntProperty" ||
                         tname == "Int64Property") &&
                        (pname.find("HP") != std::string::npos ||
                         pname.find("Health") != std::string::npos ||
                         pname.find("Hp") != std::string::npos)) {
                        std::fprintf(fp, "%s :: %s %s off=%d\n", obj_name.c_str(),
                                     pname.c_str(), tname.c_str(), off);
                    }
                    std::uintptr_t next{};
                    if (!Read(reinterpret_cast<const void*>(
                                  prop + kFPropertyPropertyLinkNextOffset), next) ||
                        next == 0 || next == prop) break;
                    prop = next;
                    ++k;
                }
                std::uintptr_t super{};
                if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                    super == 0 || super == owner) break;
                owner = super;
            }
        }
    }
    std::fclose(fp);
}


void DumpModifierAttr(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\modifier-attr.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t cdo = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && cdo == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        if (ObjectName(context.names, object) == "Default__Buff_DamageUP_C") { cdo = object; }
    }
    std::fprintf(fp, "cdo=%llx\n", static_cast<unsigned long long>(cdo));
    if (cdo == 0) { std::fclose(fp); return; }
    constexpr std::uintptr_t kModifiersArrayOffset = 584;
    std::uintptr_t arr_data{};
    std::int32_t arr_num{};
    Read(reinterpret_cast<const void*>(cdo + kModifiersArrayOffset), arr_data);
    Read(reinterpret_cast<const void*>(cdo + kModifiersArrayOffset + 8), arr_num);
    std::fprintf(fp, "modifiers data=%llx num=%d\n",
                 static_cast<unsigned long long>(arr_data), arr_num);
    if (arr_data == 0 || arr_num <= 0) { std::fclose(fp); return; }
    const std::uintptr_t elem = arr_data;
    // Attribute (FGameplayAttribute, 56B) at elem+0
    std::uintptr_t owner{};
    Read(reinterpret_cast<const void*>(elem + 48), owner);
    std::fprintf(fp, "attribute_owner=%llx %s\n", static_cast<unsigned long long>(owner),
                 ObjectName(context.names, owner).c_str());
    std::uintptr_t prop{};
    Read(reinterpret_cast<const void*>(elem + 16), prop);
    std::fprintf(fp, "attribute_prop=%llx\n", static_cast<unsigned long long>(prop));
    if (prop != 0) {
        std::uint32_t name_id{};
        Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
        std::int32_t off{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
        std::fprintf(fp, "prop name=%s off=%d\n", ResolveName(context.names, name_id).c_str(), off);
    }
    std::fclose(fp);
}


void BoostPlayerDamage(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\player-damage-boost.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t gpc{};
    if (!FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, gpc)) {
        std::fprintf(fp, "no GetPlayerCharacter\n"); std::fclose(fp); return;
    }
    std::uint8_t pb[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(gpc), pb)) {
        std::fprintf(fp, "GetPlayerCharacter failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t pawn{};
    std::memcpy(&pawn, pb, sizeof(pawn));
    if (pawn == 0) { std::fprintf(fp, "pawn=0\n"); std::fclose(fp); return; }
    std::uintptr_t pawn_cls{};
    Read(reinterpret_cast<const void*>(pawn + kObjectClassOffset), pawn_cls);
    std::uintptr_t as_fn{};
    if (!FindFunction(context.names, pawn_cls, "GetAttributeSet", 1, 8, as_fn)) {
        std::fprintf(fp, "no GetAttributeSet\n"); std::fclose(fp); return;
    }
    std::uint8_t ap[8]{};
    if (!Invoke(reinterpret_cast<void*>(pawn), reinterpret_cast<void*>(as_fn), ap)) {
        std::fprintf(fp, "GetAttributeSet failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t attrset{};
    std::memcpy(&attrset, ap, sizeof(attrset));
    std::fprintf(fp, "attrset=%llx\n", static_cast<unsigned long long>(attrset));
    if (attrset == 0) { std::fclose(fp); return; }
    std::uintptr_t cls{};
    Read(reinterpret_cast<const void*>(attrset + kObjectClassOffset), cls);
    std::fprintf(fp, "attrset class=%llx %s\n", static_cast<unsigned long long>(cls),
                 ObjectName(context.names, cls).c_str());
    constexpr std::uintptr_t kDamageUpGeneralBaseOffset = 448;
    float old{};
    Read(reinterpret_cast<const void*>(attrset + kDamageUpGeneralBaseOffset), old);
    std::fprintf(fp, "DamageUpGeneralBase old=%f\n", old);
    const float nv = 999999.0f;
    if (!WriteFloatRaw(attrset + kDamageUpGeneralBaseOffset, nv)) {
        std::fprintf(fp, "write failed\n");
        std::fclose(fp);
        return;
    }
    float after{};
    Read(reinterpret_cast<const void*>(attrset + kDamageUpGeneralBaseOffset), after);
    std::fprintf(fp, "DamageUpGeneralBase after=%f\n", after);
    std::fclose(fp);
}


void ApplyPlayerDamageBuff(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\player-buff.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t gpc{};
    if (!FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, gpc)) {
        std::fprintf(fp, "no GetPlayerCharacter\n"); std::fclose(fp); return;
    }
    std::uint8_t pb[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(gpc), pb)) {
        std::fprintf(fp, "GetPlayerCharacter failed\n"); std::fclose(fp); return;
    }
    std::uintptr_t pawn{};
    std::memcpy(&pawn, pb, sizeof(pawn));
    if (pawn == 0) { std::fprintf(fp, "pawn=0\n"); std::fclose(fp); return; }
    std::uintptr_t pawn_cls{};
    Read(reinterpret_cast<const void*>(pawn + kObjectClassOffset), pawn_cls);
    std::uintptr_t fn{};
    if (!FindFunction(context.names, pawn_cls, "ServerActivateGameEffectsWithClassByActor", 5, 124, fn)) {
        std::fprintf(fp, "no ServerActivateGameEffectsWithClassByActor\n"); std::fclose(fp); return;
    }
    // 找 Buff_DamageUP_C 的 CDO
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) {
        std::fprintf(fp, "no items\n"); std::fclose(fp); return;
    }
    std::uintptr_t buff_cdo = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && buff_cdo == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        if (ObjectName(context.names, object) == "Default__Buff_DamageUP_C") { buff_cdo = object; }
    }
    std::fprintf(fp, "buff_cdo=%llx\n", static_cast<unsigned long long>(buff_cdo));
    if (buff_cdo == 0) { std::fprintf(fp, "Buff_DamageUP_C not found\n"); std::fclose(fp); return; }
    std::uint8_t p[124]{};
    std::memcpy(p + 0, &pawn, sizeof(pawn));
    std::memcpy(p + 8, &buff_cdo, sizeof(buff_cdo));
    // ModifyData = BufferData (off=16): nLevel(40) nStackCount(56) fStrengthMult(60) fStrengthAdd(64)
    const std::int32_t level = 1;
    const std::int32_t stack = 1;
    const float mult = 10.0f;
    const float add = 0.0f;
    std::memcpy(p + 16 + 40, &level, 4);
    std::memcpy(p + 16 + 56, &stack, 4);
    std::memcpy(p + 16 + 60, &mult, 4);
    std::memcpy(p + 16 + 64, &add, 4);
    const bool ok = Invoke(reinterpret_cast<void*>(pawn), reinterpret_cast<void*>(fn), p);
    std::fprintf(fp, "apply buff ok=%d mult=10 add=0\n", ok ? 1 : 0);
    std::fclose(fp);
}


void DumpPlayerFuncs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\player-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) {
        std::fprintf(fp, "no player state\n");
        std::fclose(fp);
        return;
    }
    std::uintptr_t cc{};
    if (!Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc) || cc == 0) {
        std::fprintf(fp, "no controller class\n");
        std::fclose(fp);
        return;
    }
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cc, "GetPlayerCharacter", 1, 8, fn)) {
        std::fprintf(fp, "no GetPlayerCharacter\n");
        std::fclose(fp);
        return;
    }
    std::uint8_t p[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(fn), p)) {
        std::fprintf(fp, "GetPlayerCharacter invoke failed\n");
        std::fclose(fp);
        return;
    }
    std::uintptr_t pawn{};
    std::memcpy(&pawn, p, sizeof(pawn));
    std::fprintf(fp, "pawn=%llx\n", static_cast<unsigned long long>(pawn));
    if (pawn == 0) { std::fclose(fp); return; }
    std::uintptr_t pawn_cls{};
    if (!Read(reinterpret_cast<const void*>(pawn + kObjectClassOffset), pawn_cls) || pawn_cls == 0) {
        std::fprintf(fp, "no pawn class\n");
        std::fclose(fp);
        return;
    }
    std::fprintf(fp, "pawn class = %s\n", ObjectName(context.names, pawn_cls).c_str());
    std::uintptr_t owner = pawn_cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
            if (ObjectName(context.names, field_class) == "Function") {
                const std::string fn_name = ObjectName(context.names, field);
                if (fn_name.find("Server") == std::string::npos &&
                    fn_name.find("GameEffect") == std::string::npos &&
                    fn_name.find("Apply") == std::string::npos) {
                    if (next == field) break;
                    field = next;
                    continue;
                }
                std::uint8_t np{}; std::uint16_t ps{};
                Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                std::fprintf(fp, "Function %s np=%u ps=%u\n", fn_name.c_str(),
                             static_cast<unsigned>(np), static_cast<unsigned>(ps));
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpMonsterParams(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\monster-params.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t cls = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && cls == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t obj_cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), obj_cls);
        const std::string obj_cls_name = obj_cls != 0 ? ObjectName(context.names, obj_cls) : std::string();
        if (ObjectName(context.names, object) == "mon_029_BP_Clone_C" &&
            (obj_cls_name == "BlueprintGeneratedClass" || obj_cls_name == "Class")) {
            cls = object;
        }
    }
    if (cls == 0) { std::fprintf(fp, "class not found\n"); std::fclose(fp); return; }
    const char* targets[] = {"ServerActivateGameEffectsWithClassByActor",
                             "ServerRemoveGameEffectsWithClass",
                             "SetHP", "SetIsDead", "KillSelf",
                             "GetHP", "GetHPMax", "SetImmediatelyChangeHP"};
    std::uintptr_t owner = cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
            if (ObjectName(context.names, field_class) == "Function") {
                const std::string fn_name = ObjectName(context.names, field);
                for (const char* t : targets) {
                    if (fn_name == t) {
                        std::uint8_t np{}; std::uint16_t ps{};
                        Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                        Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                        std::fprintf(fp, "\n=== %s np=%u ps=%u ===\n", fn_name.c_str(),
                                     static_cast<unsigned>(np), static_cast<unsigned>(ps));
                        DumpFunctionParams(context, field, fp);
                    }
                }
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpStructFields(Context& context, std::uintptr_t s, std::FILE* fp, int depth) noexcept {
    if (depth > 4) return;
    const char* pad = "    ";
    std::uintptr_t prop{};
    if (!Read(reinterpret_cast<const void*>(s + kUStructPropertyLinkOffset), prop) || prop == 0) {
        for (int i = 0; i < depth; ++i) std::fputs(pad, fp);
        std::fputs("(no props)\n", fp);
        return;
    }
    std::uint32_t k = 0;
    while (prop != 0 && k < 256) {
        std::uint32_t name_id{};
        Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
        const std::string pname = ResolveName(context.names, name_id);
        std::uint16_t esz{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
        std::int32_t off{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
        std::uintptr_t prop_class{};
        Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
        std::string tname;
        if (prop_class != 0) {
            std::uint32_t tnid{};
            if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                tname = ResolveName(context.names, tnid);
            }
        }
        for (int i = 0; i < depth; ++i) std::fputs(pad, fp);
        std::fprintf(fp, "[%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                     tname.c_str(), static_cast<unsigned>(esz), off);
        if (tname == "StructProperty") {
            std::uintptr_t inner{};
            if (Read(reinterpret_cast<const void*>(prop + kFStructPropertyStructOffset), inner) &&
                inner != 0) {
                for (int i = 0; i < depth + 1; ++i) std::fputs(pad, fp);
                std::fprintf(fp, "-> %s (%llx)\n",
                             ObjectName(context.names, inner).c_str(),
                             static_cast<unsigned long long>(inner));
                DumpStructFields(context, inner, fp, depth + 1);
            }
        } else if (tname == "ArrayProperty") {
            std::uintptr_t inner_prop{};
            if (Read(reinterpret_cast<const void*>(prop + kFArrayPropertyInnerOffset), inner_prop) &&
                inner_prop != 0) {
                std::uintptr_t ip_class{};
                Read(reinterpret_cast<const void*>(inner_prop + kFFieldClassOffset), ip_class);
                std::string ip_tname;
                if (ip_class != 0) {
                    std::uint32_t ip_tnid{};
                    if (Read(reinterpret_cast<const void*>(ip_class), ip_tnid) && ip_tnid != 0) {
                        ip_tname = ResolveName(context.names, ip_tnid);
                    }
                }
                for (int i = 0; i < depth + 1; ++i) std::fputs(pad, fp);
                std::fprintf(fp, "[inner] type=%s\n", ip_tname.c_str());
                if (ip_tname == "StructProperty") {
                    std::uintptr_t elem_struct{};
                    if (Read(reinterpret_cast<const void*>(
                                 inner_prop + kFStructPropertyStructOffset), elem_struct) &&
                        elem_struct != 0) {
                        DumpStructFields(context, elem_struct, fp, depth + 1);
                    }
                }
            }
        }
        std::uintptr_t next{};
        if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
            next == 0 || next == prop) break;
        prop = next;
        ++k;
    }
}


void DumpModifyDataStruct(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\modify-data-struct.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t cls = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && cls == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t obj_cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), obj_cls);
        const std::string obj_cls_name = obj_cls != 0 ? ObjectName(context.names, obj_cls) : std::string();
        if (ObjectName(context.names, object) == "mon_029_BP_Clone_C" &&
            (obj_cls_name == "BlueprintGeneratedClass" || obj_cls_name == "Class")) {
            cls = object;
        }
    }
    if (cls == 0) { std::fprintf(fp, "class not found\n"); std::fclose(fp); return; }
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cls, "ServerActivateGameEffectsWithClassByActor", 5, 124, fn)) {
        std::fprintf(fp, "fn not found\n"); std::fclose(fp); return;
    }
    std::fprintf(fp, "fn=%llx\n", static_cast<unsigned long long>(fn));
    std::uintptr_t prop{};
    if (!Read(reinterpret_cast<const void*>(fn + kUStructPropertyLinkOffset), prop) || prop == 0) {
        std::fprintf(fp, "no params\n"); std::fclose(fp); return;
    }
    std::uint32_t k = 0;
    while (prop != 0 && k < 16) {
        std::uint32_t name_id{};
        Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
        const std::string pname = ResolveName(context.names, name_id);
        std::uint16_t esz{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
        std::int32_t off{};
        Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
        std::uintptr_t prop_class{};
        Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
        std::string tname;
        if (prop_class != 0) {
            std::uint32_t tnid{};
            if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                tname = ResolveName(context.names, tnid);
            }
        }
        std::fprintf(fp, "[%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                     tname.c_str(), static_cast<unsigned>(esz), off);
        if (tname == "StructProperty") {
            std::uintptr_t inner{};
            if (Read(reinterpret_cast<const void*>(prop + kFStructPropertyStructOffset), inner) &&
                inner != 0) {
                std::fprintf(fp, "=== struct %s (%llx) ===\n",
                             ObjectName(context.names, inner).c_str(),
                             static_cast<unsigned long long>(inner));
                DumpStructFields(context, inner, fp, 1);
            }
        }
        std::uintptr_t next{};
        if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
            next == 0 || next == prop) break;
        prop = next;
        ++k;
    }
    std::fclose(fp);
}


void DumpGEStruct(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\ge-struct.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    const char* targets[] = {"Default__Buff_DamageUP_C", "Default__GE_Kill_150_Damage_C"};
    std::uintptr_t found[2] = {0, 0};
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        const std::string obj_name = ObjectName(context.names, object);
        for (int t = 0; t < 2; ++t) {
            if (obj_name == targets[t]) found[t] = object;
        }
    }
    for (int t = 0; t < 2; ++t) {
        const std::uintptr_t cdo = found[t];
        std::fprintf(fp, "=== %s (%llx) ===\n", targets[t],
                     static_cast<unsigned long long>(cdo));
        if (cdo == 0) continue;
        std::uintptr_t owner{};
        Read(reinterpret_cast<const void*>(cdo + kObjectClassOffset), owner);
        for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
            std::fprintf(fp, "--- depth %u: %s ---\n", depth,
                         ObjectName(context.names, owner).c_str());
            DumpStructFields(context, owner, fp, 1);
            std::uintptr_t super{};
            if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                super == 0 || super == owner) break;
            owner = super;
        }
    }
    std::fclose(fp);
}


void DumpScriptStruct(Context& context, const char* name) noexcept {
    std::FILE* fp = std::fopen("D:\\script-struct.txt", "a");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t st = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && st == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (ObjectName(context.names, object) == name && cls_name == "ScriptStruct") {
            st = object;
        }
    }
    std::fprintf(fp, "=== %s (%llx) ===\n", name, static_cast<unsigned long long>(st));
    if (st != 0) DumpStructFields(context, st, fp, 0);
    std::fclose(fp);
}

void DumpModifierInfo(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\script-struct.txt", "w");
    if (fp != nullptr) std::fclose(fp);
    DumpScriptStruct(context, "GameplayModifierInfo");
    DumpScriptStruct(context, "GameplayAttribute");
}


std::string ReadFString(Context& context, const std::uintptr_t addr) noexcept {
    std::uintptr_t data{};
    std::int32_t num{};
    if (!Read(reinterpret_cast<const void*>(addr), data) || data == 0 ||
        !Read(reinterpret_cast<const void*>(addr + 8), num) || num <= 0 || num > 512) {
        return {};
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(num));
    for (std::int32_t i = 0; i < num; ++i) {
        std::uint16_t c{};
        if (!Read(reinterpret_cast<const void*>(
                      data + static_cast<std::uintptr_t>(i) * 2), c) || c == 0) break;
        out.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return out;
}

void DumpModifiers(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\ge-modifiers.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    const char* targets[] = {"Default__Buff_DamageUP_C", "Default__GE_Kill_150_Damage_C",
                             "Default__Buff_Divination_DamageUpGeneralBase_C",
                             "Default__Buff_Haniel020_Level5_DamageUpPsycheBase_C"};
    constexpr int kTargetCount = 4;
    std::uintptr_t found[kTargetCount] = {0, 0, 0, 0};
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        const std::string obj_name = ObjectName(context.names, object);
        for (int t = 0; t < kTargetCount; ++t) {
            if (obj_name == targets[t]) found[t] = object;
        }
    }
    constexpr std::uintptr_t kModifiersArrayOffset = 584;
    constexpr std::uintptr_t kModifierInfoSize = 824;
    for (int t = 0; t < kTargetCount; ++t) {
        const std::uintptr_t cdo = found[t];
        std::fprintf(fp, "=== %s (%llx) ===\n", targets[t],
                     static_cast<unsigned long long>(cdo));
        if (cdo == 0) continue;
        std::uintptr_t arr_data{};
        std::int32_t arr_num{};
        Read(reinterpret_cast<const void*>(cdo + kModifiersArrayOffset), arr_data);
        Read(reinterpret_cast<const void*>(cdo + kModifiersArrayOffset + 8), arr_num);
        std::fprintf(fp, "modifiers data=%llx num=%d\n",
                     static_cast<unsigned long long>(arr_data), arr_num);
        if (arr_data == 0 || arr_num <= 0 || arr_num > 64) continue;
        for (std::int32_t m = 0; m < arr_num; ++m) {
            const std::uintptr_t elem =
                arr_data + static_cast<std::uintptr_t>(m) * kModifierInfoSize;
            const std::string attr_name = ReadFString(context, elem + 0);
            std::uint8_t op{};
            Read(reinterpret_cast<const void*>(elem + 56), op);
            std::uint8_t mag_type{};
            Read(reinterpret_cast<const void*>(elem + 64 + 0), mag_type);
            std::fprintf(fp, "[%d] attr=%s op=%u magtype=%u", m, attr_name.c_str(),
                         static_cast<unsigned>(op), static_cast<unsigned>(mag_type));
            if (mag_type == 0) {
                float v{};
                Read(reinterpret_cast<const void*>(elem + 64 + 8 + 0), v);
                std::fprintf(fp, " value=%f", v);
            } else if (mag_type == 3) {
                std::uint32_t cmp{};
                Read(reinterpret_cast<const void*>(elem + 64 + 464 + 0), cmp);
                std::fprintf(fp, " dataname=%s", ResolveName(context.names, cmp).c_str());
            } else if (mag_type == 2) {
                std::uintptr_t cls{};
                Read(reinterpret_cast<const void*>(elem + 64 + 320 + 0), cls);
                std::fprintf(fp, " custom=%s", ObjectName(context.names, cls).c_str());
            } else if (mag_type == 1) {
                float coef{};
                Read(reinterpret_cast<const void*>(elem + 64 + 48 + 0 + 0), coef);
                std::fprintf(fp, " coef=%f", coef);
            }
            std::fprintf(fp, "\n");
        }
    }
    std::fclose(fp);
}


void DumpClassFuncs(Context& context, const char* class_name) noexcept {
    std::FILE* fp = std::fopen("D:\\class-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) {
        std::fprintf(fp, "no gobjects\n");
        std::fclose(fp);
        return;
    }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) {
        std::fprintf(fp, "no items\n");
        std::fclose(fp);
        return;
    }
    std::uintptr_t cls = 0;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count && cls == 0; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t obj_cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), obj_cls);
        const std::string obj_cls_name = obj_cls != 0 ? ObjectName(context.names, obj_cls) : std::string();
        if (ObjectName(context.names, object) == class_name &&
            (obj_cls_name == "BlueprintGeneratedClass" || obj_cls_name == "Class")) {
            cls = object;
        }
    }
    std::fprintf(fp, "class %s = %llx\n", class_name, static_cast<unsigned long long>(cls));
    if (cls == 0) { std::fclose(fp); return; }
    std::uintptr_t owner = cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::fprintf(fp, "--- depth %u ---\n", depth);
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) {
                break;
            }
            if (ObjectName(context.names, field_class) == "Function") {
                std::uint8_t np{};
                std::uint16_t ps{};
                Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                const std::string fn = ObjectName(context.names, field);
                std::fprintf(fp, "Function %s np=%u ps=%u\n", fn.c_str(),
                             static_cast<unsigned>(np), static_cast<unsigned>(ps));
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


std::uintptr_t FindDataAsset(Context& context) noexcept {
    if (!EnsureGObjects(context)) return 0;
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) {
        return 0;
    }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string("?");
        if (cls_name != "HT_CloneSystemDataAsset") continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        context.settlement_ui_cache = object;
        return object;
    }
    {
        const auto* actors = context.actors;
        std::FILE* dfp = std::fopen("D:\\actors-diag.txt", "a");
        if (dfp != nullptr) {
            std::fprintf(dfp, "actors=%p frame=%p snapshot_at=%p class_name_utf8=%p\n",
                         static_cast<const void*>(actors),
                         actors ? static_cast<const void*>(actors->frame) : nullptr,
                         actors ? static_cast<const void*>(actors->snapshot_at) : nullptr,
                         actors ? static_cast<const void*>(actors->class_name_utf8) : nullptr);
            std::fclose(dfp);
        }
        if (actors != nullptr && actors->frame != nullptr && actors->snapshot_at != nullptr &&
            actors->class_name_utf8 != nullptr) {
            AnomalyNteEntityFrameV1 aframe{sizeof(aframe)};
            const auto st = actors->frame(actors->user, &aframe);
            dfp = std::fopen("D:\\actors-diag.txt", "a");
            if (dfp != nullptr) {
                std::fprintf(dfp, "actors frame code=%d count=%u\n",
                             static_cast<int>(st.code), aframe.entity_count);
                std::fclose(dfp);
            }
        }
    }
    return 0;
}

bool ReadDataTable(Context& context, const std::uintptr_t dt,
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

bool ReadSubInfo(Context& context, const FNamePair& sub, std::uint8_t& out_type,
                 std::vector<std::int32_t>& out_levels,
                 FNamePair* out_match,
                 FNamePair* out_spawn,
                 std::int32_t* out_team) noexcept;
void SaveCache(Context& context) noexcept;

bool LoadEntries(Context& context) noexcept {
    const std::uintptr_t da = FindDataAsset(context);
    if (da == 0) return false;
    context.data_asset = da;
    std::uintptr_t dt{};
    if (!Read(reinterpret_cast<const void*>(da + kDataAssetCloneEnterOffset), dt) ||
        dt == 0) {
        return false;
    }
    std::vector<std::pair<FNamePair, std::uintptr_t>> rows;
    if (!ReadDataTable(context, dt, rows)) return false;
    std::uintptr_t sys_dt{};
    if (Read(reinterpret_cast<const void*>(da + kDataAssetCloneSystemDataOffset), sys_dt) &&
        sys_dt != 0) {
        std::vector<std::pair<FNamePair, std::uintptr_t>> sys_rows;
        if (ReadDataTable(context, sys_dt, sys_rows)) {
            context.system_rows = std::move(sys_rows);
        }
    }
    context.entries.clear();
    context.entries.reserve(rows.size());
    for (const auto& [key, row] : rows) {
        CloneEntry e;
        e.contain_id = RenderFName(context, key);
        e.contain_fname = key;
        std::uintptr_t arr_data{};
        std::int32_t arr_num{};
        Read(reinterpret_cast<const void*>(row + kContainCloneSystemIDsOffset), arr_data);
        Read(reinterpret_cast<const void*>(row + kContainCloneSystemIDsOffset + 8), arr_num);
        if (arr_data != 0 && arr_num > 0 && arr_num <= 64) {
            for (std::int32_t j = 0; j < arr_num; ++j) {
                FNamePair sub{};
                const std::size_t off = static_cast<std::size_t>(j) * 8;
                Read(reinterpret_cast<const void*>(arr_data + off), sub.cmp);
                Read(reinterpret_cast<const void*>(arr_data + off + 4), sub.number);
                e.sub_ids.push_back(sub);
                SubInfo si;
                si.sub = sub;
                static_cast<void>(ReadSubInfo(context, sub, si.type, si.levels, &si.match, nullptr, nullptr));
                e.sub_infos.push_back(std::move(si));
            }
        }
        context.entries.push_back(std::move(e));
    }
    context.entries_loaded = true;
    context.cache_validated = true;
    SaveCache(context);
    return true;
}
void SaveCache(Context& context) noexcept {
    std::FILE* fp = std::fopen(context.cache_path.c_str(), "wb");
    if (fp == nullptr) return;
    const std::uint32_t magic = 0x434C4E45u;
    std::fwrite(&magic, sizeof(magic), 1, fp);
    std::fwrite(&context.g_objects_address, sizeof(context.g_objects_address), 1, fp);
    const std::uint32_t ec = static_cast<std::uint32_t>(context.entries.size());
    std::fwrite(&ec, sizeof(ec), 1, fp);
    for (const auto& e : context.entries) {
        const std::uint32_t len = static_cast<std::uint32_t>(e.contain_id.size());
        std::fwrite(&len, sizeof(len), 1, fp);
        std::fwrite(e.contain_id.data(), 1, len, fp);
        std::fwrite(&e.contain_fname, sizeof(e.contain_fname), 1, fp);
        const std::uint32_t sc = static_cast<std::uint32_t>(e.sub_ids.size());
        std::fwrite(&sc, sizeof(sc), 1, fp);
        for (std::size_t j = 0; j < e.sub_ids.size(); ++j) {
            std::fwrite(&e.sub_ids[j], sizeof(e.sub_ids[j]), 1, fp);
            SubInfo info;
            if (j < e.sub_infos.size()) info = e.sub_infos[j];
            std::fwrite(&info.type, sizeof(info.type), 1, fp);
            const std::uint32_t lc = static_cast<std::uint32_t>(info.levels.size());
            std::fwrite(&lc, sizeof(lc), 1, fp);
            for (std::int32_t lv : info.levels) {
                std::fwrite(&lv, sizeof(lv), 1, fp);
            }
            std::fwrite(&info.match, sizeof(info.match), 1, fp);
        }
    }
    std::fclose(fp);
}

bool LoadCache(Context& context) noexcept {
    std::FILE* fp = std::fopen(context.cache_path.c_str(), "rb");
    if (fp == nullptr) return false;
    std::uint32_t magic{};
    if (std::fread(&magic, sizeof(magic), 1, fp) != 1 || magic != 0x434C4E45u) {
        std::fclose(fp);
        return false;
    }
    std::uintptr_t cached_gobjects{};
    if (std::fread(&cached_gobjects, sizeof(cached_gobjects), 1, fp) != 1) {
        std::fclose(fp);
        return false;
    }
    static_cast<void>(cached_gobjects);
    std::uint32_t ec{};
    if (std::fread(&ec, sizeof(ec), 1, fp) != 1 || ec > 64) {
        std::fclose(fp);
        return false;
    }
    context.entries.clear();
    context.entries.reserve(ec);
    for (std::uint32_t i = 0; i < ec; ++i) {
        CloneEntry e;
        std::uint32_t len{};
        if (std::fread(&len, sizeof(len), 1, fp) != 1 || len > 512) {
            std::fclose(fp);
            return false;
        }
        e.contain_id.resize(len);
        if (std::fread(e.contain_id.data(), 1, len, fp) != len) {
            std::fclose(fp);
            return false;
        }
        if (std::fread(&e.contain_fname, sizeof(e.contain_fname), 1, fp) != 1) {
            std::fclose(fp);
            return false;
        }
        std::uint32_t sc{};
        if (std::fread(&sc, sizeof(sc), 1, fp) != 1 || sc > 64) {
            std::fclose(fp);
            return false;
        }
        for (std::uint32_t j = 0; j < sc; ++j) {
            FNamePair sub{};
            if (std::fread(&sub, sizeof(sub), 1, fp) != 1) {
                std::fclose(fp);
                return false;
            }
            e.sub_ids.push_back(sub);
            SubInfo info;
            info.sub = sub;
            if (std::fread(&info.type, sizeof(info.type), 1, fp) != 1) {
                std::fclose(fp);
                return false;
            }
            std::uint32_t lc{};
            if (std::fread(&lc, sizeof(lc), 1, fp) != 1 || lc > 64) {
                std::fclose(fp);
                return false;
            }
            for (std::uint32_t k = 0; k < lc; ++k) {
                std::int32_t lv{};
                if (std::fread(&lv, sizeof(lv), 1, fp) != 1) {
                    std::fclose(fp);
                    return false;
                }
                info.levels.push_back(lv);
            }
            if (std::fread(&info.match, sizeof(info.match), 1, fp) != 1) {
                std::fclose(fp);
                return false;
            }
            e.sub_infos.push_back(std::move(info));
        }
        context.entries.push_back(std::move(e));
    }
    std::fclose(fp);
    context.entries_loaded = true;
    context.cache_validated = false;
    {
        std::FILE* dfp = std::fopen("D:\\cache-diag.txt", "a");
        if (dfp != nullptr) {
            std::fprintf(dfp, "cache loaded (pending validation): %zu entries\n",
                         context.entries.size());
            std::fclose(dfp);
        }
    }
    return true;
}



bool ReadSubInfo(Context& context, const FNamePair& sub, std::uint8_t& out_type,
                 std::vector<std::int32_t>& out_levels,
                 FNamePair* out_match = nullptr,
                 FNamePair* out_spawn = nullptr,
                 std::int32_t* out_team = nullptr) noexcept {
    for (const auto& [key, row] : context.system_rows) {
        if (key.cmp != sub.cmp || key.number != sub.number) continue;
        Read(reinterpret_cast<const void*>(row + kDataCloneTypeOffset), out_type);
        std::uintptr_t arr_data{};
        std::int32_t arr_num{};
        Read(reinterpret_cast<const void*>(row + kDataSubNodesOffset), arr_data);
        Read(reinterpret_cast<const void*>(row + kDataSubNodesOffset + 8), arr_num);
        out_levels.clear();
        if (arr_data != 0 && arr_num > 0 && arr_num <= 64) {
            for (std::int32_t j = 0; j < arr_num; ++j) {
                const std::uintptr_t node = arr_data + static_cast<std::size_t>(j) * kSubNodeSize;
                std::int32_t diff{};
                Read(reinterpret_cast<const void*>(node + kSubNodeDifficultyOffset), diff);
                out_levels.push_back(diff);
                if (j == 0) {
                    if (out_match != nullptr) {
                        Read(reinterpret_cast<const void*>(node), out_match->cmp);
                        Read(reinterpret_cast<const void*>(node + 4), out_match->number);
                    }
                    if (out_spawn != nullptr) {
                        Read(reinterpret_cast<const void*>(node + kSubNodeSpawnOffset), out_spawn->cmp);
                        Read(reinterpret_cast<const void*>(node + kSubNodeSpawnOffset + 4), out_spawn->number);
                    }
                    if (out_team != nullptr) {
                        Read(reinterpret_cast<const void*>(node + kSubNodeTeamLevelOffset), *out_team);
                    }
                }
            }
        }
        return true;
    }
    return false;
}

void DumpStructure(Context& context) noexcept {
    if (!context.entries_loaded) {
        static_cast<void>(LoadEntries(context));
    }
    std::FILE* fp = std::fopen("D:\\clone-enter-structure.txt", "w");
    if (fp == nullptr) return;
    std::fprintf(fp, "entries=%zu\n", context.entries.size());
    for (std::size_t ei = 0; ei < context.entries.size(); ++ei) {
        const auto& e = context.entries[ei];
        std::fprintf(fp, "\n[%zu] %s (%zu sub)\n", ei + 1, e.contain_id.c_str(), e.sub_ids.size());
        for (std::size_t si = 0; si < e.sub_ids.size(); ++si) {
            std::uint8_t type{};
            std::vector<std::int32_t> levels;
            FNamePair match{};
            FNamePair spawn{};
            std::int32_t team{};
            static_cast<void>(ReadSubInfo(context, e.sub_ids[si], type, levels, &match, &spawn, &team));
            std::fprintf(fp, "  sub[%zu] %s type=%u levels=", si + 1,
                         RenderFName(context, e.sub_ids[si]).c_str(),
                         static_cast<unsigned>(type));
            for (std::size_t li = 0; li < levels.size(); ++li) {
                std::fprintf(fp, "%s%d", li ? "," : "", levels[li]);
            }
            std::fprintf(fp, " match=%s spawn=%s team=%d\n",
                         RenderFName(context, match).c_str(),
                         RenderFName(context, spawn).c_str(), team);
        }
    }
    std::fclose(fp);
}

bool FindLandmarkDest(Context& context, const std::string_view target_id,
                      double (&dest)[3], std::uint32_t* out_index = nullptr) noexcept {
    const auto* service = context.map_landmarks;
    if (!LandmarksReady(service)) return false;
    const std::uint32_t count = service->count(service->user);
    for (std::uint32_t index = 0; index < count; ++index) {
        AnomalyNteMapLandmarkSnapshotV1 snapshot{sizeof(snapshot)};
        if (service->snapshot_at(service->user, index, &snapshot).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (std::string_view(snapshot.teleport_id) != target_id) continue;
        dest[0] = snapshot.destination[0];
        dest[1] = snapshot.destination[1];
        dest[2] = snapshot.destination[2];
        if (out_index != nullptr) *out_index = index;
        return true;
    }
    return false;
}

bool TeleportToLandmark(Context& context, const std::string_view target_id,
                        double (&dest)[3]) noexcept {
    const auto* service = context.map_landmarks;
    if (!LandmarksReady(service)) return false;
    const std::uint64_t sequence = service->sequence(service->user);
    std::uint32_t index = 0;
    if (!FindLandmarkDest(context, target_id, dest, &index)) return false;
    AnomalyNteMapLandmarkTeleportRequestV1 request{sizeof(request)};
    request.mode = ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL;
    request.sequence = sequence;
    request.index = index;
    request.flags = 0;
    const auto status = service->teleport(service->user, &request);
    return status.code == ANOMALY_STATUS_V1_OK;
}

bool DoEnterClone(Context& context) noexcept {
    if (!GetPlayerState(context)) return false;
    std::uintptr_t ps_cls{};
    if (!Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset),
              ps_cls) || ps_cls == 0) {
        return false;
    }
    std::uintptr_t fn = context.enter_fn;
    if (fn == 0 || ps_cls != context.player_state_cls) {
        if (!FindFunction(context.names, ps_cls, "EnterCloneSceneRequest", 4, 24, fn)) {
            return false;
        }
        context.enter_fn = fn;
        context.player_state_cls = ps_cls;
    }
    if (!context.entries_loaded) {
        static_cast<void>(LoadEntries(context));
    }
    std::size_t di = context.display_index;
    if (di >= kDisplayCount) di = 0;
    const auto& disp = kDisplays[di];
    std::size_t ei = disp.entry_index;
    if (ei < 1) ei = 1;
    if (ei > context.entries.size()) ei = context.entries.size();
    const auto& entry = context.entries[ei - 1];
    FNamePair sub{};
    std::uint8_t type = 0;
    std::vector<std::int32_t> levels;
    FNamePair match{};
    if (entry.sub_ids.empty()) {
        sub = entry.contain_fname;
        if (entry.contain_id == "Abyss_Clone") type = 8;
    } else {
        std::size_t si = 0;
        if (disp.sub_count <= 1) {
            si = disp.subs[0].sub_index;
        } else {
            std::size_t sci = context.sub_choice_index;
            if (sci >= disp.sub_count) sci = 0;
            si = disp.subs[sci].sub_index;
        }
        if (si < entry.sub_ids.size()) {
            sub = entry.sub_ids[si];
            if (si < entry.sub_infos.size()) {
                const auto& info = entry.sub_infos[si];
                type = info.type;
                levels = info.levels;
                match = info.match;
            }
        }
    }
    std::int32_t level = 1;
    if (!levels.empty()) {
        std::size_t li = context.level_index;
        if (li < 1) li = 1;
        if (li > levels.size()) li = levels.size();
        level = levels[li - 1];
    }
    const std::uint64_t fname_val =
        (static_cast<std::uint64_t>(sub.number) << 32) | sub.cmp;
    context.current_clone_fname =
        (static_cast<std::uint64_t>(entry.contain_fname.number) << 32) |
        entry.contain_fname.cmp;
    const std::uint64_t custom_val =
        (static_cast<std::uint64_t>(match.number) << 32) | match.cmp;
    std::uint8_t p[24]{};
    std::memcpy(p + 0, &fname_val, sizeof(fname_val));
    std::memcpy(p + 8, &level, sizeof(level));
    p[12] = type;
    std::memcpy(p + 16, &custom_val, sizeof(custom_val));
    std::FILE* fp = std::fopen("D:\\clone-enter.log", "a");
    if (fp != nullptr) {
        std::fprintf(fp, "Enter %s type=%u level=%d custom=%s\n",
                     RenderFName(context, sub).c_str(),
                     static_cast<unsigned>(type), level,
                     RenderFName(context, match).c_str());
        std::fclose(fp);
    }
    return Invoke(reinterpret_cast<void*>(context.player_state),
                  reinterpret_cast<void*>(fn), p);
}

void ExitClone(Context& context) noexcept {
    if (!GetPlayerState(context)) return;
    std::uintptr_t ps_cls{};
    if (!Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset),
              ps_cls) || ps_cls == 0) {
        return;
    }
    const char* names[] = {"ServerExitCloneSceneRPC", "ServerRequestExitCloneSceneRPC"};
    for (const char* n : names) {
        std::uintptr_t fn{};
        if (FindFunction(context.names, ps_cls, n, 0, 0, fn)) {
            static_cast<void>(Invoke(reinterpret_cast<void*>(context.player_state),
                                     reinterpret_cast<void*>(fn), nullptr));
            break;
        }
    }
}


void DumpPlayerStateFuncs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\player-state-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player state\n"); std::fclose(fp); return; }
    std::uintptr_t ps_cls{};
    if (!Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset),
              ps_cls) || ps_cls == 0) {
        std::fprintf(fp, "no ps class\n"); std::fclose(fp); return;
    }
    std::fprintf(fp, "ps class = %s\n", ObjectName(context.names, ps_cls).c_str());
    std::uintptr_t owner = ps_cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
            if (ObjectName(context.names, field_class) == "Function") {
                const std::string fn_name = ObjectName(context.names, field);
                bool hit = false;
                for (const char* kw : {"Victory", "Complete", "Finish", "Clear",
                                       "Settle", "Result", "Win", "Success", "End",
                                       "Clone", "Battle", "Pass", "Manager", "Test",
                                       "Force", "GetOrCache", "Stage", "GameManager"}) {
                    if (fn_name.find(kw) != std::string::npos) { hit = true; break; }
                }
                if (hit) {
                    std::uint8_t np{}; std::uint16_t ps{};
                    Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                    Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                    std::fprintf(fp, "Function %s np=%u ps=%u\n", fn_name.c_str(),
                                 static_cast<unsigned>(np), static_cast<unsigned>(ps));
                }
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpCloneRPCParams(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\clone-rpc-params.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player state\n"); std::fclose(fp); return; }
    std::uintptr_t ps_cls{};
    if (!Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset),
              ps_cls) || ps_cls == 0) {
        std::fprintf(fp, "no ps class\n"); std::fclose(fp); return;
    }
    struct Target { const char* name; std::uint8_t np; std::uint16_t ps; };
    const Target targets[] = {
        {"ServerCommonCloneFunRPC", 1, 32},
        {"ServerSurvivalGamePlayCloneFunRPC", 1, 32},
        {"ServerPassPermitRpc", 2, 48},
        {"ServerAbyssCloneRequest", 2, 32},
        {"Server_TransferClone", 1, 8},
    };
    for (const auto& t : targets) {
        std::uintptr_t fn{};
        if (!FindFunction(context.names, ps_cls, t.name, t.np, t.ps, fn)) {
            std::fprintf(fp, "\n=== %s not found ===\n", t.name);
            continue;
        }
        std::fprintf(fp, "\n=== %s ===\n", t.name);
        std::uintptr_t prop{};
        Read(reinterpret_cast<const void*>(fn + kUStructPropertyLinkOffset), prop);
        std::uint32_t k = 0;
        while (prop != 0 && k < 16) {
            std::uint32_t name_id{};
            Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
            const std::string pname = ResolveName(context.names, name_id);
            std::uint16_t esz{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
            std::int32_t off{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
            std::uintptr_t prop_class{};
            Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
            std::string tname;
            if (prop_class != 0) {
                std::uint32_t tnid{};
                if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                    tname = ResolveName(context.names, tnid);
                }
            }
            std::fprintf(fp, "  [%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                         tname.c_str(), static_cast<unsigned>(esz), off);
            if (tname == "StructProperty") {
                std::uintptr_t inner{};
                if (Read(reinterpret_cast<const void*>(prop + kFStructPropertyStructOffset), inner) &&
                    inner != 0) {
                    std::fprintf(fp, "    struct %s:\n", ObjectName(context.names, inner).c_str());
                    DumpStructFields(context, inner, fp, 2);
                }
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                next == 0 || next == prop) break;
            prop = next;
            ++k;
        }
    }
    std::fclose(fp);
}


void DumpCloneEnums(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\clone-enums.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "Enum") continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Clone") != std::string::npos ||
            obj_name.find("Rpc") != std::string::npos ||
            obj_name.find("RPC") != std::string::npos ||
            obj_name.find("Hotta") != std::string::npos ||
            obj_name.find("Battle") != std::string::npos ||
            obj_name.find("Combat") != std::string::npos ||
            obj_name.find("Survival") != std::string::npos ||
            obj_name.find("GamePlay") != std::string::npos ||
            obj_name.find("Gameplay") != std::string::npos ||
            obj_name.find("Stage") != std::string::npos ||
            obj_name.find("Award") != std::string::npos ||
            obj_name.find("Reward") != std::string::npos) {
            std::fprintf(fp, "Enum %s\n", obj_name.c_str());
        }
    }
    std::fclose(fp);
}


void ScanCloneClasses(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\clone-classes.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "Class" && cls_name != "BlueprintGeneratedClass") continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Clone") != std::string::npos ||
            obj_name.find("Abyss") != std::string::npos) {
            std::fprintf(fp, "%s | %s\n", obj_name.c_str(), cls_name.c_str());
        }
    }
    std::fclose(fp);
}


void DumpCloneManagerFuncs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\clone-manager-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    const char* class_names[] = {
        "mon_029_ControllerBP_ResourceClone_C", "BP_CloneSeqManager_C",
    };
    constexpr int kClassCount = 2;
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t classes[kClassCount] = {};
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "Class" && cls_name != "BlueprintGeneratedClass") continue;
        const std::string obj_name = ObjectName(context.names, object);
        for (int t = 0; t < kClassCount; ++t) {
            if (obj_name == class_names[t] && classes[t] == 0) classes[t] = object;
        }
    }
    for (int t = 0; t < kClassCount; ++t) {
        const std::uintptr_t cls = classes[t];
        std::fprintf(fp, "\n=== %s (%llx) ===\n", class_names[t],
                     static_cast<unsigned long long>(cls));
        if (cls == 0) continue;
        std::uintptr_t owner = cls;
        for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
            std::uintptr_t field{};
            Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
            for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
                std::uintptr_t next{}, field_class{};
                if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                    !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
                if (ObjectName(context.names, field_class) == "Function") {
                    const std::string fn_name = ObjectName(context.names, field);
                    std::uint8_t np{}; std::uint16_t ps{};
                    Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                    Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                    std::fprintf(fp, "  Function %s np=%u ps=%u\n", fn_name.c_str(),
                                 static_cast<unsigned>(np), static_cast<unsigned>(ps));
                }
                if (next == field) break;
                field = next;
            }
            std::uintptr_t super{};
            if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                super == 0 || super == owner) break;
            owner = super;
        }
    }
    std::fclose(fp);
}


void DumpEnumValues(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\enum-values.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    const char* names[] = {"EOpActSurvivalMessageType", "EAbyssFightStage",
                           "EBossStageCond", "EAbyssRewardState"};
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uintptr_t found[4] = {0, 0, 0, 0};
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "Enum") continue;
        const std::string obj_name = ObjectName(context.names, object);
        for (int t = 0; t < 4; ++t) {
            if (obj_name == names[t]) found[t] = object;
        }
    }
    for (int t = 0; t < 4; ++t) {
        const std::uintptr_t e = found[t];
        std::fprintf(fp, "=== %s (%llx) ===\n", names[t],
                     static_cast<unsigned long long>(e));
        if (e == 0) continue;
        std::int32_t num{};
        Read(reinterpret_cast<const void*>(e + 56), num);
        std::uintptr_t data{};
        Read(reinterpret_cast<const void*>(e + 64), data);
        std::fprintf(fp, "num=%d data=%llx\n", num,
                     static_cast<unsigned long long>(data));
        if (num <= 0 || num > 512 || data == 0) continue;
        for (std::int32_t i = 0; i < num; ++i) {
            std::uint32_t cmp{}, number{};
            std::int64_t value{};
            const std::uintptr_t elem = data + static_cast<std::uintptr_t>(i) * 16;
            Read(reinterpret_cast<const void*>(elem), cmp);
            Read(reinterpret_cast<const void*>(elem + 4), number);
            Read(reinterpret_cast<const void*>(elem + 8), value);
            std::string sname = ResolveName(context.names, cmp);
            if (number != 0) {
                sname += "_";
                sname += std::to_string(number - 1);
            }
            std::fprintf(fp, "  %d: %s = %lld\n", i, sname.c_str(),
                         static_cast<long long>(value));
        }
    }
    std::fclose(fp);
}


void TriggerPassPermitAward(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\passpermit-award.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player state\n"); std::fclose(fp); return; }
    std::uintptr_t ps_cls{};
    if (!Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset),
              ps_cls) || ps_cls == 0) {
        std::fprintf(fp, "no ps class\n"); std::fclose(fp); return;
    }
    std::uintptr_t fn{};
    if (!FindFunction(context.names, ps_cls, "ServerPassPermitRpc", 2, 48, fn)) {
        std::fprintf(fp, "no ServerPassPermitRpc\n"); std::fclose(fp); return;
    }
    std::uint8_t p[48]{};
    const std::int32_t msg_type = 1;  // GetAwardRequest
    std::memcpy(p + 0, &msg_type, 4);
    const bool ok = Invoke(reinterpret_cast<void*>(context.player_state),
                           reinterpret_cast<void*>(fn), p);
    std::fprintf(fp, "ServerPassPermitRpc(GetAwardRequest) ok=%d\n", ok ? 1 : 0);
    std::fclose(fp);
}


void ActivateSkill(Context& context) noexcept {
    // 调试入口：直接 ProcessEvent 调用 ActivateAbilityFromID。
    std::FILE* fp = std::fopen("D:\\activate-skill.txt", "w");
    if (fp != nullptr) {
        std::fprintf(fp, "direct ActivateAbilityFromID path\n");
        std::fclose(fp);
    }
    (void)context;
}


bool ActivateSkillByInputId(Context& context, std::int32_t input_id) noexcept {
    if (context.host == nullptr) return false;
    const anomaly::sdk::Host host(context.host);
    context.combat = host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    context.skills = host.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
    context.skill_invocation = host.Query<AnomalyNteSkillInvocationServiceV1>(
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID, ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).get();
    if (!HasField<AnomalyNteCombatServiceV1, decltype(AnomalyNteCombatServiceV1::current_combatant)>(
            context.combat, offsetof(AnomalyNteCombatServiceV1, current_combatant)) ||
        context.combat->current_combatant == nullptr ||
        !HasField<AnomalyNteSkillsServiceV1, decltype(AnomalyNteSkillsServiceV1::page)>(
            context.skills, offsetof(AnomalyNteSkillsServiceV1, page)) ||
        context.skills->frame == nullptr || context.skills->page == nullptr ||
        !HasField<AnomalyNteSkillInvocationServiceV1, decltype(AnomalyNteSkillInvocationServiceV1::activate)>(
            context.skill_invocation, offsetof(AnomalyNteSkillInvocationServiceV1, activate)) ||
        context.skill_invocation->activate == nullptr) {
        context.combat_status = "等待框架技能服务";
        return false;
    }
    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    const auto combat_status = context.combat->current_combatant(context.combat->user, &combatant);
    const auto skill_status = context.skills->frame(context.skills->user, &frame);
    if (combat_status.code != ANOMALY_STATUS_V1_OK || skill_status.code != ANOMALY_STATUS_V1_OK ||
        (combatant.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0 ||
        (frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0 ||
        combatant.character.id != frame.character.id ||
        combatant.character.generation != frame.character.generation) {
        context.combat_status = "等待框架角色技能快照";
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
        if (context.skills->page(context.skills->user, &page_request, skills.data(), &page).code != ANOMALY_STATUS_V1_OK ||
            page.generation != frame.generation || page.returned > skills.size()) {
            context.combat_status = "框架技能快照已变化，等待刷新";
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
            const auto status = context.skill_invocation->activate(context.skill_invocation->user, &request, &result);
            if (status.code != ANOMALY_STATUS_V1_OK) {
                context.combat_status = "框架技能调用失败：" + std::to_string(status.code);
                return false;
            }
            if (result.accepted != 0) return true;
        }
        if (page.next_offset <= offset) break;
        offset = page.next_offset;
    }
    context.combat_status = matched ? "框架技能未接受本次施放" :
        "框架未找到技能 ID " + std::to_string(input_id);
    return false;
}

std::uintptr_t FindWidgetProperty(Context& context, std::uintptr_t object,
                                  std::string_view property_name) noexcept;

bool InputFieldOffset(Context& context, std::uintptr_t owner,
                          std::string_view name, std::string_view type,
                          std::int32_t size, std::int32_t& offset,
                          std::uint16_t buffer_size = 0) {
    std::uintptr_t property{};
    Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), property);
    for (unsigned i = 0; property != 0 && i < 32; ++i) {
        std::uint32_t name_id{};
        Read(reinterpret_cast<const void*>(property + kFFieldNameOffset), name_id);
        if (ResolveName(context.names, name_id) == name) {
            std::uintptr_t field_class{};
            std::uint32_t type_id{};
            std::int32_t actual_size{};
            if (!Read(reinterpret_cast<const void*>(property + kFFieldClassOffset), field_class) ||
                !Read(reinterpret_cast<const void*>(field_class), type_id) ||
                ResolveName(context.names, type_id) != type ||
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

bool ResolveNormalAttackInput(Context& context) {
    if (!GetPlayerState(context)) {
        context.combat_status = "普攻输入：等待玩家";
        return false;
    }
    auto& binding = context.normal_attack;
    if (binding.controller == context.controller && binding.world == context.cached_world &&
        binding.triggered != 0 && binding.completed != 0) return true;
    binding = {};
    NormalAttackBinding next;
    std::uintptr_t cls{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cls);
    if (!FindFunction(context.names, cls, "ActivateAbilityFromID", 2, 8, next.triggered) ||
        !FindFunction(context.names, cls, "ReleaseAbilityFromID", 2, 8, next.completed)) {
        context.combat_status = "普攻输入：未找到配对的能力输入函数";
        return false;
    }
    const auto table = FindWidgetProperty(context, context.controller, "DT_AbilityInput");
    const auto row_struct = FindWidgetProperty(context, table, "RowStruct");
    std::int32_t id_offset{}, param_offset{}, action_offset{};
    std::vector<std::pair<FNamePair, std::uintptr_t>> rows;
    if (table == 0 || ObjectName(context.names, row_struct) != "HTAbilityInputRow" ||
        !InputFieldOffset(context, row_struct, "InputID", "ByteProperty", 1, id_offset) ||
        !InputFieldOffset(context, row_struct, "Param", "IntProperty", 4, param_offset) ||
        !InputFieldOffset(context, row_struct, "InputAction", "ObjectProperty", 8, action_offset) ||
        !ReadDataTable(context, table, rows)) {
        context.combat_status = "普攻输入：角色输入绑定表不可用";
        return false;
    }
    bool found{};
    for (const auto& [name, row] : rows) {
        if (RenderFName(context, name) != "MeleeAtack") continue;
        std::uintptr_t action{};
        if (found || !Read(reinterpret_cast<const void*>(row + action_offset), action) ||
            ObjectName(context.names, action) != "IA_MeleeAttack" ||
            !Read(reinterpret_cast<const void*>(row + id_offset), next.input_id) ||
            !Read(reinterpret_cast<const void*>(row + param_offset), next.input_param)) {
            context.combat_status = "普攻输入：普通攻击绑定不兼容";
            return false;
        }
        found = true;
    }
    if (!found) {
        context.combat_status = "普攻输入：未找到普通攻击绑定";
        return false;
    }
    for (const bool pressed : {false, true}) {
        const auto fn = pressed ? next.triggered : next.completed;
        auto& value = pressed ? next.pressed_value : next.released_value;
        if (!InputFieldOffset(context, fn, "InputID", "ByteProperty", 1, id_offset, 8) ||
            !InputFieldOffset(context, fn, "Param", "IntProperty", 4, param_offset, 8)) {
            context.combat_status = "普攻输入：能力输入参数不兼容";
            return false;
        }
        value[id_offset] = next.input_id;
        std::memcpy(value.data() + param_offset, &next.input_param, sizeof(next.input_param));
    }
    next.world = context.cached_world;
    next.controller = context.controller;
    binding = next;
    return true;
}

bool InvokeNormalAttack(Context& context) {
    if (!ResolveNormalAttackInput(context)) return false;
    const auto& binding = context.normal_attack;
    alignas(8) auto pressed_value = binding.pressed_value;
    alignas(8) auto released_value = binding.released_value;
    // A tap completes in this Game callback, so unload cannot strand a held input.
    const bool pressed = Invoke(reinterpret_cast<void*>(binding.controller),
        reinterpret_cast<void*>(binding.triggered), pressed_value.data());
    const bool released = Invoke(reinterpret_cast<void*>(binding.controller),
        reinterpret_cast<void*>(binding.completed), released_value.data());
    if (!pressed || !released) context.combat_status = "普攻输入调用异常，已停止";
    return pressed && released;
}


void LogRewardDiagnostic(Context& context, const std::string& message);

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

bool TryGetCurrentCloneId(Context& context, std::uint64_t& id) noexcept {
    if (!GetPlayerState(context)) return false;
    if (context.get_cur_clone_id_fn == 0) {
        std::uintptr_t ps_cls{};
        if (!Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset),
                  ps_cls) || ps_cls == 0) return false;
        if (!FindFunction(context.names, ps_cls, "GetCurCloneID", 1, 8,
                          context.get_cur_clone_id_fn)) return false;
    }
    std::uint8_t p[8]{};
    if (!Invoke(reinterpret_cast<void*>(context.player_state),
                reinterpret_cast<void*>(context.get_cur_clone_id_fn), p)) {
        return false;
    }
    std::memcpy(&id, p, 8);
    return true;
}

std::uint64_t GetCurrentCloneId(Context& context) noexcept {
    std::uint64_t id{};
    static_cast<void>(TryGetCurrentCloneId(context, id));
    return id;
}


void StopAutoCombatMovement(Context& context) noexcept {
    if (!context.auto_combat_moving) return;
    if (context.navigation != nullptr && context.navigation->stop_movement != nullptr) {
        static_cast<void>(context.navigation->stop_movement(context.navigation->user));
    }
    context.auto_combat_moving = false;
}

void ResetAutoCombatTarget(Context& context) noexcept {
    StopAutoCombatMovement(context);
    context.target_valid = false;
    context.combat_scan_valid = false;
    context.next_target_update = {};
    // 计时必须跟着目标一起清掉：否则同一只怪离开半径后再回来时，
    // 会继承上一轮的进入时刻，一锁定就被判成尸体。
    context.auto_combat_attack_since = {};
    context.auto_combat_target_hit = false;
}

double CombatDistanceSquared(const double* from, const double* to) noexcept {
    const double dx = to[0] - from[0];
    const double dy = to[1] - from[1];
    const double dz = to[2] - from[2];
    return dx * dx + dy * dy + dz * dz;
}

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

// 实体快照句柄与伤害参与者句柄不在同一 ID 空间，句柄和位置任一命中都算同一具尸体。
bool IsDeadAutoCombatTarget(
    const Context& context, const AnomalyNteEntitySnapshotV1& snap,
    const std::chrono::steady_clock::time_point now) noexcept {
    for (const auto& dead : context.auto_combat_dead_targets) {
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
    Context& context, const AnomalyGenerationHandleV1& handle, const double* pos,
    const std::chrono::steady_clock::time_point now, bool ever_hit) noexcept {
    std::erase_if(context.auto_combat_dead_targets,
        [now](const AutoCombatDeadTarget& dead) { return now >= dead.until; });
    AutoCombatDeadTarget dead;
    dead.handle = handle;
    dead.pos[0] = pos[0];
    dead.pos[1] = pos[1];
    dead.pos[2] = pos[2];
    dead.until = now +
        (ever_hit ? kAutoCombatDeadTargetTtl : kAutoCombatNeverHitTargetTtl);
    context.auto_combat_dead_targets.push_back(dead);
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
bool FindMonsterClassIds(Context& context) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (!context.monster_class_name_ids.empty() && now < context.next_class_rescan) return true;
    std::vector<std::uint32_t> next_ids;
    const bool entities_ok = CollectMonsterClassIdsFrom(context.entities, next_ids);
    const bool actors_ok = CollectMonsterClassIdsFrom(context.actors, next_ids);
    if (!entities_ok && !actors_ok) return false;
    context.monster_class_name_ids = std::move(next_ids);
    context.next_class_rescan = now + std::chrono::seconds(5);
    return true;
}

// 伤害流是否可用。不可用时不能做尸体判定，否则会误把所有目标判成尸体。
bool CombatStreamAvailable(const Context& context) noexcept {
    return context.combat != nullptr &&
        context.combat->latest_damage_sequence != nullptr &&
        context.combat->next_damage_event != nullptr;
}

// 消费战斗伤害流，记录"玩家最近一次打出了伤害"的时刻。
// 该时刻是判断当前目标是否还能打的依据：尸体和道具类目标仍在快照里，但打不出任何伤害。
void PumpAutoCombatCombatStream(Context& context) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (context.combat == nullptr) return;
    if (context.combat->current_combatant != nullptr) {
        AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
        if (context.combat->current_combatant(context.combat->user, &combatant).code ==
            ANOMALY_STATUS_V1_OK) {
            context.auto_combat_player_handle = combatant.character.id;
        }
    }
    if (!CombatStreamAvailable(context)) return;
    const std::uint64_t latest =
        context.combat->latest_damage_sequence(context.combat->user);
    // 首次进入或序列被重置时，从当前序列起步，避免把历史伤害当成刚刚命中。
    if (context.auto_combat_damage_cursor == 0 ||
        context.auto_combat_damage_cursor > latest) {
        context.auto_combat_damage_cursor = latest;
        return;
    }
    int drained = 0;
    while (context.auto_combat_damage_cursor < latest && drained < 64) {
        AnomalyNteDamageEventV1 event{sizeof(event)};
        if (context.combat->next_damage_event(
                context.combat->user, context.auto_combat_damage_cursor, &event).code !=
            ANOMALY_STATUS_V1_OK) {
            break;
        }
        if (event.sequence <= context.auto_combat_damage_cursor) break;
        context.auto_combat_damage_cursor = event.sequence;
        ++drained;
        if (context.auto_combat_player_handle != 0 &&
            event.attacker.id == context.auto_combat_player_handle) {
            context.auto_combat_last_player_hit_at = now;
        }
    }
}

bool TeleportToPosition(Context& context, const double (&position)[3]) noexcept;

void AutoCombatTick(Context& context) noexcept {
    if (context.navigation == nullptr || context.navigation->move_to_location == nullptr ||
        !GetPlayerState(context)) {
        ResetAutoCombatTarget(context);
        context.combat_status = "等待战斗服务";
        return;
    }
    double player_pos[3]{};
    if (!SnapshotPlayerPosition(context, player_pos) ||
        !std::isfinite(player_pos[0]) || !std::isfinite(player_pos[1]) ||
        !std::isfinite(player_pos[2])) {
        ResetAutoCombatTarget(context);
        context.combat_status = "等待玩家位置";
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    PumpAutoCombatCombatStream(context);
    const double radius_cm = static_cast<double>(context.combat_search_radius_m.load(
        std::memory_order_acquire)) * 100.0;
    const double radius_squared = radius_cm * radius_cm;
    if (!context.clone_check_done) {
        ResetAutoCombatTarget(context);
        context.monster_class_name_ids.clear();
        context.next_class_rescan = {};
        context.clone_check_done = true;
        const std::uint64_t clone_id = GetCurrentCloneId(context);
        if (clone_id != 0) {
            context.last_clone_id = clone_id;
        }
    }
    if (context.target_valid) {
        const double cached_distance = CombatDistanceSquared(player_pos, context.target_pos);
        if (!std::isfinite(cached_distance) || cached_distance > radius_squared) {
            ResetAutoCombatTarget(context);
        } else if (cached_distance <=
                   kAutoCombatAttackRangeCm * kAutoCombatAttackRangeCm) {
            // 已经在打它了。若连着 kAutoCombatNoDamageGrace 一点伤害都没打出来，
            // 说明这个目标打不动：可能是还留在快照里的尸体，也可能是类名像怪、
            // 实际是道具的对象。拉黑并重新选靶。
            if (context.auto_combat_attack_since.time_since_epoch().count() == 0) {
                context.auto_combat_attack_since = now;
            }
            const bool hit_recent =
                context.auto_combat_last_player_hit_at.time_since_epoch().count() != 0 &&
                now - context.auto_combat_last_player_hit_at <= kAutoCombatNoDamageGrace;
            if (hit_recent) context.auto_combat_target_hit = true;
            if (!hit_recent && CombatStreamAvailable(context) &&
                now - context.auto_combat_attack_since > kAutoCombatNoDamageGrace) {
                BlacklistAutoCombatTarget(context, context.auto_combat_target_handle,
                    context.target_pos, now, context.auto_combat_target_hit);
                ResetAutoCombatTarget(context);
            }
        } else {
            context.auto_combat_attack_since = {};
        }
    }
    // 每秒重新找一次最近的怪。这里不能再用 !target_valid 做条件：没有目标时它会让
    // 整段扫描每帧都跑（两个服务 × 每个类名一次分页，每帧几十次分页调用），
    // 这正是"开了自动战斗就掉帧"的来源。目标释放一律走 ResetAutoCombatTarget，
    // 而它会清空 next_target_update，所以"释放后立刻重新选靶"的行为不受影响。
    if (now >= context.next_target_update) {
        context.target_valid = false;
        context.combat_scan_valid = false;
        if (FindMonsterClassIds(context)) {
            CombatTargetPick pick;
            pick.best_distance_squared = radius_squared;
            // 同一份逻辑扫两个来源，取二者中更近的那个。
            const auto skip_dead =
                [&context, now](const AnomalyNteEntitySnapshotV1& snap) {
                    return IsDeadAutoCombatTarget(context, snap, now);
                };
            const bool entities_ok = PickNearestMonsterFrom(
                context.entities, context.monster_class_name_ids, player_pos,
                radius_squared, skip_dead, pick);
            const bool actors_ok = PickNearestMonsterFrom(
                context.actors, context.monster_class_name_ids, player_pos,
                radius_squared, skip_dead, pick);
            context.combat_scan_valid = entities_ok || actors_ok;
            if (pick.valid) {
                context.target_pos[0] = pick.pos[0];
                context.target_pos[1] = pick.pos[1];
                context.target_pos[2] = pick.pos[2];
                context.target_valid = true;
                if (context.auto_combat_target_handle.id != pick.handle.id ||
                    context.auto_combat_target_handle.generation !=
                        pick.handle.generation) {
                    context.auto_combat_target_handle = pick.handle;
                    context.auto_combat_attack_since = {};
                    context.auto_combat_target_hit = false;
                }
            }
        }
        context.next_target_update = now + std::chrono::milliseconds(1000);
        if (!context.combat_scan_valid) {
            ResetAutoCombatTarget(context);
            context.combat_status = "等待目标数据";
            return;
        }
        if (!context.target_valid) {
            StopAutoCombatMovement(context);
            context.combat_status = std::to_string(context.combat_search_radius_m.load()) + "米内无怪";
            if (context.pickup != nullptr) {
                AnomalyNtePickupRequestV1 req{sizeof(req)};
                req.radius = 2000.0;
                req.maximum_items = 10;
                static_cast<void>(context.pickup->request_nearby(context.pickup->user, &req));
            }
        }
    }
    if (!context.target_valid) {
        context.auto_combat_attack_since = {};
        return;
    }
    const double dx = context.target_pos[0] - player_pos[0];
    const double dy = context.target_pos[1] - player_pos[1];
    const double dz = context.target_pos[2] - player_pos[2];
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    char status[128]{};
    std::snprintf(status, sizeof(status), "目标 %.1f米", dist / 100.0);
    context.combat_status = status;
    if (dist > kAutoCombatAttackRangeCm) {
        // 怪物包围盒中心常常无法直接寻路抵达（悬空/湖面/特殊地形），游戏导航会原地不动。
        // 开发者模式下改用传送接近，绕过不可达的寻路；抬高 200cm 避免落进地面。
        if (context.developer_mode.load(std::memory_order_acquire)) {
            double tp_target[3] = {
                context.target_pos[0], context.target_pos[1],
                context.target_pos[2] + 200.0};
            if (TeleportToPosition(context, tp_target)) {
                StopAutoCombatMovement(context);
                return;
            }
        }
        // 寻路只取水平位置，高度用玩家当前高度。
        double nav_target[3] = {
            context.target_pos[0], context.target_pos[1], player_pos[2]};
        // 游戏原生寻路被每帧重复下发会反复重置（角色原地不动），
        // 因此按间隔先 stop 再下发，与 BoxAuto 的成熟做法一致。
        if (now >= context.auto_combat_nav_retry_at) {
            context.auto_combat_nav_retry_at = now + std::chrono::seconds(2);
            StopAutoCombatMovement(context);
            if (context.navigation->move_to_location(
                    context.navigation->user, nav_target).code == ANOMALY_STATUS_V1_OK) {
                context.auto_combat_moving = true;
            }
        }
    } else {
        StopAutoCombatMovement(context);
        if (context.melee_mode) {
            ++context.melee_attack_counter;
            bool do_normal_attack = true;
            if (context.melee_attack_counter >= 4) {
                context.melee_attack_counter = 0;
                if (ActivateSkillByInputId(context, 2)) {
                    do_normal_attack = false;
                }
            }
            if (do_normal_attack && !InvokeNormalAttack(context)) {
                context.auto_attack.store(false, std::memory_order_release);
                context.one_key_active = false;
                ResetAutoCombatTarget(context);
            }
        } else {
            if (context.attack_is_melee) {
                static_cast<void>(ActivateSkillByInputId(context, 2));
            } else {
                static_cast<void>(ActivateSkillByInputId(
                    context, static_cast<std::int32_t>(context.test_input_id)));
            }
            context.attack_is_melee = !context.attack_is_melee;
        }
    }
}

void DumpCombatTarget(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\combat-target.txt", "w");
    if (fp != nullptr) {
        std::fprintf(fp, "combat service disabled; no combat target dump\n");
        std::fclose(fp);
    }
    (void)context;
}

void DumpMonsterClasses(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\monster-classes.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "Class" && cls_name != "BlueprintGeneratedClass") continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.rfind("mon_", 0) != 0) continue;
        std::uint32_t cmp{};
        Read(reinterpret_cast<const void*>(object + kObjectNameOffset), cmp);
        std::fprintf(fp, "%s cmp=%u\n", obj_name.c_str(), cmp);
    }
    std::fclose(fp);
}


void DumpCloneMonsterInfo(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\clone-monster-info.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "CloneMonsterInfoListObject" && cls_name != "CloneMonsterInfoObject" &&
            cls_name != "CloneMonsterInfo") continue;
        const std::string obj_name = ObjectName(context.names, object);
        std::fprintf(fp, "=== %s (class=%s) ===\n", obj_name.c_str(), cls_name.c_str());
        std::uintptr_t owner = cls;
        for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
            std::fprintf(fp, "--- %s ---\n", ObjectName(context.names, owner).c_str());
            std::uintptr_t prop{};
            Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
            std::uint32_t k = 0;
            while (prop != 0 && k < 512) {
                std::uint32_t name_id{};
                Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
                const std::string pname = ResolveName(context.names, name_id);
                std::uint16_t esz{};
                Read(reinterpret_cast<const void*>(prop + kFPropertyElementSizeOffset), esz);
                std::int32_t off{};
                Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
                std::uintptr_t prop_class{};
                Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
                std::string tname;
                if (prop_class != 0) {
                    std::uint32_t tnid{};
                    if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                        tname = ResolveName(context.names, tnid);
                    }
                }
                std::fprintf(fp, "  [%u] %s type=%s elem=%u off=%d\n", k, pname.c_str(),
                             tname.c_str(), static_cast<unsigned>(esz), off);
                std::uintptr_t next{};
                if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                    next == 0 || next == prop) break;
                prop = next;
                ++k;
            }
            std::uintptr_t super{};
            if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                super == 0 || super == owner) break;
            owner = super;
        }
        break;
    }
    std::fclose(fp);
}


void ScanMonsterAssets(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\monster-assets.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Monster") != std::string::npos ||
            obj_name.find("Spawn") != std::string::npos ||
            obj_name.find("mon_") != std::string::npos) {
            if (obj_name.find("Default__") == std::string::npos &&
                cls_name.find("Class") == std::string::npos) {
                std::fprintf(fp, "%s | %s\n", obj_name.c_str(), cls_name.c_str());
            }
        }
    }
    std::fclose(fp);
}


void DumpEntityClasses(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\entity-classes.txt", "w");
    if (fp == nullptr) return;
    const auto* ents = context.entities;
    if (ents == nullptr || ents->frame == nullptr || ents->page == nullptr ||
        ents->class_name_utf8 == nullptr) {
        std::fprintf(fp, "entities not ready\n"); std::fclose(fp); return;
    }
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if (ents->frame(ents->user, &frame).code != ANOMALY_STATUS_V1_OK) {
        std::fprintf(fp, "frame failed\n"); std::fclose(fp); return;
    }
    std::array<AnomalyNteEntitySnapshotV1, 256> buf{};
    for (auto& s : buf) s.struct_size = sizeof(s);
    std::vector<std::string> seen;
    std::uint32_t offset = 0;
    while (true) {
        AnomalyNteEntityPageRequestV1 req{sizeof(req)};
        req.generation = frame.generation;
        req.offset = offset;
        req.capacity = 256;
        AnomalyNteEntityPageResultV1 res{sizeof(res)};
        if (ents->page(ents->user, &req, buf.data(), &res).code != ANOMALY_STATUS_V1_OK) break;
        for (std::uint32_t j = 0; j < res.returned; ++j) {
            const auto& snap = buf[j];
            std::size_t sz = 0;
            if (ents->class_name_utf8(ents->user, snap.class_id, nullptr, &sz).code !=
                    ANOMALY_STATUS_V1_OK || sz == 0) continue;
            std::string cn(sz, '\0');
            if (ents->class_name_utf8(ents->user, snap.class_id, cn.data(), &sz).code !=
                ANOMALY_STATUS_V1_OK) continue;
            cn.resize(sz - 1);
            bool dup = false;
            for (const auto& s : seen) { if (s == cn) { dup = true; break; } }
            if (!dup) {
                seen.push_back(cn);
                std::fprintf(fp, "%s (class_id=%llu)\n", cn.c_str(),
                             static_cast<unsigned long long>(snap.class_id));
            }
        }
        if (res.next_offset == 0 || res.next_offset >= res.total_matches) break;
        offset = res.next_offset;
    }
    std::fclose(fp);
}


std::uintptr_t FindChestActor(Context& context, double* position) noexcept {
    const auto* ents = context.entities;
    if (ents == nullptr || ents->frame == nullptr || ents->snapshot_at == nullptr ||
        ents->class_name_utf8 == nullptr) return 0;
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if (ents->frame(ents->user, &frame).code != ANOMALY_STATUS_V1_OK) return 0;
    for (std::uint32_t i = 0; i < frame.entity_count; ++i) {
        AnomalyNteEntitySnapshotV1 snap{sizeof(snap)};
        if (ents->snapshot_at(ents->user, frame.generation, i, &snap).code !=
            ANOMALY_STATUS_V1_OK) continue;
        std::size_t sz = 0;
        if (ents->class_name_utf8(ents->user, snap.class_id, nullptr, &sz).code !=
                ANOMALY_STATUS_V1_OK || sz == 0) continue;
        std::string cn(sz, '\0');
        if (ents->class_name_utf8(ents->user, snap.class_id, cn.data(), &sz).code !=
                ANOMALY_STATUS_V1_OK) continue;
        cn.resize(sz - 1);
        if (cn.find("CloneChest") == std::string::npos &&
            cn.find("WeeklyCloneDropBox") == std::string::npos) continue;
        const std::uint32_t idx = static_cast<std::uint32_t>(snap.entity_id);
        const std::uint32_t candidates[2] = { idx > 0 ? idx - 1 : 0, idx };
        for (std::uint32_t ci = 0; ci < 2; ++ci) {
            const std::uint32_t candidate = candidates[ci];
            if (candidate == 0 || candidate >= 2'000'000) continue;
            void* obj = ObjectAt(context, candidate);
            if (obj == nullptr) continue;
            auto cls = ReadPointer(reinterpret_cast<const std::uint8_t*>(obj) + kObjectClassOffset);
            if (cls == nullptr) continue;
            const std::string objcn = ObjectName(context.names,
                reinterpret_cast<std::uintptr_t>(cls));
            if (objcn.find("CloneChest") != std::string::npos ||
                objcn.find("WeeklyCloneDropBox") != std::string::npos) {
                const std::string obj_name = ObjectName(context.names,
                    reinterpret_cast<std::uintptr_t>(obj));
                if (obj_name.find("Default__") == std::string::npos) {
                    if (position != nullptr) std::copy_n(snap.bounds_center, 3, position);
                    return reinterpret_cast<std::uintptr_t>(obj);
                }
            }
        }
    }
    return 0;
}


bool FindChestPos(Context& context, double (&pos)[3]) noexcept {
    const auto* ents = context.entities;
    if (ents == nullptr || ents->frame == nullptr || ents->snapshot_at == nullptr ||
        ents->class_name_utf8 == nullptr) return false;
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if (ents->frame(ents->user, &frame).code != ANOMALY_STATUS_V1_OK) return false;
    for (std::uint32_t i = 0; i < frame.entity_count; ++i) {
        AnomalyNteEntitySnapshotV1 snap{sizeof(snap)};
        if (ents->snapshot_at(ents->user, frame.generation, i, &snap).code !=
            ANOMALY_STATUS_V1_OK) continue;
        std::size_t sz = 0;
        if (ents->class_name_utf8(ents->user, snap.class_id, nullptr, &sz).code !=
                ANOMALY_STATUS_V1_OK || sz == 0) continue;
        std::string cn(sz, '\0');
        if (ents->class_name_utf8(ents->user, snap.class_id, cn.data(), &sz).code !=
                ANOMALY_STATUS_V1_OK) continue;
        cn.resize(sz - 1);
        if (cn.find("CloneChest") == std::string::npos &&
            cn.find("WeeklyCloneDropBox") == std::string::npos) continue;
        pos[0] = snap.bounds_center[0];
        pos[1] = snap.bounds_center[1];
        pos[2] = snap.bounds_center[2];
        return true;
    }
    return false;
}


bool QueryWidgetBool(Context& context, std::uintptr_t object,
                     std::string_view query, bool& value) noexcept {
    std::uintptr_t cls{}, fn{};
    std::uint8_t result{};
    if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) ||
        !FindFunction(context.names, cls, query, 1, 1, fn) ||
        !Invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn), &result)) {
        return false;
    }
    value = result != 0;
    return true;
}

void LogRewardDiagnostic(Context& context, const std::string& message) {
    if (context.core != nullptr && context.core->log != nullptr) {
        context.core->log(context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            anomaly::sdk::StringView(message));
    }
}

bool IsActiveRewardWidget(Context& context, std::uintptr_t object, bool diagnose = false) noexcept {
    bool active{}, visible{};
    const bool queried_active = QueryWidgetBool(context, object, "IsActivated", active);
    bool viewport{};
    const bool queried_viewport = (!queried_active || diagnose) &&
        QueryWidgetBool(context, object, "IsInViewport", viewport);
    const bool queried_visible = QueryWidgetBool(context, object, "IsVisible", visible);
    if (diagnose) {
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        LogRewardDiagnostic(context, "reward-window candidate=" + ObjectName(context.names, object) +
            " class=" + ObjectName(context.names, cls) +
            " activated=" + std::to_string(queried_active ? static_cast<int>(active) : -1) +
            " viewport=" + std::to_string(queried_viewport ? static_cast<int>(viewport) : -1) +
            " visible=" + std::to_string(queried_visible ? static_cast<int>(visible) : -1));
    }
    return (queried_active ? active : queried_viewport && viewport) && queried_visible && visible;
}

struct RewardWindows {
    std::uintptr_t award{};
    std::uintptr_t settlement{};
    const char* award_button{"BtnConfirm"};
    const char* award_double_button{"BtnActiveCard"};
    bool ambiguous{};
    bool complete{};
    std::uint32_t scanned{};
    std::uint32_t candidates{};
    std::uint32_t skipped{};
    const char* reason{"not started"};
};

RewardWindows FindRewardWindows(Context& context, bool diagnose = false) {
    RewardWindows result;
    const auto now = std::chrono::steady_clock::now();
    const auto finish = [&](const char* reason) {
        result.reason = reason;
        if (diagnose) {
            char address[32]{};
            std::snprintf(address, sizeof(address), "0x%llX", static_cast<unsigned long long>(context.g_objects_address));
            LogRewardDiagnostic(context, "reward-window scan=" + std::string(reason) +
                " registry=" + address +
                " scanned=" + std::to_string(result.scanned) +
                " candidates=" + std::to_string(result.candidates) +
                " skipped=" + std::to_string(result.skipped) +
                " award=" + std::to_string(result.award != 0) +
                " settlement=" + std::to_string(result.settlement != 0) +
                " ambiguous=" + std::to_string(result.ambiguous));
        } else if (!result.complete && now >= context.next_reward_scan_diag) {
            // 这条路径是按 500ms 轮询的，失败是概率性的：按 2 秒节流留一条线索，
            // 否则"找不到领奖窗口"只能看到结论、看不到原因。
            context.next_reward_scan_diag = now + std::chrono::seconds(2);
            LogRewardDiagnostic(context, "reward-window scan incomplete: " + std::string(reason) +
                " scanned=" + std::to_string(result.scanned) +
                " candidates=" + std::to_string(result.candidates) +
                " skipped=" + std::to_string(result.skipped) +
                " award=" + std::to_string(result.award != 0) +
                " settlement=" + std::to_string(result.settlement != 0));
        }
        return result;
    };
    if (!HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::snapshot_at)>(
            context.objects, offsetof(AnomalyUe5ObjectsServiceV1, snapshot_at)) ||
        context.objects->snapshot_at == nullptr) return finish("object service unavailable");
    if (!EnsureGObjects(context)) return finish("registry address unavailable");
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) return finish("registry header unreadable");
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        result.scanned = static_cast<std::uint32_t>(i + 1);
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci >= static_cast<std::uint32_t>(num_chunks)) return finish("invalid registry chunk count");
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { return finish("registry chunk unreadable"); }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        std::uint32_t class_name_id{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) || cls == 0 ||
            !Read(reinterpret_cast<const void*>(cls + kObjectNameOffset), class_name_id)) continue;
        auto [entry, inserted] = context.reward_class_kinds.try_emplace(class_name_id, 0);
        if (inserted) {
            const std::string name = ResolveName(context.names, class_name_id);
            if (name.empty()) {
                // 扫描整个对象注册表时，个别条目读到撕裂的 class 指针是正常现象。
                // 跳过它即可：一个无关对象不能让整次扫描作废（否则会变成概率性失败）。
                context.reward_class_kinds.erase(entry);
                ++result.skipped;
                continue;
            }
            if (name.find("CombatAwardReceive") != std::string::npos) entry->second = 1;
            else if (name.find("CloneSystemAwards") != std::string::npos) entry->second = 2;
            else if (name.find("CloneSystemCommonTips") != std::string::npos) entry->second = 3;
        }
        if (entry->second == 0) continue;
        ++result.candidates;
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (context.objects->snapshot_at(context.objects->user, static_cast<std::uint32_t>(i),
                &snapshot).code != ANOMALY_STATUS_V1_OK) {
            ++result.skipped;
            continue;
        }
        const auto object_name = ResolveName(context.names, snapshot.name_id);
        if (object_name.empty()) {
            ++result.skipped;
            continue;
        }
        if (object_name.starts_with("Default__") || !IsActiveRewardWidget(context, object, diagnose)) continue;
        auto& selected = entry->second == 2 ? result.settlement : result.award;
        if (selected != 0) result.ambiguous = true;
        selected = object;
        if (entry->second == 3) {
            result.award_button = "Button_Single";
            result.award_double_button = "Button_Double";
        }
    }
    // 有条目读失败时不能断言"没有窗口"：标记为不完整，让调用方下一轮重试，
    // 而不是把一次瞬时读取失败变成"未发现领奖窗口"。
    if (result.award == 0 && result.settlement == 0 && result.skipped != 0) {
        return finish("skipped unreadable registry entries");
    }
    result.complete = true;
    return finish("complete");
}

std::uintptr_t FindAwardUI(Context& context) noexcept {
    const auto windows = FindRewardWindows(context);
    return windows.complete && !windows.ambiguous ? windows.award : 0;
}

bool IsAwardUIVisible(Context& context) noexcept {
    return FindAwardUI(context) != 0;
}


std::uintptr_t FindSettlementUI(Context& context) noexcept {
    const auto windows = FindRewardWindows(context);
    return windows.complete && !windows.ambiguous ? windows.settlement : 0;
}

std::uintptr_t FindWidgetProperty(Context& context, std::uintptr_t object,
                                  std::string_view property_name) noexcept {
    std::uintptr_t cls{};
    Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
    for (unsigned depth = 0; cls != 0 && depth < 64; ++depth) {
        std::uintptr_t property{};
        Read(reinterpret_cast<const void*>(cls + kUStructPropertyLinkOffset), property);
        for (unsigned count = 0; property != 0 && count < 4096; ++count) {
            std::uint32_t name_id{};
            if (!Read(reinterpret_cast<const void*>(property + kFFieldNameOffset), name_id)) return 0;
            if (ResolveName(context.names, name_id) == property_name) {
                std::uintptr_t field_class{}, value{};
                std::uint32_t type_id{};
                std::int32_t size{}, offset{};
                if (!Read(reinterpret_cast<const void*>(property + kFFieldClassOffset), field_class) ||
                    !Read(reinterpret_cast<const void*>(field_class), type_id) ||
                    ResolveName(context.names, type_id) != "ObjectProperty" ||
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

enum class RewardClickResult { Invoked, Waiting, Unavailable, Fault };

bool RewardButtonVisible(Context& context, std::uintptr_t button, bool& visible) {
    visible = false;
    auto widget = button;
    for (unsigned depth = 0; widget != 0 && depth < 64; ++depth) {
        bool own_visibility{};
        if (!QueryWidgetBool(context, widget, "IsVisible", own_visibility)) return false;
        if (!own_visibility) return true;
        std::uintptr_t cls{}, fn{}, parent{};
        if (!Read(reinterpret_cast<const void*>(widget + kObjectClassOffset), cls) ||
            !FindFunction(context.names, cls, "GetParent", 1, 8, fn) ||
            !Invoke(reinterpret_cast<void*>(widget), reinterpret_cast<void*>(fn), &parent) ||
            parent == widget) return false;
        if (parent == 0) {
            visible = true;
            return true;
        }
        widget = parent;
    }
    return false;
}

const char* SelectRewardButton(Context& context, const RewardWindows& windows, bool prefer_double) {
    // A visible but disabled double button remains the requested reward mode.
    const std::array<const char*, 2> candidates = prefer_double
        ? std::array{windows.award_double_button, windows.award_button}
        : std::array{windows.award_button, static_cast<const char*>(nullptr)};
    for (const auto name : candidates) {
        if (name == nullptr) continue;
        const auto button = FindWidgetProperty(context, windows.award, name);
        if (button == 0) continue;
        bool visible{};
        if (!RewardButtonVisible(context, button, visible)) {
            context.combat_status = "领取按钮可见状态读取失败：" + std::string(name);
            return nullptr;
        }
        if (visible) return name;
    }
    context.combat_status = "等待领取按钮显示";
    return nullptr;
}

std::uintptr_t FindBoundButtonClick(Context& context, std::uintptr_t window,
                                   std::string_view property_name, bool& rejected) {
    const std::string marker = "_" + std::string(property_name) + "_K2Node_ComponentBoundEvent_";
    std::uintptr_t cls{}, result{};
    Read(reinterpret_cast<const void*>(window + kObjectClassOffset), cls);
    rejected = false;
    for (unsigned depth = 0; cls != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(cls + kUStructChildrenOffset), field);
        for (unsigned count = 0; field != 0 && count < 4096; ++count) {
            const auto name = ObjectName(context.names, field);
            if (name.starts_with("BndEvt__") && name.find(marker) != std::string::npos &&
                name.ends_with("_CommonButtonBaseClicked__DelegateSignature")) {
                std::uintptr_t function_class{};
                std::uint8_t num_parms{};
                std::uint16_t parms_size{};
                Read(reinterpret_cast<const void*>(field + kObjectClassOffset), function_class);
                Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), num_parms);
                Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), parms_size);
                if (ObjectName(context.names, function_class) != "Function" || num_parms != 1 || parms_size != 8) {
                    rejected = true;
                    return 0;
                }
                if (result != 0) {
                    rejected = true;
                    return 0;
                }
                result = field;
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) || next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(cls + kUStructSuperStructOffset), super) || super == cls) break;
        cls = super;
    }
    return result;
}

RewardClickResult ClickRewardButton(Context& context, std::uintptr_t window,
                                    std::string_view property_name) noexcept {
    const auto button = FindWidgetProperty(context, window, property_name);
    if (button == 0) {
        context.combat_status = "未找到按钮字段：" + std::string(property_name);
        return RewardClickResult::Unavailable;
    }
    bool visible{}, enabled{}, interactable{};
    if (!QueryWidgetBool(context, button, "IsVisible", visible) ||
        !QueryWidgetBool(context, button, "GetIsEnabled", enabled) ||
        !QueryWidgetBool(context, button, "IsInteractionEnabled", interactable)) {
        context.combat_status = "按钮状态查询不可用：" + std::string(property_name);
        return RewardClickResult::Unavailable;
    }
    if (!visible || !enabled || !interactable) {
        context.combat_status = "按钮尚不可点击：" + std::string(property_name);
        return RewardClickResult::Waiting;
    }
    bool rejected{};
    const auto bound = FindBoundButtonClick(context, window, property_name, rejected);
    if (rejected) {
        context.combat_status = "按钮蓝图点击事件不唯一或参数不兼容，已停止";
        return RewardClickResult::Unavailable;
    }
    std::uintptr_t receiver = window, fn = bound;
    std::uintptr_t parameter = button;
    void* parameters = &parameter;
    if (bound == 0) {
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(button + kObjectClassOffset), cls);
        if (!FindFunction(context.names, cls, "HandleButtonClicked", 0, 0, fn)) {
            context.combat_status = "按钮没有可用的点击事件";
            return RewardClickResult::Unavailable;
        }
        receiver = button;
        parameters = nullptr;
    }
    // The bound Blueprint event runs the game action without depending on pointer/release animation state.
    if (!Invoke(reinterpret_cast<void*>(receiver), reinterpret_cast<void*>(fn), parameters)) {
        context.combat_status = "按钮点击调用异常，已停止";
        return RewardClickResult::Fault;
    }
    context.combat_status = "已调用按钮：" + std::string(property_name) + "，等待界面变化";
    LogRewardDiagnostic(context, "reward-click dispatched field=" + std::string(property_name) +
        " button=" + ObjectName(context.names, button) + " handler=" + ObjectName(context.names, fn));
    return RewardClickResult::Invoked;
}

void ExitSettlement(Context& context) noexcept {
    const std::uintptr_t ui_obj = FindSettlementUI(context);
    if (ui_obj == 0) {
        context.combat_status = "未找到唯一的当前结算窗口";
        return;
    }
    static_cast<void>(ClickRewardButton(context, ui_obj, "Button_Exit"));
}


void ClaimReward(Context& context, std::int32_t index) noexcept {
    const auto windows = FindRewardWindows(context, true);
    if (!windows.complete) {
        context.combat_status = "领奖窗口扫描未完成：" + std::string(windows.reason);
        return;
    }
    if (windows.ambiguous) {
        context.combat_status = "存在多个活动领奖/结算窗口，已停止";
        return;
    }
    if (windows.award == 0) {
        context.combat_status = "当前未发现已打开的领奖窗口";
        return;
    }
    const auto button = SelectRewardButton(context, windows, index != 1);
    if (button != nullptr) static_cast<void>(ClickRewardButton(context, windows.award, button));
}


void DumpChestChoices(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\chest-choices.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    const std::uintptr_t chest = FindChestActor(context);
    if (chest == 0) { std::fprintf(fp, "no chest\n"); std::fclose(fp); return; }
    std::uintptr_t cls{};
    Read(reinterpret_cast<const void*>(chest + kObjectClassOffset), cls);
    std::fprintf(fp, "chest=%llx class=%s\n", static_cast<unsigned long long>(chest),
                 ObjectName(context.names, cls).c_str());
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cls, "BPGetInteractEntries", 2, 24, fn)) {
        std::fprintf(fp, "no BPGetInteractEntries\n"); std::fclose(fp); return;
    }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t trigger_fn{};
    if (FindFunction(context.names, cc, "TriggerInteract", 3, 13, trigger_fn)) {
        std::uint8_t tp[13]{};
        std::memcpy(tp + 0, &chest, sizeof(chest));
        const std::int32_t zero = 0;
        std::memcpy(tp + 8, &zero, sizeof(zero));
        tp[12] = 0;
        std::fprintf(fp, "trigger=%d\n",
                     Invoke(reinterpret_cast<void*>(context.controller),
                            reinterpret_cast<void*>(trigger_fn), tp) ? 1 : 0);
    }
    auto read_choices = [&]() {
        std::uint8_t p[24]{};
        std::memcpy(p + 0, &context.controller, sizeof(context.controller));
        if (!Invoke(reinterpret_cast<void*>(chest), reinterpret_cast<void*>(fn), p)) {
            return;
        }
        std::uintptr_t data{};
        std::int32_t cnt{}, cap{};
        std::memcpy(&data, p + 8, 8);
        std::memcpy(&cnt, p + 16, 4);
        std::memcpy(&cap, p + 20, 4);
        std::fprintf(fp, "entries data=%llx count=%d cap=%d\n",
                     static_cast<unsigned long long>(data), cnt, cap);
        if (data != 0 && cnt > 0 && cnt <= 128) {
            for (std::int32_t i = 0; i < cnt; ++i) {
                std::int32_t index{};
                Read(reinterpret_cast<const void*>(
                         data + static_cast<std::uintptr_t>(i) * 360), index);
                std::fprintf(fp, "  [%d] index=%d\n", i, index);
            }
        }
    };
    read_choices();
    std::uintptr_t add_fn{};
    if (FindFunction(context.names, cls, "BPGetAdditionalInteractEntries", 2, 24, add_fn)) {
        std::fprintf(fp, "--- additional entries ---\n");
        std::uint8_t p[24]{};
        std::memcpy(p + 0, &context.controller, sizeof(context.controller));
        if (Invoke(reinterpret_cast<void*>(chest), reinterpret_cast<void*>(add_fn), p)) {
            std::uintptr_t data{};
            std::int32_t cnt{}, cap{};
            std::memcpy(&data, p + 8, 8);
            std::memcpy(&cnt, p + 16, 4);
            std::memcpy(&cap, p + 20, 4);
            std::fprintf(fp, "add entries data=%llx count=%d cap=%d\n",
                         static_cast<unsigned long long>(data), cnt, cap);
            if (data != 0 && cnt > 0 && cnt <= 128) {
                for (std::int32_t i = 0; i < cnt; ++i) {
                    std::int32_t index{};
                    Read(reinterpret_cast<const void*>(
                             data + static_cast<std::uintptr_t>(i) * 360), index);
                    std::fprintf(fp, "  [%d] index=%d\n", i, index);
                }
            }
        }
    } else {
        std::fprintf(fp, "no BPGetAdditionalInteractEntries\n");
    }
    std::uintptr_t open_fn{};
    if (FindFunction(context.names, cls, "BP_OpenTreasureBox", 1, 8, open_fn)) {
        std::uint8_t op[8]{};
        std::memcpy(op, &context.controller, sizeof(context.controller));
        std::fprintf(fp, "open invoked=%d\n",
                     Invoke(reinterpret_cast<void*>(chest),
                            reinterpret_cast<void*>(open_fn), op) ? 1 : 0);
        read_choices();
    } else {
        std::fprintf(fp, "no BP_OpenTreasureBox\n");
    }
    std::fclose(fp);
}


void DumpChestFuncs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\chest-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    const std::uintptr_t chest = FindChestActor(context);
    if (chest == 0) { std::fprintf(fp, "no chest\n"); std::fclose(fp); return; }
    std::uintptr_t cls{};
    Read(reinterpret_cast<const void*>(chest + kObjectClassOffset), cls);
    std::fprintf(fp, "chest class=%s\n", ObjectName(context.names, cls).c_str());
    std::uintptr_t chest_vtable{};
    Read(reinterpret_cast<const void*>(chest), chest_vtable);
    for (std::ptrdiff_t voff : {0x268, 0x270}) {
        std::uintptr_t vfn{};
        if (chest_vtable != 0 && Read(reinterpret_cast<const void*>(chest_vtable + voff), vfn)) {
            std::uint8_t vbytes[256]{};
            std::fprintf(fp, "\n=== chest vtable[0x%03X] -> %p ===\n",
                         static_cast<int>(voff), reinterpret_cast<void*>(vfn));
            if (vfn != 0 && Read(reinterpret_cast<const void*>(vfn), vbytes)) {
                std::fprintf(fp, "  code:");
                for (int i = 0; i < 256; ++i) {
                    std::fprintf(fp, "%02X", vbytes[i]);
                }
                std::fprintf(fp, "\n");
            }
        }
    }
    std::uintptr_t owner = cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
            if (ObjectName(context.names, field_class) == "Function") {
                const std::string fn_name = ObjectName(context.names, field);
                bool hit = false;
                for (const char* kw : {"Reward", "Double", "Multi", "Claim", "Open",
                                       "Unlock", "Get", "Interact", "Pick", "Award",
                                       "Receive"}) {
                    if (fn_name.find(kw) != std::string::npos) { hit = true; break; }
                }
                if (hit) {
                    std::uint8_t np{}; std::uint16_t ps{};
                    Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                    Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                    std::fprintf(fp, "Function %s np=%u ps=%u\n", fn_name.c_str(),
                                 static_cast<unsigned>(np), static_cast<unsigned>(ps));
                    if (fn_name == "CanOpenUI" || fn_name == "BPStartInteract" ||
                        fn_name == "BPCanTryInteract" || fn_name == "BPGetInteractEntries") {
                        DumpFunctionParams(context, field, fp);
                        std::uintptr_t native{};
                        Read(reinterpret_cast<const void*>(field + 0xD8), native);
                        std::fprintf(fp, "  native=%p\n", reinterpret_cast<void*>(native));
                        if (native != 0 && native > 0x10000) {
                            std::uint8_t bytes[256]{};
                            if (Read(reinterpret_cast<const void*>(native), bytes)) {
                                std::fprintf(fp, "  code:");
                                for (int i = 0; i < 256; ++i) {
                                    std::fprintf(fp, "%02X", bytes[i]);
                                }
                                std::fprintf(fp, "\n");
                            }
                        }
                    }
                }
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpRewardParams(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\reward-params.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t ps_cls{};
    Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset), ps_cls);
    std::uintptr_t ps_vtable{};
    Read(reinterpret_cast<const void*>(context.player_state), ps_vtable);
    std::fprintf(fp, "player_state=%p vtable=%p\n",
                 reinterpret_cast<void*>(context.player_state),
                 reinterpret_cast<void*>(ps_vtable));
    for (std::ptrdiff_t voff : {0x1008, 0x710, 0x708}) {
        std::uintptr_t vfn{};
        if (ps_vtable != 0 && Read(reinterpret_cast<const void*>(ps_vtable + voff), vfn)) {
            std::uint8_t vbytes[256]{};
            std::fprintf(fp, "\n=== PS vtable[0x%03X] -> %p ===\n",
                         static_cast<int>(voff), reinterpret_cast<void*>(vfn));
            if (vfn != 0 && Read(reinterpret_cast<const void*>(vfn), vbytes)) {
                std::fprintf(fp, "  code:");
                for (int i = 0; i < 256; ++i) {
                    std::fprintf(fp, "%02X", vbytes[i]);
                }
                std::fprintf(fp, "\n");
            }
        }
    }
    for (std::uintptr_t addr : {0x147d9cd40ULL, 0x1491ae560ULL, 0x14917cf40ULL,
                                0x1491ace30ULL, 0x143e0d710ULL}) {
        std::uint8_t vbytes[256]{};
        std::fprintf(fp, "\n=== func %p ===\n", reinterpret_cast<void*>(addr));
        if (Read(reinterpret_cast<const void*>(addr), vbytes)) {
            std::fprintf(fp, "  code:");
            for (int i = 0; i < 256; ++i) {
                std::fprintf(fp, "%02X", vbytes[i]);
            }
            std::fprintf(fp, "\n");
        }
    }
    struct Target { const char* name; std::uint8_t np; std::uint16_t ps; std::uintptr_t cls; };
    const Target targets[] = {
        {"CloneReceiveFindAward", 1, 8, ps_cls},
        {"ReceiveCombatAward", 2, 2, ps_cls},
        {"ServerReceiveAllCombatAward", 0, 0, ps_cls},
        {"CanReceiveNormalCombatAward", 2, 5, ps_cls},
        {"CanReceiveAdvancedCombatAward", 2, 5, ps_cls},
        {"IsNormalCombatAwardReceived", 2, 5, ps_cls},
        {"IsAdvancedCombatAwardReceived", 2, 5, ps_cls},
        {"ServerInteract", 2, 12, cc},
        {"Server_UnLockTreasureBox", 1, 8, cc},
        {"ServerSetUnderCursorActor", 1, 8, cc},
        {"TriggerInteract", 3, 13, cc},
    };
    for (const auto& t : targets) {
        std::uintptr_t fn{};
        if (!FindFunction(context.names, t.cls, t.name, t.np, t.ps, fn)) {
            std::fprintf(fp, "\n=== %s not found ===\n", t.name);
            continue;
        }
        std::fprintf(fp, "\n=== %s np=%u ps=%u ===\n", t.name, t.np, t.ps);
        std::uintptr_t native{};
        Read(reinterpret_cast<const void*>(fn + 0xD8), native);
        std::fprintf(fp, "  native=%p\n", reinterpret_cast<void*>(native));
        if (native != 0 && native > 0x10000) {
            std::uint8_t bytes[256]{};
            if (Read(reinterpret_cast<const void*>(native), bytes)) {
                std::fprintf(fp, "  code:");
                for (int i = 0; i < 256; ++i) {
                    std::fprintf(fp, "%02X", bytes[i]);
                }
                std::fprintf(fp, "\n");
            }
        }
        DumpFunctionParams(context, fn, fp);
    }
    std::fclose(fp);
}


void DumpAwardFuncs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\award-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t ps_cls{};
    Read(reinterpret_cast<const void*>(context.player_state + kObjectClassOffset), ps_cls);
    for (std::uintptr_t owner : {ps_cls, cc}) {
        std::fprintf(fp, "=== %s ===\n", ObjectName(context.names, owner).c_str());
        for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
            std::uintptr_t field{};
            Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
            for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
                std::uintptr_t next{}, field_class{};
                if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                    !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
                if (ObjectName(context.names, field_class) == "Function") {
                    const std::string fn_name = ObjectName(context.names, field);
                    bool hit = false;
                    for (const char* kw : {"Award", "Reward", "Receive", "Claim",
                                           "Double", "Multi", "Chest", "Treasure",
                                           "Settle", "CombatAward"}) {
                        if (fn_name.find(kw) != std::string::npos) { hit = true; break; }
                    }
                    if (hit) {
                        std::uint8_t np{}; std::uint16_t ps{};
                        Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                        Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                        std::fprintf(fp, "Function %s np=%u ps=%u\n", fn_name.c_str(),
                                     static_cast<unsigned>(np), static_cast<unsigned>(ps));
                    }
                }
                if (next == field) break;
                field = next;
            }
            std::uintptr_t super{};
            if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                super == 0 || super == owner) break;
            owner = super;
        }
    }
    std::fclose(fp);
}


void DumpAwardUIFuncs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\award-ui-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name != "Class" && cls_name != "BlueprintGeneratedClass") continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("CloneChallengeResult") == std::string::npos &&
            obj_name.find("CloneSystemAwards") == std::string::npos &&
            obj_name.find("CloneHospitalSettlement") == std::string::npos &&
            obj_name.find("CloneSystemSkipSettlement") == std::string::npos &&
            obj_name.find("CombatAwardReceive") == std::string::npos) continue;
        std::fprintf(fp, "\n=== %s ===\n", obj_name.c_str());
        std::uintptr_t owner = object;
        for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
            std::uintptr_t field{};
            Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
            for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
                std::uintptr_t next{}, field_class{};
                if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                    !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
                if (ObjectName(context.names, field_class) == "Function") {
                    const std::string fn_name = ObjectName(context.names, field);
                    bool hit = false;
                    for (const char* kw : {"Award", "Receive", "Claim", "Double",
                                           "Multi", "Get", "Confirm", "Click", "Reward"}) {
                        if (fn_name.find(kw) != std::string::npos) { hit = true; break; }
                    }
                    if (hit) {
                        std::uint8_t np{}; std::uint16_t ps{};
                        Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                        Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                        std::fprintf(fp, "Function %s np=%u ps=%u\n", fn_name.c_str(),
                                     static_cast<unsigned>(np), static_cast<unsigned>(ps));
                    }
                }
                if (next == field) break;
                field = next;
            }
            std::uintptr_t super{};
            if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
                super == 0 || super == owner) break;
            owner = super;
        }
    }
    std::fclose(fp);
}


void DumpAwardUI(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\award-ui.txt", "w");
    if (fp == nullptr) return;
    if (!EnsureGObjects(context)) { std::fprintf(fp, "no gobjects\n"); std::fclose(fp); return; }
    std::int32_t count{}, num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) { std::fprintf(fp, "no items\n"); std::fclose(fp); return; }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto ci = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (ci != cur_chunk) {
            cur_chunk = ci;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(ci) * sizeof(void*)),
                      chunk) || chunk == 0) { continue; }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) || object == 0) { continue; }
        std::uintptr_t cls{};
        Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls);
        const std::string cls_name = cls != 0 ? ObjectName(context.names, cls) : std::string();
        if (cls_name.find("CloneSystemAwards") == std::string::npos &&
            cls_name.find("CloneChallengeResult") == std::string::npos &&
            cls_name.find("CombatAward") == std::string::npos &&
            cls_name.find("CloneHospitalSettlement") == std::string::npos &&
            cls_name.find("CloneSystemSkipSettlement") == std::string::npos) continue;
        const std::string obj_name = ObjectName(context.names, object);
        if (obj_name.find("Default__") != std::string::npos) continue;
        std::fprintf(fp, "%s | %s\n", obj_name.c_str(), cls_name.c_str());
    }
    std::fclose(fp);
}


void DumpAwardWidgets(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\award-widgets.txt", "w");
    if (fp == nullptr) return;
    const std::uintptr_t ui_obj = FindAwardUI(context);
    if (ui_obj == 0) { std::fprintf(fp, "no award ui\n"); std::fclose(fp); return; }
    std::uintptr_t ui_cls{};
    Read(reinterpret_cast<const void*>(ui_obj + kObjectClassOffset), ui_cls);
    std::fprintf(fp, "ui=%llx class=%s\n", static_cast<unsigned long long>(ui_obj),
                 ObjectName(context.names, ui_cls).c_str());
    std::uintptr_t owner = ui_cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t prop{};
        Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
        std::uint32_t k = 0;
        while (prop != 0 && k < 512) {
            std::uint32_t name_id{};
            Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
            const std::string pname = ResolveName(context.names, name_id);
            std::int32_t off{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
            std::uintptr_t prop_class{};
            Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
            std::string tname;
            if (prop_class != 0) {
                std::uint32_t tnid{};
                if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                    tname = ResolveName(context.names, tnid);
                }
            }
            if (tname == "ObjectProperty") {
                std::uintptr_t v{};
                Read(reinterpret_cast<const void*>(ui_obj + static_cast<std::uintptr_t>(off)), v);
                std::fprintf(fp, "  [%u] %s Object off=%d = %s\n", k, pname.c_str(), off,
                             v != 0 ? ObjectName(context.names, v).c_str() : "(null)");
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                next == 0 || next == prop) break;
            prop = next;
            ++k;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpSettlementUI(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\settlement-ui.txt", "w");
    if (fp == nullptr) return;
    const std::uintptr_t ui_obj = FindSettlementUI(context);
    if (ui_obj == 0) { std::fprintf(fp, "no settlement ui\n"); std::fclose(fp); return; }
    std::uintptr_t ui_cls{};
    Read(reinterpret_cast<const void*>(ui_obj + kObjectClassOffset), ui_cls);
    std::fprintf(fp, "ui=%llx class=%s\n", static_cast<unsigned long long>(ui_obj),
                 ObjectName(context.names, ui_cls).c_str());
    std::uintptr_t owner = ui_cls;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t prop{};
        Read(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), prop);
        std::uint32_t k = 0;
        while (prop != 0 && k < 512) {
            std::uint32_t name_id{};
            Read(reinterpret_cast<const void*>(prop + kFFieldNameOffset), name_id);
            const std::string pname = ResolveName(context.names, name_id);
            std::int32_t off{};
            Read(reinterpret_cast<const void*>(prop + kFPropertyOffsetInternalOffset), off);
            std::uintptr_t prop_class{};
            Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
            std::string tname;
            if (prop_class != 0) {
                std::uint32_t tnid{};
                if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                    tname = ResolveName(context.names, tnid);
                }
            }
            if (tname == "ObjectProperty") {
                std::uintptr_t v{};
                Read(reinterpret_cast<const void*>(ui_obj + static_cast<std::uintptr_t>(off)), v);
                std::fprintf(fp, "  [%u] %s Object off=%d = %s\n", k, pname.c_str(), off,
                             v != 0 ? ObjectName(context.names, v).c_str() : "(null)");
            }
            std::uintptr_t next{};
            if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset), next) ||
                next == 0 || next == prop) break;
            prop = next;
            ++k;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpBtnParams(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\btn-params.txt", "w");
    if (fp == nullptr) return;
    const std::uintptr_t ui_obj = FindAwardUI(context);
    if (ui_obj == 0) { std::fprintf(fp, "no award ui\n"); std::fclose(fp); return; }
    std::uintptr_t ui_cls{};
    Read(reinterpret_cast<const void*>(ui_obj + kObjectClassOffset), ui_cls);
    std::uintptr_t vtable{};
    Read(reinterpret_cast<const void*>(ui_obj), vtable);
    std::fprintf(fp, "ui_obj=%p class=%p vtable=%p\n",
                 reinterpret_cast<void*>(ui_obj), reinterpret_cast<void*>(ui_cls),
                 reinterpret_cast<void*>(vtable));
    for (std::ptrdiff_t voff : {0x5F8, 0x700}) {
        std::uintptr_t vfn{};
        if (vtable != 0 && Read(reinterpret_cast<const void*>(vtable + voff), vfn)) {
            std::uint8_t vbytes[512]{};
            std::fprintf(fp, "\n=== vtable[0x%03X] -> %p ===\n",
                         static_cast<int>(voff), reinterpret_cast<void*>(vfn));
            if (vfn != 0 && Read(reinterpret_cast<const void*>(vfn), vbytes)) {
                std::fprintf(fp, "  code:");
                for (int i = 0; i < 512; ++i) {
                    std::fprintf(fp, "%02X", vbytes[i]);
                }
                std::fprintf(fp, "\n");
                for (int i = 0; i < 512 - 5; ++i) {
                    if (vbytes[i] == 0xE8 || vbytes[i] == 0xE9) {
                        std::int32_t rel{};
                        std::memcpy(&rel, vbytes + i + 1, 4);
                        std::uintptr_t tgt = vfn + i + 5 + rel;
                        std::uint8_t tbytes[128]{};
                        std::fprintf(fp, "  call@+%03X -> %p", i, reinterpret_cast<void*>(tgt));
                        if (Read(reinterpret_cast<const void*>(tgt), tbytes)) {
                            std::fprintf(fp, "  [");
                            for (int k = 0; k < 128; ++k) {
                                std::fprintf(fp, "%02X", tbytes[k]);
                            }
                            std::fprintf(fp, "]");
                        }
                        std::fprintf(fp, "\n");
                    }
                }
            }
        }
    }
    const char* names[] = {"OnBtnConfirmClicked", "OnBtnActiveCardClicked", "Do_ClickButton"};
    for (const char* n : names) {
        std::uintptr_t fn{};
        if (!FindFunction(context.names, ui_cls, n, 1, 8, fn)) {
            std::fprintf(fp, "\n=== %s not found ===\n", n);
            continue;
        }
        std::fprintf(fp, "\n=== %s ===  ufunc=%p\n", n, reinterpret_cast<void*>(fn));
        std::uintptr_t native{};
        Read(reinterpret_cast<const void*>(fn + 0xD8), native);
        std::fprintf(fp, "  native=%p\n", reinterpret_cast<void*>(native));
        if (native != 0 && native > 0x10000) {
            std::uint8_t bytes[384]{};
            if (Read(reinterpret_cast<const void*>(native), bytes)) {
                std::fprintf(fp, "  code:");
                for (int i = 0; i < 384; ++i) {
                    std::fprintf(fp, "%02X", bytes[i]);
                }
                std::fprintf(fp, "\n");
                for (int i = 0; i < 384 - 5; ++i) {
                    if (bytes[i] == 0xE8 || bytes[i] == 0xE9) {
                        std::int32_t rel{};
                        std::memcpy(&rel, bytes + i + 1, 4);
                        std::uintptr_t tgt = native + i + 5 + rel;
                        std::uint8_t tbytes[160]{};
                        std::fprintf(fp, "  call/jmp@+%03X -> %p", i, reinterpret_cast<void*>(tgt));
                        if (Read(reinterpret_cast<const void*>(tgt), tbytes)) {
                            std::fprintf(fp, "  [");
                            for (int k = 0; k < 160; ++k) {
                                std::fprintf(fp, "%02X", tbytes[k]);
                            }
                            std::fprintf(fp, "]");
                        }
                        std::fprintf(fp, "\n");
                    }
                }
            }
        }
        DumpFunctionParams(context, fn, fp);
    }
    std::fclose(fp);
}


void DumpControllerClickFuncs(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\controller-click-funcs.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t owner = cc;
    for (std::uint32_t depth = 0; owner != 0 && depth < 64; ++depth) {
        std::uintptr_t field{};
        Read(reinterpret_cast<const void*>(owner + kUStructChildrenOffset), field);
        for (std::uint32_t k = 0; field != 0 && k < 8192; ++k) {
            std::uintptr_t next{}, field_class{};
            if (!Read(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !Read(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class)) break;
            if (ObjectName(context.names, field_class) == "Function") {
                const std::string fn_name = ObjectName(context.names, field);
                bool hit = false;
                for (const char* kw : {"Click", "Input", "Widget", "Button", "UI",
                                       "Simulate", "Press", "Mouse", "Cursor", "Focus"}) {
                    if (fn_name.find(kw) != std::string::npos) { hit = true; break; }
                }
                if (hit) {
                    std::uint8_t np{}; std::uint16_t ps{};
                    Read(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                    Read(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                    std::fprintf(fp, "Function %s np=%u ps=%u\n", fn_name.c_str(),
                                 static_cast<unsigned>(np), static_cast<unsigned>(ps));
                }
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t super{};
        if (!Read(reinterpret_cast<const void*>(owner + kUStructSuperStructOffset), super) ||
            super == 0 || super == owner) break;
        owner = super;
    }
    std::fclose(fp);
}


void DumpBtnGeometry(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\btn-geometry.txt", "w");
    if (fp == nullptr) return;
    const std::uintptr_t ui_obj = FindAwardUI(context);
    if (ui_obj == 0) { std::fprintf(fp, "no award ui\n"); std::fclose(fp); return; }
    std::uintptr_t ui_cls{};
    Read(reinterpret_cast<const void*>(ui_obj + kObjectClassOffset), ui_cls);
    const std::uintptr_t offs[] = {4008, 4000};
    const char* names[] = {"BtnConfirm", "BtnActiveCard"};
    for (int t = 0; t < 2; ++t) {
        std::uintptr_t btn_obj{};
        Read(reinterpret_cast<const void*>(ui_obj + offs[t]), btn_obj);
        std::fprintf(fp, "%s = %llx\n", names[t],
                     static_cast<unsigned long long>(btn_obj));
        if (btn_obj == 0) continue;
        std::uintptr_t btn_cls{};
        Read(reinterpret_cast<const void*>(btn_obj + kObjectClassOffset), btn_cls);
        std::uintptr_t fn{};
        if (!FindFunction(context.names, btn_cls, "GetCachedGeometry", 1, 56, fn)) {
            std::fprintf(fp, "  no GetCachedGeometry\n");
            continue;
        }
        std::uint8_t g[56]{};
        if (!Invoke(reinterpret_cast<void*>(btn_obj), reinterpret_cast<void*>(fn), g)) {
            std::fprintf(fp, "  invoke failed\n");
            continue;
        }
        for (std::size_t o = 0; o < 56; o += 4) {
            float f{};
            std::memcpy(&f, g + o, 4);
            std::fprintf(fp, "  +%02zu: %f\n", o, f);
        }
    }
    std::fclose(fp);
}


void DumpClickParams(Context& context) noexcept {
    std::FILE* fp = std::fopen("D:\\click-params.txt", "w");
    if (fp == nullptr) return;
    if (!GetPlayerState(context)) { std::fprintf(fp, "no player\n"); std::fclose(fp); return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    struct Target { const char* name; std::uint8_t np; std::uint16_t ps; };
    const Target targets[] = {
        {"SetMouseLocation", 2, 8},
        {"GetMousePosition", 3, 9},
        {"Input_LeftMouseTriggered", 1, 32},
        {"Input_LeftMouseCompleted", 1, 32},
        {"Input_E", 1, 32},
    };
    for (const auto& t : targets) {
        std::uintptr_t fn{};
        if (!FindFunction(context.names, cc, t.name, t.np, t.ps, fn)) {
            std::fprintf(fp, "\n=== %s not found ===\n", t.name);
            continue;
        }
        std::fprintf(fp, "\n=== %s ===  ufunc=%p\n", t.name, reinterpret_cast<void*>(fn));
        std::uintptr_t native{};
        Read(reinterpret_cast<const void*>(fn + 0xD8), native);
        std::fprintf(fp, "  native=%p\n", reinterpret_cast<void*>(native));
        if (native != 0 && native > 0x10000) {
            std::uint8_t bytes[384]{};
            if (Read(reinterpret_cast<const void*>(native), bytes)) {
                std::fprintf(fp, "  code:");
                for (int i = 0; i < 384; ++i) {
                    std::fprintf(fp, "%02X", bytes[i]);
                }
                std::fprintf(fp, "\n");
                for (int i = 0; i < 384 - 5; ++i) {
                    if (bytes[i] == 0xE8 || bytes[i] == 0xE9) {
                        std::int32_t rel{};
                        std::memcpy(&rel, bytes + i + 1, 4);
                        std::uintptr_t tgt = native + i + 5 + rel;
                        std::uint8_t tbytes[128]{};
                        std::fprintf(fp, "  call/jmp@+%03X -> %p", i, reinterpret_cast<void*>(tgt));
                        if (Read(reinterpret_cast<const void*>(tgt), tbytes)) {
                            std::fprintf(fp, "  [");
                            for (int k = 0; k < 128; ++k) {
                                std::fprintf(fp, "%02X", tbytes[k]);
                            }
                            std::fprintf(fp, "]");
                        }
                        std::fprintf(fp, "\n");
                    }
                }
            }
        }
        DumpFunctionParams(context, fn, fp);
        if (t.np >= 1 && t.ps >= 32) {
            std::uintptr_t prop{};
            Read(reinterpret_cast<const void*>(fn + kUStructPropertyLinkOffset), prop);
            while (prop != 0) {
                std::uintptr_t prop_class{};
                Read(reinterpret_cast<const void*>(prop + kFFieldClassOffset), prop_class);
                std::string tname;
                if (prop_class != 0) {
                    std::uint32_t tnid{};
                    if (Read(reinterpret_cast<const void*>(prop_class), tnid) && tnid != 0) {
                        tname = ResolveName(context.names, tnid);
                    }
                }
                if (tname == "StructProperty") {
                    std::uintptr_t inner{};
                    if (Read(reinterpret_cast<const void*>(prop + kFStructPropertyStructOffset),
                             inner) && inner != 0) {
                        std::fprintf(fp, "  struct %s:\n",
                                     ObjectName(context.names, inner).c_str());
                        DumpStructFields(context, inner, fp, 2);
                    }
                }
                std::uintptr_t next{};
                if (!Read(reinterpret_cast<const void*>(prop + kFPropertyPropertyLinkNextOffset),
                          next) || next == 0 || next == prop) break;
                prop = next;
            }
        }
    }
    std::fclose(fp);
}


std::string GetPluginDir() noexcept {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&GetPluginDir), &mod) ||
        mod == nullptr) {
        return "D:\\";
    }
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
    if (n == 0) return "D:\\";
    std::wstring ws(buf, n);
    const auto pos = ws.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return "D:\\";
    ws = ws.substr(0, pos);
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "D:\\";
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}


std::string GetConfigDir() noexcept {
    std::string dir = GetPluginDir();
    if (dir.size() >= 4 && dir[0] == '\\' && dir[1] == '\\' &&
        dir[2] == '?' && dir[3] == '\\') {
        dir = dir.substr(4);
    }
    std::string lower = dir;
    for (auto& c : lower) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    const auto pos = lower.find("\\plugins\\");
    if (pos != std::string::npos) {
        dir = dir.substr(0, pos);
    }
    dir += "\\state\\plugins\\local.clone-enter";
    std::wstring wdir(dir.begin(), dir.end());
    CreateDirectoryW(wdir.c_str(), nullptr);
    return dir;
}


void GetClientSize(std::int32_t& w, std::int32_t& h) noexcept {
    HWND hwnd = FindWindowW(L"UnrealWindow", nullptr);
    if (hwnd == nullptr) hwnd = GetForegroundWindow();
    w = 0; h = 0;
    if (hwnd == nullptr) return;
    RECT r{};
    GetClientRect(hwnd, &r);
    w = r.right;
    h = r.bottom;
}


void SaveClickConfig(const Context& context) noexcept {
    const std::string path = GetConfigDir() + "\\config-click.txt";
    std::FILE* fp = std::fopen(path.c_str(), "w");
    if (fp == nullptr) return;
    std::fprintf(fp, "click_x=%d\nclick_y=%d\nclient_width=%d\nclient_height=%d\n",
                 context.click_x, context.click_y,
                 context.click_client_width, context.click_client_height);
    std::fclose(fp);
}


void LoadClickConfig(Context& context) noexcept {
    const std::string path = GetConfigDir() + "\\config-click.txt";
    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (fp == nullptr) {
        fp = std::fopen("D:\\clone-enter-config.txt", "r");
        if (fp != nullptr) {
            std::int32_t x = 0, y = 0, w = 0, h = 0;
            if (std::fscanf(fp, "click_x=%d\nclick_y=%d\nclient_width=%d\nclient_height=%d\n",
                            &x, &y, &w, &h) == 4) {
                context.click_x = x;
                context.click_y = y;
                context.click_client_width = w;
                context.click_client_height = h;
                SaveClickConfig(context);
            }
            std::fclose(fp);
            DeleteFileW(L"D:\\clone-enter-config.txt");
            return;
        }
        context.click_x = 1259;
        context.click_y = 702;
        context.click_client_width = 1920;
        context.click_client_height = 1080;
        return;
    }
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    if (std::fscanf(fp, "click_x=%d\nclick_y=%d\nclient_width=%d\nclient_height=%d\n",
                    &x, &y, &w, &h) == 4) {
        context.click_x = x;
        context.click_y = y;
        context.click_client_width = w;
        context.click_client_height = h;
    }
    std::fclose(fp);
}


void GetEffectiveClickPos(const Context& context, std::int32_t& x, std::int32_t& y) noexcept {
    x = context.click_x;
    y = context.click_y;
    if (context.click_client_width <= 0 || context.click_client_height <= 0) return;
    std::int32_t cw = 0, ch = 0;
    GetClientSize(cw, ch);
    if (cw <= 0 || ch <= 0) return;
    if (cw == context.click_client_width && ch == context.click_client_height) return;
    x = static_cast<std::int32_t>(
        static_cast<double>(context.click_x) * cw / context.click_client_width);
    y = static_cast<std::int32_t>(
        static_cast<double>(context.click_y) * ch / context.click_client_height);
}


void GetEffectiveClaimPos(const Context& context, std::int32_t& x, std::int32_t& y) noexcept {
    if (context.auto_claim_is_weekly && context.weekly_click_x != 0) {
        x = context.weekly_click_x;
        y = context.weekly_click_y;
    } else {
        x = context.click_x;
        y = context.click_y;
    }
    if (context.click_client_width <= 0 || context.click_client_height <= 0) return;
    std::int32_t cw = 0, ch = 0;
    GetClientSize(cw, ch);
    if (cw <= 0 || ch <= 0) return;
    if (cw == context.click_client_width && ch == context.click_client_height) return;
    x = static_cast<std::int32_t>(static_cast<double>(x) * cw / context.click_client_width);
    y = static_cast<std::int32_t>(static_cast<double>(y) * ch / context.click_client_height);
}


void RecordMousePos(Context& context) noexcept {
    if (!GetPlayerState(context)) { context.combat_status = "无玩家"; return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cc, "GetMousePosition", 3, 9, fn)) {
        context.combat_status = "未找到GetMousePosition"; return;
    }
    std::uint8_t p[9]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller),
                reinterpret_cast<void*>(fn), p)) {
        context.combat_status = "GetMousePosition失败"; return;
    }
    float x{}, y{};
    std::memcpy(&x, p + 0, 4);
    std::memcpy(&y, p + 4, 4);
    context.click_x = static_cast<std::int32_t>(x);
    context.click_y = static_cast<std::int32_t>(y);
    GetClientSize(context.click_client_width, context.click_client_height);
    SaveClickConfig(context);
    context.combat_status = "已记录副本领取按钮位置 " + std::to_string(context.click_x) + "," +
        std::to_string(context.click_y) + " 分辨率" +
        std::to_string(context.click_client_width) + "x" +
        std::to_string(context.click_client_height);
}


void RecordExitPos(Context& context) noexcept {
    if (!GetPlayerState(context)) { context.combat_status = "无玩家"; return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cc, "GetMousePosition", 3, 9, fn)) {
        context.combat_status = "未找到GetMousePosition"; return;
    }
    std::uint8_t p[9]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller),
                reinterpret_cast<void*>(fn), p)) {
        context.combat_status = "GetMousePosition失败"; return;
    }
    float x{}, y{};
    std::memcpy(&x, p + 0, 4);
    std::memcpy(&y, p + 4, 4);
    context.exit_click_x = static_cast<std::int32_t>(x);
    context.exit_click_y = static_cast<std::int32_t>(y);
    const std::string epath = GetConfigDir() + "\\config-exit.txt";
    std::FILE* fp = std::fopen(epath.c_str(), "w");
    if (fp != nullptr) {
        std::fprintf(fp, "%d %d\n", context.exit_click_x, context.exit_click_y);
        std::fclose(fp);
    }
    context.combat_status = "记录退出位置 " + std::to_string(context.exit_click_x) + "," +
        std::to_string(context.exit_click_y);
}


void LoadExitConfig(Context& context) noexcept {
    const std::string epath = GetConfigDir() + "\\config-exit.txt";
    std::FILE* fp = std::fopen(epath.c_str(), "r");
    if (fp == nullptr) {
        fp = std::fopen("D:\\clone-enter-exit-config.txt", "r");
        if (fp != nullptr) {
            std::int32_t x = 0, y = 0;
            if (std::fscanf(fp, "%d %d", &x, &y) == 2) {
                context.exit_click_x = x;
                context.exit_click_y = y;
                const std::string npath = GetConfigDir() + "\\config-exit.txt";
                std::FILE* nfp = std::fopen(npath.c_str(), "w");
                if (nfp != nullptr) {
                    std::fprintf(nfp, "%d %d\n", x, y);
                    std::fclose(nfp);
                }
            }
            std::fclose(fp);
            DeleteFileW(L"D:\\clone-enter-exit-config.txt");
            return;
        }
        context.exit_click_x = 746;
        context.exit_click_y = 918;
        return;
    }
    std::int32_t x = 0, y = 0;
    if (std::fscanf(fp, "%d %d", &x, &y) == 2) {
        context.exit_click_x = x;
        context.exit_click_y = y;
    }
    std::fclose(fp);
}


void RecordWeeklyPos(Context& context) noexcept {
    if (!GetPlayerState(context)) { context.combat_status = "无玩家"; return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t fn{};
    if (!FindFunction(context.names, cc, "GetMousePosition", 3, 9, fn)) {
        context.combat_status = "未找到GetMousePosition"; return;
    }
    std::uint8_t p[9]{};
    if (!Invoke(reinterpret_cast<void*>(context.controller),
                reinterpret_cast<void*>(fn), p)) {
        context.combat_status = "GetMousePosition失败"; return;
    }
    float x{}, y{};
    std::memcpy(&x, p + 0, 4);
    std::memcpy(&y, p + 4, 4);
    context.weekly_click_x = static_cast<std::int32_t>(x);
    context.weekly_click_y = static_cast<std::int32_t>(y);
    const std::string wpath = GetConfigDir() + "\\config-weekly.txt";
    std::FILE* fp = std::fopen(wpath.c_str(), "w");
    if (fp != nullptr) {
        std::fprintf(fp, "%d %d\n", context.weekly_click_x, context.weekly_click_y);
        std::fclose(fp);
    }
    context.combat_status = "记录周本领取位置 " + std::to_string(context.weekly_click_x) + "," +
        std::to_string(context.weekly_click_y);
}


void LoadWeeklyConfig(Context& context) noexcept {
    const std::string wpath = GetConfigDir() + "\\config-weekly.txt";
    std::FILE* fp = std::fopen(wpath.c_str(), "r");
    if (fp == nullptr) {
        context.weekly_click_x = 986;
        context.weekly_click_y = 696;
        return;
    }
    std::int32_t x = 0, y = 0;
    if (std::fscanf(fp, "%d %d", &x, &y) == 2) {
        context.weekly_click_x = x;
        context.weekly_click_y = y;
    }
    std::fclose(fp);
}


void MoveSystemCursor(std::int32_t vx, std::int32_t vy) noexcept {
    HWND hwnd = FindWindowW(L"UnrealWindow", nullptr);
    if (hwnd == nullptr) hwnd = GetForegroundWindow();
    if (hwnd == nullptr) return;
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    SetCursorPos(origin.x + vx, origin.y + vy);
}


void SendMouseButton(bool down) noexcept {
    HWND hwnd = FindWindowW(L"UnrealWindow", nullptr);
    if (hwnd == nullptr) hwnd = GetForegroundWindow();
    if (hwnd == nullptr) return;
    POINT cur{};
    GetCursorPos(&cur);
    ScreenToClient(hwnd, &cur);
    LPARAM lp = (static_cast<LPARAM>(cur.y) << 16) | (static_cast<LPARAM>(cur.x) & 0xFFFF);
    if (down) {
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    } else {
        PostMessageW(hwnd, WM_LBUTTONUP, 0, lp);
    }
}


void SendKeyF(bool down) noexcept {
    HWND hwnd = FindWindowW(L"UnrealWindow", nullptr);
    if (hwnd == nullptr) hwnd = GetForegroundWindow();
    if (hwnd == nullptr) return;
    if (down) {
        PostMessageW(hwnd, WM_KEYDOWN, 'F', 0);
    } else {
        PostMessageW(hwnd, WM_KEYUP, 'F', 0);
    }
}


void SimulateClick(Context& context) noexcept {
    if (!GetPlayerState(context)) { context.combat_status = "无玩家"; return; }
    std::uintptr_t cc{};
    Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cc);
    std::uintptr_t fn{};
    if (context.click_phase == 0) {
        std::int32_t ex = 0, ey = 0;
        GetEffectiveClickPos(context, ex, ey);
        if (FindFunction(context.names, cc, "SetMouseLocation", 2, 8, fn)) {
            std::uint8_t p[8]{};
            std::memcpy(p + 0, &ex, 4);
            std::memcpy(p + 4, &ey, 4);
            static_cast<void>(Invoke(reinterpret_cast<void*>(context.controller),
                                     reinterpret_cast<void*>(fn), p));
        }
        MoveSystemCursor(ex, ey);
        context.click_phase = 1;
        context.click_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(400);
        context.combat_status = "鼠标已到位，400ms后按下";
        return;
    }
    if (context.click_phase == 1) {
        std::int32_t ex = 0, ey = 0;
        GetEffectiveClickPos(context, ex, ey);
        MoveSystemCursor(ex, ey);
        SendMouseButton(true);
        context.click_phase = 2;
        context.click_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(120);
        context.combat_status = "已按下，120ms后松开";
        return;
    }
    SendMouseButton(false);
    context.click_phase = 0;
    context.combat_status = "已模拟点击 (" + std::to_string(context.click_x) + "," +
        std::to_string(context.click_y) + ")";
}


void StopAutoClaim(Context& context, const std::string& reason) noexcept {
    context.auto_claim_active = false;
    context.auto_claim_succeeded = false;
    context.combat_status = reason;
    // 临时诊断：自动领取终止只写 UI 状态，日志里只剩"没打开窗口"这个结论。
    LogRewardDiagnostic(context, "autoclaim stop: " + reason);
}

void StartAutoClaim(Context& context) noexcept {
    context.auto_claim_active = true;
    context.auto_claim_succeeded = false;
    context.auto_claim_phase = 0;
    context.auto_claim_retries = 0;
    context.auto_claim_polls = 0;
    context.auto_claim_deadline_polls = 0;
    context.auto_claim_limit_polls = 90;
    context.auto_claim_chest = 0;
    context.auto_claim_approach_polls = 0;
    context.auto_claim_poll = {};
    context.auto_claim_nav_issue = {};
    context.auto_claim_nav_deadline = {};
    context.combat_status = "自动领取：开始";
}

bool TriggerRewardChest(Context& context, std::uintptr_t chest,
                        const char** reason = nullptr) noexcept {
    const auto fail = [reason](const char* text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    std::uintptr_t cls{}, fn{};
    if (chest == 0) return fail("chest handle is null");
    if (!Read(reinterpret_cast<const void*>(context.controller + kObjectClassOffset), cls) ||
        cls == 0) return fail("controller class unreadable");
    if (!FindFunction(context.names, cls, "TriggerInteract", 3, 13, fn)) {
        return fail("TriggerInteract not found on the controller class hierarchy");
    }
    std::array<std::uint8_t, 13> parameters{};
    std::memcpy(parameters.data(), &chest, sizeof(chest));
    if (!Invoke(reinterpret_cast<void*>(context.controller), reinterpret_cast<void*>(fn),
                parameters.data())) {
        return fail("TriggerInteract process-event call failed");
    }
    if (reason != nullptr) *reason = "ok";
    return true;
}

void OpenRewardWindow(Context& context) {
    const bool moving_to_chest = context.auto_claim_active &&
        context.auto_claim_nav_deadline != std::chrono::steady_clock::time_point{};
    if (moving_to_chest && !context.auto_combat_moving && context.navigation != nullptr &&
        context.navigation->stop_movement != nullptr) {
        context.navigation->stop_movement(context.navigation->user);
    }
    context.auto_claim_active = false;
    context.auto_claim_succeeded = false;
    context.one_key_active = false;
    context.enter_pending.store(false, std::memory_order_release);
    context.one_key_pending.store(false, std::memory_order_release);
    context.auto_claim_pending.store(false, std::memory_order_release);
    context.enter_waiting_landmark = false;
    context.enter_arrived = false;
    context.auto_attack.store(false, std::memory_order_release);
    context.test_attack_waiting = false;
    ResetAutoCombatTarget(context);
    if (!GetPlayerState(context)) {
        context.combat_status = "打开领奖窗口：等待玩家";
        return;
    }
    const auto windows = FindRewardWindows(context);
    // 这条路径原本失败时只写 UI 状态，"打开窗口失败"就查不出原因。按 2 秒节流记录分支。
    const auto report_failure = [&context](const std::string& text) {
        const auto now = std::chrono::steady_clock::now();
        if (now < context.next_reward_open_diag) return;
        context.next_reward_open_diag = now + std::chrono::seconds(2);
        LogRewardDiagnostic(context, "open-reward-window failed: " + text);
    };
    if (!windows.complete || windows.ambiguous) {
        context.combat_status = "打开领奖窗口：窗口状态不可用";
        report_failure("window state unavailable complete=" +
            std::to_string(windows.complete ? 1 : 0) + " ambiguous=" +
            std::to_string(windows.ambiguous ? 1 : 0) + " reason=" + windows.reason +
            " scanned=" + std::to_string(windows.scanned) + " candidates=" +
            std::to_string(windows.candidates) + " skipped=" + std::to_string(windows.skipped));
        return;
    }
    if (windows.award != 0 || windows.settlement != 0) {
        context.combat_status = "领奖或结算窗口已打开";
        return;
    }
    double chest_position[3]{}, player_position[3]{};
    const auto chest = FindChestActor(context, chest_position);
    if (chest == 0) {
        context.combat_status = "打开领奖窗口：未找到宝箱";
        report_failure("chest actor not found in the entity snapshot");
        return;
    }
    std::string distance = "距离未知";
    if (SnapshotPlayerPosition(context, player_position)) {
        const double metres = std::sqrt(CombatDistanceSquared(player_position, chest_position)) / 100.0;
        if (std::isfinite(metres)) {
            char text[64]{};
            std::snprintf(text, sizeof(text), "距宝箱 %.1f 米", metres);
            distance = text;
        }
    }
    const char* reason = "not attempted";
    const bool triggered = TriggerRewardChest(context, chest, &reason);
    context.combat_status = triggered
        ? "已请求打开领奖窗口，" + distance : "宝箱交互调用失败，" + distance;
    if (!triggered) report_failure("chest interaction failed: " + std::string(reason));
}

// 宝箱原点偏低，传送落点抬高一些，避免落进地面/箱体里。
constexpr double kTeleportChestZOffset = 200.0;

// 开发者模式下代替寻路：直接传送到目标点。world/player 句柄取自当前快照，
// 过期句柄由 Host 拒绝，不暴露 UE 对象指针。
bool TeleportToPosition(Context& context, const double (&position)[3]) noexcept {
    if (context.session == nullptr) {
        // session 服务可能晚于插件加载才发布（加载时世界还没初始化），惰性重试。
        context.session = anomaly::sdk::Host(context.host)
            .Query<AnomalyNteSessionServiceV1>(
                ANOMALY_NTE_SESSION_SERVICE_V1_ID,
                ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    }
    if (context.session == nullptr || context.player == nullptr ||
        context.teleport == nullptr || context.teleport->teleport == nullptr) {
        return false;
    }
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    if (context.session->snapshot(context.session->user, &session_snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        context.player->snapshot(context.player->user, &player_snapshot).code !=
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
    return context.teleport->teleport(context.teleport->user, &request).code ==
        ANOMALY_STATUS_V1_OK;
}

// 宝箱交互的实际有效距离。实测从 4.3~10.8 米外调用 TriggerInteract，每次都返回成功
// 但游戏不开窗；贴到约 2 米（坐标 z 抬高 200cm 后）再交互才会生效。原先接近阈值写成
// 1200cm，导致从未执行接近动作、一直从远处交互。这里统一成一个常量。
constexpr double kAutoClaimApproachRangeCm = 300.0;
// 等待窗口状态变化的采样次数上限。实测有一次成功的领取用了约 13 次采样才等到结算窗口，
// 原先的 15 次余量太薄，所以放宽。
constexpr std::uint32_t kAutoClaimWaitPolls = 25;

void AutoClaimTick(Context& context) noexcept {
    if (!context.auto_claim_active) return;
    const auto now = std::chrono::steady_clock::now();
    // 时限按采样次数判定：游戏 tick 停摆时不会把"没被调用"算成"等待超时"。
    if (context.auto_claim_polls >= context.auto_claim_limit_polls) {
        StopAutoClaim(context, "自动领取停止：等待玩家/窗口超时");
        return;
    }
    if (context.auto_claim_phase != 0 &&
        context.auto_claim_polls >= context.auto_claim_deadline_polls) {
        StopAutoClaim(context, context.auto_claim_phase == 3 ?
            "自动领取停止：退出后未确认离开副本" : "自动领取停止：未观察到预期的领奖/结算窗口");
        return;
    }
    if (!GetPlayerState(context)) { context.combat_status = "自动领取：等待玩家"; return; }
    if (now < context.auto_claim_poll) return;
    context.auto_claim_poll = now + std::chrono::seconds(1);
    ++context.auto_claim_polls;
    if (context.auto_claim_phase == 3) {
        std::uint64_t clone_id{};
        const bool left_clone = context.auto_claim_clone_id != 0 &&
            TryGetCurrentCloneId(context, clone_id) && clone_id == 0;
        if (context.cached_world != context.auto_claim_world || left_clone) {
            context.auto_claim_active = false;
            context.auto_claim_succeeded = true;
            context.combat_status = "领奖并退出完成";
        }
        return;
    }
    const auto windows = FindRewardWindows(context);
    if (!windows.complete) {
        context.combat_status = "自动领取：等待窗口数据";
        return;
    }
    if (windows.ambiguous) {
        StopAutoClaim(context, "自动领取停止：存在多个活动领奖/结算窗口");
        return;
    }
    switch (context.auto_claim_phase) {
    case 0: {
        context.auto_claim_succeeded = false;
        context.auto_claim_world = context.cached_world;
        context.auto_claim_clone_id = GetCurrentCloneId(context);
        if (windows.award != 0 || windows.settlement != 0) {
            context.auto_claim_phase = windows.award != 0 ? 1 : 2;
            context.auto_claim_deadline_polls = context.auto_claim_polls + kAutoClaimWaitPolls;
            return;
        }
        if (context.auto_claim_polls < context.auto_claim_deadline_polls) return;
        const std::uintptr_t chest = FindChestActor(context);
        if (chest == 0) {
            ++context.auto_claim_retries;
            if (context.auto_claim_retries < 20) {
                context.auto_claim_deadline_polls = context.auto_claim_polls;
                context.combat_status = "自动领取：等待宝箱加载";
            } else {
                StopAutoClaim(context, "自动领取：找不到宝箱");
                context.auto_claim_retries = 0;
            }
            return;
        }
        context.auto_claim_retries = 0;
        {
            double chest_pos[3]{};
            if (FindChestPos(context, chest_pos)) {
                double player_pos[3]{};
                if (SnapshotPlayerPosition(context, player_pos)) {
                    const double dx = chest_pos[0] - player_pos[0];
                    const double dy = chest_pos[1] - player_pos[1];
                    const double dz = chest_pos[2] - player_pos[2];
                    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    // 开发者模式：交互前一律先传送到宝箱，不按距离判断。
                    // 实测"只是靠近"不够 —— 玩家距宝箱约 1 米时交互调用照样返回成功
                    // 但游戏不开窗；同一批 10 次成功全部是"刚传送过去再交互"。
                    // 插件算出的距离用的是包围盒中心，并不等于到可交互点的真实距离，
                    // 所以不能用它决定要不要传送。
                    if (context.developer_mode.load(std::memory_order_acquire) &&
                        context.auto_claim_approach_polls < 5 &&
                        TeleportToPosition(context, chest_pos)) {
                        ++context.auto_claim_approach_polls;
                        context.combat_status = "自动领取：传送到宝箱";
                        // 不 return：本轮紧接着交互，保持实测有效的先后顺序。
                    } else if (dist > kAutoClaimApproachRangeCm &&
                               context.auto_claim_approach_polls < 5) {
                        // 非开发者模式（或传送不可用）：按距离先靠近再交互。
                        const bool nav_timed_out =
                            context.auto_claim_nav_deadline !=
                                std::chrono::steady_clock::time_point{} &&
                            now >= context.auto_claim_nav_deadline;
                        if (context.navigation != nullptr &&
                            context.navigation->move_to_location != nullptr &&
                            !nav_timed_out) {
                            ++context.auto_claim_approach_polls;
                            if (now >= context.auto_claim_nav_issue) {
                                static_cast<void>(context.navigation->move_to_location(
                                    context.navigation->user, chest_pos));
                                context.auto_claim_nav_issue = now + std::chrono::seconds(4);
                                if (context.auto_claim_nav_deadline ==
                                    std::chrono::steady_clock::time_point{}) {
                                    context.auto_claim_nav_deadline =
                                        now + std::chrono::seconds(10);
                                }
                            }
                            context.combat_status = "自动领取：移动向宝箱";
                            context.auto_claim_deadline_polls = context.auto_claim_polls;
                            return;
                        }
                    }
                }
            }
        }
        {
            std::uintptr_t chest_cls{};
            Read(reinterpret_cast<const void*>(chest + kObjectClassOffset), chest_cls);
            const std::string chest_cls_name =
                chest_cls != 0 ? ObjectName(context.names, chest_cls) : std::string();
            context.auto_claim_is_weekly =
                chest_cls_name.find("Weekly") != std::string::npos;
            // 临时诊断：交互时玩家离宝箱多远，是区分"距离不够导致游戏忽略交互"与
            // "宝箱本身不可交互"的关键数据。
            double log_chest[3]{}, log_player[3]{};
            const bool have_chest = FindChestPos(context, log_chest);
            const bool have_player = SnapshotPlayerPosition(context, log_player);
            const double dist_cm = (have_chest && have_player)
                ? std::sqrt(CombatDistanceSquared(log_player, log_chest))
                : -1.0;
            LogRewardDiagnostic(context, "autoclaim chest: class=" + chest_cls_name +
                " handle=0x" + [&chest] {
                    char text[24]{};
                    std::snprintf(text, sizeof(text), "%llX",
                        static_cast<unsigned long long>(chest));
                    return std::string(text);
                }() + " dist_cm=" + std::to_string(static_cast<long long>(dist_cm)) +
                " chest_pos_ok=" + std::to_string(have_chest ? 1 : 0) +
                " player_ok=" + std::to_string(have_player ? 1 : 0));
        }
        context.auto_claim_chest = chest;
        const char* chest_reason = "not attempted";
        if (!TriggerRewardChest(context, chest, &chest_reason)) {
            StopAutoClaim(context,
                "自动领取停止：宝箱交互调用失败（" + std::string(chest_reason) + "）");
            return;
        }
        context.auto_claim_phase = 1;
        context.auto_claim_poll = now + std::chrono::milliseconds(2500);
        context.auto_claim_deadline_polls = context.auto_claim_polls + kAutoClaimWaitPolls;
        context.combat_status = "自动领取：打开窗口中";
        return;
    }
    case 1: {
        if (windows.settlement != 0 && windows.award == 0) {
            context.auto_claim_phase = 2;
            context.auto_claim_deadline_polls = context.auto_claim_polls + kAutoClaimWaitPolls;
            return;
        }
        if (windows.award != 0 && windows.settlement == 0) {
            const auto button = SelectRewardButton(context, windows, true);
            if (button == nullptr) return;
            const auto click = ClickRewardButton(context, windows.award, button);
            if (click == RewardClickResult::Waiting) return;
            if (click != RewardClickResult::Invoked) {
                StopAutoClaim(context, "自动领取停止：" + context.combat_status);
                return;
            }
            context.auto_claim_phase = 2;
            context.auto_claim_deadline_polls = context.auto_claim_polls + kAutoClaimWaitPolls;
            context.combat_status = std::string_view(button) == windows.award_double_button
                ? "已调用双倍领取，等待奖励列表" : "已调用普通领取，等待奖励列表";
            return;
        }
        // 两个窗口都不在：交互调用虽然被接受，游戏却可能没真的开窗（UI 未生效时会吞掉这次交互，
        // 上手实测过整整 15 秒 award=0 settlement=0 然后被判超时）。被动等只会误判失败，
        // 这里按节拍重发交互，让它在 UI 可用后生效；重发次数受剩余预算约束。
        const std::uint32_t left_polls = context.auto_claim_deadline_polls >
                context.auto_claim_polls
            ? context.auto_claim_deadline_polls - context.auto_claim_polls
            : 0;
        if (context.auto_claim_chest != 0 && left_polls > 0 &&
            context.auto_claim_polls % 3 == 0) {
            // 窗口没出现时不要只重发交互：先把玩家重新贴回宝箱再交互。
            // 游戏很可能按"玩家是否在交互范围内"决定是否真的响应。
            double again_chest[3]{}, again_player[3]{};
            const bool again_have_chest = FindChestPos(context, again_chest);
            const bool again_have_player = SnapshotPlayerPosition(context, again_player);
            const double again_dist = (again_have_chest && again_have_player)
                ? std::sqrt(CombatDistanceSquared(again_player, again_chest))
                : -1.0;
            bool teleported = false;
            if (again_have_chest &&
                context.developer_mode.load(std::memory_order_acquire)) {
                // 与主路径一致：先传送再交互，不按距离判断（实测这是唯一稳定有效的顺序）。
                teleported = TeleportToPosition(context, again_chest);
            }
            const char* again_reason = "not attempted";
            const bool accepted =
                TriggerRewardChest(context, context.auto_claim_chest, &again_reason);
            LogRewardDiagnostic(context, "autoclaim retrigger: polls=" +
                std::to_string(context.auto_claim_polls) + " left=" +
                std::to_string(left_polls) + " dist_cm=" +
                std::to_string(static_cast<long long>(again_dist)) + " teleported=" +
                std::to_string(teleported ? 1 : 0) + " accepted=" +
                std::to_string(accepted ? 1 : 0) + " reason=" + again_reason);
        }
        return;
    }
    case 2: {
        if (windows.settlement != 0 && windows.award == 0) {
            const auto click = ClickRewardButton(context, windows.settlement, "Button_Exit");
            if (click == RewardClickResult::Waiting) return;
            if (click != RewardClickResult::Invoked) {
                StopAutoClaim(context, "自动领取停止：" + context.combat_status);
                return;
            }
            context.auto_claim_phase = 3;
            context.auto_claim_deadline_polls = context.auto_claim_polls + kAutoClaimWaitPolls;
            context.combat_status = "已显示奖励并调用退出，等待离开副本";
        }
        return;
    }
    }
}


void OneKeyTick(Context& context) noexcept {
    if (!context.one_key_active) return;
    if (!GetPlayerState(context)) { context.combat_status = "一键副本：无玩家"; return; }
    const auto now = std::chrono::steady_clock::now();
    switch (context.one_key_phase) {
    case 0: {
        context.enter_pending.store(true, std::memory_order_release);
        context.one_key_phase = 1;
        context.one_key_deadline = now + std::chrono::seconds(90);
        context.combat_status = "一键副本：进副本中";
        return;
    }
    case 1: {
        bool entered = GetCurrentCloneId(context) != 0;
        if (!entered && context.one_key_direct_enter &&
            context.one_key_enter_pos_valid) {
            double pos[3]{};
            if (SnapshotPlayerPosition(context, pos)) {
                const double dx = pos[0] - context.one_key_enter_pos[0];
                const double dy = pos[1] - context.one_key_enter_pos[1];
                const double dz = pos[2] - context.one_key_enter_pos[2];
                if (std::sqrt(dx * dx + dy * dy + dz * dz) > 5000.0) {
                    entered = true;
                }
            }
        }
        if (entered) {
            context.one_key_home_pos[0] = context.landmark_dest[0];
            context.one_key_home_pos[1] = context.landmark_dest[1];
            context.one_key_home_pos[2] = context.landmark_dest[2];
            context.one_key_home_valid = true;
            ResetAutoCombatTarget(context);
            context.clone_check_done = false;
            context.auto_attack.store(true, std::memory_order_release);
            context.next_attack = now;
            context.one_key_phase = 2;
            context.one_key_no_monster_seconds = 0;
            context.one_key_met_monster = false;
            context.one_key_deadline = now +
                std::chrono::seconds(context.one_key_enter_wait);
            context.combat_status = "一键副本：等待开场动画";
            return;
        }
        if (now >= context.one_key_deadline) {
            context.combat_status = "一键副本失败：进副本超时";
            context.one_key_active = false;
        }
        return;
    }
    case 2: {
        if (now < context.one_key_deadline) return;
        context.one_key_deadline = now + std::chrono::seconds(1);
        if (!context.combat_scan_valid) {
            context.one_key_no_monster_seconds = 0;
            context.combat_status = "一键副本：等待目标数据";
            return;
        }
        if (context.target_valid) {
            context.one_key_met_monster = true;
            context.one_key_no_monster_seconds = 0;
        } else if (context.one_key_met_monster) {
            ++context.one_key_no_monster_seconds;
            context.combat_status = "一键副本：无怪 " +
                std::to_string(context.one_key_no_monster_seconds) + "s";
            if (context.one_key_no_monster_seconds >= 5) {
                context.auto_attack.store(false, std::memory_order_release);
                ResetAutoCombatTarget(context);
                context.one_key_phase = 3;
                context.one_key_deadline = now + std::chrono::seconds(90);
                context.combat_status = "一键副本：领取中";
                StartAutoClaim(context);
                std::FILE* fp = std::fopen("D:\\rpc-diag.txt", "a");
                if (fp != nullptr) {
                    std::fprintf(fp, "[onekey] trigger autoclaim\n");
                    std::fclose(fp);
                }
            }
        }
        return;
    }
    case 3: {
        if (!context.auto_claim_active) {
            if (!context.auto_claim_succeeded) {
                context.one_key_active = false;
                return;
            }
            ++context.one_key_loop_done;
            if (context.one_key_loop_done < context.one_key_loop_count) {
                context.one_key_phase = 4;
                context.one_key_deadline = now +
                    std::chrono::seconds(context.one_key_exit_wait);
                context.one_key_met_monster = false;
                context.one_key_no_monster_seconds = 0;
                context.combat_status = "一键副本：第 " +
                    std::to_string(context.one_key_loop_done + 1) + "/" +
                    std::to_string(context.one_key_loop_count) + " 轮，等待退出";
            } else {
                context.combat_status = "一键副本完成（" +
                    std::to_string(context.one_key_loop_done) + "/" +
                    std::to_string(context.one_key_loop_count) + " 轮）";
                context.one_key_active = false;
                context.one_key_loop_done = 0;
            }
            return;
        }
        if (now >= context.one_key_deadline) {
            StopAutoClaim(context, "一键副本失败：领取超时");
            context.one_key_active = false;
        }
        return;
    }
    case 4: {
        double pos[3]{};
        if (SnapshotPlayerPosition(context, pos) && context.one_key_home_valid) {
            const double dx = pos[0] - context.one_key_home_pos[0];
            const double dy = pos[1] - context.one_key_home_pos[1];
            const double dz = pos[2] - context.one_key_home_pos[2];
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist >= 5000.0) {
                context.one_key_deadline = now +
                    std::chrono::seconds(context.one_key_exit_wait);
                return;
            }
            if (now >= context.one_key_deadline) {
                context.one_key_phase = 0;
                context.combat_status = "一键副本：回到主世界，开始下一轮";
            }
            return;
        }
        if (now >= context.one_key_deadline) {
            context.one_key_phase = 0;
        }
        return;
    }
    }
}


void TestAttackTick(Context& context) noexcept {
    if (!context.test_attack_waiting) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < context.test_attack_deadline) return;
    if (!InvokeNormalAttack(context)) {
        context.test_attack_waiting = false;
        LogRewardDiagnostic(context, "normal-attack failed: " + context.combat_status);
        return;
    }
    ++context.test_attack_phase;
    context.test_attack_waiting = context.test_attack_phase < 5;
    context.test_attack_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(333);
    context.combat_status = "普通攻击输入 " + std::to_string(context.test_attack_phase) + "/5";
    LogRewardDiagnostic(context, "normal-attack ability-input id=" + std::to_string(context.normal_attack.input_id) +
        " param=" + std::to_string(context.normal_attack.input_param) + " tap=" + std::to_string(context.test_attack_phase));
}


}  // namespace

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!host || !plugin_context || host->api_major != ANOMALY_PLUGIN_API_V1_MAJOR) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    auto* context = new (std::nothrow) Context{};
    if (!context) return {ANOMALY_STATUS_V1_FAILED, 0, {nullptr, 0}};
    context->host = host;
    {
        std::FILE* fp = std::fopen("D:\\rpc-diag.txt", "a");
        if (fp != nullptr) {
            std::fprintf(fp, "[config] configdir=%s\n", GetConfigDir().c_str());
            std::fclose(fp);
        }
    }
    LoadClickConfig(*context);
    LoadExitConfig(*context);
    LoadWeeklyConfig(*context);
    const auto view = anomaly::sdk::Host(host);
    context->ui = view.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();
    context->signature = view.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
    context->names = view.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    context->objects = view.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    context->map_landmarks = view.Query<AnomalyNteMapLandmarksServiceV1>(
        ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID,
        ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION).get();
    context->player = view.Query<AnomalyNtePlayerServiceV1>(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    context->actors = view.Query<AnomalyNteActorsServiceV1>(
        ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).get();
    context->entities = view.Query<AnomalyNteEntitiesServiceV1>(
        ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION).get();
    context->session = view.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    // Skill services are queried at invocation time because their publication is dynamic.
    context->navigation = view.Query<AnomalyNteNavigationServiceV1>(
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID,
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION).get();
    context->pickup = view.Query<AnomalyNtePickupServiceV1>(
        ANOMALY_NTE_PICKUP_SERVICE_V1_ID, ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION).get();
    // 开发者模式下用于代替寻路到宝箱；未发布时为 null，自动回退到寻路。
    context->teleport = view.Query<AnomalyNtePlayerTeleportServiceV1>(
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION).get();
    context->core = view.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
    context->input = view.Query<AnomalyInputServiceV1>(
        ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION).get();
    std::string legacy_cache_path;
    if (context->core != nullptr && context->core->plugin_directory != nullptr) {
        std::size_t sz = 0;
        if (context->core->plugin_directory(context->core->user, nullptr, &sz).code ==
                ANOMALY_STATUS_V1_OK && sz > 0) {
            std::string dir(sz, char(0));
            if (context->core->plugin_directory(context->core->user, dir.data(), &sz).code ==
                    ANOMALY_STATUS_V1_OK) {
                dir.resize(sz - 1);
                legacy_cache_path = dir + "\\clone-enter-cache.bin";
            }
        }
    }
    if (legacy_cache_path.empty()) legacy_cache_path = "D:\\clone-enter-cache.bin";
    context->cache_path = GetConfigDir() + "\\clone-enter-cache.bin";
    // 缓存从插件目录迁移到 state 目录：插件目录内的文件变化会触发框架热重载，
    // 重新缓存后不应把插件自己刷掉。旧的缓存文件存在且新位置还没有时复制过去一次。
    {
        std::FILE* probe = std::fopen(context->cache_path.c_str(), "rb");
        if (probe == nullptr) {
            std::FILE* src = std::fopen(legacy_cache_path.c_str(), "rb");
            if (src != nullptr) {
                std::FILE* dst = std::fopen(context->cache_path.c_str(), "wb");
                if (dst != nullptr) {
                    char buffer[4096];
                    std::size_t count = 0;
                    while ((count = std::fread(buffer, 1, sizeof(buffer), src)) > 0) {
                        std::fwrite(buffer, 1, count, dst);
                    }
                    std::fclose(dst);
                }
                std::fclose(src);
            }
        } else {
            std::fclose(probe);
        }
    }
    if (!SignatureReady(context->signature) || !NamesReady(context->names)) {
        delete context;
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {nullptr, 0}};
    }
    static_cast<void>(LoadCache(*context));
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (!plugin_context) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    auto& context = *static_cast<Context*>(plugin_context);
    const auto key = context.exit_hotkey_key.load(std::memory_order_acquire);
    const auto result = RegisterExitHotkey(context, key, context.exit_hotkey);
    if (result == ExitHotkeyResult::Conflict) {
        context.exit_hotkey_key.store(0, std::memory_order_release);
    }
    // 热键注册失败不阻止插件加载（快捷键是可选功能）
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (!plugin_context) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    auto& context = *static_cast<Context*>(plugin_context);
    context.capturing_exit_hotkey.store(false, std::memory_order_release);
    ReleaseExitHotkey(context);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

void ANOMALY_CALL Update(void* plugin_context, const double /*delta_seconds*/) {
    if (!plugin_context) return;
    auto& context = *static_cast<Context*>(plugin_context);
    // 开发者模式在 on_draw/on_update 期间有效；但 on_draw 只在插件窗口可见时被框架
    // 调用，所以必须在这里（每帧都执行的 on_update）刷新，否则窗口关着时检测不到。
    if (context.ui != nullptr) {
        context.developer_mode.store(
            DeveloperModeEnabled(context.ui), std::memory_order_release);
    }
    if (context.open_reward_pending.exchange(false, std::memory_order_acq_rel)) {
        OpenRewardWindow(context);
        return;
    }



    if (context.capturing_exit_hotkey.load(std::memory_order_acquire) &&
        InputReady(context.input)) {
        CaptureExitHotkey(context);
    }

    if (context.load_pending.exchange(false, std::memory_order_acq_rel)) {
        static_cast<void>(LoadEntries(context));
        DumpStructure(context);
    }

    if (context.enter_pending.exchange(false, std::memory_order_acq_rel)) {
        if (!context.entries_loaded) {
            static_cast<void>(LoadEntries(context));
        } else if (!context.cache_validated) {
            context.cache_validated = true;
            if (!context.entries.empty()) {
                const std::string name =
                    RenderFName(context, context.entries[0].contain_fname);
                if (name != context.entries[0].contain_id) {
                    std::FILE* dfp = std::fopen("D:\\cache-diag.txt", "a");
                    if (dfp != nullptr) {
                        std::fprintf(dfp,
                                     "lazy cache mismatch: resolved='%s' stored='%s' cmp=%u number=%u\n",
                                     name.c_str(),
                                     context.entries[0].contain_id.c_str(),
                                     context.entries[0].contain_fname.cmp,
                                     context.entries[0].contain_fname.number);
                        std::fclose(dfp);
                    }
                    context.entries.clear();
                    context.entries_loaded = false;
                    static_cast<void>(LoadEntries(context));
                }
            }
        }
        std::string target;
        std::size_t ei = context.display_index < kDisplayCount
            ? kDisplays[context.display_index].entry_index : 1;
        if (ei < 1) ei = 1;
        if (ei > context.entries.size()) ei = context.entries.size();
        if (!context.entries.empty()) {
            target = context.entries[ei - 1].contain_id;
        }
        // Abyss_Clone 的 landmark 是 Abyss
        if (target == "Abyss_Clone") target = "Abyss";
        // 周本（5/6/13）和抢银行（9）不经过传送点，直接进本
        if (target == "RobBank" || ei == 5 || ei == 6 || ei == 13) {
            context.enter_waiting_landmark = false;
            context.one_key_direct_enter = true;
            double pos[3]{};
            if (SnapshotPlayerPosition(context, pos)) {
                context.one_key_enter_pos[0] = pos[0];
                context.one_key_enter_pos[1] = pos[1];
                context.one_key_enter_pos[2] = pos[2];
                context.one_key_enter_pos_valid = true;
            }
            double dest[3]{};
            if (FindLandmarkDest(context, target, dest)) {
                context.landmark_dest[0] = dest[0];
                context.landmark_dest[1] = dest[1];
                context.landmark_dest[2] = dest[2];
            }
            static_cast<void>(DoEnterClone(context));
        } else {
            context.one_key_direct_enter = false;
            const bool teleported = TeleportToLandmark(
                context, target, context.landmark_dest);
            if (teleported) {
                context.enter_waiting_landmark = true;
                context.enter_transfer_timeout = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                context.enter_arrived = false;
            } else {
                context.enter_waiting_landmark = false;
                static_cast<void>(DoEnterClone(context));
            }
        }
    }

    if (context.enter_waiting_landmark && !context.enter_arrived) {
        double pos[3]{};
        if (SnapshotPlayerPosition(context, pos)) {
            const double dx = pos[0] - context.landmark_dest[0];
            const double dy = pos[1] - context.landmark_dest[1];
            const double dz = pos[2] - context.landmark_dest[2];
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < 2000.0) {
                context.enter_arrived = true;
                std::uint32_t wait = context.wait_seconds;
                if (wait < 1) wait = 1;
                if (wait > 60) wait = 60;
                context.enter_deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(wait);
            } else if (std::chrono::steady_clock::now() >= context.enter_transfer_timeout) {
                context.enter_waiting_landmark = false;
                context.enter_arrived = false;
                static_cast<void>(DoEnterClone(context));
            }
        }
    }

    if (context.enter_waiting_landmark && context.enter_arrived &&
        std::chrono::steady_clock::now() >= context.enter_deadline) {
        context.enter_waiting_landmark = false;
        context.enter_arrived = false;
        static_cast<void>(DoEnterClone(context));
    }

    if (context.scan_pending.exchange(false, std::memory_order_acq_rel)) {
        ScanMonsters(context);
    }
    if (context.dump_funcs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpClassFuncs(context, "mon_029_BP_Clone_C");
    }
    if (context.kill_pending.exchange(false, std::memory_order_acq_rel)) {
        KillMonsters(context);
    }
    if (context.params_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpMonsterParams(context);
    }
    if (context.dmg_params_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpControllerDamageParams(context);
    }
    if (context.ge_scan_pending.exchange(false, std::memory_order_acq_rel)) {
        ScanGE(context);
    }
    if (context.player_funcs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpPlayerFuncs(context);
    }
    if (context.attrset_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpPlayerAttributeSet(context);
    }
    if (context.buff_pending.exchange(false, std::memory_order_acq_rel)) {
        ApplyPlayerDamageBuff(context);
    }
    if (context.modify_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpModifyDataStruct(context);
    }
    if (context.ge_struct_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpGEStruct(context);
    }
    if (context.modifier_info_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpModifierInfo(context);
    }
    if (context.modifiers_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpModifiers(context);
    }
    if (context.attr_values_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpAttributeValues(context);
    }
    if (context.ht_attr_class_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpHTAttrClass(context);
    }
    if (context.damage_attrs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpDamageAttrs(context);
    }
    if (context.modifier_attr_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpModifierAttr(context);
    }
    if (context.boost_damage_pending.exchange(false, std::memory_order_acq_rel)) {
        BoostPlayerDamage(context);
    }
    if (context.nettarget_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpNetTargetStruct(context);
    }
    if (context.charfornet_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpCharForNet(context);
    }
    if (context.player_damage_func_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpPlayerDamageFunc(context);
    }
    if (context.any_damage_pending.exchange(false, std::memory_order_acq_rel)) {
        KillMonstersViaAnyDamage(context);
    }
    if (context.kill_self_pending.exchange(false, std::memory_order_acq_rel)) {
        KillMonstersViaKillSelf(context);
    }
    if (context.death_event_pending.exchange(false, std::memory_order_acq_rel)) {
        KillMonstersViaDeathEvent(context);
    }
    if (context.nettarget_damage_pending.exchange(false, std::memory_order_acq_rel)) {
        ApplyDamageViaNetTarget(context);
    }
    if (context.set_hp_one_pending.exchange(false, std::memory_order_acq_rel)) {
        SetMonstersHPToOne(context);
    }
    if (context.hp_attrs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpHPAttrs(context);
    }
    if (context.monster_attrset_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpMonsterAttrSet(context);
    }
    if (context.scan_hp_pending.exchange(false, std::memory_order_acq_rel)) {
        ScanMonsterHP(context);
    }
    if (context.monster_hp_values_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpMonsterHPValues(context);
    }
    if (context.player_state_funcs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpPlayerStateFuncs(context);
    }
    if (context.clone_rpc_params_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpCloneRPCParams(context);
    }
    if (context.clone_enums_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpCloneEnums(context);
    }
    if (context.enum_values_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpEnumValues(context);
    }
    if (context.passpermit_award_pending.exchange(false, std::memory_order_acq_rel)) {
        TriggerPassPermitAward(context);
    }
    if (context.scan_clone_classes_pending.exchange(false, std::memory_order_acq_rel)) {
        ScanCloneClasses(context);
    }
    if (context.clone_manager_funcs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpCloneManagerFuncs(context);
    }
    if (context.activate_skill_pending.exchange(false, std::memory_order_acq_rel)) {
        ActivateSkill(context);
    }
    if (context.test_skill_pending.exchange(false, std::memory_order_acq_rel)) {
        const bool ok = ActivateSkillByInputId(
            context, static_cast<std::int32_t>(context.test_input_id));
        context.combat_status = ok ? "技能已激活" : "技能未找到";
    }
    if (context.claim_reward_pending.exchange(false, std::memory_order_acq_rel)) {
        ClaimReward(context, 1);
    }
    if (context.claim_double_pending.exchange(false, std::memory_order_acq_rel)) {
        ClaimReward(context, 2);
    }
    if (context.auto_claim_pending.exchange(false, std::memory_order_acq_rel)) {
        StartAutoClaim(context);
    }
    if (context.settlement_exit_pending.exchange(false, std::memory_order_acq_rel)) {
        ExitSettlement(context);
    }
    if (context.settlement_ui_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpSettlementUI(context);
    }
    if (context.one_key_pending.exchange(false, std::memory_order_acq_rel)) {
        context.auto_attack.store(false, std::memory_order_release);
        ResetAutoCombatTarget(context);
        context.one_key_active = true;
        context.one_key_phase = 0;
        context.one_key_no_monster_seconds = 0;
        context.one_key_met_monster = false;
        context.one_key_loop_done = 0;
        context.one_key_direct_enter = false;
        context.one_key_enter_pos_valid = false;
        if (context.one_key_loop_count < 1) context.one_key_loop_count = 1;
        context.combat_status = "一键副本：开始";
    }
    if (context.test_attack_pending.exchange(false, std::memory_order_acq_rel)) {
        context.test_attack_waiting = true;
        context.test_attack_phase = 0;
        context.test_attack_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        context.combat_status = "3秒后开始普攻连击测试";
    }
    if (context.chest_choices_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpChestChoices(context);
    }
    if (context.chest_funcs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpChestFuncs(context);
    }
    if (context.reward_params_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpRewardParams(context);
    }
    if (context.award_funcs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpAwardFuncs(context);
    }
    if (context.award_ui_funcs_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpAwardUIFuncs(context);
    }
    if (context.award_ui_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpAwardUI(context);
    }
    if (context.award_widgets_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpAwardWidgets(context);
    }
    if (context.btn_params_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpBtnParams(context);
    }
    if (context.controller_click_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpControllerClickFuncs(context);
    }
    if (context.btn_geometry_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpBtnGeometry(context);
    }
    if (context.click_params_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpClickParams(context);
    }
    if (context.record_mouse_pending.exchange(false, std::memory_order_acq_rel)) {
        context.record_waiting = true;
        context.record_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        context.combat_status = "3秒后记录副本领取按钮位置：请把鼠标移到按钮上";
    }
    if (context.record_exit_pending.exchange(false, std::memory_order_acq_rel)) {
        context.record_waiting = true;
        context.record_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        context.combat_status = "3秒后记录退出位置";
        context.record_exit_mode = true;
    }
    if (context.record_weekly_pending.exchange(false, std::memory_order_acq_rel)) {
        context.record_waiting = true;
        context.record_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        context.combat_status = "3秒后记录周本领取位置";
        context.record_weekly_mode = true;
    }
    if (context.simulate_click_pending.exchange(false, std::memory_order_acq_rel)) {
        context.click_waiting = true;
        context.click_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        context.combat_status = "3秒后模拟点击";
    }
    if (context.record_waiting &&
        std::chrono::steady_clock::now() >= context.record_deadline) {
        context.record_waiting = false;
        if (context.record_weekly_mode) {
            context.record_weekly_mode = false;
            RecordWeeklyPos(context);
        } else if (context.record_exit_mode) {
            context.record_exit_mode = false;
            RecordExitPos(context);
        } else {
            RecordMousePos(context);
        }
    }
    if (context.click_waiting &&
        std::chrono::steady_clock::now() >= context.click_deadline) {
        context.click_waiting = false;
        SimulateClick(context);
    }
    if (context.click_phase != 0 &&
        std::chrono::steady_clock::now() >= context.click_deadline) {
        SimulateClick(context);
    }
    if (context.combat_target_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpCombatTarget(context);
    }
    if (context.monster_classes_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpMonsterClasses(context);
    }
    if (context.clone_monster_info_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpCloneMonsterInfo(context);
    }
    if (context.monster_assets_pending.exchange(false, std::memory_order_acq_rel)) {
        ScanMonsterAssets(context);
    }
    if (context.entity_classes_pending.exchange(false, std::memory_order_acq_rel)) {
        DumpEntityClasses(context);
    }
    if (context.exit_pending.exchange(false, std::memory_order_acq_rel)) {
        context.one_key_active = false;
        context.one_key_phase = 0;
        context.one_key_loop_done = 0;
        context.auto_claim_active = false;
        context.auto_attack.store(false, std::memory_order_release);
        ExitClone(context);
        context.combat_status = "已退出副本";
    }
    if (context.auto_attack.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= context.next_attack) {
            AutoCombatTick(context);
            context.next_attack = now + std::chrono::milliseconds(
                context.melee_mode ? 333 : 500);
        }
    } else if (context.target_valid || context.combat_scan_valid || context.auto_combat_moving) {
        ResetAutoCombatTarget(context);
    }
    AutoClaimTick(context);
    OneKeyTick(context);
    TestAttackTick(context);
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    if (!plugin_context) return;
    auto& context = *static_cast<Context*>(plugin_context);
    const AnomalyUiServiceV1* ui = supplied_ui;
    if (ui == nullptr) {
        ui = anomaly::sdk::Host(context.host).Query<AnomalyUiServiceV1>(
                 ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION)
                 .get();
    }
    if (ui == nullptr || ui->text == nullptr || ui->button == nullptr) return;
    // 开发者模式仅在 on_draw/on_update 期间可查询，这里缓存供 Update 的自动领取使用。
    context.developer_mode.store(DeveloperModeEnabled(ui), std::memory_order_release);
    int open = 1;
    anomaly::sdk::UiWindow window(ui, "自动副本", &open);
    if (!window) return;

    std::size_t cur_di = context.display_index;
    if (cur_di >= kDisplayCount) cur_di = 0;
    if (ui->begin_menu != nullptr && ui->end_menu != nullptr) {
        const std::string cur_name = kDisplays[cur_di].name;
        if (ui->begin_menu(ui->user, anomaly::sdk::StringView(cur_name), 1)) {
            for (std::size_t i = 0; i < kDisplayCount; ++i) {
                if (ui->button(ui->user, anomaly::sdk::StringView(kDisplays[i].name), 0.0F, 0.0F) != 0) {
                    context.display_index = static_cast<std::uint32_t>(i);
                    context.sub_choice_index = 0;
                    context.level_index = 1;
                }
            }
            ui->end_menu(ui->user);
        }
        const auto& disp = kDisplays[cur_di];
        if (disp.sub_count > 1) {
            std::size_t sci = context.sub_choice_index;
            if (sci >= disp.sub_count) sci = 0;
            const std::string sub_name = disp.subs[sci].name;
            if (ui->begin_menu(ui->user, anomaly::sdk::StringView(sub_name), 1)) {
                for (std::uint32_t i = 0; i < disp.sub_count; ++i) {
                    if (ui->button(ui->user, anomaly::sdk::StringView(disp.subs[i].name), 0.0F, 0.0F) != 0) {
                        context.sub_choice_index = i;
                        context.level_index = 1;
                    }
                }
                ui->end_menu(ui->user);
            }
        }
    }
    if (ui->input_uint32 != nullptr) {
        ui->input_uint32(ui->user, anomaly::sdk::StringView("难度"),
                         &context.level_index, 1, 1);
        std::uint32_t min_lv = kDisplays[cur_di].min_level;
        std::uint32_t max_lv = kDisplays[cur_di].max_level;
        if (context.level_index < min_lv) context.level_index = min_lv;
        if (context.level_index > max_lv) context.level_index = max_lv;
        ui->input_uint32(ui->user, anomaly::sdk::StringView("传送后等待(秒)"),
                         &context.wait_seconds, 1, 1);
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("进副本"), 0.0F, 0.0F) != 0) {
        context.enter_pending.store(true, std::memory_order_release);
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("退出副本"), 0.0F, 0.0F) != 0) {
        context.exit_pending.store(true, std::memory_order_release);
    }
    if (context.capturing_exit_hotkey.load(std::memory_order_acquire)) {
        if (ui->button(ui->user, anomaly::sdk::StringView("按下新的退出键..."), 0.0F, 0.0F) != 0) {
            context.capturing_exit_hotkey.store(false, std::memory_order_release);
        }
    } else {
        const std::string label = "退出快捷键：" +
            HotkeyName(context.exit_hotkey_key.load(std::memory_order_acquire));
        if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0) {
            context.capturing_exit_hotkey.store(true, std::memory_order_release);
        }
    }
    if (ui->input_uint32 != nullptr) {
        ui->input_uint32(ui->user, anomaly::sdk::StringView("进本等待(秒)"),
                         &context.one_key_enter_wait, 1, 1);
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("一键副本"), 0.0F, 0.0F) != 0) {
        context.one_key_pending.store(true, std::memory_order_release);
    }
    if (ui->input_uint32 != nullptr) {
        ui->input_uint32(ui->user, anomaly::sdk::StringView("循环次数"),
                         &context.one_key_loop_count, 1, 1);
        ui->input_uint32(ui->user, anomaly::sdk::StringView("退出后等待(秒)"),
                         &context.one_key_exit_wait, 1, 1);
    }
    if (ui->button(ui->user, anomaly::sdk::StringView(
                       context.auto_attack.load() ? "自动攻击:开" : "自动攻击:关"), 0.0F, 0.0F) != 0) {
        const bool next = !context.auto_attack.load();
        context.auto_attack.store(next);
        if (next) {
            context.clone_check_done = false;
        }
        context.next_attack = std::chrono::steady_clock::now();
    }
    if (ui->input_uint32 != nullptr) {
        auto radius = context.combat_search_radius_m.load(std::memory_order_acquire);
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView("搜索半径(米)"),
                &radius, 5, 25) != 0) {
            context.combat_search_radius_m.store(std::clamp(radius, 1U, 1000U), std::memory_order_release);
        }
        ui->input_uint32(ui->user, anomaly::sdk::StringView("自动攻击技能ID"),
                         &context.test_input_id, 1, 1);
    }
    if (ui->button(ui->user, anomaly::sdk::StringView(
                       context.melee_mode ? "平A模式:开" : "平A模式:关"), 0.0F, 0.0F) != 0) {
        context.melee_mode = !context.melee_mode;
    }
    if (ui->text != nullptr && !context.combat_status.empty()) {
        ui->text(ui->user, anomaly::sdk::StringView(context.combat_status));
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("打开领奖窗口"), 0.0F, 0.0F) != 0) {
        context.open_reward_pending.store(true, std::memory_order_release);
    }
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("local.clone-enter"),
        anomaly::sdk::StringView("Clone Enter"),
        anomaly::sdk::StringView("CCYellowStar"),
        anomaly::sdk::StringView("0.2.13"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
