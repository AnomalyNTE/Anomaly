#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Left/right pose mirroring: each bone's change from its rest pose, seen in
// component space, is reflected across the body's sagittal plane and applied
// to the partner's rest pose (see Rig below). Quaternions are {x, y, z, w}.
namespace better_pose::mirror {

using Quat = std::array<double, 4>;
using Vec = std::array<double, 3>;

inline Quat Multiply(const Quat &a, const Quat &b) noexcept {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

inline Quat Conjugate(const Quat &q) noexcept { return {-q[0], -q[1], -q[2], q[3]}; }

inline Quat Normalize(const Quat &q) noexcept {
  const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(length > 1e-12))
    return {0.0, 0.0, 0.0, 1.0};
  return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

// 0 for the same rotation (q and -q included), up to 1.
inline double Distance(const Quat &a, const Quat &b) noexcept {
  const double dot = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return 1.0 - (std::min)(dot, 1.0);
}

inline Vec Rotate(const Quat &q, const Vec &v) noexcept {
  const Quat p{v[0], v[1], v[2], 0.0};
  const Quat r = Multiply(Multiply(q, p), Conjugate(q));
  return {r[0], r[1], r[2]};
}

// The pure quaternion of a unit axis: 0 = x, 1 = y, 2 = z.
inline Quat AxisQuat(const int axis) noexcept {
  Quat q{0.0, 0.0, 0.0, 0.0};
  q[static_cast<std::size_t>(axis)] = 1.0;
  return q;
}

// Swap a side token: whole tokens between '-', '_' or ' ' that read L/R,
// l/r, Left/Right or left/right. Returns false when the name has no side.
inline bool SwapSide(const std::string_view name, std::string &swapped) {
  swapped.assign(name);
  bool changed = false;
  std::size_t start = 0;
  while (start <= swapped.size()) {
    std::size_t end = swapped.find_first_of("-_ ", start);
    if (end == std::string::npos)
      end = swapped.size();
    const std::string token = swapped.substr(start, end - start);
    std::string replacement;
    if (token == "L") replacement = "R";
    else if (token == "R") replacement = "L";
    else if (token == "l") replacement = "r";
    else if (token == "r") replacement = "l";
    else if (token == "Left") replacement = "Right";
    else if (token == "Right") replacement = "Left";
    else if (token == "left") replacement = "right";
    else if (token == "right") replacement = "left";
    if (!replacement.empty()) {
      swapped.replace(start, token.size(), replacement);
      end = start + replacement.size();
      changed = true;
    }
    start = end + 1;
  }
  return changed;
}

enum class Side : std::uint8_t { Centre, Left, Right };

inline Side SideOf(const std::string_view name) {
  std::string ignored;
  if (!SwapSide(name, ignored))
    return Side::Centre;
  // Which way the swap went tells the side: the first side token decides.
  std::size_t start = 0;
  while (start <= name.size()) {
    std::size_t end = name.find_first_of("-_ ", start);
    if (end == std::string_view::npos)
      end = name.size();
    const auto token = name.substr(start, end - start);
    if (token == "L" || token == "l" || token == "Left" || token == "left")
      return Side::Left;
    if (token == "R" || token == "r" || token == "Right" || token == "right")
      return Side::Right;
    start = end + 1;
  }
  return Side::Centre;
}

// The mirror partner of every bone: itself for a centre bone, the other side
// for a paired one, -1 for a side bone whose partner is missing.
inline std::vector<std::int32_t> Partners(const std::vector<std::string> &names) {
  std::vector<std::int32_t> partner(names.size(), -1);
  std::string swapped;
  for (std::size_t bone{}; bone != names.size(); ++bone) {
    if (!SwapSide(names[bone], swapped)) {
      partner[bone] = static_cast<std::int32_t>(bone);
      continue;
    }
    const auto it = std::find(names.begin(), names.end(), swapped);
    if (it != names.end())
      partner[bone] = static_cast<std::int32_t>(it - names.begin());
  }
  return partner;
}

// The mirror works in the rest pose's own terms, not in any assumed axis
// convention. Two measured facts are enough:
//   - the body's lateral axis L (component space): the mean left-minus-right
//     position of every paired bone;
//   - each bone's rest component rotation R_b, and its partner's R_p.
// Reflecting bone b's rest frame across the plane normal to L gives a frame
// with the opposite handedness; the partner's rest frame differs from it by
// a fixed, measured correction C_b = R_p^-1 * S * R_b * S (S the reflection),
// which is exactly a rotation when the rig is symmetric -- whatever local axis
// the rig happens to mirror about, and whether or not left and right bones
// were authored with flipped axes. The earlier version guessed that
// correction by snapping L to the nearest local axis, which is wrong for any
// bone whose rest frame is not aligned with the body (clavicles, hands).
//
// In rotation terms, with Q the reflection as a pure quaternion of L:
//     reflect(R) = Q * R * Q   (maps a rotation to its mirror image)
// and a component-space rotation W of bone b becomes, on the partner,
//     W' = reflect(W * R_b^-1) * R_p
// i.e. the change from rest is mirrored and applied to the partner's rest.
struct Rig {
  std::vector<std::uint8_t> mirrored;  // per bone: take part in the mirror
  Vec lateral{1.0, 0.0, 0.0};          // component space, unit, left minus right
  std::vector<Quat> rest;              // component-space rest rotations
  bool valid{};
};

// `rest` are the component-space rotations and `positions` the component
// positions of the rest (base) pose.
inline Rig BuildRig(const std::vector<Quat> &rest, const std::vector<Vec> &positions,
                    const std::vector<std::int32_t> &parents,
                    const std::vector<std::int32_t> &partner,
                    const std::vector<std::string> &names) {
  Rig rig;
  const std::size_t count = rest.size();
  if (positions.size() != count || parents.size() != count || partner.size() != count ||
      names.size() != count)
    return rig;
  Vec lateral{0.0, 0.0, 0.0};
  for (std::size_t bone{}; bone != count; ++bone) {
    if (partner[bone] < 0 || static_cast<std::size_t>(partner[bone]) == bone ||
        SideOf(names[bone]) != Side::Left)
      continue;
    const auto &l = positions[bone];
    const auto &r = positions[static_cast<std::size_t>(partner[bone])];
    for (std::size_t i{}; i != 3; ++i)
      lateral[i] += l[i] - r[i];
  }
  const double length =
      std::sqrt(lateral[0] * lateral[0] + lateral[1] * lateral[1] + lateral[2] * lateral[2]);
  if (!(length > 1e-6))
    return rig;
  for (auto &value : lateral)
    value /= length;
  rig.lateral = lateral;
  rig.rest = rest;
  rig.mirrored.assign(count, 0);
  for (std::size_t bone{}; bone != count; ++bone) {
    const std::int32_t parent = parents[bone];
    // The root and its direct children say where the character stands.
    if (parent < 0 || static_cast<std::size_t>(parent) >= count ||
        parents[static_cast<std::size_t>(parent)] < 0)
      continue;
    rig.mirrored[bone] = 1;
  }
  rig.valid = true;
  return rig;
}

inline bool Mirrored(const Rig &rig, const std::size_t bone) noexcept {
  return rig.mirrored[bone] != 0;
}

// The mirror image of a rotation across the plane normal to the lateral axis.
inline Quat Reflect(const Rig &rig, const Quat &q) noexcept {
  const Quat n{rig.lateral[0], rig.lateral[1], rig.lateral[2], 0.0};
  return Normalize(Multiply(Multiply(n, q), n));
}

// `bone`'s component rotation W, mirrored onto its partner.
inline Quat MirrorComponent(const Rig &rig, const std::size_t bone, const std::size_t target,
                            const Quat &world) noexcept {
  const Quat change = Multiply(world, Conjugate(rig.rest[bone]));
  return Normalize(Multiply(Reflect(rig, change), rig.rest[target]));
}

// Component rotations from local ones, parents first (any order: memoised).
inline std::vector<Quat> Components(const std::vector<std::int32_t> &parents,
                                    const std::vector<Quat> &locals) {
  const std::size_t count = locals.size();
  std::vector<Quat> world(count, Quat{0.0, 0.0, 0.0, 1.0});
  std::vector<std::uint8_t> done(count, 0);
  for (std::size_t bone{}; bone != count; ++bone) {
    std::vector<std::size_t> chain;
    std::size_t at = bone;
    while (done[at] == 0 && chain.size() <= count) {
      chain.push_back(at);
      const std::int32_t parent = parents[at];
      if (parent < 0 || static_cast<std::size_t>(parent) >= count)
        break;
      at = static_cast<std::size_t>(parent);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const std::int32_t parent = parents[*it];
      const Quat base = parent >= 0 && static_cast<std::size_t>(parent) < count && done[static_cast<std::size_t>(parent)] != 0
                            ? world[static_cast<std::size_t>(parent)]
                            : Quat{0.0, 0.0, 0.0, 1.0};
      world[*it] = Normalize(Multiply(base, locals[*it]));
      done[*it] = 1;
    }
  }
  return world;
}


inline Vec ReflectLateral(const Rig &rig, const Vec &v) noexcept {
  const double d = v[0] * rig.lateral[0] + v[1] * rig.lateral[1] + v[2] * rig.lateral[2];
  return {v[0] - 2.0 * d * rig.lateral[0], v[1] - 2.0 * d * rig.lateral[1],
          v[2] - 2.0 * d * rig.lateral[2]};
}

enum class Operation { Flip, LeftToRight, RightToLeft };

// Mirror the final local rotations `finals` (offset * base for every bone).
// Returns the new finals. Flip reflects the whole pose; the copies reflect
// one side onto the other and leave the source side and the centre alone.
inline std::vector<Quat> Apply(const Rig &rig, const std::vector<std::int32_t> &parents,
                               const std::vector<std::int32_t> &partner,
                               const std::vector<std::string> &names,
                               const std::vector<Quat> &finals, const Operation operation) {
  std::vector<Quat> out = finals;
  const std::size_t count = finals.size();
  if (!rig.valid || rig.rest.size() != count || parents.size() != count ||
      partner.size() != count || names.size() != count)
    return out;
  // Work in component space: the mirror is a statement about where each bone
  // points in the body, and doing it there means a parent that is mirrored
  // (or not) never distorts the child. Then convert back to locals.
  const std::vector<Quat> world = Components(parents, finals);
  std::vector<Quat> target_world = world;
  for (std::size_t bone{}; bone != count; ++bone) {
    if (!Mirrored(rig, bone) || partner[bone] < 0)
      continue;
    const auto target = static_cast<std::size_t>(partner[bone]);
    if (!Mirrored(rig, target))
      continue;
    const Side side = SideOf(names[bone]);
    // Centre bones mirror onto themselves in a flip, and stay in a copy.
    const bool take = operation == Operation::Flip ||
                      (operation == Operation::LeftToRight && side == Side::Left) ||
                      (operation == Operation::RightToLeft && side == Side::Right);
    if (take)
      target_world[target] = MirrorComponent(rig, bone, target, world[bone]);
  }
  for (std::size_t bone{}; bone != count; ++bone) {
    const std::int32_t parent = parents[bone];
    const Quat parent_world = parent >= 0 && static_cast<std::size_t>(parent) < count
                                  ? target_world[static_cast<std::size_t>(parent)]
                                  : Quat{0.0, 0.0, 0.0, 1.0};
    out[bone] = Normalize(Multiply(Conjugate(parent_world), target_world[bone]));
  }
  return out;
}

}  // namespace better_pose::mirror
