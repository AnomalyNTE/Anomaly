#include "../sequence.hpp"

#include <chrono>
#include <cstdio>
#include <optional>

namespace {

using anomaly::plugins::quick_ultimate::Command;
using anomaly::plugins::quick_ultimate::Phase;
using anomaly::plugins::quick_ultimate::Sequencer;
using Clock = Sequencer::Clock;

bool Check(const bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool Equals(const std::optional<Command> value, const Command expected) {
    return value.has_value() && *value == expected;
}

bool AdvanceToFirstUltimate(
    Sequencer& sequencer,
    const std::uint32_t slot,
    const Clock::time_point start) {
    if (!Check(sequencer.Queue(slot), "slot must be accepted")) return false;
    if (!Check(
            Equals(sequencer.Poll(start, true), Command::ClearAlt),
            "sequence must clear Alt first")) {
        return false;
    }
    if (!Check(
            Equals(sequencer.Poll(start, true), Command::DigitDown),
            "sequence must press the selected digit")) {
        return false;
    }
    if (!Check(
            Equals(
                sequencer.Poll(start + std::chrono::milliseconds(20), true),
                Command::DigitUp),
            "digit must release after the configured tap")) {
        return false;
    }
    return Check(
        Equals(
            sequencer.Poll(start + std::chrono::milliseconds(20), true),
            Command::UltimateDown),
        "first ultimate must begin immediately after digit release");
}

bool HeldComboRepeatsUltimateEveryHundredMilliseconds() {
    Sequencer sequencer;
    const Clock::time_point start{};
    if (!AdvanceToFirstUltimate(sequencer, 2, start)) return false;

    const auto first_q = start + std::chrono::milliseconds(20);
    if (!Check(
            Equals(
                sequencer.Poll(first_q + std::chrono::milliseconds(20), true),
                Command::UltimateUp),
            "first ultimate must release after 20ms")) {
        return false;
    }
    if (!Check(
            sequencer.State().phase == Phase::RepeatWait,
            "held combo must enter the repeat wait")) {
        return false;
    }
    if (!Check(
            !sequencer.Poll(first_q + std::chrono::milliseconds(99), true),
            "repeat gap must prevent an early second press")) {
        return false;
    }
    if (!Check(
            Equals(
                sequencer.Poll(first_q + std::chrono::milliseconds(100), true),
                Command::UltimateDown),
            "held combo must press ultimate again after 100ms")) {
        return false;
    }
    return Check(
        Equals(
            sequencer.Poll(first_q + std::chrono::milliseconds(120), true),
            Command::UltimateUp),
        "repeated ultimate must release before the next gap");
}

bool ReleaseBeforeUltimateStopsWithoutQ() {
    Sequencer sequencer;
    const Clock::time_point start{};
    if (!Check(sequencer.Queue(1), "slot must be accepted")) return false;
    if (!Check(
            Equals(sequencer.Poll(start, true), Command::ClearAlt),
            "sequence must clear Alt")) {
        return false;
    }
    if (!Check(
            Equals(sequencer.Poll(start, true), Command::DigitDown),
            "sequence must press the digit")) {
        return false;
    }
    if (!Check(
            Equals(
                sequencer.Poll(start + std::chrono::milliseconds(20), false),
                Command::DigitUp),
            "release must still release the digit")) {
        return false;
    }
    if (!Check(
            Equals(
                sequencer.Poll(start + std::chrono::milliseconds(20), false),
                Command::RestoreAlt),
            "release before the first Q must restore Alt")) {
        return false;
    }
    return Check(
        !sequencer.Poll(start + std::chrono::seconds(1), false),
        "release before the first Q must not send Q");
}

bool ReleaseDuringUltimateStopsAfterKeyUp() {
    Sequencer sequencer;
    const Clock::time_point start{};
    if (!AdvanceToFirstUltimate(sequencer, 3, start)) return false;

    const auto release = start + std::chrono::milliseconds(40);
    if (!Check(
            Equals(sequencer.Poll(release, false), Command::UltimateUp),
            "release during Q down must release Q")) {
        return false;
    }
    if (!Check(
            sequencer.State().phase == Phase::RestoreAlt,
            "release during Q down must enter restore")) {
        return false;
    }
    return Check(
        Equals(sequencer.Poll(release, false), Command::RestoreAlt),
        "release during Q down must restore Alt");
}

bool ReleaseDuringRepeatWaitStops() {
    Sequencer sequencer;
    const Clock::time_point start{};
    if (!AdvanceToFirstUltimate(sequencer, 4, start)) return false;

    const auto first_q = start + std::chrono::milliseconds(20);
    if (!Check(
            Equals(
                sequencer.Poll(first_q + std::chrono::milliseconds(20), true),
                Command::UltimateUp),
            "first Q must release")) {
        return false;
    }
    if (!Check(
            Equals(
                sequencer.Poll(first_q + std::chrono::milliseconds(50), false),
                Command::RestoreAlt),
            "release during repeat wait must stop and restore Alt")) {
        return false;
    }
    return Check(
        !sequencer.Poll(start + std::chrono::seconds(1), false),
        "release during repeat wait must not send another Q");
}

bool LatestSlotReplacesTarget() {
    Sequencer sequencer;
    const Clock::time_point start{};
    if (!AdvanceToFirstUltimate(sequencer, 1, start)) return false;

    const auto first_q = start + std::chrono::milliseconds(20);
    if (!Check(sequencer.Queue(4), "latest slot must be queued")) return false;
    if (!Check(
            Equals(
                sequencer.Poll(first_q + std::chrono::milliseconds(20), true),
                Command::UltimateUp),
            "current Q must release before switching target")) {
        return false;
    }
    if (!Check(
            Equals(
                sequencer.Poll(first_q + std::chrono::milliseconds(21), true),
                Command::DigitDown),
            "latest slot must begin its own digit press")) {
        return false;
    }
    return Check(
        sequencer.State().active_slot == 4,
        "only the latest slot may remain active");
}

bool CancelAndFailureClearPostedState() {
    Sequencer cancelled;
    const Clock::time_point start{};
    if (!Check(cancelled.Queue(1), "slot must be accepted for cancel")) return false;
    static_cast<void>(cancelled.Poll(start, true));
    static_cast<void>(cancelled.Poll(start, true));
    cancelled.Cancel();
    if (!Check(
            cancelled.State().phase == Phase::Idle,
            "cancel must return the sequencer to idle")) {
        return false;
    }
    if (!Check(
            !cancelled.Poll(start + std::chrono::seconds(1), true),
            "cancel must remove pending commands")) {
        return false;
    }

    Sequencer failed;
    if (!Check(failed.Queue(2), "slot must be accepted for failure")) return false;
    static_cast<void>(failed.Poll(start, true));
    static_cast<void>(failed.Poll(start, true));
    failed.Fail();
    if (!Check(
            Equals(failed.Poll(start, true), Command::DigitUp),
            "failure cleanup must release the digit")) {
        return false;
    }
    if (!Check(
            Equals(failed.Poll(start, true), Command::RestoreAlt),
            "failure cleanup must restore Alt")) {
        return false;
    }
    return Check(
        !failed.Poll(start + std::chrono::seconds(1), true),
        "failure cleanup must not send ultimate");
}

}  // namespace

int main() {
    if (!HeldComboRepeatsUltimateEveryHundredMilliseconds()) return 1;
    if (!ReleaseBeforeUltimateStopsWithoutQ()) return 1;
    if (!ReleaseDuringUltimateStopsAfterKeyUp()) return 1;
    if (!ReleaseDuringRepeatWaitStops()) return 1;
    if (!LatestSlotReplacesTarget()) return 1;
    if (!CancelAndFailureClearPostedState()) return 1;
    std::puts("quick ultimate sequence fixture passed");
    return 0;
}
