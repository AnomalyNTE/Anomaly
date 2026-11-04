#pragma once

#include "keyframe_track.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// Blender's portable motion format is sparse and stores local rotation deltas.
// Native BetterPose MMD playback uses dense absolute local rotations, so this
// file validates the exchange document and expands it against the live
// character's reference pose before the normal MMD loader sees it.
namespace better_pose::blender_motion {

inline constexpr std::string_view kFormat = "betterpose-blender-motion";
inline constexpr unsigned kVersion = 1;
inline constexpr std::string_view kExtension = ".bpmotion";
inline constexpr std::size_t kMaximumBones = 4096;
inline constexpr std::size_t kMaximumFrames = 108001;
inline constexpr double kMmdFramesPerSecond = 30.0;

struct BoneSample {
  std::uint32_t index{};
  bool has_rotation{};
  std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
  bool has_position{};
  std::array<double, 3> position{};
};

struct CameraSample {
  std::array<double, 3> focus{};
  double yaw{};
  double pitch{};
  double distance{};
  double fov{};
};

struct MmdCameraKey {
  std::uint32_t frame{};
  double distance{};
  std::array<double, 3> position{};
  std::array<double, 3> rotation{};
  struct Curve {
    double x1{};
    double y1{};
    double x2{1.0};
    double y2{1.0};
  };
  std::array<Curve, 6> curves{};
  double view_angle{};
  bool perspective{};
};

struct Frame {
  double time{};
  std::array<double, 3> root{};
  std::vector<BoneSample> bones;
  std::map<std::string, float> faces;
  std::optional<CameraSample> camera;
};

struct Document {
  std::string model;
  double fps{30.0};
  double length{};
  bool bone_positions_are_deltas{};
  std::vector<std::string> bone_names;
  std::vector<std::int32_t> bone_parents;
  std::vector<Frame> frames;
  std::optional<std::vector<MmdCameraKey>> camera_track;
};

struct MmdBuildReport {
  std::uint32_t frame_count{};
  std::size_t bone_tracks{};
  std::size_t offset_tracks{};
  std::size_t morph_tracks{};
  std::size_t camera_keys{};
};

namespace detail {

inline bool Finite(const nlohmann::json &value) {
  return value.is_number() && std::isfinite(value.get<double>());
}

inline bool Finite3(const nlohmann::json &value, std::array<double, 3> &out) {
  if (!value.is_array() || value.size() != 3)
    return false;
  for (std::size_t axis{}; axis != 3; ++axis) {
    if (!Finite(value[axis]))
      return false;
    out[axis] = value[axis].get<double>();
  }
  return true;
}

inline bool ParseCameraTrack(const nlohmann::json &value,
                             std::vector<MmdCameraKey> &keys,
                             std::string &error) {
  keys.clear();
  const nlohmann::json *items = nullptr;
  if (value.is_array()) {
    items = &value;
  } else if (value.is_object()) {
    const auto found = value.find("keys");
    if (found != value.end()) {
      if (!found->is_array()) {
        error = "Invalid Blender motion camera track";
        return false;
      }
      items = &*found;
    }
  } else {
    error = "Invalid Blender motion camera track";
    return false;
  }
  if (items == nullptr || items->empty()) {
    error.clear();
    return true;
  }
  if (items->size() > kMaximumFrames) {
    error = "Invalid Blender motion frame count";
    return false;
  }
  keys.reserve(items->size());
  for (const auto &item : *items) {
    if (!item.is_object() || !item.contains("frame") ||
        !item["frame"].is_number_integer()) {
      error = "Invalid Blender motion camera track";
      return false;
    }
    const auto raw_frame = item["frame"].get<long long>();
    if (raw_frame < 0 || raw_frame > 600000) {
      error = "Invalid Blender motion camera track";
      return false;
    }
    MmdCameraKey key;
    key.frame = static_cast<std::uint32_t>(raw_frame);
    key.distance = item.value("distance", 0.0);
    std::array<double, 3> position{};
    std::array<double, 3> rotation{};
    if (!std::isfinite(key.distance) || std::abs(key.distance) > 100000.0 ||
        !Finite3(item.value("position", nlohmann::json::array()), position) ||
        !Finite3(item.value("rotation", nlohmann::json::array()), rotation)) {
      error = "Blender motion camera track contains invalid values";
      return false;
    }
    key.position = position;
    key.rotation = rotation;
    key.view_angle = static_cast<double>(
        std::clamp<std::int64_t>(item.value("view_angle", 0LL), 1LL, 179LL));
    key.perspective = item.value("perspective", true);
    if (item.contains("curves")) {
      const auto &curves = item["curves"];
      if (!curves.is_array() || curves.size() != key.curves.size()) {
        error = "Blender motion camera track contains invalid values";
        return false;
      }
      for (std::size_t index{}; index != key.curves.size(); ++index) {
        const auto &curve = curves[index];
        if (!curve.is_object()) {
          error = "Blender motion camera track contains invalid values";
          return false;
        }
        auto &target = key.curves[index];
        for (const auto &property :
             {std::pair{"x1", &target.x1}, std::pair{"y1", &target.y1},
              std::pair{"x2", &target.x2}, std::pair{"y2", &target.y2}}) {
          const auto found = curve.find(property.first);
          if (found == curve.end())
            continue;
          if (!Finite(*found)) {
            error = "Blender motion camera track contains invalid values";
            return false;
          }
          *property.second = std::clamp(found->get<double>(), 0.0, 1.0);
        }
      }
    }
    keys.push_back(key);
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const auto &left, const auto &right) {
                     return left.frame < right.frame;
                   });
  error.clear();
  return true;
}

inline MmdCameraKey CameraKeyFromSample(const std::uint32_t frame,
                                        const CameraSample &camera,
                                        const double unit_cm) {
  constexpr double kPi = 3.14159265358979323846;
  const double yaw = camera.yaw * kPi / 180.0;
  const double pitch = camera.pitch * kPi / 180.0;
  const std::array<double, 3> direction{
      std::cos(pitch) * std::cos(yaw),
      std::cos(pitch) * std::sin(yaw),
      std::sin(pitch),
  };
  // Undo MMD -> Blender: local=(x,-z,y), then mirror X.
  const std::array<double, 3> target_mmd{
      -camera.focus[0] / unit_cm,
      camera.focus[2] / unit_cm,
      -camera.focus[1] / unit_cm,
  };
  const std::array<double, 3> forward_mmd{
      -direction[0], direction[2], -direction[1]};
  MmdCameraKey key;
  key.frame = frame;
  key.position = target_mmd;
  key.rotation[0] = std::asin(std::clamp(-forward_mmd[1], -1.0, 1.0));
  key.rotation[1] = std::atan2(forward_mmd[0], forward_mmd[2]);
  key.rotation[2] = 0.0;
  key.distance = camera.distance / unit_cm;
  key.view_angle = std::clamp(camera.fov, 1.0, 179.0);
  return key;
}

inline keyframes::Quat Normalize(keyframes::Quat q) {
  const double length =
      std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(length > 1e-12))
    return {};
  return {q.x / length, q.y / length, q.z / length, q.w / length};
}

inline keyframes::Quat Multiply(const keyframes::Quat &a,
                                const keyframes::Quat &b) {
  return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

inline keyframes::Quat Slerp(keyframes::Quat a, keyframes::Quat b,
                             const double amount) {
  a = Normalize(a);
  b = Normalize(b);
  double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (dot < 0.0) {
    b = {-b.x, -b.y, -b.z, -b.w};
    dot = -dot;
  }
  double wa = 1.0 - amount;
  double wb = amount;
  if (dot < 0.9995) {
    const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
    const double sine = std::sin(theta);
    wa = std::sin((1.0 - amount) * theta) / sine;
    wb = std::sin(amount * theta) / sine;
  }
  return Normalize({wa * a.x + wb * b.x, wa * a.y + wb * b.y,
                    wa * a.z + wb * b.z, wa * a.w + wb * b.w});
}

inline const BoneSample *FindBone(const Frame &frame,
                                  const std::uint32_t index) {
  const auto found = std::find_if(
      frame.bones.begin(), frame.bones.end(),
      [index](const BoneSample &bone) { return bone.index == index; });
  return found == frame.bones.end() ? nullptr : &*found;
}

inline std::pair<std::size_t, std::size_t> Bounds(
    const std::vector<Frame> &frames, const double time) {
  const auto next = std::upper_bound(
      frames.begin(), frames.end(), time,
      [](const double value, const Frame &frame) { return value < frame.time; });
  if (next == frames.begin())
    return {0, 0};
  const std::size_t lower = static_cast<std::size_t>(next - frames.begin() - 1);
  const std::size_t upper =
      next == frames.end() ? lower : static_cast<std::size_t>(next - frames.begin());
  return {lower, upper};
}

inline double BlendAmount(const Frame &lower, const Frame &upper,
                          const double time) {
  const double span = upper.time - lower.time;
  return span > 1e-12
             ? std::clamp((time - lower.time) / span, 0.0, 1.0)
             : 0.0;
}

inline keyframes::Quat ReferenceRotation(
    const std::vector<std::array<double, 12>> &reference,
    const std::size_t index) {
  const auto &raw = reference[index];
  return Normalize({raw[0], raw[1], raw[2], raw[3]});
}

}  // namespace detail

inline bool Parse(const std::string &text,
                  const std::vector<std::string> &fallback_bone_names,
                  Document &document, std::string &error,
                  std::size_t &ignored_samples) {
  ignored_samples = 0;
  const auto root = nlohmann::json::parse(text, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    error = "Blender file is not valid JSON";
    return false;
  }
  if (root.value("format", std::string{}) != std::string(kFormat) ||
      root.value("version", 0U) != kVersion) {
    error = "Choose a Better Pose .bpmotion file";
    return false;
  }

  Document parsed;
  parsed.model = root.value("model", std::string{});
  parsed.fps = root.value("fps", 30.0);
  parsed.length = root.value("length", 0.0);
  parsed.bone_positions_are_deltas =
      root.value("bone_position_mode", std::string{}) == "delta";
  if (!std::isfinite(parsed.fps) || parsed.fps < 1.0 || parsed.fps > 240.0) {
    error = "Invalid Blender motion frame rate";
    return false;
  }
  if (!std::isfinite(parsed.length) || parsed.length < 0.0) {
    error = "Invalid Blender motion duration";
    return false;
  }
  const auto camera_track = root.find("camera_track");
  if (camera_track != root.end() && !camera_track->is_null()) {
    std::vector<MmdCameraKey> keys;
    if (!detail::ParseCameraTrack(*camera_track, keys, error))
      return false;
    if (!keys.empty())
      parsed.camera_track = std::move(keys);
  }

  const auto names = root.find("bone_names");
  if (names != root.end() && names->is_array()) {
    if (names->size() > kMaximumBones) {
      error = "Too many Blender motion bones";
      return false;
    }
    parsed.bone_names.reserve(names->size());
    for (const auto &name : *names) {
      if (!name.is_string() || name.get_ref<const std::string &>().empty()) {
        error = "Invalid Blender motion bone name";
        return false;
      }
      parsed.bone_names.push_back(name.get<std::string>());
    }
  }
  if (parsed.bone_names.empty())
    parsed.bone_names = fallback_bone_names;
  if (parsed.bone_names.empty() || parsed.bone_names.size() > kMaximumBones) {
    error = "Blender motion contains no usable bones";
    return false;
  }

  const auto parents = root.find("bone_parents");
  if (parents != root.end() && parents->is_array() &&
      parents->size() == parsed.bone_names.size()) {
    parsed.bone_parents.reserve(parents->size());
    for (const auto &parent : *parents) {
      if (!parent.is_number_integer()) {
        error = "Invalid Blender motion bone parent";
        return false;
      }
      parsed.bone_parents.push_back(parent.get<std::int32_t>());
    }
  }

  const auto frames = root.find("frames");
  if (frames == root.end() || !frames->is_array() || frames->empty() ||
      frames->size() > kMaximumFrames) {
    error = "Invalid Blender motion frame count";
    return false;
  }

  try {
    parsed.frames.reserve(frames->size());
    double previous_time = -1.0;
    for (const auto &item : *frames) {
      if (!item.is_object() || !item.contains("time") ||
          !detail::Finite(item["time"])) {
        error = "Blender motion frame has no valid time";
        return false;
      }
      Frame frame;
      frame.time = item["time"].get<double>();
      if (frame.time < 0.0 || frame.time <= previous_time) {
        error = "Blender timeline must start at zero and increase strictly";
        return false;
      }
      if (parsed.frames.empty() && frame.time > 1e-6) {
        error = "Blender timeline must start at zero";
        return false;
      }
      previous_time = frame.time;

      const auto frame_root = item.find("root");
      if (frame_root != item.end() &&
          !detail::Finite3(*frame_root, frame.root)) {
        error = "Invalid Blender motion root translation";
        return false;
      }

      const auto camera = item.find("camera");
      if (camera != item.end() && !camera->is_null()) {
        if (!camera->is_object() || !camera->contains("focus") ||
            !camera->contains("yaw") || !camera->contains("pitch") ||
            !camera->contains("distance") || !camera->contains("fov")) {
          error = "Invalid Blender motion camera track";
          return false;
        }
        CameraSample sample;
        if (!detail::Finite3((*camera)["focus"], sample.focus) ||
            !detail::Finite((*camera)["yaw"]) ||
            !detail::Finite((*camera)["pitch"]) ||
            !detail::Finite((*camera)["distance"]) ||
            !detail::Finite((*camera)["fov"])) {
          error = "Blender motion camera track contains invalid values";
          return false;
        }
        sample.yaw = (*camera)["yaw"].get<double>();
        sample.pitch = (*camera)["pitch"].get<double>();
        sample.distance = (*camera)["distance"].get<double>();
        sample.fov = (*camera)["fov"].get<double>();
        if (sample.distance > 0.0 && sample.fov > 0.0 && sample.fov < 179.0)
          frame.camera = sample;
      }

      const auto bones = item.find("bones");
      if (bones != item.end()) {
        if (!bones->is_array()) {
          error = "Invalid Blender motion bone frame";
          return false;
        }
        std::set<std::uint32_t> seen;
        frame.bones.reserve((std::min)(bones->size(), parsed.bone_names.size()));
        for (const auto &bone : *bones) {
          if (!bone.is_object() || !bone.contains("i") ||
              !bone["i"].is_number_integer()) {
            ++ignored_samples;
            continue;
          }
          const auto raw_index = bone["i"].get<long long>();
          if (raw_index < 0 ||
              static_cast<std::size_t>(raw_index) >= parsed.bone_names.size() ||
              !seen.insert(static_cast<std::uint32_t>(raw_index)).second) {
            ++ignored_samples;
            continue;
          }
          BoneSample sample;
          sample.index = static_cast<std::uint32_t>(raw_index);
          const auto q = bone.find("q");
          if (q != bone.end() && q->is_array() && q->size() == 4) {
            std::array<double, 4> raw{};
            bool valid = true;
            for (std::size_t axis{}; axis != 4; ++axis) {
              valid = valid && detail::Finite((*q)[axis]);
              if (valid)
                raw[axis] = (*q)[axis].get<double>();
            }
            const double norm = raw[0] * raw[0] + raw[1] * raw[1] +
                                raw[2] * raw[2] + raw[3] * raw[3];
            if (valid && norm >= 0.25 && norm <= 2.25) {
              sample.has_rotation = true;
              const auto normalized =
                  detail::Normalize({raw[0], raw[1], raw[2], raw[3]});
              sample.rotation = {normalized.x, normalized.y, normalized.z,
                                 normalized.w};
            }
          }
          if (!sample.has_rotation) {
            if (const auto r = bone.find("r"); r != bone.end()) {
              std::array<double, 3> angles{};
              if (detail::Finite3(*r, angles)) {
                const auto value =
                    keyframes::FromRotator(angles[0], angles[1], angles[2]);
                sample.has_rotation = true;
                sample.rotation = {value.x, value.y, value.z, value.w};
              }
            }
          }
          if (const auto p = bone.find("p"); p != bone.end())
            sample.has_position = detail::Finite3(*p, sample.position);
          if (!sample.has_rotation && !sample.has_position) {
            ++ignored_samples;
            continue;
          }
          frame.bones.push_back(sample);
        }
      }

      const auto faces = item.find("faces");
      if (faces != item.end()) {
        if (!faces->is_object() || faces->size() > kMaximumBones) {
          error = "Invalid Blender motion expression track";
          return false;
        }
        for (auto face = faces->begin(); face != faces->end(); ++face) {
          if (!face.value().is_number() ||
              !std::isfinite(face.value().get<double>()))
            continue;
          frame.faces[face.key()] =
              std::clamp(face.value().get<float>(), 0.0F, 1.0F);
        }
      }
      parsed.frames.push_back(std::move(frame));
    }
  } catch (...) {
    error = "Corrupt Blender motion data";
    return false;
  }

  document = std::move(parsed);
  error.clear();
  return true;
}

inline bool BuildCameraKeys(const Document &source, const double unit_cm,
                            std::vector<MmdCameraKey> &keys,
                            std::string &error) {
  constexpr double kPi = 3.14159265358979323846;
  keys.clear();
  if (source.camera_track.has_value()) {
    keys = *source.camera_track;
    error.clear();
    return true;
  }
  if (!std::isfinite(unit_cm) || unit_cm <= 1e-6) {
    error = "Camera import failed: invalid character unit scale";
    return false;
  }
  double previous_yaw = 0.0;
  bool have_previous_yaw = false;
  for (const auto &frame : source.frames) {
    if (!frame.camera)
      continue;
    const auto frame_number = static_cast<std::uint32_t>(
        std::clamp<long long>(
            std::llround(frame.time * kMmdFramesPerSecond), 0, 600000));
    MmdCameraKey key =
        detail::CameraKeyFromSample(frame_number, *frame.camera, unit_cm);
    // atan2 returns every yaw in [-pi, pi]. A camera smoothly crossing that boundary
    // otherwise appears to spin almost a full turn in one frame because the sampler
    // interpolates the Euler components directly. Keep the same direction while
    // unwrapping each key against the previous one.
    if (have_previous_yaw) {
      while (key.rotation[1] - previous_yaw > kPi)
        key.rotation[1] -= 2.0 * kPi;
      while (key.rotation[1] - previous_yaw < -kPi)
        key.rotation[1] += 2.0 * kPi;
    }
    previous_yaw = key.rotation[1];
    have_previous_yaw = true;
    const auto existing = std::lower_bound(
        keys.begin(), keys.end(), key.frame,
        [](const MmdCameraKey &value, const std::uint32_t wanted) {
          return value.frame < wanted;
        });
    if (existing != keys.end() && existing->frame == key.frame)
      *existing = key;
    else
      keys.insert(existing, key);
  }
  error.clear();
  return true;
}

inline bool BuildMmdDocument(
    const Document &source,
    const std::vector<std::string> &target_bone_names,
    const std::vector<std::array<double, 12>> &reference_locals,
    std::string &out, MmdBuildReport &report, std::string &error) {
  using Json = nlohmann::json;
  report = {};
  if (source.bone_names.empty() || source.frames.empty()) {
    error = "Blender motion has no importable content";
    return false;
  }
  if (target_bone_names.size() != reference_locals.size() ||
      target_bone_names.empty()) {
    error = "Current character reference pose is not ready";
    return false;
  }

  double duration =
      source.fps > 0.0 ? source.length / source.fps : 0.0;
  duration = (std::max)(duration, source.frames.back().time);
  const auto frame_count = static_cast<std::uint32_t>(std::clamp<long long>(
      std::llround(duration * kMmdFramesPerSecond) + 1, 1, 600000));

  std::unordered_map<std::string, std::uint32_t> target_by_name;
  target_by_name.reserve(target_bone_names.size());
  for (std::size_t index{}; index != target_bone_names.size(); ++index)
    target_by_name.emplace(target_bone_names[index],
                           static_cast<std::uint32_t>(index));

  struct Track {
    std::string name;
    std::uint32_t source_index{};
    std::uint32_t target_index{};
    bool rotations{};
    bool positions{};
    std::vector<std::optional<keyframes::Quat>> source_rotations;
    std::vector<std::optional<std::array<double, 3>>> source_positions;
  };
  std::vector<Track> tracks;
  tracks.reserve(source.bone_names.size());
  for (std::size_t source_index{}; source_index != source.bone_names.size();
       ++source_index) {
    const std::string &name = source.bone_names[source_index];
    const auto target = target_by_name.find(name);
    if (target == target_by_name.end())
      continue;
    Track track;
    track.name = name;
    track.source_index = static_cast<std::uint32_t>(source_index);
    track.target_index = target->second;
    track.source_rotations.resize(source.frames.size());
    track.source_positions.resize(source.frames.size());
    for (std::size_t frame{}; frame != source.frames.size(); ++frame) {
      const auto *sample =
          detail::FindBone(source.frames[frame], track.source_index);
      if (sample == nullptr)
        continue;
      if (sample->has_rotation) {
        track.rotations = true;
        track.source_rotations[frame] =
            keyframes::Quat{sample->rotation[0], sample->rotation[1],
                            sample->rotation[2], sample->rotation[3]};
      }
      if (sample->has_position) {
        track.positions = true;
        track.source_positions[frame] = sample->position;
      }
    }
    if (track.rotations || track.positions)
      tracks.push_back(std::move(track));
  }

  std::uint32_t root_target = (std::numeric_limits<std::uint32_t>::max)();
  std::string root_name;
  for (const char *candidate : {"Bip001", "Root", "root"}) {
    const auto found = target_by_name.find(candidate);
    if (found != target_by_name.end()) {
      root_target = found->second;
      root_name = candidate;
      break;
    }
  }
  if (tracks.empty() && root_target < target_bone_names.size()) {
    Track track;
    track.name = root_name;
    track.target_index = root_target;
    track.rotations = true;
    track.source_rotations.resize(source.frames.size());
    tracks.push_back(std::move(track));
  }
  if (tracks.empty()) {
    error = "Blender motion has no bones matching this character";
    return false;
  }

  try {
    Json bones = Json::object();
    Json offsets = Json::object();
    for (const auto &track : tracks) {
      Json rotations = Json::array();
      Json positions = Json::array();
      const auto reference =
          detail::ReferenceRotation(reference_locals, track.target_index);
      keyframes::Quat previous;
      bool previous_valid = false;
      for (std::uint32_t frame{}; frame != frame_count; ++frame) {
        const double time =
            static_cast<double>(frame) / kMmdFramesPerSecond;
        const auto [lower_index, upper_index] =
            detail::Bounds(source.frames, time);
        const auto &lower = source.frames[lower_index];
        const auto &upper = source.frames[upper_index];
        const double amount = detail::BlendAmount(lower, upper, time);

        keyframes::Quat delta_lower;
        keyframes::Quat delta_upper;
        if (track.source_rotations[lower_index].has_value())
          delta_lower = *track.source_rotations[lower_index];
        if (track.source_rotations[upper_index].has_value())
          delta_upper = *track.source_rotations[upper_index];
        const auto delta = detail::Slerp(delta_lower, delta_upper, amount);
        auto absolute =
            detail::Normalize(detail::Multiply(delta, reference));
        if (previous_valid &&
            absolute.x * previous.x + absolute.y * previous.y +
                    absolute.z * previous.z + absolute.w * previous.w <
                0.0)
          absolute = {-absolute.x, -absolute.y, -absolute.z, -absolute.w};
        previous = absolute;
        previous_valid = true;
        rotations.push_back(
            {absolute.x, absolute.y, absolute.z, absolute.w});

        if (track.positions) {
          const std::array<double, 3> reference_position{
              reference_locals[track.target_index][4],
              reference_locals[track.target_index][5],
              reference_locals[track.target_index][6]};
          const std::array<double, 3> fallback =
              source.bone_positions_are_deltas
                  ? std::array<double, 3>{0.0, 0.0, 0.0}
                  : reference_position;
          const std::array<double, 3> from =
              track.source_positions[lower_index].value_or(fallback);
          const std::array<double, 3> to =
              track.source_positions[upper_index].value_or(fallback);
          const std::array<double, 3> blended{
              from[0] + (to[0] - from[0]) * amount,
              from[1] + (to[1] - from[1]) * amount,
              from[2] + (to[2] - from[2]) * amount,
          };
          positions.push_back(
              source.bone_positions_are_deltas
                  ? std::array<double, 3>{
                        reference_position[0] + blended[0],
                        reference_position[1] + blended[1],
                        reference_position[2] + blended[2]}
                  : blended);
        }
      }
      bones[track.name] = std::move(rotations);
      if (track.positions) {
        offsets[track.name] = std::move(positions);
        ++report.offset_tracks;
      }
      ++report.bone_tracks;
    }

    Json roots = Json::array();
    for (std::uint32_t frame{}; frame != frame_count; ++frame) {
      const double time =
          static_cast<double>(frame) / kMmdFramesPerSecond;
      const auto [lower_index, upper_index] =
          detail::Bounds(source.frames, time);
      const auto &lower = source.frames[lower_index];
      const auto &upper = source.frames[upper_index];
      const double amount = detail::BlendAmount(lower, upper, time);
      roots.push_back(
          {lower.root[0] + (upper.root[0] - lower.root[0]) * amount,
           lower.root[1] + (upper.root[1] - lower.root[1]) * amount,
           lower.root[2] + (upper.root[2] - lower.root[2]) * amount});
    }

    Json morphs = Json::object();
    std::set<std::string> face_names;
    for (const auto &frame : source.frames)
      for (const auto &[name, weight] : frame.faces) {
        (void)weight;
        face_names.insert(name);
      }
    for (const auto &name : face_names) {
      Json keys = Json::array();
      float previous = 0.0F;
      for (std::size_t frame{}; frame != source.frames.size(); ++frame) {
        const auto found = source.frames[frame].faces.find(name);
        const float value =
            found == source.frames[frame].faces.end() ? 0.0F : found->second;
        const auto target_frame = static_cast<std::uint32_t>(
            std::clamp<long long>(
                std::llround(source.frames[frame].time *
                             kMmdFramesPerSecond),
                0, static_cast<long long>(frame_count - 1)));
        if (frame == 0 && value == 0.0F)
          keys.push_back({0, 0.0F});
        if (frame == 0 || std::abs(value - previous) > 1e-6F) {
          if (keys.empty() || keys.back()[0] != target_frame)
            keys.push_back({target_frame, value});
          else
            keys.back()[1] = value;
        }
        previous = value;
      }
      if (keys.empty() || keys.front()[0] != 0)
        keys.insert(keys.begin(), {0, 0.0F});
      morphs[name] = std::move(keys);
      ++report.morph_tracks;
    }

    Json document;
    document["schemaVersion"] = 1;
    document["kind"] = "better-pose-motion";
    document["source"] = {
        {"blenderPath", source.model},
        {"format", std::string(kFormat)},
        {"version", kVersion},
    };
    document["targetMesh"] = source.model;
    document["rootBone"] = root_name;
    document["fps"] = kMmdFramesPerSecond;
    document["firstFrame"] = 0;
    document["frameCount"] = frame_count;
    // The Blender armature location is a centimetre-space delta from the
    // character's reference pose. The runtime adds it to that pose's root
    // translation instead of replacing the bind translation.
    document["rootTranslationUnit"] = "cm-delta";
    document["mmdLegLength"] = 0.0;
    document["bones"] = std::move(bones);
    document["rootTranslation"] = std::move(roots);
    if (!offsets.empty())
      document["boneOffsets"] = std::move(offsets);
    if (!morphs.empty())
      document["morphs"] = std::move(morphs);
    document["armIkTargets"] = Json::array();
    out = document.dump();

    report.frame_count = frame_count;
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = std::string("Blender motion conversion failed: ") + exception.what();
    return false;
  }
}

}  // namespace better_pose::blender_motion
