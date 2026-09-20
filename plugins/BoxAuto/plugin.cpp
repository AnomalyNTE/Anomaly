#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/localization.h"
#include "plugins/common/localization.hpp"
#include "oracle_stone_profile.hpp"
#include "oracle_stone_runtime.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::string_view kEnvTablePath =
    "/Game/DataTable/Treasurebox/DT_EnvirTreasureBoxConfig.DT_EnvirTreasureBoxConfig";

constexpr std::string_view kRandomItemTablePath =
    "/Game/DataAssets/DataAssetSet/RandomItem/DT_RandomItem.DT_RandomItem";
constexpr std::string_view kBigWorldYaHaHaTablePath =
    "/Game/DataTable/YaHaHa/DT_BigWorldYaHaHaConfig.DT_BigWorldYaHaHaConfig";
constexpr std::string_view kRandomItemDropTablePath =
    "/Game/DataAssets/DataAssetSet/RandomItem/DT_RandomItemDrop.DT_RandomItemDrop";

constexpr std::ptrdiff_t kDataTableRowMapOffset = 0x30;
constexpr std::uint32_t kDataTableRowStride = 24;
constexpr std::uint32_t kDataTableRowPointerOffset = 8;
constexpr std::ptrdiff_t kCoordOffset = 48;
constexpr std::ptrdiff_t kRandomItemCoordOffset = 80;
constexpr std::ptrdiff_t kRandomItemTypeOffset = 32;

constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
constexpr std::string_view kGetRecordOwnerPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 "
    "48 8B 99 60 01 00 00 48 8B E9";
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
constexpr std::ptrdiff_t kUClassDefaultObjectOffset = 272;
constexpr std::ptrdiff_t kUFunctionNumParmsOffset = 180;
constexpr std::ptrdiff_t kUFunctionParmsSizeOffset = 182;
constexpr std::ptrdiff_t kUFunctionFlagsOffset = 176;
constexpr std::uint32_t kFuncNativeFlag = 0x400;
constexpr std::ptrdiff_t kWorldGameInstanceOffset = 560;
constexpr std::ptrdiff_t kGameInstanceLocalPlayersOffset = 56;
constexpr std::ptrdiff_t kLocalPlayerControllerOffset = 48;
constexpr std::ptrdiff_t kControllerPlayerStateOffset = 720;
constexpr std::ptrdiff_t kRecordOwnerFindRecordVtableOffset = 280;
constexpr std::ptrdiff_t kRecordOwnerOffset = 8;
constexpr std::ptrdiff_t kRecordIndexOffset = 16;
constexpr std::ptrdiff_t kRecordDescriptorTableOffset = 24;
constexpr std::uint32_t kRecordDescriptorStride = 40;
constexpr std::ptrdiff_t kRecordDescriptorStoreOffset = 32;
constexpr std::ptrdiff_t kRecordStoreRowsOffset = 32;
constexpr std::ptrdiff_t kRecordGetStringVtableOffset = 224;
constexpr std::uint32_t kMaximumRecordRows = 10000;
constexpr std::size_t kProcessEventVtableIndex = 0x4C;
constexpr std::ptrdiff_t kActorRootComponentOffset = 456;
constexpr std::ptrdiff_t kSceneComponentLocationOffset = 280;
constexpr std::ptrdiff_t kInteractFinishOffset = 976;
constexpr std::int32_t kScanBatchSize = 8192;
constexpr std::size_t kMaximumNameBytes = 1024;

constexpr double kTeleportZOffset = 60.0;
constexpr double kArrivalRadiusCentimeters = 600.0;
constexpr double kMovementTimeoutSeconds = 20.0;
constexpr double kProgressCheckIntervalSeconds = 3.0;
constexpr double kProgressThresholdCentimeters = 80.0;
constexpr double kReissueDelaySeconds = 4.0;
constexpr std::uint32_t kNavigationMaxAttempts = 2;
constexpr double kApproachRadiusCentimeters = 100.0;
constexpr double kFallOutThresholdCentimeters = 1000.0;
constexpr double kScanActorRadiusCentimeters = 1500.0;
constexpr std::string_view kLandmarkWorld = "XL_map_bigworld_test";
constexpr double kLandmarkArrivalRadiusCentimeters = 500.0;
constexpr double kLandmarkTransferTimeoutSeconds = 20.0;
constexpr double kLandmarkSettleSeconds = 2.0;
constexpr std::uint32_t kMaximumMapLandmarks = 4096;
constexpr double kPickupRadiusCentimeters = 600.0;
constexpr double kPickupApproachRadiusCentimeters = 250.0;
constexpr double kTeleportApproachRadiusCentimeters = 120.0;
constexpr double kTargetActorMaxDistanceCentimeters = 400.0;
constexpr std::uint32_t kPickupMaximumItems = 1;
constexpr double kPickupTimeoutSeconds = 8.0;
constexpr std::uint32_t kPickupMaximumRetries = 3;
// How long a scan for the target actor is retried before the point is re-teleported,
// and again after that: one poll a second, so this is also the seconds waited. Both
// pickup paths use it, so a point that is simply not there costs the same either way.
constexpr std::uint32_t kActorLoadWaitSeconds = 7;
constexpr double kFallbackVerifyRadiusCentimeters = 600.0;
constexpr double kShopSafePoint[3]{-129487.089561, 166235.911261, 6708.454053};
constexpr std::string_view kShopExcludedPointB018 = "HTTargetPoint_StealGoods_Item_B_018";
constexpr std::string_view kShopExcludedPointA033 = "HTTargetPoint_StealGoods_Item_A_033";
constexpr std::chrono::seconds kShopSafeRetryDelay{8};
constexpr std::chrono::seconds kShopSafeRetryInterval{2};
constexpr std::chrono::seconds kShopSafeDeadline{60};
constexpr std::uint32_t kShopSafeMaximumTransferAttempts = 3;
constexpr double kShopExitRecoveryDistance = 1800.0;
constexpr std::chrono::milliseconds kShopExitRecoverySettle{1200};
constexpr std::uint32_t kShopExitRecoveryAttempts = 3;
// How many times one shop point may restart its scan before it is skipped. The entry
// retry loop and the still-stealth loop each restart their own counters, so this is the
// only ceiling on the point as a whole.
constexpr std::uint32_t kShopRescanCycles = 3;
// Picks in one region+kind group that mean the region is spent for the week. Measured
// against a live week: A 39, B 42, C 39, D 40, while the regions with nothing spawned sat
// at 0 - so the allowance is 40 and this leaves room for a few failures.
constexpr std::uint32_t kFoodRegionSpentPicks = 35;
// Default for the panel setting below: consecutive food points in one region that turn out
// to have no actor at all before the rest of that region is skipped for this run. Regions
// whose allowance is used up keep all their remaining points in the table, and each one
// costs the whole actor-load wait.
constexpr std::uint32_t kFoodEmptyRegionRunDefault = 3;

struct RawName final {
    std::int32_t comparison_index{};
    std::uint32_t number{};
};

struct Point final {
    std::string row_name;
    double x{};
    double y{};
    double z{};
    std::string category;
};

bool IsExcludedShopPoint(const std::string_view row_name) noexcept {
    return row_name == kShopExcludedPointB018 ||
        row_name == kShopExcludedPointA033;
}

struct MapLandmark final {
    std::uint64_t sequence{};
    std::uint32_t index{};
    double destination[3]{};
    std::string teleport_id;
};

namespace oracle_stone_impl {
struct Context;
struct OracleStoneRecord;
void OracleInitialize(Context& context, const AnomalyHostApiV1* host);
bool OracleScanCatalog(Context& context);
void OracleRefreshStates(Context& context);
void OracleExecuteTeleport(Context& context);
void OracleRunAutoTeleport(Context& context, double delta_seconds);
void OracleQueueTeleport(Context& context, const double position[3]);
}  // namespace oracle_stone_impl

struct Context final {
    const AnomalyHostApiV1* host{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNtePlayerTeleportServiceV1* teleport{};
    const AnomalyNteNavigationServiceV1* navigation{};
    const AnomalyNteMapLandmarksServiceV1* map_landmarks{};
    const AnomalyNtePickupServiceV1* pickup{};
    anomaly::plugins::Localizer localizer;
    std::uintptr_t g_objects_address{};
    std::uintptr_t g_world_address{};
    std::uintptr_t controller{};
    std::uintptr_t controller_class{};
    std::uintptr_t server_interact_fn{};
    std::uintptr_t trigger_interact_fn{};
    std::uintptr_t get_record_owner_address{};
    std::uintptr_t record_owner{};
    std::uintptr_t fixed_record{};
    std::uintptr_t dynamic_record{};
    std::uintptr_t pickup_record{};
    std::unordered_set<std::string> uncollected_points;
    bool uncollected_catalog_valid{};
    std::unordered_set<std::string> picked_up_points;
    bool picked_up_valid{};
    oracle_stone_impl::Context* oracle{};

    std::mutex mutex;
    std::vector<Point> points;
    std::vector<Point> filtered_points;
    std::size_t current_index{};
    std::size_t picked{};
    std::size_t skipped{};
    bool running{};
    bool teleported{};
    bool moving{};
    bool interacted{};
    bool food_approaching{};
    std::chrono::steady_clock::time_point food_approach_deadline{};
    std::chrono::steady_clock::time_point interact_verify_deadline{};
    std::chrono::steady_clock::time_point food_nav_retry_at{};
    bool food_has_last_pos{};
    double food_last_pos[3]{};
    double fallout_baseline_z{};
    std::uint32_t fallout_retries{};
    std::uint32_t retry_count{};
    std::uint32_t interact_retry{};
    std::uint32_t can_interact_retries{};
    std::uint32_t teleport_retry{};
    std::uint32_t navigation_attempt{};
    std::chrono::steady_clock::time_point navigation_last_progress_at{};
    std::chrono::steady_clock::time_point navigation_progress_check_at{};
    std::chrono::steady_clock::time_point navigation_retry_at{};
    double navigation_last_position[3]{};
    bool navigation_has_last_position{};
    std::uint64_t landmarks_sequence{};
    std::vector<MapLandmark> landmarks;
    bool landmark_transfer_attempted{};
    bool landmark_transfer_wait{};
    double landmark_destination[3]{};
    std::chrono::steady_clock::time_point landmark_deadline{};
    std::chrono::steady_clock::time_point landmark_arrival_time{};
    std::uintptr_t target_actor{};
    std::uint8_t interact_baseline{};
    std::unordered_map<std::uintptr_t, std::string> class_name_cache;
    std::unordered_map<std::string, std::unordered_set<std::uint32_t>> class_map;
    std::chrono::steady_clock::time_point due{};
    std::string status;
    std::uint32_t type_choice{0};
    std::uint32_t start_index{0};
    char filter[128]{};
    std::string type_prefix;
    std::atomic_bool read_pending{};
    // Region keys ("Item|A") whose weekly food is spent, and when they were last recomputed.
    std::unordered_set<std::string> food_spent_groups;
    // Regions learned to be empty during this run, and the current run of empty points.
    std::unordered_set<std::string> food_empty_groups;
    // Panel setting: how many empty points in a row mark a region empty. Applies to both
    // kinds of food. Runtime state like the teleport offset, not persisted.
    std::atomic<std::uint32_t> food_empty_region_run{kFoodEmptyRegionRunDefault};
    std::string food_empty_group;
    std::uint32_t food_empty_run{};
    std::chrono::steady_clock::time_point food_state_refresh_at{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* skill_invocation{};
    bool shop_stealth_ready{};
    bool shop_safe_transfer_pending{};
    std::uint32_t shop_safe_transfer_attempts{};
    std::chrono::steady_clock::time_point shop_safe_arrival{};
    std::chrono::steady_clock::time_point shop_safe_deadline{};
    std::chrono::steady_clock::time_point shop_safe_retry_at{};
    std::uint32_t shop_take_retries{};
    std::uint32_t shop_rescan_cycles{};
    bool shop_exit_recovery_active{};
    bool shop_exit_transfer_pending{};
    std::uint32_t shop_exit_transfer_attempts{};
    std::chrono::steady_clock::time_point shop_exit_ready_at{};
    std::atomic_bool begin_pending{};
    std::atomic_bool manual_teleport_pending{};
    std::atomic_bool developer_mode{};
    std::atomic_bool stop_movement_pending{};
    double manual_teleport_x{}, manual_teleport_y{}, manual_teleport_z{};
    std::atomic<double> teleport_z_offset{kTeleportZOffset};
    bool manual_landmark_pending{};
    double manual_landmark_target[3]{};
    bool manual_navigating{};
    std::chrono::steady_clock::time_point manual_nav_last_progress_at{};
    std::chrono::steady_clock::time_point manual_nav_progress_check_at{};
    std::chrono::steady_clock::time_point manual_nav_retry_at{};
    double manual_nav_last_position[3]{};
    bool manual_nav_has_last_position{};
    std::uint64_t pickup_baseline_sequence{};
    bool pickup_started{};
    std::chrono::steady_clock::time_point pickup_deadline{};
    std::uint32_t pickup_retries{};
};

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

bool LandmarksReady(const AnomalyNteMapLandmarksServiceV1* s) noexcept {
    return HasField<AnomalyNteMapLandmarksServiceV1,
               decltype(AnomalyNteMapLandmarksServiceV1::teleport)>(
               s, offsetof(AnomalyNteMapLandmarksServiceV1, teleport)) &&
        s->sequence != nullptr && s->count != nullptr &&
        s->snapshot_at != nullptr && s->teleport != nullptr;
}

bool PickupReady(const AnomalyNtePickupServiceV1* service) noexcept {
    return HasField<AnomalyNtePickupServiceV1,
               decltype(AnomalyNtePickupServiceV1::snapshot)>(
               service, offsetof(AnomalyNtePickupServiceV1, snapshot)) &&
        service->request_nearby != nullptr && service->snapshot != nullptr;
}

bool UsesPickupService(const std::string_view category) noexcept {
    return false;
}

bool IsRelaxedPrefix(const std::string_view prefix) noexcept {
    return prefix == "PropBox_Yahaha" || prefix == "PropBox_Once" ||
        prefix == "Prison" || prefix == "InteractBox" || prefix == "ShopStealGoods_";
}

bool DeveloperModeEnabled(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1,
               decltype(AnomalyUiServiceV1::developer_mode_enabled)>(
               ui, offsetof(AnomalyUiServiceV1, developer_mode_enabled)) &&
        ui->developer_mode_enabled != nullptr &&
        ui->developer_mode_enabled(ui->user) != 0;
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
    context.landmark_arrival_time = std::chrono::steady_clock::time_point{};
    context.landmark_transfer_wait = true;
    context.landmark_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<long long>(
            kLandmarkTransferTimeoutSeconds * 1000.0));
    return true;
}

void ResetShopStealthSession(Context& context) noexcept {
    context.shop_stealth_ready = false;
    context.shop_safe_transfer_pending = false;
    context.shop_safe_transfer_attempts = 0;
    context.shop_safe_arrival = std::chrono::steady_clock::time_point{};
    context.shop_safe_deadline = std::chrono::steady_clock::time_point{};
    context.shop_safe_retry_at = std::chrono::steady_clock::time_point{};
    context.shop_exit_recovery_active = false;
    context.shop_exit_transfer_pending = false;
    context.shop_exit_transfer_attempts = 0;
    context.shop_exit_ready_at = std::chrono::steady_clock::time_point{};
}

void BeginShopExitRecovery(Context& context) noexcept {
    context.shop_stealth_ready = false;
    context.shop_safe_transfer_pending = false;
    context.shop_safe_transfer_attempts = 0;
    context.shop_safe_arrival = std::chrono::steady_clock::time_point{};
    context.shop_safe_deadline = std::chrono::steady_clock::time_point{};
    context.shop_safe_retry_at = std::chrono::steady_clock::time_point{};
    context.shop_exit_recovery_active = true;
    context.shop_exit_transfer_pending = false;
    context.shop_exit_transfer_attempts = 0;
    context.shop_exit_ready_at = std::chrono::steady_clock::time_point{};
}

// Spends one rescan of this point's budget. True means the budget is already gone, so the
// caller skips the point instead of restarting the scan again.
bool ShopRescanBudgetSpent(Context& context) noexcept {
    if (context.shop_rescan_cycles >= kShopRescanCycles) return true;
    ++context.shop_rescan_cycles;
    return false;
}

void ResetPointState(Context& context) noexcept {
    context.teleported = false;
    context.moving = false;
    context.interacted = false;
    context.food_approaching = false;
    context.food_has_last_pos = false;
    context.fallout_baseline_z = 0.0;
    context.fallout_retries = 0;
    context.target_actor = 0;
    context.retry_count = 0;
    context.interact_retry = 0;
    context.can_interact_retries = 0;
    context.teleport_retry = 0;
    context.navigation_attempt = 0;
    context.landmark_transfer_attempted = false;
    context.landmark_arrival_time = std::chrono::steady_clock::time_point{};
    context.landmark_transfer_wait = false;
    context.pickup_started = false;
    context.pickup_deadline = std::chrono::steady_clock::time_point{};
    context.pickup_retries = 0;
    context.interact_verify_deadline = std::chrono::steady_clock::time_point{};
    context.shop_take_retries = 0;
    context.shop_rescan_cycles = 0;
}

// Caller must hold context.mutex.
void ComputeApproachDestination(Context& context, const Point& p,
                                double destination[3]) noexcept {
    destination[0] = p.x;
    destination[1] = p.y;
    destination[2] = p.z;
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
}

// Caller must hold context.mutex.
bool StartNavigation(Context& context, const Point& p,
                     const std::chrono::steady_clock::time_point now) noexcept {
    if (!NavigationReady(context.navigation)) return false;
    double destination[3]{};
    ComputeApproachDestination(context, p, destination);
    if (context.navigation->move_to_location(
            context.navigation->user, destination).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    context.moving = true;
    context.navigation_has_last_position = false;
    context.navigation_last_progress_at = now;
    context.navigation_progress_check_at = now + std::chrono::milliseconds(
        static_cast<long long>(kProgressCheckIntervalSeconds * 1000.0));
    context.navigation_retry_at = now + std::chrono::milliseconds(
        static_cast<long long>(kReissueDelaySeconds * 1000.0));
    return true;
}

// Caller must hold context.mutex.
bool StartManualNavigation(Context& context,
                           const std::chrono::steady_clock::time_point now) noexcept {
    if (!NavigationReady(context.navigation)) return false;
    Point p;
    p.x = context.manual_landmark_target[0];
    p.y = context.manual_landmark_target[1];
    p.z = context.manual_landmark_target[2];
    double destination[3]{};
    ComputeApproachDestination(context, p, destination);
    if (context.navigation->move_to_location(
            context.navigation->user, destination).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    context.manual_navigating = true;
    context.manual_nav_has_last_position = false;
    context.manual_nav_last_progress_at = now;
    context.manual_nav_progress_check_at = now + std::chrono::milliseconds(
        static_cast<long long>(kProgressCheckIntervalSeconds * 1000.0));
    context.manual_nav_retry_at = now + std::chrono::milliseconds(
        static_cast<long long>(kReissueDelaySeconds * 1000.0));
    return true;
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

bool ReadBytes(const void* address, void* destination,
               const std::size_t size) noexcept {
    if (address == nullptr || destination == nullptr || size == 0) return false;
    __try {
        std::memcpy(destination, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadUtf16CString(const void* address, std::string& result) noexcept {
    result.clear();
    if (address == nullptr) return false;
    std::array<char16_t, kMaximumNameBytes / sizeof(char16_t)> value{};
    for (std::size_t offset{}; offset < value.size(); offset += 32U) {
        const auto size = (std::min)(std::size_t{32}, value.size() - offset);
        if (!ReadBytes(
                reinterpret_cast<const std::uint8_t*>(address) +
                    offset * sizeof(char16_t),
                value.data() + offset, size * sizeof(char16_t))) {
            return false;
        }
        const auto begin =
            value.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto end =
            value.begin() + static_cast<std::ptrdiff_t>(offset + size);
        const auto nul = std::find(begin, end, u'\0');
        if (nul != end) {
            for (auto cur = value.begin(); cur != nul; ++cur) {
                const auto cp = static_cast<std::uint32_t>(*cur);
                if (cp <= 0x7FU) {
                    result.push_back(static_cast<char>(cp));
                } else if (cp <= 0x7FFU) {
                    result.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
                    result.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                } else {
                    result.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
                    result.push_back(
                        static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                    result.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                }
            }
            return !result.empty();
        }
    }
    return false;
}

bool ResolveFunction(Context& context, const std::string_view pattern,
                     std::uintptr_t& address) noexcept {
    address = 0;
    if (!SignatureReady(context.signature)) return false;
    std::uintptr_t instruction{};
    if (context.signature->resolve(context.signature->user,
                                   anomaly::sdk::StringView("HTGame.exe"),
                                   anomaly::sdk::StringView(".text"),
                                   anomaly::sdk::StringView(pattern), &instruction)
            .code != ANOMALY_STATUS_V1_OK ||
        instruction == 0) {
        return false;
    }
    address = instruction;
    return true;
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

namespace oracle_stone_impl {

using oracle_stone_locator_profile::kGObjectsPattern;
using oracle_stone_locator_profile::kGWorldPattern;
using oracle_stone_locator_profile::kOracleStoneStateQueryPattern;
using oracle_stone_locator_profile::kRipDisplacementOffset;
using oracle_stone_locator_profile::kRipInstructionSize;
using oracle_stone_locator_profile::kGObjectsAddend;
using oracle_stone_locator_profile::kObjectItemsOffset;
using oracle_stone_locator_profile::kObjectCountOffset;
using oracle_stone_locator_profile::kObjectMaxCountOffset;
using oracle_stone_locator_profile::kObjectMaxChunksOffset;
using oracle_stone_locator_profile::kObjectNumChunksOffset;
using oracle_stone_locator_profile::kObjectChunkSize;
using oracle_stone_locator_profile::kObjectItemStride;
using oracle_stone_locator_profile::kWorldGameInstanceOffset;
using oracle_stone_locator_profile::kGameInstanceLocalPlayersOffset;
using oracle_stone_locator_profile::kLocalPlayerControllerOffset;
using oracle_stone_locator_profile::kControllerPlayerStateOffset;
using oracle_stone_locator_profile::kDataTableRowMapOffset;
using oracle_stone_locator_profile::kDataTableRowMapElementStride;
using oracle_stone_locator_profile::kDataTableRowMapRowOffset;
using oracle_stone_locator_profile::kDataTableRowMapData;
using oracle_stone_locator_profile::kDataTableRowMapNum;
using oracle_stone_locator_profile::kDataTableRowMapMax;
using oracle_stone_locator_profile::kDataTableRowMapInlineFlags;
using oracle_stone_locator_profile::kDataTableRowMapFlagsData;
using oracle_stone_locator_profile::kDataTableRowMapFlagsNum;
using oracle_stone_locator_profile::kDataTableRowMapFlagsMax;
using oracle_stone_locator_profile::kDataTableMaxRows;
using oracle_stone_locator_profile::kOracleStoneDataAssetTableOffset;
using oracle_stone_locator_profile::kOracleStoneLevelOffset;
using oracle_stone_locator_profile::kOracleStoneFloorOffset;
using oracle_stone_locator_profile::kOracleStoneLocationOffset;
using oracle_stone_locator_profile::kOracleStoneMapExploreOffset;
using oracle_stone_locator_profile::kOracleStoneStateContextOffset;
using oracle_stone_locator_profile::kTreasureboxDataAssetPath;

using oracle_stone_locator::OracleStoneUnknown;
using oracle_stone_locator::OracleStoneAvailable;
using oracle_stone_locator::OracleStoneCollected;
using oracle_stone_locator::TranslateOracleStoneState;

constexpr std::size_t kMaximumOracleStones = 4096;
constexpr std::size_t kMaximumStateQueriesPerTick = 64;
constexpr std::uint32_t kMaximumObjectCount = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumObjectChunks = 4096;

struct FNameValue {
    std::uint32_t comparison_index{};
    std::uint32_t number{};
};

struct NativeUtf16StringHeader {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct ObjectRegistry {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
};

struct OracleStoneRecord {
    FNameValue id_name{};
    std::string oracle_stone_id;
    std::string level;
    std::int32_t floor{};
    double world_position[3]{};
    std::uint32_t state{OracleStoneUnknown};
};

struct PendingTeleport {
    bool queued{};
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double position[3]{};
};

struct AutoTeleportState {
    enum class Stage { FindTarget, Outside, Collecting };
    std::atomic_bool enabled{false};
    std::atomic_uint32_t delay_ms{500};
    std::atomic_uint32_t offset_cm{500};
    std::atomic_uint32_t start_index{1};
    Stage stage{Stage::FindTarget};
    double wait_elapsed{};
    double target_position[3]{};
    std::string target_id;
};

struct Context {
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNtePlayerTeleportServiceV1* teleport{};

    std::uintptr_t g_objects_address{};
    std::uintptr_t g_world_address{};
    std::uintptr_t state_query{};
    ObjectRegistry registry{};
    std::uintptr_t treasure_asset{};
    std::uintptr_t data_table{};
    std::uintptr_t player_state{};

    std::mutex mutex;
    std::vector<OracleStoneRecord> records;
    PendingTeleport pending{};
    AutoTeleportState auto_teleport{};
    bool scan_attempted{};
    bool scan_ready{};
    std::uint64_t update_sequence{};
    std::uint64_t next_state_refresh_sequence{};
    std::size_t state_refresh_cursor{};
};

bool OracleResolveRipRelative(const AnomalySignatureServiceV1* signature,
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

std::string OracleRenderFName(const AnomalyUe5NamesServiceV1* names,
                              const FNameValue value) {
    std::string result = ResolveName(names, value.comparison_index);
    if (result.empty() || value.number == 0) return result;
    result.push_back('_');
    result += std::to_string(value.number - 1U);
    return result;
}

bool OracleReadFString(const std::uintptr_t address, std::string& result) {
    NativeUtf16StringHeader header{};
    if (!Read(reinterpret_cast<const void*>(address), header) ||
        header.data == nullptr || header.count <= 0 || header.count > 8192 ||
        header.capacity < header.count) {
        return false;
    }
    return ReadUtf16CString(reinterpret_cast<const void*>(header.data), result);
}

bool OracleRefreshObjectRegistry(Context& context) noexcept {
    if (context.g_objects_address == 0 &&
        !OracleResolveRipRelative(context.signature, kGObjectsPattern,
                                  kGObjectsAddend, context.g_objects_address)) {
        return false;
    }
    ObjectRegistry next{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), next.items) ||
        next.items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), next.count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectMaxCountOffset), next.max_count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectMaxChunksOffset), next.max_chunks) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), next.num_chunks) ||
        next.count == 0 || next.count > kMaximumObjectCount ||
        next.max_count < next.count || next.num_chunks == 0 ||
        next.num_chunks > next.max_chunks ||
        next.num_chunks > kMaximumObjectChunks) {
        return false;
    }
    context.registry = next;
    return true;
}

bool OracleObjectFromHandle(Context& context,
                            const AnomalyGenerationHandleV1 handle,
                            std::uintptr_t& object) noexcept {
    object = 0;
    if (!OracleRefreshObjectRegistry(context)) return false;
    const std::uint32_t index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    const std::uint32_t chunk_index = index / kObjectChunkSize;
    const std::uint32_t within_chunk = index % kObjectChunkSize;
    if (index >= context.registry.count ||
        chunk_index >= context.registry.num_chunks) {
        return false;
    }
    const auto chunk = ReadPointer(reinterpret_cast<const void*>(
        context.registry.items +
        static_cast<std::uintptr_t>(chunk_index) * sizeof(void*)));
    if (!chunk) return false;
    return Read(reinterpret_cast<const void*>(
                    reinterpret_cast<std::uintptr_t>(chunk) +
                    static_cast<std::uintptr_t>(within_chunk) * kObjectItemStride),
                object) &&
        object != 0;
}

bool OracleFindExactObject(Context& context, const std::string_view path,
                           std::uintptr_t& object) noexcept {
    object = 0;
    if (!ObjectsReady(context.objects)) return false;
    AnomalyGenerationHandleV1 handle{};
    if (context.objects->find_exact(
            context.objects->user, anomaly::sdk::StringView(path), &handle)
            .code != ANOMALY_STATUS_V1_OK ||
        handle.id == 0) {
        return false;
    }
    return OracleObjectFromHandle(context, handle, object);
}

bool OracleResolvePlayerState(Context& context) noexcept {
    if (context.player_state != 0) return true;
    if (context.g_world_address == 0 &&
        !OracleResolveRipRelative(context.signature, kGWorldPattern, 0,
                                  context.g_world_address)) {
        return false;
    }
    std::uintptr_t world{};
    std::uintptr_t game_instance{};
    std::uintptr_t local_players{};
    std::uintptr_t local_player{};
    std::uintptr_t controller{};
    if (!Read(reinterpret_cast<const void*>(context.g_world_address), world) ||
        world == 0 ||
        !Read(reinterpret_cast<const void*>(
                  world + kWorldGameInstanceOffset), game_instance) ||
        game_instance == 0 ||
        !Read(reinterpret_cast<const void*>(
                  game_instance + kGameInstanceLocalPlayersOffset), local_players) ||
        local_players == 0 ||
        !Read(reinterpret_cast<const void*>(local_players), local_player) ||
        local_player == 0 ||
        !Read(reinterpret_cast<const void*>(
                  local_player + kLocalPlayerControllerOffset), controller) ||
        controller == 0 ||
        !Read(reinterpret_cast<const void*>(
                  controller + kControllerPlayerStateOffset), context.player_state) ||
        context.player_state == 0) {
        return false;
    }
    return true;
}

bool OracleInvokeStateQuery(const std::uintptr_t function,
                            const std::uintptr_t state_context,
                            const FNameValue& id,
                            std::int32_t& result) noexcept {
    if (function == 0 || state_context == 0) return false;
    using QueryFn = std::int32_t(__fastcall*)(void*, const void*);
    const auto query = reinterpret_cast<QueryFn>(function);
    __try {
        result = query(reinterpret_cast<void*>(state_context), &id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool OracleScanCatalog(Context& context) {
    if (context.scan_attempted) return context.scan_ready;
    if (!OracleFindExactObject(context, kTreasureboxDataAssetPath,
                               context.treasure_asset) ||
        !Read(reinterpret_cast<const void*>(
                  context.treasure_asset + kOracleStoneDataAssetTableOffset),
              context.data_table) ||
        context.data_table == 0) {
        return false;
    }
    std::uintptr_t data{};
    std::int32_t num{};
    std::int32_t max{};
    std::uintptr_t flags_data{};
    std::int32_t flags_num{};
    std::int32_t flags_max{};
    if (!Read(reinterpret_cast<const void*>(
                  context.data_table + kDataTableRowMapOffset + kDataTableRowMapData), data) ||
        !Read(reinterpret_cast<const void*>(
                  context.data_table + kDataTableRowMapOffset + kDataTableRowMapNum), num) ||
        !Read(reinterpret_cast<const void*>(
                  context.data_table + kDataTableRowMapOffset + kDataTableRowMapMax), max) ||
        !Read(reinterpret_cast<const void*>(
                  context.data_table + kDataTableRowMapOffset + kDataTableRowMapFlagsData),
              flags_data) ||
        !Read(reinterpret_cast<const void*>(
                  context.data_table + kDataTableRowMapOffset + kDataTableRowMapFlagsNum),
              flags_num) ||
        !Read(reinterpret_cast<const void*>(
                  context.data_table + kDataTableRowMapOffset + kDataTableRowMapFlagsMax),
              flags_max) ||
        data == 0 || num <= 0 || num > kDataTableMaxRows || max < num ||
        flags_num < num || flags_max < flags_num) {
        return false;
    }
    const auto word_count = static_cast<std::size_t>((flags_num + 31) / 32);
    if (word_count == 0 || word_count > 128) return false;
    std::vector<std::uint32_t> flags(word_count);
    if (flags_data != 0) {
        if (!ReadBytes(reinterpret_cast<const void*>(flags_data), flags.data(),
                       flags.size() * sizeof(std::uint32_t))) {
            return false;
        }
    } else if (word_count > 4 ||
               !ReadBytes(reinterpret_cast<const void*>(
                              context.data_table + kDataTableRowMapOffset +
                              kDataTableRowMapInlineFlags),
                          flags.data(), flags.size() * sizeof(std::uint32_t))) {
        return false;
    }
    std::vector<OracleStoneRecord> discovered;
    discovered.reserve(static_cast<std::size_t>(num));
    for (std::int32_t index{}; index < num; ++index) {
        const std::size_t element_offset =
            static_cast<std::size_t>(index) * kDataTableRowMapElementStride;
        FNameValue id_name{};
        std::uintptr_t row{};
        if (!Read(reinterpret_cast<const void*>(data + element_offset), id_name) ||
            !Read(reinterpret_cast<const void*>(
                      data + element_offset + kDataTableRowMapRowOffset), row) ||
            id_name.comparison_index == 0 || row == 0) {
            continue;
        }
        OracleStoneRecord record;
        record.id_name = id_name;
        record.oracle_stone_id = OracleRenderFName(context.names, id_name);
        if (record.oracle_stone_id.empty()) continue;
        std::string level;
        if (OracleReadFString(row + kOracleStoneLevelOffset, level)) {
            record.level = std::move(level);
        }
        if (!Read(reinterpret_cast<const void*>(row + kOracleStoneFloorOffset),
                  record.floor) ||
            !Read(reinterpret_cast<const void*>(row + kOracleStoneLocationOffset),
                  record.world_position) ||
            !std::isfinite(record.world_position[0]) ||
            !std::isfinite(record.world_position[1]) ||
            !std::isfinite(record.world_position[2])) {
            continue;
        }
        discovered.push_back(std::move(record));
        if (discovered.size() >= kMaximumOracleStones) break;
    }
    if (discovered.empty()) return false;
    std::scoped_lock lock(context.mutex);
    context.records = std::move(discovered);
    context.scan_ready = true;
    context.scan_attempted = true;
    return true;
}

void OracleRefreshStates(Context& context) {
    if (!context.scan_ready) return;
    if (context.state_query == 0) {
        std::uintptr_t fn{};
        if (context.signature->resolve(
                context.signature->user, anomaly::sdk::StringView("HTGame.exe"),
                anomaly::sdk::StringView(".text"),
                anomaly::sdk::StringView(kOracleStoneStateQueryPattern), &fn)
                .code != ANOMALY_STATUS_V1_OK ||
            fn == 0) {
            return;
        }
        context.state_query = fn;
    }
    if (!OracleResolvePlayerState(context)) return;
    if (context.update_sequence < context.next_state_refresh_sequence) return;
    const std::uintptr_t state_context =
        context.player_state + kOracleStoneStateContextOffset;
    std::scoped_lock lock(context.mutex);
    const std::size_t count = context.records.size();
    if (count == 0) {
        context.state_refresh_cursor = 0;
        context.next_state_refresh_sequence = context.update_sequence + 120;
        return;
    }
    const std::size_t end =
        context.state_refresh_cursor < count
            ? (std::min)(count, context.state_refresh_cursor + kMaximumStateQueriesPerTick)
            : count;
    while (context.state_refresh_cursor < end) {
        auto& record = context.records[context.state_refresh_cursor++];
        std::int32_t native_state{};
        if (OracleInvokeStateQuery(context.state_query, state_context,
                                   record.id_name, native_state)) {
            record.state = TranslateOracleStoneState(native_state);
        }
    }
    if (context.state_refresh_cursor >= count) {
        context.state_refresh_cursor = 0;
        context.next_state_refresh_sequence = context.update_sequence + 120;
    }
}

void OracleQueueTeleport(Context& context, const double position[3]) {
    if (context.session == nullptr || context.player == nullptr ||
        context.player->snapshot == nullptr || context.session->snapshot == nullptr) {
        return;
    }
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    if (context.session->snapshot(context.session->user, &session_snapshot).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    if (context.player->snapshot(context.player->user, &player_snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        (player_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) {
        return;
    }
    std::scoped_lock lock(context.mutex);
    context.pending.queued = true;
    context.pending.world = session_snapshot.world;
    context.pending.player = player_snapshot.handle;
    for (std::size_t axis = 0; axis != 3; ++axis) {
        context.pending.position[axis] = position[axis];
    }
}

void OracleExecuteTeleport(Context& context) {
    if (context.teleport == nullptr || context.teleport->teleport == nullptr) return;
    PendingTeleport pending{};
    {
        std::scoped_lock lock(context.mutex);
        if (!context.pending.queued) return;
        pending = context.pending;
        context.pending = {};
    }
    if (pending.world.id == 0 || pending.player.id == 0) return;
    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    request.flags = 0;
    request.world = pending.world;
    request.player = pending.player;
    for (std::size_t axis = 0; axis != 3; ++axis) {
        request.position[axis] = pending.position[axis];
    }
    static_cast<void>(context.teleport->teleport(context.teleport->user, &request));
}

void OracleRunAutoTeleport(Context& context, const double delta_seconds) {
    if (!context.auto_teleport.enabled.load(std::memory_order_acquire)) return;
    switch (context.auto_teleport.stage) {
    case AutoTeleportState::Stage::FindTarget: {
        {
            std::scoped_lock lock(context.mutex);
            if (context.pending.queued) return;
        }
        double target_position[3]{};
        bool found = false;
        {
            std::scoped_lock lock(context.mutex);
            const std::uint32_t start_index =
                context.auto_teleport.start_index.load(std::memory_order_acquire);
            std::uint32_t skipped = 0;
            for (const auto& record : context.records) {
                if (record.state == OracleStoneAvailable) {
                    if (skipped + 1 < start_index) {
                        ++skipped;
                        continue;
                    }
                    for (std::size_t axis = 0; axis != 3; ++axis) {
                        target_position[axis] = record.world_position[axis];
                    }
                    context.auto_teleport.target_id = record.oracle_stone_id;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            context.auto_teleport.enabled.store(false, std::memory_order_release);
            return;
        }
        for (std::size_t axis = 0; axis != 3; ++axis) {
            context.auto_teleport.target_position[axis] = target_position[axis];
        }
        double approach[3]{};
        const double offset = static_cast<double>(
            context.auto_teleport.offset_cm.load(std::memory_order_acquire));
        approach[0] = target_position[0] + offset;
        approach[1] = target_position[1];
        approach[2] = target_position[2];
        OracleQueueTeleport(context, approach);
        context.auto_teleport.wait_elapsed = 0.0;
        context.auto_teleport.stage = AutoTeleportState::Stage::Outside;
        break;
    }
    case AutoTeleportState::Stage::Outside: {
        {
            std::scoped_lock lock(context.mutex);
            if (context.pending.queued) return;
        }
        context.auto_teleport.wait_elapsed += delta_seconds;
        const double delay = static_cast<double>(
            context.auto_teleport.delay_ms.load(std::memory_order_acquire)) / 1000.0;
        if (context.auto_teleport.wait_elapsed < delay) return;
        context.auto_teleport.wait_elapsed = 0.0;
        OracleQueueTeleport(context, context.auto_teleport.target_position);
        context.auto_teleport.stage = AutoTeleportState::Stage::Collecting;
        break;
    }
    case AutoTeleportState::Stage::Collecting: {
        context.auto_teleport.wait_elapsed += delta_seconds;
        const double delay = static_cast<double>(
            context.auto_teleport.delay_ms.load(std::memory_order_acquire)) / 1000.0;
        if (context.auto_teleport.wait_elapsed < delay) return;
        context.auto_teleport.stage = AutoTeleportState::Stage::FindTarget;
        break;
    }
    }
}

void OracleInitialize(Context& context, const AnomalyHostApiV1* host) {
    if (host == nullptr) return;
    const auto view = anomaly::sdk::Host(host);
    context.signature = view.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
    context.names = view.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    context.objects = view.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    context.session = view.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    context.player = view.Query<AnomalyNtePlayerServiceV1>(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    context.teleport = view.Query<AnomalyNtePlayerTeleportServiceV1>(
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION).get();
}

}  // namespace oracle_stone_impl

std::string ObjectName(const AnomalyUe5NamesServiceV1* names,
                       const std::uintptr_t object) noexcept {
    std::uint32_t name_id{};
    if (!Read(reinterpret_cast<const void*>(object + kObjectNameOffset), name_id)) {
        return {};
    }
    return ResolveName(names, name_id);
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

bool WriteUint32(const void* address, const std::uint32_t value) noexcept {
    if (address == nullptr) return false;
    __try {
        *reinterpret_cast<std::uint32_t*>(const_cast<void*>(address)) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeNative(Context& context, void* object, void* function,
                  void* parameters) noexcept {
    if (!object || !function) return false;
    const auto flags_address =
        reinterpret_cast<std::uintptr_t>(function) + kUFunctionFlagsOffset;
    std::uint32_t original_flags{};
    if (!Read(reinterpret_cast<const void*>(flags_address), original_flags)) {
        return false;
    }
    const std::uint32_t invocation_flags = original_flags | kFuncNativeFlag;
    if (!WriteUint32(reinterpret_cast<const void*>(flags_address),
                     invocation_flags)) {
        return false;
    }
    const bool invoked = Invoke(object, function, parameters);
    static_cast<void>(WriteUint32(reinterpret_cast<const void*>(flags_address),
                                  original_flags));
    return invoked;
}

bool FindExactObject(Context& context, const std::string_view path,
                     void*& object) noexcept {
    object = nullptr;
    if (!ObjectsReady(context.objects)) return false;
    AnomalyGenerationHandleV1 handle{};
    if (context.objects->find_exact(
            context.objects->user, anomaly::sdk::StringView(path), &handle)
            .code != ANOMALY_STATUS_V1_OK ||
        handle.id == 0) {
        return false;
    }
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    if (context.g_objects_address == 0 &&
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            context.g_objects_address)) {
        return false;
    }
    object = ObjectAt(context.g_objects_address, index);
    return object != nullptr;
}

constexpr std::string_view CategoryForChoice(const std::uint32_t choice) noexcept {
    switch (choice) {
        case 1: return "hunter";
        case 2: return "character";
        case 3: return "chameleon";
        case 4: return "furniture";
        case 5: return "prop";
        case 6: return "box_food";
        case 7: return "item_food";
        case 8: return "prison";
        case 9: return "wallet";
        case 11: return "shop_steal";
        default: return "";
    }
}

constexpr std::string_view ActorPrefixForChoice(const std::uint32_t choice) noexcept {
    switch (choice) {
        case 1: return "HunterBox_";
        case 2: return "CharacterUpBox_";
        case 3: return "ChameleonBox";
        case 4: return "FurnitureBox";
        case 5: return "PropBox_Geft";
        case 6: return "PropBox_Yahaha";
        case 7: return "PropBox_Once";
        case 8: return "Prison";
        case 9: return "InteractBox";
        case 11: return "ShopStealGoods_";
        default: return "";
    }
}

constexpr std::string_view ActorPrefixForCategory(
    const std::string_view category) noexcept {
    if (category == "hunter") return "HunterBox_";
    if (category == "character") return "CharacterUpBox_";
    if (category == "chameleon") return "ChameleonBox";
    if (category == "furniture") return "FurnitureBox";
    if (category == "prop") return "PropBox_Geft";
    if (category == "box_food") return "PropBox_Yahaha";
    if (category == "item_food") return "PropBox_Once";
    if (category == "prison") return "PropBox_Once";
    if (category == "wallet") return "InteractBox";
    if (category == "shop_steal") return "ShopStealGoods_";
    return "";
}

// Caller must hold context.mutex.
void RebuildFilteredLocked(Context& context) noexcept {
    const std::string_view category = CategoryForChoice(context.type_choice);
    context.filtered_points.clear();
    for (const Point& p : context.points) {
        if (!category.empty() && p.category != category) continue;
        context.filtered_points.push_back(p);
    }
    context.type_prefix = std::string(ActorPrefixForChoice(context.type_choice));
}

void ReadRandomItemTable(Context& context, std::vector<Point>& points) {
    void* table_object{};
    if (!ObjectsReady(context.objects)) return;
    AnomalyGenerationHandleV1 handle{};
    const auto st = context.objects->find_exact(
        context.objects->user, anomaly::sdk::StringView(kRandomItemTablePath), &handle);
    if (st.code != ANOMALY_STATUS_V1_OK || handle.id == 0) return;
    if (context.g_objects_address == 0 &&
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            context.g_objects_address)) {
        return;
    }
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    table_object = ObjectAt(context.g_objects_address, index);
    if (table_object == nullptr) return;
    struct ArrayHeader {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    } header;
    if (!Read(reinterpret_cast<const void*>(
                  reinterpret_cast<std::uintptr_t>(table_object) +
                      kDataTableRowMapOffset),
              header) ||
        header.count <= 0 || header.capacity < header.count || header.data == 0) {
        return;
    }
    for (std::int32_t i = 0; i < header.count; ++i) {
        const auto element =
            header.data + static_cast<std::uintptr_t>(i) * kDataTableRowStride;
        RawName row_id{};
        std::uintptr_t row{};
        if (!Read(reinterpret_cast<const void*>(element), row_id) ||
            row_id.comparison_index == 0) {
            continue;
        }
        if (!Read(reinterpret_cast<const void*>(
                      element + kDataTableRowPointerOffset),
                  row) ||
            row == 0) {
            continue;
        }
        double x{}, y{}, z{};
        Read(reinterpret_cast<const void*>(row + kRandomItemCoordOffset), x);
        Read(reinterpret_cast<const void*>(row + kRandomItemCoordOffset + 8), y);
        Read(reinterpret_cast<const void*>(row + kRandomItemCoordOffset + 16), z);
        if (x == 0.0 && y == 0.0 && z == 0.0) continue;
        std::uint32_t type_id{};
        if (!Read(reinterpret_cast<const void*>(row + kRandomItemTypeOffset),
                  type_id) ||
            type_id == 0) {
            continue;
        }
        const std::string type = ResolveName(context.names, type_id);
        std::string category;
        if (type.find("PropBox_Box") == 0) {
            category = "box_food";
        } else if (type.find("PropBox_Item") == 0) {
            category = "item_food";
        } else if (type.find("Prison") == 0) {
            category = "prison";
        } else if (type.find("InteractBox") == 0) {
            category = "wallet";
        } else if (type.starts_with("ShopSteal_")) {
            category = "shop_steal";
        } else {
            continue;
        }
        Point p;
        p.row_name = ResolveName(
            context.names, static_cast<std::uint32_t>(row_id.comparison_index));
        if (category == "shop_steal" && IsExcludedShopPoint(p.row_name)) continue;
        p.x = x;
        p.y = y;
        p.z = z;
        p.category = category;
        if (context.picked_up_valid &&
            (category == "box_food" || category == "item_food" ||
             category == "prison")) {
            if (context.picked_up_points.find(p.row_name) !=
                context.picked_up_points.end()) {
                continue;
            }
        }
        if ((category == "wallet" || category == "shop_steal") && context.uncollected_catalog_valid) {
            if (context.uncollected_points.find(p.row_name) ==
                context.uncollected_points.end()) {
                continue;
            }
        }
        points.push_back(std::move(p));
    }
}

void ReadTable(Context& context) {
    std::vector<Point> points;
    void* table_object{};
    if (!ObjectsReady(context.objects)) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = "读表失败：objects 服务不可用";
        return;
    }
    AnomalyGenerationHandleV1 handle{};
    const auto st = context.objects->find_exact(
        context.objects->user, anomaly::sdk::StringView(kEnvTablePath), &handle);
    if (st.code != ANOMALY_STATUS_V1_OK || handle.id == 0) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = "读表失败：find_exact code=" + std::to_string(st.code);
        return;
    }
    if (context.g_objects_address == 0 &&
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            context.g_objects_address)) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = "读表失败：GObjects 解析失败";
        return;
    }
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    table_object = ObjectAt(context.g_objects_address, index);
    if (table_object == nullptr) {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.status = "读表失败：ObjectAt 返回空 handle.id=" +
            std::to_string(handle.id);
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
        context.status = "读表失败：RowMap 读取失败";
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
        if (p.row_name.find("HunterBox_") != std::string::npos) {
            p.category = "hunter";
        } else if (p.row_name.find("CharacterUpBox_") != std::string::npos) {
            p.category = "character";
        } else if (p.row_name.find("ChameleonBox") != std::string::npos) {
            p.category = "chameleon";
        } else if (p.row_name.find("FurnitureBox") != std::string::npos) {
            p.category = "furniture";
        } else if (p.row_name.find("PropBox_Geft") != std::string::npos) {
            p.category = "prop";
        }
        points.push_back(std::move(p));
    }
    ReadRandomItemTable(context, points);
    std::lock_guard<std::mutex> lock(context.mutex);
    context.points = std::move(points);
    RebuildFilteredLocked(context);
    const std::string count_str = std::to_string(context.points.size());
    const std::array read_args{std::string_view(count_str)};
    context.status =
        context.localizer.Format("status.read", "Read {0} points", read_args);
}

bool ResolvePlayerState(Context& context, std::uintptr_t& player_state) noexcept {
    player_state = 0;
    if (context.controller == 0) return false;
    return Read(reinterpret_cast<const void*>(
                    context.controller + kControllerPlayerStateOffset),
                player_state) &&
        player_state != 0;
}

void* CallGetRecordOwner(std::uintptr_t fn, std::uintptr_t player_state) noexcept {
    using GetRecordOwner = void* (*)(void*);
    __try {
        return reinterpret_cast<GetRecordOwner>(fn)(
            reinterpret_cast<void*>(player_state));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* CallFindRecord(std::uintptr_t fn, void* owner, const char* name) noexcept {
    using FindRecord = void* (*)(void*, const char*);
    __try {
        return reinterpret_cast<FindRecord>(fn)(owner, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const char* CallGetString(std::uintptr_t fn, void* record, std::int32_t row,
                          std::int32_t index) noexcept {
    using GetString = const char* (*)(void*, std::int32_t, std::int32_t);
    __try {
        return reinterpret_cast<GetString>(fn)(record, row, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ResolveRandomItemRecords(Context& context) noexcept {
    if (context.record_owner != 0 && context.fixed_record != 0 &&
        context.dynamic_record != 0) {
        return true;
    }
    if (context.get_record_owner_address == 0 &&
        !ResolveFunction(context, kGetRecordOwnerPattern,
                         context.get_record_owner_address)) {
        return false;
    }
    std::uintptr_t player_state{};
    if (!ResolvePlayerState(context, player_state)) return false;
    void* owner = CallGetRecordOwner(context.get_record_owner_address, player_state);
    if (owner == nullptr) return false;
    const auto owner_address = reinterpret_cast<std::uintptr_t>(owner);
    std::uintptr_t owner_vtable{};
    std::uintptr_t find_record_address{};
    if (!Read(reinterpret_cast<const void*>(owner_address), owner_vtable) ||
        owner_vtable == 0 ||
        !Read(reinterpret_cast<const void*>(
                  owner_vtable + kRecordOwnerFindRecordVtableOffset),
              find_record_address) ||
        find_record_address == 0) {
        return false;
    }
    void* fixed = CallFindRecord(find_record_address, owner, "RandomItemFixedRecord");
    void* dynamic = CallFindRecord(find_record_address, owner, "RandomItemDynamicRecord");
    void* pickup = CallFindRecord(find_record_address, owner, "RandomItemPickUpRecord");
    context.record_owner = owner_address;
    context.fixed_record = reinterpret_cast<std::uintptr_t>(fixed);
    context.dynamic_record = reinterpret_cast<std::uintptr_t>(dynamic);
    context.pickup_record = reinterpret_cast<std::uintptr_t>(pickup);
    return context.fixed_record != 0 && context.dynamic_record != 0;
}

bool ReadRecordSelections(const std::uintptr_t record,
                          std::unordered_set<std::string>& points) noexcept {
    if (record == 0) return false;
    std::uintptr_t owner{};
    std::int32_t record_index{};
    if (!Read(reinterpret_cast<const void*>(record + kRecordOwnerOffset), owner) ||
        owner == 0 ||
        !Read(reinterpret_cast<const void*>(record + kRecordIndexOffset),
              record_index) ||
        record_index < 0 || record_index > 4096) {
        return false;
    }
    std::uintptr_t descriptors{};
    if (!Read(reinterpret_cast<const void*>(
                  owner + kRecordDescriptorTableOffset), descriptors) ||
        descriptors == 0) {
        return false;
    }
    const std::uintptr_t descriptor =
        descriptors +
        static_cast<std::uintptr_t>(record_index) * kRecordDescriptorStride;
    std::uintptr_t store{};
    if (!Read(reinterpret_cast<const void*>(
                  descriptor + kRecordDescriptorStoreOffset), store) ||
        store == 0) {
        return false;
    }
    struct RecordArrayHeader {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    } rows;
    if (!Read(reinterpret_cast<const void*>(store + kRecordStoreRowsOffset),
              rows) ||
        rows.count < 0 ||
        rows.count > static_cast<std::int32_t>(kMaximumRecordRows) ||
        rows.capacity < rows.count ||
        rows.capacity > static_cast<std::int32_t>(kMaximumRecordRows) ||
        (rows.count != 0 && rows.data == 0)) {
        return false;
    }
    if (rows.count == 0) return true;
    std::vector<std::uintptr_t> row_objects(static_cast<std::size_t>(rows.count));
    if (!ReadBytes(reinterpret_cast<const void*>(rows.data), row_objects.data(),
                   row_objects.size() * sizeof(std::uintptr_t))) {
        return false;
    }
    std::uintptr_t vtable{};
    std::uintptr_t get_string_address{};
    if (!Read(reinterpret_cast<const void*>(record), vtable) || vtable == 0 ||
        !Read(reinterpret_cast<const void*>(
                  vtable + kRecordGetStringVtableOffset), get_string_address) ||
        get_string_address == 0) {
        return false;
    }
    for (std::int32_t row{}; row < rows.count; ++row) {
        if (row_objects[static_cast<std::size_t>(row)] == 0) continue;
        const char* point_value = CallGetString(
            get_string_address, reinterpret_cast<void*>(record), row, 0);
        if (point_value == nullptr) continue;
        std::string point_name;
        if (ReadUtf16CString(point_value, point_name) && !point_name.empty()) {
            points.insert(std::move(point_name));
        }
    }
    return true;
}

// The row name carries the region and the kind: HTTargetPoint_PropBox_{Box|Item}_{REGION}_NNN.
// The key ("Item|A") is what says whether a whole region is spent for the week.
bool FoodGroupOf(const std::string_view name, std::string& key) {
    constexpr std::string_view prefix = "HTTargetPoint_PropBox_";
    if (!name.starts_with(prefix)) return false;
    const std::string_view rest = name.substr(prefix.size());
    const std::size_t kind_end = rest.find('_');
    if (kind_end == std::string_view::npos) return false;
    const std::string_view kind = rest.substr(0, kind_end);
    if (kind != "Box" && kind != "Item") return false;
    std::string_view region = rest.substr(kind_end + 1);
    const std::size_t number_start = region.find_last_of('_');
    if (number_start != std::string_view::npos) {
        const std::string_view number = region.substr(number_start + 1);
        bool digits = !number.empty();
        for (const char c : number) {
            if (c < '0' || c > '9') digits = false;
        }
        if (digits) region = region.substr(0, number_start);
    }
    if (region.empty()) return false;
    key.assign(kind);
    key.push_back('|');
    key.append(region);
    return true;
}

// One food point in this region had no actor at all: enough of those in a row and the rest
// of the region is skipped for this run.
void NoteFoodPointEmpty(Context& context, const std::string& name) noexcept {
    std::string key;
    if (!FoodGroupOf(name, key)) return;
    if (key != context.food_empty_group) {
        context.food_empty_group = key;
        context.food_empty_run = 0;
    }
    const std::uint32_t threshold =
        context.food_empty_region_run.load(std::memory_order_relaxed);
    if (threshold > 0 && context.food_empty_run >= threshold)
        context.food_empty_groups.insert(key);
}

// A point in this region had something in it, so the region is not empty after all.
void NoteFoodPointFound(Context& context, const std::string& name) noexcept {
    std::string key;
    if (!FoodGroupOf(name, key) || key != context.food_empty_group) return;
    context.food_empty_group.clear();
    context.food_empty_run = 0;
}

// A region counts as spent once the game has recorded enough picks there. The picked
// record is the only distance-independent signal: the available set holds what is near the
// player, and the region being visited is by definition the near one, so it always shows
// entries for a region that has just filled up.
void RefreshFoodRegionState(Context& context) noexcept {
    std::unordered_map<std::string, int> picked;
    std::string key;
    for (const auto& name : context.picked_up_points) {
        if (FoodGroupOf(name, key)) ++picked[key];
    }
    context.food_spent_groups.clear();
    for (const auto& entry : picked) {
        if (entry.second >= static_cast<int>(kFoodRegionSpentPicks))
            context.food_spent_groups.insert(entry.first);
    }
}

bool RefreshUncollectedCatalog(Context& context) noexcept {
    if (!ResolveRandomItemRecords(context)) return false;
    context.uncollected_points.clear();
    ReadRecordSelections(context.fixed_record,
                         context.uncollected_points);
    ReadRecordSelections(context.dynamic_record,
                         context.uncollected_points);
    context.uncollected_catalog_valid = true;
    return true;
}

bool RefreshPickedUpCatalog(Context& context) noexcept {
    if (!ResolveRandomItemRecords(context)) return false;
    context.picked_up_points.clear();
    if (context.pickup_record == 0) {
        context.picked_up_valid = false;
        return false;
    }
    ReadRecordSelections(context.pickup_record,
                         context.picked_up_points);
    context.picked_up_valid = true;
    return true;
}

bool GetPlayerController(Context& context) noexcept {
    if (context.controller != 0) return true;
    if (context.g_world_address == 0 &&
        !ResolveRipRelative(context, kGWorldPattern, 0, context.g_world_address)) {
        return false;
    }
    std::uintptr_t world{};
    if (!Read(reinterpret_cast<const void*>(context.g_world_address), world) ||
        world == 0) {
        return false;
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
    std::uintptr_t controller_class{};
    if (!Read(reinterpret_cast<const void*>(controller + kObjectClassOffset),
              controller_class) || controller_class == 0) {
        return false;
    }
    std::uintptr_t fn{};
    if (!FindFunction(context.names, controller_class, "ServerInteract",
                      2, 12, fn)) {
        return false;
    }
    context.controller = controller;
    context.controller_class = controller_class;
    context.server_interact_fn = fn;
    std::uintptr_t trigger_fn{};
    if (FindFunction(context.names, controller_class, "TriggerInteract",
                     3, 13, trigger_fn)) {
        context.trigger_interact_fn = trigger_fn;
    }
    return true;
}









void BuildClassMap(Context& context) noexcept {
    if (context.g_objects_address == 0) return;
    std::int32_t count{};
    std::int32_t num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) {
        return;
    }
    static const char* kPrefixes[] = {"HunterBox_", "CharacterUpBox_",
        "ChameleonBox", "FurnitureBox", "PropBox_Geft", "PropBox_Yahaha",
        "PropBox_Once", "Prison", "InteractBox", "ShopStealGoods_"};
    std::unordered_map<std::string, std::unordered_set<std::uint32_t>> class_map;
    std::unordered_map<std::uintptr_t, std::string> cache;
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto chunk_index = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (chunk_index != cur_chunk) {
            cur_chunk = chunk_index;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(chunk_index) * sizeof(void*)), chunk) ||
                chunk == 0) {
                continue;
            }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride), object) ||
            object == 0) {
            continue;
        }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) ||
            cls == 0) {
            continue;
        }
        std::uint32_t name_id{};
        if (!Read(reinterpret_cast<const void*>(cls + kObjectNameOffset), name_id) ||
            name_id == 0) {
            continue;
        }
        std::string cls_name;
        const auto it = cache.find(cls);
        if (it != cache.end()) {
            cls_name = it->second;
        } else {
            cls_name = ObjectName(context.names, cls);
            cache[cls] = cls_name;
        }
        for (const char* prefix : kPrefixes) {
            if (cls_name.find(prefix) != std::string::npos) {
                class_map[prefix].insert(name_id);
                class_map[""].insert(name_id);
                break;
            }
        }
    }
    context.class_map = std::move(class_map);
    context.class_name_cache = std::move(cache);
}

std::uintptr_t ScanForActor(Context& context, const Point& p,
                           const std::string_view type_prefix) noexcept {
    if (context.g_objects_address == 0 &&
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            context.g_objects_address)) {
        return 0;
    }
    std::int32_t count{};
    std::int32_t num_chunks{};
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
    auto map_it = context.class_map.find(std::string(type_prefix));
    if (map_it == context.class_map.end()) {
        map_it = context.class_map
                     .emplace(std::string(type_prefix),
                              std::unordered_set<std::uint32_t>{})
                     .first;
    }
    std::uint32_t current_chunk_index = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    std::uintptr_t best{};
    double best_dist = 1e30;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto chunk_index = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (chunk_index != current_chunk_index) {
            current_chunk_index = chunk_index;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(chunk_index) * sizeof(void*)), chunk) ||
                chunk == 0) {
                continue;
            }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride), object) ||
            object == 0) {
            continue;
        }
        std::uintptr_t cls{};
        if (!Read(reinterpret_cast<const void*>(object + kObjectClassOffset), cls) ||
            cls == 0) {
            continue;
        }
        std::uint32_t cls_name_id{};
        if (!Read(reinterpret_cast<const void*>(cls + kObjectNameOffset), cls_name_id)) {
            continue;
        }
        if (!map_it->second.contains(cls_name_id)) {
            std::string cls_name;
            const auto cit = context.class_name_cache.find(cls);
            if (cit != context.class_name_cache.end()) {
                cls_name = cit->second;
            } else {
                cls_name = ObjectName(context.names, cls);
                context.class_name_cache[cls] = cls_name;
            }
            if (!type_prefix.empty() && cls_name.find(type_prefix) == std::string::npos) continue;
            if (type_prefix.empty() && cls_name.find("Box") == std::string::npos) continue;
            map_it->second.insert(cls_name_id);
        }
        std::uintptr_t cdo{};
        if (Read(reinterpret_cast<const void*>(cls + kUClassDefaultObjectOffset), cdo) &&
            cdo == object) {
            continue;
        }
        const std::string obj_name = ObjectName(context.names, object);
        const bool is_uaid = obj_name.find("UAID") != std::string::npos;
        if (!is_uaid) {
            if (!IsRelaxedPrefix(type_prefix)) continue;
            std::string cls_name;
            const auto cit = context.class_name_cache.find(cls);
            if (cit != context.class_name_cache.end()) {
                cls_name = cit->second;
            } else {
                cls_name = ObjectName(context.names, cls);
                context.class_name_cache[cls] = cls_name;
            }
            const bool world_actor_class =
                cls_name.size() >= 2 && cls_name[cls_name.size() - 2] == '_' &&
                (cls_name[cls_name.size() - 1] == 'C' ||
                 cls_name[cls_name.size() - 1] == 'c');
            if (!world_actor_class) continue;
        }
        std::uintptr_t root{};
        if (!Read(reinterpret_cast<const void*>(object + kActorRootComponentOffset), root) ||
            root == 0) {
            continue;
        }
        double bx{}, by{}, bz{};
        if (!Read(reinterpret_cast<const void*>(root + kSceneComponentLocationOffset), bx) ||
            !Read(reinterpret_cast<const void*>(root + kSceneComponentLocationOffset + 8), by) ||
            !Read(reinterpret_cast<const void*>(root + kSceneComponentLocationOffset + 16), bz)) {
            continue;
        }
        const double dx = bx - p.x;
        const double dy = by - p.y;
        const double dz = bz - p.z;
        const double dist = dx * dx + dy * dy + dz * dz;
        if (dist < best_dist) {
            best = object;
            best_dist = dist;
        }
    }
    if (best != 0 &&
        best_dist > kScanActorRadiusCentimeters * kScanActorRadiusCentimeters) {
        return 0;
    }
    return best;
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
    request.position[2] = p.z + context.teleport_z_offset.load(std::memory_order_relaxed);
    return context.teleport->teleport(context.teleport->user, &request).code ==
        ANOMALY_STATUS_V1_OK;
}

bool HasShopStealthSkill(Context& context);

void Begin(Context& context) {
    std::lock_guard<std::mutex> lock(context.mutex);
    RebuildFilteredLocked(context);
    context.current_index = context.start_index < context.filtered_points.size()
        ? context.start_index : 0;
    context.picked = 0;
    context.food_empty_groups.clear();
    context.food_empty_group.clear();
    context.food_empty_run = 0;
    context.skipped = 0;
    context.running = true;
    ResetPointState(context);
    ResetShopStealthSession(context);
    if (CategoryForChoice(context.type_choice) == "shop_steal" &&
        !HasShopStealthSkill(context)) {
        context.running = false;
        context.status = context.localizer.Text(
            "status.shop_requires_zankou", "商店偷取需要使用残虹");
        return;
    }
    context.status = context.localizer.Text("status.started", "Started");
}

void Stop(Context& context) {
    std::lock_guard<std::mutex> lock(context.mutex);
    context.running = false;
    context.stop_movement_pending.store(true, std::memory_order_release);
    context.status = context.localizer.Text("status.stopped", "Stopped");
}

struct InteractArrayHeader final {
    std::uintptr_t data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

bool ReadInteractChoices(Context& context, const std::uintptr_t actor,
                         const std::uintptr_t controller,
                         const std::uintptr_t fn,
                         std::vector<std::int32_t>& choices) noexcept {
    std::uint8_t params[24]{};
    std::memcpy(params + 0, &controller, sizeof(controller));
    if (!Invoke(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(fn),
                params)) {
        return false;
    }
    InteractArrayHeader entries;
    std::memcpy(&entries, params + 8, sizeof(entries));
    if (entries.count <= 0 || entries.data == 0 || entries.count > 128 ||
        entries.capacity < entries.count) {
        return true;
    }
    for (std::int32_t i = 0; i < entries.count; ++i) {
        std::int32_t index{};
        if (!Read(reinterpret_cast<const void*>(
                      entries.data + static_cast<std::uintptr_t>(i) * 360),
                  index)) {
            return false;
        }
        choices.push_back(index);
    }
    return true;
}

bool CanTryInteract(Context& context, const std::uintptr_t actor,
                    const std::uintptr_t controller, const std::int32_t index,
                    const std::uintptr_t fn, bool& can_try) noexcept {
    std::uint8_t params[13]{};
    std::memcpy(params + 0, &controller, sizeof(controller));
    std::memcpy(params + 8, &index, sizeof(index));
    if (!Invoke(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(fn),
                params)) {
        return false;
    }
    can_try = params[12] != 0;
    return true;
}

// Why an interact attempt did or did not fire, and how many entries the game offered.
// The shop path logs it: a `can_try` count that climbs over the retries means the entry is
// armed late, while a flat zero means the standing spot never became valid.
struct InteractProbe final {
    const char* reason{"ok"};
    int entries{};
    int can_try{};
};

bool TriggerInteractPickup(Context& context, const std::uintptr_t actor,
                           InteractProbe* probe = nullptr) noexcept {
    if (context.trigger_interact_fn == 0 || actor == 0) {
        if (probe != nullptr) probe->reason = "no-actor-or-trigger";
        return false;
    }
    std::uintptr_t cls{};
    if (!Read(reinterpret_cast<const void*>(actor + kObjectClassOffset), cls) ||
        cls == 0) {
        if (probe != nullptr) probe->reason = "class-read-failed";
        return false;
    }
    std::uintptr_t entries_fn{};
    std::uintptr_t can_try_fn{};
    if (!FindFunction(context.names, cls, "BPGetInteractEntries", 2, 24,
                      entries_fn) ||
        !FindFunction(context.names, cls, "BPCanTryInteract", 3, 13,
                      can_try_fn)) {
        if (probe != nullptr) probe->reason = "interact-functions-missing";
        return false;
    }
    std::vector<std::int32_t> choices;
    if (!ReadInteractChoices(context, actor, context.controller, entries_fn,
                             choices)) {
        if (probe != nullptr) probe->reason = "entries-call-failed";
        return false;
    }
    if (probe != nullptr) probe->entries = static_cast<int>(choices.size());
    if (choices.empty()) {
        if (probe != nullptr) probe->reason = "no-entries";
        return false;
    }
    for (const std::int32_t choice : choices) {
        bool can_try{};
        if (!CanTryInteract(context, actor, context.controller, choice,
                            can_try_fn, can_try) ||
            !can_try) {
            continue;
        }
        if (probe != nullptr) ++probe->can_try;
        std::uint8_t params[13]{};
        std::memcpy(params + 0, &actor, sizeof(actor));
        std::memcpy(params + 8, &choice, sizeof(choice));
        params[12] = 0;
        if (InvokeNative(context, reinterpret_cast<void*>(context.controller),
                         reinterpret_cast<void*>(context.trigger_interact_fn),
                         params)) {
            if (probe != nullptr) probe->reason = "fired";
            return true;
        }
    }
    if (probe != nullptr && probe->can_try == 0) probe->reason = "no-can-try";
    else if (probe != nullptr) probe->reason = "trigger-refused";
    return false;
}

void LogShop(Context& context, const std::string& message) {
    const auto* core = anomaly::sdk::Host(context.host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID, 1).get();
    if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView("shop-collect " + message));
}

bool HasShopStealthSkill(Context& context) {
    if (!context.skills || !context.skills->frame || !context.skills->snapshot_at ||
        !context.skills->ability_path_utf8) return false;
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        (frame.flags & ANOMALY_NTE_SKILL_V1_VALID) == 0 || frame.skill_count > 256) return false;
    for (std::uint32_t i = 0; i < frame.skill_count; ++i) {
        AnomalyNteSkillSnapshotV1 skill{sizeof(skill)};
        if (context.skills->snapshot_at(context.skills->user, frame.generation, i, &skill).code != ANOMALY_STATUS_V1_OK ||
            (skill.flags & ANOMALY_NTE_SKILL_V1_VALID) == 0 ||
            skill.character.id != frame.character.id || skill.character.generation != frame.character.generation) continue;
        std::array<char, 1024> path{};
        std::size_t size = path.size();
        if (context.skills->ability_path_utf8(context.skills->user, skill.ability_class,
                                               path.data(), &size).code == ANOMALY_STATUS_V1_OK &&
            size > 0 && size <= path.size() && path[size - 1] == '\0' &&
            std::string_view(path.data(), size - 1).ends_with(
                "/GA_Zankou_InvisibleSkill.GA_Zankou_InvisibleSkill_C")) return true;
    }
    return false;
}

bool RequestShopStealth(Context& context) {
    if (!context.skills || !context.skill_invocation || !context.session || !context.session->snapshot ||
        !context.skills->frame || !context.skills->snapshot_at || !context.skills->ability_path_utf8 ||
        !context.skill_invocation->activate) return false;
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    AnomalyNteSessionSnapshotV1 session{sizeof(session)};
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        (frame.flags & ANOMALY_NTE_SKILL_V1_VALID) == 0 || frame.skill_count > 256 ||
        context.session->snapshot(context.session->user, &session).code != ANOMALY_STATUS_V1_OK || !session.world.id) return false;
    for (std::uint32_t i = 0; i < frame.skill_count; ++i) {
        AnomalyNteSkillSnapshotV1 skill{sizeof(skill)};
        if (context.skills->snapshot_at(context.skills->user, frame.generation, i, &skill).code != ANOMALY_STATUS_V1_OK ||
            (skill.flags & ANOMALY_NTE_SKILL_V1_VALID) == 0 || skill.character.id != frame.character.id ||
            skill.character.generation != frame.character.generation) continue;
        std::array<char, 1024> path{};
        std::size_t size = path.size();
        if (context.skills->ability_path_utf8(context.skills->user, skill.ability_class, path.data(), &size).code != ANOMALY_STATUS_V1_OK ||
            size == 0 || size > path.size() || path[size - 1] != '\0' ||
            !std::string_view(path.data(), size - 1).ends_with("/GA_Zankou_InvisibleSkill.GA_Zankou_InvisibleSkill_C")) continue;
        if ((skill.flags & ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID) != 0 && skill.cooldown_remaining_seconds > 0.0F) return false;
        AnomalyNteSkillInvocationRequestV1 request{sizeof(request)};
        request.world = session.world;
        request.character = frame.character;
        request.skill = skill.handle;
        AnomalyNteSkillInvocationResultV1 result{sizeof(result)};
        const auto status = context.skill_invocation->activate(context.skill_invocation->user, &request, &result);
        if (status.code == ANOMALY_STATUS_V1_OK && result.accepted != 0) {
            LogShop(context, "fixed safe-point invisibility accepted");
            return true;
        }
        LogShop(context, "fixed safe-point invisibility rejected code=" + std::to_string(status.code) +
            " accepted=" + std::to_string(result.accepted));
        return false;
    }
    return false;
}

bool StartShopExitTransfer(Context& context, const Point& target,
                           const std::chrono::steady_clock::time_point now) {
    if (context.shop_exit_transfer_attempts >= kShopExitRecoveryAttempts) {
        return false;
    }
    double position[3]{};
    if (!SnapshotPlayerPosition(context, position)) return false;
    double dx = position[0] - target.x;
    double dy = position[1] - target.y;
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 100.0) {
        dx = 1.0;
        dy = 0.0;
    } else {
        dx /= length;
        dy /= length;
    }
    const auto attempt = context.shop_exit_transfer_attempts;
    if (attempt == 1) {
        std::swap(dx, dy);
        dy = -dy;
    } else if (attempt == 2) {
        dx = -dx;
        dy = -dy;
    }
    Point exit{"shop-exit-recovery", position[0] + dx * kShopExitRecoveryDistance,
               position[1] + dy * kShopExitRecoveryDistance, position[2],
               "shop_steal"};
    if (!Teleport(context, exit)) return false;
    ++context.shop_exit_transfer_attempts;
    context.shop_exit_transfer_pending = true;
    context.shop_exit_ready_at = now + kShopExitRecoverySettle;
    context.teleported = false;
    context.target_actor = 0;
    context.status = "商店拿取：移到店外恢复隐身";
    LogShop(context, "shop exit transfer attempt=" +
        std::to_string(context.shop_exit_transfer_attempts));
    return true;
}

bool PrepareShopStealth(Context& context, const Point& target, const std::chrono::steady_clock::time_point now) {
    if (context.shop_exit_recovery_active) {
        if (context.shop_exit_transfer_pending) {
            if (now < context.shop_exit_ready_at) {
                context.due = now + std::chrono::milliseconds(200);
                return false;
            }
            context.shop_exit_transfer_pending = false;
            if (RequestShopStealth(context)) {
                context.shop_exit_recovery_active = false;
                context.shop_exit_transfer_attempts = 0;
                context.shop_exit_ready_at = {};
                context.shop_stealth_ready = true;
                context.teleported = false;
                context.due = now + std::chrono::seconds(1);
                context.status = "商店拿取：店外隐身成功，返回目标";
                return false;
            }
        }
        if (StartShopExitTransfer(context, target, now)) {
            context.due = now + std::chrono::milliseconds(200);
            return false;
        }
        context.shop_exit_recovery_active = false;
        context.shop_exit_transfer_pending = false;
        context.shop_exit_ready_at = {};
        context.status = "商店拿取：店外恢复失败，前往固定安全点";
    }
    if (context.shop_safe_transfer_pending) {
        double position[3]{};
        const bool arrived = SnapshotPlayerPosition(context, position) &&
            PlanarDistanceSquared(position[0], position[1], kShopSafePoint[0], kShopSafePoint[1]) <= 500.0 * 500.0 &&
            std::abs(position[2] - (kShopSafePoint[2] +
                context.teleport_z_offset.load(std::memory_order_relaxed))) <= 1500.0;
        if (arrived && context.shop_safe_arrival == std::chrono::steady_clock::time_point{}) {
            context.shop_safe_arrival = now;
            context.shop_safe_deadline = now + kShopSafeDeadline;
            LogShop(context, "fixed safe point arrived; waiting for character load");
        }
        if (!arrived && now >= context.shop_safe_retry_at) {
            if (context.shop_safe_transfer_attempts >= kShopSafeMaximumTransferAttempts) {
                context.running = false;
                context.shop_safe_transfer_pending = false;
                context.stop_movement_pending = true;
                context.status = "商店拿取：固定安全点重传次数用尽，已停止";
                context.due = now + std::chrono::seconds(1);
                return false;
            }
            Point safe{"fixed-shop-safe", kShopSafePoint[0], kShopSafePoint[1], kShopSafePoint[2], "shop_steal"};
            if (Teleport(context, safe)) {
                ++context.shop_safe_transfer_attempts;
                context.shop_safe_retry_at = now + kShopSafeRetryDelay;
                context.shop_safe_deadline = now + kShopSafeDeadline;
                context.status = "商店拿取：安全点未加载，正在重传";
                LogShop(context, "fixed safe point transfer retry=" + std::to_string(context.shop_safe_transfer_attempts));
            } else {
                context.shop_safe_retry_at = now + kShopSafeRetryInterval;
            }
        }
        if (!arrived || now - context.shop_safe_arrival < std::chrono::seconds(2)) {
            if (now >= context.shop_safe_deadline) {
                context.running = false;
                context.shop_safe_transfer_pending = false;
                context.stop_movement_pending = true;
                context.status = "商店拿取：固定安全点加载超时，已停止";
            }
            context.due = now + std::chrono::milliseconds(300);
            return false;
        }
        if (RequestShopStealth(context)) {
            context.shop_safe_transfer_pending = false;
            context.shop_stealth_ready = true;
            context.shop_safe_transfer_attempts = 0;
            context.shop_safe_arrival = {};
            context.shop_safe_deadline = {};
            context.shop_safe_retry_at = {};
            context.teleported = false;
            context.due = now + std::chrono::seconds(1);
            context.status = "商店拿取：固定点隐身成功，返回目标";
            return false;
        }
        if (now >= context.shop_safe_deadline) {
            context.running = false;
            context.shop_safe_transfer_pending = false;
            context.stop_movement_pending = true;
            context.status = "商店拿取：固定点仍无法开启隐身，已停止";
        }
        context.due = now + std::chrono::seconds(1);
        return false;
    }
    double position[3]{};
    if (!SnapshotPlayerPosition(context, position)) {
        context.due = now + std::chrono::milliseconds(500);
        return false;
    }
    Point safe{"fixed-shop-safe", kShopSafePoint[0], kShopSafePoint[1], kShopSafePoint[2], "shop_steal"};
    if (!Teleport(context, safe)) {
        context.running = false;
        context.stop_movement_pending = true;
        context.status = "商店拿取：无法到达固定安全点，已停止";
        return false;
    }
    context.shop_safe_transfer_pending = true;
    context.shop_safe_transfer_attempts = 1;
    context.shop_safe_arrival = {};
    context.shop_safe_deadline = now + kShopSafeDeadline;
    context.shop_safe_retry_at = now + kShopSafeRetryDelay;
    context.teleported = false;
    context.target_actor = 0;
    context.status = "商店拿取：前往固定安全点";
    LogShop(context, "fixed safe point transfer point=" + target.row_name);
    context.due = now + std::chrono::milliseconds(500);
    return false;
}

bool ShopProperty(Context& context, std::uintptr_t cls, std::string_view name, std::int32_t size, std::uintptr_t& field, std::int32_t& offset) {
    field = reinterpret_cast<std::uintptr_t>(ReadPointer(reinterpret_cast<void*>(cls + 112)));
    for (unsigned i = 0; field && i < 256; ++i) {
        std::uint32_t name_id{};
        if (!Read(reinterpret_cast<void*>(field + 32), name_id)) return false;
        if (ResolveName(context.names, name_id) == name) {
            std::int32_t actual_size{};
            return Read(reinterpret_cast<void*>(field + 52), actual_size) && actual_size == size &&
                Read(reinterpret_cast<void*>(field + 68), offset) && offset >= 0;
        }
        const auto next = reinterpret_cast<std::uintptr_t>(ReadPointer(reinterpret_cast<void*>(field + 72)));
        if (next == field) break;
        field = next;
    }
    return false;
}

enum class ShopTakeResult : std::uint8_t {
    retry,
    not_stealth,
    triggered,
};

bool ShopItemUsesBlueprint(Context& context, std::uintptr_t actor,
                           bool& use_blueprint) {
    const auto cls = reinterpret_cast<std::uintptr_t>(ReadPointer(reinterpret_cast<void*>(actor + kObjectClassOffset)));
    if (!cls || !ObjectName(context.names, cls).starts_with("ShopStealGoods_")) return false;
    std::uintptr_t field{};
    std::int32_t offset{};
    if (!ShopProperty(context, cls, "bUseBPInteractEntries", 1, field, offset) ||
        !Read(reinterpret_cast<void*>(actor + offset), use_blueprint)) return false;
    return true;
}

ShopTakeResult TakeShopItem(Context& context, std::uintptr_t actor,
                            InteractProbe* probe = nullptr) {
    if (!context.trigger_interact_fn) {
        if (probe != nullptr) probe->reason = "trigger-function-missing";
        return ShopTakeResult::retry;
    }
    bool use_blueprint{};
    if (!ShopItemUsesBlueprint(context, actor, use_blueprint)) {
        if (probe != nullptr) probe->reason = "class-or-property";
        return ShopTakeResult::retry;
    }
    if (!use_blueprint) {
        if (probe != nullptr) probe->reason = "not-stealth";
        LogShop(context, "shop item is not in stealth pickup state");
        return ShopTakeResult::not_stealth;
    }
    return TriggerInteractPickup(context, actor, probe)
        ? ShopTakeResult::triggered
        : ShopTakeResult::retry;
}

bool ReadActorLocation(Context& context, const std::uintptr_t actor,
                       double (&loc)[3]) noexcept {
    std::uintptr_t root{};
    if (!Read(reinterpret_cast<const void*>(actor + kActorRootComponentOffset),
              root) ||
        root == 0) {
        return false;
    }
    return Read(reinterpret_cast<const void*>(
                    root + kSceneComponentLocationOffset), loc[0]) &&
        Read(reinterpret_cast<const void*>(
                 root + kSceneComponentLocationOffset + 8), loc[1]) &&
        Read(reinterpret_cast<const void*>(
                 root + kSceneComponentLocationOffset + 16), loc[2]);
}

bool IsObjectInGObjects(Context& context, const std::uintptr_t target) noexcept {
    if (target == 0) return false;
    if (context.g_objects_address == 0 &&
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            context.g_objects_address)) {
        return true;
    }
    std::int32_t count{};
    std::int32_t num_chunks{};
    std::uintptr_t items{};
    if (!Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectItemsOffset), items) ||
        items == 0 ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectCountOffset), count) ||
        !Read(reinterpret_cast<const void*>(
                  context.g_objects_address + kObjectNumChunksOffset), num_chunks) ||
        count <= 0 || num_chunks <= 0) {
        return true;
    }
    std::uint32_t cur_chunk = 0xFFFFFFFFu;
    std::uintptr_t chunk{};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto chunk_index = static_cast<std::uint32_t>(i) / kObjectChunkSize;
        if (chunk_index != cur_chunk) {
            cur_chunk = chunk_index;
            if (!Read(reinterpret_cast<const void*>(
                          items + static_cast<std::uintptr_t>(chunk_index) * sizeof(void*)),
                      chunk) ||
                chunk == 0) {
                continue;
            }
        }
        const auto within = static_cast<std::uint32_t>(i) % kObjectChunkSize;
        std::uintptr_t object{};
        if (!Read(reinterpret_cast<const void*>(
                      chunk + static_cast<std::uintptr_t>(within) * kObjectItemStride),
                  object) ||
            object == 0) {
            continue;
        }
        if (object == target) return true;
    }
    return false;
}

void TickPickup(Context& context, const Point& p,
                const std::chrono::steady_clock::time_point now) {
    double player_pos[3]{};
    if (SnapshotPlayerPosition(context, player_pos) &&
        player_pos[2] < p.z - kFallOutThresholdCentimeters) {
        if (context.teleport_retry == 0) {
            context.teleport_retry = 1;
            context.teleported = false;
            context.retry_count = 0;
            context.due = now + std::chrono::milliseconds(2000);
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.fell_out", "Fell out of world, re-teleport [{0}]",
                args);
        } else {
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.box_not_found", "Box not found, skip [{0}]", args);
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
        }
        return;
    }
    if (!PickupReady(context.pickup)) {
        const std::array args{std::string_view(p.row_name)};
        context.status = context.localizer.Format(
            "status.pickup_unavailable", "Pickup unavailable [{0}]", args);
        ++context.skipped;
        ++context.current_index;
        ResetPointState(context);
        context.due = now;
        return;
    }
    if (!context.pickup_started) {
        AnomalyNtePickupSnapshotV1 baseline{sizeof(baseline)};
        if (context.pickup->snapshot(context.pickup->user, &baseline).code !=
            ANOMALY_STATUS_V1_OK) {
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.pickup_snapshot_failed",
                "Pickup snapshot failed [{0}]", args);
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            return;
        }
        AnomalyNtePickupRequestV1 request{sizeof(request)};
        request.flags = ANOMALY_NTE_PICKUP_V1_NONE;
        request.radius = kPickupRadiusCentimeters;
        request.maximum_items = kPickupMaximumItems;
        if (context.pickup->request_nearby(context.pickup->user, &request).code !=
            ANOMALY_STATUS_V1_OK) {
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.pickup_request_failed",
                "Pickup request failed [{0}]", args);
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            return;
        }
        context.pickup_baseline_sequence = baseline.sequence;
        context.pickup_started = true;
        context.pickup_deadline =
            now + std::chrono::milliseconds(
                      static_cast<long long>(kPickupTimeoutSeconds * 1000.0));
        const std::array args{std::string_view(p.row_name)};
        context.status = context.localizer.Format(
            "status.picking", "Picking [{0}]", args);
        context.due = now;
        return;
    }
    AnomalyNtePickupSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 snap_status =
        context.pickup->snapshot(context.pickup->user, &snapshot);
    if (snap_status.code == ANOMALY_STATUS_V1_OK &&
        (snapshot.flags & ANOMALY_NTE_PICKUP_V1_VALID) != 0 &&
        snapshot.sequence > context.pickup_baseline_sequence &&
        snapshot.state == ANOMALY_NTE_PICKUP_V1_COMPLETE) {
        if (snapshot.confirmed > 0) {
            ++context.picked;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.picked", "Picked [{0}]", args);
            return;
        }
        if (snapshot.nearby == 0 && snapshot.triggered == 0 &&
            snapshot.confirmed == 0 && snapshot.unconfirmed == 0) {
            if (context.pickup_retries < kPickupMaximumRetries) {
                ++context.pickup_retries;
                context.pickup_started = false;
                context.due = now + std::chrono::milliseconds(1000);
                context.status = "等待箱子加载 " + p.row_name +
                    " (" + std::to_string(context.pickup_retries) + ")";
                return;
            }
            if (context.target_actor == 0) {
                context.target_actor =
                    ScanForActor(context, p, ActorPrefixForCategory(p.category));
            }
            if (context.target_actor != 0) {
                double bx{}, by{}, bz{};
                std::uintptr_t root{};
                const bool near_point =
                    Read(reinterpret_cast<const void*>(
                             context.target_actor + kActorRootComponentOffset),
                         root) &&
                    root != 0 &&
                    Read(reinterpret_cast<const void*>(
                             root + kSceneComponentLocationOffset), bx) &&
                    Read(reinterpret_cast<const void*>(
                             root + kSceneComponentLocationOffset + 8), by) &&
                    Read(reinterpret_cast<const void*>(
                             root + kSceneComponentLocationOffset + 16), bz) &&
                    ((bx - p.x) * (bx - p.x) + (by - p.y) * (by - p.y) +
                     (bz - p.z) * (bz - p.z)) <=
                        kFallbackVerifyRadiusCentimeters *
                            kFallbackVerifyRadiusCentimeters;
                if (!near_point) {
                    ++context.skipped;
                    ++context.current_index;
                    ResetPointState(context);
                    context.due = now;
                    const std::array args{std::string_view(p.row_name)};
                    context.status = context.localizer.Format(
                        "status.pickup_empty", "Nothing to pick [{0}]", args);
                    return;
                }
                if (!context.interacted) {
                    if (TriggerInteractPickup(context, context.target_actor)) {
                        context.interacted = true;
                        context.due = now + std::chrono::milliseconds(2000);
                        const std::array args{std::string_view(p.row_name)};
                        context.status = context.localizer.Format(
                            "status.interacted", "Interacted [{0}]", args);
                        return;
                    }
                    ++context.skipped;
                    ++context.current_index;
                    ResetPointState(context);
                    context.due = now;
                    const std::array args{std::string_view(p.row_name)};
                    context.status = context.localizer.Format(
                        "status.pickup_failed", "Pickup failed [{0}]", args);
                    return;
                }
                ++context.picked;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                const std::array args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.picked", "Picked [{0}]", args);
                return;
            }
            if (context.teleport_retry == 0) {
                ResetPointState(context);
                context.teleport_retry = 1;
                context.due = now + std::chrono::milliseconds(2000);
                const std::array args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.reteleport", "Re-teleport [{0}]", args);
                return;
            }
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.pickup_empty", "Nothing to pick [{0}]", args);
            return;
        }
        if (context.pickup_retries < kPickupMaximumRetries) {
            ++context.pickup_retries;
            context.pickup_started = false;
            context.due = now + std::chrono::milliseconds(300);
            const std::string retries = std::to_string(context.pickup_retries);
            const std::array args{
                std::string_view(retries), std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.pickup_retry",
                "Pickup still present, retry {0} [{1}]", args);
            return;
        }
        ++context.skipped;
        ++context.current_index;
        ResetPointState(context);
        context.due = now;
        const std::array args{std::string_view(p.row_name)};
        context.status = context.localizer.Format(
            "status.pickup_failed", "Pickup failed [{0}]", args);
        return;
    }
    if (now >= context.pickup_deadline) {
        const std::array args{std::string_view(p.row_name)};
        context.status = context.localizer.Format(
            "status.pickup_timeout", "Pickup timeout [{0}]", args);
        ++context.skipped;
        ++context.current_index;
        ResetPointState(context);
        context.due = now;
        return;
    }
    context.status = "拾取中 " + p.row_name + " conf=" +
        std::to_string(snapshot.confirmed);
    context.due = now + std::chrono::milliseconds(300);
}

void TickFoodPickup(Context& context, const Point& p,
                    const std::chrono::steady_clock::time_point now) {
    if (context.target_actor == 0) {
        double player_pos[3]{};
        if (SnapshotPlayerPosition(context, player_pos) &&
            player_pos[2] < p.z - kFallOutThresholdCentimeters) {
            if (context.fallout_retries == 0) {
                context.fallout_baseline_z = player_pos[2];
            }
            const double gained = player_pos[2] - context.fallout_baseline_z;
            if (gained >= 100.0) {
                context.fallout_baseline_z = player_pos[2];
            }
            if (context.fallout_retries < 5) {
                ++context.fallout_retries;
                context.teleport_retry = 1;
                context.teleported = false;
                context.retry_count = 0;
                context.due = now + std::chrono::milliseconds(2000);
                context.status = "掉出世界，重传 " + p.row_name;
            } else {
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                context.status = "掉出世界，跳过 " + p.row_name;
            }
            return;
        }
        context.target_actor =
            ScanForActor(context, p, ActorPrefixForCategory(p.category));
        if (context.target_actor == 0 && p.category == "prison") {
            context.target_actor = ScanForActor(context, p, "Prison");
        }
        if (context.target_actor == 0) {
            ++context.retry_count;
            if (context.retry_count <= kActorLoadWaitSeconds) {
                context.due = now + std::chrono::milliseconds(1000);
                context.status = "等待加载 " + p.row_name + " (" +
                    std::to_string(context.retry_count) + ")";
            } else if (context.teleport_retry == 0) {
                std::uintptr_t verify_actor =
                    ScanForActor(context, p, ActorPrefixForCategory(p.category));
                context.teleport_retry = 1;
                context.teleported = false;
                context.retry_count = 0;
                context.due = now + std::chrono::milliseconds(2000);
                context.status = verify_actor == 0
                    ? "箱子未加载，重传 " + p.row_name
                    : "重传 " + p.row_name;
            } else {
                NoteFoodPointEmpty(context, p.row_name);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                context.status = "没找到，跳过 " + p.row_name;
            }
            return;
        }
    }
    {
        double actor_pos[3]{};
        double player_pos[3]{};
        if (ReadActorLocation(context, context.target_actor, actor_pos) &&
            SnapshotPlayerPosition(context, player_pos)) {
            const double dx = player_pos[0] - actor_pos[0];
            const double dy = player_pos[1] - actor_pos[1];
            const double dist = std::sqrt(dx * dx + dy * dy);
            const double adx = actor_pos[0] - p.x;
            const double ady = actor_pos[1] - p.y;
            const double actor_to_p = std::sqrt(adx * adx + ady * ady);
            if (actor_to_p > kTargetActorMaxDistanceCentimeters) {
                if (context.teleport_retry == 0) {
                    context.teleport_retry = 1;
                    context.teleported = false;
                    context.target_actor = 0;
                    context.retry_count = 0;
                    context.interact_retry = 0;
                    context.can_interact_retries = 0;
                    context.food_approaching = false;
                    context.food_has_last_pos = false;
                    context.due = now + std::chrono::milliseconds(2000);
                    context.status = "识别偏离，重传 " + p.row_name;
                } else {
                    ++context.skipped;
                    ++context.current_index;
                    ResetPointState(context);
                    context.due = now;
                    context.status = "识别偏离，跳过 " + p.row_name;
                }
                return;
            }
            const double approach_threshold =
                context.developer_mode.load(std::memory_order_acquire)
                    ? kTeleportApproachRadiusCentimeters
                    : kPickupApproachRadiusCentimeters;
            if (dist > approach_threshold) {
                if (context.developer_mode.load(std::memory_order_acquire)) {
                    Point ap;
                    ap.x = actor_pos[0];
                    ap.y = actor_pos[1];
                    ap.z = actor_pos[2];
                    Teleport(context, ap);
                    context.due = now + std::chrono::milliseconds(1000);
                    context.status = "靠近中(传送) " + p.row_name;
                    return;
                }
                if (NavigationReady(context.navigation)) {
                    Point ap;
                    ap.x = actor_pos[0];
                    ap.y = actor_pos[1];
                    ap.z = player_pos[2];
                    double dest[3]{};
                    ComputeApproachDestination(context, ap, dest);
                    if (!context.food_approaching) {
                        context.navigation->stop_movement(
                            context.navigation->user);
                        if (context.navigation->move_to_location(
                                context.navigation->user, dest).code ==
                            ANOMALY_STATUS_V1_OK) {
                            context.food_approaching = true;
                            context.food_approach_deadline =
                                now + std::chrono::seconds(5);
                            context.food_nav_retry_at =
                                now + std::chrono::seconds(2);
                        }
                    } else if (now >= context.food_nav_retry_at) {
                        context.food_nav_retry_at =
                            now + std::chrono::seconds(2);
                        context.navigation->stop_movement(
                            context.navigation->user);
                        context.navigation->move_to_location(
                            context.navigation->user, dest);
                    }
                    if (context.food_has_last_pos) {
                        const double lx = player_pos[0] - context.food_last_pos[0];
                        const double ly = player_pos[1] - context.food_last_pos[1];
                        const double moved = std::sqrt(lx * lx + ly * ly);
                        if (moved >= kProgressThresholdCentimeters) {
                            context.food_approach_deadline =
                                now + std::chrono::seconds(5);
                        }
                    }
                    context.food_last_pos[0] = player_pos[0];
                    context.food_last_pos[1] = player_pos[1];
                    context.food_last_pos[2] = player_pos[2];
                    context.food_has_last_pos = true;
                    if (now >= context.food_approach_deadline) {
                        context.food_approaching = false;
                        context.food_has_last_pos = false;
                    } else {
                        context.due = now + std::chrono::milliseconds(300);
                        context.status = "靠近中 " +
                            std::to_string(static_cast<long long>(dist)) + "cm " +
                            p.row_name;
                        return;
                    }
                } else {
                    context.due = now + std::chrono::milliseconds(500);
                    return;
                }
            } else {
                context.food_approaching = false;
                context.food_has_last_pos = false;
            }
        }
        if (!PickupReady(context.pickup)) {
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            context.status = "拾取不可用 " + p.row_name;
            return;
        }
        {
            double actor_pos3[3]{};
            double player_pos3[3]{};
            if (ReadActorLocation(context, context.target_actor, actor_pos3) &&
                SnapshotPlayerPosition(context, player_pos3)) {
                const double dx3 = player_pos3[0] - actor_pos3[0];
                const double dy3 = player_pos3[1] - actor_pos3[1];
                const double dz3 = player_pos3[2] - actor_pos3[2];
                const double dist3 =
                    std::sqrt(dx3 * dx3 + dy3 * dy3 + dz3 * dz3);
                if (dist3 > kTargetActorMaxDistanceCentimeters) {
                    Point ap;
                    ap.x = actor_pos3[0];
                    ap.y = actor_pos3[1];
                    ap.z = actor_pos3[2];
                    if (Teleport(context, ap)) {
                        context.due =
                            now + std::chrono::milliseconds(1000);
                        context.status = "距离过远，传送到物品 " + p.row_name;
                        return;
                    }
                    ++context.skipped;
                    ++context.current_index;
                    ResetPointState(context);
                    context.due = now;
                    context.status = "距离过远传送失败，跳过 " + p.row_name;
                    return;
                }
            }
        }
        if (!context.pickup_started) {
            AnomalyNtePickupSnapshotV1 baseline{sizeof(baseline)};
            if (context.pickup->snapshot(context.pickup->user, &baseline).code !=
                ANOMALY_STATUS_V1_OK) {
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                context.status = "拾取快照失败 " + p.row_name;
                return;
            }
            AnomalyNtePickupRequestV1 request{sizeof(request)};
            request.flags = ANOMALY_NTE_PICKUP_V1_NONE;
            request.radius = kPickupRadiusCentimeters;
            request.maximum_items = kPickupMaximumItems;
            if (context.pickup->request_nearby(context.pickup->user, &request).code !=
                ANOMALY_STATUS_V1_OK) {
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                context.status = "拾取请求失败 " + p.row_name;
                return;
            }
            context.pickup_baseline_sequence = baseline.sequence;
            context.pickup_started = true;
            context.pickup_deadline =
                now + std::chrono::milliseconds(
                          static_cast<long long>(kPickupTimeoutSeconds * 1000.0));
            context.due = now;
            context.status = "拾取中 " + p.row_name;
            return;
        }
        AnomalyNtePickupSnapshotV1 snapshot{sizeof(snapshot)};
        const AnomalyStatusV1 snap_status =
            context.pickup->snapshot(context.pickup->user, &snapshot);
        if (snap_status.code == ANOMALY_STATUS_V1_OK &&
            (snapshot.flags & ANOMALY_NTE_PICKUP_V1_VALID) != 0 &&
            snapshot.sequence > context.pickup_baseline_sequence &&
            snapshot.state == ANOMALY_NTE_PICKUP_V1_COMPLETE) {
            if (snapshot.confirmed > 0) {
                NoteFoodPointFound(context, p.row_name);
                ++context.picked;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                context.status = "已拾取 " + p.row_name;
                return;
            }
            if (snapshot.nearby == 0 && snapshot.triggered == 0 &&
                snapshot.confirmed == 0 && snapshot.unconfirmed == 0) {
                if (context.pickup_retries < kPickupMaximumRetries) {
                    ++context.pickup_retries;
                    context.pickup_started = false;
                    context.due = now + std::chrono::milliseconds(1000);
                    context.status = "等待拾取物 " + p.row_name + " (" +
                        std::to_string(context.pickup_retries) + ")";
                    return;
                }
                if (p.category == "prison" && context.target_actor != 0) {
                    if (!context.interacted) {
                        if (TriggerInteractPickup(context, context.target_actor)) {
                            context.interacted = true;
                            context.interact_verify_deadline =
                                now + std::chrono::seconds(6);
                            context.due = now + std::chrono::milliseconds(1000);
                            context.status = "监狱专属交互 " + p.row_name;
                            return;
                        }
                    } else if (now < context.interact_verify_deadline) {
                        TriggerInteractPickup(context, context.target_actor);
                        context.due = now + std::chrono::milliseconds(1000);
                        context.status = "监狱专属等待 " + p.row_name;
                        return;
                    } else {
                        ++context.picked;
                        ++context.current_index;
                        ResetPointState(context);
                        context.due = now;
                        context.status = "监狱专属拾取 " + p.row_name;
                        return;
                    }
                }
                if (context.teleport_retry == 0) {
                    context.teleport_retry = 1;
                    context.teleported = false;
                    context.target_actor = 0;
                    context.retry_count = 0;
                    context.pickup_retries = 0;
                    context.pickup_started = false;
                    context.food_approaching = false;
                    context.food_has_last_pos = false;
                    context.due = now + std::chrono::milliseconds(2000);
                    context.status = "没拾取到，重传 " + p.row_name;
                    return;
                }
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                context.status = "没拾取到，跳过 " + p.row_name;
                return;
            }
            if (context.pickup_retries < kPickupMaximumRetries) {
                ++context.pickup_retries;
                context.pickup_started = false;
                context.due = now + std::chrono::milliseconds(300);
                context.status = "拾取重试 " + p.row_name;
                return;
            }
            if (context.teleport_retry == 0) {
                context.teleport_retry = 1;
                context.teleported = false;
                context.target_actor = 0;
                context.retry_count = 0;
                context.pickup_retries = 0;
                context.pickup_started = false;
                context.food_approaching = false;
                context.food_has_last_pos = false;
                context.due = now + std::chrono::milliseconds(2000);
                context.status = "拾取失败，重传 " + p.row_name;
                return;
            }
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            context.status = "拾取失败 " + p.row_name;
            return;
        }
        if (now >= context.pickup_deadline) {
            if (context.teleport_retry == 0) {
                context.teleport_retry = 1;
                context.teleported = false;
                context.target_actor = 0;
                context.retry_count = 0;
                context.pickup_retries = 0;
                context.pickup_started = false;
                context.food_approaching = false;
                context.food_has_last_pos = false;
                context.due = now + std::chrono::milliseconds(2000);
                context.status = "拾取超时，重传 " + p.row_name;
                return;
            }
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            context.status = "拾取超时 " + p.row_name;
            return;
        }
        context.due = now + std::chrono::milliseconds(300);
        context.status = "拾取中 " + p.row_name;
    }
}

void Tick(Context& context) {
    std::lock_guard<std::mutex> lock(context.mutex);
    if (!context.running) return;
    if (context.manual_landmark_pending) return;
    if (context.manual_navigating) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < context.due) return;
    if (context.current_index >= context.filtered_points.size()) {
        context.running = false;
        const std::string picked_str = std::to_string(context.picked);
        const std::string skipped_str = std::to_string(context.skipped);
        const std::array completed_args{
            std::string_view(picked_str), std::string_view(skipped_str)};
        context.status = context.localizer.Format(
            "status.completed", "Completed: picked {0}, skipped {1}",
            completed_args);
        return;
    }
    const Point& p = context.filtered_points[context.current_index];
    if (p.category == "box_food" || p.category == "item_food") {
        std::string food_key;
        if (FoodGroupOf(p.row_name, food_key) &&
            (context.food_spent_groups.find(food_key) != context.food_spent_groups.end() ||
             context.food_empty_groups.find(food_key) != context.food_empty_groups.end())) {
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.region_spent", "Region already collected this week, skip [{0}]",
                args);
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            return;
        }
    }
    if (p.category == "shop_steal" && !context.shop_stealth_ready) {
        PrepareShopStealth(context, p, now);
        return;
    }
    if (!context.teleported && !context.moving && !context.landmark_transfer_wait) {
        if (context.developer_mode.load(std::memory_order_acquire)) {
            if (Teleport(context, p)) {
                context.teleported = true;
                context.retry_count = 0;
                context.interact_retry = 0;
                const auto wait = context.teleport_retry == 0
                    ? std::chrono::milliseconds(2000)
                    : std::chrono::milliseconds(1000);
                context.due = now + wait;
                const std::array teleport_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.teleporting", "Teleport [{0}]", teleport_args);
            } else {
                const std::array teleport_failed_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.teleport_failed", "Teleport failed, skip [{0}]",
                    teleport_failed_args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
        } else if (!NavigationReady(context.navigation)) {
            const std::array nav_unavailable_args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.navigation_unavailable", "Navigation unavailable, skip [{0}]",
                nav_unavailable_args);
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
        } else {
            double player_position[3]{};
            const bool have_player =
                SnapshotPlayerPosition(context, player_position);
            if (have_player &&
                TryBeginLandmarkTransfer(context, p, player_position[0],
                                         player_position[1])) {
                context.due = now + std::chrono::milliseconds(200);
                const std::array landmark_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.landmark_transfer", "Fast travel [{0}]", landmark_args);
            } else if (StartNavigation(context, p, now)) {
                context.due = now + std::chrono::milliseconds(200);
                const std::array navigate_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.navigating", "Walking [{0}]", navigate_args);
            } else {
                const std::array nav_failed_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.navigation_failed", "Walk failed, skip [{0}]",
                    nav_failed_args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
        }
        return;
    }
    if (context.landmark_transfer_wait) {
        double position[3]{};
        const bool have_position = SnapshotPlayerPosition(context, position);
        const bool arrived =
            have_position &&
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
            if (StartNavigation(context, p, now)) {
                context.due = now + std::chrono::milliseconds(200);
                const std::array navigate_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.navigating", "Walking [{0}]", navigate_args);
            } else {
                const std::array nav_failed_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.navigation_failed", "Walk failed, skip [{0}]",
                    nav_failed_args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
        } else {
            context.due = now + std::chrono::milliseconds(200);
        }
        return;
    }
    if (context.moving) {
        double position[3]{};
        const bool have_position = SnapshotPlayerPosition(context, position);
        bool arrived = false;
        if (have_position) {
            const double distance_squared =
                PlanarDistanceSquared(position[0], position[1], p.x, p.y);
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
            context.moving = false;
            context.teleported = true;
            context.retry_count = 0;
            context.interact_retry = 0;
            context.due = now + std::chrono::milliseconds(500);
            const std::array arrived_args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.arrived", "Arrived [{0}]", arrived_args);
        } else if (now - context.navigation_last_progress_at >=
                   std::chrono::milliseconds(static_cast<long long>(
                       kMovementTimeoutSeconds * 1000.0))) {
            context.navigation->stop_movement(context.navigation->user);
            if (context.navigation_attempt + 1 < kNavigationMaxAttempts) {
                ++context.navigation_attempt;
                context.moving = false;
                context.due = now + std::chrono::milliseconds(500);
                const std::array retry_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.retry_navigation", "Retry walk [{0}]", retry_args);
            } else {
                const std::array timeout_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.navigation_timeout", "Walk timeout, skip [{0}]",
                    timeout_args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
        } else {
            if (now >= context.navigation_retry_at) {
                context.navigation_retry_at = now + std::chrono::milliseconds(
                    static_cast<long long>(kReissueDelaySeconds * 1000.0));
                context.navigation->stop_movement(context.navigation->user);
                double destination[3]{};
                ComputeApproachDestination(context, p, destination);
                context.navigation->move_to_location(
                    context.navigation->user, destination);
            }
            const double distance_cm =
                have_position
                    ? std::sqrt(PlanarDistanceSquared(position[0], position[1], p.x, p.y))
                    : -1.0;
            if (have_position) {
                const std::string distance_str =
                    std::to_string(static_cast<long long>(distance_cm));
                const std::array distance_args{
                    std::string_view(distance_str), std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.navigating_distance", "Walking {0}cm [{1}]",
                    distance_args);
            }
            context.due = now + std::chrono::milliseconds(200);
        }
        return;
    }
    if (UsesPickupService(p.category)) {
        TickPickup(context, p, now);
        return;
    }
    if (p.category == "box_food" || p.category == "item_food" ||
        p.category == "prison" || p.category == "wallet") {
        TickFoodPickup(context, p, now);
        return;
    }
    if (context.target_actor == 0) {
        double player_pos[3]{};
        if (SnapshotPlayerPosition(context, player_pos) &&
            player_pos[2] < p.z - kFallOutThresholdCentimeters) {
            if (context.fallout_retries == 0) {
                context.fallout_baseline_z = player_pos[2];
            }
            const double gained = player_pos[2] - context.fallout_baseline_z;
            if (gained >= 100.0) {
                context.fallout_baseline_z = player_pos[2];
            }
            if (context.fallout_retries < 5) {
                ++context.fallout_retries;
                context.teleport_retry = 1;
                context.teleported = false;
                context.retry_count = 0;
                context.due = now + std::chrono::milliseconds(2000);
                const std::array fell_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.fell_out", "Fell out of world, re-teleport [{0}]",
                    fell_args);
            } else {
                const std::array not_found_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.box_not_found", "Box not found, skip [{0}]",
                    not_found_args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
            return;
        }
        context.target_actor = ScanForActor(context, p,
            p.category == "shop_steal" ? ActorPrefixForCategory(p.category) : context.type_prefix);
        if (context.target_actor == 0) {
            ++context.retry_count;
            if (context.retry_count <= kActorLoadWaitSeconds) {
                context.due = now + std::chrono::milliseconds(1000);
                const std::string retry_str = std::to_string(context.retry_count);
                const std::array waiting_args{
                    std::string_view(retry_str), std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.waiting_load", "Waiting box load {0} [{1}]",
                    waiting_args);
            } else if (context.teleport_retry == 0) {
                context.teleport_retry = 1;
                context.teleported = false;
                context.retry_count = 0;
                context.due = now + std::chrono::milliseconds(2000);
                const std::array reteleport_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.reteleport", "Re-teleport [{0}]", reteleport_args);
            } else {
                const std::array not_found_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.box_not_found", "Box not found, skip [{0}]",
                    not_found_args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
            return;
        }
    }
    if (!context.interacted) {
        double actor_pos[3]{};
        double player_pos[3]{};
        if (ReadActorLocation(context, context.target_actor, actor_pos) &&
            SnapshotPlayerPosition(context, player_pos)) {
            const double dx = player_pos[0] - actor_pos[0];
            const double dy = player_pos[1] - actor_pos[1];
            const double dist = std::sqrt(dx * dx + dy * dy);
            const double adx = actor_pos[0] - p.x;
            const double ady = actor_pos[1] - p.y;
            const double actor_to_p = std::sqrt(adx * adx + ady * ady);
            if (actor_to_p > kTargetActorMaxDistanceCentimeters) {
                if (context.teleport_retry == 0) {
                    context.teleport_retry = 1;
                    context.teleported = false;
                    context.target_actor = 0;
                    context.retry_count = 0;
                    context.interact_retry = 0;
                    context.can_interact_retries = 0;
                    context.food_approaching = false;
                    context.food_has_last_pos = false;
                    context.due = now + std::chrono::milliseconds(2000);
                    context.status = "识别偏离，重传 " + p.row_name;
                } else {
                    ++context.skipped;
                    ++context.current_index;
                    ResetPointState(context);
                    context.due = now;
                    context.status = "识别偏离，跳过 " + p.row_name;
                }
                return;
            }
            const double approach_threshold =
                context.developer_mode.load(std::memory_order_acquire)
                    ? kTeleportApproachRadiusCentimeters
                    : kPickupApproachRadiusCentimeters;
            if (dist > approach_threshold) {
                if (context.developer_mode.load(std::memory_order_acquire)) {
                    Point ap;
                    ap.x = actor_pos[0];
                    ap.y = actor_pos[1];
                    ap.z = actor_pos[2];
                    Teleport(context, ap);
                    context.due = now + std::chrono::milliseconds(1000);
                    context.status = "靠近中(传送) " + p.row_name;
                    return;
                }
                if (NavigationReady(context.navigation)) {
                    Point ap;
                    ap.x = actor_pos[0];
                    ap.y = actor_pos[1];
                    ap.z = player_pos[2];
                    double dest[3]{};
                    ComputeApproachDestination(context, ap, dest);
                    if (!context.food_approaching) {
                        context.navigation->stop_movement(
                            context.navigation->user);
                        if (context.navigation->move_to_location(
                                context.navigation->user, dest).code ==
                            ANOMALY_STATUS_V1_OK) {
                            context.food_approaching = true;
                            context.food_approach_deadline =
                                now + std::chrono::seconds(5);
                            context.food_nav_retry_at =
                                now + std::chrono::seconds(2);
                        }
                    } else if (now >= context.food_nav_retry_at) {
                        context.food_nav_retry_at =
                            now + std::chrono::seconds(2);
                        context.navigation->stop_movement(
                            context.navigation->user);
                        context.navigation->move_to_location(
                            context.navigation->user, dest);
                    }
                    context.due = now + std::chrono::milliseconds(300);
                    context.status = "靠近中 " +
                        std::to_string(static_cast<long long>(dist)) + "cm " +
                        p.row_name;
                    return;
                }
                context.due = now + std::chrono::milliseconds(500);
                return;
            }
            context.food_approaching = false;
            context.food_has_last_pos = false;
        }
        std::uint8_t baseline{};
        Read(reinterpret_cast<const void*>(
                 context.target_actor + kInteractFinishOffset), baseline);
        context.interact_baseline = baseline;
        std::uint8_t params[12]{};
        std::memcpy(params, &context.target_actor, sizeof(context.target_actor));
        ShopTakeResult shop_result = ShopTakeResult::retry;
        InteractProbe shop_probe{};
        const bool triggered = p.category == "shop_steal"
            ? (shop_result = TakeShopItem(context, context.target_actor,
                                          &shop_probe),
               shop_result == ShopTakeResult::triggered)
            : Invoke(reinterpret_cast<void*>(context.controller),
                     reinterpret_cast<void*>(context.server_interact_fn), params);
        if (triggered) {
            context.interacted = true;
            if (p.category == "shop_steal") LogShop(context, "shop interaction requested point=" + p.row_name);
            context.interact_verify_deadline = now + std::chrono::seconds(p.category == "shop_steal" ? 30 : 8);
            context.due = now + std::chrono::milliseconds(3000);
            const std::array interacted_args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.interacted", "Interacted [{0}]", interacted_args);
        } else {
            if (p.category == "shop_steal") {
                if (shop_result == ShopTakeResult::retry) {
                    if (context.interact_retry < 5) {
                        ++context.interact_retry;
                        // One line per attempt: an entry count that stays empty, or a
                        // can_try count that climbs, is the difference between a bad
                        // standing spot and an entry the game arms late.
                        double probe_distance{-1.0};
                        double probe_actor[3]{};
                        double probe_player[3]{};
                        if (ReadActorLocation(context, context.target_actor,
                                              probe_actor) &&
                            SnapshotPlayerPosition(context, probe_player)) {
                            const double pdx = probe_player[0] - probe_actor[0];
                            const double pdy = probe_player[1] - probe_actor[1];
                            probe_distance = std::sqrt(pdx * pdx + pdy * pdy);
                        }
                        LogShop(context,
                            "entry attempt=" + std::to_string(context.interact_retry) +
                                " reason=" + shop_probe.reason +
                                " entries=" + std::to_string(shop_probe.entries) +
                                " can_try=" + std::to_string(shop_probe.can_try) +
                                " dist=" + std::to_string(static_cast<long long>(
                                    probe_distance)) + "cm point=" + p.row_name);
                        context.due = now + std::chrono::milliseconds(500);
                        context.status = context.localizer.Text(
                            "status.shop_interact_retry",
                            "隐身有效，等待拾取入口重试");
                        return;
                    }
                    if (ShopRescanBudgetSpent(context)) {
                        LogShop(context, "pickup entry never became ready point=" +
                            p.row_name);
                        const std::array exhausted_args{
                            std::string_view(p.row_name)};
                        context.status = context.localizer.Format(
                            "status.shop_rescan_exhausted",
                            "Shop pickup: retries exhausted, skip [{0}]",
                            exhausted_args);
                        ++context.skipped;
                        ++context.current_index;
                        ResetPointState(context);
                        context.due = now;
                        return;
                    }
                    context.target_actor = 0;
                    context.interact_retry = 0;
                    context.due = now + std::chrono::milliseconds(700);
                    context.status = context.localizer.Text(
                        "status.shop_interact_rescan",
                        "隐身有效，重新扫描拾取目标");
                    return;
                }
                if (context.shop_take_retries >= 2) {
                    ++context.skipped;
                    ++context.current_index;
                    ResetPointState(context);
                    context.due = now;
                    context.status = "商店拿取失败，跳过 " + p.row_name;
                    return;
                }
                ++context.shop_take_retries;
                BeginShopExitRecovery(context);
                context.teleported = false;
                context.interacted = false;
                context.target_actor = 0;
                context.retry_count = 0;
                context.interact_retry = 0;
                context.teleport_retry = 0;
                context.due = now + std::chrono::milliseconds(500);
                context.status = context.localizer.Text(
                    "status.shop_stealth_retry",
                    "Shop pickup: stealth not active, moving outside to retry");
                return;
            }
            if (context.interact_retry < 2) {
                ++context.interact_retry;
                context.due = now + std::chrono::milliseconds(1000);
                const std::string interact_retry_str =
                    std::to_string(context.interact_retry);
                const std::array interact_retry_args{
                    std::string_view(interact_retry_str)};
                context.status = context.localizer.Format(
                    "status.interact_retry", "Interact failed, retry {0}",
                    interact_retry_args);
            } else if (context.teleport_retry == 0) {
                context.teleport_retry = 1;
                context.teleported = false;
                context.interacted = false;
                context.target_actor = 0;
                context.retry_count = 0;
                context.interact_retry = 0;
                context.can_interact_retries = 0;
                context.food_approaching = false;
                context.food_has_last_pos = false;
                context.due = now + std::chrono::milliseconds(2000);
                context.status = "交互失败，重传 " + p.row_name;
            } else {
                const std::array interact_failed_args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.interact_failed", "Interact failed, skip [{0}]",
                    interact_failed_args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
        }
        return;
    }
    if (p.category == "shop_steal") {
        std::uint8_t finish{};
        const bool disappeared = !IsObjectInGObjects(context, context.target_actor);
        bool completed = disappeared ||
            (Read(reinterpret_cast<const void*>(context.target_actor + kInteractFinishOffset), finish) &&
             finish != context.interact_baseline);
        if (!completed && now >= context.interact_verify_deadline) {
            RefreshUncollectedCatalog(context);
            completed = context.uncollected_catalog_valid &&
                context.uncollected_points.find(p.row_name) ==
                    context.uncollected_points.end();
        }
        if (completed) {
            LogShop(context, std::string(completed ? "take confirmed point=" : "take timed out point=") + p.row_name);
            const std::array args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.picked", "Picked [{0}]", args);
            ++context.picked;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
        } else if (now >= context.interact_verify_deadline) {
            bool still_stealth{};
            if (ShopItemUsesBlueprint(context, context.target_actor, still_stealth) &&
                still_stealth) {
                if (ShopRescanBudgetSpent(context)) {
                    LogShop(context, "take never confirmed point=" + p.row_name);
                    const std::array exhausted_args{
                        std::string_view(p.row_name)};
                    context.status = context.localizer.Format(
                        "status.shop_rescan_exhausted",
                        "Shop pickup: retries exhausted, skip [{0}]",
                        exhausted_args);
                    ++context.skipped;
                    ++context.current_index;
                    ResetPointState(context);
                    context.due = now;
                    return;
                }
                context.interacted = false;
                context.target_actor = 0;
                context.interact_retry = 0;
                context.interact_verify_deadline = {};
                context.due = now + std::chrono::milliseconds(700);
                context.status = context.localizer.Text(
                    "status.shop_stealth_wait",
                    "隐身仍有效，重新扫描拾取目标");
                return;
            }
            if (context.shop_take_retries < 2) {
                ++context.shop_take_retries;
                BeginShopExitRecovery(context);
                context.teleported = false;
                context.moving = false;
                context.interacted = false;
                context.target_actor = 0;
                context.retry_count = 0;
                context.interact_retry = 0;
                context.teleport_retry = 0;
                context.can_interact_retries = 0;
                context.due = now + std::chrono::milliseconds(500);
                context.status = context.localizer.Text(
                    "status.shop_take_retry",
                    "Pickup not confirmed, moving outside to retry");
            } else {
                LogShop(context, "take timed out point=" + p.row_name);
                const std::array args{std::string_view(p.row_name)};
                context.status = context.localizer.Format(
                    "status.pickup_timeout", "Pickup timed out [{0}]", args);
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
            }
        } else {
            // Stealth can drop the moment a take is refused, and it used to be read only
            // after the whole verification window had run out - which is what left a failed
            // point sitting on "stealth still active" for half a minute. Look while waiting,
            // but only after the completion predicates above, so a take that did land is
            // still confirmed first. Expiring the deadline hands the point to the branch
            // above on the next tick, which re-reads the uncollected catalog before it
            // decides, so a slow confirmation is not mistaken for a failed take.
            bool still_stealth{};
            if (ShopItemUsesBlueprint(context, context.target_actor, still_stealth) &&
                !still_stealth) {
                LogShop(context, "stealth lost while verifying point=" + p.row_name);
                context.interact_verify_deadline = {};
                context.due = now;
                return;
            }
            context.due = now + std::chrono::milliseconds(300);
        }
        return;
    }
    // 验证消失
    std::uint8_t current_finish{};
    Read(reinterpret_cast<const void*>(
             context.target_actor + kInteractFinishOffset), current_finish);
    if (current_finish == context.interact_baseline) {
        if (context.teleport_retry == 0) {
            context.teleport_retry = 1;
            context.teleported = false;
            context.interacted = false;
            context.target_actor = 0;
            context.retry_count = 0;
            context.interact_retry = 0;
            context.can_interact_retries = 0;
            context.due = now + std::chrono::milliseconds(2000);
            const std::array remained_reteleport_args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.box_remained_reteleport", "Box remained, re-teleport [{0}]",
                remained_reteleport_args);
        } else {
            const std::array remained_skip_args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.box_remained_skip", "Box remained, skip [{0}]",
                remained_skip_args);
            ++context.skipped;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
        }
    } else {
        const bool still_alive =
            IsObjectInGObjects(context, context.target_actor);
        if (still_alive) {
            if (context.teleport_retry == 0) {
                context.teleport_retry = 1;
                context.teleported = false;
                context.interacted = false;
                context.target_actor = 0;
                context.retry_count = 0;
                context.interact_retry = 0;
                context.can_interact_retries = 0;
                context.due = now + std::chrono::milliseconds(2000);
                context.status = "拾取未确认，重传 " + p.row_name;
            } else {
                ++context.skipped;
                ++context.current_index;
                ResetPointState(context);
                context.due = now;
                context.status = "拾取未确认，跳过 " + p.row_name;
            }
        } else {
            ++context.picked;
            ++context.current_index;
            ResetPointState(context);
            context.due = now;
            const std::array picked_args{std::string_view(p.row_name)};
            context.status = context.localizer.Format(
                "status.picked", "Picked [{0}]", picked_args);
        }
    }
}

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
    context->actors = view.Query<AnomalyNteActorsServiceV1>(
        ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).get();
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
    context->pickup = view.Query<AnomalyNtePickupServiceV1>(
        ANOMALY_NTE_PICKUP_SERVICE_V1_ID,
        ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION).get();
    context->skills = view.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
    context->skill_invocation = view.Query<AnomalyNteSkillInvocationServiceV1>(
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).get();
    context->oracle = new (std::nothrow) oracle_stone_impl::Context{};
    if (context->oracle != nullptr) {
        oracle_stone_impl::OracleInitialize(*context->oracle, host);
    }
    if (!SignatureReady(context->signature) || !NamesReady(context->names) ||
        !ObjectsReady(context->objects)) {
        delete context;
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {nullptr, 0}};
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    return plugin_context ? anomaly::sdk::Ok()
                          : AnomalyStatusV1{ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0,
                                            {nullptr, 0}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (!plugin_context) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    auto& context = *static_cast<Context*>(plugin_context);
    Stop(context);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context != nullptr) {
        delete context->oracle;
        context->oracle = nullptr;
    }
    delete context;
}

void ANOMALY_CALL Update(void* plugin_context, const double delta_seconds) {
    if (!plugin_context) return;
    auto& context = *static_cast<Context*>(plugin_context);
    if (context.read_pending.exchange(false, std::memory_order_acq_rel)) {
        GetPlayerController(context);
        RefreshUncollectedCatalog(context);
        RefreshPickedUpCatalog(context);
        RefreshFoodRegionState(context);
        ReadTable(context);
        BuildClassMap(context);
    }
    if (context.begin_pending.exchange(false, std::memory_order_acq_rel)) {
        if (!GetPlayerController(context)) {
            std::lock_guard<std::mutex> lock(context.mutex);
            context.status =
                context.localizer.Text("status.no_controller",
                                       "Unable to get player controller");
        } else {
            Begin(context);
        }
    }
    if (context.stop_movement_pending.exchange(false, std::memory_order_acq_rel)) {
        if (NavigationReady(context.navigation)) {
            context.navigation->stop_movement(context.navigation->user);
        }
    }
    if (context.manual_teleport_pending.exchange(false, std::memory_order_acq_rel)) {
        Point p;
        p.x = context.manual_teleport_x;
        p.y = context.manual_teleport_y;
        p.z = context.manual_teleport_z;
        if (context.manual_navigating && NavigationReady(context.navigation)) {
            context.navigation->stop_movement(context.navigation->user);
        }
        context.manual_navigating = false;
        context.manual_landmark_pending = false;
        if (context.developer_mode.load(std::memory_order_acquire)) {
            if (!Teleport(context, p)) {
                std::lock_guard<std::mutex> lock(context.mutex);
                context.status = context.localizer.Text(
                    "status.manual_teleport_failed", "Manual teleport failed");
            }
        } else if (NavigationReady(context.navigation)) {
            double player_position[3]{};
            const bool have_player =
                SnapshotPlayerPosition(context, player_position);
            bool landmark_started = false;
            if (have_player) {
                context.landmark_transfer_attempted = false;
                context.landmark_transfer_wait = false;
                context.landmark_arrival_time =
                    std::chrono::steady_clock::time_point{};
                landmark_started = TryBeginLandmarkTransfer(
                    context, p, player_position[0], player_position[1]);
            }
            if (landmark_started) {
                context.manual_landmark_target[0] = p.x;
                context.manual_landmark_target[1] = p.y;
                context.manual_landmark_target[2] = p.z;
                context.manual_landmark_pending = true;
                std::lock_guard<std::mutex> lock(context.mutex);
                context.status = context.localizer.Text(
                    "status.landmark_transfer", "Fast travel");
            } else {
                context.manual_landmark_target[0] = p.x;
                context.manual_landmark_target[1] = p.y;
                context.manual_landmark_target[2] = p.z;
                if (!StartManualNavigation(
                        context, std::chrono::steady_clock::now())) {
                    std::lock_guard<std::mutex> lock(context.mutex);
                    context.status = context.localizer.Text(
                        "status.manual_navigation_failed", "Manual walk failed");
                }
            }
        } else {
            std::lock_guard<std::mutex> lock(context.mutex);
            context.status = context.localizer.Text(
                "status.navigation_unavailable_short", "Navigation unavailable");
        }
    }
    if (context.manual_landmark_pending && context.landmark_transfer_wait) {
        const auto now = std::chrono::steady_clock::now();
        double position[3]{};
        const bool have_position = SnapshotPlayerPosition(context, position);
        const bool arrived =
            have_position &&
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
            context.landmark_arrival_time =
                std::chrono::steady_clock::time_point{};
            context.manual_landmark_pending = false;
            context.landmark_transfer_attempted = false;
            StartManualNavigation(context, now);
        }
    }
    if (context.manual_navigating) {
        const auto now = std::chrono::steady_clock::now();
        double position[3]{};
        const bool have_position = SnapshotPlayerPosition(context, position);
        bool arrived = false;
        if (have_position) {
            const double distance_squared = PlanarDistanceSquared(
                position[0], position[1],
                context.manual_landmark_target[0],
                context.manual_landmark_target[1]);
            arrived = distance_squared <=
                kArrivalRadiusCentimeters * kArrivalRadiusCentimeters;
        }
        if (now >= context.manual_nav_progress_check_at) {
            bool progressed = false;
            if (have_position) {
                if (context.manual_nav_has_last_position) {
                    const double moved = std::sqrt(PlanarDistanceSquared(
                        position[0], position[1],
                        context.manual_nav_last_position[0],
                        context.manual_nav_last_position[1]));
                    progressed = moved >= kProgressThresholdCentimeters;
                }
                context.manual_nav_last_position[0] = position[0];
                context.manual_nav_last_position[1] = position[1];
                context.manual_nav_last_position[2] = position[2];
                context.manual_nav_has_last_position = true;
            }
            if (progressed) {
                context.manual_nav_last_progress_at = now;
                context.manual_nav_retry_at = now + std::chrono::milliseconds(
                    static_cast<long long>(kReissueDelaySeconds * 1000.0));
            }
            context.manual_nav_progress_check_at = now + std::chrono::milliseconds(
                static_cast<long long>(kProgressCheckIntervalSeconds * 1000.0));
        }
        if (arrived) {
            context.navigation->stop_movement(context.navigation->user);
            context.manual_navigating = false;
            std::lock_guard<std::mutex> lock(context.mutex);
            context.status = "Arrived";
        } else if (now - context.manual_nav_last_progress_at >=
                   std::chrono::milliseconds(static_cast<long long>(
                       kMovementTimeoutSeconds * 1000.0))) {
            context.navigation->stop_movement(context.navigation->user);
            context.manual_navigating = false;
            std::lock_guard<std::mutex> lock(context.mutex);
            context.status = "Walk timeout";
        } else {
            if (now >= context.manual_nav_retry_at) {
                context.manual_nav_retry_at = now + std::chrono::milliseconds(
                    static_cast<long long>(kReissueDelaySeconds * 1000.0));
                context.navigation->stop_movement(context.navigation->user);
                Point mp;
                mp.x = context.manual_landmark_target[0];
                mp.y = context.manual_landmark_target[1];
                mp.z = context.manual_landmark_target[2];
                double destination[3]{};
                ComputeApproachDestination(context, mp, destination);
                context.navigation->move_to_location(
                    context.navigation->user, destination);
            }
            if (have_position) {
                const double distance_cm = std::sqrt(PlanarDistanceSquared(
                    position[0], position[1],
                    context.manual_landmark_target[0],
                    context.manual_landmark_target[1]));
                const std::string distance_str =
                    std::to_string(static_cast<long long>(distance_cm));
                std::lock_guard<std::mutex> lock(context.mutex);
                context.status = "Walking " + distance_str + "cm";
            }
        }
    }
    if (context.running &&
        std::chrono::steady_clock::now() >= context.food_state_refresh_at) {
        context.food_state_refresh_at =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        RefreshUncollectedCatalog(context);
        RefreshPickedUpCatalog(context);
        RefreshFoodRegionState(context);
    }
    Tick(context);
    if (context.oracle != nullptr) {
        auto& oracle = *context.oracle;
        ++oracle.update_sequence;
        if (oracle.update_sequence == 0) ++oracle.update_sequence;
        if (!oracle.scan_attempted) {
            oracle_stone_impl::OracleScanCatalog(oracle);
        }
        if (oracle.scan_ready) {
            oracle_stone_impl::OracleRefreshStates(oracle);
        }
        oracle_stone_impl::OracleExecuteTeleport(oracle);
        oracle_stone_impl::OracleRunAutoTeleport(oracle, delta_seconds);
    }
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    if (!plugin_context) return;
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
        context.localizer.Text("window.title", "Resource Auto Pickup");
    anomaly::sdk::UiWindow window(ui, window_title, &open);
    if (!window) return;

    const std::string read_table_label =
        context.localizer.Text("action.read_table", "Read Table");
    if (ui->button(ui->user, anomaly::sdk::StringView(read_table_label), 0.0F, 0.0F) != 0) {
        context.read_pending.store(true, std::memory_order_release);
    }
    const std::array<std::string, 12> type_names = {
        context.localizer.Text("type.all", "All"),
        context.localizer.Text("type.hunter", "Hunter Box"),
        context.localizer.Text("type.character", "Character Box"),
        context.localizer.Text("type.chameleon", "Chameleon Box"),
        context.localizer.Text("type.furniture", "Furniture"),
        context.localizer.Text("type.prop", "Prop Box"),
        context.localizer.Text("type.box_food", "Food Box"),
        context.localizer.Text("type.item_food", "Single Food"),
        context.localizer.Text("type.prison", "Prison Resource"),
        context.localizer.Text("type.wallet", "钱包"),
        context.localizer.Text("type.oracle", "乌鸦石头"),
        context.localizer.Text("type.shop_steal", "Shop Items"),
    };
    for (std::size_t i = 0; i < type_names.size(); ++i) {
        if (ui->button(ui->user, anomaly::sdk::StringView(type_names[i]), 0.0F, 0.0F) != 0) {
            std::lock_guard<std::mutex> lock(context.mutex);
            context.type_choice = static_cast<std::uint32_t>(i);
            RebuildFilteredLocked(context);
        }
        if (i + 1 < type_names.size() && (i + 1) % 6 != 0 && ui->same_line != nullptr) {
            ui->same_line(ui->user, 0.0F, 4.0F);
        }
    }
    std::uint32_t type_choice{};
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        type_choice = context.type_choice;
    }
    if (type_choice < type_names.size()) {
        const std::array current_type_args{std::string_view(type_names[type_choice])};
        const std::string current_type = context.localizer.Format(
            "label.current_type", "Type: {0}", current_type_args);
        ui->text(ui->user, anomaly::sdk::StringView(current_type));
    }
    if (type_choice == 11) {
        const std::string requirement = context.localizer.Text(
            "status.shop_requires_zankou", "商店偷取需要使用残虹");
        ui->text(ui->user, anomaly::sdk::StringView(requirement));
    }
    const bool developer_mode = context.developer_mode.load(std::memory_order_acquire);
    const std::string mode_label = context.localizer.Text(
        developer_mode ? "mode.teleport" : "mode.navigation",
        developer_mode ? "Teleport" : "Walk");
    const std::array mode_args{std::string_view(mode_label)};
    const std::string current_mode = context.localizer.Format(
        "label.current_mode", "Mode: {0}", mode_args);
    ui->text(ui->user, anomaly::sdk::StringView(current_mode));
    if (ui->input_double != nullptr) {
        double z_offset = context.teleport_z_offset.load(std::memory_order_relaxed);
        const std::string z_offset_label =
            context.localizer.Text("label.z_offset", "Z Offset (cm)");
        if (ui->input_double(ui->user, anomaly::sdk::StringView(z_offset_label),
                             &z_offset, 10.0, 100.0)) {
            context.teleport_z_offset.store(z_offset, std::memory_order_relaxed);
        }
    }
    const std::string empty_run_label =
        context.localizer.Text("label.empty_run", "Empty points per region");
    std::uint32_t empty_run =
        context.food_empty_region_run.load(std::memory_order_relaxed);
    if (ui->input_uint32(ui->user, anomaly::sdk::StringView(empty_run_label),
                         &empty_run, 1, 20)) {
        context.food_empty_region_run.store(empty_run, std::memory_order_relaxed);
    }
    const std::string start_index_label =
        context.localizer.Text("label.start_index", "Start Index");
    ui->input_uint32(ui->user, anomaly::sdk::StringView(start_index_label),
                     &context.start_index, 1, 1);
    const std::string start_label =
        context.localizer.Text("action.start", "Start Auto Pickup");
    if (ui->button(ui->user, anomaly::sdk::StringView(start_label), 0.0F, 0.0F) != 0) {
        if (type_choice == 10 && context.oracle != nullptr) {
            context.oracle->auto_teleport.start_index.store(
                context.start_index, std::memory_order_release);
            context.oracle->auto_teleport.enabled.store(true,
                                                        std::memory_order_release);
        } else {
            context.begin_pending.store(true, std::memory_order_release);
        }
    }
    const std::string stop_label = context.localizer.Text("action.stop", "Stop");
    if (ui->button(ui->user, anomaly::sdk::StringView(stop_label), 0.0F, 0.0F) != 0) {
        if (type_choice == 10 && context.oracle != nullptr) {
            context.oracle->auto_teleport.enabled.store(false,
                                                        std::memory_order_release);
        } else {
            Stop(context);
        }
    }
    if (type_choice == 10 && context.oracle != nullptr) {
        const std::string oracle_params =
            context.localizer.Text("oracle.params", "乌鸦石头参数");
        ui->text(ui->user, anomaly::sdk::StringView(oracle_params));
        if (ui->input_uint32 != nullptr) {
        auto& oracle = *context.oracle;
        std::uint32_t delay_ms =
            oracle.auto_teleport.delay_ms.load(std::memory_order_acquire);
        const std::string delay_label =
            context.localizer.Text("oracle.delay", "时间(毫秒)");
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView(delay_label),
                             &delay_ms, 100, 500)) {
            oracle.auto_teleport.delay_ms.store(delay_ms,
                                                std::memory_order_release);
        }
        std::uint32_t offset_cm =
            oracle.auto_teleport.offset_cm.load(std::memory_order_acquire);
        const std::string offset_label =
            context.localizer.Text("oracle.offset", "距离(厘米)");
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView(offset_label),
                             &offset_cm, 100, 500)) {
            oracle.auto_teleport.offset_cm.store(offset_cm,
                                                 std::memory_order_release);
        }
        }
    }
    ui->separator(ui->user);
    if (type_choice == 10 && context.oracle != nullptr) {
        auto& oracle = *context.oracle;
        std::size_t oracle_total{};
        std::size_t oracle_uncollected{};
        {
            std::scoped_lock lock(oracle.mutex);
            oracle_total = oracle.records.size();
            for (const auto& record : oracle.records) {
                if (record.state == oracle_stone_locator::OracleStoneAvailable) {
                    ++oracle_uncollected;
                }
            }
        }
        const std::string uncollected_str = std::to_string(oracle_uncollected);
        const std::string total_str = std::to_string(oracle_total);
        const std::array oracle_args{std::string_view(uncollected_str),
                                     std::string_view(total_str)};
        const std::string oracle_progress = context.localizer.Format(
            "oracle.progress", "乌鸦石头 未获取 {0} / 总 {1}", oracle_args);
        ui->text(ui->user, anomaly::sdk::StringView(oracle_progress));
    } else {
        std::string status;
        std::size_t total{};
        std::size_t current{};
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            status = context.status;
            total = context.filtered_points.size();
            current = context.current_index;
        }
        if (!status.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView(status));
        }
        const std::string current_str = std::to_string(current);
        const std::string total_str = std::to_string(total);
        const std::array progress_args{
            std::string_view(current_str), std::string_view(total_str)};
        const std::string progress_text = context.localizer.Format(
            "label.progress", "Progress {0}/{1}", progress_args);
        ui->text(ui->user, anomaly::sdk::StringView(progress_text));
    }

    ui->separator(ui->user);
    if (type_choice == 10 && context.oracle != nullptr) {
        auto& oracle = *context.oracle;
        std::vector<oracle_stone_impl::OracleStoneRecord> available;
        {
            std::scoped_lock lock(oracle.mutex);
            for (const auto& record : oracle.records) {
                if (record.state == oracle_stone_locator::OracleStoneAvailable) {
                    available.push_back(record);
                }
            }
        }
        if (ui->begin_child != nullptr && ui->end_child != nullptr) {
            ui->begin_child(ui->user, anomaly::sdk::StringView("oracle-list"),
                            0.0F, 400.0F, 0);
            const std::string tp_label =
                context.localizer.Text("action.teleport", "TP");
            for (std::size_t i = 0; i < available.size(); ++i) {
                const std::string btn = tp_label + "##oracle" + std::to_string(i);
                if (ui->button(ui->user, anomaly::sdk::StringView(btn), 0.0F, 0.0F) !=
                    0) {
                    oracle_stone_impl::OracleQueueTeleport(
                        oracle, available[i].world_position);
                }
                if (ui->same_line != nullptr) {
                    ui->same_line(ui->user, 0.0F, 4.0F);
                }
                ui->text(ui->user,
                         anomaly::sdk::StringView(available[i].oracle_stone_id));
            }
            ui->end_child(ui->user);
        }
    } else {
    std::vector<Point> list;
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        list = context.filtered_points;
    }
    if (ui->begin_child != nullptr && ui->end_child != nullptr) {
        ui->begin_child(ui->user, anomaly::sdk::StringView("list"), 0.0F, 400.0F, 0);
        const std::string tp_label =
            context.localizer.Text("action.teleport", "TP");
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string btn = tp_label + "##" + std::to_string(i);
            if (ui->button(ui->user, anomaly::sdk::StringView(btn), 0.0F, 0.0F) != 0) {
                context.manual_teleport_x = list[i].x;
                context.manual_teleport_y = list[i].y;
                context.manual_teleport_z = list[i].z;
                context.manual_teleport_pending.store(true, std::memory_order_release);
            }
            if (ui->same_line != nullptr) {
                ui->same_line(ui->user, 0.0F, 4.0F);
            }
            ui->text(ui->user, anomaly::sdk::StringView(list[i].row_name));
        }
        ui->end_child(ui->user);
    }
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.box-auto"),
        anomaly::sdk::StringView("Resource Auto Pickup"),
        anomaly::sdk::StringView("CCYellowStar"),
        anomaly::sdk::StringView("0.1.15"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
