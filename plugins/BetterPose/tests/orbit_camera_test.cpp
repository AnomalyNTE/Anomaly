#include "../orbit_camera.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using namespace better_pose::orbit;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

double Distance(const std::array<double, 3> &a, const std::array<double, 3> &b) {
  return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                   (a[2] - b[2]) * (a[2] - b[2]));
}

// Project `point` through `view` like UE: +X forward, +Y right, +Z up, pinhole.
bool Project(const View &view, const std::array<double, 3> &point, const double focal,
             double screen[2]) {
  const double p = view.rotation[0] / kDegrees;
  const double y = view.rotation[1] / kDegrees;
  const std::array<double, 3> forward{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y),
                                      std::sin(p)};
  const std::array<double, 3> right{-std::sin(y), std::cos(y), 0.0};
  const std::array<double, 3> up{-std::sin(p) * std::cos(y), -std::sin(p) * std::sin(y),
                                 std::cos(p)};
  const std::array<double, 3> d{point[0] - view.location[0], point[1] - view.location[1],
                                point[2] - view.location[2]};
  const double depth = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
  if (depth <= 1.0)
    return false;
  screen[0] = 960.0 + focal * (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / depth;
  screen[1] = 540.0 - focal * (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / depth;
  return true;
}

void StartsWhereThePlayerLooks() {
  const std::array<double, 3> location{100.0, -50.0, 180.0};
  const std::array<double, 3> rotation{-12.0, 35.0, 0.0};
  const Orbit orbit = FromView(location, rotation, 320.0);
  const View view = ViewOf(orbit);
  Check(Distance(view.location, location) < 1e-9, "switching on keeps the camera position");
  Check(std::abs(view.rotation[0] - rotation[0]) < 1e-9 &&
            std::abs(view.rotation[1] - rotation[1]) < 1e-9,
        "switching on keeps the camera angle");
  Check(std::abs(Distance(view.location, orbit.focus) - 320.0) < 1e-9,
        "the focus is the requested distance ahead");
  const Orbit steep = FromView(location, {95.0, 0.0, 0.0}, 5.0);
  Check(steep.pitch <= kMaximumPitch && steep.distance >= kMinimumDistance,
        "an out-of-range start is clamped");
}

void RotateKeepsTheFocus() {
  Orbit orbit = FromView({0, 0, 150}, {-10, 0, 0}, 300);
  const auto focus = orbit.focus;
  double before[2]{};
  Project(ViewOf(orbit), focus, 1000.0, before);
  for (int frame = 0; frame != 50; ++frame)
    Rotate(orbit, 7.0, -3.0);
  const View view = ViewOf(orbit);
  Check(std::abs(Distance(view.location, focus) - 300.0) < 1e-9, "orbiting keeps the distance");
  double after[2]{};
  Check(Project(view, focus, 1000.0, after), "the focus stays in front");
  Check(std::abs(after[0] - 960.0) < 1e-6 && std::abs(after[1] - 540.0) < 1e-6,
        "the focus stays in the middle of the screen");
  Check(orbit.yaw != 0.0, "dragging right turns the camera");
  for (int frame = 0; frame != 1000; ++frame)
    Rotate(orbit, 0.0, -50.0);
  Check(orbit.pitch <= kMaximumPitch, "pitch stops short of straight up");
  Check(std::abs(orbit.yaw) <= 180.0, "yaw stays wrapped");
}

void PanFollowsTheCursor() {
  Orbit orbit = FromView({0, 0, 150}, {-20, 60, 0}, 250);
  const double focal = 1000.0;
  const auto point = orbit.focus;  // the point under the cursor at the centre
  double before[2]{};
  Project(ViewOf(orbit), point, focal, before);
  Pan(orbit, 40.0, -25.0, focal);
  double after[2]{};
  Check(Project(ViewOf(orbit), point, focal, after), "panned point projects");
  Check(std::abs(after[0] - before[0] - 40.0) < 1e-6 && std::abs(after[1] - before[1] + 25.0) < 1e-6,
        "the point under the cursor moves with the cursor");
  const double distance = orbit.distance;
  Pan(orbit, 10, 10, 0.0);
  Check(orbit.distance == distance, "no focal length, no pan");
}

void ZoomIsBounded() {
  Orbit orbit;
  orbit.distance = 300.0;
  Zoom(orbit, 120.0);
  Check(std::abs(orbit.distance - 270.0) < 1e-9, "one notch forward is 10 % nearer");
  Zoom(orbit, -120.0);
  Check(std::abs(orbit.distance - 300.0) < 1e-9, "one notch back undoes it");
  Zoom(orbit, 120.0 * 200.0);
  Check(orbit.distance == kMinimumDistance, "zoom in stops at the minimum");
  Zoom(orbit, -120.0 * 200.0);
  Check(orbit.distance == kMaximumDistance, "zoom out stops at the maximum");
}

void FocusMovesOnlyTheFocus() {
  Orbit orbit = FromView({0, 0, 150}, {-15, 30, 0}, 280);
  const double pitch = orbit.pitch;
  const double yaw = orbit.yaw;
  Focus(orbit, {50.0, 20.0, 120.0});
  const View view = ViewOf(orbit);
  double screen[2]{};
  Check(Project(view, {50.0, 20.0, 120.0}, 1000.0, screen) &&
            std::abs(screen[0] - 960.0) < 1e-6 && std::abs(screen[1] - 540.0) < 1e-6,
        "the focused joint lands in the middle");
  Check(orbit.pitch == pitch && orbit.yaw == yaw && orbit.distance == 280.0,
        "focusing keeps the angle and distance");
}
}  // namespace

void FovBounds() {
  Check(kMinimumFov > 0.0F && kMinimumFov < kMaximumFov && kMaximumFov < 180.0F,
        "the lens range is a valid horizontal field of view");
}

int main() {
  FovBounds();
  StartsWhereThePlayerLooks();
  RotateKeepsTheFocus();
  PanFollowsTheCursor();
  ZoomIsBounded();
  FocusMovesOnlyTheFocus();
  std::cout << "PASS fov bounds, start from view, orbit, pan under cursor, zoom bounds, focus\n";
  return 0;
}
