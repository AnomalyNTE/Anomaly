#include "anomaly/sdk/cpp.hpp"
#include "plugins/common/localization.hpp"

#include "loot_class_cache.hpp"
#include "loot_catalog.generated.h"
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
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
namespace catalog = pink_paw_heist_esp::catalog;

constexpr std::size_t kEntityPageCapacity = 256;
constexpr std::uint32_t kMaximumEntityCount = 32768;
constexpr std::size_t kMaximumNameBytes = 1024;
constexpr std::size_t kLootRowsPerPage = 6;
constexpr std::uint32_t kTableSizingFixedFit = 1U << 13U;
// BankBoxes are stationary; full discovery stops after convergence. Cached identities are
// checked in small batches so manual pickups do not restart the entity-frame scan.
constexpr auto kLootRefreshInterval = std::chrono::seconds(2);
constexpr auto kKnownLootValidationInterval = std::chrono::milliseconds(50);
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
constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 4;
constexpr double kDefaultTeleportZOffsetCentimeters = 150.0;
constexpr std::string_view kSettingsSchema = R"json(
{"type":"object","additionalProperties":false,"required":["menuOpen","enabled","showActiveExtractionsOnly","showPickableOnly","minimumValue","teleportZOffset"],"properties":{"menuOpen":{"type":"boolean"},"enabled":{"type":"boolean"},"showActiveExtractionsOnly":{"type":"boolean"},"showPickableOnly":{"type":"boolean"},"minimumValue":{"type":"integer","minimum":0,"maximum":4294967295},"teleportZOffset":{"type":"number"}}}
)json";

struct LootEntity final {
    AnomalyNteEntitySnapshotV1 snapshot{sizeof(snapshot)};
    std::string class_name;
    std::string label;
    const catalog::ItemDefinition* item{};
    pink_paw_heist_esp::RobBankInspection rob_bank;
    std::uint8_t missing_observations{};
};

struct LootCache final {
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    std::vector<LootEntity> loot;
    bool available{};
};

enum class ExtractionActivation {
    unknown,
    inactive,
    active,
};

struct ExtractionPoint final {
    AnomalyNteEntitySnapshotV1 snapshot{sizeof(snapshot)};
    std::string id;
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
    bool state_complete{};
    Clock::time_point next_world_check{};
    Clock::time_point next_state_refresh{};
    std::atomic_bool refresh_requested{true};
};

struct Settings final {
    bool menu_open{true};
    bool enabled{true};
    bool show_active_extractions_only{true};
    bool show_pickable_only{true};
    std::uint32_t minimum_value{};
    double teleport_z_offset{kDefaultTeleportZOffsetCentimeters};
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
    const AnomalyTextureServiceV1* texture{};
    AnomalyGenerationHandleV1 settings_schema{};
    std::array<AnomalyGenerationHandleV1, catalog::kItemDefinitions.size()> item_icons{};
    pink_paw_heist_esp::LootClassCache loot_classes;
    pink_paw_heist_esp::LootRefreshPolicy loot_refresh{kLootRefreshInterval};

    int menu_open{1};
    int enabled{1};
    int show_active_extractions_only{1};
    int show_pickable_only{1};
    std::uint32_t minimum_value{};
    double teleport_z_offset{kDefaultTeleportZOffsetCentimeters};
    std::size_t current_page{};

    bool settings_dirty{};
    Clock::time_point last_valid_refresh{};
    Clock::time_point next_known_loot_validation{};
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
std::atomic_bool g_loot_refresh_requested{true};

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

    bool Read(Settings& settings) noexcept {
        if (!Consume('{')) return false;

        bool menu_open_seen{};
        bool enabled_seen{};
        bool show_active_extractions_only_seen{};
        bool show_pickable_only_seen{};
        bool minimum_value_seen{};
        bool teleport_z_offset_seen{};
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
            } else {
                return false;
            }

            if (Consume('}')) break;
            if (!Consume(',')) return false;
        }

        SkipWhitespace();
        return menu_open_seen && enabled_seen && show_active_extractions_only_seen &&
            show_pickable_only_seen && minimum_value_seen && teleport_z_offset_seen &&
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

Settings CurrentSettings() noexcept {
    return {
        g_context.menu_open != 0,
        g_context.enabled != 0,
        g_context.show_active_extractions_only != 0,
        g_context.show_pickable_only != 0,
        g_context.minimum_value,
        std::isfinite(g_context.teleport_z_offset)
            ? g_context.teleport_z_offset : kDefaultTeleportZOffsetCentimeters};
}

void ApplySettings(const Settings& settings) noexcept {
    g_context.menu_open = settings.menu_open ? 1 : 0;
    g_context.enabled = settings.enabled ? 1 : 0;
    g_context.show_active_extractions_only = settings.show_active_extractions_only ? 1 : 0;
    g_context.show_pickable_only = settings.show_pickable_only ? 1 : 0;
    g_context.minimum_value = settings.minimum_value;
    g_context.teleport_z_offset = std::isfinite(settings.teleport_z_offset)
        ? settings.teleport_z_offset : kDefaultTeleportZOffsetCentimeters;
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
        ",\"showActiveExtractionsOnly\":" +
        (settings.show_active_extractions_only ? "true" : "false") +
        ",\"showPickableOnly\":" +
        (settings.show_pickable_only ? "true" : "false") +
        ",\"minimumValue\":" + std::to_string(settings.minimum_value) +
        ",\"teleportZOffset\":" + FormatSettingsDouble(settings.teleport_z_offset) + "}";
}

bool LoadSettings() {
    if (!ConfigMethodsAvailable(g_context.config)) return false;

    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code != ANOMALY_STATUS_V1_OK || schema_version != kSettingsSchemaVersion ||
        size == 0 || size > kMaximumSettingsDocumentBytes) {
        return false;
    }

    std::vector<std::uint8_t> document(size);
    const AnomalyStatusV1 read_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {document.data(), document.size()}, &size);
    if (read_status.code != ANOMALY_STATUS_V1_OK || schema_version != kSettingsSchemaVersion ||
        size == 0 || size > document.size()) {
        return false;
    }

    Settings settings;
    const std::string_view serialized(
        reinterpret_cast<const char*>(document.data()), size);
    if (!SettingsDocumentReader(serialized).Read(settings)) return false;
    ApplySettings(settings);
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
    entry.item = metadata->item;
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

void ResetExtractionData() noexcept {
    g_extractions.frame = AnomalyNteEntityFrameV1{sizeof(g_extractions.frame)};
    g_extractions.points.clear();
    g_extractions.class_id = 0;
    g_extractions.available = false;
    g_extractions.complete = false;
    g_extractions.state_complete = false;
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
            if (!g_extractions.complete || g_extractions.state_complete ||
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
            return;
        }
    }

    std::vector<ExtractionPoint> points;
    {
        std::scoped_lock lock(g_extractions.mutex);
        if (!g_extractions.complete ||
            (!forced && (g_extractions.state_complete || now < g_extractions.next_state_refresh))) {
            return;
        }
        points = g_extractions.points;
    }
    const anomaly::sdk::Host host(g_context.host);
    const auto actors = host.Query<AnomalyNteActorsServiceV1>(
        ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION);
    const bool state_complete = RefreshExtractionActivation(actors.get(), points);
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
            g_extractions.state_complete = state_complete;
            g_extractions.next_state_refresh = state_complete
                ? Clock::time_point{}
                : now + kExtractionStateRetryInterval;
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
}

void ClearCache() noexcept {
    g_loot_cache.store({}, std::memory_order_release);
    g_loot_refresh_requested.store(true, std::memory_order_release);
    g_context.loot_classes.Clear();
    g_context.loot_refresh.Reset();
    g_context.current_page = 0;
    g_context.last_valid_refresh = {};
    g_context.next_known_loot_validation = {};
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
            left.rob_bank.pickability != right.rob_bank.pickability) {
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
    if (g_context.host != nullptr && CollectLoot(frame, loot)) {
        std::sort(loot.begin(), loot.end(), [](const LootEntity& left, const LootEntity& right) {
            if (left.snapshot.entity_id != right.snapshot.entity_id) {
                return left.snapshot.entity_id < right.snapshot.entity_id;
            }
            return left.class_name < right.class_name;
        });
        const bool unchanged =
            !forced && current && current->available && SameLootState(*current, loot);
        for (LootEntity& entry : loot) entry.label = BuildLootLabel(entry);
        auto cache = std::make_shared<LootCache>();
        cache->frame = frame;
        cache->loot = std::move(loot);
        cache->available = true;
        g_loot_cache.store(std::move(cache), std::memory_order_release);
        g_context.last_valid_refresh = now;
        g_context.next_known_loot_validation = now + kKnownLootValidationInterval;
        g_context.known_loot_validation_cursor = 0;
        g_context.loot_refresh.Complete(now, unchanged);
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
    if (g_rob_bank.Available() && g_rob_bank.DiscoveryPending() &&
        now >= g_context.next_rob_bank_preparation) {
        const bool refreshed = g_rob_bank.Refresh();
        g_context.next_rob_bank_preparation = now +
            (refreshed && g_rob_bank.DiscoveryPending()
                 ? kKnownLootValidationInterval
                 : kRobBankPreparationRetryInterval);
    }

    if (now < g_context.next_known_loot_validation) return;
    g_context.next_known_loot_validation = now + kKnownLootValidationInterval;

    const auto current = g_loot_cache.load(std::memory_order_acquire);
    if (!current || !current->available || current->loot.empty() ||
        !g_rob_bank.CanInspect()) {
        return;
    }

    const std::size_t start =
        g_context.known_loot_validation_cursor % current->loot.size();
    const std::size_t count =
        (std::min)(kKnownLootValidationBatch, current->loot.size());
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

bool PassesItemFilters(const LootEntity& entry) noexcept {
    return entry.item != nullptr && entry.item->value.has_value() &&
        *entry.item->value >= g_context.minimum_value;
}

bool IsPickable(const LootEntity& entry) noexcept {
    return entry.rob_bank.pickability ==
        pink_paw_heist_esp::RobBankPickability::candidate;
}

bool IsVisibleLoot(const LootEntity& entry) noexcept {
    return PassesItemFilters(entry) &&
        pink_paw_heist_esp::PassesPickabilityFilter(
            entry.rob_bank.pickability, g_context.show_pickable_only != 0);
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

std::string BuildLootLabel(const LootEntity& entry) {
    if (entry.item == nullptr || !entry.item->value.has_value()) return {};
    const std::string value = FormatValue(*entry.item->value);
    const std::string coordinates = BuildWorldCoordinates(entry);
    const std::array arguments{
        std::string_view(entry.item->name_utf8), std::string_view(value),
        std::string_view(coordinates)};
    return g_context.localizer.Format(
        "loot.label", "{0}\nValue {1} Fons\n{2}", arguments);
}

std::uint32_t LootColor(const LootEntity& entry) noexcept {
    if (entry.item == nullptr) return ANOMALY_RGBA_V1(170, 170, 170, 255);
    if (!entry.item->value.has_value()) return ANOMALY_RGBA_V1(104, 196, 255, 255);
    if (*entry.item->value >= 100000U) return ANOMALY_RGBA_V1(255, 196, 64, 255);
    if (*entry.item->value >= 10000U) return ANOMALY_RGBA_V1(255, 122, 92, 255);
    return ANOMALY_RGBA_V1(95, 226, 148, 255);
}

std::vector<const LootEntity*> CollectVisibleLoot(const LootCache& cache) {
    std::vector<const LootEntity*> visible;
    visible.reserve(cache.loot.size());
    for (const LootEntity& entry : cache.loot) {
        if (IsVisibleLoot(entry)) visible.push_back(&entry);
    }
    std::sort(visible.begin(), visible.end(), [](const LootEntity* left, const LootEntity* right) {
        const std::uint32_t left_value = *left->item->value;
        const std::uint32_t right_value = *right->item->value;
        if (left_value != right_value) return left_value > right_value;
        if (left->snapshot.entity_id != right->snapshot.entity_id) {
            return left->snapshot.entity_id < right->snapshot.entity_id;
        }
        return left->class_name < right->class_name;
    });
    return visible;
}

bool TextureMethodsAvailable(const AnomalyTextureServiceV1* service) noexcept {
    return service != nullptr &&
        HasField<AnomalyTextureServiceV1, decltype(AnomalyTextureServiceV1::request)>(
            service, offsetof(AnomalyTextureServiceV1, request)) &&
        HasField<AnomalyTextureServiceV1, decltype(AnomalyTextureServiceV1::release)>(
            service, offsetof(AnomalyTextureServiceV1, release)) &&
        HasField<AnomalyTextureServiceV1, decltype(AnomalyTextureServiceV1::state)>(
            service, offsetof(AnomalyTextureServiceV1, state)) &&
        HasField<AnomalyTextureServiceV1, decltype(AnomalyTextureServiceV1::draw)>(
            service, offsetof(AnomalyTextureServiceV1, draw)) &&
        service->request != nullptr && service->release != nullptr && service->state != nullptr &&
        service->draw != nullptr;
}

void ReleaseUiResources() noexcept {
    if (TextureMethodsAvailable(g_context.texture)) {
        for (auto& handle : g_context.item_icons) {
            if (handle.id != 0) {
                static_cast<void>(g_context.texture->release(g_context.texture->user, handle));
                handle = {};
            }
        }
    }
}

void RequestUiResources() {
    if (!TextureMethodsAvailable(g_context.texture)) return;
    for (std::size_t index = 0; index < catalog::kItemDefinitions.size(); ++index) {
        std::string relative_path = "assets/icons/";
        relative_path += catalog::kItemDefinitions[index].icon_filename;
        AnomalyTextureRequestV1 request{};
        request.struct_size = sizeof(request);
        request.relative_path = anomaly::sdk::StringView(relative_path);
        request.format = ANOMALY_TEXTURE_FORMAT_V1_AUTO;
        static_cast<void>(g_context.texture->request(
            g_context.texture->user, &request, &g_context.item_icons[index]));
    }
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

void DrawItemIcon(const catalog::ItemDefinition* item) {
    if (item == nullptr || !TextureMethodsAvailable(g_context.texture)) return;
    const std::ptrdiff_t difference = item - catalog::kItemDefinitions.data();
    if (difference < 0 || static_cast<std::size_t>(difference) >= g_context.item_icons.size()) return;
    const AnomalyGenerationHandleV1 handle = g_context.item_icons[static_cast<std::size_t>(difference)];
    if (handle.id == 0) return;
    AnomalyTextureStateV1 state{sizeof(state)};
    if (g_context.texture->state(g_context.texture->user, handle, &state).code !=
            ANOMALY_STATUS_V1_OK ||
        (state.flags & ANOMALY_TEXTURE_STATE_V1_READY) == 0) {
        return;
    }
    static_cast<void>(g_context.texture->draw(
        g_context.texture->user, handle, 28.0F, 28.0F, ANOMALY_RGBA_V1(255, 255, 255, 255)));
}

void DrawLootItemCell(
    const AnomalyUiServiceV1* ui, const AnomalyUiServiceV1* table_ui,
    const LootEntity& entry) {
    const std::string table_id = "loot-item-" + std::to_string(entry.snapshot.entity_id);
    if (table_ui->begin_table(
            table_ui->user, anomaly::sdk::StringView(table_id), 2,
            kTableSizingFixedFit, 0.0F, 0.0F) != 0) {
        table_ui->table_next_row(table_ui->user);
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        DrawItemIcon(entry.item);
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, entry.item->name_utf8);
        table_ui->end_table(table_ui->user);
        return;
    }

    DrawItemIcon(entry.item);
    Text(ui, entry.item->name_utf8);
}

void DrawLootRows(
    const AnomalyUiServiceV1* ui, const std::vector<const LootEntity*>& visible_loot,
    const std::size_t first, const std::size_t last, const bool developer_mode) {
    const auto* table_ui = TableUi(ui);
    if (table_ui != nullptr && table_ui->begin_table(
            table_ui->user, anomaly::sdk::StringView("loot"), developer_mode ? 5 : 3,
            0, 0.0F, 250.0F) != 0) {
        table_ui->table_next_row(table_ui->user);
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.loot", "Loot"));
        static_cast<void>(table_ui->table_next_column(table_ui->user));
        Text(ui, g_context.localizer.Text("column.value", "Value"));
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
            DrawLootItemCell(ui, table_ui, entry);
            static_cast<void>(table_ui->table_next_column(table_ui->user));
            Text(ui, FormatValue(*entry.item->value));
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
        DrawItemIcon(entry.item);
        const std::string value = FormatValue(*entry.item->value);
        const std::string coordinates = BuildWorldCoordinates(entry);
        const std::array arguments{
            std::string_view(entry.item->name_utf8), std::string_view(value),
            std::string_view(coordinates)};
        Text(ui, g_context.localizer.Format(
            "loot.row", "{0}  {1} Fons  {2}", arguments));
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
    const auto draw_action = [ui](const PaginationAction& action) {
        if (ButtonEnabled(ui, action.label, action.enabled)) {
            g_context.current_page = action.destination;
        }
    };

    const auto* table_ui = TableUi(ui);
    if (table_ui != nullptr && table_ui->begin_table(
            table_ui->user, anomaly::sdk::StringView("loot-pagination"),
            static_cast<std::int32_t>(actions.size()), 0, 0.0F, 0.0F) != 0) {
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

std::uint32_t ExtractionColor(const ExtractionActivation activation) noexcept {
    switch (activation) {
    case ExtractionActivation::active: return ANOMALY_RGBA_V1(72, 220, 132, 255);
    case ExtractionActivation::inactive: return ANOMALY_RGBA_V1(235, 72, 72, 255);
    default: return ANOMALY_RGBA_V1(255, 190, 72, 255);
    }
}

bool ShouldShowExtraction(const ExtractionPoint& point) noexcept {
    return g_context.show_active_extractions_only == 0 ||
        point.activation == ExtractionActivation::active;
}

void FilterExtractionPoints(std::vector<ExtractionPoint>& points) {
    std::erase_if(points, [](const ExtractionPoint& point) {
        return !ShouldShowExtraction(point);
    });
}

void DrawExtractionRows(
    const AnomalyUiServiceV1* ui,
    const std::vector<ExtractionPoint>& points,
    const bool developer_mode) {
    const auto* table_ui = TableUi(ui);
    if (table_ui != nullptr && table_ui->begin_table(
            table_ui->user, anomaly::sdk::StringView("extractions"), developer_mode ? 4 : 3,
            0, 0.0F, 220.0F) != 0) {
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
        if (changed_enabled || changed_active_only || changed_pickable_only || changed_minimum ||
            changed_teleport_offset) {
            g_context.settings_dirty = true;
            g_context.current_page = 0;
        }
        if (changed_enabled) {
            if (g_context.enabled != 0) {
                g_world_gate_refresh_requested.store(true, std::memory_order_release);
                g_loot_refresh_requested.store(true, std::memory_order_release);
                g_extractions.refresh_requested.store(true, std::memory_order_release);
            } else {
                ClearCache();
                ClearExtractionCache();
            }
        }
        if (!supports_numeric_input) {
            const std::string value = std::to_string(g_context.minimum_value);
            const std::array arguments{std::string_view(value)};
            Text(ui, g_context.localizer.Format(
                "option.minimum_value.summary", "Minimum value: {0}", arguments));
        }
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

        std::vector<ExtractionPoint> extraction_points;
        bool extraction_available{};
        {
            std::scoped_lock lock(g_extractions.mutex);
            extraction_available = g_extractions.available;
            extraction_points = g_extractions.points;
        }
        FilterExtractionPoints(extraction_points);
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
            CollectVisibleLoot(loot_cache);
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

bool ReadEspCamera(AnomalyEspCameraV1& camera) {
    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        g_context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!player || player.service->esp_snapshot == nullptr) return false;

    AnomalyNtePlayerEspSnapshotV1 snapshot{sizeof(snapshot)};
    if (player.service->esp_snapshot(player.service->user, &snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        !IsCompleteSnapshot(snapshot.flags)) {
        return false;
    }
    std::ranges::copy(snapshot.camera_position, camera.position);
    std::ranges::copy(snapshot.camera_rotation, camera.rotation);
    camera.horizontal_fov_degrees = snapshot.horizontal_fov_degrees;
    return true;
}

void DrawEsp(const AnomalyUiServiceV1* ui, const LootCache& loot_cache) {
    if (ui == nullptr || g_context.enabled == 0) return;
    std::vector<ExtractionPoint> extraction_points;
    bool extraction_available{};
    {
        std::scoped_lock lock(g_extractions.mutex);
        extraction_available = g_extractions.available;
        extraction_points = g_extractions.points;
    }
    FilterExtractionPoints(extraction_points);
    if (!loot_cache.available && !extraction_available) return;
    const bool supports_boxes =
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::draw_entity_bbox)>(
            ui, offsetof(AnomalyUiServiceV1, draw_entity_bbox)) &&
        ui->draw_entity_bbox != nullptr;
    const bool supports_labels =
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::draw_entity_label)>(
            ui, offsetof(AnomalyUiServiceV1, draw_entity_label)) &&
        ui->draw_entity_label != nullptr;
    if (!supports_boxes && !supports_labels) return;

    AnomalyEspCameraV1 camera{sizeof(camera)};
    if (!ReadEspCamera(camera)) return;

    for (const LootEntity& entry : loot_cache.loot) {
        if (!IsVisibleLoot(entry)) continue;
        const std::uint32_t color = LootColor(entry);
        AnomalyEspEntityBoundsV1 bounds{sizeof(bounds)};
        for (std::size_t index = 0; index < 3; ++index) {
            bounds.center[index] = entry.snapshot.bounds_center[index];
            bounds.extent[index] = entry.snapshot.bounds_extent[index];
        }
        const AnomalyEspBoxStyleV1 style{
            sizeof(style), ANOMALY_ESP_BOX_V1_OUTLINE, color,
            ANOMALY_RGBA_V1(0, 0, 0, 220), kFixedBoxThickness, 1.0F};
        bool visible = !supports_boxes;
        if (supports_boxes) {
            visible = ui->draw_entity_bbox(ui->user, &camera, &bounds, &style) != 0;
        }
        if (supports_labels && visible) {
            static_cast<void>(ui->draw_entity_label(
                ui->user, &camera, &bounds, anomaly::sdk::StringView(entry.label), color));
        }
    }

    for (const ExtractionPoint& point : extraction_points) {
        const std::uint32_t color = ExtractionColor(point.activation);
        AnomalyEspEntityBoundsV1 bounds{sizeof(bounds)};
        std::ranges::copy(point.snapshot.bounds_center, bounds.center);
        std::ranges::copy(point.snapshot.bounds_extent, bounds.extent);
        const AnomalyEspBoxStyleV1 style{
            sizeof(style), ANOMALY_ESP_BOX_V1_OUTLINE, color,
            ANOMALY_RGBA_V1(0, 0, 0, 220), kFixedBoxThickness, 1.0F};
        bool visible = !supports_boxes;
        if (supports_boxes) {
            visible = ui->draw_entity_bbox(ui->user, &camera, &bounds, &style) != 0;
        }
        if (supports_labels && visible) {
            const std::string activation = ExtractionActivationText(point.activation);
            const std::string coordinates = BuildWorldCoordinates(point.snapshot);
            const std::array arguments{
                std::string_view(point.id), std::string_view(activation),
                std::string_view(coordinates)};
            const std::string label = g_context.localizer.Format(
                "extraction.label", "Extraction {0}\n{1}\n{2}", arguments);
            static_cast<void>(ui->draw_entity_label(
                ui->user, &camera, &bounds, anomaly::sdk::StringView(label), color));
        }
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const anomaly::sdk::Host host_view(host);
    const auto ui = host_view.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    const auto config = host_view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    if (!ui || !HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::button_enabled)>(
            ui.get(), offsetof(AnomalyUiServiceV1, button_enabled)) ||
        ui->button_enabled == nullptr || !ConfigMethodsAvailable(config.get())) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    g_context = {};
    g_context.host = host;
    g_context.localizer = anomaly::plugins::Localizer(host);
    g_context.config = config.get();
    g_context.texture = host_view.Query<AnomalyTextureServiceV1>(
        ANOMALY_TEXTURE_SERVICE_V1_ID, ANOMALY_TEXTURE_SERVICE_V1_VERSION).get();
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
    RequestUiResources();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    const bool saved = SaveSettings();
    SetDeveloperMode(false);
    g_world_gate.Reset();
    g_in_pink_paw_world = false;
    g_world_gate_refresh_requested.store(false, std::memory_order_release);
    g_rob_bank.Stop();
    ReleaseUiResources();
    ClearCache();
    ClearExtractionCache();
    return saved ? anomaly::sdk::Ok()
                 : AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}};
}

void ANOMALY_CALL Unload(void*) {
    SetDeveloperMode(false);
    g_world_gate.Reset();
    g_in_pink_paw_world = false;
    g_world_gate_refresh_requested.store(false, std::memory_order_release);
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
    ReleaseUiResources();
    ClearExtractionCache();
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
    if (g_context.enabled != 0) {
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
        }
        g_in_pink_paw_world = active;
    }
    ProcessPendingTeleport();
    ProcessPendingPickup();
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    const auto cache = g_loot_cache.load(std::memory_order_acquire);
    const LootCache empty;
    const LootCache& snapshot = cache ? *cache : empty;
    DrawMenu(ui, snapshot);
    DrawEsp(ui, snapshot);
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
        anomaly::sdk::StringView("1.7.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
