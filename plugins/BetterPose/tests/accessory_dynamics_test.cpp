#include "../accessory_dynamics.hpp"
#include <cstdlib>
#include <iostream>

using namespace better_pose::accessory;
namespace {
void Check(bool value,const char* message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
double RotationDegrees(Quat a,Quat b) {
  const Quat delta=Multiply(a,Inverse(b));
  return 2.0*std::acos((std::min)(1.0,std::abs(delta.w)))*180.0/3.14159265358979323846;
}
Bone Make(Vec p,Quat q={}) { return {q.x,q.y,q.z,q.w,p.x,p.y,p.z,0,1,1,1,0}; }
Frame Motion(double t) {
  const double a=.45*std::sin(t*2);
  return {{20*std::sin(t*3),8*std::sin(t),170+4*std::sin(t*2)},
          {0,std::sin(a/2),0,std::cos(a/2)},1};
}
struct Rig {
  // Both sides, a fork, a rigid tip child, and a separate hard ornament.
  std::vector<std::int32_t> parents{-1,0,1,2,2,3,4,0,7,8,0,5};
  std::vector<std::string> names{"Root","hairR00","hairR01","hairR02","hairSide",
      "hairR03","hairSideTip","hairL00","hairL01","hairL02","Hat","tip_marker"};
  std::vector<Bone> bind{Make({}),Make({12,0,0}),Make({12,0,-10}),Make({12,0,-20}),
      Make({18,0,-18}),Make({12,0,-30}),Make({24,0,-26}),Make({-12,0,0}),
      Make({-12,0,-11}),Make({-12,0,-22}),Make({0,0,10}),Make({14,0,-32})};
};
void CheckPose(const Rig& rig,const std::vector<Bone>& pose) {
  Check(Length(Position(pose[0])-Position(rig.bind[0]))<1e-8,"root translated");
  Check(Length(Position(pose[10])-Position(rig.bind[10]))<1e-8,"ornament translated");
  for (std::size_t i=0;i<pose.size();++i) {
    for (double v:pose[i]) Check(std::isfinite(v),"nonfinite pose");
    const Quat q=Rotation(pose[i]);
    Check(std::abs(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1)<1e-8,"nonunit rotation");
    Check(pose[i][8]==1 && pose[i][9]==1 && pose[i][10]==1,"scale changed");
    if (rig.parents[i]<0) continue;
    const auto p=rig.parents[i];
    const Vec rest=Rotate(Inverse(Rotation(rig.bind[p])),Position(rig.bind[i])-Position(rig.bind[p]));
    const Vec offset=Rotate(Inverse(Rotation(pose[p])),Position(pose[i])-Position(pose[p]));
    Check(Length(offset-rest)<1e-7,"child offset changed: stretched or stale descendant");
  }
  const Quat leaf=Multiply(Inverse(Rotation(pose[5])),Rotation(pose[11]));
  Check(std::abs(leaf.w-1)<1e-8,"rigid leaf failed to inherit parent rotation");
}
void Classifications() {
  for (const char* name:{"Bone_hair", "Bn_l_xiu", "cloth_01", "B_ribbon_00", "qun_01",
                         "piao", "gongpai", "lalian", "xiong", "tie", "skirt",
                         "B_L_cb00", "Bn_extra"}) {
    Dynamics d;
    Check(d.Configure({Make({}),Make({0,0,-10})},{-1,0},{name,"tip"}),"eligible class skipped");
    Check(d.DrivenBones()==1,"leaf must stay rigid");
  }
  for (const char* name:{"Bn_tail", "hair_twist", "Bn_finger", "hair_ik", "Bn_m_headprop",
                         "BN_M_HEADPROA_hair", "Bn_m_headproB", "Bn_m_headlineA", "Bn_m_headlineB",
                         "hair_Hat", "Root", "pelvis_adjust", "bn_unclassified"}) {
    Dynamics d;
    Check(!d.Configure({Make({}),Make({0,0,-10})},{-1,0},{name,"tip"}),"excluded class simulated");
  }
  Check(better_pose::secondary::IsSleeve("Bn_l_xiu"),"sleeve rule");
  Check(!better_pose::secondary::IsSleeve("Bn_qunFxiuA_L"),"skirt weighted as sleeve");
  Check(better_pose::secondary::IsSwingOnly("Bn_kami"),"hair spelling rule");
  Check(better_pose::secondary::IsBreast("Bn_l_breast_001") &&
            better_pose::secondary::IsSecondary("Bn_l_breast_001"),
        "breast secondary bone was not classified");
  Check(better_pose::secondary::IsBreast("soft_bust_L") &&
            !better_pose::secondary::IsBreast("hair_bang_L"),
        "breast name classification was too narrow or too broad");
  using better_pose::secondary::MotionMaterial;
  Check(better_pose::secondary::ClassifyMotionMaterial("Bn_hairR") ==
            MotionMaterial::Hair,
        "hair material classification");
  Check(better_pose::secondary::ClassifyMotionMaterial("B_ribbon_00") ==
            MotionMaterial::Ribbon,
        "ribbon material classification");
  Check(better_pose::secondary::ClassifyMotionMaterial("belt_strap") ==
            MotionMaterial::Belt,
        "belt material classification");
  Check(better_pose::secondary::ClassifyMotionMaterial("cloth_acc_bag") ==
            MotionMaterial::Accessory,
        "accessory material classification");
  Check(better_pose::secondary::ClassifyMotionMaterial("Bn_l_xiu") ==
            MotionMaterial::Sleeve,
        "sleeve material classification");
  Check(better_pose::secondary::ClassifyMotionMaterial("Bn_qun01") ==
            MotionMaterial::Cloth,
        "cloth material classification");
  Check(better_pose::secondary::ClassifyMotionMaterial("B_L_cb00") ==
            MotionMaterial::Cloth,
        "cape accessory classification");
  auto expanded=better_pose::secondary::DynamicsTuning{};
  expanded.inertia=8.0F;
  expanded.air=8.0F;
  expanded.damping=8.0F;
  expanded.gravity=4.0F;
  expanded.collision=4.0F;
  expanded.material_inertia[better_pose::secondary::MotionMaterialIndex(
      MotionMaterial::Hair)]=8.0F;
  expanded.self_collision_strength[0]=8.0F;
  const auto safe_expanded=
      better_pose::secondary::SafeDynamicsTuning(expanded);
  Check(safe_expanded.inertia==8.0F && safe_expanded.air==8.0F &&
            safe_expanded.damping==8.0F && safe_expanded.gravity==4.0F &&
            safe_expanded.collision==4.0F &&
            safe_expanded.material_inertia[better_pose::secondary::
                MotionMaterialIndex(MotionMaterial::Hair)]==8.0F &&
            safe_expanded.self_collision_strength[0]==8.0F,
        "expanded dynamic tuning ranges were clamped to the old limits");
  const double chain_response =
      better_pose::secondary::NormalizedChainResponse(0.01, 10);
  Check(chain_response * 10 > 0.0099 && chain_response * 10 < 0.0102,
        "ribbon response accumulated beyond the requested chain amplitude");
}
std::vector<Bone> Run(int fps) {
  Rig rig; Dynamics d;
  Check(d.Configure(rig.bind,rig.parents,rig.names),"fork rig rejected");
  Check(d.DrivenBones()==7,"full hierarchy missed a branch/root or counted a leaf");
  std::array<double,3> movement{};
  for (int i=0;i<=fps*4;++i) {
    const double t=static_cast<double>(i)/fps;
    const auto pose=d.Sample(Motion(t),t,0,true);
    CheckPose(rig,pose);
    const std::array<std::size_t,3> tips{11,6,9};
    for (std::size_t j=0;j<tips.size();++j)
      movement[j]=(std::max)(movement[j],Length(Position(pose[tips[j]])-Position(rig.bind[tips[j]])));
    Check(d.Sample(Motion(t),t,0,true)==pose,"same sample integrated twice");
  }
  for (double value:movement) Check(value>.2,"one branch stayed rigid or was overwritten");
  Check(d.LengthError()<1e-7,"solver stretched bones");
  return d.Pose();
}
void Resets() {
  Rig rig; Dynamics d;
  d.Configure(rig.bind,rig.parents,rig.names);
  d.Sample(Motion(0),0,4,true); d.Sample(Motion(.05),.05,4,true);
  Check(d.Sample(Motion(.05),.05,4,false)==rig.bind,"pause did not restore bind");
  Check(d.Sample(Motion(.05),.05,4,true)==rig.bind,"resume same sample jumped");
  d.Sample(Motion(.06),.06,4,true);
  Check(d.Sample(Motion(.065),.065,5,true)==rig.bind,"short seek retained state");
  d.Sample(Motion(.07),.07,5,true);
  Check(d.Sample(Motion(0),0,5,true)==rig.bind,"loop retained state");
  d.Sample(Motion(.05),.05,5,true);
  Check(d.Sample(Motion(.4),.4,5,true)==rig.bind,"long gap retained state");
  Frame teleport=Motion(.41); teleport.position.x+=1000;
  Check(d.Sample(teleport,.41,5,true)==rig.bind,"teleport retained state");
  d.Reset(); Check(d.Pose()==rig.bind && !d.Moving(),"disable/unload reset");
  d.Sample({},0,6,true); d.Sample({},.01,6,true);
  Check(d.Sample({{},{0,0,1,0},1},.02,6,true)==rig.bind,"rotation jump retained state");
  Check(d.Sample({{},{},2},.03,6,true)==rig.bind,"scale change retained state");
  auto scaled=rig.bind; scaled[10][8]=0; scaled[10][9]=2; scaled[10][10]=3;
  Check(d.Configure(scaled,rig.parents,rig.names) && d.DrivenBones()==7,
        "unrelated scaled helper disabled all dynamics");
  for (int i=0;i<=60;++i) d.Sample(Motion(i/60.0),i/60.0,0,true);
  Check(d.Pose()[10][8]==0 && d.Pose()[10][9]==2 && d.Pose()[10][10]==3,
        "rigid helper scale changed");
  scaled=rig.bind; scaled[3][8]=2;
  Check(d.Configure(scaled,rig.parents,rig.names) && d.DrivenBones()==6,
        "scaled bone must stay rigid without disabling other strands");
  auto parents=rig.parents; parents[2]=3;
  Check(!d.Configure(rig.bind,parents,rig.names),"cyclic hierarchy accepted");
  Check(d.Configure({Make({}),Make({0,0,-5})},{-1,0},{"ribbon","tip"}),"character rebuild failed");
  Check(d.DrivenBones()==1 && d.Pose().size()==2,"old rig retained");
}
double Hang(const char* name) {
  Dynamics d; d.Configure({Make({}),Make({10,0,0})},{-1,0},{name,"tip"});
  for (int i=0;i<=600;++i) d.Sample({},i/120.0,0,true);
  const Vec tip=Position(d.Pose()[1]);
  Check(std::abs(Length(tip)-10)<1e-8,"gravity stretched segment");
  const auto settled=d.Pose();
  for (int i=601;i<=720;++i) d.Sample({},i/120.0,0,true);
  Check(Length(Position(settled[1])-Position(d.Pose()[1]))<.001,"spring did not settle");
  return std::atan2(-tip.z,tip.x)*180/3.14159265358979323846;
}
void GravityAndRoll() {
  // Analytic equilibrium: normalized .85*X + .15*(-Z), and .55*X + .45*(-Z).
  Check(std::abs(Hang("hair")-10.00798)<.01,"regular gravity share differs from body");
  Check(std::abs(Hang("Bn_l_xiu")-39.28941)<.01,"sleeve gravity share differs from body");
  Check(std::abs(Hang("B_ribbon_00")-18.43495)<.01,
        "draped ribbon gravity differs from the calibrated baseline");
  Check(std::abs(Hang("Bn_qunFxiuA_L")-10.00798)<.01,"skirt got sleeve gravity");
  const Quat roll{std::sin(.4),0,0,std::cos(.4)};
  Dynamics d; d.Configure({Make({},roll),Make({10,0,0},roll)},{-1,0},{"hair","tip"});
  Frame frame{{50,30,170},{0,std::sqrt(.5),0,std::sqrt(.5)},1.5};
  for (int i=0;i<=600;++i) d.Sample(frame,i/120.0,0,true);
  const Quat deviation=Multiply(Rotation(d.Pose()[0]),Inverse(roll));
  const Quat twist=Twist(deviation,{1,0,0});
  Check(std::abs(twist.x)<1e-8 && std::abs(twist.w-1)<1e-8,"hair gained axial twist");
  Check(std::abs(Length(Position(d.Pose()[1]))-10)<1e-8,"component scale changed local length");
  Dynamics side; side.Configure({Make({}),Make({0,0,-10})},{-1,0},{"hair","tip"});
  const double rest=frame.Point({0,0,-10}).z;
  for (int i=0;i<=600;++i) side.Sample(frame,i/120.0,0,true);
  Check(frame.Point(Position(side.Pose()[1])).z<rest-1,"gravity used component down");
}
void NativePhysicsOverrides() {
  better_pose::secondary::KawaiiPhysicsSettings native;
  native.damping = 0.18;
  native.stiffness = 0.10;
  native.radius = 1.0;
  native.limit_angle = 42.0;
  const auto mapped =
      better_pose::secondary::SpringFromKawaii(native, false);
  Check(std::abs(mapped.stiffness - 400.0) < 1e-9,
        "Kawaii stiffness did not map relative to the reference factor");
  Check(std::abs(mapped.damping - 16.0) < 1e-9,
        "Kawaii damping did not map relative to the reference factor");
  Check(std::abs(mapped.lag_degrees - 42.0) < 1e-9,
        "Kawaii angle limit was not preserved");
  const auto draped =
      better_pose::secondary::SpringFromKawaii(native, false, true);
  Check(std::abs(draped.stiffness - 320.0) < 1e-9 &&
            std::abs(draped.damping - 20.0) < 1e-9,
        "draped ribbon did not soften stiffness and increase damping");

  Rig rig;
  Dynamics d;
  Check(d.Configure(rig.bind, rig.parents, rig.names),
        "override fixture rig rejected");
  const std::array<BoneOverride, 2> overrides{{
      {"hairR00", {400.0, 16.0, 18.0}, 1.0},
      {"hairL00", {120.0, 12.0, 36.0}, 2.0},
  }};
  Check(d.ConfigureBoneOverrides(overrides) == 2,
        "per-bone native overrides did not match both strands");
  better_pose::secondary::SpringSettings effective;
  Check(d.EffectiveSpring("hairR00", effective) &&
            std::abs(effective.stiffness - 400.0) < 1e-9 &&
            std::abs(effective.lag_degrees - 18.0) < 1e-9,
        "right strand received the wrong native spring");
  Check(d.EffectiveSpring("hairL00", effective) &&
            std::abs(effective.stiffness - 120.0) < 1e-9 &&
            std::abs(effective.lag_degrees - 36.0) < 1e-9,
        "left strand received the wrong native spring");
  const std::array<BoneOverride, 1> unmatched{{{"missing", {}, 0.0}}};
  Check(d.ConfigureBoneOverrides(unmatched) == 0,
        "unknown native bone changed the spring set");

  Dynamics native_only;
  const std::vector<std::string> native_names{"Root", "B_doll00", "B_doll01"};
  const std::vector<std::int32_t> native_parents{-1, 0, 1};
  const std::vector<Bone> native_bind{
      Make({}), Make({0, 0, -8}), Make({0, 0, -16})};
  Check(!native_only.Configure(native_bind, native_parents, native_names),
        "unnamed native bone was simulated without native settings");
  const std::array<BoneOverride, 1> native_doll{{
      {"B_doll00", {400.0, 16.0, 42.0}, 1.0},
  }};
  Check(native_only.ConfigureBoneOverrides(native_doll) == 1 &&
            native_only.DrivenBones() == 1,
        "native Kawaii bone did not activate an otherwise unclassified chain");
  Dynamics sideways;
  static_cast<void>(
      sideways.Configure(native_bind, native_parents, native_names));
  const std::array<BoneOverride, 1> sideways_gravity{{
      {"B_doll00", {400.0, 16.0, 90.0}, 1.0, 1.0, {981.0, 0.0, 0.0}},
  }};
  Check(sideways.ConfigureBoneOverrides(sideways_gravity) == 1,
        "native gravity override did not apply");
  Vec effective_gravity;
  Check(sideways.EffectiveNativeGravity("B_doll00", effective_gravity) &&
            std::abs(effective_gravity.x - 981.0) < 1e-9,
        "native gravity override was not stored");
  for (int i{}; i <= 240; ++i)
    sideways.Sample({}, i / 120.0, 0, true);
  const double sideways_x = Position(sideways.Pose()[2]).x;
  std::cout << "native_gravity_tip_x=" << sideways_x
            << " driven=" << sideways.DrivenBones() << '\n';
  Check(sideways_x > 0.005,
        "native gravity override did not affect the bone chain");

}
double Depth(const std::vector<Bone>& pose,const Capsule& c) {
  double result=0;
  for (double t:{.5,.75,1.0}) {
    const Vec p=Position(pose[0])+(Position(pose[1])-Position(pose[0]))*t;
    const Vec closest=Closest(p,c.a,c.b);
    result=(std::max)(result,RadiusAt(c,closest)-Length(p-closest));
  }
  return result;
}
void Collisions() {
  const Capsule tapered{{0,0,0},{0,0,10},4,
                        better_pose::secondary::kCollideArms,8};
  Check(std::abs(RadiusAt(tapered,{0,0,5})-6)<1e-9,"tapered capsule radius");
  Check(std::abs(RadiusAt(tapered,{0,0,20})-8)<1e-9,"tapered capsule end radius");
  const std::vector<Bone> bind{Make({2,0,20}),Make({2,0,0}),Make({3,0,-2})};
  Dynamics d; d.Configure(bind,{-1,0,1},{"hair","tip","marker"});
  const std::array<Capsule,1> capsules{{{{0,0,0},{0,0,20},8,better_pose::secondary::kCollideTorso}}};
  const auto first=d.Sample({},0,0,true,capsules);
  Check(Depth(first,capsules[0])<Depth(bind,capsules[0]),"first-frame collision failed to reduce penetration");
  d.RecomputeCollisionDebug({},capsules);
  const auto& debug=d.CollisionDebug();
  Check(debug.capsules.size()==1 && !debug.contacts.empty() &&
            debug.maximum_depth>0.0 && debug.corrections>0,
        "collision debug snapshot missed capsules, contact, depth, or corrections");
  Check(std::isfinite(debug.contacts.front().strand.x) &&
            std::isfinite(debug.contacts.front().surface.z),
        "collision debug produced a nonfinite contact");
  d.PublishCollisionDebugCapsules(capsules);
  Check(d.CollisionDebug().capsules.size()==1 &&
            d.CollisionDebug().contacts.empty(),
        "capsule-only debug publication recomputed contacts");
  d.ClearCollisionDebug();
  Check(d.CollisionDebug().capsules.empty() &&
            d.CollisionDebug().contacts.empty() &&
            d.CollisionDebug().maximum_depth==0.0,
        "collision debug clear retained stale contacts");
  // The correction is capped at 5 deg per frame, so a deep penetration is resolved over
  // several frames instead of one, and once the strand is held it is left alone inside a
  // deadband of 1.5x the slack. It must still settle inside that deadband, it must not
  // sink while it is being corrected, and no frame may turn the bone past the cap.
  const double deadband=2.0*1.5;
  int settled=0;
  double previous=Depth(first,capsules[0]);
  double worst_step=0;
  double worst_head_step=0;
  Quat previous_rotation=Rotation(first[0]);
  auto previous_pose=first;
  for (int i=1;i<=300;++i) {
    const auto& pose=d.Sample({},i/120.0,0,true,capsules);
    const double depth=Depth(pose,capsules[0]);
    if (settled==0 && depth<=deadband) settled=i;
    if (settled!=0) Check(depth<=deadband,"spring pushed the strand back out of the deadband");
    else Check(depth<previous+1e-9,"collision let the strand sink deeper while correcting");
    previous=depth;
    worst_step=(std::max)(worst_step,RotationDegrees(Rotation(pose[0]),previous_rotation));
    previous_rotation=Rotation(pose[0]);
    for (std::size_t bone{};bone<pose.size();++bone)
      worst_head_step=(std::max)(
          worst_head_step,
          Length(Position(pose[bone])-Position(previous_pose[bone])));
    previous_pose=pose;
    Check(Length(Position(pose[0])-Position(bind[0]))<1e-8,"collision moved anchor");
    Check(std::abs(Length(Position(pose[1])-Position(pose[0]))-20)<1e-8,"collision stretched bone");
    Check(std::abs(Length(Position(pose[2])-Position(pose[1]))-std::sqrt(5.0))<1e-8,"collision left rigid child behind");
  }
  Check(settled>0 && settled<=60,"collision never brought the strand inside the deadband");
  // Two frames' worth of cap, allowing for the 5 deg landing on a slightly larger step.
  Check(worst_step<12.0,"single collision frame turned the bone past the cap");
  Check(worst_head_step<2.0,
        "per-node collision corrections accumulated into a chain jump");

  // Kawaii falls back to a capsule-perpendicular push even when the sampled point
  // lies on the axis. Skipping that contact lets the next frame emerge on the
  // opposite side and looks like a teleport.
  Dynamics centered;
  centered.Configure(
      {Make({.1,0,20}),Make({.1,0,0})},{-1,0},{"hair","tip"});
  centered.Sample({},0,0,true,capsules);
  Check(centered.Corrections()>0,
        "capsule-axis contact was skipped instead of using a stable fallback");

  // A moving body capsule may cross the strand. Once contact is established,
  // the strand must stay on that side until the contact actually releases.
  Dynamics crossing;
  crossing.Configure(
      {Make({0,0,20}),Make({0,0,0})},{-1,0},{"hair","tip"});
  std::array<Capsule,1> moving_capsule{{{}}};
  moving_capsule[0].a={10,0,0};
  moving_capsule[0].b={10,0,20};
  moving_capsule[0].radius=8;
  moving_capsule[0].mask=better_pose::secondary::kCollideTorso;
  bool crossing_contact=false;
  for (int i=0;i<=64;++i) {
    const double center_x=10.0-i*.3;
    moving_capsule[0].a.x=center_x;
    moving_capsule[0].b.x=center_x;
    const auto& pose=crossing.Sample({},i/120.0,1,true,moving_capsule);
    crossing_contact=crossing_contact || crossing.Corrections()>0;
    if (crossing_contact && center_x<=4.0 && center_x>=-4.0)
      Check(Position(pose[1]).x<=0.05,
            "moving capsule flipped the collision side across its axis");
  }

  const std::vector<std::string> names{"Bip001-Pelvis","Bip001-Spine","Bip001-Spine1","Bip001-Spine2",
      "Bip001-L-Thigh","Bip001-L-Calf","Bip001-L-Foot","Bip001-R-Thigh","Bip001-R-Calf","Bip001-R-Foot"};
  const std::vector<Bone> body{Make({0,0,100}),Make({0,0,115}),Make({0,0,130}),Make({0,0,145}),
      Make({-8,0,90}),Make({-8,0,50}),Make({-8,0,10}),Make({8,0,90}),Make({8,0,50}),Make({8,0,10})};
  Dynamics ribbon; ribbon.Configure({Make({0,12,0}),Make({0,12,-30})},{-1,0},{"B_ribbon_00","tip"});
  better_pose::secondary::DynamicsTuning extended;
  extended.extended_collision=true;
  ribbon.SetTuning(extended);
  ribbon.ConfigureCollisions(body,names,{{0,0,100},{},1},"Bip001-Pelvis");
  const auto volumes=ribbon.BodyCapsules(body.size(),[&](int i){return Position(body[i]);},{{40,0,0},{},2});
  Check(volumes.size()==4,"socket ancestry did not enable four leg volumes");
  Check(std::abs(volumes[0].radius-12)<1e-8,"leg radius not capped at configured maximum");
  Check(std::abs(volumes[0].a.z-153.6)<1e-8,"thigh capsule start fraction");
  Check(volumes[0].a.x==24 && volumes[0].radius==12,"body world transform not applied");
  auto moved=body; moved[4][4]-=25;
  const auto current=ribbon.BodyCapsules(moved.size(),[&](int i){return Position(moved[i]);},{});
  Check(std::abs(current[0].a.x+24.75)<1e-8,"capsule used stale bind position");
  ribbon.SetTuning({});
  ribbon.ConfigureCollisions(body,names,{{0,0,100},{},1},"Bip001-Pelvis");
  const auto compatibility_volumes =
      ribbon.BodyCapsules(body.size(),[&](int i){return Position(body[i]);},{{40,0,0},{},2});
  Check(std::abs(compatibility_volumes[0].radius-16)<1e-8,
        "compatibility leg radius did not restore the v2.3.0 half-hip cap");
  auto upper_names=names;
  upper_names.insert(upper_names.end(),{"Bip001-Neck","Bip001-Head",
      "Bip001-L-Clavicle","Bip001-L-UpperArm","Bip001-L-Forearm","Bip001-L-Hand",
      "Bip001-R-Clavicle","Bip001-R-UpperArm","Bip001-R-Forearm","Bip001-R-Hand"});
  auto upper_body=body;
  upper_body.insert(upper_body.end(),{
      Make({0,0,158}),Make({0,0,168}),
      Make({8,0,150}),Make({18,0,150}),Make({27,0,150}),Make({36,0,150}),
      Make({-8,0,150}),Make({-18,0,150}),Make({-27,0,150}),Make({-36,0,150})});
  Dynamics head; head.Configure(bind,{-1,0,1},{"hair","tip","marker"});
  head.SetTuning(extended);
  head.ConfigureCollisions(upper_body,upper_names,{},"Bip001-Head");
  const auto upper=head.BodyCapsules(
      upper_body.size(),[&](int i){return Position(upper_body[i]);},{});
  int torso_count=0,head_count=0,arm_count=0,tapered_count=0;
  bool chest_capsule=false;
  for (const auto& capsule:upper) {
    torso_count+=(capsule.mask & better_pose::secondary::kCollideTorso)!=0;
    head_count+=(capsule.mask & better_pose::secondary::kCollideHead)!=0;
    arm_count+=(capsule.mask & better_pose::secondary::kCollideArms)!=0;
    tapered_count+=capsule.end_radius>0;
    chest_capsule=chest_capsule ||
        ((capsule.mask & better_pose::secondary::kCollideTorso)!=0 &&
         std::abs(capsule.a.z-145.0)<1e-8 &&
         std::abs(capsule.b.z-158.0)<1e-8 &&
         std::abs(capsule.radius-
                  better_pose::secondary::kChestCollisionRadiusCm)<1e-8);
  }
  Check(torso_count==4 && chest_capsule && head_count==2 && arm_count==6,
        "hair did not receive torso, head and arm collision groups");
  Check(tapered_count==1,"neck-to-head capsule did not preserve its end radius");
  Check(head.BodyCapsuleCount(better_pose::secondary::kCollideHead)==2 &&
            head.BodyCapsuleCount(better_pose::secondary::kCollideArms)==6 &&
            head.BodyCapsuleCount(better_pose::secondary::kCollideTorso)==4,
        "collision group diagnostics lost generated capsules");
  std::vector<std::array<double, 3>> upper_points;
  for (const auto& bone : upper_body)
    upper_points.push_back({Position(bone).x, Position(bone).y, Position(bone).z});
  const auto fallback=BuildBodyCollisionCapsules(
      upper_names,upper_points,
      better_pose::secondary::kCollideTorso |
          better_pose::secondary::kCollideHead |
          better_pose::secondary::kCollideArms |
          better_pose::secondary::kCollideLegs,
      false);
  int fallback_torso=0,fallback_head=0,fallback_arms=0,fallback_legs=0;
  bool fallback_chest=false;
  for (const auto& capsule:fallback) {
    fallback_torso+=(capsule.mask & better_pose::secondary::kCollideTorso)!=0;
    fallback_head+=(capsule.mask & better_pose::secondary::kCollideHead)!=0;
    fallback_arms+=(capsule.mask & better_pose::secondary::kCollideArms)!=0;
    fallback_legs+=(capsule.mask & better_pose::secondary::kCollideLegs)!=0;
    fallback_chest=fallback_chest ||
        (capsule.mask==better_pose::secondary::kCollideTorso &&
         std::abs(capsule.a.z-145.0)<1e-8 &&
         std::abs(capsule.b.z-158.0)<1e-8 &&
         std::abs(capsule.radius-
                  better_pose::secondary::kChestCollisionRadiusCm)<1e-8);
  }
  Check(fallback_torso==4 && fallback_chest && fallback_head==2 &&
            fallback_arms==6 && fallback_legs==4,
        "fallback display capsules did not include the default chest volume");
  head.SetTuning({});
  head.ConfigureCollisions(upper_body,upper_names,{},"Bip001-Head");
  Check(head.MaskedBones(better_pose::secondary::kCollideHead)==0 &&
            head.MaskedBones(better_pose::secondary::kCollideArms)==0 &&
            head.BodyCapsuleCount(better_pose::secondary::kCollideTorso)==4 &&
            head.BodyCapsuleCount(better_pose::secondary::kCollideHead)==0 &&
            head.BodyCapsuleCount(better_pose::secondary::kCollideArms)==0,
        "standard collision did not retain torso, chest and leg ranges");
  ribbon.ConfigureCollisions({}, {}, {}, "Bip001-Pelvis");
  Check(ribbon.BodyCapsules(body.size(),[&](int i){return Position(body[i]);},{}).empty(),"missing body retained old capsules");
}
void Independence() {
  Rig rig; Dynamics a,b; auto changed=rig.bind;
  changed[8][6]-=8; changed[9][6]-=16;
  a.Configure(rig.bind,rig.parents,rig.names); b.Configure(changed,rig.parents,rig.names);
  Dynamics other; other.Configure({Make({}),Make({20,0,0})},{-1,0},{"Bn_l_xiu","tip"});
  for (int i=0;i<=360;++i) {
    const double t=i/120.0;
    const auto& pa=a.Sample(Motion(t),t,0,true);
    other.Sample(Motion(t*2),t,0,true);
    const auto& pb=b.Sample(Motion(t),t,0,true);
    for (auto bone:{0,1,2,3,4,5,6,10,11}) Check(pa[bone]==pb[bone],"state leaked across separate branches/accessories");
  }
}
void HairSway() {
  // Identical planar motion removes twist differences. Cloth retains the
  // previous spring response, so it is a reference for the requested increase.
  const std::vector<Bone> bind{Make({}),Make({0,0,-25})};
  Dynamics cloth;
  cloth.Configure(bind,{-1,0},{"cloth","tip"});
  std::array<Dynamics,4> hair;
  const std::array<const char*,4> names{"Bone_hairR00","Bone_hairL00","Bn_hari","Bn_kami"};
  for (std::size_t i=0;i<hair.size();++i) hair[i].Configure(bind,{-1,0},{names[i],"tip"});
  double reference=0,amplitude=0;
  for (int i=0;i<=720;++i) {
    const double t=i/120.0,angle=.35*std::sin(t*4);
    const Frame frame{{0,0,150},{0,std::sin(angle/2),0,std::cos(angle/2)},1};
    const auto& before=cloth.Sample(frame,t,0,true);
    for (auto& strand:hair) strand.Sample(frame,t,0,true);
    for (std::size_t j=1;j<hair.size();++j)
      Check(hair[0].Pose()==hair[j].Pose(),"hair categories or sides use different sway");
    if (t>=2) {
      reference=(std::max)(reference,std::abs(Position(before[1]).x));
      amplitude=(std::max)(amplitude,std::abs(Position(hair[0].Pose()[1]).x));
    }
  }
  std::cout << "hair_sway_ratio=" << amplitude/reference << '\n';
  Check(amplitude>reference*1.15,"hair sway did not noticeably increase");
  Check(amplitude<reference*2,"hair sway increase is excessive");
}
double MaximumLocalOffset(const std::vector<Bone>& bind,const Bone& tip) {
  return Length(Position(tip)-Position(bind[1]));
}
void BodyMotionAndAir() {
  const std::vector<Bone> bind{Make({}),Make({0,0,-20})};
  const std::vector<std::int32_t> parents{-1,0};
  const std::vector<std::string> names{"Bn_hairR00","tip"};

  Dynamics body; Dynamics fixed;
  Check(body.Configure(bind,parents,names),"body-motion rig rejected");
  Check(fixed.Configure(bind,parents,names),"fixed-motion rig rejected");
  const std::array<BoneOverride,1> fixed_bone{{
      {"Bn_hairR00",{120.0,12.0,35.0},0.0,0.0},
  }};
  Check(fixed.ConfigureBoneOverrides(fixed_bone)==1,
        "motion-follow override did not apply");
  double body_sway=0,fixed_sway=0,turn_sway=0;
  for (int i=0;i<=720;++i) {
    const double t=i/120.0;
    const double angle=.35*std::sin(t*3.0);
    const Frame frame{
        {18.0*std::sin(t*3.5),0,150},
        {0,0,std::sin(angle/2),std::cos(angle/2)},1};
    const Bone& body_tip=body.Sample(frame,t,0,true)[1];
    const Bone& fixed_tip=fixed.Sample(frame,t,0,true)[1];
    body_sway=(std::max)(body_sway,MaximumLocalOffset(bind,body_tip));
    fixed_sway=(std::max)(fixed_sway,MaximumLocalOffset(bind,fixed_tip));
    turn_sway=(std::max)(
        turn_sway,std::abs(Position(body_tip).x-Position(bind[1]).x));
  }
  std::cout << "body_sway=" << body_sway << " fixed_sway=" << fixed_sway
            << " turn_sway=" << turn_sway << '\n';
  Check(body_sway>fixed_sway+.2,
        "body translation and turning did not add inertial motion");
  Check(turn_sway>.05,"turning angle did not affect the accessory");

  Dynamics still; Dynamics air;
  still.Configure(bind,parents,names); air.Configure(bind,parents,names);
  auto still_tuning=better_pose::secondary::DynamicsTuning{};
  still_tuning.air=0.0F;
  still.SetTuning(still_tuning);
  auto moving_air_tuning=better_pose::secondary::DynamicsTuning{};
  moving_air_tuning.air=1.0F;
  air.SetTuning(moving_air_tuning);
  double still_side=0,air_side=0;
  for (int i=0;i<=480;++i) {
    const double t=i/120.0;
    const Frame frame{{100.0*t,0,150},{},1};
    still_side=(std::max)(
        still_side,
        std::abs(Position(still.Sample(frame,t,0,true)[1]).x));
    air_side=(std::max)(
        air_side,
        std::abs(Position(air.Sample(frame,t,0,true)[1]).x));
  }
  std::cout << "moving_air_side=" << air_side
            << " disabled_air_side=" << still_side << '\n';
  Check(air_side>still_side+.15,
        "moving air did not tilt the accessory against motion");

  Dynamics forward; Dynamics forward_native;
  forward.Configure(bind,parents,names);
  forward_native.Configure(bind,parents,names);
  auto no_air=better_pose::secondary::DynamicsTuning{};
  no_air.air=0.0F;
  forward.SetTuning(no_air);
  forward_native.SetTuning(no_air);
  const std::array<BoneOverride,1> forward_offset{{
      {"Bn_hairR00",{120.0,12.0,35.0},0.0,1.0,{},0.35},
  }};
  Check(forward_native.ConfigureBoneOverrides(forward_offset)==1,
        "forward-move override did not apply");
  double plain=0,advanced=0;
  for (int i=0;i<=480;++i) {
    const double t=i/120.0;
    const Frame frame{{100.0*t,0,150},{},1};
    plain=(std::max)(
        plain,
        std::abs(Position(forward.Sample(frame,t,0,true)[1]).x));
    advanced=(std::max)(
        advanced,
        std::abs(Position(forward_native.Sample(frame,t,0,true)[1]).x));
  }
  Check(advanced>plain+.15,
        "native forwardMoveOffset did not affect motion");
}
void MaterialResponse() {
  const std::vector<Bone> bind{Make({}),Make({0,0,-20})};
  const std::vector<std::int32_t> parents{-1,0};
  Dynamics hair,accessory;
  Check(hair.Configure(bind,parents,{"Bn_hairR00","tip"}),
        "hair response rig rejected");
  Check(accessory.Configure(bind,parents,{"cloth_acc_bag","tip"}),
        "accessory response rig rejected");
  std::array<BoneOverride,1> hair_settings{{
      {"Bn_hairR00",{120.0,12.0,35.0},0.0,1.0},
  }};
  std::array<BoneOverride,1> accessory_settings{{
      {"cloth_acc_bag",{120.0,12.0,35.0},0.0,1.0},
  }};
  Check(hair.ConfigureBoneOverrides(hair_settings)==1 &&
            accessory.ConfigureBoneOverrides(accessory_settings)==1,
        "material comparison overrides did not apply");
  double hair_motion=0,accessory_motion=0;
  for (int i=0;i<=720;++i) {
    const double t=i/120.0;
    const Frame frame{{25.0*std::sin(t*4.0),0,150},{},1};
    hair_motion=(std::max)(
        hair_motion,
        MaximumLocalOffset(bind,hair.Sample(frame,t,0,true)[1]));
    accessory_motion=(std::max)(
        accessory_motion,
        MaximumLocalOffset(bind,accessory.Sample(frame,t,0,true)[1]));
  }
  Check(hair_motion>accessory_motion+.15,
        "material groups did not separate hair and accessory response");
}
void MultiRootAttachments() {
  const std::vector<Bone> bind{
      Make({0,0,0}),Make({0,0,-10}),Make({0,0,-20}),
      Make({10,0,0}),Make({10,0,-10}),Make({10,0,-20})};
  const std::vector<std::int32_t> parents{-1,0,1,-1,3,4};
  const std::vector<std::string> names{
      "RootA","clothA0","tipA","RootB","clothB0","tipB"};
  Dynamics d;
  Check(d.Configure(bind,parents,names),"multi-root rig rejected");
  Check(d.DrivenBones()==2,"multi-root rig did not expose both strands");
  const std::array<AttachmentDrive,2> attachments{{
      {0,Make({12,0,0}),1.0},
      {3,Make({-8,0,4}),1.0},
  }};
  d.Sample({},0,0,true,{},attachments);
  const auto pose=d.Sample({},1.0/120.0,0,true,{},attachments);
  Check(Length(Position(pose[0])-Vec{12,0,0})<1e-8,
        "first attachment did not follow its own animated root");
  Check(Length(Position(pose[3])-Vec{-8,0,4})<1e-8,
        "second attachment did not follow its own animated root");
  Check(std::abs(Length(Position(pose[1])-Position(pose[0]))-10)<1e-7 &&
            std::abs(Length(Position(pose[2])-Position(pose[1]))-10)<1e-7,
        "first attached chain was stretched");
  Check(std::abs(Length(Position(pose[4])-Position(pose[3]))-10)<1e-7 &&
            std::abs(Length(Position(pose[5])-Position(pose[4]))-10)<1e-7,
        "second attached chain was stretched");
  Check(d.LengthError()<1e-7,"multi-root solver changed authored lengths");
  const auto& released=d.Sample({},2.0/120.0,0,true);
  Check(Length(Position(released[0])-Position(bind[0]))<1e-8 &&
            Length(Position(released[3])-Position(bind[3]))<1e-8,
        "removed attachment drives retained their old anchor offsets");

  Dynamics weighted;
  weighted.Configure(bind,parents,names);
  const std::array<AttachmentDrive,1> half{{
      {0,Make({20,0,0}),0.5},
  }};
  weighted.Sample({},0,0,true,{},half);
  const auto mixed=weighted.Sample({},1.0/120.0,0,true,{},half);
  Check(std::abs(Position(mixed[0]).x-10)<1e-8,
        "attachment weight did not blend toward the animated root");
}
void TuningControls() {
  const std::vector<Bone> vertical{Make({}),Make({0,0,-20})};
  const std::vector<std::int32_t> parents{-1,0};
  const std::vector<std::string> names{"Bn_hairR00","tip"};
  Dynamics normal, inertia_off;
  normal.Configure(vertical,parents,names);
  inertia_off.Configure(vertical,parents,names);
  auto tuning=better_pose::secondary::DynamicsTuning{};
  tuning.inertia=0.0F;
  inertia_off.SetTuning(tuning);
  double normal_motion=0,inertia_off_motion=0;
  for(int i=0;i<=480;++i){
    const double t=i/120.0;
    const Frame frame{{20*std::sin(t*3),0,150},{},1};
    normal_motion=(std::max)(
        normal_motion,
        MaximumLocalOffset(vertical,normal.Sample(frame,t,0,true)[1]));
    inertia_off_motion=(std::max)(
        inertia_off_motion,
        MaximumLocalOffset(vertical,inertia_off.Sample(frame,t,0,true)[1]));
  }
  Check(normal_motion>inertia_off_motion+.2,
        "inertia tuning did not attenuate body-driven sway");

  Dynamics breast_full, breast_off;
  const std::vector<std::string> breast_names{
      "Bn_l_breast_001","Bip001-L-Hair-Tip"};
  Check(breast_full.Configure(vertical,parents,breast_names) &&
            breast_off.Configure(vertical,parents,breast_names),
        "breast secondary rig was rejected");
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.breast_inertia=0.0F;
  breast_off.SetTuning(tuning);
  double breast_motion=0,breast_off_motion=0;
  for(int i=0;i<=480;++i){
    const double t=i/120.0;
    const Frame frame{{20*std::sin(t*3),0,150},{},1};
    breast_motion=(std::max)(
        breast_motion,
        MaximumLocalOffset(vertical,breast_full.Sample(frame,t,0,true)[1]));
    breast_off_motion=(std::max)(
        breast_off_motion,
        MaximumLocalOffset(vertical,breast_off.Sample(frame,t,0,true)[1]));
  }
  Check(breast_motion>breast_off_motion+.2,
        "breast inertia override did not isolate chest sway");

  Dynamics breast_amplitude_full, breast_amplitude_off;
  Check(breast_amplitude_full.Configure(vertical,parents,breast_names) &&
            breast_amplitude_off.Configure(vertical,parents,breast_names),
        "breast amplitude rig was rejected");
  tuning=better_pose::secondary::DynamicsTuning{};
  breast_amplitude_full.SetTuning(tuning);
  tuning.breast_amplitude=0.0F;
  breast_amplitude_off.SetTuning(tuning);
  double breast_amplitude_motion=0,breast_amplitude_off_motion=0;
  for(int i=0;i<=480;++i){
    const double t=i/120.0;
    const Frame frame{{20*std::sin(t*3),0,150},{},1};
    breast_amplitude_motion=(std::max)(
        breast_amplitude_motion,
        MaximumLocalOffset(
            vertical,breast_amplitude_full.Sample(frame,t,0,true)[1]));
    breast_amplitude_off_motion=(std::max)(
        breast_amplitude_off_motion,
        MaximumLocalOffset(
            vertical,breast_amplitude_off.Sample(frame,t,0,true)[1]));
  }
  Check(breast_amplitude_motion>breast_amplitude_off_motion+.2,
        "breast amplitude override did not change visible deflection");

  Dynamics ribbon_frozen;
  const std::vector<Bone> ribbon_bind{Make({}),Make({0,0,-20})};
  Check(ribbon_frozen.Configure(
            ribbon_bind,parents,
            {"Bn_l_piaodai_001","Bip001-L-Hair-Tip"}),
        "zero-inertia ribbon rig rejected");
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.material_inertia[
      better_pose::secondary::MotionMaterialIndex(
          better_pose::secondary::MotionMaterial::Ribbon)]=0.0F;
  ribbon_frozen.SetTuning(tuning);
  double ribbon_motion=0;
  for(int i=0;i<=480;++i){
    const double t=i/120.0;
    const Frame frame{{20*std::sin(t*3),0,150},{},1};
    ribbon_motion=(std::max)(
        ribbon_motion,
        MaximumLocalOffset(
            ribbon_bind,ribbon_frozen.Sample(frame,t,0,true)[1]));
  }
  Check(ribbon_motion<1e-7,
        "zero ribbon inertia still ran gravity or spring motion");

  Dynamics ribbon_low, ribbon_full;
  ribbon_low.Configure(ribbon_bind,parents,
                       {"Bn_l_piaodai_001","Bip001-L-Hair-Tip"});
  ribbon_full.Configure(ribbon_bind,parents,
                        {"Bn_l_piaodai_001","Bip001-L-Hair-Tip"});
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.material_inertia[
      better_pose::secondary::MotionMaterialIndex(
          better_pose::secondary::MotionMaterial::Ribbon)]=0.05F;
  ribbon_low.SetTuning(tuning);
  double low_ribbon_motion=0,full_ribbon_motion=0;
  for(int i=0;i<=480;++i){
    const double t=i/120.0;
    const Frame frame{{20*std::sin(t*3),0,150},{},1};
    low_ribbon_motion=(std::max)(
        low_ribbon_motion,
        MaximumLocalOffset(
            ribbon_bind,ribbon_low.Sample(frame,t,0,true)[1]));
    full_ribbon_motion=(std::max)(
        full_ribbon_motion,
        MaximumLocalOffset(
            ribbon_bind,ribbon_full.Sample(frame,t,0,true)[1]));
  }
  Check(low_ribbon_motion+.1<full_ribbon_motion,
        "low ribbon inertia did not reduce swing amplitude");

  Dynamics air_on,air_off;
  air_on.Configure(vertical,parents,names);
  air_off.Configure(vertical,parents,names);
  air_on.ConfigureAir(true,1.0);
  air_off.ConfigureAir(true,1.0);
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.air=1.0F;
  air_on.SetTuning(tuning);
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.air=0.0F;
  air_off.SetTuning(tuning);
  double air_side=0,air_off_side=0;
  for(int i=0;i<=480;++i){
    const double t=i/120.0;
    const Frame frame{{100*t,0,150},{},1};
    air_side=(std::max)(
        air_side,std::abs(Position(air_on.Sample(frame,t,0,true)[1]).x));
    air_off_side=(std::max)(
        air_off_side,std::abs(Position(air_off.Sample(frame,t,0,true)[1]).x));
  }
  Check(air_side>air_off_side+.15,
        "air tuning did not attenuate moving-air response");

  Dynamics gravity_on,gravity_off;
  const std::vector<Bone> horizontal{Make({}),Make({10,0,0})};
  gravity_on.Configure(horizontal,parents,{"B_ribbon_00","tip"});
  gravity_off.Configure(horizontal,parents,{"B_ribbon_00","tip"});
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.gravity=0.0F;
  gravity_off.SetTuning(tuning);
  for(int i=0;i<=600;++i){
    gravity_on.Sample({},i/120.0,0,true);
    gravity_off.Sample({},i/120.0,0,true);
  }
  const double gravity_on_angle=
      std::atan2(-Position(gravity_on.Pose()[1]).z,
                 Position(gravity_on.Pose()[1]).x);
  const double gravity_off_angle=
      std::atan2(-Position(gravity_off.Pose()[1]).z,
                 Position(gravity_off.Pose()[1]).x);
  Check(std::abs(gravity_off_angle)<1e-5 &&
            gravity_on_angle>.2,
        "gravity tuning did not disable gravity weighting");

  Dynamics collision;
  collision.Configure({Make({2,0,20}),Make({2,0,0}),Make({3,0,-2})},
                      {-1,0,1},{"hair","tip","marker"});
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.collision=0.0F;
  collision.SetTuning(tuning);
  const std::array<Capsule,1> capsule{{
      {{0,0,0},{0,0,20},8,better_pose::secondary::kCollideTorso}}};
  collision.Sample({},0,0,true,capsule);
  for(int i=1;i<=30;++i) collision.Sample({},i/120.0,0,true,capsule);
  Check(collision.Corrections()==0,
        "collision tuning did not disable corrections");
  Dynamics ribbon_stable;
  ribbon_stable.Configure(
      {Make({2,0,20}),Make({2,0,0}),Make({3,0,-2})},{-1,0,1},
      {"B_ribbon_00","tip","marker"});
  ribbon_stable.EnableConstraintMask(
      better_pose::secondary::kCollideTorso);
  ribbon_stable.Sample({},0,0,true,capsule);
  for(int i=1;i<=30;++i)
    ribbon_stable.Sample({},i/120.0,0,true,capsule);
  Check(ribbon_stable.Corrections()==0,
        "default ribbon collision tuning did not disable jittering corrections");
  Dynamics ribbon_active;
  ribbon_active.Configure(
      {Make({2,0,20}),Make({2,0,0}),Make({3,0,-2})},{-1,0,1},
      {"B_ribbon_00","tip","marker"});
  ribbon_active.EnableConstraintMask(
      better_pose::secondary::kCollideTorso);
  tuning=better_pose::secondary::DynamicsTuning{};
  tuning.material_collision[
      better_pose::secondary::MotionMaterialIndex(
          better_pose::secondary::MotionMaterial::Ribbon)]=1.0F;
  ribbon_active.SetTuning(tuning);
  ribbon_active.Sample({},0,0,true,capsule);
  Check(ribbon_active.Corrections()>0,
        "per-material ribbon collision tuning did not re-enable corrections");
  Check(normal.MaterialBoneCount(
            better_pose::secondary::MotionMaterial::Hair)==1,
        "material diagnostics did not count the simulated hair bone");
}

void DistanceConstraints() {
  const auto hair_material = static_cast<std::uint32_t>(
      better_pose::secondary::MotionMaterial::Hair);
  const auto ribbon_material = static_cast<std::uint32_t>(
      better_pose::secondary::MotionMaterial::Ribbon);
  const auto hair_mask = better_pose::secondary::SelfCollisionMask(
      better_pose::secondary::MotionMaterial::Hair);
  const auto ribbon_mask = better_pose::secondary::SelfCollisionMask(
      better_pose::secondary::MotionMaterial::Ribbon);
  const std::array<better_pose::bone_constraints::Candidate, 2>
      cross_material{{
          {{0, 0, 0}, {0, 0, 10}, 10, 4, 10, 1,
            hair_material, hair_mask, 0, 8, true},
          {{2, 0, 0}, {2, 0, 10}, 10, 5, 11, 0,
            ribbon_material, ribbon_mask, 0, 3, true},
      }};
  const auto cross_links =
      better_pose::bone_constraints::DiscoverSelfCollision(
          cross_material);
  Check(cross_links.size() == 1 && !cross_links.front().shared_anchor &&
            std::abs(cross_links.front().correction_share_a - 0.65) < 1e-9 &&
            std::abs(cross_links.front().correction_share_b - 0.35) < 1e-9,
        "hair and ribbon across anchors were not paired for self-collision");

  std::array<better_pose::bone_constraints::Candidate, 2> candidates{{
      {{0, 0, 0}, {0, 0, 10}, 10, 4, 10, 1, 1, 2, 0, 0, true},
      {{10, 0, 0}, {10, 0, 10}, 10, 4, 11, 1, 1, 2, 0, 0, true},
  }};
  const auto links =
      better_pose::bone_constraints::Discover(candidates);
  Check(links.size() == 1 && std::abs(links.front().rest - 10) < 1e-8,
        "adjacent strand discovery did not preserve rest spacing");

  std::array<better_pose::bone_constraints::Vec, 2> heads{{
      {0, 0, 0}, {10, 0, 0}}};
  std::array<better_pose::bone_constraints::Vec, 2> directions{{
      {0, 0, 1}, {1, 0, 0}}};
  const auto head_at = [&](const std::uint32_t index) {
    return heads[index];
  };
  const auto tip_at = [&](const std::uint32_t index) {
    return heads[index] + directions[index] * 10;
  };
  const auto apply = [&](const std::uint32_t index,
                         const better_pose::bone_constraints::Vec direction) {
    directions[index] = direction;
  };
  const auto before =
      better_pose::bone_constraints::Length(tip_at(1) - tip_at(0));
  better_pose::bone_constraints::Relax(links, 2, head_at, tip_at, apply, [] {});
  const auto after =
      better_pose::bone_constraints::Length(tip_at(1) - tip_at(0));
  Check(after + 0.25 < before,
        "distance constraints did not pull an obviously stretched link back");

  std::array<better_pose::bone_constraints::Vec, 2> self_heads{{
      {0, 0, 0}, {1, 0, 0}}};
  std::array<better_pose::bone_constraints::Vec, 2> self_directions{{
      {0, 0, 1}, {-1, 0, 0}}};
  std::array<std::uint8_t, 1> contact_state{};
  const std::array<better_pose::bone_constraints::Link, 1> self_links{{
      {0, 1, 10, 10, 0, 1, false},
  }};
  const auto self_head_at = [&](const std::uint32_t index) {
    return self_heads[index];
  };
  const auto self_tip_at = [&](const std::uint32_t index) {
    return self_heads[index] + self_directions[index] * 10;
  };
  const auto self_apply =
      [&](const std::uint32_t index,
          const better_pose::bone_constraints::Vec direction,
          const double) {
        self_directions[index] = direction;
      };
  const double self_before =
      better_pose::bone_constraints::ClosestSegmentPoints(
          self_head_at(0), self_tip_at(0), self_head_at(1),
          self_tip_at(1))
          .distance;
  better_pose::bone_constraints::RelaxSelfCollision(
      self_links, contact_state, 2, self_head_at, self_tip_at,
      self_apply, [] {});
  const double self_after =
      better_pose::bone_constraints::ClosestSegmentPoints(
          self_head_at(0), self_tip_at(0), self_head_at(1),
          self_tip_at(1))
          .distance;
  Check(contact_state[0] != 0 && self_after > self_before + 0.05,
        "self-collision did not separate intersecting neighboring segments");

  const auto run_speed_response = [&](const double relative_speed) {
    std::array<better_pose::bone_constraints::Vec, 2> heads{{
        {0, 0, 0}, {1, 0, 0}}};
    std::array<better_pose::bone_constraints::Vec, 2> directions{{
        {0, 0, 1}, {-1, 0, 0}}};
    std::array<std::uint8_t, 1> state{};
    const auto head = [&](const std::uint32_t index) {
      return heads[index];
    };
    const auto tip = [&](const std::uint32_t index) {
      return heads[index] + directions[index] * 10;
    };
    double kept_velocity{};
    const auto apply =
        [&](const std::uint32_t index,
            const better_pose::bone_constraints::Vec direction,
            const double velocity_keep) {
          directions[index] = direction;
          kept_velocity = velocity_keep;
        };
    better_pose::bone_constraints::RelaxSelfCollision(
        self_links, state, 2, head, tip, apply, [] {},
        [&](const better_pose::bone_constraints::Link &) {
          return relative_speed;
        });
    return std::pair{
        better_pose::bone_constraints::ClosestSegmentPoints(
            head(0), tip(0), head(1), tip(1))
            .distance,
        kept_velocity};
  };
  const auto slow_contact = run_speed_response(0.0);
  const auto fast_contact = run_speed_response(200.0);
  Check(fast_contact.first > slow_contact.first + 0.01,
        "fast self-contact did not separate more than slow contact");
  Check(fast_contact.second + 0.05 < slow_contact.second,
        "fast self-contact retained too much rebound velocity");
}
}
int main() {
  Classifications();
  const auto baseline=Run(120);
  for (int fps:{30,60,144}) {
    const auto pose=Run(fps); double worst=0;
    for (auto tip:{6,9,11}) worst=(std::max)(worst,Length(Position(pose[tip])-Position(baseline[tip])));
    std::cout << "fps=" << fps << " maximum_tip_difference_cm=" << worst << '\n';
    Check(worst<.75,"excessive frame-rate dependence");
  }
  Resets(); GravityAndRoll(); NativePhysicsOverrides(); Collisions();
  Independence(); HairSway(); BodyMotionAndAir(); MaterialResponse();
  MultiRootAttachments(); TuningControls();
  DistanceConstraints();
  std::cout << "PASS classification, branches, rigid descendants, lengths, resets, "
               "gravity, roll, native springs, collisions, body inertia, air, "
               "materials, tuning, multi-root attachments, distance constraints, "
               "independence\n";
}
