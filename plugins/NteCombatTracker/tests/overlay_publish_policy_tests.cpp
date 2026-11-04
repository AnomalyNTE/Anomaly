#include "overlay_publish_policy.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

using namespace std::chrono_literals;
using nte::combat_stats::OverlayPublishPolicy;
using nte::combat_stats::OverlayToggleAction;

void Check(const bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

void IdlePollingIsLimitedToOncePerSecond() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    Check(policy.ShouldPoll(start), "first server poll is allowed");
    policy.ObserveServer(start, 0);
    Check(!policy.ShouldPoll(start + 999ms), "idle poll is throttled");
    Check(policy.ShouldPoll(start + 1s), "idle poll resumes after one second");
}

void ClientJoinAndStateChangesBypassSnapshotThrottle() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    policy.ObserveServer(start, 1);
    Check(policy.ShouldPublish(start, 1), "new client gets an immediate snapshot");
    policy.RecordPublished(start, 1);
    Check(!policy.ShouldPublish(start + 100ms, 1), "unchanged snapshot is capped at 4 Hz");
    Check(policy.ShouldPublish(start + 100ms, 2), "state transition publishes immediately");
    policy.RecordPublished(start + 100ms, 2);
    Check(policy.ShouldPublish(start + 350ms, 2), "periodic snapshot resumes after 250 ms");
}

void ForcePublishesStoppingWithoutClients() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    policy.ObserveServer(start, 0);
    Check(policy.ShouldPoll(start + 10ms, true), "forced stop queries server immediately");
    Check(policy.ShouldPublish(start + 10ms, 7, true),
          "producer-stopping publish is attempted even with no clients");
}

void DisabledOverlaySuppressesPollingUntilReenabled() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    Check(policy.enabled(), "overlay publishing starts enabled");
    policy.SetEnabled(false);
    Check(!policy.enabled() && !policy.ShouldPoll(start, true),
        "disabled overlay suppresses even forced polling");
    policy.SetEnabled(true);
    Check(policy.enabled() && policy.ShouldPoll(start),
        "reenabling makes the next poll immediately eligible");
}

void FailedLaunchUsesTheToggleAsARetry() {
    Check(nte::combat_stats::ResolveOverlayToggleAction(true, true) ==
            OverlayToggleAction::Retry,
        "an enabled overlay with a launch error should retry");
    Check(nte::combat_stats::ResolveOverlayToggleAction(true, false) ==
            OverlayToggleAction::Disable &&
            nte::combat_stats::ResolveOverlayToggleAction(false, false) ==
                OverlayToggleAction::Enable,
        "the normal toggle should disable or enable the overlay");
}

}  // namespace

int main() {
    IdlePollingIsLimitedToOncePerSecond();
    ClientJoinAndStateChangesBypassSnapshotThrottle();
    ForcePublishesStoppingWithoutClients();
    DisabledOverlaySuppressesPollingUntilReenabled();
    FailedLaunchUsesTheToggleAsARetry();
    std::cout << "All overlay publish policy tests passed\n";
}
