#include "anomaly/sdk/cpp.hpp"
#include "fake_uid_profile.hpp"
#include "plugins/common/localization.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kDefaultDisplayUid = "000000000000";
// UE's empty FText is not accepted consistently by every cooked
// UTextBlock/Slate path. A zero-width separator keeps the FText valid while
// producing no visible glyph, so hiding the label does not depend on the
// widget accepting a null/empty text payload.
constexpr std::wstring_view kHiddenPrefixText = L"\u200B";
constexpr std::size_t kMaximumUidCharacters = 256;
constexpr std::size_t kMaximumUidUtf8Bytes = kMaximumUidCharacters * 4;
constexpr std::string_view kSettingsSchemaId = "fake-uid-settings-v2";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 4096;
constexpr std::uint32_t kObjectBatchSize = 128;
constexpr std::uint32_t kNeighborhoodObjectBatchSize = 512;
constexpr std::uint32_t kRoleIdNeighborhoodRadius = 8192;
// NTE can retain prior RoleID UI layers while a newer one is visible.
constexpr std::size_t kMaximumTrackedWidgets = 64;
// This is a callback-count interval, not milliseconds. The host currently
// ticks this plugin at roughly 100 ms, so 30 callbacks is about 3 seconds.
// Discovery must keep retrying while HUD widgets are created asynchronously.
constexpr std::uint64_t kObjectRescanInterval = 30;
constexpr std::uint64_t kRuntimeBindingRetryInterval = 300;
constexpr std::uint64_t kAnchorVerifyInterval = 5;
// A widget can enter the object registry one update before its FText/Slate
// state is writable. Retry transient write failures on the next game update
// instead of leaving the original UID visible for the normal rescan interval.
constexpr std::uint64_t kWidgetApplyRetryInterval = 1;
constexpr std::string_view kTargetWidgetName = "TextBlock_RoleID";
constexpr std::string_view kTargetPrefixWidgetName = "TextBlock_90";
constexpr std::string_view kTargetWidgetTemplatePath =
    "/Game/UI/Blueprints/Common/BPUI_RoleID.BPUI_RoleID_C:WidgetTree.TextBlock_RoleID";
constexpr std::string_view kTargetPrefixWidgetTemplatePath =
    "/Game/UI/Blueprints/Common/BPUI_RoleID.BPUI_RoleID_C:WidgetTree.TextBlock_90";
constexpr std::string_view kTextBlockSetTextPath = "/Script/UMG.TextBlock:SetText";
constexpr std::string_view kStringToTextPath =
    "/Script/Engine.KismetTextLibrary:Conv_StringToText";
constexpr std::string_view kKismetTextLibraryCdoPath =
    "/Script/Engine.Default__KismetTextLibrary";
constexpr std::uint32_t kNamedObjectBatchSize = 1024;

constexpr std::string_view kSettingsSchema = R"json(
{
  "type":"object",
  "additionalProperties":false,
  "required":["enabled","displayUid"],
  "properties":{
    "enabled":{"type":"boolean"},
    "hidePrefix":{"type":"boolean"},
    "displayUid":{"type":"string","minLength":1,"maxLength":1024},
    "detectedUid":{"type":"string","pattern":"^[0-9]{1,20}$"},
    "prefixNameId":{"type":"integer","minimum":1,"maximum":4294967295}
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

struct SetTextHookSnapshot final {
    std::uint32_t target_name_id{};
    std::uint32_t prefix_name_id{};
    std::uintptr_t roleid_outer{};
    std::uintptr_t roleid_panel{};
};

using FreeStringFn = void(ANOMALY_CALL*)(void* allocation);
using AssignStringFn = UnrealString*(ANOMALY_CALL*)(
    UnrealString* destination, const wchar_t* source);
using TextToStringFn = UnrealString*(ANOMALY_CALL*)(UnrealString*, const UnrealText*);
using SetTextFn = void(ANOMALY_CALL*)(void* widget, const UnrealText* text);
using ProcessEventFn = void(ANOMALY_CALL*)(void* object, void* function, void* parameters);

struct SettingsSnapshot final {
    bool enabled{true};
    bool hide_prefix{true};
    std::string display_uid;
    std::wstring display_wide;
};

struct TrackedWidget final {
    AnomalyGenerationHandleV1 handle{};
    bool prefix{};
    std::uint64_t applied_revision{};
    std::uint64_t retry_tick{};
};

enum class ApplyResult : std::uint8_t {
    Failed,
    Deferred,
    Applied
};

struct Context final {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyConfigServiceV1* config{};
    const AnomalyCoreServiceV1* core{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyHookServiceV1* hook{};
    const AnomalyWindowServiceV1* window{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5NamesServiceV1* names{};
    AnomalyGenerationHandleV1 settings_schema{};
    AnomalyGenerationHandleV1 window_handle{};
    std::atomic<std::shared_ptr<const SettingsSnapshot>> settings;
    std::array<char, kMaximumUidUtf8Bytes + 1> editor{};
    std::string ui_status;
    std::atomic<std::uint64_t> detected_uid{};
    std::atomic<std::uint32_t> save_state{};
    std::atomic<std::uint64_t> settings_revision{1};
    // Runtime writes are one-shot and user-triggered. Loading persisted
    // settings never arms this flag, so startup and later HUD/BigMap rebuilds
    // cannot mutate UMG behind the user's back.
    std::atomic_bool apply_requested{false};
    std::atomic_bool rescan_requested{false};
    std::wstring original_prefix{L"UID\uFF1A"};
    std::uint64_t update_tick{};
    std::uint64_t next_runtime_binding_tick{};
    std::uint64_t object_generation{};
    std::uint32_t object_cursor{};
    std::uint32_t scan_start{};
    std::uint32_t scan_count{};
    std::uint32_t scan_batch_size{kObjectBatchSize};
    std::uint32_t target_name_id{};
    std::uint32_t target_prefix_name_id{};
    std::atomic<std::uint32_t> persisted_prefix_name_id{};
    AnomalyGenerationHandleV1 target_template_handle{};
    AnomalyGenerationHandleV1 prefix_template_handle{};
    std::uintptr_t roleid_outer{};
    std::uintptr_t roleid_panel{};
    std::uint32_t roleid_anchor_index{};
    std::uint32_t recovery_anchor_index{};
    bool scan_active{};
    bool neighborhood_scan_active{};
    bool named_scan_active{};
    bool bootstrap_scan_started{};
    std::unordered_set<std::uint32_t> rejected_name_ids;
    std::array<TrackedWidget, kMaximumTrackedWidgets> widgets{};
    std::size_t widget_count{};
    std::uintptr_t object_registry{};
    std::uint32_t text_field_offset{};
    struct RuntimeBindings final {
        std::uintptr_t set_text{};
        std::uintptr_t assign_string{};
        std::uintptr_t free_string{};
        std::uintptr_t text_to_string{};
        std::uintptr_t gobjects_accessor{};
    std::uintptr_t process_event{};
    } runtime_bindings;
    std::string runtime_missing_bindings;
    FreeStringFn free_string{};
    AssignStringFn assign_string{};
    TextToStringFn text_to_string{};
    SetTextFn set_text{};
    ProcessEventFn process_event{};
    std::uintptr_t text_block_set_text_function{};
    std::uintptr_t string_to_text_function{};
    std::uintptr_t kismet_text_library_cdo{};
    std::uint64_t text_write_generation{};
    std::uint64_t next_text_write_binding_tick{};
    AnomalyGenerationHandleV1 set_text_hook{};
    std::uintptr_t set_text_original{};
    std::uintptr_t set_text_target{};
    std::uint64_t text_override_revision{};
    // Value-widget FName learned by the native SetText hook while the
    // template-derived name is still unarmed. The RoleID HUD is built by the
    // game thread at an arbitrary moment, and Update resolves the template
    // path only on its own tick; a write that wins that race used to be
    // forwarded verbatim, which is the raw UID the player sees. FName indexes
    // are stable for the whole process session, so the hint also survives
    // object generation resets.
    std::atomic<std::uint32_t> hook_value_name_id{};
    std::atomic<std::shared_ptr<const SetTextHookSnapshot>> text_override;
    bool text_write_binding_diagnostic_emitted{};
    std::uintptr_t set_visibility_function{};
    std::uint64_t set_visibility_generation{};
    std::uint64_t next_visibility_binding_tick{};
    std::uint32_t visibility_failure_stage{};
    std::uint32_t visibility_failure_status{};
    std::uintptr_t slot_set_position_function{};
    std::uintptr_t slot_get_position_function{};
    std::uint64_t slot_set_position_generation{};
    std::uint64_t next_slot_binding_tick{};
    bool slot_moved_diagnostic_emitted{};
    bool slot_binding_failed_emitted{};
    bool visibility_wait_diagnostic_emitted{};
    bool prefix_track_diagnostic_emitted{};
    bool prefix_apply_attempt_diagnostic_emitted{};
    std::uint32_t prefix_apply_failure_stage{};
    bool prefix_cleared_diagnostic_emitted{};
    bool prefix_visibility_pending_emitted{};
    bool prefix_visibility_failed_emitted{};
    bool prefix_visibility_diagnostic_emitted{};
    bool target_names_armed_diagnostic_emitted{};
    bool value_track_diagnostic_emitted{};
    std::uint64_t value_apply_failure_revision{};
    std::uint32_t value_apply_failure_stage{};
    std::uint64_t value_apply_success_revision{};
    bool start_attempted{};
    bool runtime_ready{};
    bool runtime_pending_emitted{};
    bool stop_completed{};
};

std::atomic<Context*> g_active_context{nullptr};
std::atomic<std::uintptr_t> g_set_text_original{0};
std::atomic<const AnomalyHookServiceV1*> g_set_text_hook_api{nullptr};
std::atomic<std::uint64_t> g_set_text_hook_id{0};
std::atomic<std::uint64_t> g_set_text_hook_generation{0};

std::uintptr_t ReadObjectOuter(const std::uintptr_t object) noexcept;
std::uintptr_t ReadWidgetPanel(const std::uintptr_t widget) noexcept;
bool IsRoleIdPrefixInstance(
    const Context& context, const std::uintptr_t widget) noexcept;
bool ResolveName(
    const AnomalyUe5NamesServiceV1& names, const std::uint32_t name_id,
    std::string& value);
// FName comparison indexes are per-session values: an index persisted in a
// previous game session points at an unrelated name after a restart. The
// persisted prefix index may only be reused when it still resolves to the
// TextBlock_90 family, otherwise it must be dropped and re-discovered.
bool PrefixNameIdPlausible(
    const AnomalyUe5NamesServiceV1& names, const std::uint32_t name_id) {
    if (name_id == 0) return false;
    std::string resolved;
    if (!ResolveName(names, name_id, resolved)) return false;
    return resolved == kTargetPrefixWidgetName ||
        resolved.starts_with("TextBlock_90_");
}

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

bool ObjectFindReady(const AnomalyUe5ObjectsServiceV1* service) noexcept {
    return HasField<AnomalyUe5ObjectsServiceV1,
               decltype(AnomalyUe5ObjectsServiceV1::find_exact)>(
               service, offsetof(AnomalyUe5ObjectsServiceV1, find_exact)) &&
        service->find_exact != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* service) noexcept {
    return HasField<AnomalyUe5NamesServiceV1,
               decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               service, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
        service->resolve_utf8 != nullptr;
}

bool HookReady(const AnomalyHookServiceV1* service) noexcept {
    return HasField<AnomalyHookServiceV1,
                    decltype(AnomalyHookServiceV1::end_callback)>(
               service, offsetof(AnomalyHookServiceV1, end_callback)) &&
        service->create != nullptr && service->release != nullptr &&
        service->begin_callback != nullptr && service->end_callback != nullptr;
}

bool WindowReady(const AnomalyWindowServiceV1* service) noexcept {
    return HasField<AnomalyWindowServiceV1, decltype(AnomalyWindowServiceV1::end)>(
               service, offsetof(AnomalyWindowServiceV1, end)) &&
        service->register_window != nullptr && service->release_window != nullptr &&
        service->set_open != nullptr && service->state != nullptr &&
        service->begin != nullptr && service->end != nullptr;
}

bool DecodeDisplayUid(
    const std::string_view value, std::wstring& wide) noexcept {
    wide.clear();
    if (value.empty() || value.size() > kMaximumUidUtf8Bytes ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int byte_count = static_cast<int>(value.size());
    const int wide_count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), byte_count, nullptr, 0);
    if (wide_count <= 0 ||
        wide_count > static_cast<int>(kMaximumUidCharacters * 2)) {
        return false;
    }
    std::wstring decoded(static_cast<std::size_t>(wide_count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), byte_count,
            decoded.data(), wide_count) != wide_count) {
        return false;
    }

    std::size_t characters{};
    for (std::size_t index = 0; index < decoded.size();) {
        std::uint32_t codepoint = static_cast<std::uint16_t>(decoded[index++]);
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
            if (index >= decoded.size()) return false;
            const std::uint32_t low = static_cast<std::uint16_t>(decoded[index++]);
            if (low < 0xDC00U || low > 0xDFFFU) return false;
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                (low - 0xDC00U);
        } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
            return false;
        }
        if (codepoint < 0x20U || (codepoint >= 0x7FU && codepoint <= 0x9FU) ||
            codepoint == 0x2028U || codepoint == 0x2029U) {
            return false;
        }
        if (++characters > kMaximumUidCharacters) return false;
    }
    if (characters == 0) return false;
    wide = std::move(decoded);
    return true;
}

std::uint64_t ParseUid(std::string_view value) noexcept;

std::shared_ptr<const SettingsSnapshot> MakeSettings(
    const bool enabled, const bool hide_prefix,
    const std::string_view display_uid) {
    std::wstring display_wide;
    if (!DecodeDisplayUid(display_uid, display_wide)) return {};
    auto settings = std::make_shared<SettingsSnapshot>();
    settings->enabled = enabled;
    settings->hide_prefix = hide_prefix;
    settings->display_uid.assign(display_uid);
    settings->display_wide = std::move(display_wide);
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
    const std::size_t count =
        (std::min)(settings->display_uid.size(), context.editor.size() - 1);
    std::copy_n(settings->display_uid.data(), count, context.editor.data());
}

bool PersistSettings(
    Context& context, const SettingsSnapshot& settings,
    std::string* const error = nullptr) noexcept {
    try {
        nlohmann::json json{
            {"enabled", settings.enabled},
            {"hidePrefix", settings.hide_prefix},
            {"displayUid", settings.display_uid}};
        const std::uint64_t detected =
            context.detected_uid.load(std::memory_order_acquire);
        if (detected != 0) json["detectedUid"] = std::to_string(detected);
        const std::uint32_t prefix_name_id =
            context.persisted_prefix_name_id.load(std::memory_order_acquire);
        if (prefix_name_id != 0) json["prefixNameId"] = prefix_name_id;
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
    Context& context, const bool enabled, const bool hide_prefix,
    const std::string_view display_uid,
    std::string& error) noexcept {
    try {
        const auto settings = MakeSettings(enabled, hide_prefix, display_uid);
        if (!settings) {
            error = "UID must contain 1-256 single-line Unicode characters";
            return false;
        }
        PublishSettings(context, settings);
        context.settings_revision.fetch_add(1, std::memory_order_acq_rel);
        context.apply_requested.store(true, std::memory_order_release);
        context.rescan_requested.store(true, std::memory_order_release);
        // Already tracked widgets may react immediately to this revision.
        // If an earlier generation left the prefix empty and it has not yet
        // been rediscovered, re-scan only the current RoleID neighborhood.
        bool prefix_tracked = false;
        for (std::size_t index = 0; index < context.widget_count; ++index) {
            prefix_tracked = prefix_tracked || context.widgets[index].prefix;
            context.widgets[index].retry_tick = context.update_tick;
        }
        if (!prefix_tracked && context.roleid_anchor_index != 0) {
            context.recovery_anchor_index = context.roleid_anchor_index;
        }
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
            const auto defaults = MakeSettings(true, true, kDefaultDisplayUid);
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
        if (!json.is_object() || json.size() < 2 || json.size() > 5 ||
            !json.contains("enabled") || !json.at("enabled").is_boolean() ||
            (json.contains("hidePrefix") && !json.at("hidePrefix").is_boolean()) ||
            !json.contains("displayUid") || !json.at("displayUid").is_string() ||
            (json.contains("detectedUid") && !json.at("detectedUid").is_string()) ||
            (json.contains("prefixNameId") && !json.at("prefixNameId").is_number_unsigned())) {
            return false;
        }
        const auto settings = MakeSettings(
            json.at("enabled").get<bool>(),
            json.value("hidePrefix", true),
            json.at("displayUid").get_ref<const std::string&>());
        if (!settings) return false;
        if (json.contains("detectedUid")) {
            const std::uint64_t detected = ParseUid(
                json.at("detectedUid").get_ref<const std::string&>());
            if (detected == 0) return false;
            context.detected_uid.store(detected, std::memory_order_release);
        }
        if (json.contains("prefixNameId")) {
            const std::uint64_t prefix_name_id = json.at("prefixNameId").get<std::uint64_t>();
            if (prefix_name_id == 0 || prefix_name_id > 0xFFFFFFFFULL) return false;
            // FName indexes are per-session: a value persisted by a previous
            // session is only usable when it still resolves to the prefix
            // TextBlock family, otherwise it would filter unrelated text.
            const std::uint32_t candidate =
                static_cast<std::uint32_t>(prefix_name_id);
            if (context.names != nullptr &&
                PrefixNameIdPlausible(*context.names, candidate)) {
                context.persisted_prefix_name_id.store(
                    candidate, std::memory_order_release);
            }
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

bool BuildValueReplacement(
    const UnrealString* const input, const std::wstring_view target_uid,
    std::wstring& replacement, std::uint64_t& detected_uid, bool& changed) {
    if (input == nullptr || input->data == nullptr || input->count <= 1 ||
        input->count > 2048 || input->capacity < input->count ||
        input->data[input->count - 1] != L'\0') {
        return false;
    }
    const std::wstring_view text(
        input->data, static_cast<std::size_t>(input->count - 1));
    detected_uid = 0;
    const bool numeric = std::all_of(
        text.begin(), text.end(),
        [](const wchar_t character) { return character >= L'0' && character <= L'9'; });
    if (numeric && !text.empty() && text.size() <= 20U) {
        for (const wchar_t character : text) {
            const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
            if (detected_uid > (UINT64_MAX - digit) / 10U) {
                detected_uid = 0;
                break;
            }
            detected_uid = detected_uid * 10U + digit;
        }
    }
    // TextBlock_RoleID is the value widget; the localized UID prefix lives in
    // the separate TextBlock_90 sibling. Always replace the complete value so
    // non-ASCII text and punctuation can never be misclassified as a suffix
    // and appended again on the next revision/verification pass.
    replacement.assign(target_uid);
    changed = replacement != text;
    return replacement.size() + 1 <= 2048;
}

bool CoreReady(const AnomalyCoreServiceV1* service) noexcept {
    return HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::log)>(
               service, offsetof(AnomalyCoreServiceV1, log)) && service->log != nullptr;
}

void Log(Context& context, const std::uint32_t level, const std::string_view message) noexcept {
    if (!CoreReady(context.core)) return;
    context.core->log(context.core->user, level, anomaly::sdk::StringView(message));
}

void LogValueApplyFailure(
    Context& context, const std::uint64_t revision, const std::uint32_t stage,
    const std::string_view message) noexcept {
    if (context.value_apply_failure_revision == revision &&
        context.value_apply_failure_stage == stage) {
        return;
    }
    context.value_apply_failure_revision = revision;
    context.value_apply_failure_stage = stage;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        "FakeUID: RoleID value apply deferred revision=" + std::to_string(revision) +
            " stage=" + std::to_string(stage) + " (" + std::string(message) + ")");
}

void LogValueApplySuccess(Context& context, const std::uint64_t revision) noexcept {
    context.value_apply_failure_revision = revision;
    context.value_apply_failure_stage = 0;
    if (context.value_apply_success_revision == revision) return;
    context.value_apply_success_revision = revision;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "FakeUID: RoleID value applied and read back revision=" +
            std::to_string(revision));
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

bool ResolveTextBlockAddress(
    const Context& context, const AnomalyGenerationHandleV1 handle,
    std::uintptr_t& widget) noexcept;
bool ResolveWidgetAddress(
    const Context& context, const AnomalyGenerationHandleV1 handle,
    std::uintptr_t& widget) noexcept;
bool ReadWidgetText(
    Context& context, const std::uintptr_t widget, std::wstring& text) noexcept;
bool ReadUnrealText(
    Context& context, const UnrealText* value, std::wstring& text) noexcept;
bool BuildUnrealText(
    Context& context, const std::wstring_view value, UnrealText& result) noexcept;
bool IsUsableUnrealText(
    const UnrealText& value, bool allow_empty = false) noexcept;
void ReleaseUnrealString(Context& context, UnrealString& value) noexcept;
bool SetNativeFunctionFlag(
    std::uintptr_t function, std::uint32_t& previous_flags) noexcept;
bool RestoreFunctionFlags(
    std::uintptr_t function, std::uint32_t previous_flags) noexcept;
bool IsRoleIdValueObject(
    const Context& context, std::uintptr_t object,
    std::uint32_t& name_id) noexcept;
bool ReadRoleIdObjectNameId(
    const Context& context, std::uintptr_t object,
    std::uint32_t& name_id) noexcept;
bool LooksLikeUidPrefix(const std::wstring_view text) noexcept;
bool InvokeProcessEvent(
    Context& context, std::uintptr_t object, std::uintptr_t function,
    void* parameters) noexcept;
bool ResolveTextWriteBindings(Context& context) noexcept;
bool EnsureTextHookSnapshot(
    Context& context, const SettingsSnapshot& settings,
    std::uint64_t revision) noexcept;
bool IsHookTargetWidget(
    const Context& context, std::uintptr_t widget,
    const SetTextHookSnapshot& override) noexcept;
bool IsHookPrefixWidget(
    const Context& context, std::uintptr_t widget,
    const SetTextHookSnapshot& override) noexcept;
void ANOMALY_CALL SetTextDetour(
    void* widget, const UnrealText* text) noexcept;
bool EnsureSetTextHook(Context& context) noexcept;
bool ReleaseSetTextHook(Context& context) noexcept;

void DropMismatchedPrefixWidgets(Context& context) noexcept {
    for (std::size_t index = 0; index < context.widget_count;) {
        if (!context.widgets[index].prefix) {
            ++index;
            continue;
        }
        std::uintptr_t widget{};
        if (ResolveTextBlockAddress(context, context.widgets[index].handle, widget) &&
            IsRoleIdPrefixInstance(context, widget)) {
            ++index;
            continue;
        }
        context.widgets[index] = context.widgets[--context.widget_count];
    }
}

bool TrackWidget(
    Context& context, const AnomalyUe5ObjectSnapshotV1& snapshot,
    const bool prefix) noexcept {
    if (prefix) {
        std::uintptr_t widget{};
        if (!ResolveTextBlockAddress(context, snapshot.handle, widget) ||
            !IsRoleIdPrefixInstance(context, widget)) {
            return false;
        }
    } else {
        bool already_tracked = false;
        for (std::size_t index = 0; index < context.widget_count; ++index) {
            if (context.widgets[index].handle.id == snapshot.handle.id &&
                context.widgets[index].handle.generation == snapshot.handle.generation) {
                already_tracked = true;
                break;
            }
        }
        // Refresh the structural anchor before duplicate-handle rejection.
        // Hot reload and HUD teardown can clear the cached WidgetTree/panel
        // while the already tracked RoleID handle remains valid; returning
        // early in that state prevents the real TextBlock_90 from ever being
        // accepted on the next scan.
        std::uintptr_t object{};
        if (ResolveTextBlockAddress(context, snapshot.handle, object)) {
            const std::uintptr_t outer = ReadObjectOuter(object);
            const std::uintptr_t panel = ReadWidgetPanel(object);
            const std::uint32_t encoded_index =
                static_cast<std::uint32_t>(snapshot.handle.id);
            const std::uint32_t object_index =
                encoded_index == 0 ? 0 : encoded_index - 1U;
            const bool newer_roleid = !already_tracked &&
                (context.roleid_anchor_index == 0 ||
                 object_index >= context.roleid_anchor_index);
            if (outer != 0 && panel != 0 &&
                (context.roleid_outer == 0 || context.roleid_panel == 0 ||
                 newer_roleid)) {
                context.roleid_outer = outer;
                context.roleid_panel = panel;
                context.roleid_anchor_index = object_index;
                DropMismatchedPrefixWidgets(context);
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    "FakeUID: RoleID WidgetTree and CanvasPanel anchored");
            }
        }
    }
    for (std::size_t index = 0; index < context.widget_count; ++index) {
        if (context.widgets[index].handle.id == snapshot.handle.id &&
            context.widgets[index].handle.generation == snapshot.handle.generation) {
            return false;
        }
    }
    if (context.widget_count == context.widgets.size()) {
        // TextBlock_90 is a separate prefix widget. Never evict it merely
        // because a later RoleID layer was created during a HUD refresh.
        std::size_t oldest_index = context.widgets.size();
        for (std::size_t index = 0; index < context.widget_count; ++index) {
            const auto& candidate = context.widgets[index];
            if (candidate.prefix != prefix) continue;
            if (oldest_index == context.widgets.size() ||
                static_cast<std::uint32_t>(candidate.handle.id >> 32U) <
                    static_cast<std::uint32_t>(context.widgets[oldest_index].handle.id >> 32U)) {
                oldest_index = index;
            }
        }
        if (oldest_index == context.widgets.size()) {
            for (std::size_t index = 0; index < context.widget_count; ++index) {
                const auto& candidate = context.widgets[index];
                if (candidate.prefix) continue;
                if (oldest_index == context.widgets.size() ||
                    static_cast<std::uint32_t>(candidate.handle.id >> 32U) <
                        static_cast<std::uint32_t>(context.widgets[oldest_index].handle.id >> 32U)) {
                    oldest_index = index;
                }
            }
        }
        if (oldest_index == context.widgets.size()) return false;
        const bool evicted_value_widget = !context.widgets[oldest_index].prefix;
        context.widgets[oldest_index] = {
            snapshot.handle, prefix, 0, 0};
        if (evicted_value_widget) {
            // The only surviving RoleID layer was evicted to make room: its
            // tree address must go stale immediately.
            context.roleid_outer = 0;
            context.roleid_panel = 0;
            context.roleid_anchor_index = 0;
        }
    } else {
        context.widgets[context.widget_count++] = {
            snapshot.handle, prefix, 0, 0};
    }
    if (prefix) context.target_prefix_name_id = snapshot.name_id;
    else context.target_name_id = snapshot.name_id;
    if (prefix) {
        // Persist the discovered FName index only when it resolves to the
        // TextBlock_90 family; arbitrary names from the text probe are
        // per-session only and must never leak into the next session.
        if (context.persisted_prefix_name_id.load(std::memory_order_acquire) !=
            snapshot.name_id) {
            std::string resolved_prefix;
            if (ResolveName(*context.names, snapshot.name_id, resolved_prefix) &&
                (resolved_prefix == kTargetPrefixWidgetName ||
                 resolved_prefix.starts_with("TextBlock_90_"))) {
                context.persisted_prefix_name_id.store(
                    snapshot.name_id, std::memory_order_release);
                const auto settings = ReadSettings(context);
                if (settings) {
                    context.save_state.store(1U, std::memory_order_release);
                    if (!ScheduleSettingsPersist(context)) {
                        context.save_state.store(3U, std::memory_order_release);
                    }
                }
            }
        }
    }
    if (prefix && !context.prefix_track_diagnostic_emitted) {
        context.prefix_track_diagnostic_emitted = true;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: TextBlock_90 candidate tracked nameId=" +
                std::to_string(snapshot.name_id));
    } else if (!prefix && !context.value_track_diagnostic_emitted) {
        context.value_track_diagnostic_emitted = true;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: TextBlock_RoleID candidate tracked nameId=" +
                std::to_string(snapshot.name_id));
    }
    context.rejected_name_ids.clear();
    return true;
}

bool ReadWidgetText(
    Context& context, const std::uintptr_t widget, std::wstring& text) noexcept;
bool ResolveWidgetAddress(
    const Context& context, const AnomalyGenerationHandleV1 handle,
    std::uintptr_t& widget) noexcept;

// Serial-validated liveness for the RoleID anchor. The raw outer address is
// only trusted while at least one tracked value TextBlock still resolves
// through the object registry with a matching serial and hangs off that exact
// tree. Running once per Update tick prevents structural fallbacks from using
// a stale anchor after a HUD teardown.
void ValidateRoleIDAnchor(Context& context) noexcept {
    if (context.roleid_outer == 0 || context.roleid_panel == 0 ||
        !ObjectsReady(context.objects)) return;
    for (std::size_t index = 0; index < context.widget_count; ++index) {
        if (context.widgets[index].prefix) continue;
        std::uintptr_t widget{};
        if (!ResolveWidgetAddress(context, context.widgets[index].handle, widget)) {
            continue;
        }
        if (ReadObjectOuter(widget) == context.roleid_outer &&
            ReadWidgetPanel(widget) == context.roleid_panel) return;
    }
    context.roleid_outer = 0;
    context.roleid_panel = 0;
    context.roleid_anchor_index = 0;
}

bool LooksLikeUidPrefix(const std::wstring_view text) noexcept;

void BeginIncrementalObjectScan(
    Context& context, const std::uint32_t previous_count,
    const std::uint32_t count) noexcept {
    context.scan_count = count;
    context.neighborhood_scan_active = false;
    context.named_scan_active = false;
    context.scan_start = (std::min)(previous_count, count);
    context.object_cursor = count;
    context.scan_batch_size = kObjectBatchSize;
    context.scan_active = context.object_cursor > context.scan_start;
}

void BeginRoleIdNeighborhoodScan(
    Context& context, const std::uint32_t count,
    const std::uint32_t roleid_index) noexcept {
    const std::uint32_t lower = roleid_index > kRoleIdNeighborhoodRadius
        ? roleid_index - kRoleIdNeighborhoodRadius
        : 0;
    const std::uint64_t upper_wide =
        static_cast<std::uint64_t>(roleid_index) + kRoleIdNeighborhoodRadius + 1ULL;
    const std::uint32_t upper = static_cast<std::uint32_t>(
        (std::min)(upper_wide, static_cast<std::uint64_t>(count)));
    context.scan_count = count;
    context.neighborhood_scan_active = true;
    context.named_scan_active = false;
    context.scan_start = lower;
    context.object_cursor = upper;
    context.scan_batch_size = kNeighborhoodObjectBatchSize;
    context.scan_active = upper > lower;
}

void BeginNamedObjectScan(
    Context& context, const std::uint32_t count) noexcept {
    context.scan_count = count;
    context.neighborhood_scan_active = false;
    context.named_scan_active = true;
    context.scan_start = 0;
    context.object_cursor = count;
    context.scan_batch_size = kNamedObjectBatchSize;
    context.scan_active = count != 0;
}

bool ResolveTemplateNameId(
    Context& context, const std::string_view path,
    const std::string_view expected_name, std::uint32_t& name_id,
    AnomalyGenerationHandleV1& template_handle) noexcept {
    if (name_id != 0 && template_handle.id != 0) return true;
    if (!ObjectFindReady(context.objects) || !ObjectsReady(context.objects) ||
        !NamesReady(context.names)) {
        return false;
    }
    AnomalyGenerationHandleV1 handle{};
    if (context.objects->find_exact(
            context.objects->user, anomaly::sdk::StringView(path), &handle).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (context.objects->snapshot_by_handle(
            context.objects->user, handle, &snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        snapshot.name_id == 0) {
        return false;
    }
    std::string resolved;
    if (!ResolveName(*context.names, snapshot.name_id, resolved) ||
        (resolved != expected_name &&
         !resolved.starts_with(std::string(expected_name) + "_"))) {
        return false;
    }
    name_id = snapshot.name_id;
    template_handle = handle;
    return true;
}

void ArmTargetWidgetNames(Context& context) noexcept {
    static_cast<void>(ResolveTemplateNameId(
        context, kTargetWidgetTemplatePath, kTargetWidgetName,
        context.target_name_id, context.target_template_handle));
    static_cast<void>(ResolveTemplateNameId(
        context, kTargetPrefixWidgetTemplatePath, kTargetPrefixWidgetName,
        context.target_prefix_name_id, context.prefix_template_handle));
    if (context.target_name_id != 0 &&
        !context.target_names_armed_diagnostic_emitted) {
        context.target_names_armed_diagnostic_emitted = true;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: RoleID widget names armed from template");
    }
}

void ScanForWidgets(Context& context) {
    if (!ObjectsReady(context.objects) || !NamesReady(context.names)) {
        return;
    }
    const std::uint64_t generation = context.objects->generation(context.objects->user);
    const std::uint32_t count = context.objects->count(context.objects->user);
    if (generation == 0 || count == 0) return;
    if (context.object_generation != generation) {
        const std::uint32_t previous_anchor = context.roleid_anchor_index;
        context.object_generation = generation;
        context.scan_count = count;
        context.target_name_id = 0;
        context.target_template_handle = {};
        context.prefix_template_handle = {};
        // The prefix FName index is stable within a game session, so restore
        // the persisted ID instead of losing it to the generation reset, but
        // only while it still resolves to the family in the live table.
        const std::uint32_t persisted_prefix_name_id =
            context.persisted_prefix_name_id.load(std::memory_order_acquire);
        if (persisted_prefix_name_id != 0 &&
            !PrefixNameIdPlausible(*context.names, persisted_prefix_name_id)) {
            context.persisted_prefix_name_id.store(0, std::memory_order_release);
        }
        context.target_prefix_name_id =
            context.persisted_prefix_name_id.load(std::memory_order_acquire);
        context.widget_count = 0;
        // A new object generation means the previous RoleID WidgetTree is
        // gone. Its raw outer address is stale from this moment; keeping it
        // would let a recycled address validate foreign trees for structural
        // fallbacks.
        context.roleid_outer = 0;
        context.roleid_panel = 0;
        context.roleid_anchor_index = 0;
        context.recovery_anchor_index = previous_anchor;
        context.rejected_name_ids.clear();
        context.bootstrap_scan_started = false;
        context.named_scan_active = false;
        context.target_names_armed_diagnostic_emitted = false;
        context.rescan_requested.store(
            previous_anchor != 0, std::memory_order_release);
    }
    ArmTargetWidgetNames(context);
    bool prefix_tracked = false;
    bool roleid_tracked = false;
    for (std::size_t index = 0; index < context.widget_count; ++index) {
        if (context.widgets[index].prefix) prefix_tracked = true;
        else roleid_tracked = true;
    }
    const bool requested = context.rescan_requested.exchange(
        false, std::memory_order_acq_rel);
    if (requested) {
        const std::uint32_t recovery_anchor = context.recovery_anchor_index;
        context.recovery_anchor_index = 0;
        // A HUD rebuild normally appends replacement widgets. Reuse the last
        // completed object count and inspect only that new range. If slots were
        // recycled without growing the table, inspect only the stale widget's
        // bounded neighborhood.
        if (recovery_anchor != 0 && recovery_anchor < count) {
            BeginRoleIdNeighborhoodScan(context, count, recovery_anchor);
        } else if (context.scan_count != 0 && count > context.scan_count) {
            BeginIncrementalObjectScan(context, context.scan_count, count);
        } else {
            context.scan_active = false;
            context.scan_count = count;
        }
    } else if (!context.scan_active && !context.bootstrap_scan_started &&
               context.target_name_id != 0 && context.widget_count == 0) {
        // Cold start / hot reload has no transferable opaque handles. Resolve
        // the stable template name once, then bootstrap with integer name-ID
        // comparisons only. No TextBlock probing or FString allocation occurs
        // over unrelated objects.
        context.bootstrap_scan_started = true;
        BeginNamedObjectScan(context, count);
    } else if (!context.scan_active && (!roleid_tracked || !prefix_tracked)) {
        // After an unsuccessful scan, do no periodic whole-table work. A late
        // HUD creation appends objects, so inspect only the newly added range.
        // A shrinking registry invalidates the old baseline. Re-probe only the
        // last RoleID neighborhood when one is known; otherwise adopt the new
        // count and wait for subsequently appended UI objects.
        if (count > context.scan_count) {
            BeginIncrementalObjectScan(context, context.scan_count, count);
        } else if (count < context.scan_count) {
            if (context.roleid_anchor_index != 0 &&
                context.roleid_anchor_index < count) {
                BeginRoleIdNeighborhoodScan(
                    context, count, context.roleid_anchor_index);
            } else {
                context.scan_count = count;
            }
        }
    }
    if (!context.scan_active) return;
    if (context.object_cursor <= context.scan_start || context.object_cursor > count) {
        context.object_cursor = count;
    }
    const auto Track = [&context, &prefix_tracked, &roleid_tracked](
                           const AnomalyUe5ObjectSnapshotV1& snapshot,
                           const bool prefix) {
        if (!TrackWidget(context, snapshot, prefix)) return false;
        if (prefix) prefix_tracked = true;
        else roleid_tracked = true;
        return prefix_tracked && roleid_tracked;
    };
    bool neighborhood_requested = false;
    std::uint32_t neighborhood_anchor{};
    const auto TrackValue =
        [&context, &Track, &prefix_tracked, &roleid_tracked,
         &neighborhood_requested, &neighborhood_anchor](
            const AnomalyUe5ObjectSnapshotV1& snapshot,
            const std::uint32_t object_index) {
        const bool complete = Track(snapshot, false);
        if (roleid_tracked && !prefix_tracked &&
            !context.neighborhood_scan_active) {
            neighborhood_requested = true;
            neighborhood_anchor = object_index;
        }
        return complete;
    };
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
        const bool is_value_template =
            context.target_template_handle.id != 0 &&
            snapshot.handle.id == context.target_template_handle.id &&
            snapshot.handle.generation == context.target_template_handle.generation;
        const bool is_prefix_template =
            context.prefix_template_handle.id != 0 &&
            snapshot.handle.id == context.prefix_template_handle.id &&
            snapshot.handle.generation == context.prefix_template_handle.generation;
        if (is_value_template || is_prefix_template) continue;
        if (context.target_name_id != 0 && snapshot.name_id == context.target_name_id) {
            if (TrackValue(snapshot, index)) {
                context.object_cursor = context.scan_start;
                break;
            }
            if (neighborhood_requested) break;
            continue;
        }
        if (context.named_scan_active && snapshot.name_id != context.target_name_id &&
            (context.target_prefix_name_id == 0 ||
             snapshot.name_id != context.target_prefix_name_id)) {
            continue;
        }
        if (context.target_prefix_name_id != 0 &&
            snapshot.name_id == context.target_prefix_name_id) {
            // Layer retention for the armed family name: later instances are
            // only adopted while they hang off the live RoleID tree (or no
            // tree is anchored yet; the apply-time gate rejects those until
            // the scan validates a fresh value TextBlock).
            std::uintptr_t named{};
            if (ResolveTextBlockAddress(context, snapshot.handle, named) &&
                IsRoleIdPrefixInstance(context, named)) {
                if (Track(snapshot, true)) {
                    context.object_cursor = context.scan_start;
                    break;
                }
            }
            continue;
        }
        if (snapshot.name_id == 0 ||
            context.rejected_name_ids.find(snapshot.name_id) !=
                context.rejected_name_ids.end()) {
            continue;
        }
        // Discovery fallback (lifecycle-safe): locate the prefix label by its
        // visible text. The HUD creates this widget late and its runtime name
        // has repeatedly failed to match any tracked label, so every object
        // with a UTextBlock-shaped vtable is probed by content instead. Once
        // the prefix is tracked this probe is disabled: reading the text of
        // every TextBlock on every pass costs FString allocations per frame.
        std::uintptr_t candidate = 0;
        const bool text_block_shaped = !prefix_tracked &&
            ResolveTextBlockAddress(context, snapshot.handle, candidate);
        if (text_block_shaped) {
            std::wstring text;
            const bool readable = ReadWidgetText(context, candidate, text);
            if (readable && LooksLikeUidPrefix(text) &&
                IsRoleIdPrefixInstance(context, candidate)) {
                if (Track(snapshot, true)) {
                    context.object_cursor = context.scan_start;
                    break;
                }
                continue;
            }
            // Outer fallback: once the UID value TextBlock is known, the
            // cleared prefix label is the sibling TextBlock in the same HUD
            // WidgetTree whose text is empty. This re-locks it by structure
            // after UI reloads even though its text no longer contains "UID".
            // BPUI_RoleID's CanvasPanel_0 has exactly two direct TextBlock
            // children in the cooked asset: TextBlock_RoleID and TextBlock_90.
            // TextBlock_90 is not marked bIsVariable, so find_exact can leave
            // target_prefix_name_id at zero even while the live widget exists.
            // Once the value TextBlock anchors this exact WidgetTree and panel,
            // the only other TextBlock child is therefore the prefix. This
            // structural fallback is what restores a prefix that an earlier
            // generation already cleared to an empty string.
            if (IsRoleIdPrefixInstance(context, candidate) &&
                text.empty() && snapshot.name_id != context.target_name_id) {
                if (Track(snapshot, true)) {
                    context.object_cursor = context.scan_start;
                    break;
                }
                continue;
            }
        }
        std::string name;
        if (ResolveName(*context.names, snapshot.name_id, name)) {
            if (name == kTargetWidgetName || name.starts_with("TextBlock_RoleID_")) {
                if (TrackValue(snapshot, index)) {
                    context.object_cursor = context.scan_start;
                    break;
                }
                if (neighborhood_requested) break;
            } else if (name == kTargetPrefixWidgetName ||
                       name.starts_with("TextBlock_90_")) {
                // Family names exist across unrelated panels: only adopt an
                // instance hanging off the live RoleID tree. A foreign-tree
                // match stays untracked and un-rejected so the real one can
                // still be captured on a later pass.
                std::uintptr_t named{};
                if (ResolveTextBlockAddress(context, snapshot.handle, named) &&
                    IsRoleIdPrefixInstance(context, named)) {
                    if (Track(snapshot, true)) {
                        context.object_cursor = context.scan_start;
                        break;
                    }
                }
            } else if (!text_block_shaped) {
                // Non-TextBlock objects are rejected by name; TextBlock-shaped
                // objects are re-probed on every scan because their text may
                // be filled in after the first pass.
                context.rejected_name_ids.insert(snapshot.name_id);
            }
        }
    }
    if (neighborhood_requested) {
        // The descending recovery pass may already have stepped past a sibling
        // created at a higher object index. Re-scan one bounded window around
        // the newly anchored RoleID in both directions, then stop.
        BeginRoleIdNeighborhoodScan(context, count, neighborhood_anchor);
        return;
    }
    if (context.object_cursor <= context.scan_start) {
        context.scan_active = false;
        context.neighborhood_scan_active = false;
        context.named_scan_active = false;
        const std::uint32_t completed_count = context.scan_count;
        context.scan_count = count;
        // Objects appended during the fixed scan window were intentionally not
        // allowed to move its tail. If the target is still missing, inspect
        // exactly that appended range next instead of restarting a full pass.
        if ((!roleid_tracked || !prefix_tracked) && count > completed_count) {
            BeginIncrementalObjectScan(context, completed_count, count);
        }
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
        const auto object_name = object == 0 ? 0U : *reinterpret_cast<const std::uint32_t*>(
            object + fake_uid_profile::kObjectNameOffset);
        if (serial != expected_serial || object == 0 ||
            (object_name != context.target_name_id &&
             object_name != context.target_prefix_name_id)) {
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

// Address resolution for discovery fallback: only the TextBlock vtable shape
// is verified, the widget name is intentionally not required.
bool ResolveTextBlockAddress(
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
        if (serial != expected_serial || object == 0) return false;
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

bool ReadRoleIdObjectNameId(
    const Context& context, const std::uintptr_t object,
    std::uint32_t& name_id) noexcept {
    name_id = 0;
    if (object == 0 || context.set_text == nullptr) return false;
    __try {
        const std::uintptr_t vtable =
            *reinterpret_cast<const std::uintptr_t*>(object);
        if (vtable == 0 ||
            *reinterpret_cast<const std::uintptr_t*>(
                vtable + fake_uid_profile::kSetTextVtableOffset) !=
                reinterpret_cast<std::uintptr_t>(context.set_text)) {
            return false;
        }
        name_id = *reinterpret_cast<const std::uint32_t*>(
            object + fake_uid_profile::kObjectNameOffset);
        return name_id != 0;
    } __except (1) {
        name_id = 0;
        return false;
    }
}

bool IsRoleIdValueObject(
    const Context& context, const std::uintptr_t object,
    std::uint32_t& name_id) noexcept {
    if (!ReadRoleIdObjectNameId(context, object, name_id) ||
        context.names == nullptr) {
        return false;
    }
    std::string name;
    if (!ResolveName(*context.names, name_id, name)) return false;
    return name == kTargetWidgetName ||
        name.starts_with("TextBlock_RoleID_");
}

bool ReadWidgetText(
    Context& context, const std::uintptr_t widget, std::wstring& text) noexcept {
    text.clear();
    if (widget == 0 || context.text_to_string == nullptr) return false;
    const auto* const value = reinterpret_cast<const UnrealText*>(
        widget + context.text_field_offset);
    return ReadUnrealText(context, value, text);
}

bool IsReadableUnrealString(
    const wchar_t* const data, const std::int32_t count) noexcept {
    if (data == nullptr || count <= 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(data);
    const auto bytes = static_cast<std::uintptr_t>(count) * sizeof(wchar_t);
    if (begin == 0 ||
        bytes > std::numeric_limits<std::uintptr_t>::max() - begin) {
        return false;
    }
    __try {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(data, &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT ||
            (info.Protect & PAGE_GUARD) != 0 ||
            (info.Protect & 0xFFU) == PAGE_NOACCESS) {
            return false;
        }
        const auto region_begin = reinterpret_cast<std::uintptr_t>(
            info.BaseAddress);
        if (info.RegionSize >
            std::numeric_limits<std::uintptr_t>::max() - region_begin) {
            return false;
        }
        const auto region_end = region_begin +
            static_cast<std::uintptr_t>(info.RegionSize);
        return begin >= region_begin && begin + bytes <= region_end;
    } __except (1) {
        return false;
    }
}

void ReleaseReadbackString(
    Context& context, UnrealString& value) noexcept {
    const auto allocation = value.data;
    const auto count = value.count;
    value = {};
    if (allocation == nullptr ||
        count <= 0 ||
        count > static_cast<std::int32_t>(kMaximumUidCharacters + 1U) ||
        !IsReadableUnrealString(allocation, count) ||
        context.free_string == nullptr) {
        return;
    }
    __try {
        context.free_string(allocation);
    } __except (1) {
    }
}

bool ReadUnrealText(
    Context& context, const UnrealText* const value, std::wstring& text) noexcept {
    text.clear();
    if (value == nullptr || context.text_to_string == nullptr ||
        context.free_string == nullptr) {
        return false;
    }
    UnrealString current{};
    __try {
        if (context.text_to_string(&current, value) == nullptr ||
            current.count < 0 || current.capacity < current.count ||
            current.count > static_cast<std::int32_t>(kMaximumUidCharacters + 1U) ||
            (current.count > 0 && current.data == nullptr)) {
            ReleaseReadbackString(context, current);
            return false;
        }
        // An intentionally hidden prefix is a valid empty FString. UE may
        // expose it as {nullptr,0,0} or as a single terminator; accepting both
        // lets an unchecked hide-prefix option restore the original label.
        if (current.count > 0 &&
            !IsReadableUnrealString(current.data, current.count)) {
            ReleaseReadbackString(context, current);
            return false;
        }
        if (current.count > 0) {
            text.assign(current.data, static_cast<std::size_t>(current.count - 1));
        }
        ReleaseReadbackString(context, current);
        return true;
    } __except (1) {
        ReleaseReadbackString(context, current);
        return false;
    }
}

bool BuildUnrealText(
    Context& context, const std::wstring_view value, UnrealText& result) noexcept {
    result = {};
    // Use a valid zero-width FText for the hidden prefix. Some cooked
    // UTextBlock paths keep the old label when passed UE's canonical empty
    // text, which is why the hidden state must not depend on a null payload.
    if (value.size() > kMaximumUidCharacters ||
        context.process_event == nullptr ||
        context.kismet_text_library_cdo == 0 ||
        context.string_to_text_function == 0 ||
        context.assign_string == nullptr || context.free_string == nullptr) {
        return false;
    }
    struct StringToTextParameters final {
        UnrealString input{};
        UnrealText result{};
    } conversion{};
    static_assert(sizeof(StringToTextParameters) == 32U);
    if (context.assign_string(
            &conversion.input, value.data()) == nullptr) {
        return false;
    }
    if (!InvokeProcessEvent(
            context, context.kismet_text_library_cdo,
            context.string_to_text_function, &conversion) ||
        !IsUsableUnrealText(conversion.result, value.empty())) {
        ReleaseUnrealString(context, conversion.input);
        return false;
    }
    ReleaseUnrealString(context, conversion.input);
    result = conversion.result;
    return true;
}

void ReleaseUnrealString(
    Context& context, UnrealString& value) noexcept {
    auto* const allocation = value.data;
    value = {};
    if (allocation == nullptr || context.free_string == nullptr) return;
    __try {
        context.free_string(allocation);
    } __except (1) {
    }
}

bool IsUsableUnrealText(
    const UnrealText& value, const bool allow_empty) noexcept {
    constexpr std::uintptr_t kMinimumX64ProcessAddress = 0x100000000ULL;
    __try {
        // UE represents FText::GetEmpty() with a null text-data pointer. It is
        // a valid value for the prefix-clearing path, so do not reject the
        // canonical empty representation as malformed.
        if (value.data == nullptr) return allow_empty;
        if (reinterpret_cast<std::uintptr_t>(value.data) <
            kMinimumX64ProcessAddress) {
            return false;
        }
        MEMORY_BASIC_INFORMATION data_info{};
        if (VirtualQuery(
                value.data, &data_info, sizeof(data_info)) !=
                sizeof(data_info) ||
            data_info.State != MEM_COMMIT ||
            (data_info.Protect & 0xFFU) == PAGE_NOACCESS ||
            (data_info.Protect & PAGE_GUARD) != 0) {
            return false;
        }
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(value.data);
        if (vtable < kMinimumX64ProcessAddress) return false;
        MEMORY_BASIC_INFORMATION vtable_info{};
        return VirtualQuery(
                   reinterpret_cast<const void*>(vtable), &vtable_info,
                   sizeof(vtable_info)) == sizeof(vtable_info) &&
            vtable_info.State == MEM_COMMIT &&
            (vtable_info.Protect & 0xFFU) != PAGE_NOACCESS &&
            (vtable_info.Protect & PAGE_GUARD) == 0;
    } __except (1) {
        return false;
    }
}

bool EnsureTextHookSnapshot(
    Context& context, const SettingsSnapshot& settings,
    const std::uint64_t revision) noexcept {
    if (!settings.enabled) {
        context.text_override.store({}, std::memory_order_release);
        context.text_override_revision = 0;
        return true;
    }
    try {
        auto current = context.text_override.load(std::memory_order_acquire);
        if (current != nullptr && context.text_override_revision == revision) {
            if (current->target_name_id == context.target_name_id &&
                current->prefix_name_id == context.target_prefix_name_id &&
                current->roleid_outer == context.roleid_outer &&
                current->roleid_panel == context.roleid_panel) {
                return true;
            }
            auto updated = std::make_shared<SetTextHookSnapshot>(*current);
            updated->target_name_id = context.target_name_id;
            updated->prefix_name_id = context.target_prefix_name_id;
            updated->roleid_outer = context.roleid_outer;
            updated->roleid_panel = context.roleid_panel;
            context.text_override.store(std::move(updated), std::memory_order_release);
            return true;
        }

        auto updated = std::make_shared<SetTextHookSnapshot>();
        updated->target_name_id = context.target_name_id;
        updated->prefix_name_id = context.target_prefix_name_id;
        updated->roleid_outer = context.roleid_outer;
        updated->roleid_panel = context.roleid_panel;
        context.text_override.store(std::move(updated), std::memory_order_release);
        context.text_override_revision = revision;
        return true;
    } catch (...) {
        context.text_override.store({}, std::memory_order_release);
        return false;
    }
}

// The RoleID value widget carries the bare UID while the localized "UID" label
// lives in a separate TextBlock. Only a numeric payload can be a value write,
// which keeps the name lookup in ArmHookValueName off every unrelated
// TextBlock the engine re-texts.
bool LooksLikeUidValue(const std::wstring_view text) noexcept {
    if (text.size() < 6 || text.size() > 20) return false;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
    }
    return true;
}

// Arms the hook from the widget's own FName. The template lookup in
// ArmTargetWidgetNames only succeeds once the RoleID blueprint is loaded, and
// a new object generation drops both the template name and the anchored tree,
// so in either window a fresh RoleID layer can only be recognized by its own
// name. FName indexes are stable for the whole process session, which lets one
// cached index cover the replacement instances of a HUD rebuild while keeping
// the string resolve off the hot path.
bool ArmHookValueName(Context& context, const std::uintptr_t widget) noexcept {
    std::uint32_t name_id = 0;
    if (!ReadRoleIdObjectNameId(context, widget, name_id)) return false;
    if (context.hook_value_name_id.load(std::memory_order_acquire) == name_id) {
        return true;
    }
    if (!IsRoleIdValueObject(context, widget, name_id)) return false;
    context.hook_value_name_id.store(name_id, std::memory_order_release);
    return true;
}

bool IsHookTargetWidget(
    const Context& context, const std::uintptr_t widget,
    const SetTextHookSnapshot& override) noexcept {
    // The template name and the name the hook learned from a live widget can
    // disagree: the blueprint template is "TextBlock_RoleID" while a rebuilt
    // HUD layer may carry the suffixed instance name. Either identity
    // recognizes the value widget, so whichever is known first does not
    // decide whether the other one still matches.
    const std::uint32_t hook_name_id =
        context.hook_value_name_id.load(std::memory_order_acquire);
    if (widget == 0 || context.set_text == nullptr ||
        (override.target_name_id == 0 && hook_name_id == 0)) {
        return false;
    }
    __try {
        const std::uintptr_t vtable =
            *reinterpret_cast<const std::uintptr_t*>(widget);
        const std::uint32_t widget_name_id =
            *reinterpret_cast<const std::uint32_t*>(
                widget + fake_uid_profile::kObjectNameOffset);
        if (vtable == 0 ||
            *reinterpret_cast<const std::uintptr_t*>(
                vtable + fake_uid_profile::kSetTextVtableOffset) !=
                reinterpret_cast<std::uintptr_t>(context.set_text) ||
            widget_name_id == 0 ||
            (widget_name_id != override.target_name_id &&
             widget_name_id != hook_name_id)) {
            return false;
        }
        if (override.roleid_outer == 0 || override.roleid_panel == 0) {
            return true;
        }
        return ReadObjectOuter(widget) == override.roleid_outer &&
            ReadWidgetPanel(widget) == override.roleid_panel;
    } __except (1) {
        return false;
    }
}

bool IsHookPrefixWidget(
    const Context& context, const std::uintptr_t widget,
    const SetTextHookSnapshot& override) noexcept {
    if (widget == 0 || context.set_text == nullptr ||
        override.prefix_name_id == 0) {
        return false;
    }
    __try {
        const std::uintptr_t vtable =
            *reinterpret_cast<const std::uintptr_t*>(widget);
        if (vtable == 0 ||
            *reinterpret_cast<const std::uintptr_t*>(
                vtable + fake_uid_profile::kSetTextVtableOffset) !=
                reinterpret_cast<std::uintptr_t>(context.set_text) ||
            *reinterpret_cast<const std::uint32_t*>(
                widget + fake_uid_profile::kObjectNameOffset) !=
                override.prefix_name_id) {
            return false;
        }
        if (override.roleid_outer == 0 || override.roleid_panel == 0) {
            return true;
        }
        return ReadObjectOuter(widget) == override.roleid_outer &&
            ReadWidgetPanel(widget) == override.roleid_panel;
    } __except (1) {
        return false;
    }
}

void CallSetTextOriginal(
    const SetTextFn original, void* const widget,
    const UnrealText* const text) noexcept {
    if (original == nullptr) return;
    __try {
        original(widget, text);
    } __except (1) {
    }
}

bool BeginSetTextCallback(
    const AnomalyHookServiceV1* const hook_api,
    const AnomalyGenerationHandleV1 hook,
    AnomalyGenerationHandleV1* const callback_lease) noexcept {
    if (hook_api == nullptr || hook_api->begin_callback == nullptr ||
        callback_lease == nullptr) {
        return false;
    }
    __try {
        return hook_api->begin_callback(
                   hook_api->user, hook, callback_lease).code ==
            ANOMALY_STATUS_V1_OK;
    } __except (1) {
        return false;
    }
}

void EndSetTextCallback(
    const AnomalyHookServiceV1* const hook_api,
    const AnomalyGenerationHandleV1 callback_lease) noexcept {
    if (hook_api == nullptr || hook_api->end_callback == nullptr ||
        callback_lease.id == 0) {
        return;
    }
    __try {
        static_cast<void>(hook_api->end_callback(
            hook_api->user, callback_lease));
    } __except (1) {
    }
}

void ANOMALY_CALL SetTextDetour(
    void* widget, const UnrealText* text) noexcept {
    const auto* const hook_api =
        g_set_text_hook_api.load(std::memory_order_acquire);
    const AnomalyGenerationHandleV1 hook_handle{
        g_set_text_hook_id.load(std::memory_order_acquire),
        g_set_text_hook_generation.load(std::memory_order_acquire)};
    AnomalyGenerationHandleV1 callback_lease{};
    if (hook_api == nullptr || hook_handle.id == 0 ||
        hook_handle.generation == 0) {
        // A hook entry without a published lease source is a setup/teardown
        // window. Do not call a trampoline whose lifetime is unknown.
        return;
    }

    if (!BeginSetTextCallback(hook_api, hook_handle, &callback_lease) ||
        callback_lease.id == 0) {
        // The hook is being drained. The trampoline may already have been
        // removed, so do not call the stale original address.
        return;
    }

    auto* const context = g_active_context.load(std::memory_order_acquire);
    const auto original_address =
        g_set_text_original.load(std::memory_order_acquire);
    if (context != nullptr && original_address != 0) {
        using Function = SetTextFn;
        const auto original = reinterpret_cast<Function>(original_address);
        thread_local bool forwarding = false;
        const auto override =
            context->text_override.load(std::memory_order_acquire);
        const auto settings = context->settings.load(std::memory_order_acquire);
        const auto widget_address = reinterpret_cast<std::uintptr_t>(widget);
        bool is_value_widget = !forwarding && settings != nullptr &&
            settings->enabled && override != nullptr &&
            IsHookTargetWidget(*context, widget_address, *override);
        bool is_hidden_prefix = !forwarding && settings != nullptr &&
            settings->enabled && settings->hide_prefix && override != nullptr &&
            IsHookPrefixWidget(*context, widget_address, *override);
        // The prefix needs its text probe whenever its FName is unknown, and
        // the value widget needs one while the template name is unarmed -- a
        // cold start, or the object generation reset that a HUD rebuild
        // performs. Once the template name is back the probe leaves the hot
        // path and the value widget is matched by name instead.
        const bool content_probe_needed = settings != nullptr &&
            (settings->hide_prefix ||
             (override != nullptr && override->target_name_id == 0));
        if (!is_hidden_prefix && !is_value_widget &&
            !forwarding && settings != nullptr && settings->enabled &&
            override != nullptr && content_probe_needed) {
            // TextBlock_90 is not guaranteed to be exposed as a named
            // variable. During a HUD rebuild its FName and slot can therefore
            // be unknown even though the engine is about to write the
            // localized prefix. The prefix text is the stable discriminator
            // for this call; a numeric payload on a widget that names itself
            // as the RoleID value widget is the other. The label never ends
            // in a digit, so the two cannot be confused.
            std::wstring incoming_text;
            if (text != nullptr &&
                ReadUnrealText(*context, text, incoming_text)) {
                is_hidden_prefix = settings->hide_prefix &&
                    LooksLikeUidPrefix(incoming_text);
                if (!is_hidden_prefix) {
                    // The value write only reaches this probe while the
                    // template name is unarmed, and the widget's own name is
                    // then the only identity that a rebuilt HUD layer has.
                    is_value_widget = LooksLikeUidValue(incoming_text) &&
                        ArmHookValueName(*context, widget_address);
                }
            }
        }
        if (is_value_widget || is_hidden_prefix) {
            // FText owns shared engine data. Never retain a raw FText copy in
            // the plugin: a copy made from Conv_StringToText can outlive the
            // ProcessEvent parameter storage and later leave a stale
            // TextData pointer in UTextBlock. Construct it on this callback
            // stack and hand it to the original only during this invocation.
            UnrealText replacement{};
            const std::wstring_view replacement_value = is_hidden_prefix
                ? kHiddenPrefixText
                : std::wstring_view(settings->display_wide);
            if (BuildUnrealText(*context, replacement_value, replacement)) {
                forwarding = true;
                CallSetTextOriginal(original, widget, &replacement);
                forwarding = false;
                EndSetTextCallback(hook_api, callback_lease);
                return;
            }
            // If the incoming value is already malformed, do not feed it back
            // into UTextBlock::SetText after a failed replacement build. This
            // is the exact failure mode behind the observed
            // 0x000000060000000D read: forwarding that FText only turns the
            // bad input into a later Slate crash.
            if (text == nullptr || !IsUsableUnrealText(*text)) {
                EndSetTextCallback(hook_api, callback_lease);
                return;
            }
        }
        forwarding = true;
        CallSetTextOriginal(original, widget, text);
        forwarding = false;
    }

    EndSetTextCallback(hook_api, callback_lease);
}

bool ReleaseSetTextHook(Context& context) noexcept {
    context.text_override.store({}, std::memory_order_release);
    Context* expected = &context;
    static_cast<void>(g_active_context.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel));
    if (context.set_text_hook.id == 0) {
        g_set_text_hook_api.store(nullptr, std::memory_order_release);
        g_set_text_hook_id.store(0, std::memory_order_release);
        g_set_text_hook_generation.store(0, std::memory_order_release);
        g_set_text_original.store(0, std::memory_order_release);
        return true;
    }
    if (!HookReady(context.hook)) return false;
    const auto status = context.hook->release(
        context.hook->user, context.set_text_hook);
    if (status.code != ANOMALY_STATUS_V1_OK &&
        status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        return false;
    }
    context.set_text_hook = {};
    context.set_text_original = 0;
    context.set_text_target = 0;
    g_set_text_hook_api.store(nullptr, std::memory_order_release);
    g_set_text_hook_id.store(0, std::memory_order_release);
    g_set_text_hook_generation.store(0, std::memory_order_release);
    g_set_text_original.store(0, std::memory_order_release);
    return true;
}

bool EnsureSetTextHook(Context& context) noexcept {
    if (!HookReady(context.hook) || context.set_text == nullptr) return false;
    const auto target = reinterpret_cast<std::uintptr_t>(context.set_text);
    if (context.set_text_hook.id != 0 &&
        context.set_text_target == target &&
        context.set_text_original != 0) {
        return true;
    }
    if (context.set_text_hook.id != 0 && !ReleaseSetTextHook(context)) {
        return false;
    }

    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = reinterpret_cast<void*>(&SetTextDetour);
    request.label = anomaly::sdk::StringView("fake-uid-text-block-set-text");
    std::uintptr_t original{};
    AnomalyGenerationHandleV1 handle{};
    const auto status = context.hook->create(
        context.hook->user, &request, &original, &handle);
    if (status.code != ANOMALY_STATUS_V1_OK ||
        original == 0 || handle.id == 0) {
        return false;
    }
    context.set_text_original = original;
    context.set_text_target = target;
    context.set_text_hook = handle;
    g_set_text_hook_api.store(context.hook, std::memory_order_release);
    g_set_text_hook_id.store(handle.id, std::memory_order_release);
    g_set_text_hook_generation.store(handle.generation, std::memory_order_release);
    g_set_text_original.store(original, std::memory_order_release);
    g_active_context.store(&context, std::memory_order_release);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "FakeUID: native TextBlock.SetText hook active");
    return true;
}

bool SetWidgetText(
    Context& context, const std::uintptr_t widget,
    const wchar_t* const value) noexcept {
    if (widget == 0 || value == nullptr || !ResolveTextWriteBindings(context)) {
        return false;
    }
    const std::wstring_view wide(value);
    if (wide.size() > kMaximumUidCharacters) return false;

    // Reuse the same reflected conversion/write route as the in-tree NTE ESC
    // menu bridge. The previous direct virtual SetText call constructed and
    // released an FText manually; the resulting text data later reached Slate
    // with a non-ITextData vtable and crashed on its virtual AddRef. ProcessEvent
    // performs the UFunction parameter copy before entering the verified native
    // vtable body and is already exercised by the host for UTF-16 labels.
    struct SetTextParameters final {
        UnrealText text{};
    } parameters{};
    static_assert(sizeof(SetTextParameters) == 16U);
    if (!BuildUnrealText(context, wide, parameters.text)) return false;
    // Deliberately mirror nte_esc_menu_bridge.cpp and leave the returned FText
    // reference owned for the process lifetime. Writes are revision/lifecycle
    // bounded; avoiding a guessed manual destructor is safer than reintroducing
    // the confirmed Slate use-after-invalid-text crash.
    return InvokeProcessEvent(
        context, widget, context.text_block_set_text_function, &parameters);
}

// UObject::OuterPrivate lives at offset 0x20. The prefix label and the UID
// value TextBlocks share the same HUD WidgetTree outer, which identifies the
// prefix even after its text has been cleared.
std::uintptr_t ReadObjectOuter(const std::uintptr_t object) noexcept {
    if (object == 0) return 0;
    __try {
        return *reinterpret_cast<const std::uintptr_t*>(object + 32);
    } __except (1) {
        return 0;
    }
}

// UWidget::Slot is at +0x30. Both RoleID TextBlocks are direct children of
// CanvasPanel_0, so their slots have the same CanvasPanel outer. Pairing this
// with the shared WidgetTree makes TextBlock_90 instance selection exact even
// though that FName is reused by many unrelated blueprints.
std::uintptr_t ReadWidgetPanel(const std::uintptr_t widget) noexcept {
    if (widget == 0) return 0;
    __try {
        const std::uintptr_t slot = *reinterpret_cast<const std::uintptr_t*>(
            widget + fake_uid_profile::kWidgetSlotOffset);
        return ReadObjectOuter(slot);
    } __except (1) {
        return 0;
    }
}

bool IsRoleIdPrefixInstance(
    const Context& context, const std::uintptr_t widget) noexcept {
    return widget != 0 && context.roleid_outer != 0 &&
        context.roleid_panel != 0 &&
        ReadObjectOuter(widget) == context.roleid_outer &&
        ReadWidgetPanel(widget) == context.roleid_panel;
}

// UWidget::Visibility (ESlateVisibility, uint8) at the verified offset 0xDC.
std::uint8_t ReadVisibilityField(const std::uintptr_t widget) noexcept {
    if (widget == 0) return 0;
    __try {
        return *reinterpret_cast<const std::uint8_t*>(
            widget + fake_uid_profile::kWidgetVisibilityOffset);
    } __except (1) {
        return 0;
    }
}

// Matches the localized "UID" prefix label (full-width/ASCII colon or space) but
// not numeric UID values, our configured display text, or a full UID-plus-value
// string that lives inside the value widget itself.
bool LooksLikeUidPrefix(const std::wstring_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == L' ' || text[begin] == L'\t' ||
           text[begin] == L'\r' || text[begin] == L'\n')) {
        ++begin;
    }
    const std::wstring_view trimmed = text.substr(begin);
    if (trimmed.size() < 3 || trimmed.size() > 8) return false;
    if (!((trimmed[0] == L'U' || trimmed[0] == L'u') &&
          (trimmed[1] == L'I' || trimmed[1] == L'i') &&
          (trimmed[2] == L'D' || trimmed[2] == L'd'))) {
        return false;
    }
    if (trimmed.size() == 3) return true;  // bare "UID"
    const wchar_t tail = trimmed.back();
    return !((tail >= L'0' && tail <= L'9') ||
        (tail >= L'A' && tail <= L'Z') ||
        (tail >= L'a' && tail <= L'z'));
}

bool ResolveObjectAddress(
    const Context& context, const AnomalyGenerationHandleV1 handle,
    std::uintptr_t& object) noexcept {
    object = 0;
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
        const std::uintptr_t candidate = *reinterpret_cast<const std::uintptr_t*>(item);
        if (serial != expected_serial || candidate == 0) return false;
        object = candidate;
        return true;
    } __except (1) {
        return false;
    }
}

bool SetNativeFunctionFlag(
    const std::uintptr_t function, std::uint32_t& previous_flags) noexcept {
    previous_flags = 0;
    if (function == 0) return false;
    __try {
        auto* const flags = reinterpret_cast<std::uint32_t*>(
            function + fake_uid_profile::kUFunctionFlagsOffset);
        previous_flags = *flags;
        *flags = previous_flags | 0x400U;
        return true;
    } __except (1) {
        return false;
    }
}

bool RestoreFunctionFlags(
    const std::uintptr_t function, const std::uint32_t previous_flags) noexcept {
    if (function == 0) return false;
    __try {
        *reinterpret_cast<std::uint32_t*>(
            function + fake_uid_profile::kUFunctionFlagsOffset) = previous_flags;
        return true;
    } __except (1) {
        return false;
    }
}

bool WriteWidgetVisibility(
    const std::uintptr_t widget, const std::uint8_t visibility) noexcept {
    if (widget == 0) return false;
    __try {
        *reinterpret_cast<std::uint8_t*>(
            widget + fake_uid_profile::kWidgetVisibilityOffset) = visibility;
        return true;
    } __except (1) {
        return false;
    }
}

bool InvokeWidgetVisibility(
    Context& context, const std::uintptr_t widget, const std::uint8_t visibility) noexcept {
    if (widget == 0 || context.process_event == nullptr ||
        context.set_visibility_function == 0 || visibility > 4) {
        return false;
    }
    std::uint8_t parameters[1] = {visibility};
    return InvokeProcessEvent(
               context, widget, context.set_visibility_function, parameters) &&
        WriteWidgetVisibility(widget, visibility);
}

// Reads the UFunction header fields used to validate the SetVisibility layout.
// Kept separate from ResolveVisibilityBinding so that the __try block never
// coexists with unwinding objects (C2712).
bool ReadVisibilityLayout(
    const std::uintptr_t visibility_function, std::uintptr_t& func,
    std::uintptr_t& class_object, std::uintptr_t& outer_object,
    std::uint8_t& num_parms, std::uint16_t& parms_size,
    std::uint16_t& return_value) noexcept {
    func = 0;
    class_object = 0;
    outer_object = 0;
    num_parms = 0;
    parms_size = 0;
    return_value = 0;
    __try {
        class_object = *reinterpret_cast<const std::uintptr_t*>(
            visibility_function + 16);
        outer_object = *reinterpret_cast<const std::uintptr_t*>(
            visibility_function + 32);
        func = *reinterpret_cast<const std::uintptr_t*>(
            visibility_function + fake_uid_profile::kUFunctionFuncOffset);
        num_parms = *reinterpret_cast<const std::uint8_t*>(
            visibility_function + fake_uid_profile::kUFunctionNumParmsOffset);
        parms_size = *reinterpret_cast<const std::uint16_t*>(
            visibility_function + fake_uid_profile::kUFunctionParmsSizeOffset);
        return_value = *reinterpret_cast<const std::uint16_t*>(
            visibility_function + fake_uid_profile::kUFunctionReturnValueOffset);
        return true;
    } __except (1) {
        return false;
    }
}

// Locates a native UFUNCTION by exact path and validates its signature so it
// can be driven through ProcessEvent. Returns the UFunction object address.
bool FindUFunctionObject(
    Context& context, const std::string_view path,
    const std::uint8_t expected_num_parms, const std::uint16_t expected_parms_size,
    const std::uint16_t expected_return_value, std::uintptr_t& function) noexcept {
    function = 0;
    if (context.process_event == nullptr || !ObjectFindReady(context.objects)) return false;
    AnomalyGenerationHandleV1 handle{};
    if (context.objects->find_exact(
            context.objects->user, anomaly::sdk::StringView(path), &handle).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    std::uintptr_t object{};
    if (!ResolveObjectAddress(context, handle, object)) return false;
    std::uintptr_t func{};
    std::uintptr_t class_object{};
    std::uintptr_t outer_object{};
    std::uint8_t num_parms{};
    std::uint16_t parms_size{};
    std::uint16_t return_value{};
    if (!ReadVisibilityLayout(
            object, func, class_object, outer_object,
            num_parms, parms_size, return_value) ||
        class_object == 0 || outer_object == 0 || func == 0 ||
        num_parms != expected_num_parms || parms_size != expected_parms_size ||
        return_value != expected_return_value) {
        return false;
    }
    function = object;
    return true;
}

bool ResolveTextWriteBindings(Context& context) noexcept {
    if (context.process_event == nullptr || !ObjectFindReady(context.objects)) return false;
    const std::uint64_t generation = context.objects->generation(context.objects->user);
    if (generation == 0 || context.update_tick < context.next_text_write_binding_tick) {
        return context.text_write_generation == generation &&
            context.text_block_set_text_function != 0 &&
            context.string_to_text_function != 0 &&
            context.kismet_text_library_cdo != 0;
    }
    if (context.text_write_generation == generation &&
        context.text_block_set_text_function != 0 &&
        context.string_to_text_function != 0 &&
        context.kismet_text_library_cdo != 0) {
        return true;
    }

    context.next_text_write_binding_tick = context.update_tick + kObjectRescanInterval;
    context.text_write_generation = 0;
    context.text_block_set_text_function = 0;
    context.string_to_text_function = 0;
    context.kismet_text_library_cdo = 0;

    std::uintptr_t set_text{};
    std::uintptr_t string_to_text{};
    if (!FindUFunctionObject(
            context, kTextBlockSetTextPath, 1, 16, 0xFFFF, set_text) ||
        !FindUFunctionObject(
            context, kStringToTextPath, 2, 32, 16, string_to_text)) {
        return false;
    }
    AnomalyGenerationHandleV1 cdo_handle{};
    if (context.objects->find_exact(
            context.objects->user, anomaly::sdk::StringView(kKismetTextLibraryCdoPath),
            &cdo_handle).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    std::uintptr_t cdo{};
    if (!ResolveObjectAddress(context, cdo_handle, cdo)) return false;

    context.text_block_set_text_function = set_text;
    context.string_to_text_function = string_to_text;
    context.kismet_text_library_cdo = cdo;
    context.text_write_generation = generation;
    if (!context.text_write_binding_diagnostic_emitted) {
        context.text_write_binding_diagnostic_emitted = true;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: reflected Conv_StringToText and TextBlock.SetText bindings resolved");
    }
    return true;
}

// Thin SEH-guarded ProcessEvent wrapper; must stay free of unwinding objects.
bool InvokeProcessEvent(
    Context& context, const std::uintptr_t object, const std::uintptr_t function,
    void* const parameters) noexcept {
    if (object == 0 || function == 0 || context.process_event == nullptr) return false;
    // The generated SDK wrappers set FUNC_Native (0x400) before every
    // ProcessEvent call. Without it, this build can enter the reflected
    // dispatch path with a native UFunction and leave FText output in an
    // invalid state (the observed bad pointer was 0x000000060000000D).
    std::uint32_t previous_flags{};
    if (!SetNativeFunctionFlag(function, previous_flags)) return false;
    bool invoked = false;
    __try {
        context.process_event(
            reinterpret_cast<void*>(object),
            reinterpret_cast<void*>(function), parameters);
        invoked = true;
    } __except (1) {
    }
    const bool restored = RestoreFunctionFlags(function, previous_flags);
    return invoked && restored;
}

// Resolves the CanvasPanelSlot call chain used to align the RoleID value:
// UWidget::Slot -> CanvasPanelSlot::GetPosition/SetPosition.
// Both reflected functions are required: preserving the live Y coordinate
// avoids replacing the layout with a guessed absolute position.
// UE 5.6's FVector2D stores two doubles, hence ParmsSize 16.
bool ResolveSlotBindings(Context& context) noexcept {
    if (context.process_event == nullptr || !ObjectFindReady(context.objects)) return false;
    const std::uint64_t generation = context.objects->generation(context.objects->user);
    if (generation == 0 || context.update_tick < context.next_slot_binding_tick) return false;
    if (context.slot_set_position_function != 0 &&
        context.slot_set_position_generation == generation) {
        return true;
    }
    context.next_slot_binding_tick = context.update_tick + kObjectRescanInterval;
    context.slot_set_position_function = 0;
    context.slot_get_position_function = 0;
    context.slot_set_position_generation = 0;
    std::uintptr_t set_position{};
    if (!FindUFunctionObject(
            context, "/Script/UMG.CanvasPanelSlot.SetPosition", 1, 16, 0xFFFF,
            set_position)) {
        return false;
    }
    std::uintptr_t get_position{};
    if (!FindUFunctionObject(
            context, "/Script/UMG.CanvasPanelSlot.GetPosition", 1, 16, 0,
            get_position)) {
        return false;
    }
    context.slot_set_position_function = set_position;
    context.slot_get_position_function = get_position;
    context.slot_set_position_generation = generation;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "FakeUID: CanvasPanelSlot GetPosition/SetPosition resolved");
    return true;
}

// The original value starts at X=43 beside TextBlock_90. When that sibling is
// cleared, move only X to 1 while retaining the widget's live Y coordinate.
// Revert restores X=43. GetPosition also makes the operation idempotent, so a
// periodic verification pass performs no Slate mutation at steady state.
void AlignRoleIdSlot(
    Context& context, const std::uintptr_t widget, const bool prefix_hidden) noexcept {
    if (widget == 0 || context.process_event == nullptr) return;
    if (!ResolveSlotBindings(context)) {
        if (!context.slot_binding_failed_emitted) {
            context.slot_binding_failed_emitted = true;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: CanvasPanelSlot.SetPosition unavailable");
        }
        return;
    }
    __try {
        const auto slot = *reinterpret_cast<std::uintptr_t*>(
            widget + fake_uid_profile::kWidgetSlotOffset);
        if (slot == 0) {
            if (!context.slot_binding_failed_emitted) {
                context.slot_binding_failed_emitted = true;
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    "FakeUID: RoleID slot pointer is null");
            }
            return;
        }
        double current[2]{};
        if (!InvokeProcessEvent(
                context, slot, context.slot_get_position_function, current)) {
            return;
        }
        const double target_x = prefix_hidden ? 1.0 : 43.0;
        if (current[0] == target_x) return;
        double parameters[2] = {target_x, current[1]};
        const bool invoked = InvokeProcessEvent(
            context, slot, context.slot_set_position_function, parameters);
        if (!invoked) {
            if (!context.slot_binding_failed_emitted) {
                context.slot_binding_failed_emitted = true;
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    "FakeUID: CanvasPanelSlot.SetPosition invoke failed");
            }
            return;
        }
        if (!context.slot_moved_diagnostic_emitted) {
            context.slot_moved_diagnostic_emitted = true;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                prefix_hidden
                    ? "FakeUID: RoleID value aligned to X=1"
                    : "FakeUID: RoleID value restored to X=43");
        }
    } __except (1) {
        if (!context.slot_binding_failed_emitted) {
            context.slot_binding_failed_emitted = true;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: CanvasPanelSlot.SetPosition invoke raised an exception");
        }
    }
}

bool ResolveVisibilityBinding(Context& context) noexcept {
    if (context.process_event == nullptr || !ObjectFindReady(context.objects)) return false;
    const std::uint64_t generation = context.objects->generation(context.objects->user);
    if (generation == 0 || context.update_tick < context.next_visibility_binding_tick) {
        return false;
    }
    if (context.set_visibility_function != 0 &&
        context.set_visibility_generation == generation) {
        return true;
    }

    context.next_visibility_binding_tick = context.update_tick + kObjectRescanInterval;
    context.set_visibility_function = 0;
    context.set_visibility_generation = 0;
    AnomalyGenerationHandleV1 visibility_handle{};
    const AnomalyStatusV1 find_status = context.objects->find_exact(
        context.objects->user,
        anomaly::sdk::StringView("/Script/UMG.Widget:SetVisibility"),
        &visibility_handle);
    if (find_status.code != ANOMALY_STATUS_V1_OK) {
        if (context.visibility_failure_stage != 1 ||
            context.visibility_failure_status != find_status.code) {
            context.visibility_failure_stage = 1;
            context.visibility_failure_status = find_status.code;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                std::string("FakeUID: SetVisibility find_exact failed status=") +
                    std::to_string(find_status.code));
        }
        return false;
    }
    std::uintptr_t visibility_function{};
    if (!ResolveObjectAddress(context, visibility_handle, visibility_function)) {
        if (context.visibility_failure_stage != 2) {
            context.visibility_failure_stage = 2;
            context.visibility_failure_status = 0;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: SetVisibility object address resolution failed");
        }
        return false;
    }
    std::uintptr_t func{};
    std::uintptr_t class_object{};
    std::uintptr_t outer_object{};
    std::uint8_t num_parms{};
    std::uint16_t parms_size{};
    std::uint16_t return_value{};
    if (!ReadVisibilityLayout(
            visibility_function, func, class_object, outer_object,
            num_parms, parms_size, return_value) ||
        class_object == 0 || outer_object == 0 || func == 0 ||
        num_parms != 1 || parms_size != 1 || return_value != 0xFFFF) {
        if (context.visibility_failure_stage != 3) {
            context.visibility_failure_stage = 3;
            context.visibility_failure_status = 0;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: SetVisibility UFunction layout validation failed (class=" +
                    std::to_string(class_object) + " outer=" + std::to_string(outer_object) +
                    " func=" + std::to_string(func) + " numParms=" + std::to_string(num_parms) +
                    " parmsSize=" + std::to_string(parms_size) +
                    " returnValue=" + std::to_string(return_value) + ")");
        }
        return false;
    }
    // ProcessEvent takes the UFunction object (not its exec pointer): the
    // earlier version stored `func` here and every invoke crashed inside SEH.
    context.set_visibility_function = visibility_function;
    context.set_visibility_generation = generation;
    if (context.visibility_failure_stage != 0) {
        context.visibility_failure_stage = 0;
        context.visibility_failure_status = 0;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: SetVisibility binding resolved");
    }
    return true;
}

ApplyResult ApplyToWidget(
    Context& context, const AnomalyGenerationHandleV1 handle,
    const SettingsSnapshot& settings, const std::uint64_t revision) {
    bool prefix = false;
    bool tracked = false;
    for (std::size_t index = 0; index < context.widget_count; ++index) {
        const auto& candidate = context.widgets[index];
        if (candidate.handle.id == handle.id &&
            candidate.handle.generation == handle.generation) {
            prefix = candidate.prefix;
            tracked = true;
            break;
        }
    }
    if (!tracked) return ApplyResult::Failed;
    if (prefix && !context.prefix_apply_attempt_diagnostic_emitted) {
        context.prefix_apply_attempt_diagnostic_emitted = true;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: TextBlock_90 apply attempt started");
    }

    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (!ObjectsReady(context.objects) ||
        context.objects->snapshot_by_handle(
            context.objects->user, handle, &snapshot).code != ANOMALY_STATUS_V1_OK ||
        (!prefix && snapshot.name_id != context.target_name_id)) {
        if (prefix && context.prefix_apply_failure_stage != 1U) {
            context.prefix_apply_failure_stage = 1U;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: TextBlock_90 apply failed at snapshot");
        } else if (!prefix) {
            LogValueApplyFailure(context, revision, 1, "snapshot/name mismatch");
        }
        return ApplyResult::Failed;
    }
    std::uintptr_t widget{};
    if (prefix) {
        if (!ResolveTextBlockAddress(context, handle, widget)) {
            if (context.prefix_apply_failure_stage != 2U) {
                context.prefix_apply_failure_stage = 2U;
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    "FakeUID: TextBlock_90 apply failed at live handle");
            }
            return ApplyResult::Failed;
        }
    } else if (!ResolveWidgetAddress(context, handle, widget)) {
        LogValueApplyFailure(context, revision, 2, "live handle resolution");
        return ApplyResult::Failed;
    }
    if (prefix) {
        // Re-verify against the live tree before any write: tracking decisions
        // age quickly around HUD rebuilds, and a recycled outer address must
        // never turn a foreign label into a clear+collapse victim. Entries
        // captured before any tree was anchored stay unwritten until the scan
        // validates a fresh value TextBlock.
        if (!IsRoleIdPrefixInstance(context, widget)) {
            if (context.prefix_apply_failure_stage != 3U) {
                context.prefix_apply_failure_stage = 3U;
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    "FakeUID: TextBlock_90 apply failed at RoleID structure");
            }
            return ApplyResult::Failed;
        }
        // Do not touch Visibility or CanvasPanelSlot here: both participate in
        // Slate layout and were present in the reproducible Apply+M crash.
        // Clearing only this exact prefix TextBlock removes the visible label
        // without changing the widget tree's layout structure.
        std::wstring current_prefix;
        if (!ReadWidgetText(context, widget, current_prefix)) {
            return ApplyResult::Deferred;
        }
        if (settings.enabled && !current_prefix.empty() &&
            LooksLikeUidPrefix(current_prefix)) {
            context.original_prefix = current_prefix;
        }
        const bool hide_prefix = settings.enabled && settings.hide_prefix;
        const std::wstring_view target_prefix = hide_prefix
            ? kHiddenPrefixText
            : std::wstring_view(context.original_prefix);
        if (current_prefix == target_prefix) return ApplyResult::Applied;
        const wchar_t* const prefix_text = hide_prefix
            ? kHiddenPrefixText.data()
            : context.original_prefix.c_str();
        if (!SetWidgetText(context, widget, prefix_text)) {
            return ApplyResult::Deferred;
        }
        std::wstring readback_prefix;
        if (!ReadWidgetText(context, widget, readback_prefix) ||
            readback_prefix != target_prefix) {
            // SetText can return while the UTextBlock still exposes its
            // previous cached text during widget construction. Do not mark
            // the prefix applied until the game-side FText readback confirms
            // that the visible label has actually changed.
            return ApplyResult::Deferred;
        }
        if (hide_prefix && !context.prefix_cleared_diagnostic_emitted) {
            context.prefix_apply_failure_stage = 0U;
            context.prefix_cleared_diagnostic_emitted = true;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: TextBlock_90 hidden via zero-width SetText (layout untouched)");
        } else if (!hide_prefix) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: TextBlock_90 prefix restored via SetText");
        }
        return ApplyResult::Applied;
    }

    std::wstring current_value;
    if (!ReadWidgetText(context, widget, current_value)) {
        LogValueApplyFailure(context, revision, 3, "TextToString");
        return ApplyResult::Deferred;
    }
    UnrealString current{
        current_value.data(),
        static_cast<std::int32_t>(current_value.size() + 1U),
        static_cast<std::int32_t>(current_value.size() + 1U)};
    std::wstring replacement;
    std::uint64_t detected{};
    bool changed{};
    std::wstring restored_uid;
    std::wstring_view target_uid = settings.display_wide;
    if (!settings.enabled) {
        const std::uint64_t known_uid =
            context.detected_uid.load(std::memory_order_acquire);
        if (known_uid == 0) {
            return ApplyResult::Applied;
        }
        restored_uid = std::to_wstring(known_uid);
        target_uid = restored_uid;
    }
    const bool built = BuildValueReplacement(
        &current, target_uid, replacement, detected, changed);
    if (!built) {
        LogValueApplyFailure(context, revision, 4, "replacement construction");
        return ApplyResult::Deferred;
    }
    if (settings.enabled && detected != 0 && changed) {
        RecordDetectedUid(context, detected);
    }
    if (!changed) {
        AlignRoleIdSlot(
            context, widget, settings.enabled && settings.hide_prefix);
        LogValueApplySuccess(context, revision);
        return ApplyResult::Applied;
    }

    if (!SetWidgetText(context, widget, replacement.c_str())) {
        LogValueApplyFailure(context, revision, 5, "reflected SetText");
        return ApplyResult::Deferred;
    }
    std::wstring readback;
    if (!ReadWidgetText(context, widget, readback) || readback != replacement) {
        LogValueApplyFailure(context, revision, 6, "post-write readback mismatch");
        return ApplyResult::Deferred;
    }
    AlignRoleIdSlot(
        context, widget, settings.enabled && settings.hide_prefix);
    LogValueApplySuccess(context, revision);
    return ApplyResult::Applied;
}

AnomalyStatusV1 ANOMALY_CALL Start(void* user);

void ANOMALY_CALL Update(void* user, double) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || context->stop_completed) return;
    try {
        ++context->update_tick;
        if (!context->runtime_ready) {
            if (context->update_tick >= context->next_runtime_binding_tick) {
                context->next_runtime_binding_tick =
                    context->update_tick + kRuntimeBindingRetryInterval;
                static_cast<void>(Start(context));
            }
            if (!context->runtime_ready) return;
        }

        static_cast<void>(ResolveTextWriteBindings(*context));
        const auto current_settings = ReadSettings(*context);
        if (!current_settings) return;
        const std::uint64_t current_revision =
            context->settings_revision.load(std::memory_order_acquire);
        if (context->scan_active ||
            context->update_tick % kAnchorVerifyInterval == 0) {
            ValidateRoleIDAnchor(*context);
        }
        ScanForWidgets(*context);
        static_cast<void>(EnsureTextHookSnapshot(
            *context, *current_settings, current_revision));
        for (std::size_t index = 0; index < context->widget_count;) {
            auto& widget = context->widgets[index];
            const bool revision_pending = widget.applied_revision < current_revision;
            // Once a widget has been applied, do not periodically call
            // TextToString on its embedded FText. During world transitions a
            // UObject can still pass the serial/vtable checks while its FText
            // payload is already being torn down; TextToString then follows a
            // stale text-data vtable and can jump to an invalid address. The
            // native SetText hook maintains the steady-state value, while
            // revision-pending, newly tracked, and deferred widgets still
            // take the normal apply path below.
            const bool retry_due =
                widget.retry_tick == 0 || context->update_tick >= widget.retry_tick;
            if (!revision_pending || !retry_due) {
                ++index;
                continue;
            }
            const ApplyResult result = ApplyToWidget(
                *context, widget.handle, *current_settings, current_revision);
            if (result == ApplyResult::Applied) {
                widget.applied_revision = current_revision;
                widget.retry_tick = 0;
                ++index;
            } else if (result == ApplyResult::Deferred) {
                widget.retry_tick = context->update_tick + kWidgetApplyRetryInterval;
                ++index;
            } else {
                const bool was_value_widget = !widget.prefix;
                const std::uint32_t encoded_index =
                    static_cast<std::uint32_t>(widget.handle.id);
                if (encoded_index != 0) {
                    context->recovery_anchor_index = encoded_index - 1U;
                }
                context->widgets[index] = context->widgets[--context->widget_count];
                context->rescan_requested.store(true, std::memory_order_release);
                if (was_value_widget) {
                    // Several RoleID layers can coexist during HUD refreshes.
                    // Removing one stale value widget must not invalidate the
                    // anchor owned by another still-live layer.
                    ValidateRoleIDAnchor(*context);
                }
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
    window.flags = 0;
    // v2 resets the old persisted 240px height/constraints. Window persistence
    // stores the original constraint set, so keeping the old ID would ignore
    // the enlarged limits even after a plugin hot reload.
    window.id = anomaly::sdk::StringView("fake-uid-settings-v2");
    const std::string title = context.localizer.Text("window.title", "Custom UID");
    window.title = anomaly::sdk::StringView(title);
    window.initial_width = 300.0F;
    window.initial_height = 340.0F;
    window.minimum_width = 260.0F;
    window.minimum_height = 320.0F;
    window.maximum_width = 520.0F;
    window.maximum_height = 650.0F;
    window.default_open = 1;
    const AnomalyStatusV1 status = context.window->register_window(
        context.window->user, &window, &context.window_handle);
    return status.code == ANOMALY_STATUS_V1_OK && context.window_handle.id != 0;
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
    context->core = Query<AnomalyCoreServiceV1>(
        host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    context->scheduler = Query<AnomalySchedulerServiceV1>(
        host, ANOMALY_SCHEDULER_SERVICE_V1_ID,
        ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    context->hook = Query<AnomalyHookServiceV1>(
        host, ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION);
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
        !NamesReady(context->names) || !HookReady(context->hook)) {
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                      "required services are unavailable");
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
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin context is invalid");
    }
    if (context->runtime_ready) return anomaly::sdk::Ok();
    context->start_attempted = true;

    static_cast<void>(EnsureWindow(*context));

    auto& bindings = context->runtime_bindings;
    std::string missing;
    const auto AddMissing = [&missing](const std::string_view name) {
        if (!missing.empty()) missing += ',';
        missing.append(name);
    };
    const auto ResolveDirect = [&](
                                   const std::string_view name,
                                   const std::string_view pattern,
                                   std::uintptr_t& address) {
        if (address == 0 && !Resolve(*context->signature, pattern, address)) {
            AddMissing(name);
        }
    };
    ResolveDirect("SetText", fake_uid_profile::kSetTextPattern, bindings.set_text);
    ResolveDirect(
        "AssignString", fake_uid_profile::kAssignStringPattern,
        bindings.assign_string);
    ResolveDirect(
        "FreeString", fake_uid_profile::kFreeStringPattern,
        bindings.free_string);
    ResolveDirect(
        "TextToString", fake_uid_profile::kTextToStringPattern,
        bindings.text_to_string);
    ResolveDirect(
        "GObjects", fake_uid_profile::kGObjectsPattern,
        bindings.gobjects_accessor);
    if (bindings.process_event == 0) {
        std::uintptr_t process_event_match{};
        if (!Resolve(
                *context->signature, fake_uid_profile::kProcessEventPattern,
                process_event_match) ||
            process_event_match <= fake_uid_profile::kProcessEventMatchOffset) {
            AddMissing("ProcessEvent");
        } else {
            bindings.process_event =
                process_event_match - fake_uid_profile::kProcessEventMatchOffset;
        }
    }
    if (!missing.empty()) {
        context->runtime_pending_emitted = true;
        if (missing != context->runtime_missing_bindings) {
            context->runtime_missing_bindings = missing;
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: runtime bindings pending missing=" + missing +
                    "; cached matches retained; retrying slowly in Update");
        }
        return anomaly::sdk::Ok();
    }
    context->runtime_missing_bindings.clear();
    std::int32_t registry_displacement{};
    std::memcpy(
        &registry_displacement,
        reinterpret_cast<const void*>(
            bindings.gobjects_accessor + fake_uid_profile::kGObjectsResolveOffset),
        sizeof(registry_displacement));
    context->text_field_offset = fake_uid_profile::kTextFieldOffset;
    if (context->text_field_offset == 0 || context->text_field_offset > 4096) {
        if (!context->runtime_pending_emitted) {
            context->runtime_pending_emitted = true;
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "FakeUID: runtime layout pending; retrying in Update");
        }
        return anomaly::sdk::Ok();
    }
    context->object_registry =
        bindings.gobjects_accessor + fake_uid_profile::kGObjectsInstructionSize +
        registry_displacement + fake_uid_profile::kGObjectsAddend;
    context->free_string = reinterpret_cast<FreeStringFn>(bindings.free_string);
    context->assign_string =
        reinterpret_cast<AssignStringFn>(bindings.assign_string);
    context->text_to_string =
        reinterpret_cast<TextToStringFn>(bindings.text_to_string);
    context->set_text = reinterpret_cast<SetTextFn>(bindings.set_text);
    context->process_event =
        reinterpret_cast<ProcessEventFn>(bindings.process_event);
    static_cast<void>(EnsureSetTextHook(*context));

    // Restore the persisted prefix family so the scan can re-track the exact
    // widget even when its text is already empty.
    // FName indexes reshuffle across sessions: the persisted value may only
    // arm when it still resolves to the TextBlock_90 family in the live
    // table; otherwise discovery recaptures the label by content instead.
    const std::uint32_t persisted_prefix_name_id =
        context->persisted_prefix_name_id.load(std::memory_order_acquire);
    if (persisted_prefix_name_id != 0 && context->names != nullptr &&
        PrefixNameIdPlausible(*context->names, persisted_prefix_name_id)) {
        context->target_prefix_name_id = persisted_prefix_name_id;
    } else {
        context->persisted_prefix_name_id.store(0, std::memory_order_release);
    }

    // UFunction lookup is Game-thread-only work. Start can run on the Lifecycle
    // worker, so Update resolves it after activation.
    if (!context->visibility_wait_diagnostic_emitted) {
        context->visibility_wait_diagnostic_emitted = true;
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: runtime ready; waiting for Game-thread widget binding");
    }
    context->runtime_ready = true;
    if (context->runtime_pending_emitted) {
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "FakeUID: runtime bindings recovered");
    }
    return anomaly::sdk::Ok();
}

void DrawText(const AnomalyUiServiceV1& ui, const std::string_view value) {
    ui.text(ui.user, anomaly::sdk::StringView(value));
}

bool Button(const AnomalyUiServiceV1& ui, const std::string_view label) {
    return ui.button(ui.user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0;
}

std::string LocalizeUiStatus(Context& context, const std::string_view status) {
    if (status == "UID must contain 1-256 single-line Unicode characters") {
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
    auto active_settings = ReadSettings(context);
    if (active_settings &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::checkbox)>(
            &ui, offsetof(AnomalyUiServiceV1, checkbox)) &&
        ui.checkbox != nullptr) {
        int hide_prefix = active_settings->hide_prefix ? 1 : 0;
        const std::string hide_prefix_label = context.localizer.Label(
            "field.hide_prefix", "Hide UID: prefix", "hide-uid-prefix");
        if (ui.checkbox(
                ui.user, anomaly::sdk::StringView(hide_prefix_label),
                &hide_prefix) != 0) {
            std::string error;
            const bool applied = ApplySettings(
                context, active_settings->enabled, hide_prefix != 0,
                active_settings->display_uid, error);
            context.ui_status = applied
                ? (error.empty()
                        ? context.localizer.Text(
                              "status.applied_saving", "Applied; saving")
                        : LocalizeUiStatus(context, error))
                : LocalizeUiStatus(context, error);
            active_settings = ReadSettings(context);
        }
    }
    const std::string display_uid = context.localizer.Label(
        "field.display_uid", "Display UID", "display-uid");
    static_cast<void>(ui.input_text(
        ui.user, anomaly::sdk::StringView(display_uid),
        context.editor.data(), context.editor.size(), ANOMALY_UI_TEXT_INPUT_V1_NONE));

    const auto value = std::string_view(context.editor.data());
    const std::string apply = context.localizer.Label("action.apply", "Apply", "apply");
    if (Button(ui, apply)) {
        std::string error;
        active_settings = ReadSettings(context);
        const bool applied = ApplySettings(
            context, true,
            active_settings == nullptr || active_settings->hide_prefix,
            value, error);
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
                context, false, settings->hide_prefix,
                settings->display_uid, error);
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
    if (!ReleaseSetTextHook(*context)) {
        return Status(ANOMALY_STATUS_V1_TIMEOUT,
                      "Custom UID SetText hook did not release");
    }
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
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.1.9"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
