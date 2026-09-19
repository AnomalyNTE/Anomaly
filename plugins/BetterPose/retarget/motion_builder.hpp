#pragma once

// MMD (VMD) -> Bip001 motion conversion, the C++ half of `tools/mmd2bip`.
//
// The Python converter in `tools/mmd2bip/mmd2bip.py` is the reference implementation and
// the only one that has been accepted in game. This port is meant to be behaviourally
// identical to it, so the rules here are deliberately literal -- every guard, threshold
// and rounding step cites the Python line it comes from, and none of them should be
// "improved" without re-measuring against a Python build on the same inputs.
//
// The port covers what a motion needs to load, play *and* look right: VMD parsing, the
// reference MMD skeleton (including its IK chains), the game's own skeleton export, the
// sparse keyframe sampler, the source model's own IK solve, the bone-name mapping, the
// output rotation math, finger chain alignment, direction correction, the hand rest
// choice, the accessory spring, the twist helpers, a track for every bone and the
// document header. Every rule here cites the Python line it comes from and none should be
// "improved" without re-measuring against the Python implementation on the same inputs.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace better_pose::mmd2bip {

// Everything the converter needs, all of it already in memory: the plugin holds the
// skeleton export as a document, and the VMD is read off disk by the caller.
struct Input {
  std::string vmd_path;        // recorded in `source.vmd`
  std::string pmx_path;        // recorded in `source.pmx`
  std::string skeleton_path;   // recorded in `source.skeleton`
  std::string skeleton_json;   // BetterPose Export Skeleton document
  std::string reference_pmx_json;  // reference MMD bone table (name/index/parent/position)
  std::vector<std::uint8_t> vmd_bytes;
  double fps = 30.0;
  std::uint32_t max_frames = 0;  // 0 = all frames
  // Solve the reference model's own IK chains before mapping (mmd2bip.py `--ik`). A motion
  // can carry nothing but IK-bone positions for the legs -- `主角.vmd` drives 左足ＩＫ at
  // thirty frames and leaves every leg bone at identity -- and without the solver those
  // legs stay in the rest pose. Off reproduces the pre-IK converter exactly.
  bool ik = true;
  // Carry the source's hip rotation (下半身) on the target's pelvis instead of folding it into
  // the thighs, and cancel it on the spine (in MMD the spine hangs off 腰, not 下半身). This is
  // what makes the hip *mesh* tilt like the original when a motion pushes the hips back;
  // folding it into the thighs makes the legs rotate while the hips stay square. The switch
  // exists because it also relocates where the hip rotation is applied, which can only be
  // judged in game side by side.
  bool hip_on_pelvis = true;
  // Anchor the character's placement to the *feet* (the mean of the two ankles' world
  // positions) instead of the pelvis. MMD pins the feet through the leg IK and moves the body
  // over them, which is why the feet read as the fixed point; anchoring to the pelvis moves
  // the whole body -- feet included -- instead. This does not make the result more faithful
  // (the bones already match the source within ~1 cm), it changes how the motion *reads*: the
  // feet stay put and the body swings over them. Off by default: it is a look, not a fix.
  bool feet_anchor = false;
};

struct Result {
  bool ok = false;
  std::string motion_json;  // the document to hand to LoadMotionDocument
  std::string error;        // human readable; empty when ok
  // Diagnostics, mirrored into the document's `diagnostics` object.
  std::uint32_t mapped_bones = 0;
  std::uint32_t frame_count = 0;
  std::uint32_t first_frame = 0;
  std::uint32_t native_fps = 30;
  std::uint32_t bone_tracks = 0;
  std::string report;  // one line per conversion step, for the plugin log
};

// Parses `input.vmd_bytes` and produces a `better-pose-motion` document.
Result BuildMotion(const Input &input);

// ---- individual units, exposed for the acceptance harness -------------------------
//
// The harness compares this port against the Python converter frame by frame, so each
// unit has to be reachable on its own.

struct VmdBoneKey {
  std::uint32_t frame = 0;
  double rotation[4] = {0.0, 0.0, 0.0, 1.0};  // x, y, z, w
  double position[3] = {0.0, 0.0, 0.0};
};

struct VmdTrack {
  std::string name;
  std::vector<VmdBoneKey> keys;  // ascending frame order
};

// One IK on/off record from the VMD's visibility/IK section. MMD's IK is on unless a
// record turns it off, so a motion that never disables anything carries a single `on`
// record at frame 0 (or none at all).
struct VmdIkState {
  std::string name;
  std::uint32_t frame = 0;
  bool on = true;
};

// One camera keyframe. The retarget itself ignores the camera track, but a plugin can drive the
// game camera from it: as in MMD, `position` is the camera's look-at target, `rotation` is Euler
// in radians, and the camera itself sits `distance` back along its view axis.
struct VmdCameraKey {
  std::uint32_t frame = 0;
  double distance = 0.0;
  double position[3] = {0.0, 0.0, 0.0};
  double rotation[3] = {0.0, 0.0, 0.0};
  std::uint8_t interpolation[24] = {};
  std::uint32_t view_angle = 0;
  bool perspective = false;
};

struct VmdDocument {
  bool ok = false;
  std::string error;
  int version = 0;
  std::string model_name;
  std::vector<std::uint32_t> bone_frame_range;  // empty when there are no keys
  std::vector<VmdTrack> tracks;
  std::vector<VmdIkState> ik_states;  // file order; one entry per record
  std::vector<VmdCameraKey> camera;              // ascending frame order
  std::vector<std::uint32_t> camera_frame_range;  // empty when there are no camera keys
  std::uint64_t bytes_consumed = 0;
  std::uint64_t file_size = 0;
  bool walk_exact = false;  // every section was walked to the end of the file
};

VmdDocument ParseVmd(const std::vector<std::uint8_t> &bytes);

// Samples a sparse track at an arbitrary frame: hold at the ends, slerp between keys.
// `out_rotation`/`out_position` are only written when the track has keys at all.
bool SampleTrack(const VmdTrack &track, double frame, double out_rotation[4],
                 double out_position[3]);

}  // namespace better_pose::mmd2bip
