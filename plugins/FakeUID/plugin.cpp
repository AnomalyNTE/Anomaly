#include "anomaly/sdk/cpp.hpp"
#include "fake_uid_profile.hpp"
#include "plugins/common/localization.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::string_view kDefaultDisplayUid = "000000000000";
constexpr std::size_t kMaximumUidDigits = 256;
constexpr std::string_view kSettingsSchemaId = "fake-uid-settings-v2";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 4096;
constexpr std::uint32_t kObjectBatchSize = 128;
constexpr std::uint32_t kFullObjectBatchSize = 512;
constexpr std::uint32_t kObjectTailWindow = 65536;
// NTE can retain prior RoleID UI layers while a newer one is visible.
constexpr std::size_t kMaximumTrackedWidgets = 8;
constexpr std::uint64_t kObjectRescanInterval = 300;
constexpr std::uint64_t kWidgetVerifyInterval = 30;
constexpr std::string_view kTargetWidgetName = "TextBlock_RoleID";

constexpr std::string_view kSettingsSchema = R"json(
{
  "type":"object",
  "additionalProperties":false,
  "required":["enabled","displayUid"],
  "properties":{
    "enabled":{"type":"boolean"},
    "displayUid":{"type":"string","pattern":"^[0-9]{1,256}$"},
    "detectedUid":{"type":"string","pattern":"^[0-9]{1,20}$"}
  }
}
)json";

struct UnrealString final {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct UnrealText final {
    void* data{};
    std::uint32_t flags{};
    std::uint32_t padding{};
};

static_assert(sizeof(wchar_t) == 2);
static_assert(sizeof(UnrealString) == 16);
static_assert(sizeof(UnrealText) == 16);

using AssignStringFn = UnrealString*(ANOMALY_CALL*)(UnrealString*, const wchar_t*);
using FromStringFn = UnrealText*(ANOMALY_CALL*)(UnrealText*, UnrealString*);
using FreeStringFn = void(ANOMALY_CALL*)(void* allocation);
using TextToStringFn = UnrealString*(ANOMALY_CALL*)(UnrealString*, const UnrealText*);
using SetTextFn = void(ANOMALY_CALL*)(void* widget, const UnrealText* text);

struct SettingsSnapshot final {
    bool enabled{true};
    std::string display_uid;
    std::wstring display_wide;
};

struct TrackedWidget final {
    AnomalyGenerationHandleV1 handle{};
    std::uint64_t applied_revision{};
    std::uint64_t last_verified_tick{};
};

struct Context final {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyConfigServiceV1* config{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyWindowServiceV1* window{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5NamesServiceV1* names{};
    AnomalyGenerationHandleV1 settings_schema{};
    AnomalyGenerationHandleV1 window_handle{};
    std::atomic<std::shared_ptr<const SettingsSnapshot>> settings;
    std::array<char, kMaximumUidDigits + 1> editor{};
    std::string ui_status;
    std::atomic<std::uint64_t> detected_uid{};
    std::atomic<std::uint32_t> save_state{};
    std::atomic<std::uint64_t> settings_revision{1};
    std::atomic_bool rescan_requested{true};
    std::uint64_t update_tick{};
    std::uint64_t next_scan_tick{};
    std::uint64_t object_generation{};
    std::uint32_t object_cursor{};
    std::uint32_t scan_start{};
    std::uint32_t scan_count{};
    std::uint32_t scan_batch_size{kObjectBatchSize};
    std::uint32_t target_name_id{};
    bool scan_active{};
    bool full_scan_active{};
    std::unordered_set<std::uint32_t> rejected_name_ids;
    std::array<TrackedWidget, kMaximumTrackedWidgets> widgets{};
    std::size_t widget_count{};
    std::uintptr_t object_registry{};
    std::uint32_t text_field_offset{};
    AssignStringFn assign_string{};
    FromStringFn from_string{};
    FreeStringFn free_string{};
    TextToStringFn text_to_string{};
    SetTextFn set_text{};
    bool start_attempted{};
    bool stop_completed{};
};

AnomalyStatusV1 Status(
    const std::uint32_t code, const std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service* Query(
    const AnomalyHostApiV1* host, const char* id, const std::uint32_t version) noexcept {
    return anomaly::sdk::Host(host).Query<Service>(id, version).get();
}

bool ConfigReady(const AnomalyConfigServiceV1* service) noexcept {
    return HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
               service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
}

bool SchedulerReady(const AnomalySchedulerServiceV1* service) noexcept {
    return HasField<AnomalySchedulerServiceV1, decltype(AnomalySchedulerServiceV1::cancel)>(
               service, offsetof(AnomalySchedulerServiceV1, cancel)) &&
        service->schedule != nullptr && service->cancel != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1* service) noexcept {
    return HasField<AnomalySignatureServiceV1, decltype(AnomalySignatureServiceV1::resolve)>(
               service, offsetof(AnomalySignatureServiceV1, resolve)) &&
        service->resolve != nullptr;
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* service) noexcept {
    return HasField<AnomalyUe5ObjectsServiceV1,
               decltype(AnomalyUe5ObjectsServiceV1::snapshot_by_handle)>(
               service, offsetof(AnomalyUe5ObjectsServiceV1, snapshot_by_handle)) &&
        service->generation != nullptr && service->count != nullptr &&
        service->snapshot_at != nullptr && service->snapshot_by_handle != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* service) noexcept {
    return HasField<AnomalyUe5NamesServiceV1,
               decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               service, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
        service->resolve_utf8 != nullptr;
}

bool WindowReady(const AnomalyWindowServiceV1* service) noexcept {
    return HasField<AnomalyWindowServiceV1, decltype(AnomalyWindowServiceV1::end)>(
               service, offsetof(AnomalyWindowServiceV1, end)) &&
        service->register_window != nullptr && service->release_window != nullptr &&
        service->set_open != nullptr && service->state != nullptr &&
        service->begin != nullptr && service->end != nullptr;
}

bool DigitsValid(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumUidDigits) return false;
    return std::all_of(value.begin(), value.end(), [](const char digit) {
        return digit >= '0' && digit <= '9';
    });
}

std::uint64_t ParseUid(std::string_view value) noexcept;

std::shared_ptr<const SettingsSnapshot> MakeSettings(
    const bool enabled, const std::string_view display_uid) {
    if (!DigitsValid(display_uid)) return {};
    auto settings = std::make_shared<SettingsSnapshot>();
    settings->enabled = enabled;
    settings->display_uid.assign(display_uid);
    settings->display_wide.reserve(display_uid.size());
    for (const char digit : display_uid) {
        settings->display_wide.push_back(static_cast<wchar_t>(digit));
    }
    return settings;
}

void PublishSettings(
    Context& context, const std::shared_ptr<const SettingsSnapshot>& settings) noexcept {
    context.settings.store(settings, std::memory_order_release);
}

std::shared_ptr<const SettingsSnapshot> ReadSettings(const Context& context) noexcept {
    return context.settings.load(std::memory_order_acquire);
}

void ResetEditor(Context& context) noexcept {
    const auto settings = ReadSettings(context);
    if (!settings) return;
    context.editor.fill('\0');
    const std::size_t count = (std::min)(settings->display_uid.size(), kMaximumUidDigits);
    std::copy_n(settings->display_uid.data(), count, context.editor.data());
}

bool PersistSettings(
    Context& context, const SettingsSnapshot& settings,
    std::string* const error = nullptr) noexcept {
    try {
        nlohmann::json json{
            {"enabled", settings.enabled}, {"displayUid", settings.display_uid}};
        const std::uint64_t detected =
            context.detected_uid.load(std::memory_order_acquire);
        if (detected != 0) json["detectedUid"] = std::to_string(detected);
        const std::string document = json.dump();
        const AnomalyStatusV1 status = context.config->write_atomic(
            context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            kSettingsSchemaVersion, Bytes(document));
        if (status.code != ANOMALY_STATUS_V1_OK) {
            if (error != nullptr) {
                *error = "Save failed (" + std::to_string(status.code) + ")";
                if (status.message.data != nullptr && status.message.size != 0) {
                    error->append(": ").append(status.message.data, status.message.size);
                }
            }
            return false;
        }
        return true;
    } catch (...) {
        if (error != nullptr) *error = "Save failed: internal error";
        return false;
    }
}

void ANOMALY_CALL PersistSettingsTask(
    void* user, AnomalyGenerationHandleV1) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    const auto settings = ReadSettings(*context);
    const bool saved = settings && PersistSettings(*context, *settings);
    context->save_state.store(saved ? 2U : 3U, std::memory_order_release);
}

bool ScheduleSettingsPersist(Context& context) noexcept {
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status = context.scheduler->schedule(
        context.scheduler->user, 0, PersistSettingsTask, &context, &task);
    return status.code == ANOMALY_STATUS_V1_OK && task.id != 0;
}

void RecordDetectedUid(Context& context, const std::uint64_t detected) noexcept {
    if (detected == 0) return;
    const std::uint64_t previous =
        context.detected_uid.exchange(detected, std::memory_order_acq_rel);
    if (previous == detected) return;
    context.save_state.store(1U, std::memory_order_release);
    if (!ScheduleSettingsPersist(context)) {
        context.save_state.store(3U, std::memory_order_release);
    }
}

bool ApplySettings(
    Context& context, const bool enabled, const std::string_view display_uid,
    std::string& error) noexcept {
    try {
        const auto settings = MakeSettings(enabled, display_uid);
        if (!settings) {
            error = "UID must contain 1-256 digits";
            return false;
        }
        PublishSettings(context, settings);
        context.settings_revision.fetch_add(1, std::memory_order_acq_rel);
        context.rescan_requested.store(true, std::memory_order_release);
        context.save_state.store(1U, std::memory_order_release);
        if (!ScheduleSettingsPersist(context)) {
            context.save_state.store(3U, std::memory_order_release);
            error = "Applied, but save scheduling failed";
            return true;
        }
        return true;
    } catch (...) {
        error = "Apply failed: internal error";
        return false;
    }
}

bool LoadSettings(Context& context) noexcept {
    try {
        std::uint32_t version{};
        std::size_t size{};
        const AnomalyStatusV1 size_status = context.config->read(
            context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &version,
            {nullptr, 0}, &size);
        if (size_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            const auto defaults = MakeSettings(true, kDefaultDisplayUid);
            if (!defaults || !PersistSettings(context, *defaults)) return false;
            PublishSettings(context, defaults);
            ResetEditor(context);
            return true;
        }
        if (size_status.code != ANOMALY_STATUS_V1_OK ||
            version != kSettingsSchemaVersion || size == 0 ||
            size > kMaximumSettingsBytes) {
            return false;
        }
        std::vector<std::uint8_t> document(size);
        std::size_t copied = document.size();
        if (context.config->read(
                context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &version,
                {document.data(), document.size()}, &copied).code != ANOMALY_STATUS_V1_OK ||
            copied == 0 || copied > document.size()) {
            return false;
        }
        const auto json = nlohmann::json::parse(document.begin(), document.begin() + copied);
        if (!json.is_object() || json.size() < 2 || json.size() > 3 ||
            !json.contains("enabled") || !json.at("enabled").is_boolean() ||
            !json.contains("displayUid") || !json.at("displayUid").is_string() ||
            (json.contains("detectedUid") && !json.at("detectedUid").is_string())) {
            return false;
        }
        const auto settings = MakeSettings(
            json.at("enabled").get<bool>(),
            json.at("displayUid").get_ref<const std::string&>());
        if (!settings) return false;
        if (json.contains("detectedUid")) {
            const std::uint64_t detected = ParseUid(
                json.at("detectedUid").get_ref<const std::string&>());
            if (detected == 0) return false;
            context.detected_uid.store(detected, std::memory_order_release);
        }
        PublishSettings(context, settings);
        ResetEditor(context);
        return true;
    } catch (...) {
        return false;
    }
}

bool Resolve(
    const AnomalySignatureServiceV1& signature,
    const std::string_view pattern,
    std::uintptr_t& target) noexcept {
    target = 0;
    return signature.resolve(
               signature.user, anomaly::sdk::StringView("HTGame.exe"),
               anomaly::sdk::StringView(".text"), anomaly::sdk::StringView(pattern),
               &target).code == ANOMALY_STATUS_V1_OK &&
        target != 0;
}

bool ResolveRipRelative32(
    const AnomalySignatureServiceV1& signature,
    const std::string_view pattern,
    const std::uint32_t displacement_offset,
    const std::uint32_t instruction_size,
    std::uintptr_t& target) noexcept {
    std::uintptr_t instruction{};
    if (!Resolve(signature, pattern, instruction) || instruction_size == 0 ||
        displacement_offset > instruction_size ||
        instruction_size - displacement_offset < sizeof(std::int32_t)) {
        return false;
    }
    std::int32_t displacement{};
    std::memcpy(
        &displacement,
        reinterpret_cast<const void*>(instruction + displacement_offset),
        sizeof(displacement));
    const auto resolved = static_cast<std::intptr_t>(instruction) +
        static_cast<std::intptr_t>(instruction_size) + displacement;
    if (resolved <= 0) return false;
    target = static_cast<std::uintptr_t>(resolved);
    return true;
}

std::uint64_t ParseUid(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 20) return 0;
    std::uint64_t result{};
    for (const char digit_character : value) {
        if (digit_character < '0' || digit_character > '9') return 0;
        const std::uint64_t digit = static_cast<std::uint64_t>(digit_character - '0');
        if (result > (UINT64_MAX - digit) / 10U) return 0;
        result = result * 10U + digit;
    }
    return result;
}

bool BuildReplacement(
    const UnrealString* const input, const std::wstring_view display_uid,
    std::wstring& replacement, std::uint64_t& detected_uid, bool& changed) {
    if (input == nullptr || input->data == nullptr || input->count <= 1 ||
        input->count > 2048 || input->capacity < input->count ||
        input->data[input->count - 1] != L'\0') {
        return false;
    }
    const std::wstring_view text(
        input->data, static_cast<std::size_t>(input->count - 1));
    std::size_t end = text.size();
    while (end != 0 && (text[end - 1] < L'0' || text[end - 1] > L'9')) --end;
    std::size_t begin = end;
    while (begin != 0 && text[begin - 1] >= L'0' && text[begin - 1] <= L'9') --begin;
    if (begin == end) return false;

    detected_uid = 0;
    for (std::size_t index = begin; index < end; ++index) {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[index] - L'0');
        if (detected_uid > (UINT64_MAX - digit) / 10U) {
            detected_uid = 0;
            break;
        }
        detected_uid = detected_uid * 10U + digit;
    }
    replacement.clear();
    replacement.reserve(text.size() - (end - begin) + display_uid.size());
    replacement.append(text.substr(0, begin));
    replacement.append(display_uid);
    replacement.append(text.substr(end));
    changed = text.substr(begin, end - begin) != display_uid;
    return replacement.size() + 1 <= 2048;
}

bool ResolveName(
    const AnomalyUe5NamesServiceV1& names, const std::uint32_t name_id,
    std::string& value) {
    std::size_t size{};
    if (name_id == 0 ||
        names.resolve_utf8(names.user, name_id, nullptr, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > 1024) {
        return false;
    }
    value.assign(size, '\0');
    if (names.resolve_utf8(names.user, name_id, value.data(), &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size == 0 || size > value.size()) {
        return false;
    }
    const std::size_t terminator = value.find('\0');
    if (terminator == std::string::npos) return false;
    value.resize(terminator);
    return true;
}

bool TrackWidget(Context& context, const AnomalyUe5ObjectSnapshotV1& snapshot) noexcept {
    for (std::size_t index = 0; index < context.widget_count; ++index) {
        if (context.widgets[index].handle.id == snapshot.handle.id &&
            context.widgets[index].handle.generation == snapshot.handle.generation) {
            return false;
        }
    }
    if (context.widget_count == context.widgets.size()) {
        const auto candidate_serial = static_cast<std::uint32_t>(snapshot.handle.id >> 32U);
        auto oldest = context.widgets.begin();
        for (auto widget = oldest + 1; widget != context.widgets.end(); ++widget) {
            if (static_cast<std::uint32_t>(widget->handle.id >> 32U) <
                static_cast<std::uint32_t>(oldest->handle.id >> 32U)) {
                oldest = widget;
            }
        }
        if (candidate_serial <= static_cast<std::uint32_t>(oldest->handle.id >> 32U)) {
            return false;
        }
        *oldest = {snapshot.handle, 0, 0};
    } else {
        context.widgets[context.widget_count++] = {snapshot.handle, 0, 0};
    }
    context.target_name_id = snapshot.name_id;
    context.rejected_name_ids.clear();
    return true;
}

void BeginObjectScan(
    Context& context, const std::uint32_t count, const bool full_scan) noexcept {
    context.scan_count = count;
    context.full_scan_active = full_scan;
    context.scan_start = full_scan || count <= kObjectTailWindow
        ? 0
        : count - kObjectTailWindow;
    context.object_cursor = count;
    context.scan_batch_size = full_scan ? kFullObjectBatchSize : kObjectBatchSize;
    context.scan_active = true;
}

void ScanForWidgets(Context& context) {
    if (!ObjectsReady(context.objects) || !NamesReady(context.names)) {
        return;
    }
    const std::uint64_t generation = context.objects->generation(context.objects->user);
    const std::uint32_t count = context.objects->count(context.objects->user);
    if (generation == 0 || count == 0) return;
    if (context.object_generation != generation) {
        context.object_generation = generation;
        context.scan_count = 0;
        context.target_name_id = 0;
        context.widget_count = 0;
        context.rejected_name_ids.clear();
        context.rescan_requested.store(true, std::memory_order_release);
    }
    const bool requested = context.rescan_requested.exchange(
        false, std::memory_order_acq_rel);
    if (requested) {
        BeginObjectScan(context, count, true);
    } else if (!context.scan_active &&
               (context.scan_count != count || context.update_tick >= context.next_scan_tick)) {
        BeginObjectScan(context, count, context.widget_count == 0);
    } else if (context.scan_active && context.scan_count != count) {
        context.scan_count = count;
        if (context.full_scan_active) {
            context.scan_start = 0;
        } else {
            context.scan_start = count > kObjectTailWindow ? count - kObjectTailWindow : 0;
        }
    }
    if (!context.scan_active) return;
    if (context.object_cursor <= context.scan_start || context.object_cursor > count) {
        context.object_cursor = count;
    }
    const std::uint32_t available = context.object_cursor - context.scan_start;
    const std::uint32_t begin = context.object_cursor -
        (std::min)(available, context.scan_batch_size);
    while (context.object_cursor > begin) {
        const std::uint32_t index = --context.object_cursor;
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (context.objects->snapshot_at(
                context.objects->user, index, &snapshot).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (context.target_name_id != 0 && snapshot.name_id == context.target_name_id) {
            static_cast<void>(TrackWidget(context, snapshot));
            continue;
        }
        if (snapshot.name_id == 0 ||
            context.rejected_name_ids.find(snapshot.name_id) !=
                context.rejected_name_ids.end()) {
            continue;
        }
        std::string name;
        if (ResolveName(*context.names, snapshot.name_id, name)) {
            if (name == kTargetWidgetName) {
                static_cast<void>(TrackWidget(context, snapshot));
            } else {
                context.rejected_name_ids.insert(snapshot.name_id);
            }
        }
    }
    if (context.object_cursor <= context.scan_start) {
        context.scan_active = false;
        context.next_scan_tick = context.update_tick + kObjectRescanInterval;
    }
}

bool ResolveWidgetAddress(
    const Context& context, const AnomalyGenerationHandleV1 handle,
    std::uintptr_t& widget) noexcept {
    widget = 0;
    if (context.object_registry == 0 || handle.id == 0) return false;
    const std::uint32_t encoded_index = static_cast<std::uint32_t>(handle.id);
    if (encoded_index == 0) return false;
    const std::uint32_t index = encoded_index - 1U;
    const std::uint32_t expected_serial = static_cast<std::uint32_t>(handle.id >> 32U);
    __try {
        const auto chunks = *reinterpret_cast<const std::uintptr_t* const*>(
            context.object_registry + fake_uid_profile::kObjectRegistryItemsOffset);
        if (chunks == nullptr) return false;
        const std::uintptr_t chunk =
            chunks[index / fake_uid_profile::kObjectChunkSize];
        if (chunk == 0) return false;
        const std::uintptr_t item =
            chunk + static_cast<std::uintptr_t>(
                        index % fake_uid_profile::kObjectChunkSize) *
                fake_uid_profile::kObjectItemStride;
        const std::uint32_t serial = *reinterpret_cast<const std::uint32_t*>(
            item + fake_uid_profile::kObjectItemSerialOffset);
        const std::uintptr_t object = *reinterpret_cast<const std::uintptr_t*>(item);
        if (serial != expected_serial || object == 0 ||
            *reinterpret_cast<const std::uint32_t*>(
                object + fake_uid_profile::kObjectNameOffset) != context.target_name_id) {
            return false;
        }
        const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(object);
        if (vtable == 0 ||
            *reinterpret_cast<const std::uintptr_t*>(
                vtable + fake_uid_profile::kSetTextVtableOffset) !=
                    reinterpret_cast<std::uintptr_t>(context.set_text)) {
            return false;
        }
        widget = object;
        return true;
    } __except (1) {
        return false;
    }
}

bool ApplyToWidget(
    Context& context, const AnomalyGenerationHandleV1 handle,
    const SettingsSnapshot& settings) {
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (!ObjectsReady(context.objects) ||
        context.objects->snapshot_by_handle(
            context.objects->user, handle, &snapshot).code != ANOMALY_STATUS_V1_OK ||
        snapshot.name_id != context.target_name_id) {
        return false;
    }
    std::uintptr_t widget{};
    if (!ResolveWidgetAddress(context, handle, widget)) return false;

    UnrealString current{};
    const auto* const current_text = reinterpret_cast<const UnrealText*>(
        widget + context.text_field_offset);
    if (context.text_to_string(&current, current_text) == nullptr ||
        current.data == nullptr || current.count <= 1 || current.capacity < current.count) {
        if (current.data != nullptr) context.free_string(current.data);
        return false;
    }
    std::wstring replacement;
    std::uint64_t detected{};
    bool changed{};
    std::wstring restored_uid;
    std::wstring_view target_uid = settings.display_wide;
    if (!settings.enabled) {
        const std::uint64_t known_uid =
            context.detected_uid.load(std::memory_order_acquire);
        if (known_uid == 0) {
            context.free_string(current.data);
            return true;
        }
        restored_uid = std::to_wstring(known_uid);
        target_uid = restored_uid;
    }
    const bool built = BuildReplacement(
        &current, target_uid, replacement, detected, changed);
    context.free_string(current.data);
    if (!built) return false;
    if (settings.enabled && detected != 0 && changed) {
        RecordDetectedUid(context, detected);
    }
    if (!changed) return true;

    UnrealString owned{};
    if (context.assign_string(&owned, replacement.c_str()) == nullptr ||
        owned.data == nullptr || owned.count <= 1 || owned.capacity < owned.count) {
        if (owned.data != nullptr) context.free_string(owned.data);
        return false;
    }
    UnrealText replacement_text{};
    if (context.from_string(&replacement_text, &owned) == nullptr) {
        context.free_string(owned.data);
        return false;
    }
    context.set_text(reinterpret_cast<void*>(widget), &replacement_text);
    context.free_string(owned.data);
    return true;
}

void ANOMALY_CALL Update(void* user, double) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || context->stop_completed) return;
    try {
        ++context->update_tick;
        ScanForWidgets(*context);
        const auto settings = ReadSettings(*context);
        if (!settings) return;
        const std::uint64_t revision =
            context->settings_revision.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < context->widget_count;) {
            auto& widget = context->widgets[index];
            const bool revision_pending = widget.applied_revision < revision;
            const bool verification_due =
                context->update_tick - widget.last_verified_tick >= kWidgetVerifyInterval;
            if (!revision_pending && !verification_due) {
                ++index;
                continue;
            }
            if (ApplyToWidget(*context, widget.handle, *settings)) {
                widget.applied_revision = revision;
                widget.last_verified_tick = context->update_tick;
                ++index;
            } else {
                context->widgets[index] = context->widgets[--context->widget_count];
            }
        }
    } catch (...) {
    }
}

AnomalyStatusV1 ReleaseWindow(Context& context) noexcept {
    if (context.window_handle.id == 0) return anomaly::sdk::Ok();
    const AnomalyStatusV1 status = context.window->release_window(
        context.window->user, context.window_handle);
    if (status.code != ANOMALY_STATUS_V1_OK &&
        status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        return Status(ANOMALY_STATUS_V1_FAILED, "Custom UID window did not release");
    }
    context.window_handle = {};
    return anomaly::sdk::Ok();
}

bool EnsureWindow(Context& context) {
    if (context.window_handle.id != 0) return true;
    if (!WindowReady(context.window)) {
        context.window = Query<AnomalyWindowServiceV1>(
            context.host, ANOMALY_WINDOW_SERVICE_V1_ID,
            ANOMALY_WINDOW_SERVICE_V1_VERSION);
    }
    if (!WindowReady(context.window)) return false;

    AnomalyWindowSpecV1 window{};
    window.struct_size = sizeof(window);
    window.flags = ANOMALY_WINDOW_V1_NO_COLLAPSE;
    window.id = anomaly::sdk::StringView("fake-uid-settings");
    const std::string title = context.localizer.Text("window.title", "Custom UID");
    window.title = anomaly::sdk::StringView(title);
    window.initial_width = 240.0F;
    window.initial_height = 170.0F;
    window.minimum_width = 220.0F;
    window.minimum_height = 150.0F;
    window.maximum_width = 300.0F;
    window.maximum_height = 240.0F;
    window.default_open = 1;
    const AnomalyStatusV1 status = context.window->register_window(
        context.window->user, &window, &context.window_handle);
    if (status.code != ANOMALY_STATUS_V1_OK || context.window_handle.id == 0) return false;
    return context.window->set_open(
        context.window->user, context.window_handle, 1).code == ANOMALY_STATUS_V1_OK;
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host is invalid");
    }
    *plugin_context = nullptr;
    auto* const context = new (std::nothrow) Context();
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED, "allocation failed");

    context->host = host;
    context->localizer = anomaly::plugins::Localizer(host);
    context->config = Query<AnomalyConfigServiceV1>(
        host, ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    context->scheduler = Query<AnomalySchedulerServiceV1>(
        host, ANOMALY_SCHEDULER_SERVICE_V1_ID,
        ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    context->signature = Query<AnomalySignatureServiceV1>(
        host, ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    context->window = Query<AnomalyWindowServiceV1>(
        host, ANOMALY_WINDOW_SERVICE_V1_ID, ANOMALY_WINDOW_SERVICE_V1_VERSION);
    context->objects = Query<AnomalyUe5ObjectsServiceV1>(
        host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
        ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
    context->names = Query<AnomalyUe5NamesServiceV1>(
        host, ANOMALY_UE5_NAMES_SERVICE_V1_ID,
        ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    if (!ConfigReady(context->config) || !SchedulerReady(context->scheduler) ||
        !SignatureReady(context->signature) || !ObjectsReady(context->objects) ||
        !NamesReady(context->names)) {
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "required services are unavailable");
    }
    const AnomalyStatusV1 schema_status = context->config->register_schema(
        context->config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(kSettingsSchema), &context->settings_schema);
    if (schema_status.code != ANOMALY_STATUS_V1_OK ||
        context->settings_schema.id == 0 || !LoadSettings(*context)) {
        delete context;
        return Status(ANOMALY_STATUS_V1_FAILED, "Custom UID settings are invalid");
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* user) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || context->start_attempted) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin context is invalid");
    }
    context->start_attempted = true;

    static_cast<void>(EnsureWindow(*context));

    std::uintptr_t set_text{};
    std::uintptr_t assign_string{};
    std::uintptr_t from_string{};
    std::uintptr_t free_string{};
    std::uintptr_t text_to_string{};
    std::uintptr_t gobjects_accessor{};
    if (!Resolve(*context->signature, fake_uid_profile::kSetTextPattern, set_text) ||
        !Resolve(*context->signature, fake_uid_profile::kAssignStringPattern, assign_string) ||
        !ResolveRipRelative32(
            *context->signature, fake_uid_profile::kFromStringPattern,
            fake_uid_profile::kFromStringResolveOffset,
            fake_uid_profile::kFromStringInstructionSize, from_string) ||
        !Resolve(*context->signature, fake_uid_profile::kFreeStringPattern, free_string) ||
        !Resolve(*context->signature, fake_uid_profile::kTextToStringPattern, text_to_string) ||
        !Resolve(*context->signature, fake_uid_profile::kGObjectsPattern, gobjects_accessor)) {
        static_cast<void>(ReleaseWindow(*context));
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UID widget signatures are unavailable");
    }
    std::int32_t registry_displacement{};
    std::memcpy(
        &registry_displacement,
        reinterpret_cast<const void*>(
            gobjects_accessor + fake_uid_profile::kGObjectsResolveOffset),
        sizeof(registry_displacement));
    context->text_field_offset = fake_uid_profile::kTextFieldOffset;
    if (context->text_field_offset == 0 || context->text_field_offset > 4096) {
        static_cast<void>(ReleaseWindow(*context));
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UID widget layout is unavailable");
    }
    context->object_registry =
        gobjects_accessor + fake_uid_profile::kGObjectsInstructionSize +
        registry_displacement + fake_uid_profile::kGObjectsAddend;
    context->assign_string = reinterpret_cast<AssignStringFn>(assign_string);
    context->from_string = reinterpret_cast<FromStringFn>(from_string);
    context->free_string = reinterpret_cast<FreeStringFn>(free_string);
    context->text_to_string = reinterpret_cast<TextToStringFn>(text_to_string);
    context->set_text = reinterpret_cast<SetTextFn>(set_text);
    return anomaly::sdk::Ok();
}

void DrawText(const AnomalyUiServiceV1& ui, const std::string_view value) {
    ui.text(ui.user, anomaly::sdk::StringView(value));
}

bool Button(const AnomalyUiServiceV1& ui, const std::string_view label) {
    return ui.button(ui.user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0;
}

std::string LocalizeUiStatus(Context& context, const std::string_view status) {
    if (status == "UID must contain 1-256 digits") {
        return context.localizer.Text("status.invalid_uid", status);
    }
    if (status == "Applied, but save scheduling failed") {
        return context.localizer.Text("status.save_schedule_failed", status);
    }
    if (status == "Apply failed: internal error") {
        return context.localizer.Text("status.apply_failed", status);
    }
    return std::string(status);
}

void DrawEditor(Context& context, const AnomalyUiServiceV1& ui) {
    const std::uint32_t save_state =
        context.save_state.exchange(0U, std::memory_order_acq_rel);
    if (save_state == 2U) {
        context.ui_status = context.localizer.Text("status.applied_saved", "Applied and saved");
    }
    if (save_state == 3U) {
        context.ui_status = context.localizer.Text("status.save_failed", "Applied; save failed");
    }
    const std::uint64_t detected = context.detected_uid.load(std::memory_order_acquire);
    std::string detected_text;
    if (detected == 0) {
        detected_text = context.localizer.Text("current.detecting", "Current UID: detecting");
    } else {
        const std::string value = std::to_string(detected);
        const std::array arguments{std::string_view(value)};
        detected_text = context.localizer.Format("current.value", "Current UID: {0}", arguments);
    }
    DrawText(ui, detected_text);
    const std::string display_uid = context.localizer.Label(
        "field.display_uid", "Display UID", "display-uid");
    static_cast<void>(ui.input_text(
        ui.user, anomaly::sdk::StringView(display_uid),
        context.editor.data(), context.editor.size(), ANOMALY_UI_TEXT_INPUT_V1_DIGITS));

    const auto value = std::string_view(context.editor.data());
    const std::string apply = context.localizer.Label("action.apply", "Apply", "apply");
    if (Button(ui, apply)) {
        std::string error;
        const bool applied = ApplySettings(context, true, value, error);
        context.ui_status = applied
            ? (error.empty()
                    ? context.localizer.Text("status.applied_saving", "Applied; saving")
                    : LocalizeUiStatus(context, error))
            : LocalizeUiStatus(context, error);
    }
    const std::string revert = context.localizer.Label(
        "action.revert", "Revert to original", "revert-to-original");
    if (Button(ui, revert)) {
        if (detected == 0) {
            context.ui_status = context.localizer.Text(
                "status.original_unknown", "Original UID has not been detected yet");
        } else {
            const auto settings = ReadSettings(context);
            std::string error;
            const bool reverted = settings && ApplySettings(
                context, false, settings->display_uid, error);
            ResetEditor(context);
            context.ui_status = reverted
                ? (error.empty()
                        ? context.localizer.Text(
                              "status.original_restored", "Original UID restored; saving")
                        : LocalizeUiStatus(context, error))
                : (error.empty()
                        ? context.localizer.Text(
                              "status.original_restore_failed", "Original UID restore failed")
                        : LocalizeUiStatus(context, error));
        }
    }
    if (!context.ui_status.empty()) {
        DrawText(ui, context.ui_status);
    }
}

void ANOMALY_CALL Draw(void* user, const AnomalyUiServiceV1* ui) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || ui == nullptr || !EnsureWindow(*context)) {
        return;
    }
    AnomalyWindowStateV1 state{sizeof(state)};
    if (context->window->state(
            context->window->user, context->window_handle, &state).code !=
            ANOMALY_STATUS_V1_OK || state.open == 0) {
        return;
    }
    std::int32_t visible{};
    if (context->window->begin(
            context->window->user, context->window_handle, 0, &visible).code !=
            ANOMALY_STATUS_V1_OK) {
        return;
    }
    if (visible != 0 && ui->service_version == ANOMALY_UI_SERVICE_V1_VERSION &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_text)>(
            ui, offsetof(AnomalyUiServiceV1, input_text)) &&
        ui->input_text != nullptr) {
        DrawEditor(*context, *ui);
    }
    static_cast<void>(context->window->end(
        context->window->user, context->window_handle));
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* user, std::uint32_t) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin context is invalid");
    }
    if (context->stop_completed) return anomaly::sdk::Ok();
    const AnomalyStatusV1 window_status = ReleaseWindow(*context);
    if (window_status.code != ANOMALY_STATUS_V1_OK) return window_status;
    context->stop_completed = true;
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* user) {
    auto* const context = static_cast<Context*>(user);
    if (context != nullptr && Stop(context, 0).code == ANOMALY_STATUS_V1_OK) {
        delete context;
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte.fake-uid"),
        anomaly::sdk::StringView("Custom UID"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.1.4"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
