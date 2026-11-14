#include "plugins/BetterTV/plugin.cpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
struct Fixture {
    Context context;
    AnomalyCoreServiceV1 core{sizeof(core)};
    AnomalyUe5ObjectsServiceV1 objects{sizeof(objects)};
    AnomalyUe5ProcessEventServiceV1 events{sizeof(events)};
    AnomalyUe5NamesServiceV1 names{sizeof(names)};
    std::array<std::array<std::uint8_t, 512>, 24> memory{};
    std::array<std::uintptr_t, 2> slots{};
    bool binding_test{}, mute_fails{}, bind_fails{}, stale_material{}, stale_event{};
    bool restore_material_fails{}, restore_audio_fails{};
    std::uintptr_t expected_audio{};
    std::uintptr_t texture_player{};
    std::vector<std::uintptr_t> texture_changes;
    std::vector<unsigned> calls;
    bool stale{};

    explicit Fixture(const char* profile) {
        std::ifstream input(profile);
        const auto layout = nlohmann::json::parse(input).at("layout");
        native_layout.kObjectIndexOffset = layout.at("kObjectIndexOffset");
        native_layout.kFunctionParmsSizeOffset = layout.at("kFunctionParmsSizeOffset");
        native_layout.kObjectNameOffset = layout.at("kObjectNameOffset");
        native_layout.kObjectOuterOffset = layout.at("kObjectOuterOffset");
        native_layout.kObjectClassOffset = layout.at("kObjectClassOffset");
        native_layout.kStructPropertyLinkOffset = layout.at("kStructPropertyLinkOffset");
        native_layout.kStructSuperOffset = layout.at("kStructSuperOffset");
        native_layout.kPropertyNameOffset = layout.at("kPropertyNameOffset");
        native_layout.kPropertyValueOffset = layout.at("kPropertyValueOffset");
        native_layout.kPropertySizeOffset = layout.at("kPropertySizeOffset");
        native_layout.kPropertyNextOffset = layout.at("kPropertyNextOffset");
        native_layout.kFieldClassOffset = layout.at("kFieldClassOffset");
        native_layout.kFieldClassNameOffset = layout.at("kFieldClassNameOffset");
        names.resolve_utf8 = [](void*, std::uint32_t id, char* output, std::size_t* size) {
            const std::string_view name = id == 100 ? "Parent" : id == 101 ? "ObjectProperty" :
                id == 17 ? "BP_FurnitureActorBase_TV_C" : "";
            if (name.empty() || *size <= name.size()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
            std::memcpy(output, name.data(), name.size() + 1);
            *size = name.size() + 1;
            return anomaly::sdk::Ok();
        };
        context.names = &names;
        for (unsigned i = 0; i < memory.size(); ++i) {
            const std::uint32_t index = i + 1, name = i + 10;
            std::memcpy(memory[i].data() + native_layout.kObjectIndexOffset, &index, sizeof(index));
            std::memcpy(memory[i].data() + native_layout.kObjectNameOffset, &name, sizeof(name));
        }
        Store(6, native_layout.kObjectClassOffset, Pointer(7));
        Store(0, native_layout.kObjectClassOffset, Pointer(7));
        Store(6, 256, Pointer(5));
        Store(7, native_layout.kStructPropertyLinkOffset, Pointer(8));
        Store(8, native_layout.kPropertyNameOffset, std::uint32_t{100});
        Store(8, native_layout.kPropertySizeOffset, std::int32_t{8});
        Store(8, native_layout.kPropertyValueOffset, std::int32_t{256});
        Store(8, native_layout.kFieldClassOffset, Pointer(9));
        Store(9, native_layout.kFieldClassNameOffset, std::uint32_t{101});
        const auto owner = Pointer(0);
        for (auto i : {1, 3}) std::memcpy(memory[i].data() + native_layout.kObjectOuterOffset, &owner, sizeof(owner));
        core.user = this;
        core.read_memory = [](void*, std::uintptr_t address, AnomalyMutableByteSpanV1 output) {
            std::memcpy(output.data, reinterpret_cast<void*>(address), output.size);
            return anomaly::sdk::Ok();
        };
        core.write_memory = [](void*, std::uintptr_t, AnomalyByteSpanV1) { return anomaly::sdk::Ok(); };
        objects.user = this;
        objects.snapshot_at = [](void* user, std::uint32_t index, AnomalyUe5ObjectSnapshotV1* output) {
            auto& f = *static_cast<Fixture*>(user);
            if (f.stale || (f.stale_material && index == 3) || (f.stale_event && index == 5) ||
                index < 1 || index > f.memory.size()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
            output->handle = {index, 1}; output->name_id = index + 9;
            return anomaly::sdk::Ok();
        };
        objects.snapshot_by_handle = [](void* user, AnomalyGenerationHandleV1 handle, AnomalyUe5ObjectSnapshotV1*) {
            auto& f = *static_cast<Fixture*>(user);
            return f.stale || !handle.id ? Status(ANOMALY_STATUS_V1_NOT_FOUND) : anomaly::sdk::Ok();
        };
        events.user = this;
        events.invoke = [](void* user, AnomalyGenerationHandleV1 object, AnomalyGenerationHandleV1 function, void* parameters, std::size_t size) {
            auto& f = *static_cast<Fixture*>(user);
            if (!f.context.mutex.try_lock()) throw std::runtime_error("engine invocation under plugin mutex");
            f.context.mutex.unlock();
            if (function.id == 102) throw std::runtime_error("disable replayed the whole native program");
            if (f.context.media_playing.load()) throw std::runtime_error("restore left native sound interception enabled");
            if (function.id == 105) *static_cast<std::int32_t*>(parameters) = static_cast<std::int32_t>(f.slots.size());
            if (function.id == 106) {
                auto& material = *static_cast<ScreenMaterialParameters*>(parameters);
                material.material = f.slots.at(material.slot);
            }
            if (function.id == 107 && f.mute_fails) return Status(ANOMALY_STATUS_V1_FAILED);
            if (function.id == 109) {
                *static_cast<std::uintptr_t*>(parameters) = f.texture_player;
                return anomaly::sdk::Ok();
            }
            if (function.id == 108) {
                f.texture_player = *static_cast<const std::uintptr_t*>(parameters);
                f.texture_changes.push_back(f.texture_player);
            }
            if (function.id == 103 && f.restore_audio_fails) return Status(ANOMALY_STATUS_V1_FAILED);
            if (function.id == 101) {
                if (f.restore_material_fails) return Status(ANOMALY_STATUS_V1_FAILED);
                const auto& material = *static_cast<const ScreenMaterialParameters*>(parameters);
                if (object.id != 2 || size != sizeof(material) || material.slot != 1)
                    throw std::runtime_error("restore changed the wrong screen slot");
                if (!f.binding_test && material.material != f.Pointer(2))
                    throw std::runtime_error("restore changed the original material");
                if (f.bind_fails && material.material == f.Pointer(5)) return Status(ANOMALY_STATUS_V1_FAILED);
                f.slots.at(material.slot) = material.material;
            }
            if (function.id == 103 && (object.id != 4 || size != 8 ||
                *static_cast<std::uintptr_t*>(parameters) != f.expected_audio))
                throw std::runtime_error("restore used the wrong native audio event");
            f.calls.push_back(static_cast<unsigned>(function.id));
            return anomaly::sdk::Ok();
        };
        context.core = &core; context.objects = &objects; context.process_events = &events;
        context.tv_owner = Pointer(0); context.tv_owner_handle = {1, 1};
        context.tv_component = Pointer(3);
        context.play_audio.handle = {1, 1};
        context.media_functions.emplace("/Script/Engine.PrimitiveComponent:SetMaterial", std::make_pair(AnomalyGenerationHandleV1{101, 1}, 16));
        context.media_functions.emplace("/Script/HTGame.MovieAudioPlayerComponent:PlayByConfig", std::make_pair(AnomalyGenerationHandleV1{102, 1}, 8));
        context.media_functions.emplace("/Script/HTGame.MovieAudioPlayerComponent:PlayAudio", std::make_pair(AnomalyGenerationHandleV1{103, 1}, 8));
        context.media_functions.emplace("/Script/Engine.PrimitiveComponent:GetNumMaterials", std::make_pair(AnomalyGenerationHandleV1{105, 1}, 4));
        context.media_functions.emplace("/Script/Engine.PrimitiveComponent:GetMaterial", std::make_pair(AnomalyGenerationHandleV1{106, 1}, 16));
        context.media_functions.emplace("/Script/HTGame.MovieAudioPlayerComponent:StopAudio", std::make_pair(AnomalyGenerationHandleV1{107, 1}, 0));
        context.media_functions.emplace(kTextureSetPlayer, std::make_pair(AnomalyGenerationHandleV1{108, 1}, 8));
        context.media_functions.emplace(kTextureGetPlayer, std::make_pair(AnomalyGenerationHandleV1{109, 1}, 8));
        context.media_functions.emplace(kTextureUpdateResource, std::make_pair(AnomalyGenerationHandleV1{120, 1}, 0));
        Prepare();
    }
    std::uintptr_t Pointer(unsigned i) { return reinterpret_cast<std::uintptr_t>(memory[i].data()); }
    template<class T> void Store(unsigned object, unsigned offset, T value) {
        std::memcpy(memory[object].data() + offset, &value, sizeof(value));
    }
    void Prepare() {
        calls.clear(); stale = false;
        texture_player = 0;
        texture_changes.clear();
        stale_material = stale_event = false;
        context.native_audio_pending = {};
        context.tv_owner = Pointer(0);
        context.tv_owner_handle = {1, 1};
        context.tv_component = Pointer(3);
        context.television_audio.clear();
        expected_audio = Pointer(4);
        context.tv_binding = {Pointer(4), Pointer(0), Pointer(3), Pointer(1), Pointer(2),
            {1, 1}, {4, 1}, {2, 1}, {3, 1}, 1};
        context.tv_audio = {Pointer(3), Pointer(4), {4, 1}, {5, 1}};
        context.media_player = {6, 1}; context.media_close = {104, 1};
        context.opened_path = L"C:/fixture/movie.mp4";
        context.media_playing.store(true); context.game_audio_muted = true;
    }
};

struct SeekFixture : Fixture {
    enum Call : unsigned { Close = 104, GetMaterial = 106, SetTexture = 108, GetTexture,
        SetLooping, Open, HasError, IsReady, IsBuffering, IsPlaying, Seek, GetTime, DetachTexture,
        RefreshTexture = 120 };
    decltype(AnomalyUe5ProcessEventServiceV1::invoke) restore_call{};
    std::vector<unsigned> operations;
    std::int64_t position{};
    bool closed{}, binding_readback_fails{};

    explicit SeekFixture(const char* profile) : Fixture(profile) {
        auto& c = context;
        c.opened_world = c.tv_binding.world;
        c.tv_binding.local_material = Pointer(5);
        c.tv_binding.texture = Pointer(10);
        c.tv_binding.texture_handle = {11, 1};
        c.media_player_pointer = Pointer(5);
        c.resume_position = 3 * kTicksPerSecond;
        c.media_duration = 12 * kTicksPerSecond;
        c.media_seekable = true;
        c.progress = {12, c.resume_position, c.media_duration, true};
        slots = {Pointer(10), Pointer(5)};
        texture_player = c.media_player_pointer;
        c.media_functions.emplace("/Script/MediaAssets.MediaPlayer:Seek",
            std::make_pair(AnomalyGenerationHandleV1{Seek, 1}, 9));
        c.media_functions.emplace("/Script/MediaAssets.MediaPlayer:GetTime",
            std::make_pair(AnomalyGenerationHandleV1{GetTime, 1}, 8));
        objects.find_exact = [](void*, AnomalyStringViewV1 path, AnomalyGenerationHandleV1* output) {
            constexpr std::pair<std::string_view, Call> functions[] = {
                {"SetLooping", SetLooping}, {"OpenFile", Open},
                {"HasError", HasError}, {"IsReady", IsReady}, {"IsBuffering", IsBuffering},
                {"IsPlaying", IsPlaying},
            };
            const std::string_view name(path.data, path.size);
            for (const auto& [suffix, id] : functions) {
                if (name.ends_with(suffix)) {
                    *output = {id, 1};
                    return anomaly::sdk::Ok();
                }
            }
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        };
        restore_call = events.invoke;
        events.user = this;
        events.invoke = [](void* user, AnomalyGenerationHandleV1 object, AnomalyGenerationHandleV1 function,
                          void* parameters, std::size_t size) {
            auto& f = *static_cast<SeekFixture*>(user);
            if (!f.context.media_playing.load())
                return f.restore_call(static_cast<Fixture*>(&f), object, function, parameters, size);
            switch (function.id) {
            case GetMaterial:
                static_cast<ScreenMaterialParameters*>(parameters)->material = f.slots[1];
                break;
            case GetTexture:
                *static_cast<std::uintptr_t*>(parameters) = f.texture_player;
                break;
            case SetTexture: {
                const auto player = *static_cast<std::uintptr_t*>(parameters);
                if (!player || !f.binding_readback_fails) f.texture_player = player;
                f.operations.push_back(player ? SetTexture : DetachTexture);
                break;
            }
            case Close:
                if (f.texture_player) throw std::runtime_error("closed video while the old texture was still attached");
                f.closed = true;
                f.operations.push_back(Close);
                break;
            case SetLooping: break;
            case Open: {
                const auto& path = *static_cast<const PassedString*>(parameters);
                if (!f.closed || std::wstring_view(path.data) != f.context.opened_path)
                    throw std::runtime_error("reopen did not use the same closed media session");
                f.closed = false;
                static_cast<std::uint8_t*>(parameters)[sizeof(PassedString)] = 1;
                f.operations.push_back(Open);
                break;
            }
            case HasError: case IsBuffering: *static_cast<std::uint8_t*>(parameters) = 0; break;
            case IsReady: case IsPlaying: *static_cast<std::uint8_t*>(parameters) = !f.closed; break;
            case Seek:
                if (f.closed || f.texture_player)
                    throw std::runtime_error("seek ran before reopen or on the stale texture");
                f.position = *static_cast<const std::int64_t*>(parameters);
                static_cast<std::uint8_t*>(parameters)[sizeof(std::int64_t)] = 1;
                f.operations.push_back(Seek);
                break;
            case GetTime: *static_cast<std::int64_t*>(parameters) = f.position; break;
            case RefreshTexture:
                if (f.texture_player != f.context.media_player_pointer)
                    throw std::runtime_error("render resource was refreshed before binding the new source");
                f.operations.push_back(RefreshTexture);
                break;
            default: throw std::runtime_error("seek invoked an unrelated television operation");
            }
            return anomaly::sdk::Ok();
        };
    }

    void Tick(MediaClock::time_point now) {
        if (!UpdatePlaybackSeek(context, context.tv_binding.world, now))
            throw std::runtime_error("seek did not consume its Game update");
    }
};

void VerifySeekRebuild(const char* profile) {
    using namespace std::chrono_literals;
    const auto start = MediaClock::now();
    SeekFixture f(profile);
    auto& c = f.context;
    c.seek_request = {11, 2 * kTicksPerSecond, start};
    if (UpdatePlaybackSeek(c, c.tv_binding.world, start) || !f.operations.empty())
        throw std::runtime_error("stale seek touched the active player");
    c.seek_request = {12, c.media_duration - 1, start};
    f.Tick(start);
    f.Tick(start + 100ms);
    f.Tick(start + 200ms);
    const std::vector<unsigned> rebuilt{SeekFixture::DetachTexture, SeekFixture::Close,
        SeekFixture::Open, SeekFixture::Seek, SeekFixture::SetTexture, SeekFixture::RefreshTexture};
    if (f.operations != rebuilt || !c.game_audio_muted || f.position != 117'500'000)
        throw std::runtime_error("seek did not rebuild once or clamp away from EOF");
    c.seek_request = {12, 8 * kTicksPerSecond, start + 300ms};
    for (auto offset : {300ms, 400ms, 500ms}) f.Tick(start + offset);
    if (!c.playback_seek || !c.seek_request || c.resume_position != 3 * kTicksPerSecond ||
        c.audio.Read().state != better_tv_video::AudioOutput::State::Idle || f.operations != rebuilt)
        throw std::runtime_error("frozen target clock resumed audio or issued a second seek");
    static_cast<void>(c.audio.Shutdown(0ms));  // The fixture has no actual media file.
    f.position = 118'500'000;
    f.Tick(start + 600ms);
    if (!c.playback_seek) throw std::runtime_error("seek resumed after only one advancing sample");
    f.position = 200'000;
    f.Tick(start + 700ms);
    if (c.playback_seek || !c.seek_request || c.resume_position != 200'000 || f.operations != rebuilt)
        throw std::runtime_error("valid loop did not complete the seek or retained the wrong pending target");
    f.Tick(start + 800ms);
    StopTakeover(c, 0, false);
    if (c.playback_seek || c.seek_request || !c.opened_path.empty() ||
        f.calls != std::vector<unsigned>{SeekFixture::Close})
        throw std::runtime_error("scene change retained the in-flight seek or touched the old television");

    SeekFixture timeout(profile);
    timeout.context.seek_request = {12, 8 * kTicksPerSecond, start};
    timeout.Tick(start);
    timeout.Tick(start + 100ms);
    timeout.Tick(start + 200ms);
    timeout.Tick(start + 3200ms);
    timeout.Tick(start + 3300ms);
    timeout.Tick(start + 3400ms);
    if (!timeout.context.playback_seek || !timeout.context.playback_seek->recovering ||
        timeout.position != 3 * kTicksPerSecond || timeout.operations.size() != 2 * rebuilt.size())
        throw std::runtime_error("frozen clock did not reopen at the previous position exactly once");
    static_cast<void>(timeout.context.audio.Shutdown(0ms));
    timeout.Tick(start + 3500ms);
    timeout.position += 1'000'000;
    timeout.Tick(start + 3600ms);
    timeout.position += 1'000'000;
    timeout.Tick(start + 3700ms);
    if (timeout.context.playback_seek || timeout.context.resume_position != 32'000'000 ||
        !timeout.context.game_audio_muted)
        throw std::runtime_error("recovered playback did not keep the original takeover");

    SeekFixture failure(profile);
    failure.binding_readback_fails = true;
    failure.context.seek_request = {12, 8 * kTicksPerSecond, start};
    for (auto offset : {0ms, 100ms, 200ms, 300ms, 400ms}) failure.Tick(start + offset);
    if (!failure.context.takeover_failed || failure.context.playback_seek || failure.context.game_audio_muted ||
        failure.calls != std::vector<unsigned>{101, 104, 103})
        throw std::runtime_error("failed texture recovery did not restore the original television");

    SeekFixture resumed(profile);
    auto& resume_context = resumed.context;
    resumed.position = 60'000'000;
    ConfirmAutomaticResume(resume_context, resume_context.tv_binding.world, resumed.position, true, start);
    resumed.Tick(start + 100ms);
    if (!resume_context.playback_seek || !resumed.operations.empty())
        throw std::runtime_error("automatic resume restarted an already open video or accepted one clock sample");
    static_cast<void>(resume_context.audio.Shutdown(0ms));
    resumed.position += 1'000'000;
    resumed.Tick(start + 200ms);
    resumed.position += 1'000'000;
    resumed.Tick(start + 300ms);
    if (resume_context.playback_seek || resume_context.resume_position != 62'000'000 ||
        !resumed.operations.empty() || !resume_context.game_audio_muted)
        throw std::runtime_error("automatic resume failed to confirm the newly bound video clock");

    ObserveNativeProgram(resume_context, resumed.Pointer(3), 42);
    const auto programme_observed = MediaClock::now();
    if (!NativeProgramIsChanging(resume_context, resumed.Pointer(3), programme_observed) ||
        NativeProgramIsChanging(resume_context, resumed.Pointer(1), programme_observed) ||
        NativeProgramIsChanging(resume_context, resumed.Pointer(3), programme_observed + 1s))
        throw std::runtime_error("native program grace period escaped the active television or did not expire");
}
}

#include "better_tv_selection_tests.hpp"

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("profile path required");
        Fixture fixture(argv[1]);
        for (unsigned i = 0; i < 10; ++i) {
            fixture.Prepare();
            RestoreTelevision(fixture.context);
            if (fixture.calls != std::vector<unsigned>{101, 104, 103})
                throw std::runtime_error("screen/close/native-audio restore order incorrect");
            if (!fixture.context.opened_path.empty() || fixture.context.game_audio_muted ||
                fixture.context.tv_binding.slot >= 0) throw std::runtime_error("takeover state remained active");
            RestoreTelevision(fixture.context);
            if (fixture.calls.size() != 3) throw std::runtime_error("restore repeated engine operations");
        }
        fixture.Prepare(); fixture.stale = true;
        RestoreTelevision(fixture.context);
        if (fixture.calls != std::vector<unsigned>{104}) throw std::runtime_error("stale television received restore calls");
        fixture.Prepare(); fixture.context.tv_binding = {}; fixture.context.game_audio_muted = false;
        RestoreTelevision(fixture.context);
        if (fixture.calls != std::vector<unsigned>{104}) throw std::runtime_error("preparing video restored an unbound television");
        fixture.Prepare(); fixture.context.alive.store(true);
        UpdateMedia(&fixture.context, 1.0); // No current world: only local media may be closed.
        if (fixture.calls != std::vector<unsigned>{104}) throw std::runtime_error("world exit restored an old television");
        UpdateMedia(&fixture.context, 1.0);
        if (fixture.calls.size() != 1) throw std::runtime_error("disabled update repeated close");
        fixture.Prepare();
        auto& context = fixture.context;
        context.settings.enabled = true;
        context.override_path = context.selected_path = context.opened_path;
        context.resume_position = 3 * kTicksPerSecond;
        context.progress = {12, context.resume_position, 12 * kTicksPerSecond, true};
        AnomalyUiServiceV1 ui{sizeof(ui)};
        ui.text = [](void*, AnomalyStringViewV1) {};
        float dragged = 0.25f;
        ui.user = &dragged;
        ui.slider_float_enabled = [](void* user, AnomalyStringViewV1, float* value, float, float, int enabled) {
            *value = *static_cast<float*>(user);
            return enabled;
        };
        DrawProgress(context, &ui);
        dragged = 0.75f;
        DrawProgress(context, &ui);
        if (!context.seek_request || context.seek_request->target != 9 * kTicksPerSecond ||
            context.seek_request->due <= MediaClock::now() || !fixture.calls.empty())
            throw std::runtime_error("progress control failed to coalesce without invoking the engine");
        UpdateMedia(&context, 1.0);
        if (!context.settings.enabled || context.resume_position != 3 * kTicksPerSecond ||
            context.seek_request || context.progress.seekable || fixture.calls != std::vector<unsigned>{104})
            throw std::runtime_error("world loss changed activation, lost progress, or retained an old seek");
        context.override_path = L"C:/fixture/other.mp4";
        UpdateMedia(&context, 1.0);
        if (context.resume_position != 0) throw std::runtime_error("new source inherited previous progress");
        fixture.Prepare();
        fixture.binding_test = true;
        context.media_player_pointer = fixture.Pointer(5);
        auto binding = context.tv_binding;
        context.tv_binding = {};
        context.media_playing.store(false);
        context.game_audio_muted = false;
        binding.slot = -1;
        fixture.slots = {fixture.Pointer(10), fixture.Pointer(2)};
        bool residue{};
        if (FindScreenSlot(context, binding, fixture.Pointer(2), fixture.Pointer(5), residue) != BindResult::Success ||
            residue || binding.slot != 1 || binding.material != fixture.Pointer(2))
            throw std::runtime_error("native screen was not identified");

        context.last_native_screen = binding;
        binding.slot = -1;
        fixture.slots[1] = fixture.Pointer(6); // Dynamic instance whose reflected Parent is the local material.
        if (FindScreenSlot(context, binding, fixture.Pointer(2), fixture.Pointer(5), residue) != BindResult::Success ||
            !residue || binding.material != fixture.Pointer(6))
            throw std::runtime_error("residual material instance was not recognized");
        if (!ResolveOriginalScreen(context, binding, 0, true) || binding.material != fixture.Pointer(2))
            throw std::runtime_error("valid original material record was not reused");
        context.last_native_screen = {};
        if (ResolveOriginalScreen(context, binding, 0, true))
            throw std::runtime_error("missing native asset was accepted");
        fixture.slots[0] = fixture.Pointer(2);
        auto ambiguous = binding;
        ambiguous.slot = -1;
        if (FindScreenSlot(context, ambiguous, fixture.Pointer(2), fixture.Pointer(5), residue) != BindResult::ScreenUnavailable)
            throw std::runtime_error("ambiguous screen slots were accepted");
        fixture.slots[0] = fixture.Pointer(10);
        fixture.calls.clear();
        fixture.mute_fails = true;
        if (ApplyScreenBinding(context, binding, fixture.Pointer(5), fixture.Pointer(5), true) != BindResult::MuteFailed ||
            fixture.slots != std::array{fixture.Pointer(10), fixture.Pointer(2)} || context.game_audio_muted)
            throw std::runtime_error("mute failure left local screen installed");

        fixture.mute_fails = false;
        fixture.bind_fails = true;
        fixture.texture_player = fixture.Pointer(5);
        if (ApplyScreenBinding(context, binding, fixture.Pointer(5), fixture.Pointer(5), false) != BindResult::BindFailed ||
            !context.game_audio_muted ||
            fixture.texture_changes != std::vector<std::uintptr_t>{0, fixture.Pointer(5)})
            throw std::runtime_error("reopened media retained its old texture queue or lost audio restoration");
        fixture.calls.clear();
        fixture.stale_material = true;
        RestoreTelevision(context);
        if (fixture.calls != std::vector<unsigned>{108, 104, 103} || fixture.texture_player ||
            fixture.slots[0] != fixture.Pointer(10))
            throw std::runtime_error("stale original screen blocked texture detach or audio restoration");

        fixture.Prepare();
        AnomalyHookServiceV1 hook{sizeof(hook)};
        hook.create = [](void*, const AnomalyHookRequestV1*, std::uintptr_t*, AnomalyGenerationHandleV1*) { return anomaly::sdk::Ok(); };
        hook.release = [](void*, AnomalyGenerationHandleV1) { return anomaly::sdk::Ok(); };
        hook.begin_callback = [](void*, AnomalyGenerationHandleV1, AnomalyGenerationHandleV1*) { return anomaly::sdk::Ok(); };
        hook.end_callback = [](void*, AnomalyGenerationHandleV1) { return anomaly::sdk::Ok(); };
        context.hook = &hook;
        static unsigned forwarded{};
        context.play_audio.original = reinterpret_cast<std::uintptr_t>(+[](void*, void*) { ++forwarded; });
        const auto play = [&](unsigned component, std::uintptr_t event) {
            PlayAudioDetour(reinterpret_cast<void*>(fixture.Pointer(component)), reinterpret_cast<void*>(event));
        };
        g_active.store(&context);
        context.resume_position = 3 * kTicksPerSecond;
        context.progress = {30, context.resume_position, 12 * kTicksPerSecond, true};
        play(3, 0);
        play(1, fixture.Pointer(5)); // Same TV class shape, different receiver.
        if (forwarded != 1 || context.tv_audio.event != fixture.Pointer(4))
            throw std::runtime_error("empty or unrelated events replaced the muted television record");
        fixture.stale_event = true;
        play(3, fixture.Pointer(4));
        if (forwarded != 1 || NeedsNativeAudio(context) || context.opened_path.empty() ||
            context.progress.generation != 30 || context.progress.position != 3 * kTicksPerSecond)
            throw std::runtime_error("stale event interrupted playback or escaped mute");
        RestoreTelevision(context);
        if (fixture.calls != std::vector<unsigned>{101, 104} || !context.native_audio_pending.id || !NeedsNativeAudio(context))
            throw std::runtime_error("stale event was replayed or did not block fresh takeover");
        play(3, fixture.Pointer(5));
        if (forwarded != 2 || context.native_audio_pending.id || NeedsNativeAudio(context))
            throw std::runtime_error("next native event did not clear the pending restoration");

        fixture.Prepare();
        ObserveNativeProgram(context, fixture.Pointer(3), 42);
        RestoreTelevision(context);
        if (fixture.calls != std::vector<unsigned>{101, 104})
            throw std::runtime_error("previous program's event was replayed");
        fixture.Prepare();
        ObserveNativeProgram(context, fixture.Pointer(3), 43);
        play(3, fixture.Pointer(5));
        fixture.expected_audio = fixture.Pointer(5);
        RestoreTelevision(context);
        if (fixture.calls != std::vector<unsigned>{101, 104, 103} || context.native_audio_pending.id || forwarded != 2)
            throw std::runtime_error("current program's event was not restored");
        g_active.store(nullptr);
        VerifySeekRebuild(argv[1]);
        VerifyTelevisionSelection(argv[1]);
        VerifyTelevisionDiscovery(argv[1]);
        VerifyFailedSelectionRestore(argv[1]);

        std::cout << "PASS: television restore, seek rebuild, frozen clock, loop, cancellation and bounded recovery\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
