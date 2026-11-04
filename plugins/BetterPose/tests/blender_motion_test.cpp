#include "blender_motion.hpp"
#include "mmd_morph_map.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(const bool condition, const char *message) {
  if (condition)
    return;
  std::cerr << message << '\n';
  std::exit(1);
}

double Dot(const std::array<double, 4> &a,
           const std::array<double, 4> &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

void TestBuildsNativeMmdMotion() {
  const std::string source = R"json({
    "format": "betterpose-blender-motion",
    "version": 1,
    "model": "test",
    "fps": 30,
    "length": 30,
    "bone_names": ["Bip001", "Bip001-Head"],
    "bone_parents": [-1, 0],
    "frames": [
      {
        "time": 0,
        "root": [0, 0, 0],
        "bones": [{"i": 1, "r": [0, 0, 0]}],
        "faces": {"smile": 0.0},
        "camera": {
          "focus": [0, 0, 100],
          "yaw": 0,
          "pitch": 0,
          "distance": 800,
          "fov": 60
        }
      },
      {
        "time": 1,
        "root": [10, 20, 30],
        "bones": [{"i": 1, "r": [0, 90, 0], "p": [1, 2, 3]}],
        "faces": {"smile": 0.75},
        "camera": {
          "focus": [100, 0, 100],
          "yaw": 90,
          "pitch": 0,
          "distance": 400,
          "fov": 60
        }
      }
    ]
  })json";

  better_pose::blender_motion::Document parsed;
  std::string error;
  std::size_t ignored{};
  Require(better_pose::blender_motion::Parse(
              source, {"Fallback"}, parsed, error, ignored),
          "valid .bpmotion should parse");
  Require(error.empty() && ignored == 0 && parsed.frames.size() == 2,
          "valid source should have two clean frames");
  Require(parsed.bone_names.size() == 2 &&
              parsed.frames[1].bones.front().index == 1,
          "bone index should resolve against the exported bone table");

  std::vector<std::array<double, 12>> reference(2);
  reference[0] = {0, 0, 0, 1, 0, 0, 100, 1, 1, 1, 0, 0};
  const auto rest =
      better_pose::keyframes::FromRotator(0.0, 30.0, 0.0);
  reference[1] = {rest.x, rest.y, rest.z, rest.w,
                  4, 5, 6, 1, 1, 1, 0, 0};

  std::string converted;
  better_pose::blender_motion::MmdBuildReport report;
  Require(better_pose::blender_motion::BuildMmdDocument(
              parsed, {"Bip001", "Bip001-Head"}, reference,
              converted, report, error),
          "parsed Blender clip should build an MMD document");
  Require(report.frame_count == 31 && report.bone_tracks == 1 &&
              report.offset_tracks == 1 && report.morph_tracks == 1,
          "build should report the dense MMD tracks");

  const auto motion = nlohmann::json::parse(converted);
  Require(motion["kind"] == "better-pose-motion" &&
              motion["rootTranslationUnit"] == "cm-delta" &&
              motion["rootBone"] == "Bip001" &&
              motion["frameCount"] == 31,
          "converted document should use the native MMD contract");
  const auto &rotations = motion["bones"]["Bip001-Head"];
  Require(rotations.size() == 31 && rotations[0].size() == 4,
          "rotation track should be dense");
  const std::array<double, 4> first{
      rotations[0][0].get<double>(), rotations[0][1].get<double>(),
      rotations[0][2].get<double>(), rotations[0][3].get<double>()};
  Require(Dot(first, {rest.x, rest.y, rest.z, rest.w}) > 0.999999,
          "zero delta should bake to the reference local rotation");
  const std::array<double, 4> last{
      rotations[30][0].get<double>(), rotations[30][1].get<double>(),
      rotations[30][2].get<double>(), rotations[30][3].get<double>()};
  Require(std::abs(Dot(last, first)) < 0.999999,
          "last frame should include Blender's 90 degree correction");
  Require(motion["rootTranslation"][30] ==
              nlohmann::json::array({10, 20, 30}),
          "root centimetre delta should survive conversion");
  const auto &positions = motion["boneOffsets"]["Bip001-Head"];
  Require(positions.size() == 31 &&
              positions[0] == nlohmann::json::array({4, 5, 6}) &&
              positions[30] == nlohmann::json::array({1, 2, 3}),
          "local bone positions should be filled and interpolated");
  const auto &morphs = motion["morphs"]["smile"];
  Require(morphs.front() == nlohmann::json::array({0, 0.0F}) &&
              morphs.back() == nlohmann::json::array({30, 0.75F}),
          "direct face target should become a native morph track");

  const auto resolved = better_pose::mmd_morph::Resolve(
      {"smile"}, {"smile", "biyan"});
  Require(resolved.size() == 1 && resolved[0].drives.size() == 1 &&
              resolved[0].drives[0].entry == 0 &&
              resolved[0].drives[0].scale == 1.0F,
          "native face-target names should map without a VMD rule");

  std::vector<better_pose::blender_motion::MmdCameraKey> camera_keys;
  Require(better_pose::blender_motion::BuildCameraKeys(
              parsed, 10.0, camera_keys, error) &&
              camera_keys.size() == 2,
          "Blender camera should become an independent camera track");
  Require(camera_keys[0].frame == 0 &&
              std::abs(camera_keys[0].position[1] - 10.0) < 1e-6 &&
              std::abs(camera_keys[0].distance - 80.0) < 1e-6 &&
              std::abs(camera_keys[0].view_angle - 60.0) < 1e-6 &&
              std::abs(camera_keys[0].rotation[1] + 1.5707963267948966) <
                  1e-6,
          "camera target, units and yaw should round-trip");
  Require(camera_keys[1].frame == 30 &&
              camera_keys[1].position ==
                  std::array<double, 3>{-10.0, 10.0, 0.0},
          "camera position should follow MMD coordinate conversion");
}

void TestRejectsForeignFormat() {
  better_pose::blender_motion::Document parsed;
  std::string error;
  std::size_t ignored{};
  Require(!better_pose::blender_motion::Parse(
              R"json({"format":"endfield-blender-motion","version":4,"frames":[]})json",
              {}, parsed, error, ignored),
          "foreign motion format must be rejected");
  Require(!error.empty(), "failed import should explain the format error");
}

void TestRetargetsDeltaBonePositions() {
  const std::string source = R"json({
    "format": "betterpose-blender-motion",
    "version": 1,
    "model": "delta-position",
    "fps": 30,
    "length": 30,
    "bone_position_mode": "delta",
    "bone_names": ["Bip001", "Bip001-Head"],
    "bone_parents": [-1, 0],
    "frames": [
      {
        "time": 0,
        "bones": [{"i": 1, "r": [0, 0, 0], "p": [0, 0, 0]}],
        "faces": {}
      },
      {
        "time": 1,
        "bones": [{"i": 1, "r": [0, 0, 0], "p": [1, 2, 3]}],
        "faces": {}
      }
    ]
  })json";

  better_pose::blender_motion::Document parsed;
  std::string error;
  std::size_t ignored{};
  Require(better_pose::blender_motion::Parse(
              source, {"Fallback"}, parsed, error, ignored) &&
              parsed.bone_positions_are_deltas,
          "delta position mode should parse");

  std::vector<std::array<double, 12>> reference(2);
  reference[0] = {0, 0, 0, 1, 0, 0, 100, 1, 1, 1, 0, 0};
  reference[1] = {0, 0, 0, 1, 4, 5, 6, 1, 1, 1, 0, 0};
  std::string converted;
  better_pose::blender_motion::MmdBuildReport report;
  Require(better_pose::blender_motion::BuildMmdDocument(
              parsed, {"Bip001", "Bip001-Head"}, reference,
              converted, report, error),
          "delta bone positions should build an MMD document");

  const auto motion = nlohmann::json::parse(converted);
  const auto &positions = motion["boneOffsets"]["Bip001-Head"];
  Require(positions[0] == nlohmann::json::array({4, 5, 6}) &&
              positions[30] == nlohmann::json::array({5, 7, 9}),
          "delta bone positions should be added to the target reference pose");
}

void TestCameraYawCrossesPiWithoutLongSpin() {
  const std::string source = R"json({
    "format": "betterpose-blender-motion",
    "version": 1,
    "model": "camera-wrap",
    "fps": 30,
    "length": 2,
    "bone_names": ["Bip001"],
    "bone_parents": [-1],
    "frames": [
      {"time": 0, "bones": [], "camera": {"focus": [0, 0, 100], "yaw": 89, "pitch": 0, "distance": 800, "fov": 60}},
      {"time": 1, "bones": [], "camera": {"focus": [0, 0, 100], "yaw": 91, "pitch": 0, "distance": 800, "fov": 60}},
      {"time": 2, "bones": [], "camera": {"focus": [0, 0, 100], "yaw": 93, "pitch": 0, "distance": 800, "fov": 60}}
    ]
  })json";
  better_pose::blender_motion::Document parsed;
  std::string error;
  std::size_t ignored{};
  Require(better_pose::blender_motion::Parse(
              source, {"Fallback"}, parsed, error, ignored),
          "camera wrap fixture should parse");
  std::vector<better_pose::blender_motion::MmdCameraKey> camera_keys;
  Require(better_pose::blender_motion::BuildCameraKeys(
              parsed, 10.0, camera_keys, error),
          "camera wrap fixture should build keys");
  Require(camera_keys.size() == 3,
          "camera wrap fixture should retain every key");
  constexpr double kPi = 3.14159265358979323846;
  Require(std::abs(camera_keys[1].rotation[1] -
                   camera_keys[0].rotation[1]) < kPi * 0.1 &&
              std::abs(camera_keys[2].rotation[1] -
                       camera_keys[1].rotation[1]) < kPi * 0.1,
          "camera yaw must use the short path across pi");
}

void TestPrefersRawCameraTrackOverBakedSamples() {
  const std::string source = R"json({
    "format": "betterpose-blender-motion",
    "version": 1,
    "model": "raw-camera",
    "fps": 30,
    "length": 2,
    "bone_names": ["Bip001"],
    "bone_parents": [-1],
    "frames": [
      {"time": 0, "bones": [], "camera": {"focus": [0, 0, 100], "yaw": 0, "pitch": 0, "distance": 800, "fov": 60}},
      {"time": 1, "bones": [], "camera": {"focus": [100, 0, 100], "yaw": 90, "pitch": 0, "distance": 400, "fov": 60}},
      {"time": 2, "bones": [], "camera": {"focus": [200, 0, 100], "yaw": 180, "pitch": 0, "distance": 200, "fov": 60}}
    ],
    "camera_track": {
      "kind": "mmd",
      "fps": 30,
      "keys": [{
        "frame": 0,
        "distance": -56.0,
        "position": [1.0, 2.0, 3.0],
        "rotation": [0.1, 0.2, 0.0],
        "view_angle": 30,
        "perspective": false,
        "curves": [
          {"x1": 0.1, "y1": 0.2, "x2": 0.8, "y2": 0.9},
          {"x1": 0.1, "y1": 0.2, "x2": 0.8, "y2": 0.9},
          {"x1": 0.1, "y1": 0.2, "x2": 0.8, "y2": 0.9},
          {"x1": 0.1, "y1": 0.2, "x2": 0.8, "y2": 0.9},
          {"x1": 0.1, "y1": 0.2, "x2": 0.8, "y2": 0.9},
          {"x1": 0.1, "y1": 0.2, "x2": 0.8, "y2": 0.9}
        ]
      }]
    }
  })json";

  better_pose::blender_motion::Document parsed;
  std::string error;
  std::size_t ignored{};
  Require(better_pose::blender_motion::Parse(
              source, {"Fallback"}, parsed, error, ignored) &&
              parsed.camera_track.has_value(),
          "raw camera track should parse alongside baked motion frames");

  std::vector<better_pose::blender_motion::MmdCameraKey> camera_keys;
  Require(better_pose::blender_motion::BuildCameraKeys(
              parsed, 10.0, camera_keys, error) &&
              camera_keys.size() == 1,
          "raw camera track should win over per-frame camera samples");
  const auto &key = camera_keys.front();
  Require(key.frame == 0 && std::abs(key.distance + 56.0) < 1e-9 &&
              key.position == std::array<double, 3>{1.0, 2.0, 3.0} &&
              key.rotation == std::array<double, 3>{0.1, 0.2, 0.0} &&
              key.view_angle == 30.0 && !key.perspective &&
              std::abs(key.curves[0].x1 - 0.1) < 1e-9 &&
              std::abs(key.curves[5].y2 - 0.9) < 1e-9,
          "raw camera key should preserve its authored distance, perspective and curves");
}

}  // namespace

int main() {
  TestBuildsNativeMmdMotion();
  TestRetargetsDeltaBonePositions();
  TestCameraYawCrossesPiWithoutLongSpin();
  TestPrefersRawCameraTrackOverBakedSamples();
  TestRejectsForeignFormat();
  std::cout << "better pose blender motion import: ok\n";
  return 0;
}
