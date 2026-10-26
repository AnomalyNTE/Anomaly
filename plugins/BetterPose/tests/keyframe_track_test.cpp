#include "../keyframe_track.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
namespace kf = better_pose::keyframes;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool Near(const double a, const double b, const double epsilon = 1e-6) {
  return std::abs(a - b) <= epsilon;
}

kf::Key PoseKey(const std::uint32_t frame, const double arm_pitch, const double root_x = 0.0,
                const kf::Ease ease = kf::Ease::Linear) {
  kf::Key key;
  key.frame = frame;
  key.ease = ease;
  key.has_pose = true;
  if (arm_pitch != 0.0)
    key.bones.push_back({"Bip001-R-UpperArm", arm_pitch, 0.0, 0.0});
  key.root_offset = {root_x, 0.0, 0.0};
  return key;
}

const kf::BoneKey *FindBone(const kf::Sampled &sampled, const char *name) {
  for (const auto &bone : sampled.bones)
    if (bone.name == name)
      return &bone;
  return nullptr;
}

void KeysStaySortedAndReplace() {
  kf::Track track;
  track.Set(PoseKey(60, 10));
  track.Set(PoseKey(0, 0));
  track.Set(PoseKey(30, 5));
  Check(track.Size() == 3, "three keys");
  Check(track.Keys()[0].frame == 0 && track.Keys()[1].frame == 30 && track.Keys()[2].frame == 60,
        "keys sorted by frame");
  track.Set(PoseKey(30, 99));
  Check(track.Size() == 3, "setting an existing frame replaces it");
  Check(track.Keys()[1].bones.front().pitch == 99.0, "the replacement holds the new pose");
  Check(track.Find(30) == 1 && track.Find(31) == track.Size(), "find by exact frame");
  Check(track.LastFrame() == 60, "last frame");
}

void MoveRemoveEase() {
  kf::Track track;
  track.Set(PoseKey(0, 0));
  track.Set(PoseKey(30, 10));
  track.Set(PoseKey(60, 20));
  const std::size_t moved = track.Move(1, 90);
  Check(moved == 2 && track.Keys()[2].frame == 90, "moving re-sorts the key");
  Check(track.Move(2, 0) == 0 && track.Size() == 2, "moving onto another key replaces it");
  Check(track.Keys()[0].bones.front().pitch == 10.0, "the moved key won");
  Check(track.SetEase(0, kf::Ease::InOut) && track.Keys()[0].ease == kf::Ease::InOut, "ease");
  Check(track.Remove(1) && track.Size() == 1, "remove");
  Check(!track.Remove(5), "remove out of range");
}

void LinearPoseInterpolation() {
  kf::Track track;
  track.Set(PoseKey(0, 0, 0));
  track.Set(PoseKey(30, 40, 30));
  const auto mid = track.Sample(15);
  Check(mid.has_pose, "sample has pose");
  const auto *arm = FindBone(mid, "Bip001-R-UpperArm");
  Check(arm != nullptr, "a bone in one key appears in the sample");
  Check(Near(arm->pitch, 20.0, 1e-6), "single-axis slerp is linear in the angle");
  Check(Near(mid.root_offset[0], 15.0), "body offset lerps");
  // Hold before the first and after the last key. The first key has the arm
  // at rest, which is not stored at all (only moved bones are recorded).
  const auto before = track.Sample(-10);
  Check(before.has_pose && FindBone(before, "Bip001-R-UpperArm") == nullptr &&
            Near(before.root_offset[0], 0.0),
        "before the first key holds it");
  Check(Near(FindBone(track.Sample(500), "Bip001-R-UpperArm")->pitch, 40.0),
        "after the last key holds it");
  Check(Near(track.Sample(500).root_offset[0], 30.0), "after the last key holds the offset");
  // A key exactly on the frame is that key.
  Check(Near(FindBone(track.Sample(30), "Bip001-R-UpperArm")->pitch, 40.0), "on the key");
}

void EaseInOut() {
  kf::Track track;
  track.Set(PoseKey(0, 0, 0, kf::Ease::InOut));
  track.Set(PoseKey(100, 0, 100));
  Check(Near(track.Sample(50).root_offset[0], 50.0), "smoothstep passes the middle at half");
  Check(track.Sample(10).root_offset[0] < 10.0, "eases in: slower than linear near the start");
  Check(track.Sample(90).root_offset[0] > 90.0, "eases out: slower than linear near the end");
  Check(Near(kf::ApplyEase(kf::Ease::InOut, 0.0), 0.0) &&
            Near(kf::ApplyEase(kf::Ease::InOut, 1.0), 1.0),
        "ends are fixed");
}

void ShortestWayAcross180() {
  kf::Track track;
  kf::Key a;
  a.frame = 0;
  a.ease = kf::Ease::Linear;
  a.has_pose = true;
  a.bones.push_back({"Head", 0.0, 170.0, 0.0});
  kf::Key b = a;
  b.frame = 10;
  b.bones.front().yaw = -170.0;
  track.Set(a);
  track.Set(b);
  const auto *head = FindBone(track.Sample(5), "Head");
  Check(head != nullptr, "head sampled");
  // Halfway from 170 to -170 the short way is 180, never 0.
  Check(Near(std::abs(head->yaw), 180.0, 1e-6), "yaw crosses 180, not back through 0");
}

void CombinedRotationRoundTrips() {
  // A general rotation survives the quaternion round trip the sampler uses.
  const auto q = kf::FromRotator(25.0, -60.0, 15.0);
  const auto back = kf::ToRotator(q);
  Check(Near(back[0], 25.0, 1e-6) && Near(back[1], -60.0, 1e-6) && Near(back[2], 15.0, 1e-6),
        "rotator -> quat -> rotator");
  // Halfway between a rotation and itself is itself.
  const auto same = kf::InterpolateRotator({25.0, -60.0, 15.0}, {25.0, -60.0, 15.0}, 0.5);
  Check(Near(same[0], 25.0, 1e-6) && Near(same[1], -60.0, 1e-6) && Near(same[2], 15.0, 1e-6),
        "interpolating equal rotations");
}

void BoneOnlyInOneKeyGoesToRest() {
  kf::Track track;
  track.Set(PoseKey(0, 30));   // arm at 30
  track.Set(PoseKey(20, 0));   // arm not recorded: at rest
  const auto *arm = FindBone(track.Sample(10), "Bip001-R-UpperArm");
  Check(arm != nullptr && Near(arm->pitch, 15.0, 1e-6), "a bone missing from a key is at rest");
}

void PartsInterpolateIndependently() {
  kf::Track track;
  kf::Key start = PoseKey(0, 0);
  start.has_camera = true;
  start.camera.focus = {0.0, 0.0, 100.0};
  start.camera.yaw = 0.0;
  start.camera.distance = 400.0;
  start.camera.fov = 60.0F;
  track.Set(start);
  // A pose-only key in the middle must not freeze the camera.
  track.Set(PoseKey(50, 45));
  kf::Key end = PoseKey(100, 90);
  end.has_camera = true;
  end.camera.focus = {100.0, 0.0, 100.0};
  end.camera.yaw = 90.0;
  end.camera.distance = 100.0;
  end.camera.fov = 30.0F;
  track.Set(end);

  const auto at25 = track.Sample(25);
  Check(at25.has_camera, "camera sampled between its own keys");
  Check(Near(at25.camera.focus[0], 25.0), "camera interpolates over its keys only (0..100)");
  Check(Near(at25.camera.yaw, 22.5), "camera yaw");
  Check(Near(at25.camera.distance, 400.0 * std::pow(0.25, 0.25), 1e-6),
        "distance interpolates geometrically");
  Check(Near(at25.camera.fov, 52.5, 1e-4), "lens");
  Check(Near(FindBone(at25, "Bip001-R-UpperArm")->pitch, 22.5, 1e-6),
        "pose interpolates over its own keys (0..50)");
  Check(!at25.has_expression, "no expression keyed, none sampled");
}

void CameraYawWrapsAndGameLens() {
  kf::Track track;
  kf::Key a;
  a.frame = 0;
  a.ease = kf::Ease::Linear;
  a.has_camera = true;
  a.camera.yaw = 170.0;
  a.camera.fov = 0.0F;  // the game's lens
  kf::Key b = a;
  b.frame = 10;
  b.camera.yaw = -170.0;
  b.camera.fov = 40.0F;
  track.Set(a);
  track.Set(b);
  const auto mid = track.Sample(5);
  Check(Near(std::abs(mid.camera.yaw), 180.0, 1e-6), "camera yaw takes the short way");
  Check(Near(mid.camera.fov, 40.0, 1e-6), "a game-lens key keeps the other key's lens");
}

void Expressions() {
  kf::Track track;
  kf::Key a;
  a.frame = 0;
  a.ease = kf::Ease::Linear;
  a.has_expression = true;
  a.morphs = {{"jawOpen", 1.0F}, {"EL_Blink", 0.5F}};
  kf::Key b = a;
  b.frame = 10;
  b.morphs = {{"jawOpen", 0.0F}, {"mouthSmile", 1.0F}};
  track.Set(a);
  track.Set(b);
  const auto mid = track.Sample(5);
  Check(mid.has_expression && mid.morphs.size() == 3, "union of morphs");
  for (const auto &m : mid.morphs) {
    if (m.name == "jawOpen")
      Check(Near(m.weight, 0.5, 1e-6), "jawOpen 1 -> 0");
    else if (m.name == "EL_Blink")
      Check(Near(m.weight, 0.25, 1e-6), "a morph missing from the next key fades to 0");
    else if (m.name == "mouthSmile")
      Check(Near(m.weight, 0.5, 1e-6), "a morph missing from the previous key fades in");
  }
}

void EmptyAndSingle() {
  kf::Track track;
  const auto none = track.Sample(10);
  Check(!none.has_pose && !none.has_expression && !none.has_camera, "an empty track samples nothing");
  track.Set(PoseKey(40, 12));
  Check(Near(FindBone(track.Sample(0), "Bip001-R-UpperArm")->pitch, 12.0) &&
            Near(FindBone(track.Sample(99), "Bip001-R-UpperArm")->pitch, 12.0),
        "a single key holds everywhere");
}

void TrackStateComparesKeys() {
  kf::TrackState a;
  a.keys.push_back(PoseKey(0, 10));
  kf::TrackState b = a;
  Check(a.SameAs(b), "equal states");
  b.keys[0].bones[0].pitch = 11.0;
  Check(!a.SameAs(b), "a changed bone differs");
  b = a;
  b.keys[0].frame = 5;
  Check(!a.SameAs(b), "a moved key differs");
  b = a;
  b.keys[0].ease = kf::Ease::InOut;
  Check(!a.SameAs(b), "a changed ease differs");
}

void FrameIsClamped() {
  kf::Track track;
  track.Set(PoseKey(kf::kMaximumFrame + 1000, 1));
  Check(track.Keys().front().frame == kf::kMaximumFrame, "frames beyond the maximum are clamped");
}
}  // namespace

int main() {
  KeysStaySortedAndReplace();
  MoveRemoveEase();
  LinearPoseInterpolation();
  EaseInOut();
  ShortestWayAcross180();
  CombinedRotationRoundTrips();
  BoneOnlyInOneKeyGoesToRest();
  PartsInterpolateIndependently();
  CameraYawWrapsAndGameLens();
  Expressions();
  EmptyAndSingle();
  TrackStateComparesKeys();
  FrameIsClamped();
  std::cout << "PASS sorted/replace, move/remove/ease, linear pose, ease in-out, 180 wrap, "
               "round trip, bone to rest, independent parts, camera wrap/lens, expressions, "
               "empty/single, track state, frame clamp\n";
  return 0;
}
