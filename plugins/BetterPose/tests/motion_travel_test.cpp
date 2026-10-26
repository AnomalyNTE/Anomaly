#include "../motion_travel.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace travel = better_pose::motion_travel;

namespace {

void Check(const bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool Near(const double actual, const double expected) {
  return std::abs(actual - expected) < 1e-12;
}

}  // namespace

int main() {
  const std::array<double, 3> authored{100.0, -40.0, 25.0};
  travel::Settings settings;
  auto applied = travel::Apply(authored, settings, false);
  Check(Near(applied[0], 100.0) && Near(applied[1], -40.0) &&
            Near(applied[2], 25.0),
        "default travel scale preserves authored motion");

  settings.scale = {0.5F, 1.5F, 0.0F};
  applied = travel::Apply(authored, settings, false);
  Check(Near(applied[0], 50.0) && Near(applied[1], -60.0) &&
            Near(applied[2], 0.0),
        "left, forward, and vertical axes scale independently");

  applied = travel::Apply(authored, settings, true);
  Check(Near(applied[0], 0.0) && Near(applied[1], 0.0) &&
            Near(applied[2], 0.0),
        "planar lock drops horizontal motion after scaling");

  travel::Settings clamped{{-1.0F, 3.0F, 1.25F}};
  clamped = travel::ClampSettings(clamped);
  applied = travel::Apply(authored, clamped, false);
  Check(Near(applied[0], 0.0) && Near(applied[1], -80.0) &&
            Near(applied[2], 31.25),
        "travel scale clamps to zero through two hundred percent");

  std::cout << "PASS independent XYZ motion travel scaling and planar lock\n";
  return 0;
}
