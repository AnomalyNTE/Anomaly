#pragma once
#include <anomaly/sdk/cpp.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace collectible_map {
struct MapPreferences {
    bool oracle{true}, gift{true}, monsters{}, city_events{}, hide_completed{}, teleport{};
    std::vector<std::string> hidden_monsters;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MapPreferences, oracle, gift, monsters, city_events,
                                                hide_completed, teleport, hidden_monsters)

class PreferencesStore {
    static constexpr auto id = "map-preferences";
    static constexpr std::string_view schema = R"({
        "type":"object","additionalProperties":false,
        "properties":{
            "oracle":{"type":"boolean"},"gift":{"type":"boolean"},"monsters":{"type":"boolean"},
            "city_events":{"type":"boolean"},"hide_completed":{"type":"boolean"},"teleport":{"type":"boolean"},
            "hidden_monsters":{"type":"array","maxItems":512,"items":{"type":"string","maxLength":512}}
        }
    })";

public:
    MapPreferences Load(const anomaly::sdk::Host& host) {
        using namespace anomaly::sdk;
        const auto config = host.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID);
        const auto scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
        if (scheduler && scheduler->struct_size >= sizeof(*scheduler.get())) scheduler_ = scheduler.get();
        AnomalyGenerationHandleV1 handle{};
        if (!config || config->struct_size < sizeof(*config.get()) || !config->register_schema ||
            !config->read || !config->write_atomic || !Succeeded(config->register_schema(config->user,
                StringView(id), 1, {reinterpret_cast<const uint8_t*>(schema.data()), schema.size()}, &handle))) return {};
        config_ = config.get();
        size_t size{};
        uint32_t version{};
        const auto status = config_->read(config_->user, StringView(id), &version, {}, &size);
        if (!Succeeded(status) || version != 1 || !size || size > 64 * 1024) return {};
        std::string document(size, '\0');
        if (!Succeeded(config_->read(config_->user, StringView(id), &version,
                {reinterpret_cast<uint8_t*>(document.data()), document.size()}, &size)) ||
            version != 1 || !size || size > document.size()) return {};
        try { return nlohmann::json::parse(document.data(), document.data() + size).get<MapPreferences>(); }
        catch (const nlohmann::json::exception&) { return {}; }
    }

    void Queue(const MapPreferences& settings) {
        if (!config_) return;
        pending_.store(std::make_shared<const MapPreferences>(settings));
        AnomalyGenerationHandleV1 task{};
        if (scheduler_ && scheduler_->schedule)
            static_cast<void>(scheduler_->schedule(scheduler_->user, 0, SaveTask, this, &task));
    }

    void Save() noexcept {
        std::scoped_lock lock(save_mutex_);
        auto settings = pending_.exchange({});
        if (!settings) return;
        try {
            const auto document = nlohmann::json(*settings).dump();
            const auto status = config_->write_atomic(config_->user, anomaly::sdk::StringView(id), 1,
                {reinterpret_cast<const uint8_t*>(document.data()), document.size()});
            if (anomaly::sdk::Succeeded(status)) return;
        } catch (...) {}
        std::shared_ptr<const MapPreferences> empty;
        pending_.compare_exchange_strong(empty, std::move(settings));
    }

private:
    static void ANOMALY_CALL SaveTask(void* raw, AnomalyGenerationHandleV1) noexcept {
        static_cast<PreferencesStore*>(raw)->Save();
    }

    const AnomalyConfigServiceV1* config_{};
    const AnomalySchedulerServiceV1* scheduler_{};
    std::atomic<std::shared_ptr<const MapPreferences>> pending_;
    std::mutex save_mutex_;
};
}
