#include "../retarget/motion_builder.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace mb = better_pose::mmd2bip;
using json = nlohmann::json;

namespace {

bool Check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

template <class T>
void Pod(std::vector<std::uint8_t> &bytes, const T &value) {
  const auto *start = reinterpret_cast<const std::uint8_t *>(&value);
  bytes.insert(bytes.end(), start, start + sizeof(value));
}

void Fixed(std::vector<std::uint8_t> &bytes, const std::string_view value,
           const std::size_t width) {
  const std::size_t count = (std::min)(value.size(), width);
  bytes.insert(bytes.end(), value.begin(), value.begin() + count);
  bytes.resize(bytes.size() + width - count, 0);
}

void FixedBytes(std::vector<std::uint8_t> &bytes,
                const std::initializer_list<std::uint8_t> value,
                const std::size_t width) {
  bytes.insert(bytes.end(), value);
  if (value.size() < width)
    bytes.resize(bytes.size() + width - value.size(), 0);
}

std::vector<std::uint8_t> VmdWithBone(std::initializer_list<std::uint8_t> name,
                                      const float position_z,
                                      const float rotation_y) {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "standard bone fixture", 20);
  Pod<std::uint32_t>(bytes, 1);
  FixedBytes(bytes, name, 15);
  Pod<std::uint32_t>(bytes, 0);
  const float position[3] = {0.0F, 0.0F, position_z};
  for (const float value : position)
    Pod(bytes, value);
  const float rotation[4] = {0.0F, rotation_y, 0.0F, 0.98480775F};
  for (const float value : rotation)
    Pod(bytes, value);
  std::array<std::uint8_t, 64> interpolation{};
  for (std::size_t channel = 0; channel < 4; ++channel) {
    interpolation[channel + 8] = static_cast<std::uint8_t>(127);
    interpolation[channel + 12] = static_cast<std::uint8_t>(127);
  }
  bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  Pod<std::uint32_t>(bytes, 0);  // morphs
  Pod<std::uint32_t>(bytes, 0);  // camera
  Pod<std::uint32_t>(bytes, 0);  // lights
  Pod<std::uint32_t>(bytes, 0);  // self-shadow
  Pod<std::uint32_t>(bytes, 0);  // visibility / IK
  return bytes;
}

std::vector<std::uint8_t> VmdWithFullWidthIkName() {
  // 左足ＩＫ in Shift-JIS. The StandardRig reference calls the same controller 左足IK.
  return VmdWithBone({0x8D, 0xB6, 0x91, 0xAB, 0x82, 0x68, 0x82, 0x6A},
                     2.0F, 0.0F);
}

std::vector<std::uint8_t> VmdWithDeformLegName() {
  // 左足D in Shift-JIS, the deform counterpart used by unitypmx-derived rigs.
  return VmdWithBone({0x8D, 0xB6, 0x91, 0xAB, 0x44}, 0.0F, 0.17364818F);
}

std::vector<std::uint8_t> VmdWithLegFkAndBadIk() {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "leg fallback fixture", 20);
  Pod<std::uint32_t>(bytes, 4);
  const auto track = [&](std::initializer_list<std::uint8_t> name,
                         const float position_z) {
    FixedBytes(bytes, name, 15);
    Pod<std::uint32_t>(bytes, 0);
    const float position[3] = {0.0F, 0.0F, position_z};
    for (const float value : position)
      Pod(bytes, value);
    const float rotation[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    for (const float value : rotation)
      Pod(bytes, value);
    std::array<std::uint8_t, 64> interpolation{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
      interpolation[channel + 8] = static_cast<std::uint8_t>(127);
      interpolation[channel + 12] = static_cast<std::uint8_t>(127);
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  };
  track({0x8D, 0xB6, 0x91, 0xAB}, 0.0F);
  track({0x8D, 0xB6, 0x82, 0xD0, 0x82, 0xB4}, 0.0F);
  track({0x8D, 0xB6, 0x91, 0xAB, 0x8E, 0xF1}, 0.0F);
  track({0x8D, 0xB6, 0x91, 0xAB, 0x82, 0x68, 0x82, 0x6A}, 20.0F);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  return bytes;
}

std::vector<std::uint8_t> VmdWithTrackNames(
    const std::initializer_list<std::vector<std::uint8_t>> &names) {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "compatibility fixture", 20);
  Pod<std::uint32_t>(bytes, static_cast<std::uint32_t>(names.size()));
  for (const auto &name : names) {
    const std::size_t count = (std::min)(name.size(), std::size_t{15});
    bytes.insert(bytes.end(), name.begin(), name.begin() + count);
    bytes.resize(bytes.size() + 15 - count, 0);
    Pod<std::uint32_t>(bytes, 0);
    const float position[3] = {};
    for (const float value : position)
      Pod(bytes, value);
    const float rotation[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    for (const float value : rotation)
      Pod(bytes, value);
    std::array<std::uint8_t, 64> interpolation{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
      interpolation[channel + 8] = static_cast<std::uint8_t>(127);
      interpolation[channel + 12] = static_cast<std::uint8_t>(127);
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  }
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  return bytes;
}

std::vector<std::uint8_t> VmdWithDiagnosticTracks() {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "diagnostic fixture", 20);
  Pod<std::uint32_t>(bytes, 4);
  const auto track = [&](const std::vector<std::uint8_t> &name,
                         const std::uint32_t frame,
                         const float rotation_y) {
    const std::size_t count = (std::min)(name.size(), std::size_t{15});
    bytes.insert(bytes.end(), name.begin(), name.begin() + count);
    bytes.resize(bytes.size() + 15 - count, 0);
    Pod(bytes, frame);
    const float position[3] = {};
    for (const float value : position)
      Pod(bytes, value);
    const float rotation[4] = {0.0F, rotation_y, 0.0F, 1.0F};
    for (const float value : rotation)
      Pod(bytes, value);
    std::array<std::uint8_t, 64> interpolation{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
      interpolation[channel + 8] = static_cast<std::uint8_t>(127);
      interpolation[channel + 12] = static_cast<std::uint8_t>(127);
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  };
  track({0x8F, 0xE3, 0x94, 0xBC, 0x90, 0x67, 0x33}, 0, 0.0F);
  track({0x8D, 0xB6, 0x82, 0xD0, 0x82, 0xB6, 0x95, 0xE2, 0x8F, 0x95},
        0, 0.0F);
  track({0x8D, 0xB6, 0x82, 0xD0, 0x82, 0xB6, 0x95, 0xE2, 0x8F, 0x95},
        7, 0.2F);
  track({0x48, 0x61, 0x69, 0x72, 0x41, 0x5F, 0x30, 0x31}, 0, 0.0F);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  return bytes;
}

std::vector<std::uint8_t> VmdWithFingerAliasAndTip() {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "finger alias fixture", 20);
  Pod<std::uint32_t>(bytes, 2);
  const auto track = [&](std::initializer_list<std::uint8_t> name,
                         const float rotation_y) {
    FixedBytes(bytes, name, 15);
    Pod<std::uint32_t>(bytes, 0);
    const float position[3] = {};
    for (const float value : position)
      Pod(bytes, value);
    const float rotation[4] = {0.0F, rotation_y, 0.0F, 0.98480775F};
    for (const float value : rotation)
      Pod(bytes, value);
    std::array<std::uint8_t, 64> interpolation{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
      interpolation[channel + 8] = static_cast<std::uint8_t>(127);
      interpolation[channel + 12] = static_cast<std::uint8_t>(127);
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  };
  track({0x8D, 0xB6, 0x90, 0x6C, 0x8D, 0xB7, 0x8E, 0x77, 0x82, 0x50},
        0.17364818F);
  track({0x8D, 0xB6, 0x90, 0x6C, 0x8D, 0xB7, 0x8E, 0x77,
         0x82, 0x52, 0x90, 0xE6},
        0.34202015F);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  return bytes;
}

std::vector<std::uint8_t> VmdWithFourDigitRingFinger() {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "four digit ring finger fixture", 20);
  Pod<std::uint32_t>(bytes, 4);
  const auto track = [&](std::initializer_list<std::uint8_t> name,
                         const float rotation_y) {
    FixedBytes(bytes, name, 15);
    Pod<std::uint32_t>(bytes, 0);
    const float position[3] = {};
    for (const float value : position)
      Pod(bytes, value);
    const float rotation[4] = {0.0F, rotation_y, 0.0F, 0.98480775F};
    for (const float value : rotation)
      Pod(bytes, value);
    std::array<std::uint8_t, 64> interpolation{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
      interpolation[channel + 8] = static_cast<std::uint8_t>(127);
      interpolation[channel + 12] = static_cast<std::uint8_t>(127);
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  };
  track({0x8D, 0xB6, 0x8F, 0xAC, 0x8E, 0x77, 0x82, 0x4F}, 0.0871557F);
  track({0x8D, 0xB6, 0x8F, 0xAC, 0x8E, 0x77, 0x82, 0x50}, 0.17364818F);
  track({0x8D, 0xB6, 0x8F, 0xAC, 0x8E, 0x77, 0x82, 0x51}, 0.25881905F);
  track({0x8D, 0xB6, 0x8F, 0xAC, 0x8E, 0x77, 0x82, 0x52}, 0.34202015F);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  return bytes;
}

std::vector<std::uint8_t> VmdWithVisibilityAndDisabledIk() {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "visibility fixture", 20);
  Pod<std::uint32_t>(bytes, 3);
  const auto track = [&](std::initializer_list<std::uint8_t> name,
                         const std::uint32_t frame,
                         const float position_z) {
    FixedBytes(bytes, name, 15);
    Pod(bytes, frame);
    const float position[3] = {0.0F, 0.0F, position_z};
    for (const float value : position)
      Pod(bytes, value);
    const float rotation[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    for (const float value : rotation)
      Pod(bytes, value);
    std::array<std::uint8_t, 64> interpolation{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
      interpolation[channel + 8] = static_cast<std::uint8_t>(127);
      interpolation[channel + 12] = static_cast<std::uint8_t>(127);
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  };
  track({0x83, 0x5A, 0x83, 0x93, 0x83, 0x5E, 0x81, 0x5B}, 0, 0.0F);
  track({0x83, 0x5A, 0x83, 0x93, 0x83, 0x5E, 0x81, 0x5B}, 10, 0.0F);
  track({0x8D, 0xB6, 0x91, 0xAB, 0x82, 0x68, 0x82, 0x6A}, 0, 20.0F);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 3);  // visibility frames
  Pod<std::uint32_t>(bytes, 0);
  const std::uint8_t visible = 1;
  Pod(bytes, visible);
  Pod<std::uint32_t>(bytes, 1);  // IK records
  FixedBytes(bytes, {0x8D, 0xB6, 0x91, 0xAB, 0x82, 0x68, 0x82, 0x6A}, 20);
  const std::uint8_t enabled = 0;
  Pod(bytes, enabled);
  Pod<std::uint32_t>(bytes, 5);
  const std::uint8_t hidden = 0;
  Pod(bytes, hidden);
  Pod<std::uint32_t>(bytes, 0);
  Pod<std::uint32_t>(bytes, 9);
  Pod(bytes, visible);
  Pod<std::uint32_t>(bytes, 0);
  return bytes;
}

constexpr const char *kSkeleton = R"json({
  "mesh": "fixture",
  "basis": "left",
  "bones": [
    {"index": 0, "name": "Bip001", "parent": -1,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,0]}},
    {"index": 1, "name": "Bip001-Pelvis", "parent": 0,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,10]}},
    {"index": 2, "name": "Bip001-L-Thigh", "parent": 1,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,0]}},
    {"index": 3, "name": "Bip001-L-Calf", "parent": 2,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,-5]}},
    {"index": 4, "name": "Bip001-L-Foot", "parent": 3,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,-5]}},
    {"index": 5, "name": "Bip001-L-Toe0", "parent": 4,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,1,0]}},
    {"index": 6, "name": "Bip001-L-Hand", "parent": 0,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [3,0,0]}},
    {"index": 7, "name": "Bip001-L-Finger1", "parent": 6,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 8, "name": "Bip001-L-Finger11", "parent": 7,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 9, "name": "Bip001-L-Finger12", "parent": 8,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 10, "name": "Bip001-L-Finger13", "parent": 9,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 11, "name": "Bip001-Spine", "parent": 1,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,0]}},
    {"index": 12, "name": "Bip001-Spine1", "parent": 11,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,4]}},
    {"index": 13, "name": "Bip001-Spine2", "parent": 12,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,0,4]}},
    {"index": 14, "name": "Bip001-L-Clavicle", "parent": 13,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [2,0,2]}},
    {"index": 15, "name": "Bip001-L-UpperArm", "parent": 14,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [3,0,0]}},
    {"index": 16, "name": "Bone-L-UpperArm-Twist", "parent": 15,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 17, "name": "Bone-L-UpperArm-Twist1", "parent": 16,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 18, "name": "Bip001-L-Prop1", "parent": 15,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [0,1,0]}},
    {"index": 19, "name": "Bip001-L-Finger4", "parent": 6,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 20, "name": "Bip001-L-Finger41", "parent": 19,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 21, "name": "Bip001-L-Finger42", "parent": 20,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 22, "name": "Bip001-L-Finger43", "parent": 21,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [1,0,0]}},
    {"index": 23, "name": "Bip001-L-Forearm", "parent": 15,
     "baseLocal": {"rotation": [0,0,0,1], "translation": [3,0,0]}}
  ]
})json";

constexpr const char *kReference = R"json({
  "kind": "better-pose-reference-pmx",
  "bones": [
    {"name": "全ての親", "index": 0, "parent": -1, "position": [0,0,0]},
    {"name": "センター", "index": 1, "parent": 0, "position": [0,10,0]},
    {"name": "下半身", "index": 2, "parent": 1, "position": [0,8,0]},
    {"name": "左足", "index": 3, "parent": 2, "position": [1,8,0]},
    {"name": "左ひざ", "index": 4, "parent": 3, "position": [1,5,0]},
    {"name": "左足首", "index": 5, "parent": 4, "position": [1,1,0]},
    {"name": "左つま先", "index": 6, "parent": 5, "position": [1,0,1]},
    {"name": "左足IK親", "index": 7, "parent": 0, "position": [1,0,0]},
    {"name": "左足IK", "index": 8, "parent": 7, "position": [1,1,0],
     "ik": {"target": 5, "loop": 64, "limitRadian": 0.6,
            "links": [
              {"bone": 4, "limitMin": [-3.13,0,0], "limitMax": [-0.001,0,0]},
              {"bone": 3}
            ]}},
    {"name": "左手首", "index": 9, "parent": 0, "position": [3,15,0]},
    {"name": "左人指1", "index": 10, "parent": 9, "position": [4,14,0]},
    {"name": "左人指2", "index": 11, "parent": 10, "position": [5,13,0]},
    {"name": "左人指3", "index": 12, "parent": 11, "position": [6,12,0]},
    {"name": "上半身", "index": 13, "parent": 1, "position": [0,11,0]},
    {"name": "上半身2", "index": 14, "parent": 13, "position": [0,13,0]},
    {"name": "上半身3", "index": 15, "parent": 14, "position": [0,14,0]},
    {"name": "左肩P", "index": 16, "parent": 15, "position": [0.7,14.9,0]},
    {"name": "左肩", "index": 17, "parent": 16, "position": [0.9,14.9,0]},
    {"name": "左肩C", "index": 18, "parent": 17, "position": [1.7,14.8,0]},
    {"name": "左腕", "index": 19, "parent": 18, "position": [1.7,14.8,0]},
    {"name": "左腕捩", "index": 20, "parent": 19, "position": [2.5,14,0]},
    {"name": "左ひじ", "index": 21, "parent": 20, "position": [3.7,12.8,0]},
    {"name": "左人指先", "index": 22, "parent": 12, "position": [7,11,0]},
    {"name": "左小指０", "index": 23, "parent": 9, "position": [3.1,13,0]},
    {"name": "左小指１", "index": 24, "parent": 23, "position": [4.1,12.5,0]},
    {"name": "左小指２", "index": 25, "parent": 24, "position": [5.1,12,0]},
    {"name": "左小指３", "index": 26, "parent": 25, "position": [6.1,11.5,0]}
  ]
})json";

constexpr const char *kDeformReference = R"json({
  "kind": "better-pose-reference-pmx",
  "bones": [
    {"name": "全ての親", "index": 0, "parent": -1, "position": [0,0,0]},
    {"name": "センター", "index": 1, "parent": 0, "position": [0,10,0]},
    {"name": "下半身", "index": 2, "parent": 1, "position": [0,8,0]},
    {"name": "左足D", "index": 3, "parent": 2, "position": [1,8,0]},
    {"name": "左ひざD", "index": 4, "parent": 3, "position": [1,5,0]},
    {"name": "左足首D", "index": 5, "parent": 4, "position": [1,1,0]},
    {"name": "左足先EX", "index": 6, "parent": 5, "position": [1,0,1]}
  ]
})json";

}  // namespace

int main() {
  bool ok = true;
  const auto standard_report = mb::AnalyzeVmdCompatibility(
      VmdWithTrackNames({{0x8F, 0xE3, 0x94, 0xBC, 0x90, 0x67, 0x33},
                         {0x8D, 0xB6, 0x8C, 0xA8, 0x50},
                         {0x89, 0x45, 0x8C, 0xA8, 0x50},
                         {0x8D, 0xB6, 0x8C, 0xA8, 0x43},
                         {0x89, 0x45, 0x8C, 0xA8, 0x43}}));
  ok &= Check(standard_report.ok &&
                  standard_report.recommended_reference == 2,
              "preflight recommends StandardRig from spine/shoulder signature");
  const auto unity_report = mb::AnalyzeVmdCompatibility(
      VmdWithTrackNames({{0x8D, 0x98, 0x83, 0x4C, 0x83, 0x83, 0x83, 0x93,
                          0x83, 0x5A, 0x83, 0x8B, 0x8D, 0xB6},
                         {0x89, 0x45, 0x91, 0xAB, 0x44}}));
  ok &= Check(unity_report.ok && unity_report.recommended_reference == 1,
              "preflight recommends Unity Miku from waist/D-chain signature");
  const auto ik_report =
      mb::AnalyzeVmdCompatibility(VmdWithLegFkAndBadIk());
  ok &= Check(ik_report.ok && ik_report.left_leg_fk &&
                  !ik_report.complete_leg_fk && ik_report.foot_ik,
              "preflight reports partial FK and IK availability");
  const auto diagnostic_report =
      mb::AnalyzeVmdCompatibility(VmdWithDiagnosticTracks());
  const auto helper = std::find_if(
      diagnostic_report.unmapped_tracks.begin(),
      diagnostic_report.unmapped_tracks.end(),
      [](const auto &track) { return track.name == "左ひじ補助"; });
  const auto accessory = std::find_if(
      diagnostic_report.unmapped_tracks.begin(),
      diagnostic_report.unmapped_tracks.end(),
      [](const auto &track) { return track.name == "HairA_01"; });
  ok &= Check(diagnostic_report.ok &&
                  diagnostic_report.unmapped_tracks.size() == 2 &&
                  helper != diagnostic_report.unmapped_tracks.end() &&
                  helper->kind ==
                      mb::VmdCompatibilityReport::TrackKind::Control &&
                  helper->first_nonzero_frame == 7 &&
                  helper->signature.dominant_axis == 1 &&
                  helper->signature.axis_dominance > .9 &&
                  helper->signature.rotation_span[1] > .1 &&
                  accessory != diagnostic_report.unmapped_tracks.end() &&
                  accessory->kind ==
                      mb::VmdCompatibilityReport::TrackKind::Accessory,
              "preflight lists unmapped tracks with first nonzero frame and kind");
  const auto finger_alias_vmd = VmdWithFingerAliasAndTip();
  const auto finger_alias_report =
      mb::AnalyzeVmdCompatibility(finger_alias_vmd);
  const auto unresolved_finger_tip = std::find_if(
      finger_alias_report.unmapped_tracks.begin(),
      finger_alias_report.unmapped_tracks.end(),
      [](const auto &track) {
        return track.name.find("先") != std::string::npos;
      });
  ok &= Check(finger_alias_report.ok &&
                  unresolved_finger_tip == finger_alias_report.unmapped_tracks.end(),
              "preflight recognizes 人差指 and numbered finger-tip tracks");
  const auto four_digit_vmd = VmdWithFourDigitRingFinger();
  const auto four_digit_report =
      mb::AnalyzeVmdCompatibility(four_digit_vmd);
  const auto unresolved_fourth_joint = std::find_if(
      four_digit_report.unmapped_tracks.begin(),
      four_digit_report.unmapped_tracks.end(),
      [](const auto &track) { return track.name == "左小指３"; });
  ok &= Check(four_digit_report.ok &&
                  unresolved_fourth_joint ==
                      four_digit_report.unmapped_tracks.end(),
              "preflight recognizes a four-segment ring finger");

  mb::Input input;
  input.vmd_path = "standard-bone.vmd";
  input.skeleton_json = kSkeleton;
  input.reference_pmx_json = kReference;
  input.vmd_bytes = VmdWithFullWidthIkName();
  input.max_frames = 1;

  const auto referenced_finger_report =
      mb::AnalyzeVmdCompatibilityForReference(
          finger_alias_vmd, kReference, kSkeleton, {}, {}, 2);
  const auto referenced_tip = std::find_if(
      referenced_finger_report.mapped_reference_tracks.begin(),
      referenced_finger_report.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左人差指３先" &&
               track.target.find("Bip001-L-Finger13") != std::string::npos;
      });
  ok &= Check(referenced_finger_report.ok &&
                  referenced_finger_report.reference_mapping_ready &&
                  referenced_finger_report.reference_mode == 2 &&
                  referenced_tip !=
                      referenced_finger_report.mapped_reference_tracks.end(),
              "reference-aware audit reports the exact mapped fingertip target");
  const auto referenced_four_digit =
      mb::AnalyzeVmdCompatibilityForReference(
          four_digit_vmd, kReference, kSkeleton, {}, {}, 0);
  const auto referenced_small_finger = std::find_if(
      referenced_four_digit.mapped_reference_tracks.begin(),
      referenced_four_digit.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左小指３" &&
               track.target.find("Bip001-L-Finger43") != std::string::npos;
      });
  const auto referenced_small_finger_middle = std::find_if(
      referenced_four_digit.mapped_reference_tracks.begin(),
      referenced_four_digit.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左小指２" &&
               track.target.find("Bip001-L-Finger42") != std::string::npos;
      });
  ok &= Check(referenced_four_digit.ok &&
                  referenced_four_digit.reference_mapping_ready &&
                  referenced_small_finger !=
                      referenced_four_digit.mapped_reference_tracks.end() &&
                  referenced_small_finger_middle !=
                      referenced_four_digit.mapped_reference_tracks.end(),
              "four source/target joints preserve one-to-one finger order");
  const auto referenced_without_fingers =
      mb::AnalyzeVmdCompatibilityForReference(
          four_digit_vmd, kDeformReference, kSkeleton, {}, {}, 1);
  const auto fallback_under_other_reference = std::find_if(
      referenced_without_fingers.mapped_reference_tracks.begin(),
      referenced_without_fingers.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左小指３" &&
               track.target.find("Bip001-L-Finger43") != std::string::npos;
      });
  ok &= Check(referenced_without_fingers.ok &&
                  referenced_without_fingers.reference_mapping_ready &&
                  fallback_under_other_reference !=
                      referenced_without_fingers.mapped_reference_tracks.end(),
              "VMD finger digits retain a target fallback when the reference lacks them");
  json short_finger_skeleton = json::parse(kSkeleton);
  auto &short_bones = short_finger_skeleton.at("bones");
  short_bones.erase(
      std::remove_if(
          short_bones.begin(), short_bones.end(),
          [](const auto &bone) {
            return bone.at("name") == "Bip001-L-Finger42" ||
                   bone.at("name") == "Bip001-L-Finger43";
          }),
      short_bones.end());
  const auto grouped_fingers =
      mb::AnalyzeVmdCompatibilityForReference(
          four_digit_vmd, kReference, short_finger_skeleton.dump(), {}, {}, 0);
  const auto source_start = std::find_if(
      grouped_fingers.mapped_reference_tracks.begin(),
      grouped_fingers.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左小指０" &&
               track.target.find("Bip001-L-Finger4") != std::string::npos;
      });
  const auto source_end = std::find_if(
      grouped_fingers.mapped_reference_tracks.begin(),
      grouped_fingers.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左小指３" &&
               track.target.find("Bip001-L-Finger41") != std::string::npos;
      });
  ok &= Check(grouped_fingers.ok &&
                  grouped_fingers.reference_mapping_ready &&
                  source_start !=
                      grouped_fingers.mapped_reference_tracks.end() &&
                  source_end !=
                      grouped_fingers.mapped_reference_tracks.end(),
              "extra source finger joints fold onto a shorter target chain");
  json missing_distal_reference = json::parse(kReference);
  auto &missing_reference_bones = missing_distal_reference.at("bones");
  missing_reference_bones.erase(
      std::remove_if(
          missing_reference_bones.begin(), missing_reference_bones.end(),
          [](const auto &bone) {
            return bone.at("name") == "左小指３";
          }),
      missing_reference_bones.end());
  const auto fallback_finger =
      mb::AnalyzeVmdCompatibilityForReference(
          four_digit_vmd, missing_distal_reference.dump(), kSkeleton, {}, {}, 0);
  const auto fallback_end = std::find_if(
      fallback_finger.mapped_reference_tracks.begin(),
      fallback_finger.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左小指３" &&
               track.target.find("Bip001-L-Finger43") != std::string::npos;
      });
  ok &= Check(fallback_finger.ok &&
                  fallback_finger.reference_mapping_ready &&
                  fallback_end !=
                      fallback_finger.mapped_reference_tracks.end(),
              "unmatched distal source digit falls back to the target fingertip");

  const auto parent_vmd =
      VmdWithBone({0x50, 0x61, 0x72, 0x65, 0x6E, 0x74, 0x54,
                   0x72, 0x61, 0x63, 0x6B},
                  2.0F, 0.17364818F);
  const std::vector<mb::ManualParentMapping> parent_mappings{
      {"ParentTrack", "Bip001-L-UpperArm", "Bip001", true, true},
      {"ParentTrack", "Bip001-L-Clavicle", "Bip001", true, true}};
  const auto parent_report =
      mb::AnalyzeVmdCompatibilityForReference(
          parent_vmd, kReference, kSkeleton, {}, {}, 0,
          parent_mappings);
  const auto mapped_parent = std::find_if(
      parent_report.mapped_reference_tracks.begin(),
      parent_report.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "ParentTrack" &&
               track.target.find("Bip001-L-UpperArm") != std::string::npos &&
               track.target.find("Bip001-L-Clavicle") != std::string::npos &&
               track.manual;
      });
  ok &= Check(parent_report.ok && parent_report.reference_mapping_ready &&
                  mapped_parent !=
                      parent_report.mapped_reference_tracks.end(),
              "reference-aware audit reports a manual parent insertion");
  const std::vector<mb::ManualFingerControl> finger_controls{
      {"ParentTrack", mb::FingerControlKind::Grip, 0, -1, 1, 1.0F, false}};
  const auto finger_control_report =
      mb::AnalyzeVmdCompatibilityForReference(
          parent_vmd, kReference, kSkeleton, {}, {}, 0, {}, finger_controls);
  const auto mapped_control = std::find_if(
      finger_control_report.mapped_reference_tracks.begin(),
      finger_control_report.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "ParentTrack" && track.manual &&
               track.target.find("Bip001-L-Finger1") != std::string::npos;
      });
  ok &= Check(finger_control_report.ok &&
                  finger_control_report.reference_mapping_ready &&
                  mapped_control !=
                      finger_control_report.mapped_reference_tracks.end(),
              "reference-aware audit reports grip controls on finger targets");

  const mb::VmdDocument vmd = mb::ParseVmd(input.vmd_bytes);
  if (!Check(vmd.ok, "fixture VMD parses") ||
      !Check(vmd.tracks.size() == 1, "fixture VMD exposes one bone track")) {
    std::cerr << vmd.error << '\n';
    return 1;
  }

  const mb::Result result = mb::BuildMotion(input);
  ok &= Check(result.ok, "standard-bone motion converts");
  if (!result.ok) {
    std::cerr << result.error << '\n';
    return 1;
  }

  const json document = json::parse(result.motion_json);
  const json &chains = document.at("diagnostics").at("ikChains");
  ok &= Check(chains.contains("左足IK"),
              "full-width VMD IK controller binds to StandardRig ASCII name");
  if (chains.contains("左足IK")) {
    const json &chain = chains.at("左足IK");
    const bool starts_away = chain.at("gapBefore").get<double>() > 1.9;
    ok &= Check(starts_away, "fixture controller starts away from the foot");
    if (!starts_away)
      std::cerr << chain.dump() << '\n';
    const bool reaches = chain.at("gapAfter").get<double>() < 0.35;
    ok &= Check(reaches, "leg IK reaches the aliased controller");
    if (!reaches)
      std::cerr << chain.dump() << '\n';
  }

  mb::Input deform_input = input;
  deform_input.reference_pmx_json = kDeformReference;
  deform_input.vmd_bytes = VmdWithDeformLegName();
  const mb::Result deform_result = mb::BuildMotion(deform_input);
  ok &= Check(deform_result.ok, "D/EX-only source motion converts");
  if (deform_result.ok) {
    const json deform = json::parse(deform_result.motion_json);
    const json &bones = deform.at("bones");
    ok &= Check(bones.contains("Bip001-L-Thigh") &&
                    bones.contains("Bip001-L-Calf") &&
                    bones.contains("Bip001-L-Foot") &&
                    bones.contains("Bip001-L-Toe0"),
                "D/EX leg chain completes every target leg bone");
  }

  mb::Input finger_input = input;
  finger_input.vmd_bytes =
      VmdWithBone({0x8D, 0xB6, 0x90, 0x6C, 0x8E, 0x77, 0x31}, 0.0F, 0.17364818F);
  const mb::Result finger_result = mb::BuildMotion(finger_input);
  ok &= Check(finger_result.ok, "plain-digit finger source motion converts");
  if (finger_result.ok) {
    const json finger = json::parse(finger_result.motion_json);
    const json &track = finger.at("bones").at("Bip001-L-Finger1");
    const json &rest = finger.at("rest").at("Bip001-L-Finger1");
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference += std::abs(track.at(0).at(i).get<double>() -
                             rest.at(i).get<double>());
    ok &= Check(difference > 0.05,
                "plain-digit reference resolves to the actual finger track");
  }
  mb::Input finger_alias_input = input;
  finger_alias_input.vmd_bytes = finger_alias_vmd;
  const mb::Result finger_alias_result =
      mb::BuildMotion(finger_alias_input);
  ok &= Check(finger_alias_result.ok,
              "index-finger alias and tip motion converts");
  if (finger_alias_result.ok) {
    const json finger_alias = json::parse(finger_alias_result.motion_json);
    const json &track =
        finger_alias.at("bones").at("Bip001-L-Finger13");
    const json &rest = finger_alias.at("rest").at("Bip001-L-Finger13");
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference += std::abs(track.at(0).at(i).get<double>() -
                             rest.at(i).get<double>());
    ok &= Check(difference > 0.05,
                "人差指 numbered tip drives the target fingertip bone");
  }
  mb::Input four_digit_input = input;
  four_digit_input.vmd_bytes = four_digit_vmd;
  const mb::Result four_digit_result =
      mb::BuildMotion(four_digit_input);
  ok &= Check(four_digit_result.ok,
              "four-segment ring finger motion converts");
  if (four_digit_result.ok) {
    const json four_digit =
        json::parse(four_digit_result.motion_json);
    const json &track =
        four_digit.at("bones").at("Bip001-L-Finger43");
    const json &rest =
        four_digit.at("rest").at("Bip001-L-Finger43");
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference += std::abs(track.at(0).at(i).get<double>() -
                             rest.at(i).get<double>());
    ok &= Check(difference > 0.05,
                "small-finger digit 3 drives the fourth target joint");
  }

  mb::Input spine_input = input;
  spine_input.vmd_bytes =
      VmdWithBone({0x8F, 0xE3, 0x94, 0xBC, 0x90, 0x67, 0x33}, 0.0F, 0.17364818F);
  const mb::Result spine_result = mb::BuildMotion(spine_input);
  ok &= Check(spine_result.ok, "three-section StandardRig spine motion converts");
  if (spine_result.ok) {
    const json spine = json::parse(spine_result.motion_json);
    const json &track = spine.at("bones").at("Bip001-Spine2");
    const json &rest = spine.at("rest").at("Bip001-Spine2");
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference += std::abs(track.at(0).at(i).get<double>() -
                             rest.at(i).get<double>());
    ok &= Check(difference > 0.05,
                "upper-chest 上半身3 drives the third target spine section");
  }

  mb::Input shoulder_input = input;
  shoulder_input.vmd_bytes =
      VmdWithBone({0x8D, 0xB6, 0x8C, 0xA8, 0x50}, 0.0F, 0.17364818F);
  const mb::Result shoulder_result = mb::BuildMotion(shoulder_input);
  ok &= Check(shoulder_result.ok, "shoulder-P source motion converts");
  if (shoulder_result.ok) {
    const json shoulder = json::parse(shoulder_result.motion_json);
    for (const char *name : {"Bip001-L-Clavicle", "Bip001-L-UpperArm"}) {
      const json &track = shoulder.at("bones").at(name).at(0);
      const json &rest = shoulder.at("rest").at(name);
      double difference = 0.0;
      for (std::size_t i = 0; i < 4; ++i)
        difference += std::abs(track.at(i).get<double>() - rest.at(i).get<double>());
      ok &= Check(difference > 0.05,
                  "shoulder-P reaches clavicle and inverse-cancels on upper arm");
    }
  }

  mb::Input parent_input = input;
  parent_input.vmd_bytes = parent_vmd;
  parent_input.manual_parent_mappings = parent_mappings;
  const mb::Result parent_result = mb::BuildMotion(parent_input);
  ok &= Check(parent_result.ok,
              "manual rotation-parent mapping motion converts");
  if (parent_result.ok) {
    const json parent = json::parse(parent_result.motion_json);
    double difference = 0.0;
    for (const char *name :
         {"Bip001-L-UpperArm", "Bip001-L-Clavicle"}) {
      const json &track = parent.at("bones").at(name).at(0);
      const json &rest = parent.at("rest").at(name);
      for (std::size_t i = 0; i < 4; ++i)
        difference += std::abs(track.at(i).get<double>() -
                               rest.at(i).get<double>());
    }
    ok &= Check(
        difference > 0.05 &&
            parent.at("diagnostics").at("manualParentMappings").size() == 2 &&
            parent.contains("boneOffsets") &&
            parent.at("boneOffsets").contains("Bip001-L-UpperArm") &&
            parent.at("boneOffsets").contains("Bip001-L-Clavicle"),
        "one source can insert rotation and position parents on multiple targets");
  }

  mb::Input finger_control_input = input;
  finger_control_input.vmd_bytes = parent_vmd;
  finger_control_input.manual_finger_controls = finger_controls;
  const mb::Result finger_control_result =
      mb::BuildMotion(finger_control_input);
  ok &= Check(finger_control_result.ok,
              "manual grip control motion converts");
  if (finger_control_result.ok) {
    const json controlled =
        json::parse(finger_control_result.motion_json);
    bool changed = false;
    for (const char *name :
         {"Bip001-L-Finger1", "Bip001-L-Finger11",
          "Bip001-L-Finger4"}) {
      const json &track = controlled.at("bones").at(name).at(0);
      const json &rest = controlled.at("rest").at(name);
      double difference = 0.0;
      for (std::size_t i = 0; i < 4; ++i)
        difference += std::abs(track.at(i).get<double>() -
                               rest.at(i).get<double>());
      changed |= difference > 0.02;
    }
    ok &= Check(
        changed &&
            controlled.at("diagnostics").at("manualFingerControls").size() == 1,
        "grip control rotates the target finger chain and records its settings");
  }
  mb::Input spread_control_input = input;
  spread_control_input.vmd_bytes = parent_vmd;
  spread_control_input.manual_finger_controls = {
      {"ParentTrack", mb::FingerControlKind::Spread, 0, 1, 1, 1.0F, false}};
  const mb::Result spread_control_result =
      mb::BuildMotion(spread_control_input);
  ok &= Check(spread_control_result.ok,
              "manual spread control motion converts");
  if (spread_control_result.ok) {
    const json controlled =
        json::parse(spread_control_result.motion_json);
    const json &track =
        controlled.at("bones").at("Bip001-L-Finger1").at(0);
    const json &rest = controlled.at("rest").at("Bip001-L-Finger1");
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference += std::abs(track.at(i).get<double>() -
                             rest.at(i).get<double>());
    ok &= Check(difference > 0.02,
                "spread control rotates the selected finger root");
  }
  mb::Input arm_control_input = input;
  arm_control_input.vmd_bytes = parent_vmd;
  arm_control_input.manual_arm_ik_controls = {
      {"ParentTrack", 0, false, 1.0F, 120.0F, true}};
  const mb::Result arm_control_result =
      mb::BuildMotion(arm_control_input);
  ok &= Check(arm_control_result.ok,
              "arm IK control motion converts");
  if (arm_control_result.ok) {
    const json controlled =
        json::parse(arm_control_result.motion_json);
    const json &targets = controlled.at("armIkTargets");
    ok &= Check(targets.size() == 1 &&
                    targets[0]["hand"] == "Bip001-L-Hand" &&
                    targets[0]["followWrist"] == true &&
                    targets[0]["offsets"].size() == 1 &&
                    targets[0]["rotations"].size() == 1,
                "arm IK control emits wrist target tracks");
  }

  mb::Input twist_input = input;
  twist_input.vmd_bytes =
      VmdWithBone({0x8D, 0xB6, 0x98, 0x72, 0x9D, 0x80}, 0.0F, 0.34202015F);
  const mb::Result twist_result = mb::BuildMotion(twist_input);
  ok &= Check(twist_result.ok, "authored upper-arm twist motion converts");
  if (twist_result.ok) {
    const json twist = json::parse(twist_result.motion_json);
    const json &track = twist.at("bones").at("Bone-L-UpperArm-Twist").at(0);
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference +=
          std::abs(track.at(i).get<double>() - (i == 3 ? 1.0 : 0.0));
    ok &= Check(difference > 0.1,
                "authored 腕捩 drives the target upper-arm twist helper");
  }

  mb::Input fallback_input = input;
  fallback_input.vmd_bytes = VmdWithLegFkAndBadIk();
  const mb::Result fallback_result = mb::BuildMotion(fallback_input);
  ok &= Check(fallback_result.ok, "FK fallback fixture converts");
  if (fallback_result.ok) {
    const json fallback = json::parse(fallback_result.motion_json);
    const json &chain =
        fallback.at("diagnostics").at("ikChains").at("左足IK");
    ok &= Check(chain.at("fallbackFrames").get<int>() == 1,
                "unreachable IK falls back to a complete FK leg");
  }

  const auto visibility_vmd = VmdWithVisibilityAndDisabledIk();
  const mb::VmdDocument visibility_document = mb::ParseVmd(visibility_vmd);
  const auto visibility_report =
      mb::AnalyzeVmdCompatibility(visibility_vmd);
  ok &= Check(visibility_document.ok &&
                  visibility_document.visibility.size() == 3 &&
                  visibility_document.visibility[0].visible &&
                  !visibility_document.visibility[1].visible &&
                  visibility_document.visibility[2].visible &&
                  visibility_document.ik_states.size() == 1 &&
                  !visibility_document.ik_states.front().on,
              "visibility and IK switch records survive VMD parsing");
  ok &= Check(visibility_report.ok && visibility_report.visibility_keys == 3,
              "compatibility report counts model visibility keys");
  mb::Input follow_input = input;
  follow_input.vmd_bytes = visibility_vmd;
  const mb::Result follow_result = mb::BuildMotion(follow_input);
  ok &= Check(follow_result.ok, "follow-motion IK mode converts");
  if (follow_result.ok) {
    const json follow = json::parse(follow_result.motion_json);
    ok &= Check(follow.contains("visibility") &&
                    !follow.at("diagnostics").at("ikChains").contains("左足IK"),
                "follow mode preserves VMD visibility and disabled IK");
  }
  mb::Input force_on_input = input;
  force_on_input.vmd_bytes = visibility_vmd;
  force_on_input.ik_mode = mb::IkMode::ForceOn;
  const mb::Result force_on_result = mb::BuildMotion(force_on_input);
  ok &= Check(force_on_result.ok, "force-on IK mode converts");
  if (force_on_result.ok) {
    const json forced = json::parse(force_on_result.motion_json);
    ok &= Check(forced.at("diagnostics").at("ikChains").contains("左足IK"),
                "force-on ignores the VMD disabled IK record");
  }
  mb::Input force_off_input = input;
  force_off_input.vmd_bytes = visibility_vmd;
  force_off_input.ik_mode = mb::IkMode::ForceOff;
  const mb::Result force_off_result = mb::BuildMotion(force_off_input);
  ok &= Check(force_off_result.ok, "force-off IK mode converts");
  if (force_off_result.ok) {
    const json disabled = json::parse(force_off_result.motion_json);
    ok &= Check(disabled.at("diagnostics").at("ikChains").empty(),
                "force-off skips all source IK chains");
  }

  mb::Input mapping_input = input;
  mapping_input.vmd_bytes = VmdWithDiagnosticTracks();
  mapping_input.max_frames = 8;
  mapping_input.manual_mappings = {
      {"左ひじ補助", "Bip001-L-Prop1"}};
  const auto referenced_manual =
      mb::AnalyzeVmdCompatibilityForReference(
          mapping_input.vmd_bytes, kReference, kSkeleton,
          mapping_input.manual_mappings, {}, 1);
  const auto mapped_manual = std::find_if(
      referenced_manual.mapped_reference_tracks.begin(),
      referenced_manual.mapped_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左ひじ補助" && track.manual &&
               track.target.find("Bip001-L-Prop1") != std::string::npos;
      });
  ok &= Check(referenced_manual.ok &&
                  referenced_manual.reference_mapping_ready &&
                  mapped_manual !=
                      referenced_manual.mapped_reference_tracks.end(),
              "reference-aware audit includes per-character manual mappings");
  const mb::Result mapping_result = mb::BuildMotion(mapping_input);
  ok &= Check(mapping_result.ok, "manual anomaly mapping fixture converts");
  if (mapping_result.ok) {
    const json mapped = json::parse(mapping_result.motion_json);
    const json &track = mapped.at("bones").at("Bip001-L-Prop1");
    const json &rest = mapped.at("rest").at("Bip001-L-Prop1");
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference += std::abs(track.at(7).at(i).get<double>() -
                             rest.at(i).get<double>());
    ok &= Check(track.size() == 8 && difference > 0.05,
                "manual mapping drives an otherwise unmapped target bone");
  }
  mb::Input ignored_input = mapping_input;
  ignored_input.manual_mappings.clear();
  ignored_input.ignored_tracks = {"左ひじ補助"};
  const mb::Result ignored_result = mb::BuildMotion(ignored_input);
  ok &= Check(ignored_result.ok, "ignored anomaly track still converts");
  if (ignored_result.ok) {
    const json ignored = json::parse(ignored_result.motion_json);
    const json &track = ignored.at("bones").at("Bip001-L-Prop1");
    double difference = 0.0;
    for (std::size_t i = 0; i < 4; ++i)
      difference += std::abs(track.at(7).at(i).get<double>() -
                             track.at(0).at(i).get<double>());
    ok &= Check(difference < 1e-6,
                "ignored source does not drive its former manual target");
  }
  const auto ignored_report =
      mb::AnalyzeVmdCompatibilityForReference(
          mapping_input.vmd_bytes, kReference, kSkeleton,
          mapping_input.manual_mappings, {"左ひじ補助"}, 0);
  const auto ignored_source = std::find_if(
      ignored_report.ignored_reference_tracks.begin(),
      ignored_report.ignored_reference_tracks.end(),
      [](const auto &track) {
        return track.name == "左ひじ補助" &&
               track.reason ==
                   mb::VmdCompatibilityReport::MappingIssueReason::Ignored;
      });
  ok &= Check(ignored_report.ok &&
                  ignored_report.reference_mapping_ready &&
                  ignored_source !=
                      ignored_report.ignored_reference_tracks.end(),
              "reference audit reports an explicitly ignored track");
  const auto occupied_report =
      mb::AnalyzeVmdCompatibilityForReference(
          mapping_input.vmd_bytes, kReference, kSkeleton,
          {{"左ひじ補助", "Bip001-L-Hand"}}, {}, 0);
  const auto occupied_issue = std::find_if(
      occupied_report.mapping_issues.begin(),
      occupied_report.mapping_issues.end(),
      [](const auto &issue) {
        return issue.source == "左ひじ補助" &&
               issue.target == "Bip001-L-Hand" &&
               issue.reason == mb::VmdCompatibilityReport::
                                   MappingIssueReason::TargetOccupied;
      });
  ok &= Check(occupied_report.ok &&
                  occupied_issue != occupied_report.mapping_issues.end(),
              "reference audit explains a manually occupied target");
  const auto stale_report =
      mb::AnalyzeVmdCompatibilityForReference(
          mapping_input.vmd_bytes, kReference, kSkeleton,
          {{"MissingSource", "Bip001-L-Prop1"},
           {"AnotherMissing", "MissingTarget"}},
          {}, 0);
  const auto stale_source = std::find_if(
      stale_report.mapping_issues.begin(), stale_report.mapping_issues.end(),
      [](const auto &issue) {
        return issue.source == "MissingSource" &&
               issue.reason == mb::VmdCompatibilityReport::
                                   MappingIssueReason::SourceMissing;
      });
  const auto missing_target = std::find_if(
      stale_report.mapping_issues.begin(), stale_report.mapping_issues.end(),
      [](const auto &issue) {
        return issue.source == "AnotherMissing" &&
               issue.reason == mb::VmdCompatibilityReport::
                                   MappingIssueReason::MissingTarget;
      });
  ok &= Check(stale_report.ok &&
                  stale_source != stale_report.mapping_issues.end() &&
                  missing_target != stale_report.mapping_issues.end(),
              "reference audit distinguishes stale source and missing target");

  json collision_skeleton = json::parse(kSkeleton);
  auto add_bone = [&](const char *name, const int parent,
                      const std::array<double, 3> &translation) {
    const int index = static_cast<int>(collision_skeleton["bones"].size());
    collision_skeleton["bones"].push_back(
        {{"index", index},
         {"name", name},
         {"parent", parent},
         {"baseLocal",
          {{"rotation", {0, 0, 0, 1}}, {"translation", translation}}}});
    return index;
  };
  const int neck = add_bone("Bip001-Neck", 13, {0, 0, 4});
  const int head = add_bone("Bip001-Head", neck, {0, 0, 5});
  add_bone("Bn_hair_test", head, {0, 0, -4});
  add_bone("Bn_hair_test_tip", head + 1, {0, 0, -6});
  const int right_clavicle = add_bone("Bip001-R-Clavicle", 13, {-2, 0, 2});
  const int right_upper = add_bone("Bip001-R-UpperArm", right_clavicle, {-3, 0, 0});
  const int right_forearm = add_bone("Bip001-R-Forearm", right_upper, {-3, 0, 0});
  add_bone("Bip001-R-Hand", right_forearm, {-3, 0, 0});
  const int cloth = add_bone("Bn_qun_test", 0, {5, 0, 0});
  add_bone("Bn_qun_test_tip", cloth, {5, 0, 0});
  const int breast = add_bone("Bn_l_breast_001", 13, {2, 0, 3});
  add_bone("Bn_l_breast_001_tip", breast, {0, 0, -5});
  const int piaodai =
      add_bone("Bn_l_piaodai_001", 0, {5, 0, 4});
  add_bone("Bn_l_piaodai_001_tip", piaodai, {5, 0, -4});
  const int constraint_left =
      add_bone("Bn_hairCL", 0, {3, 0, 0});
  add_bone("Bn_hairCL_tip", constraint_left, {0, 0, -5});
  const int constraint_right =
      add_bone("Bn_hairCR", 0, {-3, 0, 0});
  add_bone("Bn_hairCR_tip", constraint_right, {0, 0, -5});
  mb::Input collision_input = input;
  collision_input.skeleton_json = collision_skeleton.dump();
  collision_input.vmd_bytes = VmdWithBone(
      {0x42, 0x6E, 0x5F, 0x68, 0x61, 0x69, 0x72, 0x5F, 0x74, 0x65, 0x73, 0x74},
      0.0F, 0.0F);
  const mb::Result compatibility_collision_result =
      mb::BuildMotion(collision_input);
  if (!compatibility_collision_result.ok)
    std::cerr << "compatibility collision fixture: "
              << compatibility_collision_result.error << '\n';
  ok &= Check(compatibility_collision_result.ok,
              "v2.3.0 compatibility collision fixture converts");
  if (compatibility_collision_result.ok) {
    const json diagnostics =
        json::parse(compatibility_collision_result.motion_json)
            .at("diagnostics");
    ok &= Check(diagnostics.at("strandCapsulesTorso").get<int>() == 3 &&
                    diagnostics.at("strandCapsulesHead").get<int>() == 0 &&
                    diagnostics.at("strandCapsulesArms").get<int>() == 0,
                "default collision did not match the v2.3.0 torso range");
  }
  collision_input.dynamics_tuning.extended_collision = true;
  const mb::Result collision_result = mb::BuildMotion(collision_input);
  if (!collision_result.ok)
    std::cerr << "collision fixture: " << collision_result.error << '\n';
  ok &= Check(collision_result.ok, "multi-group collision fixture converts");
  if (collision_result.ok) {
    const json collision = json::parse(collision_result.motion_json);
    const json &diagnostics = collision.at("diagnostics");
    ok &= Check(diagnostics.at("strandCapsulesTorso").get<int>() == 3 &&
                    diagnostics.at("strandCapsulesHead").get<int>() == 2 &&
                    diagnostics.at("strandCapsulesArms").get<int>() == 6,
                "strand collision groups omit torso, head, or both arm chains");
  }
  mb::Input breast_amplitude_input = collision_input;
  breast_amplitude_input.vmd_bytes = VmdWithBone(
      {0x42, 0x69, 0x70, 0x30, 0x30, 0x31}, 0.0F, 0.17364818F);
  const mb::Result breast_amplitude_result =
      mb::BuildMotion(breast_amplitude_input);
  breast_amplitude_input.dynamics_tuning.breast_amplitude = 0.0F;
  const mb::Result breast_flat_result =
      mb::BuildMotion(breast_amplitude_input);
  ok &= Check(breast_amplitude_result.ok && breast_flat_result.ok,
              "main-skeleton breast amplitude fixture converts");
  if (breast_amplitude_result.ok && breast_flat_result.ok) {
    const json moving =
        json::parse(breast_amplitude_result.motion_json)
            .at("bones")
            .at("Bn_l_breast_001");
    const json flat =
        json::parse(breast_flat_result.motion_json)
            .at("bones")
            .at("Bn_l_breast_001");
    ok &= Check(moving != flat,
                "main-skeleton breast amplitude did not change chest motion");
  }
  mb::Input no_gravity_input = collision_input;
  no_gravity_input.dynamics_tuning.gravity = 0.0F;
  const mb::Result no_gravity_result =
      mb::BuildMotion(no_gravity_input);
  ok &= Check(no_gravity_result.ok,
              "tuned main-skeleton cloth conversion succeeds");
  if (collision_result.ok && no_gravity_result.ok) {
    const json gravity =
        json::parse(collision_result.motion_json).at("bones").at("Bn_qun_test");
    const json no_gravity =
        json::parse(no_gravity_result.motion_json).at("bones").at("Bn_qun_test");
    ok &= Check(gravity != no_gravity,
                "main-skeleton gravity tuning changes converted cloth");
  }
  mb::Input zero_ribbon_inertia_input = collision_input;
  zero_ribbon_inertia_input.vmd_bytes = VmdWithBone(
      {0x42, 0x6E, 0x5F, 0x6C, 0x5F, 0x70, 0x69,
       0x61, 0x6F, 0x64, 0x61, 0x69, 0x5F, 0x30, 0x30, 0x31},
      0.0F, 0.0F);
  zero_ribbon_inertia_input.dynamics_tuning.material_inertia[
      better_pose::secondary::MotionMaterialIndex(
          better_pose::secondary::MotionMaterial::Ribbon)] =
      0.0F;
  const mb::Result zero_ribbon_inertia_result =
      mb::BuildMotion(zero_ribbon_inertia_input);
  ok &= Check(zero_ribbon_inertia_result.ok,
              "zero-inertia main-skeleton ribbon converts");
  if (zero_ribbon_inertia_result.ok) {
    const json ribbon =
        json::parse(zero_ribbon_inertia_result.motion_json)
            .at("bones")
            .at("Bn_l_piaodai_001")
            .at(0);
    ok &= Check(std::abs(ribbon.at(0).get<double>()) < 1e-7 &&
                    std::abs(ribbon.at(1).get<double>()) < 1e-7 &&
                    std::abs(ribbon.at(2).get<double>()) < 1e-7 &&
                    std::abs(ribbon.at(3).get<double>() - 1.0) < 1e-7,
                "zero main-skeleton ribbon inertia still ran gravity or spring motion");
  }
  mb::Input native_spring_input = collision_input;
  native_spring_input.vmd_bytes = VmdWithBone(
      {0x42, 0x6E, 0x5F, 0x6C, 0x5F, 0x70, 0x69,
       0x61, 0x6F, 0x64, 0x61, 0x69, 0x5F, 0x30, 0x30, 0x31},
      0.0F, 0.0F);
  native_spring_input.secondary_bone_overrides.push_back(
      {"Bn_l_piaodai_001", {0.0, 0.25, 75.0}, 0.0, 1.0, {}, 0.0});
  const mb::Result native_spring_result =
      mb::BuildMotion(native_spring_input);
  ok &= Check(native_spring_result.ok,
              "main-skeleton native spring override converts");
  if (native_spring_result.ok) {
    const json native_document =
        json::parse(native_spring_result.motion_json);
    const json ribbon =
        native_document.at("bones").at("Bn_l_piaodai_001").at(0);
    ok &= Check(std::abs(ribbon.at(0).get<double>()) < 1e-7 &&
                    std::abs(ribbon.at(1).get<double>()) < 1e-7 &&
                    std::abs(ribbon.at(2).get<double>()) < 1e-7 &&
                    std::abs(ribbon.at(3).get<double>() - 1.0) < 1e-7,
                "main-skeleton ignored the captured native spring override");
    ok &= Check(
        native_document.at("diagnostics")
                .at("nativeSecondaryOverrides")
                .get<int>() ==
            1,
        "main-skeleton native override diagnostic did not report the match");
  }
  mb::Input constraint_input = collision_input;
  constraint_input.vmd_bytes =
      VmdWithBone({0x42, 0x6E, 0x5F, 0x68, 0x61, 0x69, 0x72,
                   0x43, 0x4C},
                  0.0F, 0.0F);
  constraint_input.dynamics_tuning.bone_constraints = true;
  constraint_input.dynamics_tuning.self_collision = true;
  const mb::Result constraint_result = mb::BuildMotion(constraint_input);
  ok &= Check(constraint_result.ok,
              "main-skeleton distance-constraint fixture converts");
  if (constraint_result.ok) {
    const json constraint_document =
        json::parse(constraint_result.motion_json);
    ok &= Check(
        constraint_document.at("diagnostics")
                .at("distanceConstraintLinks")
                .get<int>() >=
            1,
        "main-skeleton did not discover an adjacent strand constraint");
    const auto &constraint_diagnostics =
        constraint_document.at("diagnostics");
    const bool have_self_link =
        constraint_diagnostics.at("selfCollisionLinks").get<int>() >= 1;
    ok &= Check(have_self_link,
                "main-skeleton did not discover a self-collision candidate");
    if (!have_self_link)
      std::cerr << constraint_diagnostics.dump() << '\n';
  }

  if (!ok)
    return 1;
  std::cout << "PASS StandardRig anatomy, twists, D/EX legs and FK fallback\n";
  return 0;
}
