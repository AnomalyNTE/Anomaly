#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Geometry-only helpers shared by the offline motion converter and the live
// modular-accessory solver. Bone orientation and hierarchy stay with callers.
namespace better_pose::bone_constraints {

struct Vec {
  double x{}, y{}, z{};
  Vec operator+(const Vec &other) const {
    return {x + other.x, y + other.y, z + other.z};
  }
  Vec operator-(const Vec &other) const {
    return {x - other.x, y - other.y, z - other.z};
  }
  Vec operator*(const double scale) const {
    return {x * scale, y * scale, z * scale};
  }
};

inline double Dot(const Vec a, const Vec b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec Cross(const Vec a, const Vec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
inline double Length(const Vec value) {
  return std::sqrt(Dot(value, value));
}
inline Vec Unit(const Vec value) {
  const double length = Length(value);
  return length > 1e-9 ? value * (1.0 / length) : Vec{};
}

struct Candidate {
  Vec head{};
  Vec tip{};
  double length{};
  std::int64_t anchor{-1};
  std::uint32_t chain{};
  std::uint32_t depth{};
  std::uint32_t material{};
  std::uint32_t collision_mask{};
  double radius{};
  double layer_depth{};
  bool eligible{};
};

struct Link {
  std::uint32_t a{};
  std::uint32_t b{};
  double rest{};
  double segment_rest{};
  double radius{};
  std::uint32_t depth{};
  bool shared_anchor{};
  double correction_gain{0.22};
  double velocity_keep{0.5};
  double correction_share_a{0.5};
  double correction_share_b{0.5};
};

struct ClosestPoints {
  Vec a{};
  Vec b{};
  double distance{};
};

inline ClosestPoints ClosestSegmentPoints(const Vec a0, const Vec a1,
                                          const Vec b0, const Vec b1) {
  const Vec u = a1 - a0;
  const Vec v = b1 - b0;
  const Vec w = a0 - b0;
  const double aa = Dot(u, u);
  const double bb = Dot(u, v);
  const double cc = Dot(v, v);
  const double dd = Dot(u, w);
  const double ee = Dot(v, w);
  const double denominator = aa * cc - bb * bb;
  double s{};
  double t{};
  if (aa <= 1e-12 && cc <= 1e-12) {
    return {a0, b0, Length(a0 - b0)};
  }
  if (aa <= 1e-12) {
    t = std::clamp(ee / cc, 0.0, 1.0);
  } else if (cc <= 1e-12) {
    s = std::clamp(-dd / aa, 0.0, 1.0);
  } else {
    if (denominator > 1e-12)
      s = std::clamp((bb * ee - cc * dd) / denominator, 0.0, 1.0);
    t = (bb * s + ee) / cc;
    if (t < 0.0) {
      t = 0.0;
      s = std::clamp(-dd / aa, 0.0, 1.0);
    } else if (t > 1.0) {
      t = 1.0;
      s = std::clamp((bb - dd) / aa, 0.0, 1.0);
    }
  }
  const Vec point_a = a0 + u * s;
  const Vec point_b = b0 + v * t;
  return {point_a, point_b, Length(point_a - point_b)};
}

inline std::vector<Link> Discover(const std::span<const Candidate> nodes,
                                  const std::size_t maximum_degree = 2) {
  struct Edge {
    std::uint32_t a{};
    std::uint32_t b{};
    double distance{};
  };
  std::vector<Edge> edges;
  for (std::size_t a{}; a < nodes.size(); ++a) {
    if (!nodes[a].eligible)
      continue;
    if (nodes[a].anchor < 0)
      continue;
    const Vec direction_a = Unit(nodes[a].tip - nodes[a].head);
    for (std::size_t b = a + 1; b < nodes.size(); ++b) {
      if (!nodes[b].eligible || nodes[a].anchor != nodes[b].anchor ||
          nodes[a].chain == nodes[b].chain ||
          nodes[a].depth != nodes[b].depth ||
          nodes[a].material != nodes[b].material)
        continue;
      const Vec direction_b = Unit(nodes[b].tip - nodes[b].head);
      if (Dot(direction_a, direction_b) < 0.15)
        continue;
      const double distance = Length(nodes[a].tip - nodes[b].tip);
      const double largest_length =
          (std::max)(nodes[a].length, nodes[b].length);
      if (distance < (std::max)(0.02, 0.08 * largest_length) ||
          distance > 4.0 * largest_length)
        continue;
      edges.push_back({static_cast<std::uint32_t>(a),
                       static_cast<std::uint32_t>(b), distance});
    }
  }
  std::stable_sort(edges.begin(), edges.end(),
                   [](const Edge &left, const Edge &right) {
                     return left.distance < right.distance;
                   });
  std::vector<std::uint32_t> degree(nodes.size());
  std::vector<Link> links;
  for (const Edge &edge : edges) {
    if (degree[edge.a] >= maximum_degree ||
        degree[edge.b] >= maximum_degree)
      continue;
    const ClosestPoints closest = ClosestSegmentPoints(
        nodes[edge.a].head, nodes[edge.a].tip, nodes[edge.b].head,
        nodes[edge.b].tip);
    links.push_back(
        {edge.a, edge.b, edge.distance, closest.distance,
         nodes[edge.a].radius + nodes[edge.b].radius,
         nodes[edge.a].depth, true});
    ++degree[edge.a];
    ++degree[edge.b];
  }
  return links;
}

// Broader pairs for self-collision. The rest-pose segment distance is the
// broadphase, so distant parts of unrelated chains stay out of the solver.
inline std::vector<Link> DiscoverSelfCollision(
    const std::span<const Candidate> nodes,
    const std::size_t maximum_degree = 4,
    std::size_t *candidate_pairs = nullptr) {
  struct Edge {
    std::uint32_t a{};
    std::uint32_t b{};
    double distance{};
    bool shared_anchor{};
    double share_a{0.5};
  };
  std::vector<Edge> edges;
  for (std::size_t a{}; a < nodes.size(); ++a) {
    if (!nodes[a].eligible)
      continue;
    const std::uint32_t bit_a =
        std::uint32_t{1} << (nodes[a].material & 31U);
    for (std::size_t b = a + 1; b < nodes.size(); ++b) {
      if (!nodes[b].eligible || nodes[a].chain == nodes[b].chain)
        continue;
      const std::uint32_t bit_b =
          std::uint32_t{1} << (nodes[b].material & 31U);
      if ((nodes[a].collision_mask & bit_b) == 0 ||
          (nodes[b].collision_mask & bit_a) == 0)
        continue;
      const ClosestPoints closest = ClosestSegmentPoints(
          nodes[a].head, nodes[a].tip, nodes[b].head, nodes[b].tip);
      const double largest_length =
          (std::max)(nodes[a].length, nodes[b].length);
      const double limit =
          (std::max)(6.0, (std::min)(30.0, 2.5 * largest_length));
      if (closest.distance > limit)
        continue;
      const double depth_delta =
          nodes[a].layer_depth - nodes[b].layer_depth;
      const double share_a =
          std::abs(depth_delta) < 0.25
              ? 0.5
              : (depth_delta > 0.0 ? 0.65 : 0.35);
      edges.push_back({static_cast<std::uint32_t>(a),
                       static_cast<std::uint32_t>(b), closest.distance,
                       nodes[a].anchor == nodes[b].anchor, share_a});
    }
  }
  if (candidate_pairs != nullptr)
    *candidate_pairs = edges.size();
  std::stable_sort(edges.begin(), edges.end(),
                   [](const Edge &left, const Edge &right) {
                     return left.distance < right.distance;
                   });
  std::vector<std::uint32_t> degree(nodes.size());
  std::vector<Link> links;
  for (const Edge &edge : edges) {
    if (degree[edge.a] >= maximum_degree ||
        degree[edge.b] >= maximum_degree)
      continue;
    links.push_back(
        {edge.a, edge.b, edge.distance, edge.distance,
         nodes[edge.a].radius + nodes[edge.b].radius,
         (std::min)(nodes[edge.a].depth, nodes[edge.b].depth),
         edge.shared_anchor, 0.22, 0.5, edge.share_a,
         1.0 - edge.share_a});
    ++degree[edge.a];
    ++degree[edge.b];
  }
  return links;
}

// The tolerances deliberately leave ordinary sway untouched. Only clearly
// stretched or collapsed links are rotated back towards their rest spacing.
template <class HeadAt, class TipAt, class ApplyDirection, class Rebuild>
std::uint32_t Relax(const std::span<const Link> links, const int iterations,
                    HeadAt head_at, TipAt tip_at, ApplyDirection apply_direction,
                    Rebuild rebuild) {
  constexpr double kMinimumRatio = 0.75;
  constexpr double kMaximumRatio = 1.25;
  constexpr double kCorrectionGain = 0.28;
  std::uint32_t corrections{};
  for (int pass{}; pass < iterations; ++pass) {
    bool changed = false;
    for (const Link &link : links) {
      const Vec head_a = head_at(link.a);
      const Vec head_b = head_at(link.b);
      const Vec tip_a = tip_at(link.a);
      const Vec tip_b = tip_at(link.b);
      const Vec span = tip_b - tip_a;
      const double current = Length(span);
      const double minimum = link.rest * kMinimumRatio;
      const double maximum = link.rest * kMaximumRatio;
      if (current <= 1e-9 || (current >= minimum && current <= maximum))
        continue;
      const double target = std::clamp(current, minimum, maximum);
      const Vec axis = Unit(span);
      if (Length(axis) < 0.5)
        continue;
      const Vec center = (tip_a + tip_b) * 0.5;
      const Vec targets[2] = {center - axis * (target * 0.5),
                              center + axis * (target * 0.5)};
      const Vec heads[2] = {head_a, head_b};
      const Vec tips[2] = {tip_a, tip_b};
      const std::uint32_t indices[2] = {link.a, link.b};
      for (std::size_t endpoint{}; endpoint != 2; ++endpoint) {
        const Vec current_direction = Unit(tips[endpoint] - heads[endpoint]);
        const Vec wanted_direction = Unit(targets[endpoint] - heads[endpoint]);
        if (Length(current_direction) < 0.5 ||
            Length(wanted_direction) < 0.5)
          continue;
        apply_direction(indices[endpoint],
                        Unit(current_direction * (1.0 - kCorrectionGain) +
                             wanted_direction * kCorrectionGain));
        changed = true;
      }
    }
    if (!changed)
      break;
    rebuild();
    ++corrections;
  }
  return corrections;
}

struct ContactDebug {
  Vec a{};
  Vec b{};
  double distance{};
  double safety{};
  bool active{};
  bool a_outer{};
  bool b_outer{};
};

struct SelfCollisionResponse {
  double correction_scale{1.0};
  double velocity_keep_scale{1.0};
};

// Relative tip velocity is in model units per second. Keep ordinary sway
// gentle, but dissipate the rebound of a fast impact instead of feeding it
// back into the spring on the next frame.
inline SelfCollisionResponse SelfCollisionResponseForSpeed(
    const double relative_speed) {
  constexpr double kFullSoftSpeed = 5.0;
  constexpr double kFullFastSpeed = 80.0;
  if (!std::isfinite(relative_speed))
    return {};
  const double speed =
      std::clamp((relative_speed - kFullSoftSpeed) /
                     (kFullFastSpeed - kFullSoftSpeed),
                 0.0, 1.0);
  return {0.72 + 0.46 * speed, 1.20 - 0.75 * speed};
}

template <class HeadAt, class TipAt, class ApplyDirection, class Rebuild,
          class RelativeSpeed>
std::uint32_t RelaxSelfCollisionWithRelativeSpeed(
    const std::span<const Link> links,
    const std::span<std::uint8_t> contact_state,
    const int iterations, HeadAt head_at, TipAt tip_at,
    ApplyDirection apply_direction, Rebuild rebuild,
    const RelativeSpeed relative_speed_at,
    const double unit_scale = 1.0) {
  constexpr double kReleaseMargin = 0.35;
  const double scale = std::isfinite(unit_scale) && unit_scale > 1e-6
                           ? unit_scale
                           : 1.0;
  std::uint32_t corrections{};
  for (int pass{}; pass < iterations; ++pass) {
    bool changed = false;
    for (std::size_t index{}; index < links.size(); ++index) {
      const Link &link = links[index];
      if (link.depth == 0 && link.shared_anchor)
        continue;
      const double safety = (std::max)(
          0.2 * scale,
          (std::min)(link.segment_rest * 0.75,
                     link.radius * scale + 0.2 * scale));
      if (link.segment_rest <= safety + 1e-4)
        continue;
      const Vec head_a = head_at(link.a);
      const Vec head_b = head_at(link.b);
      const Vec tip_a = tip_at(link.a);
      const Vec tip_b = tip_at(link.b);
      const ClosestPoints closest =
          ClosestSegmentPoints(head_a, tip_a, head_b, tip_b);
      const bool held = index < contact_state.size() &&
                        contact_state[index] != 0;
      if (closest.distance >
          safety + (held ? kReleaseMargin * scale : 0.0)) {
        if (index < contact_state.size())
          contact_state[index] = 0;
        continue;
      }
      if (index < contact_state.size())
        contact_state[index] = 1;
      Vec normal = Unit(closest.b - closest.a);
      if (closest.distance <= 1e-6 || Length(normal) < 0.5)
        normal = Unit(Cross(Unit(tip_a - head_a), Unit(tip_b - head_b)));
      if (Length(normal) < 0.5)
        normal = Unit(head_b - head_a);
      if (Length(normal) < 0.5)
        continue;
      const double desired = safety + 0.1 * scale;
      const double share_a =
          std::clamp(link.correction_share_a, 0.0, 1.0);
      const double share_b =
          std::clamp(link.correction_share_b, 0.0, 1.0);
      const Vec target_a =
          closest.a -
          normal * ((desired - closest.distance) * share_a);
      const Vec target_b =
          closest.b +
          normal * ((desired - closest.distance) * share_b);
      const SelfCollisionResponse response =
          SelfCollisionResponseForSpeed(relative_speed_at(link));
      const double gain =
          std::clamp(link.correction_gain * response.correction_scale, 0.01,
                     0.8);
      const double velocity_keep =
          std::clamp(link.velocity_keep * response.velocity_keep_scale, 0.02,
                     0.98);
      const auto correct = [&](const std::uint32_t bone,
                               const Vec head, const Vec tip,
                               const Vec target) {
        const Vec current_direction = Unit(tip - head);
        const Vec wanted_direction = Unit(target - head);
        if (Length(current_direction) < 0.5 ||
            Length(wanted_direction) < 0.5)
          return;
        apply_direction(
            bone,
            Unit(current_direction * (1.0 - gain) +
                 wanted_direction * gain),
            velocity_keep);
        changed = true;
      };
      correct(link.a, head_a, tip_a, target_a);
      correct(link.b, head_b, tip_b, target_b);
    }
    if (!changed)
      break;
    rebuild();
    ++corrections;
  }
  return corrections;
}

template <class HeadAt, class TipAt, class ApplyDirection, class Rebuild>
std::uint32_t RelaxSelfCollision(
    const std::span<const Link> links,
    const std::span<std::uint8_t> contact_state,
    const int iterations, HeadAt head_at, TipAt tip_at,
    ApplyDirection apply_direction, Rebuild rebuild,
    const double unit_scale = 1.0) {
  return RelaxSelfCollisionWithRelativeSpeed(
      links, contact_state, iterations, head_at, tip_at, apply_direction,
      rebuild,
      [](const Link &) {
        return std::numeric_limits<double>::quiet_NaN();
      },
      unit_scale);
}

template <class HeadAt, class TipAt, class ApplyDirection, class Rebuild,
          class RelativeSpeed>
std::uint32_t RelaxSelfCollision(
    const std::span<const Link> links,
    const std::span<std::uint8_t> contact_state,
    const int iterations, HeadAt head_at, TipAt tip_at,
    ApplyDirection apply_direction, Rebuild rebuild,
    const RelativeSpeed relative_speed_at, const double unit_scale = 1.0) {
  return RelaxSelfCollisionWithRelativeSpeed(
      links, contact_state, iterations, head_at, tip_at, apply_direction,
      rebuild, relative_speed_at, unit_scale);
}

} // namespace better_pose::bone_constraints
