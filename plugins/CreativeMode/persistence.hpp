#pragma once
#include "model.hpp"
#include <anomaly/sdk/cpp.hpp>
#include <atomic>
#include <mutex>

namespace creative_mode {
class Persistence {
    static constexpr std::string_view Id = "creative-mode";
    static constexpr std::string_view Schema = R"({
        "type":"object","additionalProperties":false,"required":["settings","saved"],
        "properties":{
          "settings":{"type":"object","additionalProperties":false,
            "required":["filter","asset","distance","height","moveStep","angleStep","grid","snap","collision","rotation","scale"],
            "properties":{"filter":{"type":"string","maxLength":128},"asset":{"type":"string","maxLength":512},
              "distance":{"type":"number","minimum":10,"maximum":100000},
              "height":{"type":"number","minimum":-100000,"maximum":100000},
              "moveStep":{"type":"number","minimum":0.1,"maximum":10000},
              "angleStep":{"type":"number","minimum":0.1,"maximum":180},
              "grid":{"type":"number","minimum":0.1,"maximum":10000},
              "editorMode":{"type":"integer","minimum":0,"maximum":2},
              "moveHandles":{"type":"boolean"},"rotateHandles":{"type":"boolean"},"scaleHandles":{"type":"boolean"},
              "category":{"type":"integer","minimum":0,"maximum":7},
              "editorEnabled":{"type":"boolean"},"fineTune":{"type":"boolean"},"followPlayer":{"type":"boolean"},
              "folder":{"type":"string","maxLength":512},
              "previewYaw":{"type":"number"},"previewElevation":{"type":"number","minimum":-85,"maximum":85},
              "previewZoom":{"type":"number","minimum":0.25,"maximum":4},
              "previewLit":{"type":"boolean"},
              "snap":{"type":"boolean"},"collision":{"type":"boolean"},
              "rotation":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
              "scale":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number","minimum":0.01,"maximum":1000}}}},
          "saved":{"type":"object","additionalProperties":false,"required":["world","items"],
            "properties":{"world":{"type":"string","maxLength":1024},
              "items":{"type":"array","maxItems":512,"items":{"type":"object","additionalProperties":false,
                "required":["asset","pose","collision"],"properties":{
                  "asset":{"type":"string","maxLength":512},"collision":{"type":"boolean"},
                  "pose":{"type":"object","additionalProperties":false,"required":["position","rotation","scale"],
                    "properties":{
                      "position":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
                      "rotation":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
                      "scale":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number","minimum":0.01,"maximum":1000}}
                    }}}}}}}
    }}
    )";
public:
    void Initialize(const AnomalyHostApiV1* host) {
        const anomaly::sdk::Host api(host);
        config_ = api.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID).get();
        scheduler_ = api.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
        if (!config_ || config_->struct_size < offsetof(AnomalyConfigServiceV1, write_atomic) + sizeof(config_->write_atomic) ||
            !config_->register_schema || !config_->read || !config_->write_atomic ||
            !scheduler_ || scheduler_->struct_size < sizeof(*scheduler_) || !scheduler_->schedule ||
            config_->register_schema(config_->user, anomaly::sdk::StringView(Id), 1, Bytes(Schema), &schema_).code != 0) {
            failed_ = true; return;
        }
        std::uint32_t version{}; std::size_t size{};
        const auto status = config_->read(config_->user, anomaly::sdk::StringView(Id), &version, {}, &size);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) { writable_ = true; return; }
        if ((status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) ||
            !size || size > 1024 * 1024 || version != 1) {
            failed_ = true; return;
        }
        std::vector<std::uint8_t> bytes(size);
        if (config_->read(config_->user, anomaly::sdk::StringView(Id), &version, {bytes.data(), bytes.size()}, &size).code != 0 ||
            version != 1 || size > bytes.size()) { failed_ = true; return; }
        try {
            document_ = Decode(nlohmann::json::parse(bytes.begin(), bytes.begin() + size));
            writable_ = true;
        } catch (...) { failed_ = true; }
    }
    Document Snapshot() const { std::scoped_lock lock(mutex_); return document_; }
    void SettingsChanged(const Settings& settings) {
        { std::scoped_lock lock(mutex_); document_.settings = settings; ++revision_; dirty_ = true; }
        Schedule();
    }
    void LayoutChanged(SavedLayout layout) {
        { std::scoped_lock lock(mutex_); document_.saved = std::move(layout); ++revision_; dirty_ = true; }
        Schedule();
    }
    bool Failed() const { return failed_.load(); }
    void Flush() noexcept {
        // Only Worker and the drained stop callback enter here. Render never
        // takes this I/O lock and the document lock is released before writing.
        std::scoped_lock writer(write_mutex_);
        try {
            Document copy; std::uint64_t revision{};
            {
                std::scoped_lock lock(mutex_); scheduled_ = false;
                if (!dirty_ || !writable_) return;
                copy = document_; revision = revision_;
            }
            const auto json = Encode(copy).dump();
            if (config_->write_atomic(config_->user, anomaly::sdk::StringView(Id), 1, Bytes(json)).code != 0) {
                failed_ = true; return;
            }
            { std::scoped_lock lock(mutex_); if (revision_ == revision) dirty_ = false; }
            failed_ = false;
        } catch (...) { failed_ = true; }
    }
private:
    static AnomalyByteSpanV1 Bytes(std::string_view text) {
        return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    }
    void Schedule() {
        {
            std::scoped_lock lock(mutex_);
            if (!writable_) { failed_ = true; return; }
            if (scheduled_) return;
            scheduled_ = true;
        }
        AnomalyGenerationHandleV1 task{};
        if (scheduler_->schedule(scheduler_->user, 250, SaveTask, this, &task).code != 0) {
            std::scoped_lock lock(mutex_); scheduled_ = false; failed_ = true;
        }
    }
    static void ANOMALY_CALL SaveTask(void* raw, AnomalyGenerationHandleV1) noexcept {
        static_cast<Persistence*>(raw)->Flush();
    }
    const AnomalyConfigServiceV1* config_{};
    const AnomalySchedulerServiceV1* scheduler_{};
    AnomalyGenerationHandleV1 schema_{};
    mutable std::mutex mutex_;
    std::mutex write_mutex_;
    Document document_;
    std::uint64_t revision_{};
    bool writable_{}, dirty_{}, scheduled_{};
    std::atomic<bool> failed_{};
};
}
