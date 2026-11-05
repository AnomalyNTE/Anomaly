#pragma once

#include <chrono>
#include <cstdint>

namespace nte::combat_stats {

enum class OverlayToggleAction : std::uint8_t { Enable, Disable, Retry };

[[nodiscard]] OverlayToggleAction ResolveOverlayToggleAction(
    bool enabled, bool launch_failed) noexcept;

class OverlayPublishPolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] bool ShouldPoll(TimePoint now, bool force = false) const noexcept;
    void ObserveServer(TimePoint now, std::uint32_t connected_clients) noexcept;
    [[nodiscard]] bool ShouldPublish(
        TimePoint now, std::uint64_t state_revision, bool force = false) const noexcept;
    void RecordPublished(TimePoint now, std::uint64_t state_revision) noexcept;
    void SetEnabled(bool enabled) noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] std::uint32_t connected_clients() const noexcept {
        return connected_clients_;
    }

private:
    bool enabled_{true};
    std::uint32_t connected_clients_{};
    bool client_joined_{};
    bool has_published_state_{};
    std::uint64_t published_state_revision_{};
    TimePoint next_poll_{};
    TimePoint next_publish_{};
};

}  // namespace nte::combat_stats
