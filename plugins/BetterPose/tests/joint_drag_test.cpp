#include "../plugin.cpp"

#include <cstdlib>
#include <iostream>

// Joint drag math: the rotator conversion must round-trip through the joint
// sliders, the screen-space solve must land the joint on the cursor (or on the
// nearest reachable point), and the world rotation must be expressed in the
// pose model's parent space.
namespace fixture {
void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

double QuatDistance(const Quatd &a, const Quatd &b) {
  const double dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 1.0 - (std::min)(dot, 1.0);
}

// A pinhole camera at the origin looking down +X (UE forward), +Y right, +Z up.
struct Camera {
  double focal{800.0};
  float cx{960.0F};
  float cy{540.0F};
  bool operator()(const double world[3], float screen[2]) const {
    if (world[0] <= 1.0)
      return false;
    screen[0] = cx + static_cast<float>(focal * world[1] / world[0]);
    screen[1] = cy - static_cast<float>(focal * world[2] / world[0]);
    return true;
  }
};

void RotatorRoundTrip() {
  const double samples[][3]{{0, 0, 0},     {10, 20, 30},   {-45, 170, -90},
                            {89, -30, 12}, {-89, 60, 170}, {33, -179, 179},
                            {5, 0, -120},  {0, 90, 0},     {-60, -60, 60}};
  for (const auto &sample : samples) {
    const Quatd q = RotatorToQuat(sample[0], sample[1], sample[2]);
    const auto angles = QuatToRotator(q);
    Check(angles[0] >= -180.0 && angles[0] <= 180.0 && angles[1] >= -180.0 &&
              angles[1] <= 180.0 && angles[2] >= -180.0 && angles[2] <= 180.0,
          "rotator components stay inside the slider range");
    const Quatd back = RotatorToQuat(angles[0], angles[1], angles[2]);
    Check(QuatDistance(q, back) < 1e-10, "rotator round trip preserves the rotation");
  }
  // Gimbal lock: the angles may differ, the rotation may not.
  for (const double pitch : {90.0, -90.0}) {
    const Quatd q = RotatorToQuat(pitch, 40.0, 25.0);
    const auto angles = QuatToRotator(q);
    const Quatd back = RotatorToQuat(angles[0], angles[1], angles[2]);
    Check(QuatDistance(q, back) < 1e-8, "rotator round trip survives gimbal lock");
  }
}

// A camera looking along an arbitrary direction, to make sure nothing depends
// on the view being axis aligned.
struct TiltedCamera {
  Quatd view = QuatNormalize(RotatorToQuat(-25.0, 40.0, 10.0));  // camera to world
  Vec3d eye{-200.0, -150.0, 120.0};
  double focal{900.0};
  bool operator()(const double world[3], float screen[2]) const {
    const Vec3d local = QuatRotateVector(
        QuatConjugate(view), Vec3d{world[0] - eye.x, world[1] - eye.y, world[2] - eye.z});
    if (local.x <= 1.0)
      return false;
    screen[0] = 960.0F + static_cast<float>(focal * local.y / local.x);
    screen[1] = 540.0F - static_cast<float>(focal * local.z / local.x);
    return true;
  }
};

// One simulated drag: the same code path the AHUD callback runs, returning
// where the joint lands on screen.
template <typename Cam>
struct Drag {
  const Cam &camera;
  Vec3d pivot;
  Vec3d offset;
  Vec3d axis;
  float pivot_screen[2]{};
  bool ready{};
  double last_raw{};
  double angle{};
  bool Start(const float press[2]) {
    double sense{};
    const double p[3]{pivot.x, pivot.y, pivot.z};
    if (!camera(p, pivot_screen) || !ViewRotationAxis(camera, pivot, axis, sense))
      return false;
    axis = Vec3d{axis.x * sense, axis.y * sense, axis.z * sense};
    AdvanceDragAngle(pivot_screen, press, ready, last_raw, angle);
    return true;
  }
  bool Move(const float cursor[2], float joint[2]) {
    AdvanceDragAngle(pivot_screen, cursor, ready, last_raw, angle);
    const Vec3d moved = QuatRotateVector(
        QuatFromRotationVector({axis.x * angle, axis.y * angle, axis.z * angle}), offset);
    const double world[3]{pivot.x + moved.x, pivot.y + moved.y, pivot.z + moved.z};
    return camera(world, joint);
  }
};

double ScreenAngle(const float centre[2], const float point[2]) {
  return std::atan2(static_cast<double>(point[1]) - centre[1],
                    static_cast<double>(point[0]) - centre[0]);
}

template <typename Cam>
void DragFollowsCursor(const Cam &camera, const char *name) {
  const Vec3d pivot{250.0, 30.0, 60.0};
  const Vec3d offset{10.0, -15.0, -40.0};
  const double start_world[3]{pivot.x + offset.x, pivot.y + offset.y, pivot.z + offset.z};
  float press[2]{};
  Check(camera(start_world, press), name);
  Drag<Cam> drag{camera, pivot, offset, {}};
  Check(drag.Start(press), "drag starts");
  const float *centre = drag.pivot_screen;
  const double radius = std::hypot(press[0] - centre[0], press[1] - centre[1]);
  const double start_angle = ScreenAngle(centre, press);

  // Holding the mouse still, for many frames, never moves the bone.
  float joint[2]{};
  for (int frame = 0; frame != 120; ++frame) {
    Check(drag.Move(press, joint), "hold projects");
    Check(std::hypot(joint[0] - press[0], joint[1] - press[1]) < 1e-3,
          "holding the mouse still holds the joint still");
  }

  // Sweep the cursor 60 degrees around the pivot, both ways, in small steps.
  for (const double direction : {1.0, -1.0}) {
    Drag<Cam> sweep{camera, pivot, offset, {}};
    Check(sweep.Start(press), "sweep starts");
    for (int frame = 1; frame <= 30; ++frame) {
      const double a = start_angle + direction * 2.0 * frame * 3.14159265358979323846 / 180.0;
      const float cursor[2]{centre[0] + static_cast<float>(radius * 1.3 * std::cos(a)),
                            centre[1] + static_cast<float>(radius * 1.3 * std::sin(a))};
      Check(sweep.Move(cursor, joint), "sweep projects");
      const double turned = WrapAngle(ScreenAngle(centre, joint) - start_angle);
      Check(turned * direction > 0.0, "the bone turns the way the cursor turns");
    }
    // The bone turns by exactly the cursor's angle; on screen the joint
    // follows to within perspective (the circle it sweeps is tilted against
    // the screen, 1-5 degrees here over a 60 degree sweep).
    Check(std::abs(sweep.angle - direction * 60.0 * 3.14159265358979323846 / 180.0) < 1e-6,
          "the bone turns by the cursor's swept angle");
    const double final_angle = WrapAngle(ScreenAngle(centre, joint) - start_angle);
    Check(std::abs(final_angle - direction * 60.0 * 3.14159265358979323846 / 180.0) < 0.105,
          "the joint's screen angle tracks the cursor's");
    // And after the sweep, holding still stays still.
    const double held = sweep.angle;
    const float last[2]{centre[0] + static_cast<float>(radius * 1.3 * std::cos(start_angle + direction * 60.0 * 3.14159265358979323846 / 180.0)),
                        centre[1] + static_cast<float>(radius * 1.3 * std::sin(start_angle + direction * 60.0 * 3.14159265358979323846 / 180.0))};
    for (int frame = 0; frame != 60; ++frame)
      sweep.Move(last, joint);
    Check(std::abs(sweep.angle - held) < 1e-9, "holding after a sweep keeps the angle");
  }

  // Past half a turn keeps going (unwrapped), and returning undoes it.
  Drag<Cam> full{camera, pivot, offset, {}};
  Check(full.Start(press), "full turn starts");
  for (int frame = 1; frame <= 72; ++frame) {
    const double a = start_angle + frame * 5.0 * 3.14159265358979323846 / 180.0;
    const float cursor[2]{centre[0] + static_cast<float>(radius * std::cos(a)),
                          centre[1] + static_cast<float>(radius * std::sin(a))};
    full.Move(cursor, joint);
  }
  Check(std::abs(full.angle - 2.0 * 3.14159265358979323846) < 1e-6,
        "a full sweep accumulates a full turn");

  // Grazing the pivot does not spin the bone.
  Drag<Cam> graze{camera, pivot, offset, {}};
  Check(graze.Start(press), "graze drag starts");
  const float on_pivot[2]{centre[0] + 1.0F, centre[1] - 1.0F};
  graze.Move(on_pivot, joint);
  Check(graze.angle == 0.0, "a cursor on the pivot holds the angle");
}

// A biped arm: 0 root, 1 upperarm, 2 upperarm_twist (on 1), 3 lowerarm,
// 4 lowerarm_twist (on 3), 5 hand, 6 hand_adjust (on 5, no children).
void StackedBones() {
  const std::vector<std::int32_t> parents{-1, 0, 1, 1, 3, 3, 5};
  const std::vector<Vec3d> positions{{0, 0, 0},  {0, 0, 0},  {0, 0, 0}, {30, 0, 0},
                                     {30, 0, 0}, {55, 0, 0}, {55, 0, 0}};
  const auto distance2 = [&](const std::uint32_t a, const std::uint32_t b) {
    const Vec3d &p = positions[a];
    const Vec3d &q = positions[b];
    return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z);
  };
  Check(ResolveDragPivot(parents, 5, distance2) == 3, "hand turns the lower arm");
  Check(ResolveDragPivot(parents, 3, distance2) == 1, "lower arm turns the upper arm");
  Check(ResolveDragPivot(parents, 6, distance2) == 3,
        "a helper stacked on the hand skips the hand and turns the lower arm");
  Check(ResolveDragPivot(parents, 4, distance2) == 1,
        "a twist bone on the lower arm skips it and turns the upper arm");
  Check(ResolveDragPivot(parents, 1, distance2) == -1,
        "a bone stacked on the root has nothing to turn");
  Check(ResolveDragPivot(parents, 0, distance2) == -1, "the root has no pivot");
  const std::vector<std::int32_t> cycle{1, 0};
  Check(ResolveDragPivot(cycle, 0, [](std::uint32_t, std::uint32_t) { return 0.0; }) == -1,
        "a cyclic hierarchy ends");

  std::vector<std::uint32_t> weight;
  CountDescendants(parents, weight);
  Check(weight[0] == 6 && weight[1] == 5 && weight[3] == 3 && weight[5] == 1 && weight[6] == 0,
        "descendant counts");
  // Screen: the stacked pairs land within a fraction of a pixel of each other.
  const std::vector<std::array<float, 2>> screen{{100, 100},   {100, 100},   {100.4F, 99.7F},
                                                 {200, 100},   {200.3F, 100}, {280, 100},
                                                 {279.8F, 100.2F}};
  const std::vector<std::uint8_t> valid(7, 1);
  for (const float jitter : {-0.5F, 0.0F, 0.5F}) {
    Check(PickOverlayJoint(screen, valid, weight, 280.0F + jitter, 101.0F, 12.0F, 3.0F) == 5,
          "clicking the hand grabs the hand, not its helper");
    Check(PickOverlayJoint(screen, valid, weight, 201.0F + jitter, 99.0F, 12.0F, 3.0F) == 3,
          "clicking the elbow grabs the lower arm, not its twist bone");
  }
  Check(PickOverlayJoint(screen, valid, weight, 240.0F, 100.0F, 12.0F, 3.0F) ==
            (std::numeric_limits<std::uint32_t>::max)(),
        "nothing within the radius picks nothing");
  // A lone joint a few pixels off still wins over a heavier one farther away.
  Check(PickOverlayJoint(screen, valid, weight, 272.0F, 100.0F, 12.0F, 3.0F) == 5,
        "the nearest point wins when nothing is stacked on it");
}

void BodyOnlyMask() {
  // 0 pelvis, 1 spine, 2 head, 3 Bone_hairR00 (on head), 4 hair helper with no
  // keyword (on 3), 5 Bn_qun_01 skirt (on pelvis), 6 twist (stays), 7 hat,
  // 8 finger (stays), 9 Bn_tail (not secondary).
  const std::vector<std::string> names{"Bip001-Pelvis", "Bip001-Spine", "Bip001-Head",
                                       "Bone_hairR00",  "helper_end",   "Bn_qun_01",
                                       "Bip001-L-UpperArmTwist", "hat_root", "Bip001-L-Finger0",
                                       "Bn_tail_01"};
  const std::vector<std::int32_t> parents{-1, 0, 1, 2, 3, 0, 1, 2, 1, 0};
  std::vector<std::uint8_t> hidden;
  const auto total = BuildOverlayHiddenMask(names, parents, hidden);
  const std::vector<std::uint8_t> expected{0, 0, 0, 1, 1, 1, 0, 1, 0, 0};
  Check(hidden == expected, "hair, its chain, skirt and hat are hidden; body, twist, finger, tail stay");
  Check(total == 4, "hidden count");

  // The NTE player skeleton, as logged in game: engine control bones beside
  // the Bip001 rig and a facial rig under the head.
  const std::vector<std::string> nte{
      "Root",          "Bip001",       "Bip001-Pelvis", "Bip001-Head",   "Bip001-L-Hand",
      "Bip001-Prop2",  "Bone_head",    "mouth",         "Bon_uplip_M",   "BON_eyelid_up_R",
      "Bon_eyeball_L", "root_foot",    "foot_r",        "root_hand",     "hand_l",
      "wq_root_L",     "P_wq_L",       "VB VB_Curve",   "VB ik_foot_l_offset",
      "Bip001-L-Foot", "Bn_m_tail_001"};
  const std::vector<std::int32_t> nte_parents{-1, 0, 1, 2, 2, 4, 3, 6, 7, 6, 6,
                                              0, 11, 0, 13, 0, 15, 0, 12, 2, 2};
  BuildOverlayHiddenMask(nte, nte_parents, hidden);
  const std::vector<std::uint8_t> nte_expected{0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1,
                                               1, 1, 1, 1, 1, 1, 1, 1, 0, 0};
  Check(hidden == nte_expected,
        "control bones, virtual bones, prop sockets and the face rig are hidden");
  BuildOverlayHiddenMask(nte, nte_parents, hidden, false);
  Check(hidden[7] == 0 && hidden[8] == 0 && hidden[9] == 0 && hidden[10] == 0 &&
            hidden[12] == 1 && hidden[17] == 1,
        "showing the face keeps the control bones hidden");
  Check(BuildOverlayHiddenMask({}, parents, hidden) == 0 &&
            std::all_of(hidden.begin(), hidden.end(), [](auto v) { return v == 0; }),
        "without names nothing is hidden");
  const std::vector<std::int32_t> cycle{1, 0};
  Check(BuildOverlayHiddenMask({"a", "b"}, cycle, hidden) == 0, "a cyclic hierarchy ends");
}

void TwoBoneIk() {
  // A bent arm: shoulder at the origin, elbow 30 cm out and bent forward,
  // hand 25 cm further.
  const Vec3d root{0, 0, 0};
  const Vec3d mid{0, -30, 0};
  const Vec3d end{8, -52, -6};
  const double upper = V3Length(V3Sub(mid, root));
  const double lower = V3Length(V3Sub(end, mid));
  const Vec3d original_bend = V3Sub(mid, V3Scale(end, V3Dot(mid, end) / V3Dot(end, end)));
  const auto apply = [&](const TwoBoneRotations &turn, Vec3d &new_mid, Vec3d &new_end) {
    new_mid = V3Add(root, QuatRotateVector(turn.root, V3Sub(mid, root)));
    new_end = V3Add(new_mid, QuatRotateVector(turn.mid,
                                              QuatRotateVector(turn.root, V3Sub(end, mid))));
  };
  const Vec3d targets[]{{15, -40, 10}, {-10, -35, -20}, {0, -20, 5}, {30, -10, 0}};
  for (const auto &target : targets) {
    Vec3d new_mid;
    Vec3d new_end;
    apply(SolveTwoBone(root, mid, end, target), new_mid, new_end);
    Check(std::abs(V3Length(V3Sub(new_mid, root)) - upper) < 1e-9, "IK keeps the upper length");
    Check(std::abs(V3Length(V3Sub(new_end, new_mid)) - lower) < 1e-9, "IK keeps the lower length");
    Check(V3Length(V3Sub(new_end, target)) < 1e-6, "IK puts the hand on a reachable target");
    // The elbow keeps bending to the same side it bent before.
    const Vec3d bend = V3Sub(new_mid, V3Scale(new_end, V3Dot(new_mid, new_end) / V3Dot(new_end, new_end)));
    Check(V3Dot(bend, original_bend) > 0.0, "IK keeps the elbow on its side");
  }
  // Out of reach: the arm points at the target, almost straight, no NaN.
  const Vec3d distant{200, -200, 0};
  Vec3d new_mid;
  Vec3d new_end;
  apply(SolveTwoBone(root, mid, end, distant), new_mid, new_end);
  Check(std::isfinite(new_end.x) && std::isfinite(new_end.y) && std::isfinite(new_end.z),
        "an unreachable target stays finite");
  const double reach = V3Length(new_end);
  Check(reach > (upper + lower) * 0.99 && reach < upper + lower, "an unreachable target nearly straightens");
  const Vec3d toward = V3Scale(distant, 1.0 / V3Length(distant));
  Check(V3Dot(V3Scale(new_end, 1.0 / reach), toward) > 0.999, "an unreachable target is pointed at");
  // The press itself (target == hand) changes nothing.
  const auto still = SolveTwoBone(root, mid, end, end);
  Check(QuatDistance(still.root, Quatd{}) < 1e-6 && QuatDistance(still.mid, Quatd{}) < 1e-6,
        "no drag, no turn");

  Check(IsIkEndBone("Bip001-L-Hand") && IsIkEndBone("Bip001-R-Foot"), "hands and feet use IK");
  Check(!IsIkEndBone("Bip001-L-Finger0") && !IsIkEndBone("Bip001-R-Toe0") &&
            !IsIkEndBone("hand_adjust") && !IsIkEndBone("Bip001-L-Forearm"),
        "fingers, toes, helpers and forearms stay one-bone");

  // Screen-plane basis: moving along it by (dx, dy) moves the projection by (dx, dy).
  const TiltedCamera camera;
  const Vec3d point{150, 40, 30};
  Vec3d axis;
  double sense{};
  Check(ViewRotationAxis(camera, point, axis, sense), "axis");
  Vec3d right;
  Vec3d down;
  Check(ViewPlaneBasis(camera, point, axis, right, down), "basis");
  float base[2]{};
  float moved[2]{};
  const double p[3]{point.x, point.y, point.z};
  camera(p, base);
  const Vec3d shifted = V3Add(point, V3Add(V3Scale(right, 6.0), V3Scale(down, -4.0)));
  const double q[3]{shifted.x, shifted.y, shifted.z};
  camera(q, moved);
  Check(std::abs(moved[0] - base[0] - 6.0F) < 0.05F && std::abs(moved[1] - base[1] + 4.0F) < 0.05F,
        "the view-plane basis moves one pixel per unit");
  Check(std::abs(V3Dot(right, axis)) < 1e-9 && std::abs(V3Dot(down, axis)) < 1e-9,
        "the view-plane basis keeps the depth");
}

template <typename Cam>
void TwistAndDepth(const Cam &camera, const Vec3d &eye, const char *name) {
  const Vec3d pivot{250.0, 30.0, 60.0};
  const Vec3d offset{10.0, -15.0, -40.0};
  const Vec3d joint = V3Add(pivot, offset);

  // Toward the camera really points at the eye.
  Vec3d toward;
  Check(TowardCamera(camera, joint, toward), name);
  const Vec3d to_eye = V3Sub(eye, joint);
  Check(V3Dot(toward, V3Scale(to_eye, 1.0 / V3Length(to_eye))) > 0.999,
        "toward-camera points at the eye");

  // IK depth: moving the target along it brings the joint nearer, and keeps
  // it on the same screen spot.
  const Vec3d nearer = V3Add(joint, V3Scale(toward, 15.0));
  Check(V3Length(V3Sub(eye, nearer)) < V3Length(to_eye) - 14.9, "IK depth moves toward the eye");
  float a[2]{};
  float b[2]{};
  const double pa[3]{joint.x, joint.y, joint.z};
  const double pb[3]{nearer.x, nearer.y, nearer.z};
  camera(pa, a);
  camera(pb, b);
  Check(std::hypot(a[0] - b[0], a[1] - b[1]) < 0.01, "IK depth does not move the joint on screen");

  // One-bone depth: turning about cross(bone, toward) brings the joint nearer.
  Vec3d depth_axis = V3Cross(offset, toward);
  depth_axis = V3Scale(depth_axis, 1.0 / V3Length(depth_axis));
  const double angle = 10.0 * 3.14159265358979323846 / 180.0;
  const Vec3d turned = V3Add(pivot, QuatRotateVector(QuatFromRotationVector(
                                                        {depth_axis.x * angle,
                                                         depth_axis.y * angle,
                                                         depth_axis.z * angle}),
                                                    offset));
  Check(V3Length(V3Sub(eye, turned)) < V3Length(to_eye), "a positive depth notch comes nearer");
  Check(std::abs(V3Length(V3Sub(turned, pivot)) - V3Length(offset)) < 1e-9,
        "depth swing keeps the bone length");

  // Twist: about the bone's own axis, the joint does not move at all.
  const double twist = 1.2;
  const Vec3d bone_axis = V3Scale(offset, twist / V3Length(offset));
  const Vec3d twisted = QuatRotateVector(
      QuatFromRotationVector({bone_axis.x, bone_axis.y, bone_axis.z}), offset);
  Check(V3Length(V3Sub(twisted, offset)) < 1e-9, "twist keeps the joint in place");
  // ...but it does turn the bone: a perpendicular direction rotates by the angle.
  const Vec3d side = V3Cross(offset, Vec3d{0, 0, 1});
  const Vec3d side_turned = QuatRotateVector(
      QuatFromRotationVector({bone_axis.x, bone_axis.y, bone_axis.z}), side);
  const double cosine = V3Dot(side, side_turned) / (V3Length(side) * V3Length(side_turned));
  Check(std::abs(std::acos(std::clamp(cosine, -1.0, 1.0)) - twist) < 1e-9,
        "twist turns the bone by the requested angle");
}

void CircleCost() {
  constexpr double kPi = 3.14159265358979323846;
  for (const float radius : {3.0F, 5.0F, 7.0F, 10.0F, 13.0F, 16.0F}) {
    const auto detail = CircleDetailFor(radius);
    const auto rows = DiscRows(radius, detail.strip);
    const std::size_t calls = rows.size() + static_cast<std::size_t>(detail.segments) + 1;
    Check(calls <= 22, "a marker costs at most 22 draw calls");
    // The ring's chords stay within 0.4 px of the true circle.
    const double sagitta = radius * (1.0 - std::cos(kPi / detail.segments));
    Check(sagitta < 0.4, "the ring looks round");
    double area{};
    for (const auto &row : rows)
      area += static_cast<double>(row.height) * row.half_width * 2.0;
    const double circle = kPi * radius * radius;
    Check(std::abs(area - circle) / circle < 0.08, "the coarse fill still covers the circle");
  }
  const auto standard = CircleDetailFor(7.0F);
  Check(DiscRows(7.0F, standard.strip).size() + static_cast<std::size_t>(standard.segments) + 1 == 16,
        "the default 7 px marker is 16 calls");
}

void DiscStrips() {
  for (const float radius : {3.0F, 7.0F, 10.5F, 16.0F}) {
    const auto rows = DiscRows(radius, 2.0F);
    Check(!rows.empty(), "a disc has rows");
    double area{};
    float next_top = -radius;
    for (const auto &row : rows) {
      Check(std::abs(row.top - next_top) < 1e-4F, "rows are contiguous and never overlap");
      Check(row.half_width <= radius + 1e-4F, "rows stay inside the circle");
      area += static_cast<double>(row.height) * row.half_width * 2.0;
      next_top = row.top + row.height;
    }
    Check(std::abs(next_top - radius) < 1e-4F, "rows reach the bottom of the circle");
    const double circle = 3.14159265358979323846 * radius * radius;
    Check(std::abs(area - circle) / circle < 0.06, "the strips fill the circle's area");
  }
  Check(DiscRows(0.0F, 2.0F).empty() && DiscRows(5.0F, 0.0F).empty(),
        "degenerate discs draw nothing");
}

void BehindCameraIsRejected() {
  const auto never = [](const double *, float *) { return false; };
  Vec3d axis;
  double sense{};
  Check(!ViewRotationAxis(never, Vec3d{}, axis, sense), "no projection means no axis");
}

void ParentSpaceConversion() {
  // local = offset * base, world = parent * local. Adding a world rotation R
  // must give parent * offset' * base == R * parent * offset * base.
  const Quatd parent = QuatNormalize(RotatorToQuat(20.0, -35.0, 50.0));
  const Quatd offset = RotatorToQuat(10.0, 5.0, -15.0);
  const Quatd base = RotatorToQuat(-70.0, 12.0, 3.0);
  const Quatd world_rotation = QuatFromRotationVector({0.3, -0.2, 0.5});
  const Quatd next = ApplyWorldRotationToOffset(parent, world_rotation, offset);
  const Quatd lhs = QuatMultiply(parent, QuatMultiply(next, base));
  const Quatd rhs =
      QuatMultiply(world_rotation, QuatMultiply(parent, QuatMultiply(offset, base)));
  Check(QuatDistance(lhs, rhs) < 1e-12, "world rotation maps into the parent space");
  const Quatd identity = ApplyWorldRotationToOffset(parent, Quatd{}, offset);
  Check(QuatDistance(identity, offset) < 1e-12, "no rotation leaves the offset alone");
}
}  // namespace fixture

int main() {
  fixture::RotatorRoundTrip();
  fixture::DragFollowsCursor(fixture::Camera{}, "axis-aligned camera projects the joint");
  fixture::DragFollowsCursor(fixture::TiltedCamera{}, "tilted camera projects the joint");
  fixture::BehindCameraIsRejected();
  fixture::ParentSpaceConversion();
  fixture::StackedBones();
  fixture::BodyOnlyMask();
  fixture::DiscStrips();
  fixture::CircleCost();
  fixture::TwoBoneIk();
  fixture::TwistAndDepth(fixture::Camera{}, Vec3d{0, 0, 0}, "axis-aligned toward camera");
  {
    const fixture::TiltedCamera tilted;
    fixture::TwistAndDepth(tilted, tilted.eye, "tilted toward camera");
  }
  std::cout << "PASS rotator round trip, hold still, sweep both ways, full turn, pivot "
               "graze, tilted camera, behind camera, parent space, stacked bones, body-only "
               "mask, two-bone IK, twist, depth, disc strips, circle cost\n";
  return 0;
}
