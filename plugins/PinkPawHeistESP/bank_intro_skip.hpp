#pragma once

#include <chrono>
#include <cstdint>

namespace pink_paw_heist_esp {

// Wait for the intro presentation to settle and request skipping once per
// playback. A non-bank sequence never owns this action.
class BankIntroSkip final {
public:
    using Clock = std::chrono::steady_clock;

    void Observe(const std::uintptr_t bank_sequence, const Clock::time_point now) noexcept {
        if (bank_sequence == sequence_) return;
        sequence_ = bank_sequence;
        first_seen_ = now;
        dispatched_ = false;
    }
    [[nodiscard]] bool Ready(const Clock::time_point now) const noexcept {
        return sequence_ != 0 && !dispatched_ && now - first_seen_ >= std::chrono::seconds(2);
    }
    void Dispatched() noexcept { dispatched_ = true; }
    void Reset() noexcept { *this = {}; }

private:
    std::uintptr_t sequence_{};
    Clock::time_point first_seen_{};
    bool dispatched_{};
};

}  // namespace pink_paw_heist_esp
