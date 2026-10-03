#include "anomaly/sdk/cpp.hpp"

#include <Windows.h>
#include <shobjidl.h>

#include <nlohmann/json.hpp>

#include "plugins/common/localization.hpp"
#include "plugins/HiFiVehicleMusic/audio_engine.hpp"
#include "plugins/HiFiVehicleMusic/hifi_vehicle_music_profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace profile = hifi_vehicle_music_profile;
using hifi_vehicle_music::AudioEngine;
using hifi_vehicle_music::Backend;
using hifi_vehicle_music::EngineSettings;
using hifi_vehicle_music::EngineStatus;

constexpr std::string_view kSettingsSchemaId = "hifi-vehicle-music-settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
// Coalesces slider drags and repeated clicks into one config write.
constexpr std::uint32_t kPersistDelayMs = 500;
constexpr std::string_view kSettingsSchema = R"json(
{
  "type":"object",
  "additionalProperties":false,
  "properties":{
    "enabled":{"type":"boolean"},
    "backend":{"enum":["wasapi-exclusive","wasapi-shared","directsound","asio"]},
    "device":{"type":"string","maxLength":512},
    "bufferMs":{"type":"integer","minimum":2,"maximum":500},
    "volume":{"type":"number","minimum":0,"maximum":1},
    "musicFolder":{"type":"string","maxLength":4096}
  }
}
)json";

constexpr std::array<std::pair<Backend, std::string_view>, 4> kBackendKeys{{
    {Backend::WasapiExclusive, "wasapi-exclusive"},
    {Backend::WasapiShared, "wasapi-shared"},
    {Backend::DirectSound, "directsound"},
    {Backend::Asio, "asio"},
}};
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kBackendLabels{{
    {"backend.wasapi_exclusive", "WASAPI exclusive"},
    {"backend.wasapi_shared", "WASAPI shared"},
    {"backend.directsound", "DirectSound"},
    {"backend.asio", "ASIO"},
}};

// Hooks before kCoreHookCount are required for the takeover; the album hooks
// after it add the library to the in-game music list and are all-or-nothing.
enum HookIndex : std::size_t {
    kPost, kStop, kPause, kResume, kSetPlayerType, kEndGetOff, kCoreHookCount,
    kFindRow = kCoreHookCount, kForEachRow, kAlbumRow, kAlbumForEach, kOwnedCopy, kReGenerate,
    kSetCurrent, kItemRefresh, kVehiclePanel, kResolveCurrent, kSyncCurrent, kPageList, kMusicEnd,
    kHookCount
};

using PostFn = std::int64_t(__fastcall*)(void*, void*, void*, float);
// Also EndGetOffVehicle(this, bool).
using StopFn = void(__fastcall*)(void*, std::uint8_t);
using SetPlayerTypeFn = void(__fastcall*)(void*, std::uint32_t);
// Also ReGenerateNewMusicListIDs(this).
using VoidFn = void(__fastcall*)(void*);
using FTextFromStringFn = void*(__fastcall*)(void*, const wchar_t*);
using FindRowFn = void*(__fastcall*)(void*, std::uint64_t, const wchar_t*, char);
using ForEachFn = void(__fastcall*)(void*, const wchar_t*, void*);
using AlbumRowFn = void*(__fastcall*)(void*, const std::uint64_t*, std::uint8_t);
using AddUniqueFn = std::int32_t(__fastcall*)(void*, const std::uint64_t*);
using OwnedCopyFn = void*(__fastcall*)(void*, void*);
using GameInstanceFn = void*(__fastcall*)();
using SetCurrentFn = void(__fastcall*)(void*, std::uint64_t);
// RefreshItemStates, ResolveCurrentMusicListID and SyncCurrentMusicListID.
using SelfFn = std::uintptr_t(__fastcall*)(void*);
using VehiclePanelFn = char(__fastcall*)(void*, void*, char);
using SoundSubsystemFn = void*(__fastcall*)(void*);
// FString as the game passes it: {TCHAR* data, int32 num (with NUL), int32 max}.
struct GameString final {
    const wchar_t* data;
    std::int32_t num;
    std::int32_t max;
};
using ImportTextureFn = void*(__fastcall*)(const GameString*);
using SetItemFlagsFn = std::uint8_t(__fastcall*)(void*, std::int32_t);

// ForeachRow callback: fn(user, element, row), element = {FName, row*, next}.
using RowVisitFn = void(__fastcall*)(void*, void*, void*);
struct RowCallback final {
    RowVisitFn fn;
    void* user;
};
struct RowElement final {
    std::uint64_t name;
    void* row;
    std::int32_t next;
};

enum class HookState { Waiting, Installed, SignatureMissing, HookFailed };

// Why the last PostPlayerMusicSound was or was not taken over.
enum class EventDecision { Replaced, Disabled, NotVehicle, NoTrack, GameSong };

// Synthetic rows of the "HiFi Library" album, guarded by Context::album_mutex.
// Each row starts as a copy of a real row of the same table (vtable, event,
// cover) and then gets its own title, sort index and album.
struct AlbumState final {
    std::uint32_t base{};  // ComparisonIndex of a real music row; 0 until one is seen
    const std::byte* music_template{};
    const std::byte* album_template{};
    std::uint64_t generation{(std::numeric_limits<std::uint64_t>::max)()};
    // Rows are never freed: the game keeps row pointers in its UI objects.
    std::vector<std::byte*> songs;
    std::byte* album{};
    // Every listed game song (IsList set, IsHidden clear); owned while the game
    // reads the owned ids, so locked game songs show as unlocked too.
    std::vector<std::uint64_t> game_songs;
    // Library generation whose cover image was last applied to the album row.
    std::uint64_t cover_generation{(std::numeric_limits<std::uint64_t>::max)()};
    std::wstring cover;  // image the album row shows; empty = the template's cover
};

struct Settings final {
    bool enabled{true};
    Backend backend{Backend::WasapiExclusive};
    std::string device;
    std::uint32_t buffer_ms{20};
    float volume{1.0F};
    std::string folder;  // absolute UTF-8 path chosen by the user; empty = none
};

struct Context final {
    const AnomalyCoreServiceV1* core{};
    const AnomalyConfigServiceV1* config{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyHookServiceV1* hook{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalySchedulerServiceV1* scheduler{};
    // Optional: a per-frame Game-thread tick that keeps the game's current
    // song id on the library song the engine actually plays.
    const AnomalyUe5AhudServiceV1* ahud{};
    AnomalyGenerationHandleV1 ahud_subscription{};
    anomaly::plugins::Localizer localizer;
    AnomalyGenerationHandleV1 settings_schema{};
    AudioEngine engine;

    std::mutex mutex;
    Settings settings;
    bool settings_dirty{};
    bool stopped{};
    // At most one delayed persist task is queued; it clears the flag before it
    // snapshots the settings, so later edits queue the next write.
    AnomalyGenerationHandleV1 persist_task{};
    std::atomic<bool> persist_pending{};
    std::atomic<std::uint32_t> save_state{};  // 0 idle, 1 pending, 2 saved, 3 failed
    HookState hook_state{HookState::Waiting};
    std::string hook_detail;
    bool has_event{};
    std::string last_event;
    EventDecision last_decision{};
    std::array<AnomalyGenerationHandleV1, kHookCount> hooks{};
    // Not `mutex`: the album hooks call into the game while holding it, and the
    // game may re-enter a hooked lookup on the same thread.
    std::recursive_mutex album_mutex;
    AlbumState album;

    // Folder picker. The dialog runs on its own STA thread so Draw never blocks.
    std::thread picker;
    std::atomic<bool> picker_busy{};
    std::optional<std::string> picked_folder;
};

// Published to the detours. A detour only touches the context while it holds a
// callback lease, and Stop releases every hook before the context goes away.
struct HookSlot final {
    std::atomic<std::uint64_t> id{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uintptr_t> original{0};
};
std::atomic<const AnomalyHookServiceV1*> g_hook_api{nullptr};
std::array<HookSlot, kHookCount> g_slots;
std::atomic<Context*> g_context{nullptr};
std::atomic<bool> g_enabled{true};
std::atomic<std::uintptr_t> g_ftext_from_string{0};
std::atomic<std::uintptr_t> g_game_instance{0};
std::atomic<std::uintptr_t> g_add_unique{0};
std::atomic<std::uintptr_t> g_sound_subsystem{0};
// Album cover helpers; all three or none.
std::atomic<std::uintptr_t> g_import_texture{0};
std::atomic<std::uintptr_t> g_set_item_flags{0};
std::atomic<std::uintptr_t> g_object_array{0};
std::atomic<std::uintptr_t> g_serial_counter{0};
std::atomic<std::uintptr_t> g_set_current_id{0};
HMODULE g_plugin_module{};

AnomalyStatusV1 Status(const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

#define HIFI_HAS(Struct, field) \
    (HasField<Struct, decltype(Struct::field)>(service, offsetof(Struct, field)) && \
     service->field != nullptr)

bool ConfigReady(const AnomalyConfigServiceV1* service) noexcept {
    return HIFI_HAS(AnomalyConfigServiceV1, write_atomic) &&
        service->register_schema != nullptr && service->read != nullptr;
}

bool CoreReady(const AnomalyCoreServiceV1* service) noexcept {
    return HIFI_HAS(AnomalyCoreServiceV1, read_memory);
}

bool SignatureReady(const AnomalySignatureServiceV1* service) noexcept {
    return HIFI_HAS(AnomalySignatureServiceV1, resolve);
}

bool HookReady(const AnomalyHookServiceV1* service) noexcept {
    return HIFI_HAS(AnomalyHookServiceV1, end_callback) && service->create != nullptr &&
        service->release != nullptr && service->begin_callback != nullptr;
}

bool SchedulerReady(const AnomalySchedulerServiceV1* service) noexcept {
    return HIFI_HAS(AnomalySchedulerServiceV1, cancel) && service->schedule != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* service) noexcept {
    return HIFI_HAS(AnomalyUe5NamesServiceV1, resolve_utf8);
}

bool AhudReady(const AnomalyUe5AhudServiceV1* service) noexcept {
    return HIFI_HAS(AnomalyUe5AhudServiceV1, unsubscribe) && service->subscribe != nullptr;
}

bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return HIFI_HAS(AnomalyUiServiceV1, same_line) && service->begin_window != nullptr &&
        service->end_window != nullptr && service->text != nullptr &&
        service->checkbox != nullptr && service->slider_float != nullptr &&
        service->separator != nullptr && service->input_uint32 != nullptr &&
        service->input_text != nullptr && service->button != nullptr &&
        service->button_enabled != nullptr;
}

#undef HIFI_HAS

std::wstring Utf8ToWide(const std::string_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
        required, nullptr, nullptr);
    return result;
}

bool IsAbsoluteFolder(const std::string_view folder) {
    const std::wstring wide = Utf8ToWide(folder);
    return !wide.empty() && std::filesystem::path(wide).is_absolute();
}

EngineSettings ToEngineSettings(const Settings& settings) {
    EngineSettings result;
    result.backend = settings.backend;
    result.device = settings.device;
    result.buffer_ms = settings.buffer_ms;
    result.volume = settings.volume;
    result.folder = Utf8ToWide(settings.folder);
    return result;
}

void Log(const Context& context, const std::uint32_t level, const std::string& message) noexcept {
    if (context.core->log == nullptr) return;
    try {
        const std::string line = "HiFiVehicleMusic: " + message;
        context.core->log(context.core->user, level, anomaly::sdk::StringView(line));
    } catch (...) {
    }
}

// ---- settings ---------------------------------------------------------------

void ReadSettings(Context& context) {
    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = context.config->read(
        context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code == ANOMALY_STATUS_V1_OK && schema_version == kSettingsSchemaVersion &&
        size > 0 && size <= 64U * 1024U) {
        std::string document(size, '\0');
        std::size_t copied = document.size();
        const AnomalyStatusV1 read_status = context.config->read(
            context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
            {reinterpret_cast<std::uint8_t*>(document.data()), document.size()}, &copied);
        if (read_status.code == ANOMALY_STATUS_V1_OK && copied <= document.size()) {
            document.resize(copied);
            const nlohmann::json json = nlohmann::json::parse(document, nullptr, false);
            if (json.is_object()) {
                Settings& settings = context.settings;
                settings.enabled = json.value("enabled", settings.enabled);
                const std::string backend = json.value("backend", std::string{});
                for (const auto& [value, key] : kBackendKeys) {
                    if (backend == key) settings.backend = value;
                }
                settings.device = json.value("device", settings.device);
                settings.buffer_ms = std::clamp(json.value("bufferMs", settings.buffer_ms), 2U, 500U);
                settings.volume = std::clamp(json.value("volume", settings.volume), 0.0F, 1.0F);
                // Only absolute paths: the package directory changes per load.
                const std::string folder = json.value("musicFolder", std::string{});
                if (IsAbsoluteFolder(folder)) settings.folder = folder;
            }
        }
    }
    g_enabled.store(context.settings.enabled, std::memory_order_release);
}

bool SaveSettings(Context& context) {
    Settings settings;
    {
        std::scoped_lock lock(context.mutex);
        if (!context.settings_dirty) return true;
        settings = context.settings;
        // Cleared at snapshot time so an edit made during the write stays dirty.
        context.settings_dirty = false;
    }
    std::string_view backend;
    for (const auto& [value, key] : kBackendKeys) {
        if (settings.backend == value) backend = key;
    }
    const std::string document = nlohmann::json{
        {"enabled", settings.enabled},
        {"backend", backend},
        {"device", settings.device},
        {"bufferMs", settings.buffer_ms},
        {"volume", settings.volume},
        {"musicFolder", settings.folder},
    }.dump();
    const AnomalyStatusV1 status = context.config->write_atomic(
        context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(document));
    if (status.code == ANOMALY_STATUS_V1_OK) return true;
    std::scoped_lock lock(context.mutex);
    context.settings_dirty = true;
    return false;
}

void ANOMALY_CALL PersistSettingsTask(void* user, AnomalyGenerationHandleV1) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    try {
        context->persist_pending.store(false, std::memory_order_release);
        const bool saved = SaveSettings(*context);
        context->save_state.store(saved ? 2U : 3U, std::memory_order_release);
    } catch (...) {
        context->save_state.store(3U, std::memory_order_release);
    }
}

// Caller marks the settings dirty under the mutex first.
void ScheduleSettingsPersist(Context& context) noexcept {
    context.save_state.store(1U, std::memory_order_release);
    if (context.persist_pending.exchange(true, std::memory_order_acq_rel)) return;
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status = context.scheduler->schedule(
        context.scheduler->user, kPersistDelayMs, PersistSettingsTask, &context, &task);
    if (status.code != ANOMALY_STATUS_V1_OK || task.id == 0) {
        context.persist_pending.store(false, std::memory_order_release);
        context.save_state.store(3U, std::memory_order_release);
        return;
    }
    context.persist_task = task;
}

// ---- detours ------------------------------------------------------------------

bool BeginCallback(const HookIndex index, const AnomalyHookServiceV1*& api,
    AnomalyGenerationHandleV1& lease, std::uintptr_t& original) noexcept {
    api = g_hook_api.load(std::memory_order_acquire);
    const AnomalyGenerationHandleV1 hook{
        g_slots[index].id.load(std::memory_order_acquire),
        g_slots[index].generation.load(std::memory_order_acquire)};
    original = g_slots[index].original.load(std::memory_order_acquire);
    if (api == nullptr || hook.id == 0 || hook.generation == 0 || original == 0) return false;
    __try {
        return api->begin_callback(api->user, hook, &lease).code == ANOMALY_STATUS_V1_OK &&
            lease.id != 0;
    } __except (1) {
        return false;
    }
}

void EndCallback(const AnomalyHookServiceV1* api, const AnomalyGenerationHandleV1 lease) noexcept {
    __try {
        static_cast<void>(api->end_callback(api->user, lease));
    } __except (1) {
    }
}

std::int64_t CallPost(const std::uintptr_t original, void* self, void* event, void* list_id,
    const float position) noexcept {
    __try {
        return reinterpret_cast<PostFn>(original)(self, event, list_id, position);
    } __except (1) {
        return 0;
    }
}

void CallStop(const std::uintptr_t original, void* self, const std::uint8_t no_transition) noexcept {
    __try {
        reinterpret_cast<StopFn>(original)(self, no_transition);
    } __except (1) {
    }
}

void CallSetPlayerType(const std::uintptr_t original, void* self, const std::uint32_t type) noexcept {
    __try {
        reinterpret_cast<SetPlayerTypeFn>(original)(self, type);
    } __except (1) {
    }
}

void CallVoid(const std::uintptr_t original, void* self) noexcept {
    __try {
        reinterpret_cast<VoidFn>(original)(self);
    } __except (1) {
    }
}

template <typename T>
bool ReadValue(const Context& context, const std::uintptr_t address, T& value) noexcept {
    return context.core->read_memory(context.core->user, address,
               {reinterpret_cast<std::uint8_t*>(&value), sizeof(value)}).code ==
        ANOMALY_STATUS_V1_OK;
}

std::uint64_t SongId(const AlbumState& album, const std::size_t index) noexcept {
    return album.base |
        (static_cast<std::uint64_t>(profile::kSongNumberBase + index + 1U) << 32U);
}

std::uint64_t AlbumId(const AlbumState& album) noexcept {
    return album.base | (static_cast<std::uint64_t>(profile::kAlbumNumber) << 32U);
}

// Library index of a synthetic song id.
std::optional<std::size_t> SongIndex(const AlbumState& album, const std::uint64_t id) noexcept {
    const auto number = static_cast<std::uint32_t>(id >> 32U);
    if (album.base == 0 || static_cast<std::uint32_t>(id) != album.base ||
        number <= profile::kSongNumberBase ||
        number - profile::kSongNumberBase > album.songs.size()) {
        return std::nullopt;
    }
    return number - profile::kSongNumberBase - 1U;
}

// Returns true when the plugin plays the event itself and the original must be
// skipped.
bool TakeOverPost(Context& context, void* self, void* event, void* list_id,
    const float position) noexcept {
    try {
        const auto subsystem = reinterpret_cast<std::uintptr_t>(self);
        std::uint8_t player_type{};
        std::uint32_t name_id{};
        if (event == nullptr ||
            !ReadValue(context, subsystem + profile::kSubsystemPlayerTypeOffset, player_type) ||
            !ReadValue(context, reinterpret_cast<std::uintptr_t>(event) + profile::kObjectNameOffset,
                name_id)) {
            context.engine.Stop();
            return false;
        }
        std::array<char, 256> buffer{};
        std::size_t size = buffer.size();
        if (context.names->resolve_utf8(context.names->user, name_id, buffer.data(), &size).code !=
                ANOMALY_STATUS_V1_OK ||
            size > buffer.size()) {
            context.engine.Stop();
            return false;
        }
        std::string name(buffer.data(), size);
        while (!name.empty() && name.back() == '\0') name.pop_back();
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(),
            [](const char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });

        // Only songs of the library album are taken over; game songs play natively.
        std::optional<std::size_t> song;
        std::uint64_t id{};
        if (list_id != nullptr &&
            ReadValue(context, reinterpret_cast<std::uintptr_t>(list_id), id)) {
            std::scoped_lock lock(context.album_mutex);
            song = SongIndex(context.album, id);
        }
        EventDecision decision = EventDecision::Replaced;
        if (!g_enabled.load(std::memory_order_acquire)) {
            decision = EventDecision::Disabled;
        } else if (player_type != profile::kPlayerTypeVehicle) {
            decision = EventDecision::NotVehicle;
        } else if (!song) {
            decision = EventDecision::GameSong;
        } else if (!context.engine.CanPlay(key)) {
            decision = EventDecision::NoTrack;
        }
        const bool replace = decision == EventDecision::Replaced;
        bool changed = false;
        {
            std::scoped_lock lock(context.mutex);
            changed = !context.has_event || context.last_event != name ||
                context.last_decision != decision;
            context.has_event = true;
            context.last_event = name;
            context.last_decision = decision;
        }
        if (changed) {
            static constexpr std::array<std::string_view, 5> kReasons{
                "replaced", "skipped: disabled", "skipped: not vehicle music",
                "skipped: library empty or output failed", "skipped: game song"};
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "music event " + name + " (playerType " + std::to_string(player_type) + "): " +
                    std::string(kReasons[static_cast<std::size_t>(decision)]));
        }
        if (!replace) {
            // Non-vehicle music (e.g. after leaving the vehicle) must not
            // re-arm the grace period that a following vehicle Post cancels.
            if (decision == EventDecision::NoTrack) {
                context.engine.Stop();
            } else {
                context.engine.StopNow();
            }
            return false;
        }
        context.engine.Play(std::move(key), position, song);
        return true;
    } catch (...) {
        return false;
    }
}

void NotifyEngine(void (AudioEngine::*action)()) noexcept {
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context == nullptr) return;
    try {
        (context->engine.*action)();
    } catch (...) {
    }
}

std::int64_t __fastcall PostDetour(void* self, void* event, void* list_id, const float position) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kPost, api, lease, original)) return 0;
    std::int64_t result = 0;
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr && TakeOverPost(*context, self, event, list_id, position)) {
        // The skipped Post would have replaced the current Wwise music; stop it
        // through the original so the engine is not notified.
        const std::uintptr_t stop = g_slots[kStop].original.load(std::memory_order_acquire);
        if (stop != 0) CallStop(stop, self, 0);
    } else {
        result = CallPost(original, self, event, list_id, position);
    }
    EndCallback(api, lease);
    return result;
}

void __fastcall StopDetour(void* self, const std::uint8_t no_transition) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kStop, api, lease, original)) return;
    NotifyEngine(&AudioEngine::Stop);
    CallStop(original, self, no_transition);
    EndCallback(api, lease);
}

void __fastcall PauseDetour(void* self) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kPause, api, lease, original)) return;
    NotifyEngine(&AudioEngine::Pause);
    CallVoid(original, self);
    EndCallback(api, lease);
}

void __fastcall ResumeDetour(void* self) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kResume, api, lease, original)) return;
    NotifyEngine(&AudioEngine::Resume);
    CallVoid(original, self);
    EndCallback(api, lease);
}

// Leaving the vehicle switches the player type and stops the Wwise music
// directly, without StopPlayerMusicSound, so the engine is stopped here.
void __fastcall SetPlayerTypeDetour(void* self, const std::uint32_t type) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kSetPlayerType, api, lease, original)) return;
    if ((type & 0xFFU) != profile::kPlayerTypeVehicle) NotifyEngine(&AudioEngine::StopNow);
    CallSetPlayerType(original, self, type);
    EndCallback(api, lease);
}

// NPC characters also get off vehicles; only a pawn whose controller has a
// UPlayer (i.e. a player controller) counts.
bool IsLocalPlayerCharacter(const Context& context, void* character) noexcept {
    std::uintptr_t controller{};
    std::uintptr_t player{};
    std::uintptr_t owner{};
    // AAIController has unrelated data at the Player offset, so the UPlayer
    // must point back at the same controller.
    return character != nullptr &&
        ReadValue(context, reinterpret_cast<std::uintptr_t>(character) +
            profile::kPawnControllerOffset, controller) &&
        controller != 0 &&
        ReadValue(context, controller + profile::kPlayerControllerPlayerOffset, player) &&
        player != 0 &&
        ReadValue(context, player + profile::kPlayerPlayerControllerOffset, owner) &&
        owner == controller;
}

// The takeover skips the original Post, so the game's own stop paths (which
// require a Wwise playing ID) never run; the end of the get-off sequence is
// the reliable leave-vehicle signal.
void __fastcall EndGetOffDetour(void* self, const std::uint8_t set_location_and_rotation) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kEndGetOff, api, lease, original)) return;
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr && IsLocalPlayerCharacter(*context, self)) {
        try {
            if (context->engine.Snapshot().playing) {
                Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "player left the vehicle; music stopped");
            }
            context->engine.StopNow();
        } catch (...) {
        }
    }
    CallStop(original, self, set_location_and_rotation);
    EndCallback(api, lease);
}

// ---- library album --------------------------------------------------------------
//
// The library is appended to the music list as the album "HiFi Library": the
// data table iterations and lookups gain synthetic rows, and the owned list
// carries their ids only while the game reads it, so the save never sees them.

void* CallGameInstance() noexcept {
    const std::uintptr_t function = g_game_instance.load(std::memory_order_acquire);
    if (function == 0) return nullptr;
    __try {
        return reinterpret_cast<GameInstanceFn>(function)();
    } __except (1) {
        return nullptr;
    }
}

void* CallFindRow(const std::uintptr_t original, void* table, const std::uint64_t name,
    const wchar_t* context, const char warn) noexcept {
    __try {
        return reinterpret_cast<FindRowFn>(original)(table, name, context, warn);
    } __except (1) {
        return nullptr;
    }
}

void CallForEach(const std::uintptr_t original, void* table, const wchar_t* context,
    void* callback) noexcept {
    __try {
        reinterpret_cast<ForEachFn>(original)(table, context, callback);
    } __except (1) {
    }
}

void* CallAlbumRow(const std::uintptr_t original, void* game_instance, const std::uint64_t* album,
    const std::uint8_t warn) noexcept {
    __try {
        return reinterpret_cast<AlbumRowFn>(original)(game_instance, album, warn);
    } __except (1) {
        return nullptr;
    }
}

void* CallOwnedCopy(const std::uintptr_t original, void* self, void* out) noexcept {
    __try {
        return reinterpret_cast<OwnedCopyFn>(original)(self, out);
    } __except (1) {
        return nullptr;
    }
}

void CallSetCurrent(const std::uintptr_t original, void* self, const std::uint64_t id) noexcept {
    __try {
        reinterpret_cast<SetCurrentFn>(original)(self, id);
    } __except (1) {
    }
}

void CallAddUnique(void* array, const std::uint64_t id) noexcept {
    const std::uintptr_t function = g_add_unique.load(std::memory_order_acquire);
    if (function == 0) return;
    __try {
        static_cast<void>(reinterpret_cast<AddUniqueFn>(function)(array, &id));
    } __except (1) {
    }
}

void CallVisit(const RowCallback* callback, const std::uint64_t id, void* row) noexcept {
    __try {
        RowElement element{id, row, -1};
        callback->fn(callback->user, &element, row);
    } __except (1) {
    }
}

void* CallSoundSubsystem(void* world_context) noexcept {
    const std::uintptr_t function = g_sound_subsystem.load(std::memory_order_acquire);
    if (function == 0 || world_context == nullptr) return nullptr;
    __try {
        return reinterpret_cast<SoundSubsystemFn>(function)(world_context);
    } __except (1) {
        return nullptr;
    }
}

std::uintptr_t CallSelf(const std::uintptr_t original, void* self) noexcept {
    __try {
        return reinterpret_cast<SelfFn>(original)(self);
    } __except (1) {
        return 0;
    }
}

char CallVehiclePanel(const std::uintptr_t original, void* self, void* arg, const char flag) noexcept {
    __try {
        return reinterpret_cast<VehiclePanelFn>(original)(self, arg, flag);
    } __except (1) {
        return 0;
    }
}

void* CallImportTexture(const std::wstring& path) noexcept {
    const std::uintptr_t function = g_import_texture.load(std::memory_order_acquire);
    if (function == 0 || path.size() >= static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
        return nullptr;
    }
    const auto num = static_cast<std::int32_t>(path.size() + 1);
    const GameString string{path.c_str(), num, num};
    __try {
        return reinterpret_cast<ImportTextureFn>(function)(&string);
    } __except (1) {
        return nullptr;
    }
}

bool CallSetItemFlags(void* item, const std::int32_t flags) noexcept {
    const std::uintptr_t function = g_set_item_flags.load(std::memory_order_acquire);
    if (function == 0 || item == nullptr) return false;
    __try {
        static_cast<void>(reinterpret_cast<SetItemFlagsFn>(function)(item, flags));
        return true;
    } __except (1) {
        return false;
    }
}

bool CopyBytes(void* destination, const void* source, const std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (1) {
        return false;
    }
}

// Writes a new FText into `destination`; the row owns it from then on.
bool MakeText(void* destination, const wchar_t* text) noexcept {
    const std::uintptr_t from_string = g_ftext_from_string.load(std::memory_order_acquire);
    if (from_string == 0) return false;
    __try {
        alignas(16) std::byte buffer[0x20]{};
        reinterpret_cast<FTextFromStringFn>(from_string)(buffer, text);
        std::memcpy(destination, buffer, profile::kFTextSize);
        return true;
    } __except (1) {
        return false;
    }
}

// Removes one occurrence of each id in `added` (sorted) from a TArray<FName>,
// keeping the order of the rest.
void RemoveSongIds(void* array, std::vector<std::uint64_t>& added) noexcept {
    __try {
        auto* const data = *static_cast<std::uint64_t**>(array);
        auto& count = *reinterpret_cast<std::int32_t*>(static_cast<std::byte*>(array) + 8);
        std::int32_t kept = 0;
        for (std::int32_t i = 0; i < count; ++i) {
            const std::uint64_t id = data[i];
            const auto it = std::lower_bound(added.begin(), added.end(), id);
            if (it != added.end() && *it == id) {
                added.erase(it);  // one occurrence each
                continue;
            }
            data[kept++] = id;
        }
        count = kept;
    } __except (1) {
    }
}

std::uintptr_t GameTable(const Context& context, const std::uint32_t offset) noexcept {
    const auto game_instance = reinterpret_cast<std::uintptr_t>(CallGameInstance());
    std::uintptr_t table{};
    if (game_instance == 0 || !ReadValue(context, game_instance + offset, table)) return 0;
    return table;
}

// A zero-filled copy of a game row, or null.
std::byte* NewRow(const std::byte* row_template, const std::size_t size) {
    auto* const row = new (std::nothrow) std::byte[size]{};
    if (row != nullptr && !CopyBytes(row, row_template, size)) {
        delete[] row;
        return nullptr;
    }
    return row;
}

template <typename T>
void Put(std::byte* row, const std::uint32_t offset, const T value) noexcept {
    std::memcpy(row + offset, &value, sizeof(value));
}

// FUObjectItem of a UObject index, or null.
void* ObjectItem(const Context& context, const std::int32_t index) noexcept {
    const std::uintptr_t objects = g_object_array.load(std::memory_order_acquire);
    std::uintptr_t chunks{};
    std::int32_t count{};
    std::uintptr_t chunk{};
    if (objects == 0 || index < 0 || !ReadValue(context, objects, chunks) ||
        !ReadValue(context, objects + profile::kObjectArrayCountOffset, count) || index >= count ||
        !ReadValue(context, chunks + sizeof(std::uintptr_t) * (index / profile::kObjectsPerChunk),
            chunk) ||
        chunk == 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(
        chunk + std::uintptr_t{profile::kObjectItemSize} * (index % profile::kObjectsPerChunk));
}

// The object's weak-pointer serial number, allocated the way the engine's
// AllocateSerialNumber does when the object has none yet; 0 on failure.
std::int32_t ObjectSerial(const Context& context, const std::int32_t index) noexcept {
    auto* const item = static_cast<std::byte*>(ObjectItem(context, index));
    const std::uintptr_t counter = g_serial_counter.load(std::memory_order_acquire);
    if (item == nullptr || counter == 0) return 0;
    __try {
        auto* const slot = reinterpret_cast<volatile LONG*>(item + profile::kObjectItemSerialOffset);
        LONG serial = *slot;
        if (serial != 0) return serial;
        serial = InterlockedIncrement(reinterpret_cast<volatile LONG*>(counter));
        const LONG previous = InterlockedCompareExchange(slot, serial, 0);
        return previous != 0 ? previous : serial;
    } __except (1) {
        return 0;
    }
}

// Imports the library's cover image as a rooted transient texture and points
// the album row's cover at it by path. Keeps the current cover on any failure.
void ApplyCover(Context& context, AlbumState& album) {
    if (g_import_texture.load(std::memory_order_acquire) == 0) return;
    std::wstring path = context.engine.Cover();
    if (path.empty() || path == album.cover) return;
    album.cover = path;  // one attempt per image
    const auto texture = reinterpret_cast<std::uintptr_t>(CallImportTexture(path));
    std::int32_t index{};
    std::uintptr_t package{};
    std::uint64_t asset_name{};
    std::uint64_t package_name{};
    if (texture == 0 || !ReadValue(context, texture + profile::kObjectIndexOffset, index) ||
        !ReadValue(context, texture + profile::kObjectNameOffset, asset_name) ||
        !ReadValue(context, texture + profile::kObjectOuterOffset, package) || package == 0 ||
        !ReadValue(context, package + profile::kObjectNameOffset, package_name) ||
        !CallSetItemFlags(ObjectItem(context, index), static_cast<std::int32_t>(profile::kRootSetFlag))) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "album cover could not be imported");
        return;
    }
    const std::int32_t serial = ObjectSerial(context, index);
    if (serial == 0) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "album cover has no serial number");
        return;
    }
    // Weak pointer {index, serial} resolves to the texture without a load; the
    // path keeps the UI's "has a cover" check true.
    std::byte* const cover = album.album + profile::kAlbumCoverOffset;
    std::memset(cover, 0, profile::kSoftPtrSize);
    Put(cover, 0, index);
    Put(cover, sizeof(std::int32_t), serial);
    Put(cover, profile::kSoftPtrAssetPathOffset, package_name);
    Put(cover, profile::kSoftPtrAssetPathOffset + sizeof(std::uint64_t), asset_name);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "album cover imported");
}

void PrimeTemplate(Context& context);

// Builds the album row and one song row per library title. Called on the game
// thread with album_mutex held; rows replaced by a rescan are leaked on purpose.
void EnsureRows(Context& context) {
    AlbumState& album = context.album;
    // The album list can be built before any music list walked the table.
    if (album.music_template == nullptr) PrimeTemplate(context);
    if (album.music_template == nullptr) return;
    if (album.album_template == nullptr) {
        // Any real album works as a template: the template song's own album.
        const std::uintptr_t original = g_slots[kAlbumRow].original.load(std::memory_order_acquire);
        void* const game_instance = CallGameInstance();
        std::uint64_t name{};
        if (original != 0 && game_instance != nullptr &&
            CopyBytes(&name, album.music_template + profile::kMusicAlbumIdOffset, sizeof(name))) {
            album.album_template =
                static_cast<const std::byte*>(CallAlbumRow(original, game_instance, &name, 0));
        }
    }
    if (album.album == nullptr && album.album_template != nullptr) {
        std::byte* const row = NewRow(album.album_template, profile::kAlbumRowSize);
        const std::wstring name = Utf8ToWide(context.localizer.Text("album.name", "HiFi Library"));
        if (row != nullptr && MakeText(row + profile::kAlbumNameOffset, name.c_str()) &&
            MakeText(row + profile::kAlbumDescriptionOffset, L"")) {
            Put(row, profile::kAlbumSortIndexOffset, profile::kSortIndexBase);
            album.album = row;
        }
    }

    const std::uint64_t generation = context.engine.LibraryGeneration();
    // Custom cover is off: both ways of pointing the album row at an imported
    // texture made the album vanish from the list in game. The album keeps the
    // template album's cover until that is understood.
    if (generation == album.generation) return;
    std::vector<std::wstring> titles = context.engine.Titles();
    if (titles.size() > profile::kMaxSongs) titles.resize(profile::kMaxSongs);
    std::vector<std::byte*> songs;
    songs.reserve(titles.size());
    const std::array<std::byte, profile::kFStringSize> empty_string{};
    for (std::size_t i = 0; i < titles.size(); ++i) {
        std::byte* const row = NewRow(album.music_template, profile::kMusicRowSize);
        if (row == nullptr || !MakeText(row + profile::kMusicTitleOffset, titles[i].c_str()) ||
            !MakeText(row + profile::kMusicDescriptionOffset, L"")) {
            return;  // retried on the next lookup
        }
        Put(row, profile::kMusicSortIndexOffset,
            profile::kSortIndexBase + static_cast<std::int32_t>(i));
        std::memcpy(row + profile::kMusicCommentOffset, empty_string.data(), empty_string.size());
        Put(row, profile::kMusicAlbumIdOffset, AlbumId(album));
        // The event keeps the template's asset (song end posts it) but owns no
        // string buffer of the template.
        std::memcpy(row + profile::kMusicEventOffset + profile::kMusicEventSubPathOffset,
            empty_string.data(), empty_string.size());
        Put(row, profile::kMusicSourceItemOffset, std::uint64_t{0});
        Put(row, profile::kMusicSourceQuestOffset, std::uint64_t{0});
        Put(row, profile::kMusicIsListOffset, std::uint8_t{1});
        Put(row, profile::kMusicIsHiddenOffset, std::uint8_t{0});
        songs.push_back(row);
    }
    album.songs = std::move(songs);
    album.generation = generation;
}

bool AlbumActive(const AlbumState& album) noexcept {
    return g_enabled.load(std::memory_order_acquire) && album.base != 0 &&
        album.album != nullptr && !album.songs.empty();
}

// The library songs as (id, row), empty when the album is not shown.
std::vector<std::pair<std::uint64_t, std::byte*>> ActiveSongs(Context& context) noexcept {
    std::vector<std::pair<std::uint64_t, std::byte*>> result;
    try {
        std::scoped_lock lock(context.album_mutex);
        EnsureRows(context);
        if (!AlbumActive(context.album)) return result;
        result.reserve(context.album.songs.size());
        for (std::size_t i = 0; i < context.album.songs.size(); ++i) {
            result.emplace_back(SongId(context.album, i), context.album.songs[i]);
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

bool Contains(const void* array, const std::uint64_t id) noexcept {
    __try {
        const auto* const data = *static_cast<const std::uint64_t* const*>(array);
        const auto count = *reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(array) + 8);
        for (std::int32_t i = 0; i < count; ++i) {
            if (data[i] == id) return true;
        }
        return false;
    } __except (1) {
        return true;  // unreadable: never add
    }
}

// Which ids a reader of the owned array gets.
enum class OwnedScope {
    Library,  // the library songs only: play-queue paths, so next/previous stay in the library
    All,      // plus every listed game song: UI paths, so locked game songs show unlocked
};

// Adds the scope's ids to a TArray<FName>; returns exactly the ids that were
// not there before.
std::vector<std::uint64_t> AddSongIds(Context& context, void* array,
    const OwnedScope scope = OwnedScope::All) noexcept {
    std::vector<std::uint64_t> added;
    try {
        std::vector<std::uint64_t> ids;
        for (const auto& [id, row] : ActiveSongs(context)) ids.push_back(id);
        if (scope == OwnedScope::All && g_enabled.load(std::memory_order_acquire)) {
            std::scoped_lock lock(context.album_mutex);
            ids.insert(ids.end(), context.album.game_songs.begin(), context.album.game_songs.end());
        }
        for (const std::uint64_t id : ids) {
            if (Contains(array, id)) continue;
            CallAddUnique(array, id);
            added.push_back(id);
        }
    } catch (...) {
    }
    return added;
}

// Remembers the first row a table iteration visits as the row template.
// Also collects every listed game song so they can be owned for unlocking.
struct RowRecorder final {
    const RowCallback* inner;
    const std::byte* first;
    std::uint32_t base;
    std::vector<std::uint64_t>* listed;
};

void __fastcall RecordRow(void* user, void* element, void* row) {
    auto* const recorder = static_cast<RowRecorder*>(user);
    if (element != nullptr && row != nullptr) {
        const auto* const bytes = static_cast<const std::byte*>(row);
        const std::uint64_t name = static_cast<const RowElement*>(element)->name;
        if (recorder->first == nullptr) {
            recorder->first = bytes;
            recorder->base = static_cast<std::uint32_t>(name);
        }
        if (recorder->listed != nullptr && name != 0 &&
            static_cast<std::uint8_t>(bytes[profile::kMusicIsListOffset]) != 0 &&
            static_cast<std::uint8_t>(bytes[profile::kMusicIsHiddenOffset]) == 0) {
            try {
                recorder->listed->push_back(name);
            } catch (...) {
            }
        }
    }
    recorder->inner->fn(recorder->inner->user, element, row);
}

void __fastcall NoopVisit(void*, void*, void*) {}

// Walks the music table with the original ForeachRow to record the template
// row, the name base and the listed game songs. Called with album_mutex held.
void PrimeTemplate(Context& context) {
    const std::uintptr_t original = g_slots[kForEachRow].original.load(std::memory_order_acquire);
    const std::uintptr_t table = GameTable(context, profile::kGameInstanceMusicTableOffset);
    if (original == 0 || table == 0) return;
    static const RowCallback noop{&NoopVisit, nullptr};
    std::vector<std::uint64_t> listed;
    RowRecorder recorder{&noop, nullptr, 0, &listed};
    RowCallback wrapped{&RecordRow, &recorder};
    CallForEach(original, reinterpret_cast<void*>(table), L"", &wrapped);
    if (recorder.first == nullptr) return;
    context.album.music_template = recorder.first;
    context.album.base = recorder.base;
    if (!listed.empty()) context.album.game_songs = std::move(listed);
}

void* __fastcall FindRowDetour(void* table, const std::uint64_t name, const wchar_t* context_name,
    const char warn) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kFindRow, api, lease, original)) return nullptr;
    void* row = nullptr;
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr) {
        try {
            std::scoped_lock lock(context->album_mutex);
            const auto song = SongIndex(context->album, name);
            if (song && AlbumActive(context->album) &&
                reinterpret_cast<std::uintptr_t>(table) ==
                    GameTable(*context, profile::kGameInstanceMusicTableOffset)) {
                row = context->album.songs[*song];
            }
        } catch (...) {
        }
    }
    if (row == nullptr) row = CallFindRow(original, table, name, context_name, warn);
    EndCallback(api, lease);
    return row;
}

void __fastcall ForEachRowDetour(void* table, const wchar_t* context_name, void* callback) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kForEachRow, api, lease, original)) return;
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context == nullptr || callback == nullptr ||
        reinterpret_cast<std::uintptr_t>(table) !=
            GameTable(*context, profile::kGameInstanceMusicTableOffset)) {
        CallForEach(original, table, context_name, callback);
        EndCallback(api, lease);
        return;
    }
    const auto* const inner = static_cast<const RowCallback*>(callback);
    std::vector<std::uint64_t> listed;
    RowRecorder recorder{inner, nullptr, 0, &listed};
    RowCallback wrapped{&RecordRow, &recorder};
    CallForEach(original, table, context_name, &wrapped);
    if (recorder.first != nullptr) {
        try {
            std::scoped_lock lock(context->album_mutex);
            if (context->album.music_template == nullptr) {
                context->album.music_template = recorder.first;
                context->album.base = recorder.base;
            }
            if (!listed.empty()) context->album.game_songs = std::move(listed);
        } catch (...) {
        }
    }
    for (const auto& [id, row] : ActiveSongs(*context)) CallVisit(inner, id, row);
    EndCallback(api, lease);
}

void* __fastcall AlbumRowDetour(void* self, const std::uint64_t* album_name, const std::uint8_t warn) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kAlbumRow, api, lease, original)) return nullptr;
    void* row = nullptr;
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uint64_t name{};
    if (context != nullptr && album_name != nullptr &&
        ReadValue(*context, reinterpret_cast<std::uintptr_t>(album_name), name)) {
        try {
            std::scoped_lock lock(context->album_mutex);
            if (AlbumActive(context->album) && name == AlbumId(context->album)) {
                row = context->album.album;
            }
        } catch (...) {
        }
    }
    if (row == nullptr) row = CallAlbumRow(original, self, album_name, warn);
    EndCallback(api, lease);
    return row;
}

void __fastcall AlbumForEachDetour(void* table, const wchar_t* context_name, void* callback) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kAlbumForEach, api, lease, original)) return;
    CallForEach(original, table, context_name, callback);
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr && callback != nullptr &&
        reinterpret_cast<std::uintptr_t>(table) ==
            GameTable(*context, profile::kGameInstanceAlbumTableOffset)) {
        std::uint64_t id{};
        std::byte* row = nullptr;
        try {
            std::scoped_lock lock(context->album_mutex);
            EnsureRows(*context);
            if (AlbumActive(context->album)) {
                id = AlbumId(context->album);
                row = context->album.album;
            }
        } catch (...) {
        }
        if (row != nullptr) CallVisit(static_cast<const RowCallback*>(callback), id, row);
    }
    EndCallback(api, lease);
}

// The music list UI and its click path read the owned ids through this copy.
void* __fastcall OwnedCopyDetour(void* self, void* out) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kOwnedCopy, api, lease, original)) return out;
    void* const result = CallOwnedCopy(original, self, out);
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr && result != nullptr) static_cast<void>(AddSongIds(*context, result));
    EndCallback(api, lease);
    return result;
}

// These read the owned array directly, so the song ids are present only for the
// duration of the outermost such call on this game thread; nested calls leave
// them in place until it returns.
thread_local std::uint32_t t_owned_depth = 0;

// The sound subsystem last seen as `this` of a subsystem detour; the UI detours
// fall back to it when the world-context lookup fails.
std::atomic<void*> g_last_subsystem{nullptr};

bool IsLibrarySongId(Context& context, const std::uint64_t id) noexcept {
    try {
        std::scoped_lock lock(context.album_mutex);
        return AlbumActive(context.album) && SongIndex(context.album, id).has_value();
    } catch (...) {
        return false;
    }
}

// Copies a TArray<FName>'s elements out; false when unreadable.
bool ReadIds(const void* array, std::vector<std::uint64_t>& ids) noexcept {
    __try {
        const auto* const data = *static_cast<const std::uint64_t* const*>(array);
        const auto count = *reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(array) + 8);
        if (count < 0 || count > 1 << 20 || (count != 0 && data == nullptr)) return false;
        ids.resize(static_cast<std::size_t>(count));
        if (count != 0) std::memcpy(ids.data(), data, sizeof(std::uint64_t) * ids.size());
        return true;
    } __except (1) {
        return false;
    }
}

void ClearIds(void* array) noexcept {
    __try {
        *reinterpret_cast<std::int32_t*>(static_cast<std::byte*>(array) + 8) = 0;
    } __except (1) {
    }
}

// Puts back a ReadIds snapshot. The array never shrank since (ClearIds keeps
// the allocation, AddUnique only grows it), so the old elements fit.
void WriteIds(void* array, const std::vector<std::uint64_t>& ids) noexcept {
    __try {
        auto* const data = *static_cast<std::uint64_t**>(array);
        const auto capacity = *reinterpret_cast<const std::int32_t*>(static_cast<std::byte*>(array) + 12);
        if (ids.size() > static_cast<std::size_t>(capacity < 0 ? 0 : capacity)) return;
        if (!ids.empty()) std::memcpy(data, ids.data(), sizeof(std::uint64_t) * ids.size());
        *reinterpret_cast<std::int32_t*>(static_cast<std::byte*>(array) + 8) =
            static_cast<std::int32_t>(ids.size());
    } __except (1) {
    }
}

void* OwnedArray(void* subsystem) noexcept {
    if (subsystem == nullptr) subsystem = g_last_subsystem.load(std::memory_order_acquire);
    return subsystem == nullptr ? nullptr : static_cast<std::byte*>(subsystem) + profile::kSubsystemOwnedIdsOffset;
}

std::uint64_t CurrentSongId(void* subsystem) noexcept {
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uint64_t id{};
    if (context == nullptr || subsystem == nullptr ||
        !ReadValue(*context, reinterpret_cast<std::uintptr_t>(subsystem) + profile::kSubsystemCurrentIdOffset, id)) {
        return 0;
    }
    return id;
}

// UI readers: library songs and every listed game song are owned for the
// outermost call; nested calls see them already in place.
template <typename Call>
void WithSongIds(void* subsystem, Call&& call) {
    auto* const context = g_context.load(std::memory_order_acquire);
    void* const owned = OwnedArray(subsystem);
    std::vector<std::uint64_t> added;
    if (t_owned_depth == 0 && context != nullptr && owned != nullptr) {
        added = AddSongIds(*context, owned, OwnedScope::All);
    }
    const bool own = !added.empty();
    if (own) ++t_owned_depth;
    call();
    if (own) {
        std::sort(added.begin(), added.end());
        RemoveSongIds(owned, added);
        --t_owned_depth;
    }
}

// Play-queue builders. While a library song is the one being played the queue
// is built from the library alone, so the game's next/previous and song end
// stay in the library; otherwise they get the UI view (unlocked game songs).
// The array is snapshotted and restored exactly, also when nested in a UI call.
template <typename Call>
void WithQueueIds(void* subsystem, const std::uint64_t song, Call&& call) {
    auto* const context = g_context.load(std::memory_order_acquire);
    void* const owned = OwnedArray(subsystem);
    std::vector<std::uint64_t> saved;
    if (context == nullptr || owned == nullptr || !IsLibrarySongId(*context, song) ||
        !ReadIds(owned, saved)) {
        WithSongIds(subsystem, std::forward<Call>(call));
        return;
    }
    ClearIds(owned);
    static_cast<void>(AddSongIds(*context, owned, OwnedScope::Library));
    ++t_owned_depth;
    call();
    --t_owned_depth;
    WriteIds(owned, saved);
}

void __fastcall ReGenerateDetour(void* self) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kReGenerate, api, lease, original)) return;
    g_last_subsystem.store(self, std::memory_order_release);
    WithQueueIds(self, CurrentSongId(self), [&] { CallVoid(original, self); });
    EndCallback(api, lease);
}

void __fastcall SetCurrentDetour(void* self, const std::uint64_t id) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kSetCurrent, api, lease, original)) return;
    g_last_subsystem.store(self, std::memory_order_release);
    WithQueueIds(self, id, [&] { CallSetCurrent(original, self, id); });
    EndCallback(api, lease);
}

// Falls back to the first owned id when the current one is not owned.
std::uintptr_t SubsystemSelfDetour(const HookIndex index, void* self) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(index, api, lease, original)) return 0;
    std::uintptr_t result{};
    g_last_subsystem.store(self, std::memory_order_release);
    WithQueueIds(self, CurrentSongId(self), [&] { result = CallSelf(original, self); });
    EndCallback(api, lease);
    return result;
}

std::uintptr_t __fastcall ResolveCurrentDetour(void* self) {
    return SubsystemSelfDetour(kResolveCurrent, self);
}

std::uintptr_t __fastcall SyncCurrentDetour(void* self) {
    return SubsystemSelfDetour(kSyncCurrent, self);
}

// The music panel marks list entries missing from the owned array as locked.
std::uintptr_t __fastcall ItemRefreshDetour(void* self) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kItemRefresh, api, lease, original)) return 0;
    std::uintptr_t result{};
    WithSongIds(CallSoundSubsystem(self), [&] { result = CallSelf(original, self); });
    EndCallback(api, lease);
    return result;
}

// The album page of the music panel sets each entry's lock state from the
// owned array.
std::uintptr_t __fastcall PageListDetour(void* self) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kPageList, api, lease, original)) return 0;
    std::uintptr_t result{};
    WithSongIds(CallSoundSubsystem(self), [&] { result = CallSelf(original, self); });
    EndCallback(api, lease);
    return result;
}

using MusicEndFn = void(__fastcall*)(void*, std::uint8_t, void*);

void CallMusicEnd(const std::uintptr_t original, void* self, const std::uint8_t cancelled,
    void* info) noexcept {
    __try {
        reinterpret_cast<MusicEndFn>(original)(self, cancelled, info);
    } __except (1) {
    }
}

// A Wwise end callback that arrives while a library song is current belongs to
// the Wwise music the takeover stopped, not to the library song: passing it on
// skips the library song early. The engine advances library songs itself.
void __fastcall MusicEndDetour(void* self, const std::uint8_t cancelled, void* info) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kMusicEnd, api, lease, original)) return;
    auto* const context = g_context.load(std::memory_order_acquire);
    const bool library = context != nullptr && IsLibrarySongId(*context, CurrentSongId(self));
    if (!library) CallMusicEnd(original, self, cancelled, info);
    EndCallback(api, lease);
}

using SetCurrentIdFn = void(__fastcall*)(void*, const std::uint64_t*);

void CallSetCurrentId(void* subsystem, const std::uint64_t id) noexcept {
    const std::uintptr_t function = g_set_current_id.load(std::memory_order_acquire);
    if (function == 0) return;
    __try {
        reinterpret_cast<SetCurrentIdFn>(function)(subsystem, &id);
    } __except (1) {
    }
}

// Game thread, once per frame: when the engine moved on to another library
// song by itself (song end or the window's previous/next), the game still
// names the old one. Point the game's current id at the song really playing,
// so the title, the list highlight and the game's own next/previous follow.
void ANOMALY_CALL SyncCurrentSong(void* user, const AnomalyUe5AhudFrameV1*) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || g_set_current_id.load(std::memory_order_acquire) == 0) return;
    try {
        void* const subsystem = g_last_subsystem.load(std::memory_order_acquire);
        const std::optional<std::size_t> slot = context->engine.PlayingSlot();
        if (subsystem == nullptr || !slot) return;
        const std::uint64_t current = CurrentSongId(subsystem);
        std::uint64_t wanted{};
        {
            std::scoped_lock lock(context->album_mutex);
            // Only a library song is ever replaced: a game song stays the game's.
            if (!AlbumActive(context->album) || !SongIndex(context->album, current) ||
                *slot >= context->album.songs.size()) {
                return;
            }
            wanted = SongId(context->album, *slot);
        }
        if (wanted != current) CallSetCurrentId(subsystem, wanted);
    } catch (...) {
    }
}

// The vehicle music panel builds its list from the owned array.
char __fastcall VehiclePanelDetour(void* self, void* arg, const char flag) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kVehiclePanel, api, lease, original)) return 0;
    char result{};
    WithSongIds(CallSoundSubsystem(self),
        [&] { result = CallVehiclePanel(original, self, arg, flag); });
    EndCallback(api, lease);
    return result;
}

// ---- hook lifetime --------------------------------------------------------------

void ReleaseHooks(Context& context) noexcept {
    for (std::size_t i = 0; i < kHookCount; ++i) {
        if (context.hooks[i].id != 0) {
            static_cast<void>(context.hook->release(context.hook->user, context.hooks[i]));
            context.hooks[i] = {};
        }
        g_slots[i].id.store(0, std::memory_order_release);
        g_slots[i].generation.store(0, std::memory_order_release);
        g_slots[i].original.store(0, std::memory_order_release);
    }
    g_hook_api.store(nullptr, std::memory_order_release);
    g_ftext_from_string.store(0, std::memory_order_release);
    g_game_instance.store(0, std::memory_order_release);
    g_add_unique.store(0, std::memory_order_release);
    g_sound_subsystem.store(0, std::memory_order_release);
    g_import_texture.store(0, std::memory_order_release);
    g_set_item_flags.store(0, std::memory_order_release);
    g_object_array.store(0, std::memory_order_release);
    g_serial_counter.store(0, std::memory_order_release);
    g_last_subsystem.store(nullptr, std::memory_order_release);
    g_set_current_id.store(0, std::memory_order_release);
}

std::pair<HookState, std::string> InstallHooks(Context& context) {
    struct Target final {
        std::string_view pattern;
        void* detour;
        std::string_view label;
        bool call_site;  // the pattern matches an E8 rel32 calling the target
    };
    const std::array<Target, kHookCount> targets{{
        {profile::kPostPattern, reinterpret_cast<void*>(&PostDetour), "hifi-vehicle-music-post", false},
        {profile::kStopPattern, reinterpret_cast<void*>(&StopDetour), "hifi-vehicle-music-stop", false},
        {profile::kPausePattern, reinterpret_cast<void*>(&PauseDetour), "hifi-vehicle-music-pause",
            false},
        {profile::kResumePattern, reinterpret_cast<void*>(&ResumeDetour),
            "hifi-vehicle-music-resume", false},
        {profile::kSetPlayerTypePattern, reinterpret_cast<void*>(&SetPlayerTypeDetour),
            "hifi-vehicle-music-player-type", false},
        {profile::kEndGetOffPattern, reinterpret_cast<void*>(&EndGetOffDetour),
            "hifi-vehicle-music-end-get-off", false},
        {profile::kFindRowCallPattern, reinterpret_cast<void*>(&FindRowDetour),
            "hifi-vehicle-music-find-row", true},
        {profile::kForEachRowCallPattern, reinterpret_cast<void*>(&ForEachRowDetour),
            "hifi-vehicle-music-for-each-row", true},
        {profile::kAlbumRowPattern, reinterpret_cast<void*>(&AlbumRowDetour),
            "hifi-vehicle-music-album-row", false},
        {profile::kAlbumForEachCallPattern, reinterpret_cast<void*>(&AlbumForEachDetour),
            "hifi-vehicle-music-album-for-each", true},
        {profile::kOwnedCopyPattern, reinterpret_cast<void*>(&OwnedCopyDetour),
            "hifi-vehicle-music-owned-copy", false},
        {profile::kReGeneratePattern, reinterpret_cast<void*>(&ReGenerateDetour),
            "hifi-vehicle-music-regenerate", false},
        {profile::kSetCurrentPattern, reinterpret_cast<void*>(&SetCurrentDetour),
            "hifi-vehicle-music-set-current", false},
        {profile::kItemRefreshPattern, reinterpret_cast<void*>(&ItemRefreshDetour),
            "hifi-vehicle-music-item-refresh", false},
        {profile::kVehiclePanelPattern, reinterpret_cast<void*>(&VehiclePanelDetour),
            "hifi-vehicle-music-vehicle-panel", false},
        {profile::kResolveCurrentPattern, reinterpret_cast<void*>(&ResolveCurrentDetour),
            "hifi-vehicle-music-resolve-current", false},
        {profile::kSyncCurrentPattern, reinterpret_cast<void*>(&SyncCurrentDetour),
            "hifi-vehicle-music-sync-current", false},
        {profile::kPageListPattern, reinterpret_cast<void*>(&PageListDetour),
            "hifi-vehicle-music-page-list", false},
        {profile::kMusicEndPattern, reinterpret_cast<void*>(&MusicEndDetour),
            "hifi-vehicle-music-music-end", false},
    }};

    const auto resolve = [&context](const std::string_view pattern) {
        std::uintptr_t address{};
        if (context.signature->resolve(context.signature->user,
                anomaly::sdk::StringView(profile::kModule), anomaly::sdk::StringView(profile::kSection),
                anomaly::sdk::StringView(pattern), &address).code != ANOMALY_STATUS_V1_OK) {
            return std::uintptr_t{0};
        }
        return address;
    };
    // Target of the E8 rel32 at `call`, or 0.
    const auto call_target = [&context](const std::uintptr_t call) {
        std::uint8_t opcode{};
        std::int32_t rel{};
        if (call == 0 || !ReadValue(context, call, opcode) || opcode != 0xE8 ||
            !ReadValue(context, call + 1, rel)) {
            return std::uintptr_t{0};
        }
        return call + 5 + static_cast<std::intptr_t>(rel);
    };

    std::array<std::uintptr_t, kHookCount> addresses{};
    std::string_view album_missing;
    for (std::size_t i = 0; i < kHookCount; ++i) {
        addresses[i] = resolve(targets[i].pattern);
        if (targets[i].call_site) addresses[i] = call_target(addresses[i]);
        if (addresses[i] != 0) continue;
        if (i < kCoreHookCount) return {HookState::SignatureMissing, std::string(targets[i].label)};
        if (album_missing.empty()) album_missing = targets[i].label;
    }
    // Helpers the album rows are built with.
    const std::uintptr_t game_instance = resolve(profile::kGameInstancePattern);
    const std::uintptr_t add_unique = resolve(profile::kAddUniquePattern);
    const std::uintptr_t set_list_item = resolve(profile::kSetListItemPattern);
    const std::uintptr_t from_string = call_target(
        set_list_item == 0 ? 0 : set_list_item + profile::kFTextFromStringCallOffset);
    const std::uintptr_t sound_call = resolve(profile::kSoundSubsystemCallPattern);
    const std::uintptr_t sound_subsystem =
        call_target(sound_call == 0 ? 0 : sound_call + profile::kSoundSubsystemCallOffset);
    if (album_missing.empty()) {
        if (game_instance == 0) album_missing = "game instance";
        else if (add_unique == 0) album_missing = "TArray<FName>::AddUnique";
        else if (from_string == 0) album_missing = "FText::FromString";
        else if (sound_subsystem == 0) album_missing = "sound subsystem";
    }
    if (album_missing.empty()) {
        g_game_instance.store(game_instance, std::memory_order_release);
        g_add_unique.store(add_unique, std::memory_order_release);
        g_ftext_from_string.store(from_string, std::memory_order_release);
        g_sound_subsystem.store(sound_subsystem, std::memory_order_release);
        g_set_current_id.store(resolve(profile::kSetCurrentIdPattern), std::memory_order_release);
        // Cover helpers: optional, all or none.
        const std::uintptr_t import_texture = resolve(profile::kImportTexturePattern);
        const std::uintptr_t set_item_flags = resolve(profile::kSetItemFlagsPattern);
        const std::uintptr_t object_load = resolve(profile::kObjectArrayPattern);
        std::int32_t rel{};
        const std::uintptr_t object_array = object_load != 0 &&
                ReadValue(context, object_load + profile::kObjectArrayRelOffset, rel)
            ? object_load + profile::kObjectArrayInstructionSize + static_cast<std::intptr_t>(rel)
            : 0;
        const std::uintptr_t serial_xadd = resolve(profile::kSerialCounterPattern);
        std::int32_t serial_rel{};
        const std::uintptr_t serial_counter = serial_xadd != 0 &&
                ReadValue(context, serial_xadd + profile::kSerialCounterRelOffset, serial_rel)
            ? serial_xadd + profile::kSerialCounterInstructionEnd +
                static_cast<std::intptr_t>(serial_rel)
            : 0;
        if (import_texture != 0 && set_item_flags != 0 && object_array != 0 &&
            serial_counter != 0) {
            g_import_texture.store(import_texture, std::memory_order_release);
            g_set_item_flags.store(set_item_flags, std::memory_order_release);
            g_object_array.store(object_array, std::memory_order_release);
            g_serial_counter.store(serial_counter, std::memory_order_release);
        } else {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "album cover helpers not found; the album keeps the template cover");
        }
    } else {
        std::fill(addresses.begin() + kCoreHookCount, addresses.end(), std::uintptr_t{0});
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "signature not found: " + std::string(album_missing) +
                "; the music list keeps the game's songs");
    }

    g_hook_api.store(context.hook, std::memory_order_release);
    for (std::size_t i = 0; i < kHookCount; ++i) {
        if (addresses[i] == 0) continue;
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = addresses[i];
        request.detour = targets[i].detour;
        request.label = anomaly::sdk::StringView(targets[i].label);
        std::uintptr_t original{};
        AnomalyGenerationHandleV1 handle{};
        if (context.hook->create(context.hook->user, &request, &original, &handle).code !=
                ANOMALY_STATUS_V1_OK ||
            original == 0 || handle.id == 0) {
            if (handle.id != 0) context.hooks[i] = handle;
            ReleaseHooks(context);
            return {HookState::HookFailed, std::string(targets[i].label)};
        }
        context.hooks[i] = handle;
        g_slots[i].original.store(original, std::memory_order_release);
        g_slots[i].generation.store(handle.generation, std::memory_order_release);
        g_slots[i].id.store(handle.id, std::memory_order_release);
    }
    return {HookState::Installed, {}};
}

// A successful unsubscribe drains a callback already in flight.
void UnsubscribeSync(Context& context) noexcept {
    if (context.ahud_subscription.id == 0 || context.ahud == nullptr) return;
    const AnomalyGenerationHandleV1 handle = context.ahud_subscription;
    context.ahud_subscription = {};
    static_cast<void>(context.ahud->unsubscribe(context.ahud->user, handle));
}

// ---- plugin callbacks ------------------------------------------------------------

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host and context are required");
    }
    *plugin_context = nullptr;
    try {
        auto* context = new (std::nothrow) Context();
        if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED, "context allocation failed");
        const anomaly::sdk::Host host_view(host);
        context->core = host_view.Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
        context->config = host_view.Query<AnomalyConfigServiceV1>(
            ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION).get();
        context->signature = host_view.Query<AnomalySignatureServiceV1>(
            ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
        context->hook = host_view.Query<AnomalyHookServiceV1>(
            ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION).get();
        context->names = host_view.Query<AnomalyUe5NamesServiceV1>(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1U).get();
        context->ui = host_view.Query<AnomalyUiServiceV1>(
            ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();
        context->scheduler = host_view.Query<AnomalySchedulerServiceV1>(
            ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION).get();
        context->ahud = host_view.Query<AnomalyUe5AhudServiceV1>(
            ANOMALY_UE5_AHUD_SERVICE_V1_ID, ANOMALY_UE5_AHUD_SERVICE_V1_VERSION).get();
        if (!AhudReady(context->ahud)) context->ahud = nullptr;
        context->localizer = anomaly::plugins::Localizer(host);
        if (!CoreReady(context->core) || !ConfigReady(context->config) ||
            !SignatureReady(context->signature) || !HookReady(context->hook) ||
            !NamesReady(context->names) || !SchedulerReady(context->scheduler)) {
            delete context;
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "required plugin services are unavailable");
        }
        if (context->config->register_schema(context->config->user,
                anomaly::sdk::StringView(kSettingsSchemaId), kSettingsSchemaVersion,
                Bytes(kSettingsSchema), &context->settings_schema).code != ANOMALY_STATUS_V1_OK ||
            context->settings_schema.id == 0) {
            delete context;
            return Status(ANOMALY_STATUS_V1_FAILED, "settings schema registration failed");
        }
        ReadSettings(*context);
        *plugin_context = context;
        return anomaly::sdk::Ok();
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "HiFi vehicle music initialization failed");
    }
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is required");
    try {
        context->engine.Start(ToEngineSettings(context->settings));
        g_context.store(context, std::memory_order_release);
        auto [state, detail] = InstallHooks(*context);
        if (state == HookState::Installed) {
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "music hooks installed");
        } else {
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                std::string(state == HookState::SignatureMissing ? "signature not found: "
                                                                 : "hook failed: ") +
                    detail);
        }
        if (context->settings.folder.empty()) {
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "no music folder selected; vehicle music is not replaced");
        }
        if (state == HookState::Installed && context->ahud != nullptr &&
            g_set_current_id.load(std::memory_order_acquire) != 0) {
            AnomalyGenerationHandleV1 handle{};
            if (context->ahud->subscribe(context->ahud->user, SyncCurrentSong, context, &handle).code ==
                    ANOMALY_STATUS_V1_OK &&
                handle.id != 0) {
                context->ahud_subscription = handle;
            }
        }
        if (context->ahud_subscription.id == 0) {
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "current song sync unavailable; the game's title may lag behind the library song");
        }
        std::scoped_lock lock(context->mutex);
        context->hook_state = state;
        context->hook_detail = std::move(detail);
        return anomaly::sdk::Ok();
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "HiFi vehicle music start failed");
    }
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is required");
    try {
        {
            std::scoped_lock lock(context->mutex);
            if (context->stopped) return anomaly::sdk::Ok();
            context->stopped = true;
        }
        UnsubscribeSync(*context);
        ReleaseHooks(*context);
        g_context.store(nullptr, std::memory_order_release);
        context->engine.Shutdown();
        // The dialog is modal and owned by the user; wait for it to close.
        if (context->picker.joinable()) context->picker.join();
        if (context->persist_task.id != 0) {
            static_cast<void>(context->scheduler->cancel(context->scheduler->user,
                context->persist_task));
            context->persist_task = {};
        }
        context->persist_pending.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(context->mutex);
            if (context->picked_folder) {
                context->settings.folder = std::move(*context->picked_folder);
                context->picked_folder.reset();
                context->settings_dirty = true;
            }
        }
        SaveSettings(*context);
        return anomaly::sdk::Ok();
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "HiFi vehicle music stop failed");
    }
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    try {
        UnsubscribeSync(*context);
        ReleaseHooks(*context);
        g_context.store(nullptr, std::memory_order_release);
        context->engine.Shutdown();
        if (context->picker.joinable()) context->picker.join();
    } catch (...) {
    }
    delete context;
}

bool Button(const AnomalyUiServiceV1* ui, const std::string& label, const bool enabled = true) {
    return ui->button_enabled(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F,
               enabled ? 1 : 0) != 0;
}

void Text(const AnomalyUiServiceV1* ui, const std::string& text) {
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

std::optional<std::string> PickFolder() {
    std::optional<std::string> result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        FILEOPENDIALOGOPTIONS options{};
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                std::string utf8 = WideToUtf8(path);
                if (!utf8.empty()) result = std::move(utf8);
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

// Called from Draw with the context mutex held. The dialog runs on its own STA
// thread so the render thread never blocks on it.
void StartFolderPicker(Context& context) {
    if (context.picker_busy.exchange(true, std::memory_order_acq_rel)) return;
    if (context.picker.joinable()) context.picker.join();
    try {
        context.picker = std::thread([&context] {
            try {
                std::optional<std::string> folder = PickFolder();
                if (folder) {
                    std::scoped_lock lock(context.mutex);
                    if (!context.stopped) context.picked_folder = std::move(folder);
                }
            } catch (...) {
            }
            context.picker_busy.store(false, std::memory_order_release);
        });
    } catch (...) {
        context.picker_busy.store(false, std::memory_order_release);
    }
}

std::string HookStateText(const Context& context) {
    const anomaly::plugins::Localizer& l10n = context.localizer;
    const std::array<std::string_view, 1> args{context.hook_detail};
    switch (context.hook_state) {
    case HookState::Waiting:
        return l10n.Text("hooks.waiting", "Hooks: not installed");
    case HookState::Installed:
        return l10n.Text("hooks.installed", "Hooks: installed");
    case HookState::SignatureMissing:
        return l10n.Format("hooks.signature_missing", "Hooks: signature not found ({0})", args);
    case HookState::HookFailed:
        return l10n.Format("hooks.failed", "Hooks: install failed ({0})", args);
    }
    return {};
}

std::string EngineStatusText(const anomaly::plugins::Localizer& l10n,
    const hifi_vehicle_music::EngineSnapshot& snapshot) {
    const std::array<std::string_view, 1> args{snapshot.detail};
    switch (snapshot.status) {
    case EngineStatus::Starting:
        return l10n.Text("engine.starting", "Engine: starting");
    case EngineStatus::Ready:
        return l10n.Format("engine.ready", "Engine: ready ({0})", args);
    case EngineStatus::BackendUnavailable:
        return l10n.Format("engine.backend_unavailable", "Engine: {0} is unavailable", args);
    case EngineStatus::FolderUnavailable:
        return l10n.Text("engine.folder_unavailable", "Engine: music folder cannot be read");
    case EngineStatus::DecodeFailed:
        return l10n.Format("engine.decode_failed", "Engine: cannot decode {0}", args);
    case EngineStatus::OpenFailed:
        return l10n.Format("engine.open_failed", "Engine: cannot open {0} output", args);
    case EngineStatus::StartFailed:
        return l10n.Format("engine.start_failed", "Engine: cannot start {0} output", args);
    case EngineStatus::Playing:
        return l10n.Text("engine.playing", "Engine: playing");
    case EngineStatus::AsioError:
        return l10n.Format("engine.asio_error", "Engine: ASIO error: {0}", args);
    case EngineStatus::EngineError:
        return l10n.Format("engine.error", "Engine: error: {0}", args);
    }
    return {};
}

// Called from Draw with the context mutex held.
void DrawStatus(Context& context, const AnomalyUiServiceV1* ui,
    const hifi_vehicle_music::EngineSnapshot& snapshot) {
    const anomaly::plugins::Localizer& l10n = context.localizer;
    Text(ui, HookStateText(context));
    Text(ui, EngineStatusText(l10n, snapshot));
    {
        const std::string count = std::to_string(snapshot.track_count);
        const std::array<std::string_view, 1> args{count};
        Text(ui, l10n.Format("library.count", "Library: {0} tracks (in-game album \"HiFi Library\")",
            args));
    }
    if (context.has_event) {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kDecisions{{
            {"event.replaced", "replaced"},
            {"event.disabled", "game audio: replacement disabled"},
            {"event.not_vehicle", "game audio: not vehicle music"},
            {"event.no_track", "game audio: library empty or output failed"},
            {"event.game_song", "game audio: game song"},
        }};
        const auto& [key, fallback] = kDecisions[static_cast<std::size_t>(context.last_decision)];
        const std::string decision = l10n.Text(key, fallback);
        const std::array<std::string_view, 2> args{context.last_event, decision};
        Text(ui, l10n.Format("event.last", "Last event: {0} ({1})", args));
    } else {
        Text(ui, l10n.Text("event.none", "Last event: none yet"));
    }
    if (snapshot.playing) {
        const std::array<std::string_view, 1> track{snapshot.track};
        Text(ui, l10n.Format("playback.track", "Playing: {0}", track));
        const std::array<std::string_view, 1> output{snapshot.output};
        Text(ui, l10n.Format("playback.output", "Output: {0}", output));
    }
    switch (context.save_state.load(std::memory_order_acquire)) {
    case 1U:
        Text(ui, l10n.Text("settings.saving", "Settings: saving..."));
        break;
    case 2U:
        Text(ui, l10n.Text("settings.saved", "Settings: saved"));
        break;
    case 3U:
        Text(ui, l10n.Text("settings.save_failed", "Settings: save failed"));
        break;
    default:
        break;
    }
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    auto* const context = static_cast<Context*>(plugin_context);
    const AnomalyUiServiceV1* const ui = supplied_ui != nullptr
        ? supplied_ui
        : context == nullptr ? nullptr : context->ui;
    if (context == nullptr || !UiReady(ui)) return;
    try {
        const anomaly::plugins::Localizer& l10n = context->localizer;
        int open = 1;
        anomaly::sdk::UiWindow window(ui, l10n.Label("window.title", "HiFi Vehicle Music",
            "hifi-vehicle-music"), &open);
        if (!window) return;

        const hifi_vehicle_music::EngineSnapshot snapshot = context->engine.Snapshot();
        std::unique_lock lock(context->mutex);
        Settings& settings = context->settings;
        bool apply = false;
        bool changed = false;

        if (context->picked_folder) {
            settings.folder = std::move(*context->picked_folder);
            context->picked_folder.reset();
            apply = true;
        }

        int enabled = settings.enabled ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(l10n.Label(
                "option.enabled", "Replace vehicle music", "enabled")), &enabled) != 0) {
            settings.enabled = enabled != 0;
            g_enabled.store(settings.enabled, std::memory_order_release);
            if (!settings.enabled) context->engine.Stop();
            changed = true;
        }
        if (ui->slider_float(ui->user, anomaly::sdk::StringView(l10n.Label(
                "option.volume", "Volume", "volume")), &settings.volume, 0.0F, 1.0F) != 0) {
            settings.volume = std::clamp(settings.volume, 0.0F, 1.0F);
            context->engine.SetVolume(settings.volume);
            changed = true;
        }
        // Transport: previous / play-pause / next, usable while a song is loaded.
        if (Button(ui, l10n.Label("action.previous", "Previous", "previous"), snapshot.playing)) {
            context->engine.Previous();
        }
        ui->same_line(ui->user, 0.0F, -1.0F);
        const bool paused = snapshot.playing && snapshot.paused;
        if (Button(ui, paused ? l10n.Label("action.play", "Play", "play-pause")
                              : l10n.Label("action.pause", "Pause", "play-pause"),
                snapshot.playing)) {
            if (paused) {
                context->engine.Resume();
            } else {
                context->engine.Pause();
            }
        }
        ui->same_line(ui->user, 0.0F, -1.0F);
        if (Button(ui, l10n.Label("action.next", "Next", "next"), snapshot.playing)) {
            context->engine.Next();
        }
        ui->separator(ui->user);

        const std::string folder_label = settings.folder.empty()
            ? l10n.Text("folder.none", "(not selected)")
            : settings.folder;
        {
            const std::array<std::string_view, 1> args{folder_label};
            Text(ui, l10n.Format("folder.current", "Music folder: {0}", args));
        }
        const bool picker_busy = context->picker_busy.load(std::memory_order_acquire);
        if (Button(ui, l10n.Label("action.browse", "Browse...", "browse"), !picker_busy)) {
            StartFolderPicker(*context);
        }
        ui->same_line(ui->user, 0.0F, -1.0F);
        if (Button(ui, l10n.Label("action.rescan", "Rescan library", "rescan"),
                !settings.folder.empty())) {
            context->engine.Rescan();
        }
        if (settings.folder.empty()) {
            Text(ui, l10n.Text("warning.no_folder",
                "No music folder selected; the game keeps playing its own vehicle music."));
        } else if (snapshot.status != EngineStatus::Starting && snapshot.track_count == 0) {
            Text(ui, l10n.Text("warning.empty_library",
                "Library is empty; add .flac/.wav/.mp3 files to the folder."));
        }
        ui->separator(ui->user);

        Text(ui, l10n.Text("backend.title", "Output backend"));
        for (std::size_t i = 0; i < kBackendKeys.size(); ++i) {
            if (i != 0) ui->same_line(ui->user, 0.0F, -1.0F);
            if (Button(ui, l10n.Label(kBackendLabels[i].first, kBackendLabels[i].second,
                               kBackendKeys[i].second),
                    settings.backend != kBackendKeys[i].first)) {
                settings.backend = kBackendKeys[i].first;
                settings.device.clear();
                apply = true;
            }
        }
        const std::string default_device = l10n.Text("device.default", "(default)");
        {
            const std::array<std::string_view, 1> args{
                settings.device.empty() ? std::string_view(default_device)
                                        : std::string_view(settings.device)};
            Text(ui, l10n.Format("device.current", "Device: {0}", args));
        }
        if (Button(ui, default_device + "###device-default", !settings.device.empty())) {
            settings.device.clear();
            apply = true;
        }
        for (std::size_t i = 0; i < snapshot.devices.size(); ++i) {
            const std::string& name = snapshot.devices[i];
            if (Button(ui, name + "###device" + std::to_string(i), settings.device != name)) {
                settings.device = name;
                apply = true;
            }
        }
        Text(ui, l10n.Text("device.note",
            "This device only carries the replacement music; game audio keeps its own device."));
        std::uint32_t buffer_ms = settings.buffer_ms;
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView(l10n.Label(
                "option.buffer", "Buffer (ms)", "buffer")), &buffer_ms, 1, 10) != 0) {
            settings.buffer_ms = std::clamp(buffer_ms, 2U, 500U);
            changed = true;
        }
        if (Button(ui, l10n.Label("action.apply", "Apply", "apply"))) apply = true;
        if (apply) {
            context->engine.Configure(ToEngineSettings(settings));
            changed = true;
        }
        if (changed) {
            context->settings_dirty = true;
            ScheduleSettingsPersist(*context);
        }
        ui->separator(ui->user);

        DrawStatus(*context, ui, snapshot);
    } catch (...) {
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_plugin_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.hifi-vehicle-music"),
        anomaly::sdk::StringView("HiFi Vehicle Music"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, nullptr, Draw};
    return anomaly::sdk::Ok();
}
