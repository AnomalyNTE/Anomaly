#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace better_pose::motion_travel {

inline constexpr std::size_t kAxisCount = 3;

struct Settings {
  // Author-space axes: X left/right, Y forward/back, Z up/down.
  std::array<float, kAxisCount> scale{1.0F, 1.0F, 1.0F};

  bool IsDefault() const {
    for (const float value : scale) {
      if (std::abs(value - 1.0F) > 1e-7F)
        return false;
    }
    return true;
  }
};

inline float SafeScale(const float value) {
  return std::isfinite(value) ? std::clamp(value, 0.0F, 2.0F) : 1.0F;
}

inline Settings ClampSettings(Settings settings) {
  for (float &value : settings.scale)
    value = SafeScale(value);
  return settings;
}

inline std::array<double, kAxisCount> Apply(
    const std::array<double, kAxisCount> &authored,
    const Settings &settings, const bool lock_planar) {
  std::array<double, kAxisCount> applied{};
  for (std::size_t axis{}; axis != kAxisCount; ++axis)
    applied[axis] =
        authored[axis] * static_cast<double>(SafeScale(settings.scale[axis]));
  if (lock_planar) {
    applied[0] = 0.0;
    applied[1] = 0.0;
  }
  return applied;
}

}  // namespace better_pose::motion_travel
