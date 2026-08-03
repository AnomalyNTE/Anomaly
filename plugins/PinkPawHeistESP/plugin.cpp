#include "anomaly/sdk/cpp.hpp"
#include "plugins/common/localization.hpp"

#include "ahud_geometry.hpp"
#include "loot_class_cache.hpp"
#include "loot_refresh_policy.hpp"
#include "rob_bank_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using pink_paw_heist_esp::ProjectedBounds;

constexpr std::size_t kEntityPageCapacity = 256;
constexpr std::uint32_t kMaximumEntityCount = 32768;
constexpr std::size_t kMaximumNameBytes = 1024;
constexpr std::size_t kLootRowsPerPage = 6;
// BankBoxes are stationary; full discovery stops after convergence. Cached identities are
// checked in small batches so manual pickups do not restart the entity-frame scan.
constexpr auto kLootRefreshInterval = std::chrono::seconds(2);
constexpr auto kKnownLootValidationInterval = std::chrono::milliseconds(50);
constexpr auto kPickabilityContextRefreshInterval = std::chrono::milliseconds(250);
constexpr auto kRobBankPreparationRetryInterval = std::chrono::seconds(1);
constexpr std::size_t kKnownLootValidationBatch = 8;
constexpr float kFixedBoxThickness = 1.5F;
constexpr auto kCacheFailureGracePeriod =
    kLootRefreshInterval + std::chrono::milliseconds(750);
constexpr std::size_t kCollectionAttempts = 3;
constexpr std::size_t kMaximumSettingsDocumentBytes = 1024;
constexpr auto kExtractionWorldPollInterval = std::chrono::seconds(1);
constexpr auto kExtractionStateReadDelay = std::chrono::seconds(1);
constexpr auto kExtractionStateRetryInterval = std::chrono::milliseconds(250);
constexpr auto kMapPositionPublishInterval = std::chrono::milliseconds(200);
constexpr auto kMapLootSnapshotInterval = std::chrono::seconds(2);
constexpr auto kMapLootSnapshotChunkPublishInterval = std::chrono::milliseconds(100);
constexpr auto kMapLootSnapshotChunkRetryInterval = std::chrono::milliseconds(250);
constexpr std::size_t kMapLootSnapshotMaximumTextBytes = 768U * 1024U;
constexpr std::size_t kMapLootSnapshotMetadataMaximumBytes = 256U;
constexpr std::size_t kMapLootSnapshotMaximumItemsBytes =
    kMapLootSnapshotMaximumTextBytes - kMapLootSnapshotMetadataMaximumBytes;
constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 8;
constexpr std::uint32_t kPreviousSettingsSchemaVersion = 7;
constexpr std::uint32_t kLegacySettingsSchemaVersion = 6;
constexpr double kDefaultTeleportZOffsetCentimeters = 150.0;
constexpr std::uint32_t kDefaultWebSocketPort = 14514U;
constexpr std::uint32_t kMinimumWebSocketPort = 1U;
constexpr std::uint32_t kMaximumWebSocketPort = 65535U;
constexpr std::string_view kSettingsSchema = R"json(
{"type":"object","additionalProperties":false,"required":["menuOpen","enabled","drawLootBoxes","drawExtractions","showActiveExtractionsOnly","showPickableOnly","minimumValue","teleportZOffset","websocketEnabled","websocketPort"],"properties":{"menuOpen":{"type":"boolean"},"enabled":{"type":"boolean"},"drawLootBoxes":{"type":"boolean"},"drawExtractions":{"type":"boolean"},"showActiveExtractionsOnly":{"type":"boolean"},"showPickableOnly":{"type":"boolean"},"minimumValue":{"type":"integer","minimum":0,"maximum":4294967295},"teleportZOffset":{"type":"number"},"websocketEnabled":{"type":"boolean"},"websocketPort":{"type":"integer","minimum":1,"maximum":65535}}}
)json";

struct LootEntity final {
    AnomalyNteEntitySnapshotV1 snapshot{sizeof(snapshot)};
    std::string class_name;
    std::string label;
    pink_paw_heist_esp::RobBankInspection rob_bank;
    std::uint8_t missing_observations{};
};

struct LootCache final {
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    std::vector<LootEntity> loot;
    bool available{};
};

struct MapLootItem final {
    std::string id;
    std::string json;
};

struct MapLootSnapshotTransfer final {
    std::vector<std::size_t> chunk_ends;
    Clock::time_point next_chunk_publish{};
    std::uint64_t revision{};
    std::size_t next_chunk{};
    bool active{};
};

struct MapSyncState final {
    std::shared_ptr<const LootCache> observed_cache;
    std::vector<MapLootItem> current_loot;
    std::vector<MapLootItem> published_loot;
    MapLootSnapshotTransfer snapshot;
    Clock::time_point next_position_publish{};
    Clock::time_point next_snapshot_publish{};
    std::uint64_t revision{};
    std::uint32_t connected_clients{};
    bool active{};
    bool position_active{};
    bool force_snapshot{};
    bool clear_pending{};
};

enum class ExtractionActivation {
    unknown,
    inactive,
    active,
};

struct ExtractionPoint final {
    AnomalyNteEntitySnapshotV1 snapshot{sizeof(snapshot)};
    std::string id;
    std::string label;
    ExtractionActivation activation{ExtractionActivation::unknown};
};

struct ExtractionCache final {
    std::mutex mutex;
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    std::vector<ExtractionPoint> points;
    std::uint64_t class_id{};
    AnomalyGenerationHandleV1 world{};
    bool available{};
    bool complete{};
    bool activation_settled{};
    Clock::time_point next_world_check{};
    Clock::time_point next_state_refresh{};
    std::atomic_bool refresh_requested{true};
};

struct ExtractionDisplaySnapshot final {
    bool available{};
    std::vector<ExtractionPoint> points;
};

struct Settings final {
    bool menu_open{true};
    bool enabled{true};
    bool draw_loot_boxes{true};
    bool draw_extractions{true};
    bool show_active_extractions_only{true};
    bool show_pickable_only{true};
    std::uint32_t minimum_value{};
    double teleport_z_offset{kDefaultTeleportZOffsetCentimeters};
    bool websocket_enabled{true};
    std::uint32_t websocket_port{kDefaultWebSocketPort};
};

struct DisplaySettings final {
    bool enabled{true};
    bool draw_loot_boxes{true};
    bool draw_extractions{true};
    bool show_active_extractions_only{true};
    bool show_pickable_only{true};
    std::uint32_t minimum_value{};
};

struct PendingTeleport final {
    bool queued{};
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double position[3]{};
};

struct TeleportState final {
    std::mutex mutex;
    const AnomalyHostApiV1* host{};
    PendingTeleport pending{};
    bool has_result{};
    std::uint32_t result_code{ANOMALY_STATUS_V1_UNAVAILABLE};
    char result_message[192]{};
    std::atomic_bool developer_mode{};
};

struct PendingPickup final {
    bool queued{};
    pink_paw_heist_esp::RobBankEntity entity;
};

struct PickupState final {
    std::mutex mutex;
    const AnomalyHostApiV1* host{};
    PendingPickup pending{};
    bool has_result{};
    std::uint32_t result_code{ANOMALY_STATUS_V1_UNAVAILABLE};
    char result_message[192]{};
    std::atomic_bool developer_mode{};
};

struct Context final {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyConfigServiceV1* config{};
    const AnomalyUe5AhudServiceV1* ahud{};
    const AnomalyWebSocketServiceV1* websocket{};
    AnomalyGenerationHandleV1 settings_schema{};
    pink_paw_heist_esp::LootClassCache loot_classes;
    pink_paw_heist_esp::LootRefreshPolicy loot_refresh{kLootRefreshInterval};

    int menu_open{1};
    int enabled{1};
    int draw_loot_boxes{1};
    int draw_extractions{1};
    int show_active_extractions_only{1};
    int show_pickable_only{1};
    std::uint32_t minimum_value{};
    double teleport_z_offset{kDefaultTeleportZOffsetCentimeters};
    int websocket_enabled{1};
    std::uint32_t websocket_port{kDefaultWebSocketPort};
    std::size_t current_page{};

    bool settings_dirty{};
    Clock::time_point last_valid_refresh{};
    Clock::time_point next_known_loot_validation{};
    Clock::time_point next_pickability_context_refresh{};
    Clock::time_point next_rob_bank_preparation{};
    std::size_t known_loot_validation_cursor{};
};

Context g_context;
TeleportState g_teleport;
PickupState g_pickup;
ExtractionCache g_extractions;
pink_paw_heist_esp::RobBankRuntime g_rob_bank;
pink_paw_heist_esp::PinkPawWorldGate g_world_gate;
bool g_in_pink_paw_world{};
std::atomic_bool g_world_gate_refresh_requested{true};
std::atomic<std::shared_ptr<const LootCache>> g_loot_cache;
std::atomic<std::shared_ptr<const ExtractionDisplaySnapshot>> g_extraction_snapshot;
std::atomic<std::shared_ptr<const DisplaySettings>> g_display_settings;
std::atomic_bool g_loot_refresh_requested{true};
AnomalyGenerationHandleV1 g_ahud_subscription{};
MapSyncState g_map_sync;
std::atomic_bool g_websocket_map_enabled{true};

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

class SettingsDocumentReader final {
public:
    explicit SettingsDocumentReader(const std::string_view document) noexcept : document_(document) {}

    bool Read(Settings& settings, const std::uint32_t schema_version) noexcept {
        if (!Consume('{')) return false;

        const bool require_hud_draw_settings = schema_version >= kPreviousSettingsSchemaVersion;
        const bool require_websocket_settings = schema_version >= kSettingsSchemaVersion;

        bool menu_open_seen{};
        bool enabled_seen{};
        bool draw_loot_boxes_seen{};
        bool draw_extractions_seen{};
        bool show_active_extractions_only_seen{};
        bool show_pickable_only_seen{};
        bool minimum_value_seen{};
        bool teleport_z_offset_seen{};
        bool websocket_enabled_seen{};
        bool websocket_port_seen{};
        for (;;) {
            if (Consume('}')) break;

            std::string_view key;
            if (!ReadString(key) || !Consume(':')) return false;
            if (key == "menuOpen") {
                if (menu_open_seen || !ReadBoolean(settings.menu_open)) return false;
                menu_open_seen = true;
            } else if (key == "enabled") {
                if (enabled_seen || !ReadBoolean(settings.enabled)) return false;
                enabled_seen = true;
            } else if (key == "drawLootBoxes") {
                if (draw_loot_boxes_seen || !ReadBoolean(settings.draw_loot_boxes)) return false;
                draw_loot_boxes_seen = true;
            } else if (key == "drawExtractions") {
                if (draw_extractions_seen || !ReadBoolean(settings.draw_extractions)) return false;
                draw_extractions_seen = true;
            } else if (key == "showActiveExtractionsOnly") {
                if (show_active_extractions_only_seen ||
                    !ReadBoolean(settings.show_active_extractions_only)) {
                    return false;
                }
                show_active_extractions_only_seen = true;
            } else if (key == "showPickableOnly") {
                if (show_pickable_only_seen ||
                    !ReadBoolean(settings.show_pickable_only)) {
                    return false;
                }
                show_pickable_only_seen = true;
            } else if (key == "minimumValue") {
                if (minimum_value_seen || !ReadUInt32(settings.minimum_value)) return false;
                minimum_value_seen = true;
            } else if (key == "teleportZOffset") {
                if (teleport_z_offset_seen || !ReadDouble(settings.teleport_z_offset)) {
                    return false;
                }
                teleport_z_offset_seen = true;
            } else if (key == "websocketEnabled") {
                if (!require_websocket_settings || websocket_enabled_seen ||
                    !ReadBoolean(settings.websocket_enabled)) {
                    return false;
                }
                websocket_enabled_seen = true;
            } else if (key == "websocketPort") {
                if (!require_websocket_settings || websocket_port_seen ||
                    !ReadUInt32(settings.websocket_port)) {
                    return false;
                }
                websocket_port_seen = true;
            } else {
                return false;
            }

            if (Consume('}')) break;
            if (!Consume(',')) return false;
        }

        SkipWhitespace();
        return menu_open_seen && enabled_seen &&
            (!require_hud_draw_settings || (draw_loot_boxes_seen && draw_extractions_seen)) &&
            show_active_extractions_only_seen && show_pickable_only_seen &&
            minimum_value_seen && teleport_z_offset_seen &&
            (!require_websocket_settings ||
             (websocket_enabled_seen && websocket_port_seen &&
              settings.websocket_port >= kMinimumWebSocketPort &&
              settings.websocket_port <= kMaximumWebSocketPort)) &&
            cursor_ == document_.size();
    }

private:
    void SkipWhitespace() noexcept {
        while (cursor_ < document_.size()) {
            const char value = document_[cursor_];
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') break;
            ++cursor_;
        }
    }

    bool Consume(const char expected) noexcept {
        SkipWhitespace();
        if (cursor_ == document_.size() || document_[cursor_] != expected) return false;
        ++cursor_;
        return true;
    }

    bool ReadString(std::string_view& value) noexcept {
        SkipWhitespace();
        if (cursor_ == document_.size() || document_[cursor_] != '"') return false;
        const std::size_t start = ++cursor_;
        while (cursor_ < document_.size()) {
            const unsigned char character = static_cast<unsigned char>(document_[cursor_++]);
            if (character == '"') {
                value = document_.substr(start, cursor_ - start - 1);
                return true;
            }
            if (character < 0x20U || character == '\\') return false;
        }
        return false;
    }

    bool ReadBoolean(bool& value) noexcept {
        SkipWhitespace();
        if (document_.substr(cursor_, 4) == "true") {
            cursor_ += 4;
            value = true;
            return true;
        }
        if (document_.substr(cursor_, 5) == "false") {
            cursor_ += 5;
            value = false;
            return true;
        }
        return false;
    }

    bool ReadUInt32(std::uint32_t& value) noexcept {
        SkipWhitespace();
        if (cursor_ == document_.size()) return false;
        if (document_[cursor_] == '0') {
            ++cursor_;
            if (cursor_ < document_.size() && document_[cursor_] >= '0' && document_[cursor_] <= '9') {
                return false;
            }
            value = 0;
            return true;
        }
        if (document_[cursor_] < '1' || document_[cursor_] > '9') return false;

        std::uint64_t parsed{};
        while (cursor_ < document_.size() && document_[cursor_] >= '0' && document_[cursor_] <= '9') {
            parsed = parsed * 10U + static_cast<std::uint64_t>(document_[cursor_] - '0');
            if (parsed > (std::numeric_limits<std::uint32_t>::max)()) return false;
            ++cursor_;
        }
        value = static_cast<std::uint32_t>(parsed);
        return true;
    }

    bool ReadDouble(double& value) noexcept {
        SkipWhitespace();
        const std::size_t start = cursor_;
        if (cursor_ < document_.size() && document_[cursor_] == '-') ++cursor_;
        if (cursor_ == document_.size()) return false;

        if (document_[cursor_] == '0') {
            ++cursor_;
            if (cursor_ < document_.size() && document_[cursor_] >= '0' &&
                document_[cursor_] <= '9') {
                return false;
            }
        } else if (document_[cursor_] >= '1' && document_[cursor_] <= '9') {
            do {
                ++cursor_;
            } while (cursor_ < document_.size() && document_[cursor_] >= '0' &&
                     document_[cursor_] <= '9');
        } else {
            return false;
        }

        if (cursor_ < document_.size() && document_[cursor_] == '.') {
            ++cursor_;
            const std::size_t fraction_start = cursor_;
            while (cursor_ < document_.size() && document_[cursor_] >= '0' &&
                   document_[cursor_] <= '9') {
                ++cursor_;
            }
            if (fraction_start == cursor_) return false;
        }
        if (cursor_ < document_.size() &&
            (document_[cursor_] == 'e' || document_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < document_.size() &&
                (document_[cursor_] == '+' || document_[cursor_] == '-')) {
                ++cursor_;
            }
            const std::size_t exponent_start = cursor_;
            while (cursor_ < document_.size() && document_[cursor_] >= '0' &&
                   document_[cursor_] <= '9') {
                ++cursor_;
            }
            if (exponent_start == cursor_) return false;
        }

        const auto [end, error] = std::from_chars(
            document_.data() + start, document_.data() + cursor_, value,
            std::chars_format::general);
        return error == std::errc{} && end == document_.data() + cursor_ && std::isfinite(value);
    }

    std::string_view document_;
    std::size_t cursor_{};
};

bool ConfigMethodsAvailable(const AnomalyConfigServiceV1* service) noexcept {
    return service != nullptr &&
        HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::register_schema)>(
            service, offsetof(AnomalyConfigServiceV1, register_schema)) &&
        HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::read)>(
            service, offsetof(AnomalyConfigServiceV1, read)) &&
        HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
            service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
}

bool RequestConfiguredWebSocketPort() noexcept {
    const AnomalyWebSocketServiceV1* const websocket = g_context.websocket;
    if (websocket == nullptr || !HasField<AnomalyWebSocketServiceV1,
            decltype(AnomalyWebSocketServiceV1::set_port)>(
            websocket, offsetof(AnomalyWebSocketServiceV1, set_port)) ||
        websocket->set_port == nullptr) {
        return false;
    }
    const std::uint32_t port = std::clamp(
        g_context.websocket_port, kMinimumWebSocketPort, kMaximumWebSocketPort);
    return websocket->set_port(
               websocket->user, static_cast<std::uint16_t>(port)).code ==
        ANOMALY_STATUS_V1_OK;
}

Settings CurrentSettings() noexcept {
    return {
        g_context.menu_open != 0,
        g_context.enabled != 0,
        g_context.draw_loot_boxes != 0,
        g_context.draw_extractions != 0,
        g_context.show_active_extractions_only != 0,
        g_context.show_pickable_only != 0,
        g_context.minimum_value,
        std::isfinite(g_context.teleport_z_offset)
            ? g_context.teleport_z_offset : kDefaultTeleportZOffsetCentimeters,
        g_context.websocket_enabled != 0,
        std::clamp(
            g_context.websocket_port, kMinimumWebSocketPort, kMaximumWebSocketPort)};
}

DisplaySettings CurrentDisplaySettings() noexcept {
    return {
        g_context.enabled != 0,
        g_context.draw_loot_boxes != 0,
        g_context.draw_extractions != 0,
        g_context.show_active_extractions_only != 0,
        g_context.show_pickable_only != 0,
        g_context.minimum_value};
}

void PublishDisplaySettings() {
    g_display_settings.store(
        std::make_shared<const DisplaySettings>(CurrentDisplaySettings()),
        std::memory_order_release);
}

void ApplySettings(const Settings& settings) noexcept {
    g_context.menu_open = settings.menu_open ? 1 : 0;
    g_context.enabled = settings.enabled ? 1 : 0;
    g_context.draw_loot_boxes = settings.draw_loot_boxes ? 1 : 0;
    g_context.draw_extractions = settings.draw_extractions ? 1 : 0;
    g_context.show_active_extractions_only = settings.show_active_extractions_only ? 1 : 0;
    g_context.show_pickable_only = settings.show_pickable_only ? 1 : 0;
    g_context.minimum_value = settings.minimum_value;
    g_context.teleport_z_offset = std::isfinite(settings.teleport_z_offset)
        ? settings.teleport_z_offset : kDefaultTeleportZOffsetCentimeters;
    g_context.websocket_enabled = settings.websocket_enabled ? 1 : 0;
    g_context.websocket_port = std::clamp(
        settings.websocket_port, kMinimumWebSocketPort, kMaximumWebSocketPort);
    g_websocket_map_enabled.store(settings.websocket_enabled, std::memory_order_release);
    g_context.current_page = 0;
    g_context.settings_dirty = false;
}

std::string FormatSettingsDouble(const double value) {
    char buffer[64]{};
    const auto [end, error] = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    return error == std::errc{} ? std::string(buffer, end) : "150";
}

std::string SerializeSettings() {
    const Settings settings = CurrentSettings();
    return std::string{"{\"menuOpen\":"} + (settings.menu_open ? "true" : "false") +
        ",\"enabled\":" + (settings.enabled ? "true" : "false") +
        ",\"drawLootBoxes\":" + (settings.draw_loot_boxes ? "true" : "false") +
        ",\"drawExtractions\":" + (settings.draw_extractions ? "true" : "false") +
        ",\"showActiveExtractionsOnly\":" +
        (settings.show_active_extractions_only ? "true" : "false") +
        ",\"showPickableOnly\":" +
        (settings.show_pickable_only ? "true" : "false") +
        ",\"minimumValue\":" + std::to_string(settings.minimum_value) +
        ",\"teleportZOffset\":" + FormatSettingsDouble(settings.teleport_z_offset) +
        ",\"websocketEnabled\":" + (settings.websocket_enabled ? "true" : "false") +
        ",\"websocketPort\":" + std::to_string(settings.websocket_port) + "}";
}

bool LoadSettings() {
    if (!ConfigMethodsAvailable(g_context.config)) return false;

    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code != ANOMALY_STATUS_V1_OK ||
        (schema_version != kSettingsSchemaVersion &&
         schema_version != kPreviousSettingsSchemaVersion &&
         schema_version != kLegacySettingsSchemaVersion) ||
        size == 0 || size > kMaximumSettingsDocumentBytes) {
        return false;
    }
    const std::uint32_t loaded_schema_version = schema_version;

    std::vector<std::uint8_t> document(size);
    const AnomalyStatusV1 read_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {document.data(), document.size()}, &size);
    if (read_status.code != ANOMALY_STATUS_V1_OK ||
        schema_version != loaded_schema_version ||
        size == 0 || size > document.size()) {
        return false;
    }

    Settings settings;
    const std::string_view serialized(
        reinterpret_cast<const char*>(document.data()), size);
    if (!SettingsDocumentReader(serialized).Read(settings, loaded_schema_version)) {
        return false;
    }
    ApplySettings(settings);
    if (loaded_schema_version != kSettingsSchemaVersion) {
        g_context.settings_dirty = true;
    }
    return true;
}

bool SaveSettings() {
    if (!g_context.settings_dirty) return true;
    if (!ConfigMethodsAvailable(g_context.config)) return false;

    const std::string document = SerializeSettings();
    const AnomalyStatusV1 status = g_context.config->write_atomic(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(document));
    if (status.code != ANOMALY_STATUS_V1_OK) return false;
    g_context.settings_dirty = false;
    return true;
}

bool IsCompleteSnapshot(const std::uint32_t flags) noexcept {
    return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) == 0;
}

bool IsCurrentSnapshot(const std::uint32_t flags) noexcept {
    return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) == 0;
}

bool AppendLootEntity(
    const AnomalyNteEntitySnapshotV1& snapshot, const std::uint64_t frame_generation,
    const AnomalyNteEntitiesServiceV1* entities,
    std::vector<LootEntity>& loot) {
    if (!IsCompleteSnapshot(snapshot.flags) || snapshot.handle.generation != frame_generation) {
        return false;
    }

    const pink_paw_heist_esp::LootClassMetadata* metadata{};
    const auto resolution = g_context.loot_classes.Resolve(
        entities, snapshot.class_id, snapshot.class_name_id, metadata);
    if (resolution == pink_paw_heist_esp::LootClassResolution::retry) return false;
    if (resolution == pink_paw_heist_esp::LootClassResolution::unresolved) return true;
    if (metadata == nullptr || !metadata->bank_box) return true;

    LootEntity entry;
    entry.snapshot = snapshot;
    entry.class_name = metadata->name;
    entry.rob_bank = g_rob_bank.Inspect(snapshot.entity_id, entry.class_name);
    loot.push_back(std::move(entry));
    return true;
}

bool CollectLootOnce(AnomalyNteEntityFrameV1& frame, std::vector<LootEntity>& loot) {
    const anomaly::sdk::Host host(g_context.host);
    const auto service = host.Query<AnomalyNteEntitiesServiceV1>(
        ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION);

    frame = AnomalyNteEntityFrameV1{sizeof(frame)};
    if (!service ||
        !HasField<AnomalyNteEntitiesServiceV1, decltype(AnomalyNteEntitiesServiceV1::frame)>(
            service.get(), offsetof(AnomalyNteEntitiesServiceV1, frame)) ||
        !HasField<AnomalyNteEntitiesServiceV1,
            decltype(AnomalyNteEntitiesServiceV1::page)>(
            service.get(), offsetof(AnomalyNteEntitiesServiceV1, page)) ||
        service->frame == nullptr || service->page == nullptr ||
        service->frame(service->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        !IsCompleteSnapshot(frame.flags) || frame.generation == 0 ||
        frame.entity_count > kMaximumEntityCount) {
        return false;
    }

    loot.clear();
    loot.reserve(frame.entity_count);
    const auto append = [&](const AnomalyNteEntitySnapshotV1& snapshot) {
        return AppendLootEntity(snapshot, frame.generation, service.get(), loot);
    };

    std::array<AnomalyNteEntitySnapshotV1, kEntityPageCapacity> page{};
    std::uint32_t offset{};
    for (;;) {
        for (auto& snapshot : page) snapshot = AnomalyNteEntitySnapshotV1{sizeof(snapshot)};
        AnomalyNteEntityPageRequestV1 request{};
        request.struct_size = sizeof(request);
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = static_cast<std::uint32_t>(page.size());
        AnomalyNteEntityPageResultV1 result{sizeof(result)};
        if (service->page(service->user, &request, page.data(), &result).code !=
                ANOMALY_STATUS_V1_OK ||
            !IsCompleteSnapshot(result.flags) || result.generation != frame.generation ||
            result.total_matches != frame.entity_count || result.returned > page.size()) {
            return false;
        }
        if (offset > result.total_matches) {
            return result.returned == 0 && result.next_offset == result.total_matches;
        }
        if (result.returned > result.total_matches - offset) return false;
        for (std::uint32_t index = 0; index < result.returned; ++index) {
            if (!append(page[index])) return false;
        }
        const std::uint32_t consumed = offset + result.returned;
        if (consumed >= result.total_matches) {
            return result.next_offset == result.total_matches;
        }
        if (result.next_offset != consumed || result.next_offset <= offset) return false;
        offset = result.next_offset;
    }
}

bool CollectLoot(AnomalyNteEntityFrameV1& frame, std::vector<LootEntity>& loot) {
    for (std::size_t attempt = 0; attempt < kCollectionAttempts; ++attempt) {
        AnomalyNteEntityFrameV1 candidate_frame{sizeof(candidate_frame)};
        std::vector<LootEntity> candidate_loot;
        if (!CollectLootOnce(candidate_frame, candidate_loot)) continue;
        frame = candidate_frame;
        loot = std::move(candidate_loot);
        return true;
    }
    return false;
}

bool HasCurrentIdentity(const std::uint32_t flags) noexcept {
    return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0;
}

bool ExtractionServiceAvailable(const AnomalyNteActorsServiceV1* service) noexcept {
    return service != nullptr && service->service_version >= ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION &&
        HasField<AnomalyNteActorsServiceV1,
            decltype(AnomalyNteActorsServiceV1::component_bounds)>(
            service, offsetof(AnomalyNteActorsServiceV1, component_bounds)) &&
        HasField<AnomalyNteActorsServiceV1,
            decltype(AnomalyNteActorsServiceV1::bool_property)>(
            service, offsetof(AnomalyNteActorsServiceV1, bool_property)) &&
        HasField<AnomalyNteActorsServiceV1,
            decltype(AnomalyNteActorsServiceV1::fname_property_utf8)>(
            service, offsetof(AnomalyNteActorsServiceV1, fname_property_utf8)) &&
        service->frame != nullptr && service->page != nullptr &&
        service->class_name_utf8 != nullptr && service->component_bounds != nullptr &&
        service->bool_property != nullptr && service->fname_property_utf8 != nullptr;
}

std::string ReadEntityFName(
    const AnomalyNteActorsServiceV1* service,
    const AnomalyGenerationHandleV1 entity,
    const std::string_view property_name) {
    std::array<char, 128> buffer{};
    std::size_t size = buffer.size();
    AnomalyStatusV1 status = service->fname_property_utf8(
        service->user, entity, anomaly::sdk::StringView(property_name), buffer.data(), &size);
    if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= buffer.size()) {
        return std::string(buffer.data(), size - 1U);
    }
    if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 ||
        size > kMaximumNameBytes) {
        return {};
    }
    std::string value(size, '\0');
    status = service->fname_property_utf8(
        service->user, entity, anomaly::sdk::StringView(property_name), value.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > value.size()) return {};
    value.resize(size - 1U);
    return value;
}

bool CollectExtractionPoints(
    AnomalyNteEntityFrameV1& frame,
    std::vector<ExtractionPoint>& points,
    std::uint64_t& class_id) {
    const anomaly::sdk::Host host(g_context.host);
    const auto actors = host.Query<AnomalyNteActorsServiceV1>(
        ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION);
    if (!actors || !ExtractionServiceAvailable(actors.get())) return false;

    frame = AnomalyNteEntityFrameV1{sizeof(frame)};
    if (actors->frame(actors->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        !HasCurrentIdentity(frame.flags) || frame.generation == 0 ||
        frame.entity_count > kMaximumEntityCount) {
        return false;
    }

    std::array<AnomalyNteEntitySnapshotV1, kEntityPageCapacity> page{};
    if (class_id == 0) {
        pink_paw_heist_esp::LootClassCache class_cache;
        std::uint32_t offset{};
        while (class_id == 0) {
            for (auto& snapshot : page) snapshot = AnomalyNteEntitySnapshotV1{sizeof(snapshot)};
            AnomalyNteEntityPageRequestV1 request{};
            request.struct_size = sizeof(request);
            request.generation = frame.generation;
            request.offset = offset;
            request.capacity = static_cast<std::uint32_t>(page.size());
            AnomalyNteEntityPageResultV1 result{sizeof(result)};
            if (actors->page(actors->user, &request, page.data(), &result).code !=
                    ANOMALY_STATUS_V1_OK ||
                !HasCurrentIdentity(result.flags) || result.generation != frame.generation ||
                result.returned > page.size()) {
                return false;
            }
            for (std::uint32_t index = 0; index < result.returned; ++index) {
                const auto& snapshot = page[index];
                if (!HasCurrentIdentity(snapshot.flags) ||
                    snapshot.handle.generation != frame.generation) {
                    return false;
                }
                const pink_paw_heist_esp::LootClassMetadata* metadata{};
                const auto resolution = class_cache.Resolve(
                    actors.get(), snapshot.class_id, snapshot.class_name_id, metadata);
                if (resolution == pink_paw_heist_esp::LootClassResolution::retry) return false;
                if (resolution == pink_paw_heist_esp::LootClassResolution::resolved &&
                    metadata != nullptr &&
                    metadata->name == pink_paw_heist_esp::kWorldMarkerClassName) {
                    class_id = snapshot.class_id;
                    break;
                }
            }
            const std::uint32_t consumed = offset + result.returned;
            if (class_id != 0) break;
            if (consumed >= result.total_matches) return true;
            if (result.next_offset != consumed || result.next_offset <= offset) return false;
            offset = result.next_offset;
        }
    }

    std::uint32_t offset{};
    std::vector<AnomalyNteEntitySnapshotV1> matches;
    for (;;) {
        for (auto& snapshot : page) snapshot = AnomalyNteEntitySnapshotV1{sizeof(snapshot)};
        AnomalyNteEntityPageRequestV1 request{};
        request.struct_size = sizeof(request);
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = static_cast<std::uint32_t>(page.size());
        request.class_id = class_id;
        AnomalyNteEntityPageResultV1 result{sizeof(result)};
        if (actors->page(actors->user, &request, page.data(), &result).code !=
                ANOMALY_STATUS_V1_OK ||
            !HasCurrentIdentity(result.flags) || result.generation != frame.generation ||
            result.returned > page.size()) {
            return false;
        }
        for (std::uint32_t index = 0; index < result.returned; ++index) {
            const auto& snapshot = page[index];
            if (!HasCurrentIdentity(snapshot.flags) ||
                snapshot.handle.generation != frame.generation ||
                snapshot.class_id != class_id) {
                return false;
            }
            matches.push_back(snapshot);
        }
        const std::uint32_t consumed = offset + result.returned;
        if (consumed >= result.total_matches) break;
        if (result.next_offset != consumed || result.next_offset <= offset) return false;
        offset = result.next_offset;
    }

    points.clear();
    points.reserve(matches.size());
    for (AnomalyNteEntitySnapshotV1 snapshot : matches) {
        AnomalyNteEntityComponentBoundsV1 bounds{sizeof(bounds)};
        if (actors->component_bounds(
                actors->user, snapshot.handle, anomaly::sdk::StringView("Box"), &bounds).code !=
                ANOMALY_STATUS_V1_OK ||
            !HasCurrentIdentity(bounds.flags) ||
            bounds.entity.id != snapshot.handle.id ||
            bounds.entity.generation != snapshot.handle.generation) {
            continue;
        }
        std::ranges::copy(bounds.bounds_center, snapshot.bounds_center);
        std::ranges::copy(bounds.bounds_extent, snapshot.bounds_extent);

        ExtractionPoint point;
        point.snapshot = snapshot;
        point.id = ReadEntityFName(actors.get(), snapshot.handle, "Extract_ID");
        if (point.id.empty()) point.id = "#" + std::to_string(snapshot.entity_id);
        points.push_back(std::move(point));
    }
    std::sort(points.begin(), points.end(), [](const auto& left, const auto& right) {
        if (left.snapshot.bounds_center[1] != right.snapshot.bounds_center[1]) {
            return left.snapshot.bounds_center[1] < right.snapshot.bounds_center[1];
        }
        return left.id < right.id;
    });
    return true;
}

bool RefreshExtractionActivation(
    const AnomalyNteActorsServiceV1* actors,
    std::vector<ExtractionPoint>& points) {
    if (!actors || !ExtractionServiceAvailable(actors)) return false;
    bool complete = true;
    for (ExtractionPoint& point : points) {
        AnomalyNteEntityBoolPropertyV1 active{sizeof(active)};
        if (actors->bool_property(
                actors->user, point.snapshot.handle, anomaly::sdk::StringView("CanOpen"),
                &active).code != ANOMALY_STATUS_V1_OK ||
            !HasCurrentIdentity(active.flags) ||
            active.entity.id != point.snapshot.handle.id ||
            active.entity.generation != point.snapshot.handle.generation) {
            complete = false;
            continue;
        }
        point.activation = active.value != 0
            ? ExtractionActivation::active
            : ExtractionActivation::inactive;
    }
    return complete;
}

bool SameHandle(
    const AnomalyGenerationHandleV1 left,
    const AnomalyGenerationHandleV1 right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

bool CurrentWorld(AnomalyGenerationHandleV1& world) {
    if (g_context.host == nullptr) return false;
    const anomaly::sdk::Host host(g_context.host);
    const auto session = host.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    if (!session ||
        !HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::snapshot)>(
            session.get(), offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
        session->snapshot == nullptr) {
        return false;
    }

    AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
    if (session->snapshot(session->user, &snapshot).code != ANOMALY_STATUS_V1_OK ||
        snapshot.struct_size < sizeof(snapshot) ||
        snapshot.state != ANOMALY_NTE_SESSION_V1_WORLD_READY || snapshot.world.id == 0 ||
        snapshot.world.generation == 0) {
        return false;
    }
    world = snapshot.world;
    return true;
}

std::string BuildExtractionLabel(const ExtractionPoint& point);

void PublishExtractionSnapshotLocked() {
    auto snapshot = std::make_shared<ExtractionDisplaySnapshot>();
    snapshot->available = g_extractions.available;
    snapshot->points = g_extractions.points;
    for (ExtractionPoint& point : snapshot->points) {
        point.label = BuildExtractionLabel(point);
    }
    g_extraction_snapshot.store(std::move(snapshot), std::memory_order_release);
}

void ResetExtractionData() noexcept {
    g_extractions.frame = AnomalyNteEntityFrameV1{sizeof(g_extractions.frame)};
    g_extractions.points.clear();
    g_extractions.class_id = 0;
    g_extractions.available = false;
    g_extractions.complete = false;
    g_extractions.activation_settled = false;
    g_extractions.next_state_refresh = {};
}

void RefreshExtractionCacheIfDue() {
    const Clock::time_point now = Clock::now();
    const bool forced =
        g_extractions.refresh_requested.exchange(false, std::memory_order_acq_rel);
    bool check_world{};
    {
        std::scoped_lock lock(g_extractions.mutex);
        check_world = forced || now >= g_extractions.next_world_check;
        if (!check_world) {
            if (!g_extractions.complete || g_extractions.activation_settled ||
                now < g_extractions.next_state_refresh) {
                return;
            }
        }
    }

    if (check_world) {
        AnomalyGenerationHandleV1 world{};
        if (!CurrentWorld(world)) {
            std::scoped_lock lock(g_extractions.mutex);
            ResetExtractionData();
            g_extractions.world = {};
            g_extractions.next_world_check = now + kExtractionWorldPollInterval;
            g_extraction_snapshot.store({}, std::memory_order_release);
            return;
        }

        std::uint64_t class_id{};
        bool discover{};
        {
            std::scoped_lock lock(g_extractions.mutex);
            g_extractions.next_world_check = now + kExtractionWorldPollInterval;
            if (!SameHandle(g_extractions.world, world)) {
                ResetExtractionData();
                g_extractions.world = world;
                g_extraction_snapshot.store({}, std::memory_order_release);
            }
            discover = !g_extractions.complete;
            class_id = g_extractions.class_id;
        }
        if (discover) {
            AnomalyNteEntityFrameV1 frame{sizeof(frame)};
            std::vector<ExtractionPoint> points;
            const bool available = CollectExtractionPoints(frame, points, class_id);
            std::scoped_lock lock(g_extractions.mutex);
            if (!SameHandle(g_extractions.world, world)) return;
            g_extractions.available = available;
            g_extractions.complete = available;
            if (available) {
                g_extractions.frame = frame;
                g_extractions.points = std::move(points);
                g_extractions.class_id = class_id;
                g_extractions.next_state_refresh = now + kExtractionStateReadDelay;
            } else {
                g_extractions.frame = AnomalyNteEntityFrameV1{sizeof(g_extractions.frame)};
                g_extractions.points.clear();
            }
            PublishExtractionSnapshotLocked();
            return;
        }
    }

    std::vector<ExtractionPoint> points;
    {
        std::scoped_lock lock(g_extractions.mutex);
        if (!g_extractions.complete ||
            (!forced && (g_extractions.activation_settled ||
                now < g_extractions.next_state_refresh))) {
            return;
        }
        points = g_extractions.points;
    }
    const anomaly::sdk::Host host(g_context.host);
    const auto actors = host.Query<AnomalyNteActorsServiceV1>(
        ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION);
    const bool activation_complete = RefreshExtractionActivation(actors.get(), points);
    {
        std::scoped_lock lock(g_extractions.mutex);
        if (g_extractions.complete && points.size() == g_extractions.points.size()) {
            for (std::size_t index = 0; index < points.size(); ++index) {
                if (!SameHandle(points[index].snapshot.handle,
                        g_extractions.points[index].snapshot.handle)) {
                    return;
                }
            }
            g_extractions.points = std::move(points);
            g_extractions.activation_settled = activation_complete && std::ranges::any_of(
                g_extractions.points, [](const ExtractionPoint& point) {
                    return point.activation == ExtractionActivation::inactive;
                });
            g_extractions.next_state_refresh = g_extractions.activation_settled
                ? Clock::time_point{}
                : now + (activation_complete
                    ? kExtractionStateReadDelay
                    : kExtractionStateRetryInterval);
            PublishExtractionSnapshotLocked();
        }
    }
}

void ClearExtractionCache() noexcept {
    std::scoped_lock lock(g_extractions.mutex);
    ResetExtractionData();
    g_extractions.world = {};
    g_extractions.next_world_check = {};
    g_extractions.next_state_refresh = {};
    g_extractions.refresh_requested.store(true, std::memory_order_release);
    g_extraction_snapshot.store({}, std::memory_order_release);
}

void ClearCache() noexcept {
    g_loot_cache.store({}, std::memory_order_release);
    g_loot_refresh_requested.store(true, std::memory_order_release);
    g_context.loot_classes.Clear();
    g_context.loot_refresh.Reset();
    g_context.last_valid_refresh = {};
    g_context.next_known_loot_validation = {};
    g_context.next_pickability_context_refresh = {};
    g_context.next_rob_bank_preparation = {};
    g_context.known_loot_validation_cursor = 0;
}

std::string BuildLootLabel(const LootEntity& entry);

bool SameLootState(
    const LootCache& current,
    const std::vector<LootEntity>& next) noexcept {
    if (current.loot.size() != next.size()) return false;
    for (std::size_t index = 0; index < next.size(); ++index) {
        const LootEntity& left = current.loot[index];
        const LootEntity& right = next[index];
        if (left.snapshot.entity_id != right.snapshot.entity_id ||
            left.snapshot.class_name_id != right.snapshot.class_name_id ||
            left.class_name != right.class_name ||
            left.rob_bank.entity.object_serial != right.rob_bank.entity.object_serial ||
            left.rob_bank.pickability != right.rob_bank.pickability ||
            left.rob_bank.item_resolved != right.rob_bank.item_resolved ||
            left.rob_bank.name_utf8 != right.rob_bank.name_utf8 ||
            left.rob_bank.fons_value != right.rob_bank.fons_value ||
            left.rob_bank.pink_paw_coin_value != right.rob_bank.pink_paw_coin_value) {
            return false;
        }
    }
    return true;
}

void RefreshCacheIfDue() {
    const Clock::time_point now = Clock::now();
    const bool forced =
        g_loot_refresh_requested.exchange(false, std::memory_order_acq_rel);
    if (!g_context.loot_refresh.Begin(now, forced)) return;

    const auto current = g_loot_cache.load(std::memory_order_acquire);
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    std::vector<LootEntity> loot;
    const bool rob_bank_refreshed = g_rob_bank.Refresh();
    g_context.next_rob_bank_preparation = now +
        (rob_bank_refreshed && g_rob_bank.DiscoveryPending()
             ? kKnownLootValidationInterval
             : kRobBankPreparationRetryInterval);
    g_context.next_pickability_context_refresh =
        now + kPickabilityContextRefreshInterval;
    if (g_context.host != nullptr && CollectLoot(frame, loot)) {
        std::sort(loot.begin(), loot.end(), [](const LootEntity& left, const LootEntity& right) {
            if (left.snapshot.entity_id != right.snapshot.entity_id) {
                return left.snapshot.entity_id < right.snapshot.entity_id;
            }
            return left.class_name < right.class_name;
        });
        const bool unchanged =
            !forced && current && current->available && SameLootState(*current, loot);
        const bool has_loot = !loot.empty();
        for (LootEntity& entry : loot) entry.label = BuildLootLabel(entry);
        auto cache = std::make_shared<LootCache>();
        cache->frame = frame;
        cache->loot = std::move(loot);
        cache->available = true;
        g_loot_cache.store(std::move(cache), std::memory_order_release);
        g_context.last_valid_refresh = now;
        g_context.next_known_loot_validation = now + kKnownLootValidationInterval;
        g_context.known_loot_validation_cursor = 0;
        g_context.loot_refresh.Complete(now, unchanged, has_loot);
    } else {
        // Whole-collection retries reject partial data. Keep the last complete frame briefly
        // so a generation race does not make all ESP disappear for a single refresh.
        if (!current || !current->available ||
            now - g_context.last_valid_refresh >= kCacheFailureGracePeriod) {
            g_loot_cache.store({}, std::memory_order_release);
            g_context.last_valid_refresh = {};
        }
        g_context.loot_refresh.Fail(now);
    }
}

struct KnownLootValidationChange final {
    std::size_t index{};
    pink_paw_heist_esp::KnownLootValidationAction action{
        pink_paw_heist_esp::KnownLootValidationAction::unchanged};
    pink_paw_heist_esp::KnownLootValidationState state;
};

void RefreshKnownLootIfDue() {
    const Clock::time_point now = Clock::now();
    bool validate_all{};
    if (g_rob_bank.Available() && g_rob_bank.DiscoveryPending() &&
        now >= g_context.next_rob_bank_preparation) {
        const bool refreshed = g_rob_bank.Refresh();
        validate_all = refreshed && g_rob_bank.PickabilityReady();
        g_context.next_rob_bank_preparation = now +
            (refreshed && g_rob_bank.DiscoveryPending()
                 ? kKnownLootValidationInterval
                 : kRobBankPreparationRetryInterval);
    }
    if (g_rob_bank.CanInspect() && !g_rob_bank.DiscoveryPending() &&
        now >= g_context.next_pickability_context_refresh) {
        validate_all = g_rob_bank.RefreshPickabilityContext() ==
                pink_paw_heist_esp::RobBankContextRefresh::changed ||
            validate_all;
        g_context.next_pickability_context_refresh =
            now + kPickabilityContextRefreshInterval;
    }

    if (!validate_all && now < g_context.next_known_loot_validation) return;
    g_context.next_known_loot_validation = now + kKnownLootValidationInterval;

    const auto current = g_loot_cache.load(std::memory_order_acquire);
    if (!current || !current->available || current->loot.empty() ||
        !g_rob_bank.CanInspect()) {
        return;
    }

    const std::size_t start =
        g_context.known_loot_validation_cursor % current->loot.size();
    const std::size_t count = validate_all
        ? current->loot.size()
        : (std::min)(kKnownLootValidationBatch, current->loot.size());
    std::vector<KnownLootValidationChange> changes;
    changes.reserve(count);
    for (std::size_t offset{}; offset < count; ++offset) {
        const std::size_t index = (start + offset) % current->loot.size();
        const LootEntity& entry = current->loot[index];
        pink_paw_heist_esp::KnownLootValidationState state{
            entry.rob_bank, entry.missing_observations};
        const auto observation =
            g_rob_bank.Inspect(entry.snapshot.entity_id, entry.class_name);
        const auto action =
            pink_paw_heist_esp::ApplyKnownLootObservation(state, observation);
        if (action != pink_paw_heist_esp::KnownLootValidationAction::unchanged) {
            changes.push_back({index, action, state});
        }
    }
    g_context.known_loot_validation_cursor =
        (start + count) % current->loot.size();
    if (changes.empty()) return;

    auto next = std::make_shared<LootCache>(*current);
    std::sort(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
        return left.index > right.index;
    });
    for (const KnownLootValidationChange& change : changes) {
        if (change.action == pink_paw_heist_esp::KnownLootValidationAction::remove) {
            next->loot.erase(next->loot.begin() + static_cast<std::ptrdiff_t>(change.index));
            continue;
        }
        next->loot[change.index].rob_bank = change.state.inspection;
        next->loot[change.index].missing_observations =
            change.state.missing_observations;
        next->loot[change.index].label = BuildLootLabel(next->loot[change.index]);
    }
    g_loot_cache.store(std::move(next), std::memory_order_release);
}

void RemoveCachedLoot(const pink_paw_heist_esp::RobBankEntity entity) {
    if (!entity.Valid()) return;
    const auto current = g_loot_cache.load(std::memory_order_acquire);
    if (!current || !current->available) return;

    auto next = std::make_shared<LootCache>(*current);
    const std::size_t removed = std::erase_if(next->loot, [&](const LootEntity& entry) {
        return entry.rob_bank.entity.object_index == entity.object_index &&
            entry.rob_bank.entity.object_serial == entity.object_serial;
    });
    if (removed != 0) {
        g_loot_cache.store(std::move(next), std::memory_order_release);
    }
}

bool PassesItemFilters(
    const LootEntity& entry,
    const DisplaySettings& settings) noexcept {
    return entry.rob_bank.item_resolved &&
        entry.rob_bank.fons_value >= settings.minimum_value;
}

bool IsPickable(const LootEntity& entry) noexcept {
    return entry.rob_bank.pickability ==
        pink_paw_heist_esp::RobBankPickability::candidate;
}

bool IsVisibleLoot(
    const LootEntity& entry,
    const DisplaySettings& settings) noexcept {
    return PassesItemFilters(entry, settings) &&
        pink_paw_heist_esp::PassesPickabilityFilter(
            entry.rob_bank.pickability, settings.show_pickable_only);
}

std::string FormatValue(const std::uint32_t value) {
    const std::string digits = std::to_string(value);
    std::string formatted;
    formatted.reserve(digits.size() + (digits.size() - 1) / 3);
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (digits.size() - index) % 3 == 0) formatted.push_back(',');
        formatted.push_back(digits[index]);
    }
    return formatted;
}

std::string FormatCoordinate(const double value) {
    if (!std::isfinite(value) ||
        value < static_cast<double>((std::numeric_limits<std::int64_t>::min)()) ||
        value > static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
        return "?";
    }
    return std::to_string(std::llround(value));
}

std::string BuildWorldCoordinates(const AnomalyNteEntitySnapshotV1& snapshot) {
    return "X: " + FormatCoordinate(snapshot.bounds_center[0]) +
        "  Y: " + FormatCoordinate(snapshot.bounds_center[1]) +
        "  Z: " + FormatCoordinate(snapshot.bounds_center[2]);
}

std::string BuildWorldCoordinates(const LootEntity& entry) {
    return BuildWorldCoordinates(entry.snapshot);
}

std::string FonsValueText(const LootEntity& entry) {
    return FormatValue(entry.rob_bank.fons_value);
}

std::string PinkPawCoinValueText(const LootEntity& entry) {
    return FormatValue(entry.rob_bank.pink_paw_coin_value);
}

std::string BuildLootLabel(const LootEntity& entry) {
    const std::string coordinates = BuildWorldCoordinates(entry);
    if (!entry.rob_bank.item_resolved) return {};
    const std::string fons_value = FonsValueText(entry);
    const std::string pink_paw_coin_value = PinkPawCoinValueText(entry);
    const std::array arguments{
        std::string_view(entry.rob_bank.name_utf8), std::string_view(fons_value),
        std::string_view(pink_paw_coin_value), std::string_view(coordinates)};
    return g_context.localizer.Format(
        "loot.label", "{0}\nFons {1}\nPink Paw Coin {2}\n{3}", arguments);
}

std::uint32_t LootColor(const LootEntity& entry) noexcept {
    if (!entry.rob_bank.item_resolved) return ANOMALY_RGBA_V1(170, 170, 170, 255);
    if (entry.rob_bank.fons_value >= 100000U) return ANOMALY_RGBA_V1(255, 196, 64, 255);
    if (entry.rob_bank.fons_value >= 10000U) return ANOMALY_RGBA_V1(255, 122, 92, 255);
    return ANOMALY_RGBA_V1(95, 226, 148, 255);
}

std::vector<const LootEntity*> CollectVisibleLoot(
    const LootCache& cache,
    const DisplaySettings& settings) {
    std::vector<const LootEntity*> visible;
    visible.reserve(cache.loot.size());
    for (const LootEntity& entry : cache.loot) {
        if (IsVisibleLoot(entry, settings)) visible.push_back(&entry);
    }
    std::sort(visible.begin(), visible.end(), [](const LootEntity* left, const LootEntity* right) {
        if (left->rob_bank.fons_value != right->rob_bank.fons_value) {
            return left->rob_bank.fons_value > right->rob_bank.fons_value;
        }
        if (left->rob_bank.pink_paw_coin_value != right->rob_bank.pink_paw_coin_value) {
            return left->rob_bank.pink_paw_coin_value > right->rob_bank.pink_paw_coin_value;
        }
        if (left->snapshot.entity_id != right->snapshot.entity_id) {
            return left->snapshot.entity_id < right->snapshot.entity_id;
        }
        return left->class_name < right->class_name;
    });
    return visible;
}

void Text(const AnomalyUiServiceV1* ui, const std::string_view value) {
    if (ui != nullptr && ui->text != nullptr) {
        ui->text(ui->user, anomaly::sdk::StringView(value));
    }
}

bool Checkbox(const AnomalyUiServiceV1* ui, const std::string_view label, int* value) {
    return ui != nullptr && ui->checkbox != nullptr &&
        ui->checkbox(ui->user, anomaly::sdk::StringView(label), value) != 0;
}

bool Button(
    const AnomalyUiServiceV1* ui, const std::string_view label,
    const float width = 0.0F, const float height = 0.0F) {
    return ui != nullptr && ui->button != nullptr &&
        ui->button(ui->user, anomaly::sdk::StringView(label), width, height) != 0;
}

const AnomalyUiServiceV1* ButtonEnabledUi(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION) return nullptr;
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button_enabled)>(
               ui, offsetof(AnomalyUiServiceV1, button_enabled)) &&
            ui->button_enabled != nullptr
        ? ui : nullptr;
}

bool ButtonEnabled(
    const AnomalyUiServiceV1* ui, const std::string_view label, const bool enabled,
    const float width = 0.0F, const float height = 0.0F) {
    const auto* button_ui = ButtonEnabledUi(ui);
    return button_ui != nullptr && button_ui->button_enabled(
        button_ui->user, anomaly::sdk::StringView(label), width, height, enabled ? 1 : 0) != 0;
}

const AnomalyUiServiceV1* UInt32InputUi(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION) return nullptr;
    const bool available =
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_uint32)>(
            ui, offsetof(AnomalyUiServiceV1, input_uint32)) &&
        ui->input_uint32 != nullptr;
    return available ? ui : nullptr;
}

bool InputUInt32(
    const AnomalyUiServiceV1* ui, const std::string_view label, std::uint32_t* value,
    const std::uint32_t step, const std::uint32_t step_fast) {
    const auto* input_ui = UInt32InputUi(ui);
    return input_ui != nullptr && input_ui->input_uint32(
        input_ui->user, anomaly::sdk::StringView(label), value, step, step_fast) != 0;
}

const AnomalyUiServiceV1* DoubleInputUi(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION) return nullptr;
    const bool available =
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_double)>(
            ui, offsetof(AnomalyUiServiceV1, input_double)) &&
        ui->input_double != nullptr;
    return available ? ui : nullptr;
}

bool InputDouble(
    const AnomalyUiServiceV1* ui, const std::string_view label, double* value,
    const double step, const double step_fast) {
    const auto* input_ui = DoubleInputUi(ui);
    return input_ui != nullptr && input_ui->input_double(
        input_ui->user, anomaly::sdk::StringView(label), value, step, step_fast) != 0;
}

bool DeveloperModeEnabled(const AnomalyUiServiceV1* ui) noexcept {
    const auto* button_ui = ButtonEnabledUi(ui);
    return button_ui != nullptr && button_ui->developer_mode_enabled != nullptr &&
        button_ui->developer_mode_enabled(button_ui->user) != 0;
}

constexpr AnomalyStatusV1 StatusCode(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
struct ServiceQuery final {
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
    if (status.code != ANOMALY_STATUS_V1_OK || table == nullptr) {
        return {nullptr, status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : status};
    }

    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t kServicePrefixSize = offsetof(Service, user) + sizeof(void*);
    if (service->struct_size < kServicePrefixSize || service->service_version < minimum_version) {
        return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};
    }
    return {service, StatusCode(ANOMALY_STATUS_V1_OK)};
}

bool DeveloperModeEnabled(const AnomalyHostApiV1* host) noexcept {
    const auto ui = QueryService<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    return ui &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button_enabled)>(
            ui.service, offsetof(AnomalyUiServiceV1, button_enabled)) &&
        ui.service->button_enabled != nullptr &&
        ui.service->developer_mode_enabled != nullptr &&
        ui.service->developer_mode_enabled(ui.service->user) != 0;
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

bool IsCurrentWorld(const AnomalyNteSessionSnapshotV1& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
        snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY &&
        snapshot.world.id != 0 && snapshot.world.generation != 0;
}

bool IsCurrentPlayer(const AnomalyNtePlayerSnapshotV1& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
        IsCurrentSnapshot(snapshot.flags) && snapshot.handle.id != 0 &&
        snapshot.handle.generation != 0;
}

bool IsFinitePosition(const double position[3]) noexcept {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
        std::isfinite(position[2]);
}

void AppendJsonString(std::string& output, const std::string_view value) {
    constexpr std::string_view kHex = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(kHex[(character >> 4U) & 0x0FU]);
                output.push_back(kHex[character & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

bool AppendJsonNumber(std::string& output, const double value) {
    if (!std::isfinite(value)) return false;
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (error != std::errc{}) return false;
    output.append(buffer.data(), end);
    return true;
}

void AppendJsonUnsigned(std::string& output, const std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error == std::errc{}) output.append(buffer.data(), end);
}

std::string BuildMapLootItemJson(const LootEntity& entry, const std::string_view id) {
    std::string output;
    output.reserve(384U + entry.class_name.size() + entry.rob_bank.name_utf8.size());
    output += "{\"id\":";
    AppendJsonString(output, id);
    output += ",\"entityId\":";
    AppendJsonUnsigned(output, entry.snapshot.entity_id);
    output += ",\"objectIndex\":";
    AppendJsonUnsigned(output, entry.rob_bank.entity.object_index);
    output += ",\"objectSerial\":";
    AppendJsonUnsigned(output, entry.rob_bank.entity.object_serial);
    output += ",\"name\":";
    AppendJsonString(
        output, entry.rob_bank.name_utf8.empty() ? std::string_view(entry.class_name)
                                                 : std::string_view(entry.rob_bank.name_utf8));
    output += ",\"category\":";
    AppendJsonString(output, entry.class_name);
    output += ",\"className\":";
    AppendJsonString(output, entry.class_name);
    output += ",\"fonsValue\":";
    AppendJsonUnsigned(output, entry.rob_bank.fons_value);
    output += ",\"pinkPawCoinValue\":";
    AppendJsonUnsigned(output, entry.rob_bank.pink_paw_coin_value);
    output += ",\"itemResolved\":";
    output += entry.rob_bank.item_resolved ? "true" : "false";
    output += ",\"pickable\":";
    output += IsPickable(entry) ? "true" : "false";
    output += ",\"position\":";
    if (IsFinitePosition(entry.snapshot.bounds_center)) {
        output += "{\"x\":";
        static_cast<void>(AppendJsonNumber(output, entry.snapshot.bounds_center[0]));
        output += ",\"y\":";
        static_cast<void>(AppendJsonNumber(output, entry.snapshot.bounds_center[1]));
        output += ",\"z\":";
        static_cast<void>(AppendJsonNumber(output, entry.snapshot.bounds_center[2]));
        output.push_back('}');
    } else {
        output += "null";
    }
    output.push_back('}');
    return output;
}

std::vector<MapLootItem> BuildMapLootItems(const LootCache& cache) {
    std::vector<MapLootItem> items;
    items.reserve(cache.loot.size());
    for (const LootEntity& entry : cache.loot) {
        const std::string id = std::to_string(entry.snapshot.entity_id);
        items.push_back({id, BuildMapLootItemJson(entry, id)});
    }
    std::sort(items.begin(), items.end(), [](const MapLootItem& left, const MapLootItem& right) {
        return left.id < right.id;
    });
    return items;
}

bool SameMapLootItems(
    const std::vector<MapLootItem>& left, const std::vector<MapLootItem>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index{}; index != left.size(); ++index) {
        if (left[index].id != right[index].id || left[index].json != right[index].json) {
            return false;
        }
    }
    return true;
}

bool PublishMapText(const std::string_view text) noexcept {
    const AnomalyWebSocketServiceV1* const websocket = g_context.websocket;
    if (websocket == nullptr || websocket->publish_text == nullptr) return false;
    const AnomalyStatusV1 status = websocket->publish_text(
        websocket->user, anomaly::sdk::StringView(text));
    return status.code == ANOMALY_STATUS_V1_OK;
}

bool NewMapClientConnected() noexcept {
    const AnomalyWebSocketServiceV1* const websocket = g_context.websocket;
    if (websocket == nullptr || !HasField<AnomalyWebSocketServiceV1,
            decltype(AnomalyWebSocketServiceV1::server_info)>(
            websocket, offsetof(AnomalyWebSocketServiceV1, server_info)) ||
        websocket->server_info == nullptr) {
        return false;
    }
    AnomalyWebSocketServerInfoV1 info{sizeof(info)};
    if (websocket->server_info(websocket->user, &info).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    const bool increased = info.connected_clients > g_map_sync.connected_clients;
    g_map_sync.connected_clients = info.connected_clients;
    return increased;
}

double MapTimestamp() noexcept {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string BuildNavigationStateJson(const AnomalyNtePlayerSnapshotV1& player) {
    std::string output;
    output.reserve(192U);
    output += "{\"type\":\"navi-state\",\"version\":1,\"position\":{\"x\":";
    static_cast<void>(AppendJsonNumber(output, player.position[0]));
    output += ",\"y\":";
    static_cast<void>(AppendJsonNumber(output, player.position[1]));
    output += ",\"z\":";
    static_cast<void>(AppendJsonNumber(output, player.position[2]));
    output += "},\"angle\":null,\"angleConfidence\":0,\"timestamp\":";
    static_cast<void>(AppendJsonNumber(output, MapTimestamp()));
    output.push_back('}');
    return output;
}

std::string BuildNavigationClearJson() {
    std::string output{"{\"type\":\"navi-state\",\"version\":1,\"position\":null,"
                       "\"angle\":null,\"angleConfidence\":0,\"timestamp\":"};
    static_cast<void>(AppendJsonNumber(output, MapTimestamp()));
    output.push_back('}');
    return output;
}

std::vector<std::size_t> BuildLootSnapshotChunkEnds(const std::vector<MapLootItem>& items) {
    std::vector<std::size_t> ends;
    if (items.empty()) {
        ends.push_back(0U);
        return ends;
    }

    std::size_t chunk_start{};
    std::size_t chunk_bytes{};
    for (std::size_t index{}; index != items.size(); ++index) {
        const std::size_t item_bytes = items[index].json.size();
        const std::size_t separator_bytes = index == chunk_start ? 0U : 1U;
        const bool has_room = chunk_bytes <= kMapLootSnapshotMaximumItemsBytes - separator_bytes &&
            item_bytes <= kMapLootSnapshotMaximumItemsBytes - separator_bytes - chunk_bytes;
        if (index != chunk_start && !has_room) {
            ends.push_back(index);
            chunk_start = index;
            chunk_bytes = item_bytes;
        } else {
            chunk_bytes += separator_bytes + item_bytes;
        }
    }
    ends.push_back(items.size());
    return ends;
}

std::string BuildLootSnapshotChunkJson(
    const std::vector<MapLootItem>& items,
    const std::size_t begin,
    const std::size_t end,
    const std::uint64_t revision,
    const std::size_t chunk_index,
    const std::size_t chunk_count) {
    std::string output{"{\"type\":\"loot-snapshot\",\"version\":1,\"revision\":"};
    AppendJsonUnsigned(output, revision);
    output += ",\"chunkIndex\":";
    AppendJsonUnsigned(output, static_cast<std::uint64_t>(chunk_index));
    output += ",\"chunkCount\":";
    AppendJsonUnsigned(output, static_cast<std::uint64_t>(chunk_count));
    output += ",\"items\":[";
    for (std::size_t index = begin; index != end; ++index) {
        if (index != begin) output.push_back(',');
        output += items[index].json;
    }
    output += "]}";
    return output;
}

void CancelLootSnapshot() noexcept {
    MapLootSnapshotTransfer& snapshot = g_map_sync.snapshot;
    snapshot.chunk_ends.clear();
    snapshot.next_chunk_publish = {};
    snapshot.revision = 0;
    snapshot.next_chunk = 0;
    snapshot.active = false;
}

bool BeginLootSnapshot(const Clock::time_point now) {
    std::vector<std::size_t> chunk_ends;
    try {
        chunk_ends = BuildLootSnapshotChunkEnds(g_map_sync.current_loot);
    } catch (...) {
        g_map_sync.force_snapshot = false;
        g_map_sync.next_snapshot_publish = now + kMapLootSnapshotInterval;
        return false;
    }

    MapLootSnapshotTransfer& snapshot = g_map_sync.snapshot;
    snapshot.chunk_ends = std::move(chunk_ends);
    snapshot.next_chunk_publish = now;
    snapshot.revision = g_map_sync.revision + 1U;
    snapshot.next_chunk = 0;
    snapshot.active = true;
    g_map_sync.revision = snapshot.revision;
    g_map_sync.force_snapshot = false;
    return true;
}

bool PublishNextLootSnapshotChunk(const Clock::time_point now) {
    MapLootSnapshotTransfer& snapshot = g_map_sync.snapshot;
    if (!snapshot.active || now < snapshot.next_chunk_publish) return false;
    if (snapshot.next_chunk >= snapshot.chunk_ends.size()) {
        CancelLootSnapshot();
        g_map_sync.force_snapshot = true;
        return false;
    }

    const std::size_t begin = snapshot.next_chunk == 0U
        ? 0U
        : snapshot.chunk_ends[snapshot.next_chunk - 1U];
    const std::size_t end = snapshot.chunk_ends[snapshot.next_chunk];
    try {
        if (!PublishMapText(BuildLootSnapshotChunkJson(
                g_map_sync.current_loot, begin, end, snapshot.revision,
                snapshot.next_chunk, snapshot.chunk_ends.size()))) {
            snapshot.next_chunk_publish = now + kMapLootSnapshotChunkRetryInterval;
            return false;
        }
    } catch (...) {
        snapshot.next_chunk_publish = now + kMapLootSnapshotChunkRetryInterval;
        return false;
    }

    ++snapshot.next_chunk;
    if (snapshot.next_chunk == snapshot.chunk_ends.size()) {
        snapshot.active = false;
        snapshot.chunk_ends.clear();
        snapshot.next_chunk_publish = {};
        snapshot.next_chunk = 0;
        g_map_sync.published_loot = g_map_sync.current_loot;
        g_map_sync.next_snapshot_publish = now + kMapLootSnapshotInterval;
    } else {
        snapshot.next_chunk_publish = now + kMapLootSnapshotChunkPublishInterval;
    }
    return true;
}

bool PublishLootDelta() {
    std::vector<const MapLootItem*> upserts;
    std::vector<std::string_view> removed;
    std::size_t current_index{};
    std::size_t published_index{};
    while (current_index < g_map_sync.current_loot.size() ||
           published_index < g_map_sync.published_loot.size()) {
        if (published_index == g_map_sync.published_loot.size() ||
            (current_index < g_map_sync.current_loot.size() &&
             g_map_sync.current_loot[current_index].id <
                 g_map_sync.published_loot[published_index].id)) {
            upserts.push_back(&g_map_sync.current_loot[current_index++]);
        } else if (current_index == g_map_sync.current_loot.size() ||
                   g_map_sync.published_loot[published_index].id <
                       g_map_sync.current_loot[current_index].id) {
            removed.push_back(g_map_sync.published_loot[published_index++].id);
        } else {
            if (g_map_sync.current_loot[current_index].json !=
                g_map_sync.published_loot[published_index].json) {
                upserts.push_back(&g_map_sync.current_loot[current_index]);
            }
            ++current_index;
            ++published_index;
        }
    }
    if (upserts.empty() && removed.empty()) {
        g_map_sync.published_loot = g_map_sync.current_loot;
        return true;
    }

    const std::uint64_t revision = g_map_sync.revision + 1U;
    std::string output{"{\"type\":\"loot-delta\",\"version\":1,\"revision\":"};
    AppendJsonUnsigned(output, revision);
    output += ",\"upserts\":[";
    for (std::size_t index{}; index != upserts.size(); ++index) {
        if (index != 0U) output.push_back(',');
        output += upserts[index]->json;
    }
    output += "],\"removed\":[";
    for (std::size_t index{}; index != removed.size(); ++index) {
        if (index != 0U) output.push_back(',');
        AppendJsonString(output, removed[index]);
    }
    output += "]}";
    if (!PublishMapText(output)) return false;
    g_map_sync.revision = revision;
    g_map_sync.published_loot = g_map_sync.current_loot;
    return true;
}

bool FlushMapClear() {
    if (!g_map_sync.clear_pending) return true;
    CancelLootSnapshot();
    if (!PublishMapText("{\"type\":\"loot-clear\",\"version\":1}")) return false;
    if (!PublishMapText(BuildNavigationClearJson())) return false;
    g_map_sync.clear_pending = false;
    g_map_sync.observed_cache.reset();
    g_map_sync.current_loot.clear();
    g_map_sync.published_loot.clear();
    g_map_sync.position_active = false;
    g_map_sync.force_snapshot = true;
    return true;
}

void RequestMapClear() noexcept {
    g_map_sync.active = false;
    g_map_sync.clear_pending = true;
    CancelLootSnapshot();
    g_map_sync.next_position_publish = {};
    g_map_sync.next_snapshot_publish = {};
}

void SynchronizeMap(const AnomalyNtePlayerSnapshotV1* const player) {
    if (!FlushMapClear()) return;
    if (!g_map_sync.active) {
        g_map_sync.active = true;
        g_map_sync.force_snapshot = true;
    }
    if (NewMapClientConnected()) g_map_sync.force_snapshot = true;
    const Clock::time_point now = Clock::now();
    if (player != nullptr && now >= g_map_sync.next_position_publish &&
        PublishMapText(BuildNavigationStateJson(*player))) {
        g_map_sync.position_active = true;
        g_map_sync.next_position_publish = now + kMapPositionPublishInterval;
    } else if (player == nullptr && g_map_sync.position_active &&
               PublishMapText(BuildNavigationClearJson())) {
        g_map_sync.position_active = false;
        g_map_sync.next_position_publish = {};
    }

    if (g_map_sync.snapshot.active && g_map_sync.force_snapshot) {
        CancelLootSnapshot();
    }
    const auto cache = g_loot_cache.load(std::memory_order_acquire);
    const bool cache_changed = cache != g_map_sync.observed_cache;
    if (!cache || !cache->available) {
        if (cache_changed) {
            g_map_sync.observed_cache = cache;
            if (g_map_sync.snapshot.active) {
                CancelLootSnapshot();
                g_map_sync.force_snapshot = true;
            }
        }
        return;
    }
    if (cache_changed) {
        std::vector<MapLootItem> next_loot = BuildMapLootItems(*cache);
        const bool loot_changed = !SameMapLootItems(next_loot, g_map_sync.current_loot);
        g_map_sync.observed_cache = cache;
        if (loot_changed) {
            if (g_map_sync.snapshot.active) {
                CancelLootSnapshot();
                g_map_sync.force_snapshot = true;
            }
            g_map_sync.current_loot = std::move(next_loot);
            if (!g_map_sync.force_snapshot && g_map_sync.revision != 0U) {
                if (!PublishLootDelta()) g_map_sync.force_snapshot = true;
            }
        }
    }
    if (g_map_sync.snapshot.active) {
        static_cast<void>(PublishNextLootSnapshotChunk(now));
    } else if ((g_map_sync.force_snapshot || now >= g_map_sync.next_snapshot_publish) &&
               BeginLootSnapshot(now)) {
        static_cast<void>(PublishNextLootSnapshotChunk(now));
    }
}

void SynchronizeMapIfPossible() noexcept {
    if (g_context.websocket == nullptr || g_context.host == nullptr) return;
    try {
        if (!g_websocket_map_enabled.load(std::memory_order_acquire)) {
            if (g_map_sync.active) RequestMapClear();
            if (g_map_sync.clear_pending) static_cast<void>(FlushMapClear());
            return;
        }
        AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
        const AnomalyNtePlayerSnapshotV1* current_player{};
        const auto player = QueryService<AnomalyNtePlayerServiceV1>(
            g_context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
        if (player && HasField<AnomalyNtePlayerServiceV1,
                decltype(AnomalyNtePlayerServiceV1::snapshot)>(
                player.service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) &&
            player.service->snapshot != nullptr &&
            player.service->snapshot(player.service->user, &snapshot).code ==
                ANOMALY_STATUS_V1_OK &&
            IsCurrentPlayer(snapshot) && IsFinitePosition(snapshot.position)) {
            current_player = &snapshot;
        }
        SynchronizeMap(current_player);
    } catch (...) {
    }
}

void RecordTeleportResult(const AnomalyStatusV1 status) noexcept {
    std::scoped_lock lock(g_teleport.mutex);
    g_teleport.has_result = true;
    g_teleport.result_code = status.code;
    g_teleport.result_message[0] = '\0';
    if (status.message.data == nullptr || status.message.size == 0) return;
    const std::size_t count = (std::min)(
        status.message.size, sizeof(g_teleport.result_message) - 1U);
    std::memcpy(g_teleport.result_message, status.message.data, count);
    g_teleport.result_message[count] = '\0';
}

void SetTeleportDeveloperMode(const bool enabled) noexcept {
    g_teleport.developer_mode.store(enabled, std::memory_order_release);
    if (enabled) return;
    std::scoped_lock lock(g_teleport.mutex);
    g_teleport.pending = {};
}

void RecordPickupResult(const AnomalyStatusV1 status) noexcept {
    std::scoped_lock lock(g_pickup.mutex);
    g_pickup.has_result = true;
    g_pickup.result_code = status.code;
    g_pickup.result_message[0] = '\0';
    if (status.message.data == nullptr || status.message.size == 0) return;
    const std::size_t count = (std::min)(
        status.message.size, sizeof(g_pickup.result_message) - 1U);
    std::memcpy(g_pickup.result_message, status.message.data, count);
    g_pickup.result_message[count] = '\0';
}

void SetPickupDeveloperMode(const bool enabled) noexcept {
    g_pickup.developer_mode.store(enabled, std::memory_order_release);
    if (enabled) return;
    std::scoped_lock lock(g_pickup.mutex);
    g_pickup.pending = {};
}

void SetDeveloperMode(const bool enabled) noexcept {
    SetTeleportDeveloperMode(enabled);
    SetPickupDeveloperMode(enabled);
}

void QueueTeleport(
    const AnomalyNteSessionSnapshotV1& session, const AnomalyNtePlayerSnapshotV1& player,
    const double position[3]) noexcept {
    std::scoped_lock lock(g_teleport.mutex);
    g_teleport.pending.queued = true;
    g_teleport.pending.world = session.world;
    g_teleport.pending.player = player.handle;
    for (std::size_t axis = 0; axis != 3; ++axis) {
        g_teleport.pending.position[axis] = position[axis];
    }
    g_teleport.has_result = false;
    g_teleport.result_message[0] = '\0';
}

void TryQueueTeleport(const AnomalyNteEntitySnapshotV1& snapshot) {
    const double position[3]{
        snapshot.bounds_center[0], snapshot.bounds_center[1],
        snapshot.bounds_center[2] + g_context.teleport_z_offset};
    if (!g_teleport.developer_mode.load(std::memory_order_acquire) ||
        !IsFinitePosition(position) || g_context.host == nullptr) {
        RecordTeleportResult(StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT));
        return;
    }

    const auto session = QueryService<AnomalyNteSessionServiceV1>(
        g_context.host, ANOMALY_NTE_SESSION_SERVICE_V1_ID,
        ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    if (!session ||
        !HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::snapshot)>(
            session.service, offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
        session.service->snapshot == nullptr) {
        RecordTeleportResult(session ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : session.status);
        return;
    }
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    const AnomalyStatusV1 session_status = session.service->snapshot(
        session.service->user, &session_snapshot);
    if (session_status.code != ANOMALY_STATUS_V1_OK || !IsCurrentWorld(session_snapshot)) {
        RecordTeleportResult(session_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : session_status);
        return;
    }

    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        g_context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!player ||
        !HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::snapshot)>(
            player.service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) ||
        player.service->snapshot == nullptr) {
        RecordTeleportResult(player ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : player.status);
        return;
    }
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    const AnomalyStatusV1 player_status = player.service->snapshot(
        player.service->user, &player_snapshot);
    if (player_status.code != ANOMALY_STATUS_V1_OK || !IsCurrentPlayer(player_snapshot)) {
        RecordTeleportResult(player_status.code == ANOMALY_STATUS_V1_OK
            ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : player_status);
        return;
    }
    QueueTeleport(session_snapshot, player_snapshot, position);
}

void TryQueueTeleport(const LootEntity& entry) {
    TryQueueTeleport(entry.snapshot);
}

void QueuePickup(
    const pink_paw_heist_esp::RobBankEntity entity) noexcept {
    std::scoped_lock lock(g_pickup.mutex);
    g_pickup.pending.queued = true;
    g_pickup.pending.entity = entity;
    g_pickup.has_result = false;
    g_pickup.result_message[0] = '\0';
}

void TryQueuePickup(const LootEntity& entry) {
    if (!g_pickup.developer_mode.load(std::memory_order_acquire) ||
        !IsCompleteSnapshot(entry.snapshot.flags) || !IsPickable(entry) ||
        !entry.rob_bank.entity.Valid()) {
        RecordPickupResult(StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT));
        return;
    }
    QueuePickup(entry.rob_bank.entity);
}

void DrawTeleportStatus(const AnomalyUiServiceV1* ui) {
    bool queued{};
    bool has_result{};
    std::uint32_t result_code{};
    char result_message[sizeof(g_teleport.result_message)]{};
    {
        std::scoped_lock lock(g_teleport.mutex);
        queued = g_teleport.pending.queued;
        has_result = g_teleport.has_result;
        result_code = g_teleport.result_code;
        std::memcpy(result_message, g_teleport.result_message, sizeof(result_message));
    }
    if (queued) {
        Text(ui, g_context.localizer.Text("teleport.queued", "Teleport: QUEUED"));
    } else if (has_result) {
        const std::string_view result = StatusName(result_code);
        if (result_message[0] != '\0') {
            const std::array arguments{result, std::string_view(result_message)};
            Text(ui, g_context.localizer.Format(
                "teleport.result.detail", "Teleport: {0} - {1}", arguments));
        } else {
            const std::array arguments{result};
            Text(ui, g_context.localizer.Format(
                "teleport.result", "Teleport: {0}", arguments));
        }
    }
}

void DrawPickupStatus(const AnomalyUiServiceV1* ui) {
    bool queued{};
    bool has_result{};
    std::uint32_t result_code{};
    char result_message[sizeof(g_pickup.result_message)]{};
    {
        std::scoped_lock lock(g_pickup.mutex);
        queued = g_pickup.pending.queued;
        has_result = g_pickup.has_result;
        result_code = g_pickup.result_code;
        std::memcpy(result_message, g_pickup.result_message, sizeof(result_message));
    }
    if (queued) {
        Text(ui, g_context.localizer.Text("pickup.queued", "Pickup: QUEUED"));
    } else if (has_result) {
        const std::string_view result = StatusName(result_code);
        if (result_message[0] != '\0') {
            const std::array arguments{result, std::string_view(result_message)};
            Text(ui, g_context.localizer.Format(
                "pickup.result.detail", "Pickup: {0} - {1}", arguments));
        } else {
            const std::array arguments{result};
            Text(ui, g_context.localizer.Format(
                "pickup.result", "Pickup: {0}", arguments));
        }
    }
}

const AnomalyUiServiceV1* TableUi(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION) return nullptr;
    const bool complete =
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_table)>(
            ui, offsetof(AnomalyUiServiceV1, begin_table)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::table_next_row)>(
            ui, offsetof(AnomalyUiServiceV1, table_next_row)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::table_next_column)>(
            ui, offsetof(AnomalyUiServiceV1, table_next_column)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_table)>(
            ui, offsetof(AnomalyUiServiceV1, end_table)) &&
        ui->begin_table != nullptr && ui->table_next_row != nullptr &&
        ui->table_next_column != nullptr && ui->end_table != nullptr;
    return complete ? ui : nullptr;
}

const AnomalyUiServiceV1* ChildUi(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION) return nullptr;
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_child)>(
               ui, offsetof(AnomalyUiServiceV1, begin_child)) &&
            HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_child)>(
                ui, offsetof(AnomalyUiServiceV1, end_child)) &&
            ui->begin_child != nullptr && ui->end_child != nullptr
        ? ui
        : nullptr;
}

const AnomalyUiServiceV1* InlineLayoutUi(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION) return nullptr;
    const bool complete =
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::same_line)>(
            ui, offsetof(AnomalyUiServiceV1, same_line)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::set_cursor_pos_x)>(
            ui, offsetof(AnomalyUiServiceV1, set_cursor_pos_x)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::get_window_size)>(
            ui, offsetof(AnomalyUiServiceV1, get_window_size)) &&
        ui->same_line != nullptr && ui->set_cursor_pos_x != nullptr &&
        ui->get_window_size != nullptr;
    return complete ? ui : nullptr;
}

const AnomalyUiServiceV1* TextLinkUi(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION) return nullptr;
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::text_link)>(
               ui, offsetof(AnomalyUiServiceV1, text_link)) &&
            ui->text_link != nullptr
        ? ui
        : nullptr;
}

void DrawWebSocketInstructions(const AnomalyUiServiceV1* ui) {
    constexpr std::string_view kMapUrl = "https://pph.maante.org/";
    const std::string prefix = g_context.localizer.Text(
        "websocket.instructions.prefix", "Open ");
    const std::string suffix = g_context.localizer.Text(
        "websocket.instructions.suffix", ", then enable real-time positioning in the lower-left corner");
    const auto* const layout_ui = InlineLayoutUi(ui);
    const auto* const link_ui = TextLinkUi(ui);
    if (layout_ui != nullptr && link_ui != nullptr) {
        Text(ui, prefix);
        layout_ui->same_line(layout_ui->user, 0.0F, 0.0F);
        static_cast<void>(link_ui->text_link(
            link_ui->user, anomaly::sdk::StringView(kMapUrl), anomaly::sdk::StringView(kMapUrl)));
        layout_ui->same_line(layout_ui->user, 0.0F, 0.0F);
        Text(ui, suffix);
        return;
    }
    Text(ui, prefix + std::string(kMapUrl) + suffix);
}

void DrawLootRows(
    const AnomalyUiServiceV1* ui, const std::vector<const LootEntity*>& visible_loot,
    const std::size_t first, const std::size_t last, const bool developer_mode) {
    const auto* table_ui = TableUi(ui);
    constexpr std::uint32_t table_flags = ANOMALY_UI_TABLE_V1_NONE;
    if (table_ui != nullptr && table_ui->begin_table(
            table_ui->user, anomaly::sdk::StringView("loot"), developer_mode ? 6 : 4,
            table_flags, 0.0F, developer_mode ? 250.0F : 0.0F) != 0) {
        table_ui->table_next_row(table_ui->user);
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.loot", "Loot"));
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.fons_value", "Fons"));
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.pink_paw_coin_value", "Pink Paw Coin"));
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.world_coordinates", "World coordinates"));
        if (developer_mode) {
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, g_context.localizer.Text("column.teleport", "Teleport"));
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, g_context.localizer.Text("column.pickup", "Pickup"));
        }
        for (std::size_t index = first; index < last; ++index) {
            const LootEntity& entry = *visible_loot[index];
            table_ui->table_next_row(table_ui->user);
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, entry.rob_bank.name_utf8);
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, FonsValueText(entry));
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, PinkPawCoinValueText(entry));
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, BuildWorldCoordinates(entry));
            if (developer_mode) {
                static_cast<void>(table_ui->table_next_column(table_ui->user));
                const std::string button = g_context.localizer.Label(
                    "action.teleport", "Teleport",
                    "loot-teleport-" + std::to_string(entry.snapshot.entity_id));
                if (Button(ui, button)) TryQueueTeleport(entry);
                static_cast<void>(table_ui->table_next_column(table_ui->user));
                const std::string pickup = g_context.localizer.Label(
                    "action.pickup", "Pickup",
                    "loot-pickup-" + std::to_string(entry.snapshot.entity_id));
                if (ButtonEnabled(ui, pickup, IsPickable(entry))) TryQueuePickup(entry);
            }
        }
        table_ui->end_table(table_ui->user);
        return;
    }

    for (std::size_t index = first; index < last; ++index) {
        const LootEntity& entry = *visible_loot[index];
        const std::string coordinates = BuildWorldCoordinates(entry);
        const std::string fons_value = FonsValueText(entry);
        const std::string pink_paw_coin_value = PinkPawCoinValueText(entry);
        const std::array arguments{
            std::string_view(entry.rob_bank.name_utf8), std::string_view(fons_value),
            std::string_view(pink_paw_coin_value), std::string_view(coordinates)};
        Text(ui, g_context.localizer.Format(
            "loot.row", "{0}  {1} Fons  {2} Pink Paw Coin  {3}", arguments));
        if (developer_mode) {
            const std::string button = g_context.localizer.Label(
                "action.teleport", "Teleport",
                "loot-teleport-" + std::to_string(entry.snapshot.entity_id));
            if (Button(ui, button)) TryQueueTeleport(entry);
            const std::string pickup = g_context.localizer.Label(
                "action.pickup", "Pickup",
                "loot-pickup-" + std::to_string(entry.snapshot.entity_id));
            if (ButtonEnabled(ui, pickup, IsPickable(entry))) TryQueuePickup(entry);
        }
    }
}

void DrawLootPagination(
    const AnomalyUiServiceV1* ui, const std::size_t page_count) {
    const std::string current = std::to_string(g_context.current_page + 1);
    const std::string pages = std::to_string(page_count);
    const std::array page_arguments{
        std::string_view(current), std::string_view(pages)};
    Text(ui, g_context.localizer.Format(
        "pagination.summary", "Page {0} / {1}", page_arguments));

    struct PaginationAction final {
        std::string label;
        bool enabled;
        std::size_t destination;
    };
    const bool has_previous = g_context.current_page > 0;
    const bool has_next = g_context.current_page + 1 < page_count;
    const std::array actions{
        PaginationAction{g_context.localizer.Label(
            "action.first", "First", "first-page"), has_previous, 0},
        PaginationAction{g_context.localizer.Label(
            "action.previous", "Previous", "previous-page"), has_previous,
            has_previous ? g_context.current_page - 1 : 0},
        PaginationAction{g_context.localizer.Label(
            "action.next", "Next", "next-page"), has_next,
            has_next ? g_context.current_page + 1 : page_count - 1},
        PaginationAction{g_context.localizer.Label(
            "action.last", "Last", "last-page"), has_next, page_count - 1},
    };
    const auto draw_action = [ui](const PaginationAction& action, const float width = 0.0F) {
        if (ButtonEnabled(ui, action.label, action.enabled, width)) {
            g_context.current_page = action.destination;
        }
    };

    constexpr float kButtonWidth = 72.0F;
    constexpr float kButtonGap = 4.0F;
    constexpr float kPaginationRowHeight = 40.0F;
    const auto* const inline_ui = InlineLayoutUi(ui);
    const auto* const child_ui = ChildUi(ui);
    if (inline_ui != nullptr && child_ui != nullptr && ButtonEnabledUi(ui) != nullptr) {
        const int visible = child_ui->begin_child(
            child_ui->user, anomaly::sdk::StringView("loot-pagination-layout"),
            0.0F, kPaginationRowHeight, 0U);
        if (visible != 0) {
            float window_width{};
            float window_height{};
            inline_ui->get_window_size(inline_ui->user, &window_width, &window_height);
            const float group_width =
                kButtonWidth * static_cast<float>(actions.size()) +
                kButtonGap * static_cast<float>(actions.size() - 1U);
            inline_ui->set_cursor_pos_x(
                inline_ui->user, (std::max)(0.0F, (window_width - group_width) * 0.5F));
            for (std::size_t index{}; index != actions.size(); ++index) {
                if (index != 0U) inline_ui->same_line(inline_ui->user, 0.0F, kButtonGap);
                draw_action(actions[index], kButtonWidth);
            }
            child_ui->end_child(child_ui->user);
            return;
        }
        child_ui->end_child(child_ui->user);
    }

    if (inline_ui != nullptr && ButtonEnabledUi(ui) != nullptr) {
        float window_width{};
        float window_height{};
        inline_ui->get_window_size(inline_ui->user, &window_width, &window_height);
        const float group_width =
            kButtonWidth * static_cast<float>(actions.size()) +
            kButtonGap * static_cast<float>(actions.size() - 1U);
        inline_ui->set_cursor_pos_x(
            inline_ui->user, (std::max)(0.0F, (window_width - group_width) * 0.5F));
        for (std::size_t index{}; index != actions.size(); ++index) {
            if (index != 0U) inline_ui->same_line(inline_ui->user, 0.0F, kButtonGap);
            draw_action(actions[index], kButtonWidth);
        }
        return;
    }

    const auto* table_ui = TableUi(ui);
    if (table_ui != nullptr && table_ui->begin_table(
            table_ui->user, anomaly::sdk::StringView("loot-pagination"),
            static_cast<std::int32_t>(actions.size()), ANOMALY_UI_TABLE_V1_NONE,
            0.0F, 0.0F) != 0) {
        table_ui->table_next_row(table_ui->user);
        for (const PaginationAction& action : actions) {
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            draw_action(action);
        }
        table_ui->end_table(table_ui->user);
        return;
    }

    for (const PaginationAction& action : actions) draw_action(action);
}

std::string ExtractionActivationText(const ExtractionActivation activation) {
    switch (activation) {
    case ExtractionActivation::active:
        return g_context.localizer.Text("extraction.active", "Active");
    case ExtractionActivation::inactive:
        return g_context.localizer.Text("extraction.inactive", "Inactive");
    default:
        return g_context.localizer.Text("extraction.unknown", "Unknown");
    }
}

std::string BuildExtractionLabel(const ExtractionPoint& point) {
    const std::string activation = ExtractionActivationText(point.activation);
    const std::string coordinates = BuildWorldCoordinates(point.snapshot);
    const std::array arguments{
        std::string_view(point.id), std::string_view(activation),
        std::string_view(coordinates)};
    return g_context.localizer.Format(
        "extraction.label", "Extraction {0}\n{1}\n{2}", arguments);
}

std::uint32_t ExtractionColor(const ExtractionActivation activation) noexcept {
    switch (activation) {
    case ExtractionActivation::active: return ANOMALY_RGBA_V1(72, 220, 132, 255);
    case ExtractionActivation::inactive: return ANOMALY_RGBA_V1(235, 72, 72, 255);
    default: return ANOMALY_RGBA_V1(255, 190, 72, 255);
    }
}

bool ShouldShowExtraction(
    const ExtractionPoint& point,
    const DisplaySettings& settings) noexcept {
    return !settings.show_active_extractions_only ||
        point.activation == ExtractionActivation::active;
}

void FilterExtractionPoints(
    std::vector<ExtractionPoint>& points,
    const DisplaySettings& settings) {
    std::erase_if(points, [&](const ExtractionPoint& point) {
        return !ShouldShowExtraction(point, settings);
    });
}

void DrawExtractionRows(
    const AnomalyUiServiceV1* ui,
    const std::vector<ExtractionPoint>& points,
    const bool developer_mode) {
    const auto* table_ui = TableUi(ui);
    constexpr std::uint32_t table_flags = ANOMALY_UI_TABLE_V1_NONE;
    if (table_ui != nullptr && table_ui->begin_table(
            table_ui->user, anomaly::sdk::StringView("extractions"), developer_mode ? 4 : 3,
            table_flags, 0.0F, developer_mode ? 220.0F : 0.0F) != 0) {
        table_ui->table_next_row(table_ui->user);
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.extraction", "Extraction"));
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.status", "Status"));
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.world_coordinates", "World coordinates"));
        if (developer_mode) {
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, g_context.localizer.Text("column.teleport", "Teleport"));
        }
        for (const ExtractionPoint& point : points) {
            table_ui->table_next_row(table_ui->user);
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, point.id);
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, ExtractionActivationText(point.activation));
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, BuildWorldCoordinates(point.snapshot));
            if (developer_mode) {
                static_cast<void>(table_ui->table_next_column(table_ui->user));
                const std::string button = g_context.localizer.Label(
                    "action.teleport", "Teleport",
                    "extract-teleport-" + std::to_string(point.snapshot.entity_id));
                if (Button(ui, button)) TryQueueTeleport(point.snapshot);
            }
        }
        table_ui->end_table(table_ui->user);
        return;
    }

    for (const ExtractionPoint& point : points) {
        const std::string activation = ExtractionActivationText(point.activation);
        const std::string coordinates = BuildWorldCoordinates(point.snapshot);
        const std::array arguments{
            std::string_view(point.id), std::string_view(activation),
            std::string_view(coordinates)};
        Text(ui, g_context.localizer.Format(
            "extraction.row", "{0}  {1}  {2}", arguments));
        if (developer_mode) {
            const std::string button = g_context.localizer.Label(
                "action.teleport", "Teleport",
                "extract-teleport-" + std::to_string(point.snapshot.entity_id));
            if (Button(ui, button)) TryQueueTeleport(point.snapshot);
        }
    }
}

void DrawMenu(const AnomalyUiServiceV1* ui, const LootCache& loot_cache) {
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr) return;
    const bool developer_mode = DeveloperModeEnabled(ui);
    SetDeveloperMode(developer_mode);
    if (ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 640.0F, 500.0F, 4U);
    }
    const int menu_open_before = g_context.menu_open;
    const std::string title = g_context.localizer.Label(
        "window.title", "Pink Paw Heist ESP", "pink-paw-heist-esp");
    const int visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView(title), &g_context.menu_open, 0);
    if (g_context.menu_open != menu_open_before) g_context.settings_dirty = true;
    if (visible != 0) {
        const std::string enabled = g_context.localizer.Label(
            "option.enabled", "Enable display", "enabled");
        const bool changed_enabled = Checkbox(ui, enabled, &g_context.enabled);
        const std::string loot_boxes = g_context.localizer.Label(
            "option.draw_loot_boxes", "Draw loot boxes", "draw-loot-boxes");
        const bool changed_loot_boxes = Checkbox(
            ui, loot_boxes, &g_context.draw_loot_boxes);
        const std::string extractions = g_context.localizer.Label(
            "option.draw_extractions", "Draw extractions", "draw-extractions");
        const bool changed_extractions = Checkbox(
            ui, extractions, &g_context.draw_extractions);
        const std::string active_extractions = g_context.localizer.Label(
            "option.active_extractions_only", "Show active extractions only",
            "active-extractions-only");
        const bool changed_active_only = Checkbox(
            ui, active_extractions, &g_context.show_active_extractions_only);
        const std::string pickable_only = g_context.localizer.Label(
            "option.pickable_only", "Show pickable loot only", "pickable-only");
        const bool changed_pickable_only = Checkbox(
            ui, pickable_only, &g_context.show_pickable_only);
        const bool supports_numeric_input = UInt32InputUi(ui) != nullptr;
        const std::string minimum_value = g_context.localizer.Label(
            "option.minimum_value", "Minimum value", "minimum-value");
        const bool changed_minimum =
            InputUInt32(ui, minimum_value, &g_context.minimum_value, 1000, 10000);
        const bool supports_double_input = DoubleInputUi(ui) != nullptr;
        const std::string teleport_offset = g_context.localizer.Label(
            "option.teleport_z_offset", "Teleport Z offset (cm)", "teleport-z-offset");
        const bool changed_teleport_offset = developer_mode && InputDouble(
            ui, teleport_offset, &g_context.teleport_z_offset, 10.0, 100.0);
        const std::string websocket_enabled = g_context.localizer.Label(
            "option.websocket_enabled", "Enable WebSocket real-time positioning",
            "websocket-enabled");
        const bool changed_websocket_enabled = Checkbox(
            ui, websocket_enabled, &g_context.websocket_enabled);
        const std::string websocket_port = g_context.localizer.Label(
            "option.websocket_port", "WebSocket port", "websocket-port");
        const bool changed_websocket_port = InputUInt32(
            ui, websocket_port, &g_context.websocket_port, 1U, 100U);
        if (changed_websocket_port) {
            g_context.websocket_port = std::clamp(
                g_context.websocket_port, kMinimumWebSocketPort, kMaximumWebSocketPort);
            static_cast<void>(RequestConfiguredWebSocketPort());
        }
        if (changed_websocket_enabled) {
            g_websocket_map_enabled.store(
                g_context.websocket_enabled != 0, std::memory_order_release);
        }
        const bool changed_display_settings =
            changed_enabled || changed_loot_boxes || changed_extractions || changed_active_only ||
            changed_pickable_only || changed_minimum || changed_teleport_offset;
        if (changed_display_settings) {
            g_context.settings_dirty = true;
            g_context.current_page = 0;
            PublishDisplaySettings();
        }
        if (changed_websocket_enabled || changed_websocket_port) g_context.settings_dirty = true;
        if (!supports_numeric_input) {
            const std::string value = std::to_string(g_context.minimum_value);
            const std::array arguments{std::string_view(value)};
            Text(ui, g_context.localizer.Format(
                "option.minimum_value.summary", "Minimum value: {0}", arguments));
            const std::string port = std::to_string(g_context.websocket_port);
            const std::array port_arguments{std::string_view(port)};
            Text(ui, g_context.localizer.Format(
                "option.websocket_port.summary", "WebSocket port: {0}", port_arguments));
        }
        DrawWebSocketInstructions(ui);
        if (developer_mode && !supports_double_input) {
            const std::string value = FormatSettingsDouble(g_context.teleport_z_offset);
            const std::array arguments{std::string_view(value)};
            Text(ui, g_context.localizer.Format(
                "option.teleport_z_offset.summary", "Teleport Z offset (cm): {0}", arguments));
        }
        const std::string refresh = g_context.localizer.Label(
            "action.refresh", "Refresh now", "refresh-now");
        if (Button(ui, refresh)) {
            g_world_gate_refresh_requested.store(true, std::memory_order_release);
            g_loot_refresh_requested.store(true, std::memory_order_release);
            g_extractions.refresh_requested.store(true, std::memory_order_release);
        }

        const DisplaySettings display_settings = CurrentDisplaySettings();
        const auto extraction_snapshot =
            g_extraction_snapshot.load(std::memory_order_acquire);
        const bool extraction_available =
            extraction_snapshot && extraction_snapshot->available;
        std::vector<ExtractionPoint> extraction_points = extraction_snapshot
            ? extraction_snapshot->points : std::vector<ExtractionPoint>{};
        FilterExtractionPoints(extraction_points, display_settings);
        const std::string extraction_count = std::to_string(extraction_points.size());
        const std::array extraction_arguments{std::string_view(extraction_count)};
        Text(ui, g_context.localizer.Format(
            "extraction.count", "Extractions: {0}", extraction_arguments));
        if (!extraction_available) {
            Text(ui, g_context.localizer.Text(
                "extraction.unavailable", "Extraction data unavailable"));
        } else if (extraction_points.empty()) {
            Text(ui, g_context.show_active_extractions_only != 0
                    ? g_context.localizer.Text(
                          "extraction.no_active", "No active extractions")
                    : g_context.localizer.Text(
                          "extraction.none", "No extractions on the current map"));
        } else {
            DrawExtractionRows(ui, extraction_points, developer_mode);
        }

        const std::vector<const LootEntity*> visible_loot =
            CollectVisibleLoot(loot_cache, display_settings);
        const std::size_t visible_count = visible_loot.size();
        const std::string total = std::to_string(loot_cache.loot.size());
        const std::string visible_total = std::to_string(visible_count);
        const std::array summary_arguments{
            std::string_view(total), std::string_view(visible_total)};
        Text(ui, g_context.localizer.Format(
            "loot.summary", "Current BankBox frame: {0}, visible: {1}", summary_arguments));
        if (!loot_cache.available) {
            Text(ui, g_context.localizer.Text(
                "loot.snapshot_unavailable", "Entity snapshot unavailable"));
        } else if (visible_loot.empty()) {
            Text(ui, g_context.localizer.Text(
                "loot.none_above_threshold", "No loot meets the current value threshold"));
        } else {
            const std::size_t page_count =
                (visible_count + kLootRowsPerPage - 1) / kLootRowsPerPage;
            g_context.current_page = (std::min)(g_context.current_page, page_count - 1);
            const std::size_t first = g_context.current_page * kLootRowsPerPage;
            const std::size_t last = (std::min)(first + kLootRowsPerPage, visible_count);
            DrawLootRows(ui, visible_loot, first, last, developer_mode);
            DrawLootPagination(ui, page_count);
        }
        if (developer_mode) {
            DrawTeleportStatus(ui);
            DrawPickupStatus(ui);
        }
    }
    ui->end_window(ui->user);
}

bool AhudServiceAvailable(const AnomalyUe5AhudServiceV1* service) noexcept {
    return service != nullptr &&
        HasField<AnomalyUe5AhudServiceV1,
            decltype(AnomalyUe5AhudServiceV1::service_version)>(
            service, offsetof(AnomalyUe5AhudServiceV1, service_version)) &&
        service->service_version >= ANOMALY_UE5_AHUD_SERVICE_V1_VERSION &&
        HasField<AnomalyUe5AhudServiceV1,
            decltype(AnomalyUe5AhudServiceV1::subscribe)>(
            service, offsetof(AnomalyUe5AhudServiceV1, subscribe)) &&
        HasField<AnomalyUe5AhudServiceV1,
            decltype(AnomalyUe5AhudServiceV1::unsubscribe)>(
            service, offsetof(AnomalyUe5AhudServiceV1, unsubscribe)) &&
        service->subscribe != nullptr && service->unsubscribe != nullptr;
}

bool AhudFrameAvailable(const AnomalyUe5AhudFrameV1* frame) noexcept {
    return frame != nullptr &&
        HasField<AnomalyUe5AhudFrameV1,
            decltype(AnomalyUe5AhudFrameV1::viewport_height)>(
            frame, offsetof(AnomalyUe5AhudFrameV1, viewport_height)) &&
        frame->viewport_width != 0 && frame->viewport_height != 0 &&
        HasField<AnomalyUe5AhudFrameV1,
            decltype(AnomalyUe5AhudFrameV1::project)>(
            frame, offsetof(AnomalyUe5AhudFrameV1, project)) &&
        HasField<AnomalyUe5AhudFrameV1,
            decltype(AnomalyUe5AhudFrameV1::measure_text)>(
            frame, offsetof(AnomalyUe5AhudFrameV1, measure_text)) &&
        HasField<AnomalyUe5AhudFrameV1,
            decltype(AnomalyUe5AhudFrameV1::draw_text)>(
            frame, offsetof(AnomalyUe5AhudFrameV1, draw_text)) &&
        HasField<AnomalyUe5AhudFrameV1,
            decltype(AnomalyUe5AhudFrameV1::draw_line)>(
            frame, offsetof(AnomalyUe5AhudFrameV1, draw_line)) &&
        frame->project != nullptr && frame->measure_text != nullptr &&
        frame->draw_text != nullptr && frame->draw_line != nullptr;
}

bool ProjectBounds(
    const AnomalyUe5AhudFrameV1* frame,
    const AnomalyNteEntitySnapshotV1& snapshot,
    ProjectedBounds& bounds) noexcept {
    for (std::size_t axis{}; axis != 3; ++axis) {
        if (!std::isfinite(snapshot.bounds_center[axis]) ||
            !std::isfinite(snapshot.bounds_extent[axis]) ||
            snapshot.bounds_extent[axis] < 0.0) {
            return false;
        }
    }

    float left = (std::numeric_limits<float>::max)();
    float top = (std::numeric_limits<float>::max)();
    float right = (std::numeric_limits<float>::lowest)();
    float bottom = (std::numeric_limits<float>::lowest)();
    for (std::uint32_t corner{}; corner != 8; ++corner) {
        double world[3]{};
        for (std::size_t axis{}; axis != 3; ++axis) {
            const double sign = (corner & (1U << axis)) != 0 ? 1.0 : -1.0;
            world[axis] = snapshot.bounds_center[axis] + snapshot.bounds_extent[axis] * sign;
        }
        float screen[2]{};
        double depth{};
        if (frame->project(frame->user, world, screen, &depth) == 0 ||
            !std::isfinite(screen[0]) || !std::isfinite(screen[1]) ||
            !std::isfinite(depth)) {
            return false;
        }
        left = (std::min)(left, screen[0]);
        top = (std::min)(top, screen[1]);
        right = (std::max)(right, screen[0]);
        bottom = (std::max)(bottom, screen[1]);
    }

    const float viewport_width = static_cast<float>(frame->viewport_width);
    const float viewport_height = static_cast<float>(frame->viewport_height);
    if (right < 0.0F || bottom < 0.0F || left > viewport_width || top > viewport_height) {
        return false;
    }
    bounds = {left, top, right, bottom};
    return bounds.right - bounds.left >= 1.0F && bounds.bottom - bounds.top >= 1.0F;
}

void DrawBounds(
    const AnomalyUe5AhudFrameV1* frame,
    const ProjectedBounds& bounds,
    const std::uint32_t color) noexcept {
    constexpr std::uint32_t kOutlineColor = ANOMALY_RGBA_V1(0, 0, 0, 220);
    constexpr float kOutlineThickness = kFixedBoxThickness + 2.0F;
    const auto lines = pink_paw_heist_esp::ClipOutlineToViewport(
        bounds,
        static_cast<float>(frame->viewport_width),
        static_cast<float>(frame->viewport_height));
    for (const auto& line : std::span(lines.values).first(lines.count)) {
        static_cast<void>(frame->draw_line(
            frame->user, line.start_x, line.start_y, line.end_x, line.end_y,
            kOutlineColor, kOutlineThickness));
    }
    for (const auto& line : std::span(lines.values).first(lines.count)) {
        static_cast<void>(frame->draw_line(
            frame->user, line.start_x, line.start_y, line.end_x, line.end_y,
            color, kFixedBoxThickness));
    }
}

struct MeasuredTextLine final {
    std::string_view text;
    float width{};
    float height{};
};

void DrawLabel(
    const AnomalyUe5AhudFrameV1* frame,
    const ProjectedBounds& bounds,
    const std::string_view label,
    const std::uint32_t color) noexcept {
    constexpr float kBaseScale = 1.0F;
    constexpr float kLabelGap = 4.0F;
    constexpr float kShadowOffset = 1.0F;
    constexpr std::uint32_t kShadowColor = ANOMALY_RGBA_V1(0, 0, 0, 230);
    std::array<MeasuredTextLine, 8> lines{};
    std::size_t line_count{};
    float maximum_width{};
    float total_height{};
    for (std::size_t start{}; start <= label.size() && line_count < lines.size();) {
        const std::size_t newline = label.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? label.size() : newline;
        const std::string_view text = label.substr(start, end - start);
        float width{};
        float height{};
        if (!text.empty() &&
            frame->measure_text(
                frame->user, anomaly::sdk::StringView(text), kBaseScale, &width, &height) != 0 &&
            std::isfinite(width) && std::isfinite(height) && width >= 0.0F && height > 0.0F) {
            lines[line_count++] = {text, width, height};
            maximum_width = (std::max)(maximum_width, width);
            total_height += height;
        }
        if (newline == std::string_view::npos) break;
        start = newline + 1U;
    }
    if (line_count == 0) return;

    const float viewport_width = static_cast<float>(frame->viewport_width);
    const float viewport_height = static_cast<float>(frame->viewport_height);
    const float scale = pink_paw_heist_esp::FitTextScaleToViewport(
        viewport_width, viewport_height, maximum_width, total_height, kShadowOffset);
    if (scale <= 0.0F) return;
    for (MeasuredTextLine& line : std::span(lines).first(line_count)) {
        line.width *= scale;
        line.height *= scale;
    }
    total_height *= scale;
    const float drawable_width = viewport_width - kShadowOffset;
    const float drawable_height = viewport_height - kShadowOffset;
    float y = bounds.bottom + kLabelGap;
    if (y + total_height > drawable_height) y = bounds.top - kLabelGap - total_height;
    y = std::clamp(y, 0.0F, (std::max)(0.0F, drawable_height - total_height));
    const float center = (bounds.left + bounds.right) * 0.5F;
    for (const MeasuredTextLine& line : std::span(lines).first(line_count)) {
        const float maximum_x = (std::max)(0.0F, drawable_width - line.width);
        const float x = std::clamp(center - line.width * 0.5F, 0.0F, maximum_x);
        static_cast<void>(frame->draw_text(
            frame->user, anomaly::sdk::StringView(line.text),
            x + kShadowOffset, y + kShadowOffset, kShadowColor, scale));
        static_cast<void>(frame->draw_text(
            frame->user, anomaly::sdk::StringView(line.text), x, y, color, scale));
        y += line.height;
    }
}

void DrawAhudEntity(
    const AnomalyUe5AhudFrameV1* frame,
    const AnomalyNteEntitySnapshotV1& snapshot,
    const std::string_view label,
    const std::uint32_t color,
    const bool draw_bounds) noexcept {
    ProjectedBounds bounds;
    if (!ProjectBounds(frame, snapshot, bounds)) return;
    if (draw_bounds) DrawBounds(frame, bounds, color);
    DrawLabel(frame, bounds, label, color);
}

void ANOMALY_CALL DrawAhud(
    void*,
    const AnomalyUe5AhudFrameV1* frame) noexcept {
    if (!AhudFrameAvailable(frame)) return;
    const auto settings = g_display_settings.load(std::memory_order_acquire);
    if (!settings || !settings->enabled) return;

    const auto loot_cache = g_loot_cache.load(std::memory_order_acquire);
    if (loot_cache && loot_cache->available) {
        for (const LootEntity& entry : loot_cache->loot) {
            if (!IsVisibleLoot(entry, *settings)) continue;
            DrawAhudEntity(
                frame, entry.snapshot, entry.label, LootColor(entry), settings->draw_loot_boxes);
        }
    }

    if (!settings->draw_extractions) return;
    const auto extraction_snapshot =
        g_extraction_snapshot.load(std::memory_order_acquire);
    if (!extraction_snapshot || !extraction_snapshot->available) return;
    for (const ExtractionPoint& point : extraction_snapshot->points) {
        if (!ShouldShowExtraction(point, *settings)) continue;
        DrawAhudEntity(
            frame, point.snapshot, point.label, ExtractionColor(point.activation), false);
    }
}

AnomalyStatusV1 SubscribeAhud() noexcept {
    if (!AhudServiceAvailable(g_context.ahud)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    AnomalyGenerationHandleV1 handle{};
    const AnomalyStatusV1 status = g_context.ahud->subscribe(
        g_context.ahud->user, DrawAhud, nullptr, &handle);
    if (status.code != ANOMALY_STATUS_V1_OK) return status;
    if (handle.id == 0 || handle.generation == 0) {
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }
    g_ahud_subscription = handle;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 UnsubscribeAhud() noexcept {
    if (g_ahud_subscription.id == 0) return anomaly::sdk::Ok();
    const AnomalyGenerationHandleV1 handle = g_ahud_subscription;
    g_ahud_subscription = {};
    if (!AhudServiceAvailable(g_context.ahud)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    const AnomalyStatusV1 status =
        g_context.ahud->unsubscribe(g_context.ahud->user, handle);
    if (status.code == ANOMALY_STATUS_V1_OK ||
        status.code == ANOMALY_STATUS_V1_NOT_FOUND ||
        status.code == ANOMALY_STATUS_V1_UNAVAILABLE) {
        return anomaly::sdk::Ok();
    }
    return status;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const anomaly::sdk::Host host_view(host);
    const auto ui = host_view.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    const auto config = host_view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    const auto ahud = host_view.Query<AnomalyUe5AhudServiceV1>(
        ANOMALY_UE5_AHUD_SERVICE_V1_ID, ANOMALY_UE5_AHUD_SERVICE_V1_VERSION);
    const auto websocket = host_view.Query<AnomalyWebSocketServiceV1>(
        ANOMALY_WEBSOCKET_SERVICE_V1_ID, ANOMALY_WEBSOCKET_SERVICE_V1_VERSION);
    if (!ui || !HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::button_enabled)>(
            ui.get(), offsetof(AnomalyUiServiceV1, button_enabled)) ||
        ui->button_enabled == nullptr || !ConfigMethodsAvailable(config.get()) ||
        !ahud || !AhudServiceAvailable(ahud.get())) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    g_context = {};
    g_websocket_map_enabled.store(true, std::memory_order_release);
    g_ahud_subscription = {};
    g_loot_cache.store({}, std::memory_order_release);
    g_extraction_snapshot.store({}, std::memory_order_release);
    g_display_settings.store({}, std::memory_order_release);
    g_context.host = host;
    g_context.localizer = anomaly::plugins::Localizer(host);
    g_context.config = config.get();
    g_context.ahud = ahud.get();
    g_context.websocket = websocket &&
            HasField<AnomalyWebSocketServiceV1,
                decltype(AnomalyWebSocketServiceV1::publish_text)>(
                websocket.get(), offsetof(AnomalyWebSocketServiceV1, publish_text)) &&
            websocket->publish_text != nullptr
        ? websocket.get()
        : nullptr;
    g_map_sync = {};
    const AnomalyStatusV1 schema_status = g_context.config->register_schema(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(kSettingsSchema), &g_context.settings_schema);
    if (schema_status.code != ANOMALY_STATUS_V1_OK || g_context.settings_schema.id == 0 ||
        g_context.settings_schema.generation == 0) {
        g_context = {};
        return schema_status.code == ANOMALY_STATUS_V1_OK
            ? AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}}
            : schema_status;
    }
    static_cast<void>(LoadSettings());
    static_cast<void>(RequestConfiguredWebSocketPort());
    PublishDisplaySettings();
    {
        std::scoped_lock lock(g_teleport.mutex);
        g_teleport.host = host;
        g_teleport.pending = {};
        g_teleport.has_result = false;
        g_teleport.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_teleport.result_message[0] = '\0';
    }
    g_teleport.developer_mode.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(g_pickup.mutex);
        g_pickup.host = host;
        g_pickup.pending = {};
        g_pickup.has_result = false;
        g_pickup.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_pickup.result_message[0] = '\0';
    }
    g_pickup.developer_mode.store(false, std::memory_order_release);
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    {
        std::scoped_lock lock(g_teleport.mutex);
        g_teleport.pending = {};
        g_teleport.has_result = false;
        g_teleport.result_message[0] = '\0';
    }
    {
        std::scoped_lock lock(g_pickup.mutex);
        g_pickup.pending = {};
        g_pickup.has_result = false;
        g_pickup.result_message[0] = '\0';
    }
    static_cast<void>(g_rob_bank.Start(g_context.host));
    g_world_gate.Reset();
    g_in_pink_paw_world = false;
    g_world_gate_refresh_requested.store(false, std::memory_order_release);
    ClearCache();
    ClearExtractionCache();
    g_map_sync = {};
    PublishDisplaySettings();
    const AnomalyStatusV1 ahud_status = SubscribeAhud();
    if (ahud_status.code != ANOMALY_STATUS_V1_OK) {
        g_rob_bank.Stop();
        ClearCache();
        ClearExtractionCache();
        return ahud_status;
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    const AnomalyStatusV1 ahud_status = UnsubscribeAhud();
    const bool saved = SaveSettings();
    SetDeveloperMode(false);
    g_world_gate.Reset();
    g_in_pink_paw_world = false;
    g_world_gate_refresh_requested.store(false, std::memory_order_release);
    RequestMapClear();
    try {
        static_cast<void>(FlushMapClear());
    } catch (...) {
    }
    g_rob_bank.Stop();
    ClearCache();
    ClearExtractionCache();
    g_display_settings.store({}, std::memory_order_release);
    if (ahud_status.code != ANOMALY_STATUS_V1_OK) return ahud_status;
    return saved ? anomaly::sdk::Ok()
                 : AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}};
}

void ANOMALY_CALL Unload(void*) {
    static_cast<void>(UnsubscribeAhud());
    SetDeveloperMode(false);
    g_world_gate.Reset();
    g_in_pink_paw_world = false;
    g_world_gate_refresh_requested.store(false, std::memory_order_release);
    RequestMapClear();
    try {
        static_cast<void>(FlushMapClear());
    } catch (...) {
    }
    g_rob_bank.Stop();
    {
        std::scoped_lock lock(g_teleport.mutex);
        g_teleport.host = nullptr;
        g_teleport.has_result = false;
        g_teleport.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_teleport.result_message[0] = '\0';
    }
    {
        std::scoped_lock lock(g_pickup.mutex);
        g_pickup.host = nullptr;
        g_pickup.has_result = false;
        g_pickup.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_pickup.result_message[0] = '\0';
    }
    ClearCache();
    ClearExtractionCache();
    g_display_settings.store({}, std::memory_order_release);
    g_context = {};
}

void ProcessPendingTeleport() {
    PendingTeleport pending{};
    const AnomalyHostApiV1* host{};
    {
        std::scoped_lock lock(g_teleport.mutex);
        if (!g_teleport.pending.queued) return;
        pending = g_teleport.pending;
        g_teleport.pending = {};
        host = g_teleport.host;
    }
    if (!g_teleport.developer_mode.load(std::memory_order_acquire) ||
        !DeveloperModeEnabled(host)) {
        SetTeleportDeveloperMode(false);
        return;
    }
    if (host == nullptr || !IsFinitePosition(pending.position)) {
        RecordTeleportResult(StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE));
        return;
    }

    const auto teleport = QueryService<AnomalyNtePlayerTeleportServiceV1>(
        host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    if (!teleport ||
        !HasField<AnomalyNtePlayerTeleportServiceV1,
            decltype(AnomalyNtePlayerTeleportServiceV1::teleport)>(
            teleport.service,
            offsetof(AnomalyNtePlayerTeleportServiceV1, teleport)) ||
        teleport.service->teleport == nullptr) {
        RecordTeleportResult(teleport ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : teleport.status);
        return;
    }

    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    request.flags = 0;
    request.world = pending.world;
    request.player = pending.player;
    for (std::size_t axis = 0; axis != 3; ++axis) request.position[axis] = pending.position[axis];
    RecordTeleportResult(teleport.service->teleport(teleport.service->user, &request));
}

void ProcessPendingPickup() {
    PendingPickup pending{};
    const AnomalyHostApiV1* host{};
    {
        std::scoped_lock lock(g_pickup.mutex);
        if (!g_pickup.pending.queued) return;
        pending = g_pickup.pending;
        g_pickup.pending = {};
        host = g_pickup.host;
    }
    if (!g_pickup.developer_mode.load(std::memory_order_acquire) ||
        !DeveloperModeEnabled(host)) {
        SetPickupDeveloperMode(false);
        return;
    }
    if (host == nullptr || !pending.entity.Valid()) {
        RecordPickupResult(StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE));
        return;
    }

    const auto session = QueryService<AnomalyNteSessionServiceV1>(
        host, ANOMALY_NTE_SESSION_SERVICE_V1_ID,
        ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!session || !player || session.service->snapshot == nullptr ||
        player.service->snapshot == nullptr) {
        RecordPickupResult(!session ? session.status : !player ? player.status
            : StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE));
        return;
    }
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    const AnomalyStatusV1 session_status = session.service->snapshot(
        session.service->user, &session_snapshot);
    const AnomalyStatusV1 player_status = player.service->snapshot(
        player.service->user, &player_snapshot);
    if (session_status.code != ANOMALY_STATUS_V1_OK ||
        player_status.code != ANOMALY_STATUS_V1_OK ||
        !IsCurrentWorld(session_snapshot) || !IsCurrentPlayer(player_snapshot)) {
        RecordPickupResult(session_status.code != ANOMALY_STATUS_V1_OK
            ? session_status : player_status.code != ANOMALY_STATUS_V1_OK
                ? player_status : StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE));
        return;
    }

    if (!g_rob_bank.Refresh()) {
        RecordPickupResult(StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE));
        return;
    }
    const AnomalyStatusV1 status = g_rob_bank.Pickup(pending.entity);
    RecordPickupResult(status);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        RemoveCachedLoot(pending.entity);
    }
}

void ANOMALY_CALL Update(void*, double) {
    if (g_world_gate_refresh_requested.exchange(false, std::memory_order_acq_rel)) {
        g_world_gate.Invalidate();
    }
    const bool active = g_world_gate.Refresh(g_context.host) ==
        pink_paw_heist_esp::PinkPawWorldState::active;
    if (active) {
        if (!g_in_pink_paw_world) {
            g_loot_refresh_requested.store(true, std::memory_order_release);
            g_extractions.refresh_requested.store(true, std::memory_order_release);
        }
        RefreshExtractionCacheIfDue();
        RefreshCacheIfDue();
        RefreshKnownLootIfDue();
    } else if (g_in_pink_paw_world) {
        ClearCache();
        ClearExtractionCache();
        RequestMapClear();
    }
    if (active) {
        SynchronizeMapIfPossible();
    } else {
        try {
            static_cast<void>(FlushMapClear());
        } catch (...) {
        }
    }
    g_in_pink_paw_world = active;
    ProcessPendingTeleport();
    ProcessPendingPickup();
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    const auto cache = g_loot_cache.load(std::memory_order_acquire);
    const LootCache empty;
    const LootCache& snapshot = cache ? *cache : empty;
    DrawMenu(ui, snapshot);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.pink-paw-heist-esp"),
        anomaly::sdk::StringView("Pink Paw Heist ESP"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.10.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
