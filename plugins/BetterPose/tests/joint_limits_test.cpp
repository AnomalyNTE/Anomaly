#include "../joint_limits.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using namespace better_pose::limits;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

double Distance(const Quat &a, const Quat &b) {
  const double dot = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return 1.0 - (std::min)(dot, 1.0);
}

Quat Around(std::array<double, 3> axis, const double degrees) {
  const double n = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  const double h = degrees * 3.14159265358979323846 / 360.0;
  return {axis[0] / n * std::sin(h), axis[1] / n * std::sin(h), axis[2] / n * std::sin(h),
          std::cos(h)};
}

void Classification() {
  Check(LimitFor("Bip001-L-Finger1").kind == Kind::Free, "a finger root spreads: free");
  Check(LimitFor("Bip001-L-Finger0").kind == Kind::Free, "the thumb root: free");
  const auto index_mid = LimitFor("Bip001-L-Finger11");
  Check(index_mid.kind == Kind::Hinge && index_mid.axis == 2 && index_mid.sign == -1.0,
        "index middle joint: hinge about local Z, -Z bends (measured)");
  Check(LimitFor("Bip001-R-Finger42").axis == 2 && LimitFor("Bip001-R-Finger42").sign == -1.0,
        "the right pinky bends the same way as the left (measured)");
  const auto thumb_l = LimitFor("Bip001-L-Finger01");
  const auto thumb_r = LimitFor("Bip001-R-Finger01");
  Check(thumb_l.kind == Kind::Hinge && thumb_l.axis == 1 && thumb_l.sign == 1.0 &&
            thumb_r.axis == 1 && thumb_r.sign == -1.0,
        "thumb joints: hinge about local Y, mirrored sign (measured)");
  Check(LimitFor("Bip001-L-Finger13Nub").kind == Kind::Free, "nubs are not driven");
  Check(LimitFor("Bip001-L-UpperArm").kind == Kind::Free &&
            LimitFor("Bip001-L-Thigh").kind == Kind::Free &&
            LimitFor("Bip001-Head").kind == Kind::Free,
        "shoulders, hips and the head stay free");
  const auto elbow_l = LimitFor("Bip001-L-Forearm");
  const auto elbow_r = LimitFor("Bip001-R-Forearm");
  Check(elbow_l.kind == Kind::Hinge && elbow_l.axis == 2 && elbow_l.sign == 1.0 &&
            elbow_r.axis == 2 && elbow_r.sign == 1.0,
        "elbows: hinge about local Z, +Z bends, both sides (measured)");
  const auto knee = LimitFor("Bip001-R-Calf");
  Check(knee.kind == Kind::Hinge && knee.axis == 2 && knee.sign == 1.0 && knee.minimum < 0.0,
        "knees: hinge about local Z, room to straighten the rest bend (measured)");
  Check(LimitFor("Bone-L-ForeArm-Twist").kind == Kind::Free &&
            LimitFor("Bip001-L-ForearmTwist").kind == Kind::Free,
        "forearm twist helpers are not the elbow");
  Check(LimitFor("Finger11").kind == Kind::Free, "a finger with no side is not guessed");
}

void SplitAndCompose() {
  const Limit limit = LimitFor("Bip001-L-Finger11");  // Z, -1
  const Quat base = Normalize(Around({0.3, -0.8, 0.5}, 70.0));  // arbitrary rest frame
  // An offset that is a pure 30 degree bend: base * Rz(-30) * base^-1.
  const Quat bend30 = ComposeBend(limit, base, {0, 0, 0, 1}, 30.0);
  const auto split = SplitBend(limit, base, bend30);
  Check(std::abs(split.bend_degrees - 30.0) < 1e-9, "a pure bend reads back as its angle");
  Check(Distance(split.rest, {0, 0, 0, 1}) < 1e-12, "and leaves nothing else");

  // A bend on top of some other user rotation: the other part survives.
  const Quat other = Normalize(Around({1.0, 0.2, -0.4}, 12.0));
  const Quat mixed = ComposeBend(limit, base, other, 45.0);
  const auto mixed_split = SplitBend(limit, base, mixed);
  // Rotations do not commute, so the hinge share of a mix is only exact up
  // to the size of the other rotation; what the drag relies on is the exact
  // round trip below.
  Check(std::abs(mixed_split.bend_degrees - 45.0) < 12.0,
        "the bend read from a mix is within the other rotation's size");
  const Quat rebuilt = ComposeBend(limit, base, mixed_split.rest, mixed_split.bend_degrees);
  Check(Distance(rebuilt, mixed) < 1e-12, "split then compose is the identity");

  // The range: a finger does not fold back past a few degrees, nor past 100.
  const Quat too_far = ComposeBend(limit, base, {0, 0, 0, 1}, 170.0);
  Check(std::abs(SplitBend(limit, base, too_far).bend_degrees - limit.maximum) < 1e-9,
        "bend is clamped at the maximum");
  const Quat backwards = ComposeBend(limit, base, {0, 0, 0, 1}, -60.0);
  Check(std::abs(SplitBend(limit, base, backwards).bend_degrees - limit.minimum) < 1e-9,
        "bend is clamped at the minimum (small hyperextension only)");
}

void BendMovesOnlyInItsPlane() {
  // With base = identity, the hinge is the bone's own local Z. A bend must
  // keep a point on the local Z axis fixed and move one on local X within
  // the XY plane.
  const Limit limit = LimitFor("Bip001-L-Finger21");
  const Quat base{0, 0, 0, 1};
  const Quat bent = ComposeBend(limit, base, {0, 0, 0, 1}, 40.0);
  const auto rotate = [](const Quat &q, const std::array<double, 3> &v) {
    const Quat p{v[0], v[1], v[2], 0.0};
    const Quat r = Multiply(Multiply(q, p), Conjugate(q));
    return std::array<double, 3>{r[0], r[1], r[2]};
  };
  const auto z = rotate(bent, {0, 0, 1});
  const auto x = rotate(bent, {1, 0, 0});
  Check(std::abs(z[2] - 1.0) < 1e-12, "the hinge axis itself does not move");
  Check(std::abs(x[2]) < 1e-12, "the bone swings in the plane across the hinge");
  Check(std::abs(std::acos(x[0]) * 180.0 / 3.14159265358979323846 - 40.0) < 1e-9,
        "by exactly the bend");
}

void ElbowCannotFoldBackwards() {
  // An IK solve that would bend the elbow 20 degrees the wrong way (past
  // straight) is pulled back to the lower bound; a normal bend is kept.
  const Limit elbow = LimitFor("Bip001-L-Forearm");
  const Quat base = Normalize(Around({0.1, 0.9, -0.3}, 35.0));
  const Quat reversed = ComposeBend({Kind::Hinge, 2, 1.0, -180.0, 180.0}, base, {0, 0, 0, 1},
                                    -20.0);  // unclamped, as the solver might produce
  const auto split = SplitBend(elbow, base, reversed);
  Check(std::abs(split.bend_degrees + 20.0) < 1e-9, "the reversed bend is read as -20");
  const Quat fixed = ComposeBend(elbow, base, split.rest, split.bend_degrees);
  Check(std::abs(SplitBend(elbow, base, fixed).bend_degrees - elbow.minimum) < 1e-9,
        "a backwards elbow is held at its lower bound");
  const Quat normal = ComposeBend(elbow, base, {0, 0, 0, 1}, 90.0);
  const auto normal_split = SplitBend(elbow, base, normal);
  Check(Distance(ComposeBend(elbow, base, normal_split.rest, normal_split.bend_degrees), normal) <
            1e-12,
        "a bend inside the range is left exactly as solved");
}
}  // namespace

void BallJoints() {
  Check(BallFor("Bip001-L-UpperArm").valid && BallFor("Bip001-R-Thigh").valid,
        "shoulders and hips are ball joints");
  Check(!BallFor("Bip001-L-Forearm").valid && !BallFor("Bip001-Spine").valid &&
            !BallFor("Bone-L-UpperArm-Twist").valid && !BallFor("UpperArm").valid,
        "nothing else is, and a side is required");

  const Ball arm = BallFor("Bip001-L-UpperArm");
  const Quat base = Normalize(Around({0.2, 0.3, 0.9}, 40.0));
  const auto in_rest_frame = [&](const Quat &local) {
    return Normalize(Multiply(Multiply(base, local), Conjugate(base)));
  };
  const auto swing_of = [&](const Quat &offset) {
    const Quat local = Normalize(Multiply(Multiply(Conjugate(base), offset), base));
    const auto tip = RotateVector(Decompose(local).swing, {1.0, 0.0, 0.0});
    return std::acos(std::clamp(tip[0], -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
  };

  // Inside the range: untouched.
  const Quat small = in_rest_frame(Around({0.0, 0.0, 1.0}, 40.0));  // swing 40 toward +Y
  bool clamped = true;
  Check(Distance(ClampBall(arm, base, small, &clamped), small) < 1e-12 && !clamped,
        "a swing inside the range is left exactly alone");

  // Forward (+Y for the arm) by 170: past the 150 limit, pulled back to 150,
  // same direction.
  const Quat too_far = in_rest_frame(Around({0.0, 0.0, 1.0}, 170.0));
  const Quat limited = ClampBall(arm, base, too_far, &clamped);
  Check(clamped && std::abs(swing_of(limited) - 150.0) < 1e-6,
        "a forward swing past the limit stops on it");
  // Back (-Y): the limit is 60.
  const Quat back = in_rest_frame(Around({0.0, 0.0, -1.0}, 90.0));
  Check(std::abs(swing_of(ClampBall(arm, base, back)) - 60.0) < 1e-6,
        "the backward swing has its own, tighter limit");

  // Twist past 90 about the bone is clamped, the swing kept.
  const Quat twisted = in_rest_frame(Around({1.0, 0.0, 0.0}, 130.0));
  const Quat untwisted = ClampBall(arm, base, twisted);
  const Quat local = Normalize(Multiply(Multiply(Conjugate(base), untwisted), base));
  Check(std::abs(Decompose(local).twist * 180.0 / 3.14159265358979323846 - 90.0) < 1e-6,
        "twist is held at +-90");

  // Mirror: the right arm's "out" is the other sign of local Z.
  const Ball right = BallFor("Bip001-R-UpperArm");
  Check(arm.out_sign == 1.0 && right.out_sign == -1.0 && arm.forward_sign == 1.0 &&
            right.forward_sign == 1.0,
        "out is mirrored between the arms, forward is not (measured)");
  const Ball thigh_l = BallFor("Bip001-L-Thigh");
  const Ball thigh_r = BallFor("Bip001-R-Thigh");
  Check(thigh_l.forward_sign == -1.0 && thigh_l.out_sign == -1.0 && thigh_r.out_sign == 1.0,
        "the thighs' local Y points back and Z to the right (measured)");
}

int main() {
  Classification();
  SplitAndCompose();
  BendMovesOnlyInItsPlane();
  ElbowCannotFoldBackwards();
  BallJoints();
  std::cout << "PASS classification, split and compose, bend in plane, elbow range, "
               "ball joints\n";
  return 0;
}
