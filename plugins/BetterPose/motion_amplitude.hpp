#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace better_pose::motion_amplitude {

enum class Part : std::size_t {
  Torso,
  Head,
  LeftArm,
  RightArm,
  LeftHand,
  RightHand,
  LeftLeg,
  RightLeg,
  LeftFoot,
  RightFoot,
  Count,
};

inline constexpr std::size_t kPartCount = static_cast<std::size_t>(Part::Count);

inline constexpr std::array<std::string_view, kPartCount> kPartKeys = {
    "torso", "head", "left_arm", "right_arm", "left_hand",
    "right_hand", "left_leg", "right_leg", "left_foot", "right_foot"};

inline constexpr std::array<std::string_view, kPartCount> kPartLabels = {
    "Torso", "Head", "Left arm", "Right arm", "Left hand",
    "Right hand", "Left leg", "Right leg", "Left foot", "Right foot"};

inline float SafeFactor(const float value) {
  return std::isfinite(value) ? std::clamp(value, 0.0F, 2.0F) : 1.0F;
}

struct Settings {
  float master = 1.0F;
  std::array<float, kPartCount> parts = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                                         1.0F, 1.0F, 1.0F, 1.0F, 1.0F};

  float Factor(const Part part) const {
    if (part < Part::Torso || part >= Part::Count)
      return 1.0F;
    return SafeFactor(SafeFactor(master) *
                      SafeFactor(parts[static_cast<std::size_t>(part)]));
  }
};

inline Part PartForBone(const std::string_view bone_name) {
  std::string lower(bone_name);
  for (char &c : lower) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + ('a' - 'A'));
  }
  const auto contains = [&lower](const std::string_view needle) {
    return lower.find(needle) != std::string::npos;
  };
  const bool left = contains("-l-") || contains("_l_") ||
                    lower.rfind("l_", 0) == 0 || lower.rfind("l-", 0) == 0;
  const bool right = contains("-r-") || contains("_r_") ||
                     lower.rfind("r_", 0) == 0 || lower.rfind("r-", 0) == 0;

  if (contains("head") || contains("neck") || contains("jaw") ||
      contains("eye"))
    return Part::Head;
  if (contains("finger") || contains("hand") || contains("thumb"))
    return left ? Part::LeftHand : right ? Part::RightHand : Part::Count;
  if (contains("foot") || contains("toe"))
    return left ? Part::LeftFoot : right ? Part::RightFoot : Part::Count;
  if (contains("thigh") || contains("calf") || contains("leg"))
    return left ? Part::LeftLeg : right ? Part::RightLeg : Part::Count;
  if (contains("clavicle") || contains("upperarm") ||
      contains("forearm") || contains("uparmtwist") ||
      contains("foretwist") || contains("shoulder"))
    return left ? Part::LeftArm : right ? Part::RightArm : Part::Count;
  if (contains("pelvis") || contains("spine") || lower == "bip001" ||
      lower == "root")
    return Part::Torso;
  return Part::Count;
}

using Quat = std::array<double, 4>;  // x, y, z, w

inline Quat Normalize(const Quat &value) {
  const double length = std::sqrt(value[0] * value[0] + value[1] * value[1] +
                                  value[2] * value[2] + value[3] * value[3]);
  if (!(length > 1e-12) || !std::isfinite(length))
    return {0.0, 0.0, 0.0, 1.0};
  return {value[0] / length, value[1] / length, value[2] / length,
          value[3] / length};
}

inline Quat Conjugate(const Quat &value) {
  return {-value[0], -value[1], -value[2], value[3]};
}

inline Quat Multiply(const Quat &a, const Quat &b) {
  return {
      a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
      a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
      a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
      a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// Scale the shortest local rotation away from the neutral/reference pose. This
// is applied after retargeting and IK, so 100% is exactly the converted motion.
inline Quat ScaleRotation(Quat neutral, const Quat &animated, float factor) {
  neutral = Normalize(neutral);
  const Quat target = Normalize(animated);
  factor = SafeFactor(factor);
  if (std::abs(factor - 1.0F) <= 1e-7F)
    return target;
  if (factor <= 1e-7F)
    return neutral;

  Quat delta = Normalize(Multiply(Conjugate(neutral), target));
  if (delta[3] < 0.0) {
    for (double &value : delta)
      value = -value;
  }
  const double sine =
      std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] +
                delta[2] * delta[2]);
  if (sine <= 1e-9)
    return neutral;
  const double angle = 2.0 * std::atan2(sine, delta[3]);
  const double half = angle * static_cast<double>(factor) * 0.5;
  const double axis_scale = std::sin(half) / sine;
  const Quat scaled{delta[0] * axis_scale, delta[1] * axis_scale,
                    delta[2] * axis_scale, std::cos(half)};
  return Normalize(Multiply(neutral, scaled));
}

}  // namespace better_pose::motion_amplitude
