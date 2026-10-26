#include "../motion_joint_calibration.hpp"

#include <cmath>
#include <iostream>

namespace joint = better_pose::motion_joint;

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

double RotationAngle(const better_pose::motion_amplitude::Quat &q) {
  return 2.0 * std::acos(std::clamp(q[3], -1.0, 1.0)) *
         180.0 / 3.14159265358979323846;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= Check(joint::MirrorBoneName("Bip001-L-UpperArm") ==
                  "Bip001-R-UpperArm",
              "upper arm mirror name");
  ok &= Check(joint::MirrorBoneName("Bn_r_xiuA_01") == "Bn_l_xiuA_01",
              "lowercase accessory mirror name");
  ok &= Check(joint::MirrorBoneName("Hand_L") == "Hand_R",
              "suffix mirror name");
  ok &= Check(joint::MirrorBoneName("Bip001-Spine").empty(),
              "centre bone has no mirror");

  joint::Joint source;
  source.ratio = 0.75F;
  source.offset_degrees = {10.0F, 20.0F, 30.0F};
  source.mirror_offset = {true, false, true};
  const joint::Joint mirrored = joint::MirroredJoint(source);
  ok &= Check(Near(mirrored.ratio, 0.75) &&
                  Near(mirrored.offset_degrees[0], -10.0) &&
                  Near(mirrored.offset_degrees[1], 20.0) &&
                  Near(mirrored.offset_degrees[2], -30.0),
              "mirrored joint offsets");

  auto rotation = joint::RotationFromEulerDegreesYXZ({90.0F, 0.0F, 0.0F});
  ok &= Check(Near(RotationAngle(rotation), 90.0, 1e-6),
              "X Euler rotation");
  rotation = joint::RotationFromEulerDegreesYXZ({0.0F, 45.0F, 0.0F});
  ok &= Check(Near(RotationAngle(rotation), 45.0, 1e-6),
              "Y Euler rotation");
  rotation = joint::RotationFromEulerDegreesYXZ({0.0F, 0.0F, -30.0F});
  ok &= Check(Near(RotationAngle(rotation), 30.0, 1e-6),
              "Z Euler rotation");

  if (!ok)
    return 1;
  std::cout << "PASS joint mirror names and Euler offsets\n";
  return 0;
}
