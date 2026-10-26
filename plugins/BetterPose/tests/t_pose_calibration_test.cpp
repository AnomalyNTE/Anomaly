#include "../plugin.cpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Check(const bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

Vec3d Subtract(const Vec3d &a, const Vec3d &b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double Dot(const Vec3d &a, const Vec3d &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

double Length(const Vec3d &value) {
  return std::sqrt(value.x * value.x + value.y * value.y +
                   value.z * value.z);
}

Vec3d Direction(const std::vector<Transformd> &components,
                const std::uint32_t from, const std::uint32_t to) {
  const Vec3d value =
      Subtract(components[to].translation, components[from].translation);
  const double length = Length(value);
  Check(length > 1e-9, "calibration bone direction is non-zero");
  return {value.x / length, value.y / length, value.z / length};
}

double QuatDifference(const Quatd &a, const Quatd &b) {
  return 1.0 -
         std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

struct Rig {
  std::vector<std::string> names;
  std::vector<std::int32_t> parents;
  std::vector<Transformd> source;

  Rig()
      : names{"Bip001-Pelvis",
              "Bip001-Spine",
              "Bip001-Spine1",
              "Bip001-Spine2",
              "Bip001-Neck",
              "Bip001-Head",
              "Bip001-L-Clavicle",
              "Bip001-L-UpperArm",
              "Bip001-L-Forearm",
              "Bip001-L-Hand",
              "Bip001-R-Clavicle",
              "Bip001-R-UpperArm",
              "Bip001-R-Forearm",
              "Bip001-R-Hand",
              "Bip001-L-Thigh",
              "Bip001-L-Calf",
              "Bip001-L-Foot",
              "Bip001-R-Thigh",
              "Bip001-R-Calf",
              "Bip001-R-Foot"},
        parents{-1, 0, 1, 2, 3, 4, 3, 6, 7, 8, 3, 10, 11, 12,
                0, 14, 15, 0, 17, 18},
        source(names.size()) {
    source[1].translation = {0.0, 0.0, 10.0};
    source[2].translation = {0.0, 0.0, 12.0};
    source[3].translation = {0.0, 0.0, 12.0};
    source[4].translation = {0.0, 0.0, 10.0};
    source[5].translation = {0.0, 0.0, 10.0};

    source[6].translation = {0.0, -5.0, 45.0};
    source[6].rotation = RotatorToQuat(0.0, 10.0, 5.0);
    source[7].translation = {0.0, -5.0, -4.0};
    source[8].translation = {0.0, -5.0, -4.0};
    source[9].translation = {0.0, -5.0, -4.0};
    source[10].translation = {0.0, 5.0, 45.0};
    source[10].rotation = RotatorToQuat(0.0, -10.0, -5.0);
    source[11].translation = {0.0, 5.0, -4.0};
    source[12].translation = {0.0, 5.0, -4.0};
    source[13].translation = {0.0, 5.0, -4.0};

    source[14].translation = {0.0, -8.0, 0.0};
    source[15].translation = {0.0, 0.0, -28.0};
    source[16].translation = {0.0, 0.0, -8.0};
    source[16].rotation = RotatorToQuat(5.0, 15.0, 20.0);
    source[17].translation = {0.0, 8.0, 0.0};
    source[18].translation = {0.0, 0.0, -28.0};
    source[19].translation = {0.0, 0.0, -8.0};
    source[19].rotation = RotatorToQuat(-5.0, -15.0, -20.0);
  }

  std::vector<Transformd> Components(
      const std::vector<Transformd> &locals) const {
    std::vector<Transformd> components(locals.size());
    std::vector<std::uint8_t> marks(locals.size(), 0);
    for (std::uint32_t bone{}; bone != locals.size(); ++bone)
      static_cast<void>(
          ComputeBoneComponent(bone, locals, parents, components, marks));
    return components;
  }
};

}  // namespace

int main() {
  Rig rig;
  std::vector<Transformd> chosen;
  Check(BuildCalibrationPoseLocals(rig.source, rig.parents, rig.names, false,
                                   chosen),
        "A-pose generation succeeds on a complete body");
  Check(chosen.size() == rig.source.size(),
        "A-pose generation preserves the bone table");

  const auto before = rig.Components(rig.source);
  const auto after = rig.Components(chosen);
  const CalibrationValidation a_validation =
      ValidateCalibrationPoseLocals(chosen, rig.parents, rig.names);
  Check(a_validation.kind == CalibrationPoseKind::APose &&
            a_validation.issue == CalibrationIssue::None,
        "A pose passes automatic calibration validation");
  const Vec3d left = Direction(before, 17, 14);
  const Vec3d up = Direction(before, 0, 5);
  const Vec3d down{-up.x, -up.y, -up.z};

  constexpr double kDiagonal = 0.70710678118654752440;
  Check(std::abs(Dot(Direction(after, 7, 8), left) - kDiagonal) < 1e-8 &&
            std::abs(Dot(Direction(after, 7, 8), down) - kDiagonal) < 1e-8,
        "left upper arm starts in a 45-degree A pose");
  Check(std::abs(Dot(Direction(after, 8, 9), left) - kDiagonal) < 1e-8 &&
            std::abs(Dot(Direction(after, 8, 9), down) - kDiagonal) < 1e-8,
        "left forearm continues the A-pose direction");
  Check(std::abs(Dot(Direction(after, 11, 12), left) + kDiagonal) < 1e-8 &&
            std::abs(Dot(Direction(after, 11, 12), down) - kDiagonal) < 1e-8,
        "right upper arm mirrors the A pose");
  Check(std::abs(Dot(Direction(after, 12, 13), left) + kDiagonal) < 1e-8 &&
            std::abs(Dot(Direction(after, 12, 13), down) - kDiagonal) < 1e-8,
        "right forearm mirrors the A pose");
  Check(QuatDifference(chosen[6].rotation, rig.source[6].rotation) < 1e-10 &&
            QuatDifference(chosen[10].rotation,
                           rig.source[10].rotation) < 1e-10,
        "A pose preserves the clavicles' natural local rotations");
  Check(Dot(Direction(after, 0, 1), up) > 0.94,
        "spine is upright");
  for (const auto pair :
       {std::array<std::uint32_t, 3>{14, 15, 16},
        std::array<std::uint32_t, 3>{17, 18, 19}}) {
    Check(Dot(Direction(after, pair[0], pair[1]), down) > 0.94,
          "thigh is straight down");
    Check(Dot(Direction(after, pair[1], pair[2]), down) > 0.94,
          "calf is straight down");
    Check(QuatDifference(after[pair[2]].rotation,
                         before[pair[2]].rotation) < 1e-10,
          "calibration keeps the foot's original world orientation");
  }
  for (std::size_t bone{}; bone != rig.source.size(); ++bone) {
    Check(Length(Subtract(chosen[bone].translation,
                          rig.source[bone].translation)) < 1e-12,
          "A-pose keeps every local bone translation");
    Check(Length(Subtract(chosen[bone].scale, rig.source[bone].scale)) < 1e-12,
          "A-pose keeps every bone scale");
    const auto angles =
        RotationOffsetAngles(rig.source[bone].rotation,
                             chosen[bone].rotation);
    const Quatd restored = QuatNormalize(QuatMultiply(
        RotatorToQuat(angles[0], angles[1], angles[2]),
        rig.source[bone].rotation));
    Check(QuatDifference(restored, chosen[bone].rotation) < 1e-10,
          "A-pose rotations survive the runtime offset conversion");
  }

  std::vector<Transformd> t_pose;
  Check(BuildCalibrationPoseLocals(rig.source, rig.parents, rig.names, true,
                                   t_pose),
        "T-pose remains available as an optional mode");
  const CalibrationValidation t_validation =
      ValidateCalibrationPoseLocals(t_pose, rig.parents, rig.names);
  Check(t_validation.kind == CalibrationPoseKind::TPose &&
            t_validation.issue == CalibrationIssue::None,
        "T pose passes automatic calibration validation");
  const auto t_components = rig.Components(t_pose);
  Check(Dot(Direction(t_components, 7, 8), left) > 0.94 &&
            Dot(Direction(t_components, 8, 9), left) > 0.94 &&
            Dot(Direction(t_components, 11, 12), left) < -0.94 &&
            Dot(Direction(t_components, 12, 13), left) < -0.94,
        "T-pose mode puts both arm chains horizontal");
  Check(QuatDifference(t_pose[6].rotation, rig.source[6].rotation) > 1e-8 &&
            QuatDifference(t_pose[10].rotation,
                           rig.source[10].rotation) > 1e-8,
        "T-pose mode still aligns both clavicles");

  auto missing = rig.names;
  const auto hand = std::find(missing.begin(), missing.end(), "Bip001-L-Hand");
  Check(hand != missing.end(), "test rig contains the left hand");
  missing.erase(hand);
  auto missing_source = rig.source;
  auto missing_parents = rig.parents;
  missing_source.erase(missing_source.begin() + 9);
  for (auto &parent : missing_parents) {
    if (parent > 9)
      --parent;
    else if (parent == 9)
      parent = -1;
  }
  missing_parents.erase(missing_parents.begin() + 9);
  Check(!BuildCalibrationPoseLocals(missing_source, missing_parents, missing,
                                    false, chosen),
        "calibration rejects an incomplete arm");
  const CalibrationValidation missing_validation =
      ValidateCalibrationPoseLocals(missing_source, missing_parents, missing);
  Check(missing_validation.kind == CalibrationPoseKind::Invalid &&
            missing_validation.issue == CalibrationIssue::MissingBones,
        "validation reports missing calibration bones");

  auto broken_arm = rig.Components(t_pose);
  auto broken_locals = t_pose;
  const std::uint32_t left_upper =
      FindBoneIndexByName(rig.names, "Bip001-L-UpperArm");
  const std::uint32_t left_lower =
      FindBoneIndexByName(rig.names, "Bip001-L-Forearm");
  const Vec3d broken_direction =
      Subtract(broken_arm[left_lower].translation,
               broken_arm[left_upper].translation);
  const Quatd up_turn = QuatFromTo(broken_direction, down);
  const std::int32_t parent = rig.parents[left_upper];
  const Quatd parent_world =
      parent >= 0 ? broken_arm[static_cast<std::size_t>(parent)].rotation
                  : Quatd{};
  broken_locals[left_upper].rotation = ApplyWorldRotationToOffset(
      parent_world, up_turn, broken_locals[left_upper].rotation);
  const CalibrationValidation broken_validation =
      ValidateCalibrationPoseLocals(broken_locals, rig.parents, rig.names);
  Check(broken_validation.kind == CalibrationPoseKind::Invalid &&
            broken_validation.issue == CalibrationIssue::LeftArm,
        "validation identifies the invalid left arm chain");

  Context context;
  context.bone_names = rig.names;
  std::array<double, 12> calibrated{0.0, 0.0, 0.0, 1.0,
                                    1.0, 2.0, 3.0, 0.0,
                                    1.0, 1.0, 1.0, 0.0};
  context.manual_motion_reference.emplace("Bip001-L-UpperArm", calibrated);
  context.bone_angles.resize(rig.names.size());
  const auto document = BuildPoseDocument(context);
  const auto json = nlohmann::json::parse(document);
  Check(json.contains("manualMotionReference") &&
            json.at("manualMotionReference").contains("Bip001-L-UpperArm"),
        "manual calibration reference is saved in the character profile");
  context.manual_motion_reference.clear();
  Check(ApplyPoseDocument(context, json),
        "manual calibration reference is valid when loaded");
  const auto loaded =
      context.manual_motion_reference.find("Bip001-L-UpperArm");
  Check(loaded != context.manual_motion_reference.end(),
        "loaded calibration reference contains the saved bone");
  Check(std::abs(loaded->second[4] - 1.0) < 1e-12 &&
            std::abs(loaded->second[5] - 2.0) < 1e-12 &&
            std::abs(loaded->second[6] - 3.0) < 1e-12,
        "loaded calibration reference keeps its local transforms");
  Check(ApplyPoseDocument(context, nlohmann::json::parse(R"({"bones":[]})")) &&
            context.manual_motion_reference.contains("Bip001-L-UpperArm"),
        "legacy pose files do not erase the manual calibration reference");

  std::cout << "PASS generated A/T pose, body directions, lengths, and persistence\n";
  return 0;
}
