// NTE TV Video: replaces the residence TV video source with a local video.
//
// The residence TV (BP_FurnitureActorBase_TV_C) plays CRI Mana movies through
// MovieAudioPlayerComponent. Each program switch calls SetVideoFilePath(FString);
// the component then loads TVFileManaMovie (/Game/Movies/FFMpegUI/TVMovies) and
// decodes the USM from the game content directory. This plugin hooks that setter,
// rewrites the path for the TV actor only, and points it at a local video copied
// into the game content directory. Every other MovieAudioPlayer screen keeps the
// game source.

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui.h"
#include "tv_video_profile.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <atomic>
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

#include <nlohmann/json.hpp>

namespace {

using nte_tv_video_profile::native_layout;

// Residence TV actor. The plugin compares the component owner's class name, so
// cinema/story screens keep their game videos.
constexpr std::wstring_view kTvActorClassName = L"BP_FurnitureActorBase_TV_C";
// The relative path the game resolves against HT/Content. The copy keeps it short
// enough to fit the strings the game already passes in.
constexpr std::wstring_view kOverridePath = L"./Movies/FFMpeg_Mana/TV/ntetv.mp4";
constexpr std::size_t kMaxPathChars = 512;
constexpr std::size_t kMaxNameBytes = 256;
constexpr std::size_t kCopyChunkBytes = 1024U * 1024U;
constexpr std::uint64_t kMaximumVideoBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;

constexpr std::string_view kConfigSchemaId = "anomaly.nte-tv-video.settings";
constexpr std::uint32_t kConfigSchemaVersion = 1;
constexpr std::string_view kConfigSchemaJson =
    R"({"type":"object","properties":{"enabled":{"type":"boolean"},)"
    R"("videoPath":{"type":"string","maxLength":4096},)"
    R"("muteGameAudio":{"type":"boolean"}},)"
    R"("required":["enabled","videoPath","muteGameAudio"],"additionalProperties":false})";

struct HookSlot final {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t original{};
    std::uintptr_t target{};
};

struct Settings final {
    bool enabled{};
    bool mute_game_audio{true};
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

struct Context final {
    const AnomalyCoreServiceV1* core{};
    const AnomalySignatureServiceV1* signatures{};
    const AnomalyHookServiceV1* hook{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyConfigServiceV1* config{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyUiServiceV1* ui{};
    anomaly::sdk::Localizer localizer;

    HookSlot set_video{};
    HookSlot mana_file_path{};
    HookSlot play_by_config{};
    HookSlot play_audio{};
    AnomalyGenerationHandleV1 config_schema{};
    AnomalyGenerationHandleV1 copy_task{};
    AnomalyGenerationHandleV1 persist_task{};

    std::mutex mutex;
    Settings settings;
    bool settings_dirty{};
    std::string copy_detail;
    std::shared_ptr<PickerState> picker;

    std::wstring content_target;  // absolute target file under HT/Content
    std::wstring content_directory;
    std::wstring absolute_override;  // same target with forward slashes

    std::atomic<bool> enabled{false};
    std::atomic<bool> mute_game_audio{true};
    std::atomic<bool> copy_ready{false};
    std::atomic<bool> alive{false};
    std::atomic<unsigned> copy_state{0};  // 0 idle, 1 copying, 2 ready, 3 failed
    std::atomic<unsigned> tv_hits{0};
    std::atomic<bool> logged_set_missing{false};
    std::atomic<bool> logged_mana_missing{false};
    std::atomic<bool> logged_play_missing{false};
    std::atomic<bool> logged_audio_missing{false};
    std::atomic<bool> logged_copy_failed{false};
};

// Published for the detours, which run on the game thread.
std::atomic<Context*> g_active{};

using SetVideoFilePathFn = void(ANOMALY_CALL*)(void*, void*);
using PlayAudioFn = void(ANOMALY_CALL*)(void*, void*);

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

// Bounded UTF-8 rendering of a UE FString for the log only.
bool ReadFStringText(Context& context, const std::uintptr_t address, char* output,
                     const std::size_t capacity) noexcept {
    if (output == nullptr || capacity == 0) return false;
    output[0] = '\0';
    std::uintptr_t data{};
    std::int32_t length{}, maximum{};
    if (!Read(context, address, data) ||
        !Read(context, address + native_layout.kFStringNumOffset, length) ||
        !Read(context, address + native_layout.kFStringMaxOffset, maximum) ||
        data == 0 || length <= 0 || maximum < length ||
        static_cast<std::size_t>(length) >= kMaxPathChars) {
        return false;
    }
    const std::size_t characters = static_cast<std::size_t>(length);
    std::array<char16_t, kMaxPathChars> wide{};
    if (!ReadBytes(context, data, wide.data(), characters * sizeof(char16_t))) return false;
    const std::size_t copied = characters < capacity - 1 ? characters : capacity - 1;
    for (std::size_t index = 0; index < copied; ++index) {
        const char16_t unit = wide[index];
        output[index] = (unit >= 0x20 && unit < 0x7F) ? static_cast<char>(unit) : '?';
    }
    output[copied] = '\0';
    return true;
}

// Overwrites the caller-owned FString with the fixed relative override path.
// The path is shorter than the paths the game already passes, so the existing
// allocation is reused and no game allocator call is needed.
bool ApplyOverrideText(Context& context, void* path_pointer,
                       const std::wstring_view replacement) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(path_pointer);
    if (address == 0 || replacement.empty() ||
        native_layout.kFStringNumOffset == 0 || native_layout.kFStringMaxOffset == 0) {
        return false;
    }
    std::uintptr_t data{};
    std::int32_t length{}, maximum{};
    if (!Read(context, address, data) || data == 0 ||
        !Read(context, address + native_layout.kFStringNumOffset, length) ||
        !Read(context, address + native_layout.kFStringMaxOffset, maximum)) {
        return false;
    }
    if (maximum < static_cast<std::int32_t>(replacement.size()) + 1) return false;
    std::array<wchar_t, kMaxPathChars> wide{};
    if (replacement.size() >= wide.size()) return false;
    std::memcpy(wide.data(), replacement.data(), replacement.size() * sizeof(wchar_t));
    wide[replacement.size()] = L'\0';
    if (!WriteBytes(context, data, wide.data(),
                    (replacement.size() + 1) * sizeof(wchar_t))) {
        return false;
    }
    const std::int32_t new_length = static_cast<std::int32_t>(replacement.size());
    return Write(context, address + native_layout.kFStringNumOffset, new_length);
}

bool ApplyOverride(Context& context, void* path_pointer) noexcept {
    return ApplyOverrideText(context, path_pointer, kOverridePath);
}

bool LooksAbsolutePath(const char* const text) noexcept {
    if (text == nullptr) return false;
    return (text[0] != '\0' && text[1] == ':') || text[0] == '\\';
}

bool InstallHook(Context& context, const std::uintptr_t target, void* detour,
                 const std::string_view label, HookSlot& slot) noexcept {
    if (target == 0 || !HookReady(context.hook)) return false;
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = detour;
    request.label = anomaly::sdk::StringView(label);
    std::uintptr_t original{};
    AnomalyGenerationHandleV1 handle{};
    if (context.hook->create(context.hook->user, &request, &original, &handle).code !=
            ANOMALY_STATUS_V1_OK ||
        handle.id == 0 || original == 0) {
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

void ANOMALY_CALL SetVideoFilePathDetour(void* self, void* path) noexcept;
void ANOMALY_CALL ManaFilePathDetour(void* self, void* path) noexcept;
void ANOMALY_CALL PlayByConfigDetour(void* self, void* row) noexcept;
void ANOMALY_CALL PlayAudioDetour(void* self, void* argument) noexcept;

void EnsureHooks(Context& context) noexcept {
    if (context.set_video.handle.id == 0) {
        std::uintptr_t target{};
        if (ResolveValidated(context, "setVideoFilePath", target) &&
            InstallHook(context, target, reinterpret_cast<void*>(&SetVideoFilePathDetour),
                        "tv-video-set-file-path", context.set_video)) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                          "TV video hook installed setVideoFilePath=0x%llX",
                          static_cast<unsigned long long>(target));
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
        } else if (!context.logged_set_missing.exchange(true, std::memory_order_acq_rel)) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "TV video hook unavailable: setVideoFilePath signature or entry check failed");
        }
    }
    if (context.mana_file_path.handle.id == 0) {
        std::uintptr_t target{};
        if (ResolveValidated(context, "fileManaMovieSetFilePath", target) &&
            InstallHook(context, target, reinterpret_cast<void*>(&ManaFilePathDetour),
                        "tv-video-mana-file-path", context.mana_file_path)) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                          "TV video Mana hook installed SetFilePath=0x%llX",
                          static_cast<unsigned long long>(target));
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
        } else if (!context.logged_mana_missing.exchange(true, std::memory_order_acq_rel)) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "TV video Mana hook unavailable: FileManaMovie.SetFilePath signature or entry check failed");
        }
    }
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

void HandleTvSourcePath(Context& context, const char* const tag, void* path) noexcept {
    char original[kMaxPathChars]{};
    const bool readable =
        ReadFStringText(context, reinterpret_cast<std::uintptr_t>(path), original,
                        sizeof(original));
    bool applied = false;
    if (context.enabled.load(std::memory_order_acquire) &&
        context.copy_ready.load(std::memory_order_acquire)) {
        applied = ApplyOverride(context, path);
    }
    char message[768]{};
    std::snprintf(message, sizeof(message), "%s: %s%s", tag,
                  readable ? original : "<unreadable>",
                  applied ? " -> ./Movies/FFMpeg_Mana/TV/ntetv.mp4" : " (kept)");
    Log(context,
        applied ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        message);
}

void ANOMALY_CALL SetVideoFilePathDetour(void* self, void* path) noexcept {
    Context* context = g_active.load(std::memory_order_acquire);
    AnomalyGenerationHandleV1 lease{};
    bool leased = false;
    std::uintptr_t original = 0;
    try {
        if (context != nullptr && context->set_video.handle.id != 0 &&
            HookReady(context->hook)) {
            leased = context->hook
                         ->begin_callback(context->hook->user, context->set_video.handle,
                                          &lease)
                         .code == ANOMALY_STATUS_V1_OK;
            original = context->set_video.original;
        }
    } catch (...) {
    }
    try {
        if (context != nullptr && path != nullptr) {
            char actor[128]{};
            std::uintptr_t owner{};
            const bool named =
                Read(*context,
                     reinterpret_cast<std::uintptr_t>(self) + native_layout.kObjectOuterOffset,
                     owner) &&
                owner != 0 &&
                ReadFNameText(*context, owner + native_layout.kObjectNameOffset, actor,
                              sizeof(actor));
            const bool tv = IsTvComponent(*context, self);
            char source[kMaxPathChars]{};
            const bool readable = ReadFStringText(
                *context, reinterpret_cast<std::uintptr_t>(path), source, sizeof(source));
            char message[768]{};
            std::snprintf(message, sizeof(message), "SetVideoFilePath owner=%s tv=%d path=%s",
                          named ? actor : "?", tv ? 1 : 0,
                          readable ? source : "<unreadable>");
            Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
            if (tv) {
                context->tv_hits.fetch_add(1, std::memory_order_relaxed);
                HandleTvSourcePath(*context, "TV SetVideoFilePath", path);
            }
        }
    } catch (...) {
    }
    try {
        if (original != 0) {
            reinterpret_cast<SetVideoFilePathFn>(original)(self, path);
        }
    } catch (...) {
    }
    if (leased && context != nullptr && HookReady(context->hook)) {
        static_cast<void>(context->hook->end_callback(context->hook->user, lease));
    }
}

void ANOMALY_CALL ManaFilePathDetour(void* self, void* path) noexcept {
    Context* context = g_active.load(std::memory_order_acquire);
    AnomalyGenerationHandleV1 lease{};
    bool leased = false;
    std::uintptr_t original = 0;
    try {
        if (context != nullptr && context->mana_file_path.handle.id != 0 &&
            HookReady(context->hook)) {
            leased = context->hook
                         ->begin_callback(context->hook->user,
                                          context->mana_file_path.handle, &lease)
                         .code == ANOMALY_STATUS_V1_OK;
            original = context->mana_file_path.original;
        }
    } catch (...) {
    }
    try {
        if (context != nullptr && path != nullptr) {
            char source[kMaxPathChars]{};
            const bool readable = ReadFStringText(
                *context, reinterpret_cast<std::uintptr_t>(path), source, sizeof(source));
            if (readable &&
                std::string_view(source).find("Movies/FFMpeg_Mana/EP/") !=
                    std::string_view::npos) {
                context->tv_hits.fetch_add(1, std::memory_order_relaxed);
                bool applied = false;
                if (context->enabled.load(std::memory_order_acquire) &&
                    context->copy_ready.load(std::memory_order_acquire)) {
                    const std::wstring_view target =
                        LooksAbsolutePath(source)
                            ? std::wstring_view(context->absolute_override)
                            : std::wstring_view(kOverridePath);
                    applied = ApplyOverrideText(*context, path, target);
                }
                char message[768]{};
                std::snprintf(message, sizeof(message), "TV ManaFilePath: %s%s", source,
                              applied ? " -> local override" : " (kept)");
                Log(*context,
                    applied ? ANOMALY_CORE_LOG_LEVEL_V1_INFO
                            : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    message);
            }
        }
    } catch (...) {
    }
    try {
        if (original != 0) {
            reinterpret_cast<SetVideoFilePathFn>(original)(self, path);
        }
    } catch (...) {
    }
    if (leased && context != nullptr && HookReady(context->hook)) {
        static_cast<void>(context->hook->end_callback(context->hook->user, lease));
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
        if (context != nullptr && row != nullptr && IsTvComponent(*context, self)) {
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
    try {
        if (context != nullptr && original != 0 &&
            context->enabled.load(std::memory_order_acquire) &&
            context->mute_game_audio.load(std::memory_order_acquire) &&
            context->copy_ready.load(std::memory_order_acquire) &&
            IsTvComponent(*context, self)) {
            skip = true;
        }
    } catch (...) {
    }
    try {
        if (!skip && original != 0) {
            reinterpret_cast<PlayAudioFn>(original)(self, argument);
        }
    } catch (...) {
    }
    if (leased && context != nullptr && HookReady(context->hook)) {
        static_cast<void>(context->hook->end_callback(context->hook->user, lease));
    }
}

// ---------------------------------------------------------------------------
// Settings and local video copy
// ---------------------------------------------------------------------------

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
            settings.enabled = json.value("enabled", false);
            settings.mute_game_audio = json.value("muteGameAudio", true);
            settings.video_path = json.value("videoPath", std::string{});
            if (settings.video_path.size() > 4000U) settings.video_path.clear();
            {
                std::scoped_lock lock(context.mutex);
                context.settings = settings;
            }
            context.enabled.store(settings.enabled, std::memory_order_release);
            context.mute_game_audio.store(settings.mute_game_audio,
                                          std::memory_order_release);
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
        context->settings_dirty = false;
    }
    if (!ConfigReady(context->config)) return;
    try {
        const std::string document =
            nlohmann::json{{"enabled", settings.enabled},
                           {"videoPath", settings.video_path},
                           {"muteGameAudio", settings.mute_game_audio}}
                .dump();
        static_cast<void>(context->config->write_atomic(
            context->config->user, anomaly::sdk::StringView(kConfigSchemaId),
            kConfigSchemaVersion,
            {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()}));
    } catch (...) {
    }
}

void SchedulePersist(Context& context) noexcept {
    if (!SchedulerReady(context.scheduler) || context.persist_task.id != 0) return;
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

bool EnsureDirectory(const std::wstring& directory) noexcept {
    if (directory.empty()) return false;
    std::wstring current;
    current.reserve(directory.size());
    for (std::size_t index = 0; index < directory.size(); ++index) {
        current.push_back(directory[index]);
        if (directory[index] == L'\\' || index + 1 == directory.size()) {
            if (current.size() > 3) {
                if (!CreateDirectoryW(current.c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool CopyVideoFile(const std::wstring& source, const std::wstring& destination,
                   std::string& detail) noexcept {
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        detail = "open source failed";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaximumVideoBytes) {
        CloseHandle(input);
        detail = "source file is empty or too large";
        return false;
    }
    const std::wstring temporary = destination + L".part";
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        detail = "create target failed";
        return false;
    }
    std::unique_ptr<std::uint8_t[]> buffer(new (std::nothrow) std::uint8_t[kCopyChunkBytes]);
    bool ok = buffer != nullptr;
    if (ok) {
        for (;;) {
            DWORD read{};
            if (!ReadFile(input, buffer.get(), static_cast<DWORD>(kCopyChunkBytes), &read,
                          nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) break;
            DWORD written{};
            if (!WriteFile(output, buffer.get(), read, &written, nullptr) ||
                written != read) {
                ok = false;
                break;
            }
        }
        if (ok) ok = FlushFileBuffers(output) != FALSE;
    }
    CloseHandle(input);
    CloseHandle(output);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        detail = "copy failed";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        detail = "publish failed";
        return false;
    }
    return true;
}

void CopyTask(void* user, AnomalyGenerationHandleV1) noexcept {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr || !context->alive.load(std::memory_order_acquire)) return;
    context->copy_task = {};
    std::string video_path;
    {
        std::scoped_lock lock(context->mutex);
        video_path = context->settings.video_path;
    }
    const std::wstring source = Utf8ToWide(video_path);
    if (source.empty()) {
        context->copy_ready.store(false, std::memory_order_release);
        context->copy_state.store(0, std::memory_order_release);
        return;
    }
    context->copy_state.store(1, std::memory_order_release);
    std::string detail;
    const bool directory_ok = EnsureDirectory(context->content_directory);
    const bool copied = directory_ok &&
                        CopyVideoFile(source, context->content_target, detail);
    {
        std::scoped_lock lock(context->mutex);
        context->copy_detail = copied ? std::string{} : (directory_ok ? detail : "create directory failed");
    }
    context->copy_ready.store(copied, std::memory_order_release);
    context->copy_state.store(copied ? 2U : 3U, std::memory_order_release);
    if (!copied && !context->logged_copy_failed.exchange(true, std::memory_order_acq_rel)) {
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "TV video copy failed; the game video stays in place");
    }
    if (copied) {
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "TV video copied to HT/Content/Movies/FFMpeg_Mana/TV/ntetv.mp4");
    }
}

void ScheduleCopy(Context& context) noexcept {
    context.copy_ready.store(false, std::memory_order_release);
    context.copy_state.store(1, std::memory_order_release);
    context.logged_copy_failed.store(false, std::memory_order_release);
    if (!SchedulerReady(context.scheduler)) return;
    AnomalyGenerationHandleV1 task{};
    if (context.scheduler
            ->schedule(context.scheduler->user, 0, CopyTask, &context, &task)
            .code == ANOMALY_STATUS_V1_OK &&
        task.id != 0) {
        context.copy_task = task;
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
    if (context.mana_file_path.handle.id != 0) {
        return context.localizer.Text("status.hooks_installed", "Hooks: installed");
    }
    if (context.set_video.handle.id != 0) {
        return context.localizer.Text("status.hooks_partial",
                                      "Hooks: installed without the media-source hook");
    }
    return context.localizer.Text("status.hooks_missing",
                                  "Hooks: setter not found (game build not supported)");
}

std::string CopyStatusText(Context& context) noexcept {
    const unsigned state = context.copy_state.load(std::memory_order_acquire);
    if (context.settings.video_path.empty()) {
        return context.localizer.Text("status.video_none", "Video: none selected");
    }
    if (state == 1) return context.localizer.Text("status.video_copying", "Video: copying");
    if (state == 2) return context.localizer.Text("status.video_ready", "Video: ready");
    std::string detail;
    {
        std::scoped_lock lock(context.mutex);
        detail = context.copy_detail;
    }
    if (detail.empty()) detail = context.localizer.Text("status.video_idle", "Video: not copied");
    const std::array<std::string_view, 1> args{detail};
    return context.localizer.Format("status.video_failed", "Video: copy failed ({0})", args);
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
                MarkSettingsDirty(*context);
                ScheduleCopy(*context);
            }
        }
    }

    const std::string enabled_label =
        context->localizer.Text("option.enabled", "Replace the residence TV video");
    int enabled = context->settings.enabled ? 1 : 0;
    static_cast<void>(ui_v1->checkbox(ui_v1->user,
                                      anomaly::sdk::StringView(enabled_label), &enabled));
    if ((enabled != 0) != context->settings.enabled) {
        context->settings.enabled = enabled != 0;
        context->enabled.store(enabled != 0, std::memory_order_release);
        MarkSettingsDirty(*context);
    }
    const std::string mute_label =
        context->localizer.Text("option.mute", "Mute the game's TV audio");
    int mute = context->settings.mute_game_audio ? 1 : 0;
    static_cast<void>(ui_v1->checkbox(ui_v1->user,
                                      anomaly::sdk::StringView(mute_label), &mute));
    if ((mute != 0) != context->settings.mute_game_audio) {
        context->settings.mute_game_audio = mute != 0;
        context->mute_game_audio.store(mute != 0, std::memory_order_release);
        MarkSettingsDirty(*context);
    }

    Text(ui_v1, CopyStatusText(*context));
    Text(ui_v1, HookStatusText(*context));

    {
        std::string path;
        {
            std::scoped_lock lock(context->mutex);
            path = context->settings.video_path;
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
    ui_v1->same_line(ui_v1->user, 0.0F, 8.0F);
    const std::string recopy = context->localizer.Label("action.recopy", "Copy now",
                                                        "tv-video-copy");
    if (Button(ui_v1, recopy, !context->settings.video_path.empty())) {
        ScheduleCopy(*context);
    }

    const std::array<std::string_view, 1> hits{
        std::to_string(context->tv_hits.load(std::memory_order_relaxed))};
    Text(ui_v1, context->localizer.Format("text.tv_hits", "TV captures: {0}", hits));
}

// ---------------------------------------------------------------------------
// Plugin entry
// ---------------------------------------------------------------------------

bool BuildContentTargets(Context& context) noexcept {
    std::array<wchar_t, MAX_PATH> module{};
    const DWORD length = GetModuleFileNameW(nullptr, module.data(),
                                            static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size()) return false;
    wchar_t* slash = wcsrchr(module.data(), L'\\');
    if (slash == nullptr) return false;
    *slash = L'\0';
    std::wstring content = std::wstring(module.data()) + L"\\..\\..\\Content";
    std::array<wchar_t, MAX_PATH> normalized{};
    const DWORD normalized_length =
        GetFullPathNameW(content.c_str(), static_cast<DWORD>(normalized.size()),
                         normalized.data(), nullptr);
    if (normalized_length == 0 || normalized_length >= normalized.size()) return false;
    context.content_directory = std::wstring(normalized.data()) +
                                L"\\Movies\\FFMpeg_Mana\\TV";
    context.content_target = context.content_directory + L"\\ntetv.mp4";
    std::wstring absolute = context.content_target;
    std::replace(absolute.begin(), absolute.end(), L'\\', L'/');
    context.absolute_override = std::move(absolute);
    return true;
}

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
    context->names = Query<AnomalyUe5NamesServiceV1>(host, ANOMALY_UE5_NAMES_SERVICE_V1_ID,
                                                     1);
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
        context->ui == nullptr || !BuildContentTargets(*context)) {
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
    context->set_video = HookSlot{};
    context->mana_file_path = HookSlot{};
    context->play_by_config = HookSlot{};
    context->play_audio = HookSlot{};
    context->logged_set_missing.store(false, std::memory_order_release);
    context->logged_mana_missing.store(false, std::memory_order_release);
    context->logged_play_missing.store(false, std::memory_order_release);
    context->logged_audio_missing.store(false, std::memory_order_release);
    context->settings = Settings{};
    context->copy_ready.store(false, std::memory_order_release);
    context->copy_state.store(0, std::memory_order_release);
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
    g_active.store(context, std::memory_order_release);
    EnsureHooks(*context);
    if (!context->settings.video_path.empty()) ScheduleCopy(*context);
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "TV video started");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 Stop(void* plugin_context, std::uint32_t) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return anomaly::sdk::Ok();
    context->alive.store(false, std::memory_order_release);
    g_active.store(nullptr, std::memory_order_release);
    try {
        ReleaseHook(*context, context->set_video);
        ReleaseHook(*context, context->mana_file_path);
        ReleaseHook(*context, context->play_by_config);
        ReleaseHook(*context, context->play_audio);
        if (SchedulerReady(context->scheduler)) {
            if (context->copy_task.id != 0) {
                static_cast<void>(context->scheduler->cancel(context->scheduler->user,
                                                            context->copy_task));
            }
            if (context->persist_task.id != 0) {
                static_cast<void>(context->scheduler->cancel(context->scheduler->user,
                                                            context->persist_task));
            }
        }
    } catch (...) {
    }
    context->copy_task = {};
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
                   nullptr,
                   Draw,
                   nullptr};
    return anomaly::sdk::Ok();
}
