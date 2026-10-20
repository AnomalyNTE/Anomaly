#include "jelly_selection.hpp"

#include <algorithm>

namespace jelly {

CameraBasis MakeCameraBasis(const Camera& camera) noexcept {
    CameraBasis basis;
    basis.position = camera.position;
    if (!IsFinite(camera.position) || !IsFinite(camera.rotation) ||
        !IsFinite(camera.horizontal_fov_degrees) ||
        camera.horizontal_fov_degrees <= 5.0 || camera.horizontal_fov_degrees >= 175.0) {
        return basis;
    }

    const double pitch = camera.rotation[0] * kDegreesToRadians;
    const double yaw = camera.rotation[1] * kDegreesToRadians;
    const double roll = camera.rotation[2] * kDegreesToRadians;
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const double cr = std::cos(roll);
    const double sr = std::sin(roll);

    basis.forward = Vector3{cp * cy, cp * sy, sp};
    basis.right = Vector3{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr};
    basis.up = Vector3{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    basis.tan_half_horizontal =
        std::tan(camera.horizontal_fov_degrees * kDegreesToRadians * 0.5);
    basis.tan_half_vertical = basis.tan_half_horizontal / kConservativeAspect;
    basis.valid = IsFinite(basis.forward) && IsFinite(basis.right) && IsFinite(basis.up) &&
        IsFinite(basis.tan_half_horizontal) && basis.tan_half_horizontal > 0.0;
    return basis;
}

namespace {

inline double Dot(const Vector3& left, const Vector3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

// Extent of the box projected onto an axis.
inline double Radius(const Vector3& axis, const Vector3& extent) noexcept {
    return std::abs(axis[0]) * extent[0] + std::abs(axis[1]) * extent[1] +
        std::abs(axis[2]) * extent[2];
}

}  // namespace

bool IntersectsFrustum(const CameraBasis& basis, const Bounds& bounds) noexcept {
    if (!IsFinite(bounds.center) || !IsFinite(bounds.extent)) return false;
    if (bounds.extent[0] <= 0.0 || bounds.extent[1] <= 0.0 || bounds.extent[2] <= 0.0) {
        return false;
    }
    // An unreadable camera must not throw every target out of view.
    if (!basis.valid) return true;

    const Vector3 delta{
        bounds.center[0] - basis.position[0],
        bounds.center[1] - basis.position[1],
        bounds.center[2] - basis.position[2]};
    const double radius_forward = Radius(basis.forward, bounds.extent);
    const double radius_right = Radius(basis.right, bounds.extent);
    const double radius_up = Radius(basis.up, bounds.extent);
    const double ht = basis.tan_half_horizontal;
    const double vt = basis.tan_half_vertical;

    if (Dot(delta, basis.forward) + radius_forward <= kNearPlaneCentimetres) return false;
    if (ht * Dot(delta, basis.forward) - Dot(delta, basis.right) +
            (ht * radius_forward + radius_right) <= 0.0) {
        return false;
    }
    if (ht * Dot(delta, basis.forward) + Dot(delta, basis.right) +
            (ht * radius_forward + radius_right) <= 0.0) {
        return false;
    }
    if (vt * Dot(delta, basis.forward) - Dot(delta, basis.up) +
            (vt * radius_forward + radius_up) <= 0.0) {
        return false;
    }
    if (vt * Dot(delta, basis.forward) + Dot(delta, basis.up) +
            (vt * radius_forward + radius_up) <= 0.0) {
        return false;
    }
    return true;
}

Category Categorize(const std::uint32_t entity_flags, const bool class_is_character) noexcept {
    if ((entity_flags & kEntityFlagLocalPlayer) != 0) return Category::local_player;
    if (class_is_character) return Category::character;
    return Category::other_dynamic;
}

ScreenResult Screen(const CameraBasis& basis, const Bounds& bounds) noexcept {
    if (!IntersectsFrustum(basis, bounds)) return ScreenResult::culled;
    return ScreenResult::candidate;
}

bool SanePage(
    const PageResult& result, const std::uint32_t offset,
    const std::size_t capacity) noexcept {
    if (result.returned > capacity) return false;
    if (offset > result.total_matches) {
        return result.returned == 0 && result.next_offset == result.total_matches;
    }
    if (result.returned > result.total_matches - offset) return false;
    if (result.returned == 0 && offset != result.total_matches) return false;
    return result.next_offset == offset + result.returned;
}

bool Precedes(
    const Candidate& left, const Candidate& right, const Vector3& camera_position) noexcept {
    const bool left_player = left.category == Category::local_player;
    const bool right_player = right.category == Category::local_player;
    if (left_player != right_player) return left_player;
    const double left_distance = SquaredDistanceTo(left, camera_position);
    const double right_distance = SquaredDistanceTo(right, camera_position);
    if (left_distance != right_distance) return left_distance < right_distance;
    return left.frame_handle_id < right.frame_handle_id;
}

void SortForUpdate(std::vector<Candidate>& candidates, const Vector3& camera_position) noexcept {
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [&camera_position](const Candidate& left, const Candidate& right) {
            return Precedes(left, right, camera_position);
        });
}

bool KeepCandidate(
    std::vector<Candidate>& shortlist, const Candidate& candidate,
    const Vector3& camera_position, const std::size_t capacity) noexcept {
    if (capacity == 0) return false;
    if (shortlist.size() < capacity) {
        shortlist.push_back(candidate);
        return true;
    }
    // The list is full: the entry that gives way is the farthest one which is not the
    // local player. A local player that is farther away than everything else is still
    // kept, because the window asks for that model by name.
    const bool incoming_player = candidate.category == Category::local_player;
    std::size_t victim = shortlist.size();
    double worst = 0.0;
    for (std::size_t index = 0; index < shortlist.size(); ++index) {
        if (shortlist[index].category == Category::local_player) continue;
        const double distance = SquaredDistanceTo(shortlist[index], camera_position);
        if (victim == shortlist.size() || distance > worst) {
            victim = index;
            worst = distance;
        }
    }
    if (victim == shortlist.size()) {
        // Every slot holds the local player, so nothing may be dropped.
        return false;
    }
    if (incoming_player || SquaredDistanceTo(candidate, camera_position) < worst) {
        shortlist[victim] = candidate;
        return true;
    }
    return false;
}

}  // namespace jelly
