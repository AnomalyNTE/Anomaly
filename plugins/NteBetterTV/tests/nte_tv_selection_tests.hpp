#pragma once

// Uses the restoration fixture's independent object memory and call recorder.
void VerifyTelevisionSelection(const char* profile) {
    Fixture f(profile);
    auto& c = f.context;
    const auto world = f.Pointer(4);
    f.Store(0, native_layout.kObjectOuterOffset, world);
    f.Store(6, native_layout.kObjectOuterOffset, world);
    f.Store(10, native_layout.kObjectOuterOffset, f.Pointer(6));
    c.tv_world = c.opened_world = c.discovery_world = world;
    c.discovery_world_handle = {5, 1};
    c.media_functions.emplace("/Script/HTGame.MovieAudioPlayerComponent:IsPlaying",
        std::make_pair(AnomalyGenerationHandleV1{121, 1}, 1));
    const auto base_invoke = f.events.invoke;
    // The fixture is a single synchronous Game-thread consumer.
    static decltype(f.events.invoke) restore_invoke;
    restore_invoke = base_invoke;
    f.events.invoke = [](void* user, AnomalyGenerationHandleV1 object,
                         AnomalyGenerationHandleV1 function, void* parameters, std::size_t size) {
        if (function.id == 121) {
            *static_cast<std::uint8_t*>(parameters) = object.id == 4 ? 1 : 0;
            return anomaly::sdk::Ok();
        }
        return restore_invoke(user, object, function, parameters, size);
    };
    c.resume_position = 7 * kTicksPerSecond;
    TelevisionCandidate a{world, f.Pointer(0), f.Pointer(3), {5, 1}, {1, 1}, {4, 1}, "A", true};
    TelevisionCandidate b{world, f.Pointer(6), f.Pointer(10), {5, 1}, {7, 1}, {11, 1}, "B", false};
    c.televisions = {a, b};
    c.television_audio[f.Pointer(3)] = c.tv_audio;
    ObserveNativeProgram(c, f.Pointer(10), 82);
    ObserveNativeAudio(c, f.Pointer(10), f.Pointer(5));
    if (c.tv_audio.component != f.Pointer(3) || c.tv_audio.event != f.Pointer(4))
        throw std::runtime_error("unselected TV audio replaced the active TV event");
    const auto select = [&](TelevisionCandidate request) {
        c.television_request = request;
        c.tv_discovery_seconds = 0;
        return FindTelevision(c, world, 0);
    };
    select(b);
    if (c.tv_owner != b.owner || c.tv_component != b.component || c.tv_binding.slot >= 0 ||
        c.tv_audio.component != b.component || c.tv_audio.event != f.Pointer(5) ||
        c.resume_position != 7 * kTicksPerSecond || c.media_playing ||
        f.calls != std::vector<unsigned>{101, 104, 103})
        throw std::runtime_error("selecting B did not restore live A before switching and preserving progress");
    // B is off: selecting it must not start its native program or open local media.
    f.calls.clear();
    c.televisions = {b, a};
    select(b);
    if (!f.calls.empty() || c.tv_owner != b.owner)
        throw std::runtime_error("candidate reorder restarted the selected TV");
    auto stale = a;
    stale.owner_handle.generation = 2;
    select(stale);
    if (c.tv_owner != b.owner) throw std::runtime_error("stale generation selected a different TV");
    stale = a; stale.world_handle.generation = 2;
    select(stale);
    if (c.tv_owner != b.owner) throw std::runtime_error("stale world selection was accepted");
    f.Store(0, native_layout.kObjectOuterOffset, std::uintptr_t{});
    select(a);
    if (c.tv_owner != b.owner) throw std::runtime_error("actor from another world was selected");
    f.Store(0, native_layout.kObjectOuterOffset, world);
    select(a);
    if (c.tv_owner != a.owner || c.tv_audio.event != f.Pointer(4))
        throw std::runtime_error("returning to A used B's native audio event");
    // A derived class is accepted, while unrelated components remain untouched.
    f.Store(9, native_layout.kObjectNameOffset, std::uint32_t{99});
    f.Store(9, native_layout.kStructSuperOffset, f.Pointer(7));
    f.Store(6, native_layout.kObjectClassOffset, f.Pointer(9));
    if (!IsTvComponent(c, reinterpret_cast<void*>(b.component)))
        throw std::runtime_error("derived television was rejected by the audio hook filter");
    f.Store(9, native_layout.kStructSuperOffset, std::uintptr_t{});
    if (IsTvComponent(c, reinterpret_cast<void*>(b.component)))
        throw std::runtime_error("unrelated actor was treated as a television");
    f.calls.clear();
    c.television_request = b;
    FindTelevision(c, 0, 0);
    if (c.tv_owner || c.displayed_selection.id || c.television_request ||
        !c.television_display.empty() || !c.television_audio.empty() || !f.calls.empty())
        throw std::runtime_error("world exit retained targets or called stale objects");

    Fixture unloaded(profile);
    auto& u = unloaded.context;
    u.discovery_world = u.tv_world = u.tv_binding.world;
    u.discovery_world_handle = {5, 1};
    u.televisions = {a};
    unloaded.stale = true;
    FindTelevision(u, u.discovery_world, 0);
    if (u.tv_owner || u.tv_binding.slot >= 0 || u.media_playing)
        throw std::runtime_error("unloaded objects retained an active takeover");
}

void VerifyTelevisionDiscovery(const char* profile) {
    Fixture f(profile);
    auto& c = f.context;
    const auto world = f.Pointer(4);
    // Independent reflection fixture: two live TV instances share the same class.
    f.names.resolve_utf8 = [](void*, std::uint32_t id, char* output, std::size_t* size) {
        const std::string_view name = id == 100 ? "MovieAudioPlayer" : id == 101 ? "ObjectProperty" :
            id == 17 ? "BP_FurnitureActorBase_TV_C" : id == 21 ? "MovieAudioPlayerComponent" : "TV";
        if (*size <= name.size()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        std::memcpy(output, name.data(), name.size()); output[name.size()] = 0;
        *size = name.size() + 1;
        return anomaly::sdk::Ok();
    };
    f.Store(0, native_layout.kObjectOuterOffset, world);
    f.Store(6, native_layout.kObjectOuterOffset, world);
    f.Store(0, 256, f.Pointer(3));
    f.Store(6, 256, f.Pointer(10));
    f.Store(10, native_layout.kObjectOuterOffset, f.Pointer(6));
    f.Store(3, native_layout.kObjectClassOffset, f.Pointer(11));
    f.Store(10, native_layout.kObjectClassOffset, f.Pointer(11));
    c.media_functions.emplace("/Script/HTGame.MovieAudioPlayerComponent:IsPlaying",
        std::make_pair(AnomalyGenerationHandleV1{121, 1}, 1));
    f.events.invoke = [](void*, AnomalyGenerationHandleV1 object, AnomalyGenerationHandleV1 function,
                        void* parameters, std::size_t) {
        if (function.id != 121) throw std::runtime_error("discovery changed game state");
        *static_cast<std::uint8_t*>(parameters) = object.id == 4 ? 1 : 0;
        return anomaly::sdk::Ok();
    };
    const std::array<std::uintptr_t, 3> actors{f.Pointer(0), f.Pointer(6), f.Pointer(5)};
    TelevisionArray result{reinterpret_cast<std::uintptr_t>(actors.data()), 3, 3};
    std::vector<TelevisionCandidate> candidates;
    if (!ReadTelevisionArray(c, world, result, candidates) || candidates.size() != 2 ||
        candidates[0].owner != f.Pointer(0) || candidates[1].owner != f.Pointer(6) ||
        !candidates[0].playing || candidates[1].playing || candidates[1].component != f.Pointer(10))
        throw std::runtime_error("discovery did not retain both TVs and their independent playing state");
    result.count = 4;
    candidates.clear();
    if (ReadTelevisionArray(c, world, result, candidates) || !candidates.empty())
        throw std::runtime_error("invalid engine array count was read");
    f.Store(6, native_layout.kObjectOuterOffset, std::uintptr_t{});
    result.count = 3;
    if (!ReadTelevisionArray(c, world, result, candidates) || candidates.size() != 1)
        throw std::runtime_error("discovery included a TV from a detached world");
}

void VerifyFailedSelectionRestore(const char* profile) {
    for (bool audio_failure : {false, true}) {
        Fixture f(profile);
        auto& c = f.context;
        const auto world = f.Pointer(4);
        f.Store(0, native_layout.kObjectOuterOffset, world);
        f.Store(6, native_layout.kObjectOuterOffset, world);
        c.media_functions.emplace("/Script/HTGame.MovieAudioPlayerComponent:IsPlaying",
            std::make_pair(AnomalyGenerationHandleV1{121, 1}, 1));
        static decltype(f.events.invoke) original;
        original = f.events.invoke;
        f.events.invoke = [](void* user, AnomalyGenerationHandleV1 object, AnomalyGenerationHandleV1 function,
                            void* parameters, std::size_t size) {
            if (function.id == 121) { *static_cast<std::uint8_t*>(parameters) = 1; return anomaly::sdk::Ok(); }
            return original(user, object, function, parameters, size);
        };
        TelevisionCandidate b{world, f.Pointer(6), f.Pointer(10), {5, 1}, {7, 1}, {11, 1}, "B", false};
        c.televisions = {b};
        f.restore_material_fails = !audio_failure;
        f.restore_audio_fails = audio_failure;
        if (SelectTelevision(c, world, b) || c.tv_owner != f.Pointer(0) || c.tv_binding.slot < 0 ||
            !c.opened_path.empty()) throw std::runtime_error("failed restore committed B or lost A recovery");
        const auto before = f.calls;
        f.restore_material_fails = f.restore_audio_fails = false;
        f.calls.clear();
        if (!SelectTelevision(c, world, b) || c.tv_owner != b.owner || c.tv_binding.slot >= 0 ||
            f.calls != std::vector<unsigned>{audio_failure ? 103u : 101u})
            throw std::runtime_error("restore retry repeated completed operations or did not release A");
    }
}
