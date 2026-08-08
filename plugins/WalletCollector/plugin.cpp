#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ue5.h"
#include "scanner_profile.hpp"
#include "plugins/common/localization.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace nte_interactbox_scanner_profile;

constexpr std::string_view kModuleName = "HTGame.exe";
constexpr std::string_view kTextSection = ".text";
constexpr std::string_view kDataAssetPath =
    "/Game/DataAssets/DataAssetSet/RandomItem/DA_RandomItem.DA_RandomItem";
constexpr std::string_view kInteractBoxPrefix = "InteractBox_";
constexpr std::string_view kPropBoxPrefix = "PropBox_";
constexpr std::string_view kLandmarkWorld = "XL_map_bigworld_test";
constexpr std::uint32_t kMaximumObjectCount = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumObjectChunks = 4096;
constexpr std::uint32_t kMaximumDataTableRows = 32768;
constexpr std::uint32_t kMaximumRecordRows = 10000;
constexpr std::uint64_t kRecordRefreshInterval = 600;
constexpr std::size_t kMaximumNameBytes = 1024;
constexpr double kArrivalRadiusCentimeters = 160.0;
constexpr double kApproachRadiusCentimeters = 100.0;
constexpr double kMaximumArrivalHeightDeltaCentimeters = 1500.0;
constexpr double kMovementTimeoutSeconds = 120.0;
constexpr double kMovementStallSeconds = 2.5;
constexpr double kMovementProgressCentimeters = 25.0;
constexpr double kNavigationRestartDelaySeconds = 0.2;
constexpr double kLandmarkTransferTimeoutSeconds = 15.0;
constexpr double kLandmarkArrivalRadiusCentimeters = 500.0;
constexpr double kSettleSeconds = 0.75;
constexpr double kPickupRetryDelaySeconds = 0.75;
constexpr double kPickupVerificationDelaySeconds = 1.0;
constexpr double kPickupTimeoutSeconds = 8.0;
constexpr double kPickupRadiusCentimeters = 200.0;
constexpr std::uint32_t kPickupMaximumItems = 1;
constexpr std::uint32_t kMaximumPickupRetries = 3;
constexpr std::uint32_t kMaximumNavigationAttempts = 5;
constexpr std::uint32_t kMaximumTwoOptPasses = 6;
constexpr std::uint32_t kMaximumMapLandmarks = 4096;
constexpr std::uint32_t kDefaultWalletTarget = 10;
constexpr std::uint32_t kMaximumWalletTarget = 500;

struct ArrayHeader final {
  std::uintptr_t data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct FNameValue final {
  std::uint32_t comparison_index{};
  std::uint32_t number{};
};

struct ObjectRegistry final {
  std::uintptr_t items{};
  std::uint32_t count{};
  std::uint32_t max_count{};
  std::uint32_t max_chunks{};
  std::uint32_t num_chunks{};
};

struct RoutePoint final {
  std::uint64_t point_id{};
  std::string row_name;
  std::string level_name;
  std::string random_type;
  std::array<double, 3> center{};
  std::string item_id;
};

struct RecordSelection final {
  std::string point_name;
  std::string item_id;
};

struct MapLandmark final {
  std::uint64_t sequence{};
  std::uint32_t index{};
  std::array<double, 3> destination{};
  std::string teleport_id;
};

enum class AutomationState : std::uint32_t {
  idle,
  waiting_for_catalog,
  planning,
  landmark_transfer_wait,
  moving,
  settling,
  pickup_checking,
  pickup_verify_delay,
  retry_delay,
  advance,
  completed,
};

enum class CollectionMode : std::uint32_t {
  navigation,
  teleport,
};

struct Context final {
  const AnomalyHostApiV1* host{};
  anomaly::plugins::Localizer localizer;
  const AnomalyCoreServiceV1* core{};
  const AnomalyUiServiceV1* ui{};
  const AnomalyNteSessionServiceV1* session{};
  const AnomalyNtePlayerServiceV1* player{};
  const AnomalyNteNavigationServiceV1* navigation{};
  const AnomalyNtePickupServiceV1* pickup{};
  const AnomalyNtePlayerTeleportServiceV1* teleport{};
  const AnomalyNteMapLandmarksServiceV1* map_landmarks{};
  const AnomalySignatureServiceV1* signature{};
  const AnomalyUe5FrameworkServiceV1* framework{};
  const AnomalyUe5NamesServiceV1* names{};
  const AnomalyUe5ObjectsServiceV1* objects{};

  std::uintptr_t g_objects_address{};
  std::uintptr_t g_world_address{};
  std::uintptr_t get_record_owner_address{};
  std::uintptr_t record_owner{};
  std::uintptr_t fixed_record{};
  std::uintptr_t dynamic_record{};
  ObjectRegistry registry{};
  std::uintptr_t data_asset{};
  std::uintptr_t data_table{};
  std::atomic_bool table_scanned{};
  std::atomic_bool data_asset_found{};
  std::atomic_bool stopping{true};
  std::atomic_bool scanning{true};
  std::uint64_t world_id{};
  std::uint64_t world_generation{};
  std::uint64_t update_count{};
  std::atomic_bool developer_mode{};

  std::mutex mutex;
  std::vector<RoutePoint> interact_boxes;
  std::vector<RoutePoint> prop_boxes;
  std::unordered_map<std::uint64_t, RoutePoint> candidate_rows;
  std::unordered_map<std::string, std::uint64_t> candidate_names;
  std::uint32_t fixed_record_rows{};
  std::uint32_t dynamic_record_rows{};
  std::atomic_bool start_requested{};
  std::atomic_bool stop_requested{};
  AutomationState automation_state{AutomationState::idle};
  CollectionMode collection_mode{CollectionMode::navigation};
  CollectionMode active_collection_mode{CollectionMode::navigation};
  std::vector<RoutePoint> route;
  std::vector<MapLandmark> landmarks;
  std::uint64_t landmarks_sequence{};
  std::size_t route_index{};
  std::uint32_t pickup_retries{};
  std::uint32_t navigation_attempt{};
  std::uint32_t collected{};
  std::uint32_t skipped{};
  std::uint32_t wallet_target{kDefaultWalletTarget};
  std::uint32_t active_wallet_target{kDefaultWalletTarget};
  std::uint64_t pickup_baseline_sequence{};
  bool pickup_verification_pending{};
  bool landmark_transfer_attempted{};
  double automation_elapsed{};
  double movement_stalled_elapsed{};
  double current_target_distance{};
  std::array<double, 3> movement_probe_position{};
  std::array<double, 3> move_destination{};
  std::array<double, 3> landmark_destination{};
  bool movement_probe_valid{};
  std::string automation_status{"Idle"};
};

template <typename Table, typename Field>
bool HasField(const Table* table, const std::size_t offset) noexcept {
  return table != nullptr && table->struct_size >= offset + sizeof(Field);
}

AnomalyStatusV1 Status(const std::uint32_t code,
                       const std::string_view message = {}) noexcept {
  return {code, 0, {message.data(), message.size()}};
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const char* id,
                     const std::uint32_t version) noexcept {
  if (!HasField<AnomalyHostApiV1,
                decltype(AnomalyHostApiV1::query_service)>(
          host, offsetof(AnomalyHostApiV1, query_service)) ||
      host->query_service == nullptr) return nullptr;
  const void* table{};
  if (host->query_service(host->host_context, anomaly::sdk::StringView(id),
                          version, &table).code != ANOMALY_STATUS_V1_OK ||
      table == nullptr) return nullptr;
  const auto* service = static_cast<const Service*>(table);
  constexpr std::size_t prefix = offsetof(Service, user) + sizeof(void*);
  return service->struct_size >= prefix && service->service_version >= version
             ? service : nullptr;
}

bool CoreReady(const AnomalyCoreServiceV1* core) noexcept {
  return HasField<AnomalyCoreServiceV1,
                  decltype(AnomalyCoreServiceV1::read_memory)>(
             core, offsetof(AnomalyCoreServiceV1, read_memory)) &&
         core->read_memory != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1* signature) noexcept {
  return HasField<AnomalySignatureServiceV1,
                  decltype(AnomalySignatureServiceV1::resolve)>(
             signature, offsetof(AnomalySignatureServiceV1, resolve)) &&
         signature->resolve != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* names) noexcept {
  return HasField<AnomalyUe5NamesServiceV1,
                  decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
             names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
         names->resolve_utf8 != nullptr;
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* objects) noexcept {
  return HasField<AnomalyUe5ObjectsServiceV1,
                  decltype(AnomalyUe5ObjectsServiceV1::find_exact)>(
             objects, offsetof(AnomalyUe5ObjectsServiceV1, find_exact)) &&
         objects->count != nullptr && objects->find_exact != nullptr;
}


bool IsCurrentWorld(const AnomalyNteSessionSnapshotV1& snapshot) noexcept {
  return snapshot.struct_size >= sizeof(snapshot) &&
         snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY &&
         snapshot.world.id != 0 && snapshot.world.generation != 0;
}

bool IsCurrentPlayer(const AnomalyNtePlayerSnapshotV1& snapshot) noexcept {
  return snapshot.struct_size >= sizeof(snapshot) &&
         (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
         (snapshot.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE |
                            ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) == 0 &&
         snapshot.handle.id != 0 && snapshot.handle.generation != 0;
}

bool IsFinitePosition(const std::array<double, 3>& position) noexcept {
  return std::ranges::all_of(position, [](const double value) {
    return std::isfinite(value);
  });
}

bool PlayerReady(const AnomalyNtePlayerServiceV1* service) noexcept {
  return HasField<AnomalyNtePlayerServiceV1,
                  decltype(AnomalyNtePlayerServiceV1::snapshot)>(
             service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) &&
         service->snapshot != nullptr;
}

bool NavigationReady(const AnomalyNteNavigationServiceV1* service) noexcept {
  return HasField<AnomalyNteNavigationServiceV1,
                  decltype(AnomalyNteNavigationServiceV1::stop_movement)>(
             service, offsetof(AnomalyNteNavigationServiceV1, stop_movement)) &&
         service->move_to_location != nullptr && service->stop_movement != nullptr;
}

bool TeleportReady(const AnomalyNtePlayerTeleportServiceV1* service) noexcept {
  return HasField<AnomalyNtePlayerTeleportServiceV1,
                  decltype(AnomalyNtePlayerTeleportServiceV1::teleport)>(
             service, offsetof(AnomalyNtePlayerTeleportServiceV1, teleport)) &&
         service->teleport != nullptr;
}

bool LandmarksReady(const AnomalyNteMapLandmarksServiceV1* service) noexcept {
  return HasField<AnomalyNteMapLandmarksServiceV1,
                  decltype(AnomalyNteMapLandmarksServiceV1::teleport)>(
             service, offsetof(AnomalyNteMapLandmarksServiceV1, teleport)) &&
         service->sequence != nullptr && service->count != nullptr &&
         service->snapshot_at != nullptr && service->teleport != nullptr;
}

bool DeveloperModeEnabled(const AnomalyUiServiceV1* ui) noexcept {
  return HasField<AnomalyUiServiceV1,
                  decltype(AnomalyUiServiceV1::developer_mode_enabled)>(
             ui, offsetof(AnomalyUiServiceV1, developer_mode_enabled)) &&
         ui->developer_mode_enabled != nullptr &&
         ui->developer_mode_enabled(ui->user) != 0;
}

bool PickupReady(const AnomalyNtePickupServiceV1* service) noexcept {
  return HasField<AnomalyNtePickupServiceV1,
                  decltype(AnomalyNtePickupServiceV1::snapshot)>(
             service, offsetof(AnomalyNtePickupServiceV1, snapshot)) &&
         service->request_nearby != nullptr && service->snapshot != nullptr;
}

const char* StatusName(const std::uint32_t code) noexcept {
  switch (code) {
    case ANOMALY_STATUS_V1_OK: return "OK";
    case ANOMALY_STATUS_V1_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "UNAVAILABLE";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "NOT_FOUND";
    case ANOMALY_STATUS_V1_FAILED: return "FAILED";
    case ANOMALY_STATUS_V1_TIMEOUT: return "TIMEOUT";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ANOMALY_STATUS_V1_CONFLICT: return "CONFLICT";
    case ANOMALY_STATUS_V1_CANCELLED: return "CANCELLED";
    default: return "UNKNOWN_STATUS";
  }
}


std::string AutomationStateName(const Context& context,
                                const AutomationState state) {
  switch (state) {
    case AutomationState::waiting_for_catalog:
      return context.localizer.Text("state.waiting", "Waiting");
    case AutomationState::planning:
      return context.localizer.Text("state.planning", "Planning");
    case AutomationState::landmark_transfer_wait:
      return context.localizer.Text("state.landmark_transfer", "Transferring");
    case AutomationState::moving:
      return context.localizer.Text("state.moving", "Moving");
    case AutomationState::settling:
      return context.localizer.Text("state.settling", "Settling");
    case AutomationState::pickup_checking:
      return context.localizer.Text("state.checking", "Checking pickup");
    case AutomationState::pickup_verify_delay:
      return context.localizer.Text("state.verifying", "Verifying pickup");
    case AutomationState::retry_delay:
      return context.localizer.Text("state.retrying", "Retrying pickup");
    case AutomationState::advance:
      return context.localizer.Text("state.advancing", "Advancing");
    case AutomationState::completed:
      return context.localizer.Text("state.completed", "Completed");
    default:
      return context.localizer.Text("state.idle", "Idle");
  }
}

std::string CollectionModeName(const Context& context,
                               const CollectionMode mode) {
  return mode == CollectionMode::teleport
      ? context.localizer.Text("mode.teleport", "Teleport pickup")
      : context.localizer.Text("mode.navigation", "Navigation pickup");
}

std::string RouteCompletionDetail(const Context& context) {
  const std::string collected = std::to_string(context.collected);
  const std::string target = std::to_string(context.active_wallet_target);
  const std::array arguments{
      std::string_view(collected), std::string_view(target)};
  return context.localizer.Format(
      context.collected >= context.active_wallet_target
          ? "detail.goal_reached"
          : "detail.route_exhausted",
      context.collected >= context.active_wallet_target
          ? "Wallet target reached: {0}/{1}"
          : "Route exhausted: {0}/{1} wallets collected",
      arguments);
}

double PlanarDistance(const std::array<double, 3>& left,
                      const std::array<double, 3>& right) noexcept {
  return std::hypot(left[0] - right[0], left[1] - right[1]);
}

std::array<double, 3> ApproachDestination(
    const std::array<double, 3>& player,
    const RoutePoint& target,
    const std::uint32_t attempt) noexcept {
  std::array<double, 3> destination = target.center;
  if (attempt == 0) {
    const double dx = player[0] - target.center[0];
    const double dy = player[1] - target.center[1];
    const double length = std::hypot(dx, dy);
    if (length > 1.0) {
      destination[0] += dx * kApproachRadiusCentimeters / length;
      destination[1] += dy * kApproachRadiusCentimeters / length;
      return destination;
    }
  }
  static constexpr std::array<std::array<double, 2>, 4> offsets{{
      {kApproachRadiusCentimeters, 0.0},
      {-kApproachRadiusCentimeters, 0.0},
      {0.0, kApproachRadiusCentimeters},
      {0.0, -kApproachRadiusCentimeters},
  }};
  const auto& offset = offsets[(attempt == 0 ? 0 : attempt - 1U) % offsets.size()];
  destination[0] += offset[0];
  destination[1] += offset[1];
  return destination;
}

std::vector<RoutePoint> PlanRoute(
    const std::array<double, 3>& origin,
    std::vector<RoutePoint> remaining) {
  std::erase_if(remaining, [](const RoutePoint& point) {
    return !IsFinitePosition(point.center);
  });
  std::vector<RoutePoint> route;
  route.reserve(remaining.size());
  std::array<double, 3> cursor = origin;
  while (!remaining.empty()) {
    const auto nearest = std::min_element(
        remaining.begin(), remaining.end(), [&cursor](const RoutePoint& left,
                                                       const RoutePoint& right) {
          const double left_distance = PlanarDistance(cursor, left.center);
          const double right_distance = PlanarDistance(cursor, right.center);
          if (left_distance != right_distance)
            return left_distance < right_distance;
          return left.point_id < right.point_id;
        });
    cursor = nearest->center;
    route.push_back(std::move(*nearest));
    remaining.erase(nearest);
  }

  for (std::uint32_t pass{};
       pass < kMaximumTwoOptPasses && route.size() > 2; ++pass) {
    bool improved{};
    for (std::size_t first{}; first + 1 < route.size(); ++first) {
      const auto& previous = first == 0 ? origin : route[first - 1].center;
      for (std::size_t last = first + 1; last < route.size(); ++last) {
        const double before = PlanarDistance(previous, route[first].center) +
            (last + 1 < route.size()
                 ? PlanarDistance(route[last].center, route[last + 1].center)
                 : 0.0);
        const double after = PlanarDistance(previous, route[last].center) +
            (last + 1 < route.size()
                 ? PlanarDistance(route[first].center, route[last + 1].center)
                 : 0.0);
        if (after + 1.0 < before) {
          std::reverse(route.begin() + static_cast<std::ptrdiff_t>(first),
                       route.begin() + static_cast<std::ptrdiff_t>(last + 1));
          improved = true;
        }
      }
    }
    if (!improved) break;
  }
  return route;
}

bool SnapshotPlayer(Context& context,
                    AnomalyNtePlayerSnapshotV1& snapshot) noexcept {
  if (!PlayerReady(context.player)) return false;
  snapshot = {sizeof(snapshot)};
  if (context.player->snapshot(context.player->user, &snapshot).code !=
          ANOMALY_STATUS_V1_OK ||
      !IsCurrentPlayer(snapshot))
    return false;
  return true;
}

bool SnapshotPlayer(Context& context, std::array<double, 3>& position) noexcept {
  AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
  if (!SnapshotPlayer(context, snapshot)) return false;
  std::ranges::copy(snapshot.position, position.begin());
  return IsFinitePosition(position);
}

void StopAutomationMovement(Context& context) noexcept {
  if (NavigationReady(context.navigation))
    static_cast<void>(context.navigation->stop_movement(context.navigation->user));
}

bool RefreshLandmarkCatalog(Context& context) {
  if (!LandmarksReady(context.map_landmarks)) {
    context.map_landmarks = Query<AnomalyNteMapLandmarksServiceV1>(
        context.host, ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID,
        ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION);
  }
  const auto* service = context.map_landmarks;
  if (!LandmarksReady(service)) return false;

  const std::uint64_t sequence = service->sequence(service->user);
  if (sequence == 0) return false;
  {
    std::scoped_lock lock(context.mutex);
    if (context.landmarks_sequence == sequence) return true;
  }

  const std::uint32_t count = service->count(service->user);
  if (count > kMaximumMapLandmarks) return false;
  std::vector<MapLandmark> landmarks;
  landmarks.reserve(count);
  for (std::uint32_t index{}; index < count; ++index) {
    AnomalyNteMapLandmarkSnapshotV1 snapshot{sizeof(snapshot)};
    if (service->snapshot_at(service->user, index, &snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        snapshot.sequence != sequence ||
        (snapshot.flags & ANOMALY_NTE_MAP_LANDMARK_V1_VALID) == 0)
      return false;
    if (std::string_view(snapshot.world) != kLandmarkWorld) continue;
    MapLandmark landmark;
    landmark.sequence = sequence;
    landmark.index = index;
    std::ranges::copy(snapshot.destination, landmark.destination.begin());
    if (!IsFinitePosition(landmark.destination)) continue;
    landmark.teleport_id = snapshot.teleport_id;
    landmarks.push_back(std::move(landmark));
  }
  if (service->sequence(service->user) != sequence) return false;

  std::scoped_lock lock(context.mutex);
  context.landmarks_sequence = sequence;
  context.landmarks = std::move(landmarks);
  return true;
}

bool TryBeginLandmarkTransfer(Context& context,
                              const std::array<double, 3>& player_position,
                              const RoutePoint& target) {
  {
    std::scoped_lock lock(context.mutex);
    if (context.landmark_transfer_attempted) return false;
    context.landmark_transfer_attempted = true;
  }
  if (!RefreshLandmarkCatalog(context)) return false;

  std::vector<MapLandmark> landmarks;
  {
    std::scoped_lock lock(context.mutex);
    landmarks = context.landmarks;
  }
  if (landmarks.empty()) return false;
  const auto nearest = std::min_element(
      landmarks.begin(), landmarks.end(), [&target](const MapLandmark& left,
                                                     const MapLandmark& right) {
        return PlanarDistance(left.destination, target.center) <
               PlanarDistance(right.destination, target.center);
      });
  const double direct_distance = PlanarDistance(player_position, target.center);
  const double landmark_distance =
      PlanarDistance(nearest->destination, target.center);
  if (!(direct_distance > landmark_distance)) return false;

  const auto* service = context.map_landmarks;
  if (!LandmarksReady(service) ||
      service->sequence(service->user) != nearest->sequence)
    return false;
  AnomalyNteMapLandmarkTeleportRequestV1 request{sizeof(request)};
  request.mode = ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL;
  request.sequence = nearest->sequence;
  request.index = nearest->index;
  const AnomalyStatusV1 status = service->teleport(service->user, &request);
  if (status.code != ANOMALY_STATUS_V1_OK) return false;

  std::scoped_lock lock(context.mutex);
  context.landmark_destination = nearest->destination;
  context.automation_elapsed = 0.0;
  context.current_target_distance = direct_distance;
  context.movement_probe_valid = false;
  context.automation_state = AutomationState::landmark_transfer_wait;
  const std::string landmark_name = nearest->teleport_id.empty()
      ? std::to_string(nearest->index) : nearest->teleport_id;
  const std::array arguments{
      std::string_view(landmark_name), std::string_view(target.row_name)};
  context.automation_status = context.localizer.Format(
      "detail.landmark_transfer", "Transferring via {0} for {1}", arguments);
  return true;
}

void FailPickup(Context& context, const std::string_view reason_key,
                const std::string_view reason_fallback) {
  std::scoped_lock lock(context.mutex);
  const std::string reason =
      context.localizer.Text(reason_key, reason_fallback);
  context.automation_elapsed = 0.0;
  context.pickup_verification_pending = false;
  if (context.pickup_retries < kMaximumPickupRetries) {
    ++context.pickup_retries;
    context.automation_state = AutomationState::retry_delay;
    const std::string retries = std::to_string(context.pickup_retries);
    const std::string maximum = std::to_string(kMaximumPickupRetries);
    const std::array arguments{
        std::string_view(reason), std::string_view(retries),
        std::string_view(maximum)};
    context.automation_status = context.localizer.Format(
        "detail.retry", "{0}; retry {1}/{2}", arguments);
    return;
  }
  ++context.skipped;
  context.automation_state = AutomationState::advance;
  const std::array arguments{std::string_view(reason)};
  context.automation_status = context.localizer.Format(
      "detail.skipped", "{0}; skipped", arguments);
}

void BeginTeleport(Context& context, const RoutePoint& target) {
  const auto skip_target = [&context](const std::string_view key,
                                       const std::string_view fallback) {
    std::scoped_lock lock(context.mutex);
    ++context.skipped;
    context.automation_state = AutomationState::advance;
    context.automation_status = context.localizer.Text(key, fallback);
  };

  const auto* session = context.session;
  if (!HasField<AnomalyNteSessionServiceV1,
                decltype(AnomalyNteSessionServiceV1::snapshot)>(
          session, offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
      session->snapshot == nullptr) {
    session = Query<AnomalyNteSessionServiceV1>(
        context.host, ANOMALY_NTE_SESSION_SERVICE_V1_ID,
        ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    context.session = session;
  }
  if (session == nullptr || session->snapshot == nullptr) {
    skip_target("detail.teleport_unavailable", "Teleport service unavailable");
    return;
  }

  AnomalyNteSessionSnapshotV1 world{sizeof(world)};
  if (session->snapshot(session->user, &world).code != ANOMALY_STATUS_V1_OK ||
      !IsCurrentWorld(world)) {
    skip_target("detail.teleport_world_unavailable",
                "Current world handle unavailable");
    return;
  }

  AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
  if (!SnapshotPlayer(context, player) || !TeleportReady(context.teleport)) {
    skip_target("detail.teleport_unavailable", "Teleport service unavailable");
    return;
  }

  AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
  request.world = world.world;
  request.player = player.handle;
  std::ranges::copy(target.center, request.position);
  const AnomalyStatusV1 status =
      context.teleport->teleport(context.teleport->user, &request);

  std::scoped_lock lock(context.mutex);
  context.automation_elapsed = 0.0;
  context.movement_stalled_elapsed = 0.0;
  context.current_target_distance = 0.0;
  context.movement_probe_valid = false;
  if (status.code == ANOMALY_STATUS_V1_OK) {
    context.automation_state = AutomationState::settling;
    const std::array arguments{std::string_view(target.row_name)};
    context.automation_status = context.localizer.Format(
        "detail.teleporting", "Teleported to {0}", arguments);
    return;
  }

  ++context.skipped;
  context.automation_state = AutomationState::advance;
  const std::string status_name = StatusName(status.code);
  const std::array arguments{
      std::string_view(target.row_name), std::string_view(status_name)};
  context.automation_status = context.localizer.Format(
      "detail.teleport_rejected", "Teleport rejected at {0} ({1})", arguments);
}

void BeginMove(Context& context) {
  RoutePoint target;
  std::uint32_t navigation_attempt{};
  CollectionMode mode{};
  {
    std::scoped_lock lock(context.mutex);
    if (context.route_index >= context.route.size()) {
      context.automation_state = AutomationState::completed;
      context.automation_status = RouteCompletionDetail(context);
      return;
    }
    target = context.route[context.route_index];
    navigation_attempt = context.navigation_attempt;
    mode = context.active_collection_mode;
  }
  if (mode == CollectionMode::teleport &&
      !context.developer_mode.load(std::memory_order_acquire)) {
    mode = CollectionMode::navigation;
    std::scoped_lock lock(context.mutex);
    context.active_collection_mode = mode;
  }
  if (mode == CollectionMode::teleport) {
    BeginTeleport(context, target);
    return;
  }
  if (!NavigationReady(context.navigation)) {
    std::scoped_lock lock(context.mutex);
    ++context.skipped;
    context.automation_state = AutomationState::advance;
    context.automation_status = context.localizer.Text(
        "detail.navigation_unavailable", "Navigation service unavailable");
    return;
  }
  std::array<double, 3> player_position{};
  if (!SnapshotPlayer(context, player_position)) {
    std::scoped_lock lock(context.mutex);
    ++context.skipped;
    context.automation_state = AutomationState::advance;
    context.automation_status = context.localizer.Text(
        "detail.player_unavailable", "Player snapshot unavailable; skipped");
    return;
  }
  if (TryBeginLandmarkTransfer(context, player_position, target)) return;
  const auto destination =
      ApproachDestination(player_position, target, navigation_attempt);
  const AnomalyStatusV1 status = context.navigation->move_to_location(
      context.navigation->user, destination.data());
  std::scoped_lock lock(context.mutex);
  context.automation_elapsed = 0.0;
  context.movement_stalled_elapsed = 0.0;
  context.current_target_distance = PlanarDistance(player_position, target.center);
  context.movement_probe_position = player_position;
  context.move_destination = destination;
  context.movement_probe_valid = true;
  if (status.code == ANOMALY_STATUS_V1_OK) {
    context.automation_state = AutomationState::moving;
    const std::array arguments{std::string_view(target.row_name)};
    context.automation_status = context.localizer.Format(
        "detail.moving", "Moving near {0}", arguments);
    return;
  }
  ++context.skipped;
  context.automation_state = AutomationState::advance;
  const std::string status_name = StatusName(status.code);
  const std::array arguments{
      std::string_view(target.row_name), std::string_view(status_name)};
  context.automation_status = context.localizer.Format(
      "detail.navigation_rejected", "Navigation rejected {0} ({1})", arguments);
}

void BeginPickup(Context& context) {
  AnomalyNtePickupSnapshotV1 baseline{sizeof(baseline)};
  const AnomalyStatusV1 baseline_status =
      context.pickup->snapshot(context.pickup->user, &baseline);
  if (baseline_status.code != ANOMALY_STATUS_V1_OK) {
    FailPickup(context, "detail.pickup_snapshot_unavailable",
               "Pickup snapshot unavailable");
    return;
  }
  AnomalyNtePickupRequestV1 request{sizeof(request)};
  request.radius = kPickupRadiusCentimeters;
  request.maximum_items = kPickupMaximumItems;
  const AnomalyStatusV1 request_status =
      context.pickup->request_nearby(context.pickup->user, &request);
  if (request_status.code != ANOMALY_STATUS_V1_OK) {
    FailPickup(context, "detail.pickup_request_rejected",
               "Pickup request rejected");
    return;
  }
  std::scoped_lock lock(context.mutex);
  context.pickup_baseline_sequence = baseline.sequence;
  context.automation_elapsed = 0.0;
  context.automation_state = AutomationState::pickup_checking;
  const std::array arguments{
      std::string_view(context.route[context.route_index].row_name)};
  context.automation_status = context.localizer.Format(
      context.pickup_verification_pending ? "detail.rechecking"
                                          : "detail.checking",
      context.pickup_verification_pending ? "Rechecking wallet at {0}"
                                          : "Checking wallet at {0}",
      arguments);
}

void ResetAutomationForWorld(Context& context) noexcept {
  StopAutomationMovement(context);
  std::scoped_lock lock(context.mutex);
  const bool was_active = context.automation_state != AutomationState::idle &&
      context.automation_state != AutomationState::completed;
  context.route.clear();
  context.landmarks.clear();
  context.landmarks_sequence = 0;
  context.route_index = 0;
  context.pickup_retries = 0;
  context.navigation_attempt = 0;
  context.pickup_baseline_sequence = 0;
  context.pickup_verification_pending = false;
  context.landmark_transfer_attempted = false;
  context.landmark_destination = {};
  context.automation_elapsed = 0.0;
  context.movement_stalled_elapsed = 0.0;
  context.current_target_distance = 0.0;
  context.movement_probe_valid = false;
  context.automation_state = was_active ? AutomationState::waiting_for_catalog
                                        : AutomationState::idle;
  context.automation_status = context.localizer.Text(
      was_active ? "detail.world_changed" : "detail.idle",
      was_active ? "World changed; waiting for scan" : "Idle");
}

void ProcessAutomation(Context& context, double delta_seconds) {
  if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) delta_seconds = 0.0;
  delta_seconds = (std::min)(delta_seconds, 1.0);

  if (context.stop_requested.exchange(false, std::memory_order_acq_rel)) {
    StopAutomationMovement(context);
    std::scoped_lock lock(context.mutex);
    context.route.clear();
    context.route_index = 0;
    context.pickup_retries = 0;
    context.navigation_attempt = 0;
    context.pickup_baseline_sequence = 0;
    context.pickup_verification_pending = false;
    context.landmark_transfer_attempted = false;
    context.landmark_destination = {};
    context.automation_elapsed = 0.0;
    context.movement_stalled_elapsed = 0.0;
    context.current_target_distance = 0.0;
    context.movement_probe_valid = false;
    context.automation_state = AutomationState::idle;
    context.automation_status =
        context.localizer.Text("detail.stopped", "Stopped");
  }
  if (context.start_requested.exchange(false, std::memory_order_acq_rel)) {
    StopAutomationMovement(context);
    std::scoped_lock lock(context.mutex);
    context.route.clear();
    context.route_index = 0;
    context.pickup_retries = 0;
    context.navigation_attempt = 0;
    context.collected = 0;
    context.skipped = 0;
    context.active_wallet_target = (std::clamp)(
        context.wallet_target, 1U, kMaximumWalletTarget);
    context.active_collection_mode =
        context.collection_mode == CollectionMode::teleport &&
            context.developer_mode.load(std::memory_order_acquire)
        ? CollectionMode::teleport
        : CollectionMode::navigation;
    context.pickup_baseline_sequence = 0;
    context.pickup_verification_pending = false;
    context.landmark_transfer_attempted = false;
    context.landmark_destination = {};
    context.automation_elapsed = 0.0;
    context.movement_stalled_elapsed = 0.0;
    context.current_target_distance = 0.0;
    context.movement_probe_valid = false;
    context.automation_state = AutomationState::waiting_for_catalog;
    context.automation_status = context.localizer.Text(
        "detail.waiting_scan", "Waiting for wallet scan");
  }

  AutomationState state{};
  {
    std::scoped_lock lock(context.mutex);
    state = context.automation_state;
  }
  if (state == AutomationState::idle || state == AutomationState::completed)
    return;

  if (state == AutomationState::waiting_for_catalog) {
    std::vector<RoutePoint> points;
    {
      std::scoped_lock lock(context.mutex);
      points = context.interact_boxes;
    }
    if (!context.table_scanned.load(std::memory_order_acquire) || points.empty())
      return;
    std::array<double, 3> origin{};
    if (!SnapshotPlayer(context, origin)) {
      std::scoped_lock lock(context.mutex);
      context.automation_status = context.localizer.Text(
          "detail.waiting_player", "Waiting for player snapshot");
      return;
    }
    {
      std::scoped_lock lock(context.mutex);
      context.automation_state = AutomationState::planning;
      context.automation_status =
          context.localizer.Text("detail.planning", "Planning route");
    }
    auto route = PlanRoute(origin, std::move(points));
    {
      std::scoped_lock lock(context.mutex);
      context.route = std::move(route);
      context.route_index = 0;
      context.landmark_transfer_attempted = false;
      context.landmark_destination = {};
      context.automation_elapsed = 0.0;
      context.navigation_attempt = 0;
      context.movement_stalled_elapsed = 0.0;
      context.current_target_distance = 0.0;
      context.movement_probe_valid = false;
      if (context.route.empty()) {
        context.automation_state = AutomationState::completed;
        context.automation_status = context.localizer.Text(
            "detail.no_points", "No wallet points");
        return;
      }
      context.automation_state = AutomationState::planning;
      const std::string route_size = std::to_string(context.route.size());
      const std::array arguments{std::string_view(route_size)};
      context.automation_status = context.localizer.Format(
          "detail.route_ready", "Route ready: {0} points", arguments);
    }
    return;
  }

  if (state == AutomationState::planning) {
    bool ready{};
    {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed += delta_seconds;
      ready = context.automation_elapsed >= kNavigationRestartDelaySeconds;
    }
    if (ready) {
      StopAutomationMovement(context);
      BeginMove(context);
    }
    return;
  }

  if (state == AutomationState::landmark_transfer_wait) {
    std::array<double, 3> player_position{};
    std::array<double, 3> destination{};
    double elapsed{};
    {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed += delta_seconds;
      elapsed = context.automation_elapsed;
      destination = context.landmark_destination;
    }
    const bool arrived = SnapshotPlayer(context, player_position) &&
        PlanarDistance(player_position, destination) <=
            kLandmarkArrivalRadiusCentimeters;
    if (arrived || elapsed >= kLandmarkTransferTimeoutSeconds) {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed = 0.0;
      context.automation_state = AutomationState::planning;
      context.automation_status = context.localizer.Text(
          arrived ? "detail.landmark_arrived" : "detail.landmark_timeout",
          arrived ? "Landmark transfer complete; continuing by navigation"
                  : "Landmark transfer timed out; continuing by navigation");
    }
    return;
  }

  if (state == AutomationState::advance) {
    bool finished{};
    {
      std::scoped_lock lock(context.mutex);
      ++context.route_index;
      context.pickup_retries = 0;
      context.navigation_attempt = 0;
      context.pickup_baseline_sequence = 0;
      context.pickup_verification_pending = false;
      context.landmark_transfer_attempted = false;
      context.landmark_destination = {};
      context.automation_elapsed = 0.0;
      context.movement_stalled_elapsed = 0.0;
      context.current_target_distance = 0.0;
      context.movement_probe_valid = false;
      finished = context.route_index >= context.route.size();
      if (finished) {
        context.automation_state = AutomationState::completed;
        context.automation_status = RouteCompletionDetail(context);
      } else {
        context.automation_state = AutomationState::planning;
        context.automation_status = context.localizer.Text(
            "detail.planning", "Planning route");
      }
    }
    return;
  }

  if (state == AutomationState::moving) {
    std::array<double, 3> player_position{};
    RoutePoint target;
    double elapsed{};
    {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed += delta_seconds;
      elapsed = context.automation_elapsed;
      if (context.route_index >= context.route.size()) return;
      target = context.route[context.route_index];
    }
    bool stalled{};
    if (SnapshotPlayer(context, player_position)) {
      const double planar = PlanarDistance(player_position, target.center);
      const double height = std::abs(player_position[2] - target.center[2]);
      {
        std::scoped_lock lock(context.mutex);
        context.current_target_distance = planar;
        if (!context.movement_probe_valid ||
            PlanarDistance(player_position, context.movement_probe_position) >=
                kMovementProgressCentimeters) {
          context.movement_probe_position = player_position;
          context.movement_probe_valid = true;
          context.movement_stalled_elapsed = 0.0;
        } else {
          context.movement_stalled_elapsed += delta_seconds;
        }
        stalled = context.movement_stalled_elapsed >= kMovementStallSeconds;
      }
      if (planar <= kArrivalRadiusCentimeters &&
          height <= kMaximumArrivalHeightDeltaCentimeters) {
        StopAutomationMovement(context);
        std::scoped_lock lock(context.mutex);
        context.automation_elapsed = 0.0;
        context.automation_state = AutomationState::settling;
        const std::array arguments{std::string_view(target.row_name)};
        context.automation_status = context.localizer.Format(
            "detail.arrived", "Arrived at {0}", arguments);
        return;
      }
    } else {
      std::scoped_lock lock(context.mutex);
      context.movement_stalled_elapsed += delta_seconds;
      stalled = context.movement_stalled_elapsed >= kMovementStallSeconds;
    }
    if (stalled) {
      StopAutomationMovement(context);
      bool retry{};
      {
        std::scoped_lock lock(context.mutex);
        ++context.navigation_attempt;
        context.automation_elapsed = 0.0;
        context.movement_stalled_elapsed = 0.0;
        context.movement_probe_valid = false;
        retry = context.navigation_attempt < kMaximumNavigationAttempts;
        if (retry) {
          context.automation_state = AutomationState::planning;
          const std::array arguments{std::string_view(target.row_name)};
          context.automation_status = context.localizer.Format(
              "detail.try_approach", "Trying another approach to {0}",
              arguments);
        } else {
          ++context.skipped;
          context.automation_state = AutomationState::advance;
          const std::array arguments{std::string_view(target.row_name)};
          context.automation_status = context.localizer.Format(
              "detail.no_approach", "No navigable approach to {0}; skipped",
              arguments);
        }
      }
      return;
    }
    if (elapsed >= kMovementTimeoutSeconds) {
      StopAutomationMovement(context);
      std::scoped_lock lock(context.mutex);
      ++context.skipped;
      context.automation_elapsed = 0.0;
      context.automation_state = AutomationState::advance;
      const std::array arguments{std::string_view(target.row_name)};
      context.automation_status = context.localizer.Format(
          "detail.navigation_timeout", "Navigation timeout at {0}; skipped",
          arguments);
    }
    return;
  }

  if (state == AutomationState::settling) {
    bool ready{};
    {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed += delta_seconds;
      ready = context.automation_elapsed >= kSettleSeconds;
    }
    if (ready) BeginPickup(context);
    return;
  }

  if (state == AutomationState::retry_delay) {
    bool ready{};
    {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed += delta_seconds;
      ready = context.automation_elapsed >= kPickupRetryDelaySeconds;
    }
    if (ready) BeginPickup(context);
    return;
  }

  if (state == AutomationState::pickup_verify_delay) {
    bool ready{};
    {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed += delta_seconds;
      ready = context.automation_elapsed >= kPickupVerificationDelaySeconds;
    }
    if (ready) BeginPickup(context);
    return;
  }

  if (state == AutomationState::pickup_checking) {
    std::uint64_t baseline_sequence{};
    double elapsed{};
    {
      std::scoped_lock lock(context.mutex);
      context.automation_elapsed += delta_seconds;
      elapsed = context.automation_elapsed;
      baseline_sequence = context.pickup_baseline_sequence;
    }
    AnomalyNtePickupSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status =
        context.pickup->snapshot(context.pickup->user, &snapshot);
    if (status.code == ANOMALY_STATUS_V1_OK &&
        (snapshot.flags & ANOMALY_NTE_PICKUP_V1_VALID) != 0 &&
        snapshot.sequence > baseline_sequence &&
        snapshot.state == ANOMALY_NTE_PICKUP_V1_COMPLETE) {
      bool verification_pending{};
      {
        std::scoped_lock lock(context.mutex);
        verification_pending = context.pickup_verification_pending;
      }
      const bool empty_after_confirmation = verification_pending &&
          snapshot.status == ANOMALY_STATUS_V1_OK && snapshot.nearby == 0 &&
          snapshot.triggered == 0 && snapshot.confirmed == 0 &&
          snapshot.unconfirmed == 0;
      if (empty_after_confirmation) {
        std::scoped_lock lock(context.mutex);
        ++context.collected;
        context.pickup_verification_pending = false;
        context.automation_elapsed = 0.0;
        if (context.collected >= context.active_wallet_target) {
          context.automation_state = AutomationState::completed;
          context.automation_status = RouteCompletionDetail(context);
        } else {
          context.automation_state = AutomationState::advance;
          context.automation_status = context.localizer.Text(
              "detail.pickup_verified", "Wallet pickup verified");
        }
      } else if (snapshot.status == ANOMALY_STATUS_V1_OK &&
                 snapshot.confirmed > 0) {
        std::scoped_lock lock(context.mutex);
        context.automation_elapsed = 0.0;
        if (context.pickup_verification_pending) {
          if (context.pickup_retries + 1U >= kMaximumPickupRetries) {
            ++context.skipped;
            context.pickup_verification_pending = false;
            context.automation_state = AutomationState::advance;
            context.automation_status = context.localizer.Text(
                "detail.pickup_remained",
                "Wallet remained after 3 retries; skipped");
          } else {
            ++context.pickup_retries;
            context.automation_state = AutomationState::pickup_verify_delay;
            const std::string retries =
                std::to_string(context.pickup_retries);
            const std::string maximum =
                std::to_string(kMaximumPickupRetries);
            const std::array arguments{
                std::string_view(retries), std::string_view(maximum)};
            context.automation_status = context.localizer.Format(
                "detail.pickup_still_present",
                "Wallet still present; retry {0}/{1}", arguments);
          }
        } else {
          context.pickup_verification_pending = true;
          context.automation_state = AutomationState::pickup_verify_delay;
          context.automation_status = context.localizer.Text(
              "detail.pickup_waiting_disappear",
              "Waiting for wallet disappearance");
        }
      } else {
        FailPickup(context, "detail.pickup_not_confirmed",
                   "Pickup not confirmed");
      }
      return;
    }
    if (elapsed >= kPickupTimeoutSeconds)
      FailPickup(context, "detail.pickup_timeout",
                 "Pickup verification timeout");
  }
}

template <typename T>
bool Read(Context& context, const std::uintptr_t address, T& value) noexcept {
  if (!CoreReady(context.core) || address == 0) return false;
  AnomalyMutableByteSpanV1 destination{
      reinterpret_cast<std::uint8_t*>(&value), sizeof(value)};
  return context.core->read_memory(context.core->user, address, destination).code ==
         ANOMALY_STATUS_V1_OK;
}

bool ReadBytes(Context& context, const std::uintptr_t address, void* destination,
               const std::size_t size) noexcept {
  if (!CoreReady(context.core) || address == 0 || destination == nullptr || size == 0)
    return false;
  AnomalyMutableByteSpanV1 output{
      static_cast<std::uint8_t*>(destination), size};
  return context.core->read_memory(context.core->user, address, output).code ==
         ANOMALY_STATUS_V1_OK;
}

bool AddAddress(const std::uintptr_t base, const std::uint64_t offset,
                std::uintptr_t& result) noexcept {
  if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base)
    return false;
  result = base + static_cast<std::uintptr_t>(offset);
  return true;
}

bool AddSignedAddress(const std::uintptr_t base, const std::ptrdiff_t offset,
                      std::uintptr_t& result) noexcept {
  if (offset < 0) {
    const auto magnitude = static_cast<std::uintptr_t>(-(offset + 1)) + 1U;
    if (base <= magnitude) return false;
    result = base - magnitude;
    return true;
  }
  return AddAddress(base, static_cast<std::uint64_t>(offset), result);
}

bool ReadPointerAt(Context& context, const std::uintptr_t base,
                   const std::ptrdiff_t offset, std::uintptr_t& value) noexcept {
  std::uintptr_t address{};
  return AddSignedAddress(base, offset, address) && Read(context, address, value) &&
         value != 0;
}

bool ResolveSignature(Context& context, const std::string_view pattern,
                      std::uintptr_t& address) noexcept {
  address = 0;
  return SignatureReady(context.signature) &&
         context.signature->resolve(
             context.signature->user, anomaly::sdk::StringView(kModuleName),
             anomaly::sdk::StringView(kTextSection), anomaly::sdk::StringView(pattern),
             &address).code == ANOMALY_STATUS_V1_OK && address != 0;
}

bool ResolveRipRelative(Context& context, const std::string_view pattern,
                        const std::ptrdiff_t addend,
                        std::uintptr_t& address) noexcept {
  std::uintptr_t instruction{};
  if (!ResolveSignature(context, pattern, instruction)) return false;
  std::int32_t displacement{};
  std::uintptr_t displacement_address{};
  if (!AddAddress(instruction, kRipDisplacementOffset, displacement_address) ||
      !Read(context, displacement_address, displacement)) return false;
  const auto resolved = static_cast<std::intptr_t>(instruction) +
      static_cast<std::intptr_t>(kRipInstructionSize) + displacement;
  return resolved > 0 && AddSignedAddress(static_cast<std::uintptr_t>(resolved),
                                           addend, address);
}

std::string ResolveName(Context& context, const std::uint32_t name_id) {
  if (!NamesReady(context.names) || name_id == 0) return {};
  std::array<char, 128> local{};
  std::size_t size = local.size();
  AnomalyStatusV1 status = context.names->resolve_utf8(
      context.names->user, name_id, local.data(), &size);
  if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= local.size())
    return std::string(local.data(), size - 1U);
  if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 ||
      size > kMaximumNameBytes) return {};
  std::string value(size, '\0');
  status = context.names->resolve_utf8(context.names->user, name_id, value.data(), &size);
  if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > value.size()) return {};
  value.resize(size - 1U);
  return value;
}

std::string RenderFName(Context& context, const FNameValue value) {
  std::string result = ResolveName(context, value.comparison_index);
  if (result.empty() || value.number == 0) return result;
  result.push_back('_');
  result += std::to_string(value.number - 1U);
  return result;
}

std::uint64_t EncodeFName(const FNameValue value) noexcept {
  return static_cast<std::uint64_t>(value.comparison_index) |
      (static_cast<std::uint64_t>(value.number) << 32U);
}

bool RefreshRegistry(Context& context) noexcept {
  if (context.g_objects_address == 0 &&
      !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                          context.g_objects_address)) return false;
  ObjectRegistry next{};
  if (!ReadPointerAt(context, context.g_objects_address, kObjectItemsOffset, next.items) ||
      !Read(context, context.g_objects_address + kObjectCountOffset, next.count) ||
      !Read(context, context.g_objects_address + kObjectMaxCountOffset, next.max_count) ||
      !Read(context, context.g_objects_address + kObjectMaxChunksOffset, next.max_chunks) ||
      !Read(context, context.g_objects_address + kObjectNumChunksOffset, next.num_chunks) ||
      next.count == 0 || next.count > kMaximumObjectCount || next.max_count < next.count ||
      next.num_chunks == 0 || next.num_chunks > next.max_chunks ||
      next.num_chunks > kMaximumObjectChunks) return false;
  const bool identity_changed = context.registry.items == 0 ||
      next.items != context.registry.items || next.count < context.registry.count ||
      next.max_count != context.registry.max_count ||
      next.max_chunks != context.registry.max_chunks ||
      next.num_chunks < context.registry.num_chunks;
  if (identity_changed) {
    context.registry = next;
    context.data_asset = 0;
    context.data_table = 0;
    context.table_scanned.store(false, std::memory_order_release);
    context.data_asset_found.store(false, std::memory_order_release);
  } else {
    context.registry = next;
  }
  return true;
}

bool ReadUtf16Array(Context& context, const std::uintptr_t address, std::string& result) {
  result.clear();
  ArrayHeader header{};
  if (!Read(context, address, header) || header.count <= 0 || header.count > 8192 ||
      header.capacity < header.count || header.data == 0) return false;
  std::vector<char16_t> value(static_cast<std::size_t>(header.count));
  if (!ReadBytes(context, header.data, value.data(), value.size() * sizeof(char16_t))) return false;
  const std::size_t length = value.back() == u'\0' ? value.size() - 1U : value.size();
  result.reserve(length);
  for (std::size_t index{}; index < length; ++index) {
    const std::uint32_t code_point = value[index];
    if (code_point <= 0x7FU) result.push_back(static_cast<char>(code_point));
    else if (code_point <= 0x7FFU) {
      result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
      result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
      result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
      result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
      result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
  }
  return true;
}

bool ReadUtf16CString(Context& context, const std::uintptr_t address, std::string& result) {
  result.clear();
  if (address == 0) return false;
  std::array<char16_t, kMaximumNameBytes / sizeof(char16_t)> value{};
  for (std::size_t offset{}; offset < value.size(); offset += 32U) {
    const auto size = (std::min)(std::size_t{32}, value.size() - offset);
    if (!ReadBytes(context, address + offset * sizeof(char16_t), value.data() + offset,
                   size * sizeof(char16_t))) return false;
    const auto end = std::find(value.begin() + static_cast<std::ptrdiff_t>(offset),
                               value.begin() + static_cast<std::ptrdiff_t>(offset + size),
                               u'\0');
    if (end != value.begin() + static_cast<std::ptrdiff_t>(offset + size)) {
      for (auto current = value.begin(); current != end; ++current) {
        const auto code_point = static_cast<std::uint32_t>(*current);
        if (code_point <= 0x7FU) result.push_back(static_cast<char>(code_point));
        else if (code_point <= 0x7FFU) {
          result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
          result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
          result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
          result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
          result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
      }
      return !result.empty();
    }
  }
  return false;
}

bool FindDataAsset(Context& context) {
  if (context.data_asset != 0) return true;
  AnomalyGenerationHandleV1 handle{};
  if (context.objects->find_exact(
          context.objects->user, anomaly::sdk::StringView(kDataAssetPath),
          &handle).code != ANOMALY_STATUS_V1_OK ||
      handle.id == 0)
    return false;

  const std::uint32_t index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
  const std::uint32_t chunk_index = index / kObjectChunkSize;
  const std::uint32_t within_chunk = index % kObjectChunkSize;
  if (index >= context.registry.count || chunk_index >= context.registry.num_chunks)
    return false;
  std::uintptr_t chunk{};
  std::uintptr_t object{};
  if (!ReadPointerAt(
          context, context.registry.items,
          static_cast<std::ptrdiff_t>(chunk_index * sizeof(void*)), chunk) ||
      !ReadPointerAt(
          context, chunk,
          static_cast<std::ptrdiff_t>(within_chunk) * kObjectItemStride, object))
    return false;
  std::uintptr_t table{};
  if (!ReadPointerAt(context, object, kDataAssetRandomItemTableOffset, table))
    return false;
  context.data_asset = object;
  context.data_table = table;
  context.data_asset_found.store(true, std::memory_order_release);
  return true;
}

bool ScanDataTable(Context& context) {
  if (context.data_table == 0 || context.table_scanned.load(std::memory_order_acquire))
    return false;
  ArrayHeader header{};
  std::uintptr_t row_map_address{};
  if (!AddSignedAddress(context.data_table, kDataTableRowMapOffset, row_map_address) ||
      !Read(context, row_map_address, header) || header.count <= 0 ||
      header.count > static_cast<std::int32_t>(kMaximumDataTableRows) ||
      header.capacity < header.count || header.data == 0) return false;
  const std::size_t byte_count = static_cast<std::size_t>(header.count) * kDataTableRowStride;
  std::vector<std::uint8_t> elements(byte_count);
  if (!ReadBytes(context, header.data, elements.data(), elements.size())) return false;

  std::vector<RoutePoint> discovered;
  discovered.reserve(static_cast<std::size_t>(header.count));
  for (std::int32_t index{}; index < header.count; ++index) {
    const std::size_t offset = static_cast<std::size_t>(index) * kDataTableRowStride;
    FNameValue row_id{};
    std::uintptr_t row{};
    std::memcpy(&row_id, elements.data() + offset, sizeof(row_id));
    std::memcpy(&row, elements.data() + offset + kDataTableRowPointerOffset, sizeof(row));
    if (row == 0 || row_id.comparison_index == 0) continue;
    std::string level_name;
    std::uintptr_t level_address{};
    if (!AddAddress(row, kRandomItemLevelNameOffset, level_address) ||
        !ReadUtf16Array(context, level_address, level_name)) continue;
    FNameValue random_type{};
    if (!Read(context, row + kRandomItemTypeNameOffset, random_type)) continue;
    std::array<double, 3> center{};
    const auto translation = row + kRandomItemTransformOffset + kTransformTranslationOffset;
    if (!Read(context, translation, center[0]) || !Read(context, translation + 8U, center[1]) ||
        !Read(context, translation + 16U, center[2]) ||
        !std::ranges::all_of(center, [](const double value) {
          return std::isfinite(value) && std::abs(value) < 10000000.0;
        })) continue;
    std::string row_name = RenderFName(context, row_id);
    std::string random_type_name = RenderFName(context, random_type);
    if (row_name.empty()) row_name = "RandomItem";
    if (random_type_name.empty()) random_type_name = "Unknown";
    discovered.push_back({EncodeFName(row_id), std::move(row_name),
                          std::move(level_name), std::move(random_type_name), center});
  }

  bool changed = false;
  {
    std::scoped_lock lock(context.mutex);
    for (auto& item : discovered) {
      const bool is_interact = item.random_type.starts_with(kInteractBoxPrefix);
      const bool is_prop = item.random_type.starts_with(kPropBoxPrefix);
      if (!is_interact && !is_prop) continue;
      const auto point_id = item.point_id;
      const auto row_name = item.row_name;
      const auto [it, inserted] = context.candidate_rows.emplace(item.point_id, std::move(item));
      if (inserted) {
        context.candidate_names.emplace(row_name, point_id);
        changed = true;
      }
    }
  }
  context.table_scanned.store(true, std::memory_order_release);
  return changed;
}

bool ResolvePlayerState(Context& context, std::uintptr_t& player_state) {
  player_state = 0;
  if (context.g_world_address == 0 &&
      !ResolveRipRelative(context, kGWorldPattern, 0, context.g_world_address)) return false;
  std::uintptr_t world{};
  std::uintptr_t game_instance{};
  std::uintptr_t local_players{};
  std::uintptr_t local_player{};
  std::uintptr_t controller{};
  return Read(context, context.g_world_address, world) && world != 0 &&
      ReadPointerAt(context, world, kWorldGameInstanceOffset, game_instance) &&
      ReadPointerAt(context, game_instance, kGameInstanceLocalPlayersOffset, local_players) &&
      Read(context, local_players, local_player) && local_player != 0 &&
      ReadPointerAt(context, local_player, kLocalPlayerControllerOffset, controller) &&
      ReadPointerAt(context, controller, kControllerPlayerStateOffset, player_state);
}

bool ResolveRandomItemRecords(Context& context, const std::uintptr_t player_state) {
  if (player_state == 0) return false;
  if (context.record_owner != 0 && context.fixed_record != 0 &&
      context.dynamic_record != 0) return false;
  if (context.get_record_owner_address == 0 &&
      !ResolveSignature(context, kGetRecordOwnerPattern,
                        context.get_record_owner_address)) return false;
  using GetRecordOwner = void* (*)(void*);
  using FindRecord = void* (*)(void*, const char*);
  auto* owner = reinterpret_cast<GetRecordOwner>(context.get_record_owner_address)(
      reinterpret_cast<void*>(player_state));
  if (owner == nullptr) return false;
  const auto owner_address = reinterpret_cast<std::uintptr_t>(owner);
  std::uintptr_t owner_vtable{};
  std::uintptr_t find_record_address{};
  if (!Read(context, owner_address, owner_vtable) || owner_vtable == 0 ||
      !Read(context, owner_vtable + kRecordOwnerFindRecordVtableOffset,
            find_record_address) || find_record_address == 0) return false;
  const auto find_record = reinterpret_cast<FindRecord>(find_record_address);
  auto* fixed = find_record(owner, "RandomItemFixedRecord");
  auto* dynamic = find_record(owner, "RandomItemDynamicRecord");
  const auto fixed_address = reinterpret_cast<std::uintptr_t>(fixed);
  const auto dynamic_address = reinterpret_cast<std::uintptr_t>(dynamic);
  bool changed{};
  {
    std::scoped_lock lock(context.mutex);
    changed = context.record_owner != owner_address ||
        context.fixed_record != fixed_address ||
        context.dynamic_record != dynamic_address;
    context.record_owner = owner_address;
    context.fixed_record = fixed_address;
    context.dynamic_record = dynamic_address;
  }
  return changed;
}

bool SameCatalog(const std::vector<RoutePoint>& left,
                 const std::vector<RoutePoint>& right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index{}; index < left.size(); ++index) {
    if (left[index].point_id != right[index].point_id ||
        left[index].item_id != right[index].item_id)
      return false;
  }
  return true;
}

bool ReadRecordSelections(Context& context, const std::uintptr_t record,
                          std::vector<RecordSelection>& selections,
                          std::uint32_t& row_count) {
  row_count = 0;
  if (record == 0) return false;
  std::uintptr_t owner{};
  std::int32_t record_index{};
  std::uintptr_t descriptors{};
  std::uintptr_t store{};
  std::uintptr_t descriptor{};
  if (!Read(context, record + kRecordOwnerOffset, owner) || owner == 0 ||
      !Read(context, record + kRecordIndexOffset, record_index) || record_index < 0 ||
      record_index > 4096 ||
      !Read(context, owner + kRecordDescriptorTableOffset, descriptors) || descriptors == 0 ||
      !AddAddress(descriptors,
                  static_cast<std::uint64_t>(record_index) * kRecordDescriptorStride,
                  descriptor) ||
      !Read(context, descriptor + kRecordDescriptorStoreOffset, store) || store == 0)
    return false;
  ArrayHeader rows{};
  if (!Read(context, store + kRecordStoreRowsOffset, rows) || rows.count < 0 ||
      rows.count > static_cast<std::int32_t>(kMaximumRecordRows) ||
      rows.capacity < rows.count ||
      rows.capacity > static_cast<std::int32_t>(kMaximumRecordRows) ||
      (rows.count != 0 && rows.data == 0)) return false;
  row_count = static_cast<std::uint32_t>(rows.count);
  if (rows.count == 0) return true;
  std::vector<std::uintptr_t> row_objects(static_cast<std::size_t>(rows.count));
  if (!ReadBytes(context, rows.data, row_objects.data(),
                 row_objects.size() * sizeof(std::uintptr_t))) return false;
  std::uintptr_t vtable{};
  std::uintptr_t get_string_address{};
  if (!Read(context, record, vtable) || vtable == 0 ||
      !Read(context, vtable + kRecordGetStringVtableOffset, get_string_address) ||
      get_string_address == 0) return false;
  using GetString = const char* (*)(void*, std::int32_t, std::int32_t);
  const auto get_string = reinterpret_cast<GetString>(get_string_address);
  for (std::int32_t row{}; row < rows.count; ++row) {
    if (row_objects[static_cast<std::size_t>(row)] == 0) continue;
    const auto* point_value =
        get_string(reinterpret_cast<void*>(record), row, 0);
    const auto* item_value =
        get_string(reinterpret_cast<void*>(record), row, 1);
    RecordSelection selection;
    if (!ReadUtf16CString(context,
                          reinterpret_cast<std::uintptr_t>(point_value),
                          selection.point_name) ||
        !ReadUtf16CString(context,
                          reinterpret_cast<std::uintptr_t>(item_value),
                          selection.item_id) ||
        selection.point_name.empty() || selection.item_id.empty())
      continue;
    selections.push_back(std::move(selection));
  }
  return true;
}

bool RefreshRecordCatalog(Context& context) {
  if (context.candidate_rows.empty() || context.fixed_record == 0 ||
      context.dynamic_record == 0) return false;
  std::vector<RecordSelection> selections;
  std::uint32_t fixed_rows{};
  std::uint32_t dynamic_rows{};
  if (!ReadRecordSelections(context, context.fixed_record, selections, fixed_rows) ||
      !ReadRecordSelections(context, context.dynamic_record, selections, dynamic_rows))
    return false;
  std::vector<RoutePoint> next_interact;
  std::vector<RoutePoint> next_prop;
  {
    std::scoped_lock lock(context.mutex);
    for (const auto& selection : selections) {
      const auto point_id = context.candidate_names.find(selection.point_name);
      if (point_id == context.candidate_names.end()) continue;
      const auto candidate = context.candidate_rows.find(point_id->second);
      if (candidate == context.candidate_rows.end()) continue;
      RoutePoint selected = candidate->second;
      selected.item_id = selection.item_id;
      if (selected.random_type.starts_with(kInteractBoxPrefix))
        next_interact.push_back(std::move(selected));
      else if (selected.random_type.starts_with(kPropBoxPrefix))
        next_prop.push_back(std::move(selected));
    }
  }
  std::sort(next_interact.begin(), next_interact.end(),
            [](const RoutePoint& left, const RoutePoint& right) {
              return left.point_id < right.point_id;
            });
  std::sort(next_prop.begin(), next_prop.end(),
            [](const RoutePoint& left, const RoutePoint& right) {
              return left.point_id < right.point_id;
            });
  next_interact.erase(
      std::unique(next_interact.begin(), next_interact.end(),
                  [](const RoutePoint& left, const RoutePoint& right) {
                    return left.point_id == right.point_id;
                  }),
      next_interact.end());
  next_prop.erase(
      std::unique(next_prop.begin(), next_prop.end(),
                  [](const RoutePoint& left, const RoutePoint& right) {
                    return left.point_id == right.point_id;
                  }),
      next_prop.end());
  bool changed{};
  {
    std::scoped_lock lock(context.mutex);
    changed = !SameCatalog(context.interact_boxes, next_interact) ||
              !SameCatalog(context.prop_boxes, next_prop) ||
              context.fixed_record_rows != fixed_rows ||
              context.dynamic_record_rows != dynamic_rows;
    if (changed) {
      context.interact_boxes = std::move(next_interact);
      context.prop_boxes = std::move(next_prop);
      context.fixed_record_rows = fixed_rows;
      context.dynamic_record_rows = dynamic_rows;
    }
  }
  return changed;
}

void ResetCatalog(Context& context, const std::uint64_t world_id,
                  const std::uint64_t generation) {
  std::scoped_lock lock(context.mutex);
  context.world_id = world_id;
  context.world_generation = generation;
  context.interact_boxes.clear();
  context.prop_boxes.clear();
  context.candidate_rows.clear();
  context.candidate_names.clear();
  context.record_owner = 0;
  context.fixed_record = 0;
  context.dynamic_record = 0;
  context.fixed_record_rows = 0;
  context.dynamic_record_rows = 0;
}

void RestartScan(Context& context) noexcept {
  context.data_asset = 0;
  context.data_table = 0;
  context.table_scanned.store(false, std::memory_order_release);
  context.data_asset_found.store(false, std::memory_order_release);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
  if (host == nullptr || plugin_context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  auto* context = new (std::nothrow) Context();
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);
  context->host = host;
  context->localizer = anomaly::plugins::Localizer(host);
  context->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
  context->ui = Query<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
  context->session = Query<AnomalyNteSessionServiceV1>(host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
  context->player = Query<AnomalyNtePlayerServiceV1>(host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
  context->navigation = Query<AnomalyNteNavigationServiceV1>(host, ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID, ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION);
  context->pickup = Query<AnomalyNtePickupServiceV1>(host, ANOMALY_NTE_PICKUP_SERVICE_V1_ID, ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION);
  context->teleport = Query<AnomalyNtePlayerTeleportServiceV1>(host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
  context->map_landmarks = Query<AnomalyNteMapLandmarksServiceV1>(host, ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID, ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION);
  context->signature = Query<AnomalySignatureServiceV1>(host, ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
  context->framework = Query<AnomalyUe5FrameworkServiceV1>(host, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION);
  context->names = Query<AnomalyUe5NamesServiceV1>(host, ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
  context->objects = Query<AnomalyUe5ObjectsServiceV1>(host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
  if (!CoreReady(context->core) || context->ui == nullptr ||
      !SignatureReady(context->signature) || !NamesReady(context->names) ||
      !ObjectsReady(context->objects)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 object services are unavailable");
  }
  if (!PlayerReady(context->player) || !PickupReady(context->pickup)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "automation services are unavailable");
  }
  *plugin_context = context;
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
  auto* context = static_cast<Context*>(plugin_context);
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  context->stopping.store(false, std::memory_order_release);
  context->scanning.store(true, std::memory_order_release);
  context->world_id = 0;
  context->world_generation = 0;
  {
    std::scoped_lock lock(context->mutex);
    context->start_requested.store(false, std::memory_order_release);
    context->stop_requested.store(false, std::memory_order_release);
    context->automation_state = AutomationState::idle;
    context->route.clear();
    context->landmarks.clear();
    context->landmarks_sequence = 0;
    context->route_index = 0;
    context->pickup_retries = 0;
    context->navigation_attempt = 0;
    context->collected = 0;
    context->skipped = 0;
    context->active_wallet_target = context->wallet_target;
    context->pickup_baseline_sequence = 0;
    context->pickup_verification_pending = false;
    context->landmark_transfer_attempted = false;
    context->landmark_destination = {};
    context->automation_elapsed = 0.0;
    context->movement_stalled_elapsed = 0.0;
    context->current_target_distance = 0.0;
    context->movement_probe_valid = false;
    context->automation_status =
        context->localizer.Text("detail.idle", "Idle");
  }
  RestartScan(*context);
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
  auto* context = static_cast<Context*>(plugin_context);
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  context->stopping.store(true, std::memory_order_release);
  context->scanning.store(false, std::memory_order_release);
  {
    std::scoped_lock lock(context->mutex);
    context->automation_state = AutomationState::idle;
    context->route.clear();
    context->landmark_transfer_attempted = false;
    context->landmark_destination = {};
    context->automation_status =
        context->localizer.Text("detail.stopped", "Stopped");
  }
  StopAutomationMovement(*context);
  return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
  auto* context = static_cast<Context*>(plugin_context);
  if (context == nullptr) return;
  static_cast<void>(Stop(context, 0));
  delete context;
}

void ANOMALY_CALL Update(void* plugin_context, const double delta_seconds) {
  auto* context = static_cast<Context*>(plugin_context);
  if (context == nullptr || context->stopping.load(std::memory_order_acquire)) return;
  try {
    context->developer_mode.store(
        DeveloperModeEnabled(context->ui), std::memory_order_release);
    if (context->framework != nullptr && context->framework->is_game_thread != nullptr &&
        context->framework->is_game_thread(context->framework->user) == 0) return;
    if (!context->scanning.load(std::memory_order_acquire)) return;
    ++context->update_count;
    if (context->session == nullptr || context->update_count % 120U == 0U)
      context->session = Query<AnomalyNteSessionServiceV1>(context->host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    std::uint64_t world_id = 1;
    std::uint64_t world_generation = 1;
    if (context->session != nullptr && context->session->snapshot != nullptr) {
      AnomalyNteSessionSnapshotV1 session{sizeof(session)};
      if (context->session->snapshot(context->session->user, &session).code != ANOMALY_STATUS_V1_OK ||
          session.state != ANOMALY_NTE_SESSION_V1_WORLD_READY || session.world.id == 0) return;
      world_id = session.world.id;
      world_generation = session.world.generation;
    }
    if (world_id != context->world_id || world_generation != context->world_generation) {
      ResetAutomationForWorld(*context);
      ResetCatalog(*context, world_id, world_generation);
      RestartScan(*context);
    }
    if (!RefreshRegistry(*context)) return;
    const bool had_asset = context->data_asset != 0;
    if (!had_asset) static_cast<void>(FindDataAsset(*context));
    if (context->data_asset != 0) {
      const bool candidate_changed = ScanDataTable(*context);
      std::uintptr_t player_state{};
      static_cast<void>(ResolvePlayerState(*context, player_state) &&
                        ResolveRandomItemRecords(*context, player_state));
      if (context->table_scanned.load(std::memory_order_acquire) &&
          (candidate_changed || context->update_count % kRecordRefreshInterval == 0U))
        static_cast<void>(RefreshRecordCatalog(*context));
    }
    ProcessAutomation(*context, delta_seconds);
  } catch (...) {}
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
  auto* context = static_cast<Context*>(plugin_context);
  if (context == nullptr || ui == nullptr || ui->begin_window == nullptr ||
      ui->end_window == nullptr || ui->text == nullptr || ui->button == nullptr ||
      !HasField<AnomalyUiServiceV1,
                decltype(AnomalyUiServiceV1::input_uint32)>(
          ui, offsetof(AnomalyUiServiceV1, input_uint32)) ||
      ui->input_uint32 == nullptr)
    return;
  int open = 1;
  bool window_started = false;
  try {
    const std::string title = context->localizer.Label(
        "window.title", "WalletCollector", "wallet-collector");
    const bool visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView(title), &open, 0) != 0;
    window_started = true;
    if (!visible) {
      ui->end_window(ui->user);
      return;
    }

    const bool developer_mode = DeveloperModeEnabled(ui);
    context->developer_mode.store(developer_mode, std::memory_order_release);

    AutomationState state{};
    CollectionMode collection_mode{};
    CollectionMode active_collection_mode{};
    std::string detail;
    std::string target;
    std::size_t scanned{};
    std::size_t route_size{};
    std::size_t route_index{};
    std::uint32_t retries{};
    std::uint32_t navigation_attempt{};
    std::uint32_t collected{};
    std::uint32_t skipped{};
    std::uint32_t wallet_target{};
    std::uint32_t active_wallet_target{};
    double target_distance{};
    {
      std::scoped_lock lock(context->mutex);
      state = context->automation_state;
      collection_mode = context->collection_mode;
      active_collection_mode = context->active_collection_mode;
      detail = context->automation_status;
      scanned = context->interact_boxes.size();
      route_size = context->route.size();
      route_index = context->route_index;
      retries = context->pickup_retries;
      navigation_attempt = context->navigation_attempt;
      collected = context->collected;
      skipped = context->skipped;
      wallet_target = context->wallet_target;
      active_wallet_target = context->active_wallet_target;
      target_distance = context->current_target_distance;
      if (!developer_mode) {
        context->collection_mode = CollectionMode::navigation;
        if (context->active_collection_mode == CollectionMode::teleport)
          context->active_collection_mode = CollectionMode::navigation;
        collection_mode = context->collection_mode;
        active_collection_mode = context->active_collection_mode;
      }
      if (route_index < route_size) target = context->route[route_index].row_name;
    }

    const std::uint32_t available = scanned == 0
        ? kMaximumWalletTarget
        : static_cast<std::uint32_t>((std::min)(
              scanned, static_cast<std::size_t>(kMaximumWalletTarget)));
    wallet_target = (std::clamp)(wallet_target, 1U, available);
    const std::string target_label = context->localizer.Label(
        "setting.wallet_count", "Wallet target", "wallet-target");
    if (ui->input_uint32(
            ui->user, anomaly::sdk::StringView(target_label), &wallet_target,
            1U, 10U) != 0) {
      wallet_target = (std::clamp)(wallet_target, 1U, available);
    }
    {
      std::scoped_lock lock(context->mutex);
      context->wallet_target = wallet_target;
    }

    const bool mode_editable = state == AutomationState::idle ||
        state == AutomationState::completed;
    const std::string mode_setting_label = context->localizer.Text(
        "setting.mode", "Collection mode");
    ui->text(ui->user, anomaly::sdk::StringView(mode_setting_label));
    const std::string navigation_mode_label = context->localizer.Label(
        "mode.navigation", "Walk and collect", "mode-navigation");
    if (ui->button(ui->user, anomaly::sdk::StringView(navigation_mode_label),
                   0.0F, 0.0F) != 0 && mode_editable) {
      collection_mode = CollectionMode::navigation;
      std::scoped_lock lock(context->mutex);
      context->collection_mode = collection_mode;
    }
    if (developer_mode) {
      const std::string teleport_mode_label = context->localizer.Label(
          "mode.teleport", "Teleport and collect", "mode-teleport");
      if (ui->button(ui->user, anomaly::sdk::StringView(teleport_mode_label),
                     0.0F, 0.0F) != 0 && mode_editable) {
        collection_mode = CollectionMode::teleport;
        std::scoped_lock lock(context->mutex);
        context->collection_mode = collection_mode;
      }
    }
    const CollectionMode shown_mode = mode_editable
        ? collection_mode : active_collection_mode;
    const std::string mode_name = CollectionModeName(*context, shown_mode);
    const std::array mode_arguments{std::string_view(mode_name)};
    const std::string mode_line = context->localizer.Format(
        "status.mode", "Mode: {0}", mode_arguments);
    ui->text(ui->user, anomaly::sdk::StringView(mode_line));

    const std::string state_name = AutomationStateName(*context, state);
    const std::string scanned_text = std::to_string(scanned);
    const std::string route_position =
        std::to_string((std::min)(route_index, route_size));
    const std::string route_total = std::to_string(route_size);
    const std::array state_arguments{
        std::string_view(state_name), std::string_view(scanned_text),
        std::string_view(route_position), std::string_view(route_total)};
    const std::string state_line = context->localizer.Format(
        "status.summary", "State: {0} | Scanned {1} | Route {2}/{3}",
        state_arguments);
    ui->text(ui->user, anomaly::sdk::StringView(state_line));
    const std::string collected_text = std::to_string(collected);
    const std::string active_target_text =
        std::to_string(active_wallet_target);
    const std::string skipped_text = std::to_string(skipped);
    const std::string retries_text = std::to_string(retries);
    const std::string retry_maximum = std::to_string(kMaximumPickupRetries);
    const std::array result_arguments{
        std::string_view(collected_text), std::string_view(active_target_text),
        std::string_view(skipped_text), std::string_view(retries_text),
        std::string_view(retry_maximum)};
    const std::string result_line = context->localizer.Format(
        "status.result",
        "Wallets {0}/{1} | Skipped {2} | Pickup retries {3}/{4}",
        result_arguments);
    ui->text(ui->user, anomaly::sdk::StringView(result_line));
    if (route_index < route_size &&
        active_collection_mode == CollectionMode::navigation) {
      const std::string distance =
          std::to_string(static_cast<std::uint64_t>(target_distance));
      const std::string attempt = std::to_string(navigation_attempt + 1U);
      const std::string maximum_attempts =
          std::to_string(kMaximumNavigationAttempts);
      const std::array movement_arguments{
          std::string_view(distance), std::string_view(attempt),
          std::string_view(maximum_attempts)};
      const std::string movement_line = context->localizer.Format(
          "status.movement", "Distance {0} cm | Navigation attempt {1}/{2}",
          movement_arguments);
      ui->text(ui->user, anomaly::sdk::StringView(movement_line));
    }
    if (!target.empty()) {
      const std::array target_arguments{std::string_view(target)};
      const std::string target_line = context->localizer.Format(
          "status.target", "Target: {0}", target_arguments);
      ui->text(ui->user, anomaly::sdk::StringView(target_line));
    }
    ui->text(ui->user, anomaly::sdk::StringView(detail));

    const std::string start_label = context->localizer.Label(
        "action.start", "Start collecting wallets", "start-wallet-collection");
    if (ui->button(ui->user, anomaly::sdk::StringView(start_label),
                   0.0F, 0.0F) != 0)
      context->start_requested.store(true, std::memory_order_release);
    const std::string stop_label = context->localizer.Label(
        "action.stop", "Stop", "stop-wallet-collection");
    if (ui->button(ui->user, anomaly::sdk::StringView(stop_label),
                   0.0F, 0.0F) != 0)
      context->stop_requested.store(true, std::memory_order_release);
  } catch (...) {}
  if (window_started) ui->end_window(ui->user);
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
                 ANOMALY_PLUGIN_API_V1_MINOR,
                 anomaly::sdk::StringView("anomaly.local.nte-interactbox-collector"),
                 anomaly::sdk::StringView("WalletCollector"),
                 anomaly::sdk::StringView("Anomaly"),
                 anomaly::sdk::StringView("1.0.0"),
                 Load, Start, Stop, Unload, Update, Draw};
  return anomaly::sdk::Ok();
}
