#include "overlay_publish_policy.hpp"

namespace nte::combat_stats {
namespace {

constexpr auto kIdlePollInterval = std::chrono::seconds(1);
constexpr auto kPublishInterval = std::chrono::milliseconds(250);

}  // namespace

OverlayToggleAction ResolveOverlayToggleAction(
    const bool enabled, const bool launch_failed) noexcept {
    if (!enabled) return OverlayToggleAction::Enable;
    return launch_failed ? OverlayToggleAction::Retry :
        OverlayToggleAction::Disable;
}

bool OverlayPublishPolicy::ShouldPoll(const TimePoint now, const bool force) const noexcept {
    return enabled_ && (force || connected_clients_ != 0 || now >= next_poll_);
}

void OverlayPublishPolicy::ObserveServer(
    const TimePoint now, const std::uint32_t connected_clients) noexcept {
    client_joined_ = connected_clients > connected_clients_;
    connected_clients_ = connected_clients;
    if (connected_clients == 0) next_poll_ = now + kIdlePollInterval;
    else next_poll_ = now;
}

bool OverlayPublishPolicy::ShouldPublish(
    const TimePoint now,
    const std::uint64_t state_revision,
    const bool force) const noexcept {
    if (!enabled_) return false;
    if (force) return true;
    if (connected_clients_ == 0) return false;
    return client_joined_ || !has_published_state_ ||
        state_revision != published_state_revision_ || now >= next_publish_;
}

void OverlayPublishPolicy::SetEnabled(const bool enabled) noexcept {
    if (enabled_ == enabled) return;
    if (enabled) {
        *this = {};
    } else {
        enabled_ = false;
        connected_clients_ = 0;
        client_joined_ = false;
    }
}

void OverlayPublishPolicy::RecordPublished(
    const TimePoint now, const std::uint64_t state_revision) noexcept {
    has_published_state_ = true;
    published_state_revision_ = state_revision;
    client_joined_ = false;
    next_publish_ = now + kPublishInterval;
}

void OverlayPublishPolicy::Reset() noexcept {
    *this = {};
}

}  // namespace nte::combat_stats
