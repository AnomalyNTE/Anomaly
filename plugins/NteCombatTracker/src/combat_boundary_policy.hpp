#pragma once

#include "combat_stats_engine.hpp"

namespace nte::combat_stats {

enum class CombatBoundaryAction {
    None,
    AdvanceForInactivity,
    CompleteForServiceLoss,
};

// This policy must be evaluated only after the event stream has been drained.
// current_combatant snapshot validity is intentionally not part of this
// decision; a stale/invalid combatant snapshot must not close the encounter.
[[nodiscard]] inline CombatBoundaryAction PlanCombatBoundaryAction(
    const EncounterState state,
    const bool log_capture_recording,
    const bool combat_methods_available) noexcept {
    if (!combat_methods_available && log_capture_recording) {
        return CombatBoundaryAction::CompleteForServiceLoss;
    }
    if (state != EncounterState::Idle) {
        return CombatBoundaryAction::AdvanceForInactivity;
    }
    return CombatBoundaryAction::None;
}

}  // namespace nte::combat_stats
