#pragma once

#include "../secondary_rules.hpp"

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

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace better_pose::mmd2bip {

enum class IkMode : std::uint8_t {
  FollowMotion,
  ForceOn,
  ForceOff,
};

// A source track inserted as an additional rotation parent before a target bone. Parent
// entries are applied in order: the last entry is the outermost parent. This is an
// explicit composition, not a second retarget hierarchy.
struct ManualParentMapping {
  std::string source;
  std::string target;
  // Optional target-side rest anchor. Empty means the target bone's existing parent.
  std::string anchor;
  bool inherit_rotation = true;
  bool inherit_position = false;
};

enum class FingerControlKind : std::uint8_t {
  Grip,
  Spread,
};

// A source track interpreted as a procedural hand control. `finger` is -1 for all fingers,
// otherwise 0=thumb, 1=index, 2=middle, 3=ring, 4=pinky. `side` is 0=left, 1=right;
// `paired` applies the same control to both hands.
struct ManualFingerControl {
  std::string source;
  FingerControlKind kind = FingerControlKind::Grip;
  int side = 0;
  int finger = -1;
  int input_axis = 0;
  float gain = 1.0F;
  bool paired = false;
};

// A source track interpreted as a wrist target offset for a target-side two-bone IK solve.
// The first version is position-driven; optional wrist rotation follows the track's rotation.
struct ManualArmIkControl {
  std::string source;
  int side = 0;
  bool paired = false;
  float gain = 1.0F;
  float max_bend_degrees = 150.0F;
  bool follow_wrist = false;
};

// Per-bone spring values captured from the character's native KawaiiPhysics
// component. Names refer to the target skeleton, so the converter can apply
// them directly while building that skeleton's secondary-bone tracks.
struct SecondaryBoneOverride {
  std::string name;
  secondary::SpringSettings spring;
  double collision_radius = 0.0;
  double motion_follow = 1.0;
  std::array<double, 3> native_gravity{};
  double forward_move_offset = 0.0;
};

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
  IkMode ik_mode = IkMode::FollowMotion;
  // Explicit VMD bone -> target Biped bone mappings. These are appended after the built-in
  // table and are used by the plugin's anomaly-track inspector for author-specific bones.
  std::map<std::string, std::string> manual_mappings;
  // Additional rotation parents, in outer-to-inner insertion order per target. A source
  // may target more than one bone; each source/target pair is stored once.
  std::vector<ManualParentMapping> manual_parent_mappings;
  // Author-specific control tracks interpreted as grip/spread input for the target hand.
  std::vector<ManualFingerControl> manual_finger_controls;
  std::vector<ManualArmIkControl> manual_arm_ik_controls;
  std::vector<SecondaryBoneOverride> secondary_bone_overrides;
  // Source tracks the user explicitly chose not to consume. This overrides both automatic and
  // manual mappings for the same source.
  std::set<std::string> ignored_tracks;
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
  // Runtime-independent tuning for secondary bones baked into the main character
  // skeleton. Modular accessory meshes use the same values in their live solver.
  secondary::DynamicsTuning dynamics_tuning;
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
  std::uint32_t distance_constraint_links = 0;
  std::uint32_t distance_constraint_corrections = 0;
  std::uint32_t self_collision_links = 0;
  std::uint32_t self_collision_corrections = 0;
  std::uint32_t self_collision_contacts = 0;
  std::array<std::uint32_t, secondary::kMotionMaterialCount>
      self_collision_candidates_by_material{};
  std::array<std::uint32_t, secondary::kMotionMaterialCount>
      self_collision_contacts_by_material{};
  std::string report;  // one line per conversion step, for the plugin log
};

// A cheap, skeleton-independent inspection of a source VMD. `recommended_reference`
// uses the plugin's reference modes: 0 = Standard Miku PMX, 1 = Unity Miku V4C,
// 2 = Endfield Standard MMD / A. `confidence` is the score margin between the best
// and second-best reference, not a probability.
struct VmdCompatibilityReport {
  enum class TrackKind : std::uint8_t {
    Humanoid,
    Control,
    Accessory,
    Other,
  };

  enum class MappingIssueReason : std::uint8_t {
    None,
    Ignored,
    MissingReference,
    MissingTarget,
    TargetOccupied,
    SourceMissing,
    Disconnected,
    Unmapped,
  };

  struct TrackMotionSignature {
    std::array<double, 3> rotation_span{};
    std::array<double, 3> position_span{};
    int dominant_axis = -1;
    double axis_dominance = 0.0;
    std::uint32_t direction_changes = 0;
  };

  struct UnmappedTrack {
    std::string name;
    std::uint32_t key_count = 0;
    std::uint32_t first_nonzero_frame = 0;
    bool animated = false;
    TrackKind kind = TrackKind::Other;
    MappingIssueReason reason = MappingIssueReason::Unmapped;
    TrackMotionSignature signature;
  };

  struct MappedTrack {
    std::string name;
    std::string target;
    std::uint32_t key_count = 0;
    std::uint32_t first_nonzero_frame = 0;
    bool animated = false;
    bool manual = false;
    TrackKind kind = TrackKind::Other;
    TrackMotionSignature signature;
  };

  struct MappingIssue {
    std::string source;
    std::string target;
    MappingIssueReason reason = MappingIssueReason::None;
  };

  bool ok = false;
  std::string error;
  int recommended_reference = 0;
  int confidence = 0;
  std::uint32_t track_count = 0;
  std::uint32_t bone_key_count = 0;
  std::uint32_t frame_count = 0;
  std::uint32_t visibility_keys = 0;
  bool complete_leg_fk = false;
  bool left_leg_fk = false;
  bool right_leg_fk = false;
  bool foot_ik = false;
  bool toe_ik = false;
  bool leg_d = false;
  bool waist_cancel = false;
  bool shoulder_cancel = false;
  bool animated_shoulder = false;
  bool twist_tracks = false;
  bool animated_twist = false;
  bool upper_body_1 = false;
  bool upper_body_2 = false;
  bool upper_body_3 = false;
  std::vector<std::string> reasons;
  std::vector<UnmappedTrack> unmapped_tracks;
  std::uint32_t unmapped_humanoid = 0;
  std::uint32_t unmapped_control = 0;
  std::uint32_t unmapped_accessory = 0;
  std::uint32_t unmapped_other = 0;
  // Populated only by AnalyzeVmdCompatibilityForReference. This is the exact source-track ->
  // target-bone audit produced by the same BuildMapping step used for conversion.
  bool reference_mapping_ready = false;
  int reference_mode = -1;
  std::string reference_mapping_error;
  std::uint32_t reference_mapped_tracks = 0;
  std::uint32_t reference_mapped_targets = 0;
  std::vector<MappedTrack> mapped_reference_tracks;
  std::vector<UnmappedTrack> reference_unmapped_tracks;
  std::vector<UnmappedTrack> ignored_reference_tracks;
  std::vector<MappingIssue> mapping_issues;
  std::uint32_t reference_unmapped_humanoid = 0;
  std::uint32_t reference_unmapped_control = 0;
  std::uint32_t reference_unmapped_accessory = 0;
  std::uint32_t reference_unmapped_other = 0;
};

// Parses `input.vmd_bytes` and produces a `better-pose-motion` document.
Result BuildMotion(const Input &input);

// Reads only track names and whether their keys contain rotation/translation. It does not
// need a target character or reference model, so the plugin can show the recommendation
// immediately after the user picks a VMD.
VmdCompatibilityReport AnalyzeVmdCompatibility(
    const std::vector<std::uint8_t> &vmd_bytes);

// Runs the generic inspection above and then the converter's actual source/target mapping
// against one selected reference and the current character skeleton. Unresolved tracks are
// the subset that BuildMapping does not consume; mapped tracks carry their target bone name.
VmdCompatibilityReport AnalyzeVmdCompatibilityForReference(
    const std::vector<std::uint8_t> &vmd_bytes,
    const std::string &reference_pmx_json,
    const std::string &skeleton_json,
    const std::map<std::string, std::string> &manual_mappings,
    const std::set<std::string> &ignored_tracks,
    int reference_mode,
    const std::vector<ManualParentMapping> &manual_parent_mappings = {},
    const std::vector<ManualFingerControl> &manual_finger_controls = {},
    const std::vector<ManualArmIkControl> &manual_arm_ik_controls = {});

// ---- individual units, exposed for the acceptance harness -------------------------
//
// The harness compares this port against the Python converter frame by frame, so each
// unit has to be reachable on its own.

// One VMD Bezier channel. MMD stores the two handles as byte pairs and uses the following
// key's curve for each interval.
struct VmdCurve {
  double x1 = 0.0;
  double y1 = 0.0;
  double x2 = 1.0;
  double y2 = 1.0;
};

// Resolves x -> y on a VMD Bezier curve. Endpoints are exact; the interior uses the
// same cubic timing shape as MMD rather than a linear keyframe blend.
double SampleVmdCurve(const VmdCurve &curve, double x);

struct VmdBoneKey {
  std::uint32_t frame = 0;
  double rotation[4] = {0.0, 0.0, 0.0, 1.0};  // x, y, z, w
  double position[3] = {0.0, 0.0, 0.0};
  std::array<VmdCurve, 4> curves;  // position X/Y/Z, rotation
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

// MMD's model visibility flag is stored in the same optional tail section as IK switches,
// but applies to the whole model. Keys are step changes, not interpolated.
struct VmdVisibilityKey {
  std::uint32_t frame = 0;
  bool visible = true;
};

// One camera keyframe. The retarget itself ignores the camera track, but a plugin can drive the
// game camera from it: as in MMD, `position` is the camera's look-at target, `rotation` is Euler
// in radians, and the camera itself sits `distance` back along its view axis.
struct VmdCameraKey {
  std::uint32_t frame = 0;
  double distance = 0.0;
  double position[3] = {0.0, 0.0, 0.0};
  double rotation[3] = {0.0, 0.0, 0.0};
  std::array<VmdCurve, 6> curves;  // target X/Y/Z, rotation, distance, FOV
  std::uint32_t view_angle = 0;
  bool perspective = false;
};

struct VmdCameraPose {
  bool valid = false;
  double position[3] = {0.0, 0.0, 0.0};
  double rotation[3] = {0.0, 0.0, 0.0};
  double distance = 0.0;
  double view_angle = 0.0;
  bool perspective = false;
};

// One morph (facial expression) keyframe. MMD interpolates morphs linearly.
struct VmdMorphKey {
  std::uint32_t frame = 0;
  float weight = 0.0F;
};

struct VmdMorphTrack {
  std::string name;  // UTF-8 (the file stores Shift-JIS)
  std::vector<VmdMorphKey> keys;  // ascending frame order
};

struct VmdDocument {
  bool ok = false;
  std::string error;
  int version = 0;
  std::string model_name;
  std::vector<std::uint32_t> bone_frame_range;  // empty when there are no keys
  std::vector<VmdTrack> tracks;
  std::vector<VmdMorphTrack> morphs;  // file order of first appearance
  std::vector<VmdIkState> ik_states;  // file order; one entry per record
  std::vector<VmdVisibilityKey> visibility;  // ascending frame order
  std::vector<VmdCameraKey> camera;              // ascending frame order
  std::vector<std::uint32_t> camera_frame_range;  // empty when there are no camera keys
  std::uint64_t bytes_consumed = 0;
  std::uint64_t file_size = 0;
  bool walk_exact = false;  // every section was walked to the end of the file
};

VmdDocument ParseVmd(const std::vector<std::uint8_t> &bytes);

// Samples a sparse track at an arbitrary frame: hold at the ends, then apply the following
// key's Bezier timing to each position channel and a slerp to the rotation.
// `out_rotation`/`out_position` are only written when the track has keys at all.
bool SampleTrack(const VmdTrack &track, double frame, double out_rotation[4],
                 double out_position[3]);

// Samples a camera track, applying its six authored Bezier channels. `keys` must be in
// ascending frame order.
VmdCameraPose SampleCameraTrack(const std::vector<VmdCameraKey> &keys, double frame);

}  // namespace better_pose::mmd2bip
