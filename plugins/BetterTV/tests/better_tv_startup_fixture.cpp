#include "plugins/BetterTV/plugin.cpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        // Exercise the actual exported entry against the installed SDK's capacity.
        struct DescriptorBuffer {
            AnomalyPluginDescriptorV1 descriptor{};
            std::array<unsigned char, 16> sentinel;
        } buffer;
        buffer.sentinel.fill(0xA5);
        buffer.descriptor.struct_size = sizeof(buffer.descriptor) - 1;
        if (AnomalyPluginEntryV1(&buffer.descriptor).code != ANOMALY_STATUS_V1_INVALID_ARGUMENT)
            throw std::runtime_error("undersized descriptor accepted");
        buffer.descriptor.struct_size = sizeof(buffer.descriptor);
        if (AnomalyPluginEntryV1(&buffer.descriptor).code != ANOMALY_STATUS_V1_OK ||
            buffer.descriptor.on_stop != Stop || buffer.descriptor.on_update != UpdateMedia ||
            !std::all_of(buffer.sentinel.begin(), buffer.sentinel.end(),
                         [](unsigned char value) { return value == 0xA5; }))
            throw std::runtime_error("SDK descriptor entry or capacity boundary is invalid");
        unsigned finds{}, invokes{};
        std::string persisted;
        std::string saved = R"({"enabled":true,"forceMediaPlayer":true,"muteGameAudio":false,"videoPath":"C:/fixture/movie.mp4","bilibiliUrl":"https://www.bilibili.com/video/BV1xx411c7mD?p=2"})";
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
        if (context.settings.bilibili_url !=
                "https://www.bilibili.com/video/BV1xx411c7mD?p=2")
            throw std::runtime_error("the last bilibili link was not restored as input content");
        const std::wstring cache = better_tv_video::DefaultMediaCacheDirectory();
        if (cache.empty() || !IsPreparedCachePath(WideToUtf8(cache) + "/bv_fixture_p1.mp4") ||
            IsPreparedCachePath("C:/fixture/movie.mp4"))
            throw std::runtime_error("prepared cache paths are not distinguished from local sources");
        if (context.preparer.Started())
            throw std::runtime_error("startup created the preparation worker");
        if (context.settings.audio_distance.full_volume_meters != 4.0f ||
            context.settings.audio_distance.max_distance_meters != 16.0f)
            throw std::runtime_error("legacy configuration did not load the default audio distances");
        UpdateMedia(&context, 1.0);
        if (finds || invokes) throw std::runtime_error("startup touched UE media services");
        context.settings.enabled = true;
        UpdateMedia(&context, 1.0);
        if (finds || invokes) throw std::runtime_error("manual enable without a television touched media services");
        if (context.preparer.Started())
            throw std::runtime_error("manual enable started the preparation worker");
        if (context.audio.Read().state != better_tv_video::AudioOutput::State::Idle)
            throw std::runtime_error("startup started audio");
        PersistSettingsTask(&context, {});
        const auto written = nlohmann::json::parse(persisted);
        if (written.at("enabled") != false || written.at("muteGameAudio") != true)
            throw std::runtime_error("legacy audio preference overrode mandatory mute");
        if (written.at("bilibiliUrl") !=
                "https://www.bilibili.com/video/BV1xx411c7mD?p=2")
            throw std::runtime_error("the last bilibili link was not persisted");
        const auto position = RelativeAudioPosition({0, 200, 0}, {0, 0, 0}, {0, 0, 0});
        const auto turned = RelativeAudioPosition({0, 200, 0}, {0, 0, 0}, {0, 180, 0});
        if (!position || !turned || std::abs(position->right - 2.0f) > 0.001f ||
            std::abs(turned->right + 2.0f) > 0.001f || std::abs(turned->forward) > 0.001f)
            throw std::runtime_error("two-metre offset or camera-relative handedness is wrong");
        const AudioDistanceRange defaults;
        const AudioDistanceRange custom{8.0f, 24.0f};
        if (defaults.Gain(3.0f) != 1.0f || defaults.Gain(4.0f) != 1.0f ||
            defaults.Gain(10.0f) != 0.5f || defaults.Gain(16.0f) != 0.0f ||
            defaults.Gain(18.0f) != 0.0f || custom.Gain(8.0f) != 1.0f ||
            custom.Gain(16.0f) != 0.5f || custom.Gain(24.0f) != 0.0f)
            throw std::runtime_error("television distance gain does not match the playback range");
        struct SliderEdit {
            std::array<float, 2> values;
            std::array<std::array<float, 2>, 2> bounds{};
            std::size_t next{};
        };
        AnomalyUiServiceV1 ui{sizeof(ui)};
        ui.slider_float = [](void* user, AnomalyStringViewV1, float* value, float min, float max) {
            auto& edit = *static_cast<SliderEdit*>(user);
            if (edit.next >= edit.values.size()) return 0;
            edit.bounds[edit.next] = {min, max};
            *value = edit.values[edit.next++];
            return 1;
        };
        SliderEdit change{{8.0f, 24.0f}};
        ui.user = &change;
        DrawAudioDistanceSettings(context, &ui, context.settings.audio_distance);
        if (change.bounds[0] != std::array{0.5f, 15.0f} ||
            change.bounds[1] != std::array{9.0f, 60.0f})
            throw std::runtime_error("distance sliders did not constrain each other's ranges");
        PersistSettingsTask(&context, {});
        saved = persisted;
        context.settings = {};
        ReadSettings(context);
        if (context.settings.audio_distance.full_volume_meters != 8.0f ||
            context.settings.audio_distance.max_distance_meters != 24.0f ||
            context.settings.bilibili_url !=
                "https://www.bilibili.com/video/BV1xx411c7mD?p=2")
            throw std::runtime_error("edited audio distances or the link were lost after loading saved settings");
        SliderEdit typed{{100.0f, 0.0f}};
        ui.user = &typed;
        DrawAudioDistanceSettings(context, &ui, context.settings.audio_distance);
        if (context.settings.audio_distance.full_volume_meters != 20.0f ||
            context.settings.audio_distance.max_distance_meters != 21.0f)
            throw std::runtime_error("typed values bypassed the distance slider limits");

        auto invalid = nlohmann::json::parse(saved);
        invalid["audioFullVolumeMeters"] = "near";
        saved = invalid.dump();
        ReadSettings(context);
        if (context.settings.audio_distance.full_volume_meters != 4.0f ||
            context.settings.audio_distance.max_distance_meters != 24.0f ||
            context.settings.video_path != "C:/fixture/movie.mp4")
            throw std::runtime_error("invalid audio distance discarded a valid preference");
        invalid["audioFullVolumeMeters"] = 8.0f;
        invalid["audioMaxDistanceMeters"] = 8.5f;
        saved = invalid.dump();
        ReadSettings(context);
        if (context.settings.audio_distance.full_volume_meters != 4.0f ||
            context.settings.audio_distance.max_distance_meters != 16.0f)
            throw std::runtime_error("invalid distance pair did not restore both defaults");
        std::cout << "PASS: legacy activation ignored, preferences retained, startup/no-TV media calls=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
