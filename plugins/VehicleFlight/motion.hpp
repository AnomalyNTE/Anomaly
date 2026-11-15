#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace vehicle_flight {
using Vector = std::array<double, 3>;
enum class Mode { Keyboard = 0, Camera = 1 };
inline constexpr int MinSpeed = 3, DefaultSpeed = 18, MaxSpeed = 1000;

inline bool Finite(const Vector& v) {
    return std::ranges::all_of(v, [](double value) { return std::isfinite(value); });
}

inline double Length(const Vector& v) {
    return std::hypot(v[0], v[1], v[2]);
}

inline double Along(const Vector& v, const Vector& direction) {
    const double length = Length(direction);
    return length > 1.e-6 ? (v[0] * direction[0] + v[1] * direction[1] + v[2] * direction[2]) / length : 0.0;
}

inline Vector Direction(Mode mode, double pitch, double yaw, int forward, int sideways, int vertical) {
    constexpr double radians = std::numbers::pi / 180.0;
    const double p = mode == Mode::Camera ? pitch * radians : 0.0;
    const double y = yaw * radians;
    const double forward_horizontal = forward * std::cos(p);
    Vector direction{
        forward_horizontal * std::cos(y) - sideways * std::sin(y),
        forward_horizontal * std::sin(y) + sideways * std::cos(y),
        forward * std::sin(p) + vertical
    };
    const double length = Length(direction);
    if (length > 1.0) {
        for (auto& axis : direction) axis /= length;
    }
    return direction;
}

inline Vector Advance(Vector position, Vector& velocity, const Vector& direction, double seconds, int meters_per_second) {
    const double speed = 100.0 * meters_per_second; // Unreal centimeters per second.
    const double blend = 1.0 - std::exp(-seconds / 0.10);
    for (size_t axis = 0; axis < position.size(); ++axis) {
        velocity[axis] += (direction[axis] * speed - velocity[axis]) * blend;
        if (std::abs(velocity[axis]) < 2.0) velocity[axis] = 0.0;
        position[axis] += velocity[axis] * seconds;
    }
    return position;
}

inline Vector CameraTravel(Vector& velocity, double pitch, double yaw, bool forward, bool reverse) {
    const int throttle = static_cast<int>(forward) - static_cast<int>(reverse);
    if (!throttle) velocity = {};
    return Direction(Mode::Camera, pitch, yaw, throttle, 0, 0);
}

inline Vector FollowView(Vector rotation, double pitch, double yaw, double seconds) {
    const Vector desired{std::clamp(pitch, -89.0, 89.0), yaw, 0.0};
    const double blend = 1.0 - std::exp(-seconds / 0.08);
    for (size_t axis = 0; axis < rotation.size(); ++axis) {
        const double delta = std::remainder(desired[axis] - rotation[axis], 360.0);
        rotation[axis] = std::remainder(rotation[axis] + delta * blend, 360.0);
    }
    return rotation;
}

inline Vector Difference(const Vector& a, const Vector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
}
