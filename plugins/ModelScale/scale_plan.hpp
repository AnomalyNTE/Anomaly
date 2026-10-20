#pragma once

// The scale decision for one component, kept apart from any memory access so the behaviour
// that matters -- what the plugin writes and what it treats as the game's own scale -- can
// be exercised without the game.

#include <array>
#include <cstddef>

namespace model_scale {

using ScaleVector = std::array<double, 3>;

inline ScaleVector UniformScale(const double value) noexcept {
    return ScaleVector{value, value, value};
}

// What the plugin remembers about a component on the game's behalf: `base` is the scale the
// component carried before the plugin touched it, `applied` is the value the plugin last
// wrote.
struct TargetScaleState {
    ScaleVector base{1.0, 1.0, 1.0};
    ScaleVector applied{1.0, 1.0, 1.0};
    bool valid{false};
};

struct ScalePlan {
    // The live value does not match the plugin's last output, so the plugin's record of the
    // component is stale and has to be discarded before the target is computed.
    bool resynced{false};
    // Whether the component must be written this pass.
    bool write{false};
    ScaleVector scale{1.0, 1.0, 1.0};
};

inline bool NearlyEqual(const double left, const double right,
                        const double epsilon) noexcept {
    double difference = left - right;
    if (difference < 0) difference = -difference;
    const double magnitude = right < 0 ? -right : right;
    return difference <= epsilon * (magnitude + 1.0);
}

inline bool ScaleVectorEqual(const ScaleVector& left, const ScaleVector& right,
                             const double epsilon) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!NearlyEqual(left[axis], right[axis], epsilon)) return false;
    }
    return true;
}

inline double ClampFactor(const double factor, const double minimum,
                          const double maximum) noexcept {
    if (factor < minimum) return minimum;
    if (factor > maximum) return maximum;
    return factor;
}

inline bool FactorInRange(const double factor, const double minimum,
                          const double maximum) noexcept {
    return factor >= minimum && factor <= maximum;
}

// `live` is the scale currently on the component, read before this call.
//
// The target is always `base * factor`, where `base` is the scale the component had when
// the plugin took it over. The base is never re-read from the component.
//
// Reading it back is what broke the plugin once already: a heuristic cannot tell the game's
// own scale from the plugin's previous output, so after a reload -- when the plugin's record
// is empty but the component still carries the plugin's last write -- the plugin adopted its
// own output as the game's scale. Every following factor was then multiplied by that
// inflated base, and the reset action, which sets the factor back to 1.0, restored the
// inflated value instead of the original size.
//
// The component reported to the plugin is a skeletal mesh, whose RelativeScale3D the game
// leaves at 1.0 (gameplay size changes go through the capsule), so nothing is lost by not
// tracking the game's own scale here.
//
// `live` is still read, because a mismatch means the plugin no longer knows what is on the
// component -- a reload, a possession, a character swap -- and its record must be dropped
// rather than trusted.
inline ScalePlan PlanScale(const TargetScaleState& state, const ScaleVector& live,
                           const double factor, const double epsilon) noexcept {
    ScalePlan plan;
    plan.resynced = !state.valid || !ScaleVectorEqual(live, state.applied, epsilon);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        plan.scale[axis] = state.base[axis] * factor;
    }
    plan.write = !ScaleVectorEqual(live, plan.scale, epsilon);
    return plan;
}

}  // namespace model_scale
