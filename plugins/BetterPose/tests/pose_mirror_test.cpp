#include "../pose_mirror.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using namespace better_pose::mirror;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

double Dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Quat AxisAngle(Vec axis, const double radians) {
  const double length = std::sqrt(Dot(axis, axis));
  for (auto &v : axis)
    v /= length;
  const double s = std::sin(radians * 0.5);
  return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(radians * 0.5)};
}

// A symmetric toy body, deliberately authored like a real rig: the right
// side's local axes are *not* the left's reflected (the right arm's bone
// axis points the other way, and every bone carries an arbitrary roll), and
// the clavicle/hand frames are not aligned with the body. The earlier
// axis-snapping mirror broke exactly on this.
struct Body {
  std::vector<std::string> names;
  std::vector<std::int32_t> parents;
  std::vector<Quat> rest_locals;
  std::vector<Vec> rest_positions;  // component
  std::vector<std::int32_t> partner;
  Rig rig;
};

// Component space: +X is the body's left, +Y forward, +Z up.
Body MakeBody() {
  Body b;
  // name, parent, component rest rotation, component position
  struct Spec {
    const char *name;
    std::int32_t parent;
    Quat world;
    Vec position;
  };
  const Quat roll_a = AxisAngle({0.3, 0.8, 0.5}, 0.9);
  const Quat roll_b = AxisAngle({-0.6, 0.2, 0.7}, 2.1);
  // Left bones: arbitrary frames.
  const Quat l_clav = Normalize(Multiply(AxisAngle({0, 0, 1}, 0.4), roll_a));
  const Quat l_arm = Normalize(Multiply(AxisAngle({0, 1, 0}, -0.7), roll_b));
  const Quat l_hand = Normalize(Multiply(AxisAngle({1, 1, 0}, 1.3), roll_a));
  // Right bones: the reflected left frame, then an extra authored twist
  // about some local axis (a different axis convention on the right side).
  Rig reflect;
  reflect.lateral = {1.0, 0.0, 0.0};
  const auto right_of = [&](const Quat &left, const Quat &authored) {
    return Normalize(Multiply(Reflect(reflect, left), authored));
  };
  const Quat r_clav = right_of(l_clav, {1, 0, 0, 0});    // 180 deg about local X
  const Quat r_arm = right_of(l_arm, {0, 0, 1, 0});      // 180 deg about local Z
  const Quat r_hand = right_of(l_hand, {0, 1, 0, 0});    // 180 deg about local Y
  const Quat spine = AxisAngle({0.1, 0.0, 1.0}, 0.0);
  const Spec specs[] = {
      {"Root", -1, {0, 0, 0, 1}, {0, 0, 0}},
      {"Bip001", 0, AxisAngle({0, 0, 1}, 1.2), {0, 0, 90}},
      {"Bip001-Spine", 1, spine, {0, 0, 100}},
      {"Bip001-L-Clavicle", 2, l_clav, {5, 0, 140}},
      {"Bip001-R-Clavicle", 2, r_clav, {-5, 0, 140}},
      {"Bip001-L-UpperArm", 3, l_arm, {18, 0, 140}},
      {"Bip001-R-UpperArm", 4, r_arm, {-18, 0, 140}},
      {"Bip001-L-Hand", 5, l_hand, {45, 5, 120}},
      {"Bip001-R-Hand", 6, r_hand, {-45, 5, 120}},
  };
  std::vector<Quat> world;
  for (const auto &s : specs) {
    b.names.emplace_back(s.name);
    b.parents.push_back(s.parent);
    world.push_back(Normalize(s.world));
    b.rest_positions.push_back(s.position);
  }
  for (std::size_t i{}; i != world.size(); ++i) {
    const Quat parent = b.parents[i] >= 0 ? world[static_cast<std::size_t>(b.parents[i])]
                                          : Quat{0, 0, 0, 1};
    b.rest_locals.push_back(Normalize(Multiply(Conjugate(parent), world[i])));
  }
  b.partner = Partners(b.names);
  b.rig = BuildRig(world, b.rest_positions, b.parents, b.partner, b.names);
  return b;
}

// Where a bone's local +X axis points in component space (the "bone
// direction"), and a second axis, for comparing orientations as geometry.
Vec AxisOf(const Quat &world, const Vec &local) { return Rotate(world, local); }

Vec ReflectX(const Vec &v) { return {-v[0], v[1], v[2]}; }

// Mirror symmetry as geometry: the partner's frame must be the reflection
// of the bone's frame, up to the partner's authored twist (measured at rest).
bool Symmetric(const Body &b, const std::vector<Quat> &world, const std::size_t left,
               const std::size_t right) {
  // The authored correction between reflected-left and right at rest.
  const Quat rest_correction =
      Multiply(Conjugate(Reflect(b.rig, b.rig.rest[left])), b.rig.rest[right]);
  const Quat expected = Normalize(Multiply(Reflect(b.rig, world[left]), rest_correction));
  return Distance(expected, world[right]) < 1e-9;
}

void Names() {
  std::string s;
  Check(SwapSide("Bip001-L-Thigh", s) && s == "Bip001-R-Thigh", "Biped side token");
  Check(SwapSide("Bone-R-ForeArm-Twist1", s) && s == "Bone-L-ForeArm-Twist1", "twist side token");
  Check(SwapSide("Bon_eyebrow01_L", s) && s == "Bon_eyebrow01_R", "underscore suffix");
  Check(SwapSide("foot_r", s) && s == "foot_l", "lowercase suffix");
  Check(!SwapSide("Bip001-Pelvis", s), "centre bone has no side");
  Check(!SwapSide("Bone_hairLb00", s), "a letter inside a word is not a side");
  const std::vector<std::string> names{"Bip001-Head", "Bip001-L-Hand", "Bip001-R-Hand",
                                       "Bip001-L-Prop"};
  const auto partner = Partners(names);
  Check(partner[0] == 0 && partner[1] == 2 && partner[2] == 1 && partner[3] == -1,
        "partners: centre maps to itself, pairs to each other, orphans to none");
}

void RestIsFixed() {
  const auto b = MakeBody();
  Check(b.rig.valid, "rig builds");
  Check(b.rig.lateral[0] > 0.999, "lateral axis is the body's left");
  const auto flipped =
      Apply(b.rig, b.parents, b.partner, b.names, b.rest_locals, Operation::Flip);
  for (std::size_t i{}; i != flipped.size(); ++i)
    Check(Distance(flipped[i], b.rest_locals[i]) < 1e-9,
          "flipping the rest pose changes nothing, even with mismatched right-side axes");
}

void RaisedArmMovesToTheOtherSide() {
  const auto b = MakeBody();
  // Raise the left arm: rotate the upper arm 60 deg about the body's forward
  // axis (component space), expressed as a local change.
  auto locals = b.rest_locals;
  const auto rest_world = Components(b.parents, b.rest_locals);
  const Quat lift = AxisAngle({0, 1, 0}, 1.05);
  const Quat new_world = Normalize(Multiply(lift, rest_world[5]));
  locals[5] = Normalize(Multiply(Conjugate(rest_world[3]), new_world));
  // Bend the left hand a little too.
  locals[7] = Normalize(Multiply(AxisAngle({0, 0, 1}, 0.5), locals[7]));
  const auto before = Components(b.parents, locals);
  const auto flipped = Apply(b.rig, b.parents, b.partner, b.names, locals, Operation::Flip);
  const auto after = Components(b.parents, flipped);
  // The right arm now points where the left one pointed, reflected.
  const Vec left_dir = AxisOf(before[5], {1, 0, 0});
  const Vec left_side = AxisOf(before[5], {0, 1, 0});
  // Compare as geometry through the rest correction.
  Check(Symmetric(b, before, 5, 6) == false, "the raised pose is asymmetric to begin with");
  // After the flip the pose is the mirror image of the one before:
  for (const auto pair : {std::pair{3, 4}, std::pair{5, 6}, std::pair{7, 8}}) {
    const auto l = static_cast<std::size_t>(pair.first);
    const auto r = static_cast<std::size_t>(pair.second);
    const Quat correction_r =
        Multiply(Conjugate(Reflect(b.rig, b.rig.rest[l])), b.rig.rest[r]);
    const Quat correction_l =
        Multiply(Conjugate(Reflect(b.rig, b.rig.rest[r])), b.rig.rest[l]);
    Check(Distance(after[r], Normalize(Multiply(Reflect(b.rig, before[l]), correction_r))) < 1e-9,
          "the right side becomes the mirrored left");
    Check(Distance(after[l], Normalize(Multiply(Reflect(b.rig, before[r]), correction_l))) < 1e-9,
          "the left side becomes the mirrored right");
  }
  // Physically: the raised left upper arm's bone axis, reflected, is the
  // right upper arm's bone axis after the flip (with the right side's axis
  // convention: at rest its local X is the reflected left's local X turned
  // 180 deg about local Z, i.e. negated).
  const Vec right_dir = AxisOf(after[6], {1, 0, 0});
  const Vec expected = ReflectX(left_dir);
  Check(std::abs(std::abs(Dot(right_dir, expected)) - 1.0) < 1e-9,
        "the right arm points where the left arm pointed, mirrored");
  static_cast<void>(left_side);
  // Twice is the identity.
  const auto twice = Apply(b.rig, b.parents, b.partner, b.names, flipped, Operation::Flip);
  for (std::size_t i{}; i != twice.size(); ++i)
    Check(Distance(twice[i], locals[i]) < 1e-9, "flipping twice restores the pose");
}

void CentreBonesFlipTheirTwist() {
  const auto b = MakeBody();
  auto locals = b.rest_locals;
  // Turn the spine to the left (about +Z) by 30 deg.
  locals[2] = Normalize(Multiply(AxisAngle({0, 0, 1}, 0.52), locals[2]));
  const auto flipped = Apply(b.rig, b.parents, b.partner, b.names, locals, Operation::Flip);
  const auto after = Components(b.parents, flipped);
  const auto rest_world = Components(b.parents, b.rest_locals);
  const Quat change = Multiply(after[2], Conjugate(rest_world[2]));
  // A left turn about Z mirrors to a right turn: the change is -30 deg about Z.
  Check(std::abs(change[2] / std::sqrt(change[0] * change[0] + change[1] * change[1] +
                                       change[2] * change[2] + 1e-30) *
                     (change[3] >= 0 ? 1 : -1) +
                 1.0) < 1e-6 &&
            std::abs(2.0 * std::acos(std::min(1.0, std::abs(change[3]))) - 0.52) < 1e-6,
        "a spine turn flips to the other direction");
  Check(Distance(after[1], rest_world[1]) < 1e-12, "Bip001 (where the body stands) is untouched");
}

void CopyOneSide() {
  const auto b = MakeBody();
  auto locals = b.rest_locals;
  locals[5] = Normalize(Multiply(AxisAngle({0, 0, 1}, 0.8), locals[5]));   // left arm
  locals[6] = Normalize(Multiply(AxisAngle({1, 0, 0}, -0.4), locals[6]));  // right arm, other
  locals[2] = Normalize(Multiply(AxisAngle({0, 0, 1}, 0.3), locals[2]));   // spine turned
  const auto before = Components(b.parents, locals);
  const auto copied = Apply(b.rig, b.parents, b.partner, b.names, locals, Operation::LeftToRight);
  const auto after = Components(b.parents, copied);
  Check(Distance(after[5], before[5]) < 1e-9 && Distance(after[3], before[3]) < 1e-9,
        "left to right keeps the left side");
  Check(Distance(after[2], before[2]) < 1e-9, "left to right keeps the centre");
  const Quat correction =
      Multiply(Conjugate(Reflect(b.rig, b.rig.rest[5])), b.rig.rest[6]);
  Check(Distance(after[6], Normalize(Multiply(Reflect(b.rig, before[5]), correction))) < 1e-9,
        "left to right writes the mirrored left arm onto the right");
}

void MeasuredNteLegs() {
  // The live NTE capture (component rotations and positions of the rest
  // pose): mirroring the rest pose must stay the rest pose to within the
  // capture's own asymmetry.
  const std::vector<std::string> names{"Root", "Bip001", "Bip001-Pelvis", "Bip001-L-Thigh",
                                       "Bip001-R-Thigh", "Bip001-L-Calf", "Bip001-R-Calf"};
  const std::vector<std::int32_t> parents{-1, 0, 1, 2, 2, 3, 4};
  const std::vector<Quat> world{
      {0, 0, 0, 1},
      Normalize({-0.0043, 0.0047, 0.7571, 0.6532}),
      Normalize({-0.7054, -0.0565, -0.7050, 0.0474}),
      Normalize({-0.7263, -0.0867, 0.6801, -0.0494}),
      Normalize({0.7052, -0.0273, -0.7074, -0.0395}),
      Normalize({-0.7309, -0.0293, 0.6741, -0.1027}),
      Normalize({0.7022, -0.0713, -0.7084, 0.0047})};
  const std::vector<Vec> positions{{0, 0, 0},         {-1.67, 0.94, 87.00}, {-1.67, 0.94, 85.90},
                                   {4.91, 1.91, 85.89}, {-8.26, -0.04, 85.91}, {7.23, 4.20, 47.19},
                                   {-8.34, 0.64, 47.08}};
  const auto partner = Partners(names);
  const Rig rig = BuildRig(world, positions, parents, partner, names);
  Check(rig.valid, "NTE rig builds");
  std::vector<Quat> locals;
  for (std::size_t i{}; i != world.size(); ++i) {
    const Quat parent = parents[i] >= 0 ? world[static_cast<std::size_t>(parents[i])] : Quat{0, 0, 0, 1};
    locals.push_back(Normalize(Multiply(Conjugate(parent), world[i])));
  }
  const auto flipped = Apply(rig, parents, partner, names, locals, Operation::Flip);
  for (std::size_t i{}; i != flipped.size(); ++i)
    Check(Distance(flipped[i], locals[i]) < 1e-12, "the NTE rest pose is fixed by a flip");
  // Bend the left knee 40 deg about its local Z (the Biped knee hinge), flip,
  // and the right knee bends by the same amount about its own hinge.
  auto bent = locals;
  bent[5] = Normalize(Multiply(bent[5], AxisAngle({0, 0, 1}, 0.7)));
  const auto after = Components(parents, Apply(rig, parents, partner, names, bent, Operation::Flip));
  const auto rest_world = Components(parents, locals);
  const Quat right_change = Multiply(Conjugate(rest_world[6]), after[6]);
  const double angle = 2.0 * std::acos(std::min(1.0, std::abs(right_change[3])));
  Check(std::abs(angle - 0.7) < 0.02, "the right knee bends by the left knee's angle");
  // The left and right calves' rest frames differ from exact mirror images
  // by a 178 deg turn about local ~Y (the right side's authored axes), which
  // maps a left bend about +Z to a right bend about -Z: still the knee hinge.
  const Vec hinge{right_change[0], right_change[1], right_change[2]};
  Check(std::abs(hinge[2]) / std::sqrt(Dot(hinge, hinge)) > 0.95,
        "and about its own knee hinge (local Z)");
}

void DegenerateInput() {
  const Rig bad = BuildRig({}, {}, {}, {}, {});
  Check(!bad.valid, "an empty rig is invalid");
  const std::vector<Quat> finals{{0, 0, 0, 1}};
  const auto same = Apply(bad, {-1}, {0}, {"Root"}, finals, Operation::Flip);
  Check(same.size() == 1 && Distance(same[0], finals[0]) < 1e-12,
        "an invalid rig leaves the pose alone");
}
}  // namespace

int main() {
  Names();
  RestIsFixed();
  RaisedArmMovesToTheOtherSide();
  CentreBonesFlipTheirTwist();
  CopyOneSide();
  MeasuredNteLegs();
  DegenerateInput();
  std::cout << "PASS side names, rest fixed, raised arm mirrors, centre twist flips, "
               "one-side copy, NTE legs, degenerate input\n";
  return 0;
}
