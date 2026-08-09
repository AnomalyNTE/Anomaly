#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/localization.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui_resources.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kProcessEventPattern =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 "
    "48 89 9D 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 C0 00 00 00 "
    "8B 41 08 4D 8B F0 C1 E8 1E 48 8B FA F6 D0 4C 8B F9 A8 01 0F 84 ?? ?? ?? "
    "?? 33 F6 F7 82 B0 00 00 00 00 04 00 00";
constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
constexpr std::string_view kPauseGameNativePattern =
    "88 54 24 10 55 53 57 48 8B EC 48 83 EC 60 0F B6 FA 48 8B D9 "
    "E8 ?? ?? ?? ?? 84 C0 75 14 48 8B CB E8 ?? ?? ?? ?? 84 C0 75 08 "
    "48 83 C4 60 5F 5B 5D C3";
constexpr std::string_view kResumeGameNativePattern =
    "88 54 24 10 55 53 56 57 41 56 48 8B EC 48 83 EC 50 0F B6 FA 48 8B D9 "
    "E8 ?? ?? ?? ?? 84 C0 75 10 48 8B CB E8 ?? ?? ?? ?? 84 C0 "
    "0F 84 ?? ?? ?? ??";
constexpr std::string_view kTimedPauseFunctionName = "PausedGameExcludePlayerByTimer";
constexpr std::string_view kTimedPauseFunctionOwner = "HTPlayerController";
constexpr std::string_view kTargetFunctionClass = "Function";
constexpr std::string_view kCurrentControllerName = "BP_PlayerControllerBase_C";

constexpr std::uint32_t kObservedTimedPauseFunctionIndex = 58927;
constexpr std::uint32_t kObservedControllerIndex = 291654;
constexpr std::uint32_t kScanBatchSize = 512;
constexpr std::uint32_t kControllerRescanDelayTicks = 60;
constexpr std::uint32_t kObjectClassOffset = 16;
constexpr std::uint32_t kObjectNameOffset = 24;
constexpr std::uint32_t kObjectOuterOffset = 32;
constexpr std::uint32_t kStructSuperOffset = 64;
constexpr std::uint32_t kFunctionNumParmsOffset = 180;
constexpr std::uint32_t kFunctionParmsSizeOffset = 182;
constexpr std::uint32_t kFunctionReturnValueOffset = 184;
constexpr std::uint32_t kObjectRegistryItemsOffset = 16;
constexpr std::uint32_t kObjectChunkSize = 65536;
constexpr std::uint32_t kObjectItemStride = 24;
constexpr std::uint32_t kObjectItemSerialOffset = 16;
constexpr std::uint32_t kGObjectsDisplacementOffset = 3;
constexpr std::uint32_t kGObjectsInstructionSize = 7;
constexpr std::int32_t kGObjectsAddend = -16;
constexpr std::uint8_t kTimeStopPauseReason = 4;
constexpr double kDefaultDurationSeconds = 5.0;
constexpr double kMinimumDurationSeconds = 0.1;
constexpr double kMaximumDurationSeconds = 3600.0;
constexpr std::string_view kTimedPauseHotkeyId = "timed-pause";
constexpr std::string_view kInfinitePauseToggleHotkeyId = "toggle-infinite-pause";
constexpr std::uint32_t kKnownHotkeyModifiers =
    ANOMALY_INPUT_MODIFIER_V1_SHIFT |
    ANOMALY_INPUT_MODIFIER_V1_CONTROL |
    ANOMALY_INPUT_MODIFIER_V1_ALT |
    ANOMALY_INPUT_MODIFIER_V1_SUPER;
constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 1024;
constexpr std::string_view kSettingsSchema = R"json(
{"type":"object","additionalProperties":false,"required":["durationSeconds"],"properties":{"durationSeconds":{"type":"number","minimum":0.1,"maximum":3600.0},"timedPauseHotkeyVirtualKey":{"type":"integer","minimum":0,"maximum":255},"timedPauseHotkeyModifiers":{"type":"integer","minimum":0,"maximum":15},"infinitePauseToggleHotkeyVirtualKey":{"type":"integer","minimum":0,"maximum":255},"infinitePauseToggleHotkeyModifiers":{"type":"integer","minimum":0,"maximum":15}}}
)json";

using ProcessEventFn = void(ANOMALY_CALL*)(void*, void*, void*);
using PauseReasonFn = bool(ANOMALY_CALL*)(void*, std::uint8_t);

enum class PendingAction : std::uint32_t {
    none,
    timed_pause,
    infinite_pause,
    resume,
};

enum class ControlState : std::uint32_t {
    starting,
    searching,
    ready,
    timed_pause_requested,
    infinite_pause_active,
    resumed,
    nothing_to_resume,
    target_invalid,
    receiver_invalid,
    native_call_failed,
    wrong_thread,
    stopped,
};

enum class HotkeyCaptureTarget : std::uint8_t {
    none,
    timed_pause,
    infinite_pause_toggle,
};

struct Context final {
    const AnomalyCoreServiceV1* core{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyWindowServiceV1* window{};
    const AnomalyInputServiceV1* input{};
    const AnomalyLocalizationServiceV1* localization{};
    const AnomalyConfigServiceV1* config{};
    AnomalyGenerationHandleV1 window_handle{};
    AnomalyGenerationHandleV1 timed_pause_hotkey{};
    AnomalyGenerationHandleV1 infinite_pause_toggle_hotkey{};
    AnomalyGenerationHandleV1 settings_schema{};
    AnomalyGenerationHandleV1 timed_pause_function_handle{};
    AnomalyGenerationHandleV1 controller_handle{};
    std::uintptr_t process_event_target{};
    std::uintptr_t object_registry{};
    PauseReasonFn pause_game{};
    PauseReasonFn resume_game{};
    std::uintptr_t timed_pause_function{};
    std::uintptr_t player_controller_class{};
    std::uintptr_t validated_controller{};
    std::uint64_t object_generation{};
    std::uint32_t function_scan_cursor{};
    std::uint32_t controller_scan_cursor{};
    std::uint32_t controller_rescan_delay_ticks{};
    bool function_hint_checked{};
    bool controller_hint_checked{};
    bool controller_scan_complete{};
    std::atomic<double> duration_seconds{kDefaultDurationSeconds};
    std::atomic<std::uint32_t> timed_pause_hotkey_virtual_key{};
    std::atomic<std::uint32_t> timed_pause_hotkey_modifiers{};
    std::atomic<std::uint32_t> infinite_pause_toggle_hotkey_virtual_key{};
    std::atomic<std::uint32_t> infinite_pause_toggle_hotkey_modifiers{};
    std::atomic_bool settings_dirty{};
    HotkeyCaptureTarget hotkey_capture{HotkeyCaptureTarget::none};
    std::array<std::uint8_t, 32> hotkey_capture_keys{};
    bool timed_pause_hotkey_registration_failed{};
    bool infinite_pause_toggle_hotkey_registration_failed{};
    std::atomic<std::uint32_t> pending_action{};
    std::atomic<std::uint32_t> state{static_cast<std::uint32_t>(ControlState::starting)};
    std::atomic_bool infinite_pause_active{};
    std::atomic_bool developer_window_visible{};
    std::atomic_bool stopping{true};
    std::mutex lifecycle_mutex;
    bool start_attempted{};
    bool stop_completed{};
};

constexpr AnomalyStatusV1 Status(
    const std::uint32_t code, const char* const message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

constexpr AnomalyStringViewV1 View(const std::string_view value) noexcept {
    return {value.data(), value.size()};
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

template <typename Table, typename Field>
bool HasField(const Table* const table, const std::size_t offset) noexcept {
    return table != nullptr && table->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service* Query(
    const AnomalyHostApiV1* const host, const char* const id,
    const std::uint32_t version = 1) noexcept {
    if (host == nullptr || host->query_service == nullptr) return nullptr;
    const void* service{};
    if (host->query_service(host->host_context, View(id), version, &service).code !=
        ANOMALY_STATUS_V1_OK) {
        return nullptr;
    }
    return static_cast<const Service*>(service);
}

bool CoreReady(const AnomalyCoreServiceV1* const service) noexcept {
    return HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::read_memory)>(
               service, offsetof(AnomalyCoreServiceV1, read_memory)) &&
        service->read_memory != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1* const service) noexcept {
    return HasField<AnomalySignatureServiceV1,
               decltype(AnomalySignatureServiceV1::resolve)>(
               service, offsetof(AnomalySignatureServiceV1, resolve)) &&
        service->resolve != nullptr;
}

bool FrameworkReady(const AnomalyUe5FrameworkServiceV1* const service) noexcept {
    return HasField<AnomalyUe5FrameworkServiceV1,
               decltype(AnomalyUe5FrameworkServiceV1::is_game_thread)>(
               service, offsetof(AnomalyUe5FrameworkServiceV1, is_game_thread)) &&
        service->is_game_thread != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* const service) noexcept {
    return HasField<AnomalyUe5NamesServiceV1,
               decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               service, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
        service->resolve_utf8 != nullptr;
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* const service) noexcept {
    return HasField<AnomalyUe5ObjectsServiceV1,
               decltype(AnomalyUe5ObjectsServiceV1::snapshot_by_handle)>(
               service, offsetof(AnomalyUe5ObjectsServiceV1, snapshot_by_handle)) &&
        service->generation != nullptr && service->count != nullptr &&
        service->snapshot_at != nullptr && service->snapshot_by_handle != nullptr;
}

bool WindowReady(const AnomalyWindowServiceV1* const service) noexcept {
    return HasField<AnomalyWindowServiceV1, decltype(AnomalyWindowServiceV1::end)>(
               service, offsetof(AnomalyWindowServiceV1, end)) &&
        service->register_window != nullptr && service->release_window != nullptr &&
        service->set_open != nullptr && service->state != nullptr &&
        service->begin != nullptr && service->end != nullptr;
}

bool InputReady(const AnomalyInputServiceV1* const service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_INPUT_SERVICE_V1_VERSION &&
        HasField<AnomalyInputServiceV1,
            decltype(AnomalyInputServiceV1::capture_state)>(
            service, offsetof(AnomalyInputServiceV1, capture_state)) &&
        service->snapshot != nullptr && service->was_pressed != nullptr &&
        service->register_hotkey != nullptr && service->release_hotkey != nullptr;
}

bool LocalizationReady(const AnomalyLocalizationServiceV1* const service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_LOCALIZATION_SERVICE_V1_VERSION &&
        HasField<AnomalyLocalizationServiceV1,
            decltype(AnomalyLocalizationServiceV1::translate)>(
            service, offsetof(AnomalyLocalizationServiceV1, translate)) &&
        service->locale != nullptr && service->translate != nullptr;
}

bool ConfigReady(const AnomalyConfigServiceV1* const service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_CONFIG_SERVICE_V1_VERSION &&
        HasField<AnomalyConfigServiceV1,
            decltype(AnomalyConfigServiceV1::write_atomic)>(
            service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
}

bool LoadSettings(Context& context) noexcept {
    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = context.config->read(
        context.config->user, View(kSettingsSchemaId), &schema_version,
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
        const AnomalyStatusV1 read_status = context.config->read(
            context.config->user, View(kSettingsSchemaId), &schema_version,
            {document.data(), document.size()}, &copied);
        if (read_status.code != ANOMALY_STATUS_V1_OK ||
            schema_version != kSettingsSchemaVersion || copied == 0 ||
            copied > document.size()) {
            return false;
        }
        const nlohmann::json settings = nlohmann::json::parse(
            document.begin(), document.begin() + copied, nullptr, false);
        if (settings.is_discarded() || !settings.is_object() ||
            !settings.contains("durationSeconds") ||
            !settings["durationSeconds"].is_number()) {
            return false;
        }
        const double duration = settings["durationSeconds"].get<double>();
        if (!std::isfinite(duration) || duration < kMinimumDurationSeconds ||
            duration > kMaximumDurationSeconds) {
            return false;
        }
        const auto optional_integer = [&](
            const char* const key, const std::uint32_t maximum,
            std::uint32_t& value) {
            if (!settings.contains(key)) return true;
            if (!settings[key].is_number_integer()) return false;
            const std::int64_t parsed = settings[key].get<std::int64_t>();
            if (parsed < 0 || parsed > maximum) return false;
            value = static_cast<std::uint32_t>(parsed);
            return true;
        };
        std::uint32_t timed_virtual_key{};
        std::uint32_t timed_modifiers{};
        std::uint32_t toggle_virtual_key{};
        std::uint32_t toggle_modifiers{};
        if (!optional_integer(
                "timedPauseHotkeyVirtualKey", 0xff, timed_virtual_key) ||
            !optional_integer(
                "timedPauseHotkeyModifiers", kKnownHotkeyModifiers,
                timed_modifiers) ||
            !optional_integer(
                "infinitePauseToggleHotkeyVirtualKey", 0xff,
                toggle_virtual_key) ||
            !optional_integer(
                "infinitePauseToggleHotkeyModifiers", kKnownHotkeyModifiers,
                toggle_modifiers) ||
            (timed_modifiers & ~kKnownHotkeyModifiers) != 0 ||
            (toggle_modifiers & ~kKnownHotkeyModifiers) != 0) {
            return false;
        }
        context.duration_seconds.store(duration, std::memory_order_release);
        context.timed_pause_hotkey_virtual_key.store(
            timed_virtual_key, std::memory_order_release);
        context.timed_pause_hotkey_modifiers.store(
            timed_modifiers, std::memory_order_release);
        context.infinite_pause_toggle_hotkey_virtual_key.store(
            toggle_virtual_key, std::memory_order_release);
        context.infinite_pause_toggle_hotkey_modifiers.store(
            toggle_modifiers, std::memory_order_release);
        context.settings_dirty.store(false, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveSettings(Context& context) noexcept {
    if (!context.settings_dirty.load(std::memory_order_acquire)) return true;
    try {
        const nlohmann::json settings{
            {"durationSeconds", context.duration_seconds.load(std::memory_order_acquire)},
            {"timedPauseHotkeyVirtualKey",
                context.timed_pause_hotkey_virtual_key.load(
                    std::memory_order_acquire)},
            {"timedPauseHotkeyModifiers",
                context.timed_pause_hotkey_modifiers.load(
                    std::memory_order_acquire)},
            {"infinitePauseToggleHotkeyVirtualKey",
                context.infinite_pause_toggle_hotkey_virtual_key.load(
                    std::memory_order_acquire)},
            {"infinitePauseToggleHotkeyModifiers",
                context.infinite_pause_toggle_hotkey_modifiers.load(
                    std::memory_order_acquire)}};
        const std::string document = settings.dump();
        const AnomalyStatusV1 status = context.config->write_atomic(
            context.config->user, View(kSettingsSchemaId), kSettingsSchemaVersion,
            Bytes(document));
        if (status.code != ANOMALY_STATUS_V1_OK) return false;
        context.settings_dirty.store(false, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

AnomalyStringViewV1 Localized(
    const Context& context, const std::string_view key,
    const std::string_view english_fallback, char* const buffer,
    const std::size_t capacity) noexcept {
    std::size_t size = capacity;
    if (LocalizationReady(context.localization) && buffer != nullptr && capacity != 0 &&
        context.localization->translate(
            context.localization->user, View(key), View(english_fallback), nullptr, 0,
            buffer, &size).code == ANOMALY_STATUS_V1_OK &&
        size != 0 && size <= capacity) {
        return {buffer, size - 1U};
    }
    return View(english_fallback);
}

template <typename Value>
bool Read(const Context& context, const std::uintptr_t address, Value& value) noexcept {
    AnomalyMutableByteSpanV1 bytes{
        reinterpret_cast<std::uint8_t*>(&value), sizeof(value)};
    return address != 0 && CoreReady(context.core) &&
        context.core->read_memory(context.core->user, address, bytes).code ==
            ANOMALY_STATUS_V1_OK;
}

void Log(Context& context, const std::uint32_t level, const std::string_view message) noexcept {
    if (context.core != nullptr &&
        HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::log)>(
            context.core, offsetof(AnomalyCoreServiceV1, log)) &&
        context.core->log != nullptr) {
        context.core->log(context.core->user, level, View(message));
    }
}

bool ResolvePattern(
    const Context& context, const std::string_view pattern,
    std::uintptr_t& address) noexcept {
    address = 0;
    return SignatureReady(context.signature) &&
        context.signature->resolve(
            context.signature->user, View("HTGame.exe"), View(".text"), View(pattern),
            &address).code == ANOMALY_STATUS_V1_OK &&
        address != 0;
}

bool ResolveName(
    const Context& context, const std::uint32_t name_id, std::string& value) {
    std::size_t size{};
    if (name_id == 0 || !NamesReady(context.names) ||
        context.names->resolve_utf8(context.names->user, name_id, nullptr, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > 1024) {
        return false;
    }
    value.assign(size, '\0');
    if (context.names->resolve_utf8(
            context.names->user, name_id, value.data(), &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size == 0 || size > value.size()) {
        return false;
    }
    const std::size_t terminator = value.find('\0');
    if (terminator == std::string::npos) return false;
    value.resize(terminator);
    return true;
}

bool RawName(
    const Context& context, const std::uintptr_t object, std::string& name) {
    std::uint32_t name_id{};
    return Read(context, object + kObjectNameOffset, name_id) &&
        ResolveName(context, name_id, name);
}

bool ResolveObject(
    const Context& context, const AnomalyGenerationHandleV1 handle,
    std::uintptr_t& object) noexcept {
    object = 0;
    if (context.object_registry == 0 || handle.id == 0) return false;
    const std::uint32_t encoded_index = static_cast<std::uint32_t>(handle.id);
    if (encoded_index == 0) return false;
    const std::uint32_t index = encoded_index - 1U;
    const std::uint32_t expected_serial = static_cast<std::uint32_t>(handle.id >> 32U);
    std::uintptr_t chunks{};
    std::uintptr_t chunk{};
    std::uintptr_t candidate{};
    std::uint32_t serial{};
    if (!Read(context, context.object_registry + kObjectRegistryItemsOffset, chunks) ||
        chunks == 0 ||
        !Read(context, chunks +
                static_cast<std::uintptr_t>(index / kObjectChunkSize) * sizeof(void*),
            chunk) ||
        chunk == 0) {
        return false;
    }
    const std::uintptr_t item = chunk +
        static_cast<std::uintptr_t>(index % kObjectChunkSize) * kObjectItemStride;
    if (!Read(context, item, candidate) || candidate == 0 ||
        !Read(context, item + kObjectItemSerialOffset, serial) ||
        serial != expected_serial) {
        return false;
    }
    object = candidate;
    return true;
}

bool SnapshotAt(
    Context& context, const std::uint32_t index,
    AnomalyUe5ObjectSnapshotV1& snapshot) noexcept {
    snapshot = {sizeof(snapshot)};
    return context.objects->snapshot_at(context.objects->user, index, &snapshot).code ==
        ANOMALY_STATUS_V1_OK;
}

bool ClassIsA(
    const Context& context, std::uintptr_t candidate_class,
    const std::uintptr_t expected_class) noexcept {
    for (std::uint32_t depth = 0; candidate_class != 0 && depth < 64; ++depth) {
        if (candidate_class == expected_class) return true;
        std::uintptr_t parent{};
        if (!Read(context, candidate_class + kStructSuperOffset, parent) ||
            parent == candidate_class) {
            return false;
        }
        candidate_class = parent;
    }
    return false;
}

bool ValidateTimedPauseFunction(
    Context& context, const AnomalyUe5ObjectSnapshotV1& snapshot) {
    std::string name;
    if (!ResolveName(context, snapshot.name_id, name) ||
        name != kTimedPauseFunctionName) {
        return false;
    }
    std::uintptr_t function{};
    std::uintptr_t class_object{};
    std::uintptr_t owner{};
    std::string class_name;
    std::string owner_name;
    std::uint8_t num_parms{};
    std::uint16_t parms_size{};
    std::uint16_t return_offset{};
    if (!ResolveObject(context, snapshot.handle, function) ||
        !Read(context, function + kObjectClassOffset, class_object) ||
        !Read(context, function + kObjectOuterOffset, owner) ||
        !RawName(context, class_object, class_name) ||
        !RawName(context, owner, owner_name) ||
        class_name != kTargetFunctionClass || owner_name != kTimedPauseFunctionOwner ||
        !Read(context, function + kFunctionNumParmsOffset, num_parms) ||
        !Read(context, function + kFunctionParmsSizeOffset, parms_size) ||
        !Read(context, function + kFunctionReturnValueOffset, return_offset) ||
        num_parms != 1 || parms_size != sizeof(float) ||
        return_offset != (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    context.timed_pause_function_handle = snapshot.handle;
    context.timed_pause_function = function;
    context.player_controller_class = owner;
    return true;
}

bool ValidateController(
    Context& context, const AnomalyUe5ObjectSnapshotV1& snapshot) {
    std::string name;
    if (!ResolveName(context, snapshot.name_id, name) ||
        name != kCurrentControllerName) {
        return false;
    }
    std::uintptr_t controller{};
    std::uintptr_t controller_class{};
    if (!ResolveObject(context, snapshot.handle, controller) ||
        !Read(context, controller + kObjectClassOffset, controller_class) ||
        !ClassIsA(context, controller_class, context.player_controller_class)) {
        return false;
    }
    context.controller_handle = snapshot.handle;
    context.validated_controller = controller;
    std::array<char, 128> message{};
    std::snprintf(
        message.data(), message.size(), "Time-stop controller ready at 0x%llX",
        static_cast<unsigned long long>(controller));
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message.data());
    return true;
}

bool FindTimedPauseFunction(Context& context, const std::uint32_t count) {
    if (context.timed_pause_function != 0) return true;
    if (!context.function_hint_checked) {
        context.function_hint_checked = true;
        AnomalyUe5ObjectSnapshotV1 snapshot{};
        if (kObservedTimedPauseFunctionIndex < count &&
            SnapshotAt(context, kObservedTimedPauseFunctionIndex, snapshot) &&
            ValidateTimedPauseFunction(context, snapshot)) {
            return true;
        }
    }
    const std::uint32_t end = (std::min)(
        count, context.function_scan_cursor + kScanBatchSize);
    for (; context.function_scan_cursor < end; ++context.function_scan_cursor) {
        AnomalyUe5ObjectSnapshotV1 snapshot{};
        if (SnapshotAt(context, context.function_scan_cursor, snapshot) &&
            ValidateTimedPauseFunction(context, snapshot)) {
            return true;
        }
    }
    return false;
}

bool FindController(Context& context, const std::uint32_t count) {
    if (context.validated_controller != 0) return true;
    if (context.player_controller_class == 0) return false;
    if (context.controller_scan_complete) {
        if (context.controller_rescan_delay_ticks != 0) {
            --context.controller_rescan_delay_ticks;
            return false;
        }
        context.controller_scan_cursor = count;
        context.controller_hint_checked = false;
        context.controller_scan_complete = false;
    }
    if (!context.controller_hint_checked) {
        context.controller_hint_checked = true;
        AnomalyUe5ObjectSnapshotV1 snapshot{};
        if (kObservedControllerIndex < count &&
            SnapshotAt(context, kObservedControllerIndex, snapshot) &&
            ValidateController(context, snapshot)) {
            return true;
        }
    }
    if (context.controller_scan_cursor == 0 || context.controller_scan_cursor > count) {
        context.controller_scan_cursor = count;
    }
    const std::uint32_t begin = context.controller_scan_cursor > kScanBatchSize
        ? context.controller_scan_cursor - kScanBatchSize
        : 0;
    for (std::uint32_t index = context.controller_scan_cursor; index-- > begin;) {
        AnomalyUe5ObjectSnapshotV1 snapshot{};
        if (SnapshotAt(context, index, snapshot) && ValidateController(context, snapshot)) {
            return true;
        }
    }
    context.controller_scan_cursor = begin;
    context.controller_scan_complete = begin == 0;
    if (context.controller_scan_complete) {
        context.controller_rescan_delay_ticks = kControllerRescanDelayTicks;
    }
    return false;
}

void RestartControllerDiscovery(
    Context& context, const std::uint32_t count) noexcept {
    context.controller_scan_cursor = count;
    context.controller_rescan_delay_ticks = 0;
    context.controller_hint_checked = false;
    context.controller_scan_complete = false;
    context.controller_handle = {};
    context.validated_controller = 0;
    context.infinite_pause_active.store(false, std::memory_order_release);
    context.state.store(
        static_cast<std::uint32_t>(ControlState::searching),
        std::memory_order_release);
}

void ResetDiscovery(
    Context& context, const std::uint64_t generation,
    const std::uint32_t count) noexcept {
    context.object_generation = generation;
    context.function_scan_cursor = 0;
    context.function_hint_checked = false;
    context.timed_pause_function_handle = {};
    context.timed_pause_function = 0;
    context.player_controller_class = 0;
    RestartControllerDiscovery(context, count);
}

bool ValidateCurrentHandle(
    Context& context, const AnomalyGenerationHandleV1 handle,
    const std::uintptr_t expected) noexcept {
    if (handle.id == 0 || expected == 0) return false;
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    std::uintptr_t resolved{};
    return context.objects->snapshot_by_handle(
               context.objects->user, handle, &snapshot).code == ANOMALY_STATUS_V1_OK &&
        ResolveObject(context, handle, resolved) && resolved == expected;
}

struct LocalizedText final {
    std::string_view key;
    std::string_view english_fallback;
};

LocalizedText StateText(const ControlState state) noexcept {
    switch (state) {
        case ControlState::starting: return {"state.starting", "Starting"};
        case ControlState::searching:
            return {"state.searching", "Searching for player controller"};
        case ControlState::ready: return {"state.ready", "Ready"};
        case ControlState::timed_pause_requested:
            return {"state.timed_pause_active", "Timed pause active"};
        case ControlState::infinite_pause_active:
            return {"state.infinite_pause_active", "Infinite pause active"};
        case ControlState::resumed: return {"state.resumed", "Pause released"};
        case ControlState::nothing_to_resume:
            return {"state.nothing_to_resume", "No matching pause to release"};
        case ControlState::target_invalid:
            return {"state.target_invalid", "Pause function became invalid"};
        case ControlState::receiver_invalid:
            return {"state.receiver_invalid", "Player controller became invalid"};
        case ControlState::native_call_failed:
            return {"state.native_call_failed", "Pause operation was rejected"};
        case ControlState::wrong_thread:
            return {"state.wrong_thread", "Operation was not on the Game Thread"};
        case ControlState::stopped: return {"state.stopped", "Stopped"};
    }
    return {"state.unknown", "Unknown"};
}

void ANOMALY_CALL TriggerTimedPause(
    void* const user, AnomalyGenerationHandleV1,
    const AnomalyInputSnapshotV1*) noexcept {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || context->stopping.load(std::memory_order_acquire) ||
        context->infinite_pause_active.load(std::memory_order_acquire)) {
        return;
    }
    context->pending_action.store(
        static_cast<std::uint32_t>(PendingAction::timed_pause),
        std::memory_order_release);
}

void ANOMALY_CALL ToggleInfinitePause(
    void* const user, AnomalyGenerationHandleV1,
    const AnomalyInputSnapshotV1*) noexcept {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || context->stopping.load(std::memory_order_acquire)) {
        return;
    }
    const PendingAction action =
        context->infinite_pause_active.load(std::memory_order_acquire)
        ? PendingAction::resume
        : PendingAction::infinite_pause;
    context->pending_action.store(
        static_cast<std::uint32_t>(action), std::memory_order_release);
}

bool ReleaseHotkey(
    Context& context, AnomalyGenerationHandleV1& handle) noexcept {
    if (handle.id == 0) return true;
    const AnomalyStatusV1 status = context.input->release_hotkey(
        context.input->user, handle);
    if (status.code != ANOMALY_STATUS_V1_OK &&
        status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        return false;
    }
    handle = {};
    return true;
}

bool RegisterHotkey(
    Context& context, AnomalyGenerationHandleV1& handle,
    const std::string_view id, const AnomalyHotkeyCallbackV1 callback,
    const std::uint32_t virtual_key, const std::uint32_t modifiers) noexcept {
    if (virtual_key == 0) return true;
    AnomalyHotkeySpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.modifiers = modifiers;
    spec.virtual_key = virtual_key;
    spec.flags = ANOMALY_HOTKEY_V1_NONE;
    spec.id = View(id);
    return context.input->register_hotkey(
               context.input->user, &spec, callback, &context, &handle).code ==
            ANOMALY_STATUS_V1_OK &&
        handle.id != 0;
}

bool RebindHotkey(
    Context& context, AnomalyGenerationHandleV1& handle,
    std::atomic<std::uint32_t>& current_virtual_key,
    std::atomic<std::uint32_t>& current_modifiers,
    const std::string_view id, const AnomalyHotkeyCallbackV1 callback,
    const std::uint32_t virtual_key, const std::uint32_t modifiers) noexcept {
    const std::uint32_t old_virtual_key =
        current_virtual_key.load(std::memory_order_acquire);
    const std::uint32_t old_modifiers =
        current_modifiers.load(std::memory_order_acquire);
    if (old_virtual_key == virtual_key && old_modifiers == modifiers) return true;
    if (!ReleaseHotkey(context, handle)) return false;
    if (!RegisterHotkey(
            context, handle, id, callback, virtual_key, modifiers)) {
        static_cast<void>(RegisterHotkey(
            context, handle, id, callback, old_virtual_key, old_modifiers));
        return false;
    }
    current_virtual_key.store(virtual_key, std::memory_order_release);
    current_modifiers.store(modifiers, std::memory_order_release);
    context.settings_dirty.store(true, std::memory_order_release);
    return true;
}

bool RegisterWindow(Context& context) noexcept {
    if (!WindowReady(context.window)) return false;
    AnomalyWindowSpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.flags = ANOMALY_WINDOW_V1_NO_COLLAPSE;
    spec.id = View("chronos");
    spec.title = View("Chronos");
    spec.initial_width = 390.0F;
    spec.initial_height = 360.0F;
    spec.minimum_width = 350.0F;
    spec.minimum_height = 300.0F;
    spec.maximum_width = 520.0F;
    spec.maximum_height = 480.0F;
    spec.default_open = 0;
    return context.window->register_window(
               context.window->user, &spec, &context.window_handle).code ==
            ANOMALY_STATUS_V1_OK &&
        context.window_handle.id != 0;
}

bool ReleaseWindow(Context& context) noexcept {
    if (context.window_handle.id == 0) return true;
    const AnomalyStatusV1 status = context.window->release_window(
        context.window->user, context.window_handle);
    if (status.code != ANOMALY_STATUS_V1_OK &&
        status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        return false;
    }
    context.window_handle = {};
    return true;
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* const host, void** const plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host is invalid");
    }
    *plugin_context = nullptr;
    auto* const context = new (std::nothrow) Context();
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_FAILED, "context allocation failed");
    }
    context->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID);
    context->signature = Query<AnomalySignatureServiceV1>(
        host, ANOMALY_SIGNATURE_SERVICE_V1_ID);
    context->framework = Query<AnomalyUe5FrameworkServiceV1>(
        host, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID);
    context->names = Query<AnomalyUe5NamesServiceV1>(
        host, ANOMALY_UE5_NAMES_SERVICE_V1_ID);
    context->objects = Query<AnomalyUe5ObjectsServiceV1>(
        host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
    context->window = Query<AnomalyWindowServiceV1>(host, ANOMALY_WINDOW_SERVICE_V1_ID);
    context->input = Query<AnomalyInputServiceV1>(host, ANOMALY_INPUT_SERVICE_V1_ID);
    context->localization = Query<AnomalyLocalizationServiceV1>(
        host, ANOMALY_LOCALIZATION_SERVICE_V1_ID);
    context->config = Query<AnomalyConfigServiceV1>(host, ANOMALY_CONFIG_SERVICE_V1_ID);
    if (!CoreReady(context->core) || !SignatureReady(context->signature) ||
        !FrameworkReady(context->framework) || !NamesReady(context->names) ||
        !ObjectsReady(context->objects) || !WindowReady(context->window) ||
        !InputReady(context->input) || !ConfigReady(context->config)) {
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "required services are unavailable");
    }
    const AnomalyStatusV1 schema_status = context->config->register_schema(
        context->config->user, View(kSettingsSchemaId), kSettingsSchemaVersion,
        Bytes(kSettingsSchema), &context->settings_schema);
    if (schema_status.code != ANOMALY_STATUS_V1_OK ||
        context->settings_schema.id == 0) {
        delete context;
        return schema_status.code == ANOMALY_STATUS_V1_OK
            ? Status(ANOMALY_STATUS_V1_FAILED, "settings schema registration failed")
            : schema_status;
    }
    if (!LoadSettings(*context)) {
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "Chronos settings could not be loaded; using defaults");
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* const user) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is invalid");
    }
    std::scoped_lock lock(context->lifecycle_mutex);
    if (context->start_attempted) {
        return Status(ANOMALY_STATUS_V1_CONFLICT, "control already started");
    }
    context->start_attempted = true;
    if (!RegisterWindow(*context)) {
        return Status(ANOMALY_STATUS_V1_FAILED, "control window could not be registered");
    }

    std::uintptr_t gobjects_match{};
    std::uintptr_t pause_game{};
    std::uintptr_t resume_game{};
    if (!ResolvePattern(*context, kProcessEventPattern, context->process_event_target) ||
        !ResolvePattern(*context, kGObjectsPattern, gobjects_match) ||
        !ResolvePattern(*context, kPauseGameNativePattern, pause_game) ||
        !ResolvePattern(*context, kResumeGameNativePattern, resume_game)) {
        static_cast<void>(ReleaseWindow(*context));
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "required signatures are unavailable");
    }
    std::int32_t displacement{};
    if (!Read(*context, gobjects_match + kGObjectsDisplacementOffset, displacement)) {
        static_cast<void>(ReleaseWindow(*context));
        return Status(ANOMALY_STATUS_V1_FAILED, "GObjects displacement is unreadable");
    }
    context->object_registry = gobjects_match + kGObjectsInstructionSize +
        displacement + kGObjectsAddend;
    context->pause_game = reinterpret_cast<PauseReasonFn>(pause_game);
    context->resume_game = reinterpret_cast<PauseReasonFn>(resume_game);
    context->stopping.store(false, std::memory_order_release);
    if (!RegisterHotkey(
            *context, context->timed_pause_hotkey, kTimedPauseHotkeyId,
            TriggerTimedPause,
            context->timed_pause_hotkey_virtual_key.load(
                std::memory_order_acquire),
            context->timed_pause_hotkey_modifiers.load(
                std::memory_order_acquire))) {
        context->timed_pause_hotkey_registration_failed = true;
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "Chronos timed-pause hotkey registration failed; choose another binding");
    }
    if (!RegisterHotkey(
            *context, context->infinite_pause_toggle_hotkey,
            kInfinitePauseToggleHotkeyId, ToggleInfinitePause,
            context->infinite_pause_toggle_hotkey_virtual_key.load(
                std::memory_order_acquire),
            context->infinite_pause_toggle_hotkey_modifiers.load(
                std::memory_order_acquire))) {
        context->infinite_pause_toggle_hotkey_registration_failed = true;
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "Chronos infinite-pause toggle hotkey registration failed; choose another binding");
    }
    context->state.store(
        static_cast<std::uint32_t>(ControlState::searching),
        std::memory_order_release);
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "NTE time-stop control ready");
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Update(void* const user, double) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || context->stopping.load(std::memory_order_acquire)) return;

    const std::uint64_t generation = context->objects->generation(context->objects->user);
    const std::uint32_t count = context->objects->count(context->objects->user);
    if (generation == 0 || count == 0) return;
    if (context->object_generation != generation) {
        ResetDiscovery(*context, generation, count);
    }
    if (context->validated_controller != 0 &&
        !ValidateCurrentHandle(
            *context, context->controller_handle,
            context->validated_controller)) {
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "Player controller became invalid; restarting discovery");
        RestartControllerDiscovery(*context, count);
    }
    const bool function_ready = FindTimedPauseFunction(*context, count);
    const bool controller_ready = FindController(*context, count);
    if (function_ready && controller_ready &&
        static_cast<ControlState>(context->state.load(std::memory_order_acquire)) ==
            ControlState::searching) {
        context->state.store(
            static_cast<std::uint32_t>(ControlState::ready),
            std::memory_order_release);
    }

    const auto action = static_cast<PendingAction>(
        context->pending_action.exchange(
            static_cast<std::uint32_t>(PendingAction::none),
            std::memory_order_acq_rel));
    if (action == PendingAction::none) return;
    if (context->framework->is_game_thread(context->framework->user) == 0) {
        context->state.store(
            static_cast<std::uint32_t>(ControlState::wrong_thread),
            std::memory_order_release);
        return;
    }
    if (!ValidateCurrentHandle(
            *context, context->controller_handle,
            context->validated_controller)) {
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "Player controller became invalid before action; restarting discovery");
        RestartControllerDiscovery(*context, count);
        return;
    }

    if (action == PendingAction::timed_pause) {
        if (!ValidateCurrentHandle(
                *context, context->timed_pause_function_handle,
                context->timed_pause_function)) {
            context->state.store(
                static_cast<std::uint32_t>(ControlState::target_invalid),
                std::memory_order_release);
            return;
        }
        const auto process_event = reinterpret_cast<ProcessEventFn>(
            context->process_event_target);
        const float duration = static_cast<float>(std::clamp(
            context->duration_seconds.load(std::memory_order_acquire),
            kMinimumDurationSeconds, kMaximumDurationSeconds));
        std::array<std::uint8_t, sizeof(duration)> parameters{};
        std::memcpy(parameters.data(), &duration, sizeof(duration));
        process_event(
            reinterpret_cast<void*>(context->validated_controller),
            reinterpret_cast<void*>(context->timed_pause_function),
            parameters.data());
        context->state.store(
            static_cast<std::uint32_t>(ControlState::timed_pause_requested),
            std::memory_order_release);
        std::array<char, 96> message{};
        std::snprintf(
            message.data(), message.size(), "Timed pause requested for %.3f seconds",
            duration);
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message.data());
        return;
    }

    if (action == PendingAction::infinite_pause) {
        bool expected{};
        if (!context->infinite_pause_active.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (context->pause_game == nullptr ||
            !context->pause_game(
                reinterpret_cast<void*>(context->validated_controller),
                kTimeStopPauseReason)) {
            context->infinite_pause_active.store(false, std::memory_order_release);
            context->state.store(
                static_cast<std::uint32_t>(ControlState::native_call_failed),
                std::memory_order_release);
            return;
        }
        context->state.store(
            static_cast<std::uint32_t>(ControlState::infinite_pause_active),
            std::memory_order_release);
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "Infinite pause requested");
        return;
    }

    context->infinite_pause_active.store(false, std::memory_order_release);
    const bool resumed = context->resume_game != nullptr &&
        context->resume_game(
            reinterpret_cast<void*>(context->validated_controller),
            kTimeStopPauseReason);
    context->state.store(
        static_cast<std::uint32_t>(
            resumed ? ControlState::resumed : ControlState::nothing_to_resume),
        std::memory_order_release);
    Log(*context,
        resumed ? ANOMALY_CORE_LOG_LEVEL_V1_INFO
                : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        resumed ? "Pause released" : "No matching pause reason was active");
}

void DrawText(const AnomalyUiServiceV1& ui, const AnomalyStringViewV1 text) noexcept {
    ui.text(ui.user, text);
}

bool Button(
    const AnomalyUiServiceV1& ui, const AnomalyStringViewV1 label,
    const bool enabled) noexcept {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button_enabled)>(
            &ui, offsetof(AnomalyUiServiceV1, button_enabled)) &&
        ui.button_enabled != nullptr) {
        return ui.button_enabled(ui.user, label, 0.0F, 0.0F, enabled ? 1 : 0) != 0;
    }
    return enabled && ui.button(ui.user, label, 0.0F, 0.0F) != 0;
}

bool IsModifierVirtualKey(const std::uint32_t virtual_key) noexcept {
    return virtual_key == 0x10 || virtual_key == 0x11 || virtual_key == 0x12 ||
        virtual_key == 0x5b || virtual_key == 0x5c ||
        (virtual_key >= 0xa0 && virtual_key <= 0xa5);
}

std::string VirtualKeyName(const std::uint32_t virtual_key) {
    if ((virtual_key >= '0' && virtual_key <= '9') ||
        (virtual_key >= 'A' && virtual_key <= 'Z')) {
        return std::string(1, static_cast<char>(virtual_key));
    }
    if (virtual_key >= 0x70 && virtual_key <= 0x87) {
        return "F" + std::to_string(virtual_key - 0x6f);
    }
    switch (virtual_key) {
        case 0x09: return "Tab";
        case 0x0d: return "Enter";
        case 0x20: return "Space";
        case 0x21: return "Page Up";
        case 0x22: return "Page Down";
        case 0x23: return "End";
        case 0x24: return "Home";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x2d: return "Insert";
        case 0x2e: return "Delete";
        default: return "VK " + std::to_string(virtual_key);
    }
}

std::string HotkeyName(
    const std::uint32_t virtual_key, const std::uint32_t modifiers) {
    std::string name;
    const auto append = [&name](const std::string_view part) {
        if (!name.empty()) name += '+';
        name.append(part);
    };
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_CONTROL) != 0) append("Ctrl");
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_SHIFT) != 0) append("Shift");
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_ALT) != 0) append("Alt");
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_SUPER) != 0) append("Win");
    append(VirtualKeyName(virtual_key));
    return name;
}

bool BeginHotkeyCapture(
    Context& context, const HotkeyCaptureTarget target) noexcept {
    AnomalyInputSnapshotV1 snapshot{sizeof(snapshot)};
    if (context.input->snapshot(context.input->user, &snapshot).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    std::copy(
        std::begin(snapshot.keys), std::end(snapshot.keys),
        context.hotkey_capture_keys.begin());
    context.hotkey_capture = target;
    return true;
}

void CaptureHotkey(Context& context) noexcept {
    if (context.hotkey_capture == HotkeyCaptureTarget::none) return;
    AnomalyInputSnapshotV1 snapshot{sizeof(snapshot)};
    if (context.input->snapshot(context.input->user, &snapshot).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }
    for (std::uint32_t virtual_key = 8; virtual_key <= 0xff; ++virtual_key) {
        const std::size_t byte = virtual_key / 8U;
        const std::uint8_t mask = static_cast<std::uint8_t>(1U << (virtual_key % 8U));
        const bool was_down = (context.hotkey_capture_keys[byte] & mask) != 0;
        const bool is_down = (snapshot.keys[byte] & mask) != 0;
        if (is_down) {
            context.hotkey_capture_keys[byte] |= mask;
        } else {
            context.hotkey_capture_keys[byte] &= static_cast<std::uint8_t>(~mask);
        }
        if (!is_down || was_down) continue;
        if (virtual_key == 0x1b) {
            context.hotkey_capture = HotkeyCaptureTarget::none;
            return;
        }
        if (IsModifierVirtualKey(virtual_key)) continue;

        const HotkeyCaptureTarget target = context.hotkey_capture;
        const std::uint32_t selected_key = virtual_key == 0x08 ? 0 : virtual_key;
        const std::uint32_t selected_modifiers =
            selected_key == 0 ? 0 : snapshot.modifiers & kKnownHotkeyModifiers;
        bool rebound{};
        if (target == HotkeyCaptureTarget::timed_pause) {
            rebound = RebindHotkey(
                context, context.timed_pause_hotkey,
                context.timed_pause_hotkey_virtual_key,
                context.timed_pause_hotkey_modifiers, kTimedPauseHotkeyId,
                TriggerTimedPause, selected_key, selected_modifiers);
            context.timed_pause_hotkey_registration_failed = !rebound;
        } else {
            rebound = RebindHotkey(
                context, context.infinite_pause_toggle_hotkey,
                context.infinite_pause_toggle_hotkey_virtual_key,
                context.infinite_pause_toggle_hotkey_modifiers,
                kInfinitePauseToggleHotkeyId, ToggleInfinitePause,
                selected_key, selected_modifiers);
            context.infinite_pause_toggle_hotkey_registration_failed = !rebound;
        }
        context.hotkey_capture = HotkeyCaptureTarget::none;
        return;
    }
}

void DrawHotkeyEditor(
    Context& context, const AnomalyUiServiceV1& ui,
    const HotkeyCaptureTarget target,
    const std::atomic<std::uint32_t>& virtual_key,
    const std::atomic<std::uint32_t>& modifiers,
    const std::string_view label_key, const std::string_view label_fallback,
    const std::string_view widget_id, const bool registration_failed) {
    std::array<char, 96> label_buffer{};
    DrawText(ui, Localized(
        context, label_key, label_fallback,
        label_buffer.data(), label_buffer.size()));

    std::string button_label;
    if (context.hotkey_capture == target) {
        std::array<char, 128> capture_buffer{};
        const AnomalyStringViewV1 capture = Localized(
            context, "control.hotkey_capture",
            "Press a key (Esc cancels, Backspace clears)",
            capture_buffer.data(), capture_buffer.size());
        button_label.assign(capture.data, capture.size);
    } else {
        const std::uint32_t key = virtual_key.load(std::memory_order_acquire);
        if (key == 0) {
            std::array<char, 48> unset_buffer{};
            const AnomalyStringViewV1 unset = Localized(
                context, "control.hotkey_unset", "Not set",
                unset_buffer.data(), unset_buffer.size());
            button_label.assign(unset.data, unset.size);
        } else {
            button_label = HotkeyName(
                key, modifiers.load(std::memory_order_acquire));
        }
    }
    button_label += "##";
    button_label.append(widget_id);
    if (Button(ui, View(button_label), true)) {
        static_cast<void>(BeginHotkeyCapture(context, target));
    }
    if (registration_failed) {
        std::array<char, 128> failure_buffer{};
        DrawText(ui, Localized(
            context, "control.hotkey_conflict",
            "This shortcut conflicts with another registered hotkey",
            failure_buffer.data(), failure_buffer.size()));
    }
}

void Queue(Context& context, const PendingAction action) noexcept {
    context.pending_action.store(
        static_cast<std::uint32_t>(action), std::memory_order_release);
}

void ANOMALY_CALL Draw(void* const user, const AnomalyUiServiceV1* const ui) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || ui == nullptr || context->window_handle.id == 0 ||
        ui->service_version < ANOMALY_UI_SERVICE_V1_VERSION || ui->text == nullptr ||
        ui->button == nullptr) {
        return;
    }
    const bool developer_mode =
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::developer_mode_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, developer_mode_enabled)) &&
        ui->developer_mode_enabled != nullptr &&
        ui->developer_mode_enabled(ui->user) != 0;
    const bool was_visible = context->developer_window_visible.exchange(
        developer_mode, std::memory_order_acq_rel);
    if (!developer_mode) {
        if (was_visible) {
            static_cast<void>(context->window->set_open(
                context->window->user, context->window_handle, 0));
        }
        return;
    }
    if (!was_visible) {
        static_cast<void>(context->window->set_open(
            context->window->user, context->window_handle, 1));
    }
    AnomalyWindowStateV1 window_state{sizeof(window_state)};
    if (context->window->state(
            context->window->user, context->window_handle, &window_state).code !=
            ANOMALY_STATUS_V1_OK ||
        window_state.open == 0) {
        return;
    }
    std::int32_t visible{};
    if (context->window->begin(
            context->window->user, context->window_handle, 0, &visible).code !=
            ANOMALY_STATUS_V1_OK) {
        return;
    }
    if (visible != 0) {
        CaptureHotkey(*context);
        const ControlState state = static_cast<ControlState>(
            context->state.load(std::memory_order_acquire));
        const LocalizedText state_text = StateText(state);
        std::array<char, 128> state_buffer{};
        DrawText(*ui, Localized(
            *context, state_text.key, state_text.english_fallback,
            state_buffer.data(), state_buffer.size()));
        double duration = context->duration_seconds.load(std::memory_order_acquire);
        std::array<char, 64> duration_label{};
        if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_double)>(
                ui, offsetof(AnomalyUiServiceV1, input_double)) &&
            ui->input_double != nullptr &&
            ui->input_double(
                ui->user,
                Localized(
                    *context, "control.duration", "Duration (seconds)",
                    duration_label.data(), duration_label.size()),
                &duration, 0.5, 5.0) != 0) {
            context->duration_seconds.store(
                std::clamp(
                    duration, kMinimumDurationSeconds, kMaximumDurationSeconds),
                std::memory_order_release);
            context->settings_dirty.store(true, std::memory_order_release);
        }
        if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::separator)>(
                ui, offsetof(AnomalyUiServiceV1, separator)) &&
            ui->separator != nullptr) {
            ui->separator(ui->user);
        }
        DrawHotkeyEditor(
            *context, *ui, HotkeyCaptureTarget::timed_pause,
            context->timed_pause_hotkey_virtual_key,
            context->timed_pause_hotkey_modifiers,
            "control.timed_pause_hotkey", "Timed pause hotkey",
            "timed-pause-hotkey",
            context->timed_pause_hotkey_registration_failed);
        DrawHotkeyEditor(
            *context, *ui, HotkeyCaptureTarget::infinite_pause_toggle,
            context->infinite_pause_toggle_hotkey_virtual_key,
            context->infinite_pause_toggle_hotkey_modifiers,
            "control.infinite_pause_toggle_hotkey",
            "Infinite pause / resume hotkey", "infinite-pause-toggle-hotkey",
            context->infinite_pause_toggle_hotkey_registration_failed);
        if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::separator)>(
                ui, offsetof(AnomalyUiServiceV1, separator)) &&
            ui->separator != nullptr) {
            ui->separator(ui->user);
        }
        const bool ready = context->timed_pause_function != 0 &&
            context->validated_controller != 0;
        const bool infinite_active =
            context->infinite_pause_active.load(std::memory_order_acquire);
        std::array<char, 64> timed_pause_label{};
        if (Button(*ui,
                Localized(
                    *context, "control.timed_pause", "Timed pause",
                    timed_pause_label.data(), timed_pause_label.size()),
                ready && !infinite_active)) {
            Queue(*context, PendingAction::timed_pause);
        }
        std::array<char, 64> infinite_pause_label{};
        if (Button(*ui,
                Localized(
                    *context, "control.infinite_pause", "Infinite pause",
                    infinite_pause_label.data(), infinite_pause_label.size()),
                ready && !infinite_active)) {
            Queue(*context, PendingAction::infinite_pause);
        }
        std::array<char, 64> resume_label{};
        if (Button(*ui,
                Localized(
                    *context, "control.resume", "Resume",
                    resume_label.data(), resume_label.size()),
                ready)) {
            Queue(*context, PendingAction::resume);
        }
    }
    static_cast<void>(context->window->end(
        context->window->user, context->window_handle));
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* const user, std::uint32_t) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is invalid");
    }
    std::scoped_lock lock(context->lifecycle_mutex);
    if (context->stop_completed) return anomaly::sdk::Ok();
    context->stopping.store(true, std::memory_order_release);
    if (!SaveSettings(*context)) {
        return Status(ANOMALY_STATUS_V1_FAILED, "Chronos settings could not be saved");
    }
    const bool timed_hotkey_released =
        ReleaseHotkey(*context, context->timed_pause_hotkey);
    const bool toggle_hotkey_released =
        ReleaseHotkey(*context, context->infinite_pause_toggle_hotkey);
    if (!timed_hotkey_released || !toggle_hotkey_released) {
        return Status(ANOMALY_STATUS_V1_FAILED, "Chronos hotkeys did not release");
    }
    if (!ReleaseWindow(*context)) {
        return Status(ANOMALY_STATUS_V1_FAILED, "control window did not release");
    }
    context->state.store(
        static_cast<std::uint32_t>(ControlState::stopped),
        std::memory_order_release);
    context->stop_completed = true;
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* const user) {
    auto* const context = static_cast<Context*>(user);
    if (context != nullptr && Stop(context, 0).code == ANOMALY_STATUS_V1_OK) {
        delete context;
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* const descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        View("anomaly.diagnostics.nte.jin-ultimate-ability-probe"),
        View("Chronos"), View("Anomaly"), View("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
