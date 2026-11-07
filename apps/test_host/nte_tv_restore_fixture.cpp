#include "plugins/NteTvVideo/plugin.cpp"
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
    std::array<std::array<std::uint8_t, 512>, 11> memory{};
    std::array<std::uintptr_t, 2> slots{};
    bool binding_test{}, mute_fails{}, bind_fails{}, stale_material{}, stale_event{};
    std::uintptr_t expected_audio{};
    std::vector<unsigned> calls;
    bool stale{};

    explicit Fixture(const char* profile) {
        std::ifstream input(profile);
        const auto layout = nlohmann::json::parse(input).at("layout");
        native_layout.kObjectIndexOffset = layout.at("kObjectIndexOffset");
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
            if (function.id == 101) {
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
        context.media_functions.emplace("/Script/MediaAssets.MediaTexture:SetMediaPlayer", std::make_pair(AnomalyGenerationHandleV1{108, 1}, 8));
        Prepare();
    }
    std::uintptr_t Pointer(unsigned i) { return reinterpret_cast<std::uintptr_t>(memory[i].data()); }
    template<class T> void Store(unsigned object, unsigned offset, T value) {
        std::memcpy(memory[object].data() + offset, &value, sizeof(value));
    }
    void Prepare() {
        calls.clear(); stale = false;
        stale_material = stale_event = false;
        context.native_audio_pending = {};
        expected_audio = Pointer(4);
        context.tv_binding = {Pointer(4), Pointer(0), Pointer(3), Pointer(1), Pointer(2),
            {1, 1}, {4, 1}, {2, 1}, {3, 1}, 1};
        context.tv_audio = {Pointer(3), Pointer(4), {4, 1}, {5, 1}};
        context.media_player = {6, 1}; context.media_close = {104, 1};
        context.opened_path = L"C:/fixture/movie.mp4";
        context.media_playing.store(true); context.game_audio_muted = true;
    }
};
}

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
        if (ApplyScreenBinding(context, binding, fixture.Pointer(5), fixture.Pointer(5), false) != BindResult::BindFailed ||
            !context.game_audio_muted)
            throw std::runtime_error("binding failure lost the audio restoration record");
        fixture.calls.clear();
        fixture.stale_material = true;
        RestoreTelevision(context);
        if (fixture.calls != std::vector<unsigned>{104, 103} || fixture.slots[0] != fixture.Pointer(10))
            throw std::runtime_error("stale original screen blocked independent audio restoration");

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

        std::cout << "PASS: 10 restore cycles, exact material/audio, no program replay, stale/no-world/preparing isolation, residual screen and failure rollback\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
