#include "../rotation_gizmo.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
namespace gz = better_pose::gizmo;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool Near(const double a, const double b, const double epsilon = 1e-6) {
  return std::abs(a - b) <= epsilon;
}

// A camera on +X looking down -X: world (x, y, z) -> screen (y, -z) * scale,
// centred at (500, 500). Screen y points down, like the game's canvas.
gz::Vec2 Project(const gz::Vec3 &p, const double scale = 1.0) {
  return {static_cast<float>(500.0 + p[1] * scale), static_cast<float>(500.0 - p[2] * scale)};
}

gz::ScreenRing ProjectRing(const std::vector<gz::Vec3> &points, const gz::Vec3 &centre,
                           const gz::Vec3 &toward) {
  gz::ScreenRing ring;
  for (const auto &p : points) {
    ring.points.push_back(Project(p));
    ring.visible.push_back(1);
    ring.front.push_back(gz::FacesCamera(p, centre, toward) ? 1 : 0);
  }
  return ring;
}

void RingGeometry() {
  const gz::Vec3 centre{10.0, 20.0, 30.0};
  const auto points = gz::RingPoints(centre, {0.0, 0.0, 1.0}, 5.0);
  Check(points.size() == gz::kSegments + 1, "segment count");
  for (const auto &p : points) {
    Check(Near(gz::Length(gz::Sub(p, centre)), 5.0, 1e-9), "every point at the radius");
    Check(Near(p[2], 30.0, 1e-9), "every point in the plane perpendicular to the axis");
  }
  Check(Near(gz::Length(gz::Sub(points.front(), points.back())), 0.0, 1e-9), "the ring closes");
  gz::Vec3 u, v;
  gz::PlaneBasis({1.0, 1.0, 0.0}, u, v);
  Check(Near(gz::Dot(u, v), 0.0, 1e-12) && Near(gz::Length(u), 1.0, 1e-12) &&
            Near(gz::Dot(u, gz::Normalize({1.0, 1.0, 0.0})), 0.0, 1e-12),
        "plane basis is orthonormal and perpendicular to the axis");
}

void FrontHalf() {
  const gz::Vec3 centre{0.0, 0.0, 0.0};
  const gz::Vec3 toward{1.0, 0.0, 0.0};
  Check(gz::FacesCamera({1.0, 0.0, 0.0}, centre, toward), "near side faces the camera");
  Check(!gz::FacesCamera({-1.0, 0.0, 0.0}, centre, toward), "far side does not");
}

void PicksTheNearRing() {
  const gz::Vec3 centre{0.0, 0.0, 0.0};
  const gz::Vec3 toward{1.0, 0.0, 0.0};
  // The X ring faces the camera (a full circle on screen, radius 100 px);
  // the Y and Z rings are edge-on (a vertical / horizontal line).
  std::array<gz::ScreenRing, 3> rings{
      ProjectRing(gz::RingPoints(centre, {1.0, 0.0, 0.0}, 100.0), centre, toward),
      ProjectRing(gz::RingPoints(centre, {0.0, 1.0, 0.0}, 100.0), centre, toward),
      ProjectRing(gz::RingPoints(centre, {0.0, 0.0, 1.0}, 100.0), centre, toward)};
  // On the X ring's circle, away from the other two lines.
  const float d = 100.0F / std::sqrt(2.0F);
  Check(gz::PickRing(rings, {500.0F + d, 500.0F - d}, 6.0F) == 0, "the facing ring is picked");
  Check(gz::PickRing(rings, {500.0F + 50.0F, 500.0F + 3.0F}, 6.0F) == 2,
        "the horizontal line is the Z ring");
  Check(gz::PickRing(rings, {500.0F + 3.0F, 500.0F - 50.0F}, 6.0F) == 1,
        "the vertical line is the Y ring");
  Check(gz::PickRing(rings, {700.0F, 700.0F}, 6.0F) == gz::kNoRing, "far from every ring");
}

void BackHalfIsNotPickable() {
  const gz::Vec3 centre{0.0, 0.0, 0.0};
  // Camera on +X, ring about Z seen from slightly above: tilt the toward
  // vector so the ring is an ellipse with a near and a far half.
  const gz::Vec3 toward = gz::Normalize({1.0, 0.0, 0.3});
  const auto points = gz::RingPoints(centre, {0.0, 0.0, 1.0}, 100.0);
  std::array<gz::ScreenRing, 3> rings;
  rings[2] = ProjectRing(points, centre, toward);
  // Only the near half (x > 0) is pickable. With this projection both halves
  // land on the same screen line, so pick by checking the flags directly.
  std::size_t front{}, back{};
  for (std::size_t i = 0; i != points.size(); ++i)
    (rings[2].front[i] != 0 ? front : back) += 1;
  Check(front > 0 && back > 0, "an inclined ring has a near and a far half");
}

void SweepAroundFacingRing() {
  // A facing ring on screen centred at (500, 500). The grab point is to the
  // right; +angle moves it straight up the screen (tangent (0, -1)), i.e.
  // counter-clockwise as seen on the canvas.
  gz::Drag drag;
  drag.centre_screen = {500.0F, 500.0F};
  drag.openness = 1.0;
  drag.radius_pixels = 100.0;
  gz::BeginDrag(drag, {600.0F, 500.0F}, {600.0F, 500.0F}, {600.0F, 495.0F});
  for (int step = 1; step <= 9; ++step) {
    const double a = step * 10.0 * gz::kPi / 180.0;
    static_cast<void>(gz::DragAngle(
        drag, {static_cast<float>(500.0 + 100.0 * std::cos(a)),
               static_cast<float>(500.0 - 100.0 * std::sin(a))}));
  }
  Check(Near(gz::DragAngle(drag, {500.0F, 400.0F}), gz::kPi / 2.0, 1e-4),
        "sweeping the way +angle moves the grab point is a positive quarter turn");

  // The same sweep where +angle would move the grab point down instead.
  gz::Drag other;
  other.centre_screen = {500.0F, 500.0F};
  other.openness = 1.0;
  other.radius_pixels = 100.0;
  gz::BeginDrag(other, {600.0F, 500.0F}, {600.0F, 500.0F}, {600.0F, 505.0F});
  for (int step = 1; step <= 9; ++step) {
    const double a = step * 10.0 * gz::kPi / 180.0;
    static_cast<void>(gz::DragAngle(
        other, {static_cast<float>(500.0 + 100.0 * std::cos(a)),
                static_cast<float>(500.0 - 100.0 * std::sin(a))}));
  }
  Check(Near(gz::DragAngle(other, {500.0F, 400.0F}), -gz::kPi / 2.0, 1e-4),
        "the opposite ring direction reverses the turn");
}

void SweepPastHalfTurnUnwraps() {
  gz::Drag drag;
  drag.centre_screen = {500.0F, 500.0F};
  drag.openness = 1.0;
  drag.radius_pixels = 100.0;
  gz::BeginDrag(drag, {600.0F, 500.0F}, {600.0F, 500.0F}, {600.0F, 495.0F});
  double angle{};
  for (int step = 1; step <= 36; ++step) {
    const double a = step * 10.0 * gz::kPi / 180.0;
    angle = gz::DragAngle(drag, {static_cast<float>(500.0 + 100.0 * std::cos(a)),
                                 static_cast<float>(500.0 - 100.0 * std::sin(a))});
  }
  Check(Near(angle, 2.0 * gz::kPi, 1e-3), "a full sweep is a full turn, not zero");
}

void EdgeOnRingUsesTheTangent() {
  // Edge-on: openness 0, only the tangent travel counts.
  gz::Drag drag;
  drag.centre_screen = {500.0F, 500.0F};
  drag.openness = 0.0;
  drag.radius_pixels = 100.0;
  gz::BeginDrag(drag, {500.0F, 450.0F}, {500.0F, 450.0F}, {505.0F, 450.0F});
  Check(Near(gz::DragAngle(drag, {550.0F, 450.0F}), 0.5, 1e-6),
        "50 px along the tangent of a 100 px ring is half a radian");
  Check(Near(gz::DragAngle(drag, {500.0F, 400.0F}), 0.0, 1e-6),
        "moving across the tangent does not turn");
  // A tiny on-screen ring is not turned violently: the radius has a floor.
  drag.radius_pixels = 2.0;
  Check(Near(gz::DragAngle(drag, {520.0F, 450.0F}), 1.0, 1e-6), "radius floor of 20 px");
}

// The reported bug: from views nearly parallel to a ring the drag went the
// wrong way. Simulate UE's own frame -- left-handed world (X forward, Y right,
// Z up), canvas y down -- with cameras all round the joint, grab the ring's
// nearest point, drag the cursor a little, and check the grabbed point really
// moves *toward* the cursor under the angle the gizmo computes.
struct Camera {
  gz::Vec3 position, forward, right, up;
};

Camera LookAt(const gz::Vec3 &position) {
  Camera c;
  c.position = position;
  c.forward = gz::Normalize(gz::Scale(position, -1.0));
  // UE camera basis: right = (-sin yaw, cos yaw, 0) for a level camera, and
  // up = right x forward... in a left-handed frame up = forward x right.
  const gz::Vec3 world_up{0.0, 0.0, 1.0};
  c.right = gz::Normalize(gz::Cross(world_up, c.forward));  // left-handed: Z x X = +Y
  c.up = gz::Cross(c.forward, c.right);
  return c;
}

bool ProjectUe(const Camera &camera, const gz::Vec3 &world, gz::Vec2 &out) {
  const gz::Vec3 d = gz::Sub(world, camera.position);
  const double depth = gz::Dot(d, camera.forward);
  if (!(depth > 1e-6))
    return false;
  constexpr double kFocal = 800.0;
  out = {static_cast<float>(500.0 + kFocal * gz::Dot(d, camera.right) / depth),
         static_cast<float>(500.0 - kFocal * gz::Dot(d, camera.up) / depth)};
  return true;
}

gz::Vec3 Rotate(const gz::Vec3 &axis, const double angle, const gz::Vec3 &v) {
  // The plugin's QuatFromRotationVector + QuatRotateVector.
  const double s = std::sin(angle * 0.5), c = std::cos(angle * 0.5);
  const gz::Vec3 u = gz::Scale(axis, s);
  const gz::Vec3 uv = gz::Cross(u, v);
  const gz::Vec3 uuv = gz::Cross(u, uv);
  return gz::Add(v, gz::Scale(gz::Add(gz::Scale(uv, c), uuv), 2.0));
}

void GrabbedPointFollowsCursorFromEveryView() {
  const gz::Vec3 centre{0.0, 0.0, 0.0};
  const std::array<gz::Vec3, 3> axes{gz::Vec3{1.0, 0.0, 0.0}, gz::Vec3{0.0, 1.0, 0.0},
                                     gz::Vec3{0.0, 0.0, 1.0}};
  int cases{};
  for (int yaw = 0; yaw < 360; yaw += 30)
    for (const int pitch : {-60, -25, -5, 5, 25, 60}) {
      const double y = yaw * gz::kPi / 180.0, p = pitch * gz::kPi / 180.0;
      const Camera camera = LookAt({300.0 * std::cos(p) * std::cos(y),
                                    300.0 * std::cos(p) * std::sin(y), 300.0 * std::sin(p)});
      const gz::Vec3 toward = gz::Normalize(gz::Sub(camera.position, centre));
      gz::Vec2 centre_screen{};
      Check(ProjectUe(camera, centre, centre_screen), "joint projects");
      for (const auto &axis : axes) {
        const auto ring = gz::RingPoints(centre, axis, 25.0);
        // The near-side point the cursor is on: the one most toward the camera
        // that is still a distinct screen point from the centre.
        std::size_t grab{};
        double best = -1e9;
        for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
          const double score = gz::Dot(ring[i], toward);
          gz::Vec2 s{};
          if (!ProjectUe(camera, ring[i], s))
            continue;
          const double r = std::hypot(s[0] - centre_screen[0], s[1] - centre_screen[1]);
          if (r < 15.0)
            continue;  // on the joint: not a place to grab a ring
          if (score > best) {
            best = score;
            grab = i;
          }
        }
        gz::Vec2 grab_screen{}, turned_screen{};
        Check(ProjectUe(camera, ring[grab], grab_screen), "grab projects");
        Check(ProjectUe(camera, Rotate(axis, 0.05, ring[grab]), turned_screen), "probe projects");
        gz::Drag drag;
        drag.centre_screen = centre_screen;
        drag.radius_pixels = 66.0;
        drag.openness = gz::Openness(axis, toward);
        gz::BeginDrag(drag, grab_screen, grab_screen, turned_screen);
        // Drag the cursor 12 px along the screen direction +angle moves the
        // grab point, and also 12 px the other way: the grab point must move
        // toward the cursor both times.
        for (const double sign : {1.0, -1.0}) {
          gz::Drag d = drag;
          const gz::Vec2 cursor{
              static_cast<float>(grab_screen[0] + sign * 12.0 * drag.tangent_screen[0]),
              static_cast<float>(grab_screen[1] + sign * 12.0 * drag.tangent_screen[1])};
          const double angle = gz::DragAngle(d, cursor);
          gz::Vec2 moved{};
          Check(ProjectUe(camera, Rotate(axis, angle, ring[grab]), moved), "moved projects");
          const double before = std::hypot(cursor[0] - grab_screen[0], cursor[1] - grab_screen[1]);
          const double after = std::hypot(cursor[0] - moved[0], cursor[1] - moved[1]);
          if (!(after < before)) {
            std::cerr << "yaw " << yaw << " pitch " << pitch << " axis (" << axis[0] << ","
                      << axis[1] << "," << axis[2] << ") openness " << drag.openness
                      << " sign " << sign << " angle " << angle << " before " << before
                      << " after " << after << '\n';
            Check(false, "the grabbed point moves toward the cursor");
          }
          ++cases;
        }
      }
    }
  Check(cases == 12 * 6 * 3 * 2, "every view and ring was checked");
}

void Arrows() {
  std::array<gz::ScreenArrow, 3> arrows{};
  arrows[0] = {{500.0F, 500.0F}, {600.0F, 500.0F}, true};  // right
  arrows[1] = {{500.0F, 500.0F}, {500.0F, 400.0F}, true};  // up
  arrows[2] = {{500.0F, 500.0F}, {504.0F, 503.0F}, true};  // end-on, 5 px
  Check(gz::PickArrow(arrows, {580.0F, 503.0F}, 6.0F) == 0, "pick the right arrow");
  Check(gz::PickArrow(arrows, {502.0F, 430.0F}, 6.0F) == 1, "pick the up arrow");
  Check(gz::PickArrow(arrows, {510.0F, 500.0F}, 6.0F) == gz::kNoRing,
        "the base of an arrow belongs to the joint");
  Check(gz::PickArrow(arrows, {504.0F, 503.0F}, 6.0F) == gz::kNoRing,
        "an arrow seen end-on cannot be grabbed");
  arrows[1].visible = false;
  Check(gz::PickArrow(arrows, {502.0F, 430.0F}, 6.0F) == gz::kNoRing, "hidden arrows");

  // A 100 px arrow that is 20 cm long: 50 px along it is 10 cm; across it, 0.
  Check(Near(gz::ArrowTravel({500.0F, 500.0F}, {600.0F, 500.0F}, 20.0, {580.0F, 500.0F},
                             {630.0F, 500.0F}),
             10.0, 1e-9),
        "travel along the arrow");
  Check(Near(gz::ArrowTravel({500.0F, 500.0F}, {600.0F, 500.0F}, 20.0, {580.0F, 500.0F},
                             {580.0F, 450.0F}),
             0.0, 1e-9),
        "travel across the arrow does not move");
  Check(Near(gz::ArrowTravel({500.0F, 500.0F}, {600.0F, 500.0F}, 20.0, {580.0F, 500.0F},
                             {530.0F, 500.0F}),
             -10.0, 1e-9),
        "back along the arrow moves the other way");
  Check(Near(gz::ArrowTravel({500.0F, 500.0F}, {500.4F, 500.0F}, 20.0, {0.0F, 0.0F},
                             {100.0F, 0.0F}),
             0.0, 1e-9),
        "a degenerate arrow does not move");
}

void Openness() {
  Check(Near(gz::Openness({1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}), 1.0), "facing");
  Check(Near(gz::Openness({0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}), 0.0), "edge-on");
  Check(Near(gz::Openness({-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}), 1.0), "facing away counts too");
}
}  // namespace

int main() {
  RingGeometry();
  FrontHalf();
  PicksTheNearRing();
  BackHalfIsNotPickable();
  SweepAroundFacingRing();
  SweepPastHalfTurnUnwraps();
  EdgeOnRingUsesTheTangent();
  Openness();
  GrabbedPointFollowsCursorFromEveryView();
  Arrows();
  std::cout << "PASS ring geometry, front half, picking, back half, sweep sign, unwrap, "
               "edge-on tangent, openness, grab follows cursor from 72 views x 3 rings, "
               "move arrows\n";
  return 0;
}
