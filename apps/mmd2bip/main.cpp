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
//
// `--vmd-json` is accepted and ignored: the Python side needs a pre-parsed dump, this one
// parses the VMD itself, and taking the same command line keeps the two runs comparable.

#include <cstdint>
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
               "                      [--vmd-json <ignored>] [--max-frames N] [--fps 30]\n");
}

}  // namespace

int main(int argc, char **argv) {
  better_pose::mmd2bip::Input input;
  std::string output_path;
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
    else if (argument == "--hip-on-pelvis")
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
