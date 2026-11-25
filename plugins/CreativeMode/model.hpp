#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace creative_mode {
using Vector = std::array<double, 3>;
inline constexpr std::size_t MaximumItems = 512;
inline constexpr float LeaseSeconds = 5.0f;
inline bool Finite(const Vector& v) {
    return std::all_of(v.begin(), v.end(), [](double n) { return std::isfinite(n); });
}
// Only top-level asset paths are accepted. Soft path conversions then have no
// allocated subobject FString, and the engine retains ownership of loaded assets.
inline bool AssetPath(std::string_view path) {
    if (path.size() > 512 || !(path.starts_with("/Game/") || path.starts_with("/Engine/"))) return false;
    const auto dot = path.rfind('.');
    return dot != std::string_view::npos && dot > path.rfind('/') && dot + 1 < path.size() &&
        path.find('.') == dot && path.find("..") == std::string_view::npos &&
        path.find_first_of(":\\\"'\r\n\t ") == std::string_view::npos &&
        path.find('\0') == std::string_view::npos;
}
struct Pose {
    Vector position{};
    Vector rotation{}; // UE pitch, yaw, roll in degrees.
    Vector scale{1, 1, 1};
    bool Valid() const {
        return Finite(position) && Finite(rotation) && Finite(scale) &&
            std::all_of(scale.begin(), scale.end(), [](double n) { return n >= 0.01 && n <= 1000; });
    }
};
struct Transform {
    std::array<double, 4> rotation{0, 0, 0, 1};
    Vector translation{};
    double padding{};
    Vector scale{1, 1, 1};
    double padding2{};
};
static_assert(sizeof(Transform) == 96);
inline Transform NativeTransform(const Pose& pose) {
    const auto half = std::numbers::pi / 360.0;
    const auto sp = std::sin(pose.rotation[0] * half), cp = std::cos(pose.rotation[0] * half);
    const auto sy = std::sin(pose.rotation[1] * half), cy = std::cos(pose.rotation[1] * half);
    const auto sr = std::sin(pose.rotation[2] * half), cr = std::cos(pose.rotation[2] * half);
    Transform result;
    result.rotation = {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy,
                       cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
    result.translation = pose.position;
    result.scale = pose.scale;
    return result;
}
inline Vector Forward(const Vector& rotation) {
    const auto rad = std::numbers::pi / 180.0;
    const auto pitch = rotation[0] * rad, yaw = rotation[1] * rad;
    return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
}
inline void Snap(Pose& pose, double grid, double angle) {
    if (grid > 0) for (auto& n : pose.position) n = std::round(n / grid) * grid;
    if (angle > 0) for (auto& n : pose.rotation) n = std::round(n / angle) * angle;
}
struct Item {
    std::string asset;
    Pose pose;
    bool collision{true};
};
struct Settings {
    std::string filter, asset{"/Engine/BasicShapes/Cube.Cube"}, folder;
    double distance{500}, height{}, move_step{50}, angle_step{15}, grid{50};
    bool snap{true}, collision{true};
    Vector rotation{}, scale{1, 1, 1};
    std::uint32_t category{};
    bool editor_enabled{true}, fine_tune{};
    bool move_handles{true},rotate_handles{true},scale_handles{true};
    double preview_yaw{35},preview_elevation{20},preview_zoom{1};
};
inline bool Valid(const Settings& s) {
    const auto range = [](double n, double low, double high) { return std::isfinite(n) && n >= low && n <= high; };
    return s.filter.size() <= 128 && AssetPath(s.asset) && range(s.distance, 10, 100000) &&
        range(s.height, -100000, 100000) && range(s.move_step, 0.1, 10000) &&
        range(s.angle_step, 0.1, 180) && range(s.grid, 0.1, 10000) &&
        Pose{{}, s.rotation, s.scale}.Valid() && s.category <= 7 && s.folder.size() <= 512 &&
        (s.folder.empty() || s.folder.starts_with("/Game/") || s.folder.starts_with("/Engine/")) &&
        std::isfinite(s.preview_yaw) && range(s.preview_elevation,-85,85) && range(s.preview_zoom,0.25,4);
}
struct SavedLayout { std::string world; std::vector<Item> items; };
struct Document { Settings settings; SavedLayout saved; };
inline void to_json(nlohmann::json& j, const Pose& p) {
    j = {{"position", p.position}, {"rotation", p.rotation}, {"scale", p.scale}};
}
inline void from_json(const nlohmann::json& j, Pose& p) {
    j.at("position").get_to(p.position); j.at("rotation").get_to(p.rotation); j.at("scale").get_to(p.scale);
    if (!p.Valid()) throw std::runtime_error("Invalid placement transform");
}
inline void to_json(nlohmann::json& j, const Item& i) {
    j = {{"asset", i.asset}, {"pose", i.pose}, {"collision", i.collision}};
}
inline void from_json(const nlohmann::json& j, Item& i) {
    j.at("asset").get_to(i.asset); j.at("pose").get_to(i.pose); j.at("collision").get_to(i.collision);
    if (!AssetPath(i.asset)) throw std::runtime_error("Invalid placement asset path");
}
inline nlohmann::json Encode(const Document& d) {
    const auto& s = d.settings;
    return {{"settings", {{"filter", s.filter}, {"asset", s.asset}, {"distance", s.distance},
        {"height", s.height}, {"moveStep", s.move_step}, {"angleStep", s.angle_step},
        {"grid", s.grid}, {"snap", s.snap}, {"collision", s.collision}, {"rotation", s.rotation}, {"scale", s.scale},
        {"editorEnabled",s.editor_enabled},{"fineTune",s.fine_tune},{"category",s.category},
        {"moveHandles",s.move_handles},{"rotateHandles",s.rotate_handles},{"scaleHandles",s.scale_handles},
        {"folder",s.folder},{"previewYaw",s.preview_yaw},{"previewElevation",s.preview_elevation},{"previewZoom",s.preview_zoom}}},
        {"saved", {{"world", d.saved.world}, {"items", d.saved.items}}}};
}
inline Document Decode(const nlohmann::json& j) {
    Document d;
    const auto& s = j.at("settings");
    s.at("filter").get_to(d.settings.filter); s.at("asset").get_to(d.settings.asset);
    s.at("distance").get_to(d.settings.distance); s.at("height").get_to(d.settings.height);
    s.at("moveStep").get_to(d.settings.move_step); s.at("angleStep").get_to(d.settings.angle_step);
    s.at("grid").get_to(d.settings.grid); s.at("snap").get_to(d.settings.snap);
    s.at("collision").get_to(d.settings.collision);
    s.at("rotation").get_to(d.settings.rotation); s.at("scale").get_to(d.settings.scale);
    d.settings.editor_enabled = s.value("editorEnabled",true);
    d.settings.move_handles = s.value("moveHandles",true); d.settings.rotate_handles = s.value("rotateHandles",true);
    d.settings.scale_handles = s.value("scaleHandles",true);
    d.settings.fine_tune = s.value("fineTune",false); d.settings.category = s.value("category",0u);
    d.settings.folder = s.value("folder",std::string{}); d.settings.preview_yaw = s.value("previewYaw",35.0);
    d.settings.preview_elevation = s.value("previewElevation",20.0); d.settings.preview_zoom = s.value("previewZoom",1.0);
    const auto& saved = j.at("saved");
    saved.at("world").get_to(d.saved.world); saved.at("items").get_to(d.saved.items);
    if (!Valid(d.settings) || d.saved.world.size() > 1024 ||
        d.saved.items.size() > MaximumItems || (!d.saved.items.empty() && d.saved.world.empty()))
        throw std::runtime_error("Invalid CreativeMode settings");
    return d;
}
}
