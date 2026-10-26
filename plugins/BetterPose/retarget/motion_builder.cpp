#include "motion_builder.hpp"
#include "../bone_constraints.hpp"
#include "../secondary_rules.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include <windows.h>

namespace better_pose::mmd2bip {

using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------- math
// Exactly the formulas from tools/mmd2bip/mmd2bip.py lines 39-108, written the same way
// on purpose: the reference implementation uses this expansion rather than a conjugate
// product, and matching it removes one source of last-digit drift.

struct Quat {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 1.0;
};

// Quaternion product; mmd2bip's qmul is the same conventional product (verified against
// its own output), so this is a straight transcription.
Quat Multiply(const Quat &a, const Quat &b) {
  return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat Conjugate(const Quat &q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

Quat Normalize(const Quat &q) {
  const double norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (norm == 0.0)
    return Quat{};
  return Quat{q.x / norm, q.y / norm, q.z / norm, q.w / norm};
}

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

Vec3 Rotate(const Quat &q, const Vec3 &v) {
  const double tx = 2.0 * (q.y * v.z - q.z * v.y);
  const double ty = 2.0 * (q.z * v.x - q.x * v.z);
  const double tz = 2.0 * (q.x * v.y - q.y * v.x);
  return Vec3{v.x + q.w * tx + q.y * tz - q.z * ty,
              v.y + q.w * ty + q.z * tx - q.x * tz,
              v.z + q.w * tz + q.x * ty - q.y * tx};
}

double Length(const Vec3 &v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

Vec3 operator+(const Vec3 &a, const Vec3 &b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(const Vec3 &value, const double scale) {
  return Vec3{value.x * scale, value.y * scale, value.z * scale};
}

Vec3 Unit(const Vec3 &v) {
  const double length = Length(v);
  if (length < 1e-12)
    return Vec3{};
  return Vec3{v.x / length, v.y / length, v.z / length};
}

double Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

double AngleBetween(const Vec3 &a, const Vec3 &b) {
  const double na = Length(a);
  const double nb = Length(b);
  if (na < 1e-9 || nb < 1e-9)
    return 0.0;
  const double cosine = (std::max)(-1.0, (std::min)(1.0, Dot(a, b) / (na * nb)));
  return std::acos(cosine) * 180.0 / 3.14159265358979323846;
}

// Minimal rotation taking unit vector u to unit vector v (mmd2bip.py:85-100).
Quat Swing(const Vec3 &u, const Vec3 &v) {
  const double d = Dot(u, v);
  if (d > 1.0 - 1e-12)
    return Quat{};
  if (d < -1.0 + 1e-12) {
    Vec3 axis{u.y, -u.x, 0.0};
    if (axis.x * axis.x + axis.y * axis.y + axis.z * axis.z < 1e-9)
      axis = Vec3{0.0, u.z, -u.y};
    const Vec3 unit = Unit(axis);
    return Quat{unit.x, unit.y, unit.z, 0.0};
  }
  const Vec3 cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                   u.x * v.y - u.y * v.x};
  return Normalize(Quat{cross.x, cross.y, cross.z, 1.0 + d});
}

// MMD (Y up / Z forward) -> Bip001 (X left / Y forward / Z up): +90 degrees about X.
const Quat kAxisChange{0.7071067811865476, 0.0, 0.0, 0.7071067811865476};

// How much of a sleeve's own hang direction (gravity plus the body's acceleration) replaces
// "follow the arm it hangs on"; every other accessory keeps 0.15. Chosen from a measured sweep of
// 0.30 / 0.45 / 0.60 / 1.00 on 浔's rig (D:\xun.skeleton.json, 54 sleeve bones) and 残虹's
// (D:\canhong.skeleton.json, 71 after the 裙 exclusion): see the sleeve share column of the
// conversion report for the hem angle each value produced.
using secondary::kSleeveGravityWeight;

// Strand collision proxies. The converter has no mesh, so the volumes come from the rig's own
// bones: torso, neck/head, shoulder/arm/hand, and thigh/calf capsules. A strand can belong to
// several groups at once (for example long hair crossing the back and an arm).
// The radii sit a couple of centimetres *inside* the visual surface (a torso's half-depth is about
// ten centimetres), so hair lying on the back or a skirt brushing a thigh is not "collided" on
// every frame, and penetration shallower than the slack is left alone so the cloth keeps its drape.
// Measured on 浔's rig before this existed: hair came as close as 0.8 cm to the *spine axis* on the
// worst frames, i.e. straight through the middle of the torso.
using secondary::kTorsoCollisionRadiusCm;
using secondary::kNeckCollisionRadiusCm;
using secondary::kNeckTopCollisionRadiusCm;
using secondary::kHeadCollisionRadiusCm;
using secondary::kShoulderCollisionRadiusCm;
using secondary::kArmCollisionRadiusCm;
using secondary::kHandCollisionRadiusCm;
// The leg volumes take their radius from the rig itself (see `leg_floors`): a guessed 7.0/5.5 cm
// minus a 2.0 cm slack let a skirt bone sit 5.0/3.5 cm from the leg axis, which is 3-6 cm closer to
// the leg than the model's own skirt hangs at rest -- so the correction was holding the cloth
// *inside* the leg and the skirt still clipped. The slack is now only the solver's working room;
// the radius carries the clearance.
using secondary::kCollideTorso;
using secondary::kCollideLegs;
using secondary::kCollideHead;
using secondary::kCollideArms;

// ------------------------------------------------------------------- binary reading
//
// VMD stores names in Shift-JIS. Only the names matter here (they are matched against the
// PMX bone table), and the ones this converter maps are either ASCII or carry multi-byte
// full-width digits -- a byte-wise pass-through keeps those intact without a code page
// table.

bool ReadBytes(const std::uint8_t *bytes, std::size_t size, std::size_t &offset,
               void *out, std::size_t count) {
  if (offset > size || count > size - offset)
    return false;
  std::memcpy(out, bytes + offset, count);
  offset += count;
  return true;
}

bool ReadFixedName(const std::uint8_t *bytes, std::size_t size, std::size_t &offset,
                   std::size_t field_size, std::string &out) {
  if (offset > size || field_size > size - offset)
    return false;
  const auto *begin = reinterpret_cast<const char *>(bytes + offset);
  std::size_t length = 0;
  while (length < field_size && begin[length] != '\0')
    ++length;
  out.assign(begin, length);
  offset += field_size;
  return true;
}

// VMD names are Shift-JIS, the skeleton export and the reference model use UTF-8, and
// the mapping table is written in UTF-8 -- so a raw byte copy would never match a single
// mapped bone ("左腕" is 8D B7 98 5A in Shift-JIS but E5 B7 A6 E8 85 95 in UTF-8). Bone
// names go through here; the morph/IK names that are never matched are read raw.
bool ReadShiftJisName(const std::uint8_t *bytes, std::size_t size, std::size_t &offset,
                      std::size_t field_size, std::string &out) {
  std::string raw;
  if (!ReadFixedName(bytes, size, offset, field_size, raw))
    return false;
  if (raw.empty()) {
    out.clear();
    return true;
  }
  const int wide_length = MultiByteToWideChar(932, 0, raw.data(),
                                              static_cast<int>(raw.size()), nullptr, 0);
  if (wide_length <= 0) {
    out = raw;
    return true;
  }
  std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
  MultiByteToWideChar(932, 0, raw.data(), static_cast<int>(raw.size()), wide.data(),
                      wide_length);
  const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length,
                                              nullptr, 0, nullptr, nullptr);
  if (utf8_length <= 0) {
    out = raw;
    return true;
  }
  out.assign(static_cast<std::size_t>(utf8_length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, out.data(), utf8_length,
                      nullptr, nullptr);
  return true;
}

template <typename T>
bool ReadPod(const std::uint8_t *bytes, std::size_t size, std::size_t &offset, T &out) {
  return ReadBytes(bytes, size, offset, &out, sizeof(T));
}

void QuaternionNormalize(double q[4]) {
  const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (norm == 0.0) {
    q[0] = q[1] = q[2] = 0.0;
    q[3] = 1.0;
    return;
  }
  for (int i = 0; i < 4; ++i)
    q[i] /= norm;
}

void QuaternionSlerp(const double a[4], const double b[4], double t, double out[4]) {
  double to[4] = {b[0], b[1], b[2], b[3]};
  double dot = 0.0;
  for (int i = 0; i < 4; ++i)
    dot += a[i] * to[i];
  if (dot < 0.0) {
    for (int i = 0; i < 4; ++i)
      to[i] = -to[i];
    dot = -dot;
  }
  if (dot > 0.9995) {
    for (int i = 0; i < 4; ++i)
      out[i] = a[i] + (to[i] - a[i]) * t;
    QuaternionNormalize(out);
    return;
  }
  const double theta0 = std::acos((std::max)(-1.0, (std::min)(1.0, dot)));
  const double theta = theta0 * t;
  const double sin_theta0 = std::sin(theta0);
  const double s0 = std::sin(theta0 - theta) / sin_theta0;
  const double s1 = std::sin(theta) / sin_theta0;
  for (int i = 0; i < 4; ++i)
    out[i] = a[i] * s0 + to[i] * s1;
  QuaternionNormalize(out);
}

// ------------------------------------------------------------------- skeleton / model

struct Bone {
  std::uint32_t index = 0;
  std::string name;
  int parent = -1;
  Quat base_local{};      // the captured pose, replaced by refLocal when it exists
  Quat captured_local{};  // always the captured (live) pose, kept for the hand choice
  Vec3 base_translation{};
  bool has_ref_local = false;
  Quat ref_local{};
  Vec3 ref_translation{};
};

struct Skeleton {
  bool ok = false;
  std::string error;
  std::string mesh;  // the export's mesh identity, copied into targetMesh
  std::string basis;
  std::vector<Bone> bones;
  std::unordered_map<std::string, std::size_t> by_name;
};

// The reference MMD bone table: only what the mapping and the direction rules read.
struct ReferenceIkLink {
  std::uint32_t bone = 0;
  bool limited = false;
  Vec3 limit_min;
  Vec3 limit_max;
};

// One IK chain as the model declares it (PMX `ikData`): `target` is the effector bone whose
// head the solver moves, `links` are the bones it may rotate, tip first.
struct ReferenceIk {
  std::uint32_t target = 0;
  int loop = 0;
  double limit_radian = 0.0;
  std::vector<ReferenceIkLink> links;
};

struct ReferenceBone {
  std::string name;
  std::uint32_t index = 0;
  int parent = -1;
  int tail_bone = -1;
  Vec3 position;
  Vec3 tail_offset;
  bool has_ik = false;
  ReferenceIk ik;
};

bool LoadSkeleton(const std::string &document, Skeleton &out) {
  json root;
  try {
    root = json::parse(document);
  } catch (const std::exception &error) {
    out.error = std::string("skeleton is not valid json: ") + error.what();
    return false;
  }
  if (!root.is_object() || !root.contains("bones") || !root["bones"].is_array()) {
    out.error = "skeleton export has no bones array";
    return false;
  }
  if (root.contains("mesh") && root["mesh"].is_string())
    out.mesh = root["mesh"].get<std::string>();
  if (root.contains("basis") && root["basis"].is_string())
    out.basis = root["basis"].get<std::string>();
  for (const auto &item : root["bones"]) {
    Bone bone;
    bone.index = item.value("index", bone.index);
    bone.name = item.value("name", std::string());
    bone.parent = item.value("parent", -1);
    if (item.contains("baseLocal")) {
      const auto &local = item["baseLocal"];
      if (local.contains("rotation") && local["rotation"].is_array() &&
          local["rotation"].size() == 4) {
        bone.base_local = Quat{local["rotation"][0].get<double>(),
                               local["rotation"][1].get<double>(),
                               local["rotation"][2].get<double>(),
                               local["rotation"][3].get<double>()};
      }
      if (local.contains("translation") && local["translation"].is_array() &&
          local["translation"].size() == 3) {
        bone.base_translation = Vec3{local["translation"][0].get<double>(),
                                     local["translation"][1].get<double>(),
                                     local["translation"][2].get<double>()};
      }
    }
    if (item.contains("refLocal")) {
      const auto &local = item["refLocal"];
      bone.has_ref_local = true;
      if (local.contains("rotation") && local["rotation"].is_array() &&
          local["rotation"].size() == 4) {
        bone.ref_local = Quat{local["rotation"][0].get<double>(),
                              local["rotation"][1].get<double>(),
                              local["rotation"][2].get<double>(),
                              local["rotation"][3].get<double>()};
      }
      if (local.contains("translation") && local["translation"].is_array() &&
          local["translation"].size() == 3) {
        bone.ref_translation = Vec3{local["translation"][0].get<double>(),
                                    local["translation"][1].get<double>(),
                                    local["translation"][2].get<double>()};
      }
    }
    // load_skeleton() refuses a skeleton whose parents do not come first: every later
    // pass walks bones in index order and would read an uninitialised parent
    // (mmd2bip.py:361-367).
    if (bone.parent >= 0 && static_cast<std::uint32_t>(bone.parent) >= bone.index) {
      out.error = "skeleton is not topologically ordered at " + bone.name;
      return false;
    }
    out.bones.push_back(bone);
  }
  for (std::size_t i = 0; i < out.bones.size(); ++i) {
    if (!out.bones[i].name.empty())
      out.by_name[out.bones[i].name] = i;
  }
  // The engine reference (bind) pose is the pose the geometry is really skinned in, and
  // replaces the captured pose wholesale when the exporter managed to read it
  // (mmd2bip.py:675-681). The captured pose is kept because the hand chain may want it.
  std::size_t with_reference = 0;
  for (const auto &bone : out.bones)
    with_reference += bone.has_ref_local ? 1U : 0U;
  for (auto &bone : out.bones)
    bone.captured_local = bone.base_local;
  if (!out.bones.empty() && with_reference == out.bones.size()) {
    for (auto &bone : out.bones) {
      bone.base_local = bone.ref_local;
      bone.base_translation = bone.ref_translation;
    }
  }
  out.ok = true;
  return true;
}

bool LoadReference(const std::string &document, std::vector<ReferenceBone> &out,
                   std::string &error) {
  json root;
  try {
    root = json::parse(document);
  } catch (const std::exception &exception) {
    error = std::string("reference pmx is not valid json: ") + exception.what();
    return false;
  }
  if (!root.is_object() || !root.contains("bones") || !root["bones"].is_array()) {
    error = "reference pmx has no bones array";
    return false;
  }
  for (const auto &item : root["bones"]) {
    ReferenceBone bone;
    bone.name = item.value("name", std::string());
    bone.index = item.value("index", bone.index);
    bone.parent = item.value("parent", -1);
    bone.tail_bone = item.value("tailBone", -1);
    if (item.contains("position") && item["position"].is_array() &&
        item["position"].size() == 3) {
      bone.position = Vec3{item["position"][0].get<double>(),
                           item["position"][1].get<double>(),
                           item["position"][2].get<double>()};
    }
    if (item.contains("tailOffset") && item["tailOffset"].is_array() &&
        item["tailOffset"].size() == 3) {
      bone.tail_offset = Vec3{item["tailOffset"][0].get<double>(),
                              item["tailOffset"][1].get<double>(),
                              item["tailOffset"][2].get<double>()};
    }
    // Two real shapes reach this loader: the shipped reduced table writes the chain as
    // `"ik": {...}`, while the full PMX dump the acceptance harness is compared against
    // keeps the PMX flags (`"ik": true`) and the chain under `"ikData"` (pmx_dump.py).
    const json *ik_source = nullptr;
    if (item.contains("ik")) {
      if (item["ik"].is_object()) {
        ik_source = &item["ik"];
      } else if (item["ik"].is_boolean() && item["ik"].get<bool>() &&
                 item.contains("ikData") && item["ikData"].is_object()) {
        ik_source = &item["ikData"];
      }
    }
    if (ik_source != nullptr) {
      const auto &ik = *ik_source;
      bone.has_ik = true;
      bone.ik.target = ik.value("target", 0u);
      bone.ik.loop = ik.value("loop", 0);
      bone.ik.limit_radian = ik.value("limitRadian", 0.0);
      if (ik.contains("links") && ik["links"].is_array()) {
        for (const auto &entry : ik["links"]) {
          ReferenceIkLink link;
          link.bone = entry.value("bone", 0u);
          if (entry.contains("limitMin") && entry["limitMin"].is_array() &&
              entry["limitMin"].size() == 3 && entry.contains("limitMax") &&
              entry["limitMax"].is_array() && entry["limitMax"].size() == 3) {
            link.limited = true;
            link.limit_min = Vec3{entry["limitMin"][0].get<double>(),
                                  entry["limitMin"][1].get<double>(),
                                  entry["limitMin"][2].get<double>()};
            link.limit_max = Vec3{entry["limitMax"][0].get<double>(),
                                  entry["limitMax"][1].get<double>(),
                                  entry["limitMax"][2].get<double>()};
          }
          bone.ik.links.push_back(link);
        }
      }
    }
    out.push_back(bone);
  }
  return true;
}

// VMD exporters are inconsistent about full-width digits, Latin letters and the index-finger
// spelling. The target's finger chains use plain digits while MMD writes 左人指１, the
// StandardRig reference writes ASCII 左足IK while many VMDs write 左足ＩＫ, and some models
// use 人差指 instead of 人指. Comparing the original spellings silently dropped either the
// finger track or the leg-IK controller itself; a missing IK controller then left the feet at
// rest while the body moved.
std::string CanonicalBoneName(const std::string &name) {
  static const char *kFullWidth[] = {"０", "１", "２", "３", "４",
                                     "５", "６", "７", "８", "９"};
  std::string out = name;
  for (int digit = 0; digit < 10; ++digit) {
    const std::string from = kFullWidth[digit];
    const std::string to(1, static_cast<char>('0' + digit));
    std::size_t position = 0;
    while ((position = out.find(from, position)) != std::string::npos) {
      out.replace(position, from.size(), to);
      position += to.size();
    }
  }
  const auto replace_full_width_latin = [&out](const std::uint8_t prefix,
                                               const std::uint8_t first,
                                               const char base) {
    for (std::uint8_t i = 0; i < 26; ++i) {
      std::string from;
      from.push_back(static_cast<char>(0xEF));
      from.push_back(static_cast<char>(prefix));
      from.push_back(static_cast<char>(first + i));
      const std::string to(1, static_cast<char>(base + i));
      std::size_t position = 0;
      while ((position = out.find(from, position)) != std::string::npos) {
        out.replace(position, from.size(), to);
        position += to.size();
      }
    }
  };
  replace_full_width_latin(0xBC, 0xA1, 'A');
  replace_full_width_latin(0xBD, 0x81, 'a');
  const std::string index_finger_alias = "人差指";
  const std::string index_finger = "人指";
  std::size_t alias_position = 0;
  while ((alias_position = out.find(index_finger_alias, alias_position)) !=
         std::string::npos) {
    out.replace(alias_position, index_finger_alias.size(), index_finger);
    alias_position += index_finger.size();
  }
  return out;
}

struct MappingEntry {
  std::string target;
  std::vector<std::string> sources;  // MMD local rotations, composed in this order
  std::string direction;             // MMD bone whose direction this target follows
  bool position = false;             // the source carries this bone's position
  // Sources composed *conjugated*: the parent chain already carries their rotation, so a
  // target that must not inherit it multiplies it back out (the spine under a pelvis that
  // carries the hip rotation). mmd2bip.py's optional 5th table field.
  std::vector<std::string> inverse;
  // The same, composed *before* `sources`. The parent's contribution is left-multiplied, so
  // it only cancels if the conjugated source is left-multiplied too; appending it (the
  // `inverse` field) yields d(上半身)·d(下半身)^-1, which equals d(上半身) only when the two
  // rotations commute -- so a pelvis that turns the hips tilted the whole torso with it.
  // mmd2bip.py's optional 6th table field.
  std::vector<std::string> prefix_inverse;
  // Explicit parent insertions for transform evaluation and diagnostics. Rotation-only
  // parents also appear in `sources`; position-only parents must still be retained here.
  std::vector<ManualParentMapping> parents;
  // Procedural finger controls sampled from their own source tracks and composed after
  // the ordinary retargeted rotation.
  std::vector<ManualFingerControl> finger_controls;
};

// The explicit table, transcribed from mmd2bip.py:115-140.
struct StaticBone {
  const char *target;
  const char *source;
};

const StaticBone kStaticMapping[] = {
    {"Bip001", "センター"},
    {"Bip001-Pelvis", "腰"},
    {"Bip001-Spine", "上半身"},
    {"Bip001-Spine1", "上半身1"},
    {"Bip001-Spine2", "上半身2"},
    {"Bip001-Neck", "首"},
    {"Bip001-Head", "頭"},
    {"Bip001-L-Clavicle", "左肩"},
    {"Bip001-L-UpperArm", "左腕"},
    {"Bip001-L-Forearm", "左ひじ"},
    {"Bip001-L-Hand", "左手首"},
    {"Bip001-R-Clavicle", "右肩"},
    {"Bip001-R-UpperArm", "右腕"},
    {"Bip001-R-Forearm", "右ひじ"},
    {"Bip001-R-Hand", "右手首"},
};

struct LegBone {
  const char *target;
  const char *source;
  const char *direction;
  const char *fallback;
};

const LegBone kLegMapping[] = {
    {"Bip001-L-Thigh", "下半身", "左足", "左足D"},
    {"Bip001-L-Calf", "左ひざ", "左ひざ", "左ひざD"},
    {"Bip001-L-Foot", "左足首", "左足首", "左足首D"},
    {"Bip001-L-Toe0", "左つま先", "左つま先", "左足先EX"},
    {"Bip001-R-Thigh", "下半身", "右足", "右足D"},
    {"Bip001-R-Calf", "右ひざ", "右ひざ", "右ひざD"},
    {"Bip001-R-Foot", "右足首", "右足首", "右足首D"},
    {"Bip001-R-Toe0", "右つま先", "右つま先", "右足先EX"},
};

// MMD finger group -> the target's finger bone family (mmd2bip.py:142-144).
struct FingerGroup {
  const char *japanese;
  const char *english;
};

const FingerGroup kFingers[] = {
    {"親指", "Finger0"}, {"人指", "Finger1"}, {"中指", "Finger2"},
    {"薬指", "Finger3"}, {"小指", "Finger4"},
};

}  // namespace

VmdDocument ParseVmd(const std::vector<std::uint8_t> &bytes) {
  VmdDocument document;
  document.file_size = bytes.size();
  const std::uint8_t *data = bytes.data();
  const std::size_t size = bytes.size();
  std::size_t offset = 0;

  std::string header;
  if (!ReadFixedName(data, size, offset, 30, header)) {
    document.error = "vmd is shorter than its header";
    return document;
  }
  if (header.rfind("Vocaloid Motion Data file", 0) == 0)
    document.version = 1;
  else if (header.find("0002") != std::string::npos)
    document.version = 2;
  else {
    document.error = "not a VMD file (header=" + header + ")";
    return document;
  }
  if (!ReadFixedName(data, size, offset, document.version == 1 ? 10U : 20U,
                     document.model_name)) {
    document.error = "vmd is truncated in the model name";
    return document;
  }

  std::uint32_t bone_key_count = 0;
  if (!ReadPod(data, size, offset, bone_key_count)) {
    document.error = "vmd is truncated before the bone key count";
    return document;
  }

  struct MutableTrack {
    std::string name;
    std::vector<VmdBoneKey> keys;
  };
  std::vector<MutableTrack> tracks;
  std::unordered_map<std::string, std::size_t> track_index;
  std::uint32_t lowest_frame = (std::numeric_limits<std::uint32_t>::max)();
  std::uint32_t highest_frame = 0;

  for (std::uint32_t i = 0; i < bone_key_count; ++i) {
    std::string name;
    std::uint32_t frame = 0;
    float position[3] = {};
    float rotation[4] = {};
    std::uint8_t interpolation[64] = {};
    if (!ReadShiftJisName(data, size, offset, 15, name) ||
        !ReadPod(data, size, offset, frame) ||
        !ReadBytes(data, size, offset, position, sizeof(position)) ||
        !ReadBytes(data, size, offset, rotation, sizeof(rotation)) ||
        !ReadBytes(data, size, offset, interpolation, sizeof(interpolation))) {
      document.error = "vmd is truncated in the bone keyframe table";
      return document;
    }
    const auto found = track_index.find(name);
    std::size_t index = 0;
    if (found == track_index.end()) {
      index = tracks.size();
      track_index[name] = index;
      tracks.push_back(MutableTrack{name, {}});
    } else {
      index = found->second;
    }
    VmdBoneKey key;
    key.frame = frame;
    for (int k = 0; k < 4; ++k)
      key.rotation[k] = rotation[k];
    for (int k = 0; k < 3; ++k)
      key.position[k] = position[k];
    for (int channel = 0; channel < 4; ++channel) {
      VmdCurve curve{
          std::clamp(static_cast<double>(interpolation[channel]) / 127.0, 0.0, 1.0),
          std::clamp(static_cast<double>(interpolation[channel + 4]) / 127.0, 0.0, 1.0),
          std::clamp(static_cast<double>(interpolation[channel + 8]) / 127.0, 0.0, 1.0),
          std::clamp(static_cast<double>(interpolation[channel + 12]) / 127.0, 0.0, 1.0)};
      // Malformed/older exporters occasionally leave the whole block zeroed. Treat that
      // as the linear wire default so existing files keep playing as before.
      if (interpolation[channel] == 0 && interpolation[channel + 4] == 0 &&
          interpolation[channel + 8] == 0 && interpolation[channel + 12] == 0)
        curve = {};
      key.curves[static_cast<std::size_t>(channel)] = curve;
    }
    tracks[index].keys.push_back(key);
    lowest_frame = (std::min)(lowest_frame, frame);
    highest_frame = (std::max)(highest_frame, frame);
  }

  // Morph keyframes: kept per name (decoded to UTF-8, the mapping table's
  // encoding) for the expression playback; the retarget itself ignores them.
  std::uint32_t morph_key_count = 0;
  if (!ReadPod(data, size, offset, morph_key_count)) {
    document.error = "vmd is truncated before the morph key count";
    return document;
  }
  std::map<std::string, std::size_t> morph_index;
  for (std::uint32_t i = 0; i < morph_key_count; ++i) {
    std::string name;
    std::uint32_t frame = 0;
    float weight = 0.0F;
    if (!ReadShiftJisName(data, size, offset, 15, name) ||
        !ReadPod(data, size, offset, frame) || !ReadPod(data, size, offset, weight)) {
      document.error = "vmd is truncated in the morph keyframe table";
      return document;
    }
    if (name.empty() || !std::isfinite(weight))
      continue;
    auto found = morph_index.find(name);
    if (found == morph_index.end()) {
      found = morph_index.emplace(name, document.morphs.size()).first;
      document.morphs.push_back(VmdMorphTrack{name, {}});
    }
    document.morphs[found->second].keys.push_back(VmdMorphKey{frame, weight});
  }
  for (auto &track : document.morphs)
    std::stable_sort(track.keys.begin(), track.keys.end(),
                     [](const VmdMorphKey &a, const VmdMorphKey &b) { return a.frame < b.frame; });

  // Camera keyframes: frame, distance, position, rotation, 24 interpolation bytes,
  // view angle, perspective flag.
  std::uint32_t camera_key_count = 0;
  if (!ReadPod(data, size, offset, camera_key_count)) {
    document.error = "vmd is truncated before the camera key count";
    return document;
  }
  for (std::uint32_t i = 0; i < camera_key_count; ++i) {
    std::uint32_t frame = 0;
    float distance = 0.0F;
    float position[3] = {};
    float rotation[3] = {};
    std::uint8_t interpolation[24] = {};
    std::uint32_t view_angle = 0;
    std::uint8_t perspective = 0;
    if (!ReadPod(data, size, offset, frame) || !ReadPod(data, size, offset, distance) ||
        !ReadBytes(data, size, offset, position, sizeof(position)) ||
        !ReadBytes(data, size, offset, rotation, sizeof(rotation)) ||
        !ReadBytes(data, size, offset, interpolation, sizeof(interpolation)) ||
        !ReadPod(data, size, offset, view_angle) ||
        !ReadPod(data, size, offset, perspective)) {
      document.error = "vmd is truncated in the camera keyframe table";
      return document;
    }
    // Kept for callers that drive the game camera; the motion conversion ignores it.
    VmdCameraKey key;
    key.frame = frame;
    key.distance = static_cast<double>(distance);
    for (int axis = 0; axis < 3; ++axis) {
      key.position[axis] = static_cast<double>(position[axis]);
      key.rotation[axis] = static_cast<double>(rotation[axis]);
    }
    for (int channel = 0; channel < 6; ++channel) {
      const int at = channel * 4;
      VmdCurve curve{
          std::clamp(static_cast<double>(interpolation[at]) / 127.0, 0.0, 1.0),
          std::clamp(static_cast<double>(interpolation[at + 2]) / 127.0, 0.0, 1.0),
          std::clamp(static_cast<double>(interpolation[at + 1]) / 127.0, 0.0, 1.0),
          std::clamp(static_cast<double>(interpolation[at + 3]) / 127.0, 0.0, 1.0)};
      if (interpolation[at] == 0 && interpolation[at + 1] == 0 &&
          interpolation[at + 2] == 0 && interpolation[at + 3] == 0)
        curve = {};
      key.curves[static_cast<std::size_t>(channel)] = curve;
    }
    key.view_angle = view_angle;
    key.perspective = perspective != 0;
    document.camera.push_back(key);
    if (document.camera_frame_range.empty()) {
      document.camera_frame_range = {frame, frame};
    } else {
      document.camera_frame_range[0] = (std::min)(document.camera_frame_range[0], frame);
      document.camera_frame_range[1] = (std::max)(document.camera_frame_range[1], frame);
    }
  }

  // The light, self-shadow and visibility tables are the optional tail of the format:
  // exporters, and camera-only files in particular, may stop right after the camera
  // table. Nothing this converter reads lives there, so an absent or half-written tail
  // ends the parse with what has already been collected instead of failing the whole
  // file; `walk_exact` still records whether the tail was read to the byte.
  auto finish = [&]() {
    for (auto &track : tracks) {
      std::stable_sort(track.keys.begin(), track.keys.end(),
                       [](const VmdBoneKey &left, const VmdBoneKey &right) {
                         return left.frame < right.frame;
                       });
    }
    std::stable_sort(document.camera.begin(), document.camera.end(),
                     [](const VmdCameraKey &left, const VmdCameraKey &right) {
                       return left.frame < right.frame;
                     });
    std::stable_sort(document.visibility.begin(), document.visibility.end(),
                     [](const VmdVisibilityKey &left,
                        const VmdVisibilityKey &right) {
                       return left.frame < right.frame;
                     });
    document.tracks.reserve(tracks.size());
    for (auto &track : tracks)
      document.tracks.push_back(VmdTrack{std::move(track.name), std::move(track.keys)});
    if (!document.tracks.empty())
      document.bone_frame_range = {lowest_frame, highest_frame};
    document.bytes_consumed = offset;
    document.walk_exact = offset == size;
    document.ok = true;
    return document;
  };

  // Light keyframes: frame, colour, position.
  std::uint32_t light_key_count = 0;
  if (!ReadPod(data, size, offset, light_key_count))
    return finish();
  for (std::uint32_t i = 0; i < light_key_count; ++i) {
    std::uint32_t frame = 0;
    float color[3] = {};
    float position[3] = {};
    if (!ReadPod(data, size, offset, frame) ||
        !ReadBytes(data, size, offset, color, sizeof(color)) ||
        !ReadBytes(data, size, offset, position, sizeof(position)))
      return finish();
  }

  // Self-shadow keyframes: frame, mode, distance.
  std::uint32_t shadow_key_count = 0;
  if (!ReadPod(data, size, offset, shadow_key_count))
    return finish();
  for (std::uint32_t i = 0; i < shadow_key_count; ++i) {
    std::uint32_t frame = 0;
    std::uint8_t mode = 0;
    float distance = 0.0F;
    if (!ReadPod(data, size, offset, frame) || !ReadPod(data, size, offset, mode) ||
        !ReadPod(data, size, offset, distance))
      return finish();
  }

  // Bone visibility / IK enable frames. This is *one* section, not two: each frame carries
  // the model's visibility flag and then the IK state of every IK bone at that frame
  // (vmd_dump.py:101-112, which walks all seven sample files to the byte). Reading it as
  // "visibility frames" followed by a separate "IK frames" table walked every file short
  // and reported MISMATCH on files that were perfectly fine. Files older than VMD 1.0 stop
  // before this section.
  if (offset < size) {
    std::uint32_t visible_key_count = 0;
    if (!ReadPod(data, size, offset, visible_key_count))
      return finish();
    for (std::uint32_t i = 0; i < visible_key_count; ++i) {
      std::uint32_t frame = 0;
      std::uint8_t visible = 0;
      std::uint32_t ik_count = 0;
      if (!ReadPod(data, size, offset, frame) ||
          !ReadPod(data, size, offset, visible) ||
          !ReadPod(data, size, offset, ik_count))
        return finish();
      document.visibility.push_back(VmdVisibilityKey{frame, visible != 0});
      for (std::uint32_t j = 0; j < ik_count; ++j) {
        // The record name is Shift-JIS like every other VMD name: reading it as raw bytes
        // never matches the UTF-8 names in the reference table, so the trail parsed but
        // silently disabled nothing (mmd2bip.py decodes it too).
        std::string name;
        std::uint8_t on = 0;
        if (!ReadShiftJisName(data, size, offset, 20, name) ||
            !ReadPod(data, size, offset, on))
          return finish();
        VmdIkState state;
        state.name = std::move(name);
        state.frame = frame;
        state.on = on != 0;
        document.ik_states.push_back(std::move(state));
      }
    }
  }

  return finish();
}

bool AnimatedVmdTrack(const VmdTrack &track, std::uint32_t &first_frame) {
  for (const VmdBoneKey &frame : track.keys) {
    if (std::fabs(frame.rotation[0]) > 1e-5 ||
        std::fabs(frame.rotation[1]) > 1e-5 ||
        std::fabs(frame.rotation[2]) > 1e-5 ||
        std::fabs(frame.rotation[3] - 1.0) > 1e-5 ||
        std::fabs(frame.position[0]) > 1e-5 ||
        std::fabs(frame.position[1]) > 1e-5 ||
        std::fabs(frame.position[2]) > 1e-5) {
      first_frame = frame.frame;
      return true;
    }
  }
  return false;
}

VmdCompatibilityReport::TrackMotionSignature AnalyzeTrackMotion(
    const VmdTrack &track) {
  VmdCompatibilityReport::TrackMotionSignature signature;
  if (track.keys.empty())
    return signature;
  std::array<double, 3> rotation_min{};
  std::array<double, 3> rotation_max{};
  std::array<double, 3> position_min{};
  std::array<double, 3> position_max{};
  std::array<double, 3> previous_rotation{};
  for (std::size_t axis{}; axis != 3; ++axis) {
    rotation_min[axis] = rotation_max[axis] =
        track.keys.front().rotation[axis];
    position_min[axis] = position_max[axis] =
        track.keys.front().position[axis];
    previous_rotation[axis] = track.keys.front().rotation[axis];
  }
  for (const VmdBoneKey &key : track.keys) {
    std::array<double, 3> rotation{
        key.rotation[0], key.rotation[1], key.rotation[2]};
    const double dot =
        rotation[0] * previous_rotation[0] +
        rotation[1] * previous_rotation[1] +
        rotation[2] * previous_rotation[2];
    if (dot < 0.0)
      for (double &value : rotation)
        value = -value;
    for (std::size_t axis{}; axis != 3; ++axis) {
      rotation_min[axis] = (std::min)(rotation_min[axis], rotation[axis]);
      rotation_max[axis] = (std::max)(rotation_max[axis], rotation[axis]);
      position_min[axis] =
          (std::min)(position_min[axis], key.position[axis]);
      position_max[axis] =
          (std::max)(position_max[axis], key.position[axis]);
      previous_rotation[axis] = rotation[axis];
    }
  }
  double total = 0.0;
  for (std::size_t axis{}; axis != 3; ++axis) {
    signature.rotation_span[axis] =
        rotation_max[axis] - rotation_min[axis];
    signature.position_span[axis] =
        position_max[axis] - position_min[axis];
    total += signature.rotation_span[axis];
    if (signature.dominant_axis < 0 ||
        signature.rotation_span[axis] >
            signature.rotation_span[
                static_cast<std::size_t>(signature.dominant_axis)])
      signature.dominant_axis = static_cast<int>(axis);
  }
  if (signature.dominant_axis >= 0 && total > 1e-9)
    signature.axis_dominance =
        signature.rotation_span[
            static_cast<std::size_t>(signature.dominant_axis)] /
        total;
  if (signature.dominant_axis < 0 ||
      signature.rotation_span[
          static_cast<std::size_t>(signature.dominant_axis)] < 1e-5)
    return signature;
  const std::size_t dominant =
      static_cast<std::size_t>(signature.dominant_axis);
  int previous_sign = 0;
  std::array<double, 3> last_rotation{
      track.keys.front().rotation[0], track.keys.front().rotation[1],
      track.keys.front().rotation[2]};
  double previous_value = track.keys.front().rotation[dominant];
  const double threshold =
      (std::max)(1e-4, signature.rotation_span[dominant] * 0.02);
  for (const VmdBoneKey &key : track.keys) {
    std::array<double, 3> current{
        key.rotation[0], key.rotation[1], key.rotation[2]};
    if (current[0] * last_rotation[0] +
            current[1] * last_rotation[1] +
            current[2] * last_rotation[2] <
        0.0)
      for (double &component : current)
        component = -component;
    const double value = current[dominant];
    const double delta = value - previous_value;
    if (std::fabs(delta) < threshold) {
      last_rotation = current;
      continue;
    }
    const int sign = delta > 0.0 ? 1 : -1;
    if (previous_sign != 0 && sign != previous_sign)
      ++signature.direction_changes;
    previous_sign = sign;
    previous_value = value;
    last_rotation = current;
  }
  return signature;
}

VmdCompatibilityReport::UnmappedTrack DescribeVmdTrack(
    const VmdTrack &track) {
  VmdCompatibilityReport::UnmappedTrack item;
  item.name = track.name;
  item.key_count = static_cast<std::uint32_t>(track.keys.size());
  item.animated = AnimatedVmdTrack(track, item.first_nonzero_frame);
  item.signature = AnalyzeTrackMotion(track);
  const auto contains_any = [](const std::string &name,
                               std::initializer_list<const char *> needles) {
    for (const char *needle : needles)
      if (name.find(needle) != std::string::npos)
        return true;
    return false;
  };
  std::string lower = track.name;
  for (char &c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower.rfind("bn_", 0) == 0 ||
      contains_any(lower, {"hair", "hari", "kami", "skirt", "cloth",
                           "ribbon", "piao", "qun", "tail", "belt", "ear"}) ||
      contains_any(track.name, {"髪", "かみ", "裙", "衣", "袖", "飘", "髮"}))
    item.kind = VmdCompatibilityReport::TrackKind::Accessory;
  else if (contains_any(lower, {"dummy", "shadow", "ik"}) ||
           contains_any(track.name, {"補助", "捩", "調整", "キャンセル",
                                    "親", "錘", "回転"}))
    item.kind = VmdCompatibilityReport::TrackKind::Control;
  else if (contains_any(track.name, {"足", "ひざ", "足首", "つま先",
                                    "首", "頭", "上半身", "下半身",
                                    "肩", "腕", "ひじ", "手首", "指",
                                    "腰", "目", "あご"}))
    item.kind = VmdCompatibilityReport::TrackKind::Humanoid;
  return item;
}

void SortUnmappedTracks(
    std::vector<VmdCompatibilityReport::UnmappedTrack> &tracks) {
  std::stable_sort(
      tracks.begin(), tracks.end(), [](const auto &left, const auto &right) {
        const auto rank = [](VmdCompatibilityReport::TrackKind kind) {
          switch (kind) {
            case VmdCompatibilityReport::TrackKind::Humanoid:
              return 0;
            case VmdCompatibilityReport::TrackKind::Control:
              return 1;
            case VmdCompatibilityReport::TrackKind::Accessory:
              return 3;
            case VmdCompatibilityReport::TrackKind::Other:
              return 2;
          }
          return 4;
        };
        if (rank(left.kind) != rank(right.kind))
          return rank(left.kind) < rank(right.kind);
        if (left.animated != right.animated)
          return left.animated;
        if (left.key_count != right.key_count)
          return left.key_count > right.key_count;
        return left.name < right.name;
      });
}

VmdCompatibilityReport AnalyzeParsedVmdCompatibility(
    const VmdDocument &vmd) {
  VmdCompatibilityReport report;
  std::unordered_map<std::string, const VmdTrack *> tracks;
  for (const auto &track : vmd.tracks)
    tracks[CanonicalBoneName(track.name)] = &track;
  std::unordered_map<std::string, bool> mapped_names;
  for (const StaticBone &bone : kStaticMapping)
    mapped_names[CanonicalBoneName(bone.source)] = true;
  for (const LegBone &bone : kLegMapping) {
    mapped_names[CanonicalBoneName(bone.source)] = true;
    mapped_names[CanonicalBoneName(bone.direction)] = true;
    mapped_names[CanonicalBoneName(bone.fallback)] = true;
  }
  for (const char *name :
       {"全ての親", "センター", "グルーブ", "腰", "下半身",
        "上半身1", "上半身3", "左肩P", "右肩P", "左肩C", "右肩C",
        "左腕捩", "右腕捩", "左手捩", "右手捩",
        "左足IK親", "右足IK親", "左足IK", "右足IK",
        "左つま先IK", "右つま先IK"})
    mapped_names[CanonicalBoneName(name)] = true;
  for (const char *side : {"左", "右"}) {
    for (const char *finger :
         {"親指", "人指", "中指", "薬指", "小指"}) {
      std::size_t count = 0;
      for (const char *digit : {"０", "１", "２", "３", "４"}) {
        const std::string name =
            std::string(side) + finger + digit;
        if (tracks.count(CanonicalBoneName(name)) != 0 && count < 4) {
          mapped_names[CanonicalBoneName(name)] = true;
          ++count;
        }
      }
      const std::string prefix =
          CanonicalBoneName(std::string(side) + finger);
      for (const auto &[track_name, track] : tracks) {
        static_cast<void>(track);
        const std::string suffix = "先";
        if (track_name.size() > prefix.size() &&
            track_name.compare(0, prefix.size(), prefix) == 0 &&
            track_name.size() >= suffix.size() &&
            track_name.compare(track_name.size() - suffix.size(),
                               suffix.size(), suffix) == 0) {
          mapped_names[track_name] = true;
        }
      }
    }
  }
  const auto has_track = [&tracks](const std::string &name) {
    return tracks.count(CanonicalBoneName(name)) != 0;
  };
  const auto has_animated_track = [&tracks](const std::string &name) {
    const auto found = tracks.find(CanonicalBoneName(name));
    if (found == tracks.end())
      return false;
    for (const VmdBoneKey &key : found->second->keys) {
      if (std::fabs(key.rotation[0]) > 1e-5 ||
          std::fabs(key.rotation[1]) > 1e-5 ||
          std::fabs(key.rotation[2]) > 1e-5 ||
          std::fabs(key.rotation[3] - 1.0) > 1e-5 ||
          std::fabs(key.position[0]) > 1e-5 ||
          std::fabs(key.position[1]) > 1e-5 ||
          std::fabs(key.position[2]) > 1e-5)
        return true;
    }
    return false;
  };
  const auto any_side = [&has_track](const char *left, const char *right) {
    return has_track(left) || has_track(right);
  };
  const auto any_animated_side =
      [&has_animated_track](const char *left, const char *right) {
        return has_animated_track(left) || has_animated_track(right);
      };

  report.track_count = static_cast<std::uint32_t>(vmd.tracks.size());
  report.visibility_keys =
      static_cast<std::uint32_t>(vmd.visibility.size());
  for (const auto &track : vmd.tracks)
    report.bone_key_count += static_cast<std::uint32_t>(track.keys.size());
  if (!vmd.bone_frame_range.empty())
    report.frame_count =
        vmd.bone_frame_range[1] - vmd.bone_frame_range[0] + 1U;
  const auto leg_fk = [&has_track](const char *side) {
    const std::string prefix(side);
    const bool primary =
        has_track(prefix + "足") && has_track(prefix + "ひざ") &&
        has_track(prefix + "足首");
    const bool deform =
        has_track(prefix + "足D") && has_track(prefix + "ひざD") &&
        has_track(prefix + "足首D");
    return primary || deform;
  };
  report.left_leg_fk = leg_fk("左");
  report.right_leg_fk = leg_fk("右");
  report.complete_leg_fk = report.left_leg_fk && report.right_leg_fk;
  report.foot_ik = any_side("左足IK", "右足IK");
  report.toe_ik = any_side("左つま先IK", "右つま先IK");
  report.leg_d =
      any_side("左足D", "右足D") || any_side("左足先EX", "右足先EX");
  report.waist_cancel =
      has_track("腰キャンセル左") || has_track("腰キャンセル右") ||
      has_track("左腰キャンセル") || has_track("右腰キャンセル");
  report.shoulder_cancel =
      any_side("左肩P", "右肩P") && any_side("左肩C", "右肩C");
  report.animated_shoulder =
      any_animated_side("左肩P", "右肩P") ||
      any_animated_side("左肩C", "右肩C");
  report.twist_tracks =
      any_side("左腕捩", "右腕捩") || any_side("左手捩", "右手捩");
  report.animated_twist =
      any_animated_side("左腕捩", "右腕捩") ||
      any_animated_side("左手捩", "右手捩") ||
      any_animated_side("左腕捩+", "右腕捩+") ||
      any_animated_side("左手捩+", "右手捩+");
  report.upper_body_1 = has_track("上半身1");
  report.upper_body_2 = has_track("上半身2");
  report.upper_body_3 = has_track("上半身3");

  for (const VmdTrack &track : vmd.tracks) {
    const std::string key = CanonicalBoneName(track.name);
    if (mapped_names.count(key) != 0)
      continue;
    auto item = DescribeVmdTrack(track);
    switch (item.kind) {
      case VmdCompatibilityReport::TrackKind::Humanoid:
        ++report.unmapped_humanoid;
        break;
      case VmdCompatibilityReport::TrackKind::Control:
        ++report.unmapped_control;
        break;
      case VmdCompatibilityReport::TrackKind::Accessory:
        ++report.unmapped_accessory;
        break;
      case VmdCompatibilityReport::TrackKind::Other:
        ++report.unmapped_other;
        break;
    }
    report.unmapped_tracks.push_back(std::move(item));
  }
  SortUnmappedTracks(report.unmapped_tracks);

  const auto add_reason = [&report](const char *reason) {
    if (std::find(report.reasons.begin(), report.reasons.end(), reason) ==
        report.reasons.end())
      report.reasons.emplace_back(reason);
  };
  int scores[3] = {};
  if (report.upper_body_3) {
    scores[2] += 6;
    add_reason("three-section spine (上半身3)");
  } else if (report.upper_body_2) {
    scores[1] += 1;
    scores[0] += 1;
    add_reason("two-section spine (上半身2)");
  } else {
    scores[0] += 3;
    add_reason("single-section spine");
  }
  if (report.waist_cancel) {
    scores[1] += 5;
    add_reason("waist-cancel controls");
  }
  if (report.leg_d) {
    scores[1] += 4;
    add_reason("deform/D-EX leg chain");
  }
  if (report.shoulder_cancel) {
    scores[1] += 2;
    scores[2] += 2;
    add_reason("shoulder P/C controls");
  }
  if (report.upper_body_1)
    scores[0] += 1;
  if (report.animated_shoulder)
    add_reason("animated shoulder controls");
  if (report.animated_twist)
    add_reason("animated arm/wrist twist");
  if (report.foot_ik && !report.complete_leg_fk)
    add_reason("IK without complete FK");
  if (report.complete_leg_fk)
    add_reason("complete leg FK");

  const int best = *std::max_element(std::begin(scores), std::end(scores));
  report.recommended_reference =
      static_cast<int>(std::distance(std::begin(scores),
                                     std::max_element(std::begin(scores),
                                                      std::end(scores))));
  int second = 0;
  for (int score : scores)
    if (score != best)
      second = (std::max)(second, score);
  report.confidence = best - second;
  if (best < 4)
    add_reason("mixed or unspecific rig signature");
  report.ok = true;
  return report;
}

double SampleVmdCurve(const VmdCurve &curve, const double x) {
  const double input = std::clamp(x, 0.0, 1.0);
  if (input <= 0.0)
    return 0.0;
  if (input >= 1.0)
    return 1.0;
  // Identity handles need no inversion and cover the vast majority of dense motions.
  if (std::abs(curve.x1 - curve.y1) <= 1e-12 &&
      std::abs(curve.x2 - curve.y2) <= 1e-12)
    return input;

  const auto cubic = [](const double t, const double a, const double b) {
    const double inverse = 1.0 - t;
    return 3.0 * inverse * inverse * t * a + 3.0 * inverse * t * t * b +
           t * t * t;
  };
  double low = 0.0;
  double high = 1.0;
  double t = input;
  for (int iteration = 0; iteration < 22; ++iteration) {
    const double solved = cubic(t, curve.x1, curve.x2);
    if (std::abs(solved - input) <= 1e-7)
      break;
    if (solved < input)
      low = t;
    else
      high = t;
    t = (low + high) * 0.5;
  }
  return std::clamp(cubic(t, curve.y1, curve.y2), 0.0, 1.0);
}

bool SampleTrack(const VmdTrack &track, double frame, double out_rotation[4],
                 double out_position[3]) {
  if (track.keys.empty())
    return false;
  // bisect_right(frames, frame) - 1: the last key at or before the requested frame.
  std::size_t index = 0;
  {
    std::size_t low = 0;
    std::size_t high = track.keys.size();
    while (low < high) {
      const std::size_t middle = low + (high - low) / 2;
      if (static_cast<double>(track.keys[middle].frame) <= frame)
        low = middle + 1;
      else
        high = middle;
    }
    index = low == 0 ? 0 : low - 1;
  }
  if (index >= track.keys.size() - 1) {
    const VmdBoneKey &key = track.keys[index];
    for (int i = 0; i < 4; ++i)
      out_rotation[i] = key.rotation[i];
    for (int i = 0; i < 3; ++i)
      out_position[i] = key.position[i];
    return true;
  }
  const VmdBoneKey &first = track.keys[index];
  const VmdBoneKey &second = track.keys[index + 1];
  if (second.frame == first.frame) {
    for (int i = 0; i < 4; ++i)
      out_rotation[i] = first.rotation[i];
    for (int i = 0; i < 3; ++i)
      out_position[i] = first.position[i];
    return true;
  }
  const double blend =
      (frame - static_cast<double>(first.frame)) /
      (static_cast<double>(second.frame) - static_cast<double>(first.frame));
  double a[4] = {first.rotation[0], first.rotation[1], first.rotation[2],
                 first.rotation[3]};
  double b[4] = {second.rotation[0], second.rotation[1], second.rotation[2],
                 second.rotation[3]};
  QuaternionNormalize(a);
  QuaternionNormalize(b);
  QuaternionSlerp(a, b, SampleVmdCurve(second.curves[3], blend), out_rotation);
  for (int i = 0; i < 3; ++i)
    out_position[i] = first.position[i] +
                      (second.position[i] - first.position[i]) *
                          SampleVmdCurve(second.curves[static_cast<std::size_t>(i)], blend);
  return true;
}

VmdCameraPose SampleCameraTrack(const std::vector<VmdCameraKey> &keys, const double frame) {
  VmdCameraPose result;
  if (keys.empty())
    return result;
  const auto copy = [&result](const VmdCameraKey &key) {
    for (int axis = 0; axis < 3; ++axis) {
      result.position[axis] = key.position[axis];
      result.rotation[axis] = key.rotation[axis];
    }
    result.distance = key.distance;
    result.view_angle = static_cast<double>(key.view_angle);
    result.perspective = key.perspective;
    result.valid = true;
  };
  if (frame <= static_cast<double>(keys.front().frame)) {
    copy(keys.front());
    return result;
  }
  if (frame >= static_cast<double>(keys.back().frame)) {
    copy(keys.back());
    return result;
  }
  std::size_t low = 0;
  std::size_t high = keys.size() - 1;
  while (high - low > 1) {
    const std::size_t middle = (low + high) / 2;
    if (static_cast<double>(keys[middle].frame) <= frame)
      low = middle;
    else
      high = middle;
  }
  const VmdCameraKey &first = keys[low];
  const VmdCameraKey &second = keys[high];
  const double span = static_cast<double>(second.frame) - static_cast<double>(first.frame);
  const double blend = span > 1e-9
                           ? (frame - static_cast<double>(first.frame)) / span
                           : 0.0;
  for (int axis = 0; axis < 3; ++axis) {
    result.position[axis] =
        first.position[axis] +
        (second.position[axis] - first.position[axis]) *
            SampleVmdCurve(second.curves[static_cast<std::size_t>(axis)], blend);
    result.rotation[axis] =
        first.rotation[axis] +
        (second.rotation[axis] - first.rotation[axis]) *
            SampleVmdCurve(second.curves[3], blend);
  }
  result.distance =
      first.distance +
      (second.distance - first.distance) * SampleVmdCurve(second.curves[4], blend);
  result.view_angle =
      static_cast<double>(first.view_angle) +
      (static_cast<double>(second.view_angle) - static_cast<double>(first.view_angle)) *
          SampleVmdCurve(second.curves[5], blend);
  result.perspective = first.perspective;
  result.valid = true;
  return result;
}

namespace {

double Round6(double value) { return std::round(value * 1e6) / 1e6; }

json ToArray4(const Quat &q) {
  return json::array({Round6(q.x), Round6(q.y), Round6(q.z), Round6(q.w)});
}

// ------------------------------------------------------------------ rest directions
//
// A mapped bone's rest rotation is not the export's own rotation: it is rebuilt so the
// bone points where the MMD source points. These helpers answer "which way does this bone
// point" for the target skeleton and for the MMD model (mmd2bip.py:379-541).

// Direction of a bone's own child offset, in the bone's local frame. Only Bip001*
// children count as anatomical (a garment bone hangs off the spine with a *longer* offset
// and used to define the whole torso), and a bone whose only children are accessories
// continues the segment that arrives at it instead -- taking a 14.8 cm hair strand as the
// head's direction swung the head 37 degrees (mmd2bip.py:379-425).
Vec3 BoneDirectionLocal(const Skeleton &skeleton, std::size_t index) {
  const Bone &bone = skeleton.bones[index];
  std::vector<std::size_t> children;
  for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
    if (skeleton.bones[i].parent == static_cast<int>(bone.index))
      children.push_back(i);
  }
  const auto leaf_direction = [&]() -> Vec3 {
    if (Length(bone.base_translation) < 1e-3)
      return Vec3{};
    const Vec3 incoming = Unit(bone.base_translation);
    return Rotate(Conjugate(Normalize(bone.base_local)), incoming);
  };
  if (children.empty())
    return leaf_direction();
  std::vector<std::size_t> continuation;
  for (std::size_t child : children) {
    if (skeleton.bones[child].name.rfind("Bip001", 0) == 0)
      continuation.push_back(child);
  }
  if (continuation.empty()) {
    for (std::size_t child : children) {
      if (skeleton.bones[child].name.rfind("Bn_", 0) != 0)
        continuation.push_back(child);
    }
  }
  Vec3 best;
  double best_length = 0.0;
  for (std::size_t child : continuation) {
    const Vec3 &translation = skeleton.bones[child].base_translation;
    const double length = Length(translation);
    if (length > best_length) {
      best = translation;
      best_length = length;
    }
  }
  if (best_length < 1e-3)
    return leaf_direction();
  return Unit(best);
}

// Direction of the child that continues *this* chain: the MMD model says which child
// continues it and the mapping says which target bone that is, so ask that first and only
// fall back to the offset heuristic (mmd2bip.py:438-452).
Vec3 ChainDirection(const Skeleton &skeleton, const std::string &target_name,
                    const std::string &mmd_name,
                    const std::unordered_map<std::string, std::string> &first_child,
                    const std::unordered_map<std::string, std::string> &target_of_source) {
  const auto child = first_child.find(mmd_name);
  if (child != first_child.end()) {
    const auto mapped = target_of_source.find(child->second);
    if (mapped != target_of_source.end() &&
        skeleton.by_name.count(mapped->second) != 0) {
      const Bone &child_bone = skeleton.bones[skeleton.by_name.at(mapped->second)];
      const Bone &bone = skeleton.bones[skeleton.by_name.at(target_name)];
      if (child_bone.parent == static_cast<int>(bone.index))
        return Unit(child_bone.base_translation);
    }
  }
  return BoneDirectionLocal(skeleton, skeleton.by_name.at(target_name));
}

// MMD name -> the child that continues the chain (first child in PMX index order).
std::unordered_map<std::string, std::string> ReferenceFirstChildren(
    const std::vector<ReferenceBone> &reference) {
  std::unordered_map<std::string, std::string> out;
  for (const auto &bone : reference) {
    if (bone.parent < 0 || static_cast<std::size_t>(bone.parent) >= reference.size())
      continue;
    const std::string &parent = reference[static_cast<std::size_t>(bone.parent)].name;
    if (!parent.empty() && out.count(parent) == 0)
      out[parent] = bone.name;
  }
  return out;
}

// MMD name -> direction towards the child that is itself mapped (mmd2bip.py:468-494).
std::unordered_map<std::string, Vec3> ReferenceMappedChildDirections(
    const std::vector<ReferenceBone> &reference,
    const std::unordered_map<std::string, bool> &mapped_names) {
  std::unordered_map<std::string, Vec3> out;
  for (const auto &bone : reference) {
    for (const auto &child : reference) {
      if (child.parent != static_cast<int>(bone.index))
        continue;
      if (mapped_names.count(child.name) == 0)
        continue;
      const Vec3 step{child.position.x - bone.position.x,
                      child.position.y - bone.position.y,
                      child.position.z - bone.position.z};
      if (Dot(step, step) > 1e-12)
        out[bone.name] = Unit(step);
      break;
    }
  }
  return out;
}

// MMD name -> direction, from the PMX tail when it has one, else the child that continues
// the chain, else the segment arriving at a leaf (mmd2bip.py:497-541).
std::unordered_map<std::string, Vec3> ReferenceDirections(
    const std::vector<ReferenceBone> &reference,
    const std::unordered_map<std::string, std::vector<std::string>> &children) {
  std::unordered_map<std::string, Vec3> out;
  for (const auto &bone : reference) {
    if (bone.name.empty())
      continue;
    const Vec3 &origin = bone.position;
    bool have_tip = false;
    Vec3 tip;
    if (bone.tail_bone >= 0 &&
        static_cast<std::size_t>(bone.tail_bone) < reference.size()) {
      tip = reference[static_cast<std::size_t>(bone.tail_bone)].position;
      have_tip = true;
    } else if (Dot(bone.tail_offset, bone.tail_offset) > 1e-9) {
      tip = Vec3{origin.x + bone.tail_offset.x, origin.y + bone.tail_offset.y,
                 origin.z + bone.tail_offset.z};
      have_tip = true;
    }
    if (!have_tip) {
      const auto found = children.find(bone.name);
      if (found != children.end()) {
        for (const auto &child : found->second) {
          const auto child_bone = std::find_if(
              reference.begin(), reference.end(),
              [&child](const ReferenceBone &item) { return item.name == child; });
          if (child_bone == reference.end())
            continue;
          const Vec3 step = Unit(Vec3{child_bone->position.x - origin.x,
                                      child_bone->position.y - origin.y,
                                      child_bone->position.z - origin.z});
          if (Dot(step, step) > 0.5) {
            tip = child_bone->position;
            have_tip = true;
            break;
          }
        }
      }
    }
    if (!have_tip && bone.parent >= 0 &&
        static_cast<std::size_t>(bone.parent) < reference.size()) {
      const Vec3 &parent = reference[static_cast<std::size_t>(bone.parent)].position;
      const Vec3 step =
          Unit(Vec3{origin.x - parent.x, origin.y - parent.y, origin.z - parent.z});
      if (Dot(step, step) > 0.5) {
        out[bone.name] = step;
        continue;
      }
    }
    if (have_tip) {
      const Vec3 direction =
          Unit(Vec3{tip.x - origin.x, tip.y - origin.y, tip.z - origin.z});
      if (Dot(direction, direction) > 0.5)
        out[bone.name] = direction;
    }
  }
  return out;
}

// Cumulative length fractions of a bone chain (mmd2bip.py:157-166).
std::vector<double> CumulativeFractions(const std::vector<double> &lengths) {
  double total = 0.0;
  for (double value : lengths)
    total += value;
  std::vector<double> out;
  if (total <= 1e-9)
    return std::vector<double>(lengths.size(), 0.0);
  double run = 0.0;
  for (double value : lengths) {
    run += value;
    out.push_back(run / total);
  }
  return out;
}

// ------------------------------------------------------------------------ MMD IK
//
// MMD drives its own skeleton *before* anything is mapped: a motion may carry nothing but
// IK-bone positions for the legs (`主角.vmd` leaves every leg bone at identity and moves
// 左足ＩＫ at thirty frames), and those positions only become bone rotations through the
// model's IK chains. Without the solver the legs stay in the rest pose and never follow.
//
// This is a literal port of the reference's `MmdIkPose` (mmd2bip.py). Bone transform, in
// mmd_tools' convention -- the animation translation is applied in the bone's own rest
// frame -- is
//     world = parent_world * T(rest_offset) * T(vmd_position) * R(vmd_rotation)
// with `rest_offset` = position - parent.position in MMD (Y-up) coordinates.

Quat FromAxisAngle(const Vec3 &axis, double angle) {
  const double half = angle * 0.5;
  const double sine = std::sin(half);
  return Quat{axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half)};
}

// Euler angles of R = Rx * Ry * Rz. The PMX limits are a local XYZ triple, and the knee's
// `[-pi, 0, 0] .. [0, 0, 0]` is exactly one degree of freedom -- which is what stops a leg
// IK from bending the knee forwards (mmd2bip.py quat_euler_xyz).
Vec3 EulerXyz(const Quat &q) {
  const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  const double m02 = 2.0 * (xz + wy);
  const double m12 = 2.0 * (yz - wx);
  const double m22 = 1.0 - 2.0 * (xx + yy);
  const double m10 = 2.0 * (xy + wz);
  const double m11 = 1.0 - 2.0 * (xx + zz);
  const double m01 = 2.0 * (xy - wz);
  const double m00 = 1.0 - 2.0 * (yy + zz);
  const double sy = (std::max)(-1.0, (std::min)(1.0, m02));
  if (std::fabs(sy) > 0.999999)
    return Vec3{std::atan2(m10, m11), std::asin(sy), 0.0};
  return Vec3{std::atan2(-m12, m22), std::asin(sy), std::atan2(-m01, m00)};
}

Quat FromEulerXyz(const Vec3 &euler) {
  return Multiply(Multiply(FromAxisAngle(Vec3{1.0, 0.0, 0.0}, euler.x),
                           FromAxisAngle(Vec3{0.0, 1.0, 0.0}, euler.y)),
                  FromAxisAngle(Vec3{0.0, 0.0, 1.0}, euler.z));
}

Quat ClampToLimits(const Quat &q, const ReferenceIkLink &link) {
  const Vec3 euler = EulerXyz(q);
  const Vec3 low = link.limit_min;
  const Vec3 high = link.limit_max;
  return FromEulerXyz(Vec3{(std::max)(low.x, (std::min)(high.x, euler.x)),
                           (std::max)(low.y, (std::min)(high.y, euler.y)),
                           (std::max)(low.z, (std::min)(high.z, euler.z))});
}

double QuatAngle(const Quat &a, const Quat &b) {
  const double na = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
  const double nb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
  if (na < 1e-12 || nb < 1e-12)
    return 0.0;
  const double dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) / (na * nb);
  return 2.0 * std::acos((std::max)(-1.0, (std::min)(1.0, dot))) *
         180.0 / 3.14159265358979323846;
}

struct IkChain {
  std::uint32_t bone = 0;
  std::uint32_t target = 0;
  int loop = 0;
  double limit = 0.0;
  std::vector<std::uint32_t> links;
  std::vector<const ReferenceIkLink *> limits;
};

struct IkReport {
  std::string name;
  double before = 0.0;
  double after = 0.0;
  double applied = 0.0;  // degrees, the solver's own rotation of the chain
  bool fk_available = false;
  bool fallback_fk = false;
};

struct IkPose {
  std::size_t count = 0;
  std::vector<std::string> name;
  std::vector<int> parent;
  std::vector<Vec3> rest;
  std::vector<std::vector<std::uint32_t>> subtrees;
  std::vector<Quat> rot;
  std::vector<Quat> wrot;
  std::vector<Vec3> pos;
  std::vector<Vec3> wpos;
  std::vector<IkChain> chains;

  void Build(const std::vector<ReferenceBone> &reference) {
    count = reference.size();
    name.resize(count);
    parent.resize(count);
    rest.resize(count);
    rot.assign(count, Quat{});
    wrot.assign(count, Quat{});
    pos.assign(count, Vec3{});
    wpos.assign(count, Vec3{});
    for (std::size_t i = 0; i < count; ++i) {
      name[i] = reference[i].name;
      parent[i] = reference[i].parent;
      rest[i] = reference[i].position;
    }
    std::vector<std::vector<std::uint32_t>> children(count);
    for (std::size_t i = 0; i < count; ++i) {
      if (parent[i] >= 0)
        children[static_cast<std::size_t>(parent[i])].push_back(
            static_cast<std::uint32_t>(i));
    }
    subtrees.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
      std::vector<std::uint32_t> stack{static_cast<std::uint32_t>(i)};
      while (!stack.empty()) {
        const std::uint32_t current = stack.back();
        stack.pop_back();
        subtrees[i].push_back(current);
        for (std::uint32_t child : children[current])
          stack.push_back(child);
      }
      std::sort(subtrees[i].begin(), subtrees[i].end());  // parents before children
    }
    for (const ReferenceBone &bone : reference) {
      if (!bone.has_ik)
        continue;
      IkChain chain;
      chain.bone = bone.index;
      chain.target = bone.ik.target;
      chain.loop = bone.ik.loop;
      chain.limit = bone.ik.limit_radian;
      for (const ReferenceIkLink &link : bone.ik.links) {
        chain.links.push_back(link.bone);
        chain.limits.push_back(link.limited ? &link : nullptr);
      }
      chains.push_back(chain);
    }
  }

  void Refresh(const std::vector<std::uint32_t> &indices) {
    for (std::uint32_t index : indices) {
      const int p = parent[index];
      Quat prot{};
      Vec3 ppos{};
      Vec3 base = rest[index];
      if (p >= 0) {
        prot = wrot[static_cast<std::size_t>(p)];
        ppos = wpos[static_cast<std::size_t>(p)];
        const Vec3 &pr = rest[static_cast<std::size_t>(p)];
        base = Vec3{rest[index].x - pr.x, rest[index].y - pr.y, rest[index].z - pr.z};
      }
      wrot[index] = Multiply(prot, rot[index]);
      const Vec3 moved = Rotate(prot, pos[index]);
      const Vec3 offset = Rotate(prot, base);
      wpos[index] = Vec3{ppos.x + offset.x + moved.x, ppos.y + offset.y + moved.y,
                         ppos.z + offset.z + moved.z};
    }
  }

  void SetFrame(const std::vector<const VmdTrack *> &tracks, double frame) {
    double rotation[4] = {};
    double position[3] = {};
    for (std::size_t i = 0; i < count; ++i) {
      const VmdTrack *track = tracks[i];
      if (track != nullptr && SampleTrack(*track, frame, rotation, position)) {
        rot[i] = Normalize(Quat{rotation[0], rotation[1], rotation[2], rotation[3]});
        pos[i] = Vec3{position[0], position[1], position[2]};
      } else {
        rot[i] = Quat{};
        pos[i] = Vec3{};
      }
    }
    std::vector<std::uint32_t> all(count);
    for (std::size_t i = 0; i < count; ++i)
      all[i] = static_cast<std::uint32_t>(i);
    Refresh(all);
  }

  // World displacement the chain's own *translations* contribute (mmd2bip.py
  // chain_translation). Only the translation part counts: the rotation the chain above
  // already carries is reproduced by the mapped rotation of the target's root bone, and
  // adding it again would double-count it. Each bone's translation is applied in its
  // parent's frame, so the displacement is the parent's world rotation applied to it, and
  // the rotations below leave the accumulated vector alone.
  Vec3 ChainTranslation(const std::vector<std::uint32_t> &chain) const {
    Vec3 total{};
    for (std::uint32_t index : chain) {
      const int p = parent[index];
      const Quat frame = p >= 0 ? wrot[static_cast<std::size_t>(p)] : Quat{};
      const Vec3 moved = Rotate(frame, pos[index]);
      total = Vec3{total.x + moved.x, total.y + moved.y, total.z + moved.z};
    }
    return total;
  }

  double EffectorGap(const IkChain &chain) const {    const Vec3 &effector = wpos[chain.target];
    const Vec3 &ik = wpos[chain.bone];
    return Length(Vec3{effector.x - ik.x, effector.y - ik.y, effector.z - ik.z});
  }

  void SolveChain(const IkChain &chain) {
    for (int iteration = 0; iteration < chain.loop; ++iteration) {
      bool moved = false;
      for (std::size_t slot = 0; slot < chain.links.size(); ++slot) {
        const std::uint32_t link = chain.links[slot];
        const Vec3 &link_pos = wpos[link];
        const Vec3 &effector = wpos[chain.target];
        const Vec3 &ik_pos = wpos[chain.bone];
        const Vec3 a{effector.x - link_pos.x, effector.y - link_pos.y,
                     effector.z - link_pos.z};
        const Vec3 b{ik_pos.x - link_pos.x, ik_pos.y - link_pos.y,
                     ik_pos.z - link_pos.z};
        const double la = Length(a);
        const double lb = Length(b);
        if (la < 1e-9 || lb < 1e-9)
          continue;
        const Vec3 cross{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                         a.x * b.y - a.y * b.x};
        const double sine = Length(cross) / (la * lb);
        const double cosine = Dot(a, b) / (la * lb);
        const double angle = std::atan2(sine, (std::max)(-1.0, (std::min)(1.0, cosine)));
        if (angle < 1e-9 || sine < 1e-12)
          continue;
        const double step = (std::min)(angle, std::fabs(chain.limit));
        const Vec3 axis{cross.x / (sine * la * lb), cross.y / (sine * la * lb),
                        cross.z / (sine * la * lb)};
        const Quat world = FromAxisAngle(axis, step);
        const int p = parent[link];
        const Quat frame = p >= 0 ? wrot[static_cast<std::size_t>(p)] : Quat{};
        Quat local = Normalize(Multiply(Conjugate(frame), Multiply(world, wrot[link])));
        if (chain.limits[slot] != nullptr)
          local = ClampToLimits(local, *chain.limits[slot]);
        rot[link] = local;
        Refresh(subtrees[link]);
        moved = true;
      }
      if (!moved)
        break;
    }
  }

  void Solve(std::vector<IkReport> &report,
             const std::vector<std::string> &disabled,
             const std::unordered_map<std::string, bool> &fk_fallback_available) {
    for (const IkChain &chain : chains) {
      if (std::find(disabled.begin(), disabled.end(),
                    CanonicalBoneName(name[chain.bone])) !=
          disabled.end())
        continue;
      IkReport entry;
      entry.name = name[chain.bone];
      entry.before = EffectorGap(chain);
      const auto fallback =
          fk_fallback_available.find(CanonicalBoneName(entry.name));
      entry.fk_available = fallback != fk_fallback_available.end() &&
                           fallback->second;
      std::vector<Quat> before;
      before.reserve(chain.links.size());
      for (std::uint32_t link : chain.links)
        before.push_back(rot[link]);
      SolveChain(chain);
      entry.after = EffectorGap(chain);
      for (std::size_t i = 0; i < chain.links.size(); ++i)
        entry.applied = (std::max)(entry.applied, QuatAngle(before[i], rot[chain.links[i]]));
      // A complete FK leg is the safer choice when the source IK cannot reach its controller:
      // this is what happens when a VMD authored against one model is solved with a different
      // reference leg length. Keep good IK results, but never pass a half-leg residual into
      // the target-side solve. The 35% / 0.5-unit guard accepts the good Miku sample
      // (~10%) and the repaired StandardRig sample (~15%), while rejecting the measured
      // 35-85% failures.
      const double residual_limit =
          (std::max)(0.5, entry.before * 0.35);
      if (entry.fk_available && entry.before > 0.5 &&
          entry.after > residual_limit) {
        for (std::size_t i = 0; i < chain.links.size(); ++i)
          rot[chain.links[i]] = before[i];
        for (std::uint32_t link : chain.links)
          Refresh(subtrees[link]);
        entry.after = EffectorGap(chain);
        entry.fallback_fk = true;
      }
      report.push_back(entry);
    }
  }
};

const ReferenceBone *FindReference(const std::vector<ReferenceBone> &reference,
                                   const std::string &name) {
  for (const auto &bone : reference) {
    if (bone.name == name)
      return &bone;
  }
  return nullptr;
}

// Builds the target <- MMD table: the explicit bones plus both finger chains, skipping
// anything the target skeleton or the MMD model does not carry (mmd2bip.py:186-254).
std::vector<MappingEntry> BuildMapping(const std::vector<ReferenceBone> &reference,
                                       const Skeleton &skeleton,
                                       bool hip_on_pelvis,
                                       const std::map<std::string, std::string>
                                           &manual_mappings,
                                       const std::set<std::string> &ignored_tracks,
                                       const std::vector<VmdTrack> &vmd_tracks,
                                       const std::vector<ManualParentMapping>
                                           &manual_parent_mappings,
                                       const std::vector<ManualFingerControl>
                                           &manual_finger_controls,
                                       const std::vector<ManualArmIkControl>
                                           &manual_arm_ik_controls) {
  std::unordered_map<std::string, bool> has;
  std::unordered_map<std::string, std::string> reference_name;
  for (const auto &bone : reference)
    if (!bone.name.empty()) {
      const std::string canonical = CanonicalBoneName(bone.name);
      has[canonical] = true;
      reference_name[canonical] = bone.name;
    }
  const auto present = [&has](const std::string &name) {
    const auto found = has.find(CanonicalBoneName(name));
    return found != has.end() && found->second;
  };
  const auto finger_tip_source = [&vmd_tracks](
                                         const std::string &side,
                                         const FingerGroup &finger) {
    const std::string prefix =
        CanonicalBoneName(side + finger.japanese) + "先";
    const std::string numbered_prefix =
        CanonicalBoneName(side + finger.japanese);
    std::string fallback;
    for (const VmdTrack &track : vmd_tracks) {
      const std::string canonical = CanonicalBoneName(track.name);
      if (canonical == prefix)
        return track.name;
      if (fallback.empty() && canonical.size() > numbered_prefix.size() &&
          canonical.compare(0, numbered_prefix.size(), numbered_prefix) == 0 &&
          canonical.compare(canonical.size() - 3, 3, "先") == 0)
        fallback = track.name;
    }
    return fallback;
  };
  const auto finger_digit_sources = [&vmd_tracks](
                                            const std::string &side,
                                            const FingerGroup &finger) {
    const std::string prefix =
        CanonicalBoneName(side + finger.japanese);
    std::vector<std::pair<int, std::string>> numbered;
    for (const VmdTrack &track : vmd_tracks) {
      const std::string canonical = CanonicalBoneName(track.name);
      if (canonical.size() != prefix.size() + 1 ||
          canonical.compare(0, prefix.size(), prefix) != 0)
        continue;
      const char digit = canonical.back();
      if (digit >= '0' && digit <= '4')
        numbered.emplace_back(digit - '0', track.name);
    }
    std::stable_sort(numbered.begin(), numbered.end(),
                     [](const auto &left, const auto &right) {
                       return left.first < right.first;
                     });
    std::vector<std::string> sources;
    for (const auto &[digit, name] : numbered) {
      static_cast<void>(digit);
      sources.push_back(name);
    }
    return sources;
  };

  std::vector<MappingEntry> table;
  for (const auto &entry : kStaticMapping) {
    // A target this rig does not have is not an error: the spine chain alone is three, four
    // or five bones depending on the character (mmd2bip.py's build_mapping skips them too).
    if (skeleton.by_name.count(entry.target) == 0)
      continue;
    std::vector<std::string> sources;
    std::vector<std::string> inverse;
    std::vector<std::string> prefix_inverse;
    std::string direction = entry.source;
    // 髋（下半身）的旋转装在骨盆上，不是大腿上（见 kLegMapping 处的注释）。脊柱则反过来：
    // MMD 的 上半身 挂在 腰 上、不跟 下半身，所以要把骨盆多带进来的那份乘回去 —— 而父链是
    // **左乘**进来的，这份逆源也必须排在 sources 之前（prefix_inverse），右乘只在两个旋转
    // 可交换时才等价。
    if (std::string(entry.target) == "Bip001") {
      // The target's root link carries the whole MMD *placement* chain, not just its middle
      // layer: 全ての親 -> センター -> グルーブ is what says where the character stands and which
      // way it faces, and the current MMD standard puts all of it in グルーブ while leaving
      // センター at a single identity key. Mapping only センター dropped every turn in the file --
      // on 热爱105℃的你.vmd グルーブ accumulates ~2100 deg (about six turns) while the emitted
      // Bip001 rotation stayed constant (sweep 0.00 deg over 3061 frames), so the body slid
      // along the arc its own turn swung it through and never stopped facing its start.
      for (const char *name : {"全ての親", "センター", "グルーブ"}) {
        if (present(name))
          sources.push_back(name);
      }
      if (sources.empty() && present(entry.source))
        sources.push_back(entry.source);
    } else if (std::string(entry.target) == "Bip001-Pelvis") {
      if (present(entry.source)) {
        sources.push_back(entry.source);
      } else if (present("下半身")) {
        direction = "下半身";  // 腰 is missing from a surprising number of exports
      }
      if (hip_on_pelvis && present("下半身"))
        sources.push_back("下半身");
    } else if (std::string(entry.target) == "Bip001-Spine") {
      if (present(entry.source))
        sources.push_back(entry.source);
      if (hip_on_pelvis && present("下半身"))
        prefix_inverse.push_back("下半身");
    } else if (std::string(entry.target) == "Bip001-Spine1" &&
               !present(entry.source) && present("上半身3") && present("上半身2")) {
      // Three-segment StandardRig spine names the sections 上半身/上半身2/上半身3.
      // The older two-segment naming used 上半身/上半身1/上半身2. Keep all three target
      // sections driven instead of dropping the upper-chest 上半身3 track.
      sources.push_back("上半身2");
      direction = "上半身2";
    } else if (std::string(entry.target) == "Bip001-Spine2" && present("上半身3")) {
      sources.push_back("上半身3");
      direction = "上半身3";
    } else if (entry.target == std::string("Bip001-L-Clavicle") ||
               entry.target == std::string("Bip001-R-Clavicle")) {
      const std::string side =
          entry.target == std::string("Bip001-L-Clavicle") ? "左" : "右";
      if (present(side + "肩P"))
        sources.push_back(side + "肩P");
      if (present(entry.source))
        sources.push_back(entry.source);
      if (sources.empty())
        continue;
    } else if (entry.target == std::string("Bip001-L-UpperArm") ||
               entry.target == std::string("Bip001-R-UpperArm")) {
      const std::string side =
          entry.target == std::string("Bip001-L-UpperArm") ? "左" : "右";
      // MMD's shoulderC cancels shoulderP for the arm. The target's clavicle carries
      // shoulderP*shoulder, so the upper arm must prefix-invert shoulderP to preserve
      // that hierarchy instead of rotating the whole arm with the shoulder deform bone.
      if (present(side + "肩P") && present(side + "肩C"))
        prefix_inverse.push_back(side + "肩P");
      if (present(entry.source))
        sources.push_back(entry.source);
      if (sources.empty())
        continue;
    } else if (present(entry.source)) {
      sources.push_back(entry.source);
    } else {
      continue;
    }
    if (sources.empty())
      continue;
    table.push_back(
        MappingEntry{entry.target, sources, direction, false, inverse, prefix_inverse});
  }
  for (const auto &entry : kLegMapping) {
    const std::string direction =
        present(entry.direction) ? std::string(entry.direction)
                                 : (present(entry.fallback) ? std::string(entry.fallback)
                                                            : std::string());
    if (direction.empty())
      continue;
    // MMD has one more joint in the leg than the target. 下半身 (the hip) now rides on the
    // pelvis, so the thigh is driven by 足 alone; keeping 下半身 here as well would apply the
    // hip rotation twice (mmd2bip.py:130-139).
    const bool thigh = direction == "左足" || direction == "右足" ||
                       direction == "左足D" || direction == "右足D";
    const std::string source =
        present(entry.source) ? std::string(entry.source) : direction;
    table.push_back(MappingEntry{
        entry.target,
        thigh ? (hip_on_pelvis ? std::vector<std::string>{direction}
                                     : std::vector<std::string>{"下半身", direction})
              : std::vector<std::string>{source},
        direction, false});
  }
  for (const auto &[side, side_en] :
       {std::pair<const char *, const char *>{"左", "L"}, {"右", "R"}}) {
    for (const auto &finger : kFingers) {
      const std::string base =
          std::string("Bip001-") + side_en + "-" + finger.english;
      std::vector<std::string> targets;
      for (const char *suffix : {"", "1", "2", "3"}) {
        const std::string name = base + suffix;
        if (skeleton.by_name.count(name) != 0)
          targets.push_back(name);
      }
      if (targets.empty())
        continue;
      std::vector<std::string> chain;
      for (const char *digit : {"０", "１", "２", "３", "４"}) {
        const std::string candidate = std::string(side) + finger.japanese + digit;
        if (present(candidate) && chain.size() < targets.size()) {
          chain.push_back(reference_name.at(CanonicalBoneName(candidate)));
        }
      }
      const std::string tip_source = finger_tip_source(side, finger);
      if (chain.empty() && tip_source.empty())
        continue;
      const auto vmd_digits = finger_digit_sources(side, finger);
      if (vmd_digits.size() == targets.size()) {
        // Equal source/target cardinality is the standard four-segment case: preserve the
        // authored joint order one-to-one, including a source `０` that the selected
        // reference happens to omit.
        for (std::size_t index{}; index != targets.size(); ++index) {
          MappingEntry entry;
          entry.target = targets[index];
          entry.sources = {vmd_digits[index]};
          entry.direction = vmd_digits[index];
          table.push_back(std::move(entry));
        }
        if (!tip_source.empty())
          table.back().sources.push_back(tip_source);
        continue;
      }
      if (vmd_digits.size() > targets.size()) {
        // Some costumes expose only two or three target joints for the ring/pinky while the
        // source motion has four. Keep every authored source rotation by folding adjacent
        // source joints into the nearest available target joint instead of dropping the two
        // ends as unmapped.
        std::vector<std::vector<std::string>> groups(targets.size());
        for (std::size_t source{}; source != vmd_digits.size(); ++source) {
          const std::size_t target = (std::min)(
              source * targets.size() / vmd_digits.size(),
              targets.size() - 1);
          groups[target].push_back(vmd_digits[source]);
        }
        for (std::size_t index{}; index != targets.size(); ++index) {
          MappingEntry entry;
          entry.target = targets[index];
          entry.sources = groups[index];
          if (!entry.sources.empty())
            entry.direction = entry.sources.front();
          else
            entry.direction = targets[index];
          table.push_back(std::move(entry));
        }
        if (!tip_source.empty() && !table.empty() &&
            table.back().target == targets.back()) {
          table.back().sources.push_back(tip_source);
          if (table.back().direction.empty())
            table.back().direction = tip_source;
        }
        continue;
      }
      // Target finger chains commonly have three or four joints while VMD models can carry
      // both the usual three and four-segment ring/pinky chains. Align up to the target's
      // capacity by cumulative length instead of hard-coding three: the old cap left a
      // four-segment `０/１/２/３` chain's last joint reported as unmapped.
      std::vector<double> target_lengths;
      for (const auto &name : targets)
        target_lengths.push_back(Length(skeleton.bones[skeleton.by_name.at(name)]
                                            .base_translation));
      const auto target_fractions = CumulativeFractions(target_lengths);
      // MMD measures the same chain from the wrist bone into the last joint.
      std::vector<double> source_lengths;
      const ReferenceBone *hand = FindReference(reference, std::string(side) + "手首");
      const ReferenceBone *first =
          chain.empty() ? nullptr : FindReference(reference, chain[0]);
      if (hand != nullptr && first != nullptr) {
        source_lengths.push_back(Length(Vec3{first->position.x - hand->position.x,
                                             first->position.y - hand->position.y,
                                             first->position.z - hand->position.z}));
      }
      for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const ReferenceBone *a = FindReference(reference, chain[i]);
        const ReferenceBone *b = FindReference(reference, chain[i + 1]);
        if (a != nullptr && b != nullptr) {
          source_lengths.push_back(Length(Vec3{b->position.x - a->position.x,
                                               b->position.y - a->position.y,
                                               b->position.z - a->position.z}));
        }
      }
      const auto source_fractions = CumulativeFractions(source_lengths);
      // Greedy nearest-fraction alignment, strictly increasing (mmd2bip.py:169-183).
      std::vector<std::size_t> assignment;
      int used = -1;
      for (double fraction : source_fractions) {
        int best = -1;
        double best_gap = 0.0;
        for (std::size_t index = static_cast<std::size_t>(used + 1);
             index < target_fractions.size(); ++index) {
          const double gap = std::fabs(target_fractions[index] - fraction);
          if (best < 0 || gap < best_gap) {
            best = static_cast<int>(index);
            best_gap = gap;
          }
        }
        if (best < 0)
          continue;
        assignment.push_back(static_cast<std::size_t>(best));
        used = best;
      }
      std::vector<std::string> assigned(targets.size());
      for (std::size_t i = 0; i < assignment.size() && i < chain.size(); ++i)
        assigned[assignment[i]] = chain[i];
      for (std::size_t i = 0; i < targets.size(); ++i) {
        MappingEntry entry;
        entry.target = targets[i];
        if (!assigned[i].empty()) {
          entry.sources = {assigned[i]};
          entry.direction = assigned[i];
        } else {
          // The extra leading bone MMD has no counterpart for follows the hand rigidly.
          entry.direction = targets[i];
        }
        table.push_back(std::move(entry));
      }
      if (!tip_source.empty() && !table.empty() && table.back().target == targets.back()) {
        if (table.back().sources.empty()) {
          table.back().sources.push_back(tip_source);
          table.back().direction = tip_source;
        } else if (std::find(table.back().sources.begin(), table.back().sources.end(),
                             tip_source) == table.back().sources.end()) {
          table.back().sources.push_back(tip_source);
        }
      }
    }
  }
  std::unordered_map<std::string, bool> mapped_source_names;
  for (const MappingEntry &entry : table) {
    for (const std::string &source : entry.sources)
      mapped_source_names[CanonicalBoneName(source)] = true;
  }
  for (const ManualFingerControl &control : manual_finger_controls)
    mapped_source_names[CanonicalBoneName(control.source)] = true;
  for (const ManualArmIkControl &control : manual_arm_ik_controls)
    mapped_source_names[CanonicalBoneName(control.source)] = true;
  for (const auto &[side, side_en] :
       {std::pair<const char *, const char *>{"左", "L"}, {"右", "R"}}) {
    for (const auto &finger : kFingers) {
      std::vector<std::string> targets;
      const std::string base =
          std::string("Bip001-") + side_en + "-" + finger.english;
      for (const char *suffix : {"", "1", "2", "3"}) {
        const std::string name = base + suffix;
        if (skeleton.by_name.count(name) != 0)
          targets.push_back(name);
      }
      if (targets.empty())
        continue;
      for (const std::string &source :
           finger_digit_sources(side, finger)) {
        const std::string canonical = CanonicalBoneName(source);
        if (mapped_source_names[canonical])
          continue;
        const char digit = canonical.empty() ? '\0' : canonical.back();
        const std::string &target =
            digit == '0' ? targets.front() : targets.back();
        auto entry = std::find_if(
            table.begin(), table.end(),
            [&target](const MappingEntry &candidate) {
              return candidate.target == target;
            });
        if (entry == table.end()) {
          table.push_back(
              MappingEntry{target, {source}, source, false});
          mapped_source_names[canonical] = true;
          continue;
        }
        if (digit == '0') {
          entry->sources.insert(entry->sources.begin(), source);
        } else {
          const std::string tip = finger_tip_source(side, finger);
          const auto tip_position =
              tip.empty()
                  ? entry->sources.end()
                  : std::find(entry->sources.begin(),
                              entry->sources.end(), tip);
          entry->sources.insert(tip_position, source);
        }
        mapped_source_names[canonical] = true;
      }
    }
  }
  for (const auto &[source, target] : manual_mappings) {
    if (source.empty() || target.empty() ||
        skeleton.by_name.count(target) == 0)
      continue;
    const bool target_already_mapped =
        std::any_of(table.begin(), table.end(),
                    [&target](const MappingEntry &entry) {
                      return entry.target == target;
                    });
    if (target_already_mapped)
      continue;
    table.push_back(
        MappingEntry{target, std::vector<std::string>{source}, source, false});
  }
  // Treat each entry as "insert source immediately above target". Inserting at the front
  // makes later additions the outermost parent, matching the UI's "add another segment"
  // flow. A source may target several bones, while exact duplicate pairs are no-ops.
  for (const ManualParentMapping &mapping : manual_parent_mappings) {
    if (mapping.source.empty() || mapping.target.empty() ||
        skeleton.by_name.count(mapping.target) == 0 ||
        (!mapping.inherit_rotation && !mapping.inherit_position))
      continue;
    auto entry = std::find_if(
        table.begin(), table.end(), [&mapping](const MappingEntry &candidate) {
          return candidate.target == mapping.target;
        });
    if (entry == table.end()) {
      MappingEntry created;
      created.target = mapping.target;
      created.direction = mapping.target;
      entry = table.insert(table.end(), std::move(created));
    }
    ManualParentMapping combined = mapping;
    const auto existing_parent = std::find_if(
        entry->parents.begin(), entry->parents.end(),
        [&mapping](const ManualParentMapping &candidate) {
          return candidate.source == mapping.source;
        });
    if (existing_parent != entry->parents.end()) {
      combined.inherit_rotation =
          existing_parent->inherit_rotation || mapping.inherit_rotation;
      combined.inherit_position =
          existing_parent->inherit_position || mapping.inherit_position;
      if (combined.anchor.empty())
        combined.anchor = existing_parent->anchor;
      *existing_parent = combined;
    } else {
      entry->parents.push_back(combined);
    }
    const auto already_used = [&mapping](const std::vector<std::string> &sources) {
      return std::find(sources.begin(), sources.end(), mapping.source) !=
             sources.end();
    };
    if (!combined.inherit_rotation || already_used(entry->sources) ||
        already_used(entry->inverse) || already_used(entry->prefix_inverse))
      continue;
    entry->sources.insert(entry->sources.begin(), mapping.source);
  }
  const auto is_controlled_finger =
      [](const std::string &target, const ManualFingerControl &control) {
        if (control.side < 0 || control.side > 1 ||
            control.finger < -1 || control.finger > 4)
          return false;
        const bool target_left = target.rfind("Bip001-L-", 0) == 0;
        const bool target_right = target.rfind("Bip001-R-", 0) == 0;
        if (!target_left && !target_right)
          return false;
        if (!control.paired &&
            (control.side == 0) != target_left)
          return false;
        const char side = target_left ? 'L' : 'R';
        for (int finger = 0; finger != 5; ++finger) {
          if (control.finger >= 0 && control.finger != finger)
            continue;
          const std::string base = std::string("Bip001-") + side + "-" +
                                   kFingers[finger].english;
          if (target == base || target == base + "1" ||
              target == base + "2" || target == base + "3")
            return true;
        }
        return false;
      };
  for (const ManualFingerControl &control : manual_finger_controls) {
    if (control.source.empty() || control.input_axis < 0 ||
        control.input_axis > 2 || !std::isfinite(control.gain) ||
        std::fabs(control.gain) > 3.0F)
      continue;
    for (MappingEntry &entry : table) {
      const bool side_match =
          control.paired ||
          (control.side == 0 ? entry.target.rfind("Bip001-L-", 0) == 0
                             : entry.target.rfind("Bip001-R-", 0) == 0);
      if (!side_match || !is_controlled_finger(entry.target, control))
        continue;
      entry.finger_controls.push_back(control);
    }
  }
  std::unordered_map<std::string, bool> ignored_sources;
  for (const std::string &source : ignored_tracks)
    ignored_sources[CanonicalBoneName(source)] = true;
  if (!ignored_sources.empty()) {
    for (auto entry = table.begin(); entry != table.end();) {
      const bool had_source =
          !entry->sources.empty() || !entry->inverse.empty() ||
          !entry->prefix_inverse.empty() || !entry->parents.empty() ||
          !entry->finger_controls.empty();
      const auto erase_source = [&ignored_sources](
                                    std::vector<std::string> &sources) {
        sources.erase(
            std::remove_if(
                sources.begin(), sources.end(),
                [&ignored_sources](const std::string &source) {
                  return ignored_sources.count(CanonicalBoneName(source)) != 0;
                }),
            sources.end());
      };
      erase_source(entry->sources);
      erase_source(entry->inverse);
      erase_source(entry->prefix_inverse);
      entry->parents.erase(
          std::remove_if(
              entry->parents.begin(), entry->parents.end(),
              [&ignored_sources](const ManualParentMapping &mapping) {
                return ignored_sources.count(
                           CanonicalBoneName(mapping.source)) != 0;
              }),
          entry->parents.end());
      entry->finger_controls.erase(
          std::remove_if(
              entry->finger_controls.begin(), entry->finger_controls.end(),
              [&ignored_sources](const ManualFingerControl &control) {
                return ignored_sources.count(
                           CanonicalBoneName(control.source)) != 0;
              }),
          entry->finger_controls.end());
      if (ignored_sources.count(CanonicalBoneName(entry->direction)) != 0) {
        if (!entry->sources.empty())
          entry->direction = entry->sources.front();
      }
      if (had_source && entry->sources.empty() &&
          entry->inverse.empty() && entry->prefix_inverse.empty() &&
          entry->parents.empty() && entry->finger_controls.empty()) {
        entry = table.erase(entry);
      } else {
        ++entry;
      }
    }
  }
  return table;
}

}  // namespace

VmdCompatibilityReport AnalyzeVmdCompatibility(
    const std::vector<std::uint8_t> &vmd_bytes) {
  const VmdDocument vmd = ParseVmd(vmd_bytes);
  if (!vmd.ok) {
    VmdCompatibilityReport report;
    report.error = vmd.error;
    return report;
  }
  return AnalyzeParsedVmdCompatibility(vmd);
}

VmdCompatibilityReport AnalyzeVmdCompatibilityForReference(
    const std::vector<std::uint8_t> &vmd_bytes,
    const std::string &reference_pmx_json,
    const std::string &skeleton_json,
    const std::map<std::string, std::string> &manual_mappings,
    const std::set<std::string> &ignored_tracks,
    const int reference_mode,
    const std::vector<ManualParentMapping> &manual_parent_mappings,
    const std::vector<ManualFingerControl> &manual_finger_controls,
    const std::vector<ManualArmIkControl> &manual_arm_ik_controls) {
  const VmdDocument vmd = ParseVmd(vmd_bytes);
  if (!vmd.ok) {
    VmdCompatibilityReport report;
    report.error = vmd.error;
    report.reference_mode = reference_mode;
    return report;
  }
  VmdCompatibilityReport report = AnalyzeParsedVmdCompatibility(vmd);
  report.reference_mode = reference_mode;

  Skeleton skeleton;
  if (!LoadSkeleton(skeleton_json, skeleton)) {
    report.reference_mapping_error = skeleton.error;
    return report;
  }
  std::vector<ReferenceBone> reference;
  std::string reference_error;
  if (!LoadReference(reference_pmx_json, reference, reference_error)) {
    report.reference_mapping_error = reference_error;
    return report;
  }
  const auto table =
      BuildMapping(reference, skeleton, true, manual_mappings, ignored_tracks,
                   vmd.tracks, manual_parent_mappings,
                   manual_finger_controls, manual_arm_ik_controls);
  std::unordered_map<std::string, std::vector<std::string>> mapped_targets;
  std::unordered_map<std::string, std::vector<std::string>>
      mapped_sources_by_target;
  const auto add_target = [&mapped_targets](const std::string &source,
                                            const std::string &target) {
    auto &targets = mapped_targets[CanonicalBoneName(source)];
    if (std::find(targets.begin(), targets.end(), target) == targets.end())
      targets.push_back(target);
  };
  for (const MappingEntry &entry : table) {
    for (const ManualParentMapping &parent : entry.parents)
      add_target(parent.source, entry.target);
    for (const ManualFingerControl &control : entry.finger_controls)
      add_target(control.source, entry.target);
    for (const std::string &source : entry.prefix_inverse)
      add_target(source, entry.target);
    for (const std::string &source : entry.sources)
      add_target(source, entry.target);
    for (const std::string &source : entry.inverse)
      add_target(source, entry.target);
    auto &target_sources = mapped_sources_by_target[entry.target];
    const auto add_source = [&target_sources](const std::string &source) {
      const std::string canonical = CanonicalBoneName(source);
      if (std::find(target_sources.begin(), target_sources.end(), canonical) ==
          target_sources.end())
        target_sources.push_back(canonical);
    };
    for (const std::string &source : entry.prefix_inverse)
      add_source(source);
    for (const ManualParentMapping &parent : entry.parents)
      add_source(parent.source);
    for (const ManualFingerControl &control : entry.finger_controls)
      add_source(control.source);
    for (const std::string &source : entry.sources)
      add_source(source);
    for (const std::string &source : entry.inverse)
      add_source(source);
  }
  for (const ManualArmIkControl &control : manual_arm_ik_controls)
    add_target(control.source, "Arm IK");

  // IK controllers position the source pose but do not produce a Biped bone track directly.
  // Count them as consumed so the reference audit does not label a working IK chain as lost.
  for (const ReferenceBone &bone : reference) {
    if (!bone.has_ik || bone.name.empty())
      continue;
    if (std::any_of(vmd.tracks.begin(), vmd.tracks.end(),
                    [&bone](const VmdTrack &track) {
                      return CanonicalBoneName(track.name) ==
                             CanonicalBoneName(bone.name);
                    }))
      add_target(bone.name, "IK");
  }

  std::unordered_map<std::string, bool> manual_sources;
  std::unordered_map<std::string, std::string> manual_targets;
  for (const auto &[source, target] : manual_mappings) {
    const std::string canonical = CanonicalBoneName(source);
    manual_sources[canonical] = true;
    manual_targets[canonical] = target;
  }
  for (const ManualParentMapping &mapping : manual_parent_mappings)
    manual_sources[CanonicalBoneName(mapping.source)] = true;
  for (const ManualFingerControl &control : manual_finger_controls)
    manual_sources[CanonicalBoneName(control.source)] = true;
  for (const ManualArmIkControl &control : manual_arm_ik_controls)
    manual_sources[CanonicalBoneName(control.source)] = true;
  std::unordered_map<std::string, bool> ignored_source_set;
  for (const std::string &source : ignored_tracks)
    ignored_source_set[CanonicalBoneName(source)] = true;
  std::unordered_map<std::string, bool> reference_names;
  for (const ReferenceBone &bone : reference)
    reference_names[CanonicalBoneName(bone.name)] = true;
  std::unordered_map<std::string, bool> vmd_sources;
  for (const VmdTrack &track : vmd.tracks)
    vmd_sources[CanonicalBoneName(track.name)] = true;
  for (const ManualParentMapping &mapping : manual_parent_mappings) {
    if (!mapping.inherit_rotation && !mapping.inherit_position)
      continue;
    const std::string canonical = CanonicalBoneName(mapping.source);
    const VmdCompatibilityReport::MappingIssueReason reason =
        skeleton.by_name.count(mapping.target) == 0
            ? VmdCompatibilityReport::MappingIssueReason::MissingTarget
            : vmd_sources.count(canonical) == 0
                  ? VmdCompatibilityReport::MappingIssueReason::SourceMissing
                  : VmdCompatibilityReport::MappingIssueReason::None;
    if (reason == VmdCompatibilityReport::MappingIssueReason::None)
      continue;
    const auto duplicate = std::find_if(
        report.mapping_issues.begin(), report.mapping_issues.end(),
        [&mapping, reason](const auto &issue) {
          return issue.source == mapping.source &&
                 issue.target == mapping.target && issue.reason == reason;
        });
    if (duplicate == report.mapping_issues.end())
      report.mapping_issues.push_back({mapping.source, mapping.target, reason});
  }
  for (const ManualFingerControl &control : manual_finger_controls) {
    const bool has_target = std::any_of(
        table.begin(), table.end(), [&control](const MappingEntry &entry) {
          return std::any_of(
              entry.finger_controls.begin(), entry.finger_controls.end(),
              [&control](const ManualFingerControl &candidate) {
                return candidate.source == control.source &&
                       candidate.kind == control.kind &&
                       candidate.side == control.side &&
                       candidate.finger == control.finger;
              });
        });
    const std::string canonical = CanonicalBoneName(control.source);
    const VmdCompatibilityReport::MappingIssueReason reason =
        !has_target
            ? VmdCompatibilityReport::MappingIssueReason::MissingTarget
            : vmd_sources.count(canonical) == 0
                  ? VmdCompatibilityReport::MappingIssueReason::SourceMissing
                  : VmdCompatibilityReport::MappingIssueReason::None;
    if (reason == VmdCompatibilityReport::MappingIssueReason::None)
      continue;
    const std::string target = "(hand finger chain)";
    const auto duplicate = std::find_if(
        report.mapping_issues.begin(), report.mapping_issues.end(),
        [&control, &target, reason](const auto &issue) {
          return issue.source == control.source && issue.target == target &&
                 issue.reason == reason;
        });
    if (duplicate == report.mapping_issues.end())
      report.mapping_issues.push_back({control.source, target, reason});
  }
  for (const ManualArmIkControl &control : manual_arm_ik_controls) {
    const std::string canonical = CanonicalBoneName(control.source);
    const auto upper = skeleton.by_name.find(
        std::string("Bip001-") +
        (control.side == 0 ? "L-UpperArm" : "R-UpperArm"));
    if (vmd_sources.count(canonical) == 0)
      report.mapping_issues.push_back(
          {control.source, "Arm IK",
           VmdCompatibilityReport::MappingIssueReason::SourceMissing});
    else if (upper == skeleton.by_name.end())
      report.mapping_issues.push_back(
          {control.source, "Arm IK",
           VmdCompatibilityReport::MappingIssueReason::MissingTarget});
  }
  std::unordered_map<std::string, bool> unique_targets;
  for (const auto &[source, targets] : mapped_targets) {
    static_cast<void>(source);
    for (const std::string &target : targets)
      unique_targets[target] = true;
  }
  report.reference_mapped_targets =
      static_cast<std::uint32_t>(unique_targets.size());

  for (const VmdTrack &track : vmd.tracks) {
    const std::string canonical = CanonicalBoneName(track.name);
    if (ignored_source_set.count(canonical) != 0) {
      auto item = DescribeVmdTrack(track);
      item.reason = VmdCompatibilityReport::MappingIssueReason::Ignored;
      report.ignored_reference_tracks.push_back(std::move(item));
      continue;
    }
    const auto mapped = mapped_targets.find(canonical);
    if (mapped != mapped_targets.end()) {
      VmdCompatibilityReport::MappedTrack item;
      item.name = track.name;
      item.key_count = static_cast<std::uint32_t>(track.keys.size());
      item.animated =
          AnimatedVmdTrack(track, item.first_nonzero_frame);
      const auto description = DescribeVmdTrack(track);
      item.kind = description.kind;
      item.signature = description.signature;
      item.manual = manual_sources.count(canonical) != 0;
      for (const std::string &target : mapped->second) {
        if (!item.target.empty())
          item.target += ", ";
        item.target += target;
      }
      report.mapped_reference_tracks.push_back(std::move(item));
      continue;
    }
    auto item = DescribeVmdTrack(track);
    const auto manual = manual_targets.find(canonical);
    if (manual != manual_targets.end()) {
      if (skeleton.by_name.count(manual->second) == 0) {
        item.reason =
            VmdCompatibilityReport::MappingIssueReason::MissingTarget;
        report.mapping_issues.push_back(
            {track.name, manual->second, item.reason});
      } else {
        const auto occupied = mapped_sources_by_target.find(manual->second);
        if (occupied != mapped_sources_by_target.end() &&
            !occupied->second.empty() &&
            std::find(occupied->second.begin(), occupied->second.end(),
                      canonical) == occupied->second.end()) {
          item.reason =
              VmdCompatibilityReport::MappingIssueReason::TargetOccupied;
          report.mapping_issues.push_back(
              {track.name, manual->second, item.reason});
        } else if (reference_names.count(canonical) == 0) {
          item.reason =
              VmdCompatibilityReport::MappingIssueReason::MissingReference;
        }
      }
    } else if (reference_names.count(canonical) == 0) {
      item.reason =
          VmdCompatibilityReport::MappingIssueReason::MissingReference;
    }
    switch (item.kind) {
      case VmdCompatibilityReport::TrackKind::Humanoid:
        ++report.reference_unmapped_humanoid;
        break;
      case VmdCompatibilityReport::TrackKind::Control:
        ++report.reference_unmapped_control;
        break;
      case VmdCompatibilityReport::TrackKind::Accessory:
        ++report.reference_unmapped_accessory;
        break;
      case VmdCompatibilityReport::TrackKind::Other:
        ++report.reference_unmapped_other;
        break;
    }
    report.reference_unmapped_tracks.push_back(std::move(item));
  }
  for (const auto &[source, target] : manual_mappings) {
    const std::string canonical = CanonicalBoneName(source);
    if (vmd_sources.count(canonical) != 0)
      continue;
    report.mapping_issues.push_back(
        {source, target,
         skeleton.by_name.count(target) == 0
             ? VmdCompatibilityReport::MappingIssueReason::MissingTarget
             : VmdCompatibilityReport::MappingIssueReason::SourceMissing});
  }
  SortUnmappedTracks(report.reference_unmapped_tracks);
  SortUnmappedTracks(report.ignored_reference_tracks);
  std::stable_sort(report.mapped_reference_tracks.begin(),
                   report.mapped_reference_tracks.end(),
                   [](const auto &left, const auto &right) {
                     if (left.manual != right.manual)
                       return left.manual;
                     if (left.animated != right.animated)
                       return left.animated;
                     if (left.key_count != right.key_count)
                       return left.key_count > right.key_count;
                     return left.name < right.name;
                   });
  report.reference_mapped_tracks =
      static_cast<std::uint32_t>(report.mapped_reference_tracks.size());
  report.reference_mapping_ready = true;
  return report;
}

Result BuildMotion(const Input &input) {
  Result result;
  const auto fail = [&result](const std::string &message) {
    result.ok = false;
    result.error = message;
    return result;
  };

  Skeleton skeleton;
  if (!LoadSkeleton(input.skeleton_json, skeleton))
    return fail(skeleton.error);
  std::vector<ReferenceBone> reference;
  std::string reference_error;
  if (!LoadReference(input.reference_pmx_json, reference, reference_error))
    return fail(reference_error);
  const auto vmd = ParseVmd(input.vmd_bytes);
  if (!vmd.ok)
    return fail(vmd.error);
  // VMD keys are nominally authored at 30 fps, and that is what the reference converter
  // assumed. A motion exported on a 60 Hz timeline stores the same animation across twice
  // as many frames, so playing it back as 30 fps runs at half speed -- and key density
  // cannot tell the two apart (a 60 Hz resample keys every second frame, which looks
  // exactly like a sparse 30 fps motion). The only reliable signal available without
  // asking is the file's own name, which is what an author writes it in for.
  result.native_fps = 30;
  {
    const std::string &name = input.vmd_path;
    const std::string lower = [&name] {
      std::string out = name;
      for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return out;
    }();
    if (lower.find("60fps") != std::string::npos || lower.find("60_fps") != std::string::npos ||
        lower.find("-60.") != std::string::npos || lower.find("_60.") != std::string::npos)
      result.native_fps = 60;
  }

  const auto table = BuildMapping(reference, skeleton, input.hip_on_pelvis,
                                  input.manual_mappings, input.ignored_tracks,
                                  vmd.tracks,
                                  input.manual_parent_mappings,
                                  input.manual_finger_controls,
                                  input.manual_arm_ik_controls);
  std::unordered_map<std::string, const MappingEntry *> mapped_targets;
  for (const auto &entry : table)
    mapped_targets[entry.target] = &entry;

  // Frame range from the raw keys, not from the dump's recorded range: the two agree, but
  // the sampling loop is what decides which frames exist.
  const auto &range = vmd.bone_frame_range;
  if (range.empty())
    return fail("no bone keyframes in the VMD");
  const std::uint32_t frame_lo = range[0];
  const std::uint32_t frame_hi = range[1];
  const std::uint32_t step = static_cast<std::uint32_t>(
      (std::max)(1.0, std::round(static_cast<double>(result.native_fps) / input.fps)));
  std::vector<double> frames;
  for (std::uint32_t frame = frame_lo; frame <= frame_hi; frame += step)
    frames.push_back(static_cast<double>(frame));
  if (input.max_frames != 0 && frames.size() > input.max_frames)
    frames.resize(input.max_frames);

  std::unordered_map<std::string, const VmdTrack *> tracks;
  std::unordered_map<std::string, const VmdTrack *> tracks_by_canonical_name;
  for (const auto &track : vmd.tracks) {
    tracks[track.name] = &track;
    tracks_by_canonical_name[CanonicalBoneName(track.name)] = &track;
  }
  const auto find_track = [&](const std::string &name) -> const VmdTrack * {
    const auto exact = tracks.find(name);
    if (exact != tracks.end())
      return exact->second;
    const auto canonical = tracks_by_canonical_name.find(CanonicalBoneName(name));
    return canonical == tracks_by_canonical_name.end() ? nullptr : canonical->second;
  };

  // ---- source-model IK ---------------------------------------------------------------
  // The model's own skeleton is posed first and its IK chains solved, then the mapped
  // rotations are read out of that pose. With `input.ik` off nothing here is touched and
  // the converter behaves exactly as it did before the solver existed -- which is what the
  // harness checks first (mmd2bip.py `--ik`).
  // ---- source-model pose --------------------------------------------------------------
  // The model's own skeleton is posed every frame: its IK chains are solved into bone
  // rotations before the mapped rotations are read out of that pose, and the root chain's
  // combined translation becomes the root motion. `input.ik` only turns the *solver* off
  // (the regression switch, which must reproduce the pre-IK output exactly); the pose itself
  // is always built, because the root translation needs it either way.
  IkPose ik_pose;
  std::vector<const VmdTrack *> ik_tracks;
  std::unordered_map<std::string, std::size_t> ik_index_by_name;
  std::unordered_map<std::string, std::vector<VmdIkState>> ik_states;
  std::vector<IkReport> ik_reports;
  std::unordered_map<std::string, bool> fk_fallback_available;
  const bool pose_ready = !reference.empty();
  const bool solve_ik =
      input.ik && input.ik_mode != IkMode::ForceOff && pose_ready;
  if (pose_ready) {
    ik_pose.Build(reference);
    ik_tracks.assign(reference.size(), nullptr);
    for (const ReferenceBone &bone : reference) {
      ik_tracks[bone.index] = find_track(bone.name);
      ik_index_by_name[CanonicalBoneName(bone.name)] =
          bone.index;  // a duplicate name keeps the last one
    }
    const auto leg_source = [&](const std::string &primary,
                                const std::string &fallback) {
      if (FindReference(reference, primary) != nullptr)
        return primary;
      if (!fallback.empty() && FindReference(reference, fallback) != nullptr)
        return fallback;
      return std::string();
    };
    for (const std::string &side : {"左", "右"}) {
      const std::string hip = leg_source(side + "足", side + "足D");
      const std::string knee = leg_source(side + "ひざ", side + "ひざD");
      const std::string ankle = leg_source(side + "足首", side + "足首D");
      const std::string toe = leg_source(side + "つま先", side + "足先EX");
      const bool foot_fk =
          !hip.empty() && !knee.empty() && !ankle.empty() &&
          find_track(hip) != nullptr && find_track(knee) != nullptr &&
          find_track(ankle) != nullptr;
      const bool toe_fk = !toe.empty() && find_track(toe) != nullptr;
      fk_fallback_available[CanonicalBoneName(side + "足IK")] = foot_fk;
      fk_fallback_available[CanonicalBoneName(side + "つま先IK")] = toe_fk;
    }
    // MMD's IK is on unless a record says otherwise, and the records are state *changes*
    // in file order rather than sorted ones (mmd2bip.py sorts them the same way).
    for (const VmdIkState &state : vmd.ik_states)
      ik_states[CanonicalBoneName(state.name)].push_back(state);
    for (auto &[name, records] : ik_states) {
      std::stable_sort(records.begin(), records.end(),
                       [](const VmdIkState &left, const VmdIkState &right) {
                         return left.frame < right.frame;
                       });
    }
  }

  // Two rest rotations per bone, and they are not interchangeable:
  //   * `rest_reference` is the engine reference (bind) pose the geometry is skinned in.
  //     Every converted local rotation is built against it, and the mapped bones' `rest`
  //     entry is reported from it (mmd2bip.py:790, 1154-1160).
  //   * `rest_local` is what the document starts from and what an undriven bone's
  //     constant track holds -- the same reference pose here, because the whole export
  //     carries refLocal (mmd2bip.py:1341-1360).
  std::vector<Quat> rest_reference(skeleton.bones.size());
  std::vector<Quat> rest_local(skeleton.bones.size());
  for (const auto &bone : skeleton.bones) {
    rest_reference[bone.index] = bone.has_ref_local ? bone.ref_local : bone.base_local;
    rest_local[bone.index] = bone.has_ref_local ? bone.ref_local : bone.base_local;
  }
  std::vector<Quat> rest_component(skeleton.bones.size());
  for (const auto &bone : skeleton.bones) {
    const Quat parent = bone.parent >= 0
                            ? rest_component[static_cast<std::size_t>(bone.parent)]
                            : Quat{};
    rest_component[bone.index] = Multiply(parent, rest_local[bone.index]);
  }
  // `crot_cap` in the reference implementation: computed once, before any correction, and
  // every "which way does this bone point now" is measured against *this* (mmd2bip.py:730
  // and 866). Using the corrected chain here shifts every descendant by the parent's
  // correction a second time -- the neck, head, spine1 and both legs all came out wrong
  // until this was separated.
  std::vector<Quat> rest_crot_cap = rest_component;

  // Which MMD bone each target follows, and which MMD names are mapped at all.
  std::unordered_map<std::string, std::string> source_of_target;
  std::unordered_map<std::string, bool> mapped_source_names;
  for (const auto &entry : table) {
    source_of_target[entry.target] = entry.direction;
    for (const auto &source : entry.sources)
      mapped_source_names[source] = true;
  }
  for (const ManualFingerControl &control : input.manual_finger_controls)
    mapped_source_names[CanonicalBoneName(control.source)] = true;
  for (const ManualArmIkControl &control : input.manual_arm_ik_controls)
    mapped_source_names[CanonicalBoneName(control.source)] = true;
  const auto reference_first_child = ReferenceFirstChildren(reference);
  const auto reference_directions = ReferenceDirections(
      reference, [&reference] {
        std::unordered_map<std::string, std::vector<std::string>> children;
        for (const auto &bone : reference) {
          if (bone.parent < 0 || static_cast<std::size_t>(bone.parent) >= reference.size())
            continue;
          const std::string &parent =
              reference[static_cast<std::size_t>(bone.parent)].name;
          if (!parent.empty())
            children[parent].push_back(bone.name);
        }
        return children;
      }());
  const auto reference_mapped_directions =
      ReferenceMappedChildDirections(reference, mapped_source_names);

  // ---- hand chain rest: captured pose or reference pose? ---------------------------
  // Which one gives the hand a sane rest is a property of the capture: one character's
  // captured hand is relaxed and reads as a normal hand, another's is curled and every
  // finger joint then needs a 95-144 degree correction, which the bogus-reference guard
  // rejects -- leaving the fingers stuck in the curled pose. Measure both and take the
  // smaller (mmd2bip.py:725-782). `live_rot` is the captured rotation; `rest_reference`
  // is the bind pose the rest of the body uses.
  std::vector<Quat> live_rot(skeleton.bones.size());
  for (const auto &bone : skeleton.bones)
    live_rot[bone.index] = bone.captured_local;
  const auto worst_finger_correction = [&](bool use_live) {
    std::vector<Quat> probe_local = rest_reference;
    for (const auto &bone : skeleton.bones) {
      if (use_live && (bone.name.find("Finger") != std::string::npos ||
                       (bone.name.size() >= 5 &&
                        bone.name.compare(bone.name.size() - 5, 5, "-Hand") == 0)))
        probe_local[bone.index] = live_rot[bone.index];
    }
    std::vector<Quat> probe_component(skeleton.bones.size());
    for (const auto &bone : skeleton.bones) {
      const Quat parent = bone.parent >= 0
                              ? probe_component[static_cast<std::size_t>(bone.parent)]
                              : Quat{};
      probe_component[bone.index] = Multiply(parent, probe_local[bone.index]);
    }
    double worst = 0.0;
    for (const auto &bone : skeleton.bones) {
      const bool is_hand = bone.name.find("Finger") != std::string::npos ||
                           (bone.name.size() >= 5 &&
                            bone.name.compare(bone.name.size() - 5, 5, "-Hand") == 0);
      if (!is_hand)
        continue;
      const auto entry = source_of_target.find(bone.name);
      if (entry == source_of_target.end())
        continue;
      const Vec3 direction = BoneDirectionLocal(skeleton, bone.index);
      if (Length(direction) < 1e-9)
        continue;
      const auto mmd = reference_mapped_directions.find(entry->second);
      const auto fallback = reference_directions.find(entry->second);
      const Vec3 *mmd_direction = mmd != reference_mapped_directions.end()
                                      ? &mmd->second
                                      : (fallback != reference_directions.end()
                                             ? &fallback->second
                                             : nullptr);
      if (mmd_direction == nullptr)
        continue;
      const Vec3 have = Rotate(probe_component[bone.index], direction);
      const Vec3 want = Rotate(kAxisChange, *mmd_direction);
      worst = (std::max)(worst, AngleBetween(have, want));
    }
    return worst;
  };
  const double live_worst = worst_finger_correction(true);
  const double bind_worst = worst_finger_correction(false);
  const bool use_captured_hand = live_worst <= bind_worst && live_worst <= 90.0;
  if (use_captured_hand) {
    for (const auto &bone : skeleton.bones) {
      const bool is_hand = bone.name.find("Finger") != std::string::npos ||
                           (bone.name.size() >= 5 &&
                            bone.name.compare(bone.name.size() - 5, 5, "-Hand") == 0);
      if (is_hand)
        rest_local[bone.index] = live_rot[bone.index];
    }
    // The captured hand rest changes the component chain everything else builds on. Only
    // the hand chain's entries differ from the current chain, so the rest of the array
    // (with the direction corrections already in it) is kept (mmd2bip.py:814-821).
    for (const auto &bone : skeleton.bones) {
      const bool is_hand = bone.name.find("Finger") != std::string::npos ||
                           (bone.name.size() >= 5 &&
                            bone.name.compare(bone.name.size() - 5, 5, "-Hand") == 0);
      if (!is_hand)
        continue;
      const Quat parent = bone.parent >= 0
                              ? rest_component[static_cast<std::size_t>(bone.parent)]
                              : Quat{};
      rest_component[bone.index] = Multiply(parent, rest_local[bone.index]);
    }
    rest_crot_cap = rest_component;
  }
  // ---- direction correction --------------------------------------------------------
  // A mapped bone's rest rotation is rebuilt so it points where its MMD counterpart
  // points; everything downstream (including every frame's local rotation) is expressed
  // against this pose, so getting it wrong rotates the whole character.
  const auto exempt = [](const std::string &name) {
    return name.find("Finger") != std::string::npos ||
           (name.size() >= 5 && name.compare(name.size() - 5, 5, "-Hand") == 0) ||
           name.find("Foot") != std::string::npos ||
           name.find("Toe") != std::string::npos;
  };
  std::vector<Quat> rest_crot = rest_crot_cap;
  std::uint32_t direction_corrected = 0;
  std::uint32_t direction_skipped = 0;
  for (const auto &bone : skeleton.bones) {
    if (bone.name.empty())
      continue;
    // `crot_cap` (uncorrected) is what a bone's current direction is measured against;
    // the parent frame its corrected local is expressed in is the corrected chain as it
    // stands at this point (mmd2bip.py:833 cp_rest = rest_crot[p], 866).
    const Quat parent_cap = bone.parent >= 0
                                ? rest_crot_cap[static_cast<std::size_t>(bone.parent)]
                                : Quat{};
    const Quat parent_corrected = bone.parent >= 0
                                      ? rest_crot[static_cast<std::size_t>(bone.parent)]
                                      : Quat{};
    if (exempt(bone.name)) {
      rest_crot[bone.index] = Multiply(parent_corrected, rest_local[bone.index]);
      // The hand/foot chains keep the reference pose: every finger tail in the PMX is
      // authored straight along +X and the foot's direction is a marker, so the source's
      // direction is simply the wrong reference there (mmd2bip.py:812, 851).
      ++direction_skipped;
      continue;
    }
    const auto entry_target = source_of_target.find(bone.name);
    if (entry_target == source_of_target.end()) {
      rest_crot[bone.index] = Multiply(parent_corrected, rest_local[bone.index]);
      continue;
    }
    const Vec3 local_direction = BoneDirectionLocal(skeleton, bone.index);
    const Vec3 have = Length(local_direction) > 1e-9
                          ? Rotate(rest_crot_cap[bone.index], local_direction)
                          : Vec3{};
    // The MMD-side direction dictionaries are keyed by MMD bone name, and the entry
    // holds exactly the name this target follows (mmd2bip.py:816).
    const auto mmd = reference_mapped_directions.find(entry_target->second);
    const auto mmd_fallback = reference_directions.find(entry_target->second);
    const Vec3 *mmd_direction = nullptr;
    if (mmd != reference_mapped_directions.end())
      mmd_direction = &mmd->second;
    else if (mmd_fallback != reference_directions.end())
      mmd_direction = &mmd_fallback->second;
    if (Length(have) < 1e-9 || mmd_direction == nullptr) {
      ++direction_skipped;
      continue;
    }
    const Vec3 want = Rotate(kAxisChange, *mmd_direction);
    // A correction this large means the direction reference is wrong, not the character:
    // MMD's toe is a leaf whose PMX tail points forward as a marker, and swinging the toe
    // onto it pulled the toes down by 80 degrees. Fingers sit at 50, the hand at 65 and
    // the pelvis at 18.5, so the cap is well clear of the real cases (mmd2bip.py:824-832).
    if (AngleBetween(have, want) > 90.0) {
      ++direction_skipped;
      continue;
    }
    // The correction swings the *uncorrected* component rotation onto the target
    // direction, and the resulting bone-local rotation is what the pose is built from
    // (mmd2bip.py:872-894).
    const Quat corrected = Multiply(Swing(have, want), rest_crot_cap[bone.index]);
    rest_local[bone.index] = Multiply(Conjugate(parent_corrected), corrected);
    rest_crot[bone.index] = corrected;
    ++direction_corrected;
  }
  // The rigid ("every bone gets a track") pass publishes the bone's *rest* rotation --
  // the same value the mapped rest uses, from the rest source chosen above, not the
  // export's own baseLocal (mmd2bip.py:1417). Publishing baseLocal left 50 of the 144
  // rigid tracks up to 0.155 degrees away: the export's captured pose and the engine's
  // reference pose differ slightly on those bones.

  // ---------------------------------------------------------------- phase two setup
  //
  // Accessory bones (hair, cloth, ribbons) have no usable source data, so they are
  // simulated instead of converted: each one lags behind the pose it would have rigidly,
  // through a spring-damper on its own axis. With no movement the spring settles exactly
  // on the rigid pose, so this can only add life -- it cannot change the silhouette at
  // rest (mmd2bip.py:993-1031).
  const auto is_secondary = secondary::IsSecondary;
  // Hard ornaments stay rigid: a head ring dragged around by the spring is the one thing
  // the spring must never do (mmd2bip.py:HEAD_ORNAMENT_BONES, spec 5.2).
  // Matched case-insensitively, like the reference does (it lowercases the name and every
  // entry). "hat" is 九原's big hat: a hat bone the spring drives waves on top of the head.
  const auto is_ornament = secondary::IsOrnament;
  // Hair sways but does not spin: the spring bends a strand a few degrees and rolls it
  // tens, and the roll is what reads as turning in place (spec 5.1).
  const auto is_swing_only = secondary::IsSwingOnly;
  const auto twist_about = [](const Quat &q, const Vec3 &axis) {
    const double d = q.x * axis.x + q.y * axis.y + q.z * axis.z;
    const double n = std::sqrt(d * d + q.w * q.w);
    if (n < 1e-9)
      return Quat{};
    return Quat{axis.x * d / n, axis.y * d / n, axis.z * d / n, q.w / n};
  };
  // Sleeves are the one accessory that reads as cloth hanging off a stick: the arm carries it and
  // gravity pulls the hem down, so they get a heavier gravity share than hair or a skirt (see
  // kSleeveGravityWeight). The name rule has to exclude 裙 (qun): 残虹's rig has four bones that
  // are both -- Bn_qunFxiuA_L/R and Bn_qunAxiu_L01/R01 -- and all four hang off the *pelvis*,
  // i.e. they are skirt panels. Matching a bare `xiu` would have weighted a skirt like a sleeve;
  // 浔's rig has no such name, and neither one has an `xiu` chain under the pelvis after the
  // exclusion (54/54 and 71/75 bones, all hanging off the arm).
  const auto is_sleeve = secondary::IsSleeve;
  const auto is_hair = secondary::IsHair;
  struct SecondaryBone {
    std::uint32_t index = 0;
    std::string name;
    int parent = -1;
    Vec3 axis;      // child offset direction, in the bone's own local frame
    double length = 0.0;
    Quat local;     // rigid (rest) local rotation
    std::uint8_t collide = 0;  // torso / legs / head / arms bit mask
    Vec3 translation;
    secondary::MotionMaterial material = secondary::MotionMaterial::General;
    secondary::MotionResponse response =
        secondary::ResponseFor(secondary::MotionMaterial::General);
    secondary::SpringSettings spring = secondary::SpringFor(false);
    double collision_radius = 0.0;
    double motion_follow = 1.0;
    Vec3 native_gravity{};
    double forward_move_offset = 0.0;
  };
  // Rebuilt once per frame from the rig's bones (see the frame loop).
  struct StrandCapsule {
    Vec3 a;
    Vec3 b;
    double radius = 0.0;
    double end_radius = 0.0;
    std::uint8_t mask = 0;
  };
  std::vector<StrandCapsule> strand_capsules;
  std::vector<SecondaryBone> secondary;
  std::vector<Vec3> sim_direction(skeleton.bones.size());
  std::vector<Vec3> sim_velocity(skeleton.bones.size());
  std::vector<Vec3> sim_position(skeleton.bones.size());
  std::vector<Vec3> sim_position_older(skeleton.bones.size());
  std::vector<bool> sim_seeded(skeleton.bones.size(), false);
  // Collision hysteresis: a strand that was pushed out last frame keeps being pushed until it is
  // comfortably clear, instead of turning the constraint on and off at the slack boundary.
  std::vector<bool> collision_held(skeleton.bones.size(), false);
  std::vector<std::uint32_t> sim_frames(skeleton.bones.size(), 0);
  std::uint32_t rigid_accessories = 0;
  std::uint32_t sleeve_bones = 0;
  std::uint32_t native_secondary_overrides = 0;
  std::uint32_t strand_collisions = 0;
  // The deepest a strand still sits inside its proxy after the correction, per proxy set, over the
  // whole conversion -- in the converter's own geometry (see the note at the collision pass).
  double worst_strand_torso_cm = 0.0;
  double worst_strand_legs_cm = 0.0;
  double worst_strand_head_cm = 0.0;
  double worst_strand_arms_cm = 0.0;
  const auto valid_secondary_override = [](const SecondaryBoneOverride &value) {
    return !value.name.empty() && std::isfinite(value.spring.stiffness) &&
           std::isfinite(value.spring.damping) &&
           std::isfinite(value.spring.lag_degrees) &&
           value.spring.stiffness >= 0.0 && value.spring.damping >= 0.0 &&
           value.spring.stiffness <= 1000000.0 &&
           value.spring.damping <= 1000.0 &&
           value.spring.lag_degrees >= 0.0 &&
           value.spring.lag_degrees <= 180.0 &&
           std::isfinite(value.motion_follow) &&
           std::isfinite(value.forward_move_offset) &&
           std::all_of(value.native_gravity.begin(), value.native_gravity.end(),
                       [](const double component) {
                         return std::isfinite(component);
                       });
  };
  std::map<std::string, const SecondaryBoneOverride *> secondary_overrides;
  for (const SecondaryBoneOverride &value : input.secondary_bone_overrides)
    if (valid_secondary_override(value))
      secondary_overrides[value.name] = &value;
  for (const auto &bone : skeleton.bones) {
    if (bone.name.empty() || mapped_targets.count(bone.name) != 0)
      continue;
    if (!is_secondary(bone.name) || is_ornament(bone.name))
      continue;
    // Only a bone with a child offset has an axis to spring about; a leaf (a hair tip, a
    // ring) is left to the rigid pass (mmd2bip.py:1079-1087).
    // The reference resolves the bone by *name* before asking for its children, and this rig
    // has duplicate names (a bone and its child both called Bn_l_xiu). A name-keyed map keeps
    // the last one, so the first duplicate has no children there and stays rigid --
    // simulating it by index put that chain tenths of a rotation away from the reference.
    std::uint32_t owner = bone.index;
    {
      const auto resolved = skeleton.by_name.find(bone.name);
      if (resolved != skeleton.by_name.end())
        owner = skeleton.bones[resolved->second].index;
    }
    std::size_t best = 0;
    double best_length = 0.0;
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
      if (skeleton.bones[i].parent != static_cast<int>(owner))
        continue;
      const double length = Length(skeleton.bones[i].base_translation);
      if (length > best_length) {
        best = i;
        best_length = length;
      }
    }
    if (best_length < 1e-3)
      continue;
    SecondaryBone entry;
    entry.index = bone.index;
    entry.name = bone.name;
    entry.parent = bone.parent;
    entry.axis = Unit(skeleton.bones[best].base_translation);
    entry.length = best_length;
    entry.local = rest_local[bone.index];
    entry.translation = bone.base_translation;
    entry.material = secondary::ClassifyMotionMaterial(bone.name);
    entry.response = secondary::ResponseFor(entry.material);
    const bool swing_only = is_swing_only(bone.name);
    entry.spring = secondary::SpringFor(swing_only);
    const auto native = secondary_overrides.find(bone.name);
    if (native != secondary_overrides.end()) {
      const SecondaryBoneOverride &value = *native->second;
      entry.spring = {std::clamp(value.spring.stiffness, 0.0, 1000000.0),
                      std::clamp(value.spring.damping, 0.0, 1000.0),
                      std::clamp(value.spring.lag_degrees, 0.0, 180.0)};
      entry.collision_radius =
          std::clamp(value.collision_radius, 0.0, 100.0);
      entry.motion_follow = std::clamp(value.motion_follow, 0.0, 1.0);
      entry.native_gravity = {value.native_gravity[0],
                              value.native_gravity[1],
                              value.native_gravity[2]};
      entry.forward_move_offset =
          std::clamp(value.forward_move_offset, -1.0, 1.0);
      ++native_secondary_overrides;
    }
    // Which volume this strand has to stay out of. Hair is named; everything else that hangs
    // directly off the pelvis is structural -- a skirt, or the waist streamers 浔 has instead of
    // one (残虹: 110 `qun` bones; 浔: 0). Writing the rule on the hang point rather than the name
    // is what makes it work on both rigs.
    if (is_hair(bone.name)) {
      entry.collide |= kCollideTorso;
      if (input.dynamics_tuning.extended_collision)
        entry.collide |= kCollideHead | kCollideArms;
    }
    {
      std::string ancestor;
      for (int up = bone.parent; up >= 0; up = skeleton.bones[static_cast<std::size_t>(up)].parent) {
        const std::string &parent_name = skeleton.bones[static_cast<std::size_t>(up)].name;
        if (parent_name.rfind("Bn_", 0) == 0)
          continue;
        ancestor = parent_name;
        break;
      }
      if (secondary::IsLegAttachment(ancestor))
        entry.collide |= kCollideLegs;
      if (input.dynamics_tuning.extended_collision &&
          secondary::IsHeadAttachment(ancestor))
        entry.collide |= kCollideHead;
      if (input.dynamics_tuning.extended_collision &&
          secondary::IsArmAttachment(ancestor) && !is_sleeve(bone.name))
        entry.collide |= kCollideArms;
    }
    entry.collide = secondary::EffectiveCollisionMask(
        entry.collide, input.dynamics_tuning.extended_collision);
    secondary.push_back(entry);
    ++rigid_accessories;
    if (is_sleeve(bone.name))
      ++sleeve_bones;
  }
  std::vector<const SecondaryBone *> secondary_by_index(skeleton.bones.size(), nullptr);
  for (const auto &entry : secondary)
    secondary_by_index[entry.index] = &entry;
  std::vector<bone_constraints::Candidate> distance_candidates;
  std::vector<bone_constraints::Link> distance_links;
  std::vector<bone_constraints::Link> self_links;
  std::size_t self_candidate_pairs = 0;
  std::uint32_t distance_corrections = 0;
  std::vector<std::uint8_t> self_contact_state;
  std::uint32_t self_collision_corrections = 0;
  const std::uint32_t ribbon_bones = static_cast<std::uint32_t>(
      std::count_if(secondary.begin(), secondary.end(),
                    [](const SecondaryBone &entry) {
                      return entry.material ==
                             secondary::MotionMaterial::Ribbon;
                    }));
  std::vector<Vec3> ribbon_previous_direction(skeleton.bones.size());
  std::vector<Vec3> ribbon_previous_motion(skeleton.bones.size());
  std::vector<bool> ribbon_motion_ready(skeleton.bones.size(), false);
  std::vector<std::uint32_t> ribbon_chain_length(skeleton.bones.size(), 1);
  std::vector<std::uint32_t> ribbon_chain_size(skeleton.bones.size(), 0);
  for (const SecondaryBone &spring : secondary) {
    if (spring.material != secondary::MotionMaterial::Ribbon)
      continue;
    std::size_t root = spring.index;
    for (int parent = spring.parent;
         parent >= 0 &&
         secondary_by_index[static_cast<std::size_t>(parent)] != nullptr;
         parent =
             secondary_by_index[static_cast<std::size_t>(parent)]->parent)
      root = static_cast<std::size_t>(parent);
    ++ribbon_chain_size[root];
  }
  for (const SecondaryBone &spring : secondary) {
    if (spring.material != secondary::MotionMaterial::Ribbon)
      continue;
    std::size_t root = spring.index;
    for (int parent = spring.parent;
         parent >= 0 &&
         secondary_by_index[static_cast<std::size_t>(parent)] != nullptr;
         parent =
             secondary_by_index[static_cast<std::size_t>(parent)]->parent)
      root = static_cast<std::size_t>(parent);
    ribbon_chain_length[spring.index] =
        (std::max)(1U, ribbon_chain_size[root]);
  }
  std::uint64_t ribbon_reversals = 0;
  double ribbon_max_frame_degrees = 0.0;
  double ribbon_max_frame = 0.0;
  std::string ribbon_max_bone;
  if (input.dynamics_tuning.bone_constraints ||
      input.dynamics_tuning.self_collision) {
    std::vector<Quat> distance_rest_rot(skeleton.bones.size());
    std::vector<Vec3> distance_rest_pos(skeleton.bones.size());
    for (const auto &bone : skeleton.bones) {
      const Quat parent_rotation =
          bone.parent >= 0
              ? distance_rest_rot[static_cast<std::size_t>(bone.parent)]
              : Quat{};
      distance_rest_rot[bone.index] =
          Multiply(parent_rotation, rest_local[bone.index]);
      const Vec3 offset =
          Rotate(parent_rotation, bone.base_translation);
      distance_rest_pos[bone.index] =
          bone.parent >= 0
              ? distance_rest_pos[static_cast<std::size_t>(bone.parent)] +
                    offset
              : offset;
    }
    distance_candidates.assign(skeleton.bones.size(), {});
    std::vector<Vec3> body_references;
    body_references.reserve(8);
    for (const auto &bone : skeleton.bones) {
      if (bone.name.find("Pelvis") != std::string::npos ||
          bone.name.find("Spine") != std::string::npos ||
          bone.name.find("Neck") != std::string::npos ||
          bone.name.find("Head") != std::string::npos)
        body_references.push_back(distance_rest_pos[bone.index]);
    }
    for (const SecondaryBone &spring : secondary) {
      std::size_t root = spring.index;
      std::uint32_t depth{};
      for (int parent = spring.parent;
           parent >= 0 &&
           secondary_by_index[static_cast<std::size_t>(parent)] != nullptr;
           parent =
               secondary_by_index[static_cast<std::size_t>(parent)]->parent) {
        root = static_cast<std::size_t>(parent);
        ++depth;
      }
      const Vec3 head = distance_rest_pos[spring.index];
      const Vec3 tip =
          head + Rotate(distance_rest_rot[spring.index], spring.axis) *
                     spring.length;
      auto &candidate = distance_candidates[spring.index];
      candidate.head = {head.x, head.y, head.z};
      candidate.tip = {tip.x, tip.y, tip.z};
      candidate.length = spring.length;
      candidate.anchor =
          skeleton.bones[root].parent;
      candidate.chain = static_cast<std::uint32_t>(root);
      candidate.depth = depth;
      candidate.material =
          static_cast<std::uint32_t>(spring.material);
      candidate.collision_mask =
          secondary::SelfCollisionMask(
              spring.material, input.dynamics_tuning.self_collision_pairs);
      candidate.radius =
          spring.collision_radius *
          input.dynamics_tuning.self_collision_radius[
              secondary::MotionMaterialIndex(spring.material)];
      const Vec3 midpoint{
          (candidate.head.x + candidate.tip.x) * 0.5,
          (candidate.head.y + candidate.tip.y) * 0.5,
          (candidate.head.z + candidate.tip.z) * 0.5};
      double layer_depth = std::numeric_limits<double>::infinity();
      for (const Vec3 core_reference : body_references)
        layer_depth =
            (std::min)(layer_depth, Length(midpoint - core_reference));
      if (!std::isfinite(layer_depth)) {
        const Vec3 anchor =
            skeleton.bones[root].parent >= 0
                ? distance_rest_pos[static_cast<std::size_t>(
                      skeleton.bones[root].parent)]
                : Vec3{};
        layer_depth = Length(midpoint - anchor);
      }
      candidate.layer_depth = layer_depth;
      candidate.eligible = true;
    }
    distance_links = bone_constraints::Discover(distance_candidates);
    self_links =
        bone_constraints::DiscoverSelfCollision(
            distance_candidates, 4, &self_candidate_pairs);
    for (auto &link : self_links) {
      const auto material_a =
          static_cast<secondary::MotionMaterial>(
              distance_candidates[link.a].material);
      const auto material_b =
          static_cast<secondary::MotionMaterial>(
              distance_candidates[link.b].material);
      const auto index_a = secondary::MotionMaterialIndex(material_a);
      const auto index_b = secondary::MotionMaterialIndex(material_b);
      link.correction_gain =
          std::clamp(
              0.22 * std::sqrt(
                         input.dynamics_tuning.self_collision_strength[index_a] *
                         input.dynamics_tuning.self_collision_strength[index_b]),
              0.02, 0.7);
      link.velocity_keep =
          std::clamp(
              0.5 / std::sqrt(
                        input.dynamics_tuning.self_collision_damping[index_a] *
                        input.dynamics_tuning.self_collision_damping[index_b]),
              0.03, 0.95);
      if (!input.dynamics_tuning.self_collision_outer_priority) {
        link.correction_share_a = 0.5;
        link.correction_share_b = 0.5;
      }
    }
    self_contact_state.assign(self_links.size(), 0);
  }

  // ---- leg volume clearance, measured from the rig's own rest pose -------------------
  //
  // How far a skirt hangs from the leg is a property of the model, not a number to guess: at rest
  // nothing intersects the legs, so the tightest clearance the rig's own skirt samples sit at is a
  // clearance the cloth demonstrably can hold. Every leg capsule below takes that as its radius, so
  // the correction can never hold the cloth deeper inside the leg than the author's own pose does --
  // and on a skirt that hangs far out, the same rule simply reproduces its own spacing.
  //
  // The two volumes overlap when a rig's own clearance exceeds half the hip separation (浔: 9.26 cm
  // against 8.8), and the constraint is then only approximated between the legs. That is still the
  // better trade: measured against a 7.5 cm leg, the rig's own clearance leaves 残虹 with nothing
  // inside the leg at all (0 of 150 frames), where capping the radius at half the separation to make
  // the constraint exactly satisfiable leaves 23 samples inside on 7 frames. On 浔 the cap wins one
  // chain (0.71 cm on 3 frames) but loses the deepest one (1.24 cm on 1 frame).
  using secondary::kLegVolumes;
  // 0.0 = no leg strand on this rig, which leaves the volume out entirely.
  double leg_radius_cm[4] = {0.0, 0.0, 0.0, 0.0};
  double leg_clearance_cm = 0.0;   // the tightest of the four, for the report
  {
    std::vector<Quat> rest_rot(skeleton.bones.size());
    std::vector<Vec3> rest_pos(skeleton.bones.size());
    for (const auto &bone : skeleton.bones) {
      const Quat parent_rotation =
          bone.parent >= 0 ? rest_rot[static_cast<std::size_t>(bone.parent)] : Quat{};
      rest_rot[bone.index] = Multiply(parent_rotation, rest_local[bone.index]);
      const Vec3 offset = Rotate(parent_rotation, bone.base_translation);
      rest_pos[bone.index] = bone.parent >= 0
                                 ? rest_pos[static_cast<std::size_t>(bone.parent)] + offset
                                 : offset;
    }
    // A cap at half the hip separation keeps the two volumes from overlapping. That matters because
    // the corridor between the legs is what a front or back panel hangs in: with overlapping volumes
    // no direction can satisfy both, and the solver trades the panel between the two capsules --
    // measured on 残虹's skirt chains, 30% of frames reversed direction against 20% on the volumes'
    // own baseline (the sleeves, which the collision never touches). Capped, those chains sit at
    // 20-24%, i.e. indistinguishable from untouched cloth, at the cost of a handful of frames where a
    // strand rooted on the hip still sits ~1 cm inside (浔 3 of 150 sampled frames, 残虹 6; the head
    // of such a chain cannot be moved by any rotation).
    double separation_cap_cm = 0.0;
    {
      const auto left = skeleton.by_name.find("Bip001-L-Thigh");
      const auto right = skeleton.by_name.find("Bip001-R-Thigh");
      if (left != skeleton.by_name.end() && right != skeleton.by_name.end()) {
        const Vec3 &a = rest_pos[left->second];
        const Vec3 &b = rest_pos[right->second];
        separation_cap_cm = 0.5 * Length(Vec3{a.x - b.x, a.y - b.y, a.z - b.z});
      }
    }
    for (std::size_t volume = 0; volume < 4; ++volume) {
      const auto first = skeleton.by_name.find(kLegVolumes[volume].first);
      const auto second = skeleton.by_name.find(kLegVolumes[volume].second);
      if (first == skeleton.by_name.end() || second == skeleton.by_name.end())
        continue;
      const Vec3 &joint = rest_pos[first->second];
      const Vec3 &end = rest_pos[second->second];
      const Vec3 start{joint.x + (end.x - joint.x) * kLegVolumes[volume].fraction,
                       joint.y + (end.y - joint.y) * kLegVolumes[volume].fraction,
                       joint.z + (end.z - joint.z) * kLegVolumes[volume].fraction};
      const Vec3 axis{end.x - start.x, end.y - start.y, end.z - start.z};
      const double axis_squared = Dot(axis, axis);
      double tightest = (std::numeric_limits<double>::infinity)();
      for (const SecondaryBone &entry : secondary) {
        if ((entry.collide & kCollideLegs) == 0)
          continue;
        const Vec3 &head = rest_pos[entry.index];
        const Vec3 direction = Rotate(rest_rot[entry.index], entry.axis);
        for (const double slot : {0.5, 0.75, 1.0}) {
          const Vec3 point{head.x + direction.x * entry.length * slot,
                           head.y + direction.y * entry.length * slot,
                           head.z + direction.z * entry.length * slot};
          const Vec3 to_point{point.x - start.x, point.y - start.y, point.z - start.z};
          const double t = axis_squared > 1e-9
                               ? (std::max)(0.0, (std::min)(1.0, Dot(to_point, axis) / axis_squared))
                               : 0.0;
          const double distance =
              Length(Vec3{point.x - (start.x + axis.x * t), point.y - (start.y + axis.y * t),
                          point.z - (start.z + axis.z * t)});
          tightest = (std::min)(tightest, distance);
        }
      }
      if (tightest < 1e6 && tightest > 0.5) {
        const double radius =
            separation_cap_cm > 0.0 ? (std::min)(tightest, separation_cap_cm) : tightest;
        leg_radius_cm[volume] = radius;
        leg_clearance_cm =
            leg_clearance_cm > 0.0 ? (std::min)(leg_clearance_cm, radius) : radius;
      }    }
  }
  // ---- twist helpers (mmd2bip.py:build_twist_list) ---------------------------------
  // `Bone-L-ForeArm-Twist` and its fifteen siblings exist to spread a joint's twist
  // along the mesh, and they carry no source data of their own. Left rigid they make an
  // elbow or knee deform wrongly once the limb rotates a lot; each one instead takes a
  // share of its limb bone's twist, about the limb axis.
  struct TwistBone {
    std::uint32_t index = 0;
    std::string name;
    int parent = -1;
    std::uint32_t arm_index = 0;
    Vec3 axis;      // the limb bone's own child offset direction, in its local frame
    double weight = 0.5;
    std::string source_twist;
    Vec3 source_axis;
  };
  std::vector<TwistBone> twist_bones;
  for (const auto &bone : skeleton.bones) {
    const std::string &name = bone.name;
    std::size_t cursor = std::string::npos;
    if (name.rfind("Bone-", 0) == 0)
      cursor = 5;
    else if (name.rfind("Bip001-", 0) == 0)
      cursor = 7;
    if (cursor == std::string::npos || cursor + 2 > name.size() ||
        name[cursor + 1] != '-')
      continue;
    const char side = name[cursor];
    if (side != 'L' && side != 'R')
      continue;
    cursor += 2;
    const char *limb_mmd = nullptr;
    std::size_t limb_length = 0;
    for (const auto &limb : {std::pair<const char *, const char *>{"UpperArm", "腕"},
                             {"ForeArm", "ひじ"}, {"Thigh", "足"},
                             {"Calf", "ひざ"}}) {
      const std::size_t length = std::strlen(limb.first);
      if (name.compare(cursor, length, limb.first) == 0) {
        limb_mmd = limb.second;
        limb_length = length;
        break;
      }
    }
    if (limb_mmd == nullptr)
      continue;
    cursor += limb_length;
    if (name.compare(cursor, 6, "-Twist") != 0)
      continue;
    cursor += 6;
    const bool second = cursor < name.size() && name[cursor] == '1';
    if (second)
      ++cursor;
    if (cursor != name.size())
      continue;
    const std::string source = std::string(side == 'L' ? "左" : "右") + limb_mmd;
    // The limb bone this helper follows, through the mapping table. The reference keeps
    // the *last* entry for a source, so keep scanning instead of stopping at the first.
    std::string arm_target;
    for (const auto &entry : table) {
      for (const auto &candidate : entry.sources) {
        if (candidate == source)
          arm_target = entry.target;
      }
    }
    if (arm_target.empty())
      continue;
    const auto arm = skeleton.by_name.find(arm_target);
    if (arm == skeleton.by_name.end())
      continue;
    const Vec3 axis = BoneDirectionLocal(skeleton, arm->second);
    if (Length(axis) < 1e-9)
      continue;
    TwistBone item;
    item.index = bone.index;
    item.name = name;
    item.parent = bone.parent;
    item.arm_index = static_cast<std::uint32_t>(arm->second);
    item.axis = axis;
    // Twist takes half of the limb twist, Twist1 a quarter, so the two of them spread
    // three quarters along the limb (mmd2bip.py:374-376).
    item.weight = second ? 0.25 : 0.5;
    if (std::string(limb_mmd) == "腕")
      item.source_twist = std::string(side == 'L' ? "左" : "右") + "腕捩";
    else if (std::string(limb_mmd) == "ひじ")
      item.source_twist = std::string(side == 'L' ? "左" : "右") + "手捩";
    if (!item.source_twist.empty()) {
      const auto mapped = reference_mapped_directions.find(item.source_twist);
      const auto fallback = reference_directions.find(item.source_twist);
      if (mapped != reference_mapped_directions.end())
        item.source_axis = mapped->second;
      else if (fallback != reference_directions.end())
        item.source_axis = fallback->second;
    }
    twist_bones.push_back(item);
  }
  const double dt = input.fps > 0.0 ? 1.0 / input.fps : 1.0 / 30.0;
  const auto dynamics_tuning =
      secondary::SafeDynamicsTuning(input.dynamics_tuning);

  std::map<std::string, std::vector<json>> output;
  std::vector<json> output_positions;
  output_positions.reserve(frames.size());
  struct ArmIkOutput {
    int side = 0;
    std::string hand;
    std::vector<Vec3> offsets;
    std::vector<Quat> rotations;
    float max_bend_degrees = 150.0F;
    bool follow_wrist = false;
  };
  std::map<int, ArmIkOutput> arm_ik_outputs;
  for (const ManualArmIkControl &control : input.manual_arm_ik_controls) {
    if (control.source.empty() || control.side < 0 || control.side > 1 ||
        !std::isfinite(control.gain) || control.gain < 0.1F ||
        control.gain > 3.0F || !std::isfinite(control.max_bend_degrees) ||
        control.max_bend_degrees < 20.0F ||
        control.max_bend_degrees > 170.0F)
      continue;
    for (int side = control.side; side <= (control.paired ? 1 : control.side);
         ++side) {
      const std::string hand =
          std::string("Bip001-") + (side == 0 ? "L-Hand" : "R-Hand");
      if (skeleton.by_name.count(hand) == 0)
        continue;
      auto &target = arm_ik_outputs[side];
      target.side = side;
      target.hand = hand;
      target.max_bend_degrees =
          (std::min)(target.max_bend_degrees,
                     std::clamp(control.max_bend_degrees, 20.0F, 170.0F));
      target.follow_wrist = target.follow_wrist || control.follow_wrist;
    }
  }
  // The target's one pelvis bone plays *both* MMD 腰 (torso) and 下半身 (hips), so one of the
  // two offsets that hang off it is wrong unless corrected per frame. Keeping 下半身 on the
  // pelvis keeps the hips and the butt right; the spine's base then has to be counter-rotated
  // back, which is a per-frame *translation* (the rotation alone cannot move a bone's origin).
  // boneOffsets carries those absolute local translations in centimetres.
  using OffsetBone = struct {
    std::string name;
    std::size_t index;
    bool negative_hip;  // true = multiply the 下半身 delta *conjugated* (spine); false = as-is
    Vec3 world_shift;   // extra constant *world* displacement added to this bone's base (cm)
  };
  std::vector<OffsetBone> offset_bones;
  for (const char *name : {"Bip001-Spine"}) {
    const auto found = skeleton.by_name.find(name);
    if (found != skeleton.by_name.end())
      offset_bones.push_back(OffsetBone{name, found->second, true, Vec3{}});
  }
  if (!input.hip_on_pelvis) {
    for (const char *name : {"Bip001-L-Thigh", "Bip001-R-Thigh"}) {
      const auto found = skeleton.by_name.find(name);
      if (found != skeleton.by_name.end())
        offset_bones.push_back(OffsetBone{name, found->second, false, Vec3{}});
    }
  }
  std::map<std::string, std::vector<json>> output_offsets;
  for (const auto &[target_name, entry] : mapped_targets) {
    if (std::any_of(entry->parents.begin(), entry->parents.end(),
                    [](const ManualParentMapping &mapping) {
                      return mapping.inherit_position;
                    }))
      output_offsets[target_name];
  }
  const std::size_t pelvis_index = skeleton.by_name.count("Bip001-Pelvis") != 0
                                       ? skeleton.by_name.at("Bip001-Pelvis")
                                       : skeleton.bones.size();
  const auto hip_source = ik_index_by_name.count("下半身") != 0
                              ? std::optional<std::size_t>{ik_index_by_name.at("下半身")}
                              : std::nullopt;
  // Which MMD bones carry the root motion. The target's root link (Bip001, mapped from
  // センター) absorbs the whole MMD root chain -- 全ての親 -> センター -> グルーブ -> 腰 ->
  // 下半身 -- and the pelvis the target hangs off that link never moves its own local
  // offset, so *every* layer's translation belongs in the emitted root translation. Taking
  // センター's own translation dropped the layers below it: 3.95 mmd units (38 cm, mostly
  // vertical) on Chu-Chu-U-Chu and 27.6 on 主角, while the legs -- which do see the whole
  // chain, through the IK solve -- bent for a lift the body never made, so the character
  // slid instead of rising. The deepest bone that actually animates a position ends the
  // chain: a motion driving its travel through 下半身 (TDA_30fps) is a real file, and
  // stopping at センター would drop it entirely.
  std::vector<std::pair<std::string, std::uint32_t>> named_chain;
  for (const char *name : {"全ての親", "センター", "グルーブ", "腰", "下半身"}) {
    const ReferenceBone *bone = FindReference(reference, name);
    if (bone != nullptr)
      named_chain.emplace_back(name, bone->index);
  }
  std::size_t centre_slot = 0;
  for (std::size_t slot = 0; slot < named_chain.size(); ++slot) {
    if (named_chain[slot].first == "センター") {
      centre_slot = slot;
      break;
    }
  }
  const auto position_span = [](const VmdTrack &track) {
    if (track.keys.size() < 2)
      return 0.0;
    double span = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
      double low = track.keys.front().position[axis];
      double high = low;
      for (const auto &key : track.keys) {
        low = (std::min)(low, key.position[axis]);
        high = (std::max)(high, key.position[axis]);
      }
      span = (std::max)(span, high - low);
    }
    return span;
  };
  const auto position_span_of = [&](const std::string &name) {
    const VmdTrack *track = find_track(name);
    return track == nullptr ? 0.0 : position_span(*track);
  };
  std::size_t root_end = centre_slot;
  const auto deepest_with_travel = [&](std::initializer_list<const char *> names) {
    std::size_t slot = named_chain.size();
    for (const char *name : names) {
      const VmdTrack *track = find_track(name);
      if (track == nullptr || position_span(*track) <= 1e-6)
        continue;
      for (std::size_t i = 0; i < named_chain.size(); ++i) {
        if (named_chain[i].first == name)
          slot = i;
      }
    }
    return slot;
  };
  // Placement layers (全ての親/センター/グルーブ) say where the character *stands*; 腰 and 下半身
  // say where the hips sit relative to the planted feet. Only the former may move the whole
  // character: folding the hip layer in drags the feet along (rigoutput.vmd: 43 cm of
  // vertical hip sway became a global lift, so the feet floated). The hip layer is still the
  // answer when nothing above it moves -- a TDA export drives its travel through 下半身.
  std::size_t placement_end = deepest_with_travel({"全ての親", "センター", "グルーブ"});
  if (placement_end == named_chain.size())
    placement_end = deepest_with_travel({"腰", "下半身"});
  if (placement_end != named_chain.size())
    root_end = placement_end;
  // ...but the reference is the bone that *is* the target's pelvis (`Bip001-Pelvis <- 腰`),
  // not the topmost moving layer: 腰 hangs below グルーブ, so a センター rotation swings it on
  // its own lever. Emitting センター's own world position left rigoutput.vmd 68.8 cm short
  // (measured independently by feet_check.py) -- the pelvis stayed on the old arc while the
  // body pivoted. The hip layer is only used when the whole pelvis chain is static.
  {
    std::string pelvis_source;
    for (const auto &entry : table) {
      if (entry.target == "Bip001-Pelvis" && !entry.sources.empty()) {
        pelvis_source = entry.sources.front();
        break;
      }
    }
    bool upper_moves = false;
    for (const char *name : {"全ての親", "センター", "グルーブ"}) {
      if (position_span_of(name) > 1e-6)
        upper_moves = true;
      const VmdTrack *track = find_track(name);
      if (track == nullptr)
        continue;
      for (const auto &key : track->keys) {
        if (std::fabs(key.rotation[0]) > 1e-6 || std::fabs(key.rotation[1]) > 1e-6 ||
            std::fabs(key.rotation[2]) > 1e-6)
          upper_moves = true;
      }
    }
    const bool pelvis_moves = pelvis_source == "腰" || pelvis_source == "下半身";
    if (pelvis_moves && (upper_moves || position_span_of(pelvis_source) > 1e-6)) {
      for (std::size_t slot = 0; slot < named_chain.size(); ++slot) {
        if (named_chain[slot].first == pelvis_source) {
          root_end = slot;
          break;
        }
      }
    }
  }
  std::vector<std::uint32_t> root_chain;
  for (std::size_t slot = 0; slot <= root_end && slot < named_chain.size(); ++slot)
    root_chain.push_back(named_chain[slot].second);
  std::string root_track_name =
      named_chain.empty() ? std::string() : named_chain[root_end].first;
  const bool root_from_data = root_track_name != "センター";
  // Which of the standard bones this motion does not carry, so a half-driven character is
  // explained instead of guessed at.
  std::string missing_bones;
  {
    std::unordered_map<std::string, bool> present_in_vmd;
    for (const auto &track : vmd.tracks)
      present_in_vmd[CanonicalBoneName(track.name)] = true;
    for (const char *required : {"センター", "グルーブ", "腰", "下半身", "上半身",
                                 "上半身2", "首", "頭", "左肩", "左腕", "左ひじ",
                                 "左手首", "左足", "左ひざ", "左足首", "左つま先",
                                 "右足", "右ひざ", "右足首"}) {
      if (present_in_vmd.count(CanonicalBoneName(required)) == 0) {
        if (!missing_bones.empty())
          missing_bones += ",";
        missing_bones += required;
      }
    }
  }
  const auto root_mapped = mapped_targets.count("Bip001") != 0;
  // The root translation is a delta against the first frame, so that frame's reference
  // position is the anchor. Read once, on the first frame, rather than from a running
  // "have I seen one" flag, which would make the output depend on call history.
  Vec3 root_base{};
  bool have_root_base = false;
  Vec3 root_bump_base{};
  bool have_root_bump_base = false;
  // The target's own root -> pelvis offset, when the pelvis is the root's direct child: the
  // mapped root rotation already swings it, so it must be subtracted from the emission.
  const Vec3 *root_lever = nullptr;
  {
    const auto pelvis = skeleton.by_name.find("Bip001-Pelvis");
    if (pelvis != skeleton.by_name.end()) {
      const Bone &bone = skeleton.bones[pelvis->second];
      if (bone.parent >= 0 &&
          skeleton.bones[static_cast<std::size_t>(bone.parent)].name == "Bip001")
        root_lever = &bone.base_translation;
    }
  }

  // Unit scale and the source leg length. The runtime divides by `mmdLegLength` to scale
  // the root motion for whatever character is playing it (mmd2bip.py:1163-1178); both
  // sides use *segment* lengths, so the ratio does not depend on the pose the export was
  // captured in. This is not optional: without the field the plugin assumes the root
  // values are already centimetres and never scales them, which drops the character into
  // the floor. The scale is also what converts the centre track's own position into
  // centimetres for the root bone below, so it has to be known before the frame loop.
  double mmd_leg_length = 0.0;
  double unit_scale = 0.0;
  if (root_mapped) {
    const auto distance = [&reference](const char *from, const char *from_fallback,
                                       const char *to, const char *to_fallback) {
      const ReferenceBone *a = FindReference(reference, from);
      if (a == nullptr && from_fallback != nullptr)
        a = FindReference(reference, from_fallback);
      const ReferenceBone *b = FindReference(reference, to);
      if (b == nullptr && to_fallback != nullptr)
        b = FindReference(reference, to_fallback);
      if (a == nullptr || b == nullptr)
        return 0.0;
      return Length(Vec3{b->position.x - a->position.x, b->position.y - a->position.y,
                         b->position.z - a->position.z});
    };
    mmd_leg_length = distance("左足", "左足D", "左ひざ", "左ひざD") +
                     distance("左ひざ", "左ひざD", "左足首", "左足首D");
    double target_leg = 0.0;
    // The thigh bone's own translation is the hip's *lateral* offset on this rig
    // (Bip001-L-Thigh rest is ~7.7 cm sideways, not a leg segment), so it must not count
    // toward the leg length: adding it inflated the scale by ~10% and lifted the ankles
    // ~13 cm above the source's, which reads as the legs slanting and the body sitting low.
    for (const char *name : {"Bip001-L-Calf", "Bip001-L-Foot"}) {
      const auto found = skeleton.by_name.find(name);
      if (found == skeleton.by_name.end())
        continue;
      target_leg += Length(skeleton.bones[found->second].base_translation);
    }
    unit_scale = mmd_leg_length > 1e-6 ? target_leg / mmd_leg_length : 8.0;
  }
  // The root bone's own translation carries the character's travel: the centre track's
  // *absolute* position, scaled to centimetres and rotated out of MMD axes. Every child
  // offset is measured from it, and the accessory spring reads those positions for its
  // acceleration term -- leaving it at the rest translation made the position chain
  // stand still, so the spring never saw the body accelerate and a skirt or a hair chain
  // simply did not move (mmd2bip.py:1244-1246).
  const std::size_t root_index =
      root_mapped ? skeleton.by_name.at("Bip001") : skeleton.bones.size();
  const Vec3 root_rest_translation =
      root_index < skeleton.bones.size() ? skeleton.bones[root_index].base_translation
                                         : Vec3{};
  std::vector<Vec3> rest_component_position(skeleton.bones.size());
  for (const auto &bone : skeleton.bones) {
    if (bone.parent < 0) {
      rest_component_position[bone.index] = bone.base_translation;
      continue;
    }
    const std::size_t parent = static_cast<std::size_t>(bone.parent);
    const Vec3 offset = Rotate(rest_crot[parent], bone.base_translation);
    const Vec3 &parent_position = rest_component_position[parent];
    rest_component_position[bone.index] =
        Vec3{parent_position.x + offset.x, parent_position.y + offset.y,
             parent_position.z + offset.z};
  }
  std::array<Vec3, 2> palm_normals{Vec3{0.0, 1.0, 0.0},
                                   Vec3{0.0, 1.0, 0.0}};
  for (int side{}; side != 2; ++side) {
    const std::string prefix =
        std::string("Bip001-") + (side == 0 ? "L-" : "R-");
    const auto position_of = [&](const std::string &suffix)
        -> std::optional<Vec3> {
      const auto found = skeleton.by_name.find(prefix + suffix);
      return found == skeleton.by_name.end()
                 ? std::nullopt
                 : std::optional<Vec3>(
                       rest_component_position[found->second]);
    };
    const auto hand = position_of("Hand");
    const auto middle = position_of("Finger2");
    const auto index = position_of("Finger1");
    const auto pinky = position_of("Finger4");
    if (!hand || (!middle && !index))
      continue;
    const auto subtract = [](const Vec3 &a, const Vec3 &b) {
      return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
    };
    const auto cross = [](const Vec3 &a, const Vec3 &b) {
      return Vec3{a.y * b.z - a.z * b.y,
                  a.z * b.x - a.x * b.z,
                  a.x * b.y - a.y * b.x};
    };
    const Vec3 forward =
        Unit(subtract(middle ? *middle : *index, *hand));
    Vec3 across{0.0, 0.0, -1.0};
    if (index && pinky)
      across = Unit(subtract(*index, *pinky));
    Vec3 normal = Unit(cross(forward, across));
    if (Length(normal) < 1e-6)
      normal = Vec3{0.0, 1.0, 0.0};
    if (Dot(normal, Vec3{0.0, 1.0, 0.0}) < 0.0) {
      normal.x = -normal.x;
      normal.y = -normal.y;
      normal.z = -normal.z;
    }
    palm_normals[static_cast<std::size_t>(side)] = normal;
  }
  struct FingerControlAxis {
    Vec3 axis;
    double weight = 1.0;
  };
  const auto finger_control_axis =
      [&](const std::string &target,
          const ManualFingerControl &control)
      -> std::optional<FingerControlAxis> {
    int side = -1;
    if (target.rfind("Bip001-L-", 0) == 0)
      side = 0;
    else if (target.rfind("Bip001-R-", 0) == 0)
      side = 1;
    if (side < 0)
      return std::nullopt;
    static constexpr double kSpreadWeights[5] = {
        -0.65, -0.35, 0.0, 0.25, 0.50};
    for (int finger{}; finger != 5; ++finger) {
      if (control.finger >= 0 && control.finger != finger)
        continue;
      const std::string base =
          std::string("Bip001-") + (side == 0 ? "L-" : "R-") +
          kFingers[finger].english;
      for (int joint{}; joint != 4; ++joint) {
        const std::string name =
            joint == 0 ? base : base + std::to_string(joint);
        if (target != name)
          continue;
        if (control.kind == FingerControlKind::Spread) {
          if (joint != 0)
            return std::nullopt;
          return FingerControlAxis{
              palm_normals[static_cast<std::size_t>(side)],
              kSpreadWeights[finger] * (side == 0 ? 1.0 : -1.0)};
        }
        double weight = finger == 0 ? 0.55 : (joint == 0 ? 0.75 : 1.0);
        const Vec3 current =
            rest_component_position[skeleton.by_name.at(name)];
        Vec3 direction;
        if (joint < 3 && skeleton.by_name.count(base + std::to_string(joint + 1)) != 0) {
          const Vec3 next = rest_component_position[
              skeleton.by_name.at(base + std::to_string(joint + 1))];
          direction = Vec3{next.x - current.x, next.y - current.y,
                           next.z - current.z};
        } else if (joint > 0) {
          const std::string previous_name =
              joint == 1 ? base : base + std::to_string(joint - 1);
          const Vec3 previous =
              rest_component_position[skeleton.by_name.at(previous_name)];
          direction = Vec3{current.x - previous.x, current.y - previous.y,
                           current.z - previous.z};
        }
        const Vec3 normal = palm_normals[static_cast<std::size_t>(side)];
        const Vec3 axis = Unit(Vec3{
            direction.y * normal.z - direction.z * normal.y,
            direction.z * normal.x - direction.x * normal.z,
            direction.x * normal.y - direction.y * normal.x});
        if (Length(axis) < 1e-6)
          return std::nullopt;
        return FingerControlAxis{axis, weight};
      }
    }
    return std::nullopt;
  };
  std::vector<Vec3> frame_translation(skeleton.bones.size());
  for (const auto &bone : skeleton.bones)
    frame_translation[bone.index] = bone.base_translation;

  std::vector<Quat> frame_rot(skeleton.bones.size());
  std::vector<Quat> frame_local(skeleton.bones.size());
  std::vector<Vec3> frame_pos(skeleton.bones.size());

  // ---- target-side leg IK -----------------------------------------------------------
  // Arms and torso map the source's *rotations*, but the legs cannot: the source model's leg
  // proportions (the thigh/shin ratio and the 腰 -> 左足 hip offset) differ from this rig's, so
  // a baked leg lands the ankle several centimetres off and the pelvis sinks below the source's
  // -- the body reads as sitting low and toppling forward even though every joint angle is
  // faithful. Instead the TARGET's own leg is re-solved: its hip is placed where the source's
  // hip is (mapped and scaled), the ankle is aimed at the source's ankle, and a two-bone solve
  // bends the knee using the target's own segment lengths. That is what MMD does live -- which
  // is why "any model" plays a motion the same way: a leg-length mismatch becomes knee flexion,
  // not a misplaced body.
  struct LegSolve {
    std::string thigh_name;
    std::string calf_name;
    std::string foot_name;
    std::uint32_t thigh = 0;
    std::uint32_t calf = 0;
    std::uint32_t foot = 0;
    std::size_t src_ankle = 0;
    std::size_t src_knee = 0;
    std::size_t src_hip = 0;
    std::size_t waist_index = 0;
    bool have_waist = false;
    Vec3 hip_from_waist_rest{};
    double thigh_len = 0.0;
    double shin_len = 0.0;
  };
  std::vector<LegSolve> leg_solves;
  if (pose_ready && pelvis_index < skeleton.bones.size()) {
    for (const auto &side : {std::pair<const char *, const char *>{"L", "左"},
                             std::pair<const char *, const char *>{"R", "右"}}) {
      const std::string thigh_name = std::string("Bip001-") + side.first + "-Thigh";
      const std::string calf_name = std::string("Bip001-") + side.first + "-Calf";
      const std::string foot_name = std::string("Bip001-") + side.first + "-Foot";
      const auto thigh_it = skeleton.by_name.find(thigh_name);
      const auto calf_it = skeleton.by_name.find(calf_name);
      const auto foot_it = skeleton.by_name.find(foot_name);
      const ReferenceBone *src_ankle =
          FindReference(reference, std::string(side.second) + "足首");
      if (src_ankle == nullptr)
        src_ankle = FindReference(reference, std::string(side.second) + "足首D");
      const ReferenceBone *src_knee = FindReference(reference, std::string(side.second) + "ひざ");
      if (src_knee == nullptr)
        src_knee = FindReference(reference, std::string(side.second) + "ひざD");
      const ReferenceBone *src_hip = FindReference(reference, std::string(side.second) + "足");
      if (src_hip == nullptr)
        src_hip = FindReference(reference, std::string(side.second) + "足D");
      // No waist bone is needed: the solve is taken in the source's own hip frame, so a PMD
      // skeleton (下半身 straight under センター, no 腰) solves exactly like a PMX one.
      if (thigh_it == skeleton.by_name.end() || calf_it == skeleton.by_name.end() ||
          foot_it == skeleton.by_name.end() || src_ankle == nullptr || src_knee == nullptr ||
          src_hip == nullptr || unit_scale < 1e-9)
        continue;
      LegSolve solve;
      solve.thigh_name = thigh_name;
      solve.calf_name = calf_name;
      solve.foot_name = foot_name;
      solve.thigh = static_cast<std::uint32_t>(thigh_it->second);
      solve.calf = static_cast<std::uint32_t>(calf_it->second);
      solve.foot = static_cast<std::uint32_t>(foot_it->second);
      solve.src_ankle = src_ankle->index;
      solve.src_knee = src_knee->index;
      solve.src_hip = src_hip->index;
      // The source's hip offset from the placement bone (腰/下半身), kept so the hip's own travel
      // around it can be added to the leg's target below (see the solve).
      if (!root_track_name.empty()) {
        const ReferenceBone *waist = FindReference(reference, root_track_name);
        if (waist != nullptr) {
          solve.hip_from_waist_rest = Vec3{src_hip->position.x - waist->position.x,
                                           src_hip->position.y - waist->position.y,
                                           src_hip->position.z - waist->position.z};
          solve.waist_index = waist->index;
          solve.have_waist = true;
        }
      }
      solve.thigh_len = Length(skeleton.bones[solve.calf].base_translation);
      solve.shin_len = Length(skeleton.bones[solve.foot].base_translation);
      if (solve.thigh_len < 1e-6 || solve.shin_len < 1e-6)
        continue;
      leg_solves.push_back(solve);
    }
  }

  std::size_t frame_index = 0;
  for (double frame : frames) {
    if (pose_ready) {
      ik_pose.SetFrame(ik_tracks, frame);
      if (solve_ik) {
        std::vector<std::string> disabled;
        if (input.ik_mode == IkMode::FollowMotion) {
          for (const auto &[name, records] : ik_states) {
            bool state = true;
            bool seen = false;
            for (const VmdIkState &record : records) {
              if (record.frame > frame)
                break;
              state = record.on;
              seen = true;
            }
            if (seen && !state)
              disabled.push_back(name);
          }
        }
        ik_pose.Solve(ik_reports, disabled, fk_fallback_available);
      }
    }
    const auto sample_source =
        [&](const std::string &source, Quat &rotation,
            Vec3 &translation) -> bool {
        const VmdTrack *track = find_track(source);
        if (solve_ik) {
          const auto ik = ik_index_by_name.find(CanonicalBoneName(source));
          if (ik != ik_index_by_name.end()) {
            rotation = ik_pose.rot[ik->second];
            translation = ik_pose.pos[ik->second];
            return true;
          }
        }
        if (track == nullptr)
          return false;
        double sampled_rotation[4] = {};
        double sampled_position[3] = {};
        if (!SampleTrack(*track, frame, sampled_rotation, sampled_position))
          return false;
        rotation = Normalize(Quat{sampled_rotation[0], sampled_rotation[1],
                                  sampled_rotation[2], sampled_rotation[3]});
        translation =
            Vec3{sampled_position[0], sampled_position[1], sampled_position[2]};
        return true;
      };
    for (auto &[side, target] : arm_ik_outputs) {
      Vec3 offset{};
      Quat wrist_rotation{};
      const double arm_scale = unit_scale > 1e-9 ? unit_scale : 8.0;
      for (const ManualArmIkControl &control : input.manual_arm_ik_controls) {
        if (!control.paired && control.side != side)
          continue;
        Quat sampled_rotation{};
        Vec3 sampled_position{};
        if (!sample_source(control.source, sampled_rotation,
                           sampled_position))
          continue;
        const Vec3 delta = Rotate(
            kAxisChange,
            Vec3{sampled_position.x * arm_scale * control.gain,
                 sampled_position.y * arm_scale * control.gain,
                 sampled_position.z * arm_scale * control.gain});
        offset.x += delta.x;
        offset.y += delta.y;
        offset.z += delta.z;
        if (control.follow_wrist) {
          wrist_rotation = Multiply(
              wrist_rotation,
              Multiply(Multiply(kAxisChange, sampled_rotation),
                       Conjugate(kAxisChange)));
        }
      }
      target.offsets.push_back(offset);
      target.rotations.push_back(Normalize(wrist_rotation));
    }
    for (const auto &entry : table) {
      Quat motion{};
      bool any = false;
      const auto compose = [&](const std::string &source, const bool conjugate) {
        Quat value{};
        Vec3 translation{};
        if (!sample_source(source, value, translation))
          return;
        motion = Multiply(motion, conjugate ? Conjugate(value) : value);
        any = true;
      };
      // Conjugated sources first: the parent carries their rotation *left*-multiplied, so a
      // target that must not inherit it has to multiply it back out on the left too.
      for (const auto &source : entry.prefix_inverse)
        compose(source, true);
      for (const auto &source : entry.sources)
        compose(source, false);
      // Conjugated sources: the parent already carries their rotation, so a target that must
      // not inherit it multiplies it back out (the spine under a pelvis that carries the hip).
      for (const auto &source : entry.inverse)
        compose(source, true);
      if (!any)
        motion = Quat{};
      const auto &bone = skeleton.bones[skeleton.by_name.at(entry.target)];
      // The corrected component chain, exactly like the reference implementation's
      // `rest_crot` (mmd2bip.py:1154-1160): the parent frame and the bone's own rest
      // component both come from it.
      const Quat parent_rest =
          bone.parent >= 0 ? rest_crot[static_cast<std::size_t>(bone.parent)] : Quat{};
      // A world-space delta, brought into the parent's frame by the rest parent rotation
      // and back into this bone's local frame by its own rest *component* rotation. At
      // Q=identity the result collapses to the rest local rotation, which is why a
      // rest-pose check cannot catch a mix-up here.
      Quat delta =
          Multiply(Multiply(kAxisChange, motion), Conjugate(kAxisChange));
      for (const ManualFingerControl &control : entry.finger_controls) {
        Quat control_rotation{};
        Vec3 unused_translation{};
        if (!sample_source(control.source, control_rotation,
                           unused_translation))
          continue;
        if (control_rotation.w < 0.0) {
          control_rotation.x = -control_rotation.x;
          control_rotation.y = -control_rotation.y;
          control_rotation.z = -control_rotation.z;
          control_rotation.w = -control_rotation.w;
        }
        const double component =
            control.input_axis == 0
                ? control_rotation.x
                : control.input_axis == 1 ? control_rotation.y
                                          : control_rotation.z;
        const double angle =
            2.0 * std::atan2(component, control_rotation.w);
        const auto axis = finger_control_axis(entry.target, control);
        if (!axis)
          continue;
        const double weighted = std::clamp(
            angle * static_cast<double>(control.gain) * axis->weight,
            -2.6, 2.6);
        delta = Multiply(delta, FromAxisAngle(axis->axis, weighted));
      }
      const Quat value = Normalize(
          Multiply(Multiply(Conjugate(parent_rest), delta), rest_crot[bone.index]));
      output[entry.target].push_back(ToArray4(value));
      frame_local[bone.index] = value;
    }

    // Counter-rotate a bone's *base* so a parent that carries the hip rotation does not drag
    // it (see `offset_bones`). The 下半身 delta is H = A·q·A^-1 (the same world delta the
    // pelvis applies), and the correction is t = C^-1 · H^±1 · C · rest with C the parent's
    // rest component rotation -- the local frame the offset is expressed in.
    if (!offset_bones.empty() && pose_ready && pelvis_index < rest_crot.size()) {
      const Quat hip_local =
          hip_source.has_value() ? ik_pose.rot[hip_source.value()] : Quat{};
      const Quat hip_delta =
          Multiply(Multiply(kAxisChange, hip_local), Conjugate(kAxisChange));
      const Quat frame_pelvis = rest_crot[pelvis_index];
      for (const OffsetBone &slot : offset_bones) {
        if (slot.index >= frame_translation.size())
          continue;
        const Quat body = Multiply(
            Multiply(Conjugate(frame_pelvis),
                     slot.negative_hip ? Conjugate(hip_delta) : hip_delta),
            frame_pelvis);
        const Vec3 rest_offset = skeleton.bones[slot.index].base_translation;
        frame_translation[slot.index] = Rotate(body, rest_offset);
        if (Length(slot.world_shift) > 1e-9) {
          // The hip drop follows the *body's* tilt, so express it in the pelvis's *rest*
          // frame (a constant local vector). The FK below then carries it with the pelvis's
          // live component rotation, exactly like the source's 腰->左足 drop rides センター.
          // Expressed in the live frame instead, the drop stayed world-fixed and lagged the
          // body by the whole pelvis turn (~14 cm dynamic error on the knee/ankle).
          const Vec3 in_parent = Rotate(Conjugate(frame_pelvis), slot.world_shift);
          frame_translation[slot.index] =
              Vec3{frame_translation[slot.index].x + in_parent.x,
                   frame_translation[slot.index].y + in_parent.y,
                   frame_translation[slot.index].z + in_parent.z};
        }
        output_offsets[slot.name].push_back(
            json::array({Round6(frame_translation[slot.index].x),
                         Round6(frame_translation[slot.index].y),
                         Round6(frame_translation[slot.index].z)}));
      }
    }

    // Explicit rotation parents can also carry the target's placement. This is applied in
    // the target parent's rest frame and emitted through boneOffsets, so the target head and
    // every descendant move together. Rotation-only parents stay as ordinary local rotation
    // on the target and deliberately do not alter its head position.
    for (const auto &[target_name, entry] : mapped_targets) {
      if (!std::any_of(entry->parents.begin(), entry->parents.end(),
                       [](const ManualParentMapping &mapping) {
                         return mapping.inherit_position;
                       }))
        continue;
      const auto target = skeleton.by_name.find(target_name);
      if (target == skeleton.by_name.end())
        continue;
      const Bone &bone = skeleton.bones[target->second];
      const Quat parent_rest =
          bone.parent >= 0
              ? rest_crot[static_cast<std::size_t>(bone.parent)]
              : Quat{};
      Vec3 local_delta{};
      for (const ManualParentMapping &mapping : entry->parents) {
        Quat rotation{};
        Vec3 translation{};
        if (!sample_source(mapping.source, rotation, translation))
          continue;
        if (mapping.inherit_position) {
          const Vec3 moved_world = Rotate(
              kAxisChange,
              Vec3{translation.x * unit_scale, translation.y * unit_scale,
                   translation.z * unit_scale});
          const Vec3 moved_parent = Rotate(Conjugate(parent_rest), moved_world);
          local_delta.x += moved_parent.x;
          local_delta.y += moved_parent.y;
          local_delta.z += moved_parent.z;

          std::size_t anchor_index =
              bone.parent >= 0 ? static_cast<std::size_t>(bone.parent)
                               : target->second;
          if (!mapping.anchor.empty()) {
            const auto configured_anchor =
                skeleton.by_name.find(mapping.anchor);
            if (configured_anchor != skeleton.by_name.end())
              anchor_index = configured_anchor->second;
          }
          const Vec3 &anchor_position =
              rest_component_position[anchor_index];
          const Vec3 &target_position =
              rest_component_position[target->second];
          const Vec3 anchor_to_child_world{
              target_position.x - anchor_position.x,
              target_position.y - anchor_position.y,
              target_position.z - anchor_position.z};
          const Vec3 anchor_to_child =
              Rotate(Conjugate(parent_rest), anchor_to_child_world);
          if (mapping.inherit_rotation) {
            const Quat parent_rotation =
                Multiply(Multiply(kAxisChange, rotation),
                         Conjugate(kAxisChange));
            const Vec3 rotated = Rotate(parent_rotation, anchor_to_child);
            local_delta.x += rotated.x - anchor_to_child.x;
            local_delta.y += rotated.y - anchor_to_child.y;
            local_delta.z += rotated.z - anchor_to_child.z;
          }
        }
      }
      Vec3 &stored = frame_translation[target->second];
      stored.x += local_delta.x;
      stored.y += local_delta.y;
      stored.z += local_delta.z;
      auto &track = output_offsets[target_name];
      const json value =
          json::array({Round6(stored.x), Round6(stored.y), Round6(stored.z)});
      if (track.size() > frame_index)
        track[frame_index] = value;
      else
        track.push_back(value);
    }

    // The root chain moves the root bone *and* is emitted as the root translation: the
    // runtime adds the delta to a target-space translation, while every child offset here
    // is measured from the root's own position.
    bool have_root = false;
    Vec3 root_delta{};
    if (root_mapped && pose_ready) {
      // The reference bone's *world* position, not just its own translation. The target's
      // pelvis sits exactly on its root bone on this rig (Bip001 and Bip001-Pelvis share an
      // origin), so the root translation *is* the pelvis placement, and the source's pelvis
      // moves with the rotation of the chain above it as well as with its translations.
      // Emitting only the translation part left rigoutput.vmd up to 7.4 mmd units (71 cm)
      // short on the frames where センター turns hardest -- the body swept around the hip
      // while the feet stayed where the previous frame left them, which reads as skating
      // backwards around a centre point.
      Vec3 absolute = root_chain.empty() ? Vec3{} : ik_pose.wpos[root_chain.back()];
      if (input.feet_anchor) {
        // Placement follows the feet instead of the pelvis (see Input::feet_anchor). The legs
        // already reproduce the source's ankle *relative* to the pelvis within a centimetre, so
        // anchoring here lands the feet where the source's feet are and the body swings over
        // them -- the "the fixed point is on the feet" read.
        Vec3 sum{};
        int anchors = 0;
        for (const char *name : {"左足首", "右足首"}) {
          const ReferenceBone *bone = FindReference(reference, name);
          if (bone == nullptr)
            continue;
          const Vec3 &point = ik_pose.wpos[bone->index];
          sum = Vec3{sum.x + point.x, sum.y + point.y, sum.z + point.z};
          ++anchors;
        }
        if (anchors > 0)
          absolute = Vec3{sum.x / anchors, sum.y / anchors, sum.z / anchors};
      }
      if (!have_root_base) {
        root_base = absolute;
        have_root_base = true;
      }
      const Vec3 moved =
          Rotate(kAxisChange, Vec3{absolute.x * unit_scale, absolute.y * unit_scale,
                                   absolute.z * unit_scale});
      frame_translation[root_index] = Vec3{root_rest_translation.x + moved.x,
                                           root_rest_translation.y + moved.y,
                                           root_rest_translation.z + moved.z};
      have_root = true;
      root_delta = Rotate(kAxisChange, Vec3{absolute.x - root_base.x,
                                            absolute.y - root_base.y,
                                            absolute.z - root_base.z});
      // Whatever the target's own root -> pelvis lever already produces must not be counted
      // twice: the mapped root rotation swings that offset by itself.
      if (root_lever != nullptr && unit_scale > 1e-9) {
        const Vec3 bump = Rotate(frame_local[root_index], *root_lever);
        if (!have_root_bump_base) {
          root_bump_base = bump;
          have_root_bump_base = true;
        }
        root_delta = Vec3{root_delta.x - (bump.x - root_bump_base.x) / unit_scale,
                          root_delta.y - (bump.y - root_bump_base.y) / unit_scale,
                          root_delta.z - (bump.z - root_bump_base.z) / unit_scale};
      }
    }

    for (const auto &bone : skeleton.bones) {
      // Component-space pose for this bone, in index order: a mapped bone's local rotation
      // came from the mapped pass, a secondary bone's from the spring below, and anything
      // else is held rigid against its parent. The spring and every child offset are
      // expressed against the *component* rotation -- feeding them a local rotation (as an
      // earlier version did) silently rotated every accessory and its whole subtree
      // (mmd2bip.py:1188-1206).
      const Quat component_parent =
          bone.parent >= 0 ? frame_rot[static_cast<std::size_t>(bone.parent)] : Quat{};
      // Re-solve this leg (see `leg_solves`): overwrite the thigh/calf/foot locals before the
      // normal pass reads them, so the whole rest of the frame (positions, the spring, the
      // emitted document) sees the solved leg. The pelvis has already been processed here, so
      // its live world rotation/position are available.
      for (const LegSolve &solve : leg_solves) {
        if (bone.index != solve.thigh)
          continue;
        const Quat pelvis_rot = frame_rot[pelvis_index];
        // Solve in the frame of the source's own *hip*, not its pelvis. How far the hips hang
        // below the waist is a rig constant -- 0.68 mmd units on the author's model, 2.49 on the
        // PMD the user actually plays in MMD -- and transferring it moved this rig's legs 21 cm
        // below its own hips: the ankles dropped from +3.3 cm to -10.9 cm on the same frame, i.e.
        // the feet went through the floor. The hip as origin keeps what matters (this rig's hip
        // placement, the source's leg shape) and drops the constant.
        const Vec3 hip_now = ik_pose.wpos[solve.src_hip];
        const auto relative = [&](std::size_t index) {
          const Vec3 point = ik_pose.wpos[index];
          return Rotate(kAxisChange,
                        Vec3{(point.x - hip_now.x) * unit_scale, (point.y - hip_now.y) * unit_scale,
                             (point.z - hip_now.z) * unit_scale});
        };
        const Vec3 hip_rel{};
        Vec3 goal = relative(solve.src_ankle);
        const Vec3 knee_ref = relative(solve.src_knee);
        // MMD pins the foot to the 足ＩＫ target (fixed in the model's space) and lets the legs
        // connect it to the hips. Ours is placed *relative to the hip*, and the body's placement
        // follows the source's 腰/下半身 -- so the hips' own travel around that bone (2.5 units of
        // lever on a PMD: up to 21 cm when 下半身 pitches, i.e. every crouch and bend) landed
        // straight in the feet. Measured on rigoutput.vmd: the foot 19.4 cm off on average and
        // 11.7% of frames 3-18 cm *below* the ground, while MMD never puts it below rest. Add
        // exactly that travel back to the leg's target: the body keeps the placement it had, only
        // the foot moves to where MMD has it.
        if (solve.have_waist) {
          const Vec3 &waist_now = ik_pose.wpos[solve.waist_index];
          const Vec3 arc = Rotate(kAxisChange,
              Vec3{((hip_now.x - waist_now.x) - solve.hip_from_waist_rest.x) * unit_scale,
                   ((hip_now.y - waist_now.y) - solve.hip_from_waist_rest.y) * unit_scale,
                   ((hip_now.z - waist_now.z) - solve.hip_from_waist_rest.z) * unit_scale});
          goal = Vec3{goal.x + arc.x, goal.y + arc.y, goal.z + arc.z};
        }
        const double reach = solve.thigh_len + solve.shin_len;
        const Vec3 to_goal{goal.x - hip_rel.x, goal.y - hip_rel.y, goal.z - hip_rel.z};
        const double d = Length(to_goal);
        Vec3 thigh_dir{}, shin_dir{}, knee{};
        if (d < 1e-6) {
          thigh_dir = Vec3{0.0, 0.0, -1.0};
          shin_dir = thigh_dir;
          knee = Vec3{hip_rel.x, hip_rel.y, hip_rel.z + thigh_dir.z * solve.thigh_len};
        } else if (d >= reach) {
          // Unreachable: MMD straightens the leg toward the IK goal and the ankle floats the
          // shortfall. Reproducing that (rather than sinking the pelvis) is the whole point.
          thigh_dir = Unit(to_goal);
          shin_dir = thigh_dir;
          knee = Vec3{hip_rel.x + thigh_dir.x * solve.thigh_len,
                      hip_rel.y + thigh_dir.y * solve.thigh_len,
                      hip_rel.z + thigh_dir.z * solve.thigh_len};
        } else {
          const Vec3 u = Unit(to_goal);
          double cos_theta = (solve.thigh_len * solve.thigh_len + d * d -
                              solve.shin_len * solve.shin_len) /
                             (2.0 * solve.thigh_len * d);
          cos_theta = (std::max)(-1.0, (std::min)(1.0, cos_theta));
          const double theta = std::acos(cos_theta);
          // The bend plane comes from the source knee, so this knee bends the way the
          // source's does (a fixed world axis bends it sideways by tens of degrees).
          Vec3 normal{knee_ref.y * to_goal.z - knee_ref.z * to_goal.y,
                      knee_ref.z * to_goal.x - knee_ref.x * to_goal.z,
                      knee_ref.x * to_goal.y - knee_ref.y * to_goal.x};
          normal = Length(normal) > 1e-6 ? Unit(normal) : Vec3{1.0, 0.0, 0.0};
          const Vec3 in_plane = Unit(Vec3{normal.y * u.z - normal.z * u.y,
                                          normal.z * u.x - normal.x * u.z,
                                          normal.x * u.y - normal.y * u.x});
          const double sn = std::sin(theta);
          const Vec3 dir1{u.x * cos_theta + in_plane.x * sn, u.y * cos_theta + in_plane.y * sn,
                          u.z * cos_theta + in_plane.z * sn};
          const Vec3 dir2{u.x * cos_theta - in_plane.x * sn, u.y * cos_theta - in_plane.y * sn,
                          u.z * cos_theta - in_plane.z * sn};
          const Vec3 k1{hip_rel.x + dir1.x * solve.thigh_len,
                        hip_rel.y + dir1.y * solve.thigh_len,
                        hip_rel.z + dir1.z * solve.thigh_len};
          const Vec3 k2{hip_rel.x + dir2.x * solve.thigh_len,
                        hip_rel.y + dir2.y * solve.thigh_len,
                        hip_rel.z + dir2.z * solve.thigh_len};
          knee = Length(Vec3{k1.x - knee_ref.x, k1.y - knee_ref.y, k1.z - knee_ref.z}) <=
                         Length(Vec3{k2.x - knee_ref.x, k2.y - knee_ref.y, k2.z - knee_ref.z})
                     ? k1
                     : k2;
          thigh_dir = Unit(Vec3{knee.x - hip_rel.x, knee.y - hip_rel.y, knee.z - hip_rel.z});
          shin_dir = Unit(Vec3{goal.x - knee.x, goal.y - knee.y, goal.z - knee.z});
        }
        // The thigh keeps this rig's own rest translation: the hip must stay where this rig puts
        // it, swinging with the pelvis rotation that the mapped source rotation already carries.
        // Overwriting it with the source's waist->hip offset (or with the source hip's travel,
        // which mixes two different lever lengths) either hangs the legs at the source rig's
        // height -- the feet sink through the floor -- or stops being a rigid rotation of one
        // offset and spreads the two hips apart (measured: 14.8 cm -> 30-45 cm separation).
        // Rebuild the three world rotations, keeping the roll the mapped leg already had (the
        // swing is the minimal rotation from the mapped axis to the solved one), and keep the
        // foot's absolute orientation so the foot still points where the source's does.
        const Quat mapped_thigh = Multiply(pelvis_rot, frame_local[solve.thigh]);
        const Quat mapped_calf = Multiply(mapped_thigh, frame_local[solve.calf]);
        const Quat mapped_foot = Multiply(mapped_calf, frame_local[solve.foot]);
        const Quat thigh_world =
            Multiply(Swing(Rotate(mapped_thigh, Vec3{1.0, 0.0, 0.0}), thigh_dir), mapped_thigh);
        const Quat calf_world =
            Multiply(Swing(Rotate(mapped_calf, Vec3{1.0, 0.0, 0.0}), shin_dir), mapped_calf);
        frame_local[solve.thigh] = Multiply(Conjugate(pelvis_rot), thigh_world);
        frame_local[solve.calf] = Multiply(Conjugate(thigh_world), calf_world);
        frame_local[solve.foot] = Multiply(Conjugate(calf_world), mapped_foot);
        // The document is what the plugin plays, so it has to carry the solved locals too.
        const auto write_back = [&](const std::string &name, const Quat &value) {
          auto found = output.find(name);
          if (found != output.end() && frame_index < found->second.size())
            found->second[frame_index] = ToArray4(value);
        };
        write_back(solve.thigh_name, frame_local[solve.thigh]);
        write_back(solve.calf_name, frame_local[solve.calf]);
        write_back(solve.foot_name, frame_local[solve.foot]);
      }
      const bool is_mapped = mapped_targets.count(bone.name) != 0;
      const Quat local_rigid = is_mapped ? frame_local[bone.index] : rest_local[bone.index];
      const SecondaryBone *spring = secondary_by_index[bone.index];
      if (spring == nullptr) {
        frame_rot[bone.index] = Multiply(component_parent, local_rigid);
        const Vec3 offset = Rotate(component_parent, frame_translation[bone.index]);
        frame_pos[bone.index] =
            bone.parent >= 0 ? frame_pos[static_cast<std::size_t>(bone.parent)] + offset
                             : offset;
        continue;
      }
      {
        const Vec3 offset = Rotate(component_parent, frame_translation[bone.index]);
        const Vec3 head = bone.parent >= 0
                              ? frame_pos[static_cast<std::size_t>(bone.parent)] + offset
                              : offset;
        frame_pos[bone.index] = head;
        const Quat rigid_rotation = Multiply(component_parent, spring->local);
        const Vec3 axis_world = Rotate(rigid_rotation, spring->axis);
        const bool breast = secondary::IsBreast(bone.name);
        const double inertia_gain =
            dynamics_tuning.inertia *
            dynamics_tuning.material_inertia[
                secondary::MotionMaterialIndex(spring->material)] *
            (breast ? dynamics_tuning.breast_inertia : 1.0);
        if (inertia_gain <= 1e-6) {
          // Zero inertia means the authored material is stationary relative to
          // its parent. The spring may only add acceleration to a non-zero
          // baseline; running stiffness and gravity at zero still made ribbons
          // sway from a standing character.
          frame_local[bone.index] = spring->local;
          frame_rot[bone.index] = rigid_rotation;
          output[spring->name].push_back(ToArray4(spring->local));
          sim_direction[bone.index] = axis_world;
          sim_velocity[bone.index] = Vec3{};
          sim_position[bone.index] = head;
          sim_position_older[bone.index] = head;
          sim_seeded[bone.index] = true;
          sim_frames[bone.index] = 1;
          continue;
        }
        Vec3 direction = sim_seeded[bone.index] ? sim_direction[bone.index] : axis_world;
        Vec3 velocity = sim_velocity[bone.index];
        Vec3 target = axis_world;
        {
          const Vec3 previous = sim_position[bone.index];
          const Vec3 older =
              sim_seeded[bone.index] ? sim_position_older[bone.index] : head;
          sim_position_older[bone.index] = previous;
          sim_position[bone.index] = head;
          // Gravity is always part of the hanging target; only the acceleration term needs
          // two earlier samples (mmd2bip.py:1229-1250).
          Vec3 gravity_down =
              Length(spring->native_gravity) > 1e-6
                  ? spring->native_gravity
                  : Vec3{0.0, 0.0, -secondary::kGravity};
          if (sim_frames[bone.index] >= 2 && dt > 1e-9) {
            Vec3 accel{(head.x - 2.0 * previous.x + older.x) / (dt * dt),
                       (head.y - 2.0 * previous.y + older.y) / (dt * dt),
                       (head.z - 2.0 * previous.z + older.z) / (dt * dt)};
            // Cap at 3 g: the centre track is sparse and its second difference spikes
            // (mmd2bip.py:1234-1244).
            const double magnitude = Length(accel);
            const double limit = 3.0 * secondary::kGravity;
            if (magnitude > limit) {
              accel.x *= limit / magnitude;
              accel.y *= limit / magnitude;
              accel.z *= limit / magnitude;
            }
            const double response_gain = inertia_gain * spring->motion_follow;
            accel.x *= response_gain;
            accel.y *= response_gain;
            accel.z *= response_gain;
            gravity_down = Vec3{
                gravity_down.x - 0.5 * accel.x,
                gravity_down.y - 0.5 * accel.y,
                gravity_down.z - 0.5 * accel.z};
            Vec3 air_flow{
                (head.x - previous.x) / dt * -0.55 *
                    secondary::kMovingAirResponse * dynamics_tuning.air *
                    spring->response.air,
                (head.y - previous.y) / dt * -0.55 *
                    secondary::kMovingAirResponse * dynamics_tuning.air *
                    spring->response.air,
                (head.z - previous.z) / dt * -0.55 *
                    secondary::kMovingAirResponse * dynamics_tuning.air *
                    spring->response.air};
            const double air_length = Length(air_flow);
            if (air_length > 1.5 * secondary::kGravity) {
              const double scale = 1.5 * secondary::kGravity / air_length;
              air_flow.x *= scale;
              air_flow.y *= scale;
              air_flow.z *= scale;
            }
            gravity_down.x += air_flow.x;
            gravity_down.y += air_flow.y;
            gravity_down.z += air_flow.z;
            if (std::abs(spring->forward_move_offset) > 1e-6) {
              const Vec3 head_velocity{(head.x - previous.x) / dt,
                                       (head.y - previous.y) / dt,
                                       (head.z - previous.z) / dt};
              const double moving =
                  std::clamp(Length(head_velocity) / 300.0, 0.0, 1.0);
              gravity_down.x +=
                  spring->forward_move_offset * moving *
                  secondary::kGravity * 0.45 * spring->response.air;
            }
          }
          const Vec3 hang = Unit(gravity_down);
          if (Length(hang) > 0.5) {
            // How much of the strand's own hang direction (gravity plus the body's acceleration)
            // replaces "follow the bone it hangs on". Hair and skirts want to stay close to the
            // body; a sleeve wants its hem down.
            const double gravity_share = std::clamp(
                (is_sleeve(spring->name) ? kSleeveGravityWeight
                                         : secondary::kGravityWeight) *
                    dynamics_tuning.gravity *
                    (breast ? dynamics_tuning.breast_gravity : 1.0),
                0.0, 1.0);
            const Vec3 blended{axis_world.x * (1.0 - gravity_share) + hang.x * gravity_share,
                               axis_world.y * (1.0 - gravity_share) + hang.y * gravity_share,
                               axis_world.z * (1.0 - gravity_share) + hang.z * gravity_share};
            if (Dot(blended, blended) > 1e-9)
              target = Unit(blended);
          }
        }
        const bool hair_swing = is_swing_only(bone.name);
        auto spring_settings = spring->spring;
        spring_settings.damping *= dynamics_tuning.damping;
        if (breast) {
          spring_settings.stiffness *= dynamics_tuning.breast_stiffness;
          spring_settings.damping *= dynamics_tuning.breast_damping;
        }
        const Vec3 previous_ribbon_direction = direction;
        double ribbon_raw_response = 0.0;
        if (spring->material == secondary::MotionMaterial::Ribbon) {
          ribbon_raw_response = std::clamp(
              inertia_gain *
                  std::clamp(spring_settings.stiffness / 200.0, 0.0, 1.0),
              0.0, 1.0);
          const double response = secondary::NormalizedChainResponse(
              ribbon_raw_response, ribbon_chain_length[spring->index]);
          spring_settings.stiffness *= response;
          spring_settings.damping *= dynamics_tuning.ribbon_damping;
        }
        // High rebound settings need more integration steps; otherwise the
        // explicit damper can reverse velocity and make the motion unstable.
        const double step_rate =
            (std::max)(120.0, spring_settings.damping * 2.0);
        const int substeps = std::clamp(
            static_cast<int>(std::ceil(dt * step_rate - 1e-8)), 4, 128);
        const double h = dt / substeps;
        for (int substep = 0; substep < substeps; ++substep) {
          const Vec3 cross{direction.y * target.z - direction.z * target.y,
                           direction.z * target.x - direction.x * target.z,
                           direction.x * target.y - direction.y * target.x};
          velocity.x +=
              (spring_settings.stiffness * cross.x -
               spring_settings.damping * velocity.x) * h;
          velocity.y +=
              (spring_settings.stiffness * cross.y -
               spring_settings.damping * velocity.y) * h;
          velocity.z +=
              (spring_settings.stiffness * cross.z -
               spring_settings.damping * velocity.z) * h;
          const Vec3 movement{velocity.x * h, velocity.y * h,
                              velocity.z * h};
          direction = Unit(Vec3{
              direction.x + movement.y * direction.z -
                  movement.z * direction.y,
              direction.y + movement.z * direction.x -
                  movement.x * direction.z,
              direction.z + movement.x * direction.y -
                  movement.y * direction.x});
          if (Length(direction) < 0.5)
            direction = target;
        }
        const double breast_amplitude =
            breast ? dynamics_tuning.breast_amplitude : 1.0;
        if (breast && breast_amplitude <= 1e-6) {
          direction = axis_world;
          target = axis_world;
          velocity = Vec3{};
        } else if (breast &&
                   std::abs(breast_amplitude - 1.0) > 1e-6) {
          direction = Unit(Vec3{
              axis_world.x +
                  (direction.x - axis_world.x) * breast_amplitude,
              axis_world.y +
                  (direction.y - axis_world.y) * breast_amplitude,
              axis_world.z +
                  (direction.z - axis_world.z) * breast_amplitude});
        }
        const double alignment =
            (std::max)(-1.0, (std::min)(1.0, Dot(direction, target)));
        const double lag =
            std::acos(alignment) * 180.0 / 3.14159265358979323846;
        const double maximum_lag = breast
            ? std::clamp(spring_settings.lag_degrees * breast_amplitude,
                         0.0, 120.0)
            : spring_settings.lag_degrees;
        if (lag > maximum_lag) {
          const double t = maximum_lag / lag;
          direction = Unit(Vec3{
              direction.x + (target.x - direction.x) * t,
              direction.y + (target.y - direction.y) * t,
              direction.z + (target.z - direction.z) * t});
          velocity.x *= 0.5;
          velocity.y *= 0.5;
          velocity.z *= 0.5;
        }
        if (spring->material == secondary::MotionMaterial::Ribbon) {
          const double angle_degrees =
              std::acos(std::clamp(
                            Dot(previous_ribbon_direction, direction), -1.0,
                            1.0)) *
              180.0 / 3.14159265358979323846;
          const double maximum_step_degrees =
              0.75 + 7.25 * ribbon_raw_response;
          if (angle_degrees > maximum_step_degrees) {
            direction = Unit(Vec3{
                previous_ribbon_direction.x +
                    (direction.x - previous_ribbon_direction.x) *
                        (maximum_step_degrees / angle_degrees),
                previous_ribbon_direction.y +
                    (direction.y - previous_ribbon_direction.y) *
                        (maximum_step_degrees / angle_degrees),
                previous_ribbon_direction.z +
                    (direction.z - previous_ribbon_direction.z) *
                        (maximum_step_degrees / angle_degrees)});
          }
          if (Length(velocity) < 0.015)
            velocity = Vec3{};
        }
        sim_direction[bone.index] = direction;
        sim_velocity[bone.index] = velocity;
        sim_seeded[bone.index] = true;
        ++sim_frames[bone.index];
        const Vec3 local_axis = Unit(Rotate(Conjugate(component_parent), direction));
        Quat local = Multiply(Swing(spring->axis, local_axis),
                              twist_about(spring->local, spring->axis));
        if (hair_swing ||
            spring->material == secondary::MotionMaterial::Ribbon) {
          // Keep where the strand points, hold its roll at the rigid pose (spec 5.1).
          const Quat deviation = Multiply(local, Conjugate(spring->local));
          const Quat roll = twist_about(deviation, Rotate(spring->local, spring->axis));
          local = Multiply(Multiply(deviation, Conjugate(roll)), spring->local);
        }
        local = Normalize(local);
        frame_local[bone.index] = local;
        frame_rot[bone.index] = Multiply(component_parent, local);
        output[spring->name].push_back(ToArray4(local));
      }
    }

    // Twist helpers: a share of the limb's twist about the limb axis, taken from the
    // driven limb bone's delta. Applied as a rotation *about the axis* rather than a
    // fraction of the whole delta, otherwise the limb would bend in the middle instead
    // of twisting (mmd2bip.py:1375-1403).
    for (const auto &item : twist_bones) {
      const Quat base = rest_crot[item.arm_index];
      const Quat delta_world = Multiply(frame_rot[item.arm_index], Conjugate(base));
      const Quat delta_local = Multiply(Multiply(Conjugate(base), delta_world), base);
      double angle = 0.0;
      bool authored = false;
      if (!item.source_twist.empty() && Length(item.source_axis) > 1e-9) {
        const VmdTrack *track = find_track(item.source_twist);
        double rotation[4] = {};
        double position[3] = {};
        if (track != nullptr && SampleTrack(*track, frame, rotation, position)) {
          const Quat source = Normalize(
              Quat{rotation[0], rotation[1], rotation[2], rotation[3]});
          const Quat axial = twist_about(source, item.source_axis);
          double source_angle =
              2.0 * std::atan2(axial.x * item.source_axis.x +
                                   axial.y * item.source_axis.y +
                                   axial.z * item.source_axis.z,
                               axial.w);
          constexpr double kPi = 3.14159265358979323846;
          while (source_angle > kPi)
            source_angle -= 2.0 * kPi;
          while (source_angle < -kPi)
            source_angle += 2.0 * kPi;
          angle = source_angle * item.weight;
          authored = true;
        }
      }
      if (!authored) {
        const double projected = delta_local.x * item.axis.x +
                                 delta_local.y * item.axis.y +
                                 delta_local.z * item.axis.z;
        // Derived twist stays bounded: an unclamped share measured up to 164 degrees,
        // which would spin the forearm. Explicit 腕捩/手捩 is author data and keeps its
        // authored angle, shared over the target's two helper bones.
        angle = (std::max)(
            -0.35, (std::min)(0.35, 2.0 * std::atan2(projected, delta_local.w) *
                                         item.weight));
      }
      const double half = angle * 0.5;
      const double sine = std::sin(half);
      const Quat share{item.axis.x * sine, item.axis.y * sine, item.axis.z * sine,
                       std::cos(half)};
      const Quat local = Normalize(Multiply(share, rest_local[item.index]));
      output[item.name].push_back(ToArray4(local));
      frame_rot[item.index] =
          Multiply(item.parent >= 0 ? frame_rot[static_cast<std::size_t>(item.parent)]
                                    : Quat{},
                   local);
    }
    // ---- horizontal distance constraints
    if (!distance_links.empty()) {
      const auto rebuild = [&]() {
        for (const auto &bone : skeleton.bones) {
          const Quat parent_rotation =
              bone.parent >= 0
                  ? frame_rot[static_cast<std::size_t>(bone.parent)]
                  : Quat{};
          frame_rot[bone.index] =
              Multiply(parent_rotation, frame_local[bone.index]);
          const Vec3 offset =
              Rotate(parent_rotation, frame_translation[bone.index]);
          frame_pos[bone.index] =
              bone.parent >= 0
                  ? frame_pos[static_cast<std::size_t>(bone.parent)] +
                        offset
                  : offset;
        }
      };
      const auto head_at = [&](const std::uint32_t index) {
        return bone_constraints::Vec{
            frame_pos[index].x, frame_pos[index].y, frame_pos[index].z};
      };
      const auto tip_at = [&](const std::uint32_t index) {
        const SecondaryBone &spring = *secondary_by_index[index];
        const Vec3 tip =
            frame_pos[index] +
            Rotate(frame_rot[index], spring.axis) * spring.length;
        return bone_constraints::Vec{tip.x, tip.y, tip.z};
      };
      const auto apply_direction =
          [&](const std::uint32_t index,
              const bone_constraints::Vec wanted,
              const double velocity_keep = 0.5) {
            const SecondaryBone &spring = *secondary_by_index[index];
            const Quat parent_rotation =
                spring.parent >= 0
                    ? frame_rot[static_cast<std::size_t>(spring.parent)]
                    : Quat{};
            const Vec3 direction =
                Unit(Vec3{wanted.x, wanted.y, wanted.z});
            const Vec3 local_axis =
                Unit(Rotate(Conjugate(parent_rotation), direction));
            Quat local =
                Multiply(Swing(spring.axis, local_axis),
                         twist_about(spring.local, spring.axis));
            if (is_swing_only(spring.name) ||
                spring.material == secondary::MotionMaterial::Ribbon) {
              const Quat deviation =
                  Multiply(local, Conjugate(spring.local));
              const Quat roll =
                  twist_about(deviation,
                              Rotate(spring.local, spring.axis));
              local = Multiply(Multiply(deviation, Conjugate(roll)),
                               spring.local);
            }
            frame_local[index] = Normalize(local);
            frame_rot[index] =
                Multiply(parent_rotation, frame_local[index]);
            sim_direction[index] =
                Rotate(frame_rot[index], spring.axis);
            Vec3 &velocity = sim_velocity[index];
            velocity = Vec3{velocity.x * velocity_keep,
                            velocity.y * velocity_keep,
                            velocity.z * velocity_keep};
            auto track = output.find(spring.name);
            if (track != output.end() &&
                frame_index < track->second.size())
              track->second[frame_index] =
                  ToArray4(frame_local[index]);
          };
      const auto relative_speed_at =
          [&](const bone_constraints::Link &link) {
            const SecondaryBone &a = *secondary_by_index[link.a];
            const SecondaryBone &b = *secondary_by_index[link.b];
            const Vec3 velocity_a{
                sim_velocity[link.a].y * sim_direction[link.a].z -
                    sim_velocity[link.a].z * sim_direction[link.a].y,
                sim_velocity[link.a].z * sim_direction[link.a].x -
                    sim_velocity[link.a].x * sim_direction[link.a].z,
                sim_velocity[link.a].x * sim_direction[link.a].y -
                    sim_velocity[link.a].y * sim_direction[link.a].x};
            const Vec3 velocity_b{
                sim_velocity[link.b].y * sim_direction[link.b].z -
                    sim_velocity[link.b].z * sim_direction[link.b].y,
                sim_velocity[link.b].z * sim_direction[link.b].x -
                    sim_velocity[link.b].x * sim_direction[link.b].z,
                sim_velocity[link.b].x * sim_direction[link.b].y -
                    sim_velocity[link.b].y * sim_direction[link.b].x};
            return Length(velocity_a * a.length - velocity_b * b.length);
          };
      rebuild();
      if (input.dynamics_tuning.bone_constraints)
        distance_corrections +=
            bone_constraints::Relax(distance_links, 2, head_at, tip_at,
                                    apply_direction, rebuild);
      if (input.dynamics_tuning.self_collision) {
        rebuild();
        self_collision_corrections +=
            bone_constraints::RelaxSelfCollision(
                self_links, self_contact_state, 2, head_at, tip_at,
                apply_direction, rebuild, relative_speed_at);
      }
    }
    // ------------------------------------------------------------------ strand collision
    //
    // Done here, at the end of the frame, for one reason: the volumes have to come from the pose
    // this frame actually reaches. Building them at the top of the frame used the *previous*
    // frame's body (the leg bones even sit after the accessories in index order), and a strand
    // pushed out of last frame's torso is still inside this frame's when the body is moving -- the
    // correction fired 289 times and changed nothing measurable.
    //
    // Walking `secondary` in index order keeps parents before children, so a corrected strand's
    // children are corrected against the parent they actually have, and the emitted track for this
    // frame is overwritten in place (the leg solve above does the same).
    //
    // Frame 0 is included: it was skipped when the volumes still needed a previous frame, and the
    // first frame of playback is a frame the player sees -- the rig's own rest clearance is not
    // enough there, because the body is already in the source's f0 pose and a rigid strand can sit
    // inside it (measured: 浔's 3.8 cm, 残虹's 4.7 cm, both at f0 and nowhere else).
    strand_capsules.clear();
    {
      const auto add_capsule = [&](const char *first, const char *second, const double radius,
                                   const std::uint8_t mask,
                                   const double end_radius = 0.0) {
        const auto a = skeleton.by_name.find(first);
        const auto b = skeleton.by_name.find(second);
        if (a == skeleton.by_name.end() || b == skeleton.by_name.end())
          return;
        const Vec3 &start = frame_pos[a->second];
        const Vec3 &end = frame_pos[b->second];
        if (Length(start) < 1.0 || Length(end) < 1.0)
          return;
        strand_capsules.push_back(StrandCapsule{start, end, radius, end_radius, mask});
      };
      add_capsule("Bip001-Pelvis", "Bip001-Spine", kTorsoCollisionRadiusCm, kCollideTorso);
      add_capsule("Bip001-Spine", "Bip001-Spine1", kTorsoCollisionRadiusCm, kCollideTorso);
      add_capsule("Bip001-Spine1", "Bip001-Spine2", kTorsoCollisionRadiusCm, kCollideTorso);
      if (input.dynamics_tuning.extended_collision) {
        add_capsule("Bip001-Neck", "Bip001-Head", kNeckCollisionRadiusCm,
                    kCollideHead, kNeckTopCollisionRadiusCm);
        const auto head = skeleton.by_name.find("Bip001-Head");
        if (head != skeleton.by_name.end() &&
            Length(frame_pos[head->second]) >= 1.0) {
          const Vec3 center = frame_pos[head->second];
          strand_capsules.push_back(StrandCapsule{
              center, center, kHeadCollisionRadiusCm, 0.0, kCollideHead});
        }
        for (const char *side : {"L", "R"}) {
          const std::string clavicle =
              std::string("Bip001-") + side + "-Clavicle";
          const std::string upper =
              std::string("Bip001-") + side + "-UpperArm";
          const std::string forearm =
              std::string("Bip001-") + side + "-Forearm";
          const std::string hand =
              std::string("Bip001-") + side + "-Hand";
          add_capsule(clavicle.c_str(), upper.c_str(),
                      kShoulderCollisionRadiusCm, kCollideArms);
          add_capsule(upper.c_str(), forearm.c_str(), kArmCollisionRadiusCm,
                      kCollideArms);
          add_capsule(forearm.c_str(), hand.c_str(), kHandCollisionRadiusCm,
                      kCollideArms);
        }
      }
      // The thigh capsule starts a third of the way down the bone: a strand's *root* sits on the
      // pelvis and is legitimately inside the hip, and a strand whose head cannot move can never be
      // pushed out of a capsule containing it -- only its direction is free. Measured with the
      // capsule at the hip joint: the skirt's mean depth fell 4.38 -> 1.49 cm but the worst stayed
      // at 5.5, all of it in the first third of the thigh.
      const auto midway = [&](const char *bone, const char *next, const double fraction) {
        const auto a = skeleton.by_name.find(bone);
        const auto b = skeleton.by_name.find(next);
        if (a == skeleton.by_name.end() || b == skeleton.by_name.end())
          return Vec3{};
        const Vec3 &start = frame_pos[a->second];
        const Vec3 &end = frame_pos[b->second];
        return Vec3{start.x + (end.x - start.x) * fraction,
                    start.y + (end.y - start.y) * fraction,
                    start.z + (end.z - start.z) * fraction};
      };
      const auto add_leg = [&](const char *first, const char *second, const double fraction,
                               const double radius) {
        const auto a = skeleton.by_name.find(first);
        const auto b = skeleton.by_name.find(second);
        if (a == skeleton.by_name.end() || b == skeleton.by_name.end())
          return;
        const Vec3 &end = frame_pos[b->second];
        if (Length(end) < 1.0)
          return;
        const Vec3 start = midway(first, second, fraction);
        if (Length(start) < 1.0)
          return;
        strand_capsules.push_back(StrandCapsule{start, end, radius, 0.0, kCollideLegs});
      };
      for (std::size_t volume = 0; volume < 4; ++volume) {
        if (leg_radius_cm[volume] > 0.0)
          add_leg(kLegVolumes[volume].first, kLegVolumes[volume].second,
                  kLegVolumes[volume].fraction, leg_radius_cm[volume]);
      }
    }
    for (const SecondaryBone &spring : secondary) {
      const Quat parent_rotation =
          spring.parent >= 0 ? frame_rot[static_cast<std::size_t>(spring.parent)] : Quat{};
      const Vec3 parent_position =
          spring.parent >= 0 ? frame_pos[static_cast<std::size_t>(spring.parent)] : Vec3{};
      const Vec3 offset = Rotate(parent_rotation, spring.translation);
      const Vec3 head{parent_position.x + offset.x, parent_position.y + offset.y,
                      parent_position.z + offset.z};
      frame_pos[spring.index] = head;
      // The played pose is `parent x local`, so a strand has to be measured from the parent it
      // actually ends up with. The main pass built this bone's rotation from the parent's
      // *uncorrected* rotation, so every corrected parent skewed its whole chain by that
      // correction: the converter reported 2.2 cm on the worst strand while the geometry the
      // document plays measured 4.5 cm on the same bone and frame (Bn_m_piaodaiF_007, f150).
      // Re-deriving it here also means the correction is chosen against the direction the file
      // really plays, not against a stale one.
      frame_rot[spring.index] = Multiply(parent_rotation, frame_local[spring.index]);
      const double collision_gain =
          dynamics_tuning.collision *
          dynamics_tuning.material_collision[
              secondary::MotionMaterialIndex(spring.material)];
      if (spring.collide == 0 || strand_capsules.empty() ||
          collision_gain <= 1e-6)
        continue;
      const Vec3 direction = Rotate(frame_rot[spring.index], spring.axis);
      double slack = (std::numeric_limits<double>::infinity)();
      for (const StrandCapsule &capsule : strand_capsules)
        if ((capsule.mask & spring.collide) != 0)
          slack = (std::min)(slack, secondary::CollisionSlackCm(capsule.mask));
      if (!std::isfinite(slack))
        continue;
      // A strand mid-correction is driven further out than a fresh one, so the pass cannot flicker on
      // and off around the boundary while the spring keeps pulling the cloth back in.
      const bool was_held = collision_held[spring.index];
      const double threshold = was_held ? slack * 0.5 : slack;
      // The constraint looks at the outer half of the bone only. A sample right next to the joint
      // is the one thing rotation cannot fix -- a strand's root sits on the hip, so the joint is
      // legitimately inside -- and asking for it made the iteration trade the tip away.
      const auto measure = [&](const Vec3 &candidate, Vec3 *slide_to) {
        double deepest = 0.0;
        for (const double slot : {0.5, 0.75, 1.0}) {
          const Vec3 point{head.x + candidate.x * spring.length * slot,
                           head.y + candidate.y * spring.length * slot,
                           head.z + candidate.z * spring.length * slot};
          for (const StrandCapsule &capsule : strand_capsules) {
            if ((capsule.mask & spring.collide) == 0)
              continue;
            const Vec3 axis{capsule.b.x - capsule.a.x, capsule.b.y - capsule.a.y,
                            capsule.b.z - capsule.a.z};
            const Vec3 to_point{point.x - capsule.a.x, point.y - capsule.a.y,
                                point.z - capsule.a.z};
            const double axis_length_squared = Dot(axis, axis);
            const double t =
                axis_length_squared > 1e-9
                    ? (std::max)(0.0, (std::min)(1.0, Dot(to_point, axis) / axis_length_squared))
                    : 0.0;
            const Vec3 closest{capsule.a.x + axis.x * t, capsule.a.y + axis.y * t,
                               capsule.a.z + axis.z * t};
            const Vec3 out{point.x - closest.x, point.y - closest.y, point.z - closest.z};
            const double distance = Length(out);
            const double radius =
                (capsule.end_radius > 0.0
                     ? capsule.radius +
                           (capsule.end_radius - capsule.radius) * t
                     : capsule.radius) +
                spring.collision_radius;
            const double depth = radius - distance;
            // A point sitting *on* the capsule axis has no direction to be pushed along; leave it
            // for the next frame rather than inventing one.
            if (depth > deepest && distance > 0.5) {
              deepest = depth;
              if (slide_to != nullptr)
                *slide_to = Vec3{closest.x + out.x * (radius / distance),
                                 closest.y + out.y * (radius / distance),
                                 closest.z + out.z * (radius / distance)};
            }
          }
        }
        return deepest;
      };
      // Iterated, because aiming the bone at a surface point does not *put* it there: the sampled
      // point sits at `slot * length` along the bone and the vector to the corrected point is a
      // different length, so one pass leaves most of the penetration behind.
      Vec3 slid = direction;
      bool collided = false;
      double initial_worst = -1.0;
      double best_worst = (std::numeric_limits<double>::infinity)();
      Vec3 best_slid = direction;
      for (int attempt = 0; attempt < 12; ++attempt) {
        Vec3 slide_to{};
        const double deepest = measure(slid, &slide_to);
        if (attempt == 0)
          initial_worst = deepest;
        if (deepest < best_worst) {
          best_worst = deepest;
          best_slid = slid;
        }
        if (deepest <= threshold)
          break;
        const Vec3 aim = Unit(Vec3{slide_to.x - head.x, slide_to.y - head.y, slide_to.z - head.z});
        if (Length(aim) < 0.5)
          break;
        // Damped: pulling the whole way onto one surface point satisfies that point and pushes the
        // other sample out of the capsule, so a full step oscillates between them.
        const double blend =
            std::clamp(0.6 * collision_gain, 0.0, 0.95);
        slid = Unit(Vec3{slid.x + (aim.x - slid.x) * blend, slid.y + (aim.y - slid.y) * blend,
                         slid.z + (aim.z - slid.z) * blend});
        collided = true;
      }
      const bool applied = collided && best_worst < initial_worst;
      slid = applied ? best_slid : direction;
      collision_held[spring.index] = applied;
      // Whatever direction the frame ends on is what the document plays, so *that* is the number the
      // report quotes -- including a strand the iteration could not help at all. Counting only the
      // improved ones understated the delivered geometry (self-report 3.0 cm while the played pose
      // measured 3.8 on the same rig), which is exactly the number a probe has to be able to check.
      {
        const double remaining = measure(slid, nullptr);
        if ((spring.collide & kCollideTorso) != 0)
          worst_strand_torso_cm = (std::max)(worst_strand_torso_cm, remaining);
        if ((spring.collide & kCollideLegs) != 0)
          worst_strand_legs_cm = (std::max)(worst_strand_legs_cm, remaining);
        if ((spring.collide & kCollideHead) != 0)
          worst_strand_head_cm = (std::max)(worst_strand_head_cm, remaining);
        if ((spring.collide & kCollideArms) != 0)
          worst_strand_arms_cm = (std::max)(worst_strand_arms_cm, remaining);
      }
      if (!applied)
        continue;
      // Turn the strand onto the corrected direction with the *smallest* rotation that gets there,
      // so whatever the spring (and the hair's roll-holding rule) produced is preserved.
      const Vec3 before = Unit(Rotate(Conjugate(parent_rotation), direction));
      const Vec3 after = Unit(Rotate(Conjugate(parent_rotation), slid));
      const Quat correction = Swing(before, after);
      const Quat local = Normalize(Multiply(correction, frame_local[spring.index]));
      frame_local[spring.index] = local;
      frame_rot[spring.index] = Multiply(parent_rotation, local);
      auto track = output.find(spring.name);
      if (track != output.end() && frame_index < track->second.size())
        track->second[frame_index] = ToArray4(local);
      // The simulation state has to agree with what is played, or the spring drags the strand
      // straight back in. Its *velocity* matters too: correcting the position while the spring keeps
      // the velocity that carried the strand into the volume makes the two trade the same 20-45
      // degrees every frame and the cloth reads as a shake. Remove only the component pointing back
      // into the volume -- clearing the velocity outright made the cloth lag a leg that is pushing
      // it (measured: 41 of 150 frames with a sample inside the leg, against 0 when riding along).
      sim_direction[spring.index] = Rotate(frame_rot[spring.index], spring.axis);
      const Vec3 swing{correction.x, correction.y, correction.z};
      const double swing_length = Length(swing);
      if (swing_length > 1e-9) {
        const Vec3 normal = Rotate(parent_rotation,
                                   Vec3{swing.x / swing_length, swing.y / swing_length,
                                        swing.z / swing_length});
        const double along = Dot(sim_velocity[spring.index], normal);
        if (along < 0.0) {
          Vec3 &velocity = sim_velocity[spring.index];
          velocity = Vec3{velocity.x - normal.x * along, velocity.y - normal.y * along,
                          velocity.z - normal.z * along};
        }
      }
      ++strand_collisions;
    }
    for (const SecondaryBone &spring : secondary) {
      if (spring.material != secondary::MotionMaterial::Ribbon)
        continue;
      const Vec3 current = sim_direction[spring.index];
      if (ribbon_motion_ready[spring.index]) {
        const Vec3 motion = current - ribbon_previous_direction[spring.index];
        if (Length(ribbon_previous_motion[spring.index]) > 1e-6 &&
            Length(motion) > 1e-6 &&
            Dot(ribbon_previous_motion[spring.index], motion) < 0.0)
          ++ribbon_reversals;
        ribbon_previous_motion[spring.index] = motion;
        const double angle =
            std::acos(std::clamp(
                          Dot(current,
                              ribbon_previous_direction[spring.index]),
                          -1.0, 1.0)) *
            180.0 / 3.14159265358979323846;
        if (angle > ribbon_max_frame_degrees) {
          ribbon_max_frame_degrees = angle;
          ribbon_max_frame = frame;
          ribbon_max_bone = spring.name;
        }
      }
      ribbon_previous_direction[spring.index] = current;
      ribbon_motion_ready[spring.index] = true;
    }

    // Root translation: a delta in MMD units relative to the first frame, converted out
    // of MMD axes so the runtime can add it to a target-space translation
    // (mmd2bip.py:1225-1243). The runtime scales it by the live character's leg length.
    if (have_root)
      output_positions.push_back(
          json::array({Round6(root_delta.x), Round6(root_delta.y),
                       Round6(root_delta.z)}));
    ++frame_index;
  }

  // Every bone gets a track: one the file does not drive at all is left to the runtime's
  // fallback, and that is what pinned accessory tips in the scene (mmd2bip.py:1342-1360).
  std::uint32_t rigid_tracks = 0;
  for (const auto &bone : skeleton.bones) {
    if (bone.name.empty() || output.count(bone.name) != 0 ||
        mapped_targets.count(bone.name) != 0)
      continue;
    std::vector<json> track;
    track.reserve(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i)
      track.push_back(ToArray4(rest_local[bone.index]));
    output[bone.name] = std::move(track);
    ++rigid_tracks;
  }

  json document;
  document["schemaVersion"] = 1;
  document["kind"] = "better-pose-motion";
  document["source"] = json{{"vmd", input.vmd_path},
                            {"pmx", input.pmx_path},
                            {"skeleton", input.skeleton_path},
                            {"skeletonBasis", skeleton.basis},
                            {"vmdModel", vmd.model_name}};
  document["targetMesh"] = skeleton.mesh;
  document["rootBone"] = root_mapped ? "Bip001" : "";
  document["fps"] = input.fps;
  document["nativeFps"] = result.native_fps;
  document["firstFrame"] = frames.empty() ? 0 : static_cast<std::uint32_t>(frames.front());
  document["frameCount"] = frames.size();
  document["rootTranslationUnit"] = "mmd";
  document["unitScaleCmPerMmdUnit"] = Round6(unit_scale);
  document["mmdLegLength"] = Round6(mmd_leg_length);
  document["axisChange"] =
      json{{"quaternion", json::array({kAxisChange.x, kAxisChange.y, kAxisChange.z,
                                       kAxisChange.w})},
           {"description", "+90 deg about X (MMD Y-up/Z-forward -> Bip001 left=+X, "
                           "up=+Z, forward=+Y)"}};
  json rests = json::object();
  for (const auto &entry : table) {
    const auto &bone = skeleton.bones[skeleton.by_name.at(entry.target)];
    rests[entry.target] = ToArray4(rest_local[bone.index]);
  }
  document["rest"] = rests;
  json bones_object = json::object();
  document["bones"] = bones_object;
  for (auto &[name, track] : output)
    document["bones"][name] = track;
  document["rootTranslation"] = output_positions;
  if (!vmd.visibility.empty()) {
    json visibility = json::array();
    for (const VmdVisibilityKey &key : vmd.visibility)
      visibility.push_back(json::array({key.frame, key.visible}));
    document["visibility"] = std::move(visibility);
  }
  if (!output_offsets.empty()) {
    json offsets = json::object();
    for (auto &[name, track] : output_offsets)
      offsets[name] = std::move(track);
    document["boneOffsets"] = std::move(offsets);
  }
  if (!arm_ik_outputs.empty()) {
    json targets = json::array();
    for (auto &[side, target] : arm_ik_outputs) {
      json offsets = json::array();
      json rotations = json::array();
      for (const Vec3 &offset : target.offsets)
        offsets.push_back(json::array({Round6(offset.x), Round6(offset.y),
                                       Round6(offset.z)}));
      for (const Quat &rotation : target.rotations)
        rotations.push_back(json::array(
            {Round6(rotation.x), Round6(rotation.y), Round6(rotation.z),
             Round6(rotation.w)}));
      targets.push_back(
          json{{"side", side == 0 ? "L" : "R"},
               {"hand", target.hand},
               {"offsets", std::move(offsets)},
               {"rotations", std::move(rotations)},
               {"maxBendDegrees", target.max_bend_degrees},
               {"followWrist", target.follow_wrist}});
    }
    document["armIkTargets"] = std::move(targets);
  }
  // Facial keys, sparse and by MMD name: which NTE morphs they drive depends
  // on the character, so the runtime maps them (mmd_morph_map.hpp). Frames are
  // the VMD's own, the same timeline `firstFrame` indexes into. Morphs that
  // stay at zero the whole motion are dropped (some motions key 100+ at 0).
  {
    json morphs = json::object();
    for (const auto &track : vmd.morphs) {
      const bool moves = std::any_of(track.keys.begin(), track.keys.end(),
                                     [](const VmdMorphKey &k) { return k.weight > 1e-4F; });
      if (!moves)
        continue;
      json keys = json::array();
      for (const auto &key : track.keys)
        keys.push_back(json::array({key.frame, Round6(key.weight)}));
      morphs[track.name] = std::move(keys);
    }
    if (!morphs.empty())
      document["morphs"] = std::move(morphs);
  }
  // IK evidence: per chain, the worst effector-to-IK-bone distance before and after the
  // solve in MMD units, plus how far the solver had to rotate the chain. Without numbers
  // like these "the legs follow" is a feeling, not a measurement.
  struct IkSummary {
    double before = 0.0;
    double after = 0.0;
    double applied = 0.0;
    std::uint32_t fk_frames = 0;
    std::uint32_t fallback_frames = 0;
  };
  std::map<std::string, IkSummary> ik_summary;
  std::uint32_t total_ik_fallbacks = 0;
  for (const IkReport &entry : ik_reports) {
    IkSummary &summary = ik_summary[entry.name];
    summary.before = (std::max)(summary.before, entry.before);
    summary.after = (std::max)(summary.after, entry.after);
    summary.applied = (std::max)(summary.applied, entry.applied);
    summary.fk_frames += entry.fk_available ? 1U : 0U;
    summary.fallback_frames += entry.fallback_fk ? 1U : 0U;
    total_ik_fallbacks += entry.fallback_fk ? 1U : 0U;
  }
  json ik_json = json::object();
  for (const auto &[name, summary] : ik_summary) {
    ik_json[name] = json{{"gapBefore", Round6(summary.before)},
                         {"gapAfter", Round6(summary.after)},
                         {"solverDegrees", Round6(summary.applied)},
                         {"fkFrames", summary.fk_frames},
                         {"fallbackFrames", summary.fallback_frames}};
  }
  json leg_json = json::object();
  for (std::size_t volume = 0; volume < 4; ++volume) {
    if (leg_radius_cm[volume] > 0.0)
      leg_json[kLegVolumes[volume].key] = Round6(leg_radius_cm[volume]);
  }
  json parent_mappings_json = json::array();
  for (const ManualParentMapping &mapping : input.manual_parent_mappings)
    parent_mappings_json.push_back(
        json{{"source", mapping.source},
             {"target", mapping.target},
             {"anchor", mapping.anchor},
             {"inheritRotation", mapping.inherit_rotation},
             {"inheritPosition", mapping.inherit_position}});
  json finger_controls_json = json::array();
  for (const ManualFingerControl &control : input.manual_finger_controls)
    finger_controls_json.push_back(
        json{{"source", control.source},
             {"kind",
              control.kind == FingerControlKind::Grip ? "grip" : "spread"},
             {"side", control.side},
             {"finger", control.finger},
             {"inputAxis", control.input_axis},
             {"gain", Round6(control.gain)},
             {"paired", control.paired}});
  json arm_ik_controls_json = json::array();
  for (const ManualArmIkControl &control : input.manual_arm_ik_controls)
    arm_ik_controls_json.push_back(
        json{{"source", control.source},
             {"side", control.side},
             {"paired", control.paired},
             {"gain", Round6(control.gain)},
             {"maxBendDegrees", Round6(control.max_bend_degrees)},
             {"followWrist", control.follow_wrist}});
  const auto capsule_count = [&](const std::uint8_t mask) {
    return static_cast<std::size_t>(std::count_if(
        strand_capsules.begin(), strand_capsules.end(),
        [mask](const StrandCapsule &capsule) { return (capsule.mask & mask) != 0; }));
  };
  const std::uint32_t self_contact_count =
      static_cast<std::uint32_t>(std::count_if(
          self_contact_state.begin(), self_contact_state.end(),
          [](const std::uint8_t value) { return value != 0; }));
  std::array<std::uint32_t, secondary::kMotionMaterialCount>
      self_candidates_by_material{};
  std::array<std::uint32_t, secondary::kMotionMaterialCount>
      self_contacts_by_material{};
  for (std::size_t link_index{}; link_index < self_links.size();
       ++link_index) {
    const auto &link = self_links[link_index];
    const auto material_a = static_cast<std::size_t>(
        distance_candidates[link.a].material);
    const auto material_b = static_cast<std::size_t>(
        distance_candidates[link.b].material);
    ++self_candidates_by_material[material_a];
    if (material_b != material_a)
      ++self_candidates_by_material[material_b];
    if (link_index < self_contact_state.size() &&
        self_contact_state[link_index] != 0) {
      ++self_contacts_by_material[material_a];
      if (material_b != material_a)
        ++self_contacts_by_material[material_b];
    }
  }
  json self_candidates_json = json::object();
  json self_contacts_json = json::object();
  for (std::size_t index{}; index != secondary::kMotionMaterialCount;
       ++index) {
    const std::string key(secondary::kMotionMaterialKeys[index]);
    self_candidates_json[key] = self_candidates_by_material[index];
    self_contacts_json[key] = self_contacts_by_material[index];
  }
  document["diagnostics"] =
      json{{"mappedBones", table.size()},
           {"directionCorrected", direction_corrected},
           {"directionSkipped", direction_skipped},
           {"frameCount", frames.size()},
           {"nativeFps", result.native_fps},
           {"rigidTracks", rigid_tracks},
           {"secondaryBones", secondary.size()},
           {"nativeSecondaryOverrides", native_secondary_overrides},
           {"distanceConstraintLinks", distance_links.size()},
           {"distanceConstraintCorrections", distance_corrections},
           {"selfCollisionLinks", self_links.size()},
           {"selfCollisionCandidatePairs", self_candidate_pairs},
           {"selfCollisionContacts", self_contact_count},
           {"selfCollisionCandidatesByMaterial",
            std::move(self_candidates_json)},
           {"selfCollisionContactsByMaterial",
            std::move(self_contacts_json)},
           {"selfCollisionCorrections", self_collision_corrections},
           {"sleeveBones", sleeve_bones},
           {"strandCollisions", strand_collisions},
           {"strandWorstTorsoCm", Round6(worst_strand_torso_cm)},
           {"strandWorstLegsCm", Round6(worst_strand_legs_cm)},
            {"strandWorstHeadCm", Round6(worst_strand_head_cm)},
            {"strandWorstArmsCm", Round6(worst_strand_arms_cm)},
            {"strandCapsulesTorso", capsule_count(kCollideTorso)},
            {"strandCapsulesHead", capsule_count(kCollideHead)},
            {"strandCapsulesArms", capsule_count(kCollideArms)},
            {"strandCapsulesLegs", capsule_count(kCollideLegs)},
           // The leg volumes' radii, measured from the rig's rest pose: a probe replaying this
           // document needs them to measure the same geometry the correction used.
           {"strandLegRadiusCm", leg_json},
           {"twistBones", twist_bones.size()},
           {"ikChains", ik_json},
           {"ikFkFallbackFrames", total_ik_fallbacks},
           {"manualParentMappings", parent_mappings_json},
           {"manualFingerControls", finger_controls_json},
           {"manualArmIkControls", arm_ik_controls_json},
           {"armIkTargets", arm_ik_outputs.size()},
           {"phase", 2},
           {"rootTrack", root_track_name},
           {"rootFromData", root_from_data},
           {"missingStandardBones", missing_bones},
           {"note", "phase two: accessory spring, ornament and swing-only rules, and the "
                    "twist helpers are included; finger chains are mapped by cumulative "
                    "length"}};

  result.motion_json = document.dump(-1, ' ', false, json::error_handler_t::replace);
  result.ok = true;
  result.mapped_bones = static_cast<std::uint32_t>(table.size());
  result.frame_count = static_cast<std::uint32_t>(frames.size());
  result.first_frame = frames.empty() ? 0 : static_cast<std::uint32_t>(frames.front());
  result.bone_tracks = static_cast<std::uint32_t>(output.size());
  result.distance_constraint_links =
      static_cast<std::uint32_t>(distance_links.size());
  result.distance_constraint_corrections =
      distance_corrections;
  result.self_collision_links =
      static_cast<std::uint32_t>(self_links.size());
  result.self_collision_corrections =
      self_collision_corrections;
  result.self_collision_contacts =
      self_contact_count;
  result.self_collision_candidates_by_material =
      self_candidates_by_material;
  result.self_collision_contacts_by_material =
      self_contacts_by_material;
  char strand_worst_note[160]{};
  std::snprintf(strand_worst_note, sizeof(strand_worst_note),
                "torso %.1f / head %.1f / arms %.1f / legs %.1f cm "
                "(leg radius %.1f from rest)",
                worst_strand_torso_cm, worst_strand_head_cm, worst_strand_arms_cm,
                worst_strand_legs_cm, leg_clearance_cm);
  result.report = "mmd2bip(c++): nativeFps " + std::to_string(result.native_fps) + ", root " + (root_track_name.empty() ? std::string("(none)") : root_track_name + (root_from_data ? std::string("(data)") : std::string())) + ", missing[" + (missing_bones.empty() ? std::string("-") : missing_bones) + "], tracks " + std::to_string(output.size()) + ", mapped " +
                  std::to_string(table.size()) + ", frames " +
                  std::to_string(frames.size()) + ", rigid " +
                  std::to_string(rigid_tracks) + ", secondary " +
                  std::to_string(secondary.size()) + ", native springs " +
                  std::to_string(native_secondary_overrides) +
                  ", distance links " +
                  std::to_string(distance_links.size()) +
                  " (" + std::to_string(distance_corrections) +
                  " corrections), self links " +
                  std::to_string(self_links.size()) + " (" +
                  std::to_string(self_collision_corrections) +
                  " corrections), sleeves " +
                  std::to_string(sleeve_bones) + ", collisions " +
                  std::to_string(strand_collisions) + ", worst strand " + strand_worst_note +
                  ", twist " +
                  std::to_string(twist_bones.size()) + ", ik " +
                  std::to_string(ik_summary.size()) + ", legIK " +
                  std::to_string(leg_solves.size()) + ", vmd walk " +
                  (vmd.walk_exact ? "exact" : "MISMATCH") + "\n";
  if (ribbon_bones != 0) {
    char ribbon_note[224]{};
    std::snprintf(
        ribbon_note, sizeof(ribbon_note),
        "  ribbon motion: bones %u, max %.2f deg at frame %.0f (%s), "
        "reversals %llu\n",
        static_cast<unsigned>(ribbon_bones), ribbon_max_frame_degrees,
        ribbon_max_frame,
        ribbon_max_bone.empty() ? "n/a" : ribbon_max_bone.c_str(),
        static_cast<unsigned long long>(ribbon_reversals));
    result.report += ribbon_note;
  }
  for (const auto &[name, summary] : ik_summary) {
    result.report += "  ik " + name + ": gap " + std::to_string(summary.before) + " -> " +
                     std::to_string(summary.after) + " (solver " +
                     std::to_string(summary.applied) + " deg; FK fallback " +
                     std::to_string(summary.fallback_frames) + "/" +
                     std::to_string(summary.fk_frames) + " frames)\n";
  }
  return result;
}

}  // namespace better_pose::mmd2bip
