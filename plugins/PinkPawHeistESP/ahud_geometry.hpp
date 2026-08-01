#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace pink_paw_heist_esp {

struct ProjectedBounds final {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

struct ScreenLine final {
    float start_x{};
    float start_y{};
    float end_x{};
    float end_y{};
};

struct OutlineLines final {
    std::array<ScreenLine, 4> values{};
    std::size_t count{};
};

inline OutlineLines ClipOutlineToViewport(
    const ProjectedBounds& bounds,
    const float viewport_width,
    const float viewport_height) noexcept {
    OutlineLines result;
    const std::array values{
        bounds.left, bounds.top, bounds.right, bounds.bottom,
        viewport_width, viewport_height};
    if (!std::ranges::all_of(values, [](const float value) {
            return std::isfinite(value);
        }) ||
        viewport_width <= 0.0F || viewport_height <= 0.0F ||
        bounds.right - bounds.left < 1.0F ||
        bounds.bottom - bounds.top < 1.0F) {
        return result;
    }

    const float clipped_left = std::clamp(bounds.left, 0.0F, viewport_width);
    const float clipped_right = std::clamp(bounds.right, 0.0F, viewport_width);
    const float clipped_top = std::clamp(bounds.top, 0.0F, viewport_height);
    const float clipped_bottom = std::clamp(bounds.bottom, 0.0F, viewport_height);
    const auto add_horizontal = [&](const float y, const bool reverse) {
        if (y < 0.0F || y > viewport_height ||
            clipped_right - clipped_left < 1.0F) {
            return;
        }
        result.values[result.count++] = reverse
            ? ScreenLine{clipped_right, y, clipped_left, y}
            : ScreenLine{clipped_left, y, clipped_right, y};
    };
    const auto add_vertical = [&](const float x, const bool reverse) {
        if (x < 0.0F || x > viewport_width ||
            clipped_bottom - clipped_top < 1.0F) {
            return;
        }
        result.values[result.count++] = reverse
            ? ScreenLine{x, clipped_bottom, x, clipped_top}
            : ScreenLine{x, clipped_top, x, clipped_bottom};
    };

    add_horizontal(bounds.top, false);
    add_vertical(bounds.right, false);
    add_horizontal(bounds.bottom, true);
    add_vertical(bounds.left, true);
    return result;
}

inline float FitTextScaleToViewport(
    const float viewport_width,
    const float viewport_height,
    const float measured_width,
    const float measured_height,
    const float shadow_offset = 1.0F) noexcept {
    const std::array values{
        viewport_width, viewport_height, measured_width,
        measured_height, shadow_offset};
    if (!std::ranges::all_of(values, [](const float value) {
            return std::isfinite(value);
        }) ||
        viewport_width <= shadow_offset || viewport_height <= shadow_offset ||
        measured_width < 0.0F || measured_height <= 0.0F ||
        shadow_offset < 0.0F) {
        return 0.0F;
    }

    const float width_scale = measured_width > 0.0F
        ? (viewport_width - shadow_offset) / measured_width
        : 1.0F;
    const float height_scale =
        (viewport_height - shadow_offset) / measured_height;
    const float scale = (std::min)({1.0F, width_scale, height_scale});
    return std::isfinite(scale) && scale > 0.0F ? scale : 0.0F;
}

}  // namespace pink_paw_heist_esp
