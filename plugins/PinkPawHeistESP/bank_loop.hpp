#pragma once

#include <chrono>
#include <cstdint>

namespace pink_paw_heist_esp {

enum class BankLoopPhase {
    stopped, waiting_outside, entering, waiting_loot, looting, confirming_loot,
    waiting_extraction, traveling, evacuating, cooldown, failed, completed,
};

enum class BankLoopAction { none, enter, refresh_loot, teleport_extraction, evacuate };
enum class BankLoopFailure {
    none, action, world_unavailable, entry_timeout, evacuation_timeout,
    left_bank, loot_unavailable, pickup_unconfirmed, refresh_timeout,
    extraction_unavailable, arrival_timeout,
};

struct BankLoopObservation final {
    bool world_ready{};
    bool bank_known{};
    bool in_bank{};
    bool loot_ready{};
    bool loot_present{};
    bool pickup_busy{};
    bool has_targets{};
    bool has_unattempted_targets{};
    bool extraction_available{};
    bool at_extraction{};
    std::uint64_t loot_revision{};
};

// Decisions consume confirmed game observations. Sending a request alone never
// counts as entry, a collected item, or a completed round.
class BankLoop final {
public:
    using Clock = std::chrono::steady_clock;

    void Start(const Clock::time_point now, const std::uint32_t round_limit = 0) noexcept {
        *this = {};
        round_limit_ = round_limit;
        phase_ = BankLoopPhase::waiting_outside;
        since_ = now;
    }
    void Stop() noexcept { phase_ = BankLoopPhase::stopped; }
    void Fail(const BankLoopFailure reason = BankLoopFailure::action) noexcept {
        failure_ = reason;
        phase_ = BankLoopPhase::failed;
    }
    [[nodiscard]] BankLoopFailure Failure() const noexcept { return failure_; }
    [[nodiscard]] BankLoopPhase Phase() const noexcept { return phase_; }
    [[nodiscard]] std::uint32_t Rounds() const noexcept { return rounds_; }
    [[nodiscard]] std::uint32_t RoundLimit() const noexcept { return round_limit_; }
    [[nodiscard]] bool Running() const noexcept {
        return phase_ != BankLoopPhase::stopped && phase_ != BankLoopPhase::failed &&
            phase_ != BankLoopPhase::completed;
    }
    [[nodiscard]] bool Collecting() const noexcept {
        return phase_ == BankLoopPhase::looting || phase_ == BankLoopPhase::confirming_loot;
    }
    [[nodiscard]] bool SkippingCutscenes() const noexcept {
        // Bank actors/loot may be ready before the entry cinema finishes.
        // Keep observing its skip button after collection has started too.
        return Running() && phase_ != BankLoopPhase::waiting_outside;
    }

    [[nodiscard]] BankLoopAction Tick(
        const BankLoopObservation& o, const Clock::time_point now) noexcept {
        using namespace std::chrono_literals;
        if (!Running()) return BankLoopAction::none;
        // Loading is unknown, not evidence that the player has left the bank.
        if (!o.world_ready || !o.bank_known) {
            if (now - since_ >= 90s) Fail(BankLoopFailure::world_unavailable);
            return BankLoopAction::none;
        }
        if (phase_ == BankLoopPhase::waiting_outside) {
            since_ = now;
            phase_ = !o.in_bank ? BankLoopPhase::entering :
                o.loot_ready && o.loot_present ? BankLoopPhase::looting : BankLoopPhase::waiting_loot;
            return o.in_bank ? BankLoopAction::none : BankLoopAction::enter;
        }
        if (phase_ == BankLoopPhase::entering) {
            if (o.in_bank) {
                phase_ = o.loot_ready && o.loot_present
                    ? BankLoopPhase::looting : BankLoopPhase::waiting_loot;
                since_ = now;
            }
            else if (now - since_ >= 60s) Fail(BankLoopFailure::entry_timeout);
            return BankLoopAction::none;
        }
        if (phase_ == BankLoopPhase::evacuating) {
            if (!o.in_bank) {
                ++rounds_;
                phase_ = round_limit_ != 0 && rounds_ >= round_limit_
                    ? BankLoopPhase::completed : BankLoopPhase::cooldown;
                since_ = now;
            } else if (now - since_ >= 90s) Fail(BankLoopFailure::evacuation_timeout);
            return BankLoopAction::none;
        }
        if (phase_ == BankLoopPhase::cooldown) {
            if (!o.in_bank && now - since_ >= 8s) {
                phase_ = BankLoopPhase::entering;
                since_ = now;
                return BankLoopAction::enter;
            }
            return BankLoopAction::none;
        }
        // Death, manual exit, or a different dungeon must not complete a round.
        if (!o.in_bank) { Fail(BankLoopFailure::left_bank); return BankLoopAction::none; }
        if (phase_ == BankLoopPhase::waiting_loot) {
            // The extraction actors can precede every BankBox during level
            // streaming. Empty scans are not a completed bank until this run
            // has observed ready, pickable loot, irrespective of the value filter.
            if (o.loot_ready && o.loot_present) {
                phase_ = BankLoopPhase::looting;
                since_ = now;
            } else if (now - since_ >= 90s) Fail(BankLoopFailure::loot_unavailable);
            return BankLoopAction::none;
        }
        if (phase_ == BankLoopPhase::looting) {
            if (!o.loot_ready) {
                if (now - since_ >= 90s) Fail(BankLoopFailure::loot_unavailable);
            } else if (o.pickup_busy || o.has_unattempted_targets) {
                since_ = now;
            } else if (o.has_targets) {
                // A successful native return is not server confirmation. A
                // still-pickable attempted item must not be mistaken for empty.
                if (now - since_ >= 8s) Fail(BankLoopFailure::pickup_unconfirmed);
            } else if (now - since_ >= 3s) {
                phase_ = BankLoopPhase::confirming_loot;
                revision_ = o.loot_revision;
                since_ = now;
                return BankLoopAction::refresh_loot;
            }
        } else if (phase_ == BankLoopPhase::confirming_loot) {
            if (o.has_targets || o.has_unattempted_targets || o.pickup_busy) {
                phase_ = BankLoopPhase::looting;
                since_ = now;
            } else if (o.loot_ready && o.loot_revision != revision_) {
                phase_ = BankLoopPhase::waiting_extraction;
                since_ = now;
            } else if (now - since_ >= 30s) Fail(BankLoopFailure::refresh_timeout);
        } else if (phase_ == BankLoopPhase::waiting_extraction) {
            if (o.has_targets || o.has_unattempted_targets || o.pickup_busy) {
                phase_ = BankLoopPhase::looting;
                since_ = now;
                return BankLoopAction::none;
            }
            if (o.extraction_available) {
                phase_ = BankLoopPhase::traveling;
                since_ = now;
                return BankLoopAction::teleport_extraction;
            }
            if (now - since_ >= 60s) Fail(BankLoopFailure::extraction_unavailable);
        } else if (phase_ == BankLoopPhase::traveling) {
            if (o.at_extraction && now - since_ >= 1s) {
                phase_ = BankLoopPhase::evacuating;
                since_ = now;
                return BankLoopAction::evacuate;
            }
            if (now - since_ >= 20s) Fail(BankLoopFailure::arrival_timeout);
        }
        return BankLoopAction::none;
    }

private:
    BankLoopPhase phase_{BankLoopPhase::stopped};
    BankLoopFailure failure_{BankLoopFailure::none};
    Clock::time_point since_{};
    std::uint64_t revision_{};
    std::uint32_t rounds_{};
    std::uint32_t round_limit_{};
};

}  // namespace pink_paw_heist_esp
