#include "plugins/PinkPawHeistESP/ahud_geometry.hpp"

#include <cmath>
#include <iostream>

namespace {

bool Check(const bool condition, const char* const message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool Near(const float left, const float right) noexcept {
    return std::abs(left - right) < 0.0001F;
}

}  // namespace

int main() {
    using pink_paw_heist_esp::ClipOutlineToViewport;
    using pink_paw_heist_esp::FitTextScaleToViewport;
    using pink_paw_heist_esp::ProjectedBounds;

    bool result = true;
    const auto partial_left = ClipOutlineToViewport(
        ProjectedBounds{-50.0F, 10.0F, 100.0F, 60.0F}, 200.0F, 100.0F);
    result = Check(
                 partial_left.count == 3 &&
                     Near(partial_left.values[0].start_x, 0.0F) &&
                     Near(partial_left.values[0].end_x, 100.0F) &&
                     Near(partial_left.values[1].start_x, 100.0F) &&
                     Near(partial_left.values[1].end_x, 100.0F) &&
                     Near(partial_left.values[2].start_x, 100.0F) &&
                     Near(partial_left.values[2].end_x, 0.0F),
                 "partially off-screen outline synthesized a viewport-edge side") &&
        result;

    const auto surrounding = ClipOutlineToViewport(
        ProjectedBounds{-10.0F, -10.0F, 210.0F, 110.0F}, 200.0F, 100.0F);
    result = Check(
                 surrounding.count == 0,
                 "surrounding outline synthesized a full-viewport border") &&
        result;

    const auto partial_top = ClipOutlineToViewport(
        ProjectedBounds{20.0F, -10.0F, 80.0F, 40.0F}, 200.0F, 100.0F);
    result = Check(
                 partial_top.count == 3 &&
                     Near(partial_top.values[0].start_x, 80.0F) &&
                     Near(partial_top.values[0].start_y, 0.0F) &&
                     Near(partial_top.values[0].end_y, 40.0F),
                 "top-clipped outline did not retain only visible side segments") &&
        result;

    result = Check(
                 Near(FitTextScaleToViewport(100.0F, 50.0F, 200.0F, 25.0F), 0.495F) &&
                     Near(FitTextScaleToViewport(100.0F, 50.0F, 50.0F, 100.0F), 0.49F) &&
                     Near(FitTextScaleToViewport(100.0F, 50.0F, 50.0F, 20.0F), 1.0F) &&
                     FitTextScaleToViewport(1.0F, 50.0F, 50.0F, 20.0F) == 0.0F,
                 "label scale did not fit width, height, and shadow inside viewport") &&
        result;
    return result ? 0 : 1;
}
