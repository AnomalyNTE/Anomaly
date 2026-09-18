#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>   // MCI: music playback through the system codecs (see MusicPlayer)
#include <shobjidl.h>
#include <combaseapi.h>
#include <objbase.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
using Microsoft::WRL::ComPtr;
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ue5.h"
#include "anomaly/sdk/services/ui.h"
#include <nlohmann/json.hpp>
#include "better_pose_profile.hpp"
#include "accessory_dynamics.hpp"
#include "retarget/motion_builder.hpp"
#include "../common/localization.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace better_pose_profile;

constexpr std::string_view kPoseSettingsSchemaId =
    "anomaly.builtin.character-pose.settings";
constexpr std::uint32_t kPoseSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumPoseSettingsBytes = 1U << 20;
// UObject::ClassPrivate. Only used by the read-only mesh scan, where a wrong
// value cannot corrupt anything: it just matches nothing and the scan reports
// zero candidates.
constexpr std::uint32_t kObjectClassOffset = 0x10;
// UObject::NamePrivate (an FName: comparison index + number).
constexpr std::uint32_t kObjectNameOffset = 0x18;
// UObject::OuterPrivate. A component's outer is its owning actor, which is how
// the scan narrows the whole world down to the local player's own components.
constexpr std::uint32_t kObjectOuterOffset = 0x20;
// USceneComponent::AttachParent. This is read-only: it identifies the scene
// component an accessory is attached to; it is not a leader/master-pose
// binding and must never be written by this plugin.
constexpr std::uint32_t kMeshAttachParentOffset = 0xD8;
// Extra components are only bone-driven when their bones can be matched to the
// body skeleton this closely (mean position error, cm). Measured on a real
// character: name mapping 6/222 and positional error ~9.9 cm even while both
// meshes were in sync, i.e. those components do not share the body's component
// space at all -- so the threshold is set where only a genuine match passes and
// everything else is refused instead of scribbled on.
constexpr double kExtraMeshMatchCm = 2.0;
// Bind-pose counterpart: two bones that correspond sit within a few millimetres of
// each other when both skeletons are in bind pose, so anything beyond a centimetre
// is not a correspondence.
constexpr double kExtraMeshBindCm = 1.0;
// Motion documents carry every sampled frame, so they are far larger than a
// pose settings file (the 79 s test track is ~2.3 MB).
constexpr std::size_t kMaximumMotionBytes = 256U << 20;
constexpr std::string_view kPoseExportPath = "pose-export.json";
constexpr std::string_view kPoseProfileDirectory = "character-pose/profiles/";
constexpr std::string_view kPoseSettingsSchema = R"json(
{
  "type": "object",
  "additionalProperties": false,
  "required": ["bones"],
  "properties": {
    "rootOffset": {
      "type": "array",
      "minItems": 3,
      "maxItems": 3,
      "items": {"type": "number"}
    },
    "bones": {
      "type": "array",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["index", "pitch", "yaw", "roll"],
        "properties": {
          "index": {"type": "integer", "minimum": 0},
          "pitch": {"type": "number", "minimum": -180.0, "maximum": 180.0},
          "yaw": {"type": "number", "minimum": -180.0, "maximum": 180.0},
          "roll": {"type": "number", "minimum": -180.0, "maximum": 180.0}
        }
      }
    }
  }
}
)json";

struct RuntimeState {
  std::uintptr_t g_world_address{};
  std::uintptr_t character{};
  std::uintptr_t mesh{};
  std::uintptr_t anim_instance{};
  std::uint32_t animation_mode{};
  std::uint8_t animation_flags{};
  std::uint32_t bone_space_count{};
  std::uintptr_t bone_space_data{};
  std::uint32_t component_space_count{};
  std::uintptr_t component_space_data{};
  std::uint32_t local_space_count{};
  std::uintptr_t local_space_data{};
  float rate_scale{1.0F};
  float root_motion_scale{1.0F};

  bool saved_rate{};
  float original_rate_scale{1.0F};
  bool saved_root_motion{};
  float original_root_motion_scale{1.0F};
  bool saved_pause{};
  std::uint8_t original_animation_flags{};
  bool saved_forced_lod{};
  bool forced_lod_applied{};
  std::int32_t original_forced_lod{};
  bool saved_animation_mode{};
  bool animation_mode_applied{};
  std::uint8_t original_animation_mode{};
  bool saved_multi_threaded_update{};
  std::uint8_t original_multi_threaded_update_flags{};
  std::uintptr_t multi_threaded_update_instance{};

  bool saved_pose{};
  std::uint32_t pose_bone_index{};
  std::array<double, 3> original_translation{};
  std::array<double, 3> original_component_translation{};
  std::uintptr_t pose_mesh{};
  std::uintptr_t pose_data{};
  std::uint32_t pose_count{};
  std::uintptr_t pose_component_data{};
  std::uint32_t pose_component_count{};
  std::vector<std::uint32_t> pose_descendants{};
};

struct ObjectRegistry {
  std::uintptr_t items{};
  std::uint32_t count{};
  std::uint32_t max_count{};
  std::uint32_t max_chunks{};
  std::uint32_t num_chunks{};
};

struct RenderSnapshot {
  bool active{};
  std::uintptr_t character{};
  std::uintptr_t mesh{};
  std::uintptr_t anim_instance{};
  std::uint32_t animation_mode{};
  std::uint32_t bone_space_count{};
  std::uintptr_t bone_space_data{};
  std::uint32_t component_space_count{};
  std::uintptr_t component_space_data{};
  float rate_scale{1.0F};
  float root_motion_scale{1.0F};
  bool pose_available{};
  std::vector<std::string> bone_names;
  std::array<char, 256> status{};
  std::array<char, 256> reflection_status{};
  std::array<char, 256> pose_status{};
  std::uint32_t pose_bone_index{};
  std::array<double, 3> component_translation_readback{};
  std::array<double, 3> bone_translation_readback{};
};

// The camera track of a VMD (MMD camera keys), sampled on demand. A VMD's camera section lives
// beside the bone tracks but is independent of them: a dance motion and its camera are usually two
// files, so this is loaded separately and played against the motion's own frame.
//
// MMD's camera semantics: `position` is the look-at target, `rotation` is Euler in radians, the
// camera itself sits `distance` back along its view axis, and `view_angle` is the FOV in degrees.
struct CameraKey {
  double frame = 0.0;
  double distance = 0.0;
  double position[3] = {0.0, 0.0, 0.0};
  double rotation[3] = {0.0, 0.0, 0.0};
  double view_angle = 0.0;
};

struct CameraTrack {
  std::string file;
  std::vector<CameraKey> keys;   // ascending frame order
  double first_frame = 0.0;
  double last_frame = 0.0;

  bool Load(const std::vector<std::uint8_t> &bytes, const std::string &name,
            std::string *error) {
    keys.clear();
    file.clear();
    const better_pose::mmd2bip::VmdDocument document = better_pose::mmd2bip::ParseVmd(bytes);
    if (!document.ok) {
      if (error != nullptr) {
        // The size tells a missing optional tail apart from a broken mandatory section.
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), " (%zu bytes)", document.file_size);
        *error = document.error + detail;
      }
      return false;
    }
    if (document.camera.empty()) {
      if (error != nullptr)
        *error = "this VMD has no camera keys (bone-only file)";
      return false;
    }
    keys.reserve(document.camera.size());
    for (const better_pose::mmd2bip::VmdCameraKey &source : document.camera) {
      CameraKey key;
      key.frame = static_cast<double>(source.frame);
      key.distance = source.distance;
      key.view_angle = static_cast<double>(source.view_angle);
      for (int axis = 0; axis < 3; ++axis) {
        key.position[axis] = source.position[axis];
        key.rotation[axis] = source.rotation[axis];
      }
      keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(),
              [](const CameraKey &a, const CameraKey &b) { return a.frame < b.frame; });
    first_frame = keys.front().frame;
    last_frame = keys.back().frame;
    file = name;
    return true;
  }

  // Linear between keys, held at the ends. MMD's per-key Bezier easing is not applied yet: for a
  // file keyed every frame (the usual export) it makes no difference.
  bool Sample(const double frame, double out_position[3], double out_rotation[3],
              double *out_distance, double *out_fov) const {
    if (keys.empty())
      return false;
    if (frame <= keys.front().frame) {
      Copy(keys.front(), out_position, out_rotation, out_distance, out_fov);
      return true;
    }
    if (frame >= keys.back().frame) {
      Copy(keys.back(), out_position, out_rotation, out_distance, out_fov);
      return true;
    }
    std::size_t low = 0;
    std::size_t high = keys.size() - 1;
    while (high - low > 1) {
      const std::size_t middle = (low + high) / 2;
      if (keys[middle].frame <= frame)
        low = middle;
      else
        high = middle;
    }
    const CameraKey &a = keys[low];
    const CameraKey &b = keys[high];
    const double span = b.frame - a.frame;
    const double blend = span > 1e-9 ? (frame - a.frame) / span : 0.0;
    for (int axis = 0; axis < 3; ++axis) {
      out_position[axis] = a.position[axis] + (b.position[axis] - a.position[axis]) * blend;
      out_rotation[axis] = a.rotation[axis] + (b.rotation[axis] - a.rotation[axis]) * blend;
    }
    *out_distance = a.distance + (b.distance - a.distance) * blend;
    *out_fov = a.view_angle + (b.view_angle - a.view_angle) * blend;
    return true;
  }

 private:
  static void Copy(const CameraKey &key, double out_position[3], double out_rotation[3],
                   double *out_distance, double *out_fov) {
    for (int axis = 0; axis < 3; ++axis) {
      out_position[axis] = key.position[axis];
      out_rotation[axis] = key.rotation[axis];
    }
    *out_distance = key.distance;
    *out_fov = key.view_angle;
  }
};

struct Context final {
  const AnomalyHostApiV1 *host{};
  anomaly::plugins::Localizer localizer;
  const AnomalyCoreServiceV1 *core{};
  const AnomalySignatureServiceV1 *signature{};
  const AnomalyUe5NamesServiceV1 *names{};
  const AnomalyUe5ObjectsServiceV1 *objects{};
  const AnomalyUiServiceV1 *ui{};
  const AnomalyHookServiceV1 *hook{};
  const AnomalyConfigServiceV1 *config{};
  const AnomalyStorageServiceV1 *storage{};
  const AnomalySchedulerServiceV1 *scheduler{};

  std::mutex state_mutex;
  std::mutex pose_angles_mutex;
  RenderSnapshot snapshot;
  RuntimeState runtime;
  AnomalyGenerationHandleV1 settings_schema{};
  std::vector<std::array<double, 3>> bone_angles;
  std::vector<std::array<double, 12>> pose_base_locals;
  std::uintptr_t pose_base_mesh{};
  bool pose_base_ready{};
  std::atomic_bool pose_settings_dirty{};
  std::uintptr_t g_objects_address{};
  ObjectRegistry object_registry{};
  std::string reflection_status{"reflection idle"};
  std::array<char, 256> pose_status{};
  std::vector<std::string> bone_names;
  std::vector<std::int32_t> bone_parents;
  std::uintptr_t bone_parents_mesh{};
  std::uint32_t bone_parents_count{};
  bool bone_parents_ready{};
  std::uintptr_t bone_names_mesh{};
  std::uint32_t bone_names_count{};
  bool bone_names_attempted{};
  std::array<char, 128> bone_filter{};

  std::atomic_bool freeze_enabled{};
  std::atomic_bool rate_override_enabled{};
  std::atomic_bool root_motion_override_enabled{};
  std::atomic_bool pose_override_enabled{};
  AnomalyGenerationHandleV1 tick_hook{};
  std::uintptr_t tick_original{};
  std::uintptr_t tick_target{};
  std::atomic<std::uint32_t> reflection_action_requested{0};
  std::atomic<float> requested_rate_scale{1.0F};
  std::atomic<float> requested_root_motion_scale{1.0F};
  std::atomic<std::uint32_t> requested_bone_index{0};
  std::array<std::atomic<double>, 3> requested_translation{};
  std::array<std::atomic<double>, 3> requested_root_offset{};
  std::array<std::atomic<double>, 3> edited_translation{};
  std::atomic_bool pose_reset_requested{};
  std::atomic<std::uint32_t> pose_file_action_requested{0};
  std::string active_character_id;
  bool character_profiles_initialized{};
  std::array<char, 128> pose_export_name{};
  std::string pose_export_folder;
  std::string pose_import_file;

  // MMD motion tracks loaded from the offline converter's JSON. Rotation-only:
  // every driven bone gets an absolute local rotation, everything else keeps the
  // captured base pose.
  struct MotionTrack {
    std::vector<std::string> bone_names;
    std::vector<std::uint32_t> bone_indices;
    bool indices_ready{};
    std::uint32_t bone_count{};
    std::uint32_t frame_count{};
    std::uint32_t first_frame{};
    double fps{30.0};
    bool has_root{};
    std::uint32_t root_bone_index{};
    std::string root_bone_name;
    std::vector<float> rotations;   // frame_count * bone_count * 4
    std::vector<float> roots;       // frame_count * 3
    // Per-bone *local translations* (cm), applied on top of the engine reference pose. The
    // converter emits these for bones whose parent carries a rotation they must not inherit
    // (the spine under a pelvis that also carries the hip rotation): the rotation alone cannot
    // move a bone's origin, so the base offset is counter-rotated per frame instead.
    std::vector<std::string> offset_names;
    std::vector<std::uint32_t> offset_indices;
    bool offsets_ready{};
    std::vector<float> offsets;     // frame_count * offset_count * 3
    // Root translation units. "mmd" (current converter) means the values are
    // offsets in MMD units relative to the first frame, so the runtime scales
    // them by *this* character's leg length; anything else is the old absolute
    // centimetre form that carried the exporting character's height.
    bool roots_in_mmd_units{};
    double mmd_leg_length{};
    std::string mesh_id;
    std::string path;
  };
  std::mutex motion_mutex;
  MotionTrack motion;
  std::atomic_bool motion_loaded{};
  std::atomic_bool motion_playing{};
  std::atomic_bool motion_loop{true};
  std::atomic_bool motion_apply_root{true};
  // Play the motion in place: drop the root translation's two horizontal components and keep the
  // height (mmd y -> the target's z, see the root application below), so a travelling motion can
  // be judged where the character stands without losing its bob and jump.
  std::atomic_bool motion_lock_planar{};
  // Which reference bone table interprets a VMD. Off = the bundled 初音ミク PMD (the model the
  // user plays in MMD, so the game reproduces what they see there); on = the Unity MMD-for-Unity
  // plugin's own reference (Kinsama式初音ミクV4C). A motion authored against one rig reads
  // differently on the other -- resting arms differ by ~35 deg and the hips' travel around the
  // waist by up to 21 cm -- so this is a per-motion choice, not a global "better" one.
  std::atomic_bool motion_reference_unity{};
  // Camera VMD (a separate file from the motion): loaded on the worker, sampled on the render
  // thread, so the track itself is guarded and only the flags are atomic.
  std::mutex camera_mutex;
  CameraTrack camera;
  std::atomic_bool camera_loaded{};
  std::atomic_bool camera_enabled{};
  std::string camera_file;
  // Follow mode: ignore whatever camera file is loaded and simply keep the character's own
  // displacement in the middle of the frame, from a world direction anchored on the first driven
  // frame. Distance is how far back along that direction the camera sits, height how far above the
  // character's feet; the aim is the character's own chest. Both are centimetres.
  std::atomic_bool camera_follow{};
  std::atomic<double> camera_follow_distance_cm{380.0};
  std::atomic<double> camera_follow_height_cm{150.0};
  // Whether the follow camera rides the character's up/down. On is the loose, hand-held look; off
  // pins the height to the ground level the shot was anchored at, so a jump reads as the character
  // moving in the frame. The horizontal follow is unaffected either way.
  std::atomic_bool camera_follow_vertical{true};
  std::atomic<double> camera_anchor_ground{};
  std::atomic<double> camera_frame{-1.0};
  // Camera hook and character anchor. The hook rewrites the POV at view-build time (see
  // CameraPovDetour); the manager is cached to notice a camera swap, and the shot's angle
  // relative to the character is captured on the first driven frame.
  AnomalyGenerationHandleV1 camera_pov_hook{};
  std::uintptr_t camera_pov_original{};
  std::uintptr_t camera_pov_target{};
  std::uintptr_t camera_pov_resolved_target{};
  // The accessor's own struct (manager + its `lea` displacement). Only the lens is written there
  // now: the camera itself travels through the getter's out-parameters.
  std::uintptr_t camera_pov_struct{};
  std::atomic<std::uintptr_t> camera_manager{};
  std::atomic_bool camera_manager_resolved{};
  std::atomic_bool camera_hook_ready{};
  std::atomic_bool camera_anchored{};
  std::atomic<double> camera_logged_second{-1.0};
  // A free-running clock for the drive log's once-a-second throttle. The motion's own frame is the
  // wrong key for it: follow mode has no timeline, so its frame stays 0 and the log printed one
  // line and then nothing, which reads exactly like "the plugin stopped driving".
  std::atomic<double> camera_log_clock{};
  // Frames spent waiting for a plausible camera reading to anchor the shot on. After a couple of
  // seconds the first reading is taken whatever it is, so a placeholder can never leave the plugin
  // writing nothing at all.
  int camera_anchor_wait{};
  // True once the shot has been started for the current switch-on: dropped when driving is switched
  // off, kept across pauses so a resume does not re-anchor the shot.
  bool camera_was_enabled{};
  std::atomic<double> camera_yaw{};   // captured once: local +Y (the model's facing) -> world
  // Centimetres per MMD unit for the live character, published by the motion path and sized by the
  // camera's own fallback until then; it scales the camera track's travel.
  std::atomic<double> mmd_unit_cm{};
  // Two root references, both in this rig's local axes and centimetres: what the pose path actually
  // applied (honouring "lock planar motion" and the root-motion toggle), and where the file's own
  // world puts the model (the motion's root translation). A camera file is authored in a world where
  // the model walks; the difference between the two is how far the camera must not walk.
  std::array<std::atomic<double>, 3> motion_applied_offset{};
  std::array<std::atomic<double>, 3> motion_authored_offset{};
  std::atomic_bool motion_baseline_logged{};
  std::atomic<double> motion_seconds{0.0};
  std::atomic<double> motion_seek{-1.0};
  // Bumped every time a seek is applied. The music follower watches it so that moving the progress
  // bar re-syncs the track at once, however small the jump.
  std::atomic<std::uint32_t> motion_seek_serial{};
  std::atomic<double> motion_display_seconds{0.0};
  // True from the moment the cursor consumes a seek until the track has actually been told where
  // to play. While it is set the animation must listen to the bar only: pulling it towards the
  // audio would drag it back to the position the audio is still on.
  std::atomic_bool motion_seek_pending{};
  // A seek that has to cut over at once instead of waiting for the drag to settle: a loop wrap, where
  // the alternative is 0.2 s of the pre-wrap song playing over the wrapped animation.
  std::atomic_bool motion_seek_immediate{};
  std::string motion_file;
  std::array<char, 96> motion_status{};

  // Read-only scan: how many skeletal mesh components share this skeleton?
  // Motivated by a character whose outer hair layer stays frozen while the body
  // dances -- if that layer lives in a second component, the answer tells us so
  // without touching any existing write path.
  std::atomic_bool mesh_scan_requested{};
  // Read-only attach discovery: find the offset at which a component stores a
  // weak pointer to its attach parent. Needed because the frozen hair accessory
  // is almost certainly socket-attached, and the profile has no layout for
  // USceneComponent to read that from.
  std::atomic_bool attach_scan_requested{};
  std::vector<std::pair<std::uint32_t, std::string>> attach_offsets;
  // Found by the read-only attach scan (StepAttachScan) and validated by
  // resolving through GObjects to an HTSkeletalMeshComponent: the offset at which
  // a component stores its attach parent.
  std::uint32_t attach_parent_offset{};
  // Every candidate component (accepted for bone driving or not): the attach
  // resync applies to all of them, since it does not need a bone map at all.
  std::vector<std::uintptr_t> extra_targets;
  std::uint32_t extra_resync_count{};
  // Engine reference (bind) pose, read out of the character's mesh asset. A live
  // capture is whatever pose the character happened to be in, and every bone roll
  // downstream inherits that posture; the reference pose is the one the geometry is
  // actually skinned in, so it is the only pose-independent source for those rolls.
  std::vector<std::array<double, 12>> ref_locals;
  bool ref_pose_attempted{};
  std::uintptr_t ref_pose_character{};
  std::uintptr_t ref_pose_object{};
  std::string ref_pose_status;
  bool extra_build_pending{};
  std::uint32_t extra_drop_count{};
  std::uint32_t extra_resync_log_tick{};
  std::uint32_t extra_resync_last{};
  bool mesh_scan_running{};
  std::uint32_t mesh_scan_cursor{};
  std::uintptr_t mesh_scan_class{};
  std::uint32_t mesh_scan_reference_bone{};
  std::array<double, 3> mesh_scan_reference{};
  std::uint32_t mesh_scan_same_class{};
  std::uint32_t mesh_scan_mesh_objects{};
  std::uint32_t mesh_scan_mesh_assets{};
  std::uint32_t mesh_scan_world_logged{};
  std::uint32_t mesh_scan_skinned{};
  // Every object whose class says "SkeletalMesh" gets one log line (see StepMeshScan); the bound
  // keeps a huge registry from flooding the log. Read-only.
  std::uint32_t mesh_scan_all_logged{};
  // Likewise for the UFunction objects of the budgeted mesh classes (see StepMeshScan).
  std::uint32_t mesh_scan_functions_logged{};
  std::vector<std::uint32_t> mesh_scan_bone_counts;
  std::uintptr_t mesh_scan_extra_address{};
  std::uint32_t mesh_scan_extra_count{};
  std::vector<std::pair<std::uintptr_t, std::uint32_t>> mesh_scan_candidates;
  // The mesh the last *completed* scan belonged to. Kept separate from
  // extra_mesh_owner, which is only set when the scan actually found extra
  // components: using that one as the "already scanned" marker made the scan
  // restart forever whenever it found nothing.
  std::uintptr_t mesh_scan_owner{};
  ULONGLONG next_mesh_scan_retry_ms{};
  std::map<std::uintptr_t, std::string> mesh_scan_class_cache;
  std::vector<std::string> mesh_scan_class_names;
  std::vector<std::uintptr_t> skeleton_meshes;

  // Extra skeletal mesh components owned by the same pawn (hair layers,
  // accessories, cloth). They carry their own small skeletons and may have
  // independent animation and budget scheduling; the diagnostic path records
  // their pose arrays without assuming a leader-pose relationship.
  struct ExtraMesh {
    std::uintptr_t object{};
    std::uintptr_t asset{};
    std::uint32_t bone_count{};
    std::uintptr_t component_space_data{};
    std::uintptr_t bone_space_data{};
    std::vector<std::uint32_t> bone_map;   // its bone -> body bone (max = none)
    std::vector<std::array<std::uint8_t, 8>> bone_fnames;
    std::vector<std::uint32_t> position_map;
    double position_match_cm{};
    bool used_position{};
    // Bind-pose mapping: the same idea as position_map, but both skeletons are
    // compared in their *bind* pose, which does not care about the current pose of
    // either mesh. Needed for meshes whose bones are named in a different space
    // (Bone_hairRb00, Pelvis_adjust): a name mapping finds nothing, and the live
    // positions are unusable while we override the body or while the mesh sits frozen.
    std::vector<std::uint32_t> bind_map;
    double bind_match_cm{};
    bool used_bind{};
    // Modular mesh driven through its own hierarchy: its anatomy attach bones take
    // the matching body bone's transform (fuzzy name match), the bones below them lag
    // behind their rigid orientation, and the components are accumulated through its
    // own parents. This is what a separately-authored hair mesh (its own
    // Bone_hairBR00..06 chains) needs, since nothing about it matches the body's
    // skeleton by name or by position.
    std::vector<std::int32_t> parents;
    std::vector<std::array<double, 12>> bind_locals;
    // Packed (12 doubles per bone) so this struct stays free of the transform types,
    // which are declared further down the file.
    std::vector<std::array<double, 12>> bind_world;
    std::vector<std::array<double, 4>> lag;
    bool lag_ready{};
    bool used_hierarchy{};
    std::uint32_t anchor_extra{};
    std::uint32_t anchor_body{};
    struct PoseProbe {
      std::uint32_t bone{};
      std::string name;
      std::array<double, 12> expected_component{};
      std::array<double, 12> last_buffer0{};
      bool write0_ok{};
      bool write1_ok{};
      bool has_expected{};
    };
    std::vector<PoseProbe> pose_probes;
    std::uintptr_t poseable_component{};
    bool poseable_active{};
    bool poseable_original_hidden{};
    bool poseable_original_hidden_saved{};
    bool poseable_attempted{};
    std::uint32_t poseable_socket_bone{(std::numeric_limits<std::uint32_t>::max)()};
    std::array<double, 12> poseable_socket_relative{};
    std::array<double, 12> poseable_expected_relative{};
    bool poseable_bind_written{};
    bool poseable_last_write_ok{};
    better_pose::accessory::Dynamics accessory_dynamics;
    std::vector<std::array<double, 12>> poseable_expected_pose;
    bool buffers_modified{};
    std::vector<std::uint8_t> saved_component;
    std::vector<std::uint8_t> saved_bone;
  };
  std::mutex extra_mesh_mutex;
  std::vector<ExtraMesh> extra_meshes;
  std::uintptr_t extra_mesh_owner{};
  std::uint32_t extra_mesh_mapped{};
  ULONGLONG next_extra_pose_probe_ms{};
  bool poseable_prototype_enabled{true};
};

std::atomic<Context *> g_active{};

// Declared early because the mesh resolution path (which runs long before the
// extra mesh helpers are defined) has to drop them when the pawn changes.
bool DropExtraMeshes(Context &context) noexcept;

AnomalyStatusV1 Status(const std::uint32_t code,
                       const std::string_view message = {}) noexcept {
  return {code, 0, {message.data(), message.size()}};
}

template <typename Struct, typename Field>
bool HasField(const Struct *value, const std::size_t offset) noexcept {
  return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service *Query(const AnomalyHostApiV1 *host, const char *id,
                     const std::uint32_t version) noexcept {
  return anomaly::sdk::Host(host).Query<Service>(id, version).get();
}

bool CoreReady(const AnomalyCoreServiceV1 *service) noexcept {
  return HasField<AnomalyCoreServiceV1,
                  decltype(AnomalyCoreServiceV1::read_memory)>(
             service, offsetof(AnomalyCoreServiceV1, read_memory)) &&
         HasField<AnomalyCoreServiceV1,
                  decltype(AnomalyCoreServiceV1::write_memory)>(
             service, offsetof(AnomalyCoreServiceV1, write_memory)) &&
         service->read_memory != nullptr && service->write_memory != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1 *service) noexcept {
  return HasField<AnomalySignatureServiceV1,
                  decltype(AnomalySignatureServiceV1::resolve)>(
             service, offsetof(AnomalySignatureServiceV1, resolve)) &&
         service->resolve != nullptr;
}

bool UiReady(const AnomalyUiServiceV1 *service) noexcept {
  return HasField<AnomalyUiServiceV1,
                  decltype(AnomalyUiServiceV1::input_double)>(
             service, offsetof(AnomalyUiServiceV1, input_double)) &&
         service->set_next_window_size != nullptr &&
         service->begin_window != nullptr && service->end_window != nullptr &&
         service->text != nullptr && service->checkbox != nullptr &&
         service->slider_float != nullptr && service->input_double != nullptr &&
         service->separator != nullptr && service->button != nullptr &&
         service->same_line != nullptr;
}

bool HookReady(const AnomalyHookServiceV1 *service) noexcept {
  return HasField<AnomalyHookServiceV1,
                  decltype(AnomalyHookServiceV1::end_callback)>(
             service, offsetof(AnomalyHookServiceV1, end_callback)) &&
         service->create != nullptr && service->release != nullptr &&
         service->begin_callback != nullptr && service->end_callback != nullptr;
}

bool ConfigReady(const AnomalyConfigServiceV1 *service) noexcept {
  return HasField<AnomalyConfigServiceV1,
                  decltype(AnomalyConfigServiceV1::write_atomic)>(
             service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
         service->register_schema != nullptr && service->read != nullptr &&
         service->write_atomic != nullptr &&
         service->unregister_schema != nullptr;
}

bool StorageReady(const AnomalyStorageServiceV1 *service) noexcept {
  return service != nullptr && service->read != nullptr &&
         service->write_atomic != nullptr;
}

bool SchedulerReady(const AnomalySchedulerServiceV1 *service) noexcept {
  return service != nullptr && service->schedule != nullptr;
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
  return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

void EnsurePoseAngleCapacity(Context &context) noexcept {
  std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
  const auto count = context.runtime.local_space_count;
  if (context.bone_angles.size() < count)
    context.bone_angles.resize(count);
}

// The camera VMD and its switch are stored in the same settings document as the pose: the host
// recreates the plugin on every reload, and re-picking the file plus re-enabling driving after
// each rebuild is pure friction. Declared here, defined next to the UTF-8 helpers it needs.

bool ApplyPoseDocument(Context &context, const nlohmann::json &json) noexcept {  try {
    if (!json.is_object() || !json.contains("bones") ||
        !json.at("bones").is_array())
      return false;
    std::vector<std::array<double, 3>> loaded;
    for (const auto &item : json.at("bones")) {
      if (!item.is_object() || !item.contains("index") ||
          !item.contains("pitch") || !item.contains("yaw") ||
          !item.contains("roll"))
        return false;
      const auto index = item.at("index").get<std::uint64_t>();
      if (index >= kMaximumBoneIndex)
        return false;
      auto pitch = item.at("pitch").get<double>();
      auto yaw = item.at("yaw").get<double>();
      auto roll = item.at("roll").get<double>();
      const auto normalize_angle = [](double value) {
        while (value > 180.0)
          value -= 360.0;
        while (value <= -180.0)
          value += 360.0;
        return value;
      };
      pitch = normalize_angle(pitch);
      yaw = normalize_angle(yaw);
      roll = normalize_angle(roll);
      if (pitch < -180.0 || pitch > 180.0 || yaw < -180.0 || yaw > 180.0 ||
          roll < -180.0 || roll > 180.0)
        return false;
      if (loaded.size() <= index)
        loaded.resize(static_cast<std::size_t>(index) + 1);
      loaded[static_cast<std::size_t>(index)] = {pitch, yaw, roll};
    }
    std::array<double, 3> root_offset{};
    if (json.contains("rootOffset")) {
      if (!json.at("rootOffset").is_array() || json.at("rootOffset").size() != 3)
        return false;
      for (std::size_t index{}; index != 3; ++index) {
        if (!json.at("rootOffset").at(index).is_number())
          return false;
        root_offset[index] = json.at("rootOffset").at(index).get<double>();
      }
    }
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    context.bone_angles = std::move(loaded);
    context.requested_root_offset[0].store(root_offset[0], std::memory_order_release);
    context.requested_root_offset[1].store(root_offset[1], std::memory_order_release);
    context.requested_root_offset[2].store(root_offset[2], std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

// The pose document: the edited joint angles and the body offset. The camera VMD and its switches
// used to ride along in the plugin's own saved settings, which meant a reload came back with
// whatever shot was last picked -- including one picked for a different character or a different
// scene. They are runtime state now: pick the file again when it is wanted.
std::string BuildPoseDocument(Context &context) noexcept {
  nlohmann::json root = nlohmann::json::object();
  auto bones = nlohmann::json::array();
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    for (std::size_t index{}; index != context.bone_angles.size(); ++index) {
      const auto &angle = context.bone_angles[index];
      if (angle[0] == 0.0 && angle[1] == 0.0 && angle[2] == 0.0)
        continue;
      bones.push_back({{"index", index},
                       {"pitch", angle[0]},
                       {"yaw", angle[1]},
                       {"roll", angle[2]}});
    }
  }
  std::array<double, 3> root_offset{};
  root_offset[0] = context.requested_root_offset[0].load(std::memory_order_acquire);
  root_offset[1] = context.requested_root_offset[1].load(std::memory_order_acquire);
  root_offset[2] = context.requested_root_offset[2].load(std::memory_order_acquire);
  root["bones"] = std::move(bones);
  root["rootOffset"] = root_offset;
  return root.dump();
}

std::string PoseProfilePath(const std::string &character_id) noexcept {
  return "character-pose-profile-" + character_id + ".json";
}

bool PersistPoseSettings(Context &context) noexcept {
  if (!ConfigReady(context.config))
    return false;
  try {
    const std::string document = BuildPoseDocument(context);
    if (document.size() > kMaximumPoseSettingsBytes)
      return false;
    const auto config_status = context.config->write_atomic(
        context.config->user, anomaly::sdk::StringView(kPoseSettingsSchemaId),
        kPoseSettingsSchemaVersion, Bytes(document));
    bool profile_ok = true;
    if (!context.active_character_id.empty() && StorageReady(context.storage)) {
      const std::string path = PoseProfilePath(context.active_character_id);
      const std::string profile = BuildPoseDocument(context);
      profile_ok =
          context.storage
              ->write_atomic(context.storage->user,
                             anomaly::sdk::StringView(path), Bytes(profile))
              .code == ANOMALY_STATUS_V1_OK;
    }
    return config_status.code == ANOMALY_STATUS_V1_OK && profile_ok;
  } catch (...) {
    return false;
  }
}

int LoadCharacterPoseProfile(Context &context,
                             const std::string &character_id) noexcept {
  if (!StorageReady(context.storage))
    return -1;
  const std::string path = PoseProfilePath(character_id);
  std::size_t size{};
  const auto probe = context.storage->read(
      context.storage->user, anomaly::sdk::StringView(path), {nullptr, 0},
      &size);
  if (probe.code == ANOMALY_STATUS_V1_NOT_FOUND)
    return 0;
  if (probe.code != ANOMALY_STATUS_V1_OK || size == 0 ||
      size > kMaximumPoseSettingsBytes)
    return -1;
  std::string document(size, '\0');
  std::size_t copied = size;
  if (context.storage
          ->read(context.storage->user, anomaly::sdk::StringView(path),
                 {reinterpret_cast<std::uint8_t *>(document.data()),
                  document.size()},
                 &copied)
          .code != ANOMALY_STATUS_V1_OK ||
      copied == 0 || copied > document.size())
    return -1;
  try {
    const auto json =
        nlohmann::json::parse(document.begin(), document.begin() + copied);
    return ApplyPoseDocument(context, json) ? 1 : -1;
  } catch (...) {
    return -1;
  }
}

void ResetPoseValues(Context &context) noexcept {
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    for (auto &angle : context.bone_angles)
      angle = {0.0, 0.0, 0.0};
  }
  context.requested_root_offset[0].store(0.0, std::memory_order_release);
  context.requested_root_offset[1].store(0.0, std::memory_order_release);
  context.requested_root_offset[2].store(0.0, std::memory_order_release);
}

bool LoadPoseSettings(Context &context) noexcept {
  if (!ConfigReady(context.config))
    return false;
  try {
    std::uint32_t version{};
    std::size_t size{};
    const auto probe = context.config->read(
        context.config->user, anomaly::sdk::StringView(kPoseSettingsSchemaId),
        &version, {nullptr, 0}, &size);
    if (probe.code == ANOMALY_STATUS_V1_NOT_FOUND) {
      {
        std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
        context.bone_angles.clear();
      }
      return PersistPoseSettings(context);
    }
    if (probe.code != ANOMALY_STATUS_V1_OK ||
        version != kPoseSettingsSchemaVersion || size == 0 ||
        size > kMaximumPoseSettingsBytes)
      return false;
    std::string document(size, '\0');
    std::size_t copied = size;
    if (context.config
            ->read(context.config->user,
                   anomaly::sdk::StringView(kPoseSettingsSchemaId), &version,
                   {reinterpret_cast<std::uint8_t *>(document.data()),
                    document.size()},
                   &copied)
            .code != ANOMALY_STATUS_V1_OK ||
        copied == 0 || copied > document.size())
      return false;
    const auto json =
        nlohmann::json::parse(document.begin(), document.begin() + copied);
    if (!ApplyPoseDocument(context, json))
      return false;
    return true;
  } catch (...) {
    return false;
  }
}
bool ObjectsReady(const AnomalyUe5ObjectsServiceV1 *service) noexcept {
  return HasField<AnomalyUe5ObjectsServiceV1,
                  decltype(AnomalyUe5ObjectsServiceV1::find_exact)>(
             service, offsetof(AnomalyUe5ObjectsServiceV1, find_exact)) &&
         service->find_exact != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1 *service) noexcept {
  return HasField<AnomalyUe5NamesServiceV1,
                  decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
             service, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
         service->resolve_utf8 != nullptr;
}

bool AddAddress(const std::uintptr_t base, const std::uint64_t offset,
                std::uintptr_t &result) noexcept {
  if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base)
    return false;
  result = base + static_cast<std::uintptr_t>(offset);
  return true;
}

template <typename T>
bool Read(Context &context, const std::uintptr_t address, T &value) noexcept {
  if (!CoreReady(context.core) || address == 0)
    return false;
  AnomalyMutableByteSpanV1 destination{
      reinterpret_cast<std::uint8_t *>(&value), sizeof(value)};
  return context.core->read_memory(context.core->user, address, destination)
             .code == ANOMALY_STATUS_V1_OK;
}

template <typename T>
bool Write(Context &context, const std::uintptr_t address,
           const T &value) noexcept {
  if (!CoreReady(context.core) || address == 0)
    return false;
  const AnomalyByteSpanV1 source{
      reinterpret_cast<const std::uint8_t *>(&value), sizeof(value)};
  return context.core->write_memory(context.core->user, address, source).code ==
         ANOMALY_STATUS_V1_OK;
}

bool ReadPointerAt(Context &context, const std::uintptr_t base,
                   const std::uint32_t offset,
                   std::uintptr_t &value) noexcept {
  std::uintptr_t address{};
  return AddAddress(base, offset, address) && Read(context, address, value) &&
         value != 0;
}

bool ResolveSignature(Context &context, const std::string_view pattern,
                      std::uintptr_t &address) noexcept {
  address = 0;
  return SignatureReady(context.signature) &&
         context.signature
                 ->resolve(context.signature->user,
                           anomaly::sdk::StringView("HTGame.exe"),
                           anomaly::sdk::StringView(".text"),
                           anomaly::sdk::StringView(pattern), &address)
                 .code == ANOMALY_STATUS_V1_OK &&
         address != 0;
}

bool ResolveGWorld(Context &context) noexcept {
  std::uintptr_t instruction{};
  std::int32_t displacement{};
  if (!ResolveSignature(context, kGWorldPattern, instruction) ||
      !Read(context, instruction + kGWorldResolveOffset, displacement))
    return false;
  const auto resolved = static_cast<std::intptr_t>(instruction) +
                        kGWorldInstructionSize + displacement;
  if (resolved <= 0)
    return false;
  context.runtime.g_world_address = static_cast<std::uintptr_t>(resolved);
  return true;
}

bool ResolveLocalCharacter(Context &context) noexcept {
  context.runtime.character = 0;
  context.runtime.mesh = 0;
  context.runtime.anim_instance = 0;
  std::uintptr_t world{};
  std::uintptr_t game_instance{};
  std::uintptr_t local_players{};
  std::uintptr_t local_player{};
  std::uintptr_t controller{};
  std::uintptr_t character{};
  std::uintptr_t mesh{};
  if (!Read(context, context.runtime.g_world_address, world) ||
      !ReadPointerAt(context, world, kWorldGameInstanceOffset, game_instance) ||
      !ReadPointerAt(context, game_instance, kGameInstanceLocalPlayersOffset,
                     local_players) ||
      !Read(context, local_players, local_player) || local_player == 0 ||
      !ReadPointerAt(context, local_player, kLocalPlayerControllerOffset,
                     controller) ||
      !ReadPointerAt(context, controller, kControllerPawnOffset, character) ||
      !ReadPointerAt(context, character, kCharacterMeshOffset, mesh)) {
    return false;
  }
  context.runtime.character = character;
  // The reference pose belongs to one character's mesh asset. Switching characters
  // must not leave the previous one's bind pose in place: two characters can share a
  // bone count, which is the only thing the size check compares.
  if (context.ref_pose_character != character) {
    context.ref_pose_character = character;
    context.ref_locals.clear();
    context.ref_pose_object = 0;
    context.ref_pose_status = "character changed";
    context.ref_pose_attempted = false;
  }
  // Only a *different, valid* mesh invalidates the extra components. Comparing
  // against a transient zero (runtime.mesh is cleared on frames where the local
  // character cannot be resolved) made every flicker drop them again while the
  // pending flag had already latched, so extra_mesh_owner stayed 0 forever and
  // the attach resync never ran.
  if (mesh != 0 && context.runtime.mesh != 0 && context.runtime.mesh != mesh)
    static_cast<void>(DropExtraMeshes(context));
  context.runtime.mesh = mesh;
  if (context.extra_mesh_owner != mesh &&
      context.mesh_scan_owner != mesh &&
      !context.mesh_scan_requested.load(std::memory_order_acquire) &&
      !context.mesh_scan_running) {
    // The extra components belong to a pawn, so switching character invalidates
    // them; rescan once per pawn instead of asking for another scan. Cheap: it is
    // chunked across ticks and read-only.
    context.mesh_scan_requested.store(true, std::memory_order_release);
  }
  return true;
}

bool ReadAnimationState(Context &context) noexcept {
  const auto mesh = context.runtime.mesh;
  if (mesh == 0)
    return false;
  std::uint8_t animation_mode{};
  std::uint8_t animation_flags{};
  if (!Read(context, mesh + kMeshAnimationModeOffset, animation_mode) ||
      !Read(context, mesh + kMeshAnimationFlagsOffset, animation_flags))
    return false;
  static_cast<void>(Read(context, mesh + kMeshAnimScriptInstanceOffset,
                        context.runtime.anim_instance));
  context.runtime.animation_mode = animation_mode;
  context.runtime.animation_flags = animation_flags;
  return true;
}

bool ReadArrayHeader(Context &context, const std::uintptr_t array_address,
                     std::uintptr_t &data, std::uint32_t &count) noexcept {
  data = 0;
  count = 0;
  if (array_address == 0)
    return false;
  std::int32_t count_value{};
  if (!Read(context, array_address + kArrayDataOffset, data) ||
      !Read(context, array_address + kArrayCountOffset, count_value) ||
      count_value < 0)
    return false;
  count = static_cast<std::uint32_t>(count_value);
  return true;
}

bool ReadPoseArrays(Context &context) noexcept {
  const auto mesh = context.runtime.mesh;
  if (mesh == 0)
    return false;
  std::uintptr_t bone_space_data{};
  std::uint32_t bone_space_count{};
  std::uintptr_t component_space_data{};
  std::uint32_t component_space_count{};
  std::uintptr_t local_space_data{};
  std::uint32_t local_space_count{};
  const bool authoritative =
      ReadArrayHeader(context, mesh + kMeshComponentSpaceBuffer0Offset,
                      bone_space_data, bone_space_count) &&
      ReadArrayHeader(context, mesh + kMeshComponentSpaceBuffer1Offset,
                      component_space_data, component_space_count) &&
      ReadArrayHeader(context, mesh + kMeshLocalSpaceTransformsOffset,
                      local_space_data, local_space_count) &&
      bone_space_data != 0 && bone_space_count != 0 &&
      component_space_data != 0 && component_space_count != 0 &&
      local_space_data != 0 && local_space_count != 0 &&
      bone_space_count == component_space_count &&
      component_space_count == local_space_count;
  if (authoritative) {
    context.runtime.bone_space_data = bone_space_data;
    context.runtime.bone_space_count = bone_space_count;
    context.runtime.component_space_data = component_space_data;
    context.runtime.component_space_count = component_space_count;
    context.runtime.local_space_data = local_space_data;
    context.runtime.local_space_count = local_space_count;
    return true;
  }
  static_cast<void>(ReadArrayHeader(
      context, mesh + kMeshCachedBoneSpaceTransformsOffset,
      context.runtime.bone_space_data, context.runtime.bone_space_count));
  return ReadArrayHeader(
      context, mesh + kMeshCachedComponentSpaceTransformsOffset,
      context.runtime.component_space_data,
      context.runtime.component_space_count);
}

bool RestorePause(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!state.saved_pause || state.mesh == 0)
    return true;
  if (!Write(context, state.mesh + kMeshAnimationFlagsOffset,
             state.original_animation_flags))
    return false;
  state.saved_pause = false;
  state.animation_flags = state.original_animation_flags;
  return true;
}

bool ApplyPause(Context &context, const bool enabled) noexcept {
  RuntimeState &state = context.runtime;
  if (state.mesh == 0)
    return false;
  if (!enabled)
    return RestorePause(context);
  if (state.saved_pause)
    return true;
  std::uint8_t flags{};
  if (!Read(context, state.mesh + kMeshAnimationFlagsOffset, flags))
    return false;
  state.original_animation_flags = flags;
  flags |= static_cast<std::uint8_t>(1U << kAnimationFlagPauseAnimsBit);
  if (!Write(context, state.mesh + kMeshAnimationFlagsOffset, flags))
    return false;
  state.saved_pause = true;
  state.animation_flags = flags;
  return true;
}

bool RestoreRate(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!state.saved_rate || state.mesh == 0)
    return true;
  if (!Write(context, state.mesh + kMeshGlobalAnimRateScaleOffset,
             state.original_rate_scale))
    return false;
  state.saved_rate = false;
  return true;
}

bool ApplyRate(Context &context, const bool enabled, const float value) noexcept {
  RuntimeState &state = context.runtime;
  if (state.mesh == 0)
    return false;
  if (!enabled)
    return RestoreRate(context);
  if (!state.saved_rate) {
    float original{};
    if (!Read(context, state.mesh + kMeshGlobalAnimRateScaleOffset, original))
      return false;
    state.original_rate_scale = original;
    state.saved_rate = true;
  }
  return Write(context, state.mesh + kMeshGlobalAnimRateScaleOffset, value);
}

bool RestoreRootMotion(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!state.saved_root_motion || state.character == 0)
    return true;
  if (!Write(context, state.character + kCharacterAnimRootMotionScaleOffset,
             state.original_root_motion_scale))
    return false;
  state.saved_root_motion = false;
  return true;
}

bool ApplyRootMotion(Context &context, const bool enabled,
                     const float value) noexcept {
  RuntimeState &state = context.runtime;
  if (state.character == 0)
    return false;
  if (!enabled)
    return RestoreRootMotion(context);
  if (!state.saved_root_motion) {
    float original{};
    if (!Read(context, state.character + kCharacterAnimRootMotionScaleOffset,
              original))
      return false;
    state.original_root_motion_scale = original;
    state.saved_root_motion = true;
  }
  return Write(context, state.character + kCharacterAnimRootMotionScaleOffset,
               value);
}

bool RestoreMultiThreadedUpdate(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!state.saved_multi_threaded_update || state.anim_instance == 0)
    return true;
  if (state.multi_threaded_update_instance != state.anim_instance) {
    state.saved_multi_threaded_update = false;
    return true;
  }
  if (!Write(context, state.anim_instance + kAnimInstanceUseMultiThreadedUpdateOffset,
             state.original_multi_threaded_update_flags))
    return false;
  state.saved_multi_threaded_update = false;
  state.multi_threaded_update_instance = 0;
  return true;
}

bool ApplyMultiThreadedUpdate(Context &context, const bool enabled) noexcept {
  RuntimeState &state = context.runtime;
  if (!enabled)
    return RestoreMultiThreadedUpdate(context);
  if (state.anim_instance == 0)
    return false;
  if (state.saved_multi_threaded_update &&
      state.multi_threaded_update_instance != state.anim_instance) {
    state.saved_multi_threaded_update = false;
  }
  if (!state.saved_multi_threaded_update) {
    std::uint8_t flags{};
    if (!Read(context, state.anim_instance + kAnimInstanceUseMultiThreadedUpdateOffset,
              flags))
      return false;
    state.original_multi_threaded_update_flags = flags;
    state.saved_multi_threaded_update = true;
    state.multi_threaded_update_instance = state.anim_instance;
  }
  std::uint8_t flags = state.original_multi_threaded_update_flags;
  flags &= static_cast<std::uint8_t>(
      ~(1U << kAnimInstanceUseMultiThreadedUpdateBit));
  return Write(context, state.anim_instance + kAnimInstanceUseMultiThreadedUpdateOffset,
               flags);
}

bool RestorePose(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!state.saved_pose)
    return true;
  state.pose_descendants.clear();
  state.saved_pose = false;
  return true;
}

bool CallVirtualUFunction(Context &context, std::uintptr_t object,
                          std::string_view function_path, const void *parameters,
                          std::size_t parameter_size, std::string &detail,
                          void *output = nullptr) noexcept;
bool FindObjectAddressByPath(Context &context, std::string_view path, std::uintptr_t &object,
                             std::string *detail) noexcept;
bool ApplyPoseByName(Context &context, std::uint32_t bone,
                     const std::array<double, 3> &translation,
                     std::string &detail) noexcept;

bool ForceMeshObjectUpdate(Context &context,
                           const std::uintptr_t mesh) noexcept {
  if (mesh == 0)
    return false;
  std::uint8_t flags{};
  if (!Read(context, mesh + kMeshForceMeshObjectUpdateOffset, flags))
    return false;
  flags |= static_cast<std::uint8_t>(1U << kMeshForceMeshObjectUpdateBit);
  return Write(context, mesh + kMeshForceMeshObjectUpdateOffset, flags);
}

bool ForcePoseMeshObjectUpdate(Context &context) noexcept {
  return ForceMeshObjectUpdate(context, context.runtime.mesh);
}

struct Vec3d {
  double x{};
  double y{};
  double z{};
};

struct Quatd {
  double x{};
  double y{};
  double z{};
  double w{1.0};
};

struct Transformd {
  Quatd rotation{};
  Vec3d translation{};
  Vec3d scale{1.0, 1.0, 1.0};
};

Quatd QuatMultiply(const Quatd &a, const Quatd &b) noexcept {
  Quatd out;
  out.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  out.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  out.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  out.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  return out;
}

Vec3d QuatRotateVector(const Quatd &q, const Vec3d &v) noexcept {
  const Vec3d u{q.x, q.y, q.z};
  const Vec3d uv{
      u.y * v.z - u.z * v.y,
      u.z * v.x - u.x * v.z,
      u.x * v.y - u.y * v.x,
  };
  const Vec3d uuv{
      u.y * uv.z - u.z * uv.y,
      u.z * uv.x - u.x * uv.z,
      u.x * uv.y - u.y * uv.x,
  };
  const double two = 2.0;
  return Vec3d{
      v.x + two * (q.w * uv.x + uuv.x),
      v.y + two * (q.w * uv.y + uuv.y),
      v.z + two * (q.w * uv.z + uuv.z),
  };
}

Transformd TransformMultiply(const Transformd &a,
                             const Transformd &b) noexcept {
  Transformd out;
  out.rotation = QuatMultiply(a.rotation, b.rotation);
  const Vec3d scaled{
      b.translation.x * a.scale.x,
      b.translation.y * a.scale.y,
      b.translation.z * a.scale.z,
  };
  const Vec3d rotated = QuatRotateVector(a.rotation, scaled);
  out.translation = Vec3d{
      a.translation.x + rotated.x,
      a.translation.y + rotated.y,
      a.translation.z + rotated.z,
  };
  out.scale = Vec3d{
      a.scale.x * b.scale.x,
      a.scale.y * b.scale.y,
      a.scale.z * b.scale.z,
  };
  return out;
}

Quatd RotatorToQuat(const double pitch_degrees, const double yaw_degrees,
                    const double roll_degrees) noexcept {
  constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
  const double half = kDegreesToRadians * 0.5;
  const double sp = std::sin(pitch_degrees * half);
  const double cp = std::cos(pitch_degrees * half);
  const double sy = std::sin(yaw_degrees * half);
  const double cy = std::cos(yaw_degrees * half);
  const double sr = std::sin(roll_degrees * half);
  const double cr = std::cos(roll_degrees * half);
  Quatd out;
  out.x = cr * sp * sy - sr * cp * cy;
  out.y = -cr * sp * cy - sr * cp * sy;
  out.z = cr * cp * sy - sr * sp * cy;
  out.w = cr * cp * cy + sr * sp * sy;
  return out;
}

bool ReadTransform(Context &context, const std::uintptr_t address,
                   Transformd &transform) noexcept {
  if (!Read(context, address + kTransformRotationOffset, transform.rotation) ||
      !Read(context, address + kTransformTranslationOffset,
            transform.translation) ||
      !Read(context, address + kTransformScaleOffset, transform.scale))
    return false;
  return true;
}

bool WriteTransform(Context &context, const std::uintptr_t address,
                    const Transformd &transform) noexcept {
  return Write(context, address + kTransformRotationOffset,
               transform.rotation) &&
         Write(context, address + kTransformTranslationOffset,
               transform.translation) &&
         Write(context, address + kTransformScaleOffset, transform.scale);
}

struct PackedTransform {
  double rotation[4]{};
  double translation[3]{};
  double padding{};
  double scale[3]{1.0, 1.0, 1.0};
  double tail_padding{};
};

static_assert(sizeof(PackedTransform) == kTransformSize,
              "PackedTransform must match the game FTransform layout");

Transformd UnpackTransform(const PackedTransform &source) noexcept {
  Transformd out;
  out.rotation =
      Quatd{source.rotation[0], source.rotation[1], source.rotation[2],
            source.rotation[3]};
  out.translation = Vec3d{source.translation[0], source.translation[1],
                          source.translation[2]};
  out.scale = Vec3d{source.scale[0], source.scale[1], source.scale[2]};
  return out;
}

void PackTransform(const Transformd &source, PackedTransform &destination) noexcept {
  destination.rotation[0] = source.rotation.x;
  destination.rotation[1] = source.rotation.y;
  destination.rotation[2] = source.rotation.z;
  destination.rotation[3] = source.rotation.w;
  destination.translation[0] = source.translation.x;
  destination.translation[1] = source.translation.y;
  destination.translation[2] = source.translation.z;
  destination.padding = 0.0;
  destination.scale[0] = source.scale.x;
  destination.scale[1] = source.scale.y;
  destination.scale[2] = source.scale.z;
}


bool WriteBytes(Context &context, const std::uintptr_t address,
                const void *data, const std::size_t size) noexcept {
  if (!CoreReady(context.core) || address == 0 || data == nullptr || size == 0)
    return false;
  const AnomalyByteSpanV1 source{
      reinterpret_cast<const std::uint8_t *>(data), size};
  return context.core->write_memory(context.core->user, address, source).code ==
         ANOMALY_STATUS_V1_OK;
}

Transformd ComputeBoneComponent(
    const std::uint32_t bone_index,
    const std::vector<Transformd> &locals,
    const std::vector<std::int32_t> &parents,
    std::vector<Transformd> &components,
    std::vector<std::uint8_t> &marks) noexcept {
  if (marks[bone_index] == 2)
    return components[bone_index];
  if (marks[bone_index] == 1)
    return {};
  marks[bone_index] = 1;
  Transformd parent_component;
  const std::int32_t parent = parents[bone_index];
  if (parent >= 0 && static_cast<std::uint32_t>(parent) < parents.size())
    parent_component =
        ComputeBoneComponent(static_cast<std::uint32_t>(parent), locals,
                             parents, components, marks);
  components[bone_index] =
      TransformMultiply(parent_component, locals[bone_index]);
  marks[bone_index] = 2;
  return components[bone_index];
}

void ApplyPoseOverridesInTick(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!context.pose_override_enabled.load(std::memory_order_acquire) ||
      state.mesh == 0 || state.local_space_data == 0 || state.pose_data == 0 ||
      state.pose_component_data == 0 || state.local_space_count == 0 ||
      state.pose_count != state.local_space_count ||
      state.pose_component_count != state.local_space_count)
    return;

  const std::uint32_t count = state.local_space_count;
  std::vector<std::array<double, 3>> angles;
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    if (context.bone_angles.size() < count)
      return;
    angles.assign(context.bone_angles.begin(),
                  context.bone_angles.begin() + count);
  }
  if (context.bone_parents.size() != count)
    return;

  bool any_override = false;
  for (const auto &angle : angles) {
    if (angle[0] != 0.0 || angle[1] != 0.0 || angle[2] != 0.0) {
      any_override = true;
      break;
    }
  }
  if (!any_override)
    return;

  std::vector<Transformd> locals(count);
  for (std::uint32_t bone_index{}; bone_index != count; ++bone_index) {
    std::uintptr_t local_address{};
    if (!AddAddress(state.local_space_data,
                    static_cast<std::uint64_t>(bone_index) * kTransformSize,
                    local_address) ||
        !ReadTransform(context, local_address, locals[bone_index]))
      return;
    if (angles[bone_index][0] != 0.0 || angles[bone_index][1] != 0.0 ||
        angles[bone_index][2] != 0.0) {
      const Quatd offset =
          RotatorToQuat(angles[bone_index][0], angles[bone_index][1],
                        angles[bone_index][2]);
      locals[bone_index].rotation =
          QuatMultiply(offset, locals[bone_index].rotation);
    }
  }

  std::vector<Transformd> components(count);
  std::vector<std::uint8_t> marks(count, 0);
  for (std::uint32_t bone_index{}; bone_index != count; ++bone_index)
    static_cast<void>(ComputeBoneComponent(bone_index, locals,
                                           context.bone_parents, components,
                                           marks));

  std::vector<PackedTransform> packed(count);
  for (std::uint32_t bone_index{}; bone_index != count; ++bone_index)
    PackTransform(components[bone_index], packed[bone_index]);

  const auto *bytes = reinterpret_cast<const std::uint8_t *>(packed.data());
  const auto byte_count = packed.size() * sizeof(PackedTransform);
  static_cast<void>(WriteBytes(context, state.pose_data, bytes, byte_count));
  static_cast<void>(
      WriteBytes(context, state.pose_component_data, bytes, byte_count));
  static_cast<void>(ForcePoseMeshObjectUpdate(context));
}

bool BuildPoseDescendants(Context &context, const std::uint32_t bone,
                          std::vector<std::uint32_t> &descendants) noexcept {
  descendants.clear();
  const auto count = context.runtime.local_space_count;
  if (bone >= count)
    return false;
  descendants.push_back(bone);
  if (context.bone_parents.size() != count)
    return false;
  std::vector<std::vector<std::uint32_t>> children(count);
  for (std::uint32_t index{}; index != count; ++index) {
    const std::int32_t parent = context.bone_parents[index];
    if (parent >= 0 && static_cast<std::uint32_t>(parent) < count)
      children[static_cast<std::uint32_t>(parent)].push_back(index);
  }
  for (std::size_t cursor{}; cursor != descendants.size(); ++cursor) {
    const auto parent = descendants[cursor];
    for (const std::uint32_t child : children[parent])
      descendants.push_back(child);
  }
  return true;
}

bool ApplyPose(Context &context, const bool enabled, const std::uint32_t bone,
               const std::array<double, 3> &rotation) noexcept {
  RuntimeState &state = context.runtime;
  if (state.mesh == 0 || state.bone_space_data == 0 ||
      state.bone_space_count == 0 || bone >= state.bone_space_count ||
      state.component_space_data == 0 || state.component_space_count == 0 ||
      bone >= state.component_space_count || state.local_space_data == 0 ||
      state.local_space_count == 0 || bone >= state.local_space_count)
    return false;
  if (!enabled)
    return RestorePose(context);

  const bool same_bone = state.saved_pose && state.pose_bone_index == bone &&
                         state.pose_mesh == state.mesh &&
                         state.pose_data == state.bone_space_data &&
                         state.pose_component_data == state.component_space_data;
  if (!same_bone) {
    if (state.saved_pose && !RestorePose(context))
      return false;
    std::vector<std::uint32_t> descendants;
    if (!BuildPoseDescendants(context, bone, descendants) ||
        descendants.empty())
      return false;
    state.saved_pose = true;
    state.pose_mesh = state.mesh;
    state.pose_data = state.bone_space_data;
    state.pose_count = state.bone_space_count;
    state.pose_component_data = state.component_space_data;
    state.pose_component_count = state.component_space_count;
    state.pose_bone_index = bone;
    state.pose_descendants = std::move(descendants);
  }

  std::snprintf(context.pose_status.data(), context.pose_status.size(),
                "pose: joint=%u pitch=%.2f yaw=%.2f roll=%.2f bones=%u",
                static_cast<unsigned>(bone), rotation[0], rotation[1],
                rotation[2],
                static_cast<unsigned>(state.pose_descendants.size()));
  return true;
}

bool ResolvePoseTickTarget(Context &context, std::uintptr_t &target) noexcept {
  target = 0;
  const auto mesh = context.runtime.mesh;
  if (mesh == 0)
    return false;
  std::uintptr_t vtable{};
  if (!Read(context, mesh, vtable) || vtable == 0)
    return false;
  std::uintptr_t function{};
  const std::uint64_t slot_address =
      static_cast<std::uint64_t>(kSkeletalMeshTickVtableSlot) * sizeof(void *);
  if (slot_address > (std::numeric_limits<std::uintptr_t>::max)() - vtable ||
      !Read(context, vtable + static_cast<std::uintptr_t>(slot_address),
            function) ||
      function == 0)
    return false;
  if (function < 0x140000000ULL || function > 0x180000000ULL)
    return false;
  target = function;
  return true;
}

void ANOMALY_CALL SkeletalMeshTickDetour(void *object, float delta_seconds,
                                         unsigned tick_type,
                                         void *tick_function) noexcept;
void ApplyPoseOverridesDirect(Context &context) noexcept;
void ApplyMotionPoseDirect(Context &context) noexcept;

// Two root references for the camera, published as atomics so the view-build thread never touches the
// pose buffers.
void PublishRootOffsets(Context &context, const double applied[3],
                        const double authored[3]) noexcept {
  for (int axis = 0; axis < 3; ++axis) {
    context.motion_applied_offset[axis].store(applied[axis], std::memory_order_release);
    context.motion_authored_offset[axis].store(authored[axis], std::memory_order_release);
  }
}
void MaybeRefreshBoneNames(Context &context) noexcept;
bool GetBoneNameForMesh(Context &context, std::uintptr_t mesh,
                        std::uint32_t bone_index, std::string &name) noexcept;
bool GetBoneFNameForMesh(Context &context, std::uintptr_t mesh,
                         std::uint32_t bone_index,
                         std::array<std::uint8_t, 8> &fname,
                         std::string &name) noexcept;
bool TryAssetReferencePose(Context &context, std::uintptr_t asset) noexcept;
bool FindReferencePose(Context &context) noexcept;
Transformd TransformInverse(const Transformd &value) noexcept;
std::string ClassNameOf(Context &context, std::uintptr_t object) noexcept;
std::string ObjectNameOf(Context &context, std::uintptr_t object) noexcept;
void LogDiagnostic(Context &context, const std::string &message) noexcept;
void BuildExtraMeshes(Context &context) noexcept;
void WriteExtraMeshes(Context &context,
                      const std::vector<PackedTransform> &packed) noexcept;
bool RestoreExtraMeshes(Context &context) noexcept;
bool DropExtraMeshes(Context &context) noexcept;
bool EnsurePoseableAccessory(Context &context, Context::ExtraMesh &extra) noexcept;
bool WritePoseableAccessoryPose(Context &context,
                                Context::ExtraMesh &extra,
                                const std::vector<PackedTransform> &components) noexcept;
void DestroyPoseableAccessories(Context &context) noexcept;
void DestroyStalePoseableComponent(Context &context,
                                   std::uintptr_t component) noexcept;
void RestoreAccessoryVisibility(Context &context,
                                std::uintptr_t component) noexcept;

bool ReleasePoseTickHook(Context &context) noexcept {
  if (!HookReady(context.hook) || context.tick_hook.id == 0)
    return true;
  const auto status =
      context.hook->release(context.hook->user, context.tick_hook);
  if (status.code != ANOMALY_STATUS_V1_OK) {
    return false;
  }
  context.tick_hook = {};
  context.tick_original = 0;
  context.tick_target = 0;
  g_active.store(nullptr, std::memory_order_release);
  return true;
}

bool EnsurePoseTickHook(Context &context) noexcept {
  if (!HookReady(context.hook))
    return false;
  std::uintptr_t target{};
  if (!ResolvePoseTickTarget(context, target))
    return false;
  if (context.tick_hook.id != 0 && context.tick_target == target &&
      context.tick_original != 0)
    return true;
  if (context.tick_hook.id != 0) {
    if (!ReleasePoseTickHook(context))
      return false;
  }
  g_active.store(&context, std::memory_order_release);
  AnomalyHookRequestV1 request{sizeof(request)};
  request.kind = ANOMALY_HOOK_V1_FUNCTION;
  request.target = target;
  request.detour = reinterpret_cast<void *>(&SkeletalMeshTickDetour);
  request.label = anomaly::sdk::StringView("character-pose-skeletal-tick");
  std::uintptr_t original{};
  AnomalyGenerationHandleV1 handle{};
  const auto status =
      context.hook->create(context.hook->user, &request, &original, &handle);
  if (status.code != ANOMALY_STATUS_V1_OK || handle.id == 0 || original == 0) {
    g_active.store(nullptr, std::memory_order_release);
    context.tick_hook = {};
    context.tick_original = 0;
    context.tick_target = 0;
    return false;
  }
  context.tick_hook = handle;
  context.tick_original = original;
  context.tick_target = target;
  return true;
}

using SkeletalMeshTickFn = void(ANOMALY_CALL *)(void *, float, unsigned, void *);

void ANOMALY_CALL SkeletalMeshTickDetour(void *object, float delta_seconds,
                                         unsigned tick_type,
                                         void *tick_function) noexcept {
  Context *context = g_active.load(std::memory_order_acquire);
  AnomalyGenerationHandleV1 lease{};
  bool leased = false;
  SkeletalMeshTickFn original = nullptr;
  try {
    if (context != nullptr && context->tick_hook.id != 0 &&
        HookReady(context->hook)) {
      leased = context->hook
                   ->begin_callback(context->hook->user, context->tick_hook,
                                    &lease)
                   .code == ANOMALY_STATUS_V1_OK;
      original = reinterpret_cast<SkeletalMeshTickFn>(context->tick_original);
    }
  } catch (...) {
    original = context == nullptr
                   ? nullptr
                   : reinterpret_cast<SkeletalMeshTickFn>(context->tick_original);
  }

  try {
    if (original != nullptr)
      original(object, delta_seconds, tick_type, tick_function);
  } catch (...) {
  }

  try {
    if (context != nullptr &&
        (context->motion_loaded.load(std::memory_order_acquire) ||
         context->pose_override_enabled.load(std::memory_order_acquire)) &&
        object == reinterpret_cast<void *>(context->runtime.mesh)) {
      if (context->motion_loaded.load(std::memory_order_acquire))
        ApplyMotionPoseDirect(*context);
      else
        ApplyPoseOverridesDirect(*context);
    }
  } catch (...) {
  }

  if (leased && context != nullptr && HookReady(context->hook)) {
    static_cast<void>(context->hook->end_callback(context->hook->user, lease));
  }
}

// ---------------------------------------------------------------------------
// Camera VMD driving: the manager's cached POV is overwritten with the sampled track.
// ---------------------------------------------------------------------------

constexpr double kCameraDegreesToRadians = 3.14159265358979323846 / 180.0;
constexpr double kCameraRadiansToDegrees = 180.0 / 3.14159265358979323846;
// The reference model both converters are built around (data/reference-pmx.json, 初音ミク) has a
// leg of 9.418982 units -- the value `mmdLegLength` carries and the denominator of the runtime's
// centimetres-per-unit ratio. With no motion loaded there is no file to take that side from.
constexpr double kReferenceLegUnits = 9.418982;
// Last resort, when neither the reference pose nor the captured pose could be read: MMD's own
// convention of about 8 cm per unit (a 20-unit model is ~160 cm). It is 6.8 % short for a 169 cm
// character, which is what made a camera-only shot tighter than the file's own framing.
constexpr double kCameraFallbackUnitCm = 8.0;

// Camera basis of an MMD camera rotation triple (radians). MMD stores a Y-X-Z Euler
// triple and its camera looks along its own +Z, with +X to the right and +Y up.
void MmdCameraBasis(const double rotation[3], double right[3], double up[3],
                    double forward[3]) noexcept {
  const double cx = std::cos(rotation[0]);
  const double sx = std::sin(rotation[0]);
  const double cy = std::cos(rotation[1]);
  const double sy = std::sin(rotation[1]);
  const double cz = std::cos(rotation[2]);
  const double sz = std::sin(rotation[2]);
  // R = Ry * Rx * Rz; its columns are the camera's right, up and forward axes.
  const double matrix[3][3] = {
      {cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx},
      {cx * sz, cx * cz, -sx},
      {-sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx}};
  double *columns[3] = {right, up, forward};
  for (int column = 0; column < 3; ++column) {
    for (int row = 0; row < 3; ++row)
      columns[column][row] = matrix[row][column];
  }
}

// Camera basis of a game view rotation in degrees (X forward, Y right, Z up): the same
// decomposition the view-point rotation itself carries.
std::atomic<Context *> g_camera_pov{};

using CameraPovFn = void *(ANOMALY_CALL *)(void *, void *, void *);

void *ANOMALY_CALL CameraPovDetour(void *self, void *first, void *second) noexcept;

// PlayerController -> camera manager, the offset the active Profile validates.
bool ResolveCameraManager(Context &context, std::uintptr_t &manager) noexcept {
  manager = 0;
  std::uintptr_t world{};
  std::uintptr_t game_instance{};
  std::uintptr_t local_players{};
  std::uintptr_t local_player{};
  std::uintptr_t controller{};
  return Read(context, context.runtime.g_world_address, world) &&
         ReadPointerAt(context, world, kWorldGameInstanceOffset, game_instance) &&
         ReadPointerAt(context, game_instance, kGameInstanceLocalPlayersOffset,
                       local_players) &&
         Read(context, local_players, local_player) && local_player != 0 &&
         ReadPointerAt(context, local_player, kLocalPlayerControllerOffset, controller) &&
         ReadPointerAt(context, controller, kControllerCameraManagerOffset, manager) &&
         manager != 0;
}

// The manager's own vtable slot holds the view-point getter. Before it becomes a hook
// target its body is checked: the getter reads the manager's vtable (`mov rax,[rcx]`) and
// calls the POV accessor (`call [rax+0x7A0]`) before copying the POV into the caller's
// outputs. This is what keeps a wrong or stale slot (and another camera implementation)
// from being hooked, which on a stale offset would mean detouring an unrelated function.
// Replace the game's view point with the loaded camera track. The track is applied as a
// movement away from the pose the game produced on the first driven frame: the MMD
// camera's displacement is expressed in that first frame's own camera basis and rebuilt
// in the anchor's basis, which reproduces the shot without having to know how MMD's axes
// line up with this rig's (the mapping is applied to the basis and to the offsets, so it
// cancels). Both sides use the same Y-X-Z triple, so the rotation deltas simply add.
// Drive the game's camera from the loaded track: read the manager's cached POV, let
// ApplyCameraTrack turn the track's movement into a world pose relative to the anchor, and
// write it back. Nothing else in the frame rewrites the cache, and the getter (owned by the
// other plugin) copies from it, so the write reaches the view.
void *ANOMALY_CALL CameraPovDetour(void *self, void *first, void *second) noexcept;

// The view-point getter (manager vtable +0x850) belongs to the free-camera plugin, so it is not
// available as a hook target here. Its own body names the accessor that produces the POV through
// a second vtable slot, and that slot is read out of the getter's bytes rather than hardcoded:
//     48 8b 01            mov rax, [rcx]       ; the manager's vtable
//     ff 90 a0 07 00 00   call [rax+0x7A0]     ; -> the POV accessor
// Hooking the accessor and returning a patched copy of its result is the same edit the getter
// would have received, made one call closer to the source.
// The view-point getter (manager vtable +0x850) is the function the view is actually built from:
// it fills its two out-parameters with the camera's location and rotation, which is how the
// free-camera plugin drives the camera and the only place a write has been seen to reach the
// screen. Its own body names the accessor that produces the POV member, and that is still resolved
// -- for the struct the file's lens has to be written into -- from the `call [rax+slot]` and the
// accessor's `lea rax,[rcx+disp32]`, rather than hardcoded.
bool ResolveCameraPovTarget(Context &context, const std::uintptr_t manager,
                            std::uintptr_t &target) noexcept {
  target = 0;
  std::uintptr_t vtable{};
  std::uintptr_t getter{};
  if (!Read(context, manager, vtable) || vtable == 0 ||
      !ReadPointerAt(context, vtable, kCameraViewPointVtableOffset, getter) || getter == 0)
    return false;
  std::array<std::uint8_t, 32> code{};
  if (!Read(context, getter, code))
    return false;
  for (std::size_t index = 0; index + 6 <= code.size(); ++index) {
    if (code[index] != 0xFF || code[index + 1] != 0x90)
      continue;
    const std::uint32_t slot = static_cast<std::uint32_t>(code[index + 2]) |
                               (static_cast<std::uint32_t>(code[index + 3]) << 8U) |
                               (static_cast<std::uint32_t>(code[index + 4]) << 16U) |
                               (static_cast<std::uint32_t>(code[index + 5]) << 24U);
    std::uintptr_t accessor{};
    if (!ReadPointerAt(context, vtable, slot, accessor) || accessor == 0 || accessor == getter)
      continue;
    std::array<std::uint8_t, 8> body{};
    if (!Read(context, accessor, body) || body[0] != 0x48 || body[1] != 0x8D ||
        body[2] != 0x81 || body[7] != 0xC3)
      continue;
    const std::uint32_t displacement = static_cast<std::uint32_t>(body[3]) |
                                       (static_cast<std::uint32_t>(body[4]) << 8U) |
                                       (static_cast<std::uint32_t>(body[5]) << 16U) |
                                       (static_cast<std::uint32_t>(body[6]) << 24U);
    std::uintptr_t structure{};
    if (AddAddress(manager, displacement, structure) && structure != 0)
      context.camera_pov_struct = structure;
    target = getter;
    return true;
  }
  return false;
}

bool RefreshCameraPovTarget(Context &context) noexcept {
  std::uintptr_t manager{};
  if (!ResolveCameraManager(context, manager)) {
    context.camera_manager.store(0, std::memory_order_release);
    context.camera_manager_resolved.store(false, std::memory_order_release);
    return false;
  }
  // Once this plugin's detour owns the accessor, its first bytes are that detour's stub rather
  // than the plain getter, so the shape gate must not be asked again. A swapped camera is caught
  // by the manager pointer changing.
  if (context.camera_pov_hook.id != 0 &&
      context.camera_manager.load(std::memory_order_acquire) == manager)
    return true;
  std::uintptr_t target{};
  if (!ResolveCameraPovTarget(context, manager, target)) {
    context.camera_manager.store(0, std::memory_order_release);
    context.camera_manager_resolved.store(false, std::memory_order_release);
    return false;
  }
  const std::uintptr_t previous =
      context.camera_manager.exchange(manager, std::memory_order_acq_rel);
  context.camera_pov_resolved_target = target;
  context.camera_manager_resolved.store(true, std::memory_order_release);
  // A different camera taking over does *not* drop the anchor: the shot's angle is a world
  // direction, the placement is relative to the character, so the shot survives the swap. Clearing
  // it here is what left the modes writing nothing at all, because the replacement reading the
  // re-anchor waits for is often a placeholder the distance test then rejects.
  static_cast<void>(previous);
  return true;
}

bool ReleaseCameraPovHook(Context &context) noexcept {
  g_camera_pov.store(nullptr, std::memory_order_release);
  context.camera_manager.store(0, std::memory_order_release);
  context.camera_manager_resolved.store(false, std::memory_order_release);
  context.camera_hook_ready.store(false, std::memory_order_release);
  // The anchor is deliberately left alone: releasing the hook happens on every pause now, and the
  // yaw was captured from wherever the game camera happened to look, so dropping it here would make
  // the shot re-orient itself each time playback resumes.
  if (!HookReady(context.hook) || context.camera_pov_hook.id == 0) {
    context.camera_pov_hook = {};
    context.camera_pov_original = 0;
    context.camera_pov_target = 0;
    return true;
  }
  const auto status = context.hook->release(context.hook->user, context.camera_pov_hook);
  if (status.code != ANOMALY_STATUS_V1_OK)
    return false;
  context.camera_pov_hook = {};
  context.camera_pov_original = 0;
  context.camera_pov_target = 0;
  return true;
}

bool EnsureCameraPovHook(Context &context) noexcept {
  if (!HookReady(context.hook) ||
      !context.camera_manager_resolved.load(std::memory_order_acquire))
    return false;
  const std::uintptr_t target = context.camera_pov_resolved_target;
  if (target == 0)
    return false;
  if (context.camera_pov_hook.id != 0 && context.camera_pov_target == target &&
      context.camera_pov_original != 0)
    return true;
  if (context.camera_pov_hook.id != 0 && !ReleaseCameraPovHook(context))
    return false;
  context.camera_pov_target = target;
  g_camera_pov.store(&context, std::memory_order_release);
  AnomalyHookRequestV1 request{sizeof(request)};
  request.kind = ANOMALY_HOOK_V1_FUNCTION;
  request.target = target;
  request.detour = reinterpret_cast<void *>(&CameraPovDetour);
  request.label = anomaly::sdk::StringView("better-pose-camera-pov");
  std::uintptr_t original{};
  AnomalyGenerationHandleV1 handle{};
  const auto status =
      context.hook->create(context.hook->user, &request, &original, &handle);
  if (status.code != ANOMALY_STATUS_V1_OK || handle.id == 0 || original == 0) {
    g_camera_pov.store(nullptr, std::memory_order_release);
    context.camera_pov_hook = {};
    context.camera_pov_original = 0;
    context.camera_pov_target = 0;
    return false;
  }
  context.camera_pov_hook = handle;
  context.camera_pov_original = original;
  return true;
}

void LogCameraDrive(Context &context, const bool follow_mode, const double location[3],
                    const double rotation[3], const double unit_cm,
                    const double model_distance_cm, const double aim_cm, const double feet[3],
                    const double height_cm, const double applied[3],
                    const double game_location[3], const double game_rotation[3],
                    const double landed_cm) noexcept;

// MMD (x, y, z) -> the rig's local axes, the mapping the motion retarget uses. The model faces
// its own -Z, which lands on local +Y, so local +Y is "the way the character faces".
void MmdToLocal(const double source[3], double local[3]) noexcept {
  local[0] = source[0];
  local[1] = -source[2];
  local[2] = source[1];
}

// The character's feet and the middle of their bounding box, from the mesh's world bounds.
bool CharacterBounds(Context &context, double feet[3], double centre[3]) noexcept {
  const std::uintptr_t mesh = context.runtime.mesh;
  std::uintptr_t origin_address{};
  std::uintptr_t extent_address{};
  double origin[3]{};
  double extent[3]{};
  if (mesh == 0 ||
      !AddAddress(mesh, kCharacterBoundsOriginOffset, origin_address) ||
      !AddAddress(mesh, kCharacterBoundsExtentOffset, extent_address) ||
      !Read(context, origin_address, origin) || !Read(context, extent_address, extent))
    return false;
  // A character-sized box, so a stale offset cannot quietly place the camera at the origin.
  if (!(extent[2] > 1.0 && extent[2] < 500.0))
    return false;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(origin[axis]))
      return false;
  }
  feet[0] = origin[0];
  feet[1] = origin[1];
  feet[2] = origin[2] - extent[2];
  centre[0] = origin[0];
  centre[1] = origin[1];
  centre[2] = origin[2];
  return true;
}

// Is a reading of the game's own camera one that could plausibly be that camera? Right after a
// reload, and in menus or loading screens, the view point holds placeholder values -- measured as
// x = 0 with pitch 0 while the character stood 11.5 km away. Only a reading taken near the
// character is allowed to anchor the shot; the attempt is retried until one arrives, and the
// distance test alone is enough, because a placeholder is nowhere near the character.
bool GameCameraReadingIsPlausible(const double location[3], const double centre[3]) noexcept {
  const double dx = centre[0] - location[0];
  const double dy = centre[1] - location[1];
  const double dz = centre[2] - location[2];
  const double range = std::sqrt(dx * dx + dy * dy + dz * dz);
  return range >= 50.0 && range <= 2500.0;
}

// The root offset the pose path applied arrives in this camera's own axes (the rig's, the mapping
// both converters bake): its x is the character's left, its y the way they walk, its z up, and it
// is used as it arrives. Turning it a half turn first was tried and reverted: it pointed the
// track shot's aim at a point mirrored through the character, which is a shot staring at scenery.
double CameraUnitCm(Context &context) noexcept {
  const double unit = context.mmd_unit_cm.load(std::memory_order_acquire);
  return unit > 1e-6 ? unit : kCameraFallbackUnitCm;
}

// The file's distances are scaled so the subject keeps the size it has in MMD. The two fields of
// view are not the same kind of number: the game's POV field of view is horizontal (UE) while an
// MMD camera's view angle is vertical -- the model is what has to fit in frame. Converting the
// game's to vertical first is what makes this a 16:9-correct ratio instead of a 16:9-too-close one.
// The aspect is the usual 16:9; an ultrawide display shifts this by its own ratio.
// The track's camera position in MMD axes: the keyed look-at point pulled back along the camera's
// own view axis by the keyed distance (the sign is the one measured across four camera VMDs).
bool SampleCameraTrack(Context &context, double position[3], double rotation[3],
                       double *distance, double *fov) noexcept {
  std::lock_guard<std::mutex> lock(context.camera_mutex);
  return context.camera.Sample(context.camera_frame.load(std::memory_order_acquire), position,
                               rotation, distance, fov);
}

// Follow mode: keep the character's own displacement in the middle of the frame from a fixed world
// direction, ignoring any camera file. The direction is the anchored one (see the anchor in
// CameraPovDetour): the way the game's camera looked at the character when the shot started, which
// is why the camera ends up behind them -- and it never turns with the model, only the model's
// travel is followed.
void DriveFollowCamera(Context &context, const double feet[3], const double centre[3],
                       const double game_location[3], const double game_rotation[3],
                       const std::uintptr_t base, const std::uintptr_t rotation_address,
                       double location[3], double rotation[3]) noexcept {
  double applied[3]{};
  for (int axis = 0; axis < 3; ++axis)
    applied[axis] = context.motion_applied_offset[axis].load(std::memory_order_acquire);
  const double yaw = context.camera_yaw.load(std::memory_order_acquire);
  const double cos_yaw = std::cos(yaw);
  const double sin_yaw = std::sin(yaw);
  // The level everything vertical is measured from: the ground contact captured when the shot was
  // anchored. With the vertical follow on, the camera rises and falls with the character at **half**
  // the gain -- the animated bounds' bottom is recomputed from the bones every frame, and passing
  // that straight into the camera height is what made the follow feel too sensitive. The aim below
  // still uses the character's true chest height, so damping the camera does not make them drift in
  // the frame.
  const bool vertical_follow =
      context.camera_follow_vertical.load(std::memory_order_acquire);
  const double anchor_ground =
      context.camera_anchor_ground.load(std::memory_order_acquire);
  const double camera_ground =
      vertical_follow ? anchor_ground + 0.5 * (feet[2] - anchor_ground) : anchor_ground;
  // Rz(yaw) * (0, 1) is the direction from the character towards the anchored camera position.
  const double follow_x = feet[0] + applied[0] * cos_yaw - applied[1] * sin_yaw;
  const double follow_y = feet[1] + applied[0] * sin_yaw + applied[1] * cos_yaw;
  const double follow_z = feet[2] + (vertical_follow ? applied[2] : 0.0);
  // How far the game's own camera stands from the character right now. That camera does collision
  // work (it pulls in rather than clipping through a wall), so the follow distance never exceeds
  // it: a slider asking for more than the game camera itself will take is what put the shot inside
  // the scenery. The anchored direction stays ours, so the mouse still does not turn this camera.
  double game_distance = 0.0;
  {
    const double dx = follow_x - game_location[0];
    const double dy = follow_y - game_location[1];
    const double dz = follow_z - game_location[2];
    game_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  double distance = context.camera_follow_distance_cm.load(std::memory_order_relaxed);
  if (game_distance > 60.0 && game_distance < distance)
    distance = game_distance;
  const double height = context.camera_follow_height_cm.load(std::memory_order_relaxed);
  location[0] = follow_x - sin_yaw * distance;
  location[1] = follow_y + cos_yaw * distance;
  // The aim rides the character's jump (which keeps them vertically centred); the camera itself
  // only moves up and down with them, at half gain, when the vertical follow is on.
  location[2] = camera_ground + height;
  const double aim_z = follow_z + (centre[2] - feet[2]);
  const double dx = follow_x - location[0];
  const double dy = follow_y - location[1];
  const double planar = std::sqrt(dx * dx + dy * dy);
  if (planar <= 1.0)
    return;
  const double aim_dz = aim_z - location[2];
  rotation[0] = std::atan2(aim_dz, planar) * kCameraRadiansToDegrees;
  rotation[1] = std::atan2(dy, dx) * kCameraRadiansToDegrees;
  rotation[2] = 0.0;
  // The file's lens is not touched: this mode has no camera file to take one from.
  {
    auto *out_location = reinterpret_cast<double *>(base);
    auto *out_rotation = reinterpret_cast<double *>(rotation_address);
    for (int axis = 0; axis < 3; ++axis) {
      out_location[axis] = location[axis];
      out_rotation[axis] = rotation[axis];
    }
    const double landed_cm = std::sqrt((out_location[0] - location[0]) *
                                           (out_location[0] - location[0]) +
                                       (out_location[1] - location[1]) *
                                           (out_location[1] - location[1]) +
                                       (out_location[2] - location[2]) *
                                           (out_location[2] - location[2]));
    LogCameraDrive(context, true, location, rotation, CameraUnitCm(context),
                   std::sqrt(dx * dx + dy * dy + aim_dz * aim_dz), aim_z - feet[2], feet,
                   2.0 * (centre[2] - feet[2]), applied, game_location, game_rotation,
                   landed_cm);
  }
}

// The track shot: the file's own camera placement relative to the character, with the walk the file
// assumes replaced by the walk actually applied, aimed the way the file aims.
void DriveCameraTrack(Context &context, const double feet[3], const double centre[3],
                      const double game_location[3], const double game_rotation[3],
                      const double file_position[3], const double file_rotation[3],
                      const double file_distance, const double file_fov,
                      const std::uintptr_t base, const std::uintptr_t rotation_address,
                      const std::uintptr_t fov_address, double location[3],
                      double rotation[3]) noexcept {
  double right[3]{};
  double up[3]{};
  double forward[3]{};
  MmdCameraBasis(file_rotation, right, up, forward);
  double camera_mmd[3]{};
  for (int axis = 0; axis < 3; ++axis)
    camera_mmd[axis] = file_position[axis] + forward[axis] * file_distance;
  double local[3]{};
  MmdToLocal(camera_mmd, local);
  // MMD's +X is the model's left while the local frame's +X is its right, so the lateral
  // axis needs the mirror a rotation cannot give. Mirroring X leaves the front/back
  // behaviour (the Y component) exactly as it is.
  local[0] = -local[0];
  // MMD units become centimetres and nothing else: the file's own distance, height and
  // angles are used as authored. The lens is matched further down instead of the
  // distances being stretched to fit the game's.
  const double unit = CameraUnitCm(context);
  const double yaw = context.camera_yaw.load(std::memory_order_acquire);
  const double cos_yaw = std::cos(yaw);
  const double sin_yaw = std::sin(yaw);
  const double x = local[0] * unit;
  const double y = local[1] * unit;
  // The file's placement is relative to the world its model walks through, so the walk it
  // assumes is replaced by the walk actually applied: with planar motion locked the model
  // stays put and the camera must not sail past it. Both references are zero when no motion
  // is loaded, which leaves the file's placement alone.
  double applied[3]{};
  double authored[3]{};
  for (int axis = 0; axis < 3; ++axis) {
    applied[axis] = context.motion_applied_offset[axis].load(std::memory_order_acquire);
    authored[axis] = context.motion_authored_offset[axis].load(std::memory_order_acquire);
  }
  const double placed_x = x - authored[0] + applied[0];
  const double placed_y = y - authored[1] + applied[1];
  const double placed_z = local[2] * unit - authored[2] + applied[2];
  location[0] = feet[0] + placed_x * cos_yaw - placed_y * sin_yaw;
  location[1] = feet[1] + placed_x * sin_yaw + placed_y * cos_yaw;
  location[2] = feet[2] + placed_z;
  // MMD aims at its keyed look-at point, and that point carries the author's composition:
  // its height decides whether a shot reads level or from above. It is *not* always on the
  // model though -- at f0 it sits 1.24 m in front of it, and several metres off at other
  // keys -- so the aim takes the author's height and follows the model horizontally.
  double target[3]{};
  MmdToLocal(file_position, target);
  const double aim_x = feet[0] + applied[0] * cos_yaw - applied[1] * sin_yaw - location[0];
  const double aim_y = feet[1] + applied[0] * sin_yaw + applied[1] * cos_yaw - location[1];
  const double aim_z = feet[2] + target[2] * unit - authored[2] + applied[2] - location[2];
  const double planar = std::sqrt(aim_x * aim_x + aim_y * aim_y);
  if (planar > 1.0) {
    rotation[0] = std::atan2(aim_z, planar) * kCameraRadiansToDegrees;
    rotation[1] = std::atan2(aim_y, aim_x) * kCameraRadiansToDegrees;
    rotation[2] = 0.0;
  }
  // The view is built from this struct, its field of view included, so the file's lens is
  // written here: an MMD view angle is vertical and the engine's is horizontal, which is
  // the only place the aspect ratio enters.
  float lens = 0.0F;
  if (file_fov > 1.0 && file_fov < 179.0) {
    const double half = std::tan(file_fov * kCameraDegreesToRadians * 0.5) * (16.0 / 9.0);
    lens = static_cast<float>(2.0 * std::atan(half) * kCameraRadiansToDegrees);
  }
  if (lens <= 1.0F || Write(context, fov_address, lens)) {
    auto *out_location = reinterpret_cast<double *>(base);
    auto *out_rotation = reinterpret_cast<double *>(rotation_address);
    for (int axis = 0; axis < 3; ++axis) {
      out_location[axis] = location[axis];
      out_rotation[axis] = rotation[axis];
    }
    const double landed_cm = std::sqrt((out_location[0] - location[0]) *
                                           (out_location[0] - location[0]) +
                                       (out_location[1] - location[1]) *
                                           (out_location[1] - location[1]) +
                                       (out_location[2] - location[2]) *
                                           (out_location[2] - location[2]));
    LogCameraDrive(context, false, location, rotation, unit,
                   std::sqrt(placed_x * placed_x + placed_y * placed_y + placed_z * placed_z),
                   aim_z + location[2] - feet[2], feet, 2.0 * (centre[2] - feet[2]), applied,
                   game_location, game_rotation, landed_cm);
  }
}

// Rewrite the POV the view is built from. Patching the game's struct in place and returning its
// pointer unchanged is deliberate -- handing back a substitute buffer is what crashed the game once.
void *ANOMALY_CALL CameraPovDetour(void *self, void *first, void *second) noexcept {
  Context *context = g_camera_pov.load(std::memory_order_acquire);
  AnomalyGenerationHandleV1 lease{};
  bool leased = false;
  CameraPovFn original = nullptr;
  try {
    if (context != nullptr && context->camera_pov_hook.id != 0 && HookReady(context->hook)) {
      leased = context->hook
                   ->begin_callback(context->hook->user, context->camera_pov_hook, &lease)
                   .code == ANOMALY_STATUS_V1_OK;
      original = reinterpret_cast<CameraPovFn>(context->camera_pov_original);
    }
  } catch (...) {
    original = context == nullptr
                   ? nullptr
                   : reinterpret_cast<CameraPovFn>(context->camera_pov_original);
  }

  void *pov = nullptr;
  try {
    if (original != nullptr)
      pov = original(self, first, second);
  } catch (...) {
  }

  try {
    const bool follow_mode =
        context != nullptr && context->camera_follow.load(std::memory_order_acquire);
    const bool track_mode =
        context != nullptr && context->camera_enabled.load(std::memory_order_acquire) &&
        context->camera_loaded.load(std::memory_order_acquire);
    // The getter's two out-parameters are the camera the view is built from: a location and a
    // rotation, three doubles each -- the free-camera plugin hooks this same function and reads and
    // writes the same two, which is why this plugin now drives them instead of the POV member the
    // accessor returns. Writes to that member were measured to be reverted inside the same call,
    // with the view left under the game's own (mouse-rotatable) camera.
    auto *out_location = static_cast<double *>(first);
    auto *out_rotation = static_cast<double *>(second);
    if (context != nullptr && (follow_mode || track_mode) && out_location != nullptr &&
        out_rotation != nullptr) {
      const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(out_location);
      const std::uintptr_t rotation_address = reinterpret_cast<std::uintptr_t>(out_rotation);
      std::uintptr_t fov_address{};
      // The lens has no out-parameter here, so it still goes into the accessor's struct.
      if (AddAddress(context->camera_pov_struct, kCameraPovFovOffset, fov_address)) {
        double location[3] = {out_location[0], out_location[1], out_location[2]};
        double rotation[3] = {out_rotation[0], out_rotation[1], out_rotation[2]};
        bool read_ok = std::isfinite(location[0]) && std::isfinite(location[1]) &&
                       std::isfinite(location[2]) && std::isfinite(rotation[0]) &&
                       std::isfinite(rotation[1]) && std::isfinite(rotation[2]);
        if (read_ok) {
          bool usable = std::isfinite(location[0]) && std::isfinite(location[1]) &&
                        std::isfinite(location[2]) && std::isfinite(rotation[0]) &&
                        std::isfinite(rotation[1]) && std::isfinite(rotation[2]) &&
                        std::abs(rotation[0]) <= 90.0;
          double file_position[3]{};
          double file_rotation[3]{};
          double file_distance = 0.0;
          double file_fov = 0.0;
          double feet[3]{};
          double centre[3]{};
          // The game's own camera, as it was before this detour replaces it: the follow camera is
          // anchored to it and clamps its distance to it, and the log's pair of positions is what
          // tells "our shot is somewhere else" apart from "the character is not where we think".
          const double game_location[3] = {location[0], location[1], location[2]};
          const double game_rotation[3] = {rotation[0], rotation[1], rotation[2]};
          // Follow mode never reads the track, whatever file happens to be loaded.
          const bool sampled =
              track_mode && SampleCameraTrack(*context, file_position, file_rotation,
                                              &file_distance, &file_fov);
          if (usable && (follow_mode || sampled) &&
              CharacterBounds(*context, feet, centre)) {
            // The shot's angle is captured once, from the direction the game's own camera looked at
            // the character: that direction is the way the character faces. The local frame needs a
            // half turn before it is rotated onto that facing -- +Z_mmd (the way an MMD model faces)
            // lands on local -Y, not +Y, which is what put every shot behind the character.
            // Only a reading that could be a real third-person camera is allowed to anchor the
            // shot, and the attempt is retried every frame until one arrives: anchoring on the
            // placeholder view point a reload leaves behind is what aimed the shot at a wall.
            if (!context->camera_anchored.load(std::memory_order_acquire)) {
              const bool plausible =
                  GameCameraReadingIsPlausible(game_location, centre) ||
                  ++context->camera_anchor_wait > 90;
              const double dx = centre[0] - location[0];
              const double dy = centre[1] - location[1];
              if (plausible && std::sqrt(dx * dx + dy * dy) > 1.0) {
                context->camera_yaw.store(std::atan2(dy, dx) + 1.5707963267948966,
                                          std::memory_order_release);
                context->camera_anchor_ground.store(feet[2], std::memory_order_release);
                context->camera_anchored.store(true, std::memory_order_release);
              }
            }
            if (context->camera_anchored.load(std::memory_order_acquire)) {
              if (follow_mode)
                DriveFollowCamera(*context, feet, centre, game_location, game_rotation, base,
                                  rotation_address, location, rotation);
              else
                DriveCameraTrack(*context, feet, centre, game_location, game_rotation,
                                 file_position, file_rotation, file_distance, file_fov, base,
                                 rotation_address, fov_address, location, rotation);
            }
          }
        }
      }
    }
  } catch (...) {
  }

  if (leased && context != nullptr && HookReady(context->hook))
    static_cast<void>(context->hook->end_callback(context->hook->user, lease));
  return pov;
}

// One log line per second while driving: the frame and the camera that came out. Enough to tell
// "the track is not advancing" from "the write never reaches the view" without another round of
// guessing, plus the character's own numbers for checking the shot's framing.
void LogCameraDrive(Context &context, const bool follow_mode, const double location[3],
                    const double rotation[3], const double unit_cm,
                    const double model_distance_cm, const double aim_cm, const double feet[3],
                    const double height_cm, const double applied[3],
                    const double game_location[3], const double game_rotation[3],
                    const double landed_cm) noexcept {
  const double clock = context.camera_log_clock.load(std::memory_order_acquire);
  const double previous = context.camera_logged_second.load(std::memory_order_acquire);
  if (previous >= 0.0 && clock - previous < 1.0)
    return;
  context.camera_logged_second.store(clock, std::memory_order_release);
  const double seconds = context.camera_frame.load(std::memory_order_relaxed) / 30.0;
  char line[576]{};
  std::snprintf(line, sizeof(line),
                "betterpose camera drive: %s f%.0f, camera (%.1f, %.1f, %.1f) / "
                "(%.1f, %.1f, %.1f), game camera (%.1f, %.1f, %.1f) / (%.1f, %.1f, %.1f), "
                "readback off %.2f cm, unit %.2f cm/u, model %.0f cm away, aim %.0f cm up, "
                "feet (%.1f, %.1f, %.1f), applied (%.1f, %.1f, %.1f), lock %d root %d, "
                "h %.0f cm",
                follow_mode ? "follow" : "track", seconds * 30.0, location[0], location[1],
                location[2], rotation[0], rotation[1], rotation[2], game_location[0],
                game_location[1], game_location[2], game_rotation[0], game_rotation[1],
                game_rotation[2], landed_cm, unit_cm, model_distance_cm, aim_cm, feet[0],
                feet[1], feet[2], applied[0], applied[1], applied[2],
                context.motion_lock_planar.load(std::memory_order_relaxed) ? 1 : 0,
                context.motion_apply_root.load(std::memory_order_relaxed) ? 1 : 0, height_cm);
  LogDiagnostic(context, line);
}

// Where the camera should look: the character's chest, from the mesh's world bounds (the box is
// centred on them, so the chest sits above its origin). False when the character is not readable
// -- loading screens and menus -- in which case the track's own rotation is left in place.
// The accessor returns a pointer to the POV the view is built from. While driving, that POV is
// rewritten in place: the same edit the getter's output would have received, made where nothing
// can run between this call and the view build to undo it, and with the pointer itself left
// alone so the rest of the struct keeps whatever the engine put there.
bool ResolveGObjects(Context &context) noexcept {
  std::uintptr_t instruction{};
  std::int32_t displacement{};
  if (!ResolveSignature(context, kGObjectsPattern, instruction) ||
      !Read(context, instruction + kGObjectsResolveOffset, displacement))
    return false;
  const auto resolved = static_cast<std::intptr_t>(instruction) +
                        kGObjectsInstructionSize + displacement +
                        kGObjectsAddend;
  if (resolved <= 0)
    return false;
  context.g_objects_address = static_cast<std::uintptr_t>(resolved);
  return true;
}

bool RefreshObjectRegistry(Context &context) noexcept {
  if (context.object_registry.items != 0)
    return true;
  if (context.g_objects_address == 0 && !ResolveGObjects(context))
    return false;
  ObjectRegistry next{};
  std::uint32_t count{};
  std::uint32_t max_count{};
  std::uint32_t max_chunks{};
  std::uint32_t num_chunks{};
  if (!ReadPointerAt(context, context.g_objects_address, kObjectItemsOffset,
                     next.items) ||
      !Read(context, context.g_objects_address + kObjectCountOffset, count) ||
      !Read(context, context.g_objects_address + kObjectMaxCountOffset,
            max_count) ||
      !Read(context, context.g_objects_address + kObjectMaxChunksOffset,
            max_chunks) ||
      !Read(context, context.g_objects_address + kObjectNumChunksOffset,
            num_chunks) ||
      count == 0 || max_count < count || max_chunks < num_chunks ||
      num_chunks == 0 || num_chunks > 4096)
    return false;
  next.count = count;
  next.max_count = max_count;
  next.max_chunks = max_chunks;
  next.num_chunks = num_chunks;
  context.object_registry = next;
  return true;
}

bool FindObjectAddressByPath(Context &context, const std::string_view path,
                             std::uintptr_t &object,
                             std::string *detail = nullptr) noexcept {
  object = 0;
  if (!ObjectsReady(context.objects) || path.empty()) {
    if (detail != nullptr) *detail = "object service or path unavailable";
    return false;
  }
  if (!RefreshObjectRegistry(context)) {
    if (detail != nullptr) *detail = "GObjects registry unavailable";
    return false;
  }
  AnomalyGenerationHandleV1 handle{};
  auto find = [&](const std::string_view candidate) {
    handle = {};
    return context.objects
               ->find_exact(context.objects->user,
                            anomaly::sdk::StringView(candidate), &handle)
               .code == ANOMALY_STATUS_V1_OK &&
           handle.id != 0;
  };
  if (!find(path)) {
    std::string alternate(path);
    const auto separator = alternate.rfind('.');
    if (separator == std::string::npos) {
      if (detail != nullptr) *detail = "exact object lookup failed";
      return false;
    }
    alternate[separator] = ':';
    if (!find(alternate)) {
      if (detail != nullptr) *detail = "exact object lookup failed";
      return false;
    }
  }
  if (handle.id == 0) {
    if (detail != nullptr) *detail = "exact object handle is invalid";
    return false;
  }
  const std::uint32_t index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
  if (index >= context.object_registry.count) {
    if (detail != nullptr) *detail = "exact object index is out of range";
    return false;
  }
  const std::uint32_t chunk_index = index / kObjectChunkSize;
  const std::uint32_t within_chunk = index % kObjectChunkSize;
  if (chunk_index >= context.object_registry.num_chunks) {
    if (detail != nullptr) *detail = "exact object chunk is out of range";
    return false;
  }
  std::uintptr_t chunk{};
  if (!ReadPointerAt(context, context.object_registry.items,
                     static_cast<std::uint32_t>(chunk_index * sizeof(void *)),
                     chunk) ||
      !ReadPointerAt(context, chunk,
                     static_cast<std::uint32_t>(within_chunk) *
                         kObjectItemStride,
                     object)) {
    if (detail != nullptr) *detail = "exact object slot is unreadable";
    return false;
  }
  if (object == 0) {
    if (detail != nullptr) *detail = "exact object slot is null";
    return false;
  }
  return true;
}

std::string Hex(const std::uintptr_t value) noexcept;

bool ResolveName(Context &context, const std::uint32_t name_id,
                 std::string &value) noexcept {
  value.clear();
  if (!NamesReady(context.names) || name_id == 0)
    return false;
  std::array<char, 128> local{};
  std::size_t size = local.size();
  AnomalyStatusV1 status = context.names->resolve_utf8(
      context.names->user, name_id, local.data(), &size);
  if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= local.size()) {
    value.assign(local.data(), size - 1U);
    return true;
  }
  if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 ||
      size > 2048)
    return false;
  std::string buffer(size, '\0');
  status = context.names->resolve_utf8(context.names->user, name_id,
                                       buffer.data(), &size);
  if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > buffer.size())
    return false;
  buffer.resize(size - 1U);
  value = std::move(buffer);
  return true;
}

bool CallVirtualUFunction(Context &context, const std::uintptr_t object,
                          const std::string_view function_path,
                          const void *parameters,
                          const std::size_t parameter_size,
                          std::string &detail,
                          void *output) noexcept {
  detail.clear();
  if (object == 0 || function_path.empty() ||
      parameter_size > kMaximumUFunctionParameterBytes ||
      (parameter_size != 0 && parameters == nullptr)) {
    detail = "invalid object, path, or parameters";
    return false;
  }
  std::uintptr_t function{};
  if (!FindObjectAddressByPath(context, function_path, function, &detail)) {
    detail.insert(0, "find function failed: ");
    return false;
  }
  std::uintptr_t vtable{};
  if (!Read(context, object, vtable) || vtable == 0) {
    detail = "read object vtable failed";
    return false;
  }
  std::uintptr_t process_event{};
  if (!Read(context,
            vtable + static_cast<std::uint64_t>(kProcessEventVtableSlot) *
                        sizeof(void *),
            process_event) ||
      process_event == 0) {
    detail = "ProcessEvent vtable slot is unreadable";
    return false;
  }

  using ProcessEventFn = void(__fastcall *)(void *, void *, void *);
  std::array<std::uint8_t, kMaximumUFunctionParameterBytes> buffer{};
  if (parameter_size != 0)
    std::memcpy(buffer.data(), parameters, parameter_size);
  const auto invoke = reinterpret_cast<ProcessEventFn>(process_event);
  invoke(reinterpret_cast<void *>(object), reinterpret_cast<void *>(function),
         parameter_size != 0 ? buffer.data() : nullptr);
  if (output != nullptr && parameter_size != 0)
    std::memcpy(output, buffer.data(), parameter_size);
  detail = "ok function=" + Hex(function) + " process_event=" + Hex(process_event);
  return true;
}

bool GetBoneNameFName(Context &context, const std::uint32_t bone_index,
                      std::array<std::uint8_t, 8> &name) noexcept {
  name.fill(0);
  if (context.runtime.mesh == 0)
    return false;
  std::array<std::uint8_t, 12> parameters{};
  const std::int32_t index = static_cast<std::int32_t>(bone_index);
  std::memcpy(parameters.data(), &index, sizeof(index));
  std::array<std::uint8_t, 12> output{};
  std::string detail;
  if (!CallVirtualUFunction(context, context.runtime.mesh, kFunctionGetBoneNamePath,
                            parameters.data(), parameters.size(), detail,
                            output.data()))
    return false;
  std::memcpy(name.data(), output.data() + 4, name.size());
  return true;
}

bool GetParentBoneFName(Context &context,
                        const std::array<std::uint8_t, 8> &child,
                        std::array<std::uint8_t, 8> &parent) noexcept {
  parent.fill(0);
  if (context.runtime.mesh == 0)
    return false;
  std::array<std::uint8_t, 16> parameters{};
  std::memcpy(parameters.data(), child.data(), child.size());
  std::array<std::uint8_t, 16> output{};
  std::string detail;
  if (!CallVirtualUFunction(context, context.runtime.mesh,
                            kFunctionGetParentBonePath, parameters.data(),
                            parameters.size(), detail, output.data()))
    return false;
  std::memcpy(parent.data(), output.data() + 8, parent.size());
  return true;
}

bool GetBoneIndexFName(Context &context,
                       const std::array<std::uint8_t, 8> &name,
                       std::int32_t &index) noexcept {
  index = -1;
  if (context.runtime.mesh == 0)
    return false;
  std::array<std::uint8_t, 12> parameters{};
  std::memcpy(parameters.data(), name.data(), name.size());
  std::array<std::uint8_t, 12> output{};
  std::string detail;
  if (!CallVirtualUFunction(context, context.runtime.mesh,
                            kFunctionGetBoneIndexPath, parameters.data(),
                            parameters.size(), detail, output.data()))
    return false;
  std::memcpy(&index, output.data() + 8, sizeof(index));
  return true;
}

void RefreshBoneHierarchy(Context &context) noexcept {
  const auto count = context.runtime.local_space_count;
  context.bone_parents.assign(count, -1);
  if (context.bone_names.size() != count)
    return;
  for (std::uint32_t index{}; index != count; ++index) {
    std::array<std::uint8_t, 8> child_fname{};
    if (!GetBoneNameFName(context, index, child_fname))
      continue;
    std::array<std::uint8_t, 8> parent_fname{};
    if (!GetParentBoneFName(context, child_fname, parent_fname))
      continue;
    std::uint32_t parent_name_id{};
    std::memcpy(&parent_name_id, parent_fname.data(), sizeof(parent_name_id));
    std::string parent_name;
    if (!ResolveName(context, parent_name_id, parent_name))
      continue;
    const auto it = std::find(context.bone_names.begin(),
                              context.bone_names.end(), parent_name);
    if (it != context.bone_names.end())
      context.bone_parents[index] =
          static_cast<std::int32_t>(it - context.bone_names.begin());
  }
}

void RefreshBoneHierarchyDirect(Context &context) noexcept {
  const auto count = context.runtime.local_space_count;
  if (count == 0)
    return;
  if (context.bone_parents_ready &&
      context.bone_parents_mesh == context.runtime.mesh &&
      context.bone_parents_count == count)
    return;
  context.bone_parents.assign(count, -1);
  for (std::uint32_t index{}; index != count; ++index) {
    std::array<std::uint8_t, 8> child_fname{};
    if (!GetBoneNameFName(context, index, child_fname))
      continue;
    std::array<std::uint8_t, 8> parent_fname{};
    if (!GetParentBoneFName(context, child_fname, parent_fname))
      continue;
    const std::uint64_t parent_name =
        *reinterpret_cast<const std::uint64_t *>(parent_fname.data());
    if (parent_name == 0)
      continue;
    std::int32_t parent_index = -1;
    if (!GetBoneIndexFName(context, parent_fname, parent_index))
      continue;
    if (parent_index >= 0 &&
        static_cast<std::uint32_t>(parent_index) < count)
      context.bone_parents[index] = parent_index;
  }
  context.bone_parents_mesh = context.runtime.mesh;
  context.bone_parents_count = count;
  context.bone_parents_ready = true;
}

bool ApplyPoseByName(Context &context, const std::uint32_t bone,
                     const std::array<double, 3> &translation,
                     std::string &detail) noexcept {
  detail.clear();
  if (context.runtime.mesh == 0) {
    detail = "local mesh is unavailable";
    return false;
  }
  std::array<std::uint8_t, 8> name{};
  if (!GetBoneNameFName(context, bone, name)) {
    detail = "GetBoneName failed";
    return false;
  }
  std::array<std::uint8_t, 0x28> parameters{};
  std::memcpy(parameters.data(), name.data(), name.size());
  std::memcpy(parameters.data() + 0x08, translation.data(),
              sizeof(double) * 3);
  const std::uint8_t component_space = 1;
  parameters[0x20] = component_space;
  return CallVirtualUFunction(context, context.runtime.mesh,
                              kFunctionSetBoneLocationByNamePath,
                              parameters.data(), parameters.size(), detail);
}

struct Rotator3d {
  double pitch{};
  double yaw{};
  double roll{};
};

bool ApplyBoneRotationByName(Context &context, const std::uint32_t bone,
                             const double pitch, const double yaw,
                             const double roll, std::string &detail) noexcept {
  detail.clear();
  if (context.runtime.mesh == 0) {
    detail = "local mesh is unavailable";
    return false;
  }
  std::array<std::uint8_t, 8> name{};
  if (!GetBoneNameFName(context, bone, name)) {
    detail = "GetBoneName failed";
    return false;
  }
  std::array<std::uint8_t, 0x28> parameters{};
  std::memcpy(parameters.data(), name.data(), name.size());
  const Rotator3d rotation{pitch, yaw, roll};
  std::memcpy(parameters.data() + 0x08, &rotation, sizeof(rotation));
  const std::uint8_t component_space = 1;
  parameters[0x20] = component_space;
  return CallVirtualUFunction(context, context.runtime.mesh,
                              kFunctionSetBoneRotationByNamePath,
                              parameters.data(), parameters.size(), detail);
}

void ApplyPoseOverridesViaUFunction(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!context.pose_override_enabled.load(std::memory_order_acquire) ||
      state.mesh == 0)
    return;
  std::vector<std::array<double, 3>> angles;
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    if (context.bone_angles.size() < state.local_space_count)
      return;
    angles.assign(context.bone_angles.begin(),
                  context.bone_angles.begin() + state.local_space_count);
  }
  for (std::uint32_t bone{}; bone != state.local_space_count; ++bone) {
    const auto &angle = angles[bone];
    if (angle[0] == 0.0 && angle[1] == 0.0 && angle[2] == 0.0)
      continue;
    std::string detail;
    static_cast<void>(ApplyBoneRotationByName(
        context, bone, angle[0], angle[1], angle[2], detail));
  }
  static_cast<void>(ForcePoseMeshObjectUpdate(context));
}

bool CapturePoseBase(Context &context) noexcept {
  const auto mesh = context.runtime.mesh;
  const auto data = context.runtime.local_space_data;
  const auto count = context.runtime.local_space_count;
  if (mesh == 0 || data == 0 || count == 0)
    return false;
  std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
  if (context.pose_base_ready && context.pose_base_mesh == mesh &&
      context.pose_base_locals.size() == count)
    return true;
  std::vector<std::array<double, 12>> base(count);
  for (std::uint32_t bone{}; bone != count; ++bone) {
    std::uintptr_t address{};
    if (!AddAddress(data, static_cast<std::uint64_t>(bone) * kTransformSize,
                    address) ||
        !Read(context, address, base[bone]))
      return false;
  }
  context.pose_base_locals = std::move(base);
  context.pose_base_mesh = mesh;
  context.pose_base_ready = true;
  return true;
}

void ApplyPoseOverridesDirect(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (!context.pose_override_enabled.load(std::memory_order_acquire) ||
      state.mesh == 0 || state.local_space_data == 0 ||
      state.local_space_count == 0 || state.component_space_data == 0 ||
      state.component_space_count != state.local_space_count)
    return;
  if (!CapturePoseBase(context))
    return;
  const std::uint32_t count = state.local_space_count;
  std::vector<std::array<double, 12>> base_locals;
  std::vector<std::array<double, 3>> angles;
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    if (context.pose_base_locals.size() != count ||
        context.bone_angles.size() < count)
      return;
    base_locals = context.pose_base_locals;
    angles.assign(context.bone_angles.begin(),
                  context.bone_angles.begin() + count);
  }

  std::array<double, 3> root_offset{
      context.requested_root_offset[0].load(std::memory_order_acquire),
      context.requested_root_offset[1].load(std::memory_order_acquire),
      context.requested_root_offset[2].load(std::memory_order_acquire)};
  const bool has_root_offset =
      root_offset[0] != 0.0 || root_offset[1] != 0.0 || root_offset[2] != 0.0;

  std::vector<Transformd> locals(count);
  bool any_override = false;
  for (std::uint32_t bone{}; bone != count; ++bone) {
    const auto &raw = base_locals[bone];
    locals[bone].rotation = Quatd{raw[0], raw[1], raw[2], raw[3]};
    locals[bone].translation = Vec3d{raw[4], raw[5], raw[6]};
    locals[bone].scale = Vec3d{raw[8], raw[9], raw[10]};
    const auto &angle = angles[bone];
    if (angle[0] == 0.0 && angle[1] == 0.0 && angle[2] == 0.0)
      continue;
    any_override = true;
    const Quatd offset =
        RotatorToQuat(angle[0], angle[1], angle[2]);
    locals[bone].rotation = QuatMultiply(offset, locals[bone].rotation);
  }
  if (!any_override && !has_root_offset)
    return;

  if (context.bone_parents.size() != count)
    return;
  std::vector<Transformd> components(count);
  std::vector<std::uint8_t> marks(count, 0);
  for (std::uint32_t bone{}; bone != count; ++bone)
    static_cast<void>(ComputeBoneComponent(bone, locals, context.bone_parents,
                                           components, marks));

  std::vector<PackedTransform> packed(count);
  for (std::uint32_t bone{}; bone != count; ++bone)
    PackTransform(components[bone], packed[bone]);
  for (std::uint32_t bone{}; bone != count; ++bone) {
    packed[bone].translation[0] += root_offset[0];
    packed[bone].translation[1] += root_offset[1];
    packed[bone].translation[2] += root_offset[2];
  }
  // Both arrays are component-space buffers selected by the engine's read index.
  // The legacy state member bone_space_data names buffer 0, not local transforms.
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(packed.data());
  const auto byte_count = packed.size() * sizeof(PackedTransform);
  static_cast<void>(WriteBytes(context, state.component_space_data, bytes,
                               byte_count));
  static_cast<void>(WriteBytes(context, state.bone_space_data, bytes,
                               byte_count));
  WriteExtraMeshes(context, packed);
  static_cast<void>(ForcePoseMeshObjectUpdate(context));
}

// ---------------------------------------------------------------------------
// MMD motion playback
//
// The tracks come from the offline converter (tools/mmd2bip.py): per frame, an
// absolute local rotation for every driven bone plus the root translation in
// component space. Bones the file does not mention keep the captured base pose,
// exactly like the manual pose override does.
// ---------------------------------------------------------------------------

struct MotionSample {
  std::vector<std::uint32_t> indices;
  std::vector<std::array<double, 4>> rotations;
  std::array<double, 3> root_translation{0.0, 0.0, 0.0};
  std::uint32_t root_index{};
  bool has_root{};
  std::vector<std::uint32_t> offset_indices;
  std::vector<std::array<double, 3>> offsets;
};

double MotionDuration(const Context::MotionTrack &motion) noexcept {
  if (motion.frame_count <= 1)
    return 0.0;
  const double fps = motion.fps > 0.0 ? motion.fps : 30.0;
  return static_cast<double>(motion.frame_count - 1) / fps;
}

// Maps the file's bone names onto this skeleton's bone indices. Needs the bone
// name table, which can only be read on the game thread, so this runs from
// Update rather than from the loading task.
bool ResolveMotionIndices(Context &context) noexcept {
  if (!context.motion_loaded.load(std::memory_order_acquire))
    return false;
  std::lock_guard<std::mutex> lock(context.motion_mutex);
  auto &motion = context.motion;
  if (motion.indices_ready)
    return true;
  if (motion.bone_names.empty())
    return false;
  if (context.bone_names.empty())
    MaybeRefreshBoneNames(context);
  if (context.bone_names.empty())
    return false;
  std::vector<std::uint32_t> indices(motion.bone_names.size(),
                                     (std::numeric_limits<std::uint32_t>::max)());
  std::uint32_t resolved{};
  for (std::size_t track{}; track != motion.bone_names.size(); ++track) {
    const std::string &wanted = motion.bone_names[track];
    for (std::size_t bone{}; bone != context.bone_names.size(); ++bone) {
      if (context.bone_names[bone] == wanted) {
        indices[track] = static_cast<std::uint32_t>(bone);
        ++resolved;
        break;
      }
    }
  }
  if (resolved == 0)
    return false;
  // How many of the file's tracks this skeleton actually has, and which names
  // found nothing. "Fingers do not move" is only worth investigating once the
  // finger tracks are known to be resolved at all.
  {
    std::string message = "betterpose motion resolved " +
                          std::to_string(resolved) + "/" +
                          std::to_string(motion.bone_names.size());
    std::size_t shown{};
    for (std::size_t track{};
         track != motion.bone_names.size() && shown != 8; ++track) {
      if (indices[track] != (std::numeric_limits<std::uint32_t>::max)())
        continue;
      message += " ";
      message += motion.bone_names[track];
      ++shown;
    }
    if (shown != 0)
      message += " missing";
    LogDiagnostic(context, message);
  }
  motion.bone_indices = std::move(indices);
  motion.root_bone_index = (std::numeric_limits<std::uint32_t>::max)();
  if (!motion.root_bone_name.empty()) {
    for (std::size_t bone{}; bone != context.bone_names.size(); ++bone) {
      if (context.bone_names[bone] == motion.root_bone_name) {
        motion.root_bone_index = static_cast<std::uint32_t>(bone);
        break;
      }
    }
  }
  motion.offset_indices.assign(motion.offset_names.size(),
                               (std::numeric_limits<std::uint32_t>::max)());
  for (std::size_t track{}; track != motion.offset_names.size(); ++track) {
    for (std::size_t bone{}; bone != context.bone_names.size(); ++bone) {
      if (context.bone_names[bone] == motion.offset_names[track]) {
        motion.offset_indices[track] = static_cast<std::uint32_t>(bone);
        break;
      }
    }
  }
  motion.offsets_ready = motion.offsets_ready &&
                         motion.offset_indices.size() == motion.offset_names.size();
  motion.indices_ready = true;
  return true;
}

bool SampleMotion(Context &context, const double seconds,
                  MotionSample &out) noexcept {
  std::lock_guard<std::mutex> lock(context.motion_mutex);
  const auto &motion = context.motion;
  if (!motion.indices_ready || motion.bone_count == 0 || motion.frame_count == 0)
    return false;
  const double fps = motion.fps > 0.0 ? motion.fps : 30.0;
  const double duration = MotionDuration(motion);
  double time = seconds;
  if (time < 0.0)
    time = 0.0;
  if (time > duration)
    time = duration;
  const double position = time * fps;
  const auto frame0 = static_cast<std::uint32_t>(position);
  const auto frame1 = frame0 + 1 < motion.frame_count ? frame0 + 1 : frame0;
  const float blend = static_cast<float>(position - static_cast<double>(frame0));
  out.indices.resize(motion.bone_count);
  out.rotations.resize(motion.bone_count);
  for (std::size_t track{}; track != motion.bone_count; ++track) {
    out.indices[track] = motion.bone_indices[track];
    const auto offset0 =
        (static_cast<std::size_t>(frame0) * motion.bone_count + track) * 4;
    const auto offset1 =
        (static_cast<std::size_t>(frame1) * motion.bone_count + track) * 4;
    double dot{};
    for (int k{}; k != 4; ++k)
      dot += static_cast<double>(motion.rotations[offset0 + k]) *
             static_cast<double>(motion.rotations[offset1 + k]);
    const double sign = dot < 0.0 ? -1.0 : 1.0;
    std::array<double, 4> value{};
    double length{};
    for (int k{}; k != 4; ++k) {
      const double a = motion.rotations[offset0 + k];
      const double b = sign * motion.rotations[offset1 + k];
      value[k] = a + (b - a) * static_cast<double>(blend);
      length += value[k] * value[k];
    }
    length = std::sqrt(length);
    if (length > 1e-9)
      for (int k{}; k != 4; ++k)
        value[k] /= length;
    else
      value = {0.0, 0.0, 0.0, 1.0};
    out.rotations[track] = value;
  }
  out.has_root = motion.has_root &&
                 motion.root_bone_index !=
                     (std::numeric_limits<std::uint32_t>::max)() &&
                 motion.roots.size() >=
                     static_cast<std::size_t>(motion.frame_count) * 3;
  out.root_index = motion.root_bone_index;
  if (out.has_root) {
    for (int k{}; k != 3; ++k) {
      const double a = motion.roots[static_cast<std::size_t>(frame0) * 3 + k];
      const double b = motion.roots[static_cast<std::size_t>(frame1) * 3 + k];
      out.root_translation[k] = a + (b - a) * static_cast<double>(blend);
    }
  }
  const std::size_t offset_count = motion.offset_indices.size();
  if (motion.offsets_ready && offset_count != 0 &&
      motion.offsets.size() >= static_cast<std::size_t>(motion.frame_count) *
                                   offset_count * 3) {
    out.offset_indices.resize(offset_count);
    out.offsets.resize(offset_count);
    for (std::size_t track{}; track != offset_count; ++track) {
      out.offset_indices[track] = motion.offset_indices[track];
      for (int k{}; k != 3; ++k) {
        const double a = motion.offsets[
            (static_cast<std::size_t>(frame0) * offset_count + track) * 3 + k];
        const double b = motion.offsets[
            (static_cast<std::size_t>(frame1) * offset_count + track) * 3 + k];
        out.offsets[track][k] = a + (b - a) * static_cast<double>(blend);
      }
    }
  }
  return true;
}

// Leg length of the live character in centimetres, from the *segment* lengths the converter
// measures on the source side, so the ratio is this character's own centimetres per MMD unit.
// The calf's local translation is the thigh's length and the foot's is the shin's; the thigh's own
// translation is the hip's lateral offset from the pelvis (about 6.5 cm on this rig), not a
// segment, and both converters leave it out when they derive `unitScaleCmPerMmdUnit`
// (motion_builder.cpp:2120-2131, mmd2bip.py:1463-1471). Counting it made the runtime scale 8 %
// larger than the file's own: the log's camera heights read 9.233 cm per MMD unit while the
// motion JSON's `unitScaleCmPerMmdUnit` is 8.545, so every MMD-unit motion and the camera track
// were stretched by that factor. The asset's own reference pose is preferred: it is the pose the
// converter measured `mmdLegLength` against and it is readable before any motion has been applied,
// which is the state a camera file is driven in on its own.
double LiveLegLength(Context &context) noexcept {
  static constexpr const char *kChain[] = {"Bip001-L-Calf", "Bip001-L-Foot"};
  double total = 0.0;
  std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
  const std::vector<std::array<double, 12>> &source =
      context.ref_locals.size() == context.bone_names.size() && !context.ref_locals.empty()
          ? context.ref_locals
          : context.pose_base_locals;
  for (const auto *wanted : kChain) {
    for (std::size_t bone{}; bone != context.bone_names.size(); ++bone) {
      if (context.bone_names[bone] != wanted)
        continue;
      if (bone < source.size()) {
        const auto &local = source[bone];
        total += std::sqrt(local[4] * local[4] + local[5] * local[5] +
                           local[6] * local[6]);
      }
      break;
    }
  }
  return total;
}


void ApplyMotionPoseDirect(Context &context) noexcept {
  RuntimeState &state = context.runtime;
  if (state.mesh == 0 || state.local_space_data == 0 ||
      state.local_space_count == 0 || state.component_space_data == 0 ||
      state.component_space_count != state.local_space_count)
    return;
  MotionSample sample;
  if (!SampleMotion(context, context.motion_seconds.load(std::memory_order_acquire),
                    sample))
    return;
  if (!CapturePoseBase(context))
    return;
  // The reference pose is what undriven bones should sit at; read it once per
  // character. The mesh scan usually got there first, in which case this is free.
  if (context.ref_locals.empty() && !context.ref_pose_attempted) {
    context.ref_pose_attempted = true;
    if (!FindReferencePose(context)) {
      char status[160]{};
      std::snprintf(status, sizeof(status),
                    "betterpose motion baseline: captured pose (reference pose "
                    "unavailable: %s)",
                    context.ref_pose_status.c_str());
      LogDiagnostic(context, status);
    }
  }
  const std::uint32_t count = state.local_space_count;
  if (context.bone_parents.size() != count)
    return;
  std::vector<std::array<double, 12>> base_locals;
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    if (context.pose_base_locals.size() != count)
      return;
    base_locals = context.pose_base_locals;
  }
  // Bones the motion does not drive are the ones that decide whether a skirt hangs
  // naturally or stays flung: this rig's cloth and skirt bones hang off helper bones
  // (wq_*, VB*, hand/foot IK targets) that no MMD track ever covers, and their live
  // values differ from the reference pose by up to 173 degrees and 17 cm. Keeping the
  // captured pose therefore froze a skirt that happened to be mid-swing. The engine's
  // reference pose is the undeformed pose, so use it as the baseline whenever it has
  // been read; the captured pose stays the fallback.
  bool baseline_is_bind = false;
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    if (context.ref_locals.size() == count) {
      base_locals = context.ref_locals;
      baseline_is_bind = true;
    }
  }
  if (baseline_is_bind && !context.motion_baseline_logged.exchange(true)) {
    LogDiagnostic(context,
                  "betterpose motion baseline: engine reference pose "
                  "(undriven bones no longer inherit the captured pose)");
  }

  std::vector<Transformd> locals(count);
  for (std::uint32_t bone{}; bone != count; ++bone) {
    const auto &raw = base_locals[bone];
    locals[bone].rotation = Quatd{raw[0], raw[1], raw[2], raw[3]};
    locals[bone].translation = Vec3d{raw[4], raw[5], raw[6]};
    locals[bone].scale = Vec3d{raw[8], raw[9], raw[10]};
  }
  for (std::size_t track{}; track != sample.indices.size(); ++track) {
    const std::uint32_t bone = sample.indices[track];
    if (bone >= count)
      continue;
    const auto &q = sample.rotations[track];
    locals[bone].rotation = Quatd{q[0], q[1], q[2], q[3]};
  }
  for (std::size_t track{}; track != sample.offset_indices.size(); ++track) {
    const std::uint32_t bone = sample.offset_indices[track];
    if (bone >= count)
      continue;
    const auto &o = sample.offsets[track];
    locals[bone].translation = Vec3d{o[0], o[1], o[2]};
  }
  const double no_offset[3] = {0.0, 0.0, 0.0};
  PublishRootOffsets(context, no_offset, no_offset);
  if (sample.has_root && sample.root_index < count &&
      context.motion_apply_root.load(std::memory_order_acquire)) {
    const auto &track = context.motion;
    // MMD-unit offsets are relative to the character's own pose, scaled by *this*
    // character's leg length. The absolute centimetre form baked the exporting
    // character's height into the file and made a taller character play 24 cm too
    // low; keep reading it so old files still behave as before.
    if (track.roots_in_mmd_units && track.mmd_leg_length > 1e-6) {
      const double live_leg = LiveLegLength(context);
      const double scale =
          live_leg > 1e-6 ? live_leg / track.mmd_leg_length : 1.0;
      // The camera track travels in the same MMD units, so it needs the same ratio.
      context.mmd_unit_cm.store(scale, std::memory_order_release);
      const auto base = locals[sample.root_index].translation;
      // The emitted vector is already in this rig's axes (mmd y -> z), so the height is [2] and
      // the plane is [0]/[1]. "Lock planar motion" keeps the height and drops the two horizontal
      // components: the character plays in place and still bobs and jumps.
      const bool lock_planar = context.motion_lock_planar.load(std::memory_order_acquire);
      const double applied[3] = {lock_planar ? 0.0 : sample.root_translation[0] * scale,
                                 lock_planar ? 0.0 : sample.root_translation[1] * scale,
                                 sample.root_translation[2] * scale};
      const double authored[3] = {sample.root_translation[0] * scale,
                                  sample.root_translation[1] * scale,
                                  sample.root_translation[2] * scale};
      locals[sample.root_index].translation =
          Vec3d{base.x + applied[0], base.y + applied[1], base.z + applied[2]};
      PublishRootOffsets(context, applied, authored);
    } else {
      locals[sample.root_index].translation =
          Vec3d{sample.root_translation[0], sample.root_translation[1],
                sample.root_translation[2]};
      const double as_is[3] = {sample.root_translation[0], sample.root_translation[1],
                               sample.root_translation[2]};
      PublishRootOffsets(context, as_is, as_is);
    }
  }

  std::vector<Transformd> components(count);
  std::vector<std::uint8_t> marks(count, 0);
  for (std::uint32_t bone{}; bone != count; ++bone)
    static_cast<void>(ComputeBoneComponent(bone, locals, context.bone_parents,
                                           components, marks));
  std::vector<PackedTransform> packed(count);
  for (std::uint32_t bone{}; bone != count; ++bone)
    PackTransform(components[bone], packed[bone]);
  // Same as the manual pose path: 0x628 gets the component data, because that is
  // what this build actually renders from.
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(packed.data());
  const auto byte_count = packed.size() * sizeof(PackedTransform);
  static_cast<void>(WriteBytes(context, state.component_space_data, bytes,
                               byte_count));
  static_cast<void>(WriteBytes(context, state.bone_space_data, bytes,
                               byte_count));
  WriteExtraMeshes(context, packed);
  static_cast<void>(ForcePoseMeshObjectUpdate(context));
}

// Walks the GObjects registry a slice at a time (never more than a fraction of a
// tick) and collects every object whose class pointer equals our mesh's and
// whose bone array holds the same bone count. Strictly read-only.
void StepMeshScan(Context &context) noexcept {
  if (!context.mesh_scan_requested.load(std::memory_order_acquire))
    return;
  if (context.runtime.mesh == 0) {
    context.mesh_scan_requested.store(false, std::memory_order_release);
    context.mesh_scan_running = false;
    return;
  }
  if (!RefreshObjectRegistry(context)) {
    context.mesh_scan_requested.store(false, std::memory_order_release);
    context.mesh_scan_running = false;
    return;
  }
  if (!context.mesh_scan_running) {
    context.mesh_scan_running = true;
    context.mesh_scan_cursor = 0;
    context.mesh_scan_class = 0;
    context.mesh_scan_same_class = 0;
    context.mesh_scan_reference = {0.0, 0.0, 0.0};
    context.mesh_scan_candidates.clear();
    context.skeleton_meshes.clear();
    context.mesh_scan_mesh_assets = 0;
    context.mesh_scan_skinned = 0;
    context.mesh_scan_mesh_objects = 0;
    context.mesh_scan_bone_counts.clear();
    context.mesh_scan_world_logged = 0;
    context.mesh_scan_all_logged = 0;
    context.mesh_scan_functions_logged = 0;
    context.ref_locals.clear();
    if (!Read(context, context.runtime.mesh + kObjectClassOffset,
              context.mesh_scan_class))
      context.mesh_scan_class = 0;
    // Reference bone: pick one with a non-zero local translation so the compare
    // is actually discriminating (bone 5 is a spine joint in this rig).
    std::uint32_t reference = 0;
    {
      std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
      if (!context.pose_base_locals.empty()) {
        reference = static_cast<std::uint32_t>(
            (std::min)(context.pose_base_locals.size() - 1, std::size_t{5}));
        context.mesh_scan_reference = {context.pose_base_locals[reference][4],
                                       context.pose_base_locals[reference][5],
                                       context.pose_base_locals[reference][6]};
      }
    }
    context.mesh_scan_reference_bone = reference;
  }
  const ObjectRegistry &registry = context.object_registry;
  std::uint32_t budget = 20000;
  while (context.mesh_scan_cursor < registry.count && budget-- != 0) {
    const std::uint32_t index = context.mesh_scan_cursor++;
    const std::uint32_t chunk = index / kObjectChunkSize;
    if (chunk >= registry.num_chunks)
      break;
    std::uintptr_t chunk_pointer{};
    if (!ReadPointerAt(context, registry.items, chunk * sizeof(std::uintptr_t),
                       chunk_pointer) ||
        chunk_pointer == 0)
      continue;
    std::uintptr_t object{};
    if (!ReadPointerAt(context, chunk_pointer,
                       (index % kObjectChunkSize) * kObjectItemStride,
                       object) ||
        object == 0)
      continue;
    std::uintptr_t klass{};
    if (!Read(context, object + kObjectClassOffset, klass) || klass == 0)
      continue;
    // A "Mesh" name match also hits SkeletalMeshSocket / MeshDeformer, which say
    // nothing about who owns geometry, so require an actual component class.
    // The cache holds the *raw* class name because ClassNameOf shares this map.
    std::string class_name;
    const auto cached = context.mesh_scan_class_cache.find(klass);
    if (cached != context.mesh_scan_class_cache.end()) {
      class_name = cached->second;
    } else {
      std::uint32_t name_id{};
      if (Read(context, klass + kObjectNameOffset, name_id))
        static_cast<void>(ResolveName(context, name_id, class_name));
      context.mesh_scan_class_cache[klass] = class_name;
    }
    if (class_name.empty())
      continue;
    // Which UFunctions do the budgeted mesh classes really declare? Guessing the significance path
    // already cost one round trip, so list them by name and declaring class instead: read-only, one
    // line each, capped. A UFunction's class is "Function" and its outer is the UClass that declares
    // it, which is exactly what a `/Script/Module.Class.Function` path is built from.
    if (class_name == "Function" && context.mesh_scan_functions_logged < 80) {
      std::uintptr_t owner{};
      std::string owner_name;
      std::string owner_class;
      if (Read(context, object + kObjectOuterOffset, owner) && owner != 0) {
        owner_name = ObjectNameOf(context, owner);
        owner_class = ClassNameOf(context, owner);
      }
      const std::string function_name = ObjectNameOf(context, object);
      if (function_name.find("Significan") != std::string::npos ||
          function_name.find("Budget") != std::string::npos ||
          owner_name.find("Budgeted") != std::string::npos) {
        ++context.mesh_scan_functions_logged;
        char function_line[320]{};
        std::snprintf(function_line, sizeof(function_line),
                      "betterpose mesh scan function %s declared by %s [%s]",
                      function_name.c_str(), owner_name.empty() ? "-" : owner_name.c_str(),
                      owner_class.empty() ? "-" : owner_class.c_str());
        LogDiagnostic(context, function_line);
      }
    }
    // Every object whose class says "SkeletalMesh", whatever owns it: the note's next step is to find
    // out which object actually renders a modular hair piece. The passes below only log components
    // whose outer *is* the local pawn and whose class contains "MeshComponent", so a custom hair
    // component class, a component on another actor, or a mesh asset itself could never appear.
    // Read-only, one line each, capped so a big registry cannot flood the log.
    if (class_name.find("SkeletalMesh") != std::string::npos &&
        context.mesh_scan_all_logged < 400) {
      ++context.mesh_scan_all_logged;
      const bool component = class_name.find("Component") != std::string::npos;
      std::string outer_name;
      std::string outer_class;
      std::uintptr_t outer{};
      if (Read(context, object + kObjectOuterOffset, outer) && outer != 0) {
        outer_name = ObjectNameOf(context, outer);
        outer_class = ClassNameOf(context, outer);
      }
      std::string attach_name;
      if (component) {
        std::uintptr_t attach_parent{};
        if (ReadPointerAt(context, object, kMeshAttachParentOffset,
                          attach_parent) && attach_parent != 0)
          attach_name = ObjectNameOf(context, attach_parent);
      }
      std::uint32_t bones{};
      if (component) {
        std::uintptr_t data{};
        std::uint32_t count{};
        if (ReadArrayHeader(context, object + kMeshComponentSpaceBuffer0Offset, data, count) &&
            data != 0 && count <= 4096)
          bones = count;
      }
      char all_line[448]{};
      std::snprintf(all_line, sizeof(all_line),
                    "betterpose mesh scan all %llu class %s name %s outer %s [%s] bones %u attach %s",
                    static_cast<unsigned long long>(object), class_name.c_str(),
                    ObjectNameOf(context, object).c_str(),
                    outer_name.empty() ? "-" : outer_name.c_str(),
                    outer_class.empty() ? "-" : outer_class.c_str(),
                    static_cast<unsigned>(bones),
                    attach_name.empty() ? "-" : attach_name.c_str());
      LogDiagnostic(context, all_line);
    }
    // Mesh assets carry the bind pose. The scan already walks every object, so it
    // is the cheapest place to pick that up; a success is logged and reused by the
    // skeleton export.
    if (class_name.find("SkeletalMesh") != std::string::npos &&
        class_name.find("Component") == std::string::npos) {
      ++context.mesh_scan_mesh_assets;
      if (context.ref_locals.empty())
        static_cast<void>(TryAssetReferencePose(context, object));
    }
    if (class_name.find("MeshComponent") == std::string::npos)
      continue;
    ++context.mesh_scan_mesh_objects;
    // Last diagnostic for the skirt question: list the skinned meshes that are NOT
    // owned by the character, with their owner's class. An accessory attached to a
    // socket (a skirt or coat mesh on its own actor) never appeared in the earlier
    // logs because that pass only looked at components whose outer *is* the pawn.
    if (context.mesh_scan_world_logged < 12) {
      std::uintptr_t probe_data{};
      std::uint32_t probe_count{};
      std::uintptr_t owner{};
      if (ReadArrayHeader(context, object + kMeshComponentSpaceBuffer0Offset, probe_data,
                          probe_count) &&
          probe_data != 0 && probe_count != 0 && probe_count <= 4096) {
        const bool owned = Read(context, object + kObjectOuterOffset, owner) &&
                           owner == context.runtime.character;
        if (!owned) {
          const std::string owner_class =
              owner != 0 ? ClassNameOf(context, owner) : std::string();
          ++context.mesh_scan_world_logged;
          char message[256]{};
          std::snprintf(message, sizeof(message),
                        "betterpose mesh scan world %llu bones %u owner %s",
                        static_cast<unsigned long long>(object),
                        static_cast<unsigned>(probe_count),
                        owner_class.empty() ? "-" : owner_class.c_str());
          LogDiagnostic(context, message);
        }
      }
    }
    // Only the local player's own components: the outer of a component is the
    // actor that owns it, so anything else in the world is filtered out here.
    std::uintptr_t outer{};
    if (context.runtime.character == 0 ||
        !Read(context, object + kObjectOuterOffset, outer) ||
        outer != context.runtime.character)
      continue;
    // A readable bone array of a sane length means this component really owns a
    // skeleton. The bone count is the identifier that matters: our body mesh has
    // 262, a hair-only component would have its own much smaller one.
    std::uintptr_t data{};
    std::uint32_t count{};
    if (!ReadArrayHeader(context, object + kMeshComponentSpaceBuffer0Offset, data,
                         count) ||
        data == 0 || count == 0 || count > 4096)
      continue;
    ++context.mesh_scan_skinned;
    if (context.mesh_scan_bone_counts.size() < 6 &&
        std::find(context.mesh_scan_bone_counts.begin(),
                  context.mesh_scan_bone_counts.end(), count) ==
            context.mesh_scan_bone_counts.end())
      context.mesh_scan_bone_counts.push_back(count);
    // Anything that is not the body mesh itself is a candidate for the extra
    // component pass (hair layers, accessories, cloth).
    if (count != context.runtime.bone_space_count &&
        context.mesh_scan_candidates.size() < 16)
      context.mesh_scan_candidates.emplace_back(object, count);
    // Log every component that really owns a skeleton, body included: the earlier
    // version only logged components whose bone count differed from the body's, and
    // this character's second component shares the body's 298 bones, so it stayed
    // invisible. The first bone names say what the component actually is.
    {
      std::string names;
      for (std::uint32_t index{}; index != 3 && index < count; ++index) {
        std::string bone;
        if (GetBoneNameForMesh(context, object, index, bone) && !bone.empty()) {
          if (!names.empty())
            names += ",";
          names += bone;
        }
      }
      // Which asset is this component drawing? A modular hair mesh shows up here by
      // name, which is what identifies the piece that never moves.
      std::string asset_name;
      for (std::uint32_t offset{}; offset + 8 <= 0x2000 && asset_name.empty();
           offset += 8) {
        std::uintptr_t candidate{};
        if (!ReadPointerAt(context, object, offset, candidate) || candidate == 0)
          continue;
        const std::string candidate_class = ClassNameOf(context, candidate);
        if (candidate_class.find("SkeletalMesh") == std::string::npos ||
            candidate_class.find("Component") != std::string::npos)
          continue;
        asset_name = ObjectNameOf(context, candidate);
      }
      char message[384]{};
      std::snprintf(message, sizeof(message),
                    "betterpose mesh scan skinned %llu bones %u body %u asset %s "
                    "first %s",
                    static_cast<unsigned long long>(object),
                    static_cast<unsigned>(count),
                    static_cast<unsigned>(context.runtime.bone_space_count),
                    asset_name.empty() ? "-" : asset_name.c_str(), names.c_str());
      LogDiagnostic(context, message);
      // Which animation mode is each component in? The plugin only forces the BODY
      // into Custom (kAnimationModeCustom = 2) so MMD poses survive; if the hair
      // component sits in Custom too, its own AnimBlueprint
      // (nanally_fashion3_hair_AB_C) never evaluates and nothing feeds the follower.
      // Read-only.
      {
        std::uint8_t animation_mode{};
        std::uint8_t body_mode{};
        if (Read(context, object + kMeshAnimationModeOffset, animation_mode)) {
          static_cast<void>(Read(context, context.runtime.mesh +
                                             kMeshAnimationModeOffset, body_mode));
          char mode_line[192]{};
          std::snprintf(mode_line, sizeof(mode_line),
                        "betterpose mesh scan mode %llu animation %u (body %u)",
                        static_cast<unsigned long long>(object),
                        static_cast<unsigned>(animation_mode),
                        static_cast<unsigned>(body_mode));
          LogDiagnostic(context, mode_line);
        }
      }
      // Where does this component keep its rendered skinning state? The body's path
      // works and a modular hair mesh's does not, so diffing the two components'
      // fields by the class they point at is the step that finds the buffer the hair
      // actually reads. Capped, because a component has hundreds of pointers.
      std::uint32_t logged{};
      for (std::uint32_t offset{}; offset + 8 <= 0x3000 && logged < 48;
           offset += 8) {
        std::uintptr_t field{};
        if (!ReadPointerAt(context, object, offset, field) || field == 0)
          continue;
        const std::string field_class = ClassNameOf(context, field);
        if (field_class.empty())
          continue;
        char line[256]{};
        std::snprintf(line, sizeof(line),
                      "betterpose mesh scan field %llu off %X class %s name %s",
                      static_cast<unsigned long long>(object),
                      static_cast<unsigned>(offset), field_class.c_str(),
                      ObjectNameOf(context, field).c_str());
        LogDiagnostic(context, line);
        ++logged;
      }
    }
  }
  const bool done = context.mesh_scan_cursor >= registry.count;
  if (done) {
    context.mesh_scan_requested.store(false, std::memory_order_release);
    context.mesh_scan_running = false;
    // Keep the owner for diagnostics. An empty result is only a snapshot of
    // the current object registry: modular accessories can be created after
    // the pawn and after this scan completes.
    context.mesh_scan_owner = context.runtime.mesh;
    // Deliberately tiny: only digits and commas, so screen readers and OCR get
    // it right. "counts" are the distinct bone counts of skinned components.
    std::string list;
    for (std::size_t i{}; i != context.mesh_scan_bone_counts.size(); ++i) {
      if (i != 0)
        list += ",";
      list += std::to_string(context.mesh_scan_bone_counts[i]);
    }
    if (list.empty())
      list = "-";
    // Build the bone-name mapping for the extra components right away, and let
    // that report replace the plain count: "map <mapped>/<total> in <n>" tells
    // us whether the accessory mapping keys actually line up.
    // Log the outcome: the panel text is not visible to anyone reading the log, and
    // "how many mesh assets were even seen" is the number that decides whether the
    // bind-pose read is worth retrying.
    {
      char message[224]{};
      std::snprintf(message, sizeof(message),
                    "betterpose mesh scan done meshobjects %u assets %u skinned %u "
                    "skeletal %u candidates %zu refpose %s",
                    static_cast<unsigned>(context.mesh_scan_mesh_objects),
                    static_cast<unsigned>(context.mesh_scan_mesh_assets),
                    static_cast<unsigned>(context.mesh_scan_skinned),
                    static_cast<unsigned>(context.mesh_scan_all_logged),
                    context.mesh_scan_candidates.size(),
                    context.ref_locals.empty() ? context.ref_pose_status.c_str()
                                               : "read");
      LogDiagnostic(context, message);
    }
    if (context.mesh_scan_candidates.empty()) {
      static_cast<void>(DropExtraMeshes(context));
    } else {
      BuildExtraMeshes(context);
    }
  }
}

bool LoadMotionDocument(Context &context, const std::string &document,
                        const std::string &path) {
  const auto json = nlohmann::json::parse(document);
  if (json.value("kind", std::string()) != "better-pose-motion")
    throw std::runtime_error("not a better-pose motion document");
  const auto &bones = json.at("bones");
  if (!bones.is_object() || bones.empty())
    throw std::runtime_error("motion has no bone tracks");
  Context::MotionTrack track;
  track.path = path;
  track.fps = json.value("fps", 30.0);
  track.frame_count = json.value("frameCount", 0U);
  track.first_frame = json.value("firstFrame", 0U);
  track.mesh_id = json.value("targetMesh", std::string());
  track.root_bone_name = json.value("rootBone", std::string());
  track.roots_in_mmd_units =
      json.value("rootTranslationUnit", std::string()) == "mmd";
  track.mmd_leg_length = json.value("mmdLegLength", 0.0);
  if (track.frame_count == 0)
    throw std::runtime_error("motion has no frames");
  if (track.frame_count > 600000)
    throw std::runtime_error("motion is unreasonably long");

  track.bone_names.reserve(bones.size());
  for (auto it = bones.begin(); it != bones.end(); ++it) {
    track.bone_names.push_back(it.key());
    if (!it.value().is_array() || it.value().size() != track.frame_count)
      throw std::runtime_error("bone track length does not match frameCount");
  }
  track.bone_count = static_cast<std::uint32_t>(track.bone_names.size());
  track.rotations.assign(
      static_cast<std::size_t>(track.frame_count) * track.bone_count * 4, 0.0F);
  std::size_t track_index{};
  for (auto it = bones.begin(); it != bones.end(); ++it, ++track_index) {
    std::size_t frame{};
    for (const auto &key : it.value()) {
      if (!key.is_array() || key.size() != 4)
        throw std::runtime_error("bone keyframe is not a quaternion");
      const auto offset = (frame * track.bone_count + track_index) * 4;
      for (std::size_t k{}; k != 4; ++k)
        track.rotations[offset + k] = key[k].get<float>();
      ++frame;
    }
  }
  const auto roots = json.find("rootTranslation");
  if (roots != json.end() && roots->is_array() &&
      roots->size() == track.frame_count) {
    track.roots.assign(static_cast<std::size_t>(track.frame_count) * 3, 0.0F);
    std::size_t frame{};
    for (const auto &key : *roots) {
      if (!key.is_array() || key.size() != 3)
        throw std::runtime_error("root translation is not a vector");
      for (std::size_t k{}; k != 3; ++k)
        track.roots[frame * 3 + k] = key[k].get<float>();
      ++frame;
    }
    track.has_root = !track.root_bone_name.empty();
  }
  const auto offsets = json.find("boneOffsets");
  if (offsets != json.end() && offsets->is_object() &&
      !offsets->empty()) {
    track.offset_names.reserve(offsets->size());
    for (auto it = offsets->begin(); it != offsets->end(); ++it) {
      if (!it.value().is_array() || it.value().size() != track.frame_count)
        throw std::runtime_error("boneOffset track length does not match frameCount");
      track.offset_names.push_back(it.key());
    }
    const std::size_t n = track.offset_names.size();
    track.offsets.assign(static_cast<std::size_t>(track.frame_count) * n * 3, 0.0F);
    std::size_t track_index{};
    for (auto it = offsets->begin(); it != offsets->end(); ++it, ++track_index) {
      std::size_t frame{};
      for (const auto &key : it.value()) {
        if (!key.is_array() || key.size() != 3)
          throw std::runtime_error("boneOffset keyframe is not a vector");
        for (std::size_t k{}; k != 3; ++k)
          track.offsets[(frame * n + track_index) * 3 + k] = key[k].get<float>();
        ++frame;
      }
    }
    track.offsets_ready = true;
  }
  {
    std::lock_guard<std::mutex> lock(context.motion_mutex);
    context.motion = std::move(track);
  }
  context.motion_loaded.store(true, std::memory_order_release);
  context.motion_playing.store(false, std::memory_order_release);
  context.motion_seconds.store(0.0, std::memory_order_release);
  context.motion_display_seconds.store(0.0, std::memory_order_release);
  return true;
}

bool GetBoneCount(Context &context, std::int32_t &count,
                  std::string &detail) noexcept {
  count = 0;
  if (context.runtime.mesh == 0) {
    detail = "local mesh is unavailable";
    return false;
  }
  std::array<std::uint8_t, 4> parameters{};
  std::array<std::uint8_t, 4> output{};
  if (!CallVirtualUFunction(context, context.runtime.mesh,
                            kFunctionGetNumBonesPath, parameters.data(),
                            parameters.size(), detail, output.data())) {
    detail.insert(0, "GetNumBones failed: ");
    return false;
  }
  std::int32_t value{};
  std::memcpy(&value, output.data(), sizeof(value));
  if (value < 0) {
    detail = "GetNumBones returned a negative count";
    return false;
  }
  count = value;
  return true;
}

bool GetBoneNameForMesh(Context &context, const std::uintptr_t mesh,
                        const std::uint32_t bone_index,
                        std::string &name) noexcept {
  name.clear();
  if (mesh == 0)
    return false;
  std::array<std::uint8_t, 12> parameters{};
  const std::int32_t index = static_cast<std::int32_t>(bone_index);
  std::memcpy(parameters.data(), &index, sizeof(index));
  std::array<std::uint8_t, 12> output{};
  std::string detail;
  if (!CallVirtualUFunction(context, mesh, kFunctionGetBoneNamePath,
                            parameters.data(), parameters.size(), detail,
                            output.data()))
    return false;
  std::uint32_t name_id{};
  std::memcpy(&name_id, output.data() + 4, sizeof(name_id));
  if (ResolveName(context, name_id, name))
    return true;
  name = "Bone_" + std::to_string(bone_index);
  return false;
}

bool GetBoneFNameForMesh(Context &context, const std::uintptr_t mesh,
                         const std::uint32_t bone_index,
                         std::array<std::uint8_t, 8> &fname,
                         std::string &name) noexcept {
  fname.fill(0);
  name.clear();
  if (mesh == 0)
    return false;
  std::array<std::uint8_t, 12> parameters{};
  const std::int32_t index = static_cast<std::int32_t>(bone_index);
  std::memcpy(parameters.data(), &index, sizeof(index));
  std::array<std::uint8_t, 12> output{};
  std::string detail;
  if (!CallVirtualUFunction(context, mesh, kFunctionGetBoneNamePath,
                            parameters.data(), parameters.size(), detail,
                            output.data()))
    return false;
  std::memcpy(fname.data(), output.data() + 4, fname.size());
  std::uint32_t name_id{};
  std::memcpy(&name_id, fname.data(), sizeof(name_id));
  if (ResolveName(context, name_id, name))
    return true;
  name = "Bone_" + std::to_string(bone_index);
  return false;
}

bool EnsurePoseableAccessory(Context &context,
                             Context::ExtraMesh &extra) noexcept {
  if (!context.poseable_prototype_enabled || extra.asset == 0 ||
      context.runtime.character == 0 || extra.poseable_attempted)
    return extra.poseable_active;
  extra.poseable_attempted = true;

  // Preserve the actual socket contract, not a fuzzy match to a hair-chain
  // bone. The observed accessories attach directly to Head/Pelvis bones.
  std::string detail;
  std::uintptr_t source_parent{};
  std::array<std::uint8_t, 8> socket_parameters{}, socket_name{};
  if (!ReadPointerAt(context, extra.object, kMeshAttachParentOffset, source_parent) ||
      source_parent != context.runtime.mesh ||
      !CallVirtualUFunction(context, extra.object,
                            kFunctionSceneGetAttachSocketNamePath,
                            socket_parameters.data(), socket_parameters.size(),
                            detail, socket_name.data())) {
    LogDiagnostic(context, "betterpose poseable unavailable: source body socket " + detail);
    return false;
  }
  std::array<std::uint8_t, 12> index_parameters{}, index_output{};
  std::memcpy(index_parameters.data(), socket_name.data(), socket_name.size());
  std::int32_t socket_bone{-1};
  if (CallVirtualUFunction(context, context.runtime.mesh, kFunctionGetBoneIndexPath,
                           index_parameters.data(), index_parameters.size(),
                           detail, index_output.data()))
    std::memcpy(&socket_bone, index_output.data() + 8, sizeof(socket_bone));
  std::array<std::uint8_t, 96> relative_parameters{};
  if (socket_bone < 0 ||
      static_cast<std::size_t>(socket_bone) >= context.bone_names.size() ||
      !CallVirtualUFunction(context, extra.object, kFunctionSceneGetRelativeTransformPath,
                            relative_parameters.data(), relative_parameters.size(), detail,
                            extra.poseable_socket_relative.data())) {
    LogDiagnostic(context, "betterpose poseable unavailable: socket is not a body bone " + detail);
    return false;
  }
  extra.poseable_socket_bone = static_cast<std::uint32_t>(socket_bone);
  LogDiagnostic(context, "betterpose poseable socket source=" + Hex(extra.object) +
                         " bone=" + context.bone_names[extra.poseable_socket_bone] +
                         " mode=rigid-bind");

  std::uintptr_t poseable_class{};
  if (!FindObjectAddressByPath(context, "/Script/Engine.PoseableMeshComponent",
                               poseable_class, &detail)) {
    LogDiagnostic(context, "betterpose poseable create: class lookup failed " +
                               detail);
    return false;
  }

  // AddComponentByClass(TSubclassOf<UActorComponent>, bManualAttachment,
  // FTransform RelativeTransform, bDeferredFinish), ReturnValue at +120.
  std::array<std::uint8_t, 128> add_parameters{};
  std::memcpy(add_parameters.data(), &poseable_class, sizeof(poseable_class));
  add_parameters[8] = 1;
  PackedTransform identity{};
  identity.rotation[3] = 1.0;
  identity.scale[0] = identity.scale[1] = identity.scale[2] = 1.0;
  std::memcpy(add_parameters.data() + 16, &identity, sizeof(identity));
  std::array<std::uint8_t, 128> add_output{};
  if (!CallVirtualUFunction(
          context, context.runtime.character,
          kFunctionActorAddComponentByClassPath, add_parameters.data(),
          add_parameters.size(), detail, add_output.data())) {
    LogDiagnostic(context, "betterpose poseable create: AddComponentByClass failed " +
                               detail);
    return false;
  }
  std::memcpy(&extra.poseable_component, add_output.data() + 120,
              sizeof(extra.poseable_component));
  if (extra.poseable_component == 0) {
    LogDiagnostic(context, "betterpose poseable create: returned null");
    return false;
  }

  const std::array<std::uint8_t, 8> asset_parameters = [&] {
    std::array<std::uint8_t, 8> value{};
    std::memcpy(value.data(), &extra.asset, sizeof(extra.asset));
    return value;
  }();
  if (!CallVirtualUFunction(context, extra.poseable_component,
                            kFunctionSetSkeletalMeshAssetPath,
                            asset_parameters.data(), asset_parameters.size(),
                            detail)) {
    LogDiagnostic(context, "betterpose poseable create: SetSkeletalMeshAsset failed " +
                               detail);
    return false;
  }

  // Attach without a socket: each pose update explicitly composes the body
  // socket transform with the authored offset. Engine scene attachment ticks
  // otherwise lag behind the body buffers overwritten by BetterPose.
  std::array<std::uint8_t, 21> attach_parameters{};
  std::memcpy(attach_parameters.data(), &context.runtime.mesh,
              sizeof(context.runtime.mesh));
  std::array<std::uint8_t, 21> attach_output{};
  if (!CallVirtualUFunction(context, extra.poseable_component,
                            kFunctionSceneAttachComponentPath,
                            attach_parameters.data(), attach_parameters.size(),
                            detail, attach_output.data()) ||
      attach_output[20] == 0) {
    LogDiagnostic(context, "betterpose poseable create: attach failed " + detail);
    return false;
  }

  extra.poseable_active = true;
  char message[256]{};
  std::snprintf(message, sizeof(message),
                "betterpose poseable created source=%llX component=%llX "
                "asset=%llX bones=%u",
                static_cast<unsigned long long>(extra.object),
                static_cast<unsigned long long>(extra.poseable_component),
                static_cast<unsigned long long>(extra.asset),
                static_cast<unsigned>(extra.bone_count));
  LogDiagnostic(context, message);
  return true;
}

bool WritePoseableAccessoryPose(
    Context &context, Context::ExtraMesh &extra,
    const std::vector<PackedTransform> &components) noexcept {
  if (!extra.poseable_active || extra.poseable_component == 0 ||
      components.size() != extra.bone_count ||
      extra.bone_fnames.size() != extra.bone_count)
    return false;
  bool all_ok = true;
  for (std::uint32_t bone{}; bone != extra.bone_count; ++bone) {
    std::array<std::uint8_t, 113> parameters{};
    std::memcpy(parameters.data(), extra.bone_fnames[bone].data(), 8);
    std::memcpy(parameters.data() + 16, &components[bone],
                sizeof(PackedTransform));
    // EBoneSpaces::ComponentSpace in this build (WorldSpace=0,
    // ComponentSpace=1; there is no LocalSpace enum in this API).
    parameters[112] = 1;
    std::string detail;
    if (!CallVirtualUFunction(context, extra.poseable_component,
                              kFunctionPoseableSetBoneTransformByNamePath,
                              parameters.data(), parameters.size(), detail))
      all_ok = false;
  }
  return all_ok;
}

// Component placement remains socket-driven; the optional strand only changes
// the replacement's own component-space bones.
bool DrivePoseableSocketPose(Context &context, Context::ExtraMesh &extra,
                            const std::vector<PackedTransform> &body_pose) noexcept {
  if (extra.poseable_socket_bone >= body_pose.size())
    return false;
  PackedTransform authored{};
  std::memcpy(&authored, extra.poseable_socket_relative.data(), sizeof(authored));
  PackedTransform placement{};
  PackTransform(TransformMultiply(UnpackTransform(body_pose[extra.poseable_socket_bone]),
                                  UnpackTransform(authored)), placement);
  std::memcpy(extra.poseable_expected_relative.data(), &placement, sizeof(placement));
  std::array<std::uint8_t, 369> parameters{};
  static_assert(parameters.size() <= kMaximumUFunctionParameterBytes);
  std::memcpy(parameters.data(), &placement, sizeof(placement));
  std::string detail;
  if (!CallVirtualUFunction(context, extra.poseable_component,
                            kFunctionSceneSetRelativeTransformPath,
                            parameters.data(), parameters.size(), detail))
    return false;
  const bool was_dynamic = extra.accessory_dynamics.Moving();
  const bool strand_enabled = extra.accessory_dynamics.DrivenBones() != 0;
  const std::vector<std::array<double, 12>>* desired_pose = &extra.bind_world;
  if (strand_enabled) {
    std::array<std::uint8_t, 96> world_query{};
    PackedTransform component_world{};
    const bool world_ok = CallVirtualUFunction(
        context, extra.poseable_component, kFunctionSceneGetComponentTransformPath,
        world_query.data(), world_query.size(), detail, &component_world);
    const auto& s = component_world.scale;
    const bool uniform_scale = std::isfinite(s[0]) && s[0] > 1e-6 &&
                               std::abs(s[0]-s[1]) < 1e-5 && std::abs(s[0]-s[2]) < 1e-5;
    if (world_ok && uniform_scale) {
      using namespace better_pose::accessory;
      Frame frame{{component_world.translation[0],component_world.translation[1],component_world.translation[2]},
                  {component_world.rotation[0],component_world.rotation[1],component_world.rotation[2],component_world.rotation[3]},s[0]};
      const Frame placement_frame{
          {placement.translation[0],placement.translation[1],placement.translation[2]},
          {placement.rotation[0],placement.rotation[1],placement.rotation[2],placement.rotation[3]},placement.scale[0]};
      const Frame body_world = Compose(frame, Inverse(placement_frame));
      const auto capsules = extra.accessory_dynamics.BodyCapsules(body_pose.size(),
          [&](int bone) { const auto& p=body_pose[bone].translation; return Vec{p[0],p[1],p[2]}; }, body_world);
      desired_pose = &extra.accessory_dynamics.Sample(
          frame, context.motion_seconds.load(std::memory_order_acquire),
          context.motion_seek_serial.load(std::memory_order_acquire),
          context.motion_loaded.load(std::memory_order_acquire) &&
              context.motion_playing.load(std::memory_order_acquire), capsules);
    } else {
      extra.accessory_dynamics.Reset();
    }
  } else if (was_dynamic) {
    extra.accessory_dynamics.Reset();
  }
  if (!extra.poseable_bind_written || strand_enabled || was_dynamic) {
    if (extra.bind_world.size() != extra.bone_count)
      return false;
    std::vector<PackedTransform> submitted_pose(extra.bone_count);
    for (std::size_t bone{}; bone != submitted_pose.size(); ++bone)
      std::memcpy(&submitted_pose[bone], (*desired_pose)[bone].data(), sizeof(PackedTransform));
    if (!WritePoseableAccessoryPose(context, extra, submitted_pose))
      return false;
    extra.poseable_expected_pose = *desired_pose;
    for (auto &probe : extra.pose_probes) {
      probe.expected_component = (*desired_pose)[probe.bone];
      probe.has_expected = true;
    }
    extra.poseable_bind_written = true;
  }
  if (!extra.poseable_original_hidden_saved) {
    const std::array<std::uint8_t, 2> hidden{0, 0};
    if (!CallVirtualUFunction(context, extra.object, kFunctionSceneSetVisibilityPath,
                              hidden.data(), hidden.size(), detail))
      return false;
    extra.poseable_original_hidden = true;
    extra.poseable_original_hidden_saved = true;
  }
  return true;
}

void DestroyPoseableAccessories(Context &context) noexcept {
  for (auto &extra : context.extra_meshes) {
    if (extra.poseable_component == 0)
      continue;
    std::string detail;
    if (extra.poseable_original_hidden_saved) {
      const std::array<std::uint8_t, 2> visible_parameters{1, 1};
      static_cast<void>(CallVirtualUFunction(
          context, extra.object, kFunctionSceneSetVisibilityPath,
          visible_parameters.data(), visible_parameters.size(), detail));
    }
    // K2_DestroyComponent(Object) permits the component itself or its owner.
    // A null Object silently refuses destruction for these actor-owned meshes.
    const auto destroy_caller = extra.poseable_component;
    static_cast<void>(CallVirtualUFunction(
        context, extra.poseable_component,
        kFunctionActorComponentDestroyPath, &destroy_caller,
        sizeof(destroy_caller), detail));
    LogDiagnostic(context, "betterpose poseable destroy self=" + Hex(destroy_caller) +
                           " result=" + detail);
    extra.poseable_component = 0;
    extra.poseable_active = false;
    extra.poseable_original_hidden_saved = false;
    extra.poseable_attempted = false;
    extra.poseable_bind_written = false;
    extra.poseable_last_write_ok = false;
    extra.accessory_dynamics.Reset();
    extra.poseable_expected_pose.clear();
  }
}

void DestroyStalePoseableComponent(Context &context,
                                   const std::uintptr_t component) noexcept {
  if (component == 0)
    return;
  std::string detail;
  const bool called = CallVirtualUFunction(
      context, component, kFunctionActorComponentDestroyPath,
      &component, sizeof(component), detail);
  LogDiagnostic(context, "betterpose poseable stale destroy self=" + Hex(component) +
                           " called=" + (called ? "1" : "0") + " result=" + detail);
}

void RestoreAccessoryVisibility(Context &context,
                                const std::uintptr_t component) noexcept {
  if (component == 0)
    return;
  const std::array<std::uint8_t, 2> parameters{1, 1};
  std::string detail;
  static_cast<void>(CallVirtualUFunction(
      context, component, kFunctionSceneSetVisibilityPath,
      parameters.data(), parameters.size(), detail));
}

bool ReadBoneTransformForMesh(Context &context, const std::uintptr_t mesh,
                              const std::uint32_t bone_index,
                              PackedTransform &transform) noexcept {
  transform = {};
  if (mesh == 0)
    return false;
  std::array<std::uint8_t, 12> name_parameters{};
  const std::int32_t index = static_cast<std::int32_t>(bone_index);
  std::memcpy(name_parameters.data(), &index, sizeof(index));
  std::array<std::uint8_t, 12> name_output{};
  std::string detail;
  if (!CallVirtualUFunction(context, mesh, kFunctionGetBoneNamePath,
                            name_parameters.data(), name_parameters.size(),
                            detail, name_output.data()))
    return false;

  // GetBoneTransform takes an FName and ERelativeTransformSpace. Space 2 is
  // component space, matching the array that the plugin currently writes.
  std::array<std::uint8_t, 112> parameters{};
  std::memcpy(parameters.data(), name_output.data() + 4, sizeof(std::uint64_t));
  parameters[8] = 2;
  std::array<std::uint8_t, 112> output{};
  if (!CallVirtualUFunction(context, mesh, kFunctionGetBoneTransformPath,
                            parameters.data(), parameters.size(), detail,
                            output.data()))
    return false;
  // UHT places the FTransform ReturnValue after the FName and enum inputs:
  // FName (8) + enum/padding (8), then the 0x60-byte transform.
  constexpr std::size_t kReturnValueOffset = 0x10;
  std::memcpy(&transform, output.data() + kReturnValueOffset,
              sizeof(transform));
  const double norm =
      std::sqrt(transform.rotation[0] * transform.rotation[0] +
                transform.rotation[1] * transform.rotation[1] +
                transform.rotation[2] * transform.rotation[2] +
                transform.rotation[3] * transform.rotation[3]);
  return norm > 0.5 && norm < 1.5;
}

bool GetBoneName(Context &context, const std::uint32_t bone_index,
                 std::string &name) noexcept {
  return GetBoneNameForMesh(context, context.runtime.mesh, bone_index, name);
}

// ---------------------------------------------------------------------------
// Extra skeletal mesh components on the same pawn
//
// Discovered by StepMeshScan (owner == pawn, class name contains
// "MeshComponent", readable bone array whose length differs from the body's).
// Each of their bones is mapped to a body bone by name, bind pose, or hierarchy.
// The attach parent only establishes scene-component placement; it does not
// establish a leader-pose relationship.
// ---------------------------------------------------------------------------

bool ReadBoneTranslations(Context &context, const std::uintptr_t array,
                          const std::uint32_t count,
                          std::vector<std::array<double, 3>> &out) noexcept {
  out.clear();
  if (!CoreReady(context.core) || array == 0 || count == 0 || count > 8192)
    return false;
  if (context.core->read_memory == nullptr)
    return false;
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(count) * kTransformSize);
  AnomalyMutableByteSpanV1 span{raw.data(), raw.size()};
  if (context.core->read_memory(context.core->user, array, span).code !=
      ANOMALY_STATUS_V1_OK)
    return false;
  out.resize(count);
  for (std::uint32_t index{}; index != count; ++index) {
    double values[3]{};
    std::memcpy(values,
                raw.data() + static_cast<std::size_t>(index) * kTransformSize +
                    kTransformTranslationOffset,
                sizeof(values));
    out[index] = {values[0], values[1], values[2]};
  }
  return true;
}

bool ObjectAtIndex(Context &context, const std::uint32_t index,
                   std::uintptr_t &object) noexcept {
  object = 0;
  const ObjectRegistry &registry = context.object_registry;
  if (index >= registry.count)
    return false;
  const std::uint32_t chunk = index / kObjectChunkSize;
  if (chunk >= registry.num_chunks)
    return false;
  std::uintptr_t chunk_pointer{};
  if (!ReadPointerAt(context, registry.items, chunk * sizeof(std::uintptr_t),
                     chunk_pointer) ||
      chunk_pointer == 0)
    return false;
  return ReadPointerAt(context, chunk_pointer,
                       (index % kObjectChunkSize) * kObjectItemStride,
                       object) &&
         object != 0;
}

std::string ClassNameOf(Context &context, const std::uintptr_t object) noexcept {
  std::uintptr_t klass{};
  if (!Read(context, object + kObjectClassOffset, klass) || klass == 0)
    return {};
  const auto cached = context.mesh_scan_class_cache.find(klass);
  if (cached != context.mesh_scan_class_cache.end())
    return cached->second;
  std::uint32_t name_id{};
  std::string name;
  if (Read(context, klass + kObjectNameOffset, name_id))
    static_cast<void>(ResolveName(context, name_id, name));
  context.mesh_scan_class_cache[klass] = name;
  return name;
}

std::string ObjectNameOf(Context &context, const std::uintptr_t object) noexcept {
  std::uint32_t name_id{};
  std::string name;
  if (Read(context, object + kObjectNameOffset, name_id))
    static_cast<void>(ResolveName(context, name_id, name));
  return name;
}

// Read ANY mesh asset's reference (bind) skeleton: the same RawRefBoneInfo /
// RawRefBonePose pair as TryAssetReferencePose, but validated against *that* mesh's
// bone names instead of the body's. The pawn's extra meshes name their bones in a
// different space (Bone_hairRb00, Pelvis_adjust), so the name mapping finds nothing
// and their live positions cannot be compared while we override the body or while
// they sit frozen; matching them in bind pose is independent of both.
bool ReadAssetBindSkeleton(Context &context, const std::uintptr_t asset,
                           const std::uintptr_t component,
                           const std::uint32_t bone_count,
                           std::vector<std::array<double, 12>> &locals,
                           std::vector<std::int32_t> &parents) noexcept {
  if (asset == 0 || component == 0 || bone_count == 0 || bone_count > 4096)
    return false;
  for (std::uint32_t offset = 16; offset + 16 <= 0x800; offset += 8) {
    std::uintptr_t pose_data{};
    std::uint32_t pose_count{};
    std::uint32_t pose_capacity{};
    if (!ReadPointerAt(context, asset, offset, pose_data) || pose_data == 0)
      continue;
    if (!Read(context, asset + offset + 8, pose_count) ||
        !Read(context, asset + offset + 12, pose_capacity))
      continue;
    if (pose_count != bone_count || pose_capacity < pose_count ||
        pose_capacity > (1U << 20))
      continue;
    std::uintptr_t info_data{};
    std::uint32_t info_count{};
    if (!ReadPointerAt(context, asset, offset - 16, info_data) || info_data == 0)
      continue;
    if (!Read(context, asset + offset - 8, info_count) || info_count != bone_count)
      continue;
    std::array<double, 12> probe{};
    for (const std::size_t sample :
         {std::size_t{0}, static_cast<std::size_t>(bone_count - 1)}) {
      if (!Read(context, pose_data + sample * kTransformSize, probe))
        return false;
      const double norm = probe[0] * probe[0] + probe[1] * probe[1] +
                          probe[2] * probe[2] + probe[3] * probe[3];
      if (norm < 0.9 || norm > 1.1)
        return false;
      if (std::fabs(probe[8] - 1.0) > 0.05 || std::fabs(probe[9] - 1.0) > 0.05 ||
          std::fabs(probe[10] - 1.0) > 0.05)
        return false;
    }
    for (const std::uint32_t stride : {12U, 16U}) {
      bool names_ok = true;
      for (const std::uint32_t sample : {0U, bone_count / 2, bone_count - 1}) {
        std::int32_t name_id{};
        std::string expected;
        std::string found;
        if (!Read(context, info_data + sample * stride, name_id) ||
            !GetBoneNameForMesh(context, component, sample, expected) ||
            !ResolveName(context, static_cast<std::uint32_t>(name_id), found) ||
            found != expected) {
          names_ok = false;
          break;
        }
      }
      if (!names_ok)
        continue;
      std::vector<std::array<double, 12>> read_locals(bone_count);
      std::vector<std::int32_t> read_parents(bone_count, -1);
      bool ok = true;
      for (std::uint32_t bone{}; bone != bone_count && ok; ++bone) {
        std::int32_t parent{};
        if (!Read(context, pose_data + bone * kTransformSize, read_locals[bone]) ||
            !Read(context, info_data + bone * stride + 8, parent) || parent < -1 ||
            parent >= static_cast<std::int32_t>(bone_count) ||
            parent >= static_cast<std::int32_t>(bone)) {
          ok = false;                  // not a parent-before-child tree
          break;
        }
        read_parents[bone] = parent;
      }
      if (!ok)
        continue;
      locals = std::move(read_locals);
      parents = std::move(read_parents);
      return true;
    }
  }
  return false;
}

// Component-space positions from a bind skeleton: the transforms below, positions only.
std::vector<Transformd> BindPoseTransforms(
    const std::vector<std::array<double, 12>> &locals,
    const std::vector<std::int32_t> &parents) noexcept;

std::vector<std::array<double, 3>> BindPosePositions(
    const std::vector<std::array<double, 12>> &locals,
    const std::vector<std::int32_t> &parents) noexcept {
  const auto transforms = BindPoseTransforms(locals, parents);
  std::vector<std::array<double, 3>> positions(transforms.size());
  for (std::size_t bone{}; bone != transforms.size(); ++bone) {
    positions[bone] = {transforms[bone].translation.x,
                       transforms[bone].translation.y,
                       transforms[bone].translation.z};
  }
  return positions;
}

// Component-space bind transforms (rotation + position) from a bind skeleton.
//
// bone_parents is NOT guaranteed to list parents before children (ComputeBoneComponent
// recurses with a mark array for exactly that reason), and assuming it was made the
// accumulation treat most bones as roots: their bind position came out near zero, so
// the delta against the live transform measured a whole body height (1.65 m) instead
// of how far the bone had moved. Walk each bone's parent chain instead.
std::vector<Transformd> BindPoseTransforms(
    const std::vector<std::array<double, 12>> &locals,
    const std::vector<std::int32_t> &parents) noexcept {
  std::vector<Transformd> out(locals.size());
  std::vector<std::size_t> chain;
  for (std::size_t bone{}; bone != locals.size(); ++bone) {
    chain.clear();
    std::size_t current = bone;
    for (std::size_t guard{}; guard <= locals.size(); ++guard) {
      chain.push_back(current);
      const std::int32_t parent =
          current < parents.size() ? parents[current] : -1;
      if (parent < 0 || static_cast<std::size_t>(parent) >= locals.size() ||
          static_cast<std::size_t>(parent) == current)
        break;
      current = static_cast<std::size_t>(parent);
    }
    Transformd accumulated;
    for (auto step = chain.rbegin(); step != chain.rend(); ++step) {
      const auto &raw = locals[*step];
      Transformd local;
      local.rotation = Quatd{raw[0], raw[1], raw[2], raw[3]};
      local.translation = Vec3d{raw[4], raw[5], raw[6]};
      local.scale = Vec3d{raw[8], raw[9], raw[10]};
      accumulated = TransformMultiply(accumulated, local);
    }
    out[bone] = accumulated;
  }
  return out;
}

Quatd QuatConjugate(const Quatd &q) noexcept {
  return Quatd{-q.x, -q.y, -q.z, q.w};
}

Transformd TransformInverse(const Transformd &value) noexcept {
  Transformd out;
  out.rotation = QuatConjugate(value.rotation);
  out.scale = Vec3d{value.scale.x != 0.0 ? 1.0 / value.scale.x : 1.0,
                    value.scale.y != 0.0 ? 1.0 / value.scale.y : 1.0,
                    value.scale.z != 0.0 ? 1.0 / value.scale.z : 1.0};
  const Vec3d negated{-value.translation.x, -value.translation.y,
                      -value.translation.z};
  const Vec3d rotated = QuatRotateVector(out.rotation, negated);
  out.translation = Vec3d{rotated.x * out.scale.x, rotated.y * out.scale.y,
                          rotated.z * out.scale.z};
  return out;
}

// Normalised-lerp quaternion blend: stable, and a lag only ever asks for small angles.
Quatd QuatBlend(const Quatd &from, const Quatd &to, const double alpha) noexcept {
  double dot = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
  Quatd target = to;
  if (dot < 0.0) {                        // q and -q are the same rotation
    target = Quatd{-to.x, -to.y, -to.z, -to.w};
  }
  Quatd out{from.x + (target.x - from.x) * alpha,
            from.y + (target.y - from.y) * alpha,
            from.z + (target.z - from.z) * alpha,
            from.w + (target.w - from.w) * alpha};
  const double norm = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z +
                                out.w * out.w);
  if (norm < 1e-9)
    return to;
  out.x /= norm;
  out.y /= norm;
  out.z /= norm;
  out.w /= norm;
  return out;
}

// Lower-case alphanumeric words of a bone name, camelCase and digits split out.
std::vector<std::string> NameTokens(const std::string &name) noexcept {
  std::vector<std::string> tokens;
  std::string current;
  const auto flush = [&tokens, &current]() {
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  };
  for (const char raw : name) {
    const auto c = static_cast<unsigned char>(raw);
    if (std::isalnum(c) == 0) {
      flush();
      continue;
    }
    const bool upper = std::isupper(c) != 0;
    const bool digit = std::isdigit(c) != 0;
    const bool previous_digit =
        !current.empty() && std::isdigit(static_cast<unsigned char>(current.back())) != 0;
    if (!current.empty() && (upper || (digit != previous_digit)))
      flush();
    current.push_back(static_cast<char>(std::tolower(c)));
  }
  flush();
  return tokens;
}

// Which body bone does one of a modular mesh's attach bones belong to?
//
// The pawn's extra meshes name those after the anatomy with an _adjust suffix
// (head_adjust, clavicle_r_adjust, spine_01_adjust), while the body uses
// Bip001-Head, Bip001-R-Clavicle, Bip001-Spine1. Every token of the extra name has
// to appear in the body name, digits have to appear *after* the words they qualify
// (which is what keeps spine_01_adjust off Bip001-Spine2), and anatomical
// Bip001-* bones win over helper bones. Returns kNoBone when nothing matches.
std::uint32_t MatchBodyBone(Context &context, const std::string &name) noexcept {
  std::vector<std::string> words;
  std::vector<std::string> digits;
  for (auto &token : NameTokens(name)) {
    if (token == "adjust" || token == "position" || token == "root" ||
        token == "bone" || token == "bip" || token == "bip001")
      continue;
    if (std::all_of(token.begin(), token.end(), [](const char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }))
      digits.push_back(token);
    else
      words.push_back(token);
  }
  if (words.empty() && digits.empty())
    return (std::numeric_limits<std::uint32_t>::max)();
  const auto normalized_of = [](const std::string &value) {
    std::string out;
    for (const char raw : value) {
      const auto c = static_cast<unsigned char>(raw);
      if (std::isalnum(c) != 0)
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
  };
  std::uint32_t best = (std::numeric_limits<std::uint32_t>::max)();
  std::size_t best_length = 0;
  bool best_anatomical = false;
  for (std::uint32_t index{}; index != context.bone_names.size(); ++index) {
    const std::string normalized = normalized_of(context.bone_names[index]);
    if (normalized.empty())
      continue;
    std::size_t after_words = 0;
    bool ok = true;
    const auto body_tokens = NameTokens(context.bone_names[index]);
    for (const auto &word : words) {
      if (word.size() == 1) {
        // A side marker has to be a token of its own: matching "r" as a substring is
        // what put upperarm_r_adjust on Bip001-L-UpperArm (the "r" in "upperarm").
        if (std::find(body_tokens.begin(), body_tokens.end(), word) ==
            body_tokens.end()) {
          ok = false;
          break;
        }
        continue;
      }
      const std::size_t position = normalized.find(word);
      if (position == std::string::npos) {
        ok = false;
        break;
      }
      after_words = (std::max)(after_words, position + word.size());
    }
    if (!ok)
      continue;
    for (const auto &digit : digits) {
      std::string want = digit;
      const std::size_t first = want.find_first_not_of('0');
      want = first == std::string::npos ? std::string("0") : want.substr(first);
      const std::size_t position = normalized.find(want, after_words);
      if (position == std::string::npos) {
        ok = false;
        break;
      }
      after_words = position + want.size();
    }
    if (!ok)
      continue;
    const bool anatomical =
        context.bone_names[index].rfind("Bip001", 0) == 0 ||
        context.bone_names[index].rfind("Bone-", 0) == 0;
    if (best != (std::numeric_limits<std::uint32_t>::max)() &&
        !(anatomical && !best_anatomical) &&
        !(anatomical == best_anatomical && normalized.size() < best_length))
      continue;
    best = index;
    best_length = normalized.size();
    best_anatomical = anatomical;
  }
  return best;
}

// Read the engine's reference (bind) pose out of the character's mesh asset.
//
// Strictly read-only. A pointer inside the mesh component that resolves to a
// USkeletalMesh, then the FReferenceSkeleton inside that asset: RawRefBoneInfo
// (FName + parent, 12 bytes per bone) sits immediately before RawRefBonePose
// (an FTransform per bone). Candidates are accepted only when the array counts
// match this skeleton AND resolving the FNames out of the info array reproduces
// this skeleton's bone names - random memory does not do that, and a mismatched
// asset (another character's mesh) does not either.
// Read the reference (bind) pose out of one mesh asset. RawRefBoneInfo (FName +
// parent, 12 bytes per bone) sits immediately before RawRefBonePose (an FTransform
// per bone), so the pair of TArray headers identifies the block: both counts must
// equal this skeleton's bone count, the transforms must be sane, and resolving the
// FNames out of the info array must reproduce this skeleton's bone names. Random
// memory and another character's mesh both fail that.
bool TryAssetReferencePose(Context &context,
                           const std::uintptr_t asset) noexcept {
  if (asset == 0 || context.bone_names.empty())
    return false;
  const std::size_t count = context.bone_names.size();
  const auto expected = static_cast<std::uint32_t>(count);
  const auto name_matches = [&context, count](const std::uintptr_t info,
                                              const std::uint32_t stride) noexcept {
    for (std::size_t sample : {std::size_t{0}, count / 2, count - 1}) {
      std::int32_t name_id{};
      if (!Read(context, info + sample * stride, name_id))
        return false;
      std::string name;
      if (!ResolveName(context, static_cast<std::uint32_t>(name_id), name))
        return false;
      if (name != context.bone_names[sample])
        return false;
    }
    return true;
  };
  for (std::uint32_t offset = 16; offset + 16 <= 0x800; offset += 8) {
    std::uintptr_t pose_data{};
    std::uint32_t pose_count{};
    std::uint32_t pose_capacity{};
    if (!ReadPointerAt(context, asset, offset, pose_data) || pose_data == 0)
      continue;
    if (!Read(context, asset + offset + 8, pose_count) ||
        !Read(context, asset + offset + 12, pose_capacity))
      continue;
    if (pose_count != expected || pose_capacity < pose_count ||
        pose_capacity > (1U << 20))
      continue;
    std::uintptr_t info_data{};
    std::uint32_t info_count{};
    if (!ReadPointerAt(context, asset, offset - 16, info_data) || info_data == 0)
      continue;
    if (!Read(context, asset + offset - 8, info_count) || info_count != expected)
      continue;
    // Sample the first and last transform: unit rotation, unit scale.
    for (const std::size_t sample : {std::size_t{0}, count - 1}) {
      std::array<double, 12> probe{};
      if (!Read(context, pose_data + sample * kTransformSize, probe))
        return false;
      const double norm = probe[0] * probe[0] + probe[1] * probe[1] +
                          probe[2] * probe[2] + probe[3] * probe[3];
      if (norm < 0.9 || norm > 1.1)
        return false;
      if (std::fabs(probe[8] - 1.0) > 0.05 || std::fabs(probe[9] - 1.0) > 0.05 ||
          std::fabs(probe[10] - 1.0) > 0.05)
        return false;
    }
    if (!name_matches(info_data, 12) && !name_matches(info_data, 16))
      continue;
    std::vector<std::array<double, 12>> locals(count);
    for (std::size_t bone{}; bone != count; ++bone) {
      if (!Read(context, pose_data + bone * kTransformSize, locals[bone]))
        return false;
    }
    context.ref_locals = std::move(locals);
    context.ref_pose_object = asset;
    context.ref_pose_status = "reference pose read from the mesh asset";
    {
      char message[160]{};
      std::snprintf(message, sizeof(message),
                    "betterpose refpose read %zu bones from mesh asset %llX",
                    count, static_cast<unsigned long long>(asset));
      LogDiagnostic(context, message);
    }
    return true;
  }
  return false;
}

// Find the character's mesh asset and read its reference pose. Called by the
// skeleton export (and driven from the mesh scan, which already walks every
// object, so it can also succeed there).
bool FindReferencePose(Context &context) noexcept {
  if (!context.ref_locals.empty())
    return true;
  context.ref_pose_object = 0;
  context.ref_pose_status = "mesh unavailable";
  const auto mesh = context.runtime.mesh;
  if (mesh == 0)
    return false;
  if (context.bone_names.empty())
    MaybeRefreshBoneNames(context);
  const std::size_t count = context.bone_names.size();
  if (count == 0) {
    context.ref_pose_status = "bone names unavailable";
    return false;
  }
  std::vector<std::uintptr_t> candidates;
  // USkinnedMeshComponent is deep enough that SkeletalMesh can sit past 0x800, and
  // this game subclasses the asset type (HTSkeletalMesh...), so match on the class
  // name containing SkeletalMesh while excluding *Component* classes.
  const auto looks_like_mesh_asset = [&context](const std::uintptr_t object) noexcept {
    const std::string name = ClassNameOf(context, object);
    if (name.empty())
      return false;
    if (name.find("SkeletalMesh") == std::string::npos)
      return false;
    return name.find("Component") == std::string::npos;
  };
  for (std::uint32_t offset{}; offset + 8 <= 0x2000; offset += 8) {
    std::uintptr_t candidate{};
    if (!ReadPointerAt(context, mesh, offset, candidate) || candidate == 0)
      continue;
    if (!looks_like_mesh_asset(candidate))
      continue;
    if (std::find(candidates.begin(), candidates.end(), candidate) ==
        candidates.end())
      candidates.push_back(candidate);
  }
  if (candidates.empty() && RefreshObjectRegistry(context)) {
    // Fallback: the component does not hold the asset as a plain pointer here (the
    // mesh scan found it only by walking the registry), so sweep the whole registry.
    // This walks a few hundred thousand objects, which is why it only runs from an
    // explicit action - and every candidate still has to pass the FName check.
    for (std::uint32_t index{}; index < context.object_registry.count; ++index) {
      std::uintptr_t candidate{};
      if (!ObjectAtIndex(context, index, candidate))
        continue;
      if (!looks_like_mesh_asset(candidate))
        continue;
      if (TryAssetReferencePose(context, candidate))
        return true;
      candidates.push_back(candidate);
    }
  }
  if (candidates.empty()) {
    context.ref_pose_status = "no SkeletalMesh pointer in the mesh component";
    return false;
  }
  for (const auto asset : candidates) {
    if (TryAssetReferencePose(context, asset))
      return true;
  }
  char status[128]{};
  std::snprintf(status, sizeof(status),
                "no reference pose array matched %zu mesh asset candidate(s)",
                candidates.size());
  context.ref_pose_status = status;
  return false;
}

// Read-only discovery of the attach-parent offset. A component stores its attach
// parent as an FWeakObjectPtr {int32 ObjectIndex, int32 SerialNumber}, so a
// candidate offset is only accepted when the index resolves through GObjects to
// an object whose stored serial matches -- and when that holds for *every* probe
// component at once, which random data never does.
void StepAttachScan(Context &context) noexcept {
  if (!context.attach_scan_requested.load(std::memory_order_acquire))
    return;
  context.attach_scan_requested.store(false, std::memory_order_release);
  if (!RefreshObjectRegistry(context)) {
    return;
  }
  std::vector<std::uintptr_t> probes;
  if (context.runtime.mesh != 0)
    probes.push_back(context.runtime.mesh);
  for (const auto &candidate : context.mesh_scan_candidates) {
    if (probes.size() >= 5)
      break;
    probes.push_back(candidate.first);
  }
  if (probes.size() < 2) {
    return;
  }
  context.attach_offsets.clear();
  auto read_weak = [&](const std::uintptr_t object, const std::uint32_t offset,
                       std::uintptr_t &target, std::string &target_class) {
    target = 0;
    target_class.clear();
    // UE5 declares AttachParent as TObjectPtr, which is a raw pointer in a
    // non-editor build; older layouts use an FWeakObjectPtr {index, serial}.
    // Try both and accept whichever resolves to a Component/Actor class, which
    // random data has no way of doing.
    std::uintptr_t raw{};
    if (Read(context, object + offset, raw) && raw > 0x10000U &&
        raw < 0x7FFFFFFFFFFFULL) {
      std::string raw_class = ClassNameOf(context, raw);
      if (raw_class.find("Component") != std::string::npos ||
          raw_class.find("Actor") != std::string::npos) {
        target = raw;
        target_class = raw_class;
        return true;
      }
    }
    std::uint32_t index{};
    std::uint32_t serial{};
    if (!Read(context, object + offset, index) ||
        !Read(context, object + offset + 4, serial) || index == 0)
      return false;
    std::uintptr_t candidate{};
    if (!ObjectAtIndex(context, index, candidate))
      return false;
    // The serial guards against a stale index that now points elsewhere.
    const std::uint32_t chunk = index / kObjectChunkSize;
    std::uintptr_t chunk_pointer{};
    std::uint32_t stored_serial{};
    if (!ReadPointerAt(context, context.object_registry.items,
                       chunk * sizeof(std::uintptr_t), chunk_pointer) ||
        chunk_pointer == 0 ||
        !Read(context,
              chunk_pointer + (index % kObjectChunkSize) * kObjectItemStride +
                  16,
              stored_serial) ||
        stored_serial != serial)
      return false;
    target = candidate;
    target_class = ClassNameOf(context, candidate);
    return !target_class.empty();
  };
  struct AttachHit {
    std::uint32_t offset{};
    std::uint32_t hits{};
    std::string sample;
  };
  std::vector<AttachHit> hits;
  for (std::uint32_t offset = 0x80; offset < 0xC00; offset += 8) {
    std::uint32_t matched{};
    std::string sample;
    for (const auto probe : probes) {
      std::uintptr_t target{};
      std::string target_class;
      if (read_weak(probe, offset, target, target_class)) {
        ++matched;
        if (sample.empty())
          sample = target_class;
      }
    }
    // Requiring *every* probe to match was the mistake that produced "none": a
    // component with no attach parent (or an unreadable one) can never satisfy
    // it. Accept a majority instead and report how many matched, so a partial
    // answer still tells us something.
    if (matched >= 2 && matched * 2 >= probes.size())
      hits.push_back({offset, matched, sample});
  }
  std::sort(hits.begin(), hits.end(),
            [](const AttachHit &left, const AttachHit &right) {
              return left.hits > right.hits;
            });
  context.attach_offsets.clear();
  for (const auto &hit : hits)
    context.attach_offsets.emplace_back(hit.offset, hit.sample);
  std::string list;
  for (std::size_t i{}; i != hits.size() && i != 2; ++i) {
    char entry[64]{};
    std::snprintf(entry, sizeof(entry), "%s%X:%.16s %u/%zu",
                  i == 0 ? "" : " ", static_cast<unsigned>(hits[i].offset),
                  hits[i].sample.c_str(), static_cast<unsigned>(hits[i].hits),
                  probes.size());
    list += entry;
  }
  if (list.empty())
    list = "none";
  context.attach_parent_offset = hits.empty() ? 0U : hits[0].offset;
}

// Diagnostics go to the runtime log so they can be read without asking the user
// to transcribe (and OCR) a status line.
void LogDiagnostic(Context &context, const std::string &message) noexcept {
  if (context.core == nullptr || context.core->log == nullptr)
    return;
  context.core->log(context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    anomaly::sdk::StringView(message));
}

struct PoseDifference {
  double translation_cm{-1.0};
  double rotation_deg{-1.0};
  double scale{-1.0};
};

PoseDifference ComparePoseSample(const PackedTransform &value,
                                 const PackedTransform *reference) noexcept {
  PoseDifference result;
  if (reference == nullptr)
    return result;
  double translation_squared{}, scale_squared{};
  double dot{}, value_norm{}, reference_norm{};
  for (std::size_t axis{}; axis != 3; ++axis) {
    const double t = value.translation[axis] - reference->translation[axis];
    const double s = value.scale[axis] - reference->scale[axis];
    translation_squared += t * t;
    scale_squared += s * s;
  }
  for (std::size_t axis{}; axis != 4; ++axis) {
    dot += value.rotation[axis] * reference->rotation[axis];
    value_norm += value.rotation[axis] * value.rotation[axis];
    reference_norm += reference->rotation[axis] * reference->rotation[axis];
  }
  result.translation_cm = std::sqrt(translation_squared);
  result.scale = std::sqrt(scale_squared);
  const double norm = std::sqrt(value_norm * reference_norm);
  if (norm > 0.0 && std::isfinite(norm)) {
    const double cosine = (std::min)(1.0, std::abs(dot) / norm);
    result.rotation_deg = 2.0 * std::acos(cosine) * kCameraRadiansToDegrees;
  }
  return result;
}

// Samples the existing game-update boundary, not the rendering thread. The
// before-write sample can reveal changes since the last body-tick/update write;
// the after-write sample checks the two named non-root bones immediately.
void TraceExtraMeshPose(Context &context, const char *phase) noexcept {
  std::lock_guard<std::mutex> lock(context.extra_mesh_mutex);
  if (context.extra_mesh_owner != context.runtime.mesh)
    return;
  for (const auto &extra : context.extra_meshes) {
    if (extra.pose_probes.empty())
      continue;
    if (extra.poseable_active) {
      PackedTransform expected_relative{}, actual_relative{};
      std::memcpy(&expected_relative, extra.poseable_expected_relative.data(),
                  sizeof(expected_relative));
      std::array<std::uint8_t, 96> query{};
      std::string detail;
      const bool relative_ok = CallVirtualUFunction(
          context, extra.poseable_component, kFunctionSceneGetRelativeTransformPath,
          query.data(), query.size(), detail, &actual_relative);
      const auto relative_error = ComparePoseSample(
          actual_relative, relative_ok && extra.poseable_last_write_ok ? &expected_relative : nullptr);
      std::uint32_t read_count{};
      double max_cm{}, max_deg{}, max_scale{};
      for (std::uint32_t bone{}; bone != extra.bone_count; ++bone) {
        PackedTransform actual{}, expected{};
        if (!ReadBoneTransformForMesh(context, extra.poseable_component, bone, actual))
          continue;
        if (extra.poseable_expected_pose.size() != extra.bone_count)
          continue;
        std::memcpy(&expected, extra.poseable_expected_pose[bone].data(), sizeof(expected));
        const auto error = ComparePoseSample(actual, &expected);
        ++read_count;
        max_cm = (std::max)(max_cm, error.translation_cm);
        max_deg = (std::max)(max_deg, error.rotation_deg);
        max_scale = (std::max)(max_scale, error.scale);
      }
      char message[512]{};
      std::snprintf(message, sizeof(message),
                    "betterpose poseable verify component=%llX phase=%s write=%d "
                    "placement=%.4fcm/%.4fdeg/%.4fscale "
                    "bones=%u/%u pose_max=%.4fcm/%.4fdeg/%.4fscale "
                    "relative_t=%.3f,%.3f,%.3f dynamics=%d driven_bones=%zu length_error=%.6fcm",
                    static_cast<unsigned long long>(extra.poseable_component), phase,
                    extra.poseable_last_write_ok ? 1 : 0,
                    relative_error.translation_cm, relative_error.rotation_deg,
                    relative_error.scale, read_count, extra.bone_count,
                    max_cm, max_deg, max_scale, actual_relative.translation[0],
                    actual_relative.translation[1], actual_relative.translation[2],
                    extra.accessory_dynamics.Moving() ? 1 : 0,
                    extra.accessory_dynamics.DrivenBones(),
                    extra.accessory_dynamics.LengthError());
      LogDiagnostic(context, message);
      continue;
    }
    const std::array<std::uint32_t, 3> offsets{
        kMeshComponentSpaceBuffer0Offset, kMeshComponentSpaceBuffer1Offset,
        kMeshLocalSpaceTransformsOffset};
    std::array<std::uintptr_t, 3> arrays{};
    std::array<std::uint32_t, 3> counts{};
    std::array<bool, 3> readable{};
    for (std::size_t slot{}; slot != arrays.size(); ++slot) {
      readable[slot] = ReadArrayHeader(context, extra.object + offsets[slot],
                                        arrays[slot], counts[slot]) &&
                       arrays[slot] != 0 && counts[slot] == extra.bone_count;
    }
    char header[320]{};
    std::snprintf(header, sizeof(header),
                  "betterpose pose buffers %llX phase=%s "
                  "buffer0=%llX/%u buffer1=%llX/%u local=%llX/%u "
                  "write_targets=%llX,%llX",
                  static_cast<unsigned long long>(extra.object), phase,
                  static_cast<unsigned long long>(arrays[0]), counts[0],
                  static_cast<unsigned long long>(arrays[1]), counts[1],
                  static_cast<unsigned long long>(arrays[2]), counts[2],
                  static_cast<unsigned long long>(extra.bone_space_data),
                  static_cast<unsigned long long>(extra.component_space_data));
    LogDiagnostic(context, header);
    for (const auto &probe : extra.pose_probes) {
      PackedTransform expected{}, last0{}, api{};
      std::memcpy(&expected, probe.expected_component.data(), sizeof(expected));
      std::memcpy(&last0, probe.last_buffer0.data(), sizeof(last0));
      const bool api_ok =
          ReadBoneTransformForMesh(context, extra.object, probe.bone, api);
      const auto log_value = [&](const char *source, const bool ok,
                                 const PackedTransform &value,
                                 const PackedTransform *last_written) {
        const auto desired_delta = ComparePoseSample(
            value, ok && probe.has_expected ? &expected : nullptr);
        const auto api_delta = ComparePoseSample(value, ok && api_ok ? &api : nullptr);
        const auto last_delta = ComparePoseSample(value, ok ? last_written : nullptr);
        char line[640]{};
        std::snprintf(line, sizeof(line),
                      "betterpose pose sample %llX phase=%s bone=%u name=%s "
                      "source=%s ok=%d expected=%d writes=%d/%d "
                      "t=%.3f,%.3f,%.3f q=%.5f,%.5f,%.5f,%.5f "
                      "s=%.4f,%.4f,%.4f desired=%.4f/%.4f/%.4f "
                      "api=%.4f/%.4f/%.4f last=%.4f/%.4f/%.4f",
                      static_cast<unsigned long long>(extra.object), phase,
                      probe.bone, probe.name.c_str(), source, ok ? 1 : 0,
                      probe.has_expected ? 1 : 0, probe.write0_ok ? 1 : 0,
                      probe.write1_ok ? 1 : 0, value.translation[0],
                      value.translation[1], value.translation[2], value.rotation[0],
                      value.rotation[1], value.rotation[2], value.rotation[3],
                      value.scale[0], value.scale[1], value.scale[2],
                      desired_delta.translation_cm, desired_delta.rotation_deg,
                      desired_delta.scale, api_delta.translation_cm,
                      api_delta.rotation_deg, api_delta.scale,
                      last_delta.translation_cm, last_delta.rotation_deg,
                      last_delta.scale);
        LogDiagnostic(context, line);
      };
      log_value("api", api_ok, api, nullptr);
      const std::array<const char *, 3> labels{"buffer0", "buffer1", "local"};
      for (std::size_t slot{}; slot != arrays.size(); ++slot) {
        PackedTransform value{};
        const bool ok = readable[slot] &&
                        Read(context, arrays[slot] + probe.bone * kTransformSize, value);
        const PackedTransform *last_written = nullptr;
        if (probe.has_expected && slot == 0 && probe.write0_ok)
          last_written = &last0;
        else if (probe.has_expected && slot == 1 && probe.write1_ok)
          last_written = &expected;
        log_value(labels[slot], ok, value, last_written);
      }
    }
  }
}

void ResyncExtraMeshes(Context &context) noexcept {
  // Log the *state* even when this returns early: a silent no-op is what made the
  // previous test unreadable ("还是没有" told us nothing about which precondition
  // was missing).
  const std::uint32_t parent_offset = context.attach_parent_offset;
  if (parent_offset == 0)
    return;
  std::lock_guard<std::mutex> lock(context.extra_mesh_mutex);
  if (context.extra_mesh_owner != context.runtime.mesh)
    return;
  // Detection only. The old leader-pose setter experiment is off limits: it was
  // measured twice and posed the hair sideways before crashing the game.
  //   * SetMasterPoseComponent / SetLeaderPoseComponent, with force=true and
  //     again with the same leader and force=false -- re-asserting the leader
  //     re-evaluates the binding instead of merely refreshing the copy, i.e. it
  //     makes a persistent change to game state;
  //   * nudging an accessory's animation mode (away -> back, every frame or
  //     every 30), holding it in Custom, applying the pose before and after the
  //     tick, writing the follower's own two buffers, writing the body's true
  //     locals, writing the (empty) cached transform pair. None of them made a
  //     independently animated accessory follow, and two of them crashed the game.
  // The mode byte is left exactly as the game set it. What is left is to count how
  // many accessory components still have an attach parent.
  std::uint32_t attached{};
  for (const auto object : context.extra_targets) {
    std::uintptr_t parent{};
    if (Read(context, object + parent_offset, parent) && parent != 0)
      ++attached;
  }
  context.extra_resync_count = attached;
  // Report roughly every two seconds, plus immediately whenever the count
  // changes: the log is the only place I can read this from.
  if (++context.extra_resync_log_tick >= 120 ||
      attached != context.extra_resync_last) {
    context.extra_resync_log_tick = 0;
    context.extra_resync_last = attached;
    char state[192]{};
    std::snprintf(state, sizeof(state),
                  "betterpose attach state off %X targets %zu "
                  "motion %d pending %d names %zu owner %llX mesh %llX "
                  "scanowner %llX drops %u attached %u",
                  static_cast<unsigned>(parent_offset),
                  context.extra_targets.size(),
                  context.motion_loaded.load(std::memory_order_acquire) ? 1 : 0,
                  context.extra_build_pending ? 1 : 0,
                  context.bone_names.size(),
                  static_cast<unsigned long long>(context.extra_mesh_owner),
                  static_cast<unsigned long long>(context.runtime.mesh),
                  static_cast<unsigned long long>(context.mesh_scan_owner),
                  static_cast<unsigned>(context.extra_drop_count),
                  static_cast<unsigned>(attached));
    LogDiagnostic(context, state);
  }
}

void BuildExtraMeshes(Context &context) noexcept {
  // Never latch the result onto an empty mesh: runtime.mesh is zeroed on frames
  // where the local character cannot be resolved, and a build that ran on such a
  // frame stored owner = 0 while clearing the pending flag, which disabled the
  // attach resync permanently.
  if (context.runtime.mesh == 0) {
    context.extra_build_pending = true;
    return;
  }
  // Bone names are the mapping key, and they must belong to *this* mesh: after a
  // character switch the cached table is for the previous pawn.
  if (context.bone_names.empty() ||
      context.bone_names_mesh != context.runtime.mesh)
    MaybeRefreshBoneNames(context);
  if (context.bone_names.empty()) {
    // The bone table can only be read on the game thread and may simply not be
    // ready yet. Returning for good here left extra_mesh_owner unset while the
    // scan was already marked done, which silently disabled everything after it;
    // retry from Update instead.
    context.extra_build_pending = true;
    return;
  }
  std::vector<Context::ExtraMesh> built;
  std::uint32_t mapped_total{};
  std::uint32_t bone_total{};
  std::uint32_t position_total{};
  std::uint32_t bind_total{};
  std::string sample_name;
  for (const auto &candidate : context.mesh_scan_candidates) {
    const std::uintptr_t object = candidate.first;
    const std::uint32_t bone_count = candidate.second;
    if (bone_count == 0 || bone_count > 4096)
      continue;
    Context::ExtraMesh extra;
    extra.object = object;
    extra.bone_count = bone_count;
    std::uintptr_t component_data{};
    std::uint32_t component_count{};
    std::uintptr_t bone_data{};
    std::uint32_t bone_array_count{};
    if (!ReadArrayHeader(context, object + kMeshComponentSpaceBuffer1Offset,
                         component_data, component_count) ||
        !ReadArrayHeader(context, object + kMeshComponentSpaceBuffer0Offset,
                         bone_data, bone_array_count))
      continue;
    if (component_data == 0 || bone_data == 0 || component_count < bone_count ||
        bone_array_count < bone_count)
      continue;
    // Ignore poseable components created by an earlier plugin generation. They
    // are additive prototypes, not source accessories for a new mapping pass.
    const std::string candidate_class = ClassNameOf(context, object);
    if (candidate_class == "PoseableMeshComponent") {
      DestroyStalePoseableComponent(context, object);
      continue;
    }
    RestoreAccessoryVisibility(context, object);
    // Layout sanity check before we ever save or write this component: the first
    // transform must start with a plausible unit quaternion. It costs one read
    // and it is what keeps a mis-identified object from being scribbled on.
    std::array<double, 4> first_rotation{};
    if (!Read(context, component_data, first_rotation))
      continue;
    const double rotation_norm =
        std::sqrt(first_rotation[0] * first_rotation[0] +
                  first_rotation[1] * first_rotation[1] +
                  first_rotation[2] * first_rotation[2] +
                  first_rotation[3] * first_rotation[3]);
    if (rotation_norm < 0.9 || rotation_norm > 1.1)
      continue;
    extra.component_space_data = component_data;
    extra.bone_space_data = bone_data;
    extra.bone_fnames.assign(bone_count, std::array<std::uint8_t, 8>{});
    // Save the untouched buffers so unloading can put them back: without this
    // the components keep our last written pose and only a relog fixes them.
    const auto byte_count =
        static_cast<std::size_t>(bone_count) * kTransformSize;
    extra.saved_component.assign(byte_count, 0);
    extra.saved_bone.assign(byte_count, 0);
    // Read through the core service in one call: the single-value Read template
    // is sized by its argument, and these buffers are runtime sized.
    auto read_block = [&](const std::uintptr_t address,
                          std::vector<std::uint8_t> &destination) {
      if (!CoreReady(context.core) || address == 0)
        return false;
      AnomalyMutableByteSpanV1 span{destination.data(), destination.size()};
      return context.core->read_memory(context.core->user, address, span).code ==
             ANOMALY_STATUS_V1_OK;
    };
    if (!read_block(component_data, extra.saved_component) ||
        !read_block(bone_data, extra.saved_bone))
      continue;
    extra.bone_map.assign(bone_count, (std::numeric_limits<std::uint32_t>::max)());
    std::string first_name;
    std::vector<std::string> extra_names(bone_count);
    for (std::uint32_t bone{}; bone != bone_count; ++bone) {
      std::string name;
      if (!GetBoneFNameForMesh(context, object, bone, extra.bone_fnames[bone],
                               name) ||
          name.empty())
        continue;
      extra_names[bone] = name;
      if (bone == 0)
        first_name = name;
      ++bone_total;
      for (std::size_t body{}; body != context.bone_names.size(); ++body) {
        if (context.bone_names[body] == name) {
          extra.bone_map[bone] = static_cast<std::uint32_t>(body);
          ++mapped_total;
          break;
        }
      }
    }
    if (first_name.empty())
      first_name = "(noname)";
    if (sample_name.empty())
      sample_name = first_name;
    // Naming does not have to match: measure a positional mapping as well and
    // let the numbers decide which one to trust. Same skeleton layout means the
    // matching bones sit at nearly the same place in component space -- but only
    // while both are in the *same pose*. The extra components stop following the
    // moment we override the body, so a positional comparison taken then is
    // meaningless (measured 9.7 cm of pure pose error). Only use it when nothing
    // is being overridden.
    const bool in_sync =
        !context.motion_loaded.load(std::memory_order_acquire) &&
        !context.pose_override_enabled.load(std::memory_order_acquire);
    const std::uint32_t name_mapped = static_cast<std::uint32_t>(
        std::count_if(extra.bone_map.begin(), extra.bone_map.end(),
                      [](std::uint32_t value) {
                        return value !=
                               (std::numeric_limits<std::uint32_t>::max)();
                      }));
    std::vector<std::array<double, 3>> extra_positions;
    std::vector<std::array<double, 3>> body_positions;
    if (in_sync &&
        ReadBoneTranslations(context, component_data, bone_count,
                             extra_positions) &&
        ReadBoneTranslations(context, context.runtime.component_space_data,
                             context.runtime.component_space_count,
                             body_positions) &&
        !body_positions.empty()) {
      extra.position_map.assign(
          bone_count, (std::numeric_limits<std::uint32_t>::max)());
      double total_distance{};
      std::uint32_t matched{};
      for (std::uint32_t bone{}; bone != bone_count; ++bone) {
        double best = 1e30;
        std::uint32_t best_index{};
        for (std::size_t body{}; body != body_positions.size(); ++body) {
          const double dx = extra_positions[bone][0] - body_positions[body][0];
          const double dy = extra_positions[bone][1] - body_positions[body][1];
          const double dz = extra_positions[bone][2] - body_positions[body][2];
          const double distance = dx * dx + dy * dy + dz * dz;
          if (distance < best) {
            best = distance;
            best_index = static_cast<std::uint32_t>(body);
          }
        }
        if (best < 1e29) {
          extra.position_map[bone] = best_index;
          total_distance += std::sqrt(best);
          ++matched;
        }
      }
      if (matched != 0)
        extra.position_match_cm = total_distance / matched;
    }
    const std::uint32_t position_mapped =
        extra.position_map.empty()
            ? 0U
            : static_cast<std::uint32_t>(std::count_if(
                  extra.position_map.begin(), extra.position_map.end(),
                  [](std::uint32_t value) {
                    return value != (std::numeric_limits<std::uint32_t>::max)();
                  }));
    // Bind-pose mapping: compare the two skeletons in *bind* pose, which is the same
    // idea as position_map but independent of the current pose. That matters twice
    // here: we are usually overriding the body by the time a scan runs, and a mesh we
    // never drove sits frozen in an older pose, so comparing live positions measures
    // the pose difference (measured 16.2 cm mean on one character) rather than the
    // layout. Two corresponding bones sit within millimetres of each other in bind
    // pose, so kExtraMeshBindCm is a real discriminator.
    extra.bind_map.assign(bone_count, (std::numeric_limits<std::uint32_t>::max)());
    std::uint32_t bind_mapped{};
    double bind_distance{};
    std::uintptr_t extra_asset{};
    for (std::uint32_t offset{}; offset + 8 <= 0x2000; offset += 8) {
      std::uintptr_t asset_candidate{};
      if (!ReadPointerAt(context, object, offset, asset_candidate) ||
          asset_candidate == 0)
        continue;
      const std::string asset_class = ClassNameOf(context, asset_candidate);
      if (asset_class.find("SkeletalMesh") == std::string::npos ||
          asset_class.find("Component") != std::string::npos)
        continue;
      extra_asset = asset_candidate;
      break;
    }
    extra.asset = extra_asset;
    if (extra_asset != 0 && !context.ref_locals.empty() &&
        context.bone_parents.size() == context.ref_locals.size()) {
      std::vector<std::array<double, 12>> extra_locals;
      std::vector<std::int32_t> extra_parents;
      if (ReadAssetBindSkeleton(context, extra_asset, object, bone_count,
                                extra_locals, extra_parents)) {
        const auto bind_body = BindPosePositions(context.ref_locals,
                                                 context.bone_parents);
        const auto bind_extra = BindPosePositions(extra_locals, extra_parents);
        for (std::uint32_t bone{}; bone != bone_count; ++bone) {
          double best = kExtraMeshBindCm;
          std::uint32_t best_index = (std::numeric_limits<std::uint32_t>::max)();
          for (std::size_t body{}; body != bind_body.size(); ++body) {
            const double dx = bind_extra[bone][0] - bind_body[body][0];
            const double dy = bind_extra[bone][1] - bind_body[body][1];
            const double dz = bind_extra[bone][2] - bind_body[body][2];
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance < best) {
              best = distance;
              best_index = static_cast<std::uint32_t>(body);
            }
          }
          if (best_index != (std::numeric_limits<std::uint32_t>::max)()) {
            extra.bind_map[bone] = best_index;
            ++bind_mapped;
            bind_distance += best;
          }
        }
        if (bind_mapped != 0)
          extra.bind_match_cm = bind_distance / bind_mapped;
      }
    }
    // What are these meshes and how did each mapping do? Only a log can answer that
    // after a scan, and the bone names decide what is mappable at all.
    {
      char summary[512]{};
      std::snprintf(summary, sizeof(summary),
                    "betterpose extra mesh %llX asset %s class %s bones %u "
                    "name %u bind %u bindmean %.2fcm pos %u posmean %.1fcm",
                    static_cast<unsigned long long>(object),
                    extra_asset != 0 ? ObjectNameOf(context, extra_asset).c_str()
                                     : "-",
                    extra_asset != 0 ? ClassNameOf(context, extra_asset).c_str()
                                     : "-",
                    static_cast<unsigned>(bone_count),
                    static_cast<unsigned>(name_mapped),
                    static_cast<unsigned>(bind_mapped), extra.bind_match_cm,
                    static_cast<unsigned>(position_mapped),
                    extra.position_match_cm);
      LogDiagnostic(context, summary);
      for (std::uint32_t start{}; start < bone_count; start += 12) {
        std::string line =
            "betterpose extra mesh bones " + std::to_string(start) + ":";
        for (std::uint32_t bone = start; bone < bone_count && bone < start + 12;
             ++bone) {
          std::string name;
          if (!GetBoneNameForMesh(context, object, bone, name))
            name = "?";
          line += " ";
          line += name;
        }
        LogDiagnostic(context, line);
      }
    }
    // Prefer names when they mostly line up, then the bind-pose match, otherwise the
    // live positions -- but only when they really are close, since a bad mapping is
    // worse than none.
    const bool name_ok = name_mapped * 2 >= bone_count;
    const bool bind_ok = bind_mapped * 2 >= bone_count &&
                         extra.bind_match_cm > 0.0 &&
                         extra.bind_match_cm <= 0.5;
    const bool position_ok = position_mapped * 2 >= bone_count &&
                             extra.position_match_cm > 0.0 &&
                             extra.position_match_cm <= kExtraMeshMatchCm;
    if (!name_ok && !bind_ok && !position_ok) {
      // Last resort: drive the mesh through its own hierarchy. It only needs the
      // mesh's own bind skeleton plus at least two attach bones that name a body
      // part -- which is exactly how a separately authored hair mesh is built
      // (head_adjust and friends next to its own Bone_hairBR00..06 chains).
      bool drivable = false;
      if (extra_asset != 0) {
        std::vector<std::array<double, 12>> blend_locals;
        std::vector<std::int32_t> blend_parents;
        if (ReadAssetBindSkeleton(context, extra_asset, object, bone_count,
                                  blend_locals, blend_parents)) {
          std::vector<std::uint32_t> hierarchy_map(
              bone_count, (std::numeric_limits<std::uint32_t>::max)());
          std::vector<std::string> pairs;
          std::uint32_t matched{};
          for (std::uint32_t bone{}; bone != bone_count; ++bone) {
            std::string bone_name;
            if (!GetBoneNameForMesh(context, object, bone, bone_name))
              continue;
            const std::uint32_t body = MatchBodyBone(context, bone_name);
            if (body == (std::numeric_limits<std::uint32_t>::max)() ||
                body >= context.bone_names.size())
              continue;
            hierarchy_map[bone] = body;
            ++matched;
            if (pairs.size() < 8)
              pairs.push_back(bone_name + "->" + context.bone_names[body]);
          }
          // The attach bone with the most descendants is the one the chains hang
          // off (head_adjust carries all four hair chains), so that is the anchor.
          std::uint32_t anchor = (std::numeric_limits<std::uint32_t>::max)();
          std::uint32_t anchor_subtree{};
          for (std::uint32_t bone{}; bone != bone_count; ++bone) {
            if (hierarchy_map[bone] == (std::numeric_limits<std::uint32_t>::max)())
              continue;
            std::uint32_t size{};
            for (std::uint32_t other{}; other != bone_count; ++other) {
              std::int32_t walk = blend_parents[other];
              for (std::uint32_t guard{}; walk >= 0 && guard != bone_count;
                   ++guard) {
                if (static_cast<std::uint32_t>(walk) == bone) {
                  ++size;
                  break;
                }
                walk = blend_parents[static_cast<std::size_t>(walk)];
              }
            }
            if (anchor == (std::numeric_limits<std::uint32_t>::max)() ||
                size > anchor_subtree) {
              anchor = bone;
              anchor_subtree = size;
            }
          }
          if (matched >= 2 && anchor != (std::numeric_limits<std::uint32_t>::max)()) {
            extra.bone_map = std::move(hierarchy_map);
            extra.parents = std::move(blend_parents);
            extra.bind_locals = std::move(blend_locals);
            const auto bind_world =
                BindPoseTransforms(extra.bind_locals, extra.parents);
            extra.bind_world.assign(bind_world.size(), std::array<double, 12>{});
            for (std::size_t index{}; index != bind_world.size(); ++index) {
              PackedTransform packed_bind{};
              PackTransform(bind_world[index], packed_bind);
              const auto *raw = reinterpret_cast<const double *>(&packed_bind);
              for (std::size_t k{}; k != 12; ++k)
                extra.bind_world[index][k] = raw[k];
            }
            extra.lag.assign(bone_count, std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
            extra.anchor_extra = anchor;
            extra.anchor_body = extra.bone_map[anchor];
            extra.used_hierarchy = true;
            drivable = true;
            mapped_total += matched;
            std::string joined;
            for (const auto &pair : pairs) {
              joined += " ";
              joined += pair;
            }
            LogDiagnostic(context, "betterpose extra mesh hierarchy " +
                                       std::to_string(object) + " matched " +
                                       std::to_string(matched) + "/" +
                                       std::to_string(bone_count) + " anchor " +
                                       std::to_string(anchor) + "->" +
                                       std::to_string(extra.anchor_body) + joined);
          }
        }
      }
      if (!drivable) {
        position_total += position_mapped;
        continue;
      }
    }
    if (!name_ok && bind_ok) {
      extra.bone_map = extra.bind_map;
      extra.used_bind = true;
      bind_total += bind_mapped;
      mapped_total = mapped_total - name_mapped + bind_mapped;
    } else if (!name_ok && !extra.used_hierarchy) {
      extra.bone_map = extra.position_map;
      extra.used_position = true;
      position_total += position_mapped;
      mapped_total = mapped_total - name_mapped + position_mapped;
    }
    // Diagnostic samples only; these names do not change the driving map.
    const auto add_probe = [&](std::string_view name) {
      const auto found = std::find(extra_names.begin(), extra_names.end(), name);
      if (found == extra_names.end())
        return false;
      Context::ExtraMesh::PoseProbe probe;
      probe.bone = static_cast<std::uint32_t>(found - extra_names.begin());
      probe.name = *found;
      extra.pose_probes.push_back(std::move(probe));
      return true;
    };
    if (!add_probe("pelvis_adjust"))
      static_cast<void>(add_probe("head_adjust"));
    if (!add_probe("Bone_hairBR00"))
      static_cast<void>(add_probe("B_ribbon_00"));
    if (extra.used_hierarchy) {
      const bool configured = extra.accessory_dynamics.Configure(extra.bind_world, extra.parents, extra_names);
      LogDiagnostic(context, "betterpose accessory dynamics source=" + Hex(extra.object) +
                             " rules=body-secondary configured=" + std::to_string(configured) + " driven_bones=" +
                             std::to_string(extra.accessory_dynamics.DrivenBones()));
    }
    built.push_back(std::move(extra));
  }
  {
    // Put any previously written components back before replacing the set: a
    // rescan that ends up skipping a component must not leave our pose in it.
    static_cast<void>(RestoreExtraMeshes(context));
    std::lock_guard<std::mutex> lock(context.extra_mesh_mutex);
    context.extra_meshes = std::move(built);
    context.extra_mesh_owner = context.runtime.mesh;
    context.extra_mesh_mapped = mapped_total;
    context.extra_targets.clear();
    for (const auto &candidate : context.mesh_scan_candidates)
      context.extra_targets.push_back(candidate.first);
    context.extra_build_pending = false;
  }
}

void WriteExtraMeshes(Context &context,
                      const std::vector<PackedTransform> &packed) noexcept {
  std::lock_guard<std::mutex> lock(context.extra_mesh_mutex);
  if (context.extra_meshes.empty() ||
      context.extra_mesh_owner != context.runtime.mesh)
    return;
  std::vector<PackedTransform> out;
  // The body's bind pose, needed to turn the anchor's current transform into a delta.
  // Read once per call and only when some mesh actually needs it.
  std::vector<Transformd> body_bind;
  for (const auto &extra : context.extra_meshes) {
    if (extra.used_hierarchy && !context.ref_locals.empty() &&
        context.bone_parents.size() == context.ref_locals.size()) {
      body_bind = BindPoseTransforms(context.ref_locals, context.bone_parents);
      break;
    }
  }
  for (auto &extra : context.extra_meshes) {
    if (extra.component_space_data == 0 || extra.bone_count == 0)
      continue;
    if (context.poseable_prototype_enabled && extra.used_hierarchy) {
      const bool ready = EnsurePoseableAccessory(context, extra);
      if (ready && !extra.accessory_dynamics.CollisionsConfigured() &&
          extra.poseable_socket_bone < body_bind.size()) {
        std::vector<better_pose::accessory::Bone> collision_bind(body_bind.size());
        for (std::size_t i{}; i != body_bind.size(); ++i) {
          PackedTransform bone{};
          PackTransform(body_bind[i], bone);
          std::memcpy(collision_bind[i].data(), &bone, sizeof(bone));
        }
        PackedTransform authored{}, bind_placement{};
        std::memcpy(&authored, extra.poseable_socket_relative.data(), sizeof(authored));
        PackTransform(TransformMultiply(body_bind[extra.poseable_socket_bone],
                                        UnpackTransform(authored)), bind_placement);
        better_pose::accessory::Bone placement_raw{};
        std::memcpy(placement_raw.data(), &bind_placement, sizeof(bind_placement));
        extra.accessory_dynamics.ConfigureCollisions(collision_bind, context.bone_names,
            better_pose::accessory::Transform(placement_raw), context.bone_names[extra.poseable_socket_bone]);
      }
      extra.poseable_last_write_ok = ready && DrivePoseableSocketPose(context, extra, packed);
      // The prototype owns the replacement only. Do not deform the hidden
      // source as well, or reapply a second body delta to individual hair bones.
      continue;
    }
    if (extra.used_hierarchy && extra.bind_locals.size() == extra.bone_count &&
        extra.parents.size() == extra.bone_count &&
        extra.bind_world.size() == extra.bone_count &&
        extra.lag.size() == extra.bone_count) {
      // Modular mesh: hang the whole thing off the body bone its attach bone belongs
      // to, and let each bone below that trail behind its rigid orientation. Uniform
      // lag still accumulates down a chain, which is what makes a tail whip.
      // Per-bone driving replaced the single anchor delta: each attach bone follows
      // its own body bone, so the hair follows the head rather than the whole mesh
      // following whichever attach bone had the most descendants.
      out.assign(extra.bone_count, PackedTransform{});
       // Both destinations are component-space buffers. The engine selects one
       // with CurrentReadIndex, so both must receive the same component-space
       // pose. locals_out remains a diagnostic reference for the separately
       // authored hierarchy, but it is not written into a render buffer.
      std::vector<PackedTransform> locals_out(extra.bone_count);
      std::vector<Transformd> world(extra.bone_count);
      for (std::uint32_t bone{}; bone != extra.bone_count; ++bone) {
        Transformd bind_world;
        bind_world.rotation =
            Quatd{extra.bind_world[bone][0], extra.bind_world[bone][1],
                  extra.bind_world[bone][2], extra.bind_world[bone][3]};
        bind_world.translation =
            Vec3d{extra.bind_world[bone][4], extra.bind_world[bone][5],
                  extra.bind_world[bone][6]};
        bind_world.scale = Vec3d{extra.bind_world[bone][8], extra.bind_world[bone][9],
                                 extra.bind_world[bone][10]};
        const std::uint32_t mapped = extra.bone_map[bone];
        Transformd want;
        if (mapped != (std::numeric_limits<std::uint32_t>::max)() &&
            mapped < body_bind.size() && mapped < packed.size()) {
          // An attach bone: follow the body bone it belongs to by that bone's own
          // delta since bind, applied to this bone's bind transform. Applying the
          // delta (not the body's transform) is what keeps the mesh's own shape --
          // the body bone sits somewhere else.
          const Transformd delta = TransformMultiply(
              UnpackTransform(packed[mapped]),
              TransformInverse(body_bind[mapped]));
          want = TransformMultiply(delta, bind_world);
        } else {
          // Inside the mesh: follow the parent, trailing behind the rigid pose. The
          // lag is what makes a hair chain swing instead of moving like a plank.
          const std::int32_t parent = extra.parents[bone];
          Transformd local;
          local.rotation = Quatd{extra.bind_locals[bone][0], extra.bind_locals[bone][1],
                                 extra.bind_locals[bone][2], extra.bind_locals[bone][3]};
          local.translation =
              Vec3d{extra.bind_locals[bone][4], extra.bind_locals[bone][5],
                    extra.bind_locals[bone][6]};
          local.scale = Vec3d{extra.bind_locals[bone][8], extra.bind_locals[bone][9],
                              extra.bind_locals[bone][10]};
          if (parent >= 0 && static_cast<std::uint32_t>(parent) < bone) {
            const Quatd previous{extra.lag[bone][0], extra.lag[bone][1],
                                 extra.lag[bone][2], extra.lag[bone][3]};
            const Quatd lagged = QuatBlend(previous, local.rotation, 0.35);
            extra.lag[bone] = {lagged.x, lagged.y, lagged.z, lagged.w};
            local.rotation = lagged;
            want = TransformMultiply(world[static_cast<std::size_t>(parent)], local);
          } else {
            want = local;
          }
        }
        world[bone] = want;
        PackTransform(want, out[bone]);
        const std::int32_t up = extra.parents[bone];
        if (up >= 0 && static_cast<std::uint32_t>(up) < bone)
          PackTransform(TransformMultiply(
                            TransformInverse(world[static_cast<std::size_t>(up)]),
                            want),
                        locals_out[bone]);
        else
          PackTransform(want, locals_out[bone]);
      }
      extra.lag_ready = true;
      extra.buffers_modified = true;
      const auto *mesh_bytes = reinterpret_cast<const std::uint8_t *>(out.data());
      const auto mesh_size = out.size() * sizeof(PackedTransform);
      const bool wrote_component =
          WriteBytes(context, extra.component_space_data, mesh_bytes, mesh_size);
       const bool wrote_bone =
           WriteBytes(context, extra.bone_space_data, mesh_bytes, mesh_size);
       static_cast<void>(ForceMeshObjectUpdate(context, extra.object));
      for (auto &probe : extra.pose_probes) {
        std::memcpy(probe.expected_component.data(), &out[probe.bone],
                    sizeof(PackedTransform));
         std::memcpy(probe.last_buffer0.data(), &out[probe.bone],
                     sizeof(PackedTransform));
        probe.write0_ok = wrote_bone;
        probe.write1_ok = wrote_component;
        probe.has_expected = true;
      }
      continue;
    }
    out.assign(extra.bone_count, PackedTransform{});
    for (std::uint32_t bone{}; bone != extra.bone_count; ++bone) {
      const std::uint32_t source =
          bone < extra.bone_map.size()
              ? extra.bone_map[bone]
              : (std::numeric_limits<std::uint32_t>::max)();
      if (source < packed.size()) {
        out[bone] = packed[source];
      } else if (extra.saved_component.size() >=
                 (static_cast<std::size_t>(bone) + 1) * kTransformSize) {
        // Unmapped bone: keep whatever it had, so nothing collapses.
        std::memcpy(&out[bone],
                    extra.saved_component.data() +
                        static_cast<std::size_t>(bone) * kTransformSize,
                    sizeof(PackedTransform));
      }
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(out.data());
    const auto byte_count = out.size() * sizeof(PackedTransform);
    extra.buffers_modified = true;
    static_cast<void>(
        WriteBytes(context, extra.component_space_data, bytes, byte_count));
    static_cast<void>(
        WriteBytes(context, extra.bone_space_data, bytes, byte_count));
    // Same as the hierarchy path: the renderer only picks the buffer up when the
    // component's update flag is set.
    static_cast<void>(ForceMeshObjectUpdate(context, extra.object));
  }
}

bool RestoreExtraMeshes(Context &context) noexcept {
  std::lock_guard<std::mutex> lock(context.extra_mesh_mutex);
  DestroyPoseableAccessories(context);
  for (auto &extra : context.extra_meshes) {
    if (extra.buffers_modified && extra.component_space_data != 0 && !extra.saved_component.empty())
      static_cast<void>(WriteBytes(context, extra.component_space_data,
                                   extra.saved_component.data(),
                                   extra.saved_component.size()));
    if (extra.buffers_modified && extra.bone_space_data != 0 && !extra.saved_bone.empty())
      static_cast<void>(WriteBytes(context, extra.bone_space_data,
                                   extra.saved_bone.data(),
                                   extra.saved_bone.size()));
    extra.buffers_modified = false;
    for (auto &probe : extra.pose_probes) {
      probe.has_expected = false;
      probe.write0_ok = false;
      probe.write1_ok = false;
    }
  }
  return true;
}

bool DropExtraMeshes(Context &context) noexcept {  // Put the original buffers back before forgetting about them: dropping them
  // while our pose is still in place is what left a character stuck until a
  // relog.
  static_cast<void>(RestoreExtraMeshes(context));
  std::lock_guard<std::mutex> lock(context.extra_mesh_mutex);
  context.extra_meshes.clear();
  ++context.extra_drop_count;
  context.extra_mesh_owner = 0;
  context.extra_mesh_mapped = 0;
  return true;
}

bool RefreshBoneNames(Context &context, std::vector<std::string> &names,
                      std::string &detail) noexcept {
  names.clear();
  detail.clear();
  std::int32_t count{};
  if (!GetBoneCount(context, count, detail))
    return false;
  if (count <= 0 || count > 8192) {
    detail = "GetNumBones returned an invalid count";
    return false;
  }
  names.reserve(static_cast<std::size_t>(count));
  for (std::int32_t index{}; index != count; ++index) {
    std::string name;
    static_cast<void>(GetBoneName(context, static_cast<std::uint32_t>(index),
                                  name));
    names.push_back(std::move(name));
  }
  detail = "loaded " + std::to_string(count) + " bone names";
  return true;
}

void MaybeRefreshBoneNames(Context &context) noexcept {
  const auto mesh = context.runtime.mesh;
  const auto count = context.runtime.bone_space_count;
  if (mesh == 0 || count == 0)
    return;
  if (!context.bone_names.empty() && context.bone_names_mesh == mesh &&
      context.bone_names_count == count)
    return;
  if (context.bone_names_attempted && context.bone_names_mesh == mesh &&
      context.bone_names_count == count)
    return;
  context.bone_names_attempted = true;
  std::string detail;
  std::vector<std::string> names;
  if (!RefreshBoneNames(context, names, detail)) {
    context.bone_names.clear();
    return;
  }
  context.bone_names = std::move(names);
  context.bone_names_mesh = mesh;
  context.bone_names_count = count;
  RefreshBoneHierarchy(context);
}

bool ForcePoseCache(Context &context, std::string &detail) noexcept {
  detail.clear();
  if (context.runtime.mesh == 0) {
    detail = "local mesh is unavailable";
    return false;
  }

  std::array<std::uint8_t, 12> bone_name_parameters{};
  const std::int32_t bone_index = 0;
  std::memcpy(bone_name_parameters.data(), &bone_index, sizeof(bone_index));
  std::array<std::uint8_t, 12> bone_name_output{};
  if (!CallVirtualUFunction(context, context.runtime.mesh,
                            kFunctionGetBoneNamePath,
                            bone_name_parameters.data(),
                            bone_name_parameters.size(), detail,
                            bone_name_output.data())) {
    detail.insert(0, "GetBoneName failed: ");
    return false;
  }

  std::array<std::uint8_t, 112> bone_transform_parameters{};
  std::memcpy(bone_transform_parameters.data(), bone_name_output.data() + 4,
              sizeof(std::uint64_t));
  const std::uint8_t transform_space = 2;
  bone_transform_parameters[8] = transform_space;
  std::array<std::uint8_t, 112> bone_transform_output{};
  if (!CallVirtualUFunction(context, context.runtime.mesh,
                            kFunctionGetBoneTransformPath,
                            bone_transform_parameters.data(),
                            bone_transform_parameters.size(), detail,
                            bone_transform_output.data())) {
    detail.insert(0, "GetBoneTransform failed: ");
    return false;
  }

  detail = "pose cache refreshed (bone 0)";
  return true;
}

bool EnsurePoseForcedLod(Context &context, const bool enabled) noexcept {
  RuntimeState &state = context.runtime;
  if (state.mesh == 0)
    return false;
  if (!enabled) {
    if (state.saved_forced_lod && state.forced_lod_applied) {
      std::string detail;
      const std::int32_t original = state.original_forced_lod;
      if (CallVirtualUFunction(context, state.mesh, kFunctionSetForcedLodPath,
                               &original, sizeof(original), detail)) {
        state.forced_lod_applied = false;
        state.saved_forced_lod = false;
        state.original_forced_lod = 0;
      }
    } else {
      state.saved_forced_lod = false;
      state.forced_lod_applied = false;
      state.original_forced_lod = 0;
    }
    return true;
  }
  if (!state.saved_forced_lod) {
    std::int32_t original{};
    if (!Read(context, state.mesh + kMeshForcedLodModelOffset, original))
      return false;
    state.original_forced_lod = original;
    state.saved_forced_lod = true;
    state.forced_lod_applied = false;
  }
  if (state.forced_lod_applied)
    return true;
  const std::int32_t high_lod = 0;
  std::string detail;
  if (!CallVirtualUFunction(context, state.mesh, kFunctionSetForcedLodPath,
                            &high_lod, sizeof(high_lod), detail))
    return false;
  state.forced_lod_applied = true;
  return true;
}

bool EnsurePoseAnimationMode(Context &context, const bool enabled) noexcept {
  RuntimeState &state = context.runtime;
  if (state.mesh == 0)
    return false;
  if (!enabled) {
    if (state.saved_animation_mode && state.animation_mode_applied) {
      const std::uint8_t original = state.original_animation_mode;
      if (!Write(context, state.mesh + kMeshAnimationModeOffset, original))
        return false;
      state.animation_mode_applied = false;
      state.saved_animation_mode = false;
      state.original_animation_mode = 0;
    } else {
      state.saved_animation_mode = false;
      state.animation_mode_applied = false;
      state.original_animation_mode = 0;
    }
    return true;
  }
  if (!state.saved_animation_mode) {
    std::uint8_t original{};
    if (!Read(context, state.mesh + kMeshAnimationModeOffset, original))
      return false;
    state.original_animation_mode = original;
    state.saved_animation_mode = true;
    state.animation_mode_applied = false;
  }
  if (state.animation_mode_applied)
    return true;

  struct SetAnimationModeParameters {
    std::uint8_t mode;
    std::uint8_t force_init;
  };
  const SetAnimationModeParameters parameters{kAnimationModeCustom, 0};
  std::string detail;
  if (CallVirtualUFunction(context, state.mesh, kFunctionSetAnimationModePath,
                           &parameters, sizeof(parameters), detail)) {
    state.animation_mode_applied = true;
    return true;
  }

  // SetAnimationMode can be missing or overridden in stripped builds. Fall back
  // to the direct property write, which is sufficient when no AnimInstance
  // reinitialization is requested.
  const std::uint8_t custom = kAnimationModeCustom;
  if (!Write(context, state.mesh + kMeshAnimationModeOffset, custom))
    return false;
  state.animation_mode_applied = true;
  return true;
}

void SetReflectionStatus(Context &context, const std::string_view message) {
  std::scoped_lock lock(context.state_mutex);
  context.reflection_status.assign(message.data(), message.size());
}

std::wstring Utf8ToWide(const std::string_view value) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    return {};
  const int required = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0);
  if (required <= 0)
    return {};
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(),
                          required) != required)
    return {};
  return result;
}

std::string WideToUtf8(const std::wstring_view value) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    return {};
  const int required = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0)
    return {};
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), required,
                          nullptr, nullptr) != required)
    return {};
  return result;
}

class ComApartment final {
public:
  ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComApartment() {
    if (SUCCEEDED(result_))
      CoUninitialize();
  }
  [[nodiscard]] bool Usable() const noexcept {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }
private:
  HRESULT result_{};
};

std::optional<std::filesystem::path> ChooseFolder(
    const std::string_view current_utf8) {
  ComApartment apartment;
  if (!apartment.Usable())
    return std::nullopt;
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog))))
    return std::nullopt;
  DWORD options{};
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    static_cast<void>(dialog->SetOptions(options | FOS_PICKFOLDERS |
                                         FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST));
  }
  if (!current_utf8.empty()) {
    const std::wstring current = Utf8ToWide(current_utf8);
    if (!current.empty()) {
      ComPtr<IShellItem> folder;
      if (SUCCEEDED(SHCreateItemFromParsingName(
              current.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        static_cast<void>(dialog->SetFolder(folder.Get()));
    }
  }
  if (FAILED(dialog->Show(nullptr)))
    return std::nullopt;
  ComPtr<IShellItem> selected;
  if (FAILED(dialog->GetResult(&selected)))
    return std::nullopt;
  PWSTR raw{};
  if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr)
    return std::nullopt;
  std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  return result;
}

// What a file picker is for: it only changes the filter list it opens with.
enum class FileKind { Json, Vmd, Audio, Motion };

// A chosen motion path is either a source VMD, which has to be converted, or a document that was
// converted earlier, which must not go through the converter again.
bool PathIsConvertedMotion(const std::string &path) noexcept {
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return false;
  std::string extension = path.substr(dot);
  for (char &c : extension)
    c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  return extension == ".json";
}

std::optional<std::filesystem::path> ChooseFile(
    const std::string_view current_utf8, const FileKind kind = FileKind::Json) {
  ComApartment apartment;
  if (!apartment.Usable())
    return std::nullopt;
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog))))
    return std::nullopt;
  DWORD options{};
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    static_cast<void>(dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
                                         FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST));
  }
  COMDLG_FILTERSPEC json_filters[] = {
      {L"JSON", L"*.json"},
      {L"All Files", L"*.*"},
  };
  // The VMD picker opens on the motion filter, since that is the only file it can use.
  COMDLG_FILTERSPEC vmd_filters[] = {
      {L"MMD motion (VMD)", L"*.vmd"},
      {L"All Files", L"*.*"},
  };
  COMDLG_FILTERSPEC audio_filters[] = {
      {L"Audio (mp3/wav/m4a/ogg)", L"*.mp3;*.wav;*.m4a;*.aac;*.ogg;*.wma;*.flac"},
      {L"All Files", L"*.*"},
  };
  // The motion picker takes either a source VMD or an already-converted document, so a converted
  // motion can be handed around without the VMD and the reference table it was built from.
  COMDLG_FILTERSPEC motion_filters[] = {
      {L"MMD motion (VMD) or converted motion (JSON)", L"*.vmd;*.json"},
      {L"MMD motion (VMD)", L"*.vmd"},
      {L"Converted motion (JSON)", L"*.json"},
      {L"All Files", L"*.*"},
  };
  const COMDLG_FILTERSPEC *filters = json_filters;
  UINT count = ARRAYSIZE(json_filters);
  if (kind == FileKind::Vmd) {
    filters = vmd_filters;
    count = ARRAYSIZE(vmd_filters);
  } else if (kind == FileKind::Audio) {
    filters = audio_filters;
    count = ARRAYSIZE(audio_filters);
  } else if (kind == FileKind::Motion) {
    filters = motion_filters;
    count = ARRAYSIZE(motion_filters);
  }
  static_cast<void>(dialog->SetFileTypes(count, filters));
  if (!current_utf8.empty()) {
    const std::wstring current = Utf8ToWide(current_utf8);
    if (!current.empty()) {
      ComPtr<IShellItem> folder;
      if (SUCCEEDED(SHCreateItemFromParsingName(
              current.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        static_cast<void>(dialog->SetFolder(folder.Get()));
    }
  }
  if (FAILED(dialog->Show(nullptr)))
    return std::nullopt;
  ComPtr<IShellItem> selected;
  if (FAILED(dialog->GetResult(&selected)))
    return std::nullopt;
  PWSTR raw{};
  if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr)
    return std::nullopt;
  std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  return result;
}

// ---- MP3 frame table: exact time <-> byte ----------------------------------------------------
//
// MCI builds its time<->byte model from the *first* frame's bitrate, and on an MP3 whose first frame
// is the Xing/Info header that is not the audio's rate: 爱言叶4 .mp3 carries an Info frame declaring
// 64 kbps in front of 9698 frames of 128 kbps, so the device reports the 232.8 s track as 465.6 s and
// `play ... from 200000` starts about 100 s of audio in. That is the "the further in, the further
// behind" report -- and it is why a re-encoded CBR copy of the same song seeks correctly.
//
// The device also refuses `time format bytes` (MCIERR 282) on this machine, so the only lever left is
// the number handed to `play from`: turn the requested time into an exact byte offset with the table
// below, then express it in the device's own time base using the rate it believes in (file size /
// the length it reports). Verified outside the game -- seeking to 230 s plays the last 2.8 s (3.05 s
// measured) where the raw 230000 ms request was still playing after 12 s, and a CBR file, whose first
// frame *is* the audio's rate, behaves identically either way (2.96 s vs 3.06 s).
constexpr std::size_t kMp3MaxFrames = 2000000;
constexpr std::uintmax_t kMp3MaxBytes = 256ull * 1024ull * 1024ull;

struct Mp3FrameTable {
  std::vector<std::uint64_t> byte;  // start of each frame
  std::vector<double> time;         // its start time in seconds
  double total_seconds = 0.0;
  std::uint64_t file_bytes = 0;
  bool valid = false;

  // Linear inside a frame (24 ms at 48 kHz), which is finer than anything the device resolves.
  double TimeForByte(const double offset) const {
    if (!valid || byte.size() < 2)
      return 0.0;
    if (offset <= static_cast<double>(byte.front()))
      return time.front();
    if (offset >= static_cast<double>(byte.back()))
      return total_seconds;
    std::size_t low = 0;
    std::size_t high = byte.size() - 1;
    while (low < high) {
      const std::size_t mid = (low + high + 1) / 2;
      if (static_cast<double>(byte[mid]) <= offset)
        low = mid;
      else
        high = mid - 1;
    }
    const double span = static_cast<double>(byte[low + 1] - byte[low]);
    const double fraction = span > 0.0 ? (offset - static_cast<double>(byte[low])) / span : 0.0;
    return time[low] + fraction * (time[low + 1] - time[low]);
  }

  double ByteForTime(const double seconds) const {
    if (!valid || byte.size() < 2)
      return 0.0;
    if (seconds <= time.front())
      return static_cast<double>(byte.front());
    if (seconds >= total_seconds)
      return static_cast<double>(file_bytes);
    std::size_t low = 0;
    std::size_t high = time.size() - 1;
    while (low < high) {
      const std::size_t mid = (low + high + 1) / 2;
      if (time[mid] <= seconds)
        low = mid;
      else
        high = mid - 1;
    }
    const double span = time[low + 1] - time[low];
    const double fraction = span > 1e-9 ? (seconds - time[low]) / span : 0.0;
    return static_cast<double>(byte[low]) +
           fraction * static_cast<double>(byte[low + 1] - byte[low]);
  }
};

// Walks the MPEG audio frames of an MP3. Layer III only, which is what every .mp3 in practice is; a
// file that does not walk cleanly simply leaves the table invalid and the player stays on the
// device's own clock, exactly as before.
bool BuildMp3FrameTable(const std::wstring &path, Mp3FrameTable *out) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > kMp3MaxBytes)
    return false;
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  stream.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!stream)
    return false;

  static const int kBitrateV1[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                     112, 128, 160, 192, 224, 256, 320, 0};
  static const int kBitrateV2[16] = {0, 8,  16, 24,  32,  40,  48, 56,
                                     64, 80, 96, 112, 128, 144, 160, 0};
  static const int kRateV1[4] = {44100, 48000, 32000, 0};
  static const int kRateV2[4] = {22050, 24000, 16000, 0};
  static const int kRateV25[4] = {11025, 12000, 8000, 0};

  std::size_t position = 0;
  // ID3v2: 10-byte header, size is 7 bits per byte.
  if (data.size() > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
    const std::uint32_t declared = (static_cast<std::uint32_t>(data[6] & 0x7F) << 21) |
                                   (static_cast<std::uint32_t>(data[7] & 0x7F) << 14) |
                                   (static_cast<std::uint32_t>(data[8] & 0x7F) << 7) |
                                   static_cast<std::uint32_t>(data[9] & 0x7F);
    position = 10u + declared;
  }
  double time = 0.0;
  out->byte.reserve(16384);
  out->time.reserve(16384);
  while (position + 4 <= data.size() && out->byte.size() < kMp3MaxFrames) {
    const std::uint8_t *header = data.data() + position;
    if (header[0] != 0xFF || (header[1] & 0xE0) != 0xE0)
      break;
    const int version = (header[1] >> 3) & 0x03;   // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5, 1 = reserved
    const int layer = (header[1] >> 1) & 0x03;     // 1 = Layer III
    const int bitrate_index = (header[2] >> 4) & 0x0F;
    const int rate_index = (header[2] >> 2) & 0x03;
    const int padding = (header[2] >> 1) & 0x01;
    if (layer != 1 || version == 1 || rate_index == 3)
      break;
    const int bitrate =
        (version == 3 ? kBitrateV1 : kBitrateV2)[bitrate_index] * 1000;
    const int rate = version == 3 ? kRateV1[rate_index]
                                  : (version == 2 ? kRateV2[rate_index] : kRateV25[rate_index]);
    if (bitrate <= 0 || rate <= 0)
      break;
    const int samples = version == 3 ? 1152 : 576;
    const std::size_t frame_bytes =
        static_cast<std::size_t>(samples / 8) * static_cast<std::size_t>(bitrate) /
            static_cast<std::size_t>(rate) +
        static_cast<std::size_t>(padding);
    if (frame_bytes <= 4 || position + frame_bytes > data.size())
      break;
    out->byte.push_back(position);
    out->time.push_back(time);
    time += static_cast<double>(samples) / rate;
    position += frame_bytes;
  }
  out->total_seconds = time;
  out->file_bytes = data.size();
  out->valid = out->byte.size() > 8 && time > 1.0;
  return out->valid;
}

// Music, so a motion can be judged with its song. MCI (winmm) decodes mp3/wav/m4a through the
// codecs already on the machine, exposes a playhead in milliseconds and needs no device of its
// own -- which is all this fits: the plugin only has the ImGui-style UI service and raw Win32.
// It plays on its own audio path, so the game's own sound keeps working; a track that the system
// has no codec for reports its MCI error instead of failing silently.
class MusicPlayer {
 public:
  MusicPlayer() = default;
  MusicPlayer(const MusicPlayer &) = delete;
  MusicPlayer &operator=(const MusicPlayer &) = delete;
  ~MusicPlayer() { Close(); }

  bool Open(const std::string &utf8_path, std::string *error) {
    Close();
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::path(Utf8ToWide(utf8_path));
    if (!std::filesystem::exists(path, ec) || ec) {
      Set(error, "file not found");
      return false;
    }
    std::wstring extension = path.extension().wstring();
    for (wchar_t &c : extension)
      c = static_cast<wchar_t>(::towlower(c));
    // wav goes to the waveaudio device, everything else through the DirectShow-style codecs.
    const std::wstring device = extension == L".wav" ? L"waveaudio" : L"mpegvideo";
    // The alias namespace survives a hot reload (it lives as long as the process), so a previous
    // instance can still hold the name: close it first, and if it is somehow still taken fall back
    // to a numbered alias instead of leaving the panel dead with "alias already in use".
    const std::string base = "anomaly_bp_music_" + std::to_string(::GetCurrentProcessId());
    const std::string file_name = WideToUtf8(path.filename().wstring());
    std::string last_error;
    for (int attempt = 0; attempt < 4; ++attempt) {
      const std::string candidate =
          attempt == 0 ? base : base + "_" + std::to_string(attempt);
      std::string ignored;
      static_cast<void>(Command(L"close " + Utf8ToWide(candidate), &ignored));
      std::string open_error;
      if (Command(L"open \"" + Utf8ToWide(utf8_path) + L"\" type " + device + L" alias " +
                      Utf8ToWide(candidate),
                  &open_error)) {
        alias_ = candidate;
        path_ = utf8_path;
        name_ = file_name;
        muted_ = false;
        // Pin the time format: `play ... from`, `position` and `length` then all speak milliseconds.
        // Leaving it to the driver's default is how a seek ends up interpreted in another unit --
        // and 0 is the only position that means the same thing in every unit.
        std::string ignored_again;
        static_cast<void>(Command(L"set " + Utf8ToWide(alias_) + L" time format ms",
                                  &ignored_again));
        // The track's own frame table, and the rate the device believes in. Everything the player
        // asks for or reports goes through them (see Mp3FrameTable); without them it stays on the
        // device clock, which is what every non-MP3 file and every unparsable one does.
        frame_table_ = Mp3FrameTable{};
        device_rate_ = 0.0;
        timing_note_.clear();
        if (extension == L".mp3" && BuildMp3FrameTable(path.wstring(), &frame_table_)) {
          long long length_ms{};
          if (Query(L"length", &length_ms) && length_ms > 500) {
            const double rate = static_cast<double>(frame_table_.file_bytes) /
                                (static_cast<double>(length_ms) / 1000.0);
            if (rate >= 1000.0 && rate <= 200000.0) {
              device_rate_ = rate;
              char note[96]{};
              std::snprintf(note, sizeof(note), ", %zu frames, device clock x%.2f",
                            frame_table_.byte.size(),
                            (static_cast<double>(length_ms) / 1000.0) / frame_table_.total_seconds);
              timing_note_ = note;
            }
          }
        }
        return true;
      }
      last_error = open_error;
    }
    Set(error, last_error.empty() ? "could not open the audio file" : last_error);
    path_.clear();
    name_.clear();
    return false;
  }

  void Close() {
    if (!alias_.empty()) {
      std::string ignored;
      static_cast<void>(Command(L"close " + Utf8ToWide(alias_), &ignored));
    }
    alias_.clear();
    path_.clear();
    name_.clear();
    timing_note_.clear();
    frame_table_ = Mp3FrameTable{};
    device_rate_ = 0.0;
  }

  bool Play(std::string *error) {
    if (alias_.empty())
      return false;
    origin_true_seconds_ = 0.0;
    origin_device_ms_ = 0;
    return Command(L"play " + Utf8ToWide(alias_), error);
  }

  // Restarting from a known time is one command; seeking a *playing* mpegvideo device repeatedly
  // is what made the track stutter and die, so re-syncs always go through this.
  bool PlayFrom(const double seconds, std::string *error) {
    if (alias_.empty())
      return false;
    // `motion + lead` can land past the end of the track -- a 29.4 s song under a 29.4 s motion asks
    // for 29.55 s on the last frame, and a loop wrap asks again from the end -- and MCI answers that
    // with "the parameter is out of range for the specified command" instead of clamping. Ask for the
    // last sliver instead: the track ends either way, without the error line.
    const double position = ClampToTrack(seconds);
    const long long device_ms = DeviceMilliseconds(position);
    origin_true_seconds_ = position;
    origin_device_ms_ = device_ms;
    return Command(L"play " + Utf8ToWide(alias_) + L" from " + std::to_wstring(device_ms), error);
  }

  bool Pause(std::string *error) {
    if (alias_.empty())
      return false;
    return Command(L"pause " + Utf8ToWide(alias_), error);
  }

  bool Stop(std::string *error) {
    if (alias_.empty())
      return false;
    static_cast<void>(Command(L"stop " + Utf8ToWide(alias_), error));
    origin_true_seconds_ = 0.0;
    origin_device_ms_ = 0;
    return Command(L"seek " + Utf8ToWide(alias_) + L" to start", error);
  }

  bool Seek(const double seconds, std::string *error) {
    if (alias_.empty())
      return false;
    const double position = ClampToTrack(seconds);
    const long long device_ms = DeviceMilliseconds(position);
    origin_true_seconds_ = position;
    origin_device_ms_ = device_ms;
    return Command(L"seek " + Utf8ToWide(alias_) + L" to " + std::to_wstring(device_ms), error);
  }

  // Muting instead of pausing keeps the playhead running, so unmuting stays in sync.
  bool SetMuted(const bool muted, std::string *error) {
    if (alias_.empty())
      return false;
    if (!Command(L"setaudio " + Utf8ToWide(alias_) + (muted ? L" off" : L" on"), error))
      return false;
    muted_ = muted;
    return true;
  }

  double Position() const {
    long long milliseconds{};
    if (!Query(L"position", &milliseconds))
      return 0.0;
    const double seconds = static_cast<double>(milliseconds) / 1000.0;
    if (frame_table_.valid) {
      // The playhead counts real milliseconds from the position it was *given* -- verified on the
      // 64 kbps-header file: `play from 230000` read 241616 ms after 12.04 s of playing, i.e. exactly
      // the requested value plus the elapsed time, even though it landed 100 s of audio away. So the
      // track's own time is the origin plus that elapsed part, which stays exact whichever way the
      // landing went. (Multiplying by the device's assumed byte rate instead would be wrong: the
      // length and the playhead do not share a base on such a file.)
      const double value =
          origin_true_seconds_ + seconds - static_cast<double>(origin_device_ms_) / 1000.0;
      return (std::max)(0.0, (std::min)(frame_table_.total_seconds, value));
    }
    return seconds;
  }

  double Length() const {
    if (frame_table_.valid)
      return frame_table_.total_seconds;
    long long milliseconds{};
    if (!Query(L"length", &milliseconds))
      return 0.0;
    return static_cast<double>(milliseconds) / 1000.0;
  }

  // Ask the device instead of remembering: when a track ends on its own (or anything else stops
  // it) a cached flag would keep the follower from ever restarting it.
  bool playing() const {
    wchar_t reply[32]{};
    if (alias_.empty())
      return false;
    const std::wstring command =
        std::wstring(L"status ") + Utf8ToWide(alias_) + L" mode";
    if (::mciSendStringW(command.c_str(), reply, ARRAYSIZE(reply), nullptr) != 0)
      return false;
    return std::wstring(reply) == L"playing";
  }

  bool opened() const { return !alias_.empty(); }
  const std::string &name() const { return name_; }
  const std::string &timing_note() const { return timing_note_; }

 private:
  static void Set(std::string *error, const std::string &text) {
    if (error != nullptr)
      *error = text;
  }

  // A position for `play`/`seek`, expressed in the device's own time base. The device's clock and the
  // file's clock are the same thing on a CBR file and differ by the ratio of the two bitrates on one
  // whose header frame lies, which is exactly the case the frame table exists for (see Mp3FrameTable).
  long long DeviceMilliseconds(const double seconds) const {
    const double clamped = (std::max)(0.0, seconds);
    if (frame_table_.valid && device_rate_ > 0.0)
      return static_cast<long long>(frame_table_.ByteForTime(clamped) * 1000.0 / device_rate_);
    return static_cast<long long>(clamped * 1000.0);
  }

  // MCI rejects a position at or past the end of the track ("the parameter is out of range for the
  // specified command"), so a request aimed there is pulled back to the last 50 ms.
  double ClampToTrack(const double seconds) const {
    const double length = Length();
    if (length > 0.1)
      return (std::max)(0.0, (std::min)(seconds, length - 0.05));
    return (std::max)(0.0, seconds);
  }

  bool Command(const std::wstring &command, std::string *error) const {
    wchar_t reply[128]{};
    const MCIERROR code = ::mciSendStringW(command.c_str(), reply, ARRAYSIZE(reply), nullptr);
    if (code == 0)
      return true;
    wchar_t text[256]{};
    if (::mciGetErrorStringW(code, text, ARRAYSIZE(text)) != 0)
      Set(error, WideToUtf8(text));
    else
      Set(error, "MCI error " + std::to_string(static_cast<unsigned long>(code)));
    return false;
  }

  bool Query(const wchar_t *what, long long *out) const {
    if (alias_.empty())
      return false;
    wchar_t reply[64]{};
    const std::wstring command = std::wstring(L"status ") + Utf8ToWide(alias_) + L" " + what;
    if (::mciSendStringW(command.c_str(), reply, ARRAYSIZE(reply), nullptr) != 0)
      return false;
    *out = ::_wcstoi64(reply, nullptr, 10);
    return true;
  }

  std::string alias_;
  std::string path_;
  std::string name_;
  std::string timing_note_;
  Mp3FrameTable frame_table_;
  double device_rate_ = 0.0;
  // Where the playhead was last placed: the track's time there, and the value handed to the device.
  double origin_true_seconds_ = 0.0;
  long long origin_device_ms_ = 0;
  bool muted_{};
};

// One player per plugin instance. **The game tick thread owns it**, because MCI does not share a
// device between threads: an alias opened from the panel answers "the specified device is not open
// or is not recognised by MCI" to every command sent from the tick, which is exactly what happened
// when the follower was moved onto the tick while the panel still did the open().
//
// So the two directions are explicit:
//   panel -> tick : a track to open, a mute state to apply, a close request (requests, guarded)
//   tick -> panel : name, position, length, playing, error (published for display)
MusicPlayer g_music;
std::mutex g_music_mutex;          // guards the player, the request strings and g_music_name/error
std::string g_music_request_path;  // non-empty: the tick should open this and clear it
bool g_music_request_mute_pending = false;
bool g_music_request_mute = false;
bool g_music_request_close = false;
std::atomic_bool g_music_muted{false};
std::string g_music_name;
std::string g_music_error;
std::atomic_bool g_music_opened{false};
std::atomic_bool g_music_playing{false};
std::atomic<double> g_music_position{0.0};
std::atomic<double> g_music_length{0.0};
// Auto-pairing (find the song next to the motion) runs on the tick too, so this remembers the
// motion it was tried for.
std::string g_music_paired_for;

// Defined below; the follower uses it to find the song next to the motion.
std::string FindSiblingAudio(const std::string &motion_path);

// How far the track may sit from the motion before it is restarted, how long a tick may take
// before the motion is assumed to have lost time, and how often a drift may be corrected.
// How often a stopped track may be restarted (a seek is never throttled).
constexpr double kMusicResyncInterval = 1.0;
// Soft sync: while a track plays, the *animation* clock is pulled towards the device by at most a
// quarter of its rate, and a gap too large to hide snaps the animation rather than the audio.
// Measured before this: the device's position ramped away from the motion cursor by 0.30 s every
// 4-5 s (a ~6 % rate difference, not jitter), and correcting that by restarting the track is what
// the player hears as "it jumps". MCI has no rate control, so the audio is the master clock and the
// picture follows it.
constexpr double kMusicSyncGain = 1.5;
constexpr double kMusicHardSnapSeconds = 3.0;
// A drag on the progress bar bumps the seek serial on every change, and each `play from` restarts
// the device (which takes real time, and issuing them back to back can leave it mid-seek). Wait for
// the drag to settle, then issue one command for the final position.
constexpr double kMusicSeekSettleSeconds = 0.20;
// How far ahead of the motion the track is started. A device only begins producing sound a little
// after `play ... from` and MCI's playhead does not model that delay, so the offset was measured by
// ear at 0.15 s. It is no longer a slider or a saved setting: a value stored from an earlier version
// would come back with no way to change it back.
constexpr double kMusicLeadSeconds = 0.15;

void PublishMusicState(const bool opened, const bool playing, const double position,
                       const double length) noexcept {
  g_music_opened.store(opened, std::memory_order_release);
  g_music_playing.store(playing, std::memory_order_release);
  g_music_position.store(position, std::memory_order_release);
  g_music_length.store(length, std::memory_order_release);
}

// The music follows the motion, and this runs on every tick -- deliberately NOT from the panel,
// which is where it used to live: a collapsed panel culls the whole block, so the follower only
// ran while the panel was open. Drift therefore accumulated unseen (the two clocks are independent
// by construction: the motion cursor adds up the game tick's deltas, so a stalled tick, a long
// frame or a paused game all lose time, while the track runs on the audio device's own clock) and
// was corrected in one audible lurch the moment the panel was drawn. A stall now forces the
// correction, and the drift threshold is small enough to catch the device-start offset that the
// old 1.5 s window never saw.
void StepMusic(Context &context, const double delta_seconds) noexcept {
  // Never block the game tick on the panel: skip this tick instead (its critical sections are
  // short, so the next tick gets through).
  std::unique_lock<std::mutex> lock(g_music_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  static std::uint32_t music_seek_seen{};
  static double since_resync = kMusicResyncInterval;

  // ---- requests from the panel -------------------------------------------------------------
  if (g_music_request_close) {
    g_music_request_close = false;
    g_music.Close();
    g_music_name.clear();
    g_music_opened.store(false, std::memory_order_release);
  }
  if (!g_music_request_path.empty()) {
    const std::string path = g_music_request_path;
    g_music_request_path.clear();
    g_music_error.clear();
    // A manual pick counts as this motion's choice, so the sibling search below does not undo it.
    g_music_paired_for = context.motion_file;
    if (!g_music.Open(path, &g_music_error)) {
      g_music_name.clear();
      LogDiagnostic(context, "betterpose music open failed: " + g_music_error);
    } else {
      g_music_name = g_music.name();
      char line[256]{};
      std::snprintf(line, sizeof(line), "betterpose music opened: %s, %.1f s%s",
                    g_music_name.c_str(), g_music.Length(), g_music.timing_note().c_str());
      LogDiagnostic(context, line);
    }
  }
  if (g_music_request_mute_pending) {
    g_music_request_mute_pending = false;
    g_music_muted.store(g_music_request_mute, std::memory_order_release);
    g_music_error.clear();
    if (g_music.opened() &&
        !g_music.SetMuted(g_music_muted.load(std::memory_order_relaxed), &g_music_error))
      LogDiagnostic(context, "betterpose music mute failed: " + g_music_error);
  }
  // The song belongs to the motion: whenever a *different* motion is loaded the track is replaced by
  // that motion's sibling audio, and one with no sibling goes silent. Waiting for an empty player
  // instead (the earlier rule) meant the first song of the session played on for every motion after
  // it -- "it is always that same song".
  if (!context.motion_file.empty() && g_music_paired_for != context.motion_file) {
    g_music_paired_for = context.motion_file;
    g_music_error.clear();
    const std::string sibling = FindSiblingAudio(context.motion_file);
    if (sibling.empty()) {
      g_music.Close();
      g_music_name.clear();
    } else if (!g_music.Open(sibling, &g_music_error)) {
      LogDiagnostic(context, "betterpose music open failed: " + g_music_error);
    } else {
      g_music_name = g_music.name();
      char line[256]{};
      std::snprintf(line, sizeof(line), "betterpose music opened: %s, %.1f s%s",
                    g_music_name.c_str(), g_music.Length(), g_music.timing_note().c_str());
      LogDiagnostic(context, line);
    }
  }
  const bool opened = g_music.opened();
  const bool playing = opened && g_music.playing();
  const double position = opened ? g_music.Position() : 0.0;
  const double length = opened ? g_music.Length() : 0.0;
  PublishMusicState(opened, playing, position, length);
  if (!opened || !context.motion_loaded.load(std::memory_order_acquire))
    return;

  // ---- follow -------------------------------------------------------------------------------
  const bool motion_plays = context.motion_playing.load(std::memory_order_acquire);
  const double want = context.motion_display_seconds.load(std::memory_order_acquire);
  const double lead = kMusicLeadSeconds;
  const double target = want + lead;
  const std::uint32_t seek_serial = context.motion_seek_serial.load(std::memory_order_acquire);
  // A drag bumps the serial many times; wait for it to settle before touching the device, and keep
  // the animation on the bar's position until then (see motion_seek_pending).
  static std::uint32_t music_seek_pending{};
  static double since_seek_change = kMusicSeekSettleSeconds;
  static bool verify_pending = false;
  static double verify_target = 0.0;
  static bool verify_late = false;
  static double verify_late_at = 0.0;
  if (seek_serial != music_seek_pending) {
    music_seek_pending = seek_serial;
    since_seek_change = 0.0;
  }
  since_seek_change += delta_seconds;
  // A loop wrap must not wait for the drag to settle: the animation has already jumped back.
  if (context.motion_seek_immediate.exchange(false, std::memory_order_acq_rel))
    since_seek_change = kMusicSeekSettleSeconds;
  // The seek is only consumed once the track has actually been restarted, so scrubbing while paused
  // still re-syncs on the next play.
  const bool seek_ready = music_seek_pending != music_seek_seen &&
                          since_seek_change >= kMusicSeekSettleSeconds;
  // Only two things still restart the track: a settled seek (the player moved the bar, so the audio
  // has to go there) and a track that is not playing when the motion is. Drift is no longer one of
  // them -- it is corrected by nudging the *animation* clock in UpdateRuntime instead, because a
  // rate difference cannot be fixed by jumping the audio, only made audible.
  if (motion_plays && (seek_ready || (!playing && since_resync >= kMusicResyncInterval))) {
    if (!g_music.PlayFrom(target, &g_music_error)) {
      LogDiagnostic(context, "betterpose music resync failed: " + g_music_error);
    } else {
      char line[224]{};
      std::snprintf(line, sizeof(line),
                    "betterpose music resync: motion %.2f s, device %.2f s, lead %.2f s, %s",
                    want, position, lead, seek_ready ? "after a seek" : "start");
      LogDiagnostic(context, line);
    }
    music_seek_seen = music_seek_pending;
    since_resync = 0.0;
    // The device has been told where to play: the animation may follow it again.
    context.motion_seek_pending.store(false, std::memory_order_release);
    verify_pending = true;
    verify_target = target;
  } else if (!motion_plays && playing) {
    static_cast<void>(g_music.Pause(&g_music_error));
  }
  since_resync += delta_seconds;
  // Half a second after a restart, report where the device actually sits. `play ... from` is
  // asynchronous in MCI, so "did it land where we asked" is a question only the device can answer,
  // and without this line a seek that lands elsewhere is invisible in the log.
  if (verify_pending && since_resync >= 0.5) {
    verify_pending = false;
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "betterpose music seek landed: requested %.2f s, device %.2f s", verify_target,
                  position);
    LogDiagnostic(context, line);
    // A second look, two seconds later: if the driver seeks to the requested position and then
    // *plays* from somewhere else, the reading is the only witness we have, and a reading that
    // walks backwards would say so.
    verify_late = true;
    verify_late_at = since_resync;
  }
  if (verify_late && since_resync - verify_late_at >= 2.0) {
    verify_late = false;
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "betterpose music seek held: requested %.2f s, device %.2f s", verify_target,
                  position);
    LogDiagnostic(context, line);
  }
}

// A track sitting next to the motion is almost always the right one, so offer it: the exact
// basename first, then the same basename ignoring spaces (motions and songs are often named
// "爱言叶4.vmd" / "爱言叶4 .mp3").
std::string FindSiblingAudio(const std::string &motion_path) {
  std::error_code ec;
  const std::filesystem::path motion = std::filesystem::path(Utf8ToWide(motion_path));
  const std::filesystem::path folder = motion.parent_path();
  if (folder.empty() || !std::filesystem::is_directory(folder, ec) || ec)
    return std::string();
  const std::wstring stem = motion.stem().wstring();
  const wchar_t *extensions[] = {L".mp3", L".wav", L".m4a", L".aac", L".ogg", L".wma", L".flac"};
  std::string loose;
  for (const wchar_t *extension : extensions) {
    const std::filesystem::path exact = folder / (stem + extension);
    if (std::filesystem::exists(exact, ec) && !ec)
      return WideToUtf8(exact.wstring());
  }
  const auto squeeze = [](std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](wchar_t c) { return c == L' ' || c == L'\u3000'; }),
                value.end());
    for (wchar_t &c : value)
      c = static_cast<wchar_t>(::towlower(c));
    return value;
  };
  const std::wstring wanted = squeeze(stem);
  for (const auto &entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || ec)
      continue;
    std::wstring extension = entry.path().extension().wstring();
    for (wchar_t &c : extension)
      c = static_cast<wchar_t>(::towlower(c));
    bool audio = false;
    for (const wchar_t *candidate : extensions)
      audio = audio || extension == candidate;
    if (!audio)
      continue;
    if (squeeze(entry.path().stem().wstring()) == wanted) {
      loose = WideToUtf8(entry.path().wstring());
      break;
    }
  }
  return loose;
}

struct PoseFileTaskData final {
  Context *context{};
  std::uint32_t action{};
  std::string document;
  std::string path;
  // Action 5 (VMD -> motion) needs more than a path pair: the skeleton export is read on
  // the game thread and handed over here, and the converted document is written next to
  // the source motion.
  std::string skeleton_document;
  std::string reference_document;
  std::string output_path;
};

// Directory the plugin's own DLL lives in: the reference MMD bone table is shipped next
// to it, and the plugin has no other way to find its own files.
std::string ModuleDirectory() noexcept {
  HMODULE module{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module) ||
      module == nullptr)
    return std::string();
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(module, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0)
      return std::string();
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  const std::size_t slash = buffer.find_last_of(L"\\/");
  if (slash == std::wstring::npos)
    return std::string();
  return WideToUtf8(buffer.substr(0, slash));
}

std::string ReferenceBoneTablePath(bool unity_reference) noexcept {
  const std::string directory = ModuleDirectory();
  if (directory.empty())
    return std::string();
  return directory + (unity_reference ? "\\data\\reference-miku-unity.json"
                                      : "\\data\\reference-pmx.json");
}

void ANOMALY_CALL PoseFileTask(void *value, AnomalyGenerationHandleV1) {
  auto *data = static_cast<PoseFileTaskData *>(value);
  if (data == nullptr)
    return;
  Context *context = data->context;
  if (context == nullptr) {
    delete data;
    return;
  }
  try {
    if (data->action == 1 || data->action == 3) {
      const bool skeleton = data->action == 3;
      const auto fail = [context, skeleton](const char *reason) {
        SetReflectionStatus(
            *context, (skeleton ? "skeleton export failed: " : "pose export failed: ") +
                          std::string(reason));
      };
      std::ofstream file(Utf8ToWide(data->path), std::ios::binary);
      if (!file) {
        fail("cannot open file");
        delete data;
        return;
      }
      file.write(data->document.data(), static_cast<std::streamsize>(data->document.size()));
      file.close();
      if (!file) {
        fail("write error");
      } else {
        SetReflectionStatus(*context, skeleton ? "skeleton exported" : "pose exported");
      }
    } else if (data->action == 2) {
      std::ifstream file(Utf8ToWide(data->path), std::ios::binary);
      if (!file) {
        SetReflectionStatus(*context, "pose import failed: cannot open file");
        delete data;
        return;
      }
      std::string document((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
      if (file.bad() || document.empty() || document.size() > kMaximumPoseSettingsBytes) {
        SetReflectionStatus(*context, "pose import failed: unreadable file");
      } else {
        const auto json = nlohmann::json::parse(document);
        if (!ApplyPoseDocument(*context, json)) {
          SetReflectionStatus(*context, "pose import failed: invalid document");
        } else {
          context->pose_override_enabled.store(true, std::memory_order_release);
          context->pose_settings_dirty.store(true, std::memory_order_release);
          SetReflectionStatus(*context, "pose imported");
        }
      }
    } else if (data->action == 4) {
      std::ifstream file(Utf8ToWide(data->path), std::ios::binary);
      if (!file) {
        SetReflectionStatus(*context, "motion load failed: cannot open file");
        delete data;
        return;
      }
      std::string document((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
      if (file.bad() || document.empty() || document.size() > kMaximumMotionBytes) {
        SetReflectionStatus(*context, "motion load failed: unreadable file");
      } else {
        try {
          if (LoadMotionDocument(*context, document, data->path))
            SetReflectionStatus(*context, "motion loaded");
          else
            SetReflectionStatus(*context, "motion load failed: invalid document");
        } catch (const std::exception &error) {
          SetReflectionStatus(*context,
                              std::string("motion load failed: ") + error.what());
        }
      }
    } else if (data->action == 5) {
      // VMD -> better-pose motion, entirely in process: the Python converter that produced
      // the accepted files was ported for exactly this, and the port is verified against
      // it frame by frame (see tools/mmd2bip/PORT-SPEC.md).
      std::ifstream vmd(Utf8ToWide(data->path), std::ios::binary);
      if (!vmd) {
        SetReflectionStatus(*context, "motion convert failed: cannot open vmd");
        delete data;
        return;
      }
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(vmd)),
                                      std::istreambuf_iterator<char>());
      if (vmd.bad() || bytes.empty()) {
        SetReflectionStatus(*context, "motion convert failed: unreadable vmd");
        delete data;
        return;
      }
      better_pose::mmd2bip::Input input;
      input.vmd_path = data->path;
      input.pmx_path = ReferenceBoneTablePath(
          context->motion_reference_unity.load(std::memory_order_acquire));
      input.skeleton_path = "";
      input.skeleton_json = data->skeleton_document;
      input.reference_pmx_json = data->reference_document;
      input.vmd_bytes = std::move(bytes);
      // These three were player-facing debug toggles; they are part of the conversion now: solve
      // the VMD's IK (a motion can drive the legs purely through 左足ＩＫ -- rigoutput.vmd has no
      // knee tracks at all), carry the hip rotation on the pelvis, and place the body from the
      // pelvis so the feet stay where MMD's IK puts them.
      input.ik = true;
      input.hip_on_pelvis = true;
      input.feet_anchor = false;
      const auto result = better_pose::mmd2bip::BuildMotion(input);
      if (!result.ok) {
        SetReflectionStatus(*context, "motion convert failed: " + result.error);
        delete data;
        return;
      }
      {
        std::ofstream out(Utf8ToWide(data->output_path), std::ios::binary);
        if (!out) {
          SetReflectionStatus(*context, "motion convert failed: cannot write output");
          delete data;
          return;
        }
        out.write(result.motion_json.data(),
                  static_cast<std::streamsize>(result.motion_json.size()));
        out.close();
        if (!out) {
          SetReflectionStatus(*context, "motion convert failed: write error");
          delete data;
          return;
        }
      }
      LogDiagnostic(*context, "betterpose convert " + result.report);
      try {
        if (LoadMotionDocument(*context, result.motion_json, data->output_path)) {
          context->motion_file = data->output_path;
          // Surface how many bones carry a per-frame translation: the re-solved legs and the
          // spine make three. A stale build or a document without them loads as 0/1, so the
          // status line alone tells which converter produced what is playing.
          std::size_t offset_tracks = 0;
          {
            std::lock_guard<std::mutex> lock(context->motion_mutex);
            offset_tracks = context->motion.offset_names.size();
          }
          SetReflectionStatus(*context,
                              "motion converted and loaded [off " +
                                  std::to_string(offset_tracks) + "]");
        } else {
          SetReflectionStatus(*context, "motion convert failed: document rejected");
        }
      } catch (const std::exception &error) {
        SetReflectionStatus(*context,
                            std::string("motion convert failed: ") + error.what());
      }
    }
  } catch (...) {
    SetReflectionStatus(*context, "pose file action threw an exception");
  }
  delete data;
}

// In-memory FTransform layout: rot(4 doubles) + translation(3) + pad + scale(3) + pad.
nlohmann::json TransformToJson(const double *raw) noexcept {
  nlohmann::json out = nlohmann::json::object();
  out["rotation"] = {raw[0], raw[1], raw[2], raw[3]};
  out["translation"] = {raw[4], raw[5], raw[6]};
  out["scale"] = {raw[8], raw[9], raw[10]};
  return out;
}

// Export the target skeleton: bone names, parent links, and two pose references.
//
// Why: to drive this character from an external motion file (e.g. an MMD VMD), a converter must
// first know what the target skeleton looks like -- which bones exist, the parent chain, and each
// bone's local orientation in UE's convention. Only with the local orientation can the converter
// turn a source motion's rotation into "this bone's local rotation". The plugin already recomputes
// FK and writes the pose buffers itself, so this path never touches the engine's skeleton
// compatibility check.
//
// baseLocal comes from the pose captured by CapturePoseBase (the game's current pose). The
// engine's reference (bind) pose is read separately, straight out of the mesh asset's
// FReferenceSkeleton (FindReferencePose), and written as refLocal when the read succeeds: a live
// capture always carries whatever posture the character was in, and the finger and limb rolls the
// converter derives from it inherit that posture, whereas the reference pose is the one the
// geometry is actually skinned in. refLocal falling back to nothing is fine -- the converter then
// uses baseLocal exactly as before.
std::string BuildSkeletonDocument(Context &context) noexcept {
  const auto count = context.runtime.local_space_count;
  // Bone names are the prerequisite for mapping; load them now if never loaded (also refreshes
  // the parent chain).
  if (context.bone_names.size() != count ||
      context.bone_names_mesh != context.runtime.mesh) {
    std::vector<std::string> names;
    std::string detail;
    if (RefreshBoneNames(context, names, detail)) {
      context.bone_names = std::move(names);
      context.bone_names_mesh = context.runtime.mesh;
      context.bone_names_count = context.runtime.bone_space_count;
      context.bone_names_attempted = true;
      RefreshBoneHierarchy(context);
    }
  }
  // The base pose is the reference frame for local rotations; without it this export is useless.
  if (!CapturePoseBase(context))
    return std::string();
  // An index-only skeleton is not usable as a mapping target, so refuse instead of writing one.
  if (count == 0 || context.bone_names.size() != count ||
      !context.bone_parents_ready || context.bone_parents_count != count ||
      context.bone_parents.size() != count)
    return std::string();
  const auto component_data = context.runtime.component_space_data;
  const auto component_count = context.runtime.component_space_count;
  // Read the bind pose first: the export is the only consumer, and a failed read must not change
  // anything else (the document simply omits refLocal).
  static_cast<void>(FindReferencePose(context));
  const bool has_ref = context.ref_locals.size() == count;
  if (!has_ref) {
    char status[192]{};
    std::snprintf(status, sizeof(status),
                  "betterpose refpose unavailable (%s)",
                  context.ref_pose_status.c_str());
    LogDiagnostic(context, status);
  }
  auto bones = nlohmann::json::array();
  for (std::uint32_t index{}; index != count; ++index) {
    nlohmann::json bone = nlohmann::json::object();
    bone["index"] = index;
    bone["name"] = context.bone_names[index];
    bone["parent"] = context.bone_parents[index];
    std::array<double, 12> local{};
    {
      std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
      local = context.pose_base_locals[index];
    }
    bone["baseLocal"] = TransformToJson(local.data());
    if (has_ref)
      bone["refLocal"] = TransformToJson(context.ref_locals[index].data());
    if (component_data != 0 && index < component_count) {
      std::uintptr_t address{};
      std::array<double, 12> component{};
      if (AddAddress(component_data,
                     static_cast<std::uint64_t>(index) * kTransformSize, address) &&
          Read(context, address, component)) {
        bone["liveComponent"] = TransformToJson(component.data());
      }
    }
    bones.push_back(std::move(bone));
  }
  nlohmann::json root = nlohmann::json::object();
  root["schemaVersion"] = 1;
  root["kind"] = "better-pose-skeleton";
  root["basis"] = has_ref ? "captured-live-pose plus refLocal from the mesh asset's "
                            "FReferenceSkeleton (the bind pose)"
                          : "captured-live-pose (engine reference pose unavailable)";
  root["mesh"] = Hex(context.runtime.mesh);
  root["boneCount"] = count;
  root["boneSpaceCount"] = context.runtime.bone_space_count;
  root["componentSpaceCount"] = component_count;
  root["bones"] = std::move(bones);
  return root.dump();
}

void ExecutePoseFileAction(Context &context) noexcept {
  const std::uint32_t action =
      context.pose_file_action_requested.exchange(0, std::memory_order_acquire);
  if (action == 0)
    return;
  if (!SchedulerReady(context.scheduler)) {
    SetReflectionStatus(context, "pose file action failed: scheduler unavailable");
    return;
  }
  auto *data = new (std::nothrow) PoseFileTaskData();
  if (data == nullptr) {
    SetReflectionStatus(context, "pose file action failed: out of memory");
    return;
  }
  data->context = &context;
  data->action = action;
  if (action == 1 || action == 3) {
    // action 3 reuses the same folder/name inputs; only the suffix becomes .skeleton.json.
    const bool skeleton = action == 3;
    const auto fail = [&context, skeleton](const char *reason) {
      SetReflectionStatus(
          context, (skeleton ? "skeleton export failed: " : "pose export failed: ") +
                       std::string(reason));
    };
    std::string name(context.pose_export_name.data());
    if (name.empty())
      name = skeleton ? "skeleton" : "pose";
    if (name.size() < 5 || name.substr(name.size() - 5) != ".json")
      name += ".json";
    if (skeleton)
      name = name.substr(0, name.size() - 5) + ".skeleton.json";
    const std::wstring folder = Utf8ToWide(context.pose_export_folder);
    if (folder.empty()) {
      delete data;
      fail("no folder selected");
      return;
    }
    std::wstring path = folder;
    if (path.back() != L'\\' && path.back() != L'/')
      path.push_back(L'\\');
    path += Utf8ToWide(name);
    data->path = WideToUtf8(path);
    if (data->path.empty()) {
      delete data;
      fail("invalid path");
      return;
    }
    data->document =
        skeleton ? BuildSkeletonDocument(context) : BuildPoseDocument(context);
    if (data->document.empty()) {
      delete data;
      fail(skeleton ? "bone names or base pose unavailable; make the character visible first"
                    : "empty document");
      return;
    }
    if (data->document.size() > kMaximumPoseSettingsBytes) {
      delete data;
      fail("document too large");
      return;
    }
  } else if (action == 2) {
    if (context.pose_import_file.empty()) {
      delete data;
      SetReflectionStatus(context, "pose import failed: no file selected");
      return;
    }
    data->path = context.pose_import_file;
  } else if (action == 4) {
    if (context.motion_file.empty()) {
      delete data;
      SetReflectionStatus(context, "motion load failed: no file selected");
      return;
    }
    data->path = context.motion_file;
  } else if (action == 5) {
    // Conversion: the skeleton export has to be read here, on the game thread, because it
    // walks live engine memory; the task thread only touches files and the documents.
    if (context.motion_file.empty()) {
      delete data;
      SetReflectionStatus(context, "motion convert failed: no vmd selected");
      return;
    }
    data->path = context.motion_file;
    data->skeleton_document = BuildSkeletonDocument(context);
    if (data->skeleton_document.empty()) {
      delete data;
      SetReflectionStatus(
          context, "motion convert failed: skeleton unavailable (make the character visible)");
      return;
    }
    const std::string reference_path =
        ReferenceBoneTablePath(context.motion_reference_unity.load(std::memory_order_acquire));
    {
      std::ifstream reference(Utf8ToWide(reference_path), std::ios::binary);
      if (!reference) {
        delete data;
        SetReflectionStatus(context, "motion convert failed: reference bone table missing");
        return;
      }
      data->reference_document.assign(std::istreambuf_iterator<char>(reference),
                                      std::istreambuf_iterator<char>());
    }
    // Output next to the source motion so the converted document is easy to find.
    std::string output = context.motion_file;
    const std::size_t dot = output.find_last_of('.');
    const std::size_t slash = output.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
      output = output.substr(0, dot);
    data->output_path = output + ".betterpose.json";
  }
  AnomalyGenerationHandleV1 task{};
  const AnomalyStatusV1 status = context.scheduler->schedule(
      context.scheduler->user, 0, PoseFileTask, data, &task);
  if (status.code != ANOMALY_STATUS_V1_OK || task.id == 0) {
    delete data;
    SetReflectionStatus(context,
                        "pose file action failed: schedule code=" +
                            std::to_string(status.code));
    return;
  }
  SetReflectionStatus(context, action == 1   ? "pose export queued"
                               : action == 3 ? "skeleton export queued"
                               : action == 4 ? "motion load queued"
                               : action == 5 ? "motion convert queued"
                                             : "pose import queued");
}
void EnsureActiveCharacterProfile(Context &context) noexcept {
  if (context.runtime.mesh == 0 || !StorageReady(context.storage))
    return;
  const std::string profile_id = Hex(context.runtime.mesh);
  if (profile_id.empty() || profile_id == context.active_character_id)
    return;
  const bool first_profile = !context.character_profiles_initialized;
  context.active_character_id = profile_id;
  const int loaded = LoadCharacterPoseProfile(context, profile_id);
  if (loaded <= 0 && !first_profile)
    ResetPoseValues(context);
  context.character_profiles_initialized = true;
  context.pose_settings_dirty.store(true, std::memory_order_release);
}

void ExecuteReflectionAction(Context &context) noexcept {
  const std::uint32_t action =
      context.reflection_action_requested.exchange(0, std::memory_order_acquire);
  if (action == 0 || context.runtime.mesh == 0)
    return;
  try {
    bool ok = false;
    std::string detail;
    if (action == 1) {
      const std::uint8_t looping = 1;
      ok = CallVirtualUFunction(context, context.runtime.mesh, kFunctionPlayPath,
                                &looping, sizeof(looping), detail);
    } else if (action == 2) {
      ok = CallVirtualUFunction(context, context.runtime.mesh, kFunctionStopPath,
                                nullptr, 0, detail);
    } else if (action == 3) {
      struct SetPositionParameters {
        float position;
        std::uint8_t fire_notifies;
      };
      SetPositionParameters parameters{};
      parameters.position = 0.0F;
      parameters.fire_notifies = 0;
      ok = CallVirtualUFunction(context, context.runtime.mesh,
                                kFunctionSetPositionPath, &parameters,
                                sizeof(parameters), detail);
    } else if (action == 4) {
      if (context.runtime.character == 0) {
        detail = "local character is unavailable";
      } else {
        ok = CallVirtualUFunction(context, context.runtime.character,
                                  kFunctionRefreshAnimInstancePath, nullptr, 0,
                                  detail);
      }
    } else if (action == 5) {
      ok = ForcePoseCache(context, detail);
    } else if (action == 6) {
      std::vector<std::string> names;
      ok = RefreshBoneNames(context, names, detail);
      if (ok) {
        context.bone_names = std::move(names);
        context.bone_names_mesh = context.runtime.mesh;
        context.bone_names_count = context.runtime.bone_space_count;
        context.bone_names_attempted = true;
        RefreshBoneHierarchy(context);
      }
    }
    const std::string label = action == 1   ? "Play: "
                              : action == 2 ? "Stop: "
                              : action == 3 ? "Seek 0: "
                              : action == 4 ? "Refresh Anim: "
                              : action == 5 ? "Refresh Pose: "
                                            : "Load Bones: ";
    if (action == 5)
      static_cast<void>(ReadPoseArrays(context));
    SetReflectionStatus(context, label + (ok ? detail : detail));
  } catch (...) {
    SetReflectionStatus(context, "UFunction call threw an exception");
  }
}

std::string Hex(const std::uintptr_t value) noexcept {
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "0x%llX",
                static_cast<unsigned long long>(value));
  return std::string(buffer.data());
}

void SetStatus(RenderSnapshot &snapshot, const std::string_view message) {
  const auto length = (std::min)(message.size(), snapshot.status.size() - 1U);
  std::memcpy(snapshot.status.data(), message.data(), length);
  snapshot.status[length] = '\0';
}

void PublishSnapshot(Context &context, const std::string_view status) {
  RenderSnapshot next{};
  next.active = context.runtime.character != 0 && context.runtime.mesh != 0;
  next.character = context.runtime.character;
  next.mesh = context.runtime.mesh;
  next.anim_instance = context.runtime.anim_instance;
  next.animation_mode = context.runtime.animation_mode;
  next.bone_space_count = context.runtime.bone_space_count;
  next.bone_space_data = context.runtime.bone_space_data;
  next.component_space_count = context.runtime.component_space_count;
  next.component_space_data = context.runtime.component_space_data;
  next.rate_scale = context.runtime.rate_scale;
  next.root_motion_scale = context.runtime.root_motion_scale;
  next.pose_available = context.runtime.bone_space_data != 0 &&
                        context.runtime.bone_space_count != 0 &&
                        context.runtime.component_space_data != 0 &&
                        context.runtime.component_space_count != 0 &&
                        context.runtime.local_space_data != 0 &&
                        context.runtime.local_space_count != 0;
  next.bone_names = context.bone_names;
  next.pose_bone_index =
      context.requested_bone_index.load(std::memory_order_acquire);
  if (context.runtime.component_space_data != 0 &&
      next.pose_bone_index < context.runtime.component_space_count) {
    std::uintptr_t component_transform{};
    static_cast<void>(AddAddress(
        context.runtime.component_space_data,
        static_cast<std::uint64_t>(next.pose_bone_index) * kTransformSize,
        component_transform));
    static_cast<void>(Read(context,
                           component_transform + kTransformTranslationOffset,
                           next.component_translation_readback));
  }
  if (context.runtime.bone_space_data != 0 &&
      next.pose_bone_index < context.runtime.bone_space_count) {
    std::uintptr_t bone_transform{};
    static_cast<void>(AddAddress(
        context.runtime.bone_space_data,
        static_cast<std::uint64_t>(next.pose_bone_index) * kTransformSize,
        bone_transform));
    static_cast<void>(Read(context, bone_transform + kTransformTranslationOffset,
                           next.bone_translation_readback));
  }
  const auto pose_length =
      (std::min)(context.pose_status.size(), next.pose_status.size() - 1U);
  std::memcpy(next.pose_status.data(), context.pose_status.data(), pose_length);
  next.pose_status[pose_length] = '\0';
  SetStatus(next, status);
  std::scoped_lock lock(context.state_mutex);
  const auto length = (std::min)(context.reflection_status.size(),
                                 next.reflection_status.size() - 1U);
  std::memcpy(next.reflection_status.data(),
              context.reflection_status.data(), length);
  next.reflection_status[length] = '\0';
  context.snapshot = next;
}

bool RefreshRuntime(Context &context) noexcept {
  if (context.runtime.g_world_address == 0 && !ResolveGWorld(context))
    return false;
  if (!ResolveLocalCharacter(context))
    return false;
  if (!ReadAnimationState(context))
    return false;
  static_cast<void>(ReadPoseArrays(context));
  EnsurePoseAngleCapacity(context);
  MaybeRefreshBoneNames(context);
  RefreshBoneHierarchyDirect(context);
  static_cast<void>(Read(context,
                         context.runtime.mesh + kMeshGlobalAnimRateScaleOffset,
                         context.runtime.rate_scale));
  static_cast<void>(Read(
      context, context.runtime.character + kCharacterAnimRootMotionScaleOffset,
      context.runtime.root_motion_scale));
  return true;
}

void UpdateRuntime(Context &context, const double delta_seconds) noexcept {
  if (!RefreshRuntime(context)) {
    PublishSnapshot(context, "local player character is unavailable");
    return;
  }

  EnsureActiveCharacterProfile(context);

  const bool freeze_enabled =
      context.freeze_enabled.load(std::memory_order_acquire);
  const bool rate_enabled =
      context.rate_override_enabled.load(std::memory_order_acquire);
  const float rate = context.requested_rate_scale.load(std::memory_order_acquire);
  const bool root_enabled =
      context.root_motion_override_enabled.load(std::memory_order_acquire);
  const float root = context.requested_root_motion_scale.load(
      std::memory_order_acquire);

  ExecuteReflectionAction(context);
  ExecutePoseFileAction(context);
  StepMeshScan(context);
  StepAttachScan(context);
  // Retry the extra-component mapping until the bone table is readable.
  if (context.extra_build_pending && !context.mesh_scan_candidates.empty() &&
      !context.mesh_scan_running)
    BuildExtraMeshes(context);

  // Motion playback: resolve the file's bone names once the mesh is known, then
  // advance the cursor. The tick detour does the actual pose write.
  bool motion_ready = false;
  if (context.motion_loaded.load(std::memory_order_acquire)) {
    const double seek = context.motion_seek.exchange(-1.0, std::memory_order_acquire);
    double seconds = context.motion_seconds.load(std::memory_order_acquire);
    if (seek >= 0.0) {
      seconds = seek;
      context.motion_seek_serial.fetch_add(1, std::memory_order_release);
      // The animation follows the bar from here until the track has been told where to play; the
      // soft sync below would otherwise pull it straight back to the audio's old position.
      context.motion_seek_pending.store(true, std::memory_order_release);
    }
    motion_ready = ResolveMotionIndices(context);
    if (motion_ready &&
        context.motion_playing.load(std::memory_order_acquire) &&
        delta_seconds > 0.0) {
      double duration{};
      {
        std::lock_guard<std::mutex> lock(context.motion_mutex);
        duration = MotionDuration(context.motion);
      }
      seconds += delta_seconds;
      // Audio is the master clock while a track plays. The device's position cannot be resampled
      // (MCI has no rate control) and restarting the track every few seconds is exactly what the
      // player hears as "it jumps back", so the *animation* clock is pulled towards the audio
      // instead: at most a quarter of its rate, which closes a 0.3 s gap in about two seconds and
      // is invisible. A gap too large to hide snaps the animation, never the audio.
      if (!context.motion_seek_pending.load(std::memory_order_acquire) &&
          g_music_opened.load(std::memory_order_acquire) &&
          g_music_playing.load(std::memory_order_acquire)) {
        const double audio_time =
            g_music_position.load(std::memory_order_acquire) - kMusicLeadSeconds;
        const double error = audio_time - seconds;
        if (std::abs(error) > kMusicHardSnapSeconds) {
          seconds = audio_time;
        } else if (std::abs(error) > 1e-3) {
          const double adjust = (std::max)(-0.25, (std::min)(0.25, error * kMusicSyncGain));
          seconds += delta_seconds * adjust;
        }
      }
      if (seconds > duration) {
        if (context.motion_loop.load(std::memory_order_acquire) && duration > 0.0) {
          seconds = std::fmod(seconds, duration);
          // The track is the master clock and is usually longer than the motion, so without this the
          // loop wraps the animation while the song keeps playing where it was and the two only meet
          // again when one of them ends. A wrap *is* a seek back to the loop's start, so it goes
          // through the same machinery the progress bar uses: the follower restarts the track there,
          // and the animation is not pulled back to the old audio position meanwhile.
          context.motion_seek_serial.fetch_add(1, std::memory_order_release);
          context.motion_seek_pending.store(true, std::memory_order_release);
          context.motion_seek_immediate.store(true, std::memory_order_release);
        } else {
          seconds = duration;
          context.motion_playing.store(false, std::memory_order_release);
        }
      }
    }
    context.motion_seconds.store(seconds, std::memory_order_release);
    context.motion_display_seconds.store(seconds, std::memory_order_release);
    // The follower needs the value just stored, so it runs here rather than at the end of the
    // update: a tick's worth of latency is exactly what "the music lags the progress bar" is.
  }
  // Outside the motion block: picking a track or muting must work before a motion is loaded, and
  // the cursor the follower reads was just published above either way.
  StepMusic(context, delta_seconds);
  const bool motion_active = motion_ready;

  // Keep the extra-component state fresh after every hot reload: both scans are read-only and
  // chunked, so redo them automatically when a motion is loaded and the state for this pawn is
  // missing.
  if (context.motion_loaded.load(std::memory_order_acquire) &&
      !context.mesh_scan_running &&
      !context.mesh_scan_requested.load(std::memory_order_acquire) &&
      !context.attach_scan_requested.load(std::memory_order_acquire)) {
    const auto now = GetTickCount64();
    const bool empty_scan = context.mesh_scan_candidates.empty();
    const bool retry_empty =
        empty_scan && now >= context.next_mesh_scan_retry_ms;
    if (context.mesh_scan_owner != context.runtime.mesh || retry_empty) {
      context.mesh_scan_requested.store(true, std::memory_order_release);
      context.next_mesh_scan_retry_ms = now + 1000;
    } else if (context.attach_parent_offset == 0)
      context.attach_scan_requested.store(true, std::memory_order_release);
  }

  if (context.pose_reset_requested.exchange(false, std::memory_order_acquire)) {
    static_cast<void>(RestorePose(context));
    static_cast<void>(EnsurePoseAnimationMode(context, false));
    static_cast<void>(EnsurePoseForcedLod(context, false));
    context.pose_override_enabled.store(false, std::memory_order_release);
    ResetPoseValues(context);
    context.pose_settings_dirty.store(true, std::memory_order_release);
  }

  const bool master_enabled =
      context.pose_override_enabled.load(std::memory_order_acquire);  std::size_t active_joints{};
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    const auto active_count =
        (std::min)(context.bone_angles.size(),
                   static_cast<std::size_t>(context.runtime.local_space_count));
    for (std::size_t index{}; index != active_count; ++index) {
      const auto &angle = context.bone_angles[index];
      if (angle[0] != 0.0 || angle[1] != 0.0 || angle[2] != 0.0)
        ++active_joints;
    }
  }
  const bool has_root_offset =
      context.requested_root_offset[0].load(std::memory_order_acquire) != 0.0 ||
      context.requested_root_offset[1].load(std::memory_order_acquire) != 0.0 ||
      context.requested_root_offset[2].load(std::memory_order_acquire) != 0.0;
  // A loaded motion drives the pose too, and takes precedence over the manual
  // joint offsets (they stay untouched and come back when playback is off).
  const bool pose_enabled =
      motion_active || (master_enabled && (active_joints != 0 || has_root_offset));
  const bool pose_available = context.runtime.bone_space_data != 0 &&
                              context.runtime.bone_space_count != 0 &&
                              context.runtime.component_space_data != 0 &&
                              context.runtime.component_space_count != 0 &&
                              context.runtime.local_space_data != 0 &&
                              context.runtime.local_space_count != 0;
  const bool force_ok = ApplyMultiThreadedUpdate(context, pose_enabled);
  const bool pose_requested = pose_enabled && pose_available;
  // Two very different things can put the plugin in charge of the camera. Follow mode is a camera
  // mode of its own: it has no timeline and nothing to play, so it takes the camera the moment it
  // is switched on -- which is what makes its sliders do something and the mouse stop turning the
  // view. A loaded camera file is the other: that one *is* a timeline, authored against a
  // particular dance, so it only owns the camera while that dance is actually playing. (The
  // earlier "no playback, no camera" rule was about that file.)
  const bool camera_follow_mode = context.camera_follow.load(std::memory_order_acquire);
  const bool camera_track_ready =
      context.camera_enabled.load(std::memory_order_acquire) &&
      context.camera_loaded.load(std::memory_order_acquire);
  const bool camera_configured = camera_follow_mode || camera_track_ready;
  const bool motion_playing_now =
      context.motion_playing.load(std::memory_order_acquire);
  // Both modes wait for playback, and that is deliberate: with the motion paused the game's view
  // point is often a placeholder the game is not recomputing (measured frozen with x = 0 for
  // seconds), and a shot built against a placeholder is what "the camera stares at one place"
  // turned out to be. Follow mode is an extra source of the shot, not an always-on override.
  const bool camera_requested = camera_configured && motion_playing_now;
  // Size the camera's MMD world before any motion has published the exact ratio: the live
  // character's leg over the reference model's is the same definition the converters bake into
  // `mmdLegLength`, and MMD's bare 8 cm/unit convention is 6.8 % short for a 169 cm character
  // (the log had a 394 cm shot fill 96.7 % of the frame where the file's own framing fills
  // 90.5 %). An MMD-unit motion overwrites this with the file's exact ratio as soon as it plays.
  if (context.camera_enabled.load(std::memory_order_acquire) &&
      context.camera_loaded.load(std::memory_order_acquire) &&
      context.mmd_unit_cm.load(std::memory_order_relaxed) <= 1e-6) {
    const double live_leg = LiveLegLength(context);
    if (live_leg > 1e-6)
      context.mmd_unit_cm.store(live_leg / kReferenceLegUnits, std::memory_order_release);
  }
  // The camera write rides the same mesh tick as the pose, so the hook is installed for it
  // too: that tick is the one that lands after the game has computed its own camera.
  const bool hook_ok = !pose_requested || EnsurePoseTickHook(context);
  // The camera owns the POV only while the dance is playing, so pausing hands the game's camera
  // straight back. Installing it restarts the shot: the anchor (the yaw captured from the game
  // camera's direction to the character) is dropped once, on the first play after a mode goes on,
  // and kept across every later pause so the shot never re-orients itself.
  bool camera_hook_ok = !camera_requested;
  if (camera_requested) {
    if (RefreshCameraPovTarget(context))
      camera_hook_ok = EnsureCameraPovHook(context);
    if (!context.camera_was_enabled) {
      context.camera_was_enabled = true;
      context.camera_anchored.store(false, std::memory_order_release);
      context.camera_anchor_wait = 0;
      context.camera_logged_second.store(-1.0, std::memory_order_release);
    }
    context.camera_log_clock.store(
        context.camera_log_clock.load(std::memory_order_relaxed) + delta_seconds,
        std::memory_order_release);
    // The track shot rides the motion's timeline, the one the two files were authored on together;
    // follow mode has no timeline of its own, and only the readout uses this.
    context.camera_frame.store(
        context.motion_display_seconds.load(std::memory_order_acquire) * 30.0,
        std::memory_order_release);
  } else {
    if (context.camera_pov_hook.id != 0)
      static_cast<void>(ReleaseCameraPovHook(context));
    // Switching everything off (or unloading the file) clears the "already started" flag, so
    // turning a mode back on re-anchors the shot instead of resuming one the user abandoned.
    if (!camera_configured)
      context.camera_was_enabled = false;
  }
  context.camera_hook_ready.store(camera_requested && camera_hook_ok,
                                  std::memory_order_release);
  const bool lod_ok = EnsurePoseForcedLod(context, pose_requested);
  const bool freeze = freeze_enabled || pose_requested;
  const bool freeze_ok = ApplyPause(context, freeze);
  const bool rate_ok = ApplyRate(context, rate_enabled, rate);
  const bool root_ok = ApplyRootMotion(context, root_enabled, root);
  const bool mode_ok = EnsurePoseAnimationMode(context, pose_requested);
  if (!pose_requested) {
    static_cast<void>(RestorePose(context));
    static_cast<void>(RestoreExtraMeshes(context));
    static_cast<void>(ReleasePoseTickHook(context));
  }
  const auto probe_now = GetTickCount64();
  const bool probe_due = probe_now >= context.next_extra_pose_probe_ms;
  if (probe_due) {
    context.next_extra_pose_probe_ms = probe_now + 2000;
    TraceExtraMeshPose(context, pose_requested ? "before-update-write" : "idle");
  }
  if (pose_requested) {
    if (motion_active)
      ApplyMotionPoseDirect(context);
    else
      ApplyPoseOverridesDirect(context);
    if (probe_due)
      TraceExtraMeshPose(context, "after-update-write");
    ResyncExtraMeshes(context);
  }
  if (context.motion_loaded.load(std::memory_order_acquire)) {
    const double seconds =
        context.motion_display_seconds.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(context.motion_mutex);
    std::snprintf(context.motion_status.data(), context.motion_status.size(),
                  "%s%.2f/%.2fs %s", motion_active ? "" : "unmapped ",
                  static_cast<double>(seconds),
                  static_cast<double>(MotionDuration(context.motion)),
                  context.motion_playing.load(std::memory_order_acquire)
                      ? "playing"
                      : "paused");
  }
  std::snprintf(context.pose_status.data(), context.pose_status.size(),
                "joints active: %u fx %u", static_cast<unsigned>(active_joints),
                static_cast<unsigned>(context.extra_resync_count));

  bool settings_saved = true;
  if (context.pose_settings_dirty.exchange(false, std::memory_order_acquire))
    settings_saved = PersistPoseSettings(context);

  if (pose_requested && !hook_ok) {
    PublishSnapshot(context,
                    "pose override unavailable: skeletal tick hook failed");
  } else if (pose_requested && !lod_ok) {
    PublishSnapshot(context, "failed to force pose LOD 0");
  } else if (pose_requested && !mode_ok) {
    PublishSnapshot(context, "failed to switch animation mode to custom");
  } else if (!freeze_ok) {
    PublishSnapshot(context, "failed to apply animation pause flag");
  } else if (!rate_ok) {
    PublishSnapshot(context, "failed to apply animation rate scale");
  } else if (!root_ok) {
    PublishSnapshot(context, "failed to apply root motion scale");
  } else if (pose_requested) {
    PublishSnapshot(context, settings_saved
                                 ? "joint pose active (" +
                                       std::to_string(active_joints) + " joints)"
                                 : "pose active; settings save failed");
  } else if (master_enabled && active_joints != 0 && !force_ok) {
    PublishSnapshot(context,
                    "pose override unavailable: AnimInstance is not available");
  } else if (master_enabled && active_joints != 0) {
    PublishSnapshot(context,
                    "pose override pending: waiting for cached pose transforms");
  } else if (!settings_saved) {
    PublishSnapshot(context, "pose settings save failed");
  } else {
    PublishSnapshot(context, "ok");
  }
}

void RestoreAll(Context &context) noexcept {
  static_cast<void>(RestorePose(context));
  static_cast<void>(RestoreExtraMeshes(context));
  static_cast<void>(EnsurePoseAnimationMode(context, false));
  static_cast<void>(EnsurePoseForcedLod(context, false));
  static_cast<void>(RestoreMultiThreadedUpdate(context));
  static_cast<void>(RestoreRootMotion(context));
  static_cast<void>(RestoreRate(context));
  static_cast<void>(RestorePause(context));
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1 *host,
                                  void **plugin_context) {
  if (host == nullptr || plugin_context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *plugin_context = nullptr;
  auto *context = new (std::nothrow) Context();
  if (context == nullptr)
    return Status(ANOMALY_STATUS_V1_FAILED);
  context->host = host;
  context->localizer = anomaly::plugins::Localizer(host);
  context->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID,
                                              ANOMALY_CORE_SERVICE_V1_VERSION);
  context->signature =
      Query<AnomalySignatureServiceV1>(host, ANOMALY_SIGNATURE_SERVICE_V1_ID,
                                       ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
  context->names =
      Query<AnomalyUe5NamesServiceV1>(host, ANOMALY_UE5_NAMES_SERVICE_V1_ID,
                                      ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
  context->objects =
      Query<AnomalyUe5ObjectsServiceV1>(host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
                                        ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
  context->ui = Query<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID,
                                          ANOMALY_UI_SERVICE_V1_VERSION);
  context->hook = Query<AnomalyHookServiceV1>(host, ANOMALY_HOOK_SERVICE_V1_ID,
                                              ANOMALY_HOOK_SERVICE_V1_VERSION);
  context->config = Query<AnomalyConfigServiceV1>(host,
                                                  ANOMALY_CONFIG_SERVICE_V1_ID,
                                                  ANOMALY_CONFIG_SERVICE_V1_VERSION);
  context->storage =
      Query<AnomalyStorageServiceV1>(host, ANOMALY_STORAGE_SERVICE_V1_ID,
                                     ANOMALY_STORAGE_SERVICE_V1_VERSION);
  context->scheduler =
      Query<AnomalySchedulerServiceV1>(host, ANOMALY_SCHEDULER_SERVICE_V1_ID,
                                       ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
  if (!SchedulerReady(context->scheduler))
    context->scheduler = nullptr;
  if (!CoreReady(context->core) || !SignatureReady(context->signature) ||
      !NamesReady(context->names) ||
      !ObjectsReady(context->objects) ||
      !UiReady(context->ui) || !HookReady(context->hook) ||
      !ConfigReady(context->config) || !StorageReady(context->storage)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "required plugin services are unavailable");
  }
  const auto schema_status = context->config->register_schema(
      context->config->user, anomaly::sdk::StringView(kPoseSettingsSchemaId),
      kPoseSettingsSchemaVersion, Bytes(kPoseSettingsSchema),
      &context->settings_schema);
  if (schema_status.code != ANOMALY_STATUS_V1_OK ||
      context->settings_schema.id == 0 || !LoadPoseSettings(*context)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_FAILED,
                  "character pose settings are invalid");
  }
  *plugin_context = context;
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void *plugin_context) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  if (!ResolveGWorld(*context)) {
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "GWorld discovery signature is unavailable");
  }
  UpdateRuntime(*context, 0.0);
  context->reflection_action_requested.store(5, std::memory_order_release);
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void *plugin_context, std::uint32_t) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  static_cast<void>(ReleasePoseTickHook(*context));
  static_cast<void>(ReleaseCameraPovHook(*context));
  RestoreAll(*context);
  g_active.store(nullptr, std::memory_order_release);
  context->runtime = RuntimeState{};
  RenderSnapshot empty{};
  std::scoped_lock lock(context->state_mutex);
  context->snapshot = empty;
  return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void *plugin_context) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return;
  // Release the music device with the plugin: MCI aliases outlive a hot reload, so leaving this
  // to the static destructor alone makes the next load fail with "alias already in use". The device
  // belongs to the tick thread, so this also asks it to close; `open` already retries with a
  // numbered alias when a previous instance still holds the name.
  {
    std::lock_guard<std::mutex> lock(g_music_mutex);
    g_music_request_close = true;
    if (g_music.opened())
      g_music.Close();
    g_music_request_path.clear();
    g_music_name.clear();
    g_music_paired_for.clear();  // a reload should auto-pair again
  }
  g_music_opened.store(false, std::memory_order_release);
  static_cast<void>(Stop(context, 0));
  if (context->settings_schema.id != 0 && ConfigReady(context->config)) {
    static_cast<void>(context->config->unregister_schema(
        context->config->user, context->settings_schema));
  }
  context->settings_schema = {};
  delete context;
}

void ANOMALY_CALL Update(void *plugin_context, const double delta_seconds) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return;
  try {
    UpdateRuntime(*context, delta_seconds);
  } catch (...) {
    RestoreAll(*context);
    PublishSnapshot(*context, "update failed; overrides restored");
  }
}

bool ReadCurrentBoneTranslation(Context &context,
                                const RenderSnapshot &snapshot,
                                const std::uint32_t bone,
                                std::array<double, 3> &translation) noexcept {
  translation = {0.0, 0.0, 0.0};
  if (!CoreReady(context.core) || snapshot.component_space_data == 0 ||
      bone >= snapshot.component_space_count)
    return false;
  std::uintptr_t transform{};
  if (!AddAddress(snapshot.component_space_data,
                  static_cast<std::uint64_t>(bone) * kTransformSize,
                  transform) ||
      !AddAddress(transform, kTransformTranslationOffset, transform))
    return false;
  return Read(context, transform, translation);
}

void ANOMALY_CALL Draw(void *plugin_context, const AnomalyUiServiceV1 *ui) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr || !UiReady(ui))
    return;

  RenderSnapshot snapshot{};
  {
    std::scoped_lock lock(context->state_mutex);
    snapshot = context->snapshot;
  }

  const std::string title =
      context->localizer.Text("window.title", "Character Pose");
  int open = 1;
  ui->set_next_window_size(ui->user, 380.0F, 0.0F, 4U);
  const int visible = ui->begin_window(
      ui->user, anomaly::sdk::StringView(title), &open, 0);
  if (visible == 0) {
    ui->end_window(ui->user);
    return;
  }

  const std::string status_label =
      context->localizer.Text("status", "Status");
  const std::string status_text = status_label + ": " + snapshot.status.data();
  ui->text(ui->user, anomaly::sdk::StringView(status_text));

  const std::string character_line =
      "Character " + Hex(snapshot.character) + "  Mesh " + Hex(snapshot.mesh);
  ui->text(ui->user, anomaly::sdk::StringView(character_line));
  const std::string anim_line = "AnimInstance " + Hex(snapshot.anim_instance) +
                                "  Mode " + std::to_string(snapshot.animation_mode);
  ui->text(ui->user, anomaly::sdk::StringView(anim_line));
  const std::string pose_line =
      "Bones " + std::to_string(snapshot.bone_space_count) + " / Component " +
      std::to_string(snapshot.component_space_count);
  ui->text(ui->user, anomaly::sdk::StringView(pose_line));

  ui->separator(ui->user);

  int freeze = context->freeze_enabled.load(std::memory_order_acquire) ? 1 : 0;
  const std::string freeze_label =
      context->localizer.Text("freeze", "Pause animation");
  if (ui->checkbox(ui->user, anomaly::sdk::StringView(freeze_label), &freeze) !=
      0) {
    context->freeze_enabled.store(freeze != 0, std::memory_order_release);
  }

  int rate_enabled =
      context->rate_override_enabled.load(std::memory_order_acquire) ? 1 : 0;
  float rate = context->requested_rate_scale.load(std::memory_order_acquire);
  const std::string rate_toggle =
      context->localizer.Text("rate.toggle", "Override animation rate");
  if (ui->checkbox(ui->user, anomaly::sdk::StringView(rate_toggle),
                   &rate_enabled) != 0) {
    context->rate_override_enabled.store(rate_enabled != 0,
                                         std::memory_order_release);
  }
  const std::string rate_label =
      context->localizer.Text("rate", "Rate scale");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(rate_label), &rate,
                       0.0F, 3.0F) != 0) {
    context->requested_rate_scale.store(rate, std::memory_order_release);
  }

  ui->separator(ui->user);

  int pose_enabled =
      context->pose_override_enabled.load(std::memory_order_acquire) ? 1 : 0;
  const std::string pose_toggle =
      context->localizer.Text("pose.toggle", "Override joint pose");
  if (ui->checkbox(ui->user, anomaly::sdk::StringView(pose_toggle),
                   &pose_enabled) != 0) {
    context->pose_override_enabled.store(pose_enabled != 0,
                                         std::memory_order_release);
  }

  auto bone = context->requested_bone_index.load(std::memory_order_acquire);
  std::string selected_name = "None";
  if (bone < snapshot.bone_names.size())
    selected_name = snapshot.bone_names[bone];
  const std::string selected_label =
      context->localizer.Text("pose.bone.selected", "Selected bone");
  const std::string selected_line =
      selected_label + ": " + std::to_string(bone) + " " + selected_name;
  ui->text(ui->user, anomaly::sdk::StringView(selected_line));
  const std::string pose_status_line = std::string(snapshot.pose_status.data());
  ui->text(ui->user, anomaly::sdk::StringView(pose_status_line));

  const bool can_text_input =
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_text)>(
          ui, offsetof(AnomalyUiServiceV1, input_text)) &&
      ui->input_text != nullptr;
  const bool can_bone_list =
      can_text_input &&
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_child)>(
          ui, offsetof(AnomalyUiServiceV1, begin_child)) &&
      ui->begin_child != nullptr &&
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_child)>(
          ui, offsetof(AnomalyUiServiceV1, end_child)) &&
      ui->end_child != nullptr &&
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::filter_match)>(
          ui, offsetof(AnomalyUiServiceV1, filter_match)) &&
      ui->filter_match != nullptr;

  if (can_bone_list) {
    const std::string filter_label =
        context->localizer.Text("pose.filter", "Bone filter");
    static_cast<void>(ui->input_text(
        ui->user, anomaly::sdk::StringView(filter_label),
        context->bone_filter.data(), context->bone_filter.size(),
        ANOMALY_UI_TEXT_INPUT_V1_NONE));

    const auto set_filter = [&](const char *value) {
      std::snprintf(context->bone_filter.data(), context->bone_filter.size(),
                    "%s", value);
    };
    int quick_button_index = 0;
    const auto quick_button = [&](const char *value, const std::string &label) {
      if (ui->button(ui->user, anomaly::sdk::StringView(label), 56.0F, 0.0F) != 0)
        set_filter(value);
      ++quick_button_index;
      if (quick_button_index % 6 != 0)
        ui->same_line(ui->user, 0.0F, 4.0F);
    };
    quick_button("arm", context->localizer.Text("filter.arm", "Arm"));
    quick_button("forearm", context->localizer.Text("filter.elbow", "Elbow"));
    quick_button("hand", context->localizer.Text("filter.hand", "Hand"));
    quick_button("thigh", context->localizer.Text("filter.thigh", "Thigh"));
    quick_button("calf", context->localizer.Text("filter.knee", "Knee"));
    quick_button("foot", context->localizer.Text("filter.foot", "Foot"));
    quick_button("clavicle", context->localizer.Text("filter.shoulder", "Shoulder"));
    quick_button("neck", context->localizer.Text("filter.neck", "Neck"));
    quick_button("head", context->localizer.Text("filter.head", "Head"));
    quick_button("spine", context->localizer.Text("filter.spine", "Spine"));
    quick_button("pelvis", context->localizer.Text("filter.pelvis", "Pelvis"));
    quick_button("", context->localizer.Text("filter.all", "All"));

    // The host pushes its Child stack entry when `begin_child` is called, whether or not ImGui
    // culled the child, so `end_child` has to be called either way: skipping it when the child is
    // scrolled out of view leaves the stack unbalanced and the host faults the whole plugin.
    const int bone_child_open =
        ui->begin_child(ui->user, anomaly::sdk::StringView("bone-list"), 0.0F, 240.0F, 0U);
    if (bone_child_open != 0) {
      if (snapshot.bone_names.empty()) {
        const std::string empty_label = context->localizer.Text(
            "pose.bones.empty", "Bone names not loaded; press Load Bones.");
        ui->text(ui->user, anomaly::sdk::StringView(empty_label));
      } else {
        const std::string_view filter(context->bone_filter.data());
        for (std::size_t index{}; index != snapshot.bone_names.size(); ++index) {
          if (ui->filter_match(ui->user, anomaly::sdk::StringView(filter),
                               anomaly::sdk::StringView(
                                   snapshot.bone_names[index])) == 0)
            continue;
          const std::string bone_item =
              std::to_string(index) + " " + snapshot.bone_names[index];
          if (ui->button(ui->user, anomaly::sdk::StringView(bone_item), 0.0F,
                         0.0F) != 0) {
            context->requested_bone_index.store(
                static_cast<std::uint32_t>(index), std::memory_order_release);
          }
        }
      }
    }
    ui->end_child(ui->user);
  } else {
    const std::string bone_label =
        context->localizer.Text("pose.bone", "Bone index");
    double bone_value = static_cast<double>(bone);
    if (ui->input_double(ui->user, anomaly::sdk::StringView(bone_label),
                         &bone_value, 1.0, 8.0) != 0) {
      if (bone_value < 0.0)
        bone_value = 0.0;
      if (bone_value > static_cast<double>(kMaximumBoneIndex))
        bone_value = static_cast<double>(kMaximumBoneIndex);
      context->requested_bone_index.store(
          static_cast<std::uint32_t>(bone_value), std::memory_order_release);
    }
  }

  float pitch = 0.0F;
  float yaw = 0.0F;
  float roll = 0.0F;
  {
    std::lock_guard<std::mutex> lock(context->pose_angles_mutex);
    if (bone < context->bone_angles.size()) {
      pitch = static_cast<float>(context->bone_angles[bone][0]);
      yaw = static_cast<float>(context->bone_angles[bone][1]);
      roll = static_cast<float>(context->bone_angles[bone][2]);
    }
  }

  bool apply_pose_now = false;
  const auto changed_angle = [&]() {
    context->pose_override_enabled.store(true, std::memory_order_release);
    context->pose_settings_dirty.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(context->pose_angles_mutex);
      if (bone >= context->bone_angles.size())
        context->bone_angles.resize(static_cast<std::size_t>(bone) + 1);
      context->bone_angles[bone] = {pitch, yaw, roll};
    }
    apply_pose_now = true;
  };

  float root_x = static_cast<float>(
      context->requested_root_offset[0].load(std::memory_order_acquire));
  float root_y = static_cast<float>(
      context->requested_root_offset[1].load(std::memory_order_acquire));
  float root_z = static_cast<float>(
      context->requested_root_offset[2].load(std::memory_order_acquire));
  const auto changed_root_offset = [&]() {
    context->pose_override_enabled.store(true, std::memory_order_release);
    context->pose_settings_dirty.store(true, std::memory_order_release);
    context->requested_root_offset[0].store(root_x, std::memory_order_release);
    context->requested_root_offset[1].store(root_y, std::memory_order_release);
    context->requested_root_offset[2].store(root_z, std::memory_order_release);
    apply_pose_now = true;
  };

  const std::string pitch_label =
      context->localizer.Text("pose.pitch", "Pitch");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(pitch_label), &pitch,
                       -180.0F, 180.0F) != 0)
    changed_angle();
  ui->same_line(ui->user, 0.0F, 4.0F);
  const std::string pitch_reset =
      context->localizer.Label("pose.reset.pitch", "重置", "reset-pitch");
  if (ui->button(ui->user, anomaly::sdk::StringView(pitch_reset), 42.0F,
                 0.0F) != 0) {
    pitch = 0.0F;
    changed_angle();
  }

  const std::string yaw_label =
      context->localizer.Text("pose.yaw", "Yaw");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(yaw_label), &yaw,
                       -180.0F, 180.0F) != 0)
    changed_angle();
  ui->same_line(ui->user, 0.0F, 4.0F);
  const std::string yaw_reset =
      context->localizer.Label("pose.reset.yaw", "重置", "reset-yaw");
  if (ui->button(ui->user, anomaly::sdk::StringView(yaw_reset), 42.0F,
                 0.0F) != 0) {
    yaw = 0.0F;
    changed_angle();
  }

  const std::string roll_label =
      context->localizer.Text("pose.roll", "Roll");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(roll_label), &roll,
                       -180.0F, 180.0F) != 0)
    changed_angle();
  ui->same_line(ui->user, 0.0F, 4.0F);
  const std::string roll_reset =
      context->localizer.Label("pose.reset.roll", "重置", "reset-roll");
  if (ui->button(ui->user, anomaly::sdk::StringView(roll_reset), 42.0F,
                 0.0F) != 0) {
    roll = 0.0F;
    changed_angle();
  }

  ui->separator(ui->user);
  const std::string body_x_label =
      context->localizer.Text("pose.body.x", "X");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(body_x_label),
                       &root_x, -1000.0F, 1000.0F) != 0)
    changed_root_offset();
  ui->same_line(ui->user, 0.0F, 4.0F);
  const std::string body_x_reset =
      context->localizer.Label("pose.reset.body.x", "重置", "reset-body-x");
  if (ui->button(ui->user, anomaly::sdk::StringView(body_x_reset), 42.0F,
                 0.0F) != 0) {
    root_x = 0.0F;
    changed_root_offset();
  }

  const std::string body_y_label =
      context->localizer.Text("pose.body.y", "Y");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(body_y_label),
                       &root_y, -1000.0F, 1000.0F) != 0)
    changed_root_offset();
  ui->same_line(ui->user, 0.0F, 4.0F);
  const std::string body_y_reset =
      context->localizer.Label("pose.reset.body.y", "重置", "reset-body-y");
  if (ui->button(ui->user, anomaly::sdk::StringView(body_y_reset), 42.0F,
                 0.0F) != 0) {
    root_y = 0.0F;
    changed_root_offset();
  }

  const std::string body_z_label =
      context->localizer.Text("pose.body.z", "Z");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(body_z_label),
                       &root_z, -1000.0F, 1000.0F) != 0)
    changed_root_offset();
  ui->same_line(ui->user, 0.0F, 4.0F);
  const std::string body_z_reset =
      context->localizer.Label("pose.reset.body.z", "重置", "reset-body-z");
  if (ui->button(ui->user, anomaly::sdk::StringView(body_z_reset), 42.0F,
                 0.0F) != 0) {
    root_z = 0.0F;
    changed_root_offset();
  }

  if (apply_pose_now)
    ApplyPoseOverridesDirect(*context);
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string reset_label =
      context->localizer.Text("pose.reset", "Reset All");
  const bool can_confirm_popup =
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::open_popup)>(
          ui, offsetof(AnomalyUiServiceV1, open_popup)) &&
      ui->open_popup != nullptr &&
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_popup_modal)>(
          ui, offsetof(AnomalyUiServiceV1, begin_popup_modal)) &&
      ui->begin_popup_modal != nullptr &&
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_popup)>(
          ui, offsetof(AnomalyUiServiceV1, end_popup)) &&
      ui->end_popup != nullptr &&
      HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::close_current_popup)>(
          ui, offsetof(AnomalyUiServiceV1, close_current_popup)) &&
      ui->close_current_popup != nullptr;
  if (ui->button(ui->user, anomaly::sdk::StringView(reset_label), 70.0F,
                 0.0F) != 0) {
    if (can_confirm_popup)
      ui->open_popup(ui->user, anomaly::sdk::StringView("pose-reset-confirm"));
    else
      context->pose_reset_requested.store(true, std::memory_order_release);
  }

  ui->separator(ui->user);
  const std::string action_label =
      context->localizer.Text("action.status", "Action");
  const std::string action_text =
      action_label + ": " + std::string(snapshot.reflection_status.data());
  ui->text(ui->user, anomaly::sdk::StringView(action_text));
  const std::string refresh_anim_label =
      context->localizer.Text("action.refresh_anim", "Refresh Anim");
  if (ui->button(ui->user, anomaly::sdk::StringView(refresh_anim_label), 80.0F,
                 0.0F) != 0)
    context->reflection_action_requested.store(4, std::memory_order_release);
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string load_bones_label =
      context->localizer.Text("action.load_bones", "Load Bones");
  if (ui->button(ui->user, anomaly::sdk::StringView(load_bones_label), 80.0F,
                 0.0F) != 0)
    context->reflection_action_requested.store(6, std::memory_order_release);
  if (context->pose_export_name[0] == '\0') {
    std::snprintf(context->pose_export_name.data(), context->pose_export_name.size(),
                "pose.json");
  }
  ui->text(ui->user, anomaly::sdk::StringView(context->localizer.Text("pose.file.name", "File name")));
  ui->same_line(ui->user, 0.0F, 6.0F);
  ui->input_text(ui->user, anomaly::sdk::StringView("##pose-export-name"),
                 context->pose_export_name.data(),
                 context->pose_export_name.size(), 0);
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string choose_folder_label =
      context->localizer.Text("pose.choose.folder", "Choose folder");
  if (ui->button(ui->user, anomaly::sdk::StringView(choose_folder_label), 90.0F,
                 0.0F) != 0) {
    const auto selected = ChooseFolder(context->pose_export_folder);
    if (selected) {
      const std::string folder_utf8 = WideToUtf8(selected->native());
      if (!folder_utf8.empty())
        context->pose_export_folder = folder_utf8;
    }
  }
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string export_label =
      context->localizer.Text("pose.export", "Export");
  if (ui->button(ui->user, anomaly::sdk::StringView(export_label), 60.0F,
                 0.0F) != 0)
    context->pose_file_action_requested.store(1, std::memory_order_release);
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string export_skeleton_label =
      context->localizer.Text("pose.export.skeleton", "Export Skeleton");
  if (ui->button(ui->user, anomaly::sdk::StringView(export_skeleton_label), 110.0F,
                 0.0F) != 0)
    context->pose_file_action_requested.store(3, std::memory_order_release);

  ui->text(ui->user, anomaly::sdk::StringView(context->localizer.Text("pose.import.file", "Import file")));
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string choose_file_label =
      context->localizer.Text("pose.choose.file", "Choose file");
  if (ui->button(ui->user, anomaly::sdk::StringView(choose_file_label), 90.0F,
                 0.0F) != 0) {
    const auto selected = ChooseFile(context->pose_import_file);
    if (selected) {
      const std::string file_utf8 = WideToUtf8(selected->native());
      if (!file_utf8.empty())
        context->pose_import_file = file_utf8;
    }
  }
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string import_label =
      context->localizer.Text("pose.import", "Import");
  if (ui->button(ui->user, anomaly::sdk::StringView(import_label), 60.0F,
                 0.0F) != 0)
    context->pose_file_action_requested.store(2, std::memory_order_release);

  // --- MMD motion playback -------------------------------------------------
  ui->separator(ui->user);
  const std::string motion_title =
      context->localizer.Text("motion.title", "MMD motion");
  ui->text(ui->user, anomaly::sdk::StringView(motion_title));
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string motion_convert_label =
      context->localizer.Text("motion.convert", "Convert VMD and load");
  if (ui->button(ui->user, anomaly::sdk::StringView(motion_convert_label), 110.0F,
                 0.0F) != 0) {
    // One click from here: pick the VMD, export this character's skeleton on the game
    // thread, convert with the shipped reference bone table, then load the result.
    const auto selected = ChooseFile(context->motion_file, FileKind::Vmd);
    if (selected) {
      const std::string file_utf8 = WideToUtf8(selected->native());
      if (!file_utf8.empty()) {
        context->motion_file = file_utf8;
        context->pose_file_action_requested.store(5, std::memory_order_release);
      }
    }
  }  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string motion_load_label =
      context->localizer.Text("motion.load", "Load motion");
  if (ui->button(ui->user, anomaly::sdk::StringView(motion_load_label), 100.0F,
                 0.0F) != 0) {
    // Two kinds of file answer to this button. A source VMD is converted again -- a converted
    // document is built against *this* character's skeleton, so after a character switch the
    // cached file has the right bone names but the wrong rest pose. An already-converted
    // document is loaded as it is, which is also how a conversion made elsewhere (or with
    // another reference model) gets played without redoing it here.
    if (context->motion_file.empty()) {
      const auto selected = ChooseFile(context->motion_file, FileKind::Motion);
      if (selected) {
        const std::string file_utf8 = WideToUtf8(selected->native());
        if (!file_utf8.empty())
          context->motion_file = file_utf8;
      }
    }
    if (!context->motion_file.empty())
      context->pose_file_action_requested.store(
          PathIsConvertedMotion(context->motion_file) ? 4 : 5, std::memory_order_release);
  }

  const bool motion_loaded =
      context->motion_loaded.load(std::memory_order_acquire);
  const bool motion_playing =
      context->motion_playing.load(std::memory_order_acquire);
  const std::string motion_play_label = context->localizer.Text(
      motion_playing ? "motion.pause" : "motion.play",
      motion_playing ? "Pause" : "Play");
  if (ui->button(ui->user, anomaly::sdk::StringView(motion_play_label), 60.0F,
                 0.0F) != 0)
    context->motion_playing.store(!motion_playing, std::memory_order_release);
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string motion_stop_label =
      context->localizer.Text("motion.stop", "Stop");
  if (ui->button(ui->user, anomaly::sdk::StringView(motion_stop_label), 55.0F,
                 0.0F) != 0) {
    context->motion_playing.store(false, std::memory_order_release);
    context->motion_seek.store(0.0, std::memory_order_release);
  }
  ui->same_line(ui->user, 0.0F, 6.0F);
  const std::string motion_unload_label =
      context->localizer.Text("motion.unload", "Unload");
  if (ui->button(ui->user, anomaly::sdk::StringView(motion_unload_label), 70.0F,
                 0.0F) != 0) {
    context->motion_loaded.store(false, std::memory_order_release);
    context->motion_playing.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(context->motion_mutex);
    context->motion = Context::MotionTrack{};
  }
  ui->same_line(ui->user, 0.0F, 6.0F);
  int motion_loop = context->motion_loop.load(std::memory_order_acquire) ? 1 : 0;
  const std::string motion_loop_label =
      context->localizer.Text("motion.loop", "Loop");
  if (ui->checkbox(ui->user, anomaly::sdk::StringView(motion_loop_label),
                   &motion_loop) != 0)
    context->motion_loop.store(motion_loop != 0, std::memory_order_release);
  int motion_root =
      context->motion_apply_root.load(std::memory_order_acquire) ? 1 : 0;
  const std::string motion_root_label =
      context->localizer.Text("motion.root", "Root motion");
  if (ui->checkbox(ui->user, anomaly::sdk::StringView(motion_root_label),
                   &motion_root) != 0)
    context->motion_apply_root.store(motion_root != 0, std::memory_order_release);
  ui->same_line(ui->user, 0.0F, 6.0F);
  int motion_planar = context->motion_lock_planar.load(std::memory_order_acquire) ? 1 : 0;
  const std::string motion_planar_label =
      context->localizer.Text("motion.planar", "Lock planar motion");
  if (ui->checkbox(ui->user, anomaly::sdk::StringView(motion_planar_label), &motion_planar) != 0)
    context->motion_lock_planar.store(motion_planar != 0, std::memory_order_release);
  ui->same_line(ui->user, 0.0F, 6.0F);
  int motion_ref = context->motion_reference_unity.load(std::memory_order_acquire) ? 1 : 0;
  const std::string motion_ref_label =
      context->localizer.Text("motion.ref", "Unity Miku reference");
  if (ui->checkbox(ui->user, anomaly::sdk::StringView(motion_ref_label), &motion_ref) != 0)
    context->motion_reference_unity.store(motion_ref != 0, std::memory_order_release);

  // Music: pick a track and it follows the motion's own transport and playhead, so judging sync
  // needs no manual transport (which only fought the follower). MP3/WAV go through the system
  // codecs (MCI), nothing is shipped, and the game's own audio is untouched. Mute keeps the
  // playhead running so unmuting stays in sync.
  {
    // The device belongs to the game tick thread (see StepMusic), because MCI does not share a
    // device between threads. Everything here is a request or a display of published state -- this
    // panel never issues a command itself.
    const std::string music_pick_label =
        context->localizer.Text("music.pick", "Load music");
    if (ui->button(ui->user, anomaly::sdk::StringView(music_pick_label), 90.0F, 0.0F) != 0) {
      const auto selected = ChooseFile(context->motion_file, FileKind::Audio);
      if (selected.has_value()) {
        std::lock_guard<std::mutex> lock(g_music_mutex);
        g_music_request_path = WideToUtf8(selected->wstring());
        g_music_paired_for = context->motion_file;  // do not also auto-pair over this choice
      }
    }
    ui->same_line(ui->user, 0.0F, 6.0F);
    int music_mute = g_music_muted.load(std::memory_order_acquire) ? 1 : 0;
    const std::string music_mute_label = context->localizer.Text("music.mute", "Mute music");
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(music_mute_label), &music_mute) != 0) {
      std::lock_guard<std::mutex> lock(g_music_mutex);
      g_music_request_mute = music_mute != 0;
      g_music_request_mute_pending = true;
    }
    {
      // Published by the tick: name, position, length, error. No MCI call ever happens here.
      std::string name;
      std::string error;
      {
        std::lock_guard<std::mutex> lock(g_music_mutex);
        name = g_music_name;
        error = g_music_error;
      }
      const bool opened = g_music_opened.load(std::memory_order_acquire);
      const double position = g_music_position.load(std::memory_order_acquire);
      const double length = g_music_length.load(std::memory_order_acquire);
      if (opened) {
        char line[192]{};
        std::snprintf(line, sizeof(line), "%s  %d:%04.1f / %d:%04.1f%s", name.c_str(),
                      static_cast<int>(position / 60.0), position - 60.0 * (position / 60.0),
                      static_cast<int>(length / 60.0), length - 60.0 * (length / 60.0),
                      error.empty() ? "" : "  (error: see log)");
        ui->text(ui->user, anomaly::sdk::StringView(std::string(line)));
      } else if (!error.empty()) {
        ui->text(ui->user,
                 anomaly::sdk::StringView(context->localizer.Text("music.error", "music") +
                                          ": " + error));
      }
    }
  }

  // The motion's progress bar sits with the track it follows rather than at the bottom of the panel:
  // moving either one alone is what made "the music and the bar" hard to use together.
  float motion_time = static_cast<float>(
      context->motion_display_seconds.load(std::memory_order_acquire));
  float motion_duration = 0.0F;
  {
    std::lock_guard<std::mutex> lock(context->motion_mutex);
    motion_duration = static_cast<float>(MotionDuration(context->motion));
  }
  if (motion_duration <= 0.0F)
    motion_duration = 1.0F;
  if (motion_time > motion_duration)
    motion_time = motion_duration;
  const std::string motion_time_label =
      context->localizer.Text("motion.time", "Time (s)");
  if (ui->slider_float(ui->user, anomaly::sdk::StringView(motion_time_label),
                       &motion_time, 0.0F, motion_duration) != 0)
    context->motion_seek.store(static_cast<double>(motion_time),
                               std::memory_order_release);
  if (motion_loaded) {
    const std::string motion_status(context->motion_status.data());
    ui->text(ui->user, anomaly::sdk::StringView(motion_status));
  }

  // Camera VMD: load the file, then report the key range and the frame the motion is on. The
  // camera itself is driven by the view-point hook below (ported from the free-camera plugin),
  // which reads this track from the render thread.
  {
    const std::string camera_pick_label =
        context->localizer.Text("camera.pick", "Load camera VMD");
    if (ui->button(ui->user, anomaly::sdk::StringView(camera_pick_label), 110.0F, 0.0F) != 0) {
      const auto selected = ChooseFile(context->camera_file, FileKind::Vmd);
      if (selected.has_value()) {
        std::ifstream file(selected->wstring().c_str(), std::ios::binary);
        std::string error = "could not open the file";
        if (file) {
          std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
          std::lock_guard<std::mutex> lock(context->camera_mutex);
          if (context->camera.Load(bytes, WideToUtf8(selected->wstring()), &error)) {
            context->camera_file = WideToUtf8(selected->wstring());
            context->camera_loaded.store(true, std::memory_order_release);
          } else {
            context->camera_loaded.store(false, std::memory_order_release);
          }
        }
        // Remember the choice: the next plugin (re)load restores it instead of asking again.
        context->pose_settings_dirty.store(true, std::memory_order_release);
        if (!context->camera_loaded.load(std::memory_order_acquire))
          LogDiagnostic(*context, "betterpose camera vmd failed (" +
                                      WideToUtf8(selected->wstring()) + "): " + error);
      }
    }
    ui->same_line(ui->user, 0.0F, 6.0F);
    int camera_enabled =
        context->camera_enabled.load(std::memory_order_acquire) ? 1 : 0;
    const std::string camera_enable_label =
        context->localizer.Text("camera.enable", "Drive camera");
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(camera_enable_label),
                     &camera_enabled) != 0) {
      context->camera_enabled.store(camera_enabled != 0, std::memory_order_release);
      context->pose_settings_dirty.store(true, std::memory_order_release);
    }
    ui->same_line(ui->user, 0.0F, 6.0F);
    int camera_follow = context->camera_follow.load(std::memory_order_acquire) ? 1 : 0;
    const std::string camera_follow_label =
        context->localizer.Text("camera.follow", "Follow the character");
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(camera_follow_label),
                     &camera_follow) != 0) {
      context->camera_follow.store(camera_follow != 0, std::memory_order_release);
      context->pose_settings_dirty.store(true, std::memory_order_release);
    }
    if (camera_follow != 0) {
      // Only the two numbers the mode needs: distance back along the anchored direction, height
      // above the character's feet. The aim (their chest) is taken from the character itself.
      float follow_distance = static_cast<float>(
          context->camera_follow_distance_cm.load(std::memory_order_acquire));
      const std::string follow_distance_label =
          context->localizer.Text("camera.follow_distance", "Follow distance (cm)");
      if (ui->slider_float(ui->user, anomaly::sdk::StringView(follow_distance_label),
                           &follow_distance, 100.0F, 1500.0F) != 0) {
        context->camera_follow_distance_cm.store(static_cast<double>(follow_distance),
                                                 std::memory_order_release);
        context->pose_settings_dirty.store(true, std::memory_order_release);
      }
      float follow_height = static_cast<float>(
          context->camera_follow_height_cm.load(std::memory_order_acquire));
      const std::string follow_height_label =
          context->localizer.Text("camera.follow_height", "Follow height (cm)");
      if (ui->slider_float(ui->user, anomaly::sdk::StringView(follow_height_label),
                           &follow_height, 0.0F, 300.0F) != 0) {
        context->camera_follow_height_cm.store(static_cast<double>(follow_height),
                                               std::memory_order_release);
        context->pose_settings_dirty.store(true, std::memory_order_release);
      }
      int follow_vertical =
          context->camera_follow_vertical.load(std::memory_order_acquire) ? 1 : 0;
      const std::string follow_vertical_label =
          context->localizer.Text("camera.follow_vertical", "Height follows the character");
      if (ui->checkbox(ui->user, anomaly::sdk::StringView(follow_vertical_label),
                       &follow_vertical) != 0) {
        context->camera_follow_vertical.store(follow_vertical != 0,
                                             std::memory_order_release);
        context->pose_settings_dirty.store(true, std::memory_order_release);
      }
    }
    // The state line shows whenever a mode is on, even with no file: follow mode needs no file, and
    // "why is nothing happening" has to be answerable from the panel.
    if (context->camera_loaded.load(std::memory_order_acquire) || camera_follow != 0 ||
        camera_enabled != 0) {
      std::size_t count = 0;
      double first = 0.0;
      double last = 0.0;
      std::string name;
      if (context->camera_loaded.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(context->camera_mutex);
        count = context->camera.keys.size();
        first = context->camera.first_frame;
        last = context->camera.last_frame;
        name = context->camera.file;
      }
      const double frame = context->camera_frame.load(std::memory_order_acquire);
      // How far the dance has actually moved the character: this is the number the follow camera
      // tracks, so it is the one to watch when asking "why is the shot not following".
      double offset_cm = 0.0;
      if (camera_follow != 0) {
        const double dx =
            context->motion_applied_offset[0].load(std::memory_order_acquire);
        const double dy =
            context->motion_applied_offset[1].load(std::memory_order_acquire);
        offset_cm = std::sqrt(dx * dx + dy * dy);
      }
      std::string state;
      if (camera_follow != 0)
        state = context->localizer.Text("camera.state.following",
                                        "following (the file above is ignored)");
      else if (!motion_playing)
        state = context->localizer.Text("camera.state.off", "off until the motion plays");
      else if (camera_enabled != 0) {
        if (!context->camera_manager_resolved.load(std::memory_order_acquire))
          state = context->localizer.Text("camera.state.no_camera", "no view camera");
        else if (!context->camera_hook_ready.load(std::memory_order_acquire))
          state = context->localizer.Text("camera.state.no_hook", "no accessor hook");
        else if (!context->camera_anchored.load(std::memory_order_acquire))
          state = context->localizer.Text("camera.state.waiting",
                                          "waiting for the first frame");
        else
          state = context->localizer.Text("camera.state.driving", "driving");
      }
      char line[320]{};
      if (camera_follow != 0)
        std::snprintf(line, sizeof(line), "%s  |  %s %.0f cm",
                      context->localizer.Text("camera.state.follow_mode", "follow mode").c_str(),
                      context->localizer
                          .Text("camera.state.displaced", "the dance has moved them")
                          .c_str(),
                      offset_cm);
      else if (name.empty())
        std::snprintf(line, sizeof(line), "%s  %s",
                      context->localizer.Text("camera.state.no_file", "no camera VMD loaded")
                          .c_str(),
                      state.c_str());
      else
        std::snprintf(line, sizeof(line), "%s  %zu keys  f%.0f..%.0f  now f%.0f  %s",
                      name.substr(name.find_last_of("\\/") + 1).c_str(), count, first, last,
                      frame, state.c_str());
      ui->text(ui->user, anomaly::sdk::StringView(std::string(line)));
    }
  }

  if (can_confirm_popup) {
    int confirm_open = 1;
    const std::string confirm_id = "pose-reset-confirm";
    if (ui->begin_popup_modal(ui->user, anomaly::sdk::StringView(confirm_id),
                              &confirm_open, 0U) != 0) {
      const std::string confirm_text = context->localizer.Text(
          "pose.reset.confirm", "Reset all bone and body offsets?");
      ui->text(ui->user, anomaly::sdk::StringView(confirm_text));
      const std::string confirm_yes =
          context->localizer.Text("pose.reset.confirm.yes", "Reset");
      if (ui->button(ui->user, anomaly::sdk::StringView(confirm_yes), 90.0F,
                     0.0F) != 0) {
        context->pose_reset_requested.store(true, std::memory_order_release);
        ui->close_current_popup(ui->user);
      }
      ui->same_line(ui->user, 0.0F, 6.0F);
      const std::string confirm_cancel =
          context->localizer.Text("pose.reset.confirm.cancel", "Cancel");
      if (ui->button(ui->user, anomaly::sdk::StringView(confirm_cancel), 90.0F,
                     0.0F) != 0)
        ui->close_current_popup(ui->user);
      ui->end_popup(ui->user);
    }
  }

  if (!snapshot.pose_available) {
    const std::string warning = context->localizer.Text(
        "pose.unavailable", "Pose edit is inactive until the authoritative bone-space pose buffer is populated.");
    ui->text(ui->user, anomaly::sdk::StringView(warning));
  }

  ui->end_window(ui->user);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1 *descriptor) {
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *descriptor = {sizeof(*descriptor),
                 ANOMALY_PLUGIN_API_V1_MAJOR,
                 ANOMALY_PLUGIN_API_V1_MINOR,
                 anomaly::sdk::StringView("anomaly.builtin.better-pose"),
                 anomaly::sdk::StringView("Better Pose"),
                 anomaly::sdk::StringView("Anomaly"),
                 anomaly::sdk::StringView("1.0.0"),
                 Load,
                 Start,
                 Stop,
                 Unload,
                 Update,
                 Draw};
  return anomaly::sdk::Ok();
}
