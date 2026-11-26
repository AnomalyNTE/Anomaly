#pragma once
#include "model.hpp"
#include <anomaly/sdk/services/ue5.h>
#include <anomaly/sdk/services/ui_resources.h>
#include "plugins/BetterPose/rotation_gizmo.hpp"
#include <memory>
#include <optional>

namespace creative_mode::editor {
// Reuse BetterPose's projection-independent ring/arrow picking and drag math.
// UE projection, resource ownership and transform intents belong to this plugin.
namespace geometry = better_pose::gizmo;
using Point = geometry::Vec2;
using Quaternion = std::array<double, 4>;
enum class Mode : std::uint32_t { Move, Rotate, Scale };
inline constexpr std::array<std::array<unsigned, 2>, 12> Edges{{
    {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}}};
inline Vector Rotated(const Quaternion& q, const Vector& v) {
    const Vector xyz{q[0], q[1], q[2]};
    const auto twice_cross = geometry::Scale(geometry::Cross(xyz, v), 2);
    return geometry::Add(v, geometry::Add(geometry::Scale(twice_cross, q[3]), geometry::Cross(xyz, twice_cross)));
}
inline Vector TransformPoint(const Transform& t, Vector v) {
    for (std::size_t i = 0; i < 3; ++i) v[i] *= t.scale[i];
    return geometry::Add(t.translation, Rotated(t.rotation, v));
}
inline Quaternion Multiply(const Quaternion& a, const Quaternion& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
inline Vector Euler(const Quaternion& q) {
    // UE Rotator is Rz(yaw) * Ry(-pitch) * Rx(-roll).
    const double pitch = std::asin(std::clamp(2*(q[3]*q[1]-q[2]*q[0]), -1.0, 1.0));
    double roll{}, yaw{};
    if (std::abs(std::cos(pitch)) > 1e-7) {
        roll = std::atan2(2*(q[3]*q[0]+q[1]*q[2]), 1-2*(q[0]*q[0]+q[1]*q[1]));
        yaw = std::atan2(2*(q[3]*q[2]+q[0]*q[1]), 1-2*(q[1]*q[1]+q[2]*q[2]));
    } else {
        yaw = std::atan2(2*(q[3]*q[2]-q[0]*q[1]), 1-2*(q[0]*q[0]+q[2]*q[2]));
    }
    constexpr double degrees = 180.0 / std::numbers::pi;
    return {-pitch*degrees, yaw*degrees, -roll*degrees};
}
inline Pose RotatedPose(Pose pose, const Vector& axis, double angle) {
    const auto sine = std::sin(angle/2);
    pose.rotation = Euler(Multiply({axis[0]*sine, axis[1]*sine, axis[2]*sine, std::cos(angle/2)}, NativeTransform(pose).rotation));
    return pose;
}
struct Box {
    std::array<Point, 8> points{};
    std::array<bool, 8> visible{};
    double depth{};
    bool complete{};
};
inline bool Project(const AnomalyUe5AhudFrameV1& frame, const Vector& world, Point& screen, double* depth = nullptr) {
    double z{};
    if (!frame.project(frame.user, world.data(), screen.data(), &z) || !(z > 0) ||
        !std::isfinite(z) || !std::isfinite(screen[0]) || !std::isfinite(screen[1])) return false;
    if (depth) *depth = z;
    return true;
}
inline Box ProjectBox(const AnomalyUe5AhudFrameV1& frame, const std::array<Vector,8>& corners) {
    Box box; box.complete = true;
    for (std::size_t i = 0; i < 8; ++i) {
        double depth{};
        box.visible[i] = Project(frame, corners[i], box.points[i], &depth);
        box.complete &= box.visible[i]; box.depth += depth / 8;
    }
    return box;
}
inline bool InTriangle(Point p, Point a, Point b, Point c) {
    const auto cross = [](Point x, Point y, Point z) { return (y[0]-x[0])*(z[1]-x[1])-(y[1]-x[1])*(z[0]-x[0]); };
    const auto area = cross(a,b,c);
    if (std::abs(area) < 0.1f) return false;
    const auto d0 = cross(a,b,p), d1 = cross(b,c,p), d2 = cross(c,a,p);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}
inline bool HitBox(const Box& box, Point cursor) {
    constexpr std::array<std::array<unsigned,4>,6> faces{{{0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5}}};
    if (!box.complete) return false;
    for (const auto& f : faces)
        if (InTriangle(cursor,box.points[f[0]],box.points[f[1]],box.points[f[2]]) ||
            InTriangle(cursor,box.points[f[0]],box.points[f[2]],box.points[f[3]])) return true;
    return false;
}
struct Gizmo {
    Point origin{};
    std::array<Vector,3> axes{};
    std::array<geometry::ScreenArrow,3> arrows{};
    std::array<geometry::ScreenRing,3> rings;
    Vector toward{};
    double length{};
    bool visible{};
};
inline Gizmo BuildGizmo(const AnomalyUe5AhudFrameV1& frame, const Pose& pose, Mode mode) {
    Gizmo gizmo;
    if (!Project(frame, pose.position, gizmo.origin)) return gizmo;
    Vector dx{}, dy{};
    double largest{};
    for (std::size_t i = 0; i < 3; ++i) {
        auto point = pose.position; point[i] += 10;
        Point probe{};
        if (!Project(frame, point, probe)) return gizmo;
        dx[i] = (probe[0]-gizmo.origin[0])/10; dy[i] = (probe[1]-gizmo.origin[1])/10;
        largest = std::max(largest, std::hypot(dx[i],dy[i]));
    }
    if (!(largest > 1e-6)) return gizmo;
    gizmo.length = (mode == Mode::Rotate ? 65.0 : mode == Mode::Scale ? 122.0 : 90.0) / largest;
    gizmo.toward = geometry::Normalize(geometry::Cross(dx,dy));
    const auto rotation = NativeTransform(pose).rotation;
    for (std::size_t i = 0; i < 3; ++i) {
        Vector axis{}; axis[i] = 1;
        gizmo.axes[i] = mode == Mode::Move ? axis : Rotated(rotation, axis);
        auto& arrow = gizmo.arrows[i]; arrow.base = gizmo.origin;
        arrow.visible = Project(frame, geometry::Add(pose.position, geometry::Scale(gizmo.axes[i],gizmo.length)), arrow.tip);
        if (mode != Mode::Rotate) continue;
        auto& ring = gizmo.rings[i];
        const auto points = geometry::RingPoints(pose.position, gizmo.axes[i], gizmo.length);
        for (const auto& world : points) {
            Point p{}; ring.visible.push_back(Project(frame,world,p) ? 1 : 0); ring.points.push_back(p);
            ring.front.push_back(geometry::FacesCamera(world,pose.position,gizmo.toward) ? 1 : 0);
        }
    }
    gizmo.visible = true; return gizmo;
}
struct Frame {
    AnomalyGenerationHandleV1 world{};
    std::uint64_t id{};
    Pose pose;
    bool preview{}, selected{};
    std::uint32_t width{}, height{};
    Box selected_box, preview_box;
    std::array<Gizmo,3> gizmos;
};
inline void BuildHandles(Frame& result,const AnomalyUe5AhudFrameV1& native,const Settings& settings) {
    const std::array shown{settings.move_handles,settings.rotate_handles,settings.scale_handles};
    for (std::size_t i = 0; i < shown.size(); ++i)
        result.gizmos[i] = shown[i] ? BuildGizmo(native,result.pose,static_cast<Mode>(i)) : Gizmo{};
}
struct PickedHandle { Mode mode{}; int axis{-1}; };
inline PickedHandle PickHandle(const Frame& frame,Point mouse) {
    if (!frame.selected) return {};
    const auto& scale = frame.gizmos[static_cast<std::size_t>(Mode::Scale)];
    if (scale.visible) {
        const auto& box = frame.preview ? frame.preview_box : frame.selected_box;
        for (std::size_t i = 0; i < box.points.size(); ++i)
            if (box.visible[i] && std::hypot(mouse[0]-box.points[i][0],mouse[1]-box.points[i][1]) < 8)
                return {Mode::Scale,3};
        if (std::hypot(mouse[0]-scale.origin[0],mouse[1]-scale.origin[1]) < 8) return {Mode::Scale,3};
        for (int axis = 0; axis < 3; ++axis) {
            const auto& arrow = scale.arrows[axis];
            if (arrow.visible && std::hypot(arrow.tip[0]-arrow.base[0],arrow.tip[1]-arrow.base[1]) >= 12 &&
                std::hypot(mouse[0]-arrow.tip[0],mouse[1]-arrow.tip[1]) < 9) return {Mode::Scale,axis};
        }
    }
    const auto& move = frame.gizmos[static_cast<std::size_t>(Mode::Move)];
    if (move.visible) {
        const auto axis = geometry::PickArrow(move.arrows,mouse,7);
        if (axis >= 0) return {Mode::Move,axis};
    }
    const auto& rotate = frame.gizmos[static_cast<std::size_t>(Mode::Rotate)];
    if (rotate.visible) return {Mode::Rotate,geometry::PickRing(rotate.rings,mouse,7)};
    return {};
}
struct TransformIntent {
    AnomalyGenerationHandleV1 world{};
    std::uint64_t id{};
    bool preview{};
    Pose pose;
};
struct Drag {
    std::shared_ptr<const Frame> start;
    Point cursor{};
    int axis{-1};
    Mode mode{};
    geometry::Drag ring;
    bool Active() const { return start != nullptr; }
    void Clear() { start.reset(); axis = -1; }
    bool Begin(std::shared_ptr<const Frame> frame, Point mouse) {
        const auto pick = PickHandle(*frame,mouse); axis = pick.axis; mode = pick.mode;
        if (axis < 0) return false;
        const auto& g = frame->gizmos[static_cast<std::size_t>(mode)];
        if (mode == Mode::Rotate) {
            const auto& circle = g.rings[axis];
            std::size_t best = circle.points.size(); float distance = 1e9f;
            for (std::size_t i = 0; i+1 < circle.points.size(); ++i) {
                if (!circle.visible[i] || !circle.visible[i+1] || !circle.front[i]) continue;
                const auto d = std::hypot(mouse[0]-circle.points[i][0],mouse[1]-circle.points[i][1]);
                if (d < distance) { distance = d; best = i; }
            }
            if (best == circle.points.size()) { axis = -1; return false; }
            ring = {}; ring.axis = g.axes[axis]; ring.ring = axis;
            ring.centre_screen = g.origin; ring.radius_pixels = 65;
            ring.openness = geometry::Openness(ring.axis,g.toward);
            geometry::BeginDrag(ring,mouse,circle.points[best],circle.points[best+1]);
        }
        cursor = mouse; start = std::move(frame); return true;
    }
    TransformIntent Intent(const Pose& pose) const { return {start->world,start->id,start->preview,pose}; }
    TransformIntent Move(Point mouse, bool fine, bool snap, const Settings& settings) {
        auto pose = start->pose;
        const auto& g = start->gizmos[static_cast<std::size_t>(mode)];
        const auto factor = fine ? 0.1 : 1.0;
        if (mode == Mode::Move) {
            auto amount = geometry::ArrowTravel(g.arrows[axis].base,g.arrows[axis].tip,g.length,cursor,mouse)*factor;
            if (snap) amount = std::round(amount/settings.grid)*settings.grid;
            for (std::size_t i = 0; i < 3; ++i) pose.position[i] += g.axes[axis][i]*amount;
        } else if (mode == Mode::Scale) {
            double travel{};
            if (axis == 3) {
                const double x = static_cast<double>(cursor[0])-g.origin[0],y = static_cast<double>(cursor[1])-g.origin[1];
                const double dx = static_cast<double>(mouse[0])-cursor[0],dy = static_cast<double>(mouse[1])-cursor[1];
                const auto length_squared = x*x+y*y;
                travel = length_squared >= 64 ? (dx*x+dy*y)/length_squared : (dx-dy)/120;
            } else travel = geometry::ArrowTravel(g.arrows[axis].base,g.arrows[axis].tip,1,cursor,mouse);
            auto ratio = std::max(0.01, 1+travel*factor);
            if (snap) ratio = std::max(0.1, std::round(ratio*10)/10);
            if (axis == 3) ScaleUniformly(pose.scale,ratio);
            else pose.scale[axis] = std::clamp(start->pose.scale[axis]*ratio,0.01,1000.0);
        } else {
            auto angle = geometry::DragAngle(ring,mouse)*factor;
            if (snap) {
                const auto step = settings.angle_step*std::numbers::pi/180;
                angle = std::round(angle/step)*step;
            }
            pose = RotatedPose(pose,g.axes[axis],angle);
        }
        return Intent(pose);
    }
};
}
