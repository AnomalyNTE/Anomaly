#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace vehicle_flight {
using Vector = std::array<double, 3>;
inline constexpr int MinSpeed = 3, DefaultSpeed = 18, MaxSpeed = 1000;

inline bool Finite(const Vector& value) {
    return std::ranges::all_of(value, [](double axis) { return std::isfinite(axis); });
}

inline double Length(const Vector& value) {
    return std::hypot(value[0], value[1], value[2]);
}

inline Vector Direction(double yaw, int forward, int sideways, int vertical) {
    const double angle = yaw * std::numbers::pi / 180.0;
    Vector direction{
        forward * std::cos(angle) - sideways * std::sin(angle),
        forward * std::sin(angle) + sideways * std::cos(angle),
        static_cast<double>(vertical)
    };
    const double length = Length(direction);
    if (length > 1.0) {
        for (auto& axis : direction) axis /= length;
    }
    return direction;
}

inline Vector Advance(Vector position, const Vector& direction, double seconds, int meters_per_second, bool boost) {
    const int speed = std::min(MaxSpeed, meters_per_second * (boost ? 3 : 1));
    const double distance = 100.0 * speed * seconds;
    for (size_t axis = 0; axis < position.size(); ++axis) position[axis] += direction[axis] * distance;
    return position;
}

inline Vector Difference(const Vector& a, const Vector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

}
