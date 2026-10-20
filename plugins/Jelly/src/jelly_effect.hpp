#pragma once
//
// 效果计算 (effect computation).
//
// The jelly effect is a near-volume-preserving three-axis squash and stretch:
//
//     q       = amplitude * sin(2*pi*frequency*elapsed + phase)
//     factorZ = exp(q)
//     factorX = factorY = exp(-q/2)
//     output  = base * factor
//
// The three factors multiply to exactly 1, so the volume is preserved. Every
// output is computed from the target's original scale and never from the value
// the plugin wrote last, so a long run cannot accumulate drift. A zero amplitude
// makes q zero and the output identical to the base.
//
// This header is pure: no SDK, no game, no clock. It is exercised directly by
// the offline fixture (tests/jelly_logic_tests.cpp).

#include <array>
#include <cmath>
#include <cstdint>

namespace jelly {

using Vector3 = std::array<double, 3>;

inline constexpr double kTau = 6.28318530717958647692528676655900576839;

// The window's and the configuration's accepted ranges.
inline constexpr double kMinimumAmplitude = 0.0;
inline constexpr double kMaximumAmplitude = 1.0;
inline constexpr double kMinimumFrequencyHz = 0.2;
inline constexpr double kMaximumFrequencyHz = 3.0;
// The rest between two bounces: zero keeps the effect as it has always been, a positive
// value holds the model at its original scale for that long after every full period.
inline constexpr double kMinimumPauseSeconds = 0.0;
inline constexpr double kMaximumPauseSeconds = 5.0;
// The spin the window can switch on: degrees per second about the model's own up axis.
inline constexpr double kMinimumSpinDegreesPerSecond = 0.0;
inline constexpr double kMaximumSpinDegreesPerSecond = 720.0;
inline constexpr double kDefaultSpinDegreesPerSecond = 120.0;

// Tolerance for "the component already carries what the plugin wrote" and for
// detecting that something else moved it.
inline constexpr double kScaleEpsilon = 1e-6;

inline bool IsFinite(const double value) noexcept { return std::isfinite(value); }

inline bool IsFinite(const Vector3& value) noexcept {
    return IsFinite(value[0]) && IsFinite(value[1]) && IsFinite(value[2]);
}

// A zero or non-finite axis would collapse or mirror the component in a way the
// effect cannot undo, so such a base is refused rather than scaled.
inline bool IsUsableScale(const Vector3& value) noexcept {
    return IsFinite(value) && value[0] != 0.0 && value[1] != 0.0 && value[2] != 0.0;
}

inline double Clamp(const double value, const double minimum, const double maximum) noexcept {
    if (!IsFinite(value)) return minimum;
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

inline double ClampAmplitude(const double value) noexcept {
    return Clamp(value, kMinimumAmplitude, kMaximumAmplitude);
}

inline double ClampFrequency(const double value) noexcept {
    return Clamp(value, kMinimumFrequencyHz, kMaximumFrequencyHz);
}

inline double ClampPause(const double value) noexcept {
    return Clamp(value, kMinimumPauseSeconds, kMaximumPauseSeconds);
}

inline double ClampSpin(const double value) noexcept {
    return Clamp(value, kMinimumSpinDegreesPerSecond, kMaximumSpinDegreesPerSecond);
}

// The instantaneous displacement of one target at one time.
inline double Wave(
    const double amplitude, const double frequency_hz, const double elapsed_seconds,
    const double phase) noexcept {
    return amplitude * std::sin(kTau * frequency_hz * elapsed_seconds + phase);
}

// The three factors for a displacement. Their product is exactly 1.
inline Vector3 Factors(const double displacement) noexcept {
    const double stretch = std::exp(displacement);
    const double squash = std::exp(-0.5 * displacement);
    return Vector3{squash, squash, stretch};
}

// output = base * factor, per axis, computed from the original base.
inline Vector3 Apply(const Vector3& base, const Vector3& factors) noexcept {
    return Vector3{base[0] * factors[0], base[1] * factors[1], base[2] * factors[2]};
}

inline Vector3 Output(
    const Vector3& base, const double amplitude, const double frequency_hz,
    const double elapsed_seconds, const double phase) noexcept {
    return Apply(base, Factors(Wave(amplitude, frequency_hz, elapsed_seconds, phase)));
}

// A bounce and the rest that follows it. The wave runs for exactly one period of the
// frequency and the model then stays at its base until the cycle comes round again; the
// target's stable phase becomes the time offset of its cycle, so targets stay staggered
// and the rest windows do not line up. A zero pause reduces this to the continuous wave
// above, term for term.
inline double CyclicWave(
    const double amplitude, const double frequency_hz, const double elapsed_seconds,
    const double phase, const double pause_seconds) noexcept {
    if (!(frequency_hz > 0.0)) return 0.0;
    const double period = 1.0 / frequency_hz;
    const double rest = pause_seconds > 0.0 ? pause_seconds : 0.0;
    const double cycle = period + rest;
    const double offset = phase / kTau * period;
    double into = std::fmod(elapsed_seconds + offset, cycle);
    if (into < 0.0) into += cycle;
    if (into >= period) return 0.0;  // resting between bounces
    return amplitude * std::sin(kTau * frequency_hz * into);
}

inline Vector3 OutputCyclic(
    const Vector3& base, const double amplitude, const double frequency_hz,
    const double elapsed_seconds, const double phase, const double pause_seconds) noexcept {
    return Apply(
        base,
        Factors(CyclicWave(amplitude, frequency_hz, elapsed_seconds, phase, pause_seconds)));
}

// The yaw a spinning model carries, measured from its own base rotation and never
// accumulated. It is deliberately independent of the pause between bounces: the model
// keeps turning while it rests.
inline double SpinDegrees(
    const double speed_degrees_per_second, const double elapsed_seconds,
    const double phase) noexcept {
    const double offset = phase / kTau * 360.0;
    double folded = std::fmod(speed_degrees_per_second * elapsed_seconds + offset, 360.0);
    if (folded < 0.0) folded += 360.0;
    return folded;
}

// A stable phase in [0, 2*pi) derived from the target's identity. The identity is
// the actor's object-registry index, so the phase survives frames and component
// swaps for as long as the actor lives and different actors do not move in
// lockstep.
inline double PhaseForIdentity(const std::uint64_t identity) noexcept {
    std::uint64_t mixed = identity * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
    mixed ^= mixed >> 30U;
    mixed *= 0xBF58476D1CE4E5B9ULL;
    mixed ^= mixed >> 27U;
    mixed *= 0x94D049BB133111EBULL;
    mixed ^= mixed >> 31U;
    const double unit = static_cast<double>(mixed >> 11U) * (1.0 / 9007199254740992.0);
    return unit * kTau;
}

inline bool NearlyEqual(
    const double left, const double right, const double epsilon = kScaleEpsilon) noexcept {
    double difference = left - right;
    if (difference < 0.0) difference = -difference;
    const double magnitude = right < 0.0 ? -right : right;
    return difference <= epsilon * (magnitude + 1.0);
}

inline bool Equal(
    const Vector3& left, const Vector3& right, const double epsilon = kScaleEpsilon) noexcept {
    return NearlyEqual(left[0], right[0], epsilon) &&
        NearlyEqual(left[1], right[1], epsilon) &&
        NearlyEqual(left[2], right[2], epsilon);
}

// The rotator a spinning model carries: its own rotation with the spin added to the yaw.
// Pitch and roll stay exactly as the model had them, so nothing tips over.
inline Vector3 SpinRotator(const Vector3& base_rotator, const double spin_degrees) noexcept {
    return Vector3{base_rotator[0], base_rotator[1] + spin_degrees, base_rotator[2]};
}

// Two rotators describe the same pose when pitch and roll match and the yaw differs by
// whole turns at most: the engine normalises angles on the way in, so a verified write has
// to compare the pose rather than the number.
inline bool SameRotation(
    const Vector3& left, const Vector3& right,
    const double degrees_epsilon = 1e-3) noexcept {
    if (!NearlyEqual(left[0], right[0]) || !NearlyEqual(left[2], right[2])) return false;
    double delta = std::fmod(left[1] - right[1], 360.0);
    if (delta < 0.0) delta += 360.0;
    if (delta > 180.0) delta = 360.0 - delta;
    return delta <= degrees_epsilon;
}

}  // namespace jelly
