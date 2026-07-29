#include "anomaly/sdk/cpp.hpp"
#include "plugins/common/localization.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 32U * 1024U;
constexpr std::size_t kMaximumPresets = 32;
constexpr std::size_t kMaximumPresetNameBytes = 63;
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
    }
  }
}
)json";

struct CoordinatePreset {
    std::string name;
    std::array<double, 3> position{};
};

struct PendingTeleport {
    bool queued{};
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double position[3]{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyConfigServiceV1* config{};
    AnomalyGenerationHandleV1 settings_schema{};
    double target[3]{};
    std::vector<CoordinatePreset> presets;
    std::array<char, kMaximumPresetNameBytes + 1U> preset_name{};
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
        ui->begin_window != nullptr && ui->end_window != nullptr && ui->text != nullptr &&
        ui->button != nullptr && ui->input_double != nullptr && ui->separator != nullptr &&
        ui->begin_table != nullptr && ui->table_next_row != nullptr &&
        ui->table_next_column != nullptr && ui->end_table != nullptr &&
        ui->input_text != nullptr && ui->button_enabled != nullptr;
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

std::string_view TrimPresetName(const std::string_view name) noexcept {
    constexpr std::string_view whitespace = " \t\r\n";
    const std::size_t first = name.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    const std::size_t last = name.find_last_not_of(whitespace);
    return name.substr(first, last - first + 1U);
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

bool ParseSettingsDocument(
    const std::string_view document, std::array<double, 3>& target,
    std::vector<CoordinatePreset>& presets) {
    SettingsJsonReader reader(document);
    bool target_seen{};
    bool presets_seen{};
    if (!reader.Consume('{')) return false;
    for (;;) {
        std::string key;
        if (!reader.ReadString(key, 16) || !reader.Consume(':')) return false;
        if (key == "target") {
            if (target_seen || !ReadPosition(reader, target)) return false;
            target_seen = true;
        } else if (key == "presets") {
            if (presets_seen || !ReadPresets(reader, presets)) return false;
            presets_seen = true;
        } else {
            return false;
        }
        if (reader.Consume('}')) break;
        if (!reader.Consume(',')) return false;
    }
    return target_seen && presets_seen && reader.AtEnd();
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

std::string SerializeSettings(
    const std::array<double, 3>& target, const std::vector<CoordinatePreset>& presets) {
    std::string document = "{\"target\":[" + FormatDouble(target[0]) + "," +
        FormatDouble(target[1]) + "," + FormatDouble(target[2]) + "],\"presets\":[";
    for (std::size_t index = 0; index < presets.size(); ++index) {
        if (index != 0) document.push_back(',');
        const CoordinatePreset& preset = presets[index];
        document += "{\"name\":\"" + EscapeJsonString(preset.name) +
            "\",\"position\":[" + FormatDouble(preset.position[0]) + "," +
            FormatDouble(preset.position[1]) + "," + FormatDouble(preset.position[2]) + "]}";
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
        std::array<double, 3> target{};
        std::vector<CoordinatePreset> presets;
        if (read_status.code != ANOMALY_STATUS_V1_OK ||
            schema_version != kSettingsSchemaVersion || copied == 0 ||
            copied > document.size() ||
            !ParseSettingsDocument(
                {reinterpret_cast<const char*>(document.data()), copied}, target, presets)) {
            return false;
        }
        std::scoped_lock lock(g_context.mutex);
        std::ranges::copy(target, g_context.target);
        g_context.presets = std::move(presets);
        g_context.settings_dirty = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveSettings() {
    std::array<double, 3> target{};
    std::vector<CoordinatePreset> presets;
    {
        std::scoped_lock lock(g_context.mutex);
        if (!g_context.settings_dirty) return true;
        std::ranges::copy(g_context.target, target.begin());
        presets = g_context.presets;
    }
    if (!ConfigMethodsAvailable(g_context.config)) return false;

    try {
        const std::string document = SerializeSettings(target, presets);
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

void QueueRequest(
    const AnomalyNteSessionSnapshotV1& session, const AnomalyNtePlayerSnapshotV1& player,
    const double position[3]) noexcept {
    std::scoped_lock lock(g_context.mutex);
    g_context.pending.queued = true;
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

std::string FormatPosition(const std::array<double, 3>& position) {
    std::array<char, 160> buffer{};
    std::snprintf(
        buffer.data(), buffer.size(), "%.3f, %.3f, %.3f",
        position[0], position[1], position[2]);
    return buffer.data();
}

void SavePreset(const double target[3]) {
    const std::string_view trimmed = TrimPresetName(g_context.preset_name.data());
    if (!IsValidPresetName(trimmed) || !IsFinitePosition(target)) return;

    std::scoped_lock lock(g_context.mutex);
    const auto existing = std::ranges::find(
        g_context.presets, trimmed, &CoordinatePreset::name);
    if (existing != g_context.presets.end()) {
        for (std::size_t axis = 0; axis < existing->position.size(); ++axis) {
            existing->position[axis] = target[axis];
        }
    } else {
        if (g_context.presets.size() >= kMaximumPresets) return;
        CoordinatePreset preset;
        preset.name = trimmed;
        for (std::size_t axis = 0; axis < preset.position.size(); ++axis) {
            preset.position[axis] = target[axis];
        }
        g_context.presets.push_back(std::move(preset));
    }
    g_context.settings_dirty = true;
}

bool CanSavePreset() {
    const std::string_view trimmed = TrimPresetName(g_context.preset_name.data());
    if (!IsValidPresetName(trimmed)) return false;
    std::scoped_lock lock(g_context.mutex);
    return g_context.presets.size() < kMaximumPresets ||
        std::ranges::any_of(g_context.presets, [trimmed](const CoordinatePreset& preset) {
            return preset.name == trimmed;
        });
}

void DrawPresetEditor(const AnomalyUiServiceV1* ui, double target[3]) {
    ui->separator(ui->user);
    DrawText(ui, g_context.localizer.Text("preset.title", "Coordinate presets"));

    const std::string name_label = g_context.localizer.Label(
        "preset.name", "Preset name", "preset-name");
    static_cast<void>(ui->input_text(
        ui->user, anomaly::sdk::StringView(name_label), g_context.preset_name.data(),
        g_context.preset_name.size(), ANOMALY_UI_TEXT_INPUT_V1_NONE));
    const std::string save_label = g_context.localizer.Label(
        "preset.save", "Save preset", "preset-save");
    if (ui->button_enabled(
            ui->user, anomaly::sdk::StringView(save_label), 0.0F, 0.0F,
            CanSavePreset() ? 1 : 0) != 0) {
        SavePreset(target);
    }

    std::vector<CoordinatePreset> presets;
    {
        std::scoped_lock lock(g_context.mutex);
        presets = g_context.presets;
    }
    if (presets.empty()) {
        DrawText(ui, g_context.localizer.Text("preset.empty", "No presets saved"));
        return;
    }

    if (ui->begin_table(
            ui->user, anomaly::sdk::StringView("coordinate-presets"), 4, 0,
            0.0F, 220.0F) == 0) {
        return;
    }
    ui->table_next_row(ui->user);
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("preset.column.name", "Name"));
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("preset.column.position", "Coordinates"));
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("preset.column.load", "Load"));
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, g_context.localizer.Text("preset.column.delete", "Delete"));

    std::size_t load_index = presets.size();
    std::size_t delete_index = presets.size();
    for (std::size_t index = 0; index < presets.size(); ++index) {
        const CoordinatePreset& preset = presets[index];
        ui->table_next_row(ui->user);
        static_cast<void>(ui->table_next_column(ui->user));
        DrawText(ui, preset.name);
        static_cast<void>(ui->table_next_column(ui->user));
        DrawText(ui, FormatPosition(preset.position));
        static_cast<void>(ui->table_next_column(ui->user));
        const std::string load_label = g_context.localizer.Label(
            "preset.load", "Load", "preset-load-" + std::to_string(index));
        if (ui->button(
                ui->user, anomaly::sdk::StringView(load_label), 0.0F, 0.0F) != 0) {
            load_index = index;
        }
        static_cast<void>(ui->table_next_column(ui->user));
        const std::string delete_label = g_context.localizer.Label(
            "preset.delete", "Delete", "preset-delete-" + std::to_string(index));
        if (ui->button(
                ui->user, anomaly::sdk::StringView(delete_label), 0.0F, 0.0F) != 0) {
            delete_index = index;
        }
    }
    ui->end_table(ui->user);

    if (load_index < presets.size()) {
        std::ranges::copy(presets[load_index].position, target);
        std::snprintf(
            g_context.preset_name.data(), g_context.preset_name.size(), "%s",
            presets[load_index].name.c_str());
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t axis = 0; axis < presets[load_index].position.size(); ++axis) {
            g_context.target[axis] = target[axis];
        }
        g_context.settings_dirty = true;
        g_context.current_position_unavailable = false;
    }
    if (delete_index < presets.size()) {
        std::scoped_lock lock(g_context.mutex);
        if (delete_index < g_context.presets.size() &&
            g_context.presets[delete_index].name == presets[delete_index].name) {
            g_context.presets.erase(g_context.presets.begin() + delete_index);
            g_context.settings_dirty = true;
        }
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto ui = QueryService<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    const auto config = QueryService<AnomalyConfigServiceV1>(
        host, ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
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
        g_context.settings_schema = {};
        g_context.target[0] = 0.0;
        g_context.target[1] = 0.0;
        g_context.target[2] = 0.0;
        g_context.presets.clear();
        g_context.preset_name.fill('\0');
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
    std::scoped_lock lock(g_context.mutex);
    g_context.pending = {};
    g_context.has_result = false;
    g_context.result_message[0] = '\0';
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t) {
    if (context != &g_context) return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
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
    g_context.settings_schema = {};
    g_context.presets.clear();
    g_context.preset_name.fill('\0');
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
    const AnomalyHostApiV1* host{};
    {
        std::scoped_lock lock(g_context.mutex);
        if (!g_context.pending.queued) return;
        pending = g_context.pending;
        g_context.pending = {};
        host = g_context.host;
    }
    if (host == nullptr) {
        RecordResult(ANOMALY_STATUS_V1_UNAVAILABLE);
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
        RecordResult(teleport ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : teleport.status);
        return;
    }

    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    request.flags = 0;
    request.world = pending.world;
    request.player = pending.player;
    for (std::size_t axis = 0; axis != 3; ++axis) request.position[axis] = pending.position[axis];
    const AnomalyStatusV1 status = teleport.service->teleport(
        teleport.service->user, &request);
    RecordResult(status);
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

    bool target_changed = input_ui->input_double(
        input_ui->user, anomaly::sdk::StringView("X"), &target[0], 0.0, 0.0) != 0;
    target_changed |= input_ui->input_double(
        input_ui->user, anomaly::sdk::StringView("Y"), &target[1], 0.0, 0.0) != 0;
    target_changed |= input_ui->input_double(
        input_ui->user, anomaly::sdk::StringView("Z"), &target[2], 0.0, 0.0) != 0;
    const std::string current = g_context.localizer.Label(
        "action.current", "Use current coordinates", "current-coordinates");
    if (input_ui->button(
            input_ui->user, anomaly::sdk::StringView(current),
            0.0F, 0.0F) != 0) {
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
        DrawText(input_ui, g_context.localizer.Text(
            "current.unavailable", "Current coordinates are unavailable"));
    }

    const std::string apply = g_context.localizer.Label(
        "action.apply", "Teleport", "teleport");
    if (input_ui->button(
            input_ui->user, anomaly::sdk::StringView(apply),
            0.0F, 0.0F) != 0) {
        TryQueueRequest(host, target);
    }
    DrawResult(input_ui);
    DrawPresetEditor(input_ui, target);
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
        anomaly::sdk::StringView("1.2.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
