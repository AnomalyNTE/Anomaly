#include "../motion_amplitude.hpp"

#include <cmath>
#include <iostream>

namespace amp = better_pose::motion_amplitude;

namespace {

bool Check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool Near(const double actual, const double expected,
          const double epsilon = 1e-9) {
  return std::abs(actual - expected) <= epsilon;
}

amp::Quat AboutZ(const double degrees) {
  const double half = degrees * 3.14159265358979323846 / 360.0;
  return {0.0, 0.0, std::sin(half), std::cos(half)};
}

}  // namespace

int main() {
  bool ok = true;
  ok &= Check(amp::PartForBone("Bip001-L-UpperArm") == amp::Part::LeftArm,
              "left upper arm classification");
  ok &= Check(amp::PartForBone("Bip001-R-Hand") == amp::Part::RightHand,
              "right hand classification");
  ok &= Check(amp::PartForBone("Bip001-L-Finger1") == amp::Part::LeftHand,
              "left finger classification");
  ok &= Check(amp::PartForBone("Bip001-R-Thigh") == amp::Part::RightLeg,
              "right thigh classification");
  ok &= Check(amp::PartForBone("Bip001-L-Toe0") == amp::Part::LeftFoot,
              "left toe classification");
  ok &= Check(amp::PartForBone("Bip001-Head") == amp::Part::Head,
              "head classification");
  ok &= Check(amp::PartForBone("Bip001-Spine1") == amp::Part::Torso,
              "torso classification");
  ok &= Check(amp::PartForBone("Bn_hair01") == amp::Part::Count,
              "accessory remains unscaled");

  amp::Settings settings;
  settings.master = 0.5F;
  settings.parts[static_cast<std::size_t>(amp::Part::LeftArm)] = 1.5F;
  ok &= Check(Near(settings.Factor(amp::Part::LeftArm), 0.75),
              "master and part factors multiply");
  settings.master = 2.0F;
  settings.parts[static_cast<std::size_t>(amp::Part::LeftArm)] = 2.0F;
  ok &= Check(Near(settings.Factor(amp::Part::LeftArm), 2.0),
              "combined factor is capped");

  const amp::Quat neutral = AboutZ(30.0);
  const amp::Quat animated = AboutZ(60.0);
  const auto assert_angle = [&](const amp::Quat &rotation, const double expected) {
    const double w = std::clamp(rotation[3], -1.0, 1.0);
    const double degrees = 2.0 * std::acos(w) * 180.0 / 3.14159265358979323846;
    return Near(degrees, expected, 1e-6);
  };
  ok &= Check(assert_angle(amp::ScaleRotation(neutral, animated, 0.0F), 30.0),
              "zero amplitude returns to neutral");
  ok &= Check(assert_angle(amp::ScaleRotation(neutral, animated, 0.5F), 45.0),
              "half amplitude scales the local delta");
  ok &= Check(assert_angle(amp::ScaleRotation(neutral, animated, 1.0F), 60.0),
              "native amplitude is unchanged");
  ok &= Check(assert_angle(amp::ScaleRotation(neutral, animated, 2.0F), 90.0),
              "double amplitude scales the local delta");

  if (!ok)
    return 1;
  std::cout << "PASS motion amplitude grouping and rotation scaling\n";
  return 0;
}
