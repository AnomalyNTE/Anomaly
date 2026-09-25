#include "sequence.hpp"

namespace anomaly::plugins::quick_ultimate {

Sequencer::Sequencer(const Timings timings) noexcept : timings_(timings) {}

bool Sequencer::Queue(const std::uint32_t slot) noexcept {
    if (slot < 1U || slot > 4U) return false;
    queued_slot_ = slot;
    return true;
}

std::optional<Command> Sequencer::Poll(
    const Clock::time_point now,
    const bool combo_held) noexcept {
    for (;;) {
        if (queued_slot_ != 0 && phase_ == Phase::RepeatWait) {
            active_slot_ = queued_slot_;
            queued_slot_ = 0;
            failed_ = false;
            phase_ = alt_cleared_ ? Phase::DigitDown : Phase::ClearAlt;
            continue;
        }

        switch (phase_) {
        case Phase::Idle:
            if (queued_slot_ == 0) return std::nullopt;
            active_slot_ = queued_slot_;
            queued_slot_ = 0;
            failed_ = false;
            phase_ = Phase::ClearAlt;
            break;
        case Phase::ClearAlt:
            alt_cleared_ = true;
            phase_ = Phase::DigitDown;
            return Command::ClearAlt;
        case Phase::DigitDown:
            if (!combo_held) {
                phase_ = Phase::RestoreAlt;
                break;
            }
            digit_down_ = true;
            deadline_ = now + timings_.tap;
            phase_ = Phase::DigitPressed;
            return Command::DigitDown;
        case Phase::DigitPressed:
            if (now < deadline_) return std::nullopt;
            digit_down_ = false;
            if (queued_slot_ != 0) {
                active_slot_ = queued_slot_;
                queued_slot_ = 0;
                deadline_ = now + timings_.tap;
                phase_ = combo_held ? Phase::DigitDown : Phase::RestoreAlt;
            } else if (combo_held) {
                phase_ = Phase::UltimateDown;
            } else {
                phase_ = Phase::RestoreAlt;
            }
            return Command::DigitUp;
        case Phase::UltimateDown:
            if (!combo_held) {
                phase_ = Phase::RestoreAlt;
                break;
            }
            ultimate_down_ = true;
            deadline_ = now + timings_.tap;
            phase_ = Phase::UltimatePressed;
            return Command::UltimateDown;
        case Phase::UltimatePressed:
            if (now < deadline_) return std::nullopt;
            ultimate_down_ = false;
            if (queued_slot_ != 0) {
                active_slot_ = queued_slot_;
                queued_slot_ = 0;
                phase_ = combo_held ? Phase::DigitDown : Phase::RestoreAlt;
            } else if (combo_held) {
                deadline_ = now + timings_.repeat_gap;
                phase_ = Phase::RepeatWait;
            } else {
                phase_ = Phase::RestoreAlt;
            }
            return Command::UltimateUp;
        case Phase::RepeatWait:
            if (!combo_held) {
                phase_ = Phase::RestoreAlt;
                break;
            }
            if (now < deadline_) return std::nullopt;
            phase_ = Phase::UltimateDown;
            break;
        case Phase::RestoreAlt:
            alt_cleared_ = false;
            deadline_ = now + timings_.cooldown;
            phase_ = Phase::Cooldown;
            return Command::RestoreAlt;
        case Phase::Cooldown:
            if (now < deadline_) return std::nullopt;
            active_slot_ = 0;
            phase_ = Phase::Idle;
            break;
        case Phase::Cleanup:
            if (ultimate_down_) {
                ultimate_down_ = false;
                return Command::UltimateUp;
            }
            if (digit_down_) {
                digit_down_ = false;
                return Command::DigitUp;
            }
            if (alt_cleared_) {
                alt_cleared_ = false;
                deadline_ = now + timings_.cooldown;
                phase_ = Phase::Cooldown;
                return Command::RestoreAlt;
            }
            active_slot_ = 0;
            deadline_ = now + timings_.cooldown;
            phase_ = Phase::Cooldown;
            break;
        }
    }
}

void Sequencer::Fail() noexcept {
    queued_slot_ = 0;
    failed_ = true;
    phase_ = Phase::Cleanup;
}

void Sequencer::Cancel() noexcept {
    phase_ = Phase::Idle;
    deadline_ = {};
    active_slot_ = 0;
    queued_slot_ = 0;
    digit_down_ = false;
    ultimate_down_ = false;
    alt_cleared_ = false;
    failed_ = false;
}

Snapshot Sequencer::State() const noexcept {
    return {phase_, active_slot_, queued_slot_, failed_};
}

}  // namespace anomaly::plugins::quick_ultimate
