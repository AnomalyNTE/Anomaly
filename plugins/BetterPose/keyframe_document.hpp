#pragma once

#include "keyframe_track.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Pure keyframe-file serialization. Local poses are owned values, by bone name;
// documents contain no library identifiers, paths, or runtime skeleton indices.
namespace better_pose::keyframe_document {
namespace detail {

inline bool Finite3(const nlohmann::json &value, std::array<double, 3> &out) {
  if (!value.is_array() || value.size() != 3)
    return false;
  for (std::size_t axis{}; axis != 3; ++axis) {
    if (!value[axis].is_number())
      return false;
    out[axis] = value[axis].get<double>();
    if (!std::isfinite(out[axis]))
      return false;
  }
  return true;
}

inline bool ReadLocalPose(const nlohmann::json &pose,
                          std::vector<keyframes::LocalBone> &out) {
  if (!pose.is_object() || pose.empty() || pose.size() > 8192)
    return false;
  out.reserve(pose.size());
  for (auto item = pose.begin(); item != pose.end(); ++item) {
    if (item.key().empty() || !item.value().is_array() || item.value().size() != 12)
      return false;
    keyframes::LocalBone bone;
    bone.name = item.key();
    for (std::size_t axis{}; axis != 12; ++axis) {
      if (!item.value()[axis].is_number())
        return false;
      bone.transform[axis] = item.value()[axis].get<double>();
      if (!std::isfinite(bone.transform[axis]))
        return false;
    }
    // Rescale first so any finite nonzero quaternion can be normalized without
    // overflow or underflow. FTransform padding is not part of the pose.
    double maximum = 0.0;
    for (std::size_t axis{}; axis != 4; ++axis)
      maximum = (std::max)(maximum, std::abs(bone.transform[axis]));
    if (!(maximum > 0.0))
      return false;
    double squared = 0.0;
    for (std::size_t axis{}; axis != 4; ++axis) {
      bone.transform[axis] /= maximum;
      squared += bone.transform[axis] * bone.transform[axis];
    }
    const double norm = std::sqrt(squared);
    for (std::size_t axis{}; axis != 4; ++axis)
      bone.transform[axis] /= norm;
    // q and -q encode the same rotation; use the track's canonical sign.
    const auto &q = bone.transform;
    const double first = q[3] != 0.0 ? q[3] : q[2] != 0.0 ? q[2] : q[1] != 0.0 ? q[1] : q[0];
    if (first < 0.0)
      for (std::size_t axis{}; axis != 4; ++axis)
        bone.transform[axis] = -bone.transform[axis];
    bone.transform[7] = 0.0;
    bone.transform[11] = 0.0;
    out.push_back(std::move(bone));
  }
  return true;
}

inline std::uint32_t Retime(const double frame) {
  // Clamp before rounding: finite authoring rates may produce an overflowing
  // scaled length/frame, and llround must never see values outside its range.
  return static_cast<std::uint32_t>(
      std::llround(std::clamp(frame, 0.0, static_cast<double>(keyframes::kMaximumFrame))));
}

}  // namespace detail

inline std::string Build(const std::vector<keyframes::Key> &source,
                         const std::uint32_t length) {
  nlohmann::json root;
  root["format"] = "betterpose-keyframes";
  root["version"] = std::any_of(source.begin(), source.end(),
                                 [](const auto &key) { return key.has_local_pose; }) ? 2 : 1;
  root["fps"] = keyframes::kFramesPerSecond;
  root["length"] = length;
  auto keys = nlohmann::json::array();
  for (const auto &key : source) {
    nlohmann::json item;
    item["frame"] = key.frame;
    item["ease"] = key.ease == keyframes::Ease::InOut ? "inout" : "linear";
    if (key.has_pose || key.has_local_pose) {
      auto bones = nlohmann::json::array();
      for (const auto &bone : key.bones)
        bones.push_back(
            {{"name", bone.name}, {"pitch", bone.pitch}, {"yaw", bone.yaw}, {"roll", bone.roll}});
      item["pose"] = {{"bones", std::move(bones)}, {"rootOffset", key.root_offset}};
      if (key.has_local_pose) {
        auto local = nlohmann::json::object();
        for (const auto &bone : key.local_pose) {
          auto transform = bone.transform;
          transform[7] = 0.0;
          transform[11] = 0.0;
          local[bone.name] = transform;
        }
        item["pose"]["localPose"] = std::move(local);
      }
    }
    if (key.has_expression) {
      auto morphs = nlohmann::json::array();
      for (const auto &morph : key.morphs)
        morphs.push_back({{"name", morph.name}, {"weight", morph.weight}});
      item["expression"] = std::move(morphs);
    }
    if (key.has_camera)
      item["camera"] = {{"focus", key.camera.focus}, {"yaw", key.camera.yaw},
                        {"pitch", key.camera.pitch}, {"distance", key.camera.distance},
                        {"fov", key.camera.fov}};
    keys.push_back(std::move(item));
  }
  root["keys"] = std::move(keys);
  return root.dump(2);
}

// Returns false for a non-keyframe document or unsupported version. Within a
// recognized file, unreadable keys are skipped. In particular, the presence of
// localPose is authoritative: a corrupt full pose must not become a legacy key.
// As in the original parser, accepted keys append to `keys`.
inline bool Parse(const std::string &document, std::vector<keyframes::Key> &keys,
                  std::uint32_t &length, std::size_t &skipped) {
  const auto root = nlohmann::json::parse(document, nullptr, false);
  if (!root.is_object() || !root.contains("format") || !root["format"].is_string() ||
      root["format"].get<std::string>() != "betterpose-keyframes" ||
      !root.contains("keys") || !root["keys"].is_array())
    return false;
  if (root.contains("version")) {
    const auto &version = root["version"];
    if (!version.is_number_integer() || (version != 1 && version != 2))
      return false;
  }
  length = 0;
  if (root.contains("length") && root["length"].is_number_unsigned())
    length = static_cast<std::uint32_t>((std::min)(root["length"].get<std::uint64_t>(),
                                      static_cast<std::uint64_t>(keyframes::kMaximumFrame)));
  double scale = 1.0;
  if (root.contains("fps") && root["fps"].is_number()) {
    const double fps = root["fps"].get<double>();
    if (std::isfinite(fps) && fps > 0.0)
      scale = keyframes::kFramesPerSecond / fps;
  }
  length = length == 0 ? 0 : detail::Retime(static_cast<double>(length) * scale);
  skipped = 0;
  for (const auto &item : root["keys"]) {
    if (keys.size() >= keyframes::kMaximumKeys)
      break;
    if (!item.is_object() || !item.contains("frame") || !item["frame"].is_number()) {
      ++skipped;
      continue;
    }
    try {
      const double raw_frame = item["frame"].get<double>() * scale;
      if (!std::isfinite(raw_frame) || raw_frame < 0.0) {
        ++skipped;
        continue;
      }
      keyframes::Key key;
      key.frame = detail::Retime(raw_frame);
      key.ease = item.value("ease", std::string("inout")) == "linear" ? keyframes::Ease::Linear
                                                                        : keyframes::Ease::InOut;
      if (item.contains("pose") && item["pose"].is_object()) {
        const auto &pose = item["pose"];
        key.has_pose = true;
        if (pose.contains("localPose")) {
          if (!detail::ReadLocalPose(pose["localPose"], key.local_pose)) {
            ++skipped;
            continue;
          }
          key.has_local_pose = true;
        }
        if (pose.contains("bones") && pose["bones"].is_array())
          for (const auto &bone : pose["bones"]) {
            if (!bone.is_object() || !bone.contains("name") || !bone["name"].is_string())
              continue;
            const double p = bone.value("pitch", 0.0), y = bone.value("yaw", 0.0),
                         r = bone.value("roll", 0.0);
            if (!std::isfinite(p) || !std::isfinite(y) || !std::isfinite(r))
              continue;
            key.bones.push_back({bone["name"].get<std::string>(), keyframes::WrapDegrees(p),
                                 keyframes::WrapDegrees(y), keyframes::WrapDegrees(r)});
          }
        if (pose.contains("rootOffset"))
          static_cast<void>(detail::Finite3(pose["rootOffset"], key.root_offset));
      }
      if (item.contains("expression") && item["expression"].is_array()) {
        key.has_expression = true;
        for (const auto &morph : item["expression"]) {
          if (!morph.is_object() || !morph.contains("name") || !morph["name"].is_string() ||
              !morph.contains("weight") || !morph["weight"].is_number())
            continue;
          const float weight = morph["weight"].get<float>();
          if (std::isfinite(weight))
            key.morphs.push_back({morph["name"].get<std::string>(), std::clamp(weight, 0.0F, 1.0F)});
        }
      }
      if (item.contains("camera") && item["camera"].is_object()) {
        const auto &camera = item["camera"];
        keyframes::CameraKey shot;
        const double yaw = camera.value("yaw", 0.0), pitch = camera.value("pitch", 0.0),
                     distance = camera.value("distance", 300.0);
        const float fov = camera.value("fov", 0.0F);
        if (camera.contains("focus") && detail::Finite3(camera["focus"], shot.focus) &&
            std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(distance) &&
            std::isfinite(fov)) {
          shot.yaw = yaw;
          shot.pitch = pitch;
          shot.distance = distance;
          shot.fov = fov;
          key.has_camera = true;
          key.camera = shot;
        }
      }
      keys.push_back(std::move(key));
    } catch (const nlohmann::json::exception &) {
      ++skipped;
    }
  }
  return true;
}

}  // namespace better_pose::keyframe_document
