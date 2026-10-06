#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// A rotate gizmo for the selected bone, like MMD's and every 3D package's:
// three rings around the joint, one per axis of the bone's own local frame,
// red X, green Y, blue Z. Dragging a ring turns the bone about that axis only.
// Pure geometry: the plugin projects the ring's world points with the AHUD
// projection and hands the screen polyline back here for picking and for
// turning cursor motion into an angle.
namespace better_pose::gizmo {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr int kSegments = 48;
inline constexpr int kNoRing = -1;

using Vec3 = std::array<double, 3>;
using Vec2 = std::array<float, 2>;

inline Vec3 Add(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 Sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 Scale(const Vec3 &a, const double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline double Dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 Cross(const Vec3 &a, const Vec3 &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double Length(const Vec3 &a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(const Vec3 &a) {
  const double l = Length(a);
  return l > 1e-12 ? Scale(a, 1.0 / l) : Vec3{0.0, 0.0, 0.0};
}

// Two unit vectors spanning the plane perpendicular to `axis`.
inline void PlaneBasis(const Vec3 &axis, Vec3 &u, Vec3 &v) {
  const Vec3 n = Normalize(axis);
  const Vec3 helper = std::abs(n[2]) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
  u = Normalize(Cross(n, helper));
  v = Cross(n, u);
}

// The ring about `axis` through `centre`, radius `radius` (world units), as
// kSegments + 1 points (the last closes the loop).
inline std::vector<Vec3> RingPoints(const Vec3 &centre, const Vec3 &axis, const double radius) {
  Vec3 u, v;
  PlaneBasis(axis, u, v);
  std::vector<Vec3> points;
  points.reserve(kSegments + 1);
  for (int i = 0; i <= kSegments; ++i) {
    const double t = 2.0 * kPi * static_cast<double>(i) / kSegments;
    points.push_back(Add(centre, Add(Scale(u, radius * std::cos(t)), Scale(v, radius * std::sin(t)))));
  }
  return points;
}

// Whether a ring point is on the camera's side of the joint: the far half of
// a ring is drawn faint and cannot be grabbed, as in every 3D package, so the
// near half is the one under the cursor.
inline bool FacesCamera(const Vec3 &point, const Vec3 &centre, const Vec3 &toward_camera) {
  return Dot(Sub(point, centre), toward_camera) >= -1e-9;
}

inline float SegmentDistance(const Vec2 &p, const Vec2 &a, const Vec2 &b) {
  const float abx = b[0] - a[0], aby = b[1] - a[1];
  const float apx = p[0] - a[0], apy = p[1] - a[1];
  const float length2 = abx * abx + aby * aby;
  float t = length2 > 1e-6F ? (apx * abx + apy * aby) / length2 : 0.0F;
  t = std::clamp(t, 0.0F, 1.0F);
  const float dx = apx - abx * t, dy = apy - aby * t;
  return std::sqrt(dx * dx + dy * dy);
}

// One projected ring: its screen polyline and, per point, whether it was
// projected and whether it faces the camera.
struct ScreenRing {
  std::vector<Vec2> points;
  std::vector<std::uint8_t> visible;  // projected in front of the camera
  std::vector<std::uint8_t> front;    // on the camera's side of the joint
};

// The ring whose near half passes within `tolerance` pixels of `cursor`,
// nearest first; kNoRing when none does.
inline int PickRing(const std::array<ScreenRing, 3> &rings, const Vec2 &cursor,
                    const float tolerance) {
  int best = kNoRing;
  float best_distance = tolerance;
  for (int ring = 0; ring != 3; ++ring) {
    const auto &r = rings[static_cast<std::size_t>(ring)];
    for (std::size_t i = 1; i < r.points.size(); ++i) {
      if (r.visible[i - 1] == 0 || r.visible[i] == 0)
        continue;
      if (r.front[i - 1] == 0 && r.front[i] == 0)
        continue;
      const float d = SegmentDistance(cursor, r.points[i - 1], r.points[i]);
      if (d <= best_distance) {
        best_distance = d;
        best = ring;
      }
    }
  }
  return best;
}

// --- Move arrows (the body's root) -------------------------------------------
// Three arrows from the joint along the axes the body offset moves in. An
// arrow is a screen segment from the joint to its tip; dragging it moves the
// body along that axis by however far the cursor travelled along the arrow
// on screen, converted to centimetres by the arrow's own screen length (so
// the point under the cursor stays under it, whatever the zoom).

struct ScreenArrow {
  Vec2 base{};
  Vec2 tip{};
  bool visible{};
};

// The arrow nearest the cursor within `tolerance` pixels; kNoRing when none.
// The first few pixels at the base belong to the joint (and the rings), so an
// arrow is only grabbed along its outer part.
inline int PickArrow(const std::array<ScreenArrow, 3> &arrows, const Vec2 &cursor,
                     const float tolerance) {
  int best = kNoRing;
  float best_distance = tolerance;
  for (int i = 0; i != 3; ++i) {
    const auto &arrow = arrows[static_cast<std::size_t>(i)];
    if (!arrow.visible)
      continue;
    const float dx = arrow.tip[0] - arrow.base[0], dy = arrow.tip[1] - arrow.base[1];
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 12.0F)
      continue;  // seen end-on: too short to grab without hitting the others
    const Vec2 start{arrow.base[0] + dx * 0.3F, arrow.base[1] + dy * 0.3F};
    const float d = SegmentDistance(cursor, start, arrow.tip);
    if (d <= best_distance) {
      best_distance = d;
      best = i;
    }
  }
  return best;
}

// Centimetres moved along an arrow for the cursor's travel since the press.
// `arrow_cm` is the arrow's length in the world, `base`/`tip` its screen ends
// at the press.
inline double ArrowTravel(const Vec2 &base, const Vec2 &tip, const double arrow_cm,
                          const Vec2 &start_cursor, const Vec2 &cursor) {
  const double ax = static_cast<double>(tip[0]) - base[0];
  const double ay = static_cast<double>(tip[1]) - base[1];
  const double length2 = ax * ax + ay * ay;
  if (!(length2 > 1.0))
    return 0.0;
  const double mx = static_cast<double>(cursor[0]) - start_cursor[0];
  const double my = static_cast<double>(cursor[1]) - start_cursor[1];
  // (travel . arrow) / |arrow|^2 is the travel in arrow lengths.
  return (mx * ax + my * ay) / length2 * arrow_cm;
}

// How much the ring faces the camera: 1 when its axis points at the camera
// (the ring is a full circle on screen), 0 when the ring is seen edge-on
// (a line through the joint).
inline double Openness(const Vec3 &axis, const Vec3 &toward_camera) {
  return std::abs(Dot(Normalize(axis), Normalize(toward_camera)));
}

// Turning cursor motion into an angle about the ring's axis.
//
// One rule for every view: the point of the ring that was grabbed follows the
// cursor. Each piece is measured, not assumed, so no handedness convention
// (UE's world is left-handed, the canvas has y down) can flip it:
//   - `tangent_screen` is the screen direction the grabbed point moves in when
//     the bone turns by a small +angle, measured by projecting the turned
//     point (the caller does this with the real projection);
//   - `sweep_sign` says whether that same +angle turns the grabbed point
//     clockwise or counter-clockwise around the joint on screen, from the
//     cross product of (grab - centre) and the tangent.
// A ring that faces the camera is turned by sweeping around the joint (the
// angle swept on screen is the angle turned); a ring seen nearly edge-on has
// no usable "around" -- the grabbed point barely circles the joint on screen --
// so travel along the tangent is used instead, over the ring's on-screen
// radius. The two agree in sign by construction and are blended by how open
// the ring is, so the feel does not jump as the camera moves.
struct Drag {
  int ring{kNoRing};
  Vec3 axis{};            // world, unit
  Vec2 centre_screen{};   // the joint on screen
  Vec2 grab_screen{};     // the grabbed ring point on screen
  Vec2 tangent_screen{};  // unit, screen direction the grabbed point moves for +angle
  double sweep_sign{1.0}; // +1: +angle turns the grab point the way atan2 increases
  double radius_pixels{1.0};
  double openness{};
  Vec2 start_cursor{};
  bool sweep_ready{};
  double sweep_last{};
  double sweep{};         // accumulated, radians, unwrapped (atan2 sense)
};

inline double WrapRadians(double value) {
  value = std::remainder(value, 2.0 * kPi);
  return value;
}

// `grab` and `grab_turned` are the grabbed ring point on screen before and
// after a small positive turn about the axis.
inline void BeginDrag(Drag &drag, const Vec2 &cursor, const Vec2 &grab, const Vec2 &grab_turned) {
  drag.grab_screen = grab;
  const double tx = static_cast<double>(grab_turned[0]) - grab[0];
  const double ty = static_cast<double>(grab_turned[1]) - grab[1];
  const double length = std::sqrt(tx * tx + ty * ty);
  drag.tangent_screen = length > 1e-9
                            ? Vec2{static_cast<float>(tx / length), static_cast<float>(ty / length)}
                            : Vec2{0.0F, 0.0F};
  // Which way +angle circles the joint on screen: the sign of the 2D cross
  // product (radius x tangent). With the canvas's y down this is the sign of
  // the atan2 change the same motion produces.
  const double rx = static_cast<double>(grab[0]) - drag.centre_screen[0];
  const double ry = static_cast<double>(grab[1]) - drag.centre_screen[1];
  const double turn = rx * ty - ry * tx;
  drag.sweep_sign = turn >= 0.0 ? 1.0 : -1.0;
  const double dx = static_cast<double>(cursor[0]) - drag.centre_screen[0];
  const double dy = static_cast<double>(cursor[1]) - drag.centre_screen[1];
  drag.start_cursor = cursor;
  drag.sweep = 0.0;
  drag.sweep_ready = dx * dx + dy * dy > 16.0;
  drag.sweep_last = drag.sweep_ready ? std::atan2(dy, dx) : 0.0;
}

// The angle (radians, about drag.axis) for the cursor now.
inline double DragAngle(Drag &drag, const Vec2 &cursor) {
  // Around the joint.
  const double dx = static_cast<double>(cursor[0]) - drag.centre_screen[0];
  const double dy = static_cast<double>(cursor[1]) - drag.centre_screen[1];
  if (dx * dx + dy * dy > 16.0) {
    const double raw = std::atan2(dy, dx);
    if (!drag.sweep_ready) {
      drag.sweep_ready = true;
      drag.sweep_last = raw;
    } else {
      drag.sweep += WrapRadians(raw - drag.sweep_last);
      drag.sweep_last = raw;
    }
  }
  const double around = drag.sweep * drag.sweep_sign;
  // Along the tangent.
  const double mx = static_cast<double>(cursor[0]) - drag.start_cursor[0];
  const double my = static_cast<double>(cursor[1]) - drag.start_cursor[1];
  const double along = (mx * drag.tangent_screen[0] + my * drag.tangent_screen[1]) /
                       (std::max)(drag.radius_pixels, 20.0);
  const double w = std::clamp((drag.openness - 0.25) / 0.5, 0.0, 1.0);
  return around * w + along * (1.0 - w);
}

}  // namespace better_pose::gizmo
