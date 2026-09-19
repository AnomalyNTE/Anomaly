#pragma once

#include "secondary_rules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

// CPU-only accessory dynamics, in centimetres and seconds. Scene placement and
// UObject lifetime remain the plugin's responsibility.
namespace better_pose::accessory {
using Bone = std::array<double, 12>;
struct Vec {
  double x{}, y{}, z{};
  Vec operator+(Vec b) const { return {x+b.x,y+b.y,z+b.z}; }
  Vec operator-(Vec b) const { return {x-b.x,y-b.y,z-b.z}; }
  Vec operator*(double s) const { return {x*s,y*s,z*s}; }
};
inline double Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline double Length(Vec v) { return std::sqrt(Dot(v,v)); }
inline Vec Unit(Vec v, Vec fallback = {}) { const double n=Length(v); return n>1e-9 ? v*(1/n) : fallback; }
struct Quat { double x{},y{},z{},w{1}; };
inline Quat Normalize(Quat q) {
  const double n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
  return n>1e-9 ? Quat{q.x/n,q.y/n,q.z/n,q.w/n} : Quat{};
}
inline Quat Inverse(Quat q) { return {-q.x,-q.y,-q.z,q.w}; }
inline Quat Multiply(Quat a, Quat b) {
  return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
          a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
          a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
          a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline Vec Rotate(Quat q, Vec v) {
  const Vec u{q.x,q.y,q.z}; const Vec uv=Cross(u,v);
  return v+uv*(2*q.w)+Cross(u,uv)*2;
}
inline Quat Swing(Vec from, Vec to) {
  const double d=Dot(from,to); const Vec c=Cross(from,to);
  if (d < -0.999999) {
    const Vec axis=Unit(Cross(from,std::abs(from.x)<0.8 ? Vec{1,0,0}:Vec{0,1,0}),{0,0,1});
    return {axis.x,axis.y,axis.z,0};
  }
  return Normalize({c.x,c.y,c.z,1+d});
}
inline Quat Twist(Quat q, Vec axis) {
  const double d=q.x*axis.x+q.y*axis.y+q.z*axis.z;
  const double n=std::sqrt(d*d+q.w*q.w);
  return n>1e-9 ? Quat{axis.x*d/n,axis.y*d/n,axis.z*d/n,q.w/n} : Quat{};
}
inline Vec Position(const Bone& b) { return {b[4],b[5],b[6]}; }
inline Quat Rotation(const Bone& b) { return {b[0],b[1],b[2],b[3]}; }
struct Frame {
  Vec position{}; Quat rotation{}; double scale{1};
  Vec Point(Vec v) const { return position+Rotate(rotation,v*scale); }
  Vec Local(Vec v) const { return Rotate(Inverse(rotation),v-position)*(1/scale); }
};
inline Frame Compose(Frame a, Frame b) {
  return {a.Point(b.position),Normalize(Multiply(a.rotation,b.rotation)),a.scale*b.scale};
}
inline Frame Inverse(Frame f) {
  const Quat q=Inverse(f.rotation);
  return {Rotate(q,f.position*(-1/f.scale)),q,1/f.scale};
}
inline Frame Transform(const Bone& b) { return {Position(b),Rotation(b),b[8]}; }
struct Capsule { Vec a,b; double radius{}; std::uint8_t mask{}; };
inline Vec Closest(Vec point, Vec a, Vec b) {
  const Vec axis=b-a;
  const double squared=Dot(axis,axis);
  return a+axis*(squared>1e-9 ? std::clamp(Dot(point-a,axis)/squared,0.0,1.0) : 0.0);
}

class Dynamics {
 public:
  bool Configure(const std::vector<Bone>& bind, const std::vector<std::int32_t>& parents,
                 const std::vector<std::string>& names) {
    bind_=bind; pose_=bind; parents_=parents; nodes_.clear(); driven_=0;
    body_volumes_.clear(); collisions_configured_=false; Reset();
    if (parents.size()!=bind.size() || names.size()!=bind.size()) return false;
    // The Poseable path receives parent-first reference bones.
    for (std::size_t i=0;i<bind.size();++i) {
      if (parents[i]<-1 || parents[i]>=static_cast<std::int32_t>(i)) return false;
      for (double v:bind[i]) if (!std::isfinite(v)) return false;
    }
    nodes_.resize(bind.size());
    for (std::size_t i=0;i<bind.size();++i) {
      auto& node=nodes_[i];
      const auto p=parents[i];
      const Frame parent=p>=0 ? Transform(bind[static_cast<std::size_t>(p)]) : Frame{};
      // Bake the authored component-space scale into the offset. Auxiliary
      // control bones may be scaled (even to zero) without scaling the strands.
      node.offset=Rotate(Inverse(parent.rotation),Position(bind[i])-parent.position);
      node.rest=Normalize(Multiply(Inverse(parent.rotation),Rotation(bind[i])));
      node.local=node.rest;
      node.swing_only=secondary::IsSwingOnly(names[i]);
      node.gravity=secondary::IsSleeve(names[i]) ? secondary::kSleeveGravityWeight : secondary::kGravityWeight;
      if (!secondary::IsSecondary(names[i]) || secondary::IsOrnament(names[i])) continue;
      if (std::abs(bind[i][8]-1)>1e-4 || std::abs(bind[i][9]-1)>1e-4 || std::abs(bind[i][10]-1)>1e-4)
        continue;
      for (std::size_t child=i+1;child<bind.size();++child) {
        if (parents[child]!=static_cast<std::int32_t>(i)) continue;
        const Vec offset=Rotate(Inverse(Rotation(bind[i])),Position(bind[child])-Position(bind[i]));
        const double length=Length(offset);
        if (length>node.length) { node.length=length; node.axis=Unit(offset); }
      }
      node.active=node.length>=1e-3;
      if (!node.active) continue;
      ++driven_;
      if (secondary::IsHair(names[i])) node.mask|=secondary::kCollideTorso;
      for (auto ancestor=p;ancestor>=0;ancestor=parents[static_cast<std::size_t>(ancestor)]) {
        const auto& name=names[static_cast<std::size_t>(ancestor)];
        if (name.rfind("Bn_",0)==0) continue;
        if (secondary::IsLegAttachment(name)) node.mask|=secondary::kCollideLegs;
        break;
      }
    }
    return driven_!=0;
  }

  // An independent component's attachment socket supplies the body ancestry
  // missing from its own reference skeleton. Clearance is measured in body space.
  void ConfigureCollisions(const std::vector<Bone>& body_bind,
                           const std::vector<std::string>& names,
                           Frame bind_placement, std::string_view socket) {
    body_volumes_.clear(); collisions_configured_=true;
    if (body_bind.size()!=names.size()) return;
    if (secondary::IsLegAttachment(socket))
      for (auto& n:nodes_) if (n.active) n.mask|=secondary::kCollideLegs;
    const auto index=[&](std::string_view name) {
      const auto it=std::find(names.begin(),names.end(),name);
      return it==names.end() ? -1 : static_cast<int>(it-names.begin());
    };
    for (const auto pair : {std::pair{"Bip001-Pelvis","Bip001-Spine"},
                            std::pair{"Bip001-Spine","Bip001-Spine1"},
                            std::pair{"Bip001-Spine1","Bip001-Spine2"}}) {
      const int a=index(pair.first),b=index(pair.second);
      if (a>=0 && b>=0) body_volumes_.push_back({a,b,0,secondary::kTorsoCollisionRadiusCm,secondary::kCollideTorso});
    }
    const int left=index("Bip001-L-Thigh"),right=index("Bip001-R-Thigh");
    const double cap=left>=0 && right>=0 ? .5*Length(Position(body_bind[left])-Position(body_bind[right])) : 0;
    for (const auto& volume:secondary::kLegVolumes) {
      const int a=index(volume.first),b=index(volume.second);
      if (a<0 || b<0) continue;
      const Vec end=Position(body_bind[b]);
      const Vec start=Position(body_bind[a])+(end-Position(body_bind[a]))*volume.fraction;
      double tightest=(std::numeric_limits<double>::infinity)();
      for (std::size_t i=0;i<nodes_.size();++i) {
        const auto& n=nodes_[i];
        if (!n.active || !(n.mask & secondary::kCollideLegs)) continue;
        const Vec head=Position(bind_[i]);
        const Vec direction=Rotate(Rotation(bind_[i]),n.axis);
        for (double slot:{.5,.75,1.0}) {
          const Vec point=bind_placement.Point(head+direction*(n.length*slot));
          tightest=(std::min)(tightest,Length(point-Closest(point,start,end)));
        }
      }
      if (tightest>.5 && tightest<1e6)
        body_volumes_.push_back({a,b,volume.fraction,
            cap>0 ? (std::min)(tightest,cap) : tightest,tightest,secondary::kCollideLegs});
    }
  }
  bool CollisionsConfigured() const { return collisions_configured_; }
  template<class PositionAt>
  std::vector<Capsule> BodyCapsules(std::size_t count, PositionAt position, Frame body_world) const {
    std::vector<Capsule> result;
    for (const auto& v:body_volumes_) {
      if (static_cast<std::size_t>((std::max)(v.a,v.b))>=count) continue;
      const Vec a=position(v.a),b=position(v.b);
      result.push_back({body_world.Point(a+(b-a)*v.fraction),body_world.Point(b),
                        v.radius*body_world.scale,v.mask});
    }
    return result;
  }

  void Reset() { seeded_=false; moving_=false; length_error_=0; pose_=bind_; }
  std::size_t DrivenBones() const { return driven_; }
  // How many simulated bones actually collide against the given mask. A chain that
  // only collides at its first bone cannot be pushed out by rotating that one bone.
  std::size_t MaskedBones(std::uint8_t mask) const {
    std::size_t count=0;
    for (const auto& node:nodes_) if (node.active && (node.mask & mask)) ++count;
    return count;
  }
  // The measured rest clearance of every collision volume, before the hip-separation
  // cap. A radius equal to the cap means the true clearance is larger, so the strand
  // sits outside the capsule at rest and only a swing brings it in.
  std::vector<double> VolumeClearances() const {
    std::vector<double> result;
    for (const auto& volume:body_volumes_) result.push_back(volume.clearance);
    return result;
  }
  // How often the collision actually corrected something. A strand that is merely
  // swinging shows almost no hits; a strand fighting a capsule shows a hit nearly
  // every frame.
  const std::vector<Bone>& Pose() const { return pose_; }
  bool Moving() const { return moving_; }
  double LengthError() const { return length_error_; }

  // Update and the mesh detour can submit the same timeline sample. Integrate it once.
  const std::vector<Bone>& Sample(Frame frame, double time, std::uint32_t seek_serial,
                                  bool playing, std::span<const Capsule> capsules={}) {
    if (!driven_) return pose_;
    const double dt=time-time_;
    const double alignment=frame.rotation.x*previous_.rotation.x+frame.rotation.y*previous_.rotation.y+
        frame.rotation.z*previous_.rotation.z+frame.rotation.w*previous_.rotation.w;
    bool teleported=Length(frame.position-previous_.position)>100;
    for (std::size_t i=0;i<nodes_.size() && !teleported;++i)
      if (nodes_[i].active && Length(frame.Point(Position(bind_[i]))-previous_.Point(Position(bind_[i])))>100)
        teleported=true;
    if (!playing || !seeded_ || seek_serial!=serial_ || dt<0 || dt>.1 || teleported ||
        std::abs(alignment)<.7071067811865476 || std::abs(frame.scale-previous_.scale)>1e-5) {
      Seed(frame,time,seek_serial);
      if (playing && !capsules.empty()) { Finish(frame,capsules); moving_=true; }
      return pose_;
    }
    if (dt<=1e-9) return pose_;
    for (std::size_t i=0;i<nodes_.size();++i) {
      auto& n=nodes_[i];
      const Frame parent=Parent(i,frame);
      const Vec head=parent.Point(n.offset);
      n.local=n.rest;
      if (n.active) {
        const Vec axis=Rotate(Multiply(parent.rotation,n.rest),n.axis);
        const Vec head_velocity=(head-n.head)*(1/dt);
        Vec accel=n.samples>=2 ? (head_velocity-n.head_velocity)*(2/(dt+n.dt)) : Vec{};
        const double magnitude=Length(accel);
        if (magnitude>3*secondary::kGravity) accel=accel*(3*secondary::kGravity/magnitude);
        const Vec hang=Unit(Vec{0,0,-secondary::kGravity}-accel*.5);
        Vec target=Unit(axis*(1-n.gravity)+hang*n.gravity,axis);
        // Steer the spring's own target out of the collision volumes before integrating.
        // Correcting only the delivered pose leaves the spring pulling the strand straight
        // back in, and the two then trade the same few degrees every frame: measured in
        // game, a bending ribbon was corrected 1.21 times per frame and reversed direction
        // 1.38 times per frame, while a strand that never collides sits at 0.0 and 0.19.
        if (n.mask && !capsules.empty()) {
          const bool torso=(n.mask & secondary::kCollideTorso)!=0;
          const auto mask=torso ? secondary::kCollideTorso : secondary::kCollideLegs;
          const double slack=(torso ? secondary::kTorsoCollisionSlackCm
                                    : secondary::kLegCollisionSlackCm)*frame.scale;
          const auto target_depth=[&](Vec candidate,Vec* slide) {
            double deepest=0;
            for (double slot:{.5,.75,1.0}) {
              const Vec point=head+candidate*(n.length*frame.scale*slot);
              for (const auto& capsule:capsules) {
                if (!(capsule.mask & mask)) continue;
                const Vec closest=Closest(point,capsule.a,capsule.b);
                const Vec out=point-closest;
                const double distance=Length(out),depth=capsule.radius-distance;
                if (depth>deepest && distance>.5*frame.scale) {
                  deepest=depth;
                  if (slide) *slide=closest+out*(capsule.radius/distance);
                }
              }
            }
            return deepest;
          };
          if (target_depth(target,nullptr)>slack) {
            Vec candidate=target,best=target;
            double best_depth=target_depth(target,nullptr);
            for (int attempt=0;attempt<12;++attempt) {
              Vec slide{};
              const double depth=target_depth(candidate,&slide);
              if (depth<=slack) { best=candidate; break; }
              if (depth<best_depth) { best_depth=depth; best=candidate; }
              const Vec aim=Unit(slide-head);
              if (Length(aim)<.5) break;
              candidate=Unit(candidate+(aim-candidate)*.5);
            }
            target=best;
          }
        }
        const auto spring=secondary::SpringFor(n.swing_only);
        const int steps=(std::max)(4,static_cast<int>(std::ceil(dt*120-1e-8)));
        const double h=dt/steps;
        for (int step=0;step<steps;++step) {
          n.velocity=n.velocity+(Cross(n.direction,target)*spring.stiffness-n.velocity*spring.damping)*h;
          n.direction=Unit(n.direction+Cross(n.velocity*h,n.direction),target);
        }
        const double lag=std::acos(std::clamp(Dot(n.direction,target),-1.0,1.0))*180/3.14159265358979323846;
        if (lag>spring.lag_degrees) {
          n.direction=Unit(n.direction+(target-n.direction)*(spring.lag_degrees/lag),target);
          n.velocity=n.velocity*.5;
        }
        const Vec local_axis=Unit(Rotate(Inverse(parent.rotation),n.direction));
        n.local=Multiply(Swing(n.axis,local_axis),Twist(n.rest,n.axis));
        if (n.swing_only) {
          const Quat deviation=Multiply(n.local,Inverse(n.rest));
          const Quat roll=Twist(deviation,Rotate(n.rest,n.axis));
          n.local=Multiply(Multiply(deviation,Inverse(roll)),n.rest);
        }
        n.local=Normalize(n.local);
      }
      n.world={head,Normalize(Multiply(parent.rotation,n.local)),frame.scale};
    }
    Finish(frame,capsules,dt);
    previous_=frame; time_=time; moving_=true;
    return pose_;
  }
 private:
  struct Node {
    Vec offset{},axis{},direction{},velocity{},head{},head_velocity{};
    Quat rest{},local{};
    Frame world{};
    double length{},gravity{},dt{};
    std::uint64_t samples{};
    std::uint8_t mask{};
    bool active{},swing_only{},held{};
  };
  struct BodyVolume { int a,b; double fraction,radius,clearance; std::uint8_t mask; };
  Frame Parent(std::size_t i,Frame frame) const {
    return parents_[i]>=0 ? nodes_[static_cast<std::size_t>(parents_[i])].world : frame;
  }
  void Seed(Frame frame,double time,std::uint32_t serial) {
    for (std::size_t i=0;i<nodes_.size();++i) {
      auto& n=nodes_[i];
      n.local=n.rest; n.world=Compose(Parent(i,frame),{n.offset,n.local,1});
      n.direction=Rotate(n.world.rotation,n.axis); n.velocity={};
      n.head=n.world.position; n.head_velocity={}; n.samples=1; n.dt=0; n.held=false;
    }
    previous_=frame; time_=time; serial_=serial; seeded_=true; moving_=false;
    pose_=bind_; length_error_=0;
  }
  void Collide(Node& n,Frame parent,Frame frame,std::span<const Capsule> capsules) {
    const bool torso=(n.mask & secondary::kCollideTorso)!=0;
    const auto mask=torso ? secondary::kCollideTorso : secondary::kCollideLegs;
    const double slack=(torso ? secondary::kTorsoCollisionSlackCm : secondary::kLegCollisionSlackCm)*frame.scale;
    const double threshold=slack;
    const Vec head=n.world.position;
    const Vec direction=Rotate(n.world.rotation,n.axis);
    const auto measure=[&](Vec candidate,Vec* slide) {
      double deepest=0;
      for (double slot:{.5,.75,1.0}) {
        const Vec point=head+candidate*(n.length*frame.scale*slot);
        for (const auto& capsule:capsules) {
          if (!(capsule.mask & mask)) continue;
          const Vec closest=Closest(point,capsule.a,capsule.b);
          const Vec out=point-closest;
          const double distance=Length(out),depth=capsule.radius-distance;
          if (depth>deepest && distance>.5*frame.scale) {
            deepest=depth;
            if (slide) *slide=closest+out*(capsule.radius/distance);
          }
        }
      }
      return deepest;
    };
    Vec slid=direction,best=direction;
    const double initial=measure(direction,nullptr);
    // Hysteresis band with separated edges: correct only once the strand is deeper than
    // 1.5x the slack, and release the hold only once it is well clear at 0.5x. Measured in
    // game, the ribbon hovers at 0.0-0.59 cm against a 0.5 cm slack, so a single threshold
    // there restarts the correction every other frame - that is the buzz.
    const double engage=slack*1.5;
    const double release=slack*0.5;
    if (initial<=release) {
      n.held=false;
      n.direction=direction;
      return;
    }
    if (n.held && initial<=engage) {
      n.direction=direction;
      return;
    }
    double best_depth=initial;
    for (int attempt=0;attempt<12;++attempt) {
      Vec slide{};
      const double depth=measure(slid,&slide);
      if (depth<best_depth) { best_depth=depth; best=slid; }
      if (depth<=threshold) break;
      const Vec aim=Unit(slide-head);
      if (Length(aim)<.5) break;
      slid=Unit(slid+(aim-slid)*.6);
    }
    n.held=best_depth<initial;
    if (n.held) {
      // A correction is a nudge, not a snap: cap how much of it lands in one frame and
      // let the following frames finish the job. Measured in game, a bending ribbon was
      // corrected 347 times a second by 12.1 deg on average and by up to 82.2 deg in a
      // single frame, which reads as a constant shake.
      constexpr double kMaxCorrectionDegrees=5.0;
      const Vec local_direction=Unit(Rotate(Inverse(parent.rotation),direction));
      Vec local_best=Unit(Rotate(Inverse(parent.rotation),best));
      const double sweep=std::acos(std::clamp(Dot(local_direction,local_best),-1.0,1.0))*
                         180.0/3.14159265358979323846;
      if (sweep>kMaxCorrectionDegrees)
        local_best=Unit(local_direction+
                        (local_best-local_direction)*(kMaxCorrectionDegrees/sweep));
      const Quat correction=Swing(local_direction,local_best);
      n.local=Normalize(Multiply(correction,n.local));
      n.world.rotation=Normalize(Multiply(parent.rotation,n.local));
      const Vec normal=Rotate(parent.rotation,Unit({correction.x,correction.y,correction.z}));
      const double along=Dot(n.velocity,normal);
      if (along<0) n.velocity=n.velocity-normal*along;
    }
    n.direction=Rotate(n.world.rotation,n.axis);
  }
  void Finish(Frame frame,std::span<const Capsule> capsules,double dt=0) {
    pose_=bind_; length_error_=0;
    // Recompute every descendant, including non-simulated leaves and ornaments,
    // after a parent correction. Local offsets never change, so nothing stretches.
    for (std::size_t i=0;i<nodes_.size();++i) {
      auto& n=nodes_[i]; const Frame parent=Parent(i,frame);
      n.world=Compose(parent,{n.offset,n.local,1});
      if (n.active && n.mask && !capsules.empty()) {
        Collide(n,parent,frame,capsules);
      }
      // Inertia must use the delivered heads, including corrections inherited
      // from parents, rather than mixing corrected positions with old velocities.
      if (n.active) {
        n.direction=Rotate(n.world.rotation,n.axis);
        if (dt>0) {
          n.head_velocity=(n.world.position-n.head)*(1/dt);
          n.dt=dt; ++n.samples;
        }
      }
      n.head=n.world.position;
      const Vec p=frame.Local(n.world.position);
      const Quat q=Normalize(Multiply(Inverse(frame.rotation),n.world.rotation));
      pose_[i][0]=q.x; pose_[i][1]=q.y; pose_[i][2]=q.z; pose_[i][3]=q.w;
      pose_[i][4]=p.x; pose_[i][5]=p.y; pose_[i][6]=p.z;
      if (parents_[i]>=0) length_error_=(std::max)(length_error_,
          std::abs(Length(p-Position(pose_[parents_[i]]))-Length(n.offset)));
    }
  }
  std::vector<Bone> bind_,pose_;
  std::vector<std::int32_t> parents_;
  std::vector<Node> nodes_;
  std::vector<BodyVolume> body_volumes_;
  Frame previous_{};
  std::size_t driven_{};
  double time_{},length_error_{};
  std::uint32_t serial_{};
  bool seeded_{},moving_{},collisions_configured_{};
};
} // namespace better_pose::accessory
