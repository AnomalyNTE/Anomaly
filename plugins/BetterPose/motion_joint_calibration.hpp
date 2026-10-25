#pragma once

#include "motion_amplitude.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace better_pose::motion_joint {

struct Joint {
  float ratio = 1.0F;
  std::array<float, 3> offset_degrees{0.0F, 0.0F, 0.0F};
  std::array<bool, 3> mirror_offset{false, false, false};

  bool IsDefault() const {
    return std::abs(ratio - 1.0F) <= 1e-7F &&
           offset_degrees[0] == 0.0F && offset_degrees[1] == 0.0F &&
           offset_degrees[2] == 0.0F &&
           !mirror_offset[0] && !mirror_offset[1] && !mirror_offset[2];
  }
};

struct Settings {
  bool enabled = false;
  bool linked = true;
  std::map<std::string, Joint> joints;
};

inline std::string MirrorBoneName(const std::string_view name) {
  std::string mirrored(name);
  const auto replace_once = [&mirrored](const std::string_view from,
                                        const std::string_view to) {
    const std::size_t at = mirrored.find(from);
    if (at == std::string::npos)
      return false;
    mirrored.replace(at, from.size(), to);
    return true;
  };
  for (const auto &pair :
       {std::pair<std::string_view, std::string_view>{"-L-", "-R-"},
        std::pair<std::string_view, std::string_view>{"-R-", "-L-"},
        std::pair<std::string_view, std::string_view>{"-l-", "-r-"},
        std::pair<std::string_view, std::string_view>{"-r-", "-l-"},
        std::pair<std::string_view, std::string_view>{"_L_", "_R_"},
        std::pair<std::string_view, std::string_view>{"_R_", "_L_"},
        std::pair<std::string_view, std::string_view>{"_l_", "_r_"},
        std::pair<std::string_view, std::string_view>{"_r_", "_l_"}}) {
    if (replace_once(pair.first, pair.second))
      return mirrored;
  }

  const auto swap_suffix = [&](const std::string_view left,
                               const std::string_view right)
      -> std::string {
    if (mirrored.size() >= left.size() &&
        mirrored.compare(mirrored.size() - left.size(), left.size(), left) == 0) {
      return mirrored.substr(0, mirrored.size() - left.size()) + std::string(right);
    }
    if (mirrored.size() >= right.size() &&
        mirrored.compare(mirrored.size() - right.size(), right.size(), right) == 0) {
      return mirrored.substr(0, mirrored.size() - right.size()) + std::string(left);
    }
    return {};
  };
  for (const auto &pair :
       {std::pair<std::string_view, std::string_view>{"_L", "_R"},
        std::pair<std::string_view, std::string_view>{"_l", "_r"},
        std::pair<std::string_view, std::string_view>{".L", ".R"},
        std::pair<std::string_view, std::string_view>{".l", ".r"},
        std::pair<std::string_view, std::string_view>{".Left", ".Right"},
        std::pair<std::string_view, std::string_view>{".left", ".right"}}) {
    std::string result = swap_suffix(pair.first, pair.second);
    if (!result.empty())
      return result;
  }
  return {};
}

// Unity/MMD tooling convention, matching Endfield-Poser's local calibration.
inline motion_amplitude::Quat RotationFromEulerDegreesYXZ(
    const std::array<float, 3> &degrees) {
  constexpr double kDegrees = 3.14159265358979323846 / 180.0;
  const double x = degrees[0] * kDegrees * 0.5;
  const double y = degrees[1] * kDegrees * 0.5;
  const double z = degrees[2] * kDegrees * 0.5;
  const double cx = std::cos(x), sx = std::sin(x);
  const double cy = std::cos(y), sy = std::sin(y);
  const double cz = std::cos(z), sz = std::sin(z);
  return motion_amplitude::Normalize(
      {sx * cy * cz + cx * sy * sz, cx * sy * cz - sx * cy * sz,
       cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz});
}

inline Joint MirroredJoint(const Joint &source) {
  Joint result = source;
  for (std::size_t axis{}; axis != 3; ++axis)
    if (source.mirror_offset[axis])
      result.offset_degrees[axis] = -source.offset_degrees[axis];
  return result;
}

}  // namespace better_pose::motion_joint
