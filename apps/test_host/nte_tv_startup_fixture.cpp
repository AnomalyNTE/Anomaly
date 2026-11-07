#include "plugins/NteTvVideo/plugin.cpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        unsigned finds{}, invokes{};
        std::string persisted;
        const std::string saved = R"({"enabled":true,"forceMediaPlayer":true,"muteGameAudio":false,"videoPath":"C:/fixture/movie.mp4"})";
        AnomalyConfigServiceV1 config{sizeof(config)};
        config.register_schema = [](void*, AnomalyStringViewV1, std::uint32_t, AnomalyByteSpanV1,
                                    AnomalyGenerationHandleV1*) { return anomaly::sdk::Ok(); };
        struct ConfigData { const std::string& saved; std::string& persisted; } config_data{saved, persisted};
        config.user = &config_data;
        config.write_atomic = [](void* user, AnomalyStringViewV1, std::uint32_t, AnomalyByteSpanV1 bytes) {
            static_cast<ConfigData*>(user)->persisted.assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
            return anomaly::sdk::Ok();
        };
        config.read = [](void* user, AnomalyStringViewV1, std::uint32_t* version,
                         AnomalyMutableByteSpanV1 output, std::size_t* size) {
            const auto& document = static_cast<ConfigData*>(user)->saved;
            *version = 1; *size = document.size();
            if (output.data) std::memcpy(output.data, document.data(), document.size());
            return anomaly::sdk::Ok();
        };
        AnomalyUe5ObjectsServiceV1 objects{sizeof(objects)};
        objects.user = &finds;
        objects.find_exact = [](void* user, AnomalyStringViewV1, AnomalyGenerationHandleV1*) {
            ++*static_cast<unsigned*>(user);
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        };
        AnomalyUe5ProcessEventServiceV1 events{sizeof(events)};
        events.user = &invokes;
        events.invoke = [](void* user, AnomalyGenerationHandleV1, AnomalyGenerationHandleV1, void*, std::size_t) {
            ++*static_cast<unsigned*>(user);
            return anomaly::sdk::Ok();
        };
        Context context;
        context.config = &config; context.objects = &objects; context.process_events = &events;
        context.alive.store(true);
        ReadSettings(context);
        if (context.settings.enabled ||
            context.settings.video_path != "C:/fixture/movie.mp4")
            throw std::runtime_error("saved activation was restored or preferences were lost");
        UpdateMedia(&context, 1.0);
        if (finds || invokes) throw std::runtime_error("startup touched UE media services");
        context.settings.enabled = true;
        UpdateMedia(&context, 1.0);
        if (finds || invokes) throw std::runtime_error("manual enable without a television touched media services");
        if (context.audio.Read().state != nte_tv_video::AudioOutput::State::Idle)
            throw std::runtime_error("startup started audio");
        PersistSettingsTask(&context, {});
        const auto written = nlohmann::json::parse(persisted);
        if (written.at("enabled") != false || written.at("muteGameAudio") != true)
            throw std::runtime_error("legacy audio preference overrode mandatory mute");
        std::cout << "PASS: legacy activation ignored, preferences retained, startup/no-TV media calls=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
