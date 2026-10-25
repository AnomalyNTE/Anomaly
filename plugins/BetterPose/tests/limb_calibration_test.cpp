#include "../plugin.cpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace limb = better_pose::limb_calibration;

void Check(const bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

double Length(const Vec3d &value) {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vec3d Subtract(const Vec3d &a, const Vec3d &b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double Dot(const Vec3d &a, const Vec3d &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3d Unit(const Vec3d &value) {
  const double length = Length(value);
  Check(length > 1e-9, "test body axis is non-zero");
  return {value.x / length, value.y / length, value.z / length};
}

Vec3d Cross(const Vec3d &a, const Vec3d &b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

bool SameTransform(const Transformd &a, const Transformd &b) {
  constexpr double kTolerance = 1e-12;
  return std::abs(a.rotation.x - b.rotation.x) < kTolerance &&
         std::abs(a.rotation.y - b.rotation.y) < kTolerance &&
         std::abs(a.rotation.z - b.rotation.z) < kTolerance &&
         std::abs(a.rotation.w - b.rotation.w) < kTolerance &&
         std::abs(a.translation.x - b.translation.x) < kTolerance &&
         std::abs(a.translation.y - b.translation.y) < kTolerance &&
         std::abs(a.translation.z - b.translation.z) < kTolerance &&
         std::abs(a.scale.x - b.scale.x) < kTolerance &&
         std::abs(a.scale.y - b.scale.y) < kTolerance &&
         std::abs(a.scale.z - b.scale.z) < kTolerance;
}

struct Rig {
  Context context;
  std::vector<std::string> names;
  std::vector<std::int32_t> parents;
  std::vector<Transformd> locals;

  Rig()
      : names{"Bip001-Pelvis",
              "Bip001-Head",
              "Bip001-L-Thigh",
              "Bip001-R-Thigh",
              "Bip001-L-UpperArm",
              "Bip001-L-Forearm",
              "Bip001-L-Hand",
              "Bip001-R-UpperArm",
              "Bip001-R-Forearm",
              "Bip001-R-Hand",
              "Bip001-L-Calf",
              "Bip001-L-Foot",
              "Bip001-R-Calf",
              "Bip001-R-Foot"},
        parents{-1, 0, 0, 0, 0, 4, 5, 0, 7, 8, 2, 10, 3, 12},
        locals(names.size()) {
    locals[1].translation = {0.0, 0.0, 50.0};
    locals[3].translation = {0.0, 10.0, 0.0};

    locals[4].translation = {0.0, -20.0, 40.0};
    locals[5].translation = {8.0, 0.0, 0.0};
    locals[6].translation = {0.0, 0.0, -8.0};
    locals[7].translation = {0.0, 20.0, 40.0};
    locals[8].translation = {-8.0, 0.0, 0.0};
    locals[9].translation = {0.0, 0.0, -8.0};

    locals[2].translation = {0.0, -7.0, 0.0};
    locals[10].translation = {0.0, 0.0, -30.0};
    locals[11].translation = {0.0, 8.0, 0.0};
    locals[3].translation = {0.0, 7.0, 0.0};
    locals[12].translation = {0.0, 0.0, -30.0};
    locals[13].translation = {0.0, -8.0, 0.0};
  }

  std::vector<Transformd> Components() const {
    std::vector<Transformd> components(locals.size());
    std::vector<std::uint8_t> marks(locals.size(), 0);
    for (std::uint32_t bone{}; bone != locals.size(); ++bone)
      static_cast<void>(
          ComputeBoneComponent(bone, locals, parents, components, marks));
    return components;
  }

  std::uint32_t Find(const std::string_view name) const {
    const auto found = std::find(names.begin(), names.end(), name);
    Check(found != names.end(), "test rig contains requested bone");
    return static_cast<std::uint32_t>(found - names.begin());
  }

  std::uint32_t End(const std::size_t limb_index) const {
    return Find(limb::kBoneNames[limb_index][2]);
  }

  double UpperLength(const std::size_t limb_index) const {
    const auto components = Components();
    const std::uint32_t upper = Find(limb::kBoneNames[limb_index][0]);
    const std::uint32_t middle = Find(limb::kBoneNames[limb_index][1]);
    return Length(Subtract(components[middle].translation,
                           components[upper].translation));
  }

  double LowerLength(const std::size_t limb_index) const {
    const auto components = Components();
    const std::uint32_t middle = Find(limb::kBoneNames[limb_index][1]);
    const std::uint32_t end = Find(limb::kBoneNames[limb_index][2]);
    return Length(Subtract(components[end].translation,
                           components[middle].translation));
  }

  Vec3d EndPosition(const std::size_t limb_index) const {
    return Components()[End(limb_index)].translation;
  }

  std::array<Vec3d, 3> BodyAxes() const {
    const auto components = Components();
    const Vec3d left = Unit(Subtract(components[2].translation,
                                     components[3].translation));
    const Vec3d up = Unit(Subtract(components[1].translation,
                                   components[0].translation));
    const Vec3d forward = Unit(Cross(up, left));
    return {left, up, forward};
  }

  bool Apply(const limb::AllSettings &settings,
             std::array<limb::Status, limb::kLimbCount> *statuses = nullptr,
             const bool master_enabled = true) {
    {
      std::lock_guard<std::mutex> lock(context.motion_limb_mutex);
      context.motion_limb_calibration = settings;
    }
    context.motion_limb_enabled.store(master_enabled,
                                      std::memory_order_release);
    const auto components = Components();
    return ApplyLimbCalibration(context, locals, components, parents, names,
                                statuses);
  }
};

limb::AllSettings OneLimb(const std::size_t index) {
  limb::AllSettings settings;
  settings.limbs[index].enabled = true;
  return settings;
}

}  // namespace

int main() {
  {
    Rig rig;
    const auto original = rig.locals;
    auto settings = OneLimb(0);
    settings.limbs[0].offset[0] = 0.1F;
    std::array<limb::Status, limb::kLimbCount> statuses{};
    Check(!rig.Apply(settings, &statuses, false),
          "master switch disables all limb calibration");
    for (std::size_t bone{}; bone != rig.locals.size(); ++bone)
      Check(SameTransform(rig.locals[bone], original[bone]),
            "disabled master switch leaves limbs unchanged");
    Check(statuses[0] == limb::Status::Original,
          "disabled master switch reports original motion");
  }

  {
    Rig rig;
    const auto original = rig.locals;
    limb::AllSettings settings;
    settings.limbs[static_cast<std::size_t>(limb::Limb::LeftArm)].enabled = true;
    std::array<limb::Status, limb::kLimbCount> statuses{};
    Check(!rig.Apply(settings, &statuses), "neutral enabled limb skips IK");
    Check(statuses[0] == limb::Status::Original,
          "neutral limb reports the original motion");
    for (std::size_t bone{}; bone != rig.locals.size(); ++bone)
      Check(SameTransform(rig.locals[bone], original[bone]),
            "neutral enabled limb leaves every local transform unchanged");
  }

  for (std::size_t index{}; index != limb::kLimbCount; ++index) {
    {
      Rig rig;
      const auto original = rig.locals;
      limb::AllSettings settings;
      settings.limbs[index].enabled = false;
      settings.limbs[index].offset[0] = 0.1F;
      std::array<limb::Status, limb::kLimbCount> statuses{};
      Check(!rig.Apply(settings, &statuses), "disabled limb skips IK");
      Check(statuses[index] == limb::Status::Original,
            "disabled limb reports the original motion");
      for (std::size_t bone{}; bone != rig.locals.size(); ++bone)
        Check(SameTransform(rig.locals[bone], original[bone]),
              "disabled limb leaves every local transform unchanged");
    }

    for (std::size_t segment{}; segment != 2; ++segment) {
      Rig rig;
      const Vec3d before = rig.EndPosition(index);
      const double upper_length = rig.UpperLength(index);
      const double lower_length = rig.LowerLength(index);
      auto settings = OneLimb(index);
      float &ratio =
          segment == 0 ? settings.limbs[index].upper
                       : settings.limbs[index].lower;
      ratio = 1.1F;
      std::array<limb::Status, limb::kLimbCount> statuses{};
      Check(rig.Apply(settings, &statuses),
            "stretch changes a reachable limb target");
      Check(statuses[index] == limb::Status::Applied,
            "reachable limb reports applied correction");
      Check(Length(Subtract(rig.EndPosition(index), before)) > 1e-3,
            "upper/lower stretch changes the endpoint");
      Check(std::abs(rig.UpperLength(index) - upper_length) < 1e-9,
            "upper segment keeps its real bone length");
      Check(std::abs(rig.LowerLength(index) - lower_length) < 1e-9,
            "lower segment keeps its real bone length");
    }

    const auto axes = Rig{}.BodyAxes();
    for (std::size_t axis{}; axis != axes.size(); ++axis) {
      Rig rig;
      const Vec3d before = rig.EndPosition(index);
      float total_length{};
      {
        const auto components = rig.Components();
        const std::uint32_t root = rig.Find(limb::kBoneNames[index][0]);
        const std::uint32_t middle = rig.Find(limb::kBoneNames[index][1]);
        const std::uint32_t end = rig.Find(limb::kBoneNames[index][2]);
        total_length =
            static_cast<float>(Length(Subtract(components[middle].translation,
                                               components[root].translation)) +
                               Length(Subtract(components[end].translation,
                                               components[middle].translation)));
      }
      auto settings = OneLimb(index);
      settings.limbs[index].offset[axis] = 0.1F;
      Check(rig.Apply(settings), "endpoint offset changes a reachable limb");
      const Vec3d movement =
          Subtract(rig.EndPosition(index), before);
      for (std::size_t measured{}; measured != axes.size(); ++measured) {
        const double component = Dot(movement, axes[measured]);
        const double expected =
            measured == axis ? static_cast<double>(total_length) * 0.1 : 0.0;
        if (std::abs(component - expected) >= 1e-6)
          std::cerr << "offset mismatch limb=" << index
                    << " axis=" << axis << " measured=" << measured
                    << " actual=" << component
                    << " expected=" << expected << '\n';
        Check(std::abs(component - expected) < 1e-6,
              "endpoint offset uses anatomical left/up/forward axes");
      }
    }

    Rig clamped_rig;
    auto clamped = OneLimb(index);
    clamped.limbs[index].upper = 1.5F;
    clamped.limbs[index].lower = 1.5F;
    clamped.limbs[index].offset[0] = 0.5F;
    std::array<limb::Status, limb::kLimbCount> statuses{};
    Check(clamped_rig.Apply(clamped, &statuses),
          "unreachable limb still applies the closest IK pose");
    Check(statuses[index] == limb::Status::Clamped,
          "unreachable limb reports clamping");
  }

  std::cout << "PASS limb reach, endpoint offsets, and bone-length preservation\n";
  return 0;
}
