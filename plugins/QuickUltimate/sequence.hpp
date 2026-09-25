#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace anomaly::plugins::quick_ultimate {

enum class Phase : std::uint8_t {
    Idle,
    ClearAlt,
    DigitDown,
    DigitPressed,
    UltimateDown,
    UltimatePressed,
    RepeatWait,
    RestoreAlt,
    Cooldown,
    Cleanup,
};

enum class Command : std::uint8_t {
    ClearAlt,
    DigitDown,
    DigitUp,
    UltimateDown,
    UltimateUp,
    RestoreAlt,
};

struct Timings final {
    std::chrono::milliseconds tap{20};
    std::chrono::milliseconds repeat_gap{80};
    std::chrono::milliseconds cooldown{100};
};

struct Snapshot final {
    Phase phase{Phase::Idle};
    std::uint32_t active_slot{};
    std::uint32_t queued_slot{};
    bool failed{};
};

class Sequencer final {
public:
    using Clock = std::chrono::steady_clock;

    explicit Sequencer(Timings timings = {}) noexcept;

    [[nodiscard]] bool Queue(std::uint32_t slot) noexcept;
    [[nodiscard]] std::optional<Command> Poll(
        Clock::time_point now, bool combo_held) noexcept;
    void Fail() noexcept;
    void Cancel() noexcept;
    [[nodiscard]] Snapshot State() const noexcept;

private:
    Timings timings_;
    Phase phase_{Phase::Idle};
    Clock::time_point deadline_{};
    std::uint32_t active_slot_{};
    std::uint32_t queued_slot_{};
    bool digit_down_{};
    bool ultimate_down_{};
    bool alt_cleared_{};
    bool failed_{};
};

}  // namespace anomaly::plugins::quick_ultimate
