#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace better_pose::limb_calibration {

enum class Limb : std::size_t {
  LeftArm,
  RightArm,
  LeftLeg,
  RightLeg,
  Count,
};

enum class Status : int {
  NotApplicable = -1,
  Original = 0,
  Applied = 1,
  Clamped = 2,
};

inline constexpr std::size_t kLimbCount = static_cast<std::size_t>(Limb::Count);

inline constexpr std::array<std::string_view, kLimbCount> kLimbKeys = {
    "left_arm", "right_arm", "left_leg", "right_leg"};
inline constexpr std::array<std::string_view, kLimbCount> kLimbLabels = {
    "Left arm", "Right arm", "Left leg", "Right leg"};

struct Settings {
  bool enabled = false;
  float upper = 1.0F;
  float lower = 1.0F;
  // Fractions of total limb length, in anatomical left/up/forward axes.
  std::array<float, 3> offset{0.0F, 0.0F, 0.0F};

  bool IsNeutral() const {
    return std::abs(upper - 1.0F) <= 1e-7F &&
           std::abs(lower - 1.0F) <= 1e-7F &&
           std::abs(offset[0]) <= 1e-7F &&
           std::abs(offset[1]) <= 1e-7F &&
           std::abs(offset[2]) <= 1e-7F;
  }

  bool IsDefault() const { return !enabled && IsNeutral(); }
};

struct AllSettings {
  std::array<Settings, kLimbCount> limbs{};
};

inline constexpr std::array<std::array<std::string_view, 3>, kLimbCount>
    kBoneNames = {{
        {"Bip001-L-UpperArm", "Bip001-L-Forearm", "Bip001-L-Hand"},
        {"Bip001-R-UpperArm", "Bip001-R-Forearm", "Bip001-R-Hand"},
        {"Bip001-L-Thigh", "Bip001-L-Calf", "Bip001-L-Foot"},
        {"Bip001-R-Thigh", "Bip001-R-Calf", "Bip001-R-Foot"},
    }};

}  // namespace better_pose::limb_calibration
