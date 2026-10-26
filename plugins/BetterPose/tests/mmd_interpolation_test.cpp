#include "../retarget/motion_builder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace mb = better_pose::mmd2bip;

namespace {

bool Check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool Near(const double actual, const double expected, const double epsilon = 1e-5) {
  return std::abs(actual - expected) <= epsilon;
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

void CameraCurve(std::array<std::uint8_t, 24> &bytes, const int channel,
                 const std::array<std::uint8_t, 4> &x1_x2_y1_y2) {
  const std::size_t at = static_cast<std::size_t>(channel) * 4;
  for (std::size_t i = 0; i < x1_x2_y1_y2.size(); ++i)
    bytes[at + i] = x1_x2_y1_y2[i];
}

std::vector<std::uint8_t> FixtureVmd() {
  std::vector<std::uint8_t> bytes;
  Fixed(bytes, "Vocaloid Motion Data 0002", 30);
  Fixed(bytes, "interpolation fixture", 20);

  Pod<std::uint32_t>(bytes, 2);
  for (std::uint32_t key = 0; key < 2; ++key) {
    Fixed(bytes, "center", 15);
    Pod<std::uint32_t>(bytes, key * 10);
    const float position[3] = {static_cast<float>(key * 10), 0.0F, 0.0F};
    const float rotation[4] = {
        0.0F, 0.0F, key == 0 ? 0.0F : 1.0F, key == 0 ? 1.0F : 0.0F};
    for (const float value : position)
      Pod(bytes, value);
    for (const float value : rotation)
      Pod(bytes, value);

    std::array<std::uint8_t, 64> interpolation{};
    for (std::size_t channel = 0; channel < 4; ++channel)
      interpolation[channel + 8] = interpolation[channel + 12] = 127;
    if (key == 1) {
      interpolation[0] = 0;
      interpolation[4] = 0;
      interpolation[8] = 127;
      interpolation[12] = 32;
      interpolation[3] = 0;
      interpolation[7] = 0;
      interpolation[11] = 127;
      interpolation[15] = 32;
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
  }

  Pod<std::uint32_t>(bytes, 0);  // morphs
  Pod<std::uint32_t>(bytes, 2);  // camera
  for (std::uint32_t key = 0; key < 2; ++key) {
    Pod<std::uint32_t>(bytes, key * 10);
    Pod<float>(bytes, static_cast<float>(key * 10));
    const float position[3] = {static_cast<float>(key * 10), 0.0F, 0.0F};
    const float rotation[3] = {static_cast<float>(key), 0.0F, 0.0F};
    for (const float value : position)
      Pod(bytes, value);
    for (const float value : rotation)
      Pod(bytes, value);

    std::array<std::uint8_t, 24> interpolation{};
    for (int channel = 0; channel < 6; ++channel)
      CameraCurve(interpolation, channel, {0, 127, 0, 127});
    if (key == 1) {
      CameraCurve(interpolation, 0, {0, 127, 0, 32});
      CameraCurve(interpolation, 3, {0, 127, 0, 32});
      CameraCurve(interpolation, 4, {0, 127, 0, 32});
      CameraCurve(interpolation, 5, {0, 127, 0, 32});
    }
    bytes.insert(bytes.end(), interpolation.begin(), interpolation.end());
    Pod<std::uint32_t>(bytes, 30U + key * 40U);
    const std::uint8_t perspective = 0;
    Pod(bytes, perspective);
  }

  Pod<std::uint32_t>(bytes, 0);  // lights
  Pod<std::uint32_t>(bytes, 0);  // self-shadow
  Pod<std::uint32_t>(bytes, 0);  // visibility / IK
  return bytes;
}

}  // namespace

int main() {
  bool ok = true;

  ok &= Check(Near(mb::SampleVmdCurve({}, 0.0), 0.0), "linear start");
  ok &= Check(Near(mb::SampleVmdCurve({}, 0.5), 0.5), "linear midpoint");
  ok &= Check(Near(mb::SampleVmdCurve({}, 1.0), 1.0), "linear end");

  const mb::VmdCurve eased{0.0, 0.0, 1.0, 0.25};
  ok &= Check(Near(mb::SampleVmdCurve(eased, 0.5), 0.21875),
              "asymmetric Bezier midpoint");

  const auto fixture = FixtureVmd();
  const mb::VmdDocument document = mb::ParseVmd(fixture);
  ok &= Check(document.ok && document.walk_exact, "fixture VMD parses exactly");
  ok &= Check(document.tracks.size() == 1 && document.tracks.front().keys.size() == 2,
              "bone keys parsed");
  ok &= Check(document.camera.size() == 2, "camera keys parsed");
  if (!document.ok || document.tracks.empty() || document.camera.size() != 2)
    return 1;

  double position[3]{};
  double rotation[4]{};
  const double eased_midpoint = 0.125 + 0.375 * (32.0 / 127.0);
  ok &= Check(mb::SampleTrack(document.tracks.front(), 5.0, rotation, position),
              "bone track samples");
  ok &= Check(Near(position[0], eased_midpoint * 10.0), "bone X uses its own Bezier");
  ok &= Check(Near(position[1], 0.0) && Near(position[2], 0.0),
              "untouched bone channels stay fixed");
  const double half_angle = eased_midpoint * 3.14159265358979323846 / 2.0;
  ok &= Check(Near(rotation[2], std::sin(half_angle)) &&
                  Near(rotation[3], std::cos(half_angle)),
              "bone rotation uses its Bezier");

  const mb::VmdCameraPose camera = mb::SampleCameraTrack(document.camera, 5.0);
  ok &= Check(camera.valid, "camera track samples");
  ok &= Check(Near(camera.position[0], eased_midpoint * 10.0),
              "camera target uses Bezier");
  ok &= Check(Near(camera.rotation[0], eased_midpoint),
              "camera rotation uses Bezier");
  ok &= Check(Near(camera.distance, eased_midpoint * 10.0),
              "camera distance uses Bezier");
  ok &= Check(Near(camera.view_angle, 30.0 + eased_midpoint * 40.0),
              "camera FOV uses Bezier");

  const mb::VmdCameraPose end = mb::SampleCameraTrack(document.camera, 10.0);
  ok &= Check(end.valid && Near(end.position[0], 10.0) &&
                  Near(end.rotation[0], 1.0) &&
                  Near(end.distance, 10.0) && Near(end.view_angle, 70.0),
              "camera endpoint is exact");

  if (!ok)
    return 1;
  std::cout << "PASS MMD bone/camera Bezier parsing and sampling\n";
  return 0;
}
