#include "../accessory_dynamics.hpp"
#include <cstdlib>
#include <iostream>

using namespace better_pose::accessory;
namespace {
void Check(bool value,const char* message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
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
                         "piao", "gongpai", "lalian", "xiong", "tie", "skirt", "Bn_extra"}) {
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
double Depth(const std::vector<Bone>& pose,const Capsule& c) {
  double result=0;
  for (double t:{.5,.75,1.0}) {
    const Vec p=Position(pose[0])+(Position(pose[1])-Position(pose[0]))*t;
    result=(std::max)(result,c.radius-Length(p-Closest(p,c.a,c.b)));
  }
  return result;
}
void Collisions() {
  const std::vector<Bone> bind{Make({2,0,20}),Make({2,0,0}),Make({3,0,-2})};
  Dynamics d; d.Configure(bind,{-1,0,1},{"hair","tip","marker"});
  const std::array<Capsule,1> capsules{{{{0,0,0},{0,0,20},8,better_pose::secondary::kCollideTorso}}};
  const auto first=d.Sample({},0,0,true,capsules);
  Check(Depth(first,capsules[0])<Depth(bind,capsules[0])-2,"first-frame collision failed to reduce penetration");
  for (int i=1;i<=300;++i) {
    const auto& pose=d.Sample({},i/120.0,0,true,capsules);
    Check(Depth(pose,capsules[0])<2,"spring fought collision");
    Check(Length(Position(pose[0])-Position(bind[0]))<1e-8,"collision moved anchor");
    Check(std::abs(Length(Position(pose[1])-Position(pose[0]))-20)<1e-8,"collision stretched bone");
    Check(std::abs(Length(Position(pose[2])-Position(pose[1]))-std::sqrt(5.0))<1e-8,"collision left rigid child behind");
  }
  const std::vector<std::string> names{"Bip001-Pelvis","Bip001-Spine","Bip001-Spine1","Bip001-Spine2",
      "Bip001-L-Thigh","Bip001-L-Calf","Bip001-L-Foot","Bip001-R-Thigh","Bip001-R-Calf","Bip001-R-Foot"};
  const std::vector<Bone> body{Make({0,0,100}),Make({0,0,115}),Make({0,0,130}),Make({0,0,145}),
      Make({-8,0,90}),Make({-8,0,50}),Make({-8,0,10}),Make({8,0,90}),Make({8,0,50}),Make({8,0,10})};
  Dynamics ribbon; ribbon.Configure({Make({0,12,0}),Make({0,12,-30})},{-1,0},{"B_ribbon_00","tip"});
  ribbon.ConfigureCollisions(body,names,{{0,0,100},{},1},"Bip001-Pelvis");
  const auto volumes=ribbon.BodyCapsules(body.size(),[&](int i){return Position(body[i]);},{{40,0,0},{},2});
  Check(volumes.size()==7,"socket ancestry did not enable four leg volumes");
  Check(std::abs(volumes[3].radius-16)<1e-8,"leg radius not capped at half hip separation");
  Check(std::abs(volumes[3].a.z-153.6)<1e-8,"thigh capsule start fraction");
  Check(volumes[0].a.x==40 && volumes[0].radius==16,"body world transform not applied");
  auto moved=body; moved[4][4]-=25;
  const auto current=ribbon.BodyCapsules(moved.size(),[&](int i){return Position(moved[i]);},{});
  Check(std::abs(current[3].a.x+24.75)<1e-8,"capsule used stale bind position");
  Dynamics head; head.Configure(bind,{-1,0,1},{"hair","tip","marker"});
  head.ConfigureCollisions(body,names,{},"Bip001-Head");
  Check(head.BodyCapsules(body.size(),[&](int i){return Position(body[i]);},{}).size()==3,"head accessory generated leg volumes");
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
  Resets(); GravityAndRoll(); Collisions(); Independence(); HairSway();
  std::cout << "PASS classification, branches, rigid descendants, lengths, resets, gravity, roll, collisions, independence\n";
}
