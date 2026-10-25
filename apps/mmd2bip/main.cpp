// Acceptance harness for the C++ MMD motion port.
//
// Conversion is developed without a game process, so this tool is the only way to run it:
// it reads the same four inputs the Python converter takes and writes a motion document,
// which is then compared against the Python output frame by frame. It is not shipped in a
// plugin package; it exists so the port can be falsified on the machine that produced the
// reference files.
//
//   anomaly_mmd2bip --pmx <pmx.json> --vmd <motion.vmd> --skeleton <pose.skeleton.json>
//                   --out <motion.json> [--max-frames N] [--fps 30]
//                   [--ik-mode follow|on|off] [--mapping source=target]...
//                   [--ignore source]...
//
// `--vmd-json` is accepted and ignored: the Python side needs a pre-parsed dump, this one
// parses the VMD itself, and taking the same command line keeps the two runs comparable.

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "motion_builder.hpp"

namespace {

bool ReadFile(const std::string &path, std::vector<std::uint8_t> &out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return true;
}

bool ReadTextFile(const std::string &path, std::string &out) {
  std::vector<std::uint8_t> bytes;
  if (!ReadFile(path, bytes))
    return false;
  out.assign(bytes.begin(), bytes.end());
  return true;
}

void Usage() {
  std::fprintf(stderr,
               "usage: anomaly_mmd2bip --pmx <pmx.json> --vmd <motion.vmd> "
               "--skeleton <pose.skeleton.json> --out <motion.json>\n"
               "                      [--vmd-json <ignored>] [--max-frames N] [--fps 30]\n"
               "                      [--ik-mode follow|on|off] "
               "[--mapping source=target]... [--parent source=target]... "
               "[--parent-position source=target]... "
               "[--parent-no-rotation source=target]... "
               "[--parent-anchor source=target=anchor]... "
               "[--control source=kind,side,finger,axis,gain[,paired]]... "
               "[--arm-ik source=side,gain,bend[,both][,follow]]... "
               "[--ignore source]...\n");
}

}  // namespace

int main(int argc, char **argv) {
  better_pose::mmd2bip::Input input;
  std::string output_path;
  const auto parent_relation =
      [&input](const std::string &value,
               const char *option) -> better_pose::mmd2bip::ManualParentMapping * {
    const std::size_t separator = value.find('=');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == value.size()) {
      std::fprintf(stderr, "invalid %s %s (expected source=target)\n",
                   option, value.c_str());
      return nullptr;
    }
    const std::string source = value.substr(0, separator);
    const std::string target = value.substr(separator + 1);
    const auto existing = std::find_if(
        input.manual_parent_mappings.begin(),
        input.manual_parent_mappings.end(),
        [&source, &target](const auto &mapping) {
          return mapping.source == source && mapping.target == target;
        });
    if (existing != input.manual_parent_mappings.end())
      return &*existing;
    input.manual_parent_mappings.push_back({source, target});
    return &input.manual_parent_mappings.back();
  };
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--help" || argument == "-h") {
      Usage();
      return 0;
    }
    if (!has_value) {
      std::fprintf(stderr, "missing value for %s\n", argument.c_str());
      return 2;
    }
    const std::string value = argv[++i];
    if (argument == "--pmx")
      input.pmx_path = value;
    else if (argument == "--vmd")
      input.vmd_path = value;
    else if (argument == "--skeleton")
      input.skeleton_path = value;
    else if (argument == "--out")
      output_path = value;
    else if (argument == "--vmd-json") {
      // The reference converter reads a pre-parsed dump; this one parses the VMD. The
      // flag is accepted so both runs take the same command line.
    } else if (argument == "--max-frames")
      input.max_frames = static_cast<std::uint32_t>(std::stoul(value));
    else if (argument == "--fps")
      input.fps = std::stod(value);
    else if (argument == "--ik")
      input.ik = std::stoi(value) != 0;
    else if (argument == "--ik-mode") {
      if (value == "follow")
        input.ik_mode = better_pose::mmd2bip::IkMode::FollowMotion;
      else if (value == "on")
        input.ik_mode = better_pose::mmd2bip::IkMode::ForceOn;
      else if (value == "off")
        input.ik_mode = better_pose::mmd2bip::IkMode::ForceOff;
      else {
        std::fprintf(stderr, "invalid --ik-mode %s (expected follow, on, or off)\n",
                     value.c_str());
        return 2;
      }
    } else if (argument == "--mapping") {
      const std::size_t separator = value.find('=');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 == value.size()) {
        std::fprintf(stderr, "invalid --mapping %s (expected source=target)\n",
                     value.c_str());
        return 2;
      }
      input.manual_mappings[value.substr(0, separator)] =
          value.substr(separator + 1);
    } else if (argument == "--parent") {
      if (parent_relation(value, "--parent") == nullptr)
        return 2;
    } else if (argument == "--parent-position") {
      auto *mapping = parent_relation(value, "--parent-position");
      if (mapping == nullptr)
        return 2;
      mapping->inherit_position = true;
    } else if (argument == "--parent-no-rotation") {
      auto *mapping = parent_relation(value, "--parent-no-rotation");
      if (mapping == nullptr)
        return 2;
      mapping->inherit_rotation = false;
    } else if (argument == "--parent-anchor") {
      const std::size_t anchor_separator = value.rfind('=');
      if (anchor_separator == std::string::npos ||
          anchor_separator == 0 || anchor_separator + 1 == value.size()) {
        std::fprintf(stderr,
                     "invalid --parent-anchor %s "
                     "(expected source=target=anchor)\n",
                     value.c_str());
        return 2;
      }
      auto *mapping = parent_relation(
          value.substr(0, anchor_separator), "--parent-anchor");
      if (mapping == nullptr)
        return 2;
      mapping->anchor = value.substr(anchor_separator + 1);
    } else if (argument == "--control") {
      const std::size_t source_separator = value.find('=');
      if (source_separator == std::string::npos || source_separator == 0)
        return 2;
      const std::string source = value.substr(0, source_separator);
      std::string spec = value.substr(source_separator + 1);
      std::vector<std::string> fields;
      std::size_t start = 0;
      for (;;) {
        const std::size_t comma = spec.find(',', start);
        fields.push_back(spec.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
      if (fields.size() < 5 || fields.size() > 6) {
        std::fprintf(stderr, "invalid --control %s\n", value.c_str());
        return 2;
      }
      better_pose::mmd2bip::ManualFingerControl control;
      control.source = source;
      if (fields[0] == "grip")
        control.kind = better_pose::mmd2bip::FingerControlKind::Grip;
      else if (fields[0] == "spread")
        control.kind = better_pose::mmd2bip::FingerControlKind::Spread;
      else
        return 2;
      const std::string &side = fields[1];
      if (side == "left") {
        control.side = 0;
      } else if (side == "right") {
        control.side = 1;
      } else if (side == "both") {
        control.paired = true;
      } else {
        return 2;
      }
      if (fields[2] == "all")
        control.finger = -1;
      else if (fields[2] == "thumb")
        control.finger = 0;
      else if (fields[2] == "index")
        control.finger = 1;
      else if (fields[2] == "middle")
        control.finger = 2;
      else if (fields[2] == "ring")
        control.finger = 3;
      else if (fields[2] == "pinky")
        control.finger = 4;
      else
        return 2;
      if (fields[3] == "x")
        control.input_axis = 0;
      else if (fields[3] == "y")
        control.input_axis = 1;
      else if (fields[3] == "z")
        control.input_axis = 2;
      else
        return 2;
      try {
        control.gain = std::stof(fields[4]);
      } catch (...) {
        return 2;
      }
      if (!std::isfinite(control.gain) ||
          std::fabs(control.gain) > 3.0F)
        return 2;
      if (fields.size() == 6) {
        if (fields[5] != "paired" && fields[5] != "both")
          return 2;
        control.paired = true;
      }
      input.manual_finger_controls.push_back(std::move(control));
    } else if (argument == "--arm-ik") {
      const std::size_t source_separator = value.find('=');
      if (source_separator == std::string::npos || source_separator == 0)
        return 2;
      const std::string source = value.substr(0, source_separator);
      std::string spec = value.substr(source_separator + 1);
      std::vector<std::string> fields;
      std::size_t start = 0;
      for (;;) {
        const std::size_t comma = spec.find(',', start);
        fields.push_back(spec.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
      if (fields.size() < 3)
        return 2;
      better_pose::mmd2bip::ManualArmIkControl control;
      control.source = source;
      if (fields[0] == "left") {
        control.side = 0;
      } else if (fields[0] == "right") {
        control.side = 1;
      } else if (fields[0] == "both") {
        control.paired = true;
      } else {
        return 2;
      }
      try {
        control.gain = std::stof(fields[1]);
        control.max_bend_degrees = std::stof(fields[2]);
      } catch (...) {
        return 2;
      }
      if (!std::isfinite(control.gain) ||
          !std::isfinite(control.max_bend_degrees))
        return 2;
      for (std::size_t field = 3; field < fields.size(); ++field) {
        if (fields[field] == "both")
          control.paired = true;
        else if (fields[field] == "follow")
          control.follow_wrist = true;
        else
          return 2;
      }
      input.manual_arm_ik_controls.push_back(std::move(control));
    } else if (argument == "--ignore") {
      if (value.empty()) {
        std::fprintf(stderr, "--ignore requires a non-empty source track\n");
        return 2;
      }
      input.ignored_tracks.insert(value);
    } else if (argument == "--hip-on-pelvis")
      input.hip_on_pelvis = std::stoi(value) != 0;
    else if (argument == "--feet-anchor")
      input.feet_anchor = std::stoi(value) != 0;
    else {
      std::fprintf(stderr, "unknown argument %s\n", argument.c_str());
      Usage();
      return 2;
    }
  }
  if (input.pmx_path.empty() || input.vmd_path.empty() || input.skeleton_path.empty() ||
      output_path.empty()) {
    Usage();
    return 2;
  }
  if (!ReadFile(input.vmd_path, input.vmd_bytes)) {
    std::fprintf(stderr, "cannot read %s\n", input.vmd_path.c_str());
    return 1;
  }
  if (!ReadTextFile(input.pmx_path, input.reference_pmx_json)) {
    std::fprintf(stderr, "cannot read %s\n", input.pmx_path.c_str());
    return 1;
  }
  if (!ReadTextFile(input.skeleton_path, input.skeleton_json)) {
    std::fprintf(stderr, "cannot read %s\n", input.skeleton_path.c_str());
    return 1;
  }

  const auto result = better_pose::mmd2bip::BuildMotion(input);
  if (!result.ok) {
    std::fprintf(stderr, "conversion failed: %s\n", result.error.c_str());
    return 1;
  }
  std::ofstream stream(output_path, std::ios::binary);
  if (!stream) {
    std::fprintf(stderr, "cannot write %s\n", output_path.c_str());
    return 1;
  }
  stream << result.motion_json;
  std::printf("%s", result.report.c_str());
  std::printf("wrote %s (%.2f MB)\n", output_path.c_str(),
              static_cast<double>(result.motion_json.size()) / 1e6);
  return 0;
}
