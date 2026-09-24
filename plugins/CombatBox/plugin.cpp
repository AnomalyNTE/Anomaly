#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/localization.h"
#include "anomaly/sdk/services/plugin_state.h"
#include "plugins/common/localization.hpp"
#include "plugins/common/combat/auto_combat.hpp"

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
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

namespace combat = anomaly::plugins::combat;

constexpr std::string_view kTablePath =
    "/Game/DataTable/Treasurebox/DT_TreasureboxConfig.DT_TreasureboxConfig";

constexpr std::ptrdiff_t kDataTableRowMapOffset = 0x30;
constexpr std::uint32_t kDataTableRowStride = 24;
constexpr std::uint32_t kDataTableRowPointerOffset = 8;
constexpr std::ptrdiff_t kCoordOffset = 48;

constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
constexpr std::ptrdiff_t kGObjectsAddend = -16;
constexpr std::uint32_t kRipDisplacementOffset = 3;
constexpr std::uint32_t kRipInstructionSize = 7;
constexpr std::ptrdiff_t kObjectItemsOffset = 16;
constexpr std::ptrdiff_t kObjectCountOffset = 36;
constexpr std::ptrdiff_t kObjectNumChunksOffset = 44;
constexpr std::uint32_t kObjectChunkSize = 65536;
constexpr std::uint32_t kObjectItemStride = 24;
constexpr std::size_t kMaximumNameBytes = 1024;

constexpr double kTeleportZOffset = 60.0;
constexpr double kApproachRadiusCentimeters = 100.0;
constexpr std::string_view kLandmarkWorld = "XL_map_bigworld_test";
constexpr double kLandmarkArrivalRadiusCentimeters = 500.0;
constexpr double kLandmarkTransferTimeoutSeconds = 20.0;
constexpr double kLandmarkSettleSeconds = 2.0;
constexpr double kArrivalRadiusCentimeters = 600.0;
constexpr double kMovementTimeoutSeconds = 20.0;
// 传送是瞬时的，但位置快照要下一帧才更新；等到位再进战斗阶段，超时就直接开打
// （打不到怪由首次接敌超时收场，不再加一层失败状态）。
constexpr double kTeleportArrivalTimeoutSeconds = 5.0;
// 掉出世界的判据与 BoxAuto 保持一致（plugins/BoxAuto/plugin.cpp 的
// kFallOutThresholdCentimeters）：玩家 Z 比当前点低 10 米以上即视为掉出世界。
// 掉落判据用「下降速度」而不是「比基准低多少」：走路下坡每秒几米，自由落体每秒十几米。
// 基准式判据两边都错过 —— 用点位高度当基准会误报（点位在箱子/高台上），用「进战斗时自己的高度」
// 当基准则在「一开始就在下面」（传送落到地图下 / 地形没加载）时永远不触发，人卡在下面找不到怪。
constexpr double kFallOutRateCentimetersPerSecond = 1500.0;
// 比**当前点位**低这么多就算掉出世界（用户定的：点位坐标在地面上，1 米足够）。
constexpr double kFallOutBelowPointCentimeters = 100.0;
// 掉出世界后的重传次数：一次能修掉绝大多数「传送落进洞里/被挤出地图」，给到 3 次覆盖
// 地图流式加载抖动；再多只是拖时间——本点的收敛另有首次接敌超时与模块黑名单 TTL 兜底。
// 高楼/新区域的地图常常还没流式加载完，玩家会真的往下掉。**不能等**：等下去会摔死
// （用户实测），所以检测到就立刻重传，一直传到落地为止。次数只是防止真无解时死循环，
// 见过怪之后计数会清零（已经站稳开打，不该被之前的掉落惩罚）。
constexpr std::uint32_t kFallOutMaximumRetries = 10;
// 两次重传之间的最小间隔：只用来防止同一帧里反复下发传送。
constexpr auto kFallOutRetryDelay = std::chrono::milliseconds(1200);
constexpr double kProgressCheckIntervalSeconds = 3.0;
constexpr double kProgressThresholdCentimeters = 80.0;
constexpr double kReissueDelaySeconds = 4.0;
constexpr std::uint32_t kMaximumMapLandmarks = 4096;

struct RawName final {
    std::int32_t comparison_index{};
    std::uint32_t number{};
};

struct Point final {
    std::string row_name;
    double x{};
    double y{};
    double z{};
};

struct MapLandmark final {
    std::uint64_t sequence{};
    std::uint32_t index{};
    double destination[3]{};
    std::string teleport_id;
};

std::vector<Point> VisionPoints() {
    return {
        {"伤心英熊_mon_019_SadBear_BP_World_C_0", -28170, 84678, 6503},
        {"伤心英熊_mon_019_SadBear_BP_World_C_1", -283560, 348658, 2312},
        {"伤心英熊_mon_019_SadBear_BP_World_C_2", -34682, 131390, 2843},
        {"伤心英熊_mon_019_SadBear_BP_World_C_3", 27585, 115101, 7057},
        {"伤心英熊_mon_019_SadBear_BP_World_C_4", 35051, 73480, 6201},
        {"伤心英熊_mon_019_SadBear_BP_World_C_5", 36087, 138934, 3167},
        {"妖刀_mon_13_1_BP_World_C_6", -129110, 133352, 6713},
        {"妖刀_mon_13_1_BP_World_C_7", -172989, 122003, 7117},
        {"妖刀_mon_13_1_BP_World_Perform_C_8", -111304, 162488, 5980},
        {"妖刀_mon_13_1_BP_World_Perform_C_9", -145267, 79781, 7543},
        {"妖刀_mon_13_1_BP_World_Perform_C_10", -19120, 23926, 7711},
        {"妖刀_mon_13_1_BP_World_Perform_C_11", -277508, 346179, 3955},
        {"妖刀_mon_13_1_BP_World_Perform_C_12", -77961, 138950, 3892},
        {"妖刀_mon_13_1_BP_World_Perform_C_13", 29187, 100218, 7173},
        {"妖刀_mon_13_2_BP_World_C_14", -150925, 82837, 7561},
        {"妖刀_mon_13_2_BP_World_Interaction_C_15", -143290, 213280, 8766},
        {"妖刀_mon_13_BP_World_C_16", -201739, 106656, 7572},
        {"妖刀_mon_13_BP_World_C_17", -253256, 361426, 5616},
        {"妖刀_mon_13_BP_World_Interaction_C_18", -36600, 88375, 5222},
        {"妖刀_mon_13_horizontal_BP_World_C_19", -86569, 130546, 7882},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_20", -105359, 65605, 7131},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_21", -105589, 66670, 7136},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_22", -119567, 196220, 5758},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_23", -137731, 160872, 6835},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_24", -164979, 139373, 6745},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_25", -21772, 47950, 7710},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_26", -24617, 66364, 6913},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_27", -33772, 31497, 7744},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_28", -36058, 50158, 7690},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_29", -46555, 104935, 2886},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_30", -49823, 86262, 2900},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_31", -50231, 85743, 2900},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_32", -58223, 72915, 3638},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_33", 18630, 104502, 6555},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_34", 29359, 138598, 3166},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_35", 42088, 52008, 5114},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_36", 42097, 116235, 6999},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_37", 7970, 30375, 7696},
        {"贩售机附电灵_mon_14_BP_World_Interaction_C_38", 8775, 31078, 7684},
        {"洄天鱼幡_mon_025_BP_Blue_World_Passive_C_39", -133541, 153285, 13629},
        {"洄天鱼幡_mon_025_BP_Blue_World_Passive_C_40", -190922, 184853, 8251},
        {"洄天鱼幡_mon_025_BP_Blue_World_Passive_C_41", -21200, 68382, 28231},
        {"洄天鱼幡_mon_025_BP_Blue_World_Passive_C_42", -61212, -21036, 9408},
        {"洄天鱼幡_mon_025_BP_Green_World_Passive_C_43", -132500, 152691, 13629},
        {"洄天鱼幡_mon_025_BP_Green_World_Passive_C_44", -190846, 183799, 8245},
        {"洄天鱼幡_mon_025_BP_Green_World_Passive_C_45", -21939, 68863, 28294},
        {"洄天鱼幡_mon_025_BP_Green_World_Passive_C_46", -60069, -19342, 8985},
        {"洄天鱼幡_mon_025_BP_Red_World_Passive_C_47", -132508, 153895, 13629},
        {"洄天鱼幡_mon_025_BP_Red_World_Passive_C_48", -189837, 184982, 8293},
        {"洄天鱼幡_mon_025_BP_Red_World_Passive_C_49", -22069, 67703, 28445},
        {"洄天鱼幡_mon_025_BP_Red_World_Passive_C_50", -62230, -19428, 9003},
        {"洄天鱼幡_mon_025_BP_World_Vision_02_1_C_51", 5217, 109465, 28755},
        {"洄天鱼幡_mon_025_BP_World_Vision_02_2_C_52", 5217, 109465, 28755},
        {"洄天鱼幡_mon_025_BP_World_Vision_02_3_C_53", 5217, 109465, 28755},
        {"洄天鱼幡_mon_025_BP_World_W103308_01_C_54", -153361, 204915, 6668},
        {"洄天鱼幡_mon_025_BP_World_W103308_02_C_55", -153361, 204915, 6668},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_56", -110366, 79375, 15476},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_57", -123577, 190121, 8783},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_58", -124398, 96588, 12899},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_59", -139539, 156211, 11671},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_60", -141856, 127782, 12499},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_61", -179873, 146591, 10085},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_62", -181567, 239880, 2463},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_63", -184345, 98966, 12662},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_64", -207027, 193561, 7655},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_65", -35988, 119065, 13859},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_66", -45688, 88125, 9597},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_67", -76466, 69499, 10501},
        {"洄天鱼幡_mon_025_double_1_BP_World_C_68", 38406, 55294, 9518},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_69", -110366, 79375, 15476},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_70", -123577, 190121, 8783},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_71", -124398, 96588, 12899},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_72", -139539, 156211, 11671},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_73", -141856, 127782, 12499},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_74", -179873, 146591, 10085},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_75", -181567, 239880, 2463},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_76", -184345, 98966, 12662},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_77", -207027, 193561, 7655},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_78", -35988, 119065, 13859},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_79", -45688, 88125, 9597},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_80", -76466, 69499, 10501},
        {"洄天鱼幡_mon_025_double_2_BP_World_C_81", 38406, 55294, 9518},
        {"纸翼战队_mon_26_BP_World_C_82", -103972, 50917, 8488},
        {"纸翼战队_mon_26_BP_World_C_83", -124312, 59338, 8465},
        {"纸翼战队_mon_26_BP_World_C_84", -135156, 233737, 2510},
        {"纸翼战队_mon_26_BP_World_C_85", -16940, 95140, 6540},
        {"纸翼战队_mon_26_BP_World_C_86", -195094, 187235, 8278},
        {"纸翼战队_mon_26_BP_World_C_87", -254512, 348519, 2601},
        {"纸翼战队_mon_26_BP_World_C_88", -30925, 16442, 8710},
        {"纸翼战队_mon_26_BP_World_C_89", -43748, 6288, 7665},
        {"纸翼战队_mon_26_BP_World_C_90", -6871, 13074, 8910},
        {"纸翼战队_mon_26_BP_World_C_91", -80561, 288, 8355},
        {"纸翼战队_mon_26_BP_World_C_92", 23426, 77367, 6500},
        {"纸翼战队_mon_26_BP_World_C_93", 48854, 123607, 6485},
        {"纸翼战队_mon_26_BP_World_Perform_C_94", -119972, 147571, 8455},
        {"纸翼战队_mon_26_BP_World_Perform_C_95", -125859, 73580, 7985},
        {"纸翼战队_mon_26_BP_World_Perform_C_96", -201744, 213237, 18241},
        {"纸翼战队_mon_26_BP_World_Perform_C_97", -44462, 14189, 7512},
        {"纸翼战队_mon_26_BP_World_Perform_C_98", 24980, 44301, 52768},
        {"波普_mon_012_1_BP_World_Area01_002_C_99", -122624, 156946, 10139},
        {"波普_mon_012_1_BP_World_Area02_001_C_100", -182767, 124538, 7429},
        {"波普_mon_012_1_BP_World_Area02_002_C_101", -113712, 64380, 8749},
        {"波普_mon_012_1_BP_World_Area02_003_C_102", -92400, 97459, 6317},
        {"波普_mon_012_1_BP_World_AreaSpecial_01_C_103", -119497, 185081, 6131},
        {"波普_mon_012_1_BP_World_AreaSpecial_02_C_104", -128896, 126920, 6785},
        {"波普_mon_012_2_BP_World_Area01_002_C_105", -122630, 156952, 10146},
        {"波普_mon_012_2_BP_World_Area02_001_C_106", -182767, 124538, 7446},
        {"波普_mon_012_2_BP_World_Area02_002_C_107", -113712, 64380, 8779},
        {"波普_mon_012_2_BP_World_Area02_003_C_108", -92400, 97459, 6282},
        {"波普_mon_012_2_BP_World_AreaSpecial_01_C_109", -119491, 185088, 6170},
        {"波普_mon_012_2_BP_World_AreaSpecial_02_C_110", -128896, 126920, 6878},
    };
}

static const char* kVisionNames[] = {
    "伤心英熊", "妖刀", "贩售机附电灵", "洄天鱼幡", "纸翼战队", "波普"
};

struct Context final {
    const AnomalyHostApiV1* host{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNtePlayerTeleportServiceV1* teleport{};
    const AnomalyNteNavigationServiceV1* navigation{};
    const AnomalyNteMapLandmarksServiceV1* map_landmarks{};
    // 自动战斗模块需要的服务（模块自己不查服务，每次 tick 由这里重建 Host）。
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* skill_invocation{};
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyNteEntitiesServiceV1* entities{};
    const AnomalyNtePickupServiceV1* pickup{};
    const AnomalyPluginStateServiceV1* plugin_state{};
    anomaly::plugins::Localizer localizer;

    std::uintptr_t g_objects_address{};

    std::mutex mutex;
    std::vector<Point> points;
    std::unordered_set<std::string> done_set;
    std::string state_directory;
    std::string status;
    std::uint32_t type_choice{0};
    std::uint32_t sub_choice{0};
    std::vector<Point> filtered_points;
    std::atomic_bool read_pending{};
    std::atomic_bool developer_mode{};
    std::atomic_bool manual_teleport_pending{};
    std::atomic_bool save_pending{};
    double manual_teleport_x{}, manual_teleport_y{}, manual_teleport_z{};
    std::string manual_teleport_row;
    std::uint64_t landmarks_sequence{};
    std::vector<MapLandmark> landmarks;
    bool landmark_transfer_attempted{};
    bool landmark_transfer_wait{};
    double landmark_destination[3]{};
    double landmark_origin[2]{};
    double pending_target[3]{};
    std::chrono::steady_clock::time_point landmark_deadline{};
    std::chrono::steady_clock::time_point landmark_arrival_time{};
    bool navigating{};
    std::chrono::steady_clock::time_point navigation_last_progress_at{};
    std::chrono::steady_clock::time_point navigation_progress_check_at{};
    std::chrono::steady_clock::time_point navigation_retry_at{};
    double navigation_target[3]{};
    double navigation_last_position[3]{};
    bool navigation_has_last_position{};

    // —— 自动一轮：点 → 到达 → 自动战斗 → 下一个点 ——
    // 当前点的行名（只有 Update 线程写；Draw 读时走 mutex）。
    std::string current_row;
    // 一轮是否在进行中（决定本点结束后是否自动取下一个点）。
    bool auto_run{};
    // 传送后的落点确认。
    bool teleport_arrival_wait{};
    std::chrono::steady_clock::time_point teleport_arrival_deadline{};
    // 战斗阶段：共享模块的状态 + 「首次接敌超时」的起点。
    anomaly::plugins::combat::State combat_state;
    bool combat_active{};
    std::chrono::steady_clock::time_point combat_started_at{};
    // 进入战斗时玩家的高度：掉出世界用它作基准（点位高度常在箱子/高台上，不能当基准）。
    double combat_start_z{};
    // 掉落追踪：上次采样的高度与时刻（`fallout_last_at` 为空表示还没初始化）。
    double fallout_last_z{};
    std::chrono::steady_clock::time_point fallout_last_at{};
    // 下一次允许重传的时刻（掉出世界后的等待间隔）。
    std::chrono::steady_clock::time_point fallout_retry_at{};
    // 最近一次「换了目标」的时刻与句柄：无伤害兜底要从这里重新计时，
    // 否则刚出现怪就被兜底结束。
    std::chrono::steady_clock::time_point combat_target_at{};
    bool combat_had_target{};
    std::atomic<double> first_contact_timeout_seconds{15.0};
    // 无伤害兜底：伤害流不可用时模块内部的计时会被反复重置，靠不住；这里用调用方自己的时钟
    // ——「自上次伤害（从没有过就用进入战斗的时刻）起 N 秒没有伤害 ⇒ 本点打完」。
    std::atomic<double> no_damage_fallback_seconds{10.0};
    // 搜索半径（米）：怪可能在传送落点 50 米外，匹配不到就先把这个调大。
    std::atomic<double> search_radius_m{50.0};
    // 攻击策略：开 = 近战普攻为主 + 每 4 次放一次技能（模块据此才会把「打不动」置成 attack_failed，
    // 从而能立即跳过打不动的点）；关 = 只放技能，技能调用失败会被忽略。默认关，与一键副本一致。
    // 默认开：模块只在平A路径上报「打不动这个目标」，默认关会让那条判断形同虚设。
    std::atomic_bool melee_mode{true};
    std::atomic_bool auto_start_pending{};
    std::atomic_bool auto_stop_pending{};
    // 掉出世界：本点已重传几次 + 「这次重传是不是同一点的重试」。
    // 重试期间保留首次接敌超时的起点（不给重试续命），也保留「本点见过怪」这一事实。
    std::uint32_t fallout_retries{};
    bool combat_retry_pending{};
    bool combat_retry_engaged{};
};

void RebuildFilteredLocked(Context& context) {
    context.filtered_points.clear();
    if (context.type_choice == 0 || context.sub_choice == 0) {
        context.filtered_points = context.points;
    } else {
        const char* prefix = kVisionNames[context.sub_choice - 1];
        const std::size_t prefix_len = std::strlen(prefix);
        for (const Point& p : context.points) {
            if (p.row_name.compare(0, prefix_len, prefix) == 0) {
                context.filtered_points.push_back(p);
            }
        }
    }
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

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* s) noexcept {
    return HasField<AnomalyUe5ObjectsServiceV1,
               decltype(AnomalyUe5ObjectsServiceV1::find_exact)>(
               s, offsetof(AnomalyUe5ObjectsServiceV1, find_exact)) &&
        s->find_exact != nullptr;
}

bool NavigationReady(const AnomalyNteNavigationServiceV1* s) noexcept {
    return HasField<AnomalyNteNavigationServiceV1,
               decltype(AnomalyNteNavigationServiceV1::move_to_location)>(
               s, offsetof(AnomalyNteNavigationServiceV1, move_to_location)) &&
        s->move_to_location != nullptr && s->stop_movement != nullptr;
}

bool DeveloperModeEnabled(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1,
               decltype(AnomalyUiServiceV1::developer_mode_enabled)>(
               ui, offsetof(AnomalyUiServiceV1, developer_mode_enabled)) &&
        ui->developer_mode_enabled != nullptr &&
        ui->developer_mode_enabled(ui->user) != 0;
}

template <typename T>
bool Read(const void* address, T& value) noexcept {
    if (address == nullptr) return false;
    std::memcpy(&value, address, sizeof(T));
    return true;
}

void* ReadPointer(const void* address) noexcept {
    std::uintptr_t value{};
    return Read(address, value) ? reinterpret_cast<void*>(value) : nullptr;
}

bool ResolveRipRelative(
    const Context& context, const std::string_view pattern,
    const std::ptrdiff_t addend, std::uintptr_t& address) noexcept {
    address = 0;
    if (!SignatureReady(context.signature)) return false;
    std::uintptr_t instruction{};
    if (context.signature->resolve(
            context.signature->user, anomaly::sdk::StringView("HTGame.exe"),
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

void* ObjectAt(const std::uintptr_t g_objects, const std::uint32_t index) noexcept {
    std::int32_t count{};
    std::int32_t num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(g_objects + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(g_objects + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(g_objects + kObjectNumChunksOffset),
              num_chunks) ||
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

std::string ResolveName(
    const AnomalyUe5NamesServiceV1* names, const std::uint32_t name_id) {
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

bool SnapshotPlayerPosition(Context& context, double (&position)[3]) noexcept {
    if (context.player == nullptr || context.player->snapshot == nullptr) {
        return false;
    }
    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    if (context.player->snapshot(context.player->user, &snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        snapshot.handle.id == 0) {
        return false;
    }
    position[0] = snapshot.position[0];
    position[1] = snapshot.position[1];
    position[2] = snapshot.position[2];
    return true;
}

double PlanarDistanceSquared(double ax, double ay, double bx, double by) noexcept {
    const double dx = ax - bx;
    const double dy = ay - by;
    return dx * dx + dy * dy;
}

bool LandmarksReady(const AnomalyNteMapLandmarksServiceV1* s) noexcept {
    return HasField<AnomalyNteMapLandmarksServiceV1,
               decltype(AnomalyNteMapLandmarksServiceV1::teleport)>(
               s, offsetof(AnomalyNteMapLandmarksServiceV1, teleport)) &&
        s->sequence != nullptr && s->count != nullptr &&
        s->snapshot_at != nullptr && s->teleport != nullptr;
}

// Caller must hold context.mutex.
bool RefreshLandmarkCatalog(Context& context) noexcept {
    const auto* service = context.map_landmarks;
    if (!LandmarksReady(service)) return false;
    const std::uint64_t sequence = service->sequence(service->user);
    if (sequence == 0) return false;
    if (context.landmarks_sequence == sequence) return true;
    const std::uint32_t count = service->count(service->user);
    if (count > kMaximumMapLandmarks) return false;
    std::vector<MapLandmark> landmarks;
    landmarks.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        AnomalyNteMapLandmarkSnapshotV1 snapshot{sizeof(snapshot)};
        if (service->snapshot_at(service->user, index, &snapshot).code !=
                ANOMALY_STATUS_V1_OK ||
            snapshot.sequence != sequence ||
                (snapshot.flags & ANOMALY_NTE_MAP_LANDMARK_V1_VALID) == 0) {
            return false;
        }
        if (std::string_view(snapshot.world) != kLandmarkWorld) continue;
        if (!std::isfinite(snapshot.destination[0]) ||
            !std::isfinite(snapshot.destination[1]) ||
            !std::isfinite(snapshot.destination[2])) {
            continue;
        }
        MapLandmark landmark;
        landmark.sequence = sequence;
        landmark.index = index;
        landmark.destination[0] = snapshot.destination[0];
        landmark.destination[1] = snapshot.destination[1];
        landmark.destination[2] = snapshot.destination[2];
        landmark.teleport_id = snapshot.teleport_id;
        landmarks.push_back(std::move(landmark));
    }
    if (service->sequence(service->user) != sequence) return false;
    context.landmarks_sequence = sequence;
    context.landmarks = std::move(landmarks);
    return true;
}

// Caller must hold context.mutex.
bool TryBeginLandmarkTransfer(Context& context, const Point& target,
                              double player_x, double player_y) noexcept {
    if (context.landmark_transfer_attempted) return false;
    context.landmark_transfer_attempted = true;
    if (!RefreshLandmarkCatalog(context) || context.landmarks.empty()) {
        return false;
    }
    std::size_t nearest_index = 0;
    double nearest_distance = 1e300;
    for (std::size_t i = 0; i < context.landmarks.size(); ++i) {
        const double distance = PlanarDistanceSquared(
            context.landmarks[i].destination[0],
            context.landmarks[i].destination[1], target.x, target.y);
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest_index = i;
        }
    }
    const double direct_distance =
        PlanarDistanceSquared(player_x, player_y, target.x, target.y);
    if (!(direct_distance > nearest_distance)) return false;
    const MapLandmark& nearest = context.landmarks[nearest_index];
    const auto* service = context.map_landmarks;
    if (!LandmarksReady(service) ||
        service->sequence(service->user) != nearest.sequence) {
        return false;
    }
    AnomalyNteMapLandmarkTeleportRequestV1 request{sizeof(request)};
    request.mode = ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL;
    request.sequence = nearest.sequence;
    request.index = nearest.index;
    if (service->teleport(service->user, &request).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    context.landmark_destination[0] = nearest.destination[0];
    context.landmark_destination[1] = nearest.destination[1];
    context.landmark_destination[2] = nearest.destination[2];
    context.landmark_origin[0] = player_x;
    context.landmark_origin[1] = player_y;
    context.landmark_arrival_time = std::chrono::steady_clock::time_point{};
    context.landmark_transfer_wait = true;
    context.landmark_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<long long>(
            kLandmarkTransferTimeoutSeconds * 1000.0));
    return true;
}

std::string GetStateDirectory(const AnomalyPluginStateServiceV1* service) noexcept {
    if (service == nullptr || service->directory == nullptr) return {};
    std::size_t size{};
    if (service->directory(service->user, nullptr, &size).code != ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > 4096) {
        return {};
    }
    std::string value(size, '\0');
    if (service->directory(service->user, value.data(), &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > value.size()) {
        return {};
    }
    value.resize(size - 1U);
    return value;
}

void ReadTable(Context& context) {
    std::vector<Point> points;
    void* table_object{};
    if (!ObjectsReady(context.objects)) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = context.localizer.Text(
            "status.read_failed", "Read table failed");
        return;
    }
    AnomalyGenerationHandleV1 handle{};
    const auto st = context.objects->find_exact(
        context.objects->user, anomaly::sdk::StringView(kTablePath), &handle);
    if (st.code != ANOMALY_STATUS_V1_OK || handle.id == 0) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = context.localizer.Text(
            "status.read_failed", "Read table failed");
        return;
    }
    if (context.g_objects_address == 0 &&
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            context.g_objects_address)) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = context.localizer.Text(
            "status.read_failed", "Read table failed");
        return;
    }
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    table_object = ObjectAt(context.g_objects_address, index);
    if (table_object == nullptr) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = context.localizer.Text(
            "status.read_failed", "Read table failed");
        return;
    }
    struct ArrayHeader {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    } header;
    if (!Read(reinterpret_cast<const void*>(
                  reinterpret_cast<std::uintptr_t>(table_object) + kDataTableRowMapOffset),
              header) ||
        header.count <= 0 || header.capacity < header.count || header.data == 0) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = context.localizer.Text(
            "status.read_failed", "Read table failed");
        return;
    }
    for (std::int32_t i = 0; i < header.count; ++i) {
        const auto element = header.data +
            static_cast<std::uintptr_t>(i) * kDataTableRowStride;
        RawName row_id{};
        std::uintptr_t row{};
        if (!Read(reinterpret_cast<const void*>(element), row_id) ||
            row_id.comparison_index == 0) continue;
        if (!Read(reinterpret_cast<const void*>(element + kDataTableRowPointerOffset),
                  row) || row == 0) continue;
        double x{}, y{}, z{};
        Read(reinterpret_cast<const void*>(row + kCoordOffset), x);
        Read(reinterpret_cast<const void*>(row + kCoordOffset + 8), y);
        Read(reinterpret_cast<const void*>(row + kCoordOffset + 16), z);
        if (x == 0.0 && y == 0.0 && z == 0.0) continue;
        Point p;
        p.row_name = ResolveName(
            context.names, static_cast<std::uint32_t>(row_id.comparison_index));
        p.x = x;
        p.y = y;
        p.z = z;
        points.push_back(std::move(p));
    }
    std::lock_guard<std::mutex> lock(context.mutex);
    context.points = std::move(points);
    RebuildFilteredLocked(context);
    const std::string count_str = std::to_string(context.points.size());
    const std::array read_args{std::string_view(count_str)};
    context.status = context.localizer.Format(
        "status.read", "Read {0} points", read_args);
}

bool Teleport(Context& context, const Point& p) noexcept {
    if (context.session == nullptr || context.player == nullptr ||
        context.teleport == nullptr || context.teleport->teleport == nullptr) {
        return false;
    }
    AnomalyNteSessionSnapshotV1 ss{sizeof(ss)};
    AnomalyNtePlayerSnapshotV1 ps{sizeof(ps)};
    if (context.session->snapshot(context.session->user, &ss).code != ANOMALY_STATUS_V1_OK ||
        context.player->snapshot(context.player->user, &ps).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    if (ss.world.id == 0 || ps.handle.id == 0) return false;
    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    request.flags = 0;
    request.world = ss.world;
    request.player = ps.handle;
    request.position[0] = p.x;
    request.position[1] = p.y;
    request.position[2] = p.z + kTeleportZOffset;
    return context.teleport->teleport(context.teleport->user, &request).code ==
        ANOMALY_STATUS_V1_OK;
}

bool IssueNavigation(Context& context, const Point& p) noexcept {
    if (!NavigationReady(context.navigation)) return false;
    double destination[3]{p.x, p.y, p.z};
    double player_position[3]{};
    if (SnapshotPlayerPosition(context, player_position)) {
        const double dx = player_position[0] - p.x;
        const double dy = player_position[1] - p.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length > 1.0) {
            destination[0] += dx * kApproachRadiusCentimeters / length;
            destination[1] += dy * kApproachRadiusCentimeters / length;
        }
    }
    return context.navigation->move_to_location(
        context.navigation->user, destination).code == ANOMALY_STATUS_V1_OK;
}

bool StartNavigation(Context& context, const Point& p,
                     std::chrono::steady_clock::time_point now) noexcept {
    if (!IssueNavigation(context, p)) return false;
    context.navigating = true;
    context.navigation_target[0] = p.x;
    context.navigation_target[1] = p.y;
    context.navigation_target[2] = p.z;
    context.navigation_has_last_position = false;
    context.navigation_last_progress_at = now;
    context.navigation_progress_check_at = now + std::chrono::milliseconds(
        static_cast<long long>(kProgressCheckIntervalSeconds * 1000.0));
    context.navigation_retry_at = now + std::chrono::milliseconds(
        static_cast<long long>(kReissueDelaySeconds * 1000.0));
    return true;
}

void LoadDoneSet(Context& context) {
    context.done_set.clear();
    if (context.state_directory.empty()) return;
    const std::string path = context.state_directory + "\\done.txt";
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return;
    std::string line;
    char buffer[1024];
    std::size_t n{};
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                if (!line.empty()) context.done_set.insert(line);
                line.clear();
            } else {
                line.push_back(buffer[i]);
            }
        }
    }
    if (!line.empty()) context.done_set.insert(line);
    std::fclose(file);
}

void SaveDoneSet(Context& context) {
    if (context.state_directory.empty()) return;
    const std::string path = context.state_directory + "\\done.txt";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return;
    for (const std::string& row : context.done_set) {
        std::fwrite(row.data(), 1, row.size(), file);
        std::fwrite("\n", 1, 1, file);
    }
    std::fclose(file);
}

// —— 自动战斗阶段（plugins/common/combat）——
// 模块不认识面板：它每 tick 报的状态文本经这个回调落进插件自己的状态行，面板始终只显示一行。
void SetCombatStatus(void* user, const std::string& text) noexcept {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr) return;
    std::lock_guard<std::mutex> lock(context->mutex);
    context->status = text;
}

// 模块不查服务，宿主指针每次 tick 重建一份。
combat::Host MakeCombatHost(Context& context) noexcept {
    // 有些服务可能在本插件 Load 之后才发布（实体/角色快照随世界加载），Load 时查询会静默
    // 拿到空指针。这里对空指针惰性重查——与一键副本对动态服务（session/combat/skills）的处理
    // 一致。只补空的，非空不重查（避免每帧做无谓查询）。
    if (context.actors == nullptr || context.entities == nullptr ||
        context.combat == nullptr || context.skills == nullptr ||
        context.skill_invocation == nullptr || context.pickup == nullptr) {
        const auto view = anomaly::sdk::Host(context.host);
        if (context.actors == nullptr) {
            context.actors = view.Query<AnomalyNteActorsServiceV1>(
                ANOMALY_NTE_ACTORS_SERVICE_V1_ID,
                ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).get();
        }
        if (context.entities == nullptr) {
            context.entities = view.Query<AnomalyNteEntitiesServiceV1>(
                ANOMALY_NTE_ENTITIES_SERVICE_V1_ID,
                ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION).get();
        }
        if (context.combat == nullptr) {
            context.combat = view.Query<AnomalyNteCombatServiceV1>(
                ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
                ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
        }
        if (context.skills == nullptr) {
            context.skills = view.Query<AnomalyNteSkillsServiceV1>(
                ANOMALY_NTE_SKILLS_SERVICE_V1_ID,
                ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
        }
        if (context.skill_invocation == nullptr) {
            context.skill_invocation = view.Query<AnomalyNteSkillInvocationServiceV1>(
                ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
                ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).get();
        }
        if (context.pickup == nullptr) {
            context.pickup = view.Query<AnomalyNtePickupServiceV1>(
                ANOMALY_NTE_PICKUP_SERVICE_V1_ID,
                ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION).get();
        }
    }
    combat::Host host;
    host.combat = context.combat;
    host.navigation = context.navigation;
    host.actors = context.actors;
    host.entities = context.entities;
    host.names = context.names;
    host.objects = context.objects;
    host.player = context.player;
    host.teleport = context.teleport;
    host.session = context.session;
    host.signature = context.signature;
    host.skills = context.skills;
    host.skill_invocation = context.skill_invocation;
    host.pickup = context.pickup;
    host.search_radius_m = static_cast<std::uint32_t>(
        std::clamp(context.search_radius_m.load(std::memory_order_relaxed), 10.0, 300.0));
    host.developer_mode = context.developer_mode.load(std::memory_order_acquire);
    // 与副本一致：清空那一刻由模块自己发一次范围拾取（20 米 / 最多 10 件）。
    host.loot_after_kill = true;
    host.melee_mode = context.melee_mode.load(std::memory_order_relaxed);
    // test_input_id 保持模块默认值。
    host.set_status = &SetCombatStatus;
    host.status_user = &context;
    return host;
}

void SetStatusText(Context& context, const std::string_view key,
                   const std::string_view fallback) {
    std::lock_guard<std::mutex> lock(context.mutex);
    context.status = context.localizer.Text(key, fallback);
}

// 点起不来（传送/寻路失败）：状态行留下原因并停住本轮，不静默跳过。
void FailPointStart(Context& context, const std::string_view key,
                    const std::string_view fallback) {
    std::lock_guard<std::mutex> lock(context.mutex);
    context.auto_run = false;
    context.combat_retry_pending = false;
    context.combat_retry_engaged = false;
    context.status = context.localizer.Text(key, fallback);
}

// 结束战斗阶段并清空模块状态（换点、跳过、停止本轮都走这里）。
void EndCombat(Context& context) {
    if (!context.combat_active) return;
    combat::Host host = MakeCombatHost(context);
    combat::Reset(host, context.combat_state);
    context.combat_state.dead_targets.clear();
    context.combat_active = false;
}

// 开始一个点：传送（开发者模式）或寻路过去；到达由 Update 的到达分支接手进入战斗阶段。
// fallout_retry：这是「掉出世界后重传同一个点」——沿用本点的重试计数、强制走传送
// （掉出地图后寻路没有意义），并保留首次接敌超时的起点。
void StartPoint(Context& context, const Point& p,
                std::chrono::steady_clock::time_point now,
                bool fallout_retry = false) {
    context.combat_retry_pending = fallout_retry;
    if (!fallout_retry) context.fallout_retries = 0;
    // 上一轮战斗可能还在跑（例如战斗中用户直接点了另一个点）：先收干净再出发，
    // 否则模块会一边寻路一边抢导航、状态行也会被它覆盖。
    EndCombat(context);
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.current_row = p.row_name;
    }
    context.pending_target[0] = p.x;
    context.pending_target[1] = p.y;
    context.pending_target[2] = p.z;
    context.teleport_arrival_wait = false;
    context.landmark_transfer_attempted = false;
    context.landmark_transfer_wait = false;
    if (context.navigating && NavigationReady(context.navigation)) {
        context.navigation->stop_movement(context.navigation->user);
    }
    context.navigating = false;
    context.navigation_has_last_position = false;
    if (fallout_retry || context.developer_mode.load(std::memory_order_acquire)) {
        if (Teleport(context, p)) {
            // 传送会带来巨大的高度跳变：重置掉落追踪，否则下一帧会把传送本身判成掉落。
            context.fallout_last_z = 0.0;
            context.fallout_last_at = {};
            context.teleport_arrival_wait = true;
            context.teleport_arrival_deadline = now + std::chrono::milliseconds(
                static_cast<long long>(kTeleportArrivalTimeoutSeconds * 1000.0));
        } else {
            FailPointStart(context, "status.teleport_failed", "Teleport failed");
        }
        return;
    }
    if (!NavigationReady(context.navigation)) {
        FailPointStart(context, "status.navigation_unavailable",
                       "Navigation unavailable");
        return;
    }
    double player_position[3]{};
    const bool have_player = SnapshotPlayerPosition(context, player_position);
    bool landmark_started = false;
    if (have_player) {
        landmark_started = TryBeginLandmarkTransfer(
            context, p, player_position[0], player_position[1]);
    }
    if (landmark_started) {
        SetStatusText(context, "status.landmark_transfer", "Fast travel");
    } else if (StartNavigation(context, p, now)) {
        std::lock_guard<std::mutex> lock(context.mutex);
        const std::array nav_args{std::string_view(p.row_name)};
        context.status = context.localizer.Format(
            "status.navigating", "Walking [{0}]", nav_args);
    } else {
        FailPointStart(context, "status.navigation_failed", "Walk failed");
    }
}

// 到达本点：清空模块状态，开始「首次接敌超时」计时；随后每 tick 由 TickCombat 驱动。
void BeginCombat(Context& context, std::chrono::steady_clock::time_point now) {
    // 新点位：清零掉落重传计数 ✗（它只在「见到怪」时清零 ✗，否则会跨点位累加 ✗）。
    context.fallout_retries = 0;
    combat::Host host = MakeCombatHost(context);
    combat::Reset(host, context.combat_state);
    // Reset 不清黑名单表：换点必须自己清，否则上个点拉黑的目标会带到新点。
    context.combat_state.dead_targets.clear();
    context.combat_active = true;
    {
        double position[3]{};
        context.combat_start_z = SnapshotPlayerPosition(context, position)
            ? position[2] : context.pending_target[2];
    }
    context.combat_target_at = {};
    context.combat_had_target = false;
    context.fallout_retry_at = {};
    if (context.combat_retry_pending) {
        // 掉出世界后重传回到同一个点：不重置首次接敌超时的起点（否则重试可以无限续命），
        // 并恢复「本点见过怪」——Reset 会清掉它，而这一点对同一点的重传依然成立；
        // 不恢复的话，重传回来第一帧就会被首次接敌超时判跳过，重试等于白做。
        context.combat_state.met_monster = context.combat_retry_engaged;
    } else {
        context.combat_started_at = now;
    }
    context.combat_retry_pending = false;
    context.combat_retry_engaged = false;
    std::lock_guard<std::mutex> lock(context.mutex);
    const std::array args{std::string_view(context.current_row)};
    context.status = context.localizer.Format(
        "status.combat_start", "Auto combat [{0}]", args);
}

// 掉出世界（判据与 BoxAuto 相同：玩家 Z 比当前点低 10 米以上）。
bool FellOutOfWorld(Context& context) {
    // 正在打一个比你低得多的怪时（悬崖/多层地形，开发者模式下模块还会传送到怪身上），
    // 玩家位置会合法地低于点位——那不是掉出世界，所以有目标就不判。
    if (context.combat_state.target_valid) return false;
    double position[3]{};
    if (!SnapshotPlayerPosition(context, position)) return false;
    const auto now = std::chrono::steady_clock::now();
    if (context.fallout_last_at == std::chrono::steady_clock::time_point{}) {
        context.fallout_last_z = position[2];
        context.fallout_last_at = now;
        return false;
    }
    const double seconds =
        std::chrono::duration<double>(now - context.fallout_last_at).count();
    if (seconds < 0.05) return false;
    const double rate = (context.fallout_last_z - position[2]) / seconds;
    context.fallout_last_z = position[2];
    context.fallout_last_at = now;
    if (rate > kFallOutRateCentimetersPerSecond) return true;
    // 主要判据：比当前点位低 1 米（坑底速度归零也能触发）。
    return position[2] < context.pending_target[2] - kFallOutBelowPointCentimeters;
}

// 掉出世界后重传当前点：走与出发同一条路径（StartPoint 的传送分支），不另写一套。
void RetryCurrentPoint(Context& context, std::chrono::steady_clock::time_point now) {
    const bool engaged = context.combat_state.met_monster;
    Point p;
    p.x = context.pending_target[0];
    p.y = context.pending_target[1];
    p.z = context.pending_target[2];
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        p.row_name = context.current_row;
    }
    StartPoint(context, p, now, /*fallout_retry=*/true);
    // 传给 BeginCombat：重传回来后要不要把「见过怪」恢复回去（出发失败时 pending 已被清掉）。
    if (context.combat_retry_pending) context.combat_retry_engaged = engaged;
}

// 本点结束（打完或跳过）：不标记，直接取列表里的下一个点；列表走完就收工。
void AdvancePoint(Context& context, std::chrono::steady_clock::time_point now) {
    Point next;
    bool have_next = false;
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        if (!context.auto_run) return;
        std::size_t index = context.filtered_points.size();
        for (std::size_t i = 0; i < context.filtered_points.size(); ++i) {
            if (context.filtered_points[i].row_name == context.current_row) {
                index = i;
                break;
            }
        }
        if (index + 1 < context.filtered_points.size()) {
            next = context.filtered_points[index + 1];
            have_next = true;
        } else {
            context.auto_run = false;
            context.status = context.localizer.Text("status.run_finished", "Round finished");
        }
    }
    if (have_next) StartPoint(context, next, now);
}

void StopRun(Context& context) {
    if (context.navigating && NavigationReady(context.navigation)) {
        context.navigation->stop_movement(context.navigation->user);
    }
    context.navigating = false;
    context.teleport_arrival_wait = false;
    context.landmark_transfer_wait = false;
    combat::Host host = MakeCombatHost(context);
    combat::Reset(host, context.combat_state);
    context.combat_active = false;
    context.combat_retry_pending = false;
    context.combat_retry_engaged = false;
    context.fallout_retries = 0;
    std::lock_guard<std::mutex> lock(context.mutex);
    context.auto_run = false;
    context.status = context.localizer.Text("status.stopped", "Stopped");
}

// 伤害流不可用时的兜底：日志实测 `dmg=never`（打死了怪也收不到伤害事件），此时模块内部
// 「8 秒没伤害就换靶」的计时会被距离抖动/句柄变化反复清零，永远攒不满。这里用调用方自己
// 的时钟判断「多久没有伤害了」——参考点取最近一次伤害，从没有过就取进入战斗的时刻。
bool NoDamageForTooLong(Context& context, std::chrono::steady_clock::time_point now) {
    if (!context.combat_state.met_monster) return false;
    // 参考点取最晚的一个：最近一次伤害 / 最近一次换目标 / 进入战斗。
    // 换目标也要重置，否则「刚出现怪就被兜底结束」（日志实测：第 7 秒出怪、第 8 秒被判无伤害）。
    auto reference = context.combat_started_at;
    const auto last_hit = context.combat_state.last_player_hit_at;
    if (last_hit.time_since_epoch().count() != 0 && last_hit > reference) {
        reference = last_hit;
    }
    if (context.combat_target_at.time_since_epoch().count() != 0 &&
        context.combat_target_at > reference) {
        reference = context.combat_target_at;
    }
    const double seconds =
        context.no_damage_fallback_seconds.load(std::memory_order_relaxed);
    return now - reference > std::chrono::milliseconds(
        static_cast<long long>(seconds * 1000.0));
}

// 战斗阶段每 tick 调用一次。模块内部已负责选靶/接近/攻击，以及 cleared 那一次拾取。
void TickCombat(Context& context, std::chrono::steady_clock::time_point now) {
    // 掉出世界先于模块 tick 处理：地图外不该再让模块选靶/寻路。
    if (FellOutOfWorld(context)) {
        // 还没到下一次重传时刻就等着（地图可能正在加载），期间保持战斗阶段不动。
        if (now < context.fallout_retry_at) return;
        // 重传也要重启「首次接敌超时」的计时器 ✗：否则重传消耗的是同一个 15 秒 ✗，
    // 时间一到就会直接跳过（实测：传几次就跳过 ✗）。
    context.combat_started_at = now;
    context.fallout_retry_at = now + kFallOutRetryDelay;
        if (context.fallout_retries < kFallOutMaximumRetries) {
            ++context.fallout_retries;
            const std::string retries = std::to_string(context.fallout_retries);
            const std::string maximum = std::to_string(kFallOutMaximumRetries);
            RetryCurrentPoint(context, now);
            std::lock_guard<std::mutex> lock(context.mutex);
            const std::array args{std::string_view(retries), std::string_view(maximum),
                                  std::string_view(context.current_row)};
            context.status = context.localizer.Format(
                "status.fallout_retry", "Fell out of world, re-teleport {0}/{1} [{2}]",
                args);
            return;
        }
        EndCombat(context);
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            const std::array args{std::string_view(context.current_row)};
            context.status = context.localizer.Format(
                "status.fallout_skip", "Fell out of world, skip [{0}]", args);
        }
        AdvancePoint(context, now);
        return;
    }
    combat::Host host = MakeCombatHost(context);
    const combat::Result result = combat::Tick(host, context.combat_state);
    // 已经站稳开打（见过怪）⇒ 掉落计数清零，别让之前的掉落把后面的正常战斗罚掉。
    if (context.combat_state.met_monster) context.fallout_retries = 0;
    // 只在「从没目标变成有目标」时重置兜底计时——那才算重新开打。
    // 不能用「换目标」做条件：目标会在几具尸体之间每秒来回切（日志实测），那样计时会被
    // 反复刷新，兜底永远攒不满，点就卡死了。
    if (context.combat_state.target_valid && !context.combat_had_target) {
        context.combat_target_at = now;
    }
    context.combat_had_target = context.combat_state.target_valid;
    // 没有总超时：只有「这个点从头到尾没见过怪」才由这里收场（working/unavailable 都算）。
    const double timeout_seconds =
        context.first_contact_timeout_seconds.load(std::memory_order_relaxed);
    const bool first_contact_timed_out = !context.combat_state.met_monster &&
        now - context.combat_started_at >= std::chrono::milliseconds(
            static_cast<long long>(timeout_seconds * 1000.0));
    if (result == combat::Result::cleared) {
        EndCombat(context);
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            const std::array args{std::string_view(context.current_row)};
            context.status = context.localizer.Format(
                "status.point_cleared", "Cleared [{0}] (not marked)", args);
        }
        AdvancePoint(context, now);
        return;
    }
    if (NoDamageForTooLong(context, now)) {
        EndCombat(context);
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            const std::array args{std::string_view(context.current_row)};
            context.status = context.localizer.Format(
                "status.no_damage_timeout", "No damage for {0}s, done [{1}]",
                std::array{std::string_view(std::to_string(static_cast<int>(
                                context.no_damage_fallback_seconds.load(
                                    std::memory_order_relaxed)))),
                           std::string_view(context.current_row)});
        }
        AdvancePoint(context, now);
        return;
    }
    // 「打不动」（普攻/技能调用失败）：见过怪之后首次接敌超时永远不会触发，而模块自己的
    // 黑名单 TTL 是 45/120 秒——不在这里立即跳过就要白等两分钟。
    if (result == combat::Result::unavailable && context.combat_state.attack_failed) {
        EndCombat(context);
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            const std::array args{std::string_view(context.current_row)};
            context.status = context.localizer.Format(
                "status.attack_failed", "Cannot attack, skip [{0}]", args);
        }
        AdvancePoint(context, now);
        return;
    }
    if (first_contact_timed_out) {
        EndCombat(context);
        const std::string seconds = std::to_string(static_cast<int>(timeout_seconds));
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            const std::array args{std::string_view(seconds),
                                  std::string_view(context.current_row)};
            context.status = context.localizer.Format(
                "status.first_contact_timeout", "No monster in {0}s, skipped [{1}]",
                args);
        }
        AdvancePoint(context, now);
        return;
    }
    // working / unavailable：状态行已由模块经 SetCombatStatus 写入。
    // 战斗必需的服务没拿到时给出明确提示（否则模块只会笼统地说「等待目标数据」）。
    if (context.actors == nullptr || context.entities == nullptr ||
        context.combat == nullptr) {
        std::string missing;
        const auto add = [&missing](const char* name) {
            if (!missing.empty()) missing += "/";
            missing += name;
        };
        if (context.actors == nullptr) add("actors");
        if (context.entities == nullptr) add("entities");
        if (context.combat == nullptr) add("combat");
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = "战斗服务缺失: " + missing;
    }
}

void Draw(void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    if (plugin_context == nullptr) return;
    auto& context = *static_cast<Context*>(plugin_context);
    const AnomalyUiServiceV1* ui = supplied_ui;
    if (ui == nullptr) {
        ui = anomaly::sdk::Host(context.host)
                 .Query<AnomalyUiServiceV1>(
                     ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION)
                 .get();
    }
    if (ui == nullptr || ui->text == nullptr || ui->button == nullptr) return;
    context.developer_mode.store(DeveloperModeEnabled(ui),
                                 std::memory_order_release);
    int open = 1;
    const std::string window_title =
        context.localizer.Text("window.title", "打怪资源点");
    anomaly::sdk::UiWindow window(ui, window_title, &open);
    if (!window) return;

    const std::string combat_label =
        context.localizer.Text("type.combat", "打怪宝箱（xx点的赠礼）");
    const std::string vision_label =
        context.localizer.Text("type.vision", "异像家具材料");
    if (ui->button(ui->user, anomaly::sdk::StringView(combat_label), 0.0F, 0.0F) != 0) {
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            context.type_choice = 0;
            context.sub_choice = 0;
            context.points.clear();
            RebuildFilteredLocked(context);
            context.status = context.localizer.Text("status.combat_hint", "正在读表...");
        }
        context.read_pending.store(true, std::memory_order_release);
    }
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 4.0F);
    if (ui->button(ui->user, anomaly::sdk::StringView(vision_label), 0.0F, 0.0F) != 0) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.type_choice = 1;
        context.sub_choice = 0;
        context.points = VisionPoints();
        RebuildFilteredLocked(context);
        const std::string count_str = std::to_string(context.points.size());
        const std::array count_args{std::string_view(count_str)};
        context.status = context.localizer.Format("status.read", "Read {0} points", count_args);
    }
    ui->separator(ui->user);

    const std::string all_label = context.localizer.Text("sub.all", "全部");
    if (ui->button(ui->user, anomaly::sdk::StringView(all_label), 0.0F, 0.0F) != 0) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.sub_choice = 0;
        RebuildFilteredLocked(context);
    }
    if (context.type_choice == 1) {
        for (int i = 0; i < 6; ++i) {
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 4.0F);
            if (ui->button(ui->user, anomaly::sdk::StringView(kVisionNames[i]), 0.0F, 0.0F) != 0) {
                std::lock_guard<std::mutex> lock(context.mutex);
                context.sub_choice = static_cast<std::uint32_t>(i + 1);
                RebuildFilteredLocked(context);
            }
        }
    }
    ui->separator(ui->user);

    // 自动一轮：开始 / 停止，以及可调的「首次接敌超时（秒）」。
    if (ui->input_double != nullptr) {
        double timeout_seconds =
            context.first_contact_timeout_seconds.load(std::memory_order_relaxed);
        const std::string timeout_label = context.localizer.Text(
            "label.first_contact_timeout", "First contact timeout (s)");
        if (ui->input_double(ui->user, anomaly::sdk::StringView(timeout_label),
                             &timeout_seconds, 1.0, 10.0)) {
            context.first_contact_timeout_seconds.store(
                std::clamp(timeout_seconds, 1.0, 120.0), std::memory_order_relaxed);
        }
        double radius = context.search_radius_m.load(std::memory_order_relaxed);
        const std::string radius_label = context.localizer.Text(
            "label.search_radius", "Search radius (m)");
        if (ui->input_double(ui->user, anomaly::sdk::StringView(radius_label),
                             &radius, 5.0, 10.0)) {
            context.search_radius_m.store(
                std::clamp(radius, 10.0, 300.0), std::memory_order_relaxed);
        }
        double no_damage =
            context.no_damage_fallback_seconds.load(std::memory_order_relaxed);
        const std::string no_damage_label = context.localizer.Text(
            "label.no_damage_fallback", "Give up point after no damage (s)");
        if (ui->input_double(ui->user, anomaly::sdk::StringView(no_damage_label),
                             &no_damage, 5.0, 15.0)) {
            context.no_damage_fallback_seconds.store(
                std::clamp(no_damage, 5.0, 600.0), std::memory_order_relaxed);
        }
    }
    // 攻击策略开关：与一键副本的「平A模式」同一个含义。开着时模块才会把「打不动」标成
    // attack_failed，本插件据此立即跳过该点；关着时只放技能、技能失败被忽略。
    if (ui->checkbox != nullptr) {
        int melee = context.melee_mode.load(std::memory_order_relaxed) ? 1 : 0;
        const std::string melee_label =
            context.localizer.Text("action.melee_mode", "Melee mode");
        const std::string melee_id = melee_label + "##melee";
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(melee_id), &melee) != 0) {
            context.melee_mode.store(melee != 0, std::memory_order_relaxed);
        }
    }
    const std::string auto_start_label =
        context.localizer.Text("action.auto_start", "Start auto");
    if (ui->button(ui->user, anomaly::sdk::StringView(auto_start_label), 0.0F, 0.0F) != 0) {
        context.auto_start_pending.store(true, std::memory_order_release);
    }
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 4.0F);
    const std::string auto_stop_label = context.localizer.Text("action.stop", "Stop");
    if (ui->button(ui->user, anomaly::sdk::StringView(auto_stop_label), 0.0F, 0.0F) != 0) {
        context.auto_stop_pending.store(true, std::memory_order_release);
    }
    ui->separator(ui->user);

    std::string status;
    std::vector<Point> list;
    std::string current_row;
    bool auto_run = false;
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        status = context.status;
        list = context.filtered_points;
        current_row = context.current_row;
        auto_run = context.auto_run;
    }
    if (!status.empty()) {
        ui->text(ui->user, anomaly::sdk::StringView(status));
    }
    if (auto_run && !current_row.empty()) {
        std::size_t index = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].row_name == current_row) {
                index = i;
                break;
            }
        }
        const std::string position =
            std::to_string(index + 1) + "/" + std::to_string(list.size());
        const std::array progress_args{
            std::string_view(position), std::string_view(current_row)};
        const std::string progress = context.localizer.Format(
            "status.run_progress", "Auto {0} [{1}]", progress_args);
        ui->text(ui->user, anomaly::sdk::StringView(progress));
    }
    const std::string done_str = std::to_string(context.done_set.size());
    const std::string total_str = std::to_string(list.size());
    const std::array done_args{
        std::string_view(done_str), std::string_view(total_str)};
    const std::string done_count = context.localizer.Format(
        "status.done_count", "Done {0}/{1}", done_args);
    ui->text(ui->user, anomaly::sdk::StringView(done_count));

    ui->separator(ui->user);
    if (ui->begin_child != nullptr && ui->end_child != nullptr) {
        ui->begin_child(ui->user, anomaly::sdk::StringView("list"), 0.0F, 400.0F, 0);
        const bool developer_mode =
            context.developer_mode.load(std::memory_order_acquire);
        const std::string tp_label = context.localizer.Text(
            developer_mode ? "action.teleport" : "action.navigate",
            developer_mode ? "TP" : "Walk");
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Point& pt = list[i];
            const std::string tp_btn = tp_label + "##tp" + std::to_string(i);
            if (ui->button(ui->user, anomaly::sdk::StringView(tp_btn), 0.0F, 0.0F) != 0) {
                context.manual_teleport_x = pt.x;
                context.manual_teleport_y = pt.y;
                context.manual_teleport_z = pt.z;
                context.manual_teleport_row = pt.row_name;
                context.manual_teleport_pending.store(true, std::memory_order_release);
            }
            if (ui->same_line != nullptr) {
                ui->same_line(ui->user, 0.0F, 4.0F);
            }
            ui->text(ui->user, anomaly::sdk::StringView(pt.row_name));
            if (ui->same_line != nullptr) {
                ui->same_line(ui->user, 0.0F, 4.0F);
            }
            bool done = context.done_set.count(pt.row_name) > 0;
            int done_int = done ? 1 : 0;
            const std::string done_label = context.localizer.Text(
                done ? "action.unmark" : "action.mark_done",
                done ? "Unmark" : "Mark done");
            const std::string done_id =
                done_label + "##done" + std::to_string(i);
            if (ui->checkbox != nullptr &&
                ui->checkbox(ui->user, anomaly::sdk::StringView(done_id),
                             &done_int) != 0) {
                if (done_int != 0) {
                    context.done_set.insert(pt.row_name);
                } else {
                    context.done_set.erase(pt.row_name);
                }
                context.save_pending.store(true, std::memory_order_release);
            }
        }
        ui->end_child(ui->user);
    }
}

}  // namespace

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!host || !plugin_context || host->api_major != ANOMALY_PLUGIN_API_V1_MAJOR) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    auto* context = new (std::nothrow) Context{};
    if (!context) return {ANOMALY_STATUS_V1_FAILED, 0, {nullptr, 0}};
    context->host = host;
    const auto view = anomaly::sdk::Host(host);
    context->localizer = anomaly::plugins::Localizer(host);
    context->signature = view.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
    context->names = view.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    context->objects = view.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    context->session = view.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    context->player = view.Query<AnomalyNtePlayerServiceV1>(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    context->teleport = view.Query<AnomalyNtePlayerTeleportServiceV1>(
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION).get();
    context->navigation = view.Query<AnomalyNteNavigationServiceV1>(
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID,
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION).get();
    context->map_landmarks = view.Query<AnomalyNteMapLandmarksServiceV1>(
        ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID,
        ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION).get();
    // 自动战斗模块用到的服务（可选：缺哪个模块自己会报不可用）。
    context->combat = view.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
        ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    context->skills = view.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID,
        ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
    context->skill_invocation = view.Query<AnomalyNteSkillInvocationServiceV1>(
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).get();
    context->actors = view.Query<AnomalyNteActorsServiceV1>(
        ANOMALY_NTE_ACTORS_SERVICE_V1_ID,
        ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).get();
    context->entities = view.Query<AnomalyNteEntitiesServiceV1>(
        ANOMALY_NTE_ENTITIES_SERVICE_V1_ID,
        ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION).get();
    context->pickup = view.Query<AnomalyNtePickupServiceV1>(
        ANOMALY_NTE_PICKUP_SERVICE_V1_ID,
        ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION).get();
    context->plugin_state = view.Query<AnomalyPluginStateServiceV1>(
        ANOMALY_PLUGIN_STATE_SERVICE_V1_ID,
        ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION).get();
    context->state_directory = GetStateDirectory(context->plugin_state);
    if (!SignatureReady(context->signature) || !NamesReady(context->names) ||
        !ObjectsReady(context->objects)) {
        delete context;
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {nullptr, 0}};
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (!plugin_context) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    auto& context = *static_cast<Context*>(plugin_context);
    LoadDoneSet(context);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (!plugin_context) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

void ANOMALY_CALL Update(void* plugin_context, const double) {
    if (!plugin_context) return;
    auto& context = *static_cast<Context*>(plugin_context);
    const auto now = std::chrono::steady_clock::now();

    if (context.read_pending.exchange(false, std::memory_order_acq_rel)) {
        ReadTable(context);
    }

    if (context.landmark_transfer_wait) {
        double position[3]{};
        const bool have_position = SnapshotPlayerPosition(context, position);
        const bool arrived = have_position &&
            PlanarDistanceSquared(position[0], position[1],
                context.landmark_destination[0],
                context.landmark_destination[1]) <=
                kLandmarkArrivalRadiusCentimeters *
                    kLandmarkArrivalRadiusCentimeters;
        if (arrived && context.landmark_arrival_time ==
                           std::chrono::steady_clock::time_point{}) {
            context.landmark_arrival_time = now;
        }
        const bool settled =
            context.landmark_arrival_time !=
                std::chrono::steady_clock::time_point{} &&
            now - context.landmark_arrival_time >=
                std::chrono::milliseconds(static_cast<long long>(
                    kLandmarkSettleSeconds * 1000.0));
        if (settled || now >= context.landmark_deadline) {
            context.landmark_transfer_wait = false;
            context.landmark_arrival_time = std::chrono::steady_clock::time_point{};
            Point p;
            p.x = context.pending_target[0];
            p.y = context.pending_target[1];
            p.z = context.pending_target[2];
            p.row_name = context.current_row;
            if (StartNavigation(context, p, now)) {
                std::lock_guard<std::mutex> lock(context.mutex);
                const std::array nav_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.navigating", "Walking [{0}]", nav_args);
            } else {
                std::lock_guard<std::mutex> lock(context.mutex);
                context.status = context.localizer.Text(
                    "status.navigation_failed", "Walk failed");
            }
        }
    }

    if (context.navigating) {
        double position[3]{};
        const bool have_position = SnapshotPlayerPosition(context, position);
        bool arrived = false;
        if (have_position) {
            const double distance_squared = PlanarDistanceSquared(
                position[0], position[1],
                context.navigation_target[0], context.navigation_target[1]);
            arrived = distance_squared <=
                kArrivalRadiusCentimeters * kArrivalRadiusCentimeters;
        }
        if (now >= context.navigation_progress_check_at) {
            if (have_position) {
                if (context.navigation_has_last_position) {
                    const double moved = std::sqrt(PlanarDistanceSquared(
                        position[0], position[1],
                        context.navigation_last_position[0],
                        context.navigation_last_position[1]));
                    if (moved >= kProgressThresholdCentimeters) {
                        context.navigation_last_progress_at = now;
                        context.navigation_retry_at = now + std::chrono::milliseconds(
                            static_cast<long long>(kReissueDelaySeconds * 1000.0));
                    }
                }
                context.navigation_last_position[0] = position[0];
                context.navigation_last_position[1] = position[1];
                context.navigation_last_position[2] = position[2];
                context.navigation_has_last_position = true;
            }
            context.navigation_progress_check_at = now +
                std::chrono::milliseconds(static_cast<long long>(
                    kProgressCheckIntervalSeconds * 1000.0));
        }
        if (arrived) {
            context.navigation->stop_movement(context.navigation->user);
            context.navigating = false;
            // 到达即进入自动战斗阶段（状态行由模块接管）。
            BeginCombat(context, now);
        } else if (now - context.navigation_last_progress_at >=
                   std::chrono::milliseconds(static_cast<long long>(
                       kMovementTimeoutSeconds * 1000.0))) {
            context.navigation->stop_movement(context.navigation->user);
            context.navigating = false;
            std::lock_guard<std::mutex> lock(context.mutex);
            context.auto_run = false;
            context.status = context.localizer.Text(
                "status.navigation_timeout", "Walk timeout");
        } else {
            if (now >= context.navigation_retry_at) {
                context.navigation_retry_at = now + std::chrono::milliseconds(
                    static_cast<long long>(kReissueDelaySeconds * 1000.0));
                context.navigation->stop_movement(context.navigation->user);
                Point p;
                p.x = context.pending_target[0];
                p.y = context.pending_target[1];
                p.z = context.pending_target[2];
                p.row_name = context.current_row;
                if (!IssueNavigation(context, p)) {
                    context.navigating = false;
                    std::lock_guard<std::mutex> lock(context.mutex);
                    context.auto_run = false;
                    context.status = context.localizer.Text(
                        "status.navigation_failed", "Walk failed");
                }
            }
            if (context.navigating && have_position) {
                const double distance_cm = std::sqrt(PlanarDistanceSquared(
                    position[0], position[1],
                    context.navigation_target[0], context.navigation_target[1]));
                const std::string distance_str =
                    std::to_string(static_cast<long long>(distance_cm));
                const std::array distance_args{
                    std::string_view(distance_str),
                    std::string_view(context.current_row)};
                std::lock_guard<std::mutex> lock(context.mutex);
                context.status = context.localizer.Format(
                    "status.navigating_distance", "Walking {0}cm [{1}]",
                    distance_args);
            }
        }
    }

    if (context.auto_start_pending.exchange(false, std::memory_order_acq_rel)) {
        Point p;
        bool have_point = false;
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            if (!context.filtered_points.empty()) {
                p = context.filtered_points.front();
                context.auto_run = true;
                have_point = true;
            } else {
                context.status = context.localizer.Text("status.no_points", "No points");
            }
        }
        if (have_point) StartPoint(context, p, now);
    }

    if (context.manual_teleport_pending.exchange(false, std::memory_order_acq_rel)) {
        Point p;
        p.x = context.manual_teleport_x;
        p.y = context.manual_teleport_y;
        p.z = context.manual_teleport_z;
        p.row_name = context.manual_teleport_row;
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            // 单点按钮同时也是「从这一点开始跑一轮」的入口。
            context.auto_run = true;
        }
        StartPoint(context, p, now);
    }

    if (context.auto_stop_pending.exchange(false, std::memory_order_acq_rel)) {
        StopRun(context);
    }
    if (context.save_pending.exchange(false, std::memory_order_acq_rel)) {
        SaveDoneSet(context);
    }

    // 传送后的落点确认：位置快照到位就进战斗阶段；超时（传送没生效）也直接开打，
    // 打不到怪由首次接敌超时收场。
    if (context.teleport_arrival_wait) {
        double position[3]{};
        const bool have_position = SnapshotPlayerPosition(context, position);
        const bool arrived = have_position &&
            PlanarDistanceSquared(position[0], position[1],
                context.pending_target[0], context.pending_target[1]) <=
                kArrivalRadiusCentimeters * kArrivalRadiusCentimeters;
        if (arrived || now >= context.teleport_arrival_deadline) {
            context.teleport_arrival_wait = false;
            BeginCombat(context, now);
        }
    }

    // 战斗阶段：本点打完（cleared）或「从未见过怪」超时之前，每 tick 驱动一次模块。
    if (context.combat_active) {
        TickCombat(context, now);
    }
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.combat-box"),
        anomaly::sdk::StringView("打怪资源点"),
        anomaly::sdk::StringView("CCYellowStar"),
        anomaly::sdk::StringView("0.1.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
