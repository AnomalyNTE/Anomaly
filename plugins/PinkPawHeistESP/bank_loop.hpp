#pragma once

#include <chrono>
#include <cstdint>

namespace pink_paw_heist_esp {

enum class BankLoopPhase {
    stopped, waiting_outside, entering, waiting_loot, looting,
    evacuating, cooldown, failed, completed,
};

enum class BankLoopAction { none, enter, evacuate };
enum class BankLoopFailure {
    none, action, world_unavailable, entry_timeout, evacuation_timeout,
    left_bank, loot_unavailable,
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
};

// Only confirmed observations advance the loop; a sent request is not evidence.
// Evacuation is the settlement request itself, so it needs no extraction marker.
class BankLoop final {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::seconds kDefaultCooldown{6};
    static constexpr std::chrono::seconds kMinimumCooldown{1};
    static constexpr std::chrono::seconds kMaximumCooldown{600};

    void Start(const Clock::time_point now, const std::uint32_t round_limit = 0,
        const std::chrono::seconds cooldown = kDefaultCooldown) noexcept {
        *this = {};
        round_limit_ = round_limit;
        cooldown_ = Clamp(cooldown);
        phase_ = BankLoopPhase::waiting_outside;
        since_ = now;
    }
    void SetCooldown(const std::chrono::seconds cooldown) noexcept {
        cooldown_ = Clamp(cooldown);
    }
    [[nodiscard]] std::chrono::seconds Cooldown() const noexcept { return cooldown_; }
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
    [[nodiscard]] bool Collecting() const noexcept { return phase_ == BankLoopPhase::looting; }
    [[nodiscard]] bool SkippingCutscenes() const noexcept {
        // Entry cinema can outlast loot readiness; skip until departure.
        return Running() && phase_ != BankLoopPhase::waiting_outside;
    }

    [[nodiscard]] BankLoopAction Tick(
        const BankLoopObservation& o, const Clock::time_point now) noexcept {
        using namespace std::chrono_literals;
        if (!Running()) return BankLoopAction::none;
        // Loading is unknown, not evidence of leaving.
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
            if (!o.in_bank && now - since_ >= cooldown_) {
                phase_ = BankLoopPhase::entering;
                since_ = now;
                return BankLoopAction::enter;
            }
            return BankLoopAction::none;
        }
        // Death or a manual exit never completes a round.
        if (!o.in_bank) { Fail(BankLoopFailure::left_bank); return BankLoopAction::none; }
        if (phase_ == BankLoopPhase::waiting_loot) {
            // An empty scan only counts after this run saw real loot.
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
            } else {
                // Nothing left to attempt: settle without an extra scan.
                phase_ = BankLoopPhase::evacuating;
                since_ = now;
                return BankLoopAction::evacuate;
            }
        }
        return BankLoopAction::none;
    }

private:
    [[nodiscard]] static std::chrono::seconds Clamp(const std::chrono::seconds value) noexcept {
        return value < kMinimumCooldown ? kMinimumCooldown :
            value > kMaximumCooldown ? kMaximumCooldown : value;
    }

    BankLoopPhase phase_{BankLoopPhase::stopped};
    BankLoopFailure failure_{BankLoopFailure::none};
    Clock::time_point since_{};
    std::chrono::seconds cooldown_{kDefaultCooldown};
    std::uint32_t rounds_{};
    std::uint32_t round_limit_{};
};

}  // namespace pink_paw_heist_esp
