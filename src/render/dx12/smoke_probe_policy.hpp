#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ue5mem::embedded {

struct SmokeProbeRegion {
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return width != 0 && height != 0;
    }
};

[[nodiscard]] constexpr bool SmokeProbeFootprintValid(
    std::uint32_t row_count,
    std::uint64_t row_size_bytes,
    std::uint32_t row_pitch,
    std::uint64_t total_bytes,
    std::uint64_t maximum_bytes) noexcept {
    return row_count != 0 && row_size_bytes != 0 && row_pitch != 0 &&
        row_size_bytes <= row_pitch && total_bytes != 0 &&
        total_bytes <= maximum_bytes;
}

enum class SmokeProbeFenceState : std::uint8_t {
    Pending,
    Ready,
    Failed,
};

[[nodiscard]] constexpr SmokeProbeFenceState ClassifySmokeProbeFence(
    bool device_ready,
    std::uint64_t completed_value,
    std::uint64_t expected_value) noexcept {
    if (!device_ready || completed_value ==
            (std::numeric_limits<std::uint64_t>::max)() ||
        expected_value == 0) {
        return SmokeProbeFenceState::Failed;
    }
    return completed_value < expected_value
        ? SmokeProbeFenceState::Pending
        : SmokeProbeFenceState::Ready;
}

[[nodiscard]] constexpr SmokeProbeRegion ClampSmokeProbeRegion(
    std::uint64_t width,
    std::uint32_t height,
    std::uint32_t maximum_width,
    std::uint32_t maximum_height) noexcept {
    return {
        static_cast<std::uint32_t>((std::min)(
            width, static_cast<std::uint64_t>(maximum_width))),
        (std::min)(height, maximum_height)};
}

struct SmokeProbeTicket {
    std::uint64_t capture_generation{};
    std::uint64_t resize_epoch{};
};

struct SmokeProbePolicy {
    std::uint64_t active_generation{};
    std::uint64_t resize_epoch{};
    std::uint32_t retry_frames{};
    bool reported{};
};

inline void SynchronizeSmokeProbeCapture(
    SmokeProbePolicy& policy, std::uint64_t generation) noexcept {
    if (policy.active_generation == generation) return;
    policy.active_generation = generation;
    policy.resize_epoch = 0;
    policy.retry_frames = 0;
    policy.reported = false;
}

inline void RearmSmokeProbeAfterResize(SmokeProbePolicy& policy) noexcept {
    if (policy.active_generation == 0) return;
    policy.resize_epoch = policy.resize_epoch ==
            (std::numeric_limits<std::uint64_t>::max)()
        ? 1
        : policy.resize_epoch + 1;
    policy.retry_frames = 0;
    policy.reported = false;
}

[[nodiscard]] inline bool SmokeProbeReady(SmokeProbePolicy& policy) noexcept {
    if (policy.active_generation == 0 || policy.reported) return false;
    if (policy.retry_frames != 0) {
        --policy.retry_frames;
        return false;
    }
    return true;
}

[[nodiscard]] constexpr SmokeProbeTicket CurrentSmokeProbeTicket(
    const SmokeProbePolicy& policy) noexcept {
    return {policy.active_generation, policy.resize_epoch};
}

[[nodiscard]] constexpr bool SmokeProbeTicketCurrent(
    const SmokeProbePolicy& policy,
    SmokeProbeTicket ticket,
    std::uint64_t observed_generation) noexcept {
    return observed_generation != 0 &&
        policy.active_generation == observed_generation &&
        ticket.capture_generation == observed_generation &&
        ticket.resize_epoch == policy.resize_epoch;
}

inline void CompleteSmokeProbePolicy(
    SmokeProbePolicy& policy,
    SmokeProbeTicket ticket,
    std::uint64_t observed_generation,
    bool readback_succeeded,
    bool pixels_differ,
    std::uint32_t retry_frames) noexcept {
    if (!SmokeProbeTicketCurrent(policy, ticket, observed_generation)) return;
    if (readback_succeeded && pixels_differ) {
        policy.reported = true;
        policy.retry_frames = 0;
        return;
    }
    policy.retry_frames = retry_frames;
}

}  // namespace ue5mem::embedded
