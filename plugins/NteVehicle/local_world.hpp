#pragma once
#include <cstdint>

namespace nte_vehicle {

// HTGame.ERacingGameMode is a semantic contract, not a version-specific layout.
// Only solo modes are allowed; invitations, battles, friend rooms and unknown
// values are deliberately excluded alongside PVP and UGC_PVP.
[[nodiscard]] constexpr bool IsLocalRacingMode(std::uint8_t mode) noexcept {
    switch (mode) {
    case 0: // None
    case 1: // PVE
    case 5: // Quest
    case 6: // UGC_Preview
    case 7: // UGC_PVE
        return true;
    default:
        return false;
    }
}

// Published by Game after the world and reflected fields have been validated.
// Detours only read these offsets and the event actor's owning world; they never
// invoke UE functions or acquire the backend mutex.
struct LocalWorldGuardView {
    std::uintptr_t world{};
    std::uintptr_t world_address{};
    std::int64_t object_outer{};
    std::int64_t world_game_state{};
    std::int64_t racing_mode{};
};

} // namespace nte_vehicle
