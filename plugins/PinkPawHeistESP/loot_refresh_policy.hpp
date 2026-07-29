#pragma once

#include <chrono>

namespace pink_paw_heist_esp {

class LootRefreshPolicy final {
public:
    using Clock = std::chrono::steady_clock;

    explicit LootRefreshPolicy(const Clock::duration interval) noexcept
        : interval_(interval) {}

    [[nodiscard]] bool Begin(
        const Clock::time_point now,
        const bool requested) noexcept {
        if (requested) stable_ = false;
        return requested || (!stable_ && now >= next_refresh_);
    }

    void Complete(
        const Clock::time_point now,
        const bool unchanged) noexcept {
        stable_ = unchanged;
        next_refresh_ = now + interval_;
    }

    void Fail(const Clock::time_point now) noexcept {
        stable_ = false;
        next_refresh_ = now + interval_;
    }

    void Reset() noexcept {
        stable_ = false;
        next_refresh_ = {};
    }

private:
    Clock::duration interval_;
    Clock::time_point next_refresh_{};
    bool stable_{};
};

}  // namespace pink_paw_heist_esp
