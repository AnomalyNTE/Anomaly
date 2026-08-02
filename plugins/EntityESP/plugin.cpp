#include "anomaly/sdk/cpp.hpp"
#include "plugins/common/localization.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kPageSize = 20;
constexpr std::size_t kEntityPageCapacity = ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY;
constexpr std::size_t kMaximumPersistedFilters = 16384;
constexpr std::size_t kMaximumSettingsBytes = 1024u * 1024u;
constexpr std::uint32_t kSettingsSchemaVersion = 1u;
constexpr std::string_view kSettingsSchemaId = "anomaly.builtin.entity-esp.settings";
constexpr std::string_view kSettingsSchema = R"json(
{
  "type": "object",
  "additionalProperties": false,
  "required": [
    "menuOpen", "enabled", "draw2d", "draw3d", "outline", "showLocalPlayer",
    "labelCategory", "labelEntityId", "showStatic", "showStationary", "showMovable",
    "showUnknown", "showEntityControls", "thickness", "updateRateHz", "color",
    "classEnabled", "entityEnabled"
  ],
  "properties": {
    "menuOpen": {"type": "boolean"},
    "enabled": {"type": "boolean"},
    "draw2d": {"type": "boolean"},
    "draw3d": {"type": "boolean"},
    "outline": {"type": "boolean"},
    "showLocalPlayer": {"type": "boolean"},
    "labelCategory": {"type": "boolean"},
    "labelEntityId": {"type": "boolean"},
    "showStatic": {"type": "boolean"},
    "showStationary": {"type": "boolean"},
    "showMovable": {"type": "boolean"},
    "showUnknown": {"type": "boolean"},
    "showEntityControls": {"type": "boolean"},
    "thickness": {"type": "number", "minimum": 0.5, "maximum": 6.0},
    "updateRateHz": {"type": "number", "minimum": 2.0, "maximum": 60.0},
    "color": {
      "type": "array", "minItems": 4, "maxItems": 4,
      "items": {"type": "number", "minimum": 0.0, "maximum": 1.0}
    },
    "classEnabled": {
      "type": "object", "maxProperties": 16384, "additionalProperties": false,
      "patternProperties": {
        "^(0|[1-9][0-9]{0,19})$": {"type": "boolean"}
      }
    },
    "entityEnabled": {
      "type": "object", "maxProperties": 16384, "additionalProperties": false,
      "patternProperties": {
        "^(0|[1-9][0-9]{0,19})$": {"type": "boolean"}
      }
    }
  }
}
)json";

struct EntityView {
    AnomalyNteEntitySnapshotV1 snapshot{sizeof(snapshot)};
    std::string category;
    std::string entity_id_label;
    std::string category_entity_id_label;
};

struct ClassSummary {
    std::uint64_t id{};
    std::string name;
    std::size_t count{};
};

struct DisplaySettings final {
    int enabled{};
    int draw_2d{};
    int draw_3d{};
    int outline{};
    int show_local_player{};
    int label_category{};
    int label_entity_id{};
    int show_static{};
    int show_stationary{};
    int show_movable{};
    int show_unknown{};
    float thickness{1.5F};
    float update_rate_hz{30.0F};
    std::array<float, 4> color{};
    std::unordered_map<std::uint64_t, int> class_enabled;
    std::unordered_map<std::uint64_t, int> entity_enabled;
};

struct EntityCache final {
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    std::vector<EntityView> entities;
    bool available{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyCoreServiceV1* core{};
    const AnomalyConfigServiceV1* config{};
    const AnomalyUe5AhudServiceV1* ahud{};
    AnomalyGenerationHandleV1 settings_schema{};
    int menu_open{1};
    int enabled{1};
    int draw_2d{1};
    int draw_3d{};
    int outline{1};
    int show_local_player{};
    int label_category{1};
    int label_entity_id{};
    int show_static{};
    int show_stationary{};
    int show_movable{1};
    int show_unknown{};
    int show_entity_controls{};
    float thickness{1.5F};
    float update_rate_hz{30.0F};
    float color[4]{80.0F / 255.0F, 1.0F, 120.0F / 255.0F, 1.0F};
    std::unordered_map<std::uint64_t, int> class_enabled;
    std::unordered_map<std::uint64_t, int> entity_enabled;
    std::uint64_t selected_class{};
    std::size_t class_page{};
    std::size_t entity_page{};
    std::uint64_t class_summary_generation{};
    std::uint32_t class_summary_visibility{};
    std::vector<ClassSummary> class_summaries;
    bool settings_dirty{};
} g_context;

std::unordered_map<std::uint64_t, std::string> g_class_names;
std::atomic<std::shared_ptr<const DisplaySettings>> g_display_settings;
std::atomic<std::shared_ptr<const EntityCache>> g_entity_cache;
std::atomic_bool g_refresh_requested{true};
Clock::time_point g_next_refresh{};
std::uint64_t g_reconciled_cache_generation{};
AnomalyGenerationHandleV1 g_ahud_subscription{};

template <typename Struct, typename Field>
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

void LogSettingsFailure(const std::string_view operation) {
    if (!HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::log)>(
            g_context.core, offsetof(AnomalyCoreServiceV1, log)) ||
        g_context.core->log == nullptr) {
        return;
    }
    const std::string message = "player bbox settings " + std::string(operation) + " failed";
    g_context.core->log(
        g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
        anomaly::sdk::StringView(message));
}

bool ValidBoolean(int value) noexcept { return value == 0 || value == 1; }

bool ValidSettings(const Context& settings) noexcept {
    return ValidBoolean(settings.menu_open) && ValidBoolean(settings.enabled) &&
        ValidBoolean(settings.draw_2d) && ValidBoolean(settings.draw_3d) &&
        ValidBoolean(settings.outline) && ValidBoolean(settings.show_local_player) &&
        ValidBoolean(settings.label_category) && ValidBoolean(settings.label_entity_id) &&
        ValidBoolean(settings.show_static) && ValidBoolean(settings.show_stationary) &&
        ValidBoolean(settings.show_movable) && ValidBoolean(settings.show_unknown) &&
        ValidBoolean(settings.show_entity_controls) && std::isfinite(settings.thickness) &&
        settings.thickness >= 0.5F && settings.thickness <= 6.0F &&
        std::isfinite(settings.update_rate_hz) && settings.update_rate_hz >= 2.0F &&
        settings.update_rate_hz <= 60.0F &&
        std::ranges::all_of(settings.color, [](const float value) {
            return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
        }) &&
        std::ranges::all_of(settings.class_enabled, [](const auto& entry) {
            return ValidBoolean(entry.second);
        }) &&
        std::ranges::all_of(settings.entity_enabled, [](const auto& entry) {
            return ValidBoolean(entry.second);
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

    bool ParseBoolean(int& value) noexcept {
        SkipWhitespace();
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            value = 1;
            return true;
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            value = 0;
            return true;
        }
        return false;
    }

    bool ParseNumber(float& value) noexcept {
        SkipWhitespace();
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ == input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
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

    bool ParseString(std::string& value, const std::size_t maximum_size) {
        if (!Consume('"')) return false;
        value.clear();
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20U || character > 0x7FU || character == '\\' ||
                value.size() >= maximum_size) {
                return false;
            }
            value.push_back(static_cast<char>(character));
        }
        return false;
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

bool ParseFilterId(const std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    return true;
}

bool ParseFilterMap(
    SettingsJsonReader& reader, std::unordered_map<std::uint64_t, int>& filters) {
    if (!reader.Consume('{')) return false;
    if (reader.Consume('}')) return true;
    for (;;) {
        std::string id_text;
        std::uint64_t id{};
        int enabled{};
        if (!reader.ParseString(id_text, 20) || !ParseFilterId(id_text, id) ||
            !reader.Consume(':') || !reader.ParseBoolean(enabled) ||
            filters.size() >= kMaximumPersistedFilters ||
            !filters.emplace(id, enabled).second) {
            return false;
        }
        if (reader.Consume('}')) return true;
        if (!reader.Consume(',')) return false;
    }
}

bool ParseColor(SettingsJsonReader& reader, float (&color)[4]) noexcept {
    if (!reader.Consume('[')) return false;
    for (std::size_t index = 0; index < std::size(color); ++index) {
        if (!reader.ParseNumber(color[index])) return false;
        if (index + 1 == std::size(color)) {
            if (!reader.Consume(']')) return false;
        } else if (!reader.Consume(',')) {
            return false;
        }
    }
    return true;
}

void ApplySettings(Context& loaded) {
    g_context.menu_open = loaded.menu_open;
    g_context.enabled = loaded.enabled;
    g_context.draw_2d = loaded.draw_2d;
    g_context.draw_3d = loaded.draw_3d;
    g_context.outline = loaded.outline;
    g_context.show_local_player = loaded.show_local_player;
    g_context.label_category = loaded.label_category;
    g_context.label_entity_id = loaded.label_entity_id;
    g_context.show_static = loaded.show_static;
    g_context.show_stationary = loaded.show_stationary;
    g_context.show_movable = loaded.show_movable;
    g_context.show_unknown = loaded.show_unknown;
    g_context.show_entity_controls = loaded.show_entity_controls;
    g_context.thickness = loaded.thickness;
    g_context.update_rate_hz = loaded.update_rate_hz;
    std::ranges::copy(loaded.color, g_context.color);
    g_context.class_enabled = std::move(loaded.class_enabled);
    g_context.entity_enabled = std::move(loaded.entity_enabled);
    g_context.settings_dirty = false;
}

bool ParseSettingsDocument(const std::string_view document) {
    constexpr std::uint32_t kMenuOpen = 1U << 0U;
    constexpr std::uint32_t kEnabled = 1U << 1U;
    constexpr std::uint32_t kDraw2d = 1U << 2U;
    constexpr std::uint32_t kDraw3d = 1U << 3U;
    constexpr std::uint32_t kOutline = 1U << 4U;
    constexpr std::uint32_t kShowLocalPlayer = 1U << 5U;
    constexpr std::uint32_t kLabelCategory = 1U << 6U;
    constexpr std::uint32_t kLabelEntityId = 1U << 7U;
    constexpr std::uint32_t kShowStatic = 1U << 8U;
    constexpr std::uint32_t kShowStationary = 1U << 9U;
    constexpr std::uint32_t kShowMovable = 1U << 10U;
    constexpr std::uint32_t kShowUnknown = 1U << 11U;
    constexpr std::uint32_t kShowEntityControls = 1U << 12U;
    constexpr std::uint32_t kThickness = 1U << 13U;
    constexpr std::uint32_t kUpdateRateHz = 1U << 14U;
    constexpr std::uint32_t kColor = 1U << 15U;
    constexpr std::uint32_t kClassEnabled = 1U << 16U;
    constexpr std::uint32_t kEntityEnabled = 1U << 17U;
    constexpr std::uint32_t kAllFields = (1U << 18U) - 1U;

    SettingsJsonReader reader(document);
    Context loaded;
    std::uint32_t fields{};
    if (!reader.Consume('{')) return false;
    if (reader.Consume('}')) return false;
    for (;;) {
        std::string key;
        std::uint32_t field{};
        bool parsed{};
        if (!reader.ParseString(key, 32) || !reader.Consume(':')) return false;
        if (key == "menuOpen") {
            field = kMenuOpen;
            parsed = reader.ParseBoolean(loaded.menu_open);
        } else if (key == "enabled") {
            field = kEnabled;
            parsed = reader.ParseBoolean(loaded.enabled);
        } else if (key == "draw2d") {
            field = kDraw2d;
            parsed = reader.ParseBoolean(loaded.draw_2d);
        } else if (key == "draw3d") {
            field = kDraw3d;
            parsed = reader.ParseBoolean(loaded.draw_3d);
        } else if (key == "outline") {
            field = kOutline;
            parsed = reader.ParseBoolean(loaded.outline);
        } else if (key == "showLocalPlayer") {
            field = kShowLocalPlayer;
            parsed = reader.ParseBoolean(loaded.show_local_player);
        } else if (key == "labelCategory") {
            field = kLabelCategory;
            parsed = reader.ParseBoolean(loaded.label_category);
        } else if (key == "labelEntityId") {
            field = kLabelEntityId;
            parsed = reader.ParseBoolean(loaded.label_entity_id);
        } else if (key == "showStatic") {
            field = kShowStatic;
            parsed = reader.ParseBoolean(loaded.show_static);
        } else if (key == "showStationary") {
            field = kShowStationary;
            parsed = reader.ParseBoolean(loaded.show_stationary);
        } else if (key == "showMovable") {
            field = kShowMovable;
            parsed = reader.ParseBoolean(loaded.show_movable);
        } else if (key == "showUnknown") {
            field = kShowUnknown;
            parsed = reader.ParseBoolean(loaded.show_unknown);
        } else if (key == "showEntityControls") {
            field = kShowEntityControls;
            parsed = reader.ParseBoolean(loaded.show_entity_controls);
        } else if (key == "thickness") {
            field = kThickness;
            parsed = reader.ParseNumber(loaded.thickness);
        } else if (key == "updateRateHz") {
            field = kUpdateRateHz;
            parsed = reader.ParseNumber(loaded.update_rate_hz);
        } else if (key == "color") {
            field = kColor;
            parsed = ParseColor(reader, loaded.color);
        } else if (key == "classEnabled") {
            field = kClassEnabled;
            parsed = ParseFilterMap(reader, loaded.class_enabled);
        } else if (key == "entityEnabled") {
            field = kEntityEnabled;
            parsed = ParseFilterMap(reader, loaded.entity_enabled);
        } else {
            return false;
        }
        if (!parsed || (fields & field) != 0U) return false;
        fields |= field;
        if (reader.Consume('}')) break;
        if (!reader.Consume(',')) return false;
    }
    if (fields != kAllFields || !reader.AtEnd() || !ValidSettings(loaded)) return false;
    ApplySettings(loaded);
    return true;
}

bool AppendNumber(std::string& document, const float value) {
    if (!std::isfinite(value)) return false;
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general,
        std::numeric_limits<float>::max_digits10);
    if (error != std::errc{}) return false;
    document.append(buffer.data(), end);
    return true;
}

bool AppendFilterMap(
    std::string& document, const std::unordered_map<std::uint64_t, int>& filters) {
    std::vector<std::uint64_t> ids;
    ids.reserve(filters.size());
    for (const auto& [id, enabled] : filters) {
        if (!ValidBoolean(enabled)) return false;
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    if (ids.size() > kMaximumPersistedFilters) ids.resize(kMaximumPersistedFilters);

    document.push_back('{');
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) document.push_back(',');
        std::array<char, 32> id{};
        const auto [end, error] = std::to_chars(id.data(), id.data() + id.size(), ids[index]);
        if (error != std::errc{}) return false;
        document.push_back('"');
        document.append(id.data(), end);
        document += "\":";
        const auto found = filters.find(ids[index]);
        if (found == filters.end()) return false;
        document += found->second != 0 ? "true" : "false";
    }
    document.push_back('}');
    return true;
}

bool BuildSettingsDocument(std::string& document) {
    if (!ValidSettings(g_context)) return false;
    document.clear();
    document.reserve(1024);
    const auto append_boolean = [&](const std::string_view name, const int value) {
        document.push_back('"');
        document.append(name);
        document += value != 0 ? "\":true," : "\":false,";
    };
    document.push_back('{');
    append_boolean("menuOpen", g_context.menu_open);
    append_boolean("enabled", g_context.enabled);
    append_boolean("draw2d", g_context.draw_2d);
    append_boolean("draw3d", g_context.draw_3d);
    append_boolean("outline", g_context.outline);
    append_boolean("showLocalPlayer", g_context.show_local_player);
    append_boolean("labelCategory", g_context.label_category);
    append_boolean("labelEntityId", g_context.label_entity_id);
    append_boolean("showStatic", g_context.show_static);
    append_boolean("showStationary", g_context.show_stationary);
    append_boolean("showMovable", g_context.show_movable);
    append_boolean("showUnknown", g_context.show_unknown);
    append_boolean("showEntityControls", g_context.show_entity_controls);
    document += "\"thickness\":";
    if (!AppendNumber(document, g_context.thickness)) return false;
    document += ",\"updateRateHz\":";
    if (!AppendNumber(document, g_context.update_rate_hz)) return false;
    document += ",\"color\":[";
    for (std::size_t index = 0; index < std::size(g_context.color); ++index) {
        if (index != 0) document.push_back(',');
        if (!AppendNumber(document, g_context.color[index])) return false;
    }
    document += "],\"classEnabled\":";
    if (!AppendFilterMap(document, g_context.class_enabled)) return false;
    document += ",\"entityEnabled\":";
    if (!AppendFilterMap(document, g_context.entity_enabled)) return false;
    document.push_back('}');
    return document.size() <= kMaximumSettingsBytes;
}

bool HasConfigFunctions(const AnomalyConfigServiceV1* config) noexcept {
    return HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
               config, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        config->service_version >= ANOMALY_CONFIG_SERVICE_V1_VERSION &&
        config->register_schema != nullptr && config->read != nullptr &&
        config->write_atomic != nullptr;
}

enum class SettingsLoadResult { Loaded, Missing, Failed };

SettingsLoadResult LoadSettings() {
    const auto* config = g_context.config;
    if (!HasConfigFunctions(config)) return SettingsLoadResult::Failed;

    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_result = config->read(
        config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_result.code == ANOMALY_STATUS_V1_NOT_FOUND) return SettingsLoadResult::Missing;
    if (size_result.code != ANOMALY_STATUS_V1_OK || size == 0 || size > kMaximumSettingsBytes) {
        return SettingsLoadResult::Failed;
    }

    try {
        std::vector<std::uint8_t> document(size);
        std::size_t copied = document.size();
        const AnomalyStatusV1 read_result = config->read(
            config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
            {document.data(), document.size()}, &copied);
        if (read_result.code != ANOMALY_STATUS_V1_OK || copied == 0 ||
            copied > document.size() || schema_version != kSettingsSchemaVersion) {
            return SettingsLoadResult::Failed;
        }
        return ParseSettingsDocument({
                   reinterpret_cast<const char*>(document.data()), copied})
            ? SettingsLoadResult::Loaded
            : SettingsLoadResult::Failed;
    } catch (...) {
        return SettingsLoadResult::Failed;
    }
}

bool SaveSettings() {
    const auto* config = g_context.config;
    if (!HasConfigFunctions(config)) return false;
    try {
        std::string document;
        if (!BuildSettingsDocument(document)) return false;
        const AnomalyStatusV1 result = config->write_atomic(
            config->user, anomaly::sdk::StringView(kSettingsSchemaId), kSettingsSchemaVersion,
            {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()});
        if (result.code != ANOMALY_STATUS_V1_OK) return false;
        g_context.settings_dirty = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool IsCompleteSnapshot(const std::uint32_t flags) noexcept {
    return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) == 0;
}

DisplaySettings CurrentDisplaySettings() {
    DisplaySettings settings;
    settings.enabled = g_context.enabled;
    settings.draw_2d = g_context.draw_2d;
    settings.draw_3d = g_context.draw_3d;
    settings.outline = g_context.outline;
    settings.show_local_player = g_context.show_local_player;
    settings.label_category = g_context.label_category;
    settings.label_entity_id = g_context.label_entity_id;
    settings.show_static = g_context.show_static;
    settings.show_stationary = g_context.show_stationary;
    settings.show_movable = g_context.show_movable;
    settings.show_unknown = g_context.show_unknown;
    settings.thickness = g_context.thickness;
    settings.update_rate_hz = g_context.update_rate_hz;
    std::ranges::copy(g_context.color, settings.color.begin());
    settings.class_enabled = g_context.class_enabled;
    settings.entity_enabled = g_context.entity_enabled;
    return settings;
}

void PublishDisplaySettings() {
    g_display_settings.store(
        std::make_shared<const DisplaySettings>(CurrentDisplaySettings()),
        std::memory_order_release);
}

std::string MobilityName(std::uint32_t flags) {
    if ((flags & ANOMALY_NTE_ENTITY_V1_MOVABLE) != 0) return "Movable";
    if ((flags & ANOMALY_NTE_ENTITY_V1_STATIONARY) != 0) return "Stationary";
    if ((flags & ANOMALY_NTE_ENTITY_V1_STATIC) != 0) return "Static";
    return "Unknown";
}

const std::string& CategoryLabel(const EntityView& entity) noexcept {
    return entity.category;
}

bool MobilityVisible(std::uint32_t flags) noexcept {
    if ((flags & ANOMALY_NTE_ENTITY_V1_MOVABLE) != 0) return g_context.show_movable != 0;
    if ((flags & ANOMALY_NTE_ENTITY_V1_STATIONARY) != 0) return g_context.show_stationary != 0;
    if ((flags & ANOMALY_NTE_ENTITY_V1_STATIC) != 0) return g_context.show_static != 0;
    return g_context.show_unknown != 0;
}

bool MobilityVisible(const DisplaySettings& settings, std::uint32_t flags) noexcept {
    if ((flags & ANOMALY_NTE_ENTITY_V1_MOVABLE) != 0) return settings.show_movable != 0;
    if ((flags & ANOMALY_NTE_ENTITY_V1_STATIONARY) != 0) {
        return settings.show_stationary != 0;
    }
    if ((flags & ANOMALY_NTE_ENTITY_V1_STATIC) != 0) return settings.show_static != 0;
    return settings.show_unknown != 0;
}

template <typename Resolve>
std::string ResolveUtf8(Resolve&& resolve) {
    std::size_t size{};
    if (resolve(nullptr, &size).code != ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > 1024) return {};
    std::string value(size, '\0');
    if (resolve(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const std::size_t end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value;
}

std::string ResolveCategory(
    const AnomalyNteEntitiesServiceV1* entities, std::uint64_t class_id) {
    if (const auto found = g_class_names.find(class_id);
        found != g_class_names.end()) return found->second;
    std::string value;
    if (HasField<AnomalyNteEntitiesServiceV1,
            decltype(AnomalyNteEntitiesServiceV1::class_name_utf8)>(
            entities, offsetof(AnomalyNteEntitiesServiceV1, class_name_utf8)) &&
        entities->class_name_utf8 != nullptr) {
        value = ResolveUtf8([&](char* destination, std::size_t* size) {
            return entities->class_name_utf8(
                entities->user, class_id, destination, size);
        });
    }
    if (!value.empty()) g_class_names.emplace(class_id, value);
    return value;
}

bool CollectEntities(AnomalyNteEntityFrameV1& frame, std::vector<EntityView>& entities) {
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
        !IsCompleteSnapshot(frame.flags) || frame.generation == 0 || frame.entity_count > 16384) {
        return false;
    }
    entities.clear();
    entities.reserve(frame.entity_count);
    g_class_names.reserve(frame.entity_count);
    const auto append = [&](const AnomalyNteEntitySnapshotV1& snapshot) {
        if (!IsCompleteSnapshot(snapshot.flags) || snapshot.handle.generation != frame.generation) {
            return false;
        }
        EntityView entity;
        entity.snapshot = snapshot;
        entity.category = ResolveCategory(service.get(), entity.snapshot.class_id);
        if (entity.category.empty()) {
            entity.category = MobilityName(entity.snapshot.flags) + " class #" +
                std::to_string(entity.snapshot.class_id);
        }
        entity.entity_id_label = "#" + std::to_string(entity.snapshot.entity_id);
        entity.category_entity_id_label = entity.category + " " + entity.entity_id_label;
        entities.push_back(std::move(entity));
        return true;
    };

    std::array<AnomalyNteEntitySnapshotV1, kEntityPageCapacity> page{};
    std::uint32_t offset{};
    for (;;) {
        for (auto& snapshot : page) snapshot = AnomalyNteEntitySnapshotV1{sizeof(snapshot)};
        AnomalyNteEntityPageRequestV1 request{
            sizeof(request), 0, frame.generation, offset,
            static_cast<std::uint32_t>(page.size()), 0, 0, 0, 0, 0};
        AnomalyNteEntityPageResultV1 result{sizeof(result)};
        if (service->page(service->user, &request, page.data(), &result).code !=
                ANOMALY_STATUS_V1_OK ||
            !IsCompleteSnapshot(result.flags) || result.generation != frame.generation ||
            result.total_matches != frame.entity_count || result.returned > page.size()) {
            return false;
        }
        if (offset > result.total_matches) {
            return result.returned == 0 && result.next_offset == result.total_matches &&
                entities.size() == result.total_matches;
        }
        if (result.returned > result.total_matches - offset) return false;
        for (std::uint32_t index = 0; index < result.returned; ++index) {
            if (!append(page[index])) return false;
        }
        const std::uint32_t consumed = offset + result.returned;
        if (consumed >= result.total_matches) {
            return result.next_offset == result.total_matches &&
                entities.size() == result.total_matches;
        }
        if (result.next_offset != consumed || result.next_offset <= offset) return false;
        offset = result.next_offset;
    }
}
void SetAll(std::unordered_map<std::uint64_t, int>& values, int enabled) {
    for (auto& [id, value] : values) {
        static_cast<void>(id);
        value = enabled;
    }
}

bool AnyEnabled(const std::unordered_map<std::uint64_t, int>& values) {
    return values.empty() || std::ranges::any_of(values, [](const auto& entry) {
        return entry.second != 0;
    });
}

bool FilterEnabled(
    const std::unordered_map<std::uint64_t, int>& filters,
    const std::uint64_t id) noexcept {
    const auto found = filters.find(id);
    return found == filters.end() || found->second != 0;
}

bool HasVisibleMobility() noexcept {
    return g_context.show_static != 0 || g_context.show_stationary != 0 ||
        g_context.show_movable != 0 || g_context.show_unknown != 0;
}

bool HasVisibleMobility(const DisplaySettings& settings) noexcept {
    return settings.show_static != 0 || settings.show_stationary != 0 ||
        settings.show_movable != 0 || settings.show_unknown != 0;
}

bool HasActiveRendering() {
    return g_context.enabled != 0 && (g_context.draw_2d != 0 || g_context.draw_3d != 0) &&
        HasVisibleMobility() && AnyEnabled(g_context.class_enabled) &&
        AnyEnabled(g_context.entity_enabled);
}

bool HasActiveRendering(const DisplaySettings& settings) {
    return settings.enabled != 0 && (settings.draw_2d != 0 || settings.draw_3d != 0) &&
        HasVisibleMobility(settings) && AnyEnabled(settings.class_enabled) &&
        AnyEnabled(settings.entity_enabled);
}

bool ReconcileEntityFilters(const EntityCache& cache) {
    if (!cache.available || cache.entities.empty()) return false;
    g_context.class_enabled.reserve(
        g_context.class_enabled.size() + cache.entities.size());
    g_context.entity_enabled.reserve(
        g_context.entity_enabled.size() + cache.entities.size());
    bool changed{};
    for (const EntityView& entity : cache.entities) {
        changed = g_context.class_enabled.try_emplace(entity.snapshot.class_id, 1).second ||
            changed;
        changed = g_context.entity_enabled.try_emplace(entity.snapshot.entity_id, 1).second ||
            changed;
    }
    return changed;
}

std::vector<ClassSummary> BuildClassSummaries(const std::vector<EntityView>& entities) {
    std::map<std::uint64_t, ClassSummary> by_id;
    for (const auto& entity : entities) {
        if (!MobilityVisible(entity.snapshot.flags)) continue;
        auto& summary = by_id[entity.snapshot.class_id];
        summary.id = entity.snapshot.class_id;
        summary.name = CategoryLabel(entity);
        ++summary.count;
    }
    std::vector<ClassSummary> result;
    result.reserve(by_id.size());
    for (auto& [id, summary] : by_id) {
        static_cast<void>(id);
        result.push_back(std::move(summary));
    }
    std::ranges::sort(result, [](const ClassSummary& left, const ClassSummary& right) {
        if (left.count != right.count) return left.count > right.count;
        return left.name < right.name;
    });
    return result;
}

std::uint32_t MobilityVisibilityKey() noexcept {
    return (g_context.show_static != 0 ? 1u : 0u) |
        (g_context.show_stationary != 0 ? 1u << 1u : 0u) |
        (g_context.show_movable != 0 ? 1u << 2u : 0u) |
        (g_context.show_unknown != 0 ? 1u << 3u : 0u);
}

const std::vector<ClassSummary>& CachedClassSummaries(const EntityCache& cache) {
    const std::uint32_t visibility = MobilityVisibilityKey();
    if (cache.frame.generation != g_context.class_summary_generation ||
        visibility != g_context.class_summary_visibility) {
        g_context.class_summaries = BuildClassSummaries(cache.entities);
        g_context.class_summary_generation = cache.frame.generation;
        g_context.class_summary_visibility = visibility;
    }
    return g_context.class_summaries;
}

std::size_t PageCount(std::size_t item_count) noexcept {
    return std::max<std::size_t>(1, (item_count + kPageSize - 1) / kPageSize);
}

void PageButtons(
    const AnomalyUiServiceV1* ui, const std::string_view prefix_key,
    const std::string_view english_prefix, const std::string_view stable_prefix,
    const std::size_t item_count, std::size_t& page) {
    const std::size_t pages = PageCount(item_count);
    page = std::min(page, pages - 1);
    if (ui->button != nullptr) {
        const std::string previous = g_context.localizer.Label(
            "action.previous", "Previous", std::string(stable_prefix) + "-previous");
        const std::string next = g_context.localizer.Label(
            "action.next", "Next", std::string(stable_prefix) + "-next");
        if (ui->button(ui->user, anomaly::sdk::StringView(previous), 0, 0) && page > 0) --page;
        if (ui->button(ui->user, anomaly::sdk::StringView(next), 0, 0) && page + 1 < pages) ++page;
    }
    const std::string prefix = g_context.localizer.Text(prefix_key, english_prefix);
    const std::string current = std::to_string(page + 1U);
    const std::string total = std::to_string(pages);
    const std::array arguments{
        std::string_view(prefix), std::string_view(current), std::string_view(total)};
    const std::string page_text = g_context.localizer.Format(
        "pagination.summary", "{0} page {1}/{2}", arguments);
    ui->text(ui->user, anomaly::sdk::StringView(page_text));
}

bool DrawMenu(const AnomalyUiServiceV1* ui, const EntityCache& cache) {
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr) return false;
    bool settings_changed{};
    if (ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 430.0F, 620.0F, 4u);
    }
    const int menu_open_before = g_context.menu_open;
    const std::string title = g_context.localizer.Label(
        "window.title", "Entity BBox ESP", "entity-bbox-esp");
    const int visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView(title), &g_context.menu_open, 0);
    settings_changed = menu_open_before != g_context.menu_open;
    if (visible != 0) {
        const bool checkboxes =
            HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::checkbox)>(
                ui, offsetof(AnomalyUiServiceV1, checkbox)) && ui->checkbox != nullptr;
        if (checkboxes) {
            const auto checkbox = [&](const std::string_view key,
                                      const std::string_view english,
                                      const std::string_view stable_id, int* value) {
                const std::string label = g_context.localizer.Label(key, english, stable_id);
                if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), value) != 0) {
                    settings_changed = true;
                }
            };
            checkbox("option.enabled", "Enabled", "enabled", &g_context.enabled);
            checkbox("option.bbox_2d", "2D bbox", "bbox-2d", &g_context.draw_2d);
            checkbox("option.box_3d", "3D box", "box-3d", &g_context.draw_3d);
            checkbox("option.outline", "Outline", "outline", &g_context.outline);
            checkbox("option.include_local_player", "Include local player",
                "include-local-player", &g_context.show_local_player);
            checkbox("option.label_category", "Label category",
                "label-category", &g_context.label_category);
            checkbox("option.label_entity_id", "Label entity ID",
                "label-entity-id", &g_context.label_entity_id);
            const std::string mobility =
                g_context.localizer.Text("section.mobility", "Mobility");
            ui->text(ui->user, anomaly::sdk::StringView(mobility));
            checkbox("option.movable", "Movable", "movable", &g_context.show_movable);
            checkbox("option.stationary", "Stationary", "stationary", &g_context.show_stationary);
            checkbox("option.static", "Static", "static", &g_context.show_static);
            checkbox("option.unknown", "Unknown", "unknown", &g_context.show_unknown);
            checkbox("option.show_entity_controls", "Show entity controls",
                "show-entity-controls", &g_context.show_entity_controls);
        }
        if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::slider_float)>(
                ui, offsetof(AnomalyUiServiceV1, slider_float)) &&
            ui->slider_float != nullptr) {
            const std::string update_rate = g_context.localizer.Label(
                "option.update_rate", "Update rate (Hz)", "update-rate");
            if (ui->slider_float(
                    ui->user, anomaly::sdk::StringView(update_rate),
                    &g_context.update_rate_hz, 2.0F, 60.0F) != 0) {
                settings_changed = true;
            }
            const std::string thickness = g_context.localizer.Label(
                "option.thickness", "Thickness", "thickness");
            if (ui->slider_float(
                    ui->user, anomaly::sdk::StringView(thickness),
                    &g_context.thickness, 0.5F, 6.0F) != 0) {
                settings_changed = true;
            }
        }
        if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::color_edit4)>(
                ui, offsetof(AnomalyUiServiceV1, color_edit4)) &&
            ui->color_edit4 != nullptr) {
            const std::string color =
                g_context.localizer.Label("option.color", "Color", "color");
            if (ui->color_edit4(
                    ui->user, anomaly::sdk::StringView(color), g_context.color) != 0) {
                settings_changed = true;
            }
        }

        const auto& classes = CachedClassSummaries(cache);
        std::string summary;
        if (cache.available) {
            const std::string entity_count = std::to_string(cache.entities.size());
            const std::string class_count = std::to_string(classes.size());
            const std::array arguments{
                std::string_view(entity_count), std::string_view(class_count)};
            summary = g_context.localizer.Format(
                "summary.cached", "{0} cached entities / {1} visible classes", arguments);
        } else {
            summary = g_context.localizer.Text("summary.unavailable", "No entity snapshot");
        }
        ui->text(ui->user, anomaly::sdk::StringView(summary));
        if (!HasActiveRendering()) {
            const std::string paused = g_context.localizer.Text(
                "summary.paused", "Entity collection paused");
            ui->text(ui->user, anomaly::sdk::StringView(paused));
        }

        if (ui->button != nullptr) {
            const auto button = [&](const std::string_view key, const std::string_view english,
                                    const std::string_view stable_id) {
                const std::string label = g_context.localizer.Label(key, english, stable_id);
                return ui->button(ui->user, anomaly::sdk::StringView(label), 0, 0) != 0;
            };
            if (button("action.refresh", "Refresh now", "refresh-now")) {
                g_refresh_requested.store(true, std::memory_order_release);
            }
            if (button("action.enable_all_classes", "Enable all classes", "enable-all-classes")) {
                SetAll(g_context.class_enabled, 1);
                settings_changed = true;
            }
            if (button(
                    "action.disable_all_classes", "Disable all classes", "disable-all-classes")) {
                SetAll(g_context.class_enabled, 0);
                settings_changed = true;
            }
            if (button(
                    "action.enable_all_entities", "Enable all entities", "enable-all-entities")) {
                SetAll(g_context.entity_enabled, 1);
                settings_changed = true;
            }
            if (button("action.disable_all_entities", "Disable all entities",
                    "disable-all-entities")) {
                SetAll(g_context.entity_enabled, 0);
                settings_changed = true;
            }
        }

        if (checkboxes) {
            PageButtons(ui, "section.classes", "Classes", "classes",
                classes.size(), g_context.class_page);
            const std::size_t first = g_context.class_page * kPageSize;
            const std::size_t last = std::min(classes.size(), first + kPageSize);
            for (std::size_t index = first; index < last; ++index) {
                const auto& entry = classes[index];
                const std::string label = entry.name + " (" + std::to_string(entry.count) +
                    ")##class-" + std::to_string(entry.id);
                if (ui->checkbox(
                        ui->user, anomaly::sdk::StringView(label),
                        &g_context.class_enabled[entry.id]) != 0) {
                    settings_changed = true;
                }
                if (g_context.show_entity_controls != 0 && ui->button != nullptr) {
                    const std::string select = g_context.localizer.Label(
                        "action.select", "Select", "class-select-" + std::to_string(entry.id));
                    if (ui->button(ui->user, anomaly::sdk::StringView(select), 0, 0)) {
                        g_context.selected_class = entry.id;
                        g_context.entity_page = 0;
                    }
                }
            }

            if (g_context.show_entity_controls != 0 && g_context.selected_class != 0) {
                std::vector<const EntityView*> selected;
                for (const auto& entity : cache.entities) {
                    if (entity.snapshot.class_id == g_context.selected_class &&
                        MobilityVisible(entity.snapshot.flags)) selected.push_back(&entity);
                }
                PageButtons(ui, "section.entities", "Entities", "entities",
                    selected.size(), g_context.entity_page);
                const std::size_t entity_first = g_context.entity_page * kPageSize;
                const std::size_t entity_last =
                    std::min(selected.size(), entity_first + kPageSize);
                for (std::size_t index = entity_first; index < entity_last; ++index) {
                    const auto& entity = *selected[index];
                    const std::string label = CategoryLabel(entity) + " #" +
                        std::to_string(entity.snapshot.entity_id) + "##entity-" +
                        std::to_string(entity.snapshot.entity_id);
                    if (ui->checkbox(
                            ui->user, anomaly::sdk::StringView(label),
                            &g_context.entity_enabled[entity.snapshot.entity_id]) != 0) {
                        settings_changed = true;
                    }
                }
            }
        }
    }
    ui->end_window(ui->user);
    return settings_changed;
}

void ClearEntityCache() noexcept {
    g_entity_cache.store({}, std::memory_order_release);
    g_class_names.clear();
}

void RefreshCacheIfDue() {
    const auto now = Clock::now();
    if (!g_refresh_requested.exchange(false, std::memory_order_acq_rel) &&
        now < g_next_refresh) {
        return;
    }
    auto cache = std::make_shared<EntityCache>();
    if (g_context.host != nullptr && CollectEntities(cache->frame, cache->entities)) {
        cache->available = true;
        g_entity_cache.store(std::move(cache), std::memory_order_release);
    } else {
        // Keep drawing only snapshots that were complete when sampled. Generation changes and
        // partial refreshes still clear the cache; an older Tick alone does not invalidate it.
        ClearEntityCache();
    }
    const auto settings = g_display_settings.load(std::memory_order_acquire);
    const float rate = std::clamp(
        settings ? settings->update_rate_hz : 30.0F, 2.0F, 60.0F);
    g_next_refresh = now +
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
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
        HasField<AnomalyUe5AhudFrameV1,
            decltype(AnomalyUe5AhudFrameV1::draw_rect)>(
            frame, offsetof(AnomalyUe5AhudFrameV1, draw_rect)) &&
        frame->project != nullptr && frame->measure_text != nullptr &&
        frame->draw_text != nullptr && frame->draw_line != nullptr && frame->draw_rect != nullptr;
}

struct ProjectedEntityBox final {
    std::array<std::array<float, 2>, 8> corners{};
    float left{(std::numeric_limits<float>::max)()};
    float top{(std::numeric_limits<float>::max)()};
    float right{(std::numeric_limits<float>::lowest)()};
    float bottom{(std::numeric_limits<float>::lowest)()};
};

struct ScreenLine final {
    float start_x{};
    float start_y{};
    float end_x{};
    float end_y{};
};

struct OutlineLines final {
    std::array<ScreenLine, 4> values{};
    std::size_t count{};
};

bool ProjectEntityBox(
    const AnomalyUe5AhudFrameV1* frame,
    const AnomalyNteEntitySnapshotV1& snapshot,
    ProjectedEntityBox& bounds) noexcept {
    for (std::size_t axis{}; axis != 3; ++axis) {
        if (!std::isfinite(snapshot.bounds_center[axis]) ||
            !std::isfinite(snapshot.bounds_extent[axis]) ||
            snapshot.bounds_extent[axis] < 0.0) {
            return false;
        }
    }

    ProjectedEntityBox projected;
    for (std::uint32_t corner{}; corner != projected.corners.size(); ++corner) {
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
        projected.corners[corner] = {screen[0], screen[1]};
        projected.left = (std::min)(projected.left, screen[0]);
        projected.top = (std::min)(projected.top, screen[1]);
        projected.right = (std::max)(projected.right, screen[0]);
        projected.bottom = (std::max)(projected.bottom, screen[1]);
    }

    const float viewport_width = static_cast<float>(frame->viewport_width);
    const float viewport_height = static_cast<float>(frame->viewport_height);
    if (projected.right - projected.left < 1.0F ||
        projected.bottom - projected.top < 1.0F || projected.right < 0.0F ||
        projected.bottom < 0.0F || projected.left > viewport_width ||
        projected.top > viewport_height) {
        return false;
    }
    bounds = projected;
    return true;
}

OutlineLines ClipOutlineToViewport(
    const ProjectedEntityBox& bounds,
    const float viewport_width,
    const float viewport_height) noexcept {
    OutlineLines result;
    const std::array values{
        bounds.left, bounds.top, bounds.right, bounds.bottom,
        viewport_width, viewport_height};
    if (!std::ranges::all_of(values, [](const float value) {
            return std::isfinite(value);
        }) ||
        viewport_width <= 0.0F || viewport_height <= 0.0F ||
        bounds.right - bounds.left < 1.0F || bounds.bottom - bounds.top < 1.0F) {
        return result;
    }

    const float left = std::clamp(bounds.left, 0.0F, viewport_width);
    const float right = std::clamp(bounds.right, 0.0F, viewport_width);
    const float top = std::clamp(bounds.top, 0.0F, viewport_height);
    const float bottom = std::clamp(bounds.bottom, 0.0F, viewport_height);
    const auto add_horizontal = [&](const float y, const bool reverse) {
        if (y < 0.0F || y > viewport_height || right - left < 1.0F) return;
        result.values[result.count++] = reverse
            ? ScreenLine{right, y, left, y} : ScreenLine{left, y, right, y};
    };
    const auto add_vertical = [&](const float x, const bool reverse) {
        if (x < 0.0F || x > viewport_width || bottom - top < 1.0F) return;
        result.values[result.count++] = reverse
            ? ScreenLine{x, bottom, x, top} : ScreenLine{x, top, x, bottom};
    };
    add_horizontal(bounds.top, false);
    add_vertical(bounds.right, false);
    add_horizontal(bounds.bottom, true);
    add_vertical(bounds.left, true);
    return result;
}

bool ClipLineToViewport(
    ScreenLine& line, const float viewport_width, const float viewport_height) noexcept {
    const std::array values{
        line.start_x, line.start_y, line.end_x, line.end_y,
        viewport_width, viewport_height};
    if (!std::ranges::all_of(values, [](const float value) {
            return std::isfinite(value);
        }) ||
        viewport_width <= 0.0F || viewport_height <= 0.0F) {
        return false;
    }

    const float delta_x = line.end_x - line.start_x;
    const float delta_y = line.end_y - line.start_y;
    float start_t{};
    float end_t{1.0F};
    const auto clip = [&](const float p, const float q) {
        if (p == 0.0F) return q >= 0.0F;
        const float ratio = q / p;
        if (p < 0.0F) {
            if (ratio > end_t) return false;
            if (ratio > start_t) start_t = ratio;
        } else {
            if (ratio < start_t) return false;
            if (ratio < end_t) end_t = ratio;
        }
        return true;
    };
    if (!clip(-delta_x, line.start_x) ||
        !clip(delta_x, viewport_width - line.start_x) ||
        !clip(-delta_y, line.start_y) ||
        !clip(delta_y, viewport_height - line.start_y)) {
        return false;
    }
    const ScreenLine original = line;
    line.start_x = original.start_x + start_t * delta_x;
    line.start_y = original.start_y + start_t * delta_y;
    line.end_x = original.start_x + end_t * delta_x;
    line.end_y = original.start_y + end_t * delta_y;
    return true;
}

float FitTextScaleToViewport(
    const float viewport_width,
    const float viewport_height,
    const float measured_width,
    const float measured_height,
    const float horizontal_padding,
    const float vertical_padding) noexcept {
    const std::array values{
        viewport_width, viewport_height, measured_width, measured_height,
        horizontal_padding, vertical_padding};
    if (!std::ranges::all_of(values, [](const float value) {
            return std::isfinite(value);
        }) ||
        viewport_width <= horizontal_padding * 2.0F ||
        viewport_height <= vertical_padding * 2.0F || measured_width < 0.0F ||
        measured_height <= 0.0F || horizontal_padding < 0.0F || vertical_padding < 0.0F) {
        return 0.0F;
    }

    const float width_scale = measured_width > 0.0F
        ? (viewport_width - horizontal_padding * 2.0F) / measured_width
        : 1.0F;
    const float height_scale =
        (viewport_height - vertical_padding * 2.0F) / measured_height;
    const float scale = (std::min)({1.0F, width_scale, height_scale});
    return std::isfinite(scale) && scale > 0.0F ? scale : 0.0F;
}

struct LabelMeasurement final {
    float width{};
    float height{};
    bool available{};
};

using LabelMeasurementCache = std::unordered_map<std::string_view, LabelMeasurement>;

bool MeasureLabel(
    const AnomalyUe5AhudFrameV1* frame,
    const std::string_view label,
    const float text_scale,
    LabelMeasurementCache* const measurements,
    float& width,
    float& height) noexcept {
    const auto measure = [&]() {
        LabelMeasurement result;
        result.available = frame->measure_text(
            frame->user, anomaly::sdk::StringView(label), text_scale,
            &result.width, &result.height) != 0 &&
            std::isfinite(result.width) && std::isfinite(result.height) &&
            result.width >= 0.0F && result.height > 0.0F;
        return result;
    };
    if (measurements == nullptr) {
        const LabelMeasurement measurement = measure();
        if (!measurement.available) return false;
        width = measurement.width;
        height = measurement.height;
        return true;
    }
    try {
        const auto found = measurements->find(label);
        if (found != measurements->end()) {
            if (!found->second.available) return false;
            width = found->second.width;
            height = found->second.height;
            return true;
        }
        const LabelMeasurement measurement = measure();
        measurements->emplace(label, measurement);
        if (!measurement.available) return false;
        width = measurement.width;
        height = measurement.height;
        return true;
    } catch (...) {
        return false;
    }
}

void Draw2dBounds(
    const AnomalyUe5AhudFrameV1* frame,
    const ProjectedEntityBox& bounds,
    const DisplaySettings& settings,
    const std::uint32_t color) noexcept {
    const OutlineLines lines = ClipOutlineToViewport(
        bounds, static_cast<float>(frame->viewport_width),
        static_cast<float>(frame->viewport_height));
    constexpr std::uint32_t kOutlineColor = ANOMALY_RGBA_V1(0, 0, 0, 220);
    if (settings.outline != 0) {
        const float outline_thickness = settings.thickness + 2.0F;
        for (std::size_t index{}; index != lines.count; ++index) {
            const ScreenLine& line = lines.values[index];
            static_cast<void>(frame->draw_line(
                frame->user, line.start_x, line.start_y, line.end_x, line.end_y,
                kOutlineColor, outline_thickness));
        }
    }
    for (std::size_t index{}; index != lines.count; ++index) {
        const ScreenLine& line = lines.values[index];
        static_cast<void>(frame->draw_line(
            frame->user, line.start_x, line.start_y, line.end_x, line.end_y,
            color, settings.thickness));
    }
}

void Draw3dBounds(
    const AnomalyUe5AhudFrameV1* frame,
    const ProjectedEntityBox& bounds,
    const DisplaySettings& settings,
    const std::uint32_t color) noexcept {
    constexpr std::array<std::array<std::size_t, 2>, 12> kEdges{{
        {{0, 1}}, {{0, 2}}, {{0, 4}}, {{1, 3}}, {{1, 5}}, {{2, 3}},
        {{2, 6}}, {{3, 7}}, {{4, 5}}, {{4, 6}}, {{5, 7}}, {{6, 7}}}};
    constexpr std::uint32_t kOutlineColor = ANOMALY_RGBA_V1(0, 0, 0, 220);
    const float viewport_width = static_cast<float>(frame->viewport_width);
    const float viewport_height = static_cast<float>(frame->viewport_height);
    const auto draw_edges = [&](const std::uint32_t edge_color, const float thickness) {
        for (const auto& edge : kEdges) {
            ScreenLine line{
                bounds.corners[edge[0]][0], bounds.corners[edge[0]][1],
                bounds.corners[edge[1]][0], bounds.corners[edge[1]][1]};
            if (!ClipLineToViewport(line, viewport_width, viewport_height)) continue;
            static_cast<void>(frame->draw_line(
                frame->user, line.start_x, line.start_y, line.end_x, line.end_y,
                edge_color, thickness));
        }
    };
    if (settings.outline != 0) draw_edges(kOutlineColor, settings.thickness + 2.0F);
    draw_edges(color, settings.thickness);
}

void DrawLabel(
    const AnomalyUe5AhudFrameV1* frame,
    const ProjectedEntityBox& bounds,
    const std::string_view label,
    const std::uint32_t color,
    LabelMeasurementCache* const measurements) noexcept {
    if (label.empty()) return;
    constexpr float kTextScale = 1.0F;
    constexpr float kHorizontalPadding = 3.0F;
    constexpr float kVerticalPadding = 2.0F;
    constexpr float kLabelGap = 4.0F;
    float width{};
    float height{};
    if (!MeasureLabel(frame, label, kTextScale, measurements, width, height)) {
        return;
    }

    const float viewport_width = static_cast<float>(frame->viewport_width);
    const float viewport_height = static_cast<float>(frame->viewport_height);
    const float scale = FitTextScaleToViewport(
        viewport_width, viewport_height, width, height,
        kHorizontalPadding, kVerticalPadding);
    if (scale <= 0.0F) return;
    width *= scale;
    height *= scale;

    const float maximum_x = viewport_width - kHorizontalPadding - width;
    const float maximum_y = viewport_height - kVerticalPadding - height;
    if (maximum_x < kHorizontalPadding || maximum_y < kVerticalPadding) return;
    const float x = std::clamp(
        (bounds.left + bounds.right - width) * 0.5F,
        kHorizontalPadding, maximum_x);
    float y = bounds.top - height - kLabelGap;
    if (y < kVerticalPadding) y = bounds.bottom + kLabelGap;
    y = std::clamp(y, kVerticalPadding, maximum_y);
    static_cast<void>(frame->draw_rect(
        frame->user, x - kHorizontalPadding, y - kVerticalPadding,
        width + kHorizontalPadding * 2.0F, height + kVerticalPadding * 2.0F,
        ANOMALY_RGBA_V1(0, 0, 0, 180)));
    static_cast<void>(frame->draw_text(
        frame->user, anomaly::sdk::StringView(label), x, y, color, scale));
}

std::uint32_t PackColor(const std::array<float, 4>& rgba) noexcept {
    const auto channel = [](const float value) {
        return static_cast<std::uint32_t>(std::lround(
            std::clamp(std::isfinite(value) ? value : 0.0F, 0.0F, 1.0F) * 255.0F));
    };
    return ANOMALY_RGBA_V1(
        channel(rgba[0]), channel(rgba[1]), channel(rgba[2]), channel(rgba[3]));
}

void ANOMALY_CALL DrawAhud(
    void*, const AnomalyUe5AhudFrameV1* frame) noexcept {
    if (!AhudFrameAvailable(frame)) return;
    const auto settings = g_display_settings.load(std::memory_order_acquire);
    const auto cache = g_entity_cache.load(std::memory_order_acquire);
    if (!settings || !cache || !cache->available || !HasActiveRendering(*settings)) return;

    const std::uint32_t color = PackColor(settings->color);
    LabelMeasurementCache category_measurements;
    LabelMeasurementCache* const measurements =
        settings->label_category != 0 && settings->label_entity_id == 0
            ? &category_measurements
            : nullptr;
    for (const EntityView& entity : cache->entities) {
        if (!MobilityVisible(*settings, entity.snapshot.flags) ||
            !FilterEnabled(settings->class_enabled, entity.snapshot.class_id) ||
            !FilterEnabled(settings->entity_enabled, entity.snapshot.entity_id) ||
            (settings->show_local_player == 0 &&
             (entity.snapshot.flags & ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER) != 0)) {
            continue;
        }
        ProjectedEntityBox bounds;
        if (!ProjectEntityBox(frame, entity.snapshot, bounds)) continue;
        if (settings->draw_2d != 0) Draw2dBounds(frame, bounds, *settings, color);
        if (settings->draw_3d != 0) Draw3dBounds(frame, bounds, *settings, color);
        if (settings->label_category != 0 || settings->label_entity_id != 0) {
            const std::string_view label = settings->label_category != 0
                ? (settings->label_entity_id != 0
                    ? std::string_view(entity.category_entity_id_label)
                    : std::string_view(entity.category))
                : std::string_view(entity.entity_id_label);
            DrawLabel(frame, bounds, label, color, measurements);
        }
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
    const AnomalyStatusV1 status = g_context.ahud->unsubscribe(g_context.ahud->user, handle);
    if (status.code == ANOMALY_STATUS_V1_OK || status.code == ANOMALY_STATUS_V1_NOT_FOUND ||
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
    if (!ui) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    const auto core = host_view.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    const auto config = host_view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    const auto ahud = host_view.Query<AnomalyUe5AhudServiceV1>(
        ANOMALY_UE5_AHUD_SERVICE_V1_ID, ANOMALY_UE5_AHUD_SERVICE_V1_VERSION);
    if (!config || !HasConfigFunctions(config.get()) || !ahud ||
        !AhudServiceAvailable(ahud.get())) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    g_context = {};
    g_ahud_subscription = {};
    ClearEntityCache();
    g_display_settings.store({}, std::memory_order_release);
    g_refresh_requested.store(true, std::memory_order_release);
    g_next_refresh = {};
    g_reconciled_cache_generation = 0;
    g_context.host = host;
    g_context.localizer = anomaly::plugins::Localizer(host);
    g_context.core = core.get();
    g_context.config = config.get();
    g_context.ahud = ahud.get();
    const AnomalyStatusV1 registered = config->register_schema(
        config->user, anomaly::sdk::StringView(kSettingsSchemaId), kSettingsSchemaVersion,
        {reinterpret_cast<const std::uint8_t*>(kSettingsSchema.data()), kSettingsSchema.size()},
        &g_context.settings_schema);
    if (registered.code != ANOMALY_STATUS_V1_OK || g_context.settings_schema.id == 0 ||
        g_context.settings_schema.generation == 0) {
        g_context = {};
        g_display_settings.store({}, std::memory_order_release);
        return registered.code == ANOMALY_STATUS_V1_OK
            ? AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}}
            : registered;
    }
    if (LoadSettings() == SettingsLoadResult::Failed) LogSettingsFailure("load");
    PublishDisplaySettings();
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    ClearEntityCache();
    g_refresh_requested.store(true, std::memory_order_release);
    g_next_refresh = {};
    g_reconciled_cache_generation = 0;
    PublishDisplaySettings();
    const AnomalyStatusV1 ahud_status = SubscribeAhud();
    if (ahud_status.code != ANOMALY_STATUS_V1_OK) {
        ClearEntityCache();
        g_display_settings.store({}, std::memory_order_release);
        return ahud_status;
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    const AnomalyStatusV1 ahud_status = UnsubscribeAhud();
    const bool saved = !g_context.settings_dirty || SaveSettings();
    if (!saved) LogSettingsFailure("write");
    ClearEntityCache();
    g_display_settings.store({}, std::memory_order_release);
    g_reconciled_cache_generation = 0;
    if (ahud_status.code != ANOMALY_STATUS_V1_OK) return ahud_status;
    return saved ? anomaly::sdk::Ok() : AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}};
}

void ANOMALY_CALL Unload(void*) {
    static_cast<void>(UnsubscribeAhud());
    ClearEntityCache();
    g_display_settings.store({}, std::memory_order_release);
    g_reconciled_cache_generation = 0;
    g_context = {};
}

void ANOMALY_CALL Update(void*, double) {
    const auto settings = g_display_settings.load(std::memory_order_acquire);
    if (!settings || !HasActiveRendering(*settings)) {
        // Retain the last snapshot for the settings UI, but make a newly enabled display
        // refresh on its next Game-thread update instead of waiting for a stale interval.
        g_next_refresh = {};
        return;
    }
    RefreshCacheIfDue();
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    const auto cache = g_entity_cache.load(std::memory_order_acquire);
    const EntityCache empty;
    const EntityCache& snapshot = cache ? *cache : empty;
    bool filters_changed{};
    if (cache && snapshot.frame.generation != g_reconciled_cache_generation) {
        filters_changed = ReconcileEntityFilters(snapshot);
        g_reconciled_cache_generation = snapshot.frame.generation;
    } else if (!cache) {
        g_reconciled_cache_generation = 0;
    }
    const bool settings_changed = DrawMenu(ui, snapshot);
    if (settings_changed) g_context.settings_dirty = true;
    if (filters_changed || settings_changed) PublishDisplaySettings();
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.entity-esp"),
        anomaly::sdk::StringView("Entity ESP"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("2.4.2"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
