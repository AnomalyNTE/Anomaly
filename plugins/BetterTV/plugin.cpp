// Better TV: replaces the residence TV video source with a local video.
//
// The residence TV (BP_FurnitureActorBase_TV_C) plays CRI Mana movies through
// MovieAudioPlayerComponent. Each program switch calls SetVideoFilePath(FString);
// the component then loads TVFileManaMovie (/Game/Movies/FFMpegUI/TVMovies) and
// decodes the USM from the game content directory. This plugin hooks that setter,
// rewrites the path in memory for the TV actor only. No video is copied into the
// game content directory. Every other MovieAudioPlayer screen keeps the game source.

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui.h"
#include "tv_video_profile.hpp"
#include "audio_output.hpp"
#include "media_prepare.hpp"
#include "file_picker.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <map>

#include <nlohmann/json.hpp>

namespace {

using better_tv_profile::native_layout;
using better_tv_video::AudioDistanceRange;

// Residence TV actor. The plugin compares the component owner's class name, so
// cinema/story screens keep their game videos.
constexpr std::wstring_view kTvActorClassName = L"BP_FurnitureActorBase_TV_C";
// Blueprint function that selects the TV video pipeline (see IsManaPlayerFunction).
constexpr std::size_t kMaxPathChars = 512;
constexpr std::size_t kMaxNameBytes = 256;
// The bilibili link input keeps its own edited copy until the user parses it.
constexpr std::size_t kMaxLinkChars = 1024;

constexpr std::string_view kConfigSchemaId = "shikari.better-tv.settings";
constexpr std::uint32_t kConfigSchemaVersion = 1;
constexpr std::string_view kConfigSchemaJson =
    R"({"type":"object","properties":{"enabled":{"type":"boolean"},)"
    R"("videoPath":{"type":"string","maxLength":4096},)"
    R"("bilibiliUrl":{"type":"string","maxLength":2048},)"
    R"("muteGameAudio":{"type":"boolean"},)"
    R"("forceMediaPlayer":{"type":"boolean"},)"
    R"("audioFullVolumeMeters":{"type":"number","minimum":0.5,"maximum":20},)"
    R"("audioMaxDistanceMeters":{"type":"number","minimum":1.5,"maximum":60}},)"
    R"("required":["enabled","videoPath","muteGameAudio","forceMediaPlayer"],)"
    R"("additionalProperties":false})";

struct HookSlot final {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t original{};
    std::uintptr_t target{};
};

struct Settings final {
    bool enabled{};
    bool force_media_player{};
    std::string video_path;
    // Last link the user submitted. Restored into the input on startup, never
    // fetched automatically.
    std::string bilibili_url;
    AudioDistanceRange audio_distance;
};

// Bilibili preparation state shared by the game update (consumer) and draw
// (renderer). Guarded by Context::mutex.
struct PrepareDisplay final {
    std::uint64_t request_id{};
    std::uint64_t applied_id{};
    better_tv_video::PrepareStage stage{better_tv_video::PrepareStage::Idle};
    better_tv_video::PrepareError error{better_tv_video::PrepareError::None};
    bool active{};
    std::uint64_t received_bytes{};
    std::uint64_t total_bytes{};
    float progress{};
};

struct TelevisionBinding final {
    std::uintptr_t world{}, owner{}, component{}, mesh{}, material{};
    AnomalyGenerationHandleV1 owner_handle{}, component_handle{}, mesh_handle{}, material_handle{};
    std::int32_t slot{-1};
    std::uintptr_t local_material{}, texture{};
    AnomalyGenerationHandleV1 texture_handle{};
    bool screen_restored{}, audio_restored{}, audio_restore_pending{};
};

struct TelevisionAudio final {
    std::uintptr_t component{}, event{};
    AnomalyGenerationHandleV1 component_handle{}, event_handle{};
    std::uintptr_t program{};
};

enum class AudioIssue { None, HookUnavailable, ComponentMismatch, MissingEvent, StaleComponent, StaleEvent };
enum class AudioObservation { Valid, Empty, Invalid };

using MediaClock = std::chrono::steady_clock;
constexpr std::int64_t kTicksPerSecond = 10000000;
struct PlaybackProgress final {
    std::uint64_t generation{};
    std::int64_t position{}, duration{};
    bool seekable{};
    std::optional<std::int64_t> preview;
};
struct SeekRequest final {
    std::uint64_t generation{};
    std::int64_t target{};
    MediaClock::time_point due;
};
enum class SeekStage { Reopening, Opening, Confirming };
struct PlaybackSeek final {
    std::uint64_t id{};
    std::int64_t target{}, previous{};
    SeekStage stage{};
    MediaClock::time_point deadline, issued, last_sample;
    std::optional<std::int64_t> last_position;
    unsigned advances{};
    bool automatic{}, recovering{}, play_requested{};
};

struct TelevisionCandidate final {
    std::uintptr_t world{}, owner{}, component{};
    AnomalyGenerationHandleV1 world_handle{}, owner_handle{}, component_handle{};
    std::string name;
    bool playing{};
};

struct Context final {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalySignatureServiceV1* signatures{};
    const AnomalyHookServiceV1* hook{};
    const AnomalyPatchServiceV1* patch{};
    const AnomalyUe5NamesServiceV1* names{};
    // Used by the MediaPlayer experiment: resolve live objects/functions and
    // call the media framework the game already ships.
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* process_events{};
    const AnomalyConfigServiceV1* config{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyUiServiceV1* ui{};
    anomaly::sdk::Localizer localizer;

    HookSlot play_by_config{};
    HookSlot play_audio{};
    // HTUtil::SetFilePathForPlayer(UObject*, const FString&) is the shared funnel
    // every TV pipeline calls with the program path.
    // UFileMediaSource::SetFilePath stores the final path the media framework opens.
    AnomalyGenerationHandleV1 config_schema{};
    AnomalyGenerationHandleV1 persist_task{};

    std::mutex mutex;
    Settings settings;
    bool settings_dirty{};
    PrepareDisplay prepare;  // Guarded by mutex.
    // Last preparation result written to the log; game thread only.
    std::uint64_t prepare_logged{};
    // Previous replacement switch state, used to detect the turn-off transition
    // (game thread only).
    bool enabled_prev{};
    // Render-thread link input; initialized once from the restored settings.
    std::array<char, kMaxLinkChars> link_input{};
    bool link_input_initialized{};
    // Worker-owned download/merge pipeline. Started on the lifecycle thread;
    // render callbacks never create it or touch its files.
    better_tv_video::MediaPreparer preparer;
    better_tv_video::PrepareOptions prepare_options;
    // The user's video path, applied in memory only. Nothing is ever written to
    // the game directory: the plugin overrides strings and hook results.
    std::wstring override_path;
    better_tv_video::FilePicker picker;
    std::optional<std::string> prepare_intent;
    std::uint64_t prepare_serial{};
    AnomalyGenerationHandleV1 prepare_task{};

    std::atomic<bool> force_media_player{false};
    std::atomic<bool> alive{false};
    std::atomic<unsigned> tv_hits{0};
    // UFunction whose native thunk is the hook target, discovered on the Game thread.
    std::atomic<std::uintptr_t> mana_player_node{0};
    std::atomic<unsigned> mana_player_probes{0};
    std::atomic<unsigned> mana_player_invocations{0};
    std::atomic<bool> logged_set_missing{false};
    std::atomic<bool> logged_mana_missing{false};
    std::atomic<bool> logged_play_missing{false};
    std::atomic<bool> logged_audio_missing{false};
    std::atomic<bool> logged_mana_player_missing{false};
    std::atomic<bool> logged_mana_player_forced{false};
    std::atomic<bool> logged_set_path_for_player_missing{false};
    std::atomic<bool> logged_media_source_missing{false};
    std::atomic<unsigned> set_path_logs{0};
    std::atomic<unsigned> media_source_logs{0};
    // MediaPlayer experiment state: 0 = not started, 1 = waiting for the media
    // player to finish preparing, 2 = finished.
    std::atomic<int> media_probe_phase{2};
    std::atomic<unsigned> media_probe_attempts{0};
    AnomalyGenerationHandleV1 media_player{};
    std::wstring opened_path;
    std::wstring selected_path;
    std::int64_t resume_position{};
    bool takeover_failed{};
    PlaybackProgress progress; // Render snapshot, protected by mutex.
    std::optional<SeekRequest> seek_request; // Latest Render intent, protected by mutex.
    std::optional<PlaybackSeek> playback_seek; // Game thread, leaves the TV takeover installed.
    std::uint64_t next_seek_id{};
    std::int64_t media_duration{};
    bool media_seekable{};
    MediaClock::time_point native_program_transition_until;
    std::uintptr_t opened_world{};
    double media_poll_seconds{};
    double media_wait_seconds{};
    double tv_discovery_seconds{1.0};
    std::atomic<bool> media_playing{false};
    std::string media_status;
    std::uintptr_t tv_owner{};
    std::uintptr_t tv_world{};
    AnomalyGenerationHandleV1 media_close{};
    std::uintptr_t tv_component{};
    AnomalyGenerationHandleV1 tv_owner_handle{};
    TelevisionBinding tv_binding;
    TelevisionBinding last_native_screen;
    TelevisionAudio tv_audio;
    std::map<std::uintptr_t, TelevisionAudio> television_audio; // Game only.
    std::vector<TelevisionCandidate> televisions; // Game only.
    std::vector<TelevisionCandidate> television_display; // mutex protected.
    std::optional<TelevisionCandidate> television_request; // mutex protected.
    AnomalyGenerationHandleV1 displayed_selection{}; // mutex protected.
    AnomalyGenerationHandleV1 discovery_world_handle{};
    std::uintptr_t discovery_world{};
    bool refresh_televisions{}; // mutex protected.
    bool television_discovery_failed{}; // mutex protected.
    AnomalyGenerationHandleV1 native_audio_pending{};
    AudioIssue last_audio_issue{AudioIssue::None};
    AudioObservation last_audio_observation{AudioObservation::Valid};
    bool game_audio_muted{};
    std::uintptr_t media_player_pointer{};
    AnomalyGenerationHandleV1 native_audio_patch{};
    std::map<std::string, std::pair<AnomalyGenerationHandleV1, std::uint16_t>> media_functions;
    std::uintptr_t media_sound{};
    bool media_sound_ready{};
    better_tv_video::AudioOutput audio;
    better_tv_video::AudioOutput::Snapshot last_audio{};
    std::uint64_t audio_session{}, audio_timeline{};
    bool logged_spatial_pose{};
};

// Published for the detours, which run on the game thread.
std::atomic<Context*> g_active{};

using SetVideoFilePathFn = void(ANOMALY_CALL*)(void*, void*);
using PlayAudioFn = void(ANOMALY_CALL*)(void*, void*);
// Native UFunction thunk: (UObject* context, FFrame& stack, void* result).
using NativeThunkFn = void(ANOMALY_CALL*)(void*, void*, void*);

AnomalyStatusV1 Status(const std::uint32_t code,
                       const std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const char* id,
                     const std::uint32_t version) noexcept {
    return anomaly::sdk::Host(host).Query<Service>(id, version).get();
}

bool CoreReady(const AnomalyCoreServiceV1* service) noexcept {
    return HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::write_memory)>(
               service, offsetof(AnomalyCoreServiceV1, write_memory)) &&
           service->read_memory != nullptr && service->write_memory != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1* service) noexcept {
    return HasField<AnomalySignatureServiceV1, decltype(AnomalySignatureServiceV1::resolve)>(
               service, offsetof(AnomalySignatureServiceV1, resolve)) &&
           service->resolve != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* service) noexcept {
    return HasField<AnomalyUe5NamesServiceV1,
                    decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               service, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
           service->resolve_utf8 != nullptr;
}

bool HookReady(const AnomalyHookServiceV1* service) noexcept {
    return HasField<AnomalyHookServiceV1, decltype(AnomalyHookServiceV1::end_callback)>(
               service, offsetof(AnomalyHookServiceV1, end_callback)) &&
           service->create != nullptr && service->release != nullptr &&
           service->begin_callback != nullptr && service->end_callback != nullptr;
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

void Log(Context& context, const std::uint32_t level,
         const std::string_view message) noexcept {
    if (context.core != nullptr && context.core->log != nullptr) {
        context.core->log(context.core->user, level, anomaly::sdk::StringView(message));
    }
}

template <typename T>
bool Read(Context& context, const std::uintptr_t address, T& value) noexcept {
    if (!CoreReady(context.core) || address == 0) return false;
    AnomalyMutableByteSpanV1 destination{reinterpret_cast<std::uint8_t*>(&value),
                                         sizeof(value)};
    return context.core->read_memory(context.core->user, address, destination).code ==
           ANOMALY_STATUS_V1_OK;
}

template <typename T>
bool Write(Context& context, const std::uintptr_t address, const T& value) noexcept {
    if (!CoreReady(context.core) || address == 0) return false;
    AnomalyByteSpanV1 source{reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)};
    return context.core->write_memory(context.core->user, address, source).code ==
           ANOMALY_STATUS_V1_OK;
}

bool ReadBytes(Context& context, const std::uintptr_t address, void* data,
               const std::size_t size) noexcept {
    if (!CoreReady(context.core) || address == 0 || data == nullptr || size == 0)
        return false;
    AnomalyMutableByteSpanV1 destination{reinterpret_cast<std::uint8_t*>(data), size};
    return context.core->read_memory(context.core->user, address, destination).code ==
           ANOMALY_STATUS_V1_OK;
}

bool WriteBytes(Context& context, const std::uintptr_t address, const void* data,
                const std::size_t size) noexcept {
    if (!CoreReady(context.core) || address == 0 || data == nullptr || size == 0)
        return false;
    AnomalyByteSpanV1 source{reinterpret_cast<const std::uint8_t*>(data), size};
    return context.core->write_memory(context.core->user, address, source).code ==
           ANOMALY_STATUS_V1_OK;
}

std::string WideToUtf8(const std::wstring_view value) noexcept {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size, nullptr,
                            nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring Utf8ToWide(const std::string_view value) noexcept {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size) != size) {
        return {};
    }
    return result;
}

// FName id of the owner actor's class equals the TV blueprint class.
bool NameEquals(Context& context, const std::uintptr_t address,
                const std::wstring_view expected_ascii) noexcept {
    std::uint32_t id{};
    char name[kMaxNameBytes]{};
    std::size_t size = sizeof(name);
    if (!Read(context, address, id) || id == 0 ||
        context.names->resolve_utf8(context.names->user, id, name, &size).code !=
            ANOMALY_STATUS_V1_OK) {
        return false;
    }
    const std::string expected = WideToUtf8(expected_ascii);
    return size == expected.size() + 1 &&
           std::string_view(name, expected.size()) == expected;
}

bool ReadFNameText(Context& context, const std::uintptr_t address, char* output,
                   const std::size_t capacity) noexcept {
    if (output == nullptr || capacity < 2) return false;
    output[0] = '\0';
    std::uint32_t id{};
    std::size_t size = capacity;
    return Read(context, address, id) && id != 0 &&
           context.names->resolve_utf8(context.names->user, id, output, &size).code ==
               ANOMALY_STATUS_V1_OK;
}

// Class name of a live UObject, used only to describe what the game handed to
// the video pipeline in diagnostics.
bool ObjectClassText(Context& context, const void* object, char* output,
                     const std::size_t capacity) noexcept {
    if (object == nullptr || output == nullptr || capacity == 0) return false;
    output[0] = '\0';
    std::uintptr_t type{};
    return Read(context,
                reinterpret_cast<std::uintptr_t>(object) + native_layout.kObjectClassOffset,
                type) &&
           type != 0 &&
           ReadFNameText(context, type + native_layout.kObjectNameOffset, output, capacity);
}

bool IsTvActor(Context& context, std::uintptr_t owner) noexcept {
    std::uintptr_t type{};
    if (!owner || !Read(context, owner + native_layout.kObjectClassOffset, type)) return false;
    for (unsigned depth = 0; type && depth < 32; ++depth) {
        if (NameEquals(context, type + native_layout.kObjectNameOffset, kTvActorClassName)) return true;
        if (!Read(context, type + native_layout.kStructSuperOffset, type)) return false;
    }
    return false;
}

bool IsTvComponent(Context& context, void* self) noexcept {
    std::uintptr_t owner{};
    return self && Read(context, reinterpret_cast<std::uintptr_t>(self) +
        native_layout.kObjectOuterOffset, owner) && IsTvActor(context, owner);
}


bool InstallHook(Context& context, const std::uintptr_t target, void* detour,
                 const std::string_view label, HookSlot& slot,
                 AnomalyStatusV1* create_status = nullptr) noexcept {
    if (target == 0 || !HookReady(context.hook)) return false;
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = detour;
    request.label = anomaly::sdk::StringView(label);
    std::uintptr_t original{};
    AnomalyGenerationHandleV1 handle{};
    const AnomalyStatusV1 status =
        context.hook->create(context.hook->user, &request, &original, &handle);
    if (create_status != nullptr) *create_status = status;
    if (status.code != ANOMALY_STATUS_V1_OK || handle.id == 0 || original == 0) {
        return false;
    }
    slot.handle = handle;
    slot.original = original;
    slot.target = target;
    return true;
}
void ReleaseHook(Context& context, HookSlot& slot) noexcept {
    if (slot.handle.id == 0) return;
    if (HookReady(context.hook)) {
        static_cast<void>(context.hook->release(context.hook->user, slot.handle));
    }
    slot = HookSlot{};
}

bool ResolveValidated(Context& context, const std::string_view symbol,
                      std::uintptr_t& target) noexcept {
    target = better_tv_profile::Address(symbol);
    return target != 0 && better_tv_profile::compatibility.MatchesEntry(
                              context.core, symbol, target);
}

bool PinPointer(Context&, std::uintptr_t, AnomalyGenerationHandleV1&);
void ANOMALY_CALL PlayByConfigDetour(void* self, void* row) noexcept;
void ANOMALY_CALL PlayAudioDetour(void* self, void* argument) noexcept;

void EnsureHooks(Context& context) noexcept {
    if (context.play_by_config.handle.id == 0) {
        std::uintptr_t target{};
        if (ResolveValidated(context, "playByConfig", target) &&
            InstallHook(context, target, reinterpret_cast<void*>(&PlayByConfigDetour),
                        "tv-video-play-by-config", context.play_by_config)) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                          "TV video PlayByConfig hook installed=0x%llX",
                          static_cast<unsigned long long>(target));
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
        } else if (!context.logged_play_missing.exchange(true, std::memory_order_acq_rel)) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "TV video PlayByConfig hook unavailable");
        }
    }
    if (context.play_audio.handle.id == 0) {
        std::uintptr_t target{};
        if (ResolveValidated(context, "playAudio", target) &&
            InstallHook(context, target, reinterpret_cast<void*>(&PlayAudioDetour),
                        "tv-video-play-audio", context.play_audio)) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV video audio hook installed");
        } else if (!context.logged_audio_missing.exchange(true, std::memory_order_acq_rel)) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "TV video audio hook unavailable: playAudio signature or entry check failed");
        }
    }
}

// The residence TV ships a parallel MediaPlayer setup (TVMediaPlayer +
// TVFileMediaSource) that the blueprint never selects in this build. The
// experiment drives it directly from the game thread to learn whether the media
// framework can open a local file; nothing else in the game is touched.
constexpr std::string_view kMediaPlayerAsset =
    "/Game/Movies/FFMpegUI/TVMovies/TVMediaPlayer.TVMediaPlayer";
constexpr std::string_view kMediaSourceAsset =
    "/Game/Movies/FFMpegUI/TVMovies/TVFileMediaSource.TVFileMediaSource";

bool FindObjectHandle(Context& context, const std::string_view path,
                      AnomalyGenerationHandleV1& handle) noexcept {
    handle = AnomalyGenerationHandleV1{};
    if (context.objects == nullptr || context.objects->find_exact == nullptr) return false;
    return context.objects->find_exact(context.objects->user,
                                       anomaly::sdk::StringView(path), &handle)
                   .code == ANOMALY_STATUS_V1_OK &&
        handle.id != 0;
}

bool CanInvoke(const Context& context) noexcept {
    if (context.process_events == nullptr || context.process_events->invoke == nullptr)
        return false;
    const std::size_t required = offsetof(AnomalyUe5ProcessEventServiceV1, invoke) +
                                 sizeof(AnomalyUe5ProcessEventServiceV1::invoke);
    return context.process_events->struct_size >= required;
}

// UE function paths are resolved through the game's StaticFindObject; the
// accepted separator differs between "Class:Function" and "Class.Function"
// spellings, so both are attempted and the outcome is reported.
struct ResolvedFunction final {
    AnomalyGenerationHandleV1 handle{};
    std::uint32_t colon_code{};
    std::uint32_t dot_code{};
};

ResolvedFunction ResolveFunction(Context& context, const std::string_view class_path,
                                 const std::string_view function) noexcept {
    ResolvedFunction result{};
    if (context.objects == nullptr || context.objects->find_exact == nullptr) {
        result.colon_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        return result;
    }
    char buffer[256]{};
    for (int attempt = 0; attempt < 2 && result.handle.id == 0; ++attempt) {
        const char separator = attempt == 0 ? ':' : '.';
        const int written = std::snprintf(buffer, sizeof(buffer), "%.*s%c%.*s",
                                          static_cast<int>(class_path.size()),
                                          class_path.data(), separator,
                                          static_cast<int>(function.size()),
                                          function.data());
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) continue;
        AnomalyGenerationHandleV1 handle{};
        const std::uint32_t code =
            context.objects
                ->find_exact(context.objects->user,
                             anomaly::sdk::StringView(
                                 std::string_view(buffer, static_cast<std::size_t>(written))),
                             &handle)
                .code;
        if (attempt == 0) result.colon_code = code;
        else result.dot_code = code;
        if (code == ANOMALY_STATUS_V1_OK && handle.id != 0) {
            result.handle = handle;
        }
    }
    return result;
}

bool InvokeResolved(Context& context, const AnomalyGenerationHandleV1 object,
                    const ResolvedFunction& function, void* parameters,
                    const std::size_t size, std::uint32_t& code) noexcept {
    if (function.handle.id == 0) return false;
    code = context.process_events
               ->invoke(context.process_events->user, object, function.handle, parameters, size)
               .code;
    return code == ANOMALY_STATUS_V1_OK;
}

// The public object service validates identities; the native lookup supplies the
// pointer required by UE's in-process UObject parameters. No pointer crosses the SDK.
std::uintptr_t FindNativeUnchecked(std::uintptr_t function, const wchar_t* path) noexcept {
    using Find = std::uintptr_t(ANOMALY_CALL*)(void*, void*, const wchar_t*, bool);
    __try {
        return reinterpret_cast<Find>(function)(nullptr, reinterpret_cast<void*>(-1), path, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool PinPointer(Context& context, std::uintptr_t pointer, AnomalyGenerationHandleV1& handle) {
    std::uint32_t index{}, name{};
    if (!pointer || !Read(context, pointer + native_layout.kObjectIndexOffset, index) ||
        !Read(context, pointer + native_layout.kObjectNameOffset, name)) return false;
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (context.objects->snapshot_at(context.objects->user, index, &snapshot).code != 0 ||
        snapshot.name_id != name || snapshot.handle.id == 0) return false;
    handle = snapshot.handle;
    return true;
}

std::uintptr_t FindNativeObject(Context& context, const std::string_view path) {
    AnomalyGenerationHandleV1 expected{};
    if (!FindObjectHandle(context, path, expected)) return 0;
    const auto find = better_tv_profile::Address("staticFindObject");
    if (!find) return 0;
    const auto wide = Utf8ToWide(path);
    const auto pointer = FindNativeUnchecked(find, wide.c_str());
    AnomalyGenerationHandleV1 actual{};
    return PinPointer(context, pointer, actual) && actual.id == expected.id &&
        actual.generation == expected.generation ? pointer : 0;
}

struct ReflectedParameter {
    const wchar_t* name;
    const wchar_t* type;
    std::size_t offset, size;
    const wchar_t* structure{};
};

bool MatchParameters(Context& context, std::uintptr_t function,
                     std::span<const ReflectedParameter> expected) {
    if (expected.empty()) return true;
    std::uintptr_t field{};
    if (!Read(context, function + native_layout.kStructPropertyLinkOffset, field)) return false;
    unsigned seen{};
    for (; field;) {
        std::uintptr_t type{};
        std::uint32_t offset{}, size{};
        if (!Read(context, field + native_layout.kFieldClassOffset, type) || !type ||
            !Read(context, field + native_layout.kPropertyValueOffset, offset) ||
            !Read(context, field + native_layout.kPropertySizeOffset, size)) return false;
        const auto match = std::ranges::find_if(expected, [&](const ReflectedParameter& parameter) {
            return parameter.offset == offset && parameter.size == size &&
                NameEquals(context, field + native_layout.kPropertyNameOffset, parameter.name) &&
                NameEquals(context, type + native_layout.kFieldClassNameOffset, parameter.type);
        });
        if (match == expected.end()) return false;
        if (match->structure) {
            std::uintptr_t structure{};
            if (!Read(context, field + native_layout.kStructPropertyStructOffset, structure) || !structure ||
                !NameEquals(context, structure + native_layout.kObjectNameOffset, match->structure)) return false;
        }
        const auto bit = 1u << (match - expected.begin());
        if (seen & bit) return false;
        seen |= bit;
        if (!Read(context, field + native_layout.kPropertyNextOffset, field)) return false;
    }
    return seen == (1u << expected.size()) - 1;
}

bool InvokePointer(Context& context, std::uintptr_t receiver, const char* path,
                   void* parameters, std::size_t size, std::size_t unpadded_size = 0,
                   std::span<const ReflectedParameter> expected = {}) {
    const auto matches_size = [=](std::size_t actual) {
        return actual == size || (unpadded_size && actual == unpadded_size);
    };
    AnomalyGenerationHandleV1 object{};
    if (!PinPointer(context, receiver, object)) return false;
    auto found = context.media_functions.find(path);
    if (found == context.media_functions.end()) {
        const auto function = FindNativeObject(context, path);
        std::uint16_t parms_size{};
        AnomalyGenerationHandleV1 handle{};
        if (!function || !Read(context, function + native_layout.kFunctionParmsSizeOffset, parms_size) ||
            !matches_size(parms_size) || !PinPointer(context, function, handle) ||
            !MatchParameters(context, function, expected)) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                std::string("TV function contract unavailable: ") + path);
            return false;
        }
        found = context.media_functions.emplace(path, std::make_pair(handle, parms_size)).first;
    }
    if (!matches_size(found->second.second)) return false;
    return context.process_events->invoke(context.process_events->user, object,
        found->second.first, parameters, size).code == ANOMALY_STATUS_V1_OK;
}

bool ObjectProperty(Context& context, std::uintptr_t object, const wchar_t* name,
                    std::size_t size, std::uintptr_t& field_address) {
    std::uintptr_t type{};
    if (!Read(context, object + native_layout.kObjectClassOffset, type)) return false;
    for (unsigned depth = 0; type != 0 && depth < 32; ++depth) {
        std::uintptr_t property{};
        if (!Read(context, type + native_layout.kStructPropertyLinkOffset, property)) return false;
        for (unsigned i = 0; property != 0 && i < 512; ++i) {
            if (NameEquals(context, property + native_layout.kPropertyNameOffset, name)) {
                std::int32_t offset{}, element_size{};
                if (!Read(context, property + native_layout.kPropertyValueOffset, offset) ||
                    !Read(context, property + native_layout.kPropertySizeOffset, element_size) ||
                    offset < 0 || offset > 65536 || element_size != size) return false;
                std::uintptr_t field_class{};
                if (!Read(context, property + native_layout.kFieldClassOffset, field_class) || !field_class ||
                    !NameEquals(context, field_class + native_layout.kFieldClassNameOffset,
                        size == sizeof(std::uintptr_t) ? L"ObjectProperty" : L"BoolProperty")) return false;
                if (size == 1) {
                    std::array<std::uint8_t, 4> boolean{};
                    if (!ReadBytes(context, property + native_layout.kBoolPropertyLayoutOffset,
                        boolean.data(), boolean.size()) || boolean[0] != 1 || boolean[1] != 0 ||
                        boolean[3] != 0xFF) return false;
                }
                field_address = object + static_cast<std::uintptr_t>(offset);
                return true;
            }
            if (!Read(context, property + native_layout.kPropertyNextOffset, property)) return false;
        }
        if (!Read(context, type + native_layout.kStructSuperOffset, type)) return false;
    }
    return false;
}

std::uintptr_t CurrentWorld(Context& context) {
    const auto instruction = better_tv_profile::Address("worldAccessor");
    std::int32_t displacement{};
    std::uintptr_t world{};
    // Decode the validated x64 MOV r64,[RIP+disp32] instruction.
    if (!instruction || !Read(context, instruction + 3, displacement) ||
        !Read(context, instruction + 7 + displacement, world) || !world) return 0;
    AnomalyGenerationHandleV1 world_handle{};
    if (!PinPointer(context, world, world_handle)) return 0;
    char type[64]{};
    if (!ObjectClassText(context, reinterpret_cast<void*>(world), type, sizeof(type)) ||
        std::string_view(type) != "World") return 0;
    return world;
}

bool LivePointer(Context&, std::uintptr_t, AnomalyGenerationHandleV1);
bool SameHandle(AnomalyGenerationHandleV1, AnomalyGenerationHandleV1);
bool OwnsComponent(Context&, std::uintptr_t, std::uintptr_t);
bool StopTakeover(Context&, std::uintptr_t, bool);

bool BelongsToWorld(Context& context, std::uintptr_t object, std::uintptr_t world) {
    for (unsigned depth = 0; object && depth < 32; ++depth) {
        if (object == world) return true;
        if (!Read(context, object + native_layout.kObjectOuterOffset, object)) return false;
    }
    return false;
}

bool ReadTelevision(Context& context, std::uintptr_t world, std::uintptr_t owner,
                    TelevisionCandidate& result) {
    result = {};
    if (!IsTvActor(context, owner) || !BelongsToWorld(context, owner, world) ||
        !PinPointer(context, world, result.world_handle) ||
        !PinPointer(context, owner, result.owner_handle)) return false;
    result.world = world;
    result.owner = owner;
    char name[kMaxNameBytes]{};
    if (ReadFNameText(context, owner + native_layout.kObjectNameOffset, name, sizeof(name))) result.name = name;
    std::uintptr_t field{};
    if (ObjectProperty(context, owner, L"MovieAudioPlayer", sizeof(result.component), field) &&
        Read(context, field, result.component) && OwnsComponent(context, owner, result.component) &&
        PinPointer(context, result.component, result.component_handle)) {
        char type[128]{};
        if (ObjectClassText(context, reinterpret_cast<void*>(result.component), type, sizeof(type)) &&
            std::string_view(type) == "MovieAudioPlayerComponent") {
            std::uint8_t playing{};
            result.playing = InvokePointer(context, result.component,
                "/Script/HTGame.MovieAudioPlayerComponent:IsPlaying", &playing, sizeof(playing)) && playing;
        } else result.component = 0;
    } else result.component = 0;
    return true;
}

// Native UE TArray ABI. Its allocation belongs to FMemory, never the plugin CRT.
struct TelevisionArray { std::uintptr_t data{}; std::int32_t count{}, capacity{}; };
static_assert(sizeof(TelevisionArray) == 16);
void FreeTelevisionArray(std::uintptr_t function, std::uintptr_t data) noexcept {
    __try { reinterpret_cast<void(ANOMALY_CALL*)(void*)>(function)(reinterpret_cast<void*>(data)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool ReadTelevisionArray(Context& context, std::uintptr_t world, const TelevisionArray& actors,
                         std::vector<TelevisionCandidate>& output) {
    // Bound engine output before reading the caller-owned result buffer.
    if (actors.count < 0 || actors.capacity < actors.count || actors.count > 65536 ||
        (actors.count && !actors.data)) return false;
    for (std::int32_t i = 0; i < actors.count; ++i) {
        std::uintptr_t owner{};
        TelevisionCandidate candidate;
        if (Read(context, actors.data + i * sizeof(owner), owner) &&
            ReadTelevision(context, world, owner, candidate)) output.push_back(std::move(candidate));
    }
    return true;
}

bool EnumerateTelevisions(Context& context, std::uintptr_t world,
                         std::vector<TelevisionCandidate>& output) {
    std::uintptr_t release{};
    if (!ResolveValidated(context, "memoryFree", release)) return false;
    const auto actor_class = FindNativeObject(context,
        "/Game/Blueprints/Level/World/Common/Room/BP_FurnitureActorBase_TV.BP_FurnitureActorBase_TV_C");
    const auto statics = FindNativeObject(context, "/Script/Engine.Default__GameplayStatics");
    if (!actor_class || !statics) return false;
    struct Parameters { std::uintptr_t world, type; TelevisionArray actors; } parameters{world, actor_class};
    constexpr ReflectedParameter fields[] = {
        {L"WorldContextObject", L"ObjectProperty", offsetof(Parameters, world), sizeof(parameters.world)},
        {L"ActorClass", L"ClassProperty", offsetof(Parameters, type), sizeof(parameters.type)},
        {L"OutActors", L"ArrayProperty", offsetof(Parameters, actors), sizeof(parameters.actors)}};
    struct Release {
        std::uintptr_t function; TelevisionArray& array;
        ~Release() { if (array.data) FreeTelevisionArray(function, array.data); }
    } cleanup{release, parameters.actors};
    if (!InvokePointer(context, statics, "/Script/Engine.GameplayStatics:GetAllActorsOfClass",
            &parameters, sizeof(parameters), 0, fields)) return false;
    if (!ReadTelevisionArray(context, world, parameters.actors, output)) return false;
    std::ranges::sort(output, {}, [](const auto& item) { return item.owner_handle.id; });
    return true;
}

bool SelectTelevision(Context& context, std::uintptr_t world, const TelevisionCandidate& request) {
    const auto found = std::ranges::find_if(context.televisions, [&](const auto& item) {
        return item.world == world && SameHandle(item.world_handle, request.world_handle) &&
            SameHandle(item.owner_handle, request.owner_handle);
    });
    if (found == context.televisions.end() || !LivePointer(context, world, found->world_handle) ||
        !LivePointer(context, found->owner, found->owner_handle) ||
        !BelongsToWorld(context, found->owner, world)) return false;
    if (SameHandle(context.tv_owner_handle, found->owner_handle) && context.tv_owner == found->owner) return true;
    std::uint8_t old_playing{};
    const bool restore_audio = context.tv_binding.component &&
        LivePointer(context, context.tv_binding.component, context.tv_binding.component_handle) &&
        InvokePointer(context, context.tv_binding.component,
            "/Script/HTGame.MovieAudioPlayerComponent:IsPlaying", &old_playing, sizeof(old_playing)) && old_playing;
    if (!StopTakeover(context, world, restore_audio)) {
        std::scoped_lock lock(context.mutex);
        context.media_status = context.localizer.Text("tv.restore_failed", "Could not restore the previous television; select again to retry");
        return false;
    }
    context.tv_owner = found->owner;
    context.tv_owner_handle = found->owner_handle;
    context.tv_world = world;
    context.tv_component = found->component;
    context.tv_audio = {};
    const auto audio = context.television_audio.find(found->component);
    if (audio != context.television_audio.end() &&
        SameHandle(audio->second.component_handle, found->component_handle)) context.tv_audio = audio->second;
    context.takeover_failed = false;
    std::scoped_lock lock(context.mutex);
    context.displayed_selection = found->owner_handle;
    return true;
}

bool FindTelevision(Context& context, std::uintptr_t world, double elapsed) {
    const bool world_changed = world != context.discovery_world ||
        (world && !LivePointer(context, world, context.discovery_world_handle));
    if (world_changed || (context.tv_owner && (!LivePointer(context, context.tv_owner, context.tv_owner_handle) ||
            !BelongsToWorld(context, context.tv_owner, world)))) {
        StopTakeover(context, world_changed ? 0 : world, false);
        context.tv_owner = context.tv_component = 0;
        context.tv_owner_handle = {};
        context.tv_audio = {};
        context.takeover_failed = false;
        context.televisions.clear();
        if (world_changed) {
            std::erase_if(context.television_audio, [&](const auto& item) {
                return !world || !LivePointer(context, item.first, item.second.component_handle) ||
                    !BelongsToWorld(context, item.first, world);
            });
            context.media_player = {};
            context.media_close = {};
            context.media_player_pointer = 0;
            context.media_functions.clear();
        }
        context.discovery_world = world;
        context.discovery_world_handle = {};
        if (world) PinPointer(context, world, context.discovery_world_handle);
        context.tv_discovery_seconds = 1;
        std::scoped_lock lock(context.mutex);
        context.television_display.clear();
        context.displayed_selection = {};
        context.television_request.reset();
    }
    bool refresh{};
    std::optional<TelevisionCandidate> request;
    {
        std::scoped_lock lock(context.mutex);
        refresh = std::exchange(context.refresh_televisions, false);
        request = std::exchange(context.television_request, {});
    }
    context.tv_discovery_seconds += elapsed;
    if (world && (refresh || context.tv_discovery_seconds >= 1)) {
        context.tv_discovery_seconds = 0;
        std::vector<TelevisionCandidate> candidates;
        const bool success = EnumerateTelevisions(context, world, candidates);
        context.televisions = std::move(candidates);
        std::erase_if(context.television_audio, [&](const auto& item) {
            return !LivePointer(context, item.first, item.second.component_handle) ||
                !BelongsToWorld(context, item.first, world);
        });
        std::scoped_lock lock(context.mutex);
        context.television_display = context.televisions;
        context.television_discovery_failed = !success;
    }
    if (request) SelectTelevision(context, world, *request);
    return context.tv_owner != 0;
}

bool NativeProgramIsChanging(const Context& context, std::uintptr_t component, MediaClock::time_point now) {
    return context.media_playing.load(std::memory_order_acquire) &&
        component == context.tv_binding.component && now < context.native_program_transition_until;
}

bool TelevisionPlaying(Context& context, bool keep_during_program_change = false) {
    std::uintptr_t field{}, component{};
    if (!ObjectProperty(context, context.tv_owner, L"MovieAudioPlayer", sizeof(component), field) ||
        !Read(context, field, component) || !component) return false;
    char type[128]{};
    if (!ObjectClassText(context, reinterpret_cast<void*>(component), type, sizeof(type)) ||
        std::string_view(type) != "MovieAudioPlayerComponent") return false;
    std::uint8_t playing{};
    if (!InvokePointer(context, component, "/Script/HTGame.MovieAudioPlayerComponent:IsPlaying",
        &playing, sizeof(playing))) return false;
    context.tv_component = component;
    const auto audio = context.television_audio.find(component);
    if (audio != context.television_audio.end() && LivePointer(context, component, audio->second.component_handle))
        context.tv_audio = audio->second;
    return playing != 0 || (keep_during_program_change &&
        NativeProgramIsChanging(context, component, MediaClock::now()));
}

bool CloseMediaPlayer(Context& context) {
    return context.media_close.id && context.media_player.id && context.process_events &&
        context.process_events->invoke(context.process_events->user,
            context.media_player, context.media_close, nullptr, 0).code == ANOMALY_STATUS_V1_OK;
}

void DetachLocalTexture(Context&, const TelevisionBinding&);

void CloseLocalMedia(Context& context, const TelevisionBinding& binding = {}) {
    context.media_playing.store(false);
    context.audio.Transport({}, 0, false);
    ++context.audio_session;
    context.logged_spatial_pose = false;
    if (!context.opened_path.empty()) {
        DetachLocalTexture(context, binding);
        static_cast<void>(CloseMediaPlayer(context));
    }
    context.opened_path.clear();
    context.opened_world = 0;
    context.media_probe_phase.store(2);
    if (context.playback_seek)
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "TV seek #" + std::to_string(context.playback_seek->id) + " cancelled");
    context.playback_seek.reset();
    context.media_duration = 0;
    context.media_seekable = false;
    context.native_program_transition_until = {};
    {
        std::scoped_lock lock(context.mutex);
        context.progress = {context.progress.generation + 1};
        context.seek_request.reset();
    }
}

bool LivePointer(Context& context, std::uintptr_t pointer, AnomalyGenerationHandleV1 expected) {
    AnomalyGenerationHandleV1 actual{};
    return expected.id && PinPointer(context, pointer, actual) &&
        actual.id == expected.id && actual.generation == expected.generation;
}

bool OwnsComponent(Context& context, std::uintptr_t owner, std::uintptr_t component) {
    std::uintptr_t outer{};
    return Read(context, component + native_layout.kObjectOuterOffset, outer) && outer == owner;
}

bool SameHandle(AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) {
    return a.id == b.id && a.generation == b.generation;
}

AudioIssue CheckNativeAudio(Context& context, std::uintptr_t component) {
    if (!context.play_audio.handle.id) return AudioIssue::HookUnavailable;
    const auto& audio = context.tv_audio;
    if (audio.component != component) return AudioIssue::ComponentMismatch;
    if (!audio.event) return AudioIssue::MissingEvent;
    if (!LivePointer(context, component, audio.component_handle)) return AudioIssue::StaleComponent;
    if (!LivePointer(context, audio.event, audio.event_handle)) return AudioIssue::StaleEvent;
    return AudioIssue::None;
}

void ReportAudioIssue(Context& context, AudioIssue issue) {
    if (std::exchange(context.last_audio_issue, issue) == issue) return;
    switch (issue) {
    case AudioIssue::None: return;
    case AudioIssue::HookUnavailable: Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "TV audio recovery: hook unavailable"); break;
    case AudioIssue::ComponentMismatch: Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV audio recovery: no event for selected component"); break;
    case AudioIssue::MissingEvent: Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV audio recovery: no event for current program"); break;
    case AudioIssue::StaleComponent: Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "TV audio recovery: stale component handle"); break;
    case AudioIssue::StaleEvent: Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "TV audio recovery: stale event handle"); break;
    }
}

bool NeedsNativeAudio(Context& context) {
    if (context.media_playing.load(std::memory_order_acquire)) return false;
    const auto issue = CheckNativeAudio(context, context.tv_component);
    ReportAudioIssue(context, issue);
    return issue != AudioIssue::None;
}

bool AudioComponentRelevant(Context& context, std::uintptr_t component) {
    if (context.tv_binding.slot >= 0) return context.tv_binding.component == component;
    return !context.tv_component || context.tv_component == component ||
        !LivePointer(context, context.tv_owner, context.tv_owner_handle);
}

void ObserveNativeProgram(Context& context, std::uintptr_t component, std::uintptr_t program) {
    AnomalyGenerationHandleV1 handle{};
    if (!PinPointer(context, component, handle)) return;
    if (program && context.media_playing.load(std::memory_order_acquire) &&
        component == context.tv_binding.component && SameHandle(handle, context.tv_binding.component_handle)) {
        context.native_program_transition_until = MediaClock::now() + std::chrono::seconds(1);
    }
    const auto& audio = context.television_audio[component];
    if (program && audio.component == component && SameHandle(audio.component_handle, handle) && audio.program == program)
        return;
    // PlayByConfig can synchronously call PlayAudio. Publish the new program first.
    auto& recorded = context.television_audio[component];
    recorded = {component, 0, handle, {}, program};
    if (AudioComponentRelevant(context, component)) context.tv_audio = recorded;
}

AnomalyGenerationHandleV1 ObserveNativeAudio(Context& context, std::uintptr_t component, std::uintptr_t event) {
    AnomalyGenerationHandleV1 component_handle{}, event_handle{};
    const auto observation = !event ? AudioObservation::Empty :
        PinPointer(context, component, component_handle) && PinPointer(context, event, event_handle) ?
            AudioObservation::Valid : AudioObservation::Invalid;
    if (std::exchange(context.last_audio_observation, observation) != observation) {
        if (observation == AudioObservation::Empty)
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV audio recovery: ignored empty event");
        if (observation == AudioObservation::Invalid)
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "TV audio recovery: ignored unverifiable event");
    }
    if (observation != AudioObservation::Valid) return {};
    const auto& old = context.television_audio[component];
    const auto program = old.component == component && SameHandle(old.component_handle, component_handle) ? old.program : 0;
    auto& recorded = context.television_audio[component];
    recorded = {component, event, component_handle, event_handle, program};
    if (AudioComponentRelevant(context, component)) context.tv_audio = recorded;
    return component_handle;
}

enum class BindResult { Success, ScreenUnavailable, RecoveryFailed, MuteFailed, BindFailed };

constexpr auto kNativeScreenAsset =
    "/Game/Movies/FFMpegUI/TVMovies/TVManaPlayer_Texture_Mat.TVManaPlayer_Texture_Mat";
constexpr auto kLocalScreenAsset =
    "/Game/Movies/FFMpegUI/TVMovies/TVMediaPlayer_Video_Mat.TVMediaPlayer_Video_Mat";
constexpr auto kSetScreenMaterial = "/Script/Engine.PrimitiveComponent:SetMaterial";
constexpr auto kTextureGetPlayer = "/Script/MediaAssets.MediaTexture:GetMediaPlayer";
constexpr auto kTextureSetPlayer = "/Script/MediaAssets.MediaTexture:SetMediaPlayer";
constexpr auto kTextureUpdateResource = "/Script/MediaAssets.MediaTexture:UpdateResource";

bool GetTexturePlayer(Context& context, std::uintptr_t texture, std::uintptr_t& player) {
    return InvokePointer(context, texture, kTextureGetPlayer, &player, sizeof(player));
}

bool SetTexturePlayer(Context& context, std::uintptr_t texture, std::uintptr_t player) {
    std::uintptr_t current{};
    return InvokePointer(context, texture, kTextureSetPlayer, &player, sizeof(player)) &&
        GetTexturePlayer(context, texture, current) && current == player;
}

void DetachLocalTexture(Context& context, const TelevisionBinding& binding) {
    if (!LivePointer(context, binding.texture, binding.texture_handle)) return;
    std::uintptr_t current{};
    if (!GetTexturePlayer(context, binding.texture, current) ||
        (current && current == context.media_player_pointer &&
            !SetTexturePlayer(context, binding.texture, 0))) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "TV restore: could not detach the local media texture");
    }
}

struct ScreenMaterialParameters {
    std::int32_t slot;
    std::int32_t padding{};
    std::uintptr_t material{};
};

std::uintptr_t MaterialBase(Context& context, std::uintptr_t material,
                            std::uintptr_t native, std::uintptr_t local) {
    for (unsigned depth = 0; material && depth < 8; ++depth) {
        if (material == native || material == local) return material;
        std::uintptr_t field{}, parent{};
        if (!ObjectProperty(context, material, L"Parent", sizeof(parent), field) ||
            !Read(context, field, parent) || parent == material) break;
        material = parent;
    }
    return 0;
}

BindResult FindScreenSlot(Context& context, TelevisionBinding& binding,
                          std::uintptr_t native, std::uintptr_t local, bool& residue) {
    std::int32_t count{};
    if (!InvokePointer(context, binding.mesh, "/Script/Engine.PrimitiveComponent:GetNumMaterials",
            &count, sizeof(count)) || count < 1 || count > 64) return BindResult::ScreenUnavailable;
    for (std::int32_t slot = 0; slot < count; ++slot) {
        ScreenMaterialParameters parameters{slot};
        if (!InvokePointer(context, binding.mesh, "/Script/Engine.PrimitiveComponent:GetMaterial",
                &parameters, sizeof(parameters))) return BindResult::ScreenUnavailable;
        const auto base = MaterialBase(context, parameters.material, native, local);
        if (!base) continue;
        if (binding.slot >= 0) return BindResult::ScreenUnavailable;
        binding.slot = slot;
        binding.material = parameters.material;
        residue = base == local;
    }
    return binding.slot >= 0 ? BindResult::Success : BindResult::ScreenUnavailable;
}

bool NativeManaChain(Context& context, std::uintptr_t component) {
    // The native component's Play/IsPlaying path uses this Mana pair.
    const auto matches = [&](const wchar_t* property, const wchar_t* asset, std::string_view type) {
        std::uintptr_t field{}, object{};
        AnomalyGenerationHandleV1 handle{};
        char class_name[128]{};
        return ObjectProperty(context, component, property, sizeof(object), field) &&
            Read(context, field, object) && PinPointer(context, object, handle) &&
            ObjectClassText(context, reinterpret_cast<void*>(object), class_name, sizeof(class_name)) &&
            class_name == type && NameEquals(context, object + native_layout.kObjectNameOffset, asset);
    };
    return matches(L"ManaMediaPlayer", L"TVManaPlayer", "ManaPlayer") &&
        matches(L"ManaFileMediaSource", L"TVFileManaMovie", "FileManaMovie");
}

bool SetScreenMaterial(Context& context, const TelevisionBinding& binding, std::uintptr_t material) {
    ScreenMaterialParameters parameters{binding.slot, 0, material};
    return InvokePointer(context, binding.mesh, kSetScreenMaterial, &parameters, sizeof(parameters));
}

BindResult ApplyScreenBinding(Context& context, TelevisionBinding binding, std::uintptr_t material,
                              std::uintptr_t texture, bool residue) {
    if (residue && !SetScreenMaterial(context, binding, binding.material)) return BindResult::RecoveryFailed;
    const auto player = context.media_player_pointer;
    std::uintptr_t previous{};
    if (!player || !PinPointer(context, texture, binding.texture_handle) ||
        !GetTexturePlayer(context, texture, previous) || (previous && previous != player))
        return BindResult::BindFailed;
    if (!InvokePointer(context, binding.component,
            "/Script/HTGame.MovieAudioPlayerComponent:StopAudio", nullptr, 0)) return BindResult::MuteFailed;
    context.game_audio_muted = true;
    binding.local_material = material;
    binding.texture = texture;
    context.tv_binding = binding;
    // The asset can still reference this same UObject from a previous session.
    // Changing through null forces UE to discard the old sample queue.
    if ((previous && !SetTexturePlayer(context, texture, 0)) ||
        !SetTexturePlayer(context, texture, player) ||
        !InvokePointer(context, texture, kTextureUpdateResource, nullptr, 0) ||
        !SetScreenMaterial(context, binding, material)) return BindResult::BindFailed;
    return BindResult::Success;
}

bool ResolveOriginalScreen(Context& context, TelevisionBinding& binding, std::uintptr_t native, bool residue) {
    if (residue) {
        const auto& saved = context.last_native_screen;
        const auto same_handle = [](AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) {
            return a.id == b.id && a.generation == b.generation;
        };
        if (saved.world == binding.world && saved.owner == binding.owner && saved.mesh == binding.mesh &&
            saved.component == binding.component && saved.slot == binding.slot &&
            same_handle(saved.owner_handle, binding.owner_handle) &&
            same_handle(saved.mesh_handle, binding.mesh_handle) &&
            same_handle(saved.component_handle, binding.component_handle) &&
            LivePointer(context, saved.material, saved.material_handle)) {
            binding.material = saved.material;
        } else {
            if (!native || !NativeManaChain(context, binding.component)) return false;
            binding.material = native;
        }
    }
    return PinPointer(context, binding.material, binding.material_handle);
}

BindResult BindTelevision(Context& context) {
    TelevisionBinding binding;
    binding.world = context.tv_world;
    binding.owner = context.tv_owner;
    binding.owner_handle = context.tv_owner_handle;
    binding.component = context.tv_component;
    std::uintptr_t field{};
    if (!LivePointer(context, binding.owner, binding.owner_handle) ||
        !ObjectProperty(context, binding.owner, L"StaticMesh", sizeof(binding.mesh), field) ||
        !Read(context, field, binding.mesh) ||
        !PinPointer(context, binding.mesh, binding.mesh_handle) ||
        !PinPointer(context, binding.component, binding.component_handle) ||
        !OwnsComponent(context, binding.owner, binding.mesh) ||
        !OwnsComponent(context, binding.owner, binding.component)) return BindResult::ScreenUnavailable;

    const auto native = FindNativeObject(context, kNativeScreenAsset);
    const auto local = FindNativeObject(context, kLocalScreenAsset);
    if (!local) return BindResult::BindFailed;
    bool residue{};
    const auto screen = FindScreenSlot(context, binding, native, local, residue);
    if (screen != BindResult::Success) return screen;
    if (!ResolveOriginalScreen(context, binding, native, residue)) return BindResult::RecoveryFailed;
    context.last_native_screen = binding;
    const auto texture = FindNativeObject(context,
        "/Game/Movies/FFMpegUI/TVMovies/TVMediaPlayer_Video.TVMediaPlayer_Video");
    const auto result = ApplyScreenBinding(context, binding, local, texture, residue);
    if (result == BindResult::Success) Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        std::string(residue ? "TV recovered residual screen and bound local media slot=" :
                             "TV media material bound to screen slot=") + std::to_string(binding.slot));
    return result;
}

bool RestoreTelevision(Context& context, bool restore_audio = true) {
    context.media_playing.store(false);
    context.audio.Transport({}, 0, false);
    auto& binding = context.tv_binding;
    binding.audio_restore_pending = binding.audio_restore_pending || context.game_audio_muted;
    context.game_audio_muted = false;
    const bool owner_valid = binding.slot >= 0 && LivePointer(context, binding.owner, binding.owner_handle);
    if (!owner_valid) {
        const auto stale = binding;
        binding = {};
        CloseLocalMedia(context, stale);
        return true;
    }
    if (!binding.screen_restored && LivePointer(context, binding.mesh, binding.mesh_handle) &&
        LivePointer(context, binding.material, binding.material_handle) &&
        OwnsComponent(context, binding.owner, binding.mesh))
        binding.screen_restored = SetScreenMaterial(context, binding, binding.material);
    // Closing the media does not discard a failed screen/audio recovery record.
    CloseLocalMedia(context, binding);
    if (!restore_audio || !binding.audio_restore_pending) binding.audio_restored = true;
    if (!binding.audio_restored && LivePointer(context, binding.component, binding.component_handle) &&
        OwnsComponent(context, binding.owner, binding.component)) {
        const auto issue = CheckNativeAudio(context, binding.component);
        ReportAudioIssue(context, issue);
        if (issue == AudioIssue::None) {
            auto event = context.tv_audio.event;
            binding.audio_restored = InvokePointer(context, binding.component,
                "/Script/HTGame.MovieAudioPlayerComponent:PlayAudio", &event, sizeof(event));
        }
        context.native_audio_pending = binding.audio_restored ? AnomalyGenerationHandleV1{} : binding.component_handle;
    }
    const bool restored = binding.screen_restored && binding.audio_restored;
    if (restored) binding = {};
    return restored;
}

bool MediaBool(Context&, const char*, bool&);

using AudioVector = std::array<double, 3>;
enum class CameraSource { Snapshot, Reflection };

bool CameraSnapshot(Context& context, AudioVector& location, AudioVector& rotation) {
    const auto* player = Query<AnomalyNtePlayerServiceV1>(
        context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!HasField<AnomalyNtePlayerServiceV1, decltype(AnomalyNtePlayerServiceV1::camera_snapshot)>(
            player, offsetof(AnomalyNtePlayerServiceV1, camera_snapshot)) || !player->camera_snapshot) return false;
    const auto* world = Query<AnomalyUe5WorldServiceV1>(
        context.host, ANOMALY_UE5_WORLD_SERVICE_V1_ID, ANOMALY_UE5_WORLD_SERVICE_V1_VERSION);
    if (!HasField<AnomalyUe5WorldServiceV1, decltype(AnomalyUe5WorldServiceV1::snapshot)>(
            world, offsetof(AnomalyUe5WorldServiceV1, snapshot)) || !world->current || !world->snapshot) return false;
    AnomalyGenerationHandleV1 current{};
    AnomalyUe5WorldSnapshotV1 current_world{sizeof(current_world)};
    AnomalyNteCameraSnapshotV1 camera{sizeof(camera)};
    std::uint32_t world_name{};
    if (world->current(world->user, &current).code != ANOMALY_STATUS_V1_OK || !current.id ||
        world->snapshot(world->user, current, &current_world).code != ANOMALY_STATUS_V1_OK ||
        !Read(context, context.tv_binding.world + native_layout.kObjectNameOffset, world_name) ||
        world_name != current_world.name_id ||
        player->camera_snapshot(player->user, &camera).code != ANOMALY_STATUS_V1_OK ||
        !(camera.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) ||
        (camera.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) ||
        !camera.sequence || camera.world.id != current.id || camera.world.generation != current.generation) return false;
    std::ranges::copy(camera.position, location.begin());
    std::ranges::copy(camera.rotation, rotation.begin());
    return true;
}

bool IsCameraManager(Context& context, std::uintptr_t object) {
    std::uintptr_t type{};
    if (!Read(context, object + native_layout.kObjectClassOffset, type)) return false;
    for (unsigned depth = 0; type && depth < 32; ++depth) {
        if (NameEquals(context, type + native_layout.kObjectNameOffset, L"PlayerCameraManager")) return true;
        if (!Read(context, type + native_layout.kStructSuperOffset, type)) return false;
    }
    return false;
}

bool ReflectedCamera(Context& context, AudioVector& location, AudioVector& rotation) {
    struct Parameters {
        std::uintptr_t world;
        std::int32_t index{};
        std::uintptr_t manager{};
    } parameters{context.tv_binding.world};
    constexpr ReflectedParameter manager_fields[] = {
        {L"WorldContextObject", L"ObjectProperty", offsetof(Parameters, world), sizeof(Parameters::world)},
        {L"PlayerIndex", L"IntProperty", offsetof(Parameters, index), sizeof(Parameters::index)},
        {L"ReturnValue", L"ObjectProperty", offsetof(Parameters, manager), sizeof(Parameters::manager)},
    };
    const auto statics = FindNativeObject(context, "/Script/Engine.Default__GameplayStatics");
    if (!statics || !InvokePointer(context, statics, "/Script/Engine.GameplayStatics:GetPlayerCameraManager",
            &parameters, sizeof(parameters), 0, manager_fields) ||
        !parameters.manager || !IsCameraManager(context, parameters.manager)) return false;
    constexpr ReflectedParameter vector_return[]{{L"ReturnValue", L"StructProperty", 0, sizeof(AudioVector), L"Vector"}};
    constexpr ReflectedParameter rotator_return[]{{L"ReturnValue", L"StructProperty", 0, sizeof(AudioVector), L"Rotator"}};
    return InvokePointer(context, parameters.manager, "/Script/Engine.PlayerCameraManager:GetCameraLocation",
            &location, sizeof(location), 0, vector_return) &&
        InvokePointer(context, parameters.manager, "/Script/Engine.PlayerCameraManager:GetCameraRotation",
            &rotation, sizeof(rotation), 0, rotator_return);
}

std::optional<better_tv_video::AudioPosition> RelativeAudioPosition(const AudioVector& screen,
    const AudioVector& camera, const AudioVector& rotation) {
    const auto finite = [](double value) { return std::isfinite(value); };
    if (!std::ranges::all_of(screen, finite) || !std::ranges::all_of(camera, finite) ||
        !std::ranges::all_of(rotation, finite)) return std::nullopt;
    constexpr double radians = 0.017453292519943295;
    const auto pitch = rotation[0] * radians;
    const auto yaw = rotation[1] * radians;
    const auto roll = rotation[2] * radians;
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cr = std::cos(roll), sr = std::sin(roll);
    const AudioVector forward{cp * cy, cp * sy, sp};
    const AudioVector right{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    const AudioVector up{-cr * sp * cy - sr * sy, -cr * sp * sy + sr * cy, cr * cp};
    const AudioVector delta{screen[0] - camera[0], screen[1] - camera[1], screen[2] - camera[2]};
    const auto project = [&](const AudioVector& axis) {
        return static_cast<float>((delta[0] * axis[0] + delta[1] * axis[1] + delta[2] * axis[2]) / 100.0);
    };
    const better_tv_video::AudioPosition result{project(right), project(up), project(forward)};
    if (!finite(result.right) || !finite(result.up) || !finite(result.forward)) return std::nullopt;
    return result;
}

std::optional<better_tv_video::AudioPosition> TelevisionAudioPosition(Context& context) {
    const auto& binding = context.tv_binding;
    constexpr ReflectedParameter location_return[]{{L"ReturnValue", L"StructProperty", 0, sizeof(AudioVector), L"Vector"}};
    AudioVector screen{}, camera{}, rotation{};
    if (!InvokePointer(context, binding.mesh, "/Script/Engine.SceneComponent:K2_GetComponentLocation",
            &screen, sizeof(screen), 0, location_return)) return std::nullopt;
    std::optional<better_tv_video::AudioPosition> position;
    auto source = CameraSource::Snapshot;
    if (CameraSnapshot(context, camera, rotation)) position = RelativeAudioPosition(screen, camera, rotation);
    if (!position && ReflectedCamera(context, camera, rotation)) {
        position = RelativeAudioPosition(screen, camera, rotation);
        source = CameraSource::Reflection;
    }
    if (!context.logged_spatial_pose) {
        context.logged_spatial_pose = true;
        if (position) {
            char message[192]{};
            std::snprintf(message, sizeof(message), "TV spatial pose camera=%s distance_m=%.2f right_m=%.2f",
                source == CameraSource::Snapshot ? "snapshot" : "reflection",
                std::hypot(position->right, position->up, position->forward), position->right);
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
        } else {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "TV spatial pose unavailable");
        }
    }
    return position;
}

void UpdateAudio(Context& context, const std::wstring& path, std::int64_t ticks, bool playing) {
    AudioDistanceRange distance_range;
    {
        std::scoped_lock lock(context.mutex);
        distance_range = context.settings.audio_distance;
    }
    const auto source = playing ? TelevisionAudioPosition(context) : std::nullopt;
    context.audio.Transport(path, ticks, playing,
        {context.audio_session, context.audio_timeline, source, distance_range});
    const auto audio = context.audio.Read();
    if (audio.state != context.last_audio.state || audio.route != context.last_audio.route ||
        audio.spatial_error != context.last_audio.spatial_error) {
        context.last_audio = audio;
        char message[180]{};
        std::snprintf(message, sizeof(message), "TV audio state=%u route=%u error=0x%08X spatial_error=0x%08X",
            static_cast<unsigned>(audio.state), static_cast<unsigned>(audio.route), audio.error, audio.spatial_error);
        Log(context, audio.state == better_tv_video::AudioOutput::State::Failed ?
            ANOMALY_CORE_LOG_LEVEL_V1_WARNING : ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
    }
}

// UE FString: { TCHAR* data, int32 num, int32 max }.
struct PassedString final {
    const wchar_t* data{};
    std::int32_t num{};
    std::int32_t max{};
};

void MediaStatus(Context& context, const std::string& status) {
    {
        std::scoped_lock lock(context.mutex);
        if (context.media_status == status) return;
        context.media_status = status;
    }
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV media: " + status);
}

bool MediaBool(Context& context, const char* function, bool& value) {
    const auto resolved = ResolveFunction(context, "/Script/MediaAssets.MediaPlayer", function);
    std::uint8_t result{};
    std::uint32_t code = ANOMALY_STATUS_V1_UNAVAILABLE;
    if (!InvokeResolved(context, context.media_player, resolved, &result, sizeof(result), code))
        return false;
    value = result != 0;
    return true;
}

bool OpenLocalMediaFile(Context& context, const std::wstring& path, std::uint32_t& code) {
    const auto looping = ResolveFunction(context, "/Script/MediaAssets.MediaPlayer", "SetLooping");
    struct { std::uint8_t loop{1}; std::uint8_t result{}; } loop;
    static_cast<void>(InvokeResolved(context, context.media_player, looping, &loop, sizeof(loop), code));
    const auto open = ResolveFunction(context, "/Script/MediaAssets.MediaPlayer", "OpenFile");
    struct { PassedString path; std::uint8_t result{}; } parameters{
        {path.c_str(), static_cast<std::int32_t>(path.size() + 1),
         static_cast<std::int32_t>(path.size() + 1)}};
    return InvokeResolved(context, context.media_player, open, &parameters, sizeof(parameters), code) &&
        parameters.result;
}

bool RequestMediaSeek(Context& context, std::int64_t target) {
    struct Parameters { std::int64_t time; std::uint8_t accepted{}; };
    static_assert(offsetof(Parameters, accepted) == sizeof(std::int64_t));
    Parameters parameters{target};
    return InvokePointer(context, context.media_player_pointer, "/Script/MediaAssets.MediaPlayer:Seek",
        &parameters, sizeof(parameters), offsetof(Parameters, accepted) + sizeof(parameters.accepted)) &&
        parameters.accepted;
}

bool StopTakeover(Context& context, std::uintptr_t world, bool restore_audio) {
    if (context.tv_binding.slot >= 0 && world && context.tv_binding.world == world)
        return RestoreTelevision(context, restore_audio);
    const auto binding = context.tv_binding;
    context.tv_binding = {};
    context.game_audio_muted = false;
    if (!context.opened_path.empty()) CloseLocalMedia(context, binding);
    return true;
}

constexpr auto kSeekEndMargin = kTicksPerSecond / 4;

std::int64_t ClampSeekTarget(const Context& context, std::int64_t target) {
    return std::clamp(target, std::int64_t{0},
        std::max(std::int64_t{0}, context.media_duration - kSeekEndMargin));
}

void LogPlaybackSeek(Context& context, const PlaybackSeek& seek, std::string_view step,
                   std::uint32_t level = ANOMALY_CORE_LOG_LEVEL_V1_INFO) {
    Log(context, level, "TV seek #" + std::to_string(seek.id) +
        (seek.recovering ? " recovery " : " ") + std::string(step));
}

bool ValidateSeekBinding(Context& context) {
    const auto& binding = context.tv_binding;
    if (binding.slot < 0 || !binding.local_material || binding.world != context.opened_world ||
        !LivePointer(context, binding.owner, binding.owner_handle) ||
        !LivePointer(context, binding.component, binding.component_handle) ||
        !LivePointer(context, binding.mesh, binding.mesh_handle) ||
        !OwnsComponent(context, binding.owner, binding.mesh) ||
        !LivePointer(context, binding.texture, binding.texture_handle) ||
        !LivePointer(context, context.media_player_pointer, context.media_player)) return false;
    ScreenMaterialParameters screen{binding.slot};
    return InvokePointer(context, binding.mesh, "/Script/Engine.PrimitiveComponent:GetMaterial",
            &screen, sizeof(screen)) &&
        MaterialBase(context, screen.material, 0, binding.local_material) == binding.local_material;
}

bool ReadSeekTexturePlayer(Context& context, std::uintptr_t& player) {
    return ValidateSeekBinding(context) && GetTexturePlayer(context, context.tv_binding.texture, player);
}

bool SetSeekTexturePlayer(Context& context, std::uintptr_t player) {
    std::uintptr_t current{};
    if (!ReadSeekTexturePlayer(context, current) ||
        (current && current != context.media_player_pointer)) return false;
    return current == player || SetTexturePlayer(context, context.tv_binding.texture, player);
}

bool BeginSeekRebuild(Context& context, PlaybackSeek& seek) {
    seek.stage = SeekStage::Reopening;
    seek.last_position.reset();
    seek.advances = 0;
    seek.play_requested = false;
    LogPlaybackSeek(context, seek, "detach and close target_ticks=" + std::to_string(seek.target));
    return SetSeekTexturePlayer(context, 0) && CloseMediaPlayer(context);
}

void AwaitSeekClock(PlaybackSeek& seek, MediaClock::time_point now) {
    seek.stage = SeekStage::Confirming;
    seek.issued = now;
    seek.deadline = now + std::chrono::seconds(3);
}

void FailPlaybackSeek(Context& context, std::uintptr_t world, std::string_view reason) {
    auto& seek = *context.playback_seek;
    LogPlaybackSeek(context, seek, reason, ANOMALY_CORE_LOG_LEVEL_V1_WARNING);
    if (!seek.recovering) {
        seek.recovering = true;
        seek.target = ClampSeekTarget(context, seek.previous);
        MediaStatus(context, seek.automatic
            ? context.localizer.Text("media.resume_recovering", "Restarting local playback")
            : context.localizer.Text("media.seek_recovering", "Restoring the previous playback position"));
        if (BeginSeekRebuild(context, seek)) return;
        LogPlaybackSeek(context, seek, "detach or close failed", ANOMALY_CORE_LOG_LEVEL_V1_WARNING);
    }
    context.playback_seek.reset();
    StopTakeover(context, world, true);
    context.takeover_failed = true;
    MediaStatus(context, context.localizer.Text("media.seek_stopped", "Seek failed and local playback was stopped"));
}

void ConfirmAutomaticResume(Context& context, std::uintptr_t world, std::int64_t target,
                            bool accepted, MediaClock::time_point now) {
    auto& seek = context.playback_seek.emplace();
    seek.id = ++context.next_seek_id;
    seek.target = target;
    seek.automatic = true;
    AwaitSeekClock(seek, now);
    ++context.audio_timeline;
    MediaStatus(context, context.localizer.Text("media.seeking", "Seeking"));
    LogPlaybackSeek(context, seek, "resume after texture binding target_ticks=" + std::to_string(target));
    if (!accepted) FailPlaybackSeek(context, world, "resume rejected");
}

bool ConfirmSeekClock(PlaybackSeek& seek, std::int64_t position, std::int64_t duration,
                      MediaClock::time_point now) {
    constexpr auto tolerance = kTicksPerSecond / 10;
    const auto ticks_since = [now](MediaClock::time_point time) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now - time).count() / 100;
    };
    if (!seek.last_position) {
        if (position < seek.target - tolerance || position > seek.target + ticks_since(seek.issued) + tolerance)
            return false;
    } else {
        const auto budget = ticks_since(seek.last_sample) + tolerance;
        auto advance = position - *seek.last_position;
        // Allow a real loop after the first target sample, including very short clips.
        if (advance < 0 && duration - *seek.last_position <= budget && position <= budget)
            advance += duration;
        if (advance < 0 || advance > budget) {
            seek.last_position.reset();
            seek.advances = 0;
            return false;
        }
        seek.advances = advance > 0 ? seek.advances + 1 : 0;
    }
    seek.last_position = position;
    seek.last_sample = now;
    return seek.advances >= 2;
}

std::optional<SeekRequest> TakeReadySeek(Context& context, MediaClock::time_point now) {
    std::scoped_lock lock(context.mutex);
    if (!context.seek_request || context.seek_request->due > now) return std::nullopt;
    auto request = std::exchange(context.seek_request, std::nullopt);
    if (!context.media_seekable || request->generation != context.progress.generation) return std::nullopt;
    return request;
}

bool UpdatePlaybackSeek(Context& context, std::uintptr_t world, MediaClock::time_point now) {
    const auto fail = [&](std::string_view reason) {
        FailPlaybackSeek(context, world, reason);
        return true;
    };
    if (!context.playback_seek) {
        const auto request = TakeReadySeek(context, now);
        if (!request) return false;
        auto& seek = context.playback_seek.emplace();
        seek.id = ++context.next_seek_id;
        seek.target = ClampSeekTarget(context, request->target);
        seek.previous = context.resume_position;
        ++context.audio_timeline;
        UpdateAudio(context, context.opened_path, context.resume_position, false);
        MediaStatus(context, context.localizer.Text("media.seeking", "Seeking"));
        if (!BeginSeekRebuild(context, seek)) return fail("detach or close failed");
        return true;
    }
    auto& seek = *context.playback_seek;
    if (seek.stage == SeekStage::Reopening) {
        // The UE facade can discard its backend on Close. IsClosed reports false
        // with no backend, so reopen on the next Game update after Close returns.
        std::uint32_t code = ANOMALY_STATUS_V1_UNAVAILABLE;
        if (!OpenLocalMediaFile(context, context.opened_path, code))
            return fail("reopen rejected status=" + std::to_string(code));
        seek.stage = SeekStage::Opening;
        seek.deadline = now + std::chrono::seconds(5);
        LogPlaybackSeek(context, seek, "reopening");
        return true;
    }
    bool error{}, ready{}, buffering{};
    if (!MediaBool(context, "HasError", error) || error) return fail("media error");
    if (!MediaBool(context, "IsReady", ready) || !MediaBool(context, "IsBuffering", buffering))
        return fail("could not read media state");
    if (!ready || buffering) {
        seek.advances = 0;
        return now >= seek.deadline ? fail("media readiness timed out") : true;
    }
    if (seek.stage == SeekStage::Opening) {
        if (seek.target && !RequestMediaSeek(context, seek.target)) return fail("seek rejected");
        if (!SetSeekTexturePlayer(context, context.media_player_pointer)) return fail("texture binding failed");
        if (!InvokePointer(context, context.tv_binding.texture, kTextureUpdateResource, nullptr, 0))
            return fail("texture resource rebuild failed");
        AwaitSeekClock(seek, now);
        LogPlaybackSeek(context, seek, "texture resource rebuilt, waiting for playback clock");
        return true;
    }
    bool playing{};
    if (!MediaBool(context, "IsPlaying", playing)) return fail("could not read playback state");
    if (!playing && !seek.play_requested) {
        bool accepted{};
        if (!MediaBool(context, "Play", accepted) || !accepted) return fail("play rejected");
        seek.play_requested = true;
    }
    std::int64_t position{};
    const bool sampled = playing && InvokePointer(context, context.media_player_pointer,
        "/Script/MediaAssets.MediaPlayer:GetTime", &position, sizeof(position)) &&
        position >= 0 && position <= context.media_duration;
    if (!sampled) seek.advances = 0;
    if (sampled && ConfirmSeekClock(seek, position, context.media_duration, now)) {
        std::uintptr_t current{};
        if (!ReadSeekTexturePlayer(context, current) || current != context.media_player_pointer)
            return fail("texture binding changed before confirmation");
        LogPlaybackSeek(context, seek, "playback confirmed ticks=" + std::to_string(position));
        const bool recovered = seek.recovering;
        const bool automatic = seek.automatic;
        context.playback_seek.reset();
        context.resume_position = position;
        {
            std::scoped_lock lock(context.mutex);
            context.progress.position = position;
            context.progress.duration = context.media_duration;
            context.progress.seekable = context.media_seekable;
            if (!context.seek_request) context.progress.preview.reset();
        }
        // Keep the current audio backend. Its timeline changed when the seek began.
        UpdateAudio(context, context.opened_path, position, true);
        if (recovered && automatic)
            MediaStatus(context, context.localizer.Text("media.resume_recovered", "Video restarted from the beginning"));
        else
            MediaStatus(context, recovered
                ? context.localizer.Text("media.seek_recovered", "Seek failed and the previous playback position was restored")
                : context.localizer.Text("media.active", "Playing local video"));
        return true;
    }
    return now >= seek.deadline ? fail("playback clock did not advance") : true;
}

void UpdatePrepare(Context& context) noexcept;
void ResetBilibiliPrepare(Context& context) noexcept;
void DiscardPreparedSource(Context& context, const std::string& path) noexcept;

void ANOMALY_CALL UpdateMedia(void* plugin_context, double delta_seconds) noexcept {
    auto* pointer = static_cast<Context*>(plugin_context);
    if (!pointer || !pointer->alive.load(std::memory_order_acquire)) return;
    Context& context = *pointer;
    try {
        context.media_poll_seconds += delta_seconds;
        if (context.media_poll_seconds < 0.1) return;
        const double elapsed = std::exchange(context.media_poll_seconds, 0);
        // A validated prepared file enters the same source-change path as a picked
        // file, so switching pages/links clears the previous resume position below.
        UpdatePrepare(context);
        std::wstring path;
        bool enabled{};
        {
            std::scoped_lock lock(context.mutex);
            path = context.override_path;
            enabled = context.settings.enabled;
        }
        const auto world = enabled || !context.opened_path.empty() || context.native_audio_pending.id ? CurrentWorld(context) : 0;
        const bool login = world && NameEquals(context, world + native_layout.kObjectNameOffset, L"Updater_P");
        if (login || (world && world != context.tv_world)) context.native_audio_pending = {};
        if (login) {
            std::scoped_lock lock(context.mutex);
            context.settings.enabled = false;
        }
        enabled = enabled && !login;
        const bool enabled_before = context.enabled_prev;
        context.enabled_prev = enabled;
        const bool disable_transition = enabled_before && !enabled;
        const bool source_changed = path != context.selected_path;
        if (!enabled || source_changed) {
            const bool active_tv = !context.opened_path.empty() && context.tv_binding.slot >= 0 &&
                world == context.tv_binding.world && !login &&
                LivePointer(context, context.tv_binding.owner, context.tv_binding.owner_handle) &&
                TelevisionPlaying(context) && context.tv_component == context.tv_binding.component;
            if (!StopTakeover(context, login ? 0 : world, active_tv || context.tv_binding.audio_restore_pending)) {
                MediaStatus(context, context.localizer.Text("tv.restore_failed", "Could not restore the previous television; select again to retry"));
                return;
            }
            const std::wstring previous = context.selected_path;
            context.resume_position = 0;
            context.takeover_failed = false;
            if (disable_transition || login) {
                // Turning replacement off (or a login reset) cancels an active
                // preparation and reclaims the prepared artifact after the media
                // was closed above.
                ResetBilibiliPrepare(context);
                if (!previous.empty()) DiscardPreparedSource(context, WideToUtf8(previous));
                context.selected_path.clear();
            } else {
                if (source_changed && !previous.empty() && previous != path)
                    DiscardPreparedSource(context, WideToUtf8(previous));
                context.selected_path = path;
            }
            if (!enabled || login) {
                FindTelevision(context, 0, 0);
                MediaStatus(context, context.native_audio_pending.id ?
                    context.localizer.Text("media.native_audio_pending", "Original sound will resume with the television's next audio event") :
                    context.localizer.Text("media.idle", "Idle"));
                return;
            }
        }
        if (!CanInvoke(context)) return;
        const bool television = FindTelevision(context, world, elapsed);
        if (path.empty()) return;
        if (!television || (!context.opened_path.empty() && context.opened_world != world) ||
            !TelevisionPlaying(context, true) || (context.tv_binding.slot >= 0 &&
                (context.tv_binding.component != context.tv_component ||
                    !LivePointer(context, context.tv_binding.mesh, context.tv_binding.mesh_handle)))) {
            const bool scene_changed = context.opened_world && context.opened_world != world;
            StopTakeover(context, world, false);
            context.takeover_failed = false;
            if (scene_changed) {
                context.media_player = {};
                context.media_close = {};
                context.media_player_pointer = 0;
                context.media_functions.clear();
            }
            const auto waiting = context.localizer.Text("media.waiting_tv", "Waiting for an active television");
            std::scoped_lock lock(context.mutex);
            context.media_status = waiting;
            return;
        }
        if (context.takeover_failed) return;
        if (context.tv_binding.slot >= 0 && context.opened_path.empty()) {
            MediaStatus(context, context.localizer.Text("tv.restore_failed", "Could not restore the previous television; select again to retry"));
            return;
        }
        if (NeedsNativeAudio(context)) {
            StopTakeover(context, world, true);
            const auto waiting = context.localizer.Text("media.waiting_audio", "Waiting for the television audio event");
            std::scoped_lock lock(context.mutex);
            context.media_status = waiting;
            return;
        }
        if (context.media_player.id == 0) {
            if (!FindObjectHandle(context, kMediaPlayerAsset, context.media_player)) return;
        }
        if (context.media_player_pointer == 0)
            context.media_player_pointer = FindNativeObject(context, kMediaPlayerAsset);
        if (context.opened_path != path) {
            context.opened_world = world;
            context.audio.Transport({}, 0, false);
            if (path.size() >= kMaxPathChars) {
                context.opened_path = path;
                MediaStatus(context, context.localizer.Text("media.path_too_long", "Video path is too long"));
                return;
            }
            context.media_close = ResolveFunction(context, "/Script/MediaAssets.MediaPlayer", "Close").handle;
            std::uint32_t code = ANOMALY_STATUS_V1_UNAVAILABLE;
            const bool opened = OpenLocalMediaFile(context, path, code);
            context.opened_path = path;
            context.media_wait_seconds = 0;
            context.media_playing.store(false);
            context.media_probe_phase.store(opened ? 1 : 2);
            const std::string status_code = std::to_string(code);
            const std::array<std::string_view, 1> arguments{status_code};
            MediaStatus(context, opened ? context.localizer.Text("media.opening", "Opening") :
                context.localizer.Format("media.open_failed", "OpenFile rejected the local video (status={0})", arguments));
            return;
        }
        const auto now = MediaClock::now();
        if (context.media_playing.load() && UpdatePlaybackSeek(context, world, now)) return;
        const int phase = context.media_probe_phase.load();
        if (phase == 1) {
            context.media_wait_seconds += elapsed;
            bool error{}, ready{}, playing{};
            if (!MediaBool(context, "HasError", error) || error || context.media_wait_seconds >= 10.0) {
                CloseLocalMedia(context);
                context.takeover_failed = true;
                MediaStatus(context, error ? context.localizer.Text("media.decoder_error", "The media decoder reported an error") :
                    context.localizer.Text("media.timeout", "Timed out waiting for media playback"));
                return;
            }
            static_cast<void>(MediaBool(context, "IsReady", ready));
            static_cast<void>(MediaBool(context, "IsPlaying", playing));
            if (!playing) {
                bool accepted{};
                if (ready) static_cast<void>(MediaBool(context, "Play", accepted));
                return;
            }
            static_cast<void>(InvokePointer(context, context.media_player_pointer,
                "/Script/MediaAssets.MediaPlayer:GetDuration", &context.media_duration, sizeof(context.media_duration)));
            context.media_seekable = context.media_duration > 0 &&
                MediaBool(context, "SupportsSeeking", context.media_seekable) && context.media_seekable;
            const auto resume = context.media_seekable ? ClampSeekTarget(context, context.resume_position) : 0;
            const bool resume_accepted = !resume || RequestMediaSeek(context, resume);
            const auto bound = BindTelevision(context);
            if (bound != BindResult::Success) {
                StopTakeover(context, world, true);
                context.takeover_failed = true;
                switch (bound) {
                case BindResult::ScreenUnavailable:
                    MediaStatus(context, context.localizer.Text("media.screen_unavailable", "Could not identify a unique television screen slot")); break;
                case BindResult::RecoveryFailed:
                    MediaStatus(context, context.localizer.Text("media.screen_recovery_failed", "Could not restore the original television screen")); break;
                case BindResult::MuteFailed:
                    MediaStatus(context, context.localizer.Text("media.mute_failed", "Could not mute the original television audio")); break;
                default:
                    MediaStatus(context, context.localizer.Text("media.screen_bind_failed", "Could not bind local video to the television screen")); break;
                }
                return;
            }
            context.media_playing.store(true);
            context.media_probe_phase.store(2);
            if (resume) {
                ConfirmAutomaticResume(context, world, resume, resume_accepted, now);
                return;
            }
            MediaStatus(context, context.localizer.Text("media.active", "Playing local video"));
        } else if (!context.media_playing.load()) {
            return;
        }
        std::int64_t position{};
        const bool sampled = InvokePointer(context, context.media_player_pointer,
            "/Script/MediaAssets.MediaPlayer:GetTime", &position, sizeof(position)) && position >= 0;
        bool playing{};
        static_cast<void>(MediaBool(context, "IsPlaying", playing));
        if (sampled) {
            if (position + kTicksPerSecond / 10 < context.resume_position)
                ++context.audio_timeline;  // Electra wrapped to the beginning.
            context.resume_position = position;
        }
        UpdateAudio(context, path, context.resume_position, sampled && playing);
        {
            std::scoped_lock lock(context.mutex);
            context.progress.position = context.resume_position;
            context.progress.duration = context.media_duration;
            context.progress.seekable = context.media_seekable;
            if (!context.seek_request) context.progress.preview.reset();
        }
    } catch (...) {
        context.media_probe_phase.store(2);
        context.media_playing.store(false);
        context.audio.Transport({}, 0, false);
    }
}

void ANOMALY_CALL PlayByConfigDetour(void* self, void* row) noexcept {
    Context* context = g_active.load(std::memory_order_acquire);
    AnomalyGenerationHandleV1 lease{};
    bool leased = false;
    std::uintptr_t original = 0;
    try {
        if (context != nullptr && context->play_by_config.handle.id != 0 &&
            HookReady(context->hook)) {
            leased = context->hook
                         ->begin_callback(context->hook->user,
                                          context->play_by_config.handle, &lease)
                         .code == ANOMALY_STATUS_V1_OK;
            original = context->play_by_config.original;
        }
    } catch (...) {
    }
    try {
        if (leased && context != nullptr && IsTvComponent(*context, self)) {
            ObserveNativeProgram(*context, reinterpret_cast<std::uintptr_t>(self), reinterpret_cast<std::uintptr_t>(row));
            // PlayByConfig(FName) passes the FName value in the argument register.
            std::uint32_t name_id =
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(row));
            char row_name[128]{};
            if (name_id != 0) {
                std::size_t size = sizeof(row_name);
                static_cast<void>(context->names->resolve_utf8(
                    context->names->user, name_id, row_name, &size));
            }
            char message[256]{};
            std::snprintf(message, sizeof(message), "TV PlayByConfig row=%s",
                          name_id != 0 ? row_name : "?");
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
        }
    } catch (...) {
    }
    try {
        if (original != 0) {
            reinterpret_cast<PlayAudioFn>(original)(self, row);
        }
    } catch (...) {
    }
    if (leased && context != nullptr && HookReady(context->hook)) {
        static_cast<void>(context->hook->end_callback(context->hook->user, lease));
    }
}

void ANOMALY_CALL PlayAudioDetour(void* self, void* argument) noexcept {
    Context* context = g_active.load(std::memory_order_acquire);
    AnomalyGenerationHandleV1 lease{};
    bool leased = false;
    std::uintptr_t original = 0;
    try {
        if (context != nullptr && context->play_audio.handle.id != 0 &&
            HookReady(context->hook)) {
            leased = context->hook
                         ->begin_callback(context->hook->user, context->play_audio.handle,
                                          &lease)
                         .code == ANOMALY_STATUS_V1_OK;
            original = context->play_audio.original;
        }
    } catch (...) {
    }
    bool skip = false;
    AnomalyGenerationHandleV1 observed_component{};
    try {
        // Observe the actual UObject argument even while replacement is off,
        // so stopping already-playing native sound remains reversible.
        if (leased && context != nullptr && IsTvComponent(*context, self))
            observed_component = ObserveNativeAudio(*context, reinterpret_cast<std::uintptr_t>(self),
                reinterpret_cast<std::uintptr_t>(argument));
        if (leased && context != nullptr && original != 0 &&
            context->media_playing.load(std::memory_order_acquire) &&
            context->tv_binding.component == reinterpret_cast<std::uintptr_t>(self) &&
            LivePointer(*context, context->tv_binding.component, context->tv_binding.component_handle)) {
            skip = true;
            context->game_audio_muted = true;
        }
    } catch (...) {
    }
    try {
        if (!skip && original != 0) {
            reinterpret_cast<PlayAudioFn>(original)(self, argument);
            if (leased && context != nullptr && observed_component.id &&
                SameHandle(context->native_audio_pending, observed_component)) context->native_audio_pending = {};
            if (leased && context != nullptr &&
                context->tv_binding.component == reinterpret_cast<std::uintptr_t>(self))
                context->game_audio_muted = false;
        }
    } catch (...) {
    }
    if (leased && context != nullptr && HookReady(context->hook)) {
        static_cast<void>(context->hook->end_callback(context->hook->user, lease));
    }
}

// ---------------------------------------------------------------------------
// Settings and local video selection
// ---------------------------------------------------------------------------

void UpdateOverridePath(Context& context) noexcept;

AudioDistanceRange ReadAudioDistanceRange(const nlohmann::json& document) {
    const auto read = [&](const char* key, float fallback, float minimum, float maximum) {
        const auto field = document.find(key);
        if (field == document.end() || !field->is_number()) return fallback;
        const auto value = field->get<double>();
        return std::isfinite(value) && value >= minimum && value <= maximum
            ? static_cast<float>(value) : fallback;
    };
    AudioDistanceRange range;
    range.full_volume_meters = read("audioFullVolumeMeters", range.full_volume_meters,
        AudioDistanceRange::kMinFullVolumeMeters, AudioDistanceRange::kMaxFullVolumeMeters);
    range.max_distance_meters = read("audioMaxDistanceMeters", range.max_distance_meters,
        AudioDistanceRange::kMinFullVolumeMeters + AudioDistanceRange::kMinimumFadeMeters,
        AudioDistanceRange::kMaxDistanceMeters);
    if (range.max_distance_meters < range.full_volume_meters + AudioDistanceRange::kMinimumFadeMeters)
        return {};
    return range;
}

void ReadSettings(Context& context) noexcept {
    if (!ConfigReady(context.config)) return;
    try {
        std::uint32_t version{};
        std::size_t size{};
        AnomalyStatusV1 status = context.config->read(
            context.config->user, anomaly::sdk::StringView(kConfigSchemaId), &version,
            {nullptr, 0}, &size);
        if (status.code != ANOMALY_STATUS_V1_NOT_FOUND &&
            status.code != ANOMALY_STATUS_V1_OK) {
            return;
        }
        if (status.code == ANOMALY_STATUS_V1_OK && version == kConfigSchemaVersion &&
            size > 0 && size <= 64U * 1024U) {
            std::string document(size, '\0');
            std::size_t copied = document.size();
            status = context.config->read(
                context.config->user, anomaly::sdk::StringView(kConfigSchemaId), &version,
                {reinterpret_cast<std::uint8_t*>(document.data()), document.size()},
                &copied);
            if (status.code != ANOMALY_STATUS_V1_OK || copied > document.size()) return;
            document.resize(copied);
            const nlohmann::json json = nlohmann::json::parse(document, nullptr, false);
            if (!json.is_object()) return;
            Settings settings;
            settings.enabled = false; // Activation is local to this plugin session.
            settings.force_media_player = json.value("forceMediaPlayer", false);
            settings.video_path = json.value("videoPath", std::string{});
            settings.bilibili_url = json.value("bilibiliUrl", std::string{});
            settings.audio_distance = ReadAudioDistanceRange(json);
            if (settings.video_path.size() > 4000U) settings.video_path.clear();
            if (settings.bilibili_url.size() > 2000U) settings.bilibili_url.clear();
            {
                std::scoped_lock lock(context.mutex);
                context.settings = settings;
            }
            context.force_media_player.store(settings.force_media_player,
                                             std::memory_order_release);
            UpdateOverridePath(context);
        }
    } catch (...) {
    }
}

void PersistSettingsTask(void* user, AnomalyGenerationHandleV1) noexcept {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr || !context->alive.load(std::memory_order_acquire)) return;
    Settings settings;
    {
        std::scoped_lock lock(context->mutex);
        settings = context->settings;
        context->persist_task = {};
        context->settings_dirty = false;
    }
    if (!ConfigReady(context->config)) return;
    try {
        const std::string document =
            nlohmann::json{{"enabled", false},
                           {"videoPath", settings.video_path},
                           {"bilibiliUrl", settings.bilibili_url},
                           {"muteGameAudio", true},
                           {"forceMediaPlayer", settings.force_media_player},
                           {"audioFullVolumeMeters", settings.audio_distance.full_volume_meters},
                           {"audioMaxDistanceMeters", settings.audio_distance.max_distance_meters}}
                .dump();
        static_cast<void>(context->config->write_atomic(
            context->config->user, anomaly::sdk::StringView(kConfigSchemaId),
            kConfigSchemaVersion,
            {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()}));
    } catch (...) {
    }
}

void SchedulePersist(Context& context) noexcept {
    if (!SchedulerReady(context.scheduler)) return;
    std::scoped_lock lock(context.mutex);
    if (context.persist_task.id != 0) return;
    AnomalyGenerationHandleV1 task{};
    if (context.scheduler
            ->schedule(context.scheduler->user, 500, PersistSettingsTask, &context, &task)
            .code == ANOMALY_STATUS_V1_OK &&
        task.id != 0) {
        context.persist_task = task;
    }
}

void MarkSettingsDirty(Context& context) noexcept {
    {
        std::scoped_lock lock(context.mutex);
        context.settings_dirty = true;
    }
    SchedulePersist(context);
}

// Mirrors the selected video path into the memory-only override buffer. No file
// is ever written: only the in-memory FString arguments and hook results change.
void UpdateOverridePath(Context& context) noexcept {
    std::wstring wide;
    {
        std::scoped_lock lock(context.mutex);
        wide = Utf8ToWide(context.settings.video_path);
        context.override_path = wide;
    }
}

// ---------------------------------------------------------------------------
// Bilibili link preparation
// ---------------------------------------------------------------------------

bool PrepareActive(const better_tv_video::PrepareStage stage) noexcept {
    using Stage = better_tv_video::PrepareStage;
    return stage == Stage::Resolving || stage == Stage::Downloading ||
           stage == Stage::Validating;
}

// Starts the worker lazily so plugin startup never touches the network or opens
// media. Only the per-session cache root is prepared here.
bool EnsurePreparer(Context& context) noexcept {
    if (context.preparer.Started()) return true;
    if (context.preparer.Start(context.prepare_options)) return true;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
        "bilibili preparation worker failed to start");
    return false;
}

void LogPrepareResult(Context& context, const better_tv_video::PrepareSnapshot& snapshot) noexcept {
    if (context.prepare_logged == snapshot.request_id) return;
    context.prepare_logged = snapshot.request_id;
    char message[512]{};
    std::snprintf(message, sizeof(message), "bilibili preparation id=%llu stage=%d error=%d %s",
                  static_cast<unsigned long long>(snapshot.request_id),
                  static_cast<int>(snapshot.stage), static_cast<int>(snapshot.error),
                  snapshot.detail.c_str());
    Log(context, snapshot.stage == better_tv_video::PrepareStage::Ready
                     ? ANOMALY_CORE_LOG_LEVEL_V1_INFO
                     : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        message);
}

// Applies a validated prepared file once. The game update is the only consumer, so
// the prepared path enters the normal source-change path after it is ready.
void UpdatePrepare(Context& context) noexcept {
    try {
        const auto snapshot = context.preparer.Poll();
        if (snapshot.request_id == 0) return;
        {
            std::scoped_lock lock(context.mutex);
            if (snapshot.request_id != context.prepare.request_id) return;
        }
        if (snapshot.stage == better_tv_video::PrepareStage::Ready && !snapshot.path.empty()) {
            std::uint64_t applied{};
            {
                std::scoped_lock lock(context.mutex);
                applied = context.prepare.applied_id;
            }
            if (applied != snapshot.request_id) {
                if (snapshot.path.size() >= kMaxPathChars) {
                    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                        "prepared media path is too long");
                } else {
                    {
                        std::scoped_lock lock(context.mutex);
                        context.settings.video_path = snapshot.path;
                    }
                    UpdateOverridePath(context);
                    MarkSettingsDirty(context);
                }
                std::scoped_lock lock(context.mutex);
                context.prepare.applied_id = snapshot.request_id;
            }
        }
        {
            std::scoped_lock lock(context.mutex);
            if (snapshot.request_id != context.prepare.request_id) return;
            context.prepare.stage = snapshot.stage;
            context.prepare.error = snapshot.error;
            context.prepare.active = PrepareActive(snapshot.stage);
            context.prepare.received_bytes = snapshot.received_bytes;
            context.prepare.total_bytes = snapshot.total_bytes;
            context.prepare.progress = snapshot.progress;
        }
        LogPrepareResult(context, snapshot);
    } catch (...) {
    }
}

void PrepareLinkTask(void* user, AnomalyGenerationHandleV1) noexcept {
    auto& context = *static_cast<Context*>(user);
    try {
        std::optional<std::string> link;
        std::uint64_t serial{};
        {
            std::scoped_lock lock(context.mutex);
            context.prepare_task = {};
            link = std::exchange(context.prepare_intent, {});
            serial = context.prepare_serial;
        }
        if (!link || !context.alive.load()) return;
        const bool ready = EnsurePreparer(context); // Worker domain: directories and thread startup.
        std::scoped_lock lock(context.mutex);
        if (serial != context.prepare_serial) return;
        const auto id = ready ? context.preparer.Submit(*link) : 0;
        context.prepare.request_id = id;
        if (!id) {
            context.prepare.active = false;
            context.prepare.stage = better_tv_video::PrepareStage::Failed;
            context.prepare.error = better_tv_video::PrepareError::Internal;
        }
    } catch (...) {}
}

void SubmitBilibiliLink(Context& context) noexcept {
    std::string link(context.link_input.data());
    const auto first = link.find_first_not_of(" \t\r\n");
    const auto last = link.find_last_not_of(" \t\r\n");
    link = first == std::string::npos ? std::string{} : link.substr(first, last - first + 1);
    if (link.empty() || link.size() > 2000U) {
        std::scoped_lock lock(context.mutex);
        context.prepare = PrepareDisplay{};
        context.prepare.stage = better_tv_video::PrepareStage::Failed;
        context.prepare.error = better_tv_video::PrepareError::InvalidLink;
        return;
    }
    {
        std::scoped_lock lock(context.mutex);
        context.prepare_intent = link;
        ++context.prepare_serial;
        context.prepare = PrepareDisplay{};
        context.prepare.stage = better_tv_video::PrepareStage::Resolving;
        context.prepare.active = true;
        context.settings.bilibili_url = link;
        if (!context.prepare_task.id) {
            AnomalyGenerationHandleV1 task{};
            if (!SchedulerReady(context.scheduler) || context.scheduler->schedule(context.scheduler->user,
                    0, PrepareLinkTask, &context, &task).code != ANOMALY_STATUS_V1_OK) {
                context.prepare_intent.reset();
                context.prepare.active = false;
                context.prepare.stage = better_tv_video::PrepareStage::Failed;
                context.prepare.error = better_tv_video::PrepareError::Internal;
            } else context.prepare_task = task;
        }
    }
    MarkSettingsDirty(context);
}

void CancelBilibiliPrepare(Context& context) noexcept {
    context.preparer.Cancel();
    std::scoped_lock lock(context.mutex);
    // Drop late results of the cancelled request by request id; the display keeps
    // the cancellation copy until the next submit or source selection.
    context.prepare_intent.reset();
    ++context.prepare_serial;
    context.prepare.request_id = 0;
    if (!context.prepare.active) return;
    context.prepare.active = false;
    context.prepare.stage = better_tv_video::PrepareStage::Cancelled;
    context.prepare.error = better_tv_video::PrepareError::Cancelled;
}

void ResetBilibiliPrepare(Context& context) noexcept {
    context.preparer.Cancel();
    std::scoped_lock lock(context.mutex);
    context.prepare_intent.reset();
    ++context.prepare_serial;
    context.prepare = PrepareDisplay{};
}

std::string TrimmedLink(const char* text) noexcept {
    if (text == nullptr) return {};
    const std::string_view value(text);
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

// Prepared artifacts live in the per-process cache and are reclaimed when the
// process exits, so a path from a previous session is never restored as a source.
bool IsPreparedCachePath(const std::string& path) noexcept {
    if (path.empty()) return false;
    const std::wstring cache = better_tv_video::DefaultMediaCacheDirectory();
    if (cache.empty()) return false;
    const std::wstring wide = Utf8ToWide(path);
    if (wide.size() < cache.size()) return false;
    return CompareStringOrdinal(wide.data(), static_cast<int>(cache.size()), cache.data(),
                                static_cast<int>(cache.size()), TRUE) == CSTR_EQUAL;
}

// Deletes one prepared artifact after the television media has been closed and
// clears the remembered source when it pointed at that artifact. User-owned local
// files are never touched.
void DiscardPreparedSource(Context& context, const std::string& path) noexcept {
    if (path.empty() || !IsPreparedCachePath(path)) return;
    context.preparer.RequestDiscard(path);
    {
        std::scoped_lock lock(context.mutex);
        if (context.settings.video_path == path) context.settings.video_path.clear();
    }
    UpdateOverridePath(context);
    MarkSettingsDirty(context);

}

std::string PrepareReasonText(Context& context, const better_tv_video::PrepareError error) {
    using Error = better_tv_video::PrepareError;
    switch (error) {
    case Error::InvalidLink:
        return context.localizer.Text("prepare.reason.invalid_link",
            "The link is not a supported normal submission");
    case Error::Network:
        return context.localizer.Text("prepare.reason.network",
            "The bilibili service could not be reached");
    case Error::Api:
        return context.localizer.Text("prepare.reason.api",
            "The bilibili API rejected the page");
    case Error::NoPlayableStream:
        return context.localizer.Text("prepare.reason.no_playable_stream",
            "No compatible muxed H.264/AAC stream is available");
    case Error::SegmentedStream:
        return context.localizer.Text("prepare.reason.segmented",
            "Segmented videos are not supported");
    case Error::Download:
        return context.localizer.Text("prepare.reason.download",
            "The video could not be downloaded");
    case Error::InvalidMedia:
        return context.localizer.Text("prepare.reason.invalid_media",
            "The downloaded file did not pass validation");
    case Error::Oversize:
        return context.localizer.Text("prepare.reason.oversize",
            "The video exceeds the 512 MiB limit");
    case Error::Budget:
        return context.localizer.Text("prepare.reason.budget",
            "The media cache exceeds its 2 GiB budget");
    case Error::Cancelled:
        return context.localizer.Text("prepare.cancelled", "Cancelled");
    case Error::Internal:
        return context.localizer.Text("prepare.reason.internal",
            "Internal preparation error");
    default:
        return {};
    }
}

std::string FormatMebibytes(const std::uint64_t bytes) {
    constexpr std::uint64_t kMebibyte = 1024ull * 1024ull;
    return std::to_string((bytes + kMebibyte / 2) / kMebibyte);
}

// Text for the existing playback status row. Ready entries only show while
// replacement is off; with replacement on the normal playback status takes over
// once the prepared file is applied.
std::string PrepareStatusText(Context& context) {
    PrepareDisplay display;
    bool enabled{};
    {
        std::scoped_lock lock(context.mutex);
        display = context.prepare;
        enabled = context.settings.enabled;
    }
    using Stage = better_tv_video::PrepareStage;
    switch (display.stage) {
    case Stage::Resolving:
        return context.localizer.Text("prepare.resolving", "Resolving the bilibili link");
    case Stage::Downloading: {
        const int percent = display.total_bytes > 0
                                ? static_cast<int>(display.progress * 100.0F + 0.5F)
                                : 0;
        const std::string percent_text = std::to_string(percent);
        const std::string received = FormatMebibytes(display.received_bytes);
        const std::string total = FormatMebibytes(display.total_bytes);
        const std::array<std::string_view, 3> arguments{percent_text, received, total};
        return context.localizer.Format(
            "prepare.downloading",
            "Downloading the video {0}% ({1} MiB / {2} MiB)", arguments);
    }
    case Stage::Validating:
        return context.localizer.Text("prepare.validating", "Validating the video file");
    case Stage::Failed: {
        const std::string reason = PrepareReasonText(context, display.error);
        const std::array<std::string_view, 1> arguments{reason};
        return context.localizer.Format("prepare.failed", "Preparation failed: {0}",
                                        arguments);
    }
    case Stage::Cancelled:
        return context.localizer.Text("prepare.cancelled", "Cancelled");
    case Stage::Ready:
        return enabled ? std::string{}
                       : context.localizer.Text("prepare.ready_disabled",
                             "Ready; enable replacement to play");
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void Text(const AnomalyUiServiceV1* ui, const std::string& text) noexcept {
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

bool Button(const AnomalyUiServiceV1* ui, const std::string& label, const bool enabled) noexcept {
    return ui->button_enabled(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F,
                              enabled ? 1 : 0) != 0;
}

std::string HookStatusText(Context& context) noexcept {
    if (context.play_by_config.handle.id && context.play_audio.handle.id)
        return context.localizer.Text("status.hooks_installed", "Hooks: installed");
    return context.localizer.Text("status.hooks_missing", "Hooks: TV playback hooks unavailable");
}

std::string FormatTime(std::int64_t ticks) {
    const auto seconds = ticks / kTicksPerSecond;
    char time[32]{};
    std::snprintf(time, sizeof(time), "%lld:%02lld:%02lld",
        static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60),
        static_cast<long long>(seconds % 60));
    return time;
}

void DrawProgress(Context& context, const AnomalyUiServiceV1* ui) {
    PlaybackProgress progress;
    {
        std::scoped_lock lock(context.mutex);
        progress = context.progress;
    }
    const bool seekable = progress.seekable && progress.duration > 0;
    const auto position = progress.preview.value_or(progress.position);
    float value = progress.duration > 0 ? static_cast<float>(
        std::clamp(static_cast<double>(position) / static_cast<double>(progress.duration), 0.0, 1.0)) : 0.0f;
    const auto label = context.localizer.Label("progress.slider", "Video progress", "tv-video-seek");
    int changed{};
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::slider_float_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, slider_float_enabled)) && ui->slider_float_enabled)
        changed = ui->slider_float_enabled(ui->user, anomaly::sdk::StringView(label), &value, 0.0f, 1.0f, seekable);
    else if (seekable && ui->slider_float)
        changed = ui->slider_float(ui->user, anomaly::sdk::StringView(label), &value, 0.0f, 1.0f);
    auto displayed_position = position;
    if (changed && seekable && std::isfinite(value)) {
        const auto ratio = std::clamp(static_cast<long double>(value), 0.0L, 1.0L);
        const auto target = ratio >= 1 ? progress.duration - 1 :
            static_cast<std::int64_t>(ratio * progress.duration);
        std::scoped_lock lock(context.mutex);
        if (context.progress.generation == progress.generation && context.progress.seekable && context.settings.enabled) {
            context.seek_request = SeekRequest{progress.generation, target, MediaClock::now() + std::chrono::milliseconds(200)};
            context.progress.preview = target;
            displayed_position = target;
        }
    }
    const auto current = FormatTime(displayed_position);
    const auto total = progress.duration > 0 ? FormatTime(progress.duration) : std::string("--:--:--");
    const std::array<std::string_view, 2> times{current, total};
    Text(ui, context.localizer.Format("progress.time", "{0} / {1}", times));
}

// Read-only download progress. The host owns no progress-bar widget, so reuse the
// existing disabled slider when the append-only API is present and fall back to an
// ASCII bar on older runtimes.
void DrawPrepareProgress(Context& context, const AnomalyUiServiceV1* ui) {
    PrepareDisplay display;
    {
        std::scoped_lock lock(context.mutex);
        display = context.prepare;
    }
    if (display.stage != better_tv_video::PrepareStage::Downloading) return;
    const float fraction = display.total_bytes > 0
                               ? std::clamp(display.progress, 0.0F, 1.0F)
                               : 0.0F;
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::slider_float_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, slider_float_enabled)) &&
        ui->slider_float_enabled != nullptr) {
        float value = fraction * 100.0F;
        const auto label = context.localizer.Label(
            "prepare.progress_label", "Download progress", "tv-video-prepare-progress");
        static_cast<void>(ui->slider_float_enabled(
            ui->user, anomaly::sdk::StringView(label), &value, 0.0F, 100.0F, 0));
        return;
    }
    constexpr int kCells = 20;
    const int filled = static_cast<int>(fraction * static_cast<float>(kCells) + 0.5F);
    std::string bar("[");
    bar.append(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(kCells - filled), '-');
    bar.push_back(']');
    Text(ui, bar);
}

void DrawAudioDistanceSettings(Context& context, const AnomalyUiServiceV1* ui, AudioDistanceRange range) {
    if (!ui->slider_float) return;
    const auto full_label = context.localizer.Label("audio.full_volume_distance",
        "Full-volume distance (m)", "tv-audio-full-volume");
    const auto max_label = context.localizer.Label("audio.max_distance",
        "Mute distance (m)", "tv-audio-max-distance");
    constexpr auto gap = AudioDistanceRange::kMinimumFadeMeters;
    // The host also accepts typed input, which may exceed the slider bounds.
    const auto draw_slider = [&](const std::string& label, float value, float minimum, float maximum) {
        float edited = value;
        if (!ui->slider_float(ui->user, anomaly::sdk::StringView(label), &edited, minimum, maximum) ||
            !std::isfinite(edited)) return value;
        return std::clamp(edited, minimum, maximum);
    };
    const AudioDistanceRange before = range;
    range.full_volume_meters = draw_slider(full_label, range.full_volume_meters,
        AudioDistanceRange::kMinFullVolumeMeters,
        std::min(AudioDistanceRange::kMaxFullVolumeMeters, range.max_distance_meters - gap));
    range.max_distance_meters = draw_slider(max_label, range.max_distance_meters,
        range.full_volume_meters + gap, AudioDistanceRange::kMaxDistanceMeters);
    if (range.full_volume_meters != before.full_volume_meters ||
        range.max_distance_meters != before.max_distance_meters) {
        {
            std::scoped_lock lock(context.mutex);
            context.settings.audio_distance = range;
        }
        MarkSettingsDirty(context);
    }
}

void DrawTelevisions(Context& context, const AnomalyUiServiceV1* ui) {
    std::vector<TelevisionCandidate> rows;
    AnomalyGenerationHandleV1 selected{};
    bool failed{}, enabled{};
    {
        std::scoped_lock lock(context.mutex);
        rows = context.television_display;
        selected = context.displayed_selection;
        failed = context.television_discovery_failed;
        enabled = context.settings.enabled;
    }
    if (!enabled) return;
    const auto refresh = context.localizer.Text("tv.refresh", "Refresh televisions");
    if (ui->button(ui->user, anomaly::sdk::StringView(refresh), 0, 0)) {
        std::scoped_lock lock(context.mutex);
        context.refresh_televisions = true;
    }
    if (failed) Text(ui, context.localizer.Text("tv.discovery_failed", "Television discovery unavailable; check the compatibility profile"));
    else if (rows.empty()) Text(ui, context.localizer.Text("tv.empty", "No loaded residence televisions"));
    else if (!selected.id) Text(ui, context.localizer.Text("tv.choose", "Select a television below"));
    for (const auto& row : rows) {
        const auto identity = std::to_string(row.owner_handle.id);
        const auto state = (row.playing ? context.localizer.Text("tv.playing", "Playing") :
            context.localizer.Text("tv.off", "Off or unavailable"));
        const std::array<std::string_view, 3> args{row.name, identity, state};
        Text(ui, context.localizer.Format("tv.row", "{0} [{1}] - {2}", args));
        const auto label = (SameHandle(selected, row.owner_handle) ? context.localizer.Text("tv.selected", "Selected") :
            context.localizer.Text("tv.select", "Select")) + "##tv" + identity;
        if (ui->button(ui->user, anomaly::sdk::StringView(label), 0, 0)) {
            std::scoped_lock lock(context.mutex);
            context.television_request = row;
        }
    }
}

void Draw(void* plugin_context, const AnomalyUiServiceV1* ui_v1) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || ui_v1 == nullptr || context->ui == nullptr) return;
    const std::string title = context->localizer.Text("window.title", "Better TV");
    anomaly::sdk::UiWindow window(context->ui, title);
    if (!window) return;

    {
        const auto picked = context->picker.TakeResult();
        if (picked.path) {
            {
                std::scoped_lock lock(context->mutex);
                context->settings.video_path = WideToUtf8(*picked.path);
            }
            UpdateOverridePath(*context);
            MarkSettingsDirty(*context);
            ResetBilibiliPrepare(*context);
        }
    }

    Settings ui_settings;
    {
        std::scoped_lock lock(context->mutex);
        ui_settings = context->settings;
    }
    const std::string enabled_label =
        context->localizer.Text("option.enabled", "Replace the residence TV video");
    int enabled = ui_settings.enabled ? 1 : 0;
    static_cast<void>(ui_v1->checkbox(ui_v1->user,
                                      anomaly::sdk::StringView(enabled_label), &enabled));
    if ((enabled != 0) != ui_settings.enabled) {
        ui_settings.enabled = enabled != 0;
        {
            std::scoped_lock lock(context->mutex);
            context->settings.enabled = ui_settings.enabled;
        }
        MarkSettingsDirty(*context);
    }
    DrawTelevisions(*context, ui_v1);
    Text(ui_v1, HookStatusText(*context));
    std::string playback_status;
    {
        std::scoped_lock lock(context->mutex);
        playback_status = context->media_status;
    }
    const std::string prepare_status = PrepareStatusText(*context);
    if (!prepare_status.empty()) playback_status = prepare_status;
    const std::array<std::string_view, 1> playback_args{playback_status};
    Text(ui_v1, context->localizer.Format("text.playback", "Playback: {0}", playback_args));
    DrawProgress(*context, ui_v1);
    DrawPrepareProgress(*context, ui_v1);
    const auto audio = context->audio.Read();
    std::string audio_text;
    using AudioState = better_tv_video::AudioOutput::State;
    switch (audio.state) {
    case AudioState::Idle: audio_text = context->localizer.Text("audio.idle", "Idle"); break;
    case AudioState::Opening: audio_text = context->localizer.Text("audio.opening", "Opening"); break;
    case AudioState::Playing: audio_text = context->localizer.Text("audio.playing", "Playing"); break;
    case AudioState::Paused: audio_text = context->localizer.Text("audio.paused", "Paused"); break;
    case AudioState::NoAudio: audio_text = context->localizer.Text("audio.no_track", "No audio track"); break;
    case AudioState::Failed: {
        char error[16]{};
        std::snprintf(error, sizeof(error), "0x%08X", audio.error);
        const std::array<std::string_view, 1> arguments{error};
        audio_text = context->localizer.Format("audio.failed", "Failed ({0})", arguments);
        break;
    }
    }
    const std::array<std::string_view, 1> audio_args{audio_text};
    Text(ui_v1, context->localizer.Format("text.audio", "Audio: {0}", audio_args));
    using AudioRoute = better_tv_video::AudioOutput::Route;
    if (audio.route != AudioRoute::None && audio.state != AudioState::Failed) {
        const auto route = audio.route == AudioRoute::Spatial
            ? context->localizer.Text("audio.route_spatial", "External 3D audio")
            : context->localizer.Text("audio.route_external", "Standard external audio");
        const std::array<std::string_view, 1> args{route};
        Text(ui_v1, context->localizer.Format("text.audio_route", "Audio output: {0}", args));
    }
    DrawAudioDistanceSettings(*context, ui_v1, ui_settings.audio_distance);

    {
        std::string path;
        {
            std::scoped_lock lock(context->mutex);
            path = ui_settings.video_path;
        }
        if (path.empty()) path = context->localizer.Text("text.none", "(none)");
        const std::array<std::string_view, 1> args{path};
        Text(ui_v1, context->localizer.Format("text.path", "Video file: {0}", args));
    }

    const std::string browse = context->localizer.Label("action.browse", "Choose video...",
                                                        "tv-video-browse");
    if (Button(ui_v1, browse, true)) {
        context->picker.Request();
    }

    if (!context->link_input_initialized) {
        const auto& restored = ui_settings.bilibili_url;
        const auto count = std::min(restored.size(), context->link_input.size() - 1);
        if (count > 0) std::memcpy(context->link_input.data(), restored.data(), count);
        context->link_input[count] = '\0';
        context->link_input_initialized = true;
    }
    const std::string link_label = context->localizer.Label(
        "bilibili.link", "Bilibili link", "tv-video-bilibili-link");
    if (ui_v1->input_text != nullptr) {
        if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::set_next_item_width)>(
                ui_v1, offsetof(AnomalyUiServiceV1, set_next_item_width)) &&
            ui_v1->set_next_item_width != nullptr)
            ui_v1->set_next_item_width(ui_v1->user, -1.0F);
        static_cast<void>(ui_v1->input_text(
            ui_v1->user, anomaly::sdk::StringView(link_label), context->link_input.data(),
            context->link_input.size(), 0));
    }
    bool prepare_active{};
    {
        std::scoped_lock lock(context->mutex);
        prepare_active = context->prepare.active;
    }
    const std::string edited_link = TrimmedLink(context->link_input.data());
    // A new link submitted while a task runs replaces it; the same button cancels
    // the active task when the input still holds the running link.
    const bool submit_new =
        prepare_active && !edited_link.empty() && edited_link != ui_settings.bilibili_url;
    const std::string action =
        prepare_active && !submit_new
            ? context->localizer.Label("action.cancel", "Cancel", "tv-video-bilibili-cancel")
            : context->localizer.Label("action.parse", "Parse", "tv-video-bilibili-parse");
    if (Button(ui_v1, action, true)) {
        if (prepare_active && !submit_new) CancelBilibiliPrepare(*context);
        else SubmitBilibiliLink(*context);
    }
    const std::array<std::string_view, 1> hits{
        std::to_string(context->tv_hits.load(std::memory_order_relaxed))};
    Text(ui_v1, context->localizer.Format("text.tv_hits", "TV captures: {0}", hits));
}

// ---------------------------------------------------------------------------
// Plugin entry
// ---------------------------------------------------------------------------

AnomalyStatusV1 Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr)
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *plugin_context = nullptr;
    if (!better_tv_profile::LoadCompatibility(host))
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "compatibility profile rejected");
    auto* context = new (std::nothrow) Context();
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);
    context->host = host;
    context->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID,
                                                ANOMALY_CORE_SERVICE_V1_VERSION);
    context->signatures = Query<AnomalySignatureServiceV1>(
        host, ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    context->hook = Query<AnomalyHookServiceV1>(host, ANOMALY_HOOK_SERVICE_V1_ID,
                                                ANOMALY_HOOK_SERVICE_V1_VERSION);
    context->patch = Query<AnomalyPatchServiceV1>(host, ANOMALY_PATCH_SERVICE_V1_ID, 1);
    context->names = Query<AnomalyUe5NamesServiceV1>(host, ANOMALY_UE5_NAMES_SERVICE_V1_ID,
                                                     1);
    context->objects = Query<AnomalyUe5ObjectsServiceV1>(host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
                                                         ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
    context->process_events = Query<AnomalyUe5ProcessEventServiceV1>(
        host, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID,
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION);
    context->config = Query<AnomalyConfigServiceV1>(host, ANOMALY_CONFIG_SERVICE_V1_ID,
                                                    ANOMALY_CONFIG_SERVICE_V1_VERSION);
    context->scheduler = Query<AnomalySchedulerServiceV1>(
        host, ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    context->ui = Query<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID,
                                            ANOMALY_UI_SERVICE_V1_VERSION);
    context->localizer = anomaly::sdk::Localizer(host);
    if (!CoreReady(context->core) || !SignatureReady(context->signatures) ||
        !HookReady(context->hook) || !NamesReady(context->names) ||
        !ConfigReady(context->config) || !SchedulerReady(context->scheduler) ||
        context->ui == nullptr) {
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                      "core, signature, hook, names, config, scheduler and ui services are required");
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 Start(void* plugin_context) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->play_by_config = HookSlot{};
    context->play_audio = HookSlot{};
    context->media_probe_phase.store(0, std::memory_order_release);
    context->media_probe_attempts.store(0, std::memory_order_release);
    context->logged_play_missing.store(false, std::memory_order_release);
    context->logged_audio_missing.store(false, std::memory_order_release);
    context->settings = Settings{};
    context->enabled_prev = false;
    context->prepare = PrepareDisplay{};
    context->prepare_logged = 0;
    context->media_status = context->localizer.Text("media.idle", "Idle");
    context->alive.store(true, std::memory_order_release);
    {
        AnomalyGenerationHandleV1 schema{};
        const AnomalyByteSpanV1 document{
            reinterpret_cast<const std::uint8_t*>(kConfigSchemaJson.data()),
            kConfigSchemaJson.size()};
        if (context->config->register_schema(
                context->config->user, anomaly::sdk::StringView(kConfigSchemaId),
                kConfigSchemaVersion, document, &schema)
                .code == ANOMALY_STATUS_V1_OK) {
            context->config_schema = schema;
        }
    }
    ReadSettings(*context);
    if (!context->picker.Start(
            Utf8ToWide(context->localizer.Text("picker.video_files", "Video files")),
            Utf8ToWide(context->localizer.Text("picker.all_files", "All files"))))
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "file picker STA unavailable");
    {
        // The media cache is per process: a prepared artifact from a previous
        // session no longer exists, so only a user-owned local path is restored.
        if (IsPreparedCachePath(context->settings.video_path))
            context->settings.video_path.clear();
        // Restore only the input content: startup must not resolve or fetch it.
        const auto& restored = context->settings.bilibili_url;
        const auto count = std::min(restored.size(), context->link_input.size() - 1);
        if (count > 0) std::memcpy(context->link_input.data(), restored.data(), count);
        context->link_input[count] = '\0';
        context->link_input_initialized = true;
    }
    // Cache reclamation and the worker setup belong to the lifecycle thread, never
    // to a render callback. No network or media is touched here.
    context->prepare_options.cache_directory = better_tv_video::DefaultMediaCacheDirectory();
    if (!EnsurePreparer(*context))
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "bilibili preparation worker is unavailable this session");
    {
        char message[320]{};
        std::snprintf(message, sizeof(message),
                      "TV video settings enabled=%d mediaPlayer=%d videoPath=%s",
                      context->settings.enabled ? 1 : 0,
                      context->settings.force_media_player ? 1 : 0,
                      context->settings.video_path.c_str());
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
    }
    g_active.store(context, std::memory_order_release);
    EnsureHooks(*context);
    UpdateOverridePath(*context);
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV video started");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 Stop(void* plugin_context, std::uint32_t deadline_ms) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return anomaly::sdk::Ok();
    context->alive.store(false, std::memory_order_release);
    g_active.store(nullptr, std::memory_order_release);
    context->persist_task = {};
    const auto started = MediaClock::now();
    const auto remaining_time = [&] {
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(MediaClock::now() - started);
        return spent.count() >= deadline_ms ? 0U : deadline_ms - static_cast<std::uint32_t>(spent.count());
    };
    if (!context->picker.Stop(std::chrono::milliseconds(remaining_time())))
        return Status(ANOMALY_STATUS_V1_TIMEOUT, "file picker did not stop in time");
    bool prepare_stopped = true;
    if (context->preparer.Started()) {
        prepare_stopped = context->preparer.Stop(remaining_time());
        // A player may still hold the current artifact; leftovers are reclaimed by
        // the next Start.
        if (prepare_stopped) context->preparer.DiscardAll();
    }
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        MediaClock::now() - started);
    const auto remaining = spent.count() >= deadline_ms
        ? 0U
        : static_cast<std::uint32_t>(deadline_ms - spent.count());
    const bool audio_stopped = context->audio.Shutdown(std::chrono::milliseconds(remaining));
    if (!prepare_stopped)
        return Status(ANOMALY_STATUS_V1_TIMEOUT, "bilibili preparation worker did not stop in time");
    if (!audio_stopped)
        return Status(ANOMALY_STATUS_V1_TIMEOUT, "television audio worker did not stop in time");
    return anomaly::sdk::Ok();
}

void Unload(void* plugin_context) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    delete context;
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {sizeof(*descriptor),
                   ANOMALY_PLUGIN_API_V1_MAJOR,
                   ANOMALY_PLUGIN_API_V1_MINOR,
                   anomaly::sdk::StringView("shikari.better-tv"),
                   anomaly::sdk::StringView("Better TV"),
                   anomaly::sdk::StringView("Shikari"),
                   anomaly::sdk::StringView("0.3.0"),
                   Load,
                   Start,
                   Stop,
                   Unload,
                   UpdateMedia,
                   Draw,
                   nullptr};
    return anomaly::sdk::Ok();
}
