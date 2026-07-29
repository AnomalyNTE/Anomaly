#include "anomaly/sdk/cpp.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 1024;
constexpr std::string_view kSettingsSchema = R"json(
{"type":"object","additionalProperties":false,"required":["value"],"properties":{"value":{"type":"integer","minimum":0,"maximum":4294967295}}}
)json";

struct Context {
    const AnomalyUiServiceV1* ui{};
    const AnomalyConfigServiceV1* config{};
    AnomalyGenerationHandleV1 settings_schema{};
    std::uint32_t value{};
    bool settings_dirty{};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool ConfigMethodsAvailable(const AnomalyConfigServiceV1* service) noexcept {
    return HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
               service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->service_version >= ANOMALY_CONFIG_SERVICE_V1_VERSION &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
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

    bool ConsumeLiteral(const std::string_view expected) noexcept {
        SkipWhitespace();
        if (input_.substr(position_, expected.size()) != expected) return false;
        position_ += expected.size();
        return true;
    }

    bool ReadUint32(std::uint32_t& value) noexcept {
        SkipWhitespace();
        const std::size_t begin = position_;
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
        const auto [end, error] = std::from_chars(
            input_.data() + begin, input_.data() + position_, value);
        return error == std::errc{} && end == input_.data() + position_;
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

bool ParseSettingsDocument(const std::string_view document, std::uint32_t& value) noexcept {
    SettingsJsonReader reader(document);
    return reader.Consume('{') && reader.ConsumeLiteral("\"value\"") && reader.Consume(':') &&
        reader.ReadUint32(value) && reader.Consume('}') && reader.AtEnd();
}

enum class SettingsLoadResult { Loaded, Missing, Failed };

SettingsLoadResult LoadSettings() {
    if (!ConfigMethodsAvailable(g_context.config)) return SettingsLoadResult::Failed;
    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code == ANOMALY_STATUS_V1_NOT_FOUND) return SettingsLoadResult::Missing;
    if (size_status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > kMaximumSettingsBytes) {
        return SettingsLoadResult::Failed;
    }

    try {
        std::vector<std::uint8_t> document(size);
        std::size_t copied = document.size();
        const AnomalyStatusV1 read_status = g_context.config->read(
            g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
            {document.data(), document.size()}, &copied);
        std::uint32_t value{};
        if (read_status.code != ANOMALY_STATUS_V1_OK ||
            schema_version != kSettingsSchemaVersion || copied == 0 ||
            copied > document.size() ||
            !ParseSettingsDocument(
                {reinterpret_cast<const char*>(document.data()), copied}, value)) {
            return SettingsLoadResult::Failed;
        }
        g_context.value = value;
        g_context.settings_dirty = false;
        return SettingsLoadResult::Loaded;
    } catch (...) {
        return SettingsLoadResult::Failed;
    }
}

bool SaveSettings() {
    if (!g_context.settings_dirty) return true;
    if (!ConfigMethodsAvailable(g_context.config)) return false;
    const std::string document = "{\"value\":" + std::to_string(g_context.value) + "}";
    const AnomalyStatusV1 status = g_context.config->write_atomic(
        g_context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(document));
    if (status.code != ANOMALY_STATUS_V1_OK) return false;
    g_context.settings_dirty = false;
    return true;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const anomaly::sdk::Host view(host);
    const auto ui = view.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    const auto config = view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    if (!ui || !ConfigMethodsAvailable(config.get())) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    g_context = {};
    g_context.ui = ui.get();
    g_context.config = config.get();
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
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    return SaveSettings() ? anomaly::sdk::Ok()
                          : AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}};
}

void ANOMALY_CALL Unload(void*) { g_context = {}; }

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    int open = 1;
    anomaly::sdk::UiWindow window(ui, "Reliable Config", &open);
    if (!window) return;
    const std::string text = "Atomic JSON config value: " + std::to_string(g_context.value);
    ui->text(ui->user, anomaly::sdk::StringView(text));
    if (ui->button(ui->user, anomaly::sdk::StringView("Increment"), 0, 0)) {
        ++g_context.value;
        g_context.settings_dirty = true;
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.example.reliable-config"),
        anomaly::sdk::StringView("Reliable Config"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.2.0"), Load, Start, Stop, Unload, nullptr, Draw};
    return anomaly::sdk::Ok();
}
