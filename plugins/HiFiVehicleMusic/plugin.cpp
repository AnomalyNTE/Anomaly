#include "anomaly/sdk/cpp.hpp"

#include <Windows.h>
#include <winhttp.h>
#include <intrin.h>
#include <shobjidl.h>

#include <nlohmann/json.hpp>

#include "plugins/common/localization.hpp"
#include "plugins/HiFiVehicleMusic/audio_engine.hpp"
#include "plugins/HiFiVehicleMusic/hifi_vehicle_music_profile.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
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
using hifi_vehicle_music::DsdMode;
using hifi_vehicle_music::EngineSettings;
using hifi_vehicle_music::EngineStatus;
using hifi_vehicle_music::EngineSnapshot;

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
    "dsdMode":{"enum":["pcm","native-dop-pcm","dop-native-pcm"]},
    "bufferMs":{"type":"integer","minimum":2,"maximum":500},
    "volume":{"type":"number","minimum":0,"maximum":1},
    "musicFolder":{"type":"string","maxLength":4096},
    "metingKind":{"enum":["","search","playlist"]},
    "metingQuery":{"type":"string","maxLength":512}
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
constexpr std::array<std::pair<DsdMode, std::string_view>, 3> kDsdModeKeys{{
    {DsdMode::PcmOnly, "pcm"},
    {DsdMode::NativeThenDop, "native-dop-pcm"},
    {DsdMode::DopThenNative, "dop-native-pcm"},
}};
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kDsdModeLabels{{
    {"dsd.pcm", "PCM only"},
    {"dsd.native_dop_pcm", "Native > DoP > PCM"},
    {"dsd.dop_native_pcm", "DoP > Native > PCM"},
}};

// Hooks before kCoreHookCount are required for the takeover; the album hooks
// after it add the library to the in-game music list and are all-or-nothing.
//
// The stop path drains every hook with the host stop deadline (1s) as its only
// bound, so a detour that can outlive that budget -- a table walk, a row
// build, any call that blocks -- quarantines the plugin with "host stop
// deadline exceeded" (hifi-vehicle-music-hook-unload-quarantine.md). Every
// detour in this set is therefore bounded: snapshot reads, atomic stores,
// engine enqueues, cached name lookups, and original calls through their own
// (possibly nested) lease. The heavy work -- row builds, FText creation, the
// cover import -- lives on the builder thread, never under a callback lease.
enum HookIndex : std::size_t {
    kPost, kStop, kPause, kResume, kSetPlayerType, kEndGetOff, kCoreHookCount,
    kFindRow = kCoreHookCount, kForEachRow, kAlbumRow, kAlbumForEach, kOwnedCopy, kReGenerate,
    kSetCurrent, kItemRefresh, kVehiclePanel, kResolveCurrent, kSyncCurrent, kPageList, kMusicEnd,
    kPositionFraction, kPositionSeconds, kDuration, kChangeSound, kEntryClick, kDetailCover,
    kPageCover, kItemSelected, kRowClick, kListClick, kSetCurrentIdHook, kDuration2,
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

// The published album state. Detours only read it; the builder thread replaces
// it wholesale, so no detour ever walks a table, builds a row or waits on the
// builder. Rows are never freed: the game keeps row pointers in its UI
// objects, so a replaced snapshot leaks its rows like replaced rows did.
struct AlbumSnapshot final {
    std::uint32_t base{};  // ComparisonIndex of a real music row; 0 until one is seen
    std::byte* album{};    // synthetic "HiFi Library" album row; null until built
    std::vector<std::byte*> songs;
    // Every listed game song (IsList set, IsHidden clear); owned while the
    // game reads the owned ids, so locked game songs show as unlocked too.
    std::vector<std::uint64_t> game_songs;
    // The separate "Meting" album: its own album row and song rows, named
    // with kMetingAlbumNumber / kMetingSongNumberBase on the same base.
    std::byte* meting_album{};
    std::vector<std::byte*> meting_songs;
};

// What the table detours observed on their last walk or lookup: the template
// rows the synthetic rows are cloned from and the listed game song ids. The
// detours write it with bounded stores (no game calls); the builder thread
// consumes it off the callback leases.
struct AlbumCapture final {
    const std::byte* music_template{};
    const std::byte* album_template{};
    std::uint32_t base{};
    std::vector<std::uint64_t> game_songs;
};

// The builder thread's working state: the rows it last built and what they
// were built for, so a rebuild happens only when the library or a template
// actually changed. Owned by the builder thread alone.
struct AlbumBuild final {
    bool rows_valid{};
    std::uint64_t generation{};  // library generation the song rows were built for
    std::uint32_t base{};
    std::vector<std::byte*> songs;
    std::byte* album{};
    std::wstring album_name;  // name written into `album`
    std::wstring cover;       // image last imported; empty = none tried
    void* cover_texture{};    // its rooted UTexture2D, or null
    // Meting rows, rebuilt when the song list generation or the base changes.
    std::uint64_t meting_generation{};
    std::uint32_t meting_base{};
    std::vector<std::byte*> meting_songs;
    std::byte* meting_album{};
    bool meting_cover_tried{};  // one import attempt per plugin generation
    void* meting_cover_texture{};
};

// One song a Meting search or playlist returned.
struct MetingSong final {
    std::string name;
    std::string artist;
    std::string url;  // Meting proxy link; redirects to the real audio
    std::string pic;  // Meting proxy link to the song's cover (JPEG); may be empty
};

// A request queued for the Meting worker.
struct MetingRequest final {
    bool download{};
    std::string url;
    std::size_t index{};
    std::uint64_t generation{};
    // A list request: what produced it ("search"/"playlist" and the keyword or
    // id), saved once it succeeds so the next load can replay it.
    std::string kind;
    std::string query;
};

struct Settings final {
    bool enabled{true};
    Backend backend{Backend::WasapiExclusive};
    DsdMode dsd_mode{DsdMode::NativeThenDop};
    std::string device;
    std::uint32_t buffer_ms{20};
    float volume{1.0F};
    std::string folder;  // absolute UTF-8 path chosen by the user; empty = none
    // The last Meting list request that succeeded, replayed on load so the
    // Meting album survives a restart. Only the request is stored: no song
    // list, no audio. Empty kind = none.
    std::string meting_kind;   // "search" or "playlist"
    std::string meting_query;  // the keyword, or the playlist id
};

struct PlaybackLayout {
    std::uint32_t playing_id{};
    std::uint32_t paused{};
    std::uint32_t player_type{};
};

struct AlbumLayout {
    std::uint32_t current_id{};
    std::uint32_t owned_ids{};
};

struct Context final {
    const AnomalyCoreServiceV1* core{};
    const AnomalyConfigServiceV1* config{};
    const AnomalySignatureServiceV2* signature{};
    PlaybackLayout layout;
    std::optional<AlbumLayout> album_layout;
    const AnomalyHookServiceV1* hook{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalySchedulerServiceV1* scheduler{};
    // Optional: a per-frame Game-thread tick that keeps the game's current
    // song id on the library song the engine actually plays.
    const AnomalyUe5AhudServiceV1* ahud{};
    AnomalyGenerationHandleV1 ahud_subscription{};
    // Kept so the AHUD service can be looked up again: it is published only after
    // the game's reflection gate opens, which is after a boot-time load.
    const AnomalyHostApiV1* host{};
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
    // Builds and publishes the album snapshot off the callback leases and
    // retries the AHUD subscription that a boot-time load misses. Joined first
    // in Stop/Unload; every step between allocations checks the stop flag, so
    // the join is bounded.
    std::thread builder;
    std::atomic<bool> builder_stopping{};
    std::mutex builder_wake_mutex;
    std::condition_variable builder_wake;
    AlbumBuild build;  // builder-thread owned

    // Folder picker. The dialog runs on its own STA thread so Draw never blocks.
    std::thread picker;
    std::atomic<bool> picker_busy{};
    std::optional<std::string> picked_folder;

    // Meting: one worker thread runs every network request (search, playlist,
    // audio download) off the callback leases and off Draw. Stop closes the
    // in-flight WinHTTP handles so the worker's join stays bounded.
    std::thread meting_worker;
    std::atomic<bool> meting_stopping{};
    std::mutex meting_mutex;
    std::condition_variable meting_wake;
    std::deque<MetingRequest> meting_requests;  // guarded by meting_mutex
    // The song list the album shows; guarded by meting_mutex. The generation
    // changes whenever a search or playlist replaces it.
    std::vector<MetingSong> meting_songs;
    std::atomic<std::uint64_t> meting_generation{0};
    // The download queued or in flight; guarded by meting_mutex. The game posts
    // a pick several times in a row, and each must not download again.
    bool meting_download_pending{};
    std::size_t meting_download_index{};
    std::uint64_t meting_download_generation{};
    std::atomic<bool> meting_busy{};
    // Downloads failed in a row; reset by a successful one or a user pick.
    std::atomic<int> meting_failures{0};
    // Set by the worker when a list request succeeded with a new kind/query;
    // Draw, which owns the settings persistence, schedules the save.
    std::atomic<bool> meting_settings_changed{};
    std::string meting_status;  // guarded by meting_mutex
    std::array<char, 160> meting_query{};
    std::array<char, 512> meting_playlist_id{};  // an id or a whole share link
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

// ---- album snapshot -------------------------------------------------------
//
// Detours read the published snapshot and write the capture buffer; the
// builder thread turns captures into rows and publishes the next snapshot.
// Nothing below blocks on game calls or on plugin work that holds a callback
// lease.
std::atomic<std::shared_ptr<const AlbumSnapshot>> g_album;
std::atomic<void*> g_album_cover{nullptr};  // rooted UTexture2D built by the builder
std::atomic<void*> g_meting_cover{nullptr};  // the Meting album's cover, same lifetime
// The album thumbnail whose spinning disc shows a plugin album's cover: set by
// SetAlbumCover (Game thread), applied by the per-frame tick (Game thread). The
// class pointer guards against a freed thumbnail whose memory was reused.
std::atomic<std::uintptr_t> g_disc_view{0};
std::atomic<std::uintptr_t> g_disc_view_class{0};
std::atomic<std::uint64_t> g_disc_album{0};  // the album the thumbnail shows
// The player's spinning disc: the music detailed view, its class (stale-object
// guard) and the offset of its disc image. Found by the builder, read by the tick.
std::atomic<std::uintptr_t> g_player_view{0};
std::atomic<std::uintptr_t> g_player_view_class{0};
std::atomic<std::uint32_t> g_player_image_offset{0};
// Counts the moments the music UI shows an album or takes over a song; the
// builder searches for the player's disc only after one of them.
std::atomic<std::uint64_t> g_disc_cover_event{0};
// Per-song Meting cover. The worker downloads the image into memory (pending);
// the Game-thread tick imports it with ImportBufferAsTexture2D, roots it, and
// unroots the previous song's cover, so at most one song cover is kept.
// key = (generation << 32) | (index + 1); 0 = none.
std::atomic<std::uintptr_t> g_import_buffer{0};          // the UFunction
std::atomic<std::uintptr_t> g_rendering_library_cdo{0};  // its class default object
std::mutex g_song_cover_mutex;
std::vector<std::uint8_t> g_song_cover_pending;  // guarded by g_song_cover_mutex
std::uint64_t g_song_cover_pending_key{};        // guarded by g_song_cover_mutex
std::atomic<void*> g_song_cover{nullptr};         // Game thread writes; tick reads
std::atomic<std::uint64_t> g_song_cover_key{0};

// ProcessEvent parameter block of ImportBufferAsTexture2D: {UObject* world
// context, TArray<uint8> buffer, UTexture2D* return}.
struct ImportBufferParameters final {
    void* world_context;
    struct {
        const std::uint8_t* data;
        std::int32_t count;
        std::int32_t max;
    } buffer;
    void* result;
};
static_assert(sizeof(ImportBufferParameters) == 0x20);

std::uint64_t SongCoverKey(const std::size_t index, const std::uint64_t generation) noexcept {
    return (generation << 32U) | (static_cast<std::uint64_t>(index) + 1U);
}
// MaterialInstanceDynamic::SetTextureParameterValue and the disc parameter's
// FName; resolved once on the builder thread. 0 = not resolved.
std::atomic<std::uintptr_t> g_set_texture_parameter{0};
std::atomic<std::uint64_t> g_disc_parameter_name{0};
// The music/album UDataTable pointers, refreshed by the builder off the
// leases; detours compare their table argument against these.
std::atomic<std::uintptr_t> g_music_table{0};
std::atomic<std::uintptr_t> g_album_table{0};
// UHTUI_MusicPlayer panels the duration callbacks have seen. The game keeps
// more than one music panel alive, and the label each one shows lives on its
// owner's progress strip, so every callback registers its panel here and the
// pushes resolve the visible label of all of them.
std::atomic<std::uintptr_t> g_music_views[4]{};
std::mutex g_capture_mutex;
AlbumCapture g_capture;  // last observation of the game's table walks
// Serializes snapshot builds between the builder thread and the game thread
// (a list detour publishing on first open). The builder skips its cycle when
// the lock is held; a game-thread detour waits at most one build.
std::mutex g_build_mutex;

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

bool SignatureReady(const AnomalySignatureServiceV2* service) noexcept {
    return HIFI_HAS(AnomalySignatureServiceV2, resolve) &&
        HIFI_HAS(AnomalySignatureServiceV2, resolve_layout);
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
    result.dsd_mode = settings.dsd_mode;
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
                const std::string dsd_mode = json.value("dsdMode", std::string{});
                for (const auto& [value, key] : kDsdModeKeys) {
                    if (dsd_mode == key) settings.dsd_mode = value;
                }
                settings.device = json.value("device", settings.device);
                settings.buffer_ms = std::clamp(json.value("bufferMs", settings.buffer_ms), 2U, 500U);
                settings.volume = std::clamp(json.value("volume", settings.volume), 0.0F, 1.0F);
                // Only absolute paths: the package directory changes per load.
                const std::string folder = json.value("musicFolder", std::string{});
                if (IsAbsoluteFolder(folder)) settings.folder = folder;
                const std::string meting_kind = json.value("metingKind", std::string{});
                std::string meting_query = json.value("metingQuery", std::string{});
                if ((meting_kind == "search" || meting_kind == "playlist") && !meting_query.empty() &&
                    meting_query.size() <= 512U) {
                    settings.meting_kind = meting_kind;
                    settings.meting_query = std::move(meting_query);
                }
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
    std::string_view dsd_mode;
    for (const auto& [value, key] : kDsdModeKeys) {
        if (settings.dsd_mode == value) dsd_mode = key;
    }
    const std::string document = nlohmann::json{
        {"enabled", settings.enabled},
        {"backend", backend},
        {"dsdMode", dsd_mode},
        {"device", settings.device},
        {"bufferMs", settings.buffer_ms},
        {"volume", settings.volume},
        {"musicFolder", settings.folder},
        {"metingKind", settings.meting_kind},
        {"metingQuery", settings.meting_query},
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

// Runs `call(original)` with another hook's original under that hook's own
// lease. Calling a second hook's trampoline from inside a first hook's
// callback is otherwise unbounded: the host revokes hooks newest-first, so the
// callee's trampoline can be freed while the caller's callback is still in
// flight. The nested lease pins it for the call and lets the host's drain wait
// for it. Returns false when the lease is unavailable -- the plugin is
// stopping -- in which case the call is skipped.
template <typename Call>
bool CallUnderLease(const HookIndex index, Call&& call) noexcept {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(index, api, lease, original)) return false;
    call(original);
    EndCallback(api, lease);
    return true;
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

std::uint64_t SongId(const AlbumSnapshot& album, const std::size_t index) noexcept {
    return album.base |
        (static_cast<std::uint64_t>(profile::kSongNumberBase + index + 1U) << 32U);
}

std::uint64_t AlbumId(const std::uint32_t base) noexcept {
    return base | (static_cast<std::uint64_t>(profile::kAlbumNumber) << 32U);
}

// Library index of a synthetic song id.
std::optional<std::size_t> SongIndex(const AlbumSnapshot& album, const std::uint64_t id) noexcept {
    const auto number = static_cast<std::uint32_t>(id >> 32U);
    if (album.base == 0 || static_cast<std::uint32_t>(id) != album.base ||
        number <= profile::kSongNumberBase ||
        number - profile::kSongNumberBase > album.songs.size()) {
        return std::nullopt;
    }
    return number - profile::kSongNumberBase - 1U;
}

bool AlbumActive(const AlbumSnapshot& album) noexcept {
    return g_enabled.load(std::memory_order_acquire) && album.base != 0 &&
        album.album != nullptr && !album.songs.empty();
}

std::uint64_t MetingSongId(const AlbumSnapshot& album, const std::size_t index) noexcept {
    return album.base |
        (static_cast<std::uint64_t>(profile::kMetingSongNumberBase + index + 1U) << 32U);
}

std::uint64_t MetingAlbumId(const std::uint32_t base) noexcept {
    return base | (static_cast<std::uint64_t>(profile::kMetingAlbumNumber) << 32U);
}

// Meting song index of a synthetic id.
std::optional<std::size_t> MetingSongIndex(const AlbumSnapshot& album, const std::uint64_t id) noexcept {
    const auto number = static_cast<std::uint32_t>(id >> 32U);
    if (album.base == 0 || static_cast<std::uint32_t>(id) != album.base ||
        number <= profile::kMetingSongNumberBase ||
        number - profile::kMetingSongNumberBase > album.meting_songs.size()) {
        return std::nullopt;
    }
    return number - profile::kMetingSongNumberBase - 1U;
}

// The Meting album shows independently of the local library: a user without a
// music folder can still use it.
bool MetingAlbumActive(const AlbumSnapshot& album) noexcept {
    return g_enabled.load(std::memory_order_acquire) && album.base != 0 &&
        album.meting_album != nullptr && !album.meting_songs.empty();
}

// The event-name cache: resolving a FName goes through the host's names
// service, which takes the adapter's state mutex while the host publishes its
// semantic state. A detour must not wait there, so each name id is resolved
// once and then served from the cache.
struct NameCacheEntry final {
    std::uint32_t id{};
    std::string name;
};
std::mutex g_name_mutex;
std::array<NameCacheEntry, 32> g_name_cache;
std::size_t g_name_cache_next{};

std::string ResolveEventName(Context& context, const std::uint32_t name_id) {
    {
        std::scoped_lock lock(g_name_mutex);
        for (const auto& entry : g_name_cache) {
            if (entry.id == name_id) return entry.name;
        }
    }
    std::array<char, 256> buffer{};
    std::size_t size = buffer.size();
    if (context.names->resolve_utf8(context.names->user, name_id, buffer.data(), &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size > buffer.size()) {
        return {};
    }
    std::string name(buffer.data(), size);
    while (!name.empty() && name.back() == '\0') name.pop_back();
    {
        std::scoped_lock lock(g_name_mutex);
        g_name_cache[g_name_cache_next] = {name_id, name};
        g_name_cache_next = (g_name_cache_next + 1U) % g_name_cache.size();
    }
    return name;
}

// Returns true when the plugin plays the event itself and the original must be
// skipped. requested then names the album song the takeover switched to.
// Queues the download of Meting song `index`; defined with the Meting worker.
// Bounded: a mutex-guarded queue push and a condition-variable notify.
// False when the same pick is already downloading or nothing was queued.
bool QueueMetingDownload(Context& context, std::size_t index) noexcept;

bool TakeOverPost(Context& context, void* self, void* event, void* list_id,
    const float position, std::optional<std::size_t>& requested) noexcept {
    try {
        const auto subsystem = reinterpret_cast<std::uintptr_t>(self);
        std::uint8_t player_type{};
        std::uint32_t name_id{};
        if (event == nullptr ||
            !ReadValue(context, subsystem + context.layout.player_type, player_type) ||
            !ReadValue(context, reinterpret_cast<std::uintptr_t>(event) + profile::kObjectNameOffset,
                name_id)) {
            context.engine.Stop();
            return false;
        }
        const std::string name = ResolveEventName(context, name_id);
        if (name.empty()) {
            context.engine.Stop();
            return false;
        }
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(),
            [](const char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });

        // Only songs of the library album are taken over; game songs play
        // natively. Reading the published snapshot keeps this bounded: no
        // table walk, no row build, no lock the builder holds across game
        // calls.
        std::optional<std::size_t> song;
        std::optional<std::size_t> meting;
        std::uint64_t id{};
        if (list_id != nullptr &&
            ReadValue(context, reinterpret_cast<std::uintptr_t>(list_id), id)) {
            const auto album = g_album.load(std::memory_order_acquire);
            if (album != nullptr) {
                song = SongIndex(*album, id);
                if (!song && MetingAlbumActive(*album)) meting = MetingSongIndex(*album, id);
            }
        }
        EventDecision decision = EventDecision::Replaced;
        if (!g_enabled.load(std::memory_order_acquire)) {
            decision = EventDecision::Disabled;
        } else if (player_type != profile::kPlayerTypeVehicle) {
            decision = EventDecision::NotVehicle;
        } else if (!song && !meting) {
            decision = EventDecision::GameSong;
        } else if (song && !context.engine.CanPlay(key)) {
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
        if (meting) {
            // Re-posting the Meting song already playing (station change,
            // re-entering the vehicle) keeps it; a seek re-post only seeks.
            const auto playing = context.engine.MemorySlot();
            if (playing == meting) {
                if (position > 0.0F && position < 1.0F) context.engine.Seek(position);
                else context.engine.Resume();
            } else if (QueueMetingDownload(context, *meting)) {
                // A pick of the player's: failed downloads may be skipped again.
                context.meting_failures.store(0, std::memory_order_release);
                // The download runs on the Meting worker; the engine stops the
                // previous song now so the template event does not play meanwhile.
                context.engine.StopNow();
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    "Meting song " + std::to_string(*meting) + " picked; downloading");
            }
            return true;
        }
        context.engine.Play(std::move(key), position, song);
        requested = song;
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

void WritePlayingId(const Context& context, void* subsystem, const std::uint32_t id) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint32_t*>(
            static_cast<std::byte*>(subsystem) + context.layout.playing_id) = id;
    } __except (1) {
    }
}

void WritePausedFlag(const Context& context, void* subsystem, const std::uint8_t paused) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(
            static_cast<std::byte*>(subsystem) + context.layout.paused) = paused;
    } __except (1) {
    }
}

// Defined with the duration callbacks: writes the duration label the player
// shows through the game's own second duration callback. slot names the song
// the label should describe when the caller already knows it.
void PushDurationLabel(Context& context, const std::optional<std::size_t> requested = {}) noexcept;

void RememberSubsystem(void* subsystem) noexcept;
void CallSetCurrentId(void* subsystem, std::uint64_t id) noexcept;
std::uint64_t CurrentSongId(void* subsystem) noexcept;
bool IsLibrarySongId(std::uint64_t id) noexcept;

std::int64_t __fastcall PostDetour(void* self, void* event, void* list_id, const float position) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kPost, api, lease, original)) return 0;
    std::int64_t result = 0;
    auto* const context = g_context.load(std::memory_order_acquire);
    std::optional<std::size_t> requested;
    // `self` is the sound subsystem: remember it so the per-frame sync can keep
    // the current song id on the plugin song (a Meting pick reaches the post
    // without passing any other subsystem detour that would remember it).
    RememberSubsystem(self);
    std::uint64_t posted{};
    if (context != nullptr && list_id != nullptr)
        static_cast<void>(ReadValue(*context, reinterpret_cast<std::uintptr_t>(list_id), posted));
    if (context != nullptr && TakeOverPost(*context, self, event, list_id, position, requested)) {
        // The skipped Post would have replaced the current Wwise music; stop it
        // through the original so the engine is not notified. The nested lease
        // keeps the Stop trampoline alive for the call; when it is unavailable
        // the plugin is stopping and the Wwise music keeps fading on its own.
        CallUnderLease(kStop, [self](const std::uintptr_t stop) { CallStop(stop, self, 0); });
        // The player UI shows progress only for a non-zero PlayingID; the
        // position detours answer this one from the engine.
        WritePlayingId(*context, self, profile::kPlayingId);
        // The skipped Post would also have cleared the Paused flag. Left set
        // (e.g. after resuming through a repost) the next pause click is taken
        // for a resume and seems to do nothing.
        WritePausedFlag(*context, self, 0);
        // The Stop above broadcast the paused state; the replaced Post would
        // have broadcast the resume, so panels following the delegate would
        // disagree with the flag. Replay the resume through the original.
        CallUnderLease(kResume, [self](const std::uintptr_t resume) { CallVoid(resume, self); });
        // The replaced Post's chain never runs the duration query the player
        // label follows, so it would keep the previous song's length; write it
        // from the engine through the game's own callback. The engine's Play
        // command is asynchronous -- its playing slot still names the previous
        // song here -- so the requested song's length is pushed directly.
        PushDurationLabel(*context, requested);
        // A plugin song now plays: its disc in the player should show the cover.
        g_disc_cover_event.fetch_add(1, std::memory_order_acq_rel);
        // The original Stop above clears the subsystem's current song id. Left
        // empty, the paused player's play button finds no current song and
        // reports the album as locked, so the posted plugin song is restored at
        // once. Only an emptied id is restored: a non-empty one is the game's
        // own pick and is left to the per-frame sync.
        if (posted != 0 && IsLibrarySongId(posted) && CurrentSongId(self) == 0) {
            CallSetCurrentId(self, posted);
        }
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

// The library's cover image as a rooted transient texture, imported by the
// builder thread; null when there is no image or it cannot be imported (the
// widget keeps the template's cover). The album row is never changed: doing
// that hid the album. All game calls here run off the callback leases.
// A file in assets/ of the plugin's own package directory, or empty when it is
// missing: the host loads the DLL from its package cache, which mirrors the
// package contents.
std::wstring AssetPath(const wchar_t* const name) {
    std::array<wchar_t, 4096> buffer{};
    const DWORD size = GetModuleFileNameW(g_plugin_module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return {};
    const std::filesystem::path path =
        std::filesystem::path(std::wstring(buffer.data(), size)).parent_path() / L"assets" / name;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path.wstring() : std::wstring{};
}

// The local album's cover: the music folder's own cover image when it has one,
// otherwise the plugin's bundled library cover.
void* ImportCoverTexture(Context& context, AlbumBuild& build) {
    if (g_import_texture.load(std::memory_order_acquire) == 0) return nullptr;
    std::wstring path = context.engine.Cover();
    if (path.empty()) path = AssetPath(L"library-cover.png");
    if (path.empty()) return nullptr;
    if (path == build.cover) return build.cover_texture;
    build.cover = path;  // one attempt per image
    build.cover_texture = nullptr;
    void* const texture = CallImportTexture(path);
    std::int32_t index{};
    if (texture == nullptr ||
        !ReadValue(context, reinterpret_cast<std::uintptr_t>(texture) + profile::kObjectIndexOffset, index) ||
        !CallSetItemFlags(ObjectItem(context, index), static_cast<std::int32_t>(profile::kRootSetFlag))) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "album cover could not be imported");
        return nullptr;
    }
    build.cover_texture = texture;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "album cover imported");
    return texture;
}

using SetBrushFromTextureFn = void(__fastcall*)(void*, void*, std::uint8_t);

void SetImageTexture(void* image, void* texture) noexcept {
    __try {
        const auto vtable = *static_cast<std::uintptr_t* const*>(image);
        reinterpret_cast<SetBrushFromTextureFn>(
            vtable[profile::kImageSetBrushFromTextureSlot / sizeof(std::uintptr_t)])(image, texture, 0);
    } __except (1) {
    }
}

// The imported cover of a plugin album (local library or Meting), or null for
// a game album or a cover that was not imported.
void* CoverTexture(const AlbumSnapshot& snapshot, const std::uint64_t album) noexcept {
    if (AlbumActive(snapshot) && album == AlbumId(snapshot.base)) {
        return g_album_cover.load(std::memory_order_acquire);
    }
    if (MetingAlbumActive(snapshot) && album == MetingAlbumId(snapshot.base)) {
        return g_meting_cover.load(std::memory_order_acquire);
    }
    return nullptr;
}

// Puts the imported cover on `image` when `album` is a plugin album. The
// import runs on the builder thread; this only applies the published texture,
// so no game import runs inside a callback lease.
void ApplyCover(const std::uint64_t album, void* image) noexcept {
    if (image == nullptr) return;
    const auto snapshot = g_album.load(std::memory_order_acquire);
    if (snapshot == nullptr) return;
    void* const texture = CoverTexture(*snapshot, album);
    if (texture != nullptr) SetImageTexture(image, texture);
}

// Defined with the disc cover code below.
void ApplyDiscCover(Context& context) noexcept;

// The Meting album's cover, imported once on the builder thread like the local
// album's (the import is a game call, so it never runs inside a callback lease).
void* ImportMetingCover(Context& context, AlbumBuild& build) {
    if (build.meting_cover_tried) return build.meting_cover_texture;
    if (g_import_texture.load(std::memory_order_acquire) == 0) return nullptr;
    build.meting_cover_tried = true;
    const std::wstring path = AssetPath(L"meting-cover.png");
    if (path.empty()) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "Meting cover image is missing");
        return nullptr;
    }
    void* const texture = CallImportTexture(path);
    std::int32_t index{};
    if (texture == nullptr ||
        !ReadValue(context, reinterpret_cast<std::uintptr_t>(texture) + profile::kObjectIndexOffset, index) ||
        !CallSetItemFlags(ObjectItem(context, index), static_cast<std::int32_t>(profile::kRootSetFlag))) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "Meting cover could not be imported");
        return nullptr;
    }
    build.meting_cover_texture = texture;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "Meting cover imported");
    return texture;
}

// Records what the table detours observed: the row templates the synthetic
// rows are cloned from and the listed game song ids. Bounded stores only; the
// builder thread turns captures into rows off the callback leases.
void CaptureMusicWalk(const std::byte* first, const std::uint32_t base,
    std::vector<std::uint64_t>&& listed) noexcept {
    try {
        std::scoped_lock lock(g_capture_mutex);
        g_capture.music_template = first;
        g_capture.base = base;
        if (!listed.empty()) g_capture.game_songs = std::move(listed);
    } catch (...) {
    }
}

void CaptureMusicRow(void* row, const std::uint64_t name) noexcept {
    if (row == nullptr || name == 0) return;
    try {
        std::scoped_lock lock(g_capture_mutex);
        if (g_capture.music_template == nullptr) {
            g_capture.music_template = static_cast<const std::byte*>(row);
            g_capture.base = static_cast<std::uint32_t>(name);
        }
    } catch (...) {
    }
}

void CaptureAlbumRow(const void* row) noexcept {
    if (row == nullptr) return;
    try {
        std::scoped_lock lock(g_capture_mutex);
        if (g_capture.album_template == nullptr) {
            g_capture.album_template = static_cast<const std::byte*>(row);
        }
    } catch (...) {
    }
}

// A template no longer reads (a map switch replaced its table): drop it so the
// detours capture the next one they see.
void InvalidateMusicTemplate() noexcept {
    std::scoped_lock lock(g_capture_mutex);
    g_capture.music_template = nullptr;
    g_capture.base = 0;
}

void InvalidateAlbumTemplate() noexcept {
    std::scoped_lock lock(g_capture_mutex);
    g_capture.album_template = nullptr;
}

// Refreshes the music/album table pointers the detours compare against.
// GetGameInstance is a game call, so it runs here on the builder thread, never
// inside a callback lease.
void RefreshTables(const Context& context) {
    void* const game_instance = CallGameInstance();
    if (game_instance == nullptr) return;
    std::uintptr_t table{};
    if (ReadValue(context, reinterpret_cast<std::uintptr_t>(game_instance) +
            profile::kGameInstanceMusicTableOffset, table)) {
        g_music_table.store(table, std::memory_order_release);
    }
    if (ReadValue(context, reinterpret_cast<std::uintptr_t>(game_instance) +
            profile::kGameInstanceAlbumTableOffset, table)) {
        g_album_table.store(table, std::memory_order_release);
    }
}

// Builds the album row and one song row per library title from the latest
// captures and publishes the next snapshot. Runs on the builder thread: the
// game calls (FText, the row copies) happen here, never inside a callback
// lease, and every step checks the stop flag so Stop's join stays bounded.
// Rows replaced by a rescan are leaked on purpose: the game keeps row pointers
// in its UI objects.
void BuildAlbumSnapshot(Context& context) {
    if (context.builder_stopping.load(std::memory_order_acquire)) return;
    AlbumBuild& build = context.build;
    RefreshTables(context);

    const std::byte* music_template{};
    const std::byte* album_template{};
    std::uint32_t base{};
    std::vector<std::uint64_t> game_songs;
    {
        std::scoped_lock lock(g_capture_mutex);
        music_template = g_capture.music_template;
        album_template = g_capture.album_template;
        base = g_capture.base;
        game_songs = std::move(g_capture.game_songs);
    }
    const auto published = g_album.load(std::memory_order_acquire);
    if (game_songs.empty() && published != nullptr) game_songs = published->game_songs;
    const bool game_songs_changed = published == nullptr ||
        game_songs != published->game_songs;
    bool changed = game_songs_changed;

    // The song rows: rebuilt when the library, the name base or the template
    // changed since the ones in the build.
    const std::uint64_t library_generation = context.engine.LibraryGeneration();
    const bool rows_stale = music_template != nullptr &&
        (!build.rows_valid || build.generation != library_generation || build.base != base);
    if (rows_stale) {
        std::vector<std::wstring> titles = context.engine.Titles();
        if (titles.size() > profile::kMaxSongs) titles.resize(profile::kMaxSongs);
        // An empty library is usually the first scan still running. Publishing
        // it would record the generation and leave the album inactive until
        // the next scan, so the vehicle music stays the game's; keep the state
        // alone and let the next cycle build the rows once the scan has titles.
        if (!titles.empty()) {
            std::vector<std::byte*> songs;
            songs.reserve(titles.size());
            const std::array<std::byte, profile::kFStringSize> empty_string{};
            for (std::size_t i = 0; i < titles.size(); ++i) {
                if (context.builder_stopping.load(std::memory_order_acquire)) return;
                std::byte* const row = NewRow(music_template, profile::kMusicRowSize);
                if (row == nullptr ||
                    !MakeText(row + profile::kMusicTitleOffset, titles[i].c_str()) ||
                    !MakeText(row + profile::kMusicDescriptionOffset, L"")) {
                    // The template no longer reads; a later capture rebuilds.
                    InvalidateMusicTemplate();
                    build.rows_valid = false;
                    return;  // the partial row leaks like a replaced one
                }
                Put(row, profile::kMusicSortIndexOffset,
                    profile::kSortIndexBase + static_cast<std::int32_t>(i));
                std::memcpy(row + profile::kMusicCommentOffset, empty_string.data(),
                    empty_string.size());
                Put(row, profile::kMusicAlbumIdOffset, AlbumId(base));
                // The event keeps the template's asset (song end posts it) but
                // owns no string buffer of the template.
                std::memcpy(row + profile::kMusicEventOffset + profile::kMusicEventSubPathOffset,
                    empty_string.data(), empty_string.size());
                Put(row, profile::kMusicSourceItemOffset, std::uint64_t{0});
                Put(row, profile::kMusicSourceQuestOffset, std::uint64_t{0});
                Put(row, profile::kMusicIsListOffset, std::uint8_t{1});
                Put(row, profile::kMusicIsHiddenOffset, std::uint8_t{0});
                songs.push_back(row);
            }
            // The song ids name the base the rows were built for; rows built
            // for another base are leaked and rebuilt.
            build.songs = std::move(songs);
            build.generation = library_generation;
            build.base = base;
            build.rows_valid = true;
            changed = true;
        }
    }
    if (context.builder_stopping.load(std::memory_order_acquire)) return;

    // The album row: named after the library folder. It needs a template; the
    // album table walk records one. Choosing another folder writes a fresh
    // row (the old one leaks like replaced song rows).
    const std::wstring name = context.engine.LibraryName();
    if ((build.album == nullptr || name != build.album_name) && album_template != nullptr &&
        !name.empty()) {
        std::byte* const row = NewRow(album_template, profile::kAlbumRowSize);
        if (row == nullptr || !MakeText(row + profile::kAlbumNameOffset, name.c_str()) ||
            !MakeText(row + profile::kAlbumDescriptionOffset, L"")) {
            InvalidateAlbumTemplate();
            return;  // retried with the next capture
        }
        Put(row, profile::kAlbumSortIndexOffset, profile::kSortIndexBase);
        build.album = row;
        build.album_name = name;
        ImportCoverTexture(context, build);
        g_album_cover.store(build.cover_texture, std::memory_order_release);
        changed = true;
    }
    // The Meting album: independent of the local library, so it needs only the
    // templates and the base. Rebuilt when a search or playlist replaced the
    // song list or the base moved.
    const std::uint64_t meting_generation = context.meting_generation.load(std::memory_order_acquire);
    if (music_template != nullptr && base != 0 &&
        (build.meting_generation != meting_generation || build.meting_base != base)) {
        std::vector<MetingSong> meting;
        {
            std::scoped_lock lock(context.meting_mutex);
            meting = context.meting_songs;
        }
        if (meting.size() > profile::kMaxSongs) meting.resize(profile::kMaxSongs);
        std::vector<std::byte*> rows;
        rows.reserve(meting.size());
        const std::array<std::byte, profile::kFStringSize> empty_string{};
        for (std::size_t i = 0; i < meting.size(); ++i) {
            if (context.builder_stopping.load(std::memory_order_acquire)) return;
            std::byte* const row = NewRow(music_template, profile::kMusicRowSize);
            const std::wstring title = Utf8ToWide(meting[i].name + " - " + meting[i].artist);
            if (row == nullptr || !MakeText(row + profile::kMusicTitleOffset, title.c_str()) ||
                !MakeText(row + profile::kMusicDescriptionOffset, L"")) {
                InvalidateMusicTemplate();
                return;
            }
            Put(row, profile::kMusicSortIndexOffset,
                profile::kSortIndexBase + 0x8000 + static_cast<std::int32_t>(i));
            std::memcpy(row + profile::kMusicCommentOffset, empty_string.data(), empty_string.size());
            Put(row, profile::kMusicAlbumIdOffset, MetingAlbumId(base));
            std::memcpy(row + profile::kMusicEventOffset + profile::kMusicEventSubPathOffset,
                empty_string.data(), empty_string.size());
            Put(row, profile::kMusicSourceItemOffset, std::uint64_t{0});
            Put(row, profile::kMusicSourceQuestOffset, std::uint64_t{0});
            Put(row, profile::kMusicIsListOffset, std::uint8_t{1});
            Put(row, profile::kMusicIsHiddenOffset, std::uint8_t{0});
            rows.push_back(row);
        }
        build.meting_songs = std::move(rows);
        build.meting_generation = meting_generation;
        build.meting_base = base;
        changed = true;
    }
    if (build.meting_album == nullptr && album_template != nullptr) {
        std::byte* const row = NewRow(album_template, profile::kAlbumRowSize);
        if (row == nullptr || !MakeText(row + profile::kAlbumNameOffset, L"Meting") ||
            !MakeText(row + profile::kAlbumDescriptionOffset, L"")) {
            InvalidateAlbumTemplate();
            return;
        }
        Put(row, profile::kAlbumSortIndexOffset, profile::kSortIndexBase + 1);
        build.meting_album = row;
        changed = true;
    }
    if (build.meting_album != nullptr && !build.meting_cover_tried) {
        g_meting_cover.store(ImportMetingCover(context, build), std::memory_order_release);
    }
    if (!changed) return;
    // The local album needs valid song rows; the Meting album only its rows
    // for the current base. Publish when either has something to show.
    const bool meting_ready = build.meting_album != nullptr && !build.meting_songs.empty() &&
        build.meting_base == base && base != 0;
    if (!build.rows_valid && !meting_ready) return;

    auto next = std::make_shared<AlbumSnapshot>();
    next->base = build.rows_valid ? build.base : base;
    if (build.rows_valid && build.base == next->base) {
        next->album = build.album;
        next->songs = build.songs;
    }
    if (meting_ready && build.meting_base == next->base) {
        next->meting_album = build.meting_album;
        next->meting_songs = build.meting_songs;
    }
    next->game_songs = std::move(game_songs);
    g_album.store(std::move(next), std::memory_order_release);
}

// The library songs as (id, row), empty when the album is not shown.
std::vector<std::pair<std::uint64_t, std::byte*>> ActiveSongs() noexcept {
    std::vector<std::pair<std::uint64_t, std::byte*>> result;
    const auto album = g_album.load(std::memory_order_acquire);
    if (album == nullptr) return result;
    if (AlbumActive(*album)) {
        result.reserve(album->songs.size() + album->meting_songs.size());
        for (std::size_t i = 0; i < album->songs.size(); ++i) {
            result.emplace_back(SongId(*album, i), album->songs[i]);
        }
    }
    if (MetingAlbumActive(*album)) {
        for (std::size_t i = 0; i < album->meting_songs.size(); ++i) {
            result.emplace_back(MetingSongId(*album, i), album->meting_songs[i]);
        }
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
    Library,  // the local library songs only: play-queue paths while a local song plays
    Meting,   // the Meting songs only: play-queue paths while a Meting song plays
    All,      // both albums plus every listed game song: UI paths, so locked game songs show unlocked
};

// Adds the scope's ids to a TArray<FName>; returns exactly the ids that were
// not there before. Reads only the published snapshot; the game call is
// TArray<FName>::AddUnique at its resolved address, never a hook trampoline,
// so it cannot race a revocation.
std::vector<std::uint64_t> AddSongIds(void* array,
    const OwnedScope scope = OwnedScope::All) noexcept {
    std::vector<std::uint64_t> added;
    try {
        std::vector<std::uint64_t> ids;
        const auto snapshot = g_album.load(std::memory_order_acquire);
        for (const auto& [id, row] : ActiveSongs()) {
            const bool is_meting = snapshot != nullptr && MetingSongIndex(*snapshot, id).has_value();
            if (scope == OwnedScope::Library && is_meting) continue;
            if (scope == OwnedScope::Meting && !is_meting) continue;
            ids.push_back(id);
        }
        if (scope == OwnedScope::All && g_enabled.load(std::memory_order_acquire)) {
            const auto album = g_album.load(std::memory_order_acquire);
            if (album != nullptr) {
                ids.insert(ids.end(), album->game_songs.begin(), album->game_songs.end());
            }
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

// The game builds the album list before it ever walks the music table, so
// waiting for the table detours to observe a walk leaves the library album
// invisible until the first song plays. When the music template is still
// missing, walk the music table once with the original ForeachRow and record
// what the detours would have recorded. Game thread only (called from a list
// detour): UE table structures are not safe to walk from another thread while
// the game builds them, which crashed startup when this ran on the builder
// thread. The walk hook's lease pins the trampoline; FindRow calls the
// trampoline makes inside enter their own detours and take their own leases.
void PrimeMusicTemplate() {
    bool need_music = false;
    {
        std::scoped_lock lock(g_capture_mutex);
        need_music = g_capture.music_template == nullptr;
    }
    if (!need_music) return;
    const std::uintptr_t music_table = g_music_table.load(std::memory_order_acquire);
    if (music_table == 0) return;
    CallUnderLease(kForEachRow, [music_table](const std::uintptr_t original) {
        static const RowCallback noop{&NoopVisit, nullptr};
        std::vector<std::uint64_t> listed;
        RowRecorder recorder{&noop, nullptr, 0, &listed};
        RowCallback wrapped{&RecordRow, &recorder};
        CallForEach(original, reinterpret_cast<void*>(music_table), L"", &wrapped);
        if (recorder.first != nullptr) {
            CaptureMusicWalk(recorder.first, recorder.base, std::move(listed));
        }
    });
}

// Builds and publishes the snapshot synchronously when a game-thread list
// detour needs rows the builder thread has not published yet (first open of
// the music or album list). g_build_mutex serializes this against the builder
// thread's own build; the builder skips its cycle when the lock is held
// instead of making the game thread's detour wait behind it.
void BuildSnapshotInline(Context& context) {
    std::scoped_lock lock(g_build_mutex);
    BuildAlbumSnapshot(context);
}

// Bounded: the lookup only reads the published snapshot; a real row of the
// music table is recorded as the template the builder clones.
void* __fastcall FindRowDetour(void* table, const std::uint64_t name, const wchar_t* context_name,
    const char warn) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kFindRow, api, lease, original)) return nullptr;
    void* row = nullptr;
    const bool music_table = reinterpret_cast<std::uintptr_t>(table) ==
        g_music_table.load(std::memory_order_acquire);
    const auto album = g_album.load(std::memory_order_acquire);
    if (album != nullptr && music_table) {
        const auto song = SongIndex(*album, name);
        if (song && AlbumActive(*album)) {
            row = album->songs[*song];
        } else if (MetingAlbumActive(*album)) {
            const auto meting = MetingSongIndex(*album, name);
            if (meting) row = album->meting_songs[*meting];
        }
    }
    if (row == nullptr) {
        row = CallFindRow(original, table, name, context_name, warn);
        if (row != nullptr && music_table) CaptureMusicRow(row, name);
    }
    EndCallback(api, lease);
    return row;
}

// Bounded: the original walk runs with a recording wrapper (stores only), and
// the library rows come from the published snapshot. The first walk (nothing
// published yet) builds and publishes the snapshot inline on this game thread
// so the very first list is complete -- afterwards the builder thread keeps it
// current.
void __fastcall ForEachRowDetour(void* table, const wchar_t* context_name, void* callback) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kForEachRow, api, lease, original)) return;
    if (callback == nullptr || reinterpret_cast<std::uintptr_t>(table) !=
            g_music_table.load(std::memory_order_acquire)) {
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
        CaptureMusicWalk(recorder.first, recorder.base, std::move(listed));
    }
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr && g_album.load(std::memory_order_acquire) == nullptr) {
        BuildSnapshotInline(*context);
    }
    for (const auto& [id, row] : ActiveSongs()) CallVisit(inner, id, row);
    EndCallback(api, lease);
}

// Bounded: the library album row comes from the snapshot; a real album row is
// recorded as the template the builder clones the library album row from.
void* __fastcall AlbumRowDetour(void* self, const std::uint64_t* album_name, const std::uint8_t warn) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kAlbumRow, api, lease, original)) return nullptr;
    void* row = nullptr;
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uint64_t name{};
    const auto album = g_album.load(std::memory_order_acquire);
    if (context != nullptr && album != nullptr && album_name != nullptr &&
        ReadValue(*context, reinterpret_cast<std::uintptr_t>(album_name), name)) {
        if (AlbumActive(*album) && name == AlbumId(album->base)) {
            row = album->album;
        } else if (MetingAlbumActive(*album) && name == MetingAlbumId(album->base)) {
            row = album->meting_album;
        }
    }
    if (row == nullptr) {
        row = CallAlbumRow(original, self, album_name, warn);
        if (row != nullptr) CaptureAlbumRow(row);
    }
    EndCallback(api, lease);
    return row;
}

// Bounded: the original walk runs with a recording wrapper that also captures
// an album-row template; the library album row is visited from the snapshot.
// The first walk (nothing published yet) primes the missing music template on
// this game thread and builds and publishes the snapshot inline, so the album
// list is complete the first time it is shown.
void __fastcall AlbumForEachDetour(void* table, const wchar_t* context_name, void* callback) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kAlbumForEach, api, lease, original)) return;
    const bool album_table = callback != nullptr && reinterpret_cast<std::uintptr_t>(table) ==
        g_album_table.load(std::memory_order_acquire);
    if (album_table) {
        const auto* const inner = static_cast<const RowCallback*>(callback);
        RowRecorder recorder{inner, nullptr, 0, nullptr};
        RowCallback wrapped{&RecordRow, &recorder};
        CallForEach(original, table, context_name, &wrapped);
        if (recorder.first != nullptr) CaptureAlbumRow(recorder.first);
        auto* const context = g_context.load(std::memory_order_acquire);
        const auto current = g_album.load(std::memory_order_acquire);
        if (context != nullptr && (current == nullptr ||
                (current->album == nullptr && current->meting_album == nullptr))) {
            PrimeMusicTemplate();
            BuildSnapshotInline(*context);
        }
    } else {
        CallForEach(original, table, context_name, callback);
    }
    if (album_table) {
        const auto album = g_album.load(std::memory_order_acquire);
        if (album != nullptr && album->album != nullptr && AlbumActive(*album)) {
            CallVisit(static_cast<const RowCallback*>(callback), AlbumId(album->base), album->album);
        }
        if (album != nullptr && MetingAlbumActive(*album)) {
            CallVisit(static_cast<const RowCallback*>(callback), MetingAlbumId(album->base),
                album->meting_album);
        }
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
    if (result != nullptr) static_cast<void>(AddSongIds(result));
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
// Its vtable, remembered with it. The subsystem belongs to the world it was
// resolved in and a map switch destroys that world; the object at the address
// then carries another vtable. Reading and writing through a destroyed
// subsystem (the per-frame sync posts the current song id into it) corrupts
// whatever the allocator handed that memory to, so a stale one is dropped.
std::atomic<std::uintptr_t> g_last_subsystem_vtable{0};

void RememberSubsystem(void* subsystem) noexcept {
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uintptr_t vtable{};
    if (subsystem == nullptr || context == nullptr ||
        !ReadValue(*context, reinterpret_cast<std::uintptr_t>(subsystem), vtable) || vtable == 0) {
        g_last_subsystem.store(nullptr, std::memory_order_release);
        g_last_subsystem_vtable.store(0, std::memory_order_release);
        return;
    }
    g_last_subsystem_vtable.store(vtable, std::memory_order_release);
    g_last_subsystem.store(subsystem, std::memory_order_release);
}

// The remembered subsystem while it is still the object that was remembered;
// null once the world it belonged to is gone.
void* CachedSubsystem() noexcept {
    void* const subsystem = g_last_subsystem.load(std::memory_order_acquire);
    const std::uintptr_t vtable = g_last_subsystem_vtable.load(std::memory_order_acquire);
    if (subsystem == nullptr || vtable == 0) return nullptr;
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uintptr_t current{};
    if (context != nullptr &&
        ReadValue(*context, reinterpret_cast<std::uintptr_t>(subsystem), current) &&
        current == vtable) {
        return subsystem;
    }
    g_last_subsystem.store(nullptr, std::memory_order_release);
    g_last_subsystem_vtable.store(0, std::memory_order_release);
    return nullptr;
}

// A plugin song: a local library song or a Meting song. The play-queue and
// song-end paths treat both the same way -- the engine owns their playback.
bool IsLibrarySongId(const std::uint64_t id) noexcept {
    const auto album = g_album.load(std::memory_order_acquire);
    if (album == nullptr) return false;
    return (AlbumActive(*album) && SongIndex(*album, id).has_value()) ||
        (MetingAlbumActive(*album) && MetingSongIndex(*album, id).has_value());
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
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context == nullptr || !context->album_layout) return nullptr;
    if (subsystem == nullptr) subsystem = CachedSubsystem();
    return subsystem == nullptr ? nullptr : static_cast<std::byte*>(subsystem) + context->album_layout->owned_ids;
}

std::uint64_t CurrentSongId(void* subsystem) noexcept {
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uint64_t id{};
    if (context == nullptr || !context->album_layout || subsystem == nullptr ||
        !ReadValue(*context, reinterpret_cast<std::uintptr_t>(subsystem) + context->album_layout->current_id, id)) {
        return 0;
    }
    return id;
}

// UI readers: library songs and every listed game song are owned for the
// outermost call; nested calls see them already in place.
template <typename Call>
void WithSongIds(void* subsystem, Call&& call) {
    void* const owned = OwnedArray(subsystem);
    std::vector<std::uint64_t> added;
    if (t_owned_depth == 0 && owned != nullptr) {
        added = AddSongIds(owned, OwnedScope::All);
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
    void* const owned = OwnedArray(subsystem);
    std::vector<std::uint64_t> saved;
    if (owned == nullptr || !IsLibrarySongId(song) || !ReadIds(owned, saved)) {
        WithSongIds(subsystem, std::forward<Call>(call));
        return;
    }
    ClearIds(owned);
    // The queue holds only the album the playing song belongs to, so the
    // game's next/previous and song end stay in that album: a Meting song
    // never advances into the local library and the other way round.
    const auto album = g_album.load(std::memory_order_acquire);
    const bool meting = album != nullptr && MetingSongIndex(*album, song).has_value();
    static_cast<void>(AddSongIds(owned, meting ? OwnedScope::Meting : OwnedScope::Library));
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
    RememberSubsystem(self);
    WithQueueIds(self, CurrentSongId(self), [&] { CallVoid(original, self); });
    EndCallback(api, lease);
}

// Diagnostic (off): names the game call site of a hooked function as
// HTGame+offset. Its call sites are commented out with it; re-enable both to
// calibrate a hook offset against a new game build.
// void LogCaller(const char* what, void* return_address, const std::uint64_t id) noexcept {
//     auto* const context = g_context.load(std::memory_order_acquire);
//     if (context == nullptr) return;
//     try {
//         const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"HTGame.exe"));
//         const auto address = reinterpret_cast<std::uintptr_t>(return_address);
//         char offset[32]{};
//         std::snprintf(offset, sizeof(offset), "0x%llx",
//             static_cast<unsigned long long>(base != 0 && address > base ? address - base : address));
//         // The 24 bytes before the return address, so the call site can be found
//         // by pattern even if this build's layout differs from the analysed one.
//         std::string bytes;
//         std::array<std::uint8_t, 24> code{};
//         if (address > code.size() && ReadValue(*context, address - code.size(), code)) {
//             for (const std::uint8_t b : code) {
//                 char hex[4]{};
//                 std::snprintf(hex, sizeof(hex), "%02X ", b);
//                 bytes += hex;
//             }
//         }
//         Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
//             std::string(what) + " from HTGame+" + offset + ", library " +
//                 (IsLibrarySongId(*context, id) ? "yes" : "no") + ", code before: " + bytes);
//     } catch (...) {
//     }
// }

void __fastcall SetCurrentDetour(void* self, const std::uint64_t id) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kSetCurrent, api, lease, original)) return;
    // LogCaller("SetCurrentMusicListID", _ReturnAddress(), id);  // probe, see LogCaller
    RememberSubsystem(self);
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
    RememberSubsystem(self);
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
    const bool library = context != nullptr && IsLibrarySongId(CurrentSongId(self));
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
// True while the takeover owns playback: the PlayingID is the one Post wrote.
// A game song's post replaces it with a real Wwise id.
bool TakenOver(const Context& context, void* subsystem) noexcept {
    std::uint32_t id{};
    return subsystem != nullptr &&
        ReadValue(context, reinterpret_cast<std::uintptr_t>(subsystem) + context.layout.playing_id, id) &&
        id == profile::kPlayingId;
}

void ANOMALY_CALL SyncCurrentSong(void* user, const AnomalyUe5AhudFrameV1*) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    ApplyDiscCover(*context);
    if (g_set_current_id.load(std::memory_order_acquire) == 0) return;
    try {
        void* const subsystem = CachedSubsystem();
        const std::optional<std::size_t> slot = context->engine.PlayingSlot();
        static std::optional<std::size_t> last_slot;
        static std::optional<std::chrono::steady_clock::time_point> mismatch_since;
        const bool engine_moved = slot != last_slot;
        last_slot = slot;
        // The engine moved to another song (switch or auto-advance): the game
        // never queries the length on that path, so write the player UI's
        // duration label from the engine. Pushed before the takeover gate
        // below: the game rewrites its playing-id field exactly while it
        // rebuilds panel state, and this push fires only on the transition
        // frame -- gating it there can swallow it for good.
        if (engine_moved && slot) PushDurationLabel(*context);
        // A Meting song starting: its length is known only once decoded.
        static std::optional<std::size_t> last_memory;
        const std::optional<std::size_t> memory = context->engine.MemorySlot();
        const bool memory_moved = memory != last_memory;
        if (memory_moved) {
            last_memory = memory;
            if (memory) PushDurationLabel(*context);
        }
        if (subsystem == nullptr || (!slot && !memory) || !TakenOver(*context, subsystem)) {
            mismatch_since.reset();
            return;
        }
        const std::uint64_t current = CurrentSongId(subsystem);
        const auto album = g_album.load(std::memory_order_acquire);
        if (album == nullptr) return;
        // The plugin song the engine plays: a local library song or, from
        // memory, a Meting song. Both drift the same way when the game rebuilds
        // its state, and a play click then names a song the album does not own.
        std::uint64_t wanted{};
        if (slot) {
            if (!AlbumActive(*album) || *slot >= album->songs.size()) return;
            wanted = SongId(*album, *slot);
        } else {
            if (!MetingAlbumActive(*album) || *memory >= album->meting_songs.size()) return;
            wanted = MetingSongId(*album, *memory);
        }
        const bool current_is_library = IsLibrarySongId(current);
        if (wanted == current) {
            mismatch_since.reset();
            return;
        }
        // The engine moved on by itself (song end, the window's previous/next):
        // follow at once. Otherwise the game changed its current song without a
        // post (it restores the saved last song, a game song, when it rebuilds
        // its state), while the library song keeps playing. A song the player
        // picked posts within moments and ends the takeover, so only a mismatch
        // that outlasts that is corrected; acting at once made picks need a
        // second click.
        const auto now = std::chrono::steady_clock::now();
        if (!mismatch_since) mismatch_since = now;
        // An empty current id is no pick of the player's: nothing to wait for.
        if (current == 0 || ((engine_moved || memory_moved) && current_is_library) ||
            now - *mismatch_since >= std::chrono::seconds(2)) {
            mismatch_since.reset();
            CallSetCurrentId(subsystem, wanted);
        }
    } catch (...) {
    }
}

using PositionFn = float(__fastcall*)(std::uint32_t);
using DurationFn = std::uintptr_t(__fastcall*)(void*, void*);

float CallPosition(const std::uintptr_t original, const std::uint32_t id) noexcept {
    __try {
        return reinterpret_cast<PositionFn>(original)(id);
    } __except (1) {
        return 0.0F;
    }
}

std::uintptr_t CallDuration(const std::uintptr_t original, void* user, void* info) noexcept {
    __try {
        return reinterpret_cast<DurationFn>(original)(user, info);
    } __except (1) {
        return 0;
    }
}

void WriteDuration(void* info, const float seconds) noexcept {
    __try {
        *reinterpret_cast<float*>(static_cast<std::byte*>(info) + profile::kDurationInfoSecondsOffset) =
            seconds;
    } __except (1) {
    }
}

// The strip's own length cache: its blueprint re-formats the duration label
// from it, so it moves with the label on every push.
void WriteStripDuration(void* strip, const float seconds) noexcept {
    __try {
        *reinterpret_cast<float*>(static_cast<std::byte*>(strip) +
            profile::kUiStripDurationCacheOffset) = seconds;
    } __except (1) {
    }
}

// A duration callback revealed this panel view; true when it was newly added.
// Slots hold distinct views: a single register would lose the second panel
// the moment any callback for the first one fires again.
bool RegisterMusicView(const std::uintptr_t ui) noexcept {
    if (ui == 0) return false;
    for (auto& slot : g_music_views) {
        if (slot.load(std::memory_order_relaxed) == ui) return false;
    }
    for (auto& slot : g_music_views) {
        std::uintptr_t expected = 0;
        if (slot.compare_exchange_strong(expected, ui,
                std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
    }
    // Full: replace round-robin; live panels re-register on every callback.
    static std::atomic<std::size_t> next{0};
    g_music_views[next.fetch_add(1, std::memory_order_relaxed) % std::size(g_music_views)]
        .store(ui, std::memory_order_release);
    return true;
}

// Wwise position queries for the player UI. The takeover's PlayingID is
// answered from the engine; any other id goes to Wwise.
float PositionDetour(const HookIndex index, const std::uint32_t id, const bool fraction) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(index, api, lease, original)) return 0.0F;
    float result = 0.0F;
    auto* const context = g_context.load(std::memory_order_acquire);
    if (id == profile::kPlayingId && context != nullptr) {
        const AudioEngine::Progress progress = context->engine.Position();
        if (!fraction) {
            result = progress.seconds;
        } else if (progress.duration > 0.0F) {
            result = std::clamp(progress.seconds / progress.duration, 0.0F, 1.0F);
        }
    } else {
        result = CallPosition(original, id);
    }
    EndCallback(api, lease);
    return result;
}

float __fastcall PositionFractionDetour(const std::uint32_t id) {
    return PositionDetour(kPositionFraction, id, true);
}

float __fastcall PositionSecondsDetour(const std::uint32_t id) {
    return PositionDetour(kPositionSeconds, id, false);
}

// The player shows the length Wwise reports for the row's event, which for a
// library song is the template song's; replace it with the file's length.
// The current library song's file length, or 0 when a game song is current.
float CurrentLibraryDuration(Context& context) noexcept {
    void* const subsystem = CachedSubsystem();
    if (subsystem == nullptr) return 0.0F;
    try {
        // The song the engine plays: whenever it has a slot it owns playback,
        // whatever the subsystem's playing-id field says -- the game rewrites
        // that field when it rebuilds its panel state, and gating on it made
        // the length vanish exactly then. Otherwise the game's current song
        // if it is a library song (about to be posted).
        // A Meting song plays from memory; its length is the decoder's.
        if (context.engine.MemorySlot()) return context.engine.MemoryDuration();
        std::optional<std::size_t> song = context.engine.PlayingSlot();
        if (!song) {
            const auto album = g_album.load(std::memory_order_acquire);
            if (album != nullptr && AlbumActive(*album)) {
                song = SongIndex(*album, CurrentSongId(subsystem));
            }
        }
        return song ? context.engine.Duration(*song) : 0.0F;
    } catch (...) {
        return 0.0F;
    }
}

std::uintptr_t __fastcall DurationDetour(void* user, void* info) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kDuration, api, lease, original)) return 0;
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr && info != nullptr) {
        // user is {uint32 playing_id; UHTUI_MusicPlayer* ui}: the label owner.
        std::uintptr_t ui{};
        if (user != nullptr) {
            ReadValue(*context, reinterpret_cast<std::uintptr_t>(user) + 8, ui);
            RegisterMusicView(ui);
        }
        const float seconds = CurrentLibraryDuration(*context);
        if (seconds > 0.0F) WriteDuration(info, seconds);
    }
    const std::uintptr_t result = CallDuration(original, user, info);
    EndCallback(api, lease);
    return result;
}

// The album view's play/resume path reports the length through a second
// callback whose info argument is one pointer further away.
std::uintptr_t __fastcall Duration2Detour(void* user, void** info) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kDuration2, api, lease, original)) return 0;
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uintptr_t inner{};
    if (context != nullptr && info != nullptr &&
        ReadValue(*context, reinterpret_cast<std::uintptr_t>(info), inner) && inner != 0) {
        // user is {UHTUI_MusicPlayer* ui}: the label owner.
        std::uintptr_t ui{};
        if (user != nullptr) {
            ReadValue(*context, reinterpret_cast<std::uintptr_t>(user), ui);
            RegisterMusicView(ui);
        }
        const float seconds = CurrentLibraryDuration(*context);
        if (seconds > 0.0F) WriteDuration(reinterpret_cast<void*>(inner), seconds);
    }
    const std::uintptr_t result = CallDuration(original, user, info);
    EndCallback(api, lease);
    return result;
}

// The duration label a player panel shows. The game registers the panel
// itself with its duration queries, but the panel's label slot is empty: the
// visible time labels live on the progress strip of the music view that owns
// the panel -- panel -> widget tree -> owning view -> strip (+0x1000) ->
// duration label (+0xF98). The owner check (+0xFF8 == panel) rejects views
// that do not own this panel (the vehicle panel's tree, for one). The chain
// is walked fresh on every push: the panels are rebuilt as the player mounts
// and unmounts, and a freed panel no longer passes the owner check. The strip
// is returned with the label: its own length cache (+0xFA4) feeds the
// blueprint that re-formats the label, and leaving it at the template's 30
// seconds would write the stale text right back.
std::uintptr_t ResolveDurationLabel(const Context& context, const std::uintptr_t panel,
    std::uintptr_t& strip) noexcept {
    std::uintptr_t tree{}, owner{}, owner_panel{}, label{}, label_vtable{};
    strip = 0;
    if (!ReadValue(context, panel + profile::kUiOuterOffset, tree) || tree == 0) return 0;
    if (!ReadValue(context, tree + profile::kUiOuterOffset, owner) || owner == 0) return 0;
    if (!ReadValue(context, owner + profile::kUiOwnerPanelOffset, owner_panel) ||
        owner_panel != panel) {
        return 0;
    }
    if (!ReadValue(context, owner + profile::kUiOwnerStripOffset, strip) || strip == 0) return 0;
    if (!ReadValue(context, strip + profile::kUiStripDurationLabelOffset, label) ||
        label == 0 || !ReadValue(context, label, label_vtable) || label_vtable == 0) {
        strip = 0;
        return 0;
    }
    return label;
}

void PushDurationLabel(Context& context, const std::optional<std::size_t> requested) noexcept {
    // PlayingSlot() lags a just-posted switch (the engine thread has not run
    // the command yet); the durations table answers the requested song at once.
    const float seconds = requested ? context.engine.Duration(*requested) : CurrentLibraryDuration(context);
    if (seconds <= 0.0F) return;
    for (const auto& slot : g_music_views) {
        const std::uintptr_t ui = slot.load(std::memory_order_acquire);
        if (ui == 0) continue;
        std::uintptr_t strip{};
        const std::uintptr_t label = ResolveDurationLabel(context, ui, strip);
        // The nested lease pins the callback's trampoline for the calls; the
        // callback reads the seconds from *info + 0x58, caches them at
        // view +0x10DC and SetTexts the label at view +0x1018.
        CallUnderLease(kDuration2, [&](const std::uintptr_t duration) {
            std::byte info[96] = {};
            *reinterpret_cast<float*>(info + profile::kDurationInfoSecondsOffset) = seconds;
            void* inner = info;
            // The panel itself: refresh its length cache the way the game's
            // own query for this panel would have.
            void* panel_user[1] = {reinterpret_cast<void*>(ui)};
            static_cast<void>(CallDuration(duration, panel_user, &inner));
            // The strip's cache first: the label text the SetText below
            // writes is re-formatted from it.
            if (strip != 0) WriteStripDuration(reinterpret_cast<void*>(strip), seconds);
            if (label == 0) return;
            // A synthetic view whose label slot points at the strip's duration
            // label: the game's own callback then writes, through its own
            // formatter, the label the player actually sees.
            alignas(8) std::byte view[profile::kUiDurationCacheOffset + sizeof(float)] = {};
            *reinterpret_cast<std::uintptr_t*>(view + profile::kUiDurationLabelOffset) = label;
            void* view_user[1] = {view};
            static_cast<void>(CallDuration(duration, view_user, &inner));
        });
    }
}

using ChangeSoundFn = void(__fastcall*)(void*, float);

void CallChangeSound(const std::uintptr_t original, void* self, const float position) noexcept {
    __try {
        reinterpret_cast<ChangeSoundFn>(original)(self, position);
    } __except (1) {
    }
}

// The progress slider seeks through ChangePlayerMusicSound(position). For a
// library song that would reload and repost the template event; seek the
// engine instead. Callers that change song (position 0) are left alone.
void __fastcall ChangeSoundDetour(void* self, const float position) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kChangeSound, api, lease, original)) return;
    // LogCaller("ChangePlayerMusicSound", _ReturnAddress(), CurrentSongId(self));  // probe, see LogCaller
    auto* const context = g_context.load(std::memory_order_acquire);
    // Seek the engine whenever it owns playback, also when the game's current
    // song id has drifted to a game song.
    const bool seek = context != nullptr && position > 0.0F && position < 1.0F && TakenOver(*context, self) &&
        (context->engine.PlayingSlot().has_value() || context->engine.MemorySlot().has_value());
    if (seek) {
        try {
            context->engine.Seek(position);
        } catch (...) {
        }
    } else {
        CallChangeSound(original, self, position);
    }
    EndCallback(api, lease);
}

using EntryClickFn = std::uintptr_t(__fastcall*)(void*, const std::uint64_t*);

std::uintptr_t CallEntryClick(const std::uintptr_t original, void* self, const std::uint64_t* id) noexcept {
    __try {
        return reinterpret_cast<EntryClickFn>(original)(self, id);
    } __except (1) {
        return 0;
    }
}

// A song clicked in the album view. The game plays it only when it finds the
// song among the panel's list entries, which it does not when the album has
// fewer songs than the list shows at once. When a library song was clicked
// and the game did not switch to it, switch the way a song end does: queue
// it, make it current and post it (the post is taken over as usual).
std::uintptr_t __fastcall EntryClickDetour(void* self, const std::uint64_t* id) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kEntryClick, api, lease, original)) return 0;
    auto* const context = g_context.load(std::memory_order_acquire);
    const std::uintptr_t result = CallEntryClick(original, self, id);
    std::uint64_t song{};
    if (context != nullptr && id != nullptr &&
        ReadValue(*context, reinterpret_cast<std::uintptr_t>(id), song) && IsLibrarySongId(song)) {
        void* subsystem = CallSoundSubsystem(self);
        if (subsystem == nullptr) subsystem = CachedSubsystem();
        if (subsystem != nullptr && CurrentSongId(subsystem) != song) {
            // Both calls go through other hooks' trampolines, so each runs
            // under its own nested lease (see CallUnderLease).
            CallUnderLease(kSetCurrent, [&](const std::uintptr_t set_current) {
                WithQueueIds(subsystem, song, [&] { CallSetCurrent(set_current, subsystem, song); });
            });
            CallSetCurrentId(subsystem, song);
            CallUnderLease(kChangeSound, [&](const std::uintptr_t change) {
                CallChangeSound(change, subsystem, 0.0F);
            });
        }
    }
    EndCallback(api, lease);
    return result;
}

using DetailCoverFn = char(__fastcall*)(void*, std::uint64_t);
using PageCoverFn = std::uintptr_t(__fastcall*)(void*, void*);

char CallDetailCover(const std::uintptr_t original, void* self, const std::uint64_t album) noexcept {
    __try {
        return reinterpret_cast<DetailCoverFn>(original)(self, album);
    } __except (1) {
        return 0;
    }
}

std::uintptr_t CallPageCover(const std::uintptr_t original, void* self, void* item) noexcept {
    __try {
        return reinterpret_cast<PageCoverFn>(original)(self, item);
    } __except (1) {
        return 0;
    }
}

void* ReadPointer(const Context& context, void* base, const std::uint32_t offset) noexcept {
    std::uintptr_t value{};
    if (base == nullptr || !ReadValue(context, reinterpret_cast<std::uintptr_t>(base) + offset, value)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(value);
}

// The album view's cover image.
char __fastcall DetailCoverDetour(void* self, const std::uint64_t album) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kDetailCover, api, lease, original)) return 0;
    const char result = CallDetailCover(original, self, album);
    auto* const context = g_context.load(std::memory_order_acquire);
    if (context != nullptr) {
        // The thumbnail's cover is the label of the spinning disc, drawn through
        // a material: remember it for the per-frame tick, which writes the cover
        // into the material (a brush swap here is overwritten by the blueprint).
        // The album, not its texture, is remembered: the tick looks the cover up
        // each frame, so a thumbnail shown before the album's songs or cover
        // were ready (e.g. right after a hot reload) still gets it once they are.
        // A game album finds no plugin cover there and keeps its own.
        std::uintptr_t klass{};
        if (ReadValue(*context, reinterpret_cast<std::uintptr_t>(self) + profile::kObjectClassOffset, klass)) {
            g_disc_album.store(album, std::memory_order_release);
            g_disc_view_class.store(klass, std::memory_order_release);
            g_disc_view.store(reinterpret_cast<std::uintptr_t>(self), std::memory_order_release);
        }
        // The music UI is up: the builder may look for the player's disc now.
        g_disc_cover_event.fetch_add(1, std::memory_order_acq_rel);
    }
    EndCallback(api, lease);
    return result;
}

// An album tile in the album list.
std::uintptr_t __fastcall PageCoverDetour(void* self, void* item) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kPageCover, api, lease, original)) return 0;
    const std::uintptr_t result = CallPageCover(original, self, item);
    auto* const context = g_context.load(std::memory_order_acquire);
    std::uint64_t album{};
    if (context != nullptr && item != nullptr &&
        ReadValue(*context, reinterpret_cast<std::uintptr_t>(item) + profile::kPageItemAlbumIdOffset, album)) {
        ApplyCover(album, ReadPointer(*context, self, profile::kPageCoverImageOffset));
    }
    EndCallback(api, lease);
    return result;
}

using RowClickFn = std::uint8_t(__fastcall*)(void*);
using ListClickFn = std::uint8_t(__fastcall*)(void*, void*);

std::uint8_t CallRowClick(const std::uintptr_t original, void* self) noexcept {
    __try {
        return reinterpret_cast<RowClickFn>(original)(self);
    } __except (1) {
        return 0;
    }
}

std::uint8_t CallListClick(const std::uintptr_t original, void* list, void* item) noexcept {
    __try {
        return reinterpret_cast<ListClickFn>(original)(list, item);
    } __except (1) {
        return 0;
    }
}

// Diagnostic probe (off): a list row handling a click. It logged the row's
// click-related fields (+0x420, +0x38C, +0x4B8, +0x35E) and the handler result.
std::uint8_t __fastcall RowClickDetour(void* self) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kRowClick, api, lease, original)) return 0;
    // auto* const context = g_context.load(std::memory_order_acquire);
    // std::int32_t method{}, mode{};
    // std::uint8_t flag_4b8{}, flag_35e{};
    // if (context != nullptr && self != nullptr) {
    //     const auto row = reinterpret_cast<std::uintptr_t>(self);
    //     static_cast<void>(ReadValue(*context, row + 0x420, method));
    //     static_cast<void>(ReadValue(*context, row + 0x38C, mode));
    //     static_cast<void>(ReadValue(*context, row + 0x4B8, flag_4b8));
    //     static_cast<void>(ReadValue(*context, row + 0x35E, flag_35e));
    // }
    const std::uint8_t result = CallRowClick(original, self);
    // if (context != nullptr) {
    //     char text[128]{};
    //     std::snprintf(text, sizeof(text), "row click: method %d mode %d f4b8 %d f35e %d -> %d", method, mode,
    //         static_cast<int>(flag_4b8), static_cast<int>(flag_35e), static_cast<int>(result));
    //     Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, text);
    // }
    EndCallback(api, lease);
    return result;
}

// Diagnostic probe (off): the list broadcasting that an item was clicked.
std::uint8_t __fastcall ListClickDetour(void* list, void* item) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kListClick, api, lease, original)) return 0;
    const std::uint8_t result = CallListClick(original, list, item);
    // auto* const context = g_context.load(std::memory_order_acquire);
    // if (context != nullptr) {
    //     Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
    //         "list click broadcast -> " + std::to_string(static_cast<int>(result)));
    // }
    EndCallback(api, lease);
    return result;
}

void CallSetCurrentIdOriginal(const std::uintptr_t original, void* self, const std::uint64_t* id) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, const std::uint64_t*)>(original)(self, id);
    } __except (1) {
    }
}

// Diagnostic probe (off): who makes a song current, logged as the game call site
// so the album view's own click handler could be found.
void __fastcall SetCurrentIdDetour(void* self, const std::uint64_t* id) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kSetCurrentIdHook, api, lease, original)) return;
    // auto* const context = g_context.load(std::memory_order_acquire);
    // std::uint64_t song{};
    // if (context != nullptr && id != nullptr && ReadValue(*context, reinterpret_cast<std::uintptr_t>(id), song) &&
    //     song != CurrentSongId(self)) {
    //     LogCaller("SetCurrentPlayerMusicListID", _ReturnAddress(), song);
    // }
    CallSetCurrentIdOriginal(original, self, id);
    EndCallback(api, lease);
}

using ItemSelectedFn = void(__fastcall*)(void*, void*);

void CallItemSelected(const std::uintptr_t original, void* self, void* item) noexcept {
    __try {
        reinterpret_cast<ItemSelectedFn>(original)(self, item);
    } __except (1) {
    }
}

// Diagnostic probe (off): logged each list-entry selection with the entry's song.
void __fastcall ItemSelectedDetour(void* self, void* item) {
    const AnomalyHookServiceV1* api{};
    AnomalyGenerationHandleV1 lease{};
    std::uintptr_t original{};
    if (!BeginCallback(kItemSelected, api, lease, original)) return;
    // auto* const context = g_context.load(std::memory_order_acquire);
    // std::uint64_t song{};
    // if (context != nullptr && item != nullptr &&
    //     ReadValue(*context, reinterpret_cast<std::uintptr_t>(item) + profile::kVehicleItemSongIdOffset, song)) {
    //     Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
    //         "list entry selected: library " + std::string(IsLibrarySongId(*context, song) ? "yes" : "no"));
    // }
    CallItemSelected(original, self, item);
    EndCallback(api, lease);
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
    g_last_subsystem_vtable.store(0, std::memory_order_release);
    g_set_current_id.store(0, std::memory_order_release);
    g_album.store(nullptr, std::memory_order_release);
    g_album_cover.store(nullptr, std::memory_order_release);
    g_meting_cover.store(nullptr, std::memory_order_release);
    g_disc_view.store(0, std::memory_order_release);
    g_disc_view_class.store(0, std::memory_order_release);
    g_disc_album.store(0, std::memory_order_release);
    g_player_view.store(0, std::memory_order_release);
    g_player_view_class.store(0, std::memory_order_release);
    g_player_image_offset.store(0, std::memory_order_release);
    // The song cover texture stays rooted (like the album covers): the disc may
    // still show it after unload, and an unrooted texture there would dangle.
    g_import_buffer.store(0, std::memory_order_release);
    g_rendering_library_cdo.store(0, std::memory_order_release);
    g_song_cover.store(nullptr, std::memory_order_release);
    g_song_cover_key.store(0, std::memory_order_release);
    {
        std::scoped_lock lock(g_song_cover_mutex);
        g_song_cover_pending.clear();
        g_song_cover_pending_key = 0;
    }
    g_set_texture_parameter.store(0, std::memory_order_release);
    g_disc_parameter_name.store(0, std::memory_order_release);
    g_music_table.store(0, std::memory_order_release);
    g_album_table.store(0, std::memory_order_release);
    for (auto& slot : g_music_views) slot.store(0, std::memory_order_release);
    {
        std::scoped_lock lock(g_capture_mutex);
        g_capture = {};
    }
    {
        std::scoped_lock lock(g_name_mutex);
        g_name_cache = {};
        g_name_cache_next = 0;
    }
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
        {profile::kPositionFractionPattern, reinterpret_cast<void*>(&PositionFractionDetour),
            "hifi-vehicle-music-position-fraction", false},
        {profile::kPositionSecondsPattern, reinterpret_cast<void*>(&PositionSecondsDetour),
            "hifi-vehicle-music-position-seconds", false},
        {profile::kDurationCallbackPattern, reinterpret_cast<void*>(&DurationDetour),
            "hifi-vehicle-music-duration", false},
        {profile::kChangeSoundPattern, reinterpret_cast<void*>(&ChangeSoundDetour),
            "hifi-vehicle-music-change-sound", false},
        {profile::kEntryClickPattern, reinterpret_cast<void*>(&EntryClickDetour),
            "hifi-vehicle-music-entry-click", false},
        {profile::kDetailCoverPattern, reinterpret_cast<void*>(&DetailCoverDetour),
            "hifi-vehicle-music-detail-cover", false},
        {profile::kPageCoverPattern, reinterpret_cast<void*>(&PageCoverDetour),
            "hifi-vehicle-music-page-cover", false},
        {profile::kItemSelectedPattern, reinterpret_cast<void*>(&ItemSelectedDetour),
            "hifi-vehicle-music-item-selected", false},
        {profile::kRowClickPattern, reinterpret_cast<void*>(&RowClickDetour),
            "hifi-vehicle-music-row-click", false},
        {profile::kListClickPattern, reinterpret_cast<void*>(&ListClickDetour),
            "hifi-vehicle-music-list-click", false},
        {profile::kSetCurrentIdPattern, reinterpret_cast<void*>(&SetCurrentIdDetour),
            "hifi-vehicle-music-set-current-id", false},
        {profile::kDurationCallback2Pattern, reinterpret_cast<void*>(&Duration2Detour),
            "hifi-vehicle-music-duration-2", false},
    }};

    auto playback = anomaly::sdk::ResolveLayout(context.signature, profile::kPlaybackSchema);
    if (!playback) return {HookState::SignatureMissing, playback.error()};
    const auto& layout = playback.value();
    context.layout = {
        layout.offset("PlayingID"), layout.offset("Paused"), layout.offset("PlayerType")};

    auto album = anomaly::sdk::ResolveLayout(context.signature, profile::kAlbumSchema);
    std::string album_missing;
    if (album) {
        if (album.value().offset("PlayerType") != context.layout.player_type)
            album_missing = "PlayerType witnesses disagree with playback layout";
        else
            context.album_layout = AlbumLayout{
                album.value().offset("CurrentID"), album.value().offset("OwnedIDs")};
    } else {
        album_missing = album.error();
    }
    if (!album_missing.empty()) context.album_layout.reset();

    const auto resolve = [&context, &layout, &album](const std::string_view pattern) {
        for (const auto& witness : profile::kPlaybackWitnesses) {
            if (pattern == std::string_view(witness.pattern.data, witness.pattern.size))
                return layout.address({witness.id.data, witness.id.size});
        }
        for (const auto& witness : profile::kAlbumWitnesses) {
            if (pattern == std::string_view(witness.pattern.data, witness.pattern.size))
                return context.album_layout
                    ? album.value().address({witness.id.data, witness.id.size}) : std::uintptr_t{0};
        }

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
    for (std::size_t i = 0; i < kHookCount; ++i) {
        if (i >= kCoreHookCount && !context.album_layout) continue;
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

// The UI event trace that used to sit here resolved FName ids through the Host's
// names service from the Game thread. The Host waits inside that call while it
// publishes its semantic state, and a Game thread that waits there freezes the
// game, so a boot-time or map-switch event killed the process's input. The trace
// is removed until the Host stops blocking a Game thread caller; what it found is
// recorded in README.md.

// A successful unsubscribe drains a callback already in flight.
void UnsubscribeSync(Context& context) noexcept {
    if (context.ahud_subscription.id == 0 || context.ahud == nullptr) return;
    const AnomalyGenerationHandleV1 handle = context.ahud_subscription;
    context.ahud_subscription = {};
    static_cast<void>(context.ahud->unsubscribe(context.ahud->user, handle));
}

// Subscribing is idempotent, so the paths that need the per-frame sync call this
// every time: at boot the plugin starts before the game's reflection gate opens,
// so the Host publishes no AHUD service yet. It republishes services without
// notifying plugins, hence the lookup here. Calling into a service that is
// already available is safe from the Game thread; only touching one whose state
// is still initializing is not, and that is what the lookup rules out.
void EnsureSyncSubscription(Context& context) noexcept {
    if (context.ahud_subscription.id != 0) return;
    if (context.ahud == nullptr && context.host != nullptr) {
        context.ahud = anomaly::sdk::Host(context.host)
                           .Query<AnomalyUe5AhudServiceV1>(
                               ANOMALY_UE5_AHUD_SERVICE_V1_ID, ANOMALY_UE5_AHUD_SERVICE_V1_VERSION)
                           .get();
    }
    if (context.ahud == nullptr || !AhudReady(context.ahud) ||
        g_set_current_id.load(std::memory_order_acquire) == 0) {
        return;
    }
    AnomalyGenerationHandleV1 handle{};
    if (context.ahud->subscribe(context.ahud->user, SyncCurrentSong, &context, &handle).code !=
            ANOMALY_STATUS_V1_OK ||
        handle.id == 0) {
        return;
    }
    context.ahud_subscription = handle;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "current song sync subscribed");
}

// ---- Meting ---------------------------------------------------------------------
//
// Search and playlist metadata come from a MetingAPI endpoint; a song's audio is
// downloaded into memory only when it is played and handed to the engine, so
// nothing is written to disk. Every request runs on the Meting worker thread,
// never inside a callback lease or in Draw.

constexpr std::string_view kMetingEndpoint = "https://api.qijieya.cn/meting/";
constexpr std::size_t kMetingListBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMetingAudioBytes = 128U * 1024U * 1024U;
// Failed downloads skipped in a row before the worker stops skipping.
constexpr int kMetingMaxSkips = 3;
// A song cover is a small JPEG (tens of KB); anything far larger is refused.
constexpr std::size_t kMetingCoverBytes = 4U * 1024U * 1024U;

// Queues the Meting song `step` places after `from`; defined with the queue.
void StepMeting(Context& context, std::size_t from, int step) noexcept;

// The request in flight. Stop closes it so a blocked receive or read returns at
// once and the worker's join stays bounded.
std::mutex g_http_mutex;
HINTERNET g_http_request{};
std::atomic<bool> g_http_cancelled{};

std::string UrlEncode(const std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4U]);
            out.push_back(kHex[c & 15U]);
        }
    }
    return out;
}

// A GET following redirects (the Meting url type redirects to the CDN file).
std::optional<std::vector<std::uint8_t>> HttpGet(const std::string_view url,
    const std::size_t max_bytes, std::string& error) {
    const std::wstring wide = Utf8ToWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    std::array<wchar_t, 256> host{};
    std::array<wchar_t, 4096> path{};
    std::array<wchar_t, 4096> extra{};
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    parts.lpszExtraInfo = extra.data();
    parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (wide.empty() || !WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        error = "invalid URL";
        return std::nullopt;
    }
    const HINTERNET session = WinHttpOpen(L"Anomaly-HiFiVehicleMusic/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        error = "WinHTTP unavailable";
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);
    const HINTERNET connection = WinHttpConnect(session, host.data(), parts.nPort, 0);
    std::wstring target(path.data());
    target.append(extra.data());
    HINTERNET request = connection == nullptr ? nullptr :
        WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    {
        std::scoped_lock lock(g_http_mutex);
        if (g_http_cancelled.load(std::memory_order_acquire)) {
            if (request != nullptr) WinHttpCloseHandle(request);
            request = nullptr;
        }
        g_http_request = request;
    }
    std::vector<std::uint8_t> body;
    bool ok = false;
    if (request == nullptr) {
        error = "request could not be opened";
    } else if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = "network request failed";
    } else {
        DWORD status{};
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status < 200 || status >= 300) {
            error = "HTTP " + std::to_string(status);
        } else {
            ok = true;
            for (;;) {
                DWORD available{};
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    error = "read failed";
                    ok = false;
                    break;
                }
                if (available == 0) break;
                if (body.size() + available > max_bytes) {
                    error = "response too large";
                    ok = false;
                    break;
                }
                const std::size_t old = body.size();
                body.resize(old + available);
                DWORD read{};
                if (!WinHttpReadData(request, body.data() + old, available, &read)) {
                    error = "read failed";
                    ok = false;
                    break;
                }
                body.resize(old + read);
            }
        }
    }
    {
        std::scoped_lock lock(g_http_mutex);
        // Stop may have closed it already; close only a handle still ours.
        if (g_http_request == request && request != nullptr) WinHttpCloseHandle(request);
        g_http_request = nullptr;
    }
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok) return std::nullopt;
    return body;
}

// Closes the request in flight; the blocked WinHTTP call returns an error.
void CancelMetingHttp() noexcept {
    g_http_cancelled.store(true, std::memory_order_release);
    std::scoped_lock lock(g_http_mutex);
    if (g_http_request != nullptr) {
        WinHttpCloseHandle(g_http_request);
        g_http_request = nullptr;
    }
}

std::vector<MetingSong> ParseMetingSongs(const std::vector<std::uint8_t>& body, std::string& error) {
    std::vector<MetingSong> songs;
    const auto root = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
    if (!root.is_array()) {
        error = "unexpected response";
        return songs;
    }
    for (const auto& item : root) {
        if (!item.is_object()) continue;
        const auto text = [&item](const char* key) {
            const auto found = item.find(key);
            return found != item.end() && found->is_string() ? found->get<std::string>() : std::string{};
        };
        MetingSong song{text("name"), text("artist"), text("url"), text("pic")};
        if (!song.name.empty() && !song.url.empty()) songs.push_back(std::move(song));
        if (songs.size() >= profile::kMaxSongs) break;
    }
    return songs;
}

// Worker thread, after a song started: downloads its cover into memory and
// hands it to the Game-thread tick, which imports it. Nothing touches disk; a
// failure only keeps the album cover on the disc. Fetched even before the
// import function is resolved: the first song of a session is picked about the
// moment the music UI opens, and the resolve runs only after that, so gating
// on it here left the first song without its cover. The pending image waits
// for the tick, which imports it once the function is there.
void FetchSongCover(Context& context, const std::size_t index, const std::uint64_t generation) {
    std::string pic;
    {
        std::scoped_lock lock(context.meting_mutex);
        if (index < context.meting_songs.size()) pic = context.meting_songs[index].pic;
    }
    if (pic.empty()) return;
    std::string error;
    auto bytes = HttpGet(pic, kMetingCoverBytes, error);
    if (context.meting_stopping.load(std::memory_order_acquire)) return;
    if (!bytes || bytes->empty()) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "Meting song cover download failed: " + error);
        return;
    }
    std::scoped_lock lock(g_song_cover_mutex);
    g_song_cover_pending = std::move(*bytes);
    g_song_cover_pending_key = SongCoverKey(index, generation);
}

void SetMetingStatus(Context& context, std::string status) {
    std::scoped_lock lock(context.meting_mutex);
    context.meting_status = std::move(status);
}

// Applies the result on the worker itself: Draw runs only while the panel is
// open, so a song picked in the game's album with the panel closed must start
// without it. The engine queues the command; the builder rebuilds the rows.
void RunMetingRequest(Context& context, const MetingRequest& request) {
    std::string error;
    const auto body = HttpGet(request.url,
        request.download ? kMetingAudioBytes : kMetingListBytes, error);
    if (context.meting_stopping.load(std::memory_order_acquire)) return;
    if (request.download) {
        {
            std::scoped_lock lock(context.meting_mutex);
            if (context.meting_download_pending && context.meting_download_index == request.index &&
                context.meting_download_generation == request.generation) {
                context.meting_download_pending = false;
            }
        }
        if (request.generation != context.meting_generation.load(std::memory_order_acquire)) return;
        std::string title;
        {
            std::scoped_lock lock(context.meting_mutex);
            if (request.index < context.meting_songs.size()) title = context.meting_songs[request.index].name;
        }
        if (!body || body->empty()) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "Meting download failed: " + title + ": " + error);
            SetMetingStatus(context, "download failed: " + error);
            // Skip to the next song, as a song end does: a song that cannot be
            // fetched (VIP-only, removed) would otherwise leave the car silent.
            // Bounded so a dead network does not walk the whole list.
            const int failures = context.meting_failures.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (failures < kMetingMaxSkips) {
                StepMeting(context, request.index, 1);
            } else {
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    "Meting: " + std::to_string(failures) + " downloads failed in a row; stopped skipping");
            }
            return;
        }
        context.meting_failures.store(0, std::memory_order_release);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "Meting song " + std::to_string(request.index) + " downloaded (" +
                std::to_string(body->size() / 1024U) + " KB)");
        context.engine.PlayMemory(title, std::move(*body), 0.0F, request.index);
        SetMetingStatus(context, {});
        FetchSongCover(context, request.index, request.generation);
        return;
    }
    context.meting_busy.store(false, std::memory_order_release);
    if (!body) {
        SetMetingStatus(context, error);
        return;
    }
    std::vector<MetingSong> songs = ParseMetingSongs(*body, error);
    if (!error.empty()) {
        SetMetingStatus(context, error);
        return;
    }
    const std::size_t count = songs.size();
    {
        std::scoped_lock lock(context.meting_mutex);
        context.meting_songs = std::move(songs);
        // The rows rebuild for the new list; a pending download is stale.
        context.meting_generation.fetch_add(1, std::memory_order_acq_rel);
        context.meting_status = std::to_string(count);
    }
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "Meting list loaded: " + std::to_string(count) + " songs");
    context.builder_wake.notify_all();
    // Remember the request so the next load rebuilds the same album. Only a
    // non-empty list is remembered; a changed one is saved through the
    // scheduler, like every other setting.
    if (count > 0 && !request.kind.empty()) {
        std::scoped_lock lock(context.mutex);
        if (context.settings.meting_kind != request.kind || context.settings.meting_query != request.query) {
            context.settings.meting_kind = request.kind;
            context.settings.meting_query = request.query;
            context.settings_dirty = true;
            context.meting_settings_changed.store(true, std::memory_order_release);
        }
    }
}

// A list request for `kind` ("search"/"playlist") and its keyword or id.
MetingRequest MetingListRequest(const std::string& kind, const std::string& query) {
    MetingRequest request;
    request.kind = kind;
    request.query = query;
    request.url = std::string(kMetingEndpoint) + "?server=netease&type=" + kind + "&id=" + UrlEncode(query) +
        (kind == "search" ? "&limit=30" : "");
    return request;
}

void MetingLoop(Context* context) {
    while (!context->meting_stopping.load(std::memory_order_acquire)) {
        MetingRequest request;
        {
            std::unique_lock lock(context->meting_mutex);
            context->meting_wake.wait(lock, [context] {
                return context->meting_stopping.load(std::memory_order_acquire) ||
                    !context->meting_requests.empty();
            });
            if (context->meting_stopping.load(std::memory_order_acquire)) return;
            // A newer download supersedes an older one still queued.
            request = std::move(context->meting_requests.front());
            context->meting_requests.pop_front();
        }
        try {
            RunMetingRequest(*context, request);
        } catch (...) {
        }
    }
}

bool QueueMetingRequest(Context& context, MetingRequest request) noexcept {
    try {
        {
            std::scoped_lock lock(context.meting_mutex);
            if (request.download) {
                // Only the latest pick matters; drop downloads still waiting.
                std::erase_if(context.meting_requests, [](const MetingRequest& queued) {
                    return queued.download;
                });
            }
            context.meting_requests.push_back(std::move(request));
        }
        context.meting_wake.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

bool QueueMetingDownload(Context& context, const std::size_t index) noexcept {
    try {
        MetingRequest request;
        request.download = true;
        request.index = index;
        {
            std::scoped_lock lock(context.meting_mutex);
            if (index >= context.meting_songs.size()) return false;
            const std::uint64_t generation = context.meting_generation.load(std::memory_order_acquire);
            if (context.meting_download_pending && context.meting_download_index == index &&
                context.meting_download_generation == generation) {
                return false;  // the same pick is already downloading
            }
            context.meting_download_pending = true;
            context.meting_download_index = index;
            context.meting_download_generation = generation;
            request.url = context.meting_songs[index].url;
            request.generation = generation;
        }
        return QueueMetingRequest(context, std::move(request));
    } catch (...) {
        return false;
    }
}

// Queues the Meting song `step` places after `from` (wrapping around).
void StepMeting(Context& context, const std::size_t from, const int step) noexcept {
    std::size_t count{};
    {
        std::scoped_lock lock(context.meting_mutex);
        count = context.meting_songs.size();
    }
    if (count == 0) return;
    // step is -1 or 1; adding count keeps the left operand non-negative.
    const auto signed_next = static_cast<std::ptrdiff_t>(from % count) + step + static_cast<std::ptrdiff_t>(count);
    const std::size_t next = static_cast<std::size_t>(signed_next) % count;
    static_cast<void>(QueueMetingDownload(context, next));
}

void StartMetingWorker(Context& context) {
    g_http_cancelled.store(false, std::memory_order_release);
    context.meting_stopping.store(false, std::memory_order_release);
    context.meting_worker = std::thread(MetingLoop, &context);
}

// Bounded: closing the request in flight makes the worker's WinHTTP call return.
void StopMetingWorker(Context& context) noexcept {
    try {
        {
            std::scoped_lock lock(context.meting_mutex);
            context.meting_stopping.store(true, std::memory_order_release);
        }
        context.meting_wake.notify_all();
        CancelMetingHttp();
        if (context.meting_worker.joinable()) context.meting_worker.join();
    } catch (...) {
    }
}

// ---- spinning disc cover ---------------------------------------------------------
//
// The album thumbnail's disc label is a MaterialInstanceDynamic whose texture
// parameter holds the album cover, so a plugin album's cover is written into
// that parameter through MaterialInstanceDynamic::SetTextureParameterValue.

// Builder thread, once the disc material is known: MaterialInstanceDynamic::
// SetTextureParameterValue, found as the object named so whose outer is the
// material's own class (a UFunction's outer is the class declaring it). One
// pass over the object array, reading two fields per object and resolving the
// name only of that class's members. Stays 0 when not found, which keeps the
// game's cover on the disc.
void ResolveDiscFunction(Context& context, const std::uintptr_t material) noexcept {
    if (g_set_texture_parameter.load(std::memory_order_acquire) != 0) return;
    static bool tried = false;
    if (tried) return;
    tried = true;
    try {
        const std::uintptr_t objects = g_object_array.load(std::memory_order_acquire);
        std::uintptr_t klass{};
        std::int32_t total{};
        if (objects == 0 || !ReadValue(context, material + profile::kObjectClassOffset, klass) || klass == 0 ||
            !ReadValue(context, objects + profile::kObjectArrayCountOffset, total) || total <= 0) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "disc cover function lookup unavailable");
            return;
        }
        for (std::int32_t index = 0; index < total; ++index) {
            if (context.builder_stopping.load(std::memory_order_acquire)) return;
            auto* const item = static_cast<std::byte*>(ObjectItem(context, index));
            std::uintptr_t object{};
            std::uintptr_t outer{};
            if (item == nullptr || !ReadValue(context, reinterpret_cast<std::uintptr_t>(item), object) ||
                object == 0 || !ReadValue(context, object + profile::kObjectOuterOffset, outer) || outer != klass) {
                continue;
            }
            std::uint32_t name_id{};
            if (!ReadValue(context, object + profile::kObjectNameOffset, name_id) || name_id == 0) continue;
            std::array<char, 64> buffer{};
            std::size_t size = buffer.size();
            if (context.names->resolve_utf8(context.names->user, name_id, buffer.data(), &size).code !=
                    ANOMALY_STATUS_V1_OK || size > buffer.size()) {
                continue;
            }
            std::string text(buffer.data(), size);
            while (!text.empty() && text.back() == '\0') text.pop_back();
            if (text == profile::kSetTextureParameterFunction) {
                g_set_texture_parameter.store(object, std::memory_order_release);
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "disc cover function resolved");
                return;
            }
        }
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "disc cover function not found");
    } catch (...) {
    }
}

std::string ObjectName(Context& context, std::uintptr_t object);

// Builder thread, once: UKismetRenderingLibrary::ImportBufferAsTexture2D and
// the library's class default object to call it on. One pass over the object
// array: the class is the object named KismetRenderingLibrary whose own class
// is "Class"; the function is its member of that name; the default object is
// the instance of that class whose name starts with "Default__". Stays 0 when
// any is missing, which keeps the album cover on the disc.
void ResolveImportBuffer(Context& context) noexcept {
    static bool tried = false;
    if (tried || g_import_buffer.load(std::memory_order_acquire) != 0) return;
    const std::uintptr_t objects = g_object_array.load(std::memory_order_acquire);
    std::int32_t total{};
    if (objects == 0 || !ReadValue(context, objects + profile::kObjectArrayCountOffset, total) || total <= 0) return;
    tried = true;
    try {
        std::uint32_t class_name_id{};
        std::uint32_t function_name_id{};
        std::uintptr_t library{};
        // First the class, by name: the names are resolved only for objects
        // whose own class is named "Class" (a few thousand), not for all.
        std::uintptr_t class_class{};
        for (std::int32_t index = 0; index < total && library == 0; ++index) {
            if (context.builder_stopping.load(std::memory_order_acquire)) return;
            auto* const item = static_cast<std::byte*>(ObjectItem(context, index));
            std::uintptr_t object{};
            std::uintptr_t klass{};
            if (item == nullptr || !ReadValue(context, reinterpret_cast<std::uintptr_t>(item), object) ||
                object == 0 || !ReadValue(context, object + profile::kObjectClassOffset, klass) || klass == 0) {
                continue;
            }
            if (class_class == 0) {
                if (ObjectName(context, klass) != "Class") continue;
                class_class = klass;
            } else if (klass != class_class) {
                continue;
            }
            std::uint32_t name{};
            if (!ReadValue(context, object + profile::kObjectNameOffset, name)) continue;
            if (class_name_id != 0) {
                if (name == class_name_id) library = object;
                continue;
            }
            if (ObjectName(context, object) == profile::kRenderingLibraryClass) {
                class_name_id = name;
                library = object;
            }
        }
        if (library == 0) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "song cover import unavailable: no rendering library");
            return;
        }
        std::uintptr_t function{};
        std::uintptr_t default_object{};
        for (std::int32_t index = 0; index < total && (function == 0 || default_object == 0); ++index) {
            if (context.builder_stopping.load(std::memory_order_acquire)) return;
            auto* const item = static_cast<std::byte*>(ObjectItem(context, index));
            std::uintptr_t object{};
            std::uintptr_t outer{};
            std::uintptr_t klass{};
            if (item == nullptr || !ReadValue(context, reinterpret_cast<std::uintptr_t>(item), object) ||
                object == 0) {
                continue;
            }
            if (default_object == 0 && ReadValue(context, object + profile::kObjectClassOffset, klass) &&
                klass == library && ObjectName(context, object).rfind("Default__", 0) == 0) {
                default_object = object;
                continue;
            }
            if (function != 0 || !ReadValue(context, object + profile::kObjectOuterOffset, outer) || outer != library) {
                continue;
            }
            std::uint32_t name{};
            if (!ReadValue(context, object + profile::kObjectNameOffset, name)) continue;
            if (function_name_id != 0 ? name == function_name_id
                                      : ObjectName(context, object) == profile::kImportBufferFunction) {
                function_name_id = name;
                function = object;
            }
        }
        if (function == 0 || default_object == 0) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                std::string("song cover import unavailable: ") +
                    (function == 0 ? "no ImportBufferAsTexture2D" : "no library default object"));
            return;
        }
        // ProcessEvent lays the parameters out by the function's own property
        // offsets, so they must be the layout ImportBufferParameters assumes:
        // 3 parameters, a 0x20 block, the return value at 0x18. UFunction keeps
        // them after FunctionFlags as NumParms (u8), a pad byte, ParmsSize (u16)
        // and ReturnValueOffset (u16) -- read in game at +0xB4/+0xB6/+0xB8 as
        // 03 00 20 00 18 00. Matched as that run, not at a fixed offset.
        bool layout_ok = false;
        for (std::uint32_t offset = 0xA0; offset < 0x100 && !layout_ok; ++offset) {
            std::uint8_t count{};
            std::uint16_t size{};
            std::uint16_t return_offset{};
            if (ReadValue(context, function + offset, count) && count == 3 &&
                ReadValue(context, function + offset + 2, size) && size == sizeof(ImportBufferParameters) &&
                ReadValue(context, function + offset + 4, return_offset) &&
                return_offset == offsetof(ImportBufferParameters, result)) {
                layout_ok = true;
            }
        }
        if (!layout_ok) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "song cover import unavailable: ImportBufferAsTexture2D parameters do not match");
            return;
        }
        g_rendering_library_cdo.store(default_object, std::memory_order_release);
        g_import_buffer.store(function, std::memory_order_release);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "song cover import ready");
    } catch (...) {
    }
}

// Builder thread, once a thumbnail was seen: the disc parameter's FName, read
// from the material's own parameter list and matched by name. The names
// service's reverse lookup only searches recently registered names, and this
// engine-content name is old, so it is resolved forward from the material.
// The UTF-8 name of a UObject, or empty. Builder thread only: name resolution
// may block and must not run on the Game thread.
std::string ObjectName(Context& context, const std::uintptr_t object) {
    std::uint32_t name_id{};
    if (object == 0 || !ReadValue(context, object + profile::kObjectNameOffset, name_id) || name_id == 0) return {};
    std::array<char, 128> buffer{};
    std::size_t size = buffer.size();
    if (context.names->resolve_utf8(context.names->user, name_id, buffer.data(), &size).code !=
            ANOMALY_STATUS_V1_OK || size > buffer.size()) {
        return {};
    }
    std::string text(buffer.data(), size);
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

// Builder thread, once a thumbnail was seen: the disc parameter's FName, read
// from the material's own parameter list and matched by name (the names
// service's reverse lookup only searches recently registered names), and the
// UFunction that sets it.
void ResolveDiscParameter(Context& context) noexcept {
    if (g_disc_parameter_name.load(std::memory_order_acquire) != 0 &&
        g_set_texture_parameter.load(std::memory_order_acquire) != 0) {
        return;
    }
    const std::uintptr_t view = g_disc_view.load(std::memory_order_acquire);
    if (view == 0) return;
    try {
        std::uintptr_t image{};
        std::uintptr_t material{};
        std::uintptr_t entries{};
        std::int32_t count{};
        if (!ReadValue(context, view + profile::kDetailCoverImageOffset, image) || image == 0 ||
            !ReadValue(context, image + profile::kImageBrushResourceOffset, material) || material == 0) {
            return;
        }
        ResolveDiscFunction(context, material);
        if (g_disc_parameter_name.load(std::memory_order_acquire) != 0) return;
        if (!ReadValue(context, material + profile::kMaterialTextureParametersOffset, entries) ||
            !ReadValue(context, material + profile::kMaterialTextureParametersOffset + 8, count) ||
            entries == 0 || count <= 0 || count > 64) {
            return;
        }
        for (std::int32_t i = 0; i < count; ++i) {
            std::uint64_t name{};
            if (!ReadValue(context, entries + static_cast<std::uintptr_t>(i) * profile::kTextureParameterSize, name)) {
                return;
            }
            std::array<char, 64> buffer{};
            std::size_t size = buffer.size();
            if (context.names->resolve_utf8(context.names->user, static_cast<std::uint32_t>(name), buffer.data(),
                    &size).code != ANOMALY_STATUS_V1_OK || size > buffer.size()) {
                continue;
            }
            std::string text(buffer.data(), size);
            while (!text.empty() && text.back() == '\0') text.pop_back();
            if (text == profile::kDiscTextureParameter) {
                g_disc_parameter_name.store(name, std::memory_order_release);
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "disc cover ready");
                return;
            }
        }
    } catch (...) {
    }
}

// Builder thread, every few seconds until found: the player's spinning disc.
// It is the AlbumCoverImg image of the music detailed view (the player panel),
// drawn through its own MaterialInstanceDynamic with the same Tex1 parameter.
// Found by walking the object array for that material class, then up its
// outer chain (image < WidgetTree < view); the view and the image's field
// offset in it are remembered so the Game-thread tick only reads pointers.
//
// The search is a full pass over the object array, so it runs only when the
// music UI has just shown an album (SetAlbumCover bumps g_disc_cover_event),
// which is when the player panel exists, and retries with a growing delay
// (1, 2, 4 ... 32 builder cycles of 250 ms) while the panel is not there yet,
// giving up until the next album shows after 6 tries. A known view costs one
// read per cycle.
void FindPlayerDisc(Context& context) noexcept {
    static std::uint64_t seen_event{};
    static int attempts{};
    static int wait{};
    const std::uintptr_t function = g_set_texture_parameter.load(std::memory_order_acquire);
    if (function == 0) return;
    const std::uintptr_t known = g_player_view.load(std::memory_order_acquire);
    if (known != 0) {
        std::uintptr_t klass{};
        if (ReadValue(context, known + profile::kObjectClassOffset, klass) &&
            klass == g_player_view_class.load(std::memory_order_acquire)) {
            return;  // still alive
        }
        g_player_view.store(0, std::memory_order_release);
    }
    const std::uint64_t event = g_disc_cover_event.load(std::memory_order_acquire);
    if (event != seen_event) {  // an album was just shown: search afresh
        seen_event = event;
        attempts = 0;
        wait = 0;
    }
    if (attempts >= 6) return;
    if (wait > 0) {
        --wait;
        return;
    }
    wait = (1 << attempts) - 1;  // the next try comes 1, 2, 4 ... cycles later
    ++attempts;
    try {
        // A UFunction's outer is the class declaring it: MaterialInstanceDynamic.
        std::uintptr_t mid_class{};
        const std::uintptr_t objects = g_object_array.load(std::memory_order_acquire);
        std::int32_t total{};
        if (!ReadValue(context, function + profile::kObjectOuterOffset, mid_class) || mid_class == 0 ||
            objects == 0 || !ReadValue(context, objects + profile::kObjectArrayCountOffset, total)) {
            return;
        }
        for (std::int32_t index = 0; index < total; ++index) {
            if (context.builder_stopping.load(std::memory_order_acquire)) return;
            auto* const item = static_cast<std::byte*>(ObjectItem(context, index));
            std::uintptr_t material{};
            std::uintptr_t klass{};
            if (item == nullptr || !ReadValue(context, reinterpret_cast<std::uintptr_t>(item), material) ||
                material == 0 || !ReadValue(context, material + profile::kObjectClassOffset, klass) ||
                klass != mid_class) {
                continue;
            }
            std::uintptr_t image{};
            std::uintptr_t tree{};
            std::uintptr_t view{};
            std::uintptr_t view_class{};
            if (!ReadValue(context, material + profile::kObjectOuterOffset, image) || image == 0 ||
                !ReadValue(context, image + profile::kObjectOuterOffset, tree) || tree == 0 ||
                !ReadValue(context, tree + profile::kObjectOuterOffset, view) || view == 0 ||
                !ReadValue(context, view + profile::kObjectClassOffset, view_class) || view_class == 0 ||
                ObjectName(context, image) != profile::kPlayerDiscImageName ||
                ObjectName(context, view_class) != profile::kPlayerViewClassName) {
                continue;
            }
            // The image's field in the view; the image is the one the brush is on.
            for (std::uint32_t offset = 0x28; offset < 0x2000; offset += 8) {
                std::uintptr_t field{};
                if (!ReadValue(context, view + offset, field) || field != image) continue;
                g_player_image_offset.store(offset, std::memory_order_release);
                g_player_view_class.store(view_class, std::memory_order_release);
                g_player_view.store(view, std::memory_order_release);
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "player disc found");
                return;
            }
        }
    } catch (...) {
    }
}

using ProcessEventFn = void(__fastcall*)(void*, void*, void*);

bool CallProcessEvent(void* object, const std::uintptr_t function, void* parameters) noexcept {
    __try {
        const auto vtable = *static_cast<std::uintptr_t* const*>(object);
        reinterpret_cast<ProcessEventFn>(vtable[profile::kProcessEventVtableIndex])(
            object, reinterpret_cast<void*>(function), parameters);
        return true;
    } __except (1) {
        return false;
    }
}

// Puts `texture` into the Tex1 parameter of the material `image` draws with,
// unless it already shows it. Bounded: a few guarded reads, and a ProcessEvent
// call only when the blueprint put another texture back.
void SetDiscTexture(Context& context, const std::uintptr_t image, void* const texture) noexcept {
    const std::uintptr_t function = g_set_texture_parameter.load(std::memory_order_acquire);
    const std::uint64_t parameter = g_disc_parameter_name.load(std::memory_order_acquire);
    std::uintptr_t material{};
    std::uintptr_t entries{};
    std::int32_t count{};
    if (function == 0 || parameter == 0 || image == 0 || texture == nullptr ||
        !ReadValue(context, image + profile::kImageBrushResourceOffset, material) || material == 0 ||
        !ReadValue(context, material + profile::kMaterialTextureParametersOffset, entries) ||
        !ReadValue(context, material + profile::kMaterialTextureParametersOffset + 8, count) ||
        entries == 0 || count <= 0 || count > 64) {
        return;  // not the expected material instance
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t entry = entries + static_cast<std::uintptr_t>(i) * profile::kTextureParameterSize;
        std::uint64_t name{};
        std::uintptr_t value{};
        if (!ReadValue(context, entry, name) ||
            !ReadValue(context, entry + profile::kTextureParameterValueOffset, value)) {
            return;
        }
        if (name != parameter) continue;
        if (value == reinterpret_cast<std::uintptr_t>(texture)) return;
        struct {
            std::uint64_t name;
            void* value;
        } parameters{parameter, texture};
        static_cast<void>(CallProcessEvent(reinterpret_cast<void*>(material), function, &parameters));
        return;
    }
}

// Sets or clears RootSet on a UObject's item; the import roots its texture so
// GC keeps it while the disc shows it, and unrooting lets GC take it back.
void SetRooted(Context& context, void* const object, const bool rooted) noexcept {
    std::int32_t index{};
    if (object == nullptr ||
        !ReadValue(context, reinterpret_cast<std::uintptr_t>(object) + profile::kObjectIndexOffset, index)) {
        return;
    }
    auto* const item = static_cast<std::byte*>(ObjectItem(context, index));
    if (item == nullptr) return;
    if (rooted) {
        static_cast<void>(CallSetItemFlags(item, static_cast<std::int32_t>(profile::kRootSetFlag)));
        return;
    }
    __try {
        auto* const flags = reinterpret_cast<volatile LONG*>(item + profile::kObjectItemFlagsOffset);
        InterlockedAnd(flags, ~static_cast<LONG>(profile::kRootSetFlag));
    } __except (1) {
    }
}

void* CallImportBuffer(const std::uintptr_t function, const std::uintptr_t library,
    const std::vector<std::uint8_t>& bytes) noexcept {
    ImportBufferParameters parameters{};
    parameters.buffer.data = bytes.data();
    parameters.buffer.count = static_cast<std::int32_t>(bytes.size());
    parameters.buffer.max = static_cast<std::int32_t>(bytes.size());
    if (!CallProcessEvent(reinterpret_cast<void*>(library), function, &parameters)) return nullptr;
    return parameters.result;
}

// Game thread, per frame: imports a downloaded song cover. At most one import
// per frame, and only when the worker handed a new image over. The previous
// song's cover is unrooted, so only the current one stays alive.
void ImportPendingSongCover(Context& context) noexcept {
    const std::uintptr_t function = g_import_buffer.load(std::memory_order_acquire);
    const std::uintptr_t library = g_rendering_library_cdo.load(std::memory_order_acquire);
    if (function == 0 || library == 0) return;
    std::vector<std::uint8_t> bytes;
    std::uint64_t key{};
    {
        std::unique_lock lock(g_song_cover_mutex, std::try_to_lock);
        if (!lock.owns_lock() || g_song_cover_pending.empty()) return;
        bytes.swap(g_song_cover_pending);
        key = g_song_cover_pending_key;
    }
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) return;
    void* const texture = CallImportBuffer(function, library, bytes);
    if (texture == nullptr) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "song cover could not be imported");
        return;
    }
    SetRooted(context, texture, true);
    void* const previous = g_song_cover.exchange(texture, std::memory_order_acq_rel);
    g_song_cover_key.store(key, std::memory_order_release);
    if (previous != nullptr && previous != texture) SetRooted(context, previous, false);
}

// The cover of the plugin song now playing, or null while a game song plays:
// a Meting song's own cover once it is imported (its album's until then), or
// the local album's.
void* PlayingCoverTexture(Context& context) noexcept {
    void* const subsystem = CachedSubsystem();
    if (subsystem == nullptr || !TakenOver(context, subsystem)) return nullptr;
    if (const auto memory = context.engine.MemorySlot()) {
        void* const song = g_song_cover.load(std::memory_order_acquire);
        const std::uint64_t key =
            SongCoverKey(*memory, context.meting_generation.load(std::memory_order_acquire));
        if (song != nullptr && g_song_cover_key.load(std::memory_order_acquire) == key) return song;
        return g_meting_cover.load(std::memory_order_acquire);
    }
    if (context.engine.PlayingSlot()) return g_album_cover.load(std::memory_order_acquire);
    return nullptr;
}

// Game thread, per frame: keeps both discs on the plugin album's cover -- the
// album thumbnail for the album it shows, and the player's disc for the album
// now playing. A game album or game song keeps the game's own cover.
void ApplyDiscCover(Context& context) noexcept {
    ImportPendingSongCover(context);
    if (g_set_texture_parameter.load(std::memory_order_acquire) == 0 ||
        g_disc_parameter_name.load(std::memory_order_acquire) == 0) {
        return;
    }
    const std::uintptr_t view = g_disc_view.load(std::memory_order_acquire);
    std::uintptr_t klass{};
    std::uintptr_t image{};
    const auto snapshot = g_album.load(std::memory_order_acquire);
    if (view != 0 && snapshot != nullptr &&
        ReadValue(context, view + profile::kObjectClassOffset, klass) &&
        klass == g_disc_view_class.load(std::memory_order_acquire) &&
        ReadValue(context, view + profile::kDetailCoverImageOffset, image)) {
        SetDiscTexture(context, image, CoverTexture(*snapshot, g_disc_album.load(std::memory_order_acquire)));
    }
    const std::uintptr_t player = g_player_view.load(std::memory_order_acquire);
    if (player != 0 &&
        ReadValue(context, player + profile::kObjectClassOffset, klass) &&
        klass == g_player_view_class.load(std::memory_order_acquire) &&
        ReadValue(context, player + g_player_image_offset.load(std::memory_order_acquire), image)) {
        SetDiscTexture(context, image, PlayingCoverTexture(context));
    }
}
// Background worker: retries the AHUD subscription that a boot-time load
// misses (TakeOverPost used to retry it, but that query ran inside a callback
// lease) and builds and publishes the album snapshot from what the table
// detours captured.
void BuilderLoop(Context* context) {
    while (!context->builder_stopping.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(context->builder_wake_mutex);
        context->builder_wake.wait_for(lock, std::chrono::milliseconds(250), [context] {
            return context->builder_stopping.load(std::memory_order_acquire);
        });
        if (context->builder_stopping.load(std::memory_order_acquire)) return;
        lock.unlock();
        try {
            EnsureSyncSubscription(*context);
            // A Meting song ended: continue with the next one in the Meting
            // list. The per-frame sync then points the game's current song at it.
            if (const auto ended = context->engine.TakeMemoryEnded()) StepMeting(*context, *ended, 1);
            ResolveDiscParameter(*context);
            FindPlayerDisc(*context);
            // Only once the disc can show a texture at all.
            if (g_set_texture_parameter.load(std::memory_order_acquire) != 0) ResolveImportBuffer(*context);
            // Skip the cycle while a game-thread detour is publishing under
            // g_build_mutex; the next cycle picks up the remaining work.
            std::unique_lock<std::mutex> build_lock(g_build_mutex, std::try_to_lock);
            if (build_lock.owns_lock()) BuildAlbumSnapshot(*context);
        } catch (...) {
        }
    }
}

// Joins the builder thread. Bounded: BuildAlbumSnapshot checks the stop flag
// between its allocations, so the join waits at most one row build.
void StopBuilder(Context& context) noexcept {
    try {
        context.builder_stopping.store(true, std::memory_order_release);
        context.builder_wake.notify_all();
        if (context.builder.joinable()) context.builder.join();
    } catch (...) {
    }
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
        context->host = host;
        context->core = host_view.Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
        context->config = host_view.Query<AnomalyConfigServiceV1>(
            ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION).get();
        context->signature = host_view.Query<AnomalySignatureServiceV2>(
            ANOMALY_SIGNATURE_SERVICE_V2_ID, ANOMALY_SIGNATURE_SERVICE_V2_VERSION).get();
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
        // Diagnostic probe (off): where this build's ChangePlayerMusicSound lives,
        // to calibrate the caller offsets the LogCaller probe logged.
        // {
        //     const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"HTGame.exe"));
        //     const std::uintptr_t target = context->hooks[kChangeSound].id != 0
        //         ? g_slots[kChangeSound].original.load(std::memory_order_acquire)
        //         : 0;
        //     char text[96]{};
        //     std::snprintf(text, sizeof(text), "module base 0x%llx, change-sound trampoline 0x%llx",
        //         static_cast<unsigned long long>(base), static_cast<unsigned long long>(target));
        //     Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, text);
        // }
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
        if (state == HookState::Installed) {
            EnsureSyncSubscription(*context);
            // The worker keeps the callback leases free of host queries and
            // album work; without it the takeover still works, only the sync
            // retry and the album build are lost.
            try {
                context->builder = std::thread(BuilderLoop, context);
            } catch (...) {
                Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    "background worker could not start; song sync may lag");
            }
        }
        try {
            StartMetingWorker(*context);
            // Rebuild the Meting album from the last list request that
            // succeeded; the worker fetches it like a fresh search.
            MetingRequest replay;
            {
                std::scoped_lock lock(context->mutex);
                if (!context->settings.meting_kind.empty()) {
                    replay = MetingListRequest(context->settings.meting_kind, context->settings.meting_query);
                }
            }
            if (!replay.url.empty()) {
                context->meting_busy.store(true, std::memory_order_release);
                if (!QueueMetingRequest(*context, std::move(replay))) {
                    context->meting_busy.store(false, std::memory_order_release);
                }
            }
        } catch (...) {
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "Meting worker could not start; search is unavailable");
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
        // Before the hooks and the engine: the builder reads their globals and
        // drives the engine, so it must be gone first.
        StopBuilder(*context);
        StopMetingWorker(*context);
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
        StopBuilder(*context);
        StopMetingWorker(*context);
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
        const std::string album = WideToUtf8(context.engine.LibraryName());
        const std::array<std::string_view, 2> args{count, album};
        Text(ui, l10n.Format("library.count", "Library: {0} tracks (in-game album \"{1}\")", args));
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

// The playlist id in what the user typed: the id itself, or a share link such
// as music.163.com/playlist?id=7307314698&uct2=..., music.163.com/#/playlist?id=...
// or y.music.163.com/m/playlist/7307314698. The digits after "id=" win; else
// the digits after "playlist/". Empty when neither is there.
std::string PlaylistId(const std::string_view input) {
    std::size_t first = 0;
    std::size_t last = input.size();
    while (first < last && std::isspace(static_cast<unsigned char>(input[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) --last;
    const std::string_view text = input.substr(first, last - first);
    const auto digits_at = [&text](std::size_t at) {
        std::string digits;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') digits.push_back(text[at++]);
        return digits;
    };
    if (!text.empty() && std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
        return std::string(text);
    }
    // "id=" as a whole parameter name, not the tail of e.g. "userid=".
    for (std::size_t at = text.find("id="); at != std::string_view::npos; at = text.find("id=", at + 1)) {
        if (at == 0 || text[at - 1] == '?' || text[at - 1] == '&') {
            std::string digits = digits_at(at + 3);
            if (!digits.empty()) return digits;
        }
    }
    const std::size_t path = text.find("playlist/");
    return path == std::string_view::npos ? std::string{} : digits_at(path + 9);
}

// The Meting section: search, playlist import and the current song list. A song
// plays when picked in the game's "Meting" album or with its button here.
void DrawMeting(Context& context, const AnomalyUiServiceV1* ui) {
    const anomaly::plugins::Localizer& l10n = context.localizer;
    Text(ui, l10n.Text("meting.title", "Meting library (memory playback, no audio cache)"));
    const bool busy = context.meting_busy.load(std::memory_order_acquire);
    ui->input_text(ui->user, anomaly::sdk::StringView(l10n.Label("meting.search", "Search songs", "meting-query")),
        context.meting_query.data(), context.meting_query.size(), 0);
    ui->same_line(ui->user, 0.0F, -1.0F);
    if (Button(ui, l10n.Label("meting.search_button", "Search", "meting-search"), !busy)) {
        const std::string query(context.meting_query.data());
        if (!query.empty()) {
            MetingRequest request = MetingListRequest("search", query);
            context.meting_busy.store(true, std::memory_order_release);
            SetMetingStatus(context, {});
            if (!QueueMetingRequest(context, std::move(request))) {
                context.meting_busy.store(false, std::memory_order_release);
            }
        }
    }
    ui->input_text(ui->user,
        anomaly::sdk::StringView(
            l10n.Label("meting.playlist", "NetEase playlist ID or link", "meting-playlist-id")),
        context.meting_playlist_id.data(), context.meting_playlist_id.size(), 0);
    ui->same_line(ui->user, 0.0F, -1.0F);
    if (Button(ui, l10n.Label("meting.playlist_button", "Import playlist", "meting-playlist"), !busy)) {
        const std::string id = PlaylistId(context.meting_playlist_id.data());
        if (id.empty()) {
            SetMetingStatus(context, "no playlist id in the input");
        } else {
            MetingRequest request = MetingListRequest("playlist", id);
            context.meting_busy.store(true, std::memory_order_release);
            SetMetingStatus(context, {});
            if (!QueueMetingRequest(context, std::move(request))) {
                context.meting_busy.store(false, std::memory_order_release);
            }
        }
    }
    std::vector<MetingSong> songs;
    std::string status;
    {
        std::scoped_lock lock(context.meting_mutex);
        songs = context.meting_songs;
        status = context.meting_status;
    }
    if (busy) {
        Text(ui, l10n.Text("meting.busy", "Working..."));
    } else if (!status.empty()) {
        // A bare count after a successful list; anything else is an error.
        const bool count = std::all_of(status.begin(), status.end(),
            [](const char c) { return c >= '0' && c <= '9'; });
        const std::array<std::string_view, 1> args{status};
        if (count) {
            Text(ui, l10n.Format("meting.found", "{0} songs in the in-game album \"Meting\"", args));
        } else {
            Text(ui, l10n.Format("meting.error", "Meting: {0}", args));
        }
    } else if (songs.empty()) {
        Text(ui, l10n.Text("meting.empty", "No results yet"));
    }
    const std::optional<std::size_t> playing = context.engine.MemorySlot();
    for (std::size_t i = 0; i < songs.size(); ++i) {
        const std::string label = (playing == i ? "> " : "") + songs[i].name + " - " + songs[i].artist +
            "###meting-song-" + std::to_string(i);
        if (Button(ui, label)) {
            context.meting_failures.store(0, std::memory_order_release);  // a fresh pick may skip again
            QueueMetingDownload(context, i);
        }
    }
    // A list request succeeded with a new keyword/id: persist it like any
    // other setting change (Draw owns the scheduled save).
    if (context.meting_settings_changed.exchange(false, std::memory_order_acq_rel)) {
        ScheduleSettingsPersist(context);
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
        // A Meting song steps within the Meting list; the engine's own
        // previous/next walk only the local playlist.
        const std::optional<std::size_t> memory_slot = context->engine.MemorySlot();
        if (Button(ui, l10n.Label("action.previous", "Previous", "previous"), snapshot.playing)) {
            if (memory_slot) StepMeting(*context, *memory_slot, -1);
            else context->engine.Previous();
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
            if (memory_slot) StepMeting(*context, *memory_slot, 1);
            else context->engine.Next();
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
        DrawMeting(*context, ui);
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
        Text(ui, l10n.Text("dsd.title", "DSD output"));
        for (std::size_t i = 0; i < kDsdModeKeys.size(); ++i) {
            if (i != 0) ui->same_line(ui->user, 0.0F, -1.0F);
            if (Button(ui, l10n.Label(kDsdModeLabels[i].first, kDsdModeLabels[i].second,
                               kDsdModeKeys[i].second),
                    settings.dsd_mode != kDsdModeKeys[i].first)) {
                settings.dsd_mode = kDsdModeKeys[i].first;
                apply = true;
            }
        }
        Text(ui, l10n.Text("dsd.note",
            "Native and DoP pass the DSD bitstream unchanged and need the ASIO backend; "
            "volume does not apply to them"));
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
