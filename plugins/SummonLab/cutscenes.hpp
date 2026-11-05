#pragma once

// Included inside plugin.cpp's private namespace after the UE access helpers.
// Playback settings are copied at the native boundary, so asynchronous loads
// carry the policy with that request. Shared assets and CDOs are never edited.
struct CutsceneBool {
    std::uint32_t offset{};
    std::uint8_t mask{}, set_mask{};
    bool value{};
};

struct CutsceneSettings {
    std::vector<CutsceneBool> movie, gameplay;
    std::uint32_t rate{}, loops{};
};

struct CutsceneState {
    anomaly::sdk::Profile profile;
    std::array<NativeCombatHook, 7> hooks{};
    std::atomic<bool> ready{false};
    std::atomic<DWORD> game_thread{};
    bool pending{};
    CutsceneSettings settings;
    // Keep weak identities beyond management's death removal: delayed death
    // scripts can still request a sequence while their actor remains alive.
    std::map<std::uintptr_t, AnomalyGenerationHandleV1> sources;
    std::uintptr_t cleanup_after{}, scoped_controller{};
};
CutsceneState g_cutscenes;

// Fixed native parameter ABIs, checked against reflected struct sizes/fields.
using CutsceneMovieSettings = std::array<std::uint8_t, 40>;
struct alignas(16) CutsceneGameplaySettings { std::array<std::uint8_t, 192> bytes; };
static_assert(sizeof(CutsceneGameplaySettings) == 192);

bool CutsceneField(std::uintptr_t owner, const char* name, const char* kind,
                   std::size_t size, std::uint32_t& offset, std::uintptr_t* field_out = nullptr) {
    const auto field = FindProperty(owner, name);
    std::uintptr_t type{};
    std::int32_t width{}, count{}, position{}, owner_size{};
    std::uint32_t id{};
    char type_name[64]{};
    if (!field || !ReadValue(field + g_layout.fieldClass, &type) ||
        !ReadValue(type + g_layout.fieldClassName, &id) || !ResolveName(id, type_name, sizeof(type_name)) ||
        std::strcmp(type_name, kind) != 0 ||
        !ReadValue(field + g_layout.propertyElementSize, &width) || width != size ||
        !ReadValue(field + g_layout.propertyArrayDim, &count) || count != 1 ||
        !ReadValue(field + g_layout.propertyOffset, &position) || position < 0 ||
        !ReadValue(owner + g_layout.structSize, &owner_size) || position > owner_size ||
        width > owner_size - position) return false;
    offset = static_cast<std::uint32_t>(position);
    if (field_out) *field_out = field;
    return true;
}

bool CutsceneStruct(std::uintptr_t function, const char* field_name, const char* type_name,
                    std::size_t size, std::uintptr_t& type) {
    std::int32_t declared{};
    return DiagnosticStruct(FindProperty(function, field_name), type_name, &type, &declared) && declared == size;
}

bool CutsceneBoolField(std::uintptr_t owner, const char* name, bool value,
                      std::vector<CutsceneBool>& output) {
    CutsceneBool edit;
    std::uintptr_t field{};
    std::uint8_t byte{};
    if (!CutsceneField(owner, name, "BoolProperty", 1, edit.offset, &field) ||
        !ReadValue(field + g_layout.boolByteOffset, &byte) || byte != 0 ||
        !ReadValue(field + g_layout.boolFieldMask, &edit.mask) ||
        !ReadValue(field + g_layout.boolByteMask, &edit.set_mask) ||
        !edit.mask || !edit.set_mask || (edit.set_mask & (edit.set_mask - 1)) != 0 ||
        (edit.mask != 0xff && edit.mask != edit.set_mask)) return false;
    edit.value = value;
    output.push_back(edit);
    return true;
}

bool PrepareCutsceneSettings(std::uintptr_t movie, std::uintptr_t gameplay) {
    CutsceneSettings settings;
    if (!CutsceneField(movie, "PlayRate", "FloatProperty", sizeof(float), settings.rate)) return false;
    std::uintptr_t loop_type{};
    std::uint32_t loop_offset{}, count_offset{};
    if (!CutsceneField(movie, "LoopCount", "StructProperty", sizeof(std::int32_t), loop_offset) ||
        !CutsceneStruct(movie, "LoopCount", "MovieSceneSequenceLoopCount", sizeof(std::int32_t), loop_type) ||
        !CutsceneField(loop_type, "Value", "IntProperty", sizeof(std::int32_t), count_offset)) return false;
    settings.loops = loop_offset + count_offset;
    for (const auto* name : {"bDisableMovementInput", "bDisableLookAtInput", "bHidePlayer", "bHideHud", "bPauseAtEnd"})
        if (!CutsceneBoolField(movie, name, false, settings.movie)) return false;
    if (!CutsceneBoolField(movie, "bDisableCameraCuts", true, settings.movie)) return false;
    for (const auto* name : {"bDisableKeyboardInput", "bHideUI", "bHideCursor", "bHideHeadUpInfo",
             "bPauseCameraSwitch", "bHideSpawnedNpc", "bStopBehaviorTree", "bHideVehicle",
             "bHideSpawnedVisualActor", "bPauseGameSound", "bIsSequenceFadeIn", "bIsSequenceFadeOut",
             "bBreakAllSkillMontange", "bResetCamera", "bSyncCameraYaw", "bInvitedNpcHidden"})
        if (!CutsceneBoolField(gameplay, name, false, settings.gameplay)) return false;
    g_cutscenes.settings = std::move(settings);
    return true;
}

void CutsceneEditBools(std::uint8_t* bytes, const std::vector<CutsceneBool>& edits) {
    for (const auto& edit : edits)
        bytes[edit.offset] = static_cast<std::uint8_t>((bytes[edit.offset] & ~edit.mask) |
            (edit.value ? edit.set_mask : 0));
}

bool CopyCutsceneSettings(const void* movie, const void* gameplay,
                          CutsceneMovieSettings& movie_copy, CutsceneGameplaySettings& gameplay_copy) {
    if (!ReadBytes(reinterpret_cast<std::uintptr_t>(movie), movie_copy.data(), movie_copy.size()) ||
        !ReadBytes(reinterpret_cast<std::uintptr_t>(gameplay), gameplay_copy.bytes.data(), gameplay_copy.bytes.size())) return false;
    CutsceneEditBools(movie_copy.data(), g_cutscenes.settings.movie);
    CutsceneEditBools(gameplay_copy.bytes.data(), g_cutscenes.settings.gameplay);
    // Finish through the engine's normal Play/PlayTo evaluation. Stop() alone
    // does not emit OnFinished and would strand blueprints waiting on it.
    const float rate = 10000.0f;
    const std::int32_t loops = 0;
    std::memcpy(movie_copy.data() + g_cutscenes.settings.rate, &rate, sizeof(rate));
    std::memcpy(movie_copy.data() + g_cutscenes.settings.loops, &loops, sizeof(loops));
    return true;
}

bool IsCutsceneSource(std::uintptr_t source) {
    if (!source || g_cutscenes.game_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return false;
    if (g_birth_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
        const auto* birth = g_birth_initialization;
        std::uintptr_t type{};
        std::uint32_t flags{};
        if (birth && birth->skip_cutscenes && (!birth->actor || birth->actor == source) &&
            ReadValue(source + g_layout.objectClass, &type) && type == birth->actor_class &&
            ReadValue(source + g_layout.objectFlags, &flags) && (flags & 0x30U) == 0) return true;
    }
    const auto found = g_cutscenes.sources.find(source);
    AnomalyGenerationHandleV1 actual{};
    return found != g_cutscenes.sources.end() && HandleFromPointer(source, &actual) &&
        actual.id == found->second.id && actual.generation == found->second.generation;
}

class CutsceneLease {
public:
    explicit CutsceneLease(std::size_t index) : service_(g_context.hooks) {
        if (service_ && g_cutscenes.hooks[index].handle.id)
            active_ = service_->begin_callback(service_->user, g_cutscenes.hooks[index].handle, &lease_).code == ANOMALY_STATUS_V1_OK;
    }
    ~CutsceneLease() noexcept {
        if (active_) { try { static_cast<void>(service_->end_callback(service_->user, lease_)); } catch (...) {} }
    }
    explicit operator bool() const noexcept { return active_; }
private:
    const AnomalyHookServiceV1* service_{};
    AnomalyGenerationHandleV1 lease_{};
    bool active_{};
};

// FFrame::Object is the blueprint caller, while the exec receiver below is
// the player controller. The scope ends with the synchronous native call;
// copied settings, rather than a global flag, survive asynchronous loading.
template <std::size_t Index>
void ANOMALY_CALL CutsceneExec(std::uintptr_t object, std::uintptr_t frame, void* result) noexcept {
    using Function = void (ANOMALY_CALL*)(std::uintptr_t, std::uintptr_t, void*);
    const auto original = reinterpret_cast<Function>(g_cutscenes.hooks[Index].original);
    try {
        CutsceneLease lease(Index);
        if (!lease || !g_cutscenes.ready.load(std::memory_order_acquire) ||
            g_cutscenes.game_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) {
            if (original) original(object, frame, result);
            return;
        }
        std::uintptr_t caller{};
        const auto previous = g_cutscenes.scoped_controller;
        g_cutscenes.scoped_controller = ReadValue(frame + g_layout.scriptFrameObject, &caller) &&
            IsCutsceneSource(caller) ? object : 0;
        try { if (original) original(object, frame, result); }
        catch (...) { g_cutscenes.scoped_controller = previous; throw; }
        g_cutscenes.scoped_controller = previous;
    } catch (...) {}
}

// Native factories return UObject*, controller playback functions return void.
// The soft reference argument is passed through unchanged: its native callee
// owns the by-value temporary and may free its string on return.
template <std::size_t Index, bool Factory>
auto ANOMALY_CALL CutscenePlayback(std::uintptr_t source, std::uintptr_t asset,
                                  const void* movie, const void* gameplay) noexcept
    -> std::conditional_t<Factory, std::uintptr_t, void> {
    using Result = std::conditional_t<Factory, std::uintptr_t, void>;
    using Function = Result (ANOMALY_CALL*)(std::uintptr_t, std::uintptr_t, const void*, const void*);
    const auto original = reinterpret_cast<Function>(g_cutscenes.hooks[Index].original);
    try {
        CutsceneLease lease(Index);
        alignas(16) CutsceneMovieSettings movie_copy;
        CutsceneGameplaySettings gameplay_copy;
        bool suppress{};
        try {
            if (lease && g_cutscenes.ready.load(std::memory_order_acquire) &&
                g_cutscenes.game_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
                const bool selected = Factory ? IsCutsceneSource(source) :
                    g_cutscenes.scoped_controller != 0 && source == g_cutscenes.scoped_controller;
                suppress = selected && CopyCutsceneSettings(movie, gameplay, movie_copy, gameplay_copy);
                if (suppress && g_context.core && g_context.core->log) {
                    char message[128]{};
                    std::snprintf(message, sizeof(message), "SummonLab: cutscene fast-forward route=%zu source=0x%llx",
                        Index, static_cast<unsigned long long>(source));
                    g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(message));
                }
            }
        } catch (...) {}
        if (original) return original(source, asset, suppress ? movie_copy.data() : movie,
            suppress ? gameplay_copy.bytes.data() : gameplay);
    } catch (...) {}
    if constexpr (Factory) return 0;
}

AnomalyStatusV1 ReleaseCutsceneHooks() {
    g_cutscenes.ready.store(false, std::memory_order_release);
    for (auto it = g_cutscenes.hooks.rbegin(); it != g_cutscenes.hooks.rend(); ++it) {
        if (g_context.hooks && it->handle.id) {
            const auto status = g_context.hooks->release(g_context.hooks->user, it->handle);
            if (status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
        }
        *it = {};
    }
    return anomaly::sdk::Ok();
}

bool InstallCutsceneHooks() {
    if (!g_context.hooks || !g_final_tower.objects || !g_final_tower.chunk_size) return false;
    constexpr std::array<const char*, 5> paths{
        "/Script/HTGame.HTPlayerController:PlayWorldLevelSequence",
        "/Script/HTGame.HTPlayerController:PlaySoftSequence",
        "/Script/HTGame.HTPlayerController:PlaySequence",
        "/Script/HTGame.AsyncCreateLevelSequencePlayerBase:AsyncCreateLevelSequencePlayer",
        "/Script/HTGame.AsyncPlaySoftSequenceBase:AsyncPlaySoftSequence"};
    constexpr std::array<const char*, 5> entry_names{
        "cutscene-exec-world", "cutscene-exec-soft", "cutscene-exec-play",
        "cutscene-exec-create", "cutscene-exec-async-play"};
    constexpr std::array<std::uint16_t, 5> parameter_sizes{112, 272, 240, 296, 296};
    std::array<std::uintptr_t, paths.size()> functions{}, entries{};
    std::uintptr_t movie_type{}, gameplay_type{};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t chunks{}, chunk{};
        std::uint32_t flags{};
        std::uint16_t size{};
        if (!FindHandle(paths[i], &handle) || !ProbeHandle(handle) ||
            !ReadValue(g_final_tower.objects, &chunks) || !chunks) return false;
        const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
        if (!ReadValue(chunks + sizeof(std::uintptr_t) * (index / g_final_tower.chunk_size), &chunk) || !chunk ||
            !ReadValue(chunk + g_final_tower.item_size * static_cast<std::uintptr_t>(index % g_final_tower.chunk_size) +
                g_final_tower.object_offset, &functions[i])) return false;
        if (!functions[i] ||
            !ReadValue(functions[i] + g_layout.functionFlags, &flags) || (flags & 0x400U) == 0 ||
            !ReadValue(functions[i] + g_layout.functionParameterSize, &size) || size != parameter_sizes[i] ||
            !ReadValue(functions[i] + g_layout.functionExec, &entries[i]) || !entries[i]) return false;
        const auto verified = anomaly::sdk::ResolveProfileSymbolWith(g_context.core, g_cutscenes.profile, entry_names[i],
            [&](const anomaly::sdk::ProfileCandidate& candidate, std::uintptr_t* address) {
                if (candidate.call_offset) return AnomalyStatusV1{ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
                *address = entries[i]; return anomaly::sdk::Ok();
            });
        if (!verified) return false;
        if (i) {
            std::uintptr_t movie{}, gameplay{};
            if (!CutsceneStruct(functions[i], "Settings", "MovieSceneSequencePlaybackSettings", sizeof(CutsceneMovieSettings), movie) ||
                !CutsceneStruct(functions[i], i == 3 ? "InGameplaySettings" : "GameplaySettings",
                    "HTSequencePlaybackSettings", sizeof(CutsceneGameplaySettings), gameplay) ||
                (movie_type && (movie != movie_type || gameplay != gameplay_type))) return false;
            movie_type = movie; gameplay_type = gameplay;
        }
    }
    if (!PrepareCutsceneSettings(movie_type, gameplay_type)) return false;
    std::array<std::uintptr_t, 7> targets{entries[0], entries[1], entries[2]};
    constexpr std::array<const char*, 4> leaves{
        "cutscene-play-soft", "cutscene-play", "cutscene-create", "cutscene-async-play"};
    const std::array<std::uintptr_t, 4> callers{entries[0], entries[2], entries[3], entries[4]};
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const auto resolved = anomaly::sdk::ResolveProfileSymbolWith(g_context.core, g_cutscenes.profile, leaves[i],
            [&](const anomaly::sdk::ProfileCandidate& candidate, std::uintptr_t* address) {
                const auto callsite = candidate.values.find("execCallsite");
                if (!candidate.call_offset || *candidate.call_offset != 0 ||
                    callsite == candidate.values.end() || callsite->second > 4096)
                    return AnomalyStatusV1{ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
                *address = callers[i] + callsite->second; return anomaly::sdk::Ok();
            });
        if (!resolved) return false;
        targets[i + 3] = resolved.address;
    }
    const std::array<void*, 7> detours{
        reinterpret_cast<void*>(&CutsceneExec<0>), reinterpret_cast<void*>(&CutsceneExec<1>),
        reinterpret_cast<void*>(&CutsceneExec<2>), reinterpret_cast<void*>(&CutscenePlayback<3, false>),
        reinterpret_cast<void*>(&CutscenePlayback<4, false>), reinterpret_cast<void*>(&CutscenePlayback<5, true>),
        reinterpret_cast<void*>(&CutscenePlayback<6, true>)};
    for (std::size_t i = 0; i < targets.size(); ++i) {
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = targets[i]; request.detour = detours[i];
        request.label = anomaly::sdk::StringView(i < 3 ? entry_names[i] : leaves[i - 3]);
        auto& hook = g_cutscenes.hooks[i];
        if (g_context.hooks->create(g_context.hooks->user, &request, &hook.original, &hook.handle).code != ANOMALY_STATUS_V1_OK ||
            !hook.original || !hook.handle.id) {
            static_cast<void>(ReleaseCutsceneHooks());
            return false;
        }
    }
    return true;
}

void UpdateCutscenes() {
    g_cutscenes.game_thread.store(GetCurrentThreadId(), std::memory_order_release);
    if (g_cutscenes.pending) {
        g_cutscenes.pending = false;
        bool ready{};
        try { ready = InstallCutsceneHooks(); } catch (...) { static_cast<void>(ReleaseCutsceneHooks()); }
        g_cutscenes.ready.store(ready, std::memory_order_release);
        g_cutscenes.profile = {};
        if (g_context.core && g_context.core->log)
            g_context.core->log(g_context.core->user, ready ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView(ready ? "SummonLab: instance cutscene control ready" : "SummonLab: instance cutscene control unavailable"));
    }
    // Bounded weak-identity cleanup, including actors removed from management.
    for (std::size_t i = 0, count = std::min<std::size_t>(16, g_cutscenes.sources.size()); i < count; ++i) {
        auto it = g_cutscenes.sources.upper_bound(g_cutscenes.cleanup_after);
        if (it == g_cutscenes.sources.end()) it = g_cutscenes.sources.begin();
        g_cutscenes.cleanup_after = it->first;
        AnomalyGenerationHandleV1 actual{};
        if (!HandleFromPointer(it->first, &actual) || actual.id != it->second.id || actual.generation != it->second.generation)
            g_cutscenes.sources.erase(it);
    }
}

void ResetCutscenes() {
    g_cutscenes.profile = {};
    g_cutscenes.pending = false;
    g_cutscenes.ready.store(false, std::memory_order_release);
    g_cutscenes.game_thread.store(0, std::memory_order_release);
    g_cutscenes.settings = {};
    g_cutscenes.sources.clear();
    g_cutscenes.cleanup_after = g_cutscenes.scoped_controller = 0;
}
