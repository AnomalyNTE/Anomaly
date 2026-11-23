#pragma once

#include "anomaly/sdk/cpp.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace anomaly::plugins {

// The settings owner serializes a snapshot; only Worker/Lifecycle writes it.
// The host drains scheduled callbacks before Stop and destroys this queue after Unload.
class ConfigSaveQueue final {
public:
    void Bind(const AnomalyHostApiV1* host, const AnomalyConfigServiceV1* config,
              std::string_view id, std::uint32_t version) {
        Reset();
        config_ = config;
        id_ = id;
        version_ = version;
        const auto scheduler = sdk::Host(host).Query<AnomalySchedulerServiceV1>(
            ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
        if (scheduler && scheduler->struct_size >= sizeof(*scheduler.get()) && scheduler->schedule)
            scheduler_ = scheduler.get();
    }

    bool Queue(std::string_view document) noexcept {
        if (!config_ || !config_->write_atomic) return false;
        try {
            std::string snapshot(document);
            std::scoped_lock lock(mutex_);
            pending_ = std::move(snapshot);
            changed_at_ = Clock::now();
            ScheduleLocked(500);
            return true;
        } catch (...) { return false; }
    }

    bool Flush() noexcept { return FlushPending(false); }

    // Lifecycle only, after the previous generation's callbacks have drained.
    void Reset() {
        std::scoped_lock lock(mutex_);
        pending_.reset();
        scheduled_ = false;
        config_ = nullptr;
        scheduler_ = nullptr;
    }

private:
    using Clock = std::chrono::steady_clock;

    void ScheduleLocked(std::uint32_t delay) {
        if (scheduled_ || !scheduler_) return;
        AnomalyGenerationHandleV1 task{};
        scheduled_ = sdk::Succeeded(scheduler_->schedule(
            scheduler_->user, delay, SaveTask, this, &task)) && task.id != 0;
    }

    bool FlushPending(bool debounce) noexcept {
        std::scoped_lock io_lock(io_mutex_);
        std::optional<std::string> document;
        {
            std::scoped_lock lock(mutex_);
            if (debounce && pending_) {
                const auto remaining = std::chrono::milliseconds(500) - (Clock::now() - changed_at_);
                if (remaining > Clock::duration::zero()) {
                    ScheduleLocked(static_cast<std::uint32_t>(
                        std::chrono::ceil<std::chrono::milliseconds>(remaining).count()));
                    return true;
                }
            }
            document = std::exchange(pending_, {});
        }
        if (!document) return true;
        try {
            if (sdk::Succeeded(config_->write_atomic(config_->user, sdk::StringView(id_), version_,
                    {reinterpret_cast<const std::uint8_t*>(document->data()), document->size()})))
                return true;
        } catch (...) {}
        std::scoped_lock lock(mutex_);
        if (!pending_) pending_ = std::move(document);
        if (debounce) ScheduleLocked(500);
        return false;
    }

    static void ANOMALY_CALL SaveTask(void* raw, AnomalyGenerationHandleV1) noexcept {
        auto& queue = *static_cast<ConfigSaveQueue*>(raw);
        {
            std::scoped_lock lock(queue.mutex_);
            queue.scheduled_ = false;
        }
        static_cast<void>(queue.FlushPending(true));
    }

    const AnomalyConfigServiceV1* config_{};
    const AnomalySchedulerServiceV1* scheduler_{};
    std::string id_;
    std::uint32_t version_{};
    std::mutex mutex_, io_mutex_;
    std::optional<std::string> pending_;
    Clock::time_point changed_at_{};
    bool scheduled_{};
};

} // namespace anomaly::plugins
