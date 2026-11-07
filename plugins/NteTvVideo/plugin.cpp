// NTE TV Video: replaces the residence TV video source with a local video.
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
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui.h"
#include "tv_video_profile.hpp"
#include "audio_output.hpp"

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
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <map>

#include <nlohmann/json.hpp>

namespace {

using nte_tv_video_profile::native_layout;

// Residence TV actor. The plugin compares the component owner's class name, so
// cinema/story screens keep their game videos.
constexpr std::wstring_view kTvActorClassName = L"BP_FurnitureActorBase_TV_C";
// Blueprint function that selects the TV video pipeline (see IsManaPlayerFunction).
constexpr std::size_t kMaxPathChars = 512;
constexpr std::size_t kMaxNameBytes = 256;

constexpr std::string_view kConfigSchemaId = "anomaly.nte-tv-video.settings";
constexpr std::uint32_t kConfigSchemaVersion = 1;
constexpr std::string_view kConfigSchemaJson =
    R"({"type":"object","properties":{"enabled":{"type":"boolean"},)"
    R"("videoPath":{"type":"string","maxLength":4096},)"
    R"("muteGameAudio":{"type":"boolean"},)"
    R"("forceMediaPlayer":{"type":"boolean"}},)"
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
};

// The file dialog runs on its own thread; this state remains valid after the
// plugin context is gone, so the thread never touches a destroyed context.
struct PickerState final {
    std::mutex mutex;
    bool busy{};
    bool done{};
    std::string path;
};

struct TelevisionBinding final {
    std::uintptr_t world{}, owner{}, component{}, mesh{}, material{};
    AnomalyGenerationHandleV1 owner_handle{}, component_handle{}, mesh_handle{}, material_handle{};
    std::int32_t slot{-1};
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

struct Context final {
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
    // The user's video path, applied in memory only. Nothing is ever written to
    // the game directory: the plugin overrides strings and hook results.
    std::wstring override_path;
    std::shared_ptr<PickerState> picker;

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
    std::optional<SeekRequest> seeking; // Game thread, due is the completion deadline.
    std::int64_t media_duration{};
    bool media_seekable{};
    bool seek_failed{};
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
    AnomalyGenerationHandleV1 native_audio_pending{};
    AudioIssue last_audio_issue{AudioIssue::None};
    AudioObservation last_audio_observation{AudioObservation::Valid};
    bool game_audio_muted{};
    std::uintptr_t media_player_pointer{};
    AnomalyGenerationHandleV1 native_audio_patch{};
    std::map<std::string, std::pair<AnomalyGenerationHandleV1, std::uint16_t>> media_functions;
    std::uintptr_t media_sound{};
    bool media_sound_ready{};
    nte_tv_video::AudioOutput audio;
    nte_tv_video::AudioOutput::State last_audio_state{nte_tv_video::AudioOutput::State::Idle};
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

bool IsTvComponent(Context& context, void* self) noexcept {
    const auto component = reinterpret_cast<std::uintptr_t>(self);
    if (component == 0 || native_layout.kObjectOuterOffset == 0) return false;
    std::uintptr_t owner{};
    if (!Read(context, component + native_layout.kObjectOuterOffset, owner) || owner == 0)
        return false;
    std::uintptr_t type{};
    if (!Read(context, owner + native_layout.kObjectClassOffset, type) || type == 0)
        return false;
    return NameEquals(context, type + native_layout.kObjectNameOffset, kTvActorClassName);
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
    target = nte_tv_video_profile::Address(symbol);
    return target != 0 && nte_tv_video_profile::compatibility.MatchesEntry(
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
    const auto find = nte_tv_video_profile::Address("staticFindObject");
    if (!find) return 0;
    const auto wide = Utf8ToWide(path);
    const auto pointer = FindNativeUnchecked(find, wide.c_str());
    AnomalyGenerationHandleV1 actual{};
    return PinPointer(context, pointer, actual) && actual.id == expected.id &&
        actual.generation == expected.generation ? pointer : 0;
}

bool InvokePointer(Context& context, std::uintptr_t receiver, const char* path,
                   void* parameters, std::size_t size, std::size_t unpadded_size = 0) {
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
            !matches_size(parms_size) || !PinPointer(context, function, handle)) {
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
    const auto instruction = nte_tv_video_profile::Address("worldAccessor");
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

bool FindTelevision(Context& context, std::uintptr_t world, double elapsed) {
    if (!world) return false;
    AnomalyUe5ObjectSnapshotV1 current{sizeof(current)};
    if (context.tv_owner && context.tv_world == world &&
        context.objects->snapshot_by_handle(context.objects->user, context.tv_owner_handle, &current).code == 0)
        return true;
    context.tv_owner = 0;
    context.tv_component = 0;
    context.tv_discovery_seconds += elapsed;
    if (context.tv_discovery_seconds < 1.0) return false;
    context.tv_discovery_seconds = 0;
    const auto actor_class = FindNativeObject(context,
        "/Game/Blueprints/Level/World/Common/Room/BP_FurnitureActorBase_TV.BP_FurnitureActorBase_TV_C");
    const auto statics = FindNativeObject(context, "/Script/Engine.Default__GameplayStatics");
    if (!actor_class || !statics) return false;
    struct { std::uintptr_t world, type, result{}; } parameters{world, actor_class};
    if (InvokePointer(context, statics, "/Script/Engine.GameplayStatics:GetActorOfClass",
            &parameters, sizeof(parameters)) && parameters.result &&
        PinPointer(context, parameters.result, context.tv_owner_handle)) {
        context.tv_owner = parameters.result;
        context.tv_world = world;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV actor found in the active world");
        return true;
    }
    return false;
}

bool TelevisionPlaying(Context& context) {
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
    return playing != 0;
}

void CloseLocalMedia(Context& context) {
    context.media_playing.store(false);
    context.audio.Transport({}, 0, false);
    if (!context.opened_path.empty() && context.media_close.id && context.media_player.id && context.process_events)
        static_cast<void>(context.process_events->invoke(context.process_events->user,
            context.media_player, context.media_close, nullptr, 0));
    context.opened_path.clear();
    context.opened_world = 0;
    context.media_probe_phase.store(2);
    context.seeking.reset();
    context.media_duration = 0;
    context.media_seekable = false;
    context.seek_failed = false;
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
    if (!AudioComponentRelevant(context, component)) return;
    AnomalyGenerationHandleV1 handle{};
    if (!PinPointer(context, component, handle)) return;
    const auto& audio = context.tv_audio;
    if (program && audio.component == component && SameHandle(audio.component_handle, handle) && audio.program == program)
        return;
    // PlayByConfig can synchronously call PlayAudio. Publish the new program first.
    context.tv_audio = {component, 0, handle, {}, program};
}

AnomalyGenerationHandleV1 ObserveNativeAudio(Context& context, std::uintptr_t component, std::uintptr_t event) {
    if (!AudioComponentRelevant(context, component)) return {};
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
    const auto& old = context.tv_audio;
    const auto program = old.component == component && SameHandle(old.component_handle, component_handle) ? old.program : 0;
    context.tv_audio = {component, event, component_handle, event_handle, program};
    return component_handle;
}

enum class BindResult { Success, ScreenUnavailable, RecoveryFailed, MuteFailed, BindFailed };

constexpr auto kNativeScreenAsset =
    "/Game/Movies/FFMpegUI/TVMovies/TVManaPlayer_Texture_Mat.TVManaPlayer_Texture_Mat";
constexpr auto kLocalScreenAsset =
    "/Game/Movies/FFMpegUI/TVMovies/TVMediaPlayer_Video_Mat.TVMediaPlayer_Video_Mat";
constexpr auto kSetScreenMaterial = "/Script/Engine.PrimitiveComponent:SetMaterial";

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
    auto player = context.media_player_pointer;
    if (!texture || !player || !InvokePointer(context, texture,
            "/Script/MediaAssets.MediaTexture:SetMediaPlayer", &player, sizeof(player)))
        return BindResult::BindFailed;
    if (!InvokePointer(context, binding.component,
            "/Script/HTGame.MovieAudioPlayerComponent:StopAudio", nullptr, 0)) return BindResult::MuteFailed;
    context.game_audio_muted = true;
    context.tv_binding = binding;
    if (!SetScreenMaterial(context, binding, material)) return BindResult::BindFailed;
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

void RestoreTelevision(Context& context, bool restore_audio = true) {
    // Release interception before any engine call can re-enter the audio hook.
    context.media_playing.store(false);
    context.audio.Transport({}, 0, false);
    const auto binding = context.tv_binding;
    const bool muted = context.game_audio_muted;
    context.tv_binding = {};
    context.game_audio_muted = false;
    const bool owner_valid = binding.slot >= 0 && LivePointer(context, binding.owner, binding.owner_handle);
    bool material = false;
    if (owner_valid && LivePointer(context, binding.mesh, binding.mesh_handle) &&
        LivePointer(context, binding.material, binding.material_handle) &&
        OwnsComponent(context, binding.owner, binding.mesh)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV restore: restoring screen material");
        material = SetScreenMaterial(context, binding, binding.material);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV restore: screen material call returned");
    }
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV restore: closing local media");
    CloseLocalMedia(context);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV restore: local media close returned");
    bool audio = !muted || !restore_audio;
    if (owner_valid && muted && restore_audio && LivePointer(context, binding.component, binding.component_handle) &&
        OwnsComponent(context, binding.owner, binding.component)) {
        const auto issue = CheckNativeAudio(context, binding.component);
        ReportAudioIssue(context, issue);
        if (issue == AudioIssue::None) {
            auto event = context.tv_audio.event;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV restore: restoring native audio event");
            audio = InvokePointer(context, binding.component,
                "/Script/HTGame.MovieAudioPlayerComponent:PlayAudio", &event, sizeof(event));
        }
        context.native_audio_pending = audio ? AnomalyGenerationHandleV1{} : binding.component_handle;
    }
    Log(context, material && audio ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        "TV restore complete: material=" + std::to_string(material) + " audio=" + std::to_string(audio));
}

bool MediaBool(Context&, const char*, bool&);
void UpdateAudio(Context& context, const std::wstring& path, std::int64_t ticks, bool playing) {
    context.audio.Transport(path, ticks, playing);
    const auto audio = context.audio.Read();
    if (audio.state != context.last_audio_state) {
        context.last_audio_state = audio.state;
        char message[160]{};
        std::snprintf(message, sizeof(message), "TV audio state=%u error=0x%08X",
            static_cast<unsigned>(audio.state), audio.error);
        Log(context, audio.state == nte_tv_video::AudioOutput::State::Failed ?
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

bool SeekVideo(Context& context, SeekRequest request, MediaClock::time_point now) {
    struct Parameters { std::int64_t time; std::uint8_t accepted{}; };
    static_assert(offsetof(Parameters, accepted) == sizeof(std::int64_t));
    request.target = std::clamp(request.target, std::int64_t{0}, context.media_duration - 1);
    Parameters parameters{request.target};
    if (!InvokePointer(context, context.media_player_pointer, "/Script/MediaAssets.MediaPlayer:Seek",
            &parameters, sizeof(parameters), offsetof(Parameters, accepted) + sizeof(parameters.accepted)) ||
        !parameters.accepted) return false;
    request.due = now + std::chrono::seconds(2);
    context.seeking = request;
    return true;
}

void StopTakeover(Context& context, std::uintptr_t world, bool restore_audio) {
    if (context.opened_path.empty()) return;
    if (context.tv_binding.slot >= 0 && world && context.tv_binding.world == world)
        RestoreTelevision(context, restore_audio);
    else {
        context.tv_binding = {};
        context.game_audio_muted = false;
        CloseLocalMedia(context);
    }
}

void ANOMALY_CALL UpdateMedia(void* plugin_context, double delta_seconds) noexcept {
    auto* pointer = static_cast<Context*>(plugin_context);
    if (!pointer || !pointer->alive.load(std::memory_order_acquire)) return;
    Context& context = *pointer;
    try {
        context.media_poll_seconds += delta_seconds;
        if (context.media_poll_seconds < 0.1) return;
        const double elapsed = std::exchange(context.media_poll_seconds, 0);
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
        const bool source_changed = path != context.selected_path;
        if (!enabled || login || source_changed) {
            if (login) {
                std::scoped_lock lock(context.mutex);
                context.settings.enabled = false;
            }
            const bool active_tv = !context.opened_path.empty() && context.tv_binding.slot >= 0 &&
                world == context.tv_binding.world && !login &&
                LivePointer(context, context.tv_binding.owner, context.tv_binding.owner_handle) &&
                TelevisionPlaying(context) && context.tv_component == context.tv_binding.component;
            StopTakeover(context, login ? 0 : world, active_tv);
            context.resume_position = 0;
            context.takeover_failed = false;
            context.selected_path = path;
            if (!enabled || login || path.empty()) {
                MediaStatus(context, context.native_audio_pending.id ?
                    context.localizer.Text("media.native_audio_pending", "Original sound will resume with the television's next audio event") :
                    context.localizer.Text("media.idle", "Idle"));
                return;
            }
        }
        if (path.empty() || !CanInvoke(context)) return;
        const bool television = FindTelevision(context, world, elapsed);
        if (!television || (!context.opened_path.empty() && context.opened_world != world) ||
            !TelevisionPlaying(context) || (context.tv_binding.slot >= 0 &&
                context.tv_binding.component != context.tv_component)) {
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
            const auto open = ResolveFunction(context, "/Script/MediaAssets.MediaPlayer", "OpenFile");
            const auto looping = ResolveFunction(context, "/Script/MediaAssets.MediaPlayer", "SetLooping");
            struct { std::uint8_t loop{1}; std::uint8_t result{}; } loop;
            std::uint32_t code = ANOMALY_STATUS_V1_UNAVAILABLE;
            static_cast<void>(InvokeResolved(context, context.media_player, looping, &loop, sizeof(loop), code));
            struct { PassedString path; std::uint8_t result{}; } parameters{
                {path.c_str(), static_cast<std::int32_t>(path.size() + 1),
                 static_cast<std::int32_t>(path.size() + 1)}};
            const bool invoked = InvokeResolved(context, context.media_player, open,
                &parameters, sizeof(parameters), code);
            context.opened_path = path;
            context.media_wait_seconds = 0;
            context.media_playing.store(false);
            context.media_probe_phase.store(invoked && parameters.result ? 1 : 2);
            const std::string status_code = std::to_string(code);
            const std::array<std::string_view, 1> arguments{status_code};
            MediaStatus(context, invoked && parameters.result ? context.localizer.Text("media.opening", "Opening") :
                context.localizer.Format("media.open_failed", "OpenFile rejected the local video (status={0})", arguments));
            return;
        }
        const auto now = MediaClock::now();
        const int phase = context.media_probe_phase.load();
        bool seek_issued = false;
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
            context.media_probe_phase.store(3);
            if (context.resume_position > 0 && context.media_seekable) {
                seek_issued = SeekVideo(context, {0, context.resume_position, {}}, now);
                context.seek_failed = !seek_issued;
            }
        } else if (phase != 3 && !context.media_playing.load()) {
            return;
        }

        std::optional<SeekRequest> request;
        if (context.media_playing.load() && !context.seeking) {
            std::scoped_lock lock(context.mutex);
            if (context.seek_request && context.seek_request->due <= now) {
                if (context.seek_request->generation == context.progress.generation && context.media_seekable)
                    request = context.seek_request;
                context.seek_request.reset();
            }
        }
        if (request) {
            seek_issued = SeekVideo(context, *request, now);
            context.seek_failed = !seek_issued;
        }
        if (seek_issued) {
            UpdateAudio(context, path, context.resume_position, false);
            return;
        }

        std::int64_t position{};
        const bool sampled = InvokePointer(context, context.media_player_pointer,
            "/Script/MediaAssets.MediaPlayer:GetTime", &position, sizeof(position)) && position >= 0;
        const bool finishing_seek = context.seeking.has_value();
        if (finishing_seek) {
            const auto target = context.seeking->target;
            constexpr auto tolerance = kTicksPerSecond / 2;
            const bool reached = sampled && (std::abs(position - target) <= tolerance ||
                (context.media_duration - target <= tolerance && position <= tolerance));
            if (!reached && now < context.seeking->due) {
                UpdateAudio(context, path, context.resume_position, false);
                return;
            }
            context.seek_failed = !reached;
            context.seeking.reset();
        }
        if (!context.media_playing.load()) {
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
            MediaStatus(context, context.localizer.Text("media.active", "Playing local video"));
        }
        bool playing{};
        static_cast<void>(MediaBool(context, "IsPlaying", playing));
        if (sampled) context.resume_position = position;
        UpdateAudio(context, path, context.resume_position, sampled && playing);
        {
            std::scoped_lock lock(context.mutex);
            context.progress.position = context.resume_position;
            context.progress.duration = context.media_duration;
            context.progress.seekable = context.media_seekable;
            if (!context.seek_request) context.progress.preview.reset();
        }
        if (request || finishing_seek || (phase == 1 && context.seek_failed))
            MediaStatus(context, context.seek_failed ? context.localizer.Text("media.seek_failed", "Could not seek to the requested position") :
                context.localizer.Text("media.active", "Playing local video"));
    } catch (...) {
        context.media_probe_phase.store(2);
        context.media_playing.store(false);
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
            if (settings.video_path.size() > 4000U) settings.video_path.clear();
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
                           {"muteGameAudio", true},
                           {"forceMediaPlayer", settings.force_media_player}}
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
// UI
// ---------------------------------------------------------------------------

std::optional<std::wstring> PickVideoFile() {
    std::optional<std::wstring> result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        const COMDLG_FILTERSPEC filters[] = {
            {L"Video files", L"*.mp4;*.m4v;*.mov;*.webm;*.mkv;*.usm"},
            {L"All files", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
        FILEOPENDIALOGOPTIONS options{};
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        }
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                result = std::wstring(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

void StartFilePicker(Context& context) noexcept {
    std::shared_ptr<PickerState> state;
    {
        std::scoped_lock lock(context.mutex);
        state = context.picker;
    }
    if (state == nullptr) return;
    {
        std::scoped_lock lock(state->mutex);
        if (state->busy) return;
        state->busy = true;
    }
    try {
        std::thread([state] {
            std::optional<std::wstring> picked;
            try {
                picked = PickVideoFile();
            } catch (...) {
            }
            std::scoped_lock lock(state->mutex);
            if (picked) {
                const std::string utf8 = WideToUtf8(*picked);
                if (!utf8.empty()) {
                    state->path = utf8;
                    state->done = true;
                }
            }
            state->busy = false;
        }).detach();
    } catch (...) {
        std::scoped_lock lock(state->mutex);
        state->busy = false;
    }
}

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

void Draw(void* plugin_context, const AnomalyUiServiceV1* ui_v1) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || ui_v1 == nullptr || context->ui == nullptr) return;
    const std::string title = context->localizer.Text("window.title", "NTE TV Video");
    anomaly::sdk::UiWindow window(context->ui, title);
    if (!window) return;

    {
        std::shared_ptr<PickerState> state;
        {
            std::scoped_lock lock(context->mutex);
            state = context->picker;
        }
        if (state != nullptr) {
            std::string picked;
            {
                std::scoped_lock lock(state->mutex);
                if (!state->busy && state->done) {
                    picked = std::move(state->path);
                    state->path.clear();
                    state->done = false;
                }
            }
            if (!picked.empty()) {
                {
                    std::scoped_lock lock(context->mutex);
                    context->settings.video_path = picked;
                }
                UpdateOverridePath(*context);
                MarkSettingsDirty(*context);
            }
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
    Text(ui_v1, HookStatusText(*context));
    std::string playback_status;
    {
        std::scoped_lock lock(context->mutex);
        playback_status = context->media_status;
    }
    const std::array<std::string_view, 1> playback_args{playback_status};
    Text(ui_v1, context->localizer.Format("text.playback", "Playback: {0}", playback_args));
    DrawProgress(*context, ui_v1);
    const auto audio = context->audio.Read();
    std::string audio_text;
    using AudioState = nte_tv_video::AudioOutput::State;
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
        StartFilePicker(*context);
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
    if (!nte_tv_video_profile::LoadCompatibility(host))
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "compatibility profile rejected");
    auto* context = new (std::nothrow) Context();
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);
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
    try {
        context->picker = std::make_shared<PickerState>();
    } catch (...) {
        delete context;
        return Status(ANOMALY_STATUS_V1_FAILED);
    }
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

AnomalyStatusV1 Stop(void* plugin_context, std::uint32_t) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return anomaly::sdk::Ok();
    context->alive.store(false, std::memory_order_release);
    context->audio.Shutdown();
    g_active.store(nullptr, std::memory_order_release);
    context->persist_task = {};
    return anomaly::sdk::Ok();
}

void Unload(void* plugin_context) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    static_cast<void>(Stop(context, 0));
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
                   anomaly::sdk::StringView("anomaly.nte-tv-video"),
                   anomaly::sdk::StringView("NTE TV Video"),
                   anomaly::sdk::StringView("Anomaly"),
                   anomaly::sdk::StringView("0.1.0"),
                   Load,
                   Start,
                   Stop,
                   Unload,
                   UpdateMedia,
                   Draw,
                   nullptr};
    return anomaly::sdk::Ok();
}
