#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

// Anatomical joint limits for on-screen dragging.
//
// A free drag turns the bone about the view ray, which is right for a
// shoulder but wrong for a finger joint: it is a hinge, and dragging a
// fingertip seen from the front pushes the finger sideways out of its plane.
// With limits on, a hinge bone only turns about its own bend axis and the
// bend is held in a range. Everything else drags freely as before.
//
// Axes and bend directions were measured on the NTE Biped rest pose
// (pose.skeleton.json, both hands), as the local axis whose rotation swings
// the child joint along the palm normal:
//   index..pinky, every segment: local Z, -Z bends toward the palm (L and R)
//   thumb segments:              local Y, +Y bends on the left, -Y on the right
//   forearm (elbow):             local Z, +Z swings the hand forward (L and R)
//   calf (knee):                 local Z, +Z swings the foot back (L and R)
// The finger roots (Finger0..Finger4) also spread sideways, so they stay free.
// The rest pose already carries a little bend (elbow ~2 deg, knee ~9 deg),
// which the range is measured from.
namespace better_pose::limits {

enum class Kind { Free, Hinge };

struct Limit {
  Kind kind{Kind::Free};
  int axis{2};        // local axis of the hinge: 0 = X, 1 = Y, 2 = Z
  double sign{1.0};   // rotation sign about `axis` that bends (flexes) the joint
  double minimum{};   // degrees of bend from the rest pose
  double maximum{};
};

inline std::string Lower(const std::string_view name) {
  std::string out(name);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Biped finger bones are "Bip001-<L|R>-Finger<digit><segment>": digit 0 is the
// thumb; "Finger1" is the index root, "Finger11"/"Finger12" its middle and
// distal joints.
inline Limit LimitFor(const std::string_view name) {
  const std::string low = Lower(name);
  if (low.find("twist") != std::string::npos || low.find("adjust") != std::string::npos ||
      low.find("nub") != std::string::npos)
    return {};
  const bool left = low.find("-l-") != std::string::npos || low.find("_l_") != std::string::npos;
  const bool right = low.find("-r-") != std::string::npos || low.find("_r_") != std::string::npos;
  // Elbow and knee: a Biped forearm/calf is named "...-L-Forearm" / "...-L-Calf".
  // The small negative lower bound undoes the rest pose's own bend (the arm
  // can straighten fully) without letting the joint fold backwards.
  if (low.size() >= 7 && low.compare(low.size() - 7, 7, "forearm") == 0 && left != right)
    return {Kind::Hinge, 2, 1.0, -5.0, 150.0};
  if (low.size() >= 4 && low.compare(low.size() - 4, 4, "calf") == 0 && left != right)
    return {Kind::Hinge, 2, 1.0, -10.0, 150.0};
  const auto finger = low.find("finger");
  if (finger == std::string::npos)
    return {};
  const std::string rest = low.substr(finger + 6);
  if (rest.size() < 2 || !std::isdigit(static_cast<unsigned char>(rest[0])) ||
      !std::isdigit(static_cast<unsigned char>(rest[1])))
    return {};  // finger root: free (spreads and bends)
  if (left == right)
    return {};  // side unknown: do not guess a direction
  if (rest[0] == '0')
    return {Kind::Hinge, 1, left ? 1.0 : -1.0, -10.0, 80.0};  // thumb
  return {Kind::Hinge, 2, -1.0, -5.0, 100.0};
}

// Quaternions {x, y, z, w}.
using Quat = std::array<double, 4>;

inline Quat Multiply(const Quat &a, const Quat &b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

inline Quat Normalize(const Quat &q) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(n > 1e-12))
    return {0.0, 0.0, 0.0, 1.0};
  return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}

inline Quat Conjugate(const Quat &q) { return {-q[0], -q[1], -q[2], q[3]}; }

inline Quat AxisAngle(const int axis, const double radians) {
  Quat q{0.0, 0.0, 0.0, std::cos(radians * 0.5)};
  q[static_cast<std::size_t>(axis)] = std::sin(radians * 0.5);
  return q;
}

// The pose model is local = offset * base. With the hinge as the base's own
// local axis, a bend b of the joint is local = base * R(axis, b), i.e.
//   offset = base * R(axis, b) * base^-1 * rest_offset
// where rest_offset is whatever else the user has put on the bone. Split the
// current offset into that bend and the rest: the bend is the twist of
// (base^-1 * offset * base) about the axis.
struct Split {
  double bend_degrees{};  // in the bending direction (sign applied)
  Quat rest{0.0, 0.0, 0.0, 1.0};  // offset with the bend removed
};

inline Split SplitBend(const Limit &limit, const Quat &base, const Quat &offset) {
  constexpr double kDegrees = 180.0 / 3.14159265358979323846;
  const Quat local = Normalize(Multiply(Multiply(Conjugate(base), offset), base));
  const double along = local[static_cast<std::size_t>(limit.axis)];
  double angle = 2.0 * std::atan2(along, local[3]);  // radians about +axis
  if (angle > 3.14159265358979323846)
    angle -= 2.0 * 3.14159265358979323846;
  if (angle < -3.14159265358979323846)
    angle += 2.0 * 3.14159265358979323846;
  const Quat bend_world = Multiply(Multiply(base, AxisAngle(limit.axis, angle)), Conjugate(base));
  Split split;
  split.bend_degrees = angle * kDegrees * limit.sign;
  split.rest = Normalize(Multiply(Conjugate(bend_world), offset));
  return split;
}

// The offset for a bend (degrees, bending direction), clamped to the range.
inline Quat ComposeBend(const Limit &limit, const Quat &base, const Quat &rest,
                        const double bend_degrees) {
  constexpr double kRadians = 3.14159265358979323846 / 180.0;
  const double bend = std::clamp(bend_degrees, limit.minimum, limit.maximum);
  const Quat bend_world = Multiply(
      Multiply(base, AxisAngle(limit.axis, bend * limit.sign * kRadians)), Conjugate(base));
  return Normalize(Multiply(bend_world, rest));
}

// ---------------------------------------------------------------------------
// Ball joints: shoulder (upper arm) and hip (thigh).
//
// A ball joint is free in all three directions, so the drag stays free and
// the result is pushed back inside a range afterwards. The range is written
// in body terms and measured from a natural rest direction, not the game's
// A-pose:
//   swing: where the bone points, as the angle between the rest pointing
//          direction and the new one, limited per quadrant of that swing
//          (forward/back, out/in), blended between them elliptically;
//   twist: rotation about the bone's own axis, limited to +-twist.
//
// Measured on the NTE Biped rest pose (pose.skeleton.json): every upper arm
// and thigh has its bone along local X; local Y points forward (upper arms)
// or backward (thighs); local Z completes the frame (for the upper arm it
// points out and up, for the thigh toward the body's right on both sides).
// So in the bone's own rest frame: X = bone, +-Y = forward/back, +-Z = the
// side axis; which sign of Z is "out" is given per bone below.
struct Ball {
  bool valid{};
  double forward{};  // degrees of swing toward the body's front
  double back{};     // toward the back
  double out{};      // away from the body's midline
  double in{};       // across the body
  double twist{};    // +- about the bone
  double forward_sign{1.0};  // local Y sign that is "forward" (upper arm +1, thigh -1)
  double out_sign{1.0};      // local Z sign that is "out"
};

// The swing is measured from the rest pose. The game's rest pose has the
// upper arm about 55 degrees below horizontal (bone L+0.57 U-0.82) and the
// thigh straight down, which the numbers below account for: an arm raised
// straight up is ~145 degrees of swing from this rest, straight forward ~90,
// hanging straight down at the side ~35 of "in".
inline Ball BallFor(const std::string_view name) {
  const std::string low = Lower(name);
  if (low.find("twist") != std::string::npos || low.find("adjust") != std::string::npos)
    return {};
  const bool left = low.find("-l-") != std::string::npos || low.find("_l_") != std::string::npos;
  const bool right = low.find("-r-") != std::string::npos || low.find("_r_") != std::string::npos;
  if (left == right)
    return {};
  const auto ends_with = [&](const std::string_view tail) {
    return low.size() >= tail.size() && low.compare(low.size() - tail.size(), tail.size(), tail) == 0;
  };
  if (ends_with("upperarm")) {
    // Measured: local Z is out-and-up on the left, out-and-down on the
    // right mirrored -- it points toward the body's +left on both, so "out"
    // is +Z on the left arm and -Z on the right.
    return {true, 150.0, 60.0, 165.0, 45.0, 90.0, 1.0, left ? 1.0 : -1.0};
  }
  if (ends_with("thigh")) {
    // Measured: local Y points back and local Z to the body's right on both
    // legs, so forward is -Y, and "out" is -Z on the left, +Z on the right.
    return {true, 120.0, 30.0, 50.0, 25.0, 45.0, -1.0, left ? -1.0 : 1.0};
  }
  return {};
}

// Swing-twist decomposition in the bone's own rest frame: `local` (offset
// expressed in the rest frame, base^-1 * offset * base) = swing * twist, with
// twist about local X (the bone) and swing moving the bone's direction.
struct SwingTwist {
  Quat swing{0.0, 0.0, 0.0, 1.0};
  double twist{};  // radians about +X
};

inline SwingTwist Decompose(const Quat &local) {
  SwingTwist out;
  double angle = 2.0 * std::atan2(local[0], local[3]);
  if (angle > 3.14159265358979323846)
    angle -= 2.0 * 3.14159265358979323846;
  if (angle < -3.14159265358979323846)
    angle += 2.0 * 3.14159265358979323846;
  out.twist = angle;
  out.swing = Normalize(Multiply(local, Conjugate(AxisAngle(0, angle))));
  return out;
}

inline std::array<double, 3> RotateVector(const Quat &q, const std::array<double, 3> &v) {
  const Quat r = Multiply(Multiply(q, {v[0], v[1], v[2], 0.0}), Conjugate(q));
  return {r[0], r[1], r[2]};
}

// The allowed swing angle for a swing direction (y, z in the rest frame,
// the bone's tip displacement off the X axis): an ellipse between the
// quadrant limits.
inline double AllowedSwing(const Ball &ball, const double y, const double z) {
  const double forward = y * ball.forward_sign;
  const double out = z * ball.out_sign;
  const double a = forward >= 0.0 ? ball.forward : ball.back;
  const double b = out >= 0.0 ? ball.out : ball.in;
  const double length = std::sqrt(y * y + z * z);
  if (!(length > 1e-12))
    return (std::min)(a, b);
  const double cy = forward / length;
  const double cz = out / length;
  // Polar radius of the ellipse with semi-axes a (forward/back), b (out/in).
  return 1.0 / std::sqrt((cy * cy) / (a * a) + (cz * cz) / (b * b));
}

// Clamp an offset (parent frame) of a ball joint whose rest local rotation is
// `base`. Returns the offset unchanged when it is inside the range.
inline Quat ClampBall(const Ball &ball, const Quat &base, const Quat &offset,
                      bool *clamped = nullptr) {
  constexpr double kDegrees = 180.0 / 3.14159265358979323846;
  if (clamped != nullptr)
    *clamped = false;
  if (!ball.valid)
    return offset;
  const Quat local = Normalize(Multiply(Multiply(Conjugate(base), offset), base));
  SwingTwist st = Decompose(local);
  bool changed = false;
  const double twist_limit = ball.twist / kDegrees;
  if (std::abs(st.twist) > twist_limit) {
    st.twist = std::clamp(st.twist, -twist_limit, twist_limit);
    changed = true;
  }
  // Where the swing sends the bone (local X).
  const auto tip = RotateVector(st.swing, {1.0, 0.0, 0.0});
  const double swing_angle = std::acos(std::clamp(tip[0], -1.0, 1.0)) * kDegrees;
  const double allowed = AllowedSwing(ball, tip[1], tip[2]);
  if (swing_angle > allowed) {
    // Same direction, shorter swing: rotate about the swing's own axis.
    std::array<double, 3> axis{0.0, -tip[2], tip[1]};  // X cross tip
    const double n = std::sqrt(axis[1] * axis[1] + axis[2] * axis[2]);
    if (n > 1e-12) {
      const double half = allowed / kDegrees * 0.5;
      st.swing = {0.0, axis[1] / n * std::sin(half), axis[2] / n * std::sin(half), std::cos(half)};
      changed = true;
    }
  }
  if (!changed)
    return offset;
  if (clamped != nullptr)
    *clamped = true;
  const Quat limited = Normalize(Multiply(st.swing, AxisAngle(0, st.twist)));
  return Normalize(Multiply(Multiply(base, limited), Conjugate(base)));
}

}  // namespace better_pose::limits
