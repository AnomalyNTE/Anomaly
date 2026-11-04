#pragma once

#include <cstdint>
#include <optional>

namespace nte::combat_stats {

struct CursorFrameDecision {
    bool reset_applied{};
    bool cursor_gap{};
};

struct CursorAcceptDecision {
    bool accepted{};
    bool cursor_gap{};
    std::uint64_t missing_first{};
    std::uint64_t missing_last{};
};

class CombatCursor {
public:
    void RequestReset() noexcept;
    [[nodiscard]] CursorFrameDecision BeginFrame(
        std::optional<std::uint64_t> latest_sequence) noexcept;
    [[nodiscard]] bool RecoverFromNotFound(std::uint64_t latest_sequence) noexcept;
    [[nodiscard]] CursorAcceptDecision AcceptNext(std::uint64_t sequence) noexcept;
    [[nodiscard]] bool Accept(std::uint64_t sequence) noexcept;
    void Clear() noexcept;

    [[nodiscard]] std::uint64_t value() const noexcept;
    [[nodiscard]] bool reset_pending() const noexcept;

private:
    std::uint64_t value_{};
    bool reset_pending_{};
    bool recovery_attempted_{};
};

}  // namespace nte::combat_stats
