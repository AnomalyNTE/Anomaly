#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Keyframe animation for the manual pose: the user poses the character, adds
// that pose as a key at a frame, poses again at a later frame, and playback
// interpolates between the keys. Pure data and math, no game access.
//
// A key holds whatever the user chose to record, each part optional:
//   - the pose: final named local transforms for new keys, legacy slider
//     offsets (pitch/yaw/roll, degrees, applied as offset * reference) for old
//     keys, and the body offset (cm);
//   - the expression: the driven morph weights, by morph name;
//   - the pose camera: focus, yaw, pitch, distance and lens.
// A part a key does not carry is not keyed there: interpolation for that part
// runs between the neighbouring keys that do carry it, and outside them it
// holds the nearest one. So a camera move can be keyed on two keys of a long
// dance without freezing the camera at every pose key in between.
//
// Bones are stored by name, like pose files (pose_document.hpp): an index is
// only this skeleton's order, and an animation is worth more when it carries
// over to another character with the same rig.
namespace better_pose::keyframes {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kFramesPerSecond = 30.0;  // MMD's rate, and the camera's
inline constexpr std::uint32_t kMaximumFrame = 30U * 60U * 30U;  // 30 minutes
inline constexpr std::size_t kMaximumKeys = 2000;

enum class Ease : std::uint8_t {
  Linear = 0,
  InOut = 1,  // smoothstep: starts and ends at rest
};

// The ease applies to the segment that *starts* at the key, i.e. how the
// motion leaves this key towards the next one.
inline double ApplyEase(const Ease ease, const double t) noexcept {
  const double u = std::clamp(t, 0.0, 1.0);
  switch (ease) {
  case Ease::InOut:
    return u * u * (3.0 - 2.0 * u);
  case Ease::Linear:
  default:
    return u;
  }
}

struct BoneKey {
  std::string name;
  double pitch{};
  double yaw{};
  double roll{};
};

// Named final local transform: quaternion xyzw, translation xyz + pad,
// scale xyz + pad. New keys own these values, never a pose-library handle.
struct LocalBone {
  std::string name;
  std::array<double, 12> transform{0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                   0.0, 0.0, 1.0, 1.0, 1.0, 0.0};
};

struct MorphKey {
  std::string name;
  float weight{};
};

struct CameraKey {
  std::array<double, 3> focus{};
  double yaw{};
  double pitch{};
  double distance{300.0};
  float fov{};  // 0: the game's own lens
};

struct Key {
  std::uint32_t frame{};
  Ease ease{Ease::InOut};
  bool has_pose{};
  std::vector<BoneKey> bones;  // only bones with a non-zero offset
  std::array<double, 3> root_offset{};
  bool has_expression{};
  std::vector<MorphKey> morphs;  // the morphs the user drives at this key
  bool has_camera{};
  CameraKey camera;
  bool has_local_pose{};
  std::vector<LocalBone> local_pose;
};

// ---------------------------------------------------------------------------
// Rotation math, in the plugin's convention (UE FRotator <-> FQuat, degrees),
// duplicated here so this header stands alone. Interpolating the three angles
// directly would take the long way round across +-180 degrees and wobble
// between the axes; a pose key is a rotation, so it is interpolated as one.

struct Quat {
  double x{}, y{}, z{}, w{1.0};
};

inline Quat FromRotator(const double pitch, const double yaw, const double roll) noexcept {
  const double half = kPi / 360.0;
  const double sp = std::sin(pitch * half), cp = std::cos(pitch * half);
  const double sy = std::sin(yaw * half), cy = std::cos(yaw * half);
  const double sr = std::sin(roll * half), cr = std::cos(roll * half);
  return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy,
          cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}

inline double WrapDegrees(double value) noexcept {
  value = std::remainder(value, 360.0);
  return value <= -180.0 ? value + 360.0 : value;
}

inline std::array<double, 3> ToRotator(Quat q) noexcept {
  const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(length > 1e-12))
    return {0.0, 0.0, 0.0};
  q = {q.x / length, q.y / length, q.z / length, q.w / length};
  const double degrees = 180.0 / kPi;
  const double singularity = q.z * q.x - q.w * q.y;
  const double yaw = std::atan2(2.0 * (q.w * q.z + q.x * q.y),
                                1.0 - 2.0 * (q.y * q.y + q.z * q.z)) *
                     degrees;
  constexpr double kThreshold = 0.4999995;
  double pitch{}, roll{};
  if (singularity < -kThreshold) {
    pitch = -90.0;
    roll = -yaw - 2.0 * std::atan2(q.x, q.w) * degrees;
  } else if (singularity > kThreshold) {
    pitch = 90.0;
    roll = yaw - 2.0 * std::atan2(q.x, q.w) * degrees;
  } else {
    pitch = std::asin(std::clamp(2.0 * singularity, -1.0, 1.0)) * degrees;
    roll = std::atan2(-2.0 * (q.w * q.x + q.y * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)) *
           degrees;
  }
  return {WrapDegrees(pitch), WrapDegrees(yaw), WrapDegrees(roll)};
}

// Shortest-arc spherical interpolation.
inline Quat Slerp(const Quat &a, Quat b, const double t) noexcept {
  double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (dot < 0.0) {
    b = {-b.x, -b.y, -b.z, -b.w};
    dot = -dot;
  }
  double wa = 1.0 - t;
  double wb = t;
  if (dot < 0.9995) {
    const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
    const double s = std::sin(theta);
    wa = std::sin((1.0 - t) * theta) / s;
    wb = std::sin(t * theta) / s;
  }
  Quat out{wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
  const double length =
      std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
  if (length > 1e-12)
    out = {out.x / length, out.y / length, out.z / length, out.w / length};
  return out;
}

inline std::array<double, 3> InterpolateRotator(const std::array<double, 3> &a,
                                                const std::array<double, 3> &b,
                                                const double t) noexcept {
  if (t <= 0.0)
    return a;
  if (t >= 1.0)
    return b;
  return ToRotator(Slerp(FromRotator(a[0], a[1], a[2]), FromRotator(b[0], b[1], b[2]), t));
}

// Angles that differ by whole turns describe the same orientation; a yaw from
// 170 to -170 crosses 180, it does not spin back through 0.
inline double InterpolateAngle(const double a, const double b, const double t) noexcept {
  return a + WrapDegrees(b - a) * t;
}

inline Quat Multiply(const Quat &a, const Quat &b) noexcept {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

namespace detail {
using LocalIndex = std::map<std::string, const LocalBone *>;

inline Quat Rotation(const LocalBone &bone) noexcept {
  const auto &v = bone.transform;
  return {v[0], v[1], v[2], v[3]};
}

inline Quat Canonical(Quat q) noexcept {
  const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  q = {q.x / length, q.y / length, q.z / length, q.w / length};
  // q and -q are identical. Resolve the half-turn tie lexicographically too.
  const double first = q.w != 0.0 ? q.w : q.z != 0.0 ? q.z : q.y != 0.0 ? q.y : q.x;
  if (first < 0.0)
    q = {-q.x, -q.y, -q.z, -q.w};
  return q;
}

inline void SetRotation(LocalBone &bone, const Quat &q) noexcept {
  bone.transform[0] = q.x;
  bone.transform[1] = q.y;
  bone.transform[2] = q.z;
  bone.transform[3] = q.w;
  bone.transform[7] = bone.transform[11] = 0.0;
}

inline bool Valid(const LocalBone &bone) noexcept {
  if (bone.name.empty())
    return false;
  for (const double value : bone.transform)
    if (!std::isfinite(value))
      return false;
  const auto q = Rotation(bone);
  const double norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return std::abs(norm - 1.0) <= 1e-4;
}

// When a rig is known, foreign names are ignored (including foreign payloads).
// Duplicate known names, nonfinite transforms and nonunit quaternions fail.
inline bool Index(const std::vector<LocalBone> &pose, LocalIndex &out,
                  const LocalIndex *known = nullptr) {
  for (const auto &bone : pose) {
    if (known && known->find(bone.name) == known->end())
      continue;
    if (!Valid(bone) || !out.emplace(bone.name, &bone).second)
      return false;
  }
  return true;
}
}  // namespace detail

// Capture final locals in reference-rig order. Library omissions use unposed
// reference locals; unknown library/offset names are ignored. Invalid known
// names/transforms/offsets return an empty vector, not fabricated identities.
// A nonempty, validated reference rig is required for capture.
inline std::vector<LocalBone> ComposeLocalPose(const std::vector<LocalBone> &reference,
                                              const std::vector<LocalBone> &libraryPose,
                                              const std::vector<BoneKey> &offsets) {
  detail::LocalIndex ref, library;
  if (reference.empty() || !detail::Index(reference, ref) ||
      !detail::Index(libraryPose, library, &ref))
    return {};
  std::map<std::string, const BoneKey *> offsetIndex;
  for (const auto &offset : offsets) {
    if (ref.find(offset.name) == ref.end())
      continue;
    if (!std::isfinite(offset.pitch) || !std::isfinite(offset.yaw) ||
        !std::isfinite(offset.roll) || !offsetIndex.emplace(offset.name, &offset).second)
      return {};
  }
  std::vector<LocalBone> out;
  out.reserve(reference.size());
  for (const auto &base : reference) {
    const auto found = library.find(base.name);
    LocalBone bone = found == library.end() ? base : *found->second;
    auto q = detail::Rotation(bone);
    const auto offset = offsetIndex.find(base.name);
    if (offset != offsetIndex.end()) {
      const auto &e = *offset->second;
      q = Multiply(FromRotator(e.pitch, e.yaw, e.roll), q);
    }
    detail::SetRotation(bone, detail::Canonical(q));
    out.push_back(std::move(bone));
  }
  return out;
}

// Rotation-only compatibility for old exporters: full = offset * reference.
// Translation/scale cannot be represented by BoneKey. Empty means no offsets
// or invalid known data; callers must retain full locals as the authoritative key.
inline std::vector<BoneKey> GenerateOffsetFallback(const std::vector<LocalBone> &full,
                                                  const std::vector<LocalBone> &reference) {
  detail::LocalIndex ref, pose;
  if (reference.empty() || !detail::Index(reference, ref) || !detail::Index(full, pose, &ref))
    return {};
  std::vector<BoneKey> out;
  out.reserve(pose.size());
  for (const auto &base : reference) {
    const auto found = pose.find(base.name);
    if (found == pose.end())
      continue;
    const auto r = detail::Canonical(detail::Rotation(base));
    const auto q = detail::Canonical(Multiply(detail::Rotation(*found->second),
                                             {-r.x, -r.y, -r.z, r.w}));
    const auto e = ToRotator(q);
    if (std::abs(e[0]) > 1e-9 || std::abs(e[1]) > 1e-9 || std::abs(e[2]) > 1e-9)
      out.push_back({base.name, e[0], e[1], e[2]});
  }
  return out;
}

// Full/full interpolation needs no reference for shared names. Missing names
// use reference locals when available; an unmatched foreign bone with no
// reference is skipped, not promoted to an identity transform. Exact endpoints
// are retained verbatim. Invalid known data marks the sample invalid.
inline bool MixLocalPoses(const std::vector<LocalBone> &a, const std::vector<LocalBone> &b,
                          const std::vector<LocalBone> &reference, const double t,
                          std::vector<LocalBone> &out) {
  detail::LocalIndex ref, from, to;
  if (!detail::Index(reference, ref))
    return false;
  const auto *known = reference.empty() ? nullptr : &ref;
  if (t <= 0.0 || t >= 1.0) {
    const auto &endpoint = t <= 0.0 ? a : b;
    detail::LocalIndex checked;
    if (!detail::Index(endpoint, checked, known))
      return false;
    out = endpoint;
    return true;
  }
  if (!detail::Index(a, from, known) || !detail::Index(b, to, known))
    return false;
  detail::LocalIndex names = ref;
  names.insert(from.begin(), from.end());
  names.insert(to.begin(), to.end());
  out.clear();
  out.reserve(names.size());
  for (const auto &entry : names) {
    const auto &name = entry.first;
    const auto fa = from.find(name), fb = to.find(name), fallback = ref.find(name);
    const LocalBone *aa = fa != from.end() ? fa->second
                         : fallback != ref.end() ? fallback->second : nullptr;
    const LocalBone *bb = fb != to.end() ? fb->second
                         : fallback != ref.end() ? fallback->second : nullptr;
    if (!aa || !bb)
      continue;
    LocalBone bone = *aa;
    detail::SetRotation(bone, detail::Canonical(Slerp(
        detail::Canonical(detail::Rotation(*aa)), detail::Canonical(detail::Rotation(*bb)), t)));
    for (const std::size_t start : {std::size_t{4}, std::size_t{8}})
      for (std::size_t axis{}; axis != 3; ++axis) {
        const auto i = start + axis;
        bone.transform[i] = aa->transform[i] + (bb->transform[i] - aa->transform[i]) * t;
      }
    out.push_back(std::move(bone));
  }
  return true;
}

// ---------------------------------------------------------------------------
// The track.

struct Sampled {
  bool pose_valid{true};
  bool has_pose{};
  std::vector<BoneKey> bones;  // the union of both keys' bones, by name
  std::array<double, 3> root_offset{};
  bool has_expression{};
  std::vector<MorphKey> morphs;
  bool has_camera{};
  CameraKey camera;
  bool has_local_pose{};
  std::vector<LocalBone> local_pose;
};

class Track {
public:
  const std::vector<Key> &Keys() const noexcept { return keys_; }
  bool Empty() const noexcept { return keys_.empty(); }
  std::size_t Size() const noexcept { return keys_.size(); }

  std::uint32_t LastFrame() const noexcept { return keys_.empty() ? 0U : keys_.back().frame; }

  // Insert, or replace the key already at that frame. Keys stay sorted by
  // frame. Returns the key's index, or Size() when the track is full.
  std::size_t Set(Key key) {
    key.frame = (std::min)(key.frame, kMaximumFrame);
    const auto it = std::lower_bound(keys_.begin(), keys_.end(), key.frame,
                                     [](const Key &k, std::uint32_t f) { return k.frame < f; });
    if (it != keys_.end() && it->frame == key.frame) {
      *it = std::move(key);
      return static_cast<std::size_t>(it - keys_.begin());
    }
    if (keys_.size() >= kMaximumKeys)
      return keys_.size();
    const auto placed = keys_.insert(it, std::move(key));
    return static_cast<std::size_t>(placed - keys_.begin());
  }

  bool Remove(const std::size_t index) {
    if (index >= keys_.size())
      return false;
    keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
  }

  // Move a key to another frame. A key already there is replaced. Returns
  // the key's new index, or Size() when `index` is out of range.
  std::size_t Move(const std::size_t index, const std::uint32_t frame) {
    if (index >= keys_.size())
      return keys_.size();
    Key key = keys_[index];
    keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(index));
    key.frame = frame;
    return Set(std::move(key));
  }

  bool SetEase(const std::size_t index, const Ease ease) {
    if (index >= keys_.size())
      return false;
    keys_[index].ease = ease;
    return true;
  }

  void Clear() noexcept { keys_.clear(); }

  // Index of the key at exactly `frame`, or Size().
  std::size_t Find(const std::uint32_t frame) const noexcept {
    const auto it = std::lower_bound(keys_.begin(), keys_.end(), frame,
                                     [](const Key &k, std::uint32_t f) { return k.frame < f; });
    return it != keys_.end() && it->frame == frame ? static_cast<std::size_t>(it - keys_.begin())
                                                   : keys_.size();
  }

  // The pose at a (fractional) frame. Each part interpolates between the
  // nearest keys on either side that carry it, eased by the earlier key's
  // ease; before the first or after the last such key it holds that key.
  Sampled Sample(const double frame, const std::vector<LocalBone> &reference = {}) const {
    Sampled out;
    SamplePart(
        frame, [](const Key &k) { return k.has_pose; },
        [&](const Key &a, const Key &b, const double t) {
          out.has_pose = true;
          if (!a.has_local_pose && !b.has_local_pose) {
            // Preserve the old offset-only path exactly, including its union.
            MixBones(a.bones, b.bones, t, out.bones);
          } else {
            out.has_local_pose = true;
            // Do not require a reference for an exact full endpoint even if
            // the next key is legacy; no interpolation is needed there.
            if ((t <= 0.0 && a.has_local_pose) || (t >= 1.0 && b.has_local_pose)) {
              const auto &pose = t <= 0.0 ? a.local_pose : b.local_pose;
              out.pose_valid = MixLocalPoses(pose, pose, reference, 0.0, out.local_pose);
            } else if ((!a.has_local_pose || !b.has_local_pose) && reference.empty()) {
              out.pose_valid = false;
            } else {
              const auto legacyA = a.has_local_pose ? std::vector<LocalBone>{}
                                                    : ComposeLocalPose(reference, {}, a.bones);
              const auto legacyB = b.has_local_pose ? std::vector<LocalBone>{}
                                                    : ComposeLocalPose(reference, {}, b.bones);
              if ((!a.has_local_pose && legacyA.empty()) || (!b.has_local_pose && legacyB.empty()))
                out.pose_valid = false;
              else
                out.pose_valid = MixLocalPoses(a.has_local_pose ? a.local_pose : legacyA,
                                              b.has_local_pose ? b.local_pose : legacyB,
                                              reference, t, out.local_pose);
            }
          }
          for (std::size_t axis{}; axis != 3; ++axis)
            out.root_offset[axis] =
                a.root_offset[axis] + (b.root_offset[axis] - a.root_offset[axis]) * t;
        });
    SamplePart(
        frame, [](const Key &k) { return k.has_expression; },
        [&](const Key &a, const Key &b, const double t) {
          out.has_expression = true;
          MixMorphs(a.morphs, b.morphs, t, out.morphs);
        });
    SamplePart(
        frame, [](const Key &k) { return k.has_camera; },
        [&](const Key &a, const Key &b, const double t) {
          out.has_camera = true;
          out.camera = MixCamera(a.camera, b.camera, t);
        });
    return out;
  }

private:
  template <typename Has, typename Mix>
  void SamplePart(const double frame, Has has, Mix mix) const {
    const Key *before = nullptr;
    const Key *after = nullptr;
    for (const auto &key : keys_) {
      if (!has(key))
        continue;
      if (static_cast<double>(key.frame) <= frame)
        before = &key;
      else {
        after = &key;
        break;
      }
    }
    if (before == nullptr && after == nullptr)
      return;
    if (before == nullptr) {
      mix(*after, *after, 0.0);
      return;
    }
    if (after == nullptr) {
      mix(*before, *before, 0.0);
      return;
    }
    const double span = static_cast<double>(after->frame) - static_cast<double>(before->frame);
    const double raw = span > 0.0 ? (frame - static_cast<double>(before->frame)) / span : 0.0;
    mix(*before, *after, ApplyEase(before->ease, raw));
  }

  // A bone present in only one key is at rest (zero offset) in the other: a
  // key records every bone the user had moved, so a bone missing from a key
  // was at rest there.
  static void MixBones(const std::vector<BoneKey> &a, const std::vector<BoneKey> &b,
                       const double t, std::vector<BoneKey> &out) {
    out.clear();
    out.reserve(a.size() + b.size());
    for (const auto &bone : a) {
      const auto other = std::find_if(b.begin(), b.end(),
                                      [&](const BoneKey &k) { return k.name == bone.name; });
      const std::array<double, 3> from{bone.pitch, bone.yaw, bone.roll};
      const std::array<double, 3> to =
          other != b.end() ? std::array<double, 3>{other->pitch, other->yaw, other->roll}
                           : std::array<double, 3>{0.0, 0.0, 0.0};
      const auto mixed = InterpolateRotator(from, to, t);
      out.push_back({bone.name, mixed[0], mixed[1], mixed[2]});
    }
    for (const auto &bone : b) {
      const bool seen = std::any_of(a.begin(), a.end(),
                                    [&](const BoneKey &k) { return k.name == bone.name; });
      if (seen)
        continue;
      const auto mixed =
          InterpolateRotator({0.0, 0.0, 0.0}, {bone.pitch, bone.yaw, bone.roll}, t);
      out.push_back({bone.name, mixed[0], mixed[1], mixed[2]});
    }
  }

  // A morph present in only one key goes to / comes from 0 in the other: a key
  // records every morph the user drove, so one missing was at rest there.
  static void MixMorphs(const std::vector<MorphKey> &a, const std::vector<MorphKey> &b,
                        const double t, std::vector<MorphKey> &out) {
    out.clear();
    out.reserve(a.size() + b.size());
    const float ft = static_cast<float>(t);
    for (const auto &morph : a) {
      const auto other = std::find_if(b.begin(), b.end(),
                                      [&](const MorphKey &k) { return k.name == morph.name; });
      const float to = other != b.end() ? other->weight : 0.0F;
      out.push_back({morph.name, morph.weight + (to - morph.weight) * ft});
    }
    for (const auto &morph : b) {
      const bool seen = std::any_of(a.begin(), a.end(),
                                    [&](const MorphKey &k) { return k.name == morph.name; });
      if (!seen)
        out.push_back({morph.name, morph.weight * ft});
    }
  }

  static CameraKey MixCamera(const CameraKey &a, const CameraKey &b, const double t) {
    CameraKey out;
    for (std::size_t axis{}; axis != 3; ++axis)
      out.focus[axis] = a.focus[axis] + (b.focus[axis] - a.focus[axis]) * t;
    out.yaw = WrapDegrees(InterpolateAngle(a.yaw, b.yaw, t));
    out.pitch = a.pitch + (b.pitch - a.pitch) * t;
    // Distance is a zoom: equal ratios per frame read as an even push-in,
    // where a straight line in centimetres slows down visibly near the end.
    if (a.distance > 1e-6 && b.distance > 1e-6)
      out.distance = a.distance * std::pow(b.distance / a.distance, t);
    else
      out.distance = a.distance + (b.distance - a.distance) * t;
    // A key with the game's lens (0) keeps the other key's lens, so keying
    // one zoom does not snap the lens back to the game's in between.
    if (a.fov > 0.0F && b.fov > 0.0F)
      out.fov = a.fov + (b.fov - a.fov) * static_cast<float>(t);
    else
      out.fov = a.fov > 0.0F ? a.fov : b.fov;
    return out;
  }

  std::vector<Key> keys_;
};

// Undo/redo of the track itself (adding, replacing, moving, deleting keys),
// for history.hpp's History<State>.
struct TrackState {
  std::vector<Key> keys;

  bool SameAs(const TrackState &other) const noexcept {
    if (keys.size() != other.keys.size())
      return false;
    for (std::size_t i{}; i != keys.size(); ++i) {
      const auto &a = keys[i];
      const auto &b = other.keys[i];
      if (a.frame != b.frame || a.ease != b.ease || a.has_pose != b.has_pose ||
          a.has_expression != b.has_expression || a.has_camera != b.has_camera ||
          a.bones.size() != b.bones.size() || a.morphs.size() != b.morphs.size() ||
          a.root_offset != b.root_offset || a.has_local_pose != b.has_local_pose ||
          a.local_pose.size() != b.local_pose.size())
        return false;
      for (std::size_t j{}; j != a.bones.size(); ++j)
        if (a.bones[j].name != b.bones[j].name || a.bones[j].pitch != b.bones[j].pitch ||
            a.bones[j].yaw != b.bones[j].yaw || a.bones[j].roll != b.bones[j].roll)
          return false;
      for (std::size_t j{}; j != a.local_pose.size(); ++j)
        if (a.local_pose[j].name != b.local_pose[j].name ||
            a.local_pose[j].transform != b.local_pose[j].transform)
          return false;
      for (std::size_t j{}; j != a.morphs.size(); ++j)
        if (a.morphs[j].name != b.morphs[j].name || a.morphs[j].weight != b.morphs[j].weight)
          return false;
      if (a.has_camera && (a.camera.focus != b.camera.focus || a.camera.yaw != b.camera.yaw ||
                           a.camera.pitch != b.camera.pitch ||
                           a.camera.distance != b.camera.distance || a.camera.fov != b.camera.fov))
        return false;
    }
    return true;
  }
};

}  // namespace better_pose::keyframes
