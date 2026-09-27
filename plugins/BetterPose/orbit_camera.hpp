#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// A mouse-only orbit camera for posing, MMD style: it circles a focus point
// at a distance, and every control moves the camera, never the character.
// Rotations are UE rotators in degrees (pitch up, yaw about +Z, roll), the
// camera looks along +X of its own frame, +Y is its right and +Z its up.
namespace better_pose::orbit {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDegrees = 180.0 / kPi;
inline constexpr double kMinimumDistance = 20.0;    // cm
inline constexpr double kMaximumDistance = 3000.0;  // cm
inline constexpr double kMaximumPitch = 89.0;       // degrees, short of the pole
// Horizontal field of view, degrees (UE's convention). 15 is a long lens for
// a close-up of a hand or a face; 120 is wider than any gameplay camera.
inline constexpr float kMinimumFov = 15.0F;
inline constexpr float kMaximumFov = 120.0F;

struct Orbit {
  std::array<double, 3> focus{};  // world, cm
  double yaw{};                   // degrees: the direction the camera looks
  double pitch{};                 // degrees: + looks up
  double distance{300.0};         // cm, camera to focus
};

struct View {
  std::array<double, 3> location{};
  std::array<double, 3> rotation{};  // pitch, yaw, roll
};

inline std::array<double, 3> Forward(const double pitch, const double yaw) noexcept {
  const double p = pitch / kDegrees;
  const double y = yaw / kDegrees;
  return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

inline View ViewOf(const Orbit &orbit) noexcept {
  const auto forward = Forward(orbit.pitch, orbit.yaw);
  View view;
  for (std::size_t axis{}; axis != 3; ++axis)
    view.location[axis] = orbit.focus[axis] - forward[axis] * orbit.distance;
  view.rotation = {orbit.pitch, orbit.yaw, 0.0};
  return view;
}

// Start from the view the player already has, circling the point `distance`
// ahead of it, so turning the pose camera on does not jump the shot.
inline Orbit FromView(const std::array<double, 3> &location,
                      const std::array<double, 3> &rotation, const double distance) noexcept {
  Orbit orbit;
  orbit.pitch = std::clamp(rotation[0], -kMaximumPitch, kMaximumPitch);
  orbit.yaw = rotation[1];
  orbit.distance = std::clamp(distance, kMinimumDistance, kMaximumDistance);
  const auto forward = Forward(orbit.pitch, orbit.yaw);
  for (std::size_t axis{}; axis != 3; ++axis)
    orbit.focus[axis] = location[axis] + forward[axis] * orbit.distance;
  return orbit;
}

// Right drag: dragging right swings the camera round to the left of the
// subject (the subject turns right on screen), dragging down looks from above.
inline void Rotate(Orbit &orbit, const double dx_pixels, const double dy_pixels) noexcept {
  constexpr double kDegreesPerPixel = 0.3;
  orbit.yaw += dx_pixels * kDegreesPerPixel;
  orbit.yaw = std::remainder(orbit.yaw, 360.0);
  orbit.pitch = std::clamp(orbit.pitch - dy_pixels * kDegreesPerPixel, -kMaximumPitch,
                           kMaximumPitch);
}

// Middle drag: the focus follows the cursor in the view plane, scaled so the
// point under the cursor stays under it (one pixel of travel moves the focus
// by the size of one pixel at the focus distance). `pixels_per_cm_at_one_cm`
// is the projection's focal length in pixels.
inline void Pan(Orbit &orbit, const double dx_pixels, const double dy_pixels,
                const double focal_pixels) noexcept {
  if (!(focal_pixels > 1e-6))
    return;
  const double y = orbit.yaw / kDegrees;
  const double p = orbit.pitch / kDegrees;
  const std::array<double, 3> right{-std::sin(y), std::cos(y), 0.0};
  const std::array<double, 3> up{-std::sin(p) * std::cos(y), -std::sin(p) * std::sin(y),
                                 std::cos(p)};
  const double cm_per_pixel = orbit.distance / focal_pixels;
  for (std::size_t axis{}; axis != 3; ++axis)
    orbit.focus[axis] += (-right[axis] * dx_pixels + up[axis] * dy_pixels) * cm_per_pixel;
}

// Wheel: each notch (120 units) is 10 % nearer or farther.
inline void Zoom(Orbit &orbit, const double wheel_units) noexcept {
  orbit.distance = std::clamp(orbit.distance * std::pow(0.9, wheel_units / 120.0),
                              kMinimumDistance, kMaximumDistance);
}

// Frame a point: keep the angle, move the focus, keep the distance.
inline void Focus(Orbit &orbit, const std::array<double, 3> &point) noexcept {
  orbit.focus = point;
}

}  // namespace better_pose::orbit
