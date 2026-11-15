#pragma once
#include "flight_hotkey.hpp"
#include "motion.hpp"
#include <mutex>
#include <optional>
#include <utility>

namespace vehicle_flight {
struct Preferences {
    int speed{DefaultSpeed};
    Binding toggle;
    bool noclip{};
    Mode mode{Mode::Keyboard};
};

class PreferencesStore {
    static constexpr std::string_view Id = "vehicle-flight-controls";
    static constexpr std::string_view Schema = R"({
        "type":"object","additionalProperties":false,"required":["speed","key","modifiers"],
        "properties":{"speed":{"type":"integer","minimum":3,"maximum":1000},
                      "key":{"type":"integer","minimum":0,"maximum":255},
                      "modifiers":{"type":"integer","minimum":0,"maximum":7},
                      "noclip":{"type":"boolean"},
                      "mode":{"type":"integer","minimum":0,"maximum":1}}
    })";
public:
    Preferences Load(const anomaly::sdk::Host& host) {
        using namespace anomaly::sdk;
        const auto config = host.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID);
        const auto scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
        if (scheduler && scheduler->struct_size >= sizeof(*scheduler.get()) && scheduler->schedule)
            scheduler_ = scheduler.get();
        AnomalyGenerationHandleV1 handle{};
        if (!config || config->struct_size < sizeof(*config.get()) || !config->register_schema ||
            !config->read || !config->write_atomic ||
            !Succeeded(config->register_schema(config->user, StringView(Id), 1, Bytes(Schema), &handle))) return {};
        config_ = config.get();
        std::array<uint8_t, 128> data{};
        uint32_t version{};
        size_t size = data.size();
        if (!Succeeded(config_->read(config_->user, StringView(Id), &version, {data.data(), data.size()}, &size)) ||
            version != 1 || !size || size > data.size()) return {};

        const auto json = host.Query<AnomalyJsonServiceV1>(ANOMALY_JSON_SERVICE_V1_ID);
        if (!json || json->struct_size < sizeof(*json.get()) || !json->parse || !json->object_find ||
            !json->number_value || !json->release) return {};
        AnomalyGenerationHandleV1 root{};
        if (!Succeeded(json->parse(json->user, {reinterpret_cast<const char*>(data.data()), size}, &root))) return {};
        const auto number = [&](std::string_view key, uint32_t min, uint32_t max, uint32_t fallback) {
            AnomalyGenerationHandleV1 field{};
            if (!Succeeded(json->object_find(json->user, root, StringView(key), &field))) return fallback;
            double value{};
            const auto status = json->number_value(json->user, field, &value);
            static_cast<void>(json->release(json->user, field));
            return Succeeded(status) && value >= min && value <= max && std::floor(value) == value
                ? static_cast<uint32_t>(value) : fallback;
        };
        Preferences result;
        result.speed = static_cast<int>(number("speed", MinSpeed, MaxSpeed, DefaultSpeed));
        result.toggle.key = number("key", 0, 255, 0);
        result.toggle.modifiers = number("modifiers", 0, Modifiers, 0);
        result.mode = static_cast<Mode>(number("mode", 0, 1, 0));
        if (!Bindable(result.toggle.key)) result.toggle = {};
        AnomalyGenerationHandleV1 noclip{};
        if (json->boolean_value && Succeeded(json->object_find(json->user, root, StringView("noclip"), &noclip))) {
            int32_t value{};
            result.noclip = Succeeded(json->boolean_value(json->user, noclip, &value)) && value != 0;
            static_cast<void>(json->release(json->user, noclip));
        }
        static_cast<void>(json->release(json->user, root));
        return result;
    }

    void Queue(Preferences settings) {
        if (!config_) {
            failed_ = true;
            return;
        }
        {
            std::scoped_lock lock(mutex_);
            pending_ = settings;
            if (scheduled_) return;
            scheduled_ = true;
        }
        AnomalyGenerationHandleV1 task{};
        if (!scheduler_ || !anomaly::sdk::Succeeded(scheduler_->schedule(scheduler_->user, 250, SaveTask, this, &task))) {
            std::scoped_lock lock(mutex_);
            scheduled_ = false;
            failed_ = true;
        }
    }

    bool Failed() const { return failed_.load(); }

    void Flush() noexcept {
        std::optional<Preferences> settings;
        {
            std::scoped_lock lock(mutex_);
            scheduled_ = false;
            settings = std::exchange(pending_, {});
        }
        if (!settings) return;
        try {
            const auto document = "{\"speed\":" + std::to_string(settings->speed) +
                ",\"key\":" + std::to_string(settings->toggle.key) +
                ",\"modifiers\":" + std::to_string(settings->toggle.modifiers) +
                ",\"noclip\":" + (settings->noclip ? "true" : "false") +
                ",\"mode\":" + std::to_string(static_cast<int>(settings->mode)) + "}";
            if (anomaly::sdk::Succeeded(config_->write_atomic(config_->user, anomaly::sdk::StringView(Id), 1, Bytes(document)))) {
                failed_ = false;
                return;
            }
        } catch (...) {}
        std::scoped_lock lock(mutex_);
        if (!pending_) pending_ = settings;
        failed_ = true;
    }

private:
    static AnomalyByteSpanV1 Bytes(std::string_view value) {
        return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
    }

    static void ANOMALY_CALL SaveTask(void* raw, AnomalyGenerationHandleV1) noexcept {
        static_cast<PreferencesStore*>(raw)->Flush();
    }

    const AnomalyConfigServiceV1* config_{};
    const AnomalySchedulerServiceV1* scheduler_{};
    std::mutex mutex_;
    std::optional<Preferences> pending_;
    bool scheduled_{};
    std::atomic<bool> failed_{};
};
}
