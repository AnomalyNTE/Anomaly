#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "plugins/common/localization.hpp"

#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumPresets = 32;
constexpr std::size_t kMaximumPresetNameBytes = 63;
constexpr std::size_t kMaximumImportedPoints = 4096;
constexpr std::size_t kMaximumPointNameBytes = 255;
constexpr std::size_t kMaximumPointCategoryBytes = 255;
constexpr std::size_t kPointsPerPage = 10;

// Tracked map target read chain. These offsets mirror the validated UE5/NTE
// layout used by the active profile and the 5.6.1-0+UE5-HT SDK dump.
constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
constexpr std::uint32_t kRipDisplacementOffset = 3;
constexpr std::uint32_t kRipInstructionSize = 7;
constexpr std::uint32_t kWorldGameInstanceOffset = 0x230;
constexpr std::uint32_t kGameInstanceLocalPlayersOffset = 0x38;
constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;
constexpr std::uint32_t kControllerPlayerStateOffset = 0x2D0;
// HTPlayerState.CurNavigationPathEffect (BP_NavigationPathActor_C) ->
// LastRequestParam (the navigation request) -> GoalLocation (FVector, double
// precision = 24 bytes). HT 1.4 dropped NavPathEffectParam.GoalLocation and
// leaves the player state's own CurrentPathEffectParam empty while tracking, so
// the tracked position now lives on the navigation actor that draws the route.
constexpr std::uint32_t kPlayerStateCurNavigationPathEffectOffset = 0x19C8;
constexpr std::uint32_t kNavPathActorLastRequestParamOffset = 0x4F0;
constexpr std::uint32_t kNavPathRequestGoalLocationOffset = 0x28;
// NTE streams world-partition cells around the local player controller's
// streaming-source update - the same call the camera tools hook for their free
// camera. Moving the player before those cells exist drops the character
// through unloaded terrain, so a teleport hands the destination to the streamer
// first and only moves the player once the preload window has elapsed.
constexpr double kPreloadDelayDefault = 2.0;
constexpr double kPreloadDelayMaximum = 15.0;
// The host teleport bridge uses bSweep=false and places the actor exactly at
// the requested position. Coordinates taken from map markers, imported points
// or tracked goals are often at (or slightly below) the walkable floor, which
// leaves the character embedded in geometry. We lift the target Z before
// sending, then watch the player for a short window and re-issue with a higher
// lift if the player is still well below the intended landing height.
constexpr double kLandingLiftDefault = 250.0;
constexpr double kLandingLiftMaximum = 2000.0;
constexpr std::uint32_t kSinkRetriesDefault = 2;
constexpr std::uint32_t kSinkRetriesMaximum = 8;
constexpr std::uint32_t kLandingSettleTicks = 40;
constexpr std::uint32_t kLandingWatchTicks = 180;
constexpr double kSinkThreshold = 300.0;
constexpr double kSinkRetryLiftStep = 200.0;
constexpr std::string_view kSettingsSchema = R"json(
{
  "type": "object",
  "additionalProperties": false,
  "required": ["target", "presets"],
  "properties": {
    "target": {
      "type": "array", "minItems": 3, "maxItems": 3,
      "items": {"type": "number"}
    },
    "presets": {
      "type": "array", "maxItems": 32,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "position"],
        "properties": {
          "name": {"type": "string", "minLength": 1, "maxLength": 63},
          "position": {
            "type": "array", "minItems": 3, "maxItems": 3,
            "items": {"type": "number"}
          }
        }
      }
    },
    "forwardDistance": {"type": "number"},
    "zLift": {"type": "number"},
    "landingLift": {"type": "number"},
    "sinkRetries": {"type": "integer"},
    "preloadDelay": {"type": "number"},
    "forwardHotkey": {"type": "integer"},
    "points": {
      "type": "array", "maxItems": 4096,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "x", "y", "z"],
        "properties": {
          "category": {"type": "string", "minLength": 1, "maxLength": 255},
          "color": {"type": "integer"},
          "name": {"type": "string", "minLength": 1, "maxLength": 255},
          "x": {"type": "number"},
          "y": {"type": "number"},
          "z": {"type": "number"}
        }
      }
    }
  }
}
)json";

struct CoordinatePreset {
    std::string name;
    std::array<double, 3> position{};
};

struct ImportedPoint {
    std::string category;
    std::string name;
    std::uint32_t color{};
    std::array<double, 3> position{};
};

struct ImportFile {
    std::string path;
    std::string label;
};

struct TeleportSettings {
    std::array<double, 3> target{};
    std::vector<CoordinatePreset> presets;
    double forward_distance{1000.0};
    double z_lift{100.0};
    double landing_lift{250.0};
    std::uint32_t sink_retries{2};
    double preload_delay{kPreloadDelayDefault};
    std::uint32_t forward_hotkey{};
    std::vector<ImportedPoint> points;
};

enum class TeleportAction : std::uint8_t { none, forward };

struct PendingTeleport {
    bool queued{};
    bool apply_landing_lift{true};
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double position[3]{};
};

// Tracks a teleport after it is issued so the plugin can detect the player
// sinking into geometry (the host bridge uses bSweep=false and places the
// actor exactly at the requested position). When the player's Z stays well
// below the intended landing Z after a short settle window, we re-issue the
// teleport with a higher landing lift instead of leaving the player inside
// the ground.
struct LandingMonitor {
    bool active{};
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double base_position[3]{};
    std::uint32_t retries{};
    std::uint32_t settle_ticks{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyConfigServiceV1* config{};
    const AnomalyInputServiceV1* input{};
    const AnomalyJsonServiceV1* json{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyCoreServiceV1* core{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyNtePlayerTeleportServiceV1* teleport_service{};
    AnomalyGenerationHandleV1 settings_schema{};
    double target[3]{};
    std::uintptr_t g_world_address{};
    bool tracked_target_valid{};
    bool tracked_target_read_requested{};
    bool tracked_target_teleport_requested{};
    std::array<double, 3> tracked_target{};
    std::vector<CoordinatePreset> presets;
    std::vector<ImportedPoint> imported_points;
    double forward_distance{1000.0};
    double z_lift{100.0};
    double landing_lift{250.0};
    std::uint32_t sink_retries{2};
    double preload_delay{kPreloadDelayDefault};
    std::uint32_t forward_hotkey_key{};
    AnomalyGenerationHandleV1 forward_hotkey{};
    bool capturing_forward{};
    TeleportAction hotkey_action{TeleportAction::none};
    LandingMonitor landing{};
    std::string import_folder;
    std::vector<ImportFile> import_files;
    std::size_t imported_page{};
    bool import_folder_selected{};
    bool import_file_selected{};
    std::string import_selected_file;
    std::array<char, 128> import_filter{};
    bool import_running{};
    std::uint32_t import_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    char import_message[192]{};
    PendingTeleport pending{};
    bool has_result{};
    bool current_position_unavailable{};
    bool settings_dirty{};
    std::uint32_t result_code{ANOMALY_STATUS_V1_UNAVAILABLE};
    char result_message[192]{};
    std::mutex mutex;
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

constexpr AnomalyStatusV1 StatusCode(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

template <typename Service>
struct ServiceQuery {
    const Service* service{};
    AnomalyStatusV1 status{StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};

    [[nodiscard]] explicit operator bool() const noexcept { return service != nullptr; }
};

template <typename Service>
ServiceQuery<Service> QueryService(
    const AnomalyHostApiV1* host, const std::string_view id,
    const std::uint32_t minimum_version) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->api_major != ANOMALY_PLUGIN_API_V1_MAJOR || host->query_service == nullptr) {
        return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};
    }

    const void* table{};
    const AnomalyStatusV1 status = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), minimum_version, &table);
    if (status.code != ANOMALY_STATUS_V1_OK) return {nullptr, status};
    if (table == nullptr) return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};

    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t kServicePrefixSize = offsetof(Service, user) + sizeof(void*);
    if (service->struct_size < kServicePrefixSize || service->service_version < minimum_version) {
        return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};
    }
    return {service, StatusCode(ANOMALY_STATUS_V1_OK)};
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
    default: return "UNKNOWN_STATUS";
    }
}

bool HasUiFunctions(const AnomalyUiServiceV1* ui) noexcept {
    return ui != nullptr && ui->service_version >= ANOMALY_UI_SERVICE_V1_VERSION &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_window)>(
            ui, offsetof(AnomalyUiServiceV1, begin_window)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_window)>(
            ui, offsetof(AnomalyUiServiceV1, end_window)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::text)>(
            ui, offsetof(AnomalyUiServiceV1, text)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button)>(
            ui, offsetof(AnomalyUiServiceV1, button)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_double)>(
            ui, offsetof(AnomalyUiServiceV1, input_double)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::separator)>(
            ui, offsetof(AnomalyUiServiceV1, separator)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_table)>(
            ui, offsetof(AnomalyUiServiceV1, begin_table)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_table)>(
            ui, offsetof(AnomalyUiServiceV1, end_table)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_text)>(
            ui, offsetof(AnomalyUiServiceV1, input_text)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, button_enabled)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_tab_bar)>(
            ui, offsetof(AnomalyUiServiceV1, begin_tab_bar)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_tab_item)>(
            ui, offsetof(AnomalyUiServiceV1, begin_tab_item)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_tab_item)>(
            ui, offsetof(AnomalyUiServiceV1, end_tab_item)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_tab_bar)>(
            ui, offsetof(AnomalyUiServiceV1, end_tab_bar)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::same_line)>(
            ui, offsetof(AnomalyUiServiceV1, same_line)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_child)>(
            ui, offsetof(AnomalyUiServiceV1, begin_child)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_child)>(
            ui, offsetof(AnomalyUiServiceV1, end_child)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::open_popup)>(
            ui, offsetof(AnomalyUiServiceV1, open_popup)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_popup_modal)>(
            ui, offsetof(AnomalyUiServiceV1, begin_popup_modal)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_popup)>(
            ui, offsetof(AnomalyUiServiceV1, end_popup)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::close_current_popup)>(
            ui, offsetof(AnomalyUiServiceV1, close_current_popup)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::filter_match)>(
            ui, offsetof(AnomalyUiServiceV1, filter_match)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::set_next_window_size_constraints)>(
            ui, offsetof(AnomalyUiServiceV1, set_next_window_size_constraints)) &&
        ui->begin_window != nullptr && ui->end_window != nullptr && ui->text != nullptr &&
        ui->button != nullptr && ui->input_double != nullptr && ui->separator != nullptr &&
        ui->begin_table != nullptr && ui->table_next_row != nullptr &&
        ui->table_next_column != nullptr && ui->end_table != nullptr &&
        ui->input_text != nullptr && ui->button_enabled != nullptr &&
        ui->begin_tab_bar != nullptr && ui->begin_tab_item != nullptr &&
        ui->end_tab_item != nullptr && ui->end_tab_bar != nullptr &&
        ui->same_line != nullptr && ui->begin_child != nullptr &&
        ui->end_child != nullptr && ui->open_popup != nullptr &&
        ui->begin_popup_modal != nullptr && ui->end_popup != nullptr &&
        ui->close_current_popup != nullptr && ui->filter_match != nullptr &&
        ui->set_next_window_size_constraints != nullptr;
}

bool ConfigMethodsAvailable(const AnomalyConfigServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_CONFIG_SERVICE_V1_VERSION &&
        HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::register_schema)>(
            service, offsetof(AnomalyConfigServiceV1, register_schema)) &&
        HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::read)>(
            service, offsetof(AnomalyConfigServiceV1, read)) &&
        HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
            service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
}

bool InputMethodsAvailable(const AnomalyInputServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_INPUT_SERVICE_V1_VERSION &&
        HasField<AnomalyInputServiceV1, decltype(AnomalyInputServiceV1::register_hotkey)>(
            service, offsetof(AnomalyInputServiceV1, register_hotkey)) &&
        HasField<AnomalyInputServiceV1, decltype(AnomalyInputServiceV1::release_hotkey)>(
            service, offsetof(AnomalyInputServiceV1, release_hotkey)) &&
        HasField<AnomalyInputServiceV1, decltype(AnomalyInputServiceV1::was_pressed)>(
            service, offsetof(AnomalyInputServiceV1, was_pressed)) &&
        service->register_hotkey != nullptr && service->release_hotkey != nullptr &&
        service->was_pressed != nullptr;
}

bool JsonMethodsAvailable(const AnomalyJsonServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_JSON_SERVICE_V1_VERSION &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::parse)>(
            service, offsetof(AnomalyJsonServiceV1, parse)) &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::release)>(
            service, offsetof(AnomalyJsonServiceV1, release)) &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::kind)>(
            service, offsetof(AnomalyJsonServiceV1, kind)) &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::number_value)>(
            service, offsetof(AnomalyJsonServiceV1, number_value)) &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::string_value)>(
            service, offsetof(AnomalyJsonServiceV1, string_value)) &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::array_size)>(
            service, offsetof(AnomalyJsonServiceV1, array_size)) &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::array_item)>(
            service, offsetof(AnomalyJsonServiceV1, array_item)) &&
        HasField<AnomalyJsonServiceV1, decltype(AnomalyJsonServiceV1::object_find)>(
            service, offsetof(AnomalyJsonServiceV1, object_find)) &&
        service->parse != nullptr && service->release != nullptr && service->kind != nullptr &&
        service->number_value != nullptr && service->string_value != nullptr &&
        service->array_size != nullptr && service->array_item != nullptr &&
        service->object_find != nullptr;
}

bool SchedulerMethodsAvailable(const AnomalySchedulerServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_SCHEDULER_SERVICE_V1_VERSION &&
        HasField<AnomalySchedulerServiceV1, decltype(AnomalySchedulerServiceV1::schedule)>(
            service, offsetof(AnomalySchedulerServiceV1, schedule)) &&
        service->schedule != nullptr;
}

bool CoreMethodsAvailable(const AnomalyCoreServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_CORE_SERVICE_V1_VERSION &&
        HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::read_memory)>(
            service, offsetof(AnomalyCoreServiceV1, read_memory)) &&
        service->read_memory != nullptr;
}

bool SignatureMethodsAvailable(const AnomalySignatureServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_SIGNATURE_SERVICE_V1_VERSION &&
        HasField<AnomalySignatureServiceV1, decltype(AnomalySignatureServiceV1::resolve)>(
            service, offsetof(AnomalySignatureServiceV1, resolve)) &&
        service->resolve != nullptr;
}

bool TeleportServiceMethodsAvailable(
    const AnomalyNtePlayerTeleportServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION &&
        HasField<AnomalyNtePlayerTeleportServiceV1,
            decltype(AnomalyNtePlayerTeleportServiceV1::preload)>(
            service, offsetof(AnomalyNtePlayerTeleportServiceV1, preload)) &&
        HasField<AnomalyNtePlayerTeleportServiceV1,
            decltype(AnomalyNtePlayerTeleportServiceV1::cancel_preload)>(
            service, offsetof(AnomalyNtePlayerTeleportServiceV1, cancel_preload)) &&
        service->teleport != nullptr && service->preload != nullptr &&
        service->cancel_preload != nullptr;
}

bool IsFinitePosition(const double position[3]) noexcept;
bool IsFinitePosition(const std::array<double, 3>& position) noexcept;
bool TryReadCurrentPosition(const AnomalyHostApiV1* host, double position[3]);

bool ReadBytes(
    const std::uintptr_t address, void* destination, const std::size_t size) noexcept {
    if (!CoreMethodsAvailable(g_context.core) || address == 0 || destination == nullptr ||
        size == 0) {
        return false;
    }
    AnomalyMutableByteSpanV1 output{static_cast<std::uint8_t*>(destination), size};
    return g_context.core->read_memory(g_context.core->user, address, output).code ==
        ANOMALY_STATUS_V1_OK;
}

template <typename Value>
bool ReadValue(const std::uintptr_t address, Value& value) noexcept {
    return ReadBytes(address, &value, sizeof(value));
}

bool AddAddress(const std::uintptr_t base, const std::uint64_t offset,
                std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

bool ReadPointerAt(const std::uintptr_t base, const std::uint64_t offset,
                   std::uintptr_t& value) noexcept {
    std::uintptr_t address{};
    return AddAddress(base, offset, address) && ReadValue(address, value) && value != 0;
}

bool ResolveSignature(const std::string_view pattern, std::uintptr_t& address) noexcept {
    address = 0;
    if (!SignatureMethodsAvailable(g_context.signature)) return false;
    return g_context.signature->resolve(
        g_context.signature->user, anomaly::sdk::StringView("HTGame.exe"),
        anomaly::sdk::StringView(".text"), anomaly::sdk::StringView(pattern), &address).code ==
        ANOMALY_STATUS_V1_OK && address != 0;
}

bool ResolveGWorld() noexcept {
    if (g_context.g_world_address != 0) return true;
    std::uintptr_t instruction{};
    if (!ResolveSignature(kGWorldPattern, instruction)) return false;
    std::int32_t displacement{};
    std::uintptr_t displacement_address{};
    if (!AddAddress(instruction, kRipDisplacementOffset, displacement_address) ||
        !ReadValue(displacement_address, displacement)) return false;
    const auto resolved = static_cast<std::intptr_t>(instruction) +
        static_cast<std::intptr_t>(kRipInstructionSize) + displacement;
    if (resolved <= 0) return false;
    g_context.g_world_address = static_cast<std::uintptr_t>(resolved);
    return true;
}

bool ReadTrackedTarget(double position[3]) noexcept {
    if (!ResolveGWorld()) return false;
    std::uintptr_t world{};
    std::uintptr_t game_instance{};
    std::uintptr_t local_players{};
    std::uintptr_t local_player{};
    std::uintptr_t controller{};
    std::uintptr_t player_state{};
    if (!ReadValue(g_context.g_world_address, world) || world == 0 ||
        !ReadPointerAt(world, kWorldGameInstanceOffset, game_instance) ||
        !ReadPointerAt(game_instance, kGameInstanceLocalPlayersOffset, local_players) ||
        !ReadValue(local_players, local_player) || local_player == 0 ||
        !ReadPointerAt(local_player, kLocalPlayerControllerOffset, controller) ||
        !ReadPointerAt(controller, kControllerPlayerStateOffset, player_state)) {
        return false;
    }
    std::uintptr_t nav_actor{};
    if (!ReadPointerAt(
            player_state, kPlayerStateCurNavigationPathEffectOffset, nav_actor)) {
        return false;
    }
    std::uintptr_t goal_location{};
    if (!AddAddress(nav_actor,
                    kNavPathActorLastRequestParamOffset +
                        kNavPathRequestGoalLocationOffset,
                    goal_location)) {
        return false;
    }
    std::array<double, 3> location{};
    if (!ReadBytes(goal_location, location.data(), sizeof(location))) return false;
    if (!IsFinitePosition(location) ||
        !std::ranges::any_of(location, [](const double value) {
            return std::abs(value) > 1.0;
        })) {
        return false;
    }
    std::ranges::copy(location, position);
    return true;
}

// Preload diagnostics go to the core log so a silently skipped preload can be
// told apart from a working one without a debugger attached.
void Log(const std::uint32_t level, const std::string& message) noexcept {
    if (!CoreMethodsAvailable(g_context.core)) return;
    try {
        g_context.core->log(g_context.core->user, level,
                            anomaly::sdk::StringView(message.c_str()));
    } catch (...) {
    }
}

// The framework owns the streaming-source hook and the preload window, so the plugin only
// asks for a preload through the teleport service. A failure is not fatal: the host still
// teleports, it just cannot stream the destination in first.
void ArmTeleportPreload(const PendingTeleport& pending) noexcept {
    if (g_context.teleport_service == nullptr ||
        !TeleportServiceMethodsAvailable(g_context.teleport_service)) {
        return;
    }
    double delay{};
    {
        std::scoped_lock lock(g_context.mutex);
        delay = g_context.preload_delay;
    }
    if (!std::isfinite(delay) || delay <= 0.0) return;
    AnomalyNtePlayerTeleportPreloadRequestV1 request{sizeof(request)};
    request.duration_milliseconds = static_cast<std::uint32_t>(delay * 1000.0);
    for (std::size_t axis = 0; axis != 3; ++axis) {
        request.position[axis] = pending.position[axis];
    }
    const AnomalyStatusV1 status =
        g_context.teleport_service->preload(g_context.teleport_service->user, &request);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "teleport preload unavailable; teleporting without a streamed preload");
    }
}

void DrawText(const AnomalyUiServiceV1* ui, const std::string_view text) {
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawStatus(
    const AnomalyUiServiceV1* ui,
    const std::uint32_t code,
    const char* message) {
    const std::string_view status = StatusName(code);
    if (message == nullptr || message[0] == '\0') {
        const std::array arguments{status};
        DrawText(ui, g_context.localizer.Format(
            "teleport.result", "Teleport: {0}", arguments));
    } else {
        const std::array arguments{status, std::string_view(message)};
        DrawText(ui, g_context.localizer.Format(
            "teleport.result.detail", "Teleport: {0} - {1}", arguments));
    }
}

bool IsCurrentWorld(const AnomalyNteSessionSnapshotV1& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
        snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY &&
        snapshot.world.id != 0 && snapshot.world.generation != 0;
}

bool IsCurrentPlayer(const AnomalyNtePlayerSnapshotV1& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
        (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (snapshot.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) ==
            0 &&
        snapshot.handle.id != 0 && snapshot.handle.generation != 0;
}

bool IsFinitePosition(const double position[3]) noexcept {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
        std::isfinite(position[2]);
}

bool IsFinitePosition(const std::array<double, 3>& position) noexcept {
    return IsFinitePosition(position.data());
}

bool IsValidPresetName(const std::string_view name) noexcept {
    return !name.empty() && name.size() <= kMaximumPresetNameBytes &&
        std::ranges::none_of(name, [](const unsigned char character) {
            return character < 0x20U;
        });
}

class SettingsJsonReader final {
public:
    explicit SettingsJsonReader(const std::string_view input) noexcept : input_(input) {}

    bool Consume(const char expected) noexcept {
        SkipWhitespace();
        if (position_ == input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool ReadString(std::string& value, const std::size_t maximum_size) {
        if (!Consume('"')) return false;
        value.clear();
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return value.size() <= maximum_size;
            if (character < 0x20U) return false;
            if (character != '\\') {
                if (value.size() >= maximum_size) return false;
                value.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ == input_.size() || value.size() >= maximum_size) return false;
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return false;
            }
        }
        return false;
    }

    bool ReadNumber(double& value) noexcept {
        SkipWhitespace();
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ == input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') {
                return false;
            }
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            do {
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0' &&
                     input_[position_] <= '9');
        } else {
            return false;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fraction_begin) return false;
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponent_begin) return false;
        }
        const auto [end, error] = std::from_chars(
            input_.data() + begin, input_.data() + position_, value,
            std::chars_format::general);
        return error == std::errc{} && end == input_.data() + position_ &&
            std::isfinite(value);
    }

    bool ReadUnsigned(std::uint32_t& value) noexcept {
        double number{};
        if (!ReadNumber(number) || number < 0.0 ||
            number > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) ||
            std::floor(number) != number) {
            return false;
        }
        value = static_cast<std::uint32_t>(number);
        return true;
    }

    bool AtEnd() noexcept {
        SkipWhitespace();
        return position_ == input_.size();
    }

private:
    void SkipWhitespace() noexcept {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' ||
                input_[position_] == '\r' || input_[position_] == '\t')) {
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

bool ReadPosition(SettingsJsonReader& reader, std::array<double, 3>& position) noexcept {
    if (!reader.Consume('[')) return false;
    for (std::size_t axis = 0; axis < position.size(); ++axis) {
        if (!reader.ReadNumber(position[axis])) return false;
        if (axis + 1U < position.size()) {
            if (!reader.Consume(',')) return false;
        } else if (!reader.Consume(']')) {
            return false;
        }
    }
    return IsFinitePosition(position);
}

bool ReadPreset(SettingsJsonReader& reader, CoordinatePreset& preset) {
    bool name_seen{};
    bool position_seen{};
    if (!reader.Consume('{')) return false;
    for (;;) {
        std::string key;
        if (!reader.ReadString(key, 16) || !reader.Consume(':')) return false;
        if (key == "name") {
            if (name_seen || !reader.ReadString(preset.name, kMaximumPresetNameBytes)) {
                return false;
            }
            name_seen = true;
        } else if (key == "position") {
            if (position_seen || !ReadPosition(reader, preset.position)) return false;
            position_seen = true;
        } else {
            return false;
        }
        if (reader.Consume('}')) break;
        if (!reader.Consume(',')) return false;
    }
    return name_seen && position_seen && IsValidPresetName(preset.name);
}

bool ReadPresets(SettingsJsonReader& reader, std::vector<CoordinatePreset>& presets) {
    if (!reader.Consume('[')) return false;
    if (reader.Consume(']')) return true;
    for (;;) {
        if (presets.size() >= kMaximumPresets) return false;
        CoordinatePreset preset;
        if (!ReadPreset(reader, preset) ||
            std::ranges::any_of(presets, [&preset](const CoordinatePreset& existing) {
                return existing.name == preset.name;
            })) {
            return false;
        }
        presets.push_back(std::move(preset));
        if (reader.Consume(']')) return true;
        if (!reader.Consume(',')) return false;
    }
}

std::string DerivePointCategory(const std::string_view name) {
    std::size_t position = 0;
    while (position < name.size() && std::isspace(
        static_cast<unsigned char>(name[position])) != 0) {
        ++position;
    }
    const std::size_t digits_begin = position;
    while (position < name.size() && name[position] >= '0' && name[position] <= '9') {
        ++position;
    }
    if (position == digits_begin) return std::string(name);
    while (position < name.size() && std::isspace(
        static_cast<unsigned char>(name[position])) != 0) {
        ++position;
    }
    if (position >= name.size() ||
        (name[position] != '.' && name[position] != ':' &&
         static_cast<unsigned char>(name[position]) != 0xEF)) {
        return std::string(name);
    }
    if (static_cast<unsigned char>(name[position]) == 0xEF && position + 2U < name.size() &&
        static_cast<unsigned char>(name[position + 1U]) == 0xBC &&
        static_cast<unsigned char>(name[position + 2U]) == 0x9A) {
        position += 3U;
    } else {
        ++position;
    }
    while (position < name.size() && std::isspace(
        static_cast<unsigned char>(name[position])) != 0) {
        ++position;
    }
    return position < name.size() ? std::string(name.substr(position)) : std::string(name);
}

bool ReadImportedPoint(SettingsJsonReader& reader, ImportedPoint& point) {
    bool name_seen{};
    bool category_seen{};
    bool color_seen{};
    bool x_seen{};
    bool y_seen{};
    bool z_seen{};
    if (!reader.Consume('{')) return false;
    for (;;) {
        std::string key;
        if (!reader.ReadString(key, 16) || !reader.Consume(':')) return false;
        if (key == "category") {
            if (category_seen ||
                !reader.ReadString(point.category, kMaximumPointCategoryBytes)) {
                return false;
            }
            category_seen = true;
        } else if (key == "color") {
            if (color_seen || !reader.ReadUnsigned(point.color)) return false;
            color_seen = true;
        } else if (key == "name") {
            if (name_seen || !reader.ReadString(point.name, kMaximumPointNameBytes)) {
                return false;
            }
            name_seen = true;
        } else if (key == "x") {
            if (x_seen || !reader.ReadNumber(point.position[0])) return false;
            x_seen = true;
        } else if (key == "y") {
            if (y_seen || !reader.ReadNumber(point.position[1])) return false;
            y_seen = true;
        } else if (key == "z") {
            if (z_seen || !reader.ReadNumber(point.position[2])) return false;
            z_seen = true;
        } else {
            return false;
        }
        if (reader.Consume('}')) break;
        if (!reader.Consume(',')) return false;
    }
    if (!name_seen || !x_seen || !y_seen || !z_seen ||
        !IsFinitePosition(point.position) || point.name.empty()) {
        return false;
    }
    if (point.category.empty()) point.category = DerivePointCategory(point.name);
    return !point.category.empty();
}

bool ReadImportedPoints(SettingsJsonReader& reader, std::vector<ImportedPoint>& points) {
    if (!reader.Consume('[')) return false;
    if (reader.Consume(']')) return true;
    for (;;) {
        if (points.size() >= kMaximumImportedPoints) return false;
        ImportedPoint point;
        if (!ReadImportedPoint(reader, point)) return false;
        points.push_back(std::move(point));
        if (reader.Consume(']')) return true;
        if (!reader.Consume(',')) return false;
    }
}

bool ParseSettingsDocument(const std::string_view document, TeleportSettings& settings) {
    SettingsJsonReader reader(document);
    bool target_seen{};
    bool presets_seen{};
    if (!reader.Consume('{')) return false;
    for (;;) {
        std::string key;
        if (!reader.ReadString(key, 16) || !reader.Consume(':')) return false;
        if (key == "target") {
            if (target_seen || !ReadPosition(reader, settings.target)) return false;
            target_seen = true;
        } else if (key == "presets") {
            if (presets_seen || !ReadPresets(reader, settings.presets)) return false;
            presets_seen = true;
        } else if (key == "forwardDistance") {
            if (!reader.ReadNumber(settings.forward_distance)) return false;
        } else if (key == "zLift") {
            if (!reader.ReadNumber(settings.z_lift)) return false;
        } else if (key == "landingLift") {
            if (!reader.ReadNumber(settings.landing_lift)) return false;
        } else if (key == "sinkRetries") {
            if (!reader.ReadUnsigned(settings.sink_retries)) return false;
        } else if (key == "preloadDelay") {
            if (!reader.ReadNumber(settings.preload_delay)) return false;
        } else if (key == "forwardHotkey") {
            if (!reader.ReadUnsigned(settings.forward_hotkey)) return false;
        } else if (key == "points") {
            if (!settings.points.empty() ||
                !ReadImportedPoints(reader, settings.points)) {
                return false;
            }
        } else {
            return false;
        }
        if (reader.Consume('}')) break;
        if (!reader.Consume(',')) return false;
    }
    return target_seen && presets_seen && reader.AtEnd() &&
        IsFinitePosition(settings.target) &&
        std::isfinite(settings.forward_distance) && std::isfinite(settings.z_lift) &&
        std::isfinite(settings.landing_lift) && settings.landing_lift >= 0.0 &&
        settings.landing_lift <= 2000.0 && settings.sink_retries <= 8U &&
        std::isfinite(settings.preload_delay) && settings.preload_delay >= 0.0 &&
        settings.preload_delay <= kPreloadDelayMaximum &&
        settings.forward_hotkey < 256U;
}

std::string EscapeJsonString(const std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

std::string FormatDouble(const double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
    return error == std::errc{} ? std::string(buffer.data(), end) : "0";
}

std::string SerializeSettings(const TeleportSettings& settings) {
    std::string document = "{\"target\":[" + FormatDouble(settings.target[0]) + "," +
        FormatDouble(settings.target[1]) + "," + FormatDouble(settings.target[2]) +
        "],\"presets\":[";
    for (std::size_t index = 0; index < settings.presets.size(); ++index) {
        if (index != 0) document.push_back(',');
        const CoordinatePreset& preset = settings.presets[index];
        document += "{\"name\":\"" + EscapeJsonString(preset.name) +
            "\",\"position\":[" + FormatDouble(preset.position[0]) + "," +
            FormatDouble(preset.position[1]) + "," + FormatDouble(preset.position[2]) + "]}";
    }
    document += "],\"forwardDistance\":" + FormatDouble(settings.forward_distance) +
        ",\"zLift\":" + FormatDouble(settings.z_lift) +
        ",\"landingLift\":" + FormatDouble(settings.landing_lift) +
        ",\"sinkRetries\":" + std::to_string(settings.sink_retries) +
        ",\"preloadDelay\":" + FormatDouble(settings.preload_delay) +
        ",\"forwardHotkey\":" + std::to_string(settings.forward_hotkey) +
        ",\"points\":[";
    for (std::size_t index = 0; index < settings.points.size(); ++index) {
        if (index != 0) document.push_back(',');
        const ImportedPoint& point = settings.points[index];
        document += "{\"category\":\"" + EscapeJsonString(point.category) +
            "\",\"color\":" + std::to_string(point.color) +
            ",\"name\":\"" + EscapeJsonString(point.name) +
            "\",\"x\":" + FormatDouble(point.position[0]) +
            ",\"y\":" + FormatDouble(point.position[1]) +
            ",\"z\":" + FormatDouble(point.position[2]) + "}";
    }
    document += "]}";
    return document;
}

bool LoadSettings() {
    if (!ConfigMethodsAvailable(g_context.config)) return false;
    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code == ANOMALY_STATUS_V1_NOT_FOUND) return true;
    if (size_status.code != ANOMALY_STATUS_V1_OK ||
        schema_version != kSettingsSchemaVersion || size == 0 ||
        size > kMaximumSettingsBytes) {
        return false;
    }

    try {
        std::vector<std::uint8_t> document(size);
        std::size_t copied = document.size();
        const AnomalyStatusV1 read_status = g_context.config->read(
            g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            &schema_version, {document.data(), document.size()}, &copied);
        TeleportSettings settings;
        if (read_status.code != ANOMALY_STATUS_V1_OK ||
            schema_version != kSettingsSchemaVersion || copied == 0 ||
            copied > document.size() ||
            !ParseSettingsDocument(
                {reinterpret_cast<const char*>(document.data()), copied}, settings)) {
            return false;
        }
        std::scoped_lock lock(g_context.mutex);
        std::ranges::copy(settings.target, g_context.target);
        g_context.presets = std::move(settings.presets);
        g_context.imported_points = std::move(settings.points);
        g_context.forward_distance = settings.forward_distance;
        g_context.z_lift = settings.z_lift;
        g_context.landing_lift = settings.landing_lift;
        g_context.sink_retries = settings.sink_retries;
        g_context.preload_delay = settings.preload_delay;
        g_context.forward_hotkey_key = settings.forward_hotkey;
        g_context.settings_dirty = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveSettings() {
    TeleportSettings settings;
    {
        std::scoped_lock lock(g_context.mutex);
        if (!g_context.settings_dirty) return true;
        std::ranges::copy(g_context.target, settings.target.begin());
        settings.presets = g_context.presets;
        settings.forward_distance = g_context.forward_distance;
        settings.z_lift = g_context.z_lift;
        settings.landing_lift = g_context.landing_lift;
        settings.sink_retries = g_context.sink_retries;
        settings.preload_delay = g_context.preload_delay;
        settings.forward_hotkey = g_context.forward_hotkey_key;
        settings.points = g_context.imported_points;
    }
    if (!ConfigMethodsAvailable(g_context.config)) return false;

    try {
        const std::string document = SerializeSettings(settings);
        const AnomalyStatusV1 status = g_context.config->write_atomic(
            g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            kSettingsSchemaVersion, Bytes(document));
        if (status.code != ANOMALY_STATUS_V1_OK) return false;
        std::scoped_lock lock(g_context.mutex);
        g_context.settings_dirty = false;
        return true;
    } catch (...) {
        return false;
    }
}

void RecordResult(const AnomalyStatusV1 status) noexcept {
    std::scoped_lock lock(g_context.mutex);
    g_context.has_result = true;
    g_context.result_code = status.code;
    g_context.result_message[0] = '\0';
    if (status.message.data == nullptr || status.message.size == 0) return;
    const std::size_t count = status.message.size < sizeof(g_context.result_message) - 1U
        ? status.message.size
        : sizeof(g_context.result_message) - 1U;
    std::memcpy(g_context.result_message, status.message.data, count);
    g_context.result_message[count] = '\0';
}

void RecordResult(const std::uint32_t code) noexcept {
    RecordResult(StatusCode(code));
}

std::wstring Utf8ToWide(const std::string_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required) != required) {
        return {};
    }
    return result;
}

bool ReadImportFile(const std::filesystem::path& path, std::string& document) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    document.assign(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return !file.bad() && !document.empty();
}

std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }

    [[nodiscard]] bool Usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{};
};

std::optional<std::filesystem::path> ChooseFolder(
    const std::string_view current_utf8) {
    ComApartment apartment;
    if (!apartment.Usable()) return std::nullopt;

    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        static_cast<void>(dialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST));
    }
    if (!current_utf8.empty()) {
        const std::wstring current = Utf8ToWide(current_utf8);
        if (!current.empty()) {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    current.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                static_cast<void>(dialog->SetFolder(folder.Get()));
            }
        }
    }
    if (FAILED(dialog->Show(nullptr))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

bool UsableHotkey(const std::uint32_t key) noexcept {
    return key > 0 && key < 256U && key != VK_LBUTTON && key != VK_RBUTTON &&
        key != VK_MBUTTON && key != VK_XBUTTON1 && key != VK_XBUTTON2;
}

std::string VirtualKeyName(const std::uint32_t key) {
    if (key >= '0' && key <= '9') return std::string(1, static_cast<char>(key));
    if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1U);
    switch (key) {
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_SPACE: return "Space";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "Left";
    case VK_UP: return "Up";
    case VK_RIGHT: return "Right";
    case VK_DOWN: return "Down";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_NUMPAD0: return "Num0";
    case VK_NUMPAD1: return "Num1";
    case VK_NUMPAD2: return "Num2";
    case VK_NUMPAD3: return "Num3";
    case VK_NUMPAD4: return "Num4";
    case VK_NUMPAD5: return "Num5";
    case VK_NUMPAD6: return "Num6";
    case VK_NUMPAD7: return "Num7";
    case VK_NUMPAD8: return "Num8";
    case VK_NUMPAD9: return "Num9";
    default: return std::to_string(key);
    }
}

void SetTeleportAction(const TeleportAction action) noexcept {
    std::scoped_lock lock(g_context.mutex);
    if (!g_context.capturing_forward) {
        g_context.hotkey_action = action;
    }
}

void ANOMALY_CALL ForwardHotkey(void*, AnomalyGenerationHandleV1,
                                const AnomalyInputSnapshotV1*) noexcept {
    SetTeleportAction(TeleportAction::forward);
}

bool RegisterHotkey(
    const AnomalyInputServiceV1* input, const std::uint32_t key,
    const std::string_view id, AnomalyHotkeyCallbackV1 callback,
    AnomalyGenerationHandleV1& handle) noexcept {
    if (!InputMethodsAvailable(input) || !UsableHotkey(key)) return false;
    try {
        const std::string hotkey_id(id);
        AnomalyHotkeySpecV1 spec{sizeof(spec)};
        spec.virtual_key = key;
        spec.flags = ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS |
            ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED;
        spec.id = anomaly::sdk::StringView(hotkey_id);
        handle = {};
        return input->register_hotkey(input->user, &spec, callback, &g_context, &handle)
            .code == ANOMALY_STATUS_V1_OK &&
            handle.id != 0;
    } catch (...) {
        handle = {};
        return false;
    }
}

bool RegisterForwardHotkey(Context& context, const std::uint32_t key,
                           AnomalyGenerationHandleV1& handle) noexcept {
    return RegisterHotkey(
        context.input, key, "nte-teleport-forward-" + std::to_string(key),
        ForwardHotkey, handle);
}

void ReleaseHotkeys(Context& context) noexcept {
    if (context.forward_hotkey.id != 0 && InputMethodsAvailable(context.input)) {
        static_cast<void>(context.input->release_hotkey(
            context.input->user, context.forward_hotkey));
    }
    context.forward_hotkey = {};
}

bool ReplaceForwardHotkey(Context& context, const std::uint32_t key) noexcept {
    if (key == context.forward_hotkey_key) return true;
    if (key == 0) {
        const auto previous = context.forward_hotkey;
        if (previous.id != 0 && InputMethodsAvailable(context.input)) {
            static_cast<void>(context.input->release_hotkey(
                context.input->user, previous));
        }
        context.forward_hotkey = {};
        context.forward_hotkey_key = 0;
        context.settings_dirty = true;
        return true;
    }
    AnomalyGenerationHandleV1 replacement{};
    if (!RegisterForwardHotkey(context, key, replacement)) return false;
    const auto previous = context.forward_hotkey;
    if (previous.id != 0 && InputMethodsAvailable(context.input) &&
        context.input->release_hotkey(context.input->user, previous).code !=
            ANOMALY_STATUS_V1_OK) {
        static_cast<void>(context.input->release_hotkey(
            context.input->user, replacement));
        return false;
    }
    context.forward_hotkey = replacement;
    context.forward_hotkey_key = key;
    context.settings_dirty = true;
    return true;
}

bool CaptureHotkey(
    Context& context, bool& capturing,
    bool (*replace)(Context&, std::uint32_t)) noexcept {
    if (!InputMethodsAvailable(context.input)) {
        capturing = false;
        return false;
    }
    int pressed{};
    if (context.input->was_pressed(context.input->user, VK_ESCAPE, &pressed).code ==
            ANOMALY_STATUS_V1_OK &&
        pressed != 0) {
        capturing = false;
        return true;
    }
    if (context.input->was_pressed(context.input->user, VK_BACK, &pressed).code ==
            ANOMALY_STATUS_V1_OK &&
        pressed != 0 && replace(context, 0)) {
        capturing = false;
        return true;
    }
    for (std::uint32_t candidate = 1; candidate < 256U; ++candidate) {
        if (!UsableHotkey(candidate)) continue;
        pressed = 0;
        if (context.input->was_pressed(context.input->user, candidate, &pressed).code ==
                ANOMALY_STATUS_V1_OK &&
            pressed != 0 && replace(context, candidate)) {
            capturing = false;
            return true;
        }
    }
    return false;
}

std::string StatusMessage(const AnomalyStatusV1& status) {
    if (status.message.data == nullptr || status.message.size == 0) return {};
    return std::string(status.message.data, status.message.size);
}

void SetImportStatus(const std::uint32_t code, const std::string_view message = {}) {
    std::scoped_lock lock(g_context.mutex);
    g_context.import_status = code;
    const std::size_t count = message.size() < sizeof(g_context.import_message) - 1U
        ? message.size()
        : sizeof(g_context.import_message) - 1U;
    if (count != 0) std::memcpy(g_context.import_message, message.data(), count);
    g_context.import_message[count] = '\0';
}

class JsonHandleScope final {
public:
    explicit JsonHandleScope(const AnomalyJsonServiceV1* json) noexcept : json_(json) {}
    ~JsonHandleScope() {
        if (json_ == nullptr) return;
        for (auto iterator = handles_.rbegin(); iterator != handles_.rend(); ++iterator) {
            static_cast<void>(json_->release(json_->user, *iterator));
        }
    }

    void Add(const AnomalyGenerationHandleV1 handle) { handles_.push_back(handle); }

private:
    const AnomalyJsonServiceV1* json_;
    std::vector<AnomalyGenerationHandleV1> handles_;
};

bool JsonObjectString(
    const AnomalyJsonServiceV1& json, const AnomalyGenerationHandleV1 object,
    const std::string_view key, std::string& value) {
    AnomalyGenerationHandleV1 child{};
    const AnomalyStatusV1 find_status = json.object_find(
        json.user, object, anomaly::sdk::StringView(key), &child);
    if (find_status.code != ANOMALY_STATUS_V1_OK) return false;
    JsonHandleScope scope(&json);
    scope.Add(child);
    std::size_t required{};
    const AnomalyStatusV1 size_status = json.string_value(
        json.user, child, nullptr, &required);
    if (size_status.code != ANOMALY_STATUS_V1_OK || required == 0) return false;
    value.resize(required);
    std::size_t capacity = required;
    if (json.string_value(json.user, child, value.data(), &capacity).code !=
            ANOMALY_STATUS_V1_OK ||
        capacity == 0) {
        return false;
    }
    value.resize(capacity - 1U);
    return true;
}

bool JsonObjectNumber(
    const AnomalyJsonServiceV1& json, const AnomalyGenerationHandleV1 object,
    const std::string_view key, double& value) {
    AnomalyGenerationHandleV1 child{};
    const AnomalyStatusV1 find_status = json.object_find(
        json.user, object, anomaly::sdk::StringView(key), &child);
    if (find_status.code != ANOMALY_STATUS_V1_OK) return false;
    JsonHandleScope scope(&json);
    scope.Add(child);
    return json.number_value(json.user, child, &value).code == ANOMALY_STATUS_V1_OK;
}

bool ImportPointsFromDocument(
    const AnomalyJsonServiceV1& json, const std::string_view document,
    std::vector<ImportedPoint>& points, std::string& error) {
    points.clear();
    AnomalyGenerationHandleV1 root{};
    const AnomalyStatusV1 parse_status = json.parse(
        json.user, anomaly::sdk::StringView(document), &root);
    if (parse_status.code != ANOMALY_STATUS_V1_OK || root.id == 0) {
        error = StatusMessage(parse_status);
        if (error.empty()) error = "JSON parse failed";
        return false;
    }
    JsonHandleScope scope(&json);
    scope.Add(root);

    std::uint32_t kind{};
    if (json.kind(json.user, root, &kind).code != ANOMALY_STATUS_V1_OK ||
        kind != ANOMALY_JSON_V1_ARRAY) {
        error = "Points document must be a JSON array";
        return false;
    }
    std::size_t count{};
    if (json.array_size(json.user, root, &count).code != ANOMALY_STATUS_V1_OK ||
        count > kMaximumImportedPoints) {
        error = "Points array is too large";
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        AnomalyGenerationHandleV1 item{};
        if (json.array_item(json.user, root, index, &item).code != ANOMALY_STATUS_V1_OK) {
            error = "Unable to read points array item";
            return false;
        }
        scope.Add(item);
        if (json.kind(json.user, item, &kind).code != ANOMALY_STATUS_V1_OK ||
            kind != ANOMALY_JSON_V1_OBJECT) {
            error = "Points array item is not an object";
            return false;
        }
        ImportedPoint point;
        if (!JsonObjectString(json, item, "name", point.name) || point.name.empty()) {
            error = "Point is missing a name";
            return false;
        }
        double color_number{};
        if (JsonObjectNumber(json, item, "color", color_number) &&
            color_number >= 0.0 &&
            color_number <= static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) &&
            std::floor(color_number) == color_number) {
            point.color = static_cast<std::uint32_t>(color_number);
        }
        double x{};
        double y{};
        double z{};
        if (!JsonObjectNumber(json, item, "x", x) ||
            !JsonObjectNumber(json, item, "y", y) ||
            !JsonObjectNumber(json, item, "z", z)) {
            error = "Point is missing x, y, or z";
            return false;
        }
        point.position = {x, y, z};
        if (!IsFinitePosition(point.position)) {
            error = "Point contains a non-finite coordinate";
            return false;
        }
        static_cast<void>(JsonObjectString(json, item, "category", point.category));
        if (point.category.empty()) point.category = DerivePointCategory(point.name);
        if (point.category.empty()) {
            error = "Point category is empty";
            return false;
        }
        points.push_back(std::move(point));
    }
    return true;
}

class ImportRunningGuard final {
public:
    explicit ImportRunningGuard(Context* context) noexcept : context_(context) {}
    ~ImportRunningGuard() {
        if (context_ == nullptr) return;
        std::scoped_lock lock(context_->mutex);
        context_->import_running = false;
    }
private:
    Context* context_;
};

void ANOMALY_CALL ScanFolderTask(void* value, AnomalyGenerationHandleV1) {
    auto* context = static_cast<Context*>(value);
    if (context == nullptr) return;
    ImportRunningGuard guard(context);
    try {
        std::string folder_utf8;
        {
            std::scoped_lock lock(context->mutex);
            folder_utf8 = context->import_folder;
        }
        if (folder_utf8.empty()) {
            SetImportStatus(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "Folder path is empty");
            return;
        }
        const std::wstring folder_wide = Utf8ToWide(folder_utf8);
        if (folder_wide.empty()) {
            SetImportStatus(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "Folder path is not valid UTF-8");
            return;
        }
        std::error_code filesystem_error;
        const std::filesystem::path folder(folder_wide);
        if (!std::filesystem::is_directory(folder, filesystem_error) || filesystem_error) {
            SetImportStatus(ANOMALY_STATUS_V1_NOT_FOUND, "Folder does not exist");
            return;
        }
        std::vector<ImportFile> files;
        const std::filesystem::recursive_directory_iterator end;
        for (std::filesystem::recursive_directory_iterator iterator(folder, filesystem_error);
             iterator != end;
             iterator.increment(filesystem_error)) {
            if (filesystem_error) break;
            if (!iterator->is_regular_file(filesystem_error) || filesystem_error) continue;
            const std::filesystem::path extension = iterator->path().extension();
            if (extension != L".json" && extension != L".JSON") continue;
            ImportFile file;
            file.path = WideToUtf8(iterator->path().native());
            std::error_code relative_error;
            const std::filesystem::path relative = std::filesystem::relative(
                iterator->path(), folder, relative_error);
            file.label = relative_error
                ? WideToUtf8(iterator->path().filename().native())
                : WideToUtf8(relative.native());
            files.push_back(std::move(file));
        }
        std::sort(files.begin(), files.end(), [](const ImportFile& left, const ImportFile& right) {
            return left.label < right.label;
        });
        const std::size_t file_count = files.size();
        {
            std::scoped_lock lock(context->mutex);
            context->import_files = std::move(files);
        }
        const std::string file_count_text = std::to_string(file_count);
        const std::string message = context->localizer.Format(
            "import.scan.found", "Found {0} JSON files",
            std::array{std::string_view(file_count_text)});
        SetImportStatus(ANOMALY_STATUS_V1_OK, message);
    } catch (...) {
        SetImportStatus(ANOMALY_STATUS_V1_FAILED, "Folder scan raised an exception");
    }
}

void ANOMALY_CALL ImportFileTask(void* value, AnomalyGenerationHandleV1) {
    auto* context = static_cast<Context*>(value);
    if (context == nullptr) return;
    ImportRunningGuard guard(context);
    try {
        std::string file_utf8;
        const AnomalyJsonServiceV1* json{};
        {
            std::scoped_lock lock(context->mutex);
            file_utf8 = context->import_selected_file;
            json = context->json;
        }
        if (!JsonMethodsAvailable(json)) {
            SetImportStatus(ANOMALY_STATUS_V1_UNAVAILABLE, "JSON service is unavailable");
            return;
        }
        const std::wstring file_wide = Utf8ToWide(file_utf8);
        if (file_wide.empty()) {
            SetImportStatus(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "Import file path is invalid");
            return;
        }
        const std::filesystem::path path(file_wide);
        std::string document;
        if (!ReadImportFile(path, document)) {
            SetImportStatus(ANOMALY_STATUS_V1_NOT_FOUND, "Import file could not be read");
            return;
        }
        std::vector<ImportedPoint> points;
        std::string error;
        if (!ImportPointsFromDocument(*json, document, points, error)) {
            SetImportStatus(ANOMALY_STATUS_V1_FAILED, error);
            return;
        }
        const std::size_t imported_count = points.size();
        {
            std::scoped_lock lock(context->mutex);
            context->imported_points = std::move(points);
            context->imported_page = 0;
            context->settings_dirty = true;
        }
        const std::string imported_count_text = std::to_string(imported_count);
        const std::string message = context->localizer.Format(
            "import.imported", "Imported {0} points",
            std::array{std::string_view(imported_count_text)});
        SetImportStatus(ANOMALY_STATUS_V1_OK, message);
    } catch (...) {
        SetImportStatus(ANOMALY_STATUS_V1_FAILED, "JSON import raised an exception");
    }
}

void ScheduleFolderScan(Context& context) {
    if (!SchedulerMethodsAvailable(context.scheduler)) {
        SetImportStatus(ANOMALY_STATUS_V1_UNAVAILABLE, "Scheduler is unavailable");
        return;
    }
    {
        std::scoped_lock lock(context.mutex);
        if (context.import_running) return;
        context.import_running = true;
    }
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status = context.scheduler->schedule(
        context.scheduler->user, 0, ScanFolderTask, &context, &task);
    if (status.code != ANOMALY_STATUS_V1_OK || task.id == 0) {
        {
            std::scoped_lock lock(context.mutex);
            context.import_running = false;
        }
        SetImportStatus(status.code, StatusMessage(status));
    }
}

void ScheduleImportFile(Context& context) {
    if (!SchedulerMethodsAvailable(context.scheduler)) {
        SetImportStatus(ANOMALY_STATUS_V1_UNAVAILABLE, "Scheduler is unavailable");
        return;
    }
    {
        std::scoped_lock lock(context.mutex);
        if (context.import_running) return;
        context.import_running = true;
    }
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status = context.scheduler->schedule(
        context.scheduler->user, 0, ImportFileTask, &context, &task);
    if (status.code != ANOMALY_STATUS_V1_OK || task.id == 0) {
        {
            std::scoped_lock lock(context.mutex);
            context.import_running = false;
        }
        SetImportStatus(status.code, StatusMessage(status));
    }
}

AnomalyStatusV1 IssueTeleport(
    const AnomalyHostApiV1* host, const AnomalyGenerationHandleV1 world,
    const AnomalyGenerationHandleV1 player, const double position[3]) {
    const auto teleport = QueryService<AnomalyNtePlayerTeleportServiceV1>(
        host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    if (!teleport ||
        !HasField<AnomalyNtePlayerTeleportServiceV1,
            decltype(AnomalyNtePlayerTeleportServiceV1::teleport)>(
            teleport.service,
            offsetof(AnomalyNtePlayerTeleportServiceV1, teleport)) ||
        teleport.service->teleport == nullptr) {
        return teleport ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : teleport.status;
    }
    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    // The host streams the destination in by default; a zero preload delay asks for the
    // previous immediate behaviour instead.
    double preload_delay{};
    {
        std::scoped_lock lock(g_context.mutex);
        preload_delay = g_context.preload_delay;
    }
    request.flags = std::isfinite(preload_delay) && preload_delay > 0.0
        ? 0u
        : ANOMALY_NTE_PLAYER_TELEPORT_REQUEST_V1_IMMEDIATE;
    request.world = world;
    request.player = player;
    for (std::size_t axis = 0; axis != 3; ++axis) request.position[axis] = position[axis];
    return teleport.service->teleport(teleport.service->user, &request);
}

void ProcessLandingMonitor(const AnomalyHostApiV1* host) {
    LandingMonitor monitor{};
    double landing_lift{};
    std::uint32_t sink_retries{};
    {
        std::scoped_lock lock(g_context.mutex);
        monitor = g_context.landing;
        landing_lift = g_context.landing_lift;
        sink_retries = g_context.sink_retries;
    }
    if (!monitor.active || host == nullptr) return;

    ++monitor.settle_ticks;
    if (monitor.settle_ticks < kLandingSettleTicks) {
        std::scoped_lock lock(g_context.mutex);
        g_context.landing.settle_ticks = monitor.settle_ticks;
        return;
    }
    if (monitor.settle_ticks > kLandingWatchTicks) {
        std::scoped_lock lock(g_context.mutex);
        g_context.landing = {};
        return;
    }

    double player_position[3]{};
    if (!TryReadCurrentPosition(host, player_position)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.landing.settle_ticks = monitor.settle_ticks;
        return;
    }

    if (player_position[2] >= monitor.base_position[2] - kSinkThreshold) {
        std::scoped_lock lock(g_context.mutex);
        g_context.landing = {};
        return;
    }
    if (monitor.retries >= sink_retries) {
        std::scoped_lock lock(g_context.mutex);
        g_context.landing = {};
        return;
    }

    std::array<double, 3> retry{};
    std::ranges::copy(monitor.base_position, retry.begin());
    const double lift = std::isfinite(landing_lift) ? landing_lift : kLandingLiftDefault;
    retry[2] += lift + static_cast<double>(monitor.retries + 1U) * kSinkRetryLiftStep;
    const AnomalyStatusV1 status = IssueTeleport(host, monitor.world, monitor.player, retry.data());
    if (status.code != ANOMALY_STATUS_V1_OK) {
        std::scoped_lock lock(g_context.mutex);
        g_context.landing = {};
        return;
    }
    std::scoped_lock lock(g_context.mutex);
    g_context.landing.retries = monitor.retries + 1U;
    g_context.landing.settle_ticks = 0;
}

void QueueRequest(
    const AnomalyNteSessionSnapshotV1& session, const AnomalyNtePlayerSnapshotV1& player,
    const double position[3], const bool apply_landing_lift = true) noexcept {
    std::scoped_lock lock(g_context.mutex);
    g_context.pending.queued = true;
    g_context.pending.apply_landing_lift = apply_landing_lift;
    g_context.pending.world = session.world;
    g_context.pending.player = player.handle;
    for (std::size_t axis = 0; axis != 3; ++axis) {
        g_context.pending.position[axis] = position[axis];
    }
    g_context.has_result = false;
    g_context.result_message[0] = '\0';
}

void DrawResult(const AnomalyUiServiceV1* ui) {
    bool queued{};
    bool has_result{};
    std::uint32_t result_code{};
    char result_message[sizeof(g_context.result_message)]{};
    {
        std::scoped_lock lock(g_context.mutex);
        queued = g_context.pending.queued;
        has_result = g_context.has_result;
        result_code = g_context.result_code;
        std::memcpy(result_message, g_context.result_message, sizeof(result_message));
    }
    if (queued) {
        DrawText(ui, g_context.localizer.Text(
            "teleport.state.queued", "Teleport: QUEUED"));
    } else if (has_result) {
        DrawStatus(ui, result_code, result_message);
    } else {
        DrawText(ui, g_context.localizer.Text(
            "teleport.state.idle", "Teleport: IDLE"));
    }
}

void TryQueueRequest(const AnomalyHostApiV1* host, const double position[3]) {
    if (!IsFinitePosition(position)) {
        RecordResult(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        return;
    }

    const auto session = QueryService<AnomalyNteSessionServiceV1>(
        host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    if (!session ||
        !HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::snapshot)>(
            session.service, offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
        session.service->snapshot == nullptr) {
        RecordResult(session ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : session.status);
        return;
    }

    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    const AnomalyStatusV1 session_status = session.service->snapshot(
        session.service->user, &session_snapshot);
    if (session_status.code != ANOMALY_STATUS_V1_OK || !IsCurrentWorld(session_snapshot)) {
        RecordResult(session_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)
            : session_status);
        return;
    }

    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!player ||
        !HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::snapshot)>(
            player.service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) ||
        player.service->snapshot == nullptr) {
        RecordResult(player ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : player.status);
        return;
    }

    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    const AnomalyStatusV1 player_status = player.service->snapshot(
        player.service->user, &player_snapshot);
    if (player_status.code != ANOMALY_STATUS_V1_OK || !IsCurrentPlayer(player_snapshot)) {
        RecordResult(player_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)
            : player_status);
        return;
    }

    QueueRequest(session_snapshot, player_snapshot, position);
}

bool TryReadCurrentPosition(const AnomalyHostApiV1* host, double position[3]) {
    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!player ||
        !HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::snapshot)>(
            player.service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) ||
        player.service->snapshot == nullptr) {
        return false;
    }

    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = player.service->snapshot(player.service->user, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK || !IsCurrentPlayer(snapshot) ||
        !IsFinitePosition(snapshot.position)) {
        return false;
    }
    std::ranges::copy(snapshot.position, position);
    return true;
}

bool IsCurrentCamera(const AnomalyNteCameraSnapshotV1& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
        (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (snapshot.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) ==
            0 &&
        snapshot.world.id != 0 && snapshot.world.generation != 0 &&
        snapshot.player.id != 0 && snapshot.player.generation != 0 &&
        IsFinitePosition(snapshot.position) && IsFinitePosition(snapshot.rotation);
}

void QueueDirectionalTeleport(
    const AnomalyHostApiV1* host, const TeleportAction action) {
    if (action == TeleportAction::none) {
        RecordResult(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        return;
    }

    const auto session = QueryService<AnomalyNteSessionServiceV1>(
        host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    if (!session ||
        !HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::snapshot)>(
            session.service, offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
        session.service->snapshot == nullptr) {
        RecordResult(session ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : session.status);
        return;
    }
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    const AnomalyStatusV1 session_status = session.service->snapshot(
        session.service->user, &session_snapshot);
    if (session_status.code != ANOMALY_STATUS_V1_OK || !IsCurrentWorld(session_snapshot)) {
        RecordResult(session_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)
            : session_status);
        return;
    }

    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!player ||
        !HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::snapshot)>(
            player.service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) ||
        !HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::camera_snapshot)>(
            player.service, offsetof(AnomalyNtePlayerServiceV1, camera_snapshot)) ||
        player.service->snapshot == nullptr || player.service->camera_snapshot == nullptr) {
        RecordResult(player ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : player.status);
        return;
    }

    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    const AnomalyStatusV1 player_status = player.service->snapshot(
        player.service->user, &player_snapshot);
    if (player_status.code != ANOMALY_STATUS_V1_OK || !IsCurrentPlayer(player_snapshot)) {
        RecordResult(player_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)
            : player_status);
        return;
    }

    AnomalyNteCameraSnapshotV1 camera{sizeof(camera)};
    const AnomalyStatusV1 camera_status = player.service->camera_snapshot(
        player.service->user, &camera);
    if (camera_status.code != ANOMALY_STATUS_V1_OK || !IsCurrentCamera(camera)) {
        RecordResult(camera_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)
            : camera_status);
        return;
    }

    double distance{};
    double z_lift{};
    {
        std::scoped_lock lock(g_context.mutex);
        distance = g_context.forward_distance;
        z_lift = g_context.z_lift;
    }
    if (!std::isfinite(distance) || distance <= 0.0 || !std::isfinite(z_lift)) {
        RecordResult(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        return;
    }

    constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
    const double yaw = camera.rotation[1] * kDegreesToRadians;
    std::array<double, 3> destination{};
    destination[0] = camera.position[0] + std::cos(yaw) * distance;
    destination[1] = camera.position[1] + std::sin(yaw) * distance;
    destination[2] = camera.position[2] + z_lift;
    if (!IsFinitePosition(destination)) {
        RecordResult(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        return;
    }
    QueueRequest(session_snapshot, player_snapshot, destination.data(), false);
}

std::string FormatPosition(const std::array<double, 3>& position) {
    std::array<char, 160> buffer{};
    std::snprintf(
        buffer.data(), buffer.size(), "%.3f, %.3f, %.3f",
        position[0], position[1], position[2]);
    return buffer.data();
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto ui = QueryService<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    const auto config = QueryService<AnomalyConfigServiceV1>(
        host, ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    const auto input = QueryService<AnomalyInputServiceV1>(
        host, ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    const auto json = QueryService<AnomalyJsonServiceV1>(
        host, ANOMALY_JSON_SERVICE_V1_ID, ANOMALY_JSON_SERVICE_V1_VERSION);
    const auto scheduler = QueryService<AnomalySchedulerServiceV1>(
        host, ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    const auto core = QueryService<AnomalyCoreServiceV1>(
        host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    const auto signature = QueryService<AnomalySignatureServiceV1>(
        host, ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    const auto teleport = QueryService<AnomalyNtePlayerTeleportServiceV1>(
        host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    if (!ui) return ui.status;
    if (!config) return config.status;
    if (!HasUiFunctions(ui.service) || !ConfigMethodsAvailable(config.service)) {
        return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.host = host;
        g_context.localizer = anomaly::plugins::Localizer(host);
        g_context.config = config.service;
        g_context.input = InputMethodsAvailable(input.service) ? input.service : nullptr;
        g_context.json = JsonMethodsAvailable(json.service) ? json.service : nullptr;
        g_context.scheduler = SchedulerMethodsAvailable(scheduler.service)
            ? scheduler.service
            : nullptr;
        g_context.core = CoreMethodsAvailable(core.service) ? core.service : nullptr;
        g_context.signature = SignatureMethodsAvailable(signature.service)
            ? signature.service
            : nullptr;
        g_context.teleport_service =
            TeleportServiceMethodsAvailable(teleport.service) ? teleport.service : nullptr;
        g_context.g_world_address = 0;
        g_context.tracked_target_valid = false;
        g_context.tracked_target_read_requested = false;
        g_context.tracked_target_teleport_requested = false;
        g_context.tracked_target = {};
        g_context.settings_schema = {};
        g_context.target[0] = 0.0;
        g_context.target[1] = 0.0;
        g_context.target[2] = 0.0;
        g_context.presets.clear();
        g_context.imported_points.clear();
        g_context.forward_distance = 1000.0;
        g_context.z_lift = 100.0;
        g_context.landing_lift = 250.0;
        g_context.sink_retries = 2;
        g_context.preload_delay = kPreloadDelayDefault;
        g_context.forward_hotkey_key = 0;
        g_context.forward_hotkey = {};
        g_context.capturing_forward = false;
        g_context.hotkey_action = TeleportAction::none;
        g_context.landing = {};
        g_context.import_folder.clear();
        g_context.import_files.clear();
        g_context.imported_page = 0;
        g_context.import_folder_selected = false;
        g_context.import_file_selected = false;
        g_context.import_selected_file.clear();
        g_context.import_filter.fill('\0');
        g_context.import_running = false;
        g_context.import_status = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_context.import_message[0] = '\0';
        g_context.pending = {};
        g_context.has_result = false;
        g_context.current_position_unavailable = false;
        g_context.settings_dirty = false;
        g_context.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_context.result_message[0] = '\0';
    }
    const AnomalyStatusV1 schema_status = g_context.config->register_schema(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(kSettingsSchema), &g_context.settings_schema);
    if (schema_status.code != ANOMALY_STATUS_V1_OK || g_context.settings_schema.id == 0 ||
        g_context.settings_schema.generation == 0) {
        std::scoped_lock lock(g_context.mutex);
        g_context.host = nullptr;
        g_context.localizer = {};
        g_context.config = nullptr;
        g_context.input = nullptr;
        g_context.json = nullptr;
        g_context.scheduler = nullptr;
        g_context.settings_schema = {};
        return schema_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_FAILED)
            : schema_status;
    }
    static_cast<void>(LoadSettings());
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g_context) return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.pending = {};
        g_context.has_result = false;
        g_context.hotkey_action = TeleportAction::none;
        g_context.landing = {};
        g_context.result_message[0] = '\0';
    }
    ReleaseHotkeys(g_context);
    if (g_context.forward_hotkey_key != 0) {
        static_cast<void>(RegisterForwardHotkey(
            g_context, g_context.forward_hotkey_key, g_context.forward_hotkey));
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t) {
    if (context != &g_context) return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    ReleaseHotkeys(g_context);
    const bool settings_saved = SaveSettings();
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.pending = {};
    }
    return settings_saved ? anomaly::sdk::Ok() : StatusCode(ANOMALY_STATUS_V1_FAILED);
}

void ANOMALY_CALL Unload(void* context) {
    if (context != &g_context) return;
    std::scoped_lock lock(g_context.mutex);
    g_context.host = nullptr;
    g_context.localizer = {};
    g_context.config = nullptr;
    g_context.input = nullptr;
    g_context.json = nullptr;
    g_context.scheduler = nullptr;
    g_context.core = nullptr;
    g_context.signature = nullptr;
    g_context.teleport_service = nullptr;
    g_context.g_world_address = 0;
    g_context.tracked_target_valid = false;
    g_context.tracked_target_read_requested = false;
    g_context.tracked_target_teleport_requested = false;
    g_context.tracked_target = {};
    g_context.settings_schema = {};
    g_context.presets.clear();
    g_context.imported_points.clear();
    g_context.forward_hotkey = {};
    g_context.capturing_forward = false;
    g_context.hotkey_action = TeleportAction::none;
    g_context.landing = {};
    g_context.import_folder.clear();
    g_context.import_files.clear();
    g_context.imported_page = 0;
    g_context.import_folder_selected = false;
    g_context.import_file_selected = false;
    g_context.import_selected_file.clear();
    g_context.import_filter.fill('\0');
    g_context.import_running = false;
    g_context.pending = {};
    g_context.has_result = false;
    g_context.current_position_unavailable = false;
    g_context.settings_dirty = false;
    g_context.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
    g_context.result_message[0] = '\0';
}

void ANOMALY_CALL Update(void* context, double) {
    if (context != &g_context) return;

    PendingTeleport pending{};
    TeleportAction action{TeleportAction::none};
    bool import_folder_selected{};
    bool import_file_selected{};
    const AnomalyHostApiV1* host{};
    {
        std::scoped_lock lock(g_context.mutex);
        host = g_context.host;
        if (g_context.pending.queued) {
            pending = g_context.pending;
            g_context.pending = {};
        } else {
            action = g_context.hotkey_action;
            g_context.hotkey_action = TeleportAction::none;
            import_folder_selected = g_context.import_folder_selected;
            g_context.import_folder_selected = false;
            import_file_selected = g_context.import_file_selected;
            g_context.import_file_selected = false;
        }
    }
    if (host == nullptr) {
        if (pending.queued || action != TeleportAction::none) {
            RecordResult(ANOMALY_STATUS_V1_UNAVAILABLE);
        }
        return;
    }

    ProcessLandingMonitor(host);

    if (import_folder_selected) ScheduleFolderScan(g_context);
    if (import_file_selected) ScheduleImportFile(g_context);

    bool tracked_teleport_requested{};
    bool tracked_read_requested{};
    {
        std::scoped_lock lock(g_context.mutex);
        tracked_teleport_requested = g_context.tracked_target_teleport_requested;
        g_context.tracked_target_teleport_requested = false;
        tracked_read_requested = g_context.tracked_target_read_requested;
        g_context.tracked_target_read_requested = false;
    }
    if (tracked_read_requested) {
        std::array<double, 3> position{};
        const bool valid = ReadTrackedTarget(position.data());
        std::scoped_lock lock(g_context.mutex);
        g_context.tracked_target_valid = valid;
        if (valid) g_context.tracked_target = position;
        else g_context.tracked_target = {};
    }
    if (tracked_teleport_requested) {
        std::array<double, 3> position{};
        if (ReadTrackedTarget(position.data())) {
            TryQueueRequest(host, position.data());
        } else {
            std::scoped_lock lock(g_context.mutex);
            g_context.tracked_target_valid = false;
            g_context.tracked_target = {};
            g_context.has_result = true;
            g_context.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
            g_context.result_message[0] = '\0';
        }
        return;
    }

    if (action != TeleportAction::none) {
        QueueDirectionalTeleport(host, action);
        return;
    }

    // Terrain preload: the destination is handed to the streamer first and the
    // teleport is parked until the window elapses, so the character lands on
    // loaded terrain instead of falling through it.
    if (!pending.queued) return;
    ArmTeleportPreload(pending);

    // The host teleport bridge places the actor exactly at the requested
    // position (bSweep=false). Coordinates from map markers, imported points
    // or tracked goals are frequently at (or just below) the walkable floor,
    // which leaves the character embedded in the ground. When enabled we lift
    // the destination Z so the character falls onto the surface, then watch
    // the player for a short window and re-issue with a higher lift if it
    // still sank through geometry.
    double landing_lift{};
    std::uint32_t sink_retries{};
    {
        std::scoped_lock lock(g_context.mutex);
        landing_lift = g_context.landing_lift;
        sink_retries = g_context.sink_retries;
    }

    std::array<double, 3> target{};
    std::ranges::copy(pending.position, target.begin());
    if (pending.apply_landing_lift && std::isfinite(landing_lift) && landing_lift > 0.0) {
        target[2] += landing_lift;
    }
    const AnomalyStatusV1 status = IssueTeleport(host, pending.world, pending.player, target.data());
    RecordResult(status);

    if (status.code == ANOMALY_STATUS_V1_OK && pending.apply_landing_lift &&
        std::isfinite(landing_lift) && landing_lift > 0.0) {
        std::scoped_lock lock(g_context.mutex);
        g_context.landing = {};
        g_context.landing.active = true;
        g_context.landing.world = pending.world;
        g_context.landing.player = pending.player;
        std::ranges::copy(pending.position, g_context.landing.base_position);
        g_context.landing.retries = 0;
        g_context.landing.settle_ticks = 0;
    }
}

void DrawImportStatus(const AnomalyUiServiceV1* ui) {
    bool running{};
    std::uint32_t import_status{};
    char import_message[sizeof(g_context.import_message)]{};
    {
        std::scoped_lock lock(g_context.mutex);
        running = g_context.import_running;
        import_status = g_context.import_status;
        std::memcpy(import_message, g_context.import_message, sizeof(import_message));
    }
    if (running) {
        DrawText(ui, g_context.localizer.Text("import.running", "Importing..."));
        return;
    }
    if (import_status == ANOMALY_STATUS_V1_UNAVAILABLE) return;
    const std::string_view message = import_message[0] == '\0'
        ? std::string_view{}
        : std::string_view(import_message);
    DrawStatus(ui, import_status, message.data());
}

void DrawTabCoordinate(
    const AnomalyUiServiceV1* ui, const AnomalyHostApiV1* host, double target[3]) {
    const std::string label = g_context.localizer.Label(
        "tab.coordinates", "Coordinates", "tab-coordinates");
    if (ui->begin_tab_item(
            ui->user, anomaly::sdk::StringView(label), nullptr, 0, 1) == 0) {
        return;
    }

    bool target_changed = ui->input_double(
        ui->user, anomaly::sdk::StringView("X"), &target[0], 0.0, 0.0) != 0;
    target_changed |= ui->input_double(
        ui->user, anomaly::sdk::StringView("Y"), &target[1], 0.0, 0.0) != 0;
    target_changed |= ui->input_double(
        ui->user, anomaly::sdk::StringView("Z"), &target[2], 0.0, 0.0) != 0;
    const std::string current = g_context.localizer.Label(
        "action.current", "Use current coordinates", "current-coordinates");
    if (ui->button(
            ui->user, anomaly::sdk::StringView(current), 0.0F, 0.0F) != 0) {
        if (TryReadCurrentPosition(host, target)) {
            target_changed = true;
            g_context.current_position_unavailable = false;
        } else {
            g_context.current_position_unavailable = true;
        }
    }
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t axis = 0; axis != 3; ++axis) g_context.target[axis] = target[axis];
        if (target_changed) g_context.settings_dirty = true;
    }
    if (g_context.current_position_unavailable) {
        DrawText(ui, g_context.localizer.Text(
            "current.unavailable", "Current coordinates are unavailable"));
    }

    const std::string apply = g_context.localizer.Label(
        "action.apply", "Teleport", "teleport");
    if (ui->button(
            ui->user, anomaly::sdk::StringView(apply), 0.0F, 0.0F) != 0) {
        TryQueueRequest(host, target);
    }
    ui->end_tab_item(ui->user);
}

void DrawTabTrackedTarget(const AnomalyUiServiceV1* ui) {
    const std::string label = g_context.localizer.Label(
        "tab.tracked_target", "Tracked Target", "tab-tracked-target");
    if (ui->begin_tab_item(
            ui->user, anomaly::sdk::StringView(label), nullptr, 0, 1) == 0) {
        return;
    }

    bool available{};
    std::array<double, 3> position{};
    {
        std::scoped_lock lock(g_context.mutex);
        available = g_context.tracked_target_valid;
        position = g_context.tracked_target;
    }

    ui->separator(ui->user);
    if (available) {
        DrawText(ui, g_context.localizer.Format(
            "tracked.position", "Tracked target: {0}",
            std::array{std::string_view(FormatPosition(position))}));
    } else {
        DrawText(ui, g_context.localizer.Text(
            "tracked.unavailable", "No tracked map target available"));
    }

    const std::string refresh = g_context.localizer.Label(
        "action.tracked.refresh", "Read tracked target", "read-tracked-target");
    if (ui->button(
            ui->user, anomaly::sdk::StringView(refresh), 0.0F, 0.0F) != 0) {
        std::scoped_lock lock(g_context.mutex);
        g_context.tracked_target_read_requested = true;
    }

    const std::string teleport = g_context.localizer.Label(
        "action.tracked.teleport", "Teleport to tracked target", "teleport-tracked-target");
    if (ui->button_enabled(
            ui->user, anomaly::sdk::StringView(teleport), 0.0F, 0.0F,
            available ? 1 : 0) != 0) {
        std::scoped_lock lock(g_context.mutex);
        g_context.tracked_target_teleport_requested = true;
    }

    ui->end_tab_item(ui->user);
}

void DrawTabPoints(const AnomalyUiServiceV1* ui, const AnomalyHostApiV1* host) {
    const std::string label = g_context.localizer.Label(
        "tab.points", "Points", "tab-points");
    if (ui->begin_tab_item(
            ui->user, anomaly::sdk::StringView(label), nullptr, 0, 1) == 0) {
        return;
    }

    ui->separator(ui->user);
    DrawText(ui, g_context.localizer.Text("import.title", "Import JSON points"));

    std::string folder;
    std::vector<ImportFile> files;
    bool running{};
    {
        std::scoped_lock lock(g_context.mutex);
        folder = g_context.import_folder;
        files = g_context.import_files;
        running = g_context.import_running;
    }
    if (folder.empty()) {
        DrawText(ui, g_context.localizer.Text(
            "import.folder.empty", "No folder selected"));
    } else {
        DrawText(ui, g_context.localizer.Format(
            "import.folder", "Points folder: {0}",
            std::array{std::string_view(folder)}));
    }
    const std::string choose_label = g_context.localizer.Label(
        "import.choose.folder", "Choose folder", "choose-folder");
    if (ui->button(
            ui->user, anomaly::sdk::StringView(choose_label), 0.0F, 0.0F) != 0) {
        const auto selected = ChooseFolder(folder);
        if (selected) {
            const std::string folder_utf8 = WideToUtf8(selected->native());
            if (!folder_utf8.empty()) {
                std::scoped_lock lock(g_context.mutex);
                g_context.import_folder = folder_utf8;
                g_context.import_folder_selected = true;
                g_context.import_files.clear();
                g_context.import_filter.fill('\0');
            }
        }
    }

    const std::string pick_label = g_context.localizer.Label(
        "import.choose.file", "Choose JSON file", "choose-json-file");
    const std::string picker_id = g_context.localizer.Label(
        "import.choose.file.title", "Select a JSON file", "json-file-picker");
    if (ui->button_enabled(
            ui->user, anomaly::sdk::StringView(pick_label), 0.0F, 0.0F,
            files.empty() || running ? 0 : 1) != 0) {
        ui->open_popup(ui->user, anomaly::sdk::StringView(picker_id));
    }

    {
        ui->set_next_window_size_constraints(ui->user, 360.0F, 240.0F, 0.0F, 0.0F);
        int popup_open = 1;
        if (ui->begin_popup_modal(
                ui->user, anomaly::sdk::StringView(picker_id),
                &popup_open, 0) != 0) {
            const std::string filter_label = g_context.localizer.Label(
                "import.filter", "Filter", "import-filter");
            static_cast<void>(ui->input_text(
                ui->user, anomaly::sdk::StringView(filter_label),
                g_context.import_filter.data(), g_context.import_filter.size(),
                ANOMALY_UI_TEXT_INPUT_V1_NONE));
            const std::string_view filter(g_context.import_filter.data());
            if (ui->begin_child(
                    ui->user, anomaly::sdk::StringView("json-file-list"),
                    0.0F, 260.0F, 0) != 0) {
                for (const ImportFile& file : files) {
                    if (ui->filter_match(
                            ui->user, anomaly::sdk::StringView(filter),
                            anomaly::sdk::StringView(file.label)) == 0) {
                        continue;
                    }
                    const std::string file_label = g_context.localizer.Label(
                        "import.file", file.label, "import-file-" + file.path);
                    if (ui->button(
                            ui->user, anomaly::sdk::StringView(file_label),
                            0.0F, 0.0F) != 0) {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.import_selected_file = file.path;
                        g_context.import_file_selected = true;
                        ui->close_current_popup(ui->user);
                    }
                }
                ui->end_child(ui->user);
            }
            const std::string cancel_label = g_context.localizer.Label(
                "import.cancel", "Cancel", "import-cancel");
            if (ui->button(
                    ui->user, anomaly::sdk::StringView(cancel_label),
                    0.0F, 0.0F) != 0) {
                ui->close_current_popup(ui->user);
            }
            ui->end_popup(ui->user);
        }
    }

    DrawImportStatus(ui);

    ui->separator(ui->user);
    DrawText(ui, g_context.localizer.Text("points.title", "Imported points"));
    std::vector<ImportedPoint> points;
    std::size_t page{};
    {
        std::scoped_lock lock(g_context.mutex);
        points = g_context.imported_points;
        page = g_context.imported_page;
    }
    if (points.empty()) {
        DrawText(ui, g_context.localizer.Text("points.empty", "No imported points"));
        ui->end_tab_item(ui->user);
        return;
    }

    const std::size_t total_pages = (points.size() + kPointsPerPage - 1U) / kPointsPerPage;
    if (page >= total_pages) page = total_pages - 1U;
    const std::size_t begin = page * kPointsPerPage;
    const std::size_t end = std::min(points.size(), begin + kPointsPerPage);

    const std::string page_number = std::to_string(page + 1U);
    const std::string page_total = std::to_string(total_pages);
    DrawText(ui, g_context.localizer.Format(
        "points.page", "Page {0} of {1}",
        std::array{std::string_view(page_number), std::string_view(page_total)}));
    const std::string previous_label = g_context.localizer.Label(
        "points.previous", "Previous", "points-previous");
    if (ui->button_enabled(
            ui->user, anomaly::sdk::StringView(previous_label), 0.0F, 0.0F,
            page > 0U ? 1 : 0) != 0) {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.imported_page > 0U) --g_context.imported_page;
    }
    ui->same_line(ui->user, 0.0F, 8.0F);
    const std::string next_label = g_context.localizer.Label(
        "points.next", "Next", "points-next");
    if (ui->button_enabled(
            ui->user, anomaly::sdk::StringView(next_label), 0.0F, 0.0F,
            page + 1U < total_pages ? 1 : 0) != 0) {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.imported_page + 1U < total_pages) ++g_context.imported_page;
    }

    if (ui->begin_table(
            ui->user, anomaly::sdk::StringView("imported-points"), 4, 0,
            0.0F, 220.0F) == 0) {
        ui->end_tab_item(ui->user);
        return;
    }
    ui->table_next_row(ui->user);
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("points.column.category", "Category"));
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("points.column.name", "Name"));
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("points.column.position", "Coordinates"));
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("points.column.load", "Teleport"));

    std::size_t teleport_index = points.size();
    for (std::size_t index = begin; index < end; ++index) {
        const ImportedPoint& point = points[index];
        ui->table_next_row(ui->user);
        static_cast<void>(ui->table_next_column(ui->user));
        DrawText(ui, point.category);
        static_cast<void>(ui->table_next_column(ui->user));
        DrawText(ui, point.name);
        static_cast<void>(ui->table_next_column(ui->user));
        DrawText(ui, FormatPosition(point.position));
        static_cast<void>(ui->table_next_column(ui->user));
        const std::string teleport_label = g_context.localizer.Label(
            "points.load", "Teleport", "imported-load-" + std::to_string(index));
        if (ui->button(
                ui->user, anomaly::sdk::StringView(teleport_label), 0.0F, 0.0F) != 0) {
            teleport_index = index;
        }
    }
    ui->end_table(ui->user);

    if (teleport_index < points.size()) {
        TryQueueRequest(host, points[teleport_index].position.data());
    }
    ui->end_tab_item(ui->user);
}

void DrawTabSettings(const AnomalyUiServiceV1* ui) {
    const std::string label = g_context.localizer.Label(
        "tab.settings", "Settings", "tab-settings");
    if (ui->begin_tab_item(
            ui->user, anomaly::sdk::StringView(label), nullptr, 0, 1) == 0) {
        return;
    }

    ui->separator(ui->user);
    DrawText(ui, g_context.localizer.Text("hotkey.title", "Teleport hotkeys"));

    bool capturing_forward{};
    std::uint32_t forward_key{};
    double forward_distance{};
    double z_lift{};
    double landing_lift{};
    std::uint32_t sink_retries{};
    double preload_delay{};
    {
        std::scoped_lock lock(g_context.mutex);
        capturing_forward = g_context.capturing_forward;
        forward_key = g_context.forward_hotkey_key;
        forward_distance = g_context.forward_distance;
        z_lift = g_context.z_lift;
        landing_lift = g_context.landing_lift;
        sink_retries = g_context.sink_retries;
        preload_delay = g_context.preload_delay;
    }

    const std::string forward_name = forward_key == 0
        ? g_context.localizer.Text("hotkey.none", "None")
        : VirtualKeyName(forward_key);
    if (capturing_forward) {
        DrawText(ui, g_context.localizer.Text(
            "hotkey.capture", "Press a key, Escape cancels, Backspace clears"));
        static_cast<void>(CaptureHotkey(
            g_context, g_context.capturing_forward, ReplaceForwardHotkey));
    } else {
        std::string capture_label = forward_name;
        capture_label += "###capture-forward";
        if (ui->button(
                ui->user, anomaly::sdk::StringView(capture_label), 0.0F, 0.0F) != 0) {
            std::scoped_lock lock(g_context.mutex);
            g_context.capturing_forward = true;
        }
    }

    ui->separator(ui->user);
    const std::string forward_distance_label = g_context.localizer.Label(
        "settings.distance.forward", "Forward distance", "forward-distance");
    const std::string z_lift_label = g_context.localizer.Label(
        "settings.distance.zlift", "Z offset", "z-offset");
    bool options_changed = ui->input_double(
        ui->user, anomaly::sdk::StringView(forward_distance_label),
        &forward_distance, 100.0, 1000.0) != 0;
    options_changed |= ui->input_double(
        ui->user, anomaly::sdk::StringView(z_lift_label),
        &z_lift, 10.0, 100.0) != 0;
    const std::string landing_lift_label = g_context.localizer.Label(
        "settings.distance.landing_lift", "Landing lift", "landing-lift");
    const std::string sink_retries_label = g_context.localizer.Label(
        "settings.distance.sink_retries", "Sink retries", "sink-retries");
    const std::string preload_delay_label = g_context.localizer.Label(
        "settings.distance.preload_delay", "Preload delay (s)", "preload-delay");
    double sink_retries_input = static_cast<double>(sink_retries);
    double preload_delay_input = preload_delay;
    options_changed |= ui->input_double(
        ui->user, anomaly::sdk::StringView(landing_lift_label),
        &landing_lift, 0.0, 300.0) != 0;
    options_changed |= ui->input_double(
        ui->user, anomaly::sdk::StringView(sink_retries_label),
        &sink_retries_input, 0.0, 4.0) != 0;
    options_changed |= ui->input_double(
        ui->user, anomaly::sdk::StringView(preload_delay_label),
        &preload_delay_input, 0.0, static_cast<float>(kPreloadDelayMaximum)) != 0;
    {
        std::scoped_lock lock(g_context.mutex);
        if (options_changed) {
            if (std::isfinite(forward_distance)) {
                g_context.forward_distance = forward_distance;
            }
            if (std::isfinite(z_lift)) g_context.z_lift = z_lift;
            if (std::isfinite(landing_lift)) {
                g_context.landing_lift = std::clamp(landing_lift, 0.0, kLandingLiftMaximum);
            }
            if (std::isfinite(sink_retries_input)) {
                g_context.sink_retries = std::clamp(
                    static_cast<std::uint32_t>(sink_retries_input), 0U, kSinkRetriesMaximum);
            }
            if (std::isfinite(preload_delay_input)) {
                g_context.preload_delay = std::clamp(
                    preload_delay_input, 0.0, kPreloadDelayMaximum);
            }
            g_context.settings_dirty = true;
        }
    }
    ui->end_tab_item(ui->user);
}

void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui) {
    if (context != &g_context) return;

    const AnomalyHostApiV1* host{};
    double target[3]{};
    {
        std::scoped_lock lock(g_context.mutex);
        host = g_context.host;
        for (std::size_t axis = 0; axis != 3; ++axis) target[axis] = g_context.target[axis];
    }
    if (host == nullptr) return;
    const AnomalyUiServiceV1* input_ui =
        ui != nullptr && ui->service_version == ANOMALY_UI_SERVICE_V1_VERSION
        ? ui
        : nullptr;
    if (!HasUiFunctions(input_ui)) {
        const auto queried_ui = QueryService<AnomalyUiServiceV1>(
            host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
        input_ui = queried_ui.service;
    }
    if (!HasUiFunctions(input_ui)) return;

    int open = 1;
    const std::string title = g_context.localizer.Label(
        "window.title", "Teleport", "teleport");
    anomaly::sdk::UiWindow window(input_ui, title, &open);
    if (!window) return;

    if (input_ui->begin_tab_bar(
            input_ui->user, anomaly::sdk::StringView("teleport-tabs"), 0) != 0) {
        DrawTabCoordinate(input_ui, host, target);
        DrawTabTrackedTarget(input_ui);
        DrawTabPoints(input_ui, host);
        DrawTabSettings(input_ui);
        input_ui->end_tab_bar(input_ui->user);
    }
    DrawResult(input_ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-teleport"),
        anomaly::sdk::StringView("Teleport"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.4.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
