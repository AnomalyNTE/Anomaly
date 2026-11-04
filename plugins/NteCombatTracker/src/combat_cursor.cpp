#include "combat_cursor.hpp"

#include <limits>

namespace nte::combat_stats {

void CombatCursor::RequestReset() noexcept {
    reset_pending_ = true;
}

CursorFrameDecision CombatCursor::BeginFrame(
    const std::optional<std::uint64_t> latest_sequence) noexcept {
    recovery_attempted_ = false;
    if (!latest_sequence.has_value()) return {};
    if (reset_pending_) {
        value_ = *latest_sequence;
        reset_pending_ = false;
        return {.reset_applied = true};
    }
    if (*latest_sequence < value_) {
        value_ = 0;
        return {.cursor_gap = true};
    }
    return {};
}

bool CombatCursor::RecoverFromNotFound(const std::uint64_t latest_sequence) noexcept {
    if (recovery_attempted_ || value_ == 0 || latest_sequence <= value_) return false;
    value_ = 0;
    recovery_attempted_ = true;
    return true;
}

CursorAcceptDecision CombatCursor::AcceptNext(const std::uint64_t sequence) noexcept {
    if (sequence <= value_) return {};
    CursorAcceptDecision decision{.accepted = true};
    if (value_ != (std::numeric_limits<std::uint64_t>::max)() &&
        sequence > value_ + 1) {
        decision.cursor_gap = true;
        decision.missing_first = value_ + 1;
        decision.missing_last = sequence - 1;
    }
    value_ = sequence;
    return decision;
}

bool CombatCursor::Accept(const std::uint64_t sequence) noexcept {
    return AcceptNext(sequence).accepted;
}

void CombatCursor::Clear() noexcept {
    value_ = 0;
    reset_pending_ = false;
    recovery_attempted_ = false;
}

std::uint64_t CombatCursor::value() const noexcept {
    return value_;
}

bool CombatCursor::reset_pending() const noexcept {
    return reset_pending_;
}

}  // namespace nte::combat_stats
