#pragma once

#include "bone_constraints.hpp"
#include "secondary_rules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
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
inline Vec Limit(Vec v, double maximum) {
  const double length=Length(v);
  return length>maximum && length>1e-9 ? v*(maximum/length) : v;
}
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
struct Capsule {
  Vec a,b;
  double radius{};
  std::uint8_t mask{};
  // Zero keeps the older constant-radius representation. A positive value
  // tapers the radius from `a` to `b`, matching the two-radius capsules used
  // by the reference cloth collider.
  double end_radius{};
};
inline Vec Closest(Vec point, Vec a, Vec b) {
  const Vec axis=b-a;
  const double squared=Dot(axis,axis);
  return a+axis*(squared>1e-9 ? std::clamp(Dot(point-a,axis)/squared,0.0,1.0) : 0.0);
}
inline double RadiusAt(const Capsule& capsule, Vec closest) {
  if (!(capsule.end_radius>0.0)) return capsule.radius;
  const Vec axis=capsule.b-capsule.a;
  const double squared=Dot(axis,axis);
  const double t=squared>1e-9 ? std::clamp(Dot(closest-capsule.a,axis)/squared,0.0,1.0) : 0.0;
  return capsule.radius+(capsule.end_radius-capsule.radius)*t;
}
inline Vec PerpendicularDirection(Vec axis, Vec preferred) {
  axis=Unit(axis);
  if (Length(axis)<.5) return Unit(preferred,{1,0,0});
  preferred=Unit(preferred-axis*Dot(preferred,axis));
  if (Length(preferred)>.5) return preferred;
  const Vec basis=std::abs(axis.x)<.8 ? Vec{1,0,0} : Vec{0,1,0};
  return Unit(Cross(axis,basis),{1,0,0});
}
struct CollisionContact {
  Vec strand{};
  Vec surface{};
  Vec capsule_a{};
  Vec capsule_b{};
  double radius{};
  double depth{};
  std::uint32_t bone{};
  std::uint8_t mask{};
};
struct DistanceLinkDebug {
  Vec a{};
  Vec b{};
  double rest{};
  double current{};
  bool corrected{};
};
struct CollisionDebugSnapshot {
  std::vector<Capsule> capsules;
  std::vector<CollisionContact> contacts;
  std::vector<DistanceLinkDebug> distance_links;
  std::vector<bone_constraints::ContactDebug> self_contacts;
  std::size_t corrections{};
  double maximum_depth{};
};
struct BoneOverride {
  std::string name;
  secondary::SpringSettings spring;
  double collision_radius{};
  double motion_follow{1.0};
  Vec native_gravity{};
  double forward_move_offset{};
};
struct AttachmentDrive {
  std::uint32_t bone{};
  Bone local{};
  double weight{1.0};
};

inline std::vector<Capsule> BuildBodyCollisionCapsules(
    std::span<const std::string> names,
    std::span<const std::array<double, 3>> points, std::uint8_t masks,
    bool extended_collision) {
  std::vector<Capsule> capsules;
  if (masks == 0 || names.empty() || names.size() != points.size())
    return capsules;
  const auto index=[&](std::string_view name) {
    const auto it=std::find(names.begin(),names.end(),name);
    return it==names.end() ? -1 : static_cast<int>(it-names.begin());
  };
  const auto position=[&](int bone) {
    const auto& p=points[static_cast<std::size_t>(bone)];
    return Vec{p[0],p[1],p[2]};
  };
  const auto add=[&](int a,int b,double radius,std::uint8_t mask,
                     double end_radius=0.0) {
    if (a>=0 && b>=0)
      capsules.push_back({position(a),position(b),radius,mask,end_radius});
  };
  const auto add_chain=[&](const char* first,const char* second,double radius,
                           std::uint8_t mask,double end_radius=0.0) {
    add(index(first),index(second),radius,mask,end_radius);
  };

  if ((masks & secondary::kCollideTorso) != 0) {
    add_chain("Bip001-Pelvis","Bip001-Spine",
              secondary::kTorsoCollisionRadiusCm,secondary::kCollideTorso);
    add_chain("Bip001-Spine","Bip001-Spine1",
              secondary::kTorsoCollisionRadiusCm,secondary::kCollideTorso);
    add_chain("Bip001-Spine1","Bip001-Spine2",
              secondary::kTorsoCollisionRadiusCm,secondary::kCollideTorso);
    add_chain("Bip001-Spine2","Bip001-Neck",
              secondary::kChestCollisionRadiusCm,secondary::kCollideTorso);
  }
  if ((masks & secondary::kCollideHead) != 0) {
    const int neck=index("Bip001-Neck"),head=index("Bip001-Head");
    add(neck,head,secondary::kNeckCollisionRadiusCm,secondary::kCollideHead,
        secondary::kNeckTopCollisionRadiusCm);
    add(head,head,secondary::kHeadCollisionRadiusCm,secondary::kCollideHead);
  }
  if ((masks & secondary::kCollideArms) != 0) {
    for (const char* side:{"L","R"}) {
      const std::string clavicle=std::string("Bip001-")+side+"-Clavicle";
      const std::string upper=std::string("Bip001-")+side+"-UpperArm";
      const std::string forearm=std::string("Bip001-")+side+"-Forearm";
      const std::string hand=std::string("Bip001-")+side+"-Hand";
      add_chain(clavicle.c_str(),upper.c_str(),
                secondary::kShoulderCollisionRadiusCm,secondary::kCollideArms);
      add_chain(upper.c_str(),forearm.c_str(),
                secondary::kArmCollisionRadiusCm,secondary::kCollideArms);
      add_chain(forearm.c_str(),hand.c_str(),
                secondary::kHandCollisionRadiusCm,secondary::kCollideArms);
    }
  }
  if ((masks & secondary::kCollideLegs) != 0) {
    const int left=index("Bip001-L-Thigh"),right=index("Bip001-R-Thigh");
    double cap=left>=0 && right>=0
                   ? .5*Length(position(left)-position(right))
                   : 0.0;
    if (extended_collision)
      cap=(std::min)(secondary::kLegCollisionRadiusCm,cap);
    const double thigh_radius=(std::max)(1.0,cap);
    const double calf_radius=(std::max)(1.0,cap*.78);
    for (const char* side:{"L","R"}) {
      const std::string thigh=std::string("Bip001-")+side+"-Thigh";
      const std::string calf=std::string("Bip001-")+side+"-Calf";
      const std::string foot=std::string("Bip001-")+side+"-Foot";
      const int a=index(thigh),b=index(calf),c=index(foot);
      if (a>=0 && b>=0) {
        const Vec start=position(a)+(position(b)-position(a))*.33;
        capsules.push_back(
            {start,position(b),thigh_radius,secondary::kCollideLegs,0.0});
      }
      add(b,c,calf_radius,secondary::kCollideLegs);
    }
  }
  return capsules;
}

class Dynamics {
  struct BodyVolume {
    int a,b;
    double fraction,radius,clearance;
    std::uint8_t mask;
    double end_radius{};
  };
 public:
  bool Configure(const std::vector<Bone>& bind, const std::vector<std::int32_t>& parents,
                 const std::vector<std::string>& names) {
    bind_=bind; pose_=bind; parents_=parents; nodes_.clear();
    collision_chains_.clear(); driven_=0;
    body_volumes_.clear(); collisions_configured_=false; Reset();
    air_enabled_=false; air_scale_=0.0;
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
      node.rest_offset=node.offset;
      node.rest=Normalize(Multiply(Inverse(parent.rotation),Rotation(bind[i])));
      node.local=node.rest;
      node.swing_only=secondary::IsSwingOnly(names[i]);
      node.draped=secondary::IsDrapedRibbon(names[i]);
      node.spring=secondary::SpringFor(node.swing_only);
      node.material=secondary::ClassifyMotionMaterial(names[i]);
      node.response=secondary::ResponseFor(node.material);
      node.name=names[i];
      node.gravity = secondary::IsDrapedRibbon(names[i])
                         ? secondary::kDrapedGravityWeight
                         : (secondary::IsSleeve(names[i])
                                ? secondary::kSleeveGravityWeight
                                : secondary::kGravityWeight);
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
      if (secondary::IsHair(names[i]))
        node.constraint_mask |= secondary::kCollideTorso |
                                secondary::kCollideHead |
                                secondary::kCollideArms;
      for (auto ancestor=p;ancestor>=0;ancestor=parents[static_cast<std::size_t>(ancestor)]) {
        const auto& name=names[static_cast<std::size_t>(ancestor)];
        if (name.rfind("Bn_",0)==0) continue;
        if (secondary::IsLegAttachment(name))
          node.constraint_mask |= secondary::kCollideLegs;
        if (secondary::IsHeadAttachment(name))
          node.constraint_mask |= secondary::kCollideHead;
        if (secondary::IsArmAttachment(name) && !secondary::IsSleeve(names[i]))
          node.constraint_mask |= secondary::kCollideArms;
        break;
      }
    }
    collision_chains_.assign(nodes_.size(), -1);
    for (std::size_t i{}; i < nodes_.size(); ++i) {
      if (!nodes_[i].active)
        continue;
      const std::int32_t parent = parents_[i];
      collision_chains_[i] =
          parent >= 0 &&
                  nodes_[static_cast<std::size_t>(parent)].active
              ? collision_chains_[static_cast<std::size_t>(parent)]
              : static_cast<std::int32_t>(i);
    }
    RefreshConstraintMasks();
    RebuildBoneConstraints();
    return driven_!=0;
  }

  // An independent component's attachment socket supplies the body ancestry
  // missing from its own reference skeleton. Clearance is measured in body space.
  void ConfigureCollisions(const std::vector<Bone>& body_bind,
                           const std::vector<std::string>& names,
                           Frame bind_placement, std::string_view socket) {
    body_volumes_.clear(); collisions_configured_=true;
    if (body_bind.size()!=names.size()) return;
    std::uint8_t needed=0;
    for (auto& n:nodes_) {
      if (!n.active) continue;
      needed|=n.mask;
    }
    if (secondary::IsLegAttachment(socket)) {
      for (auto& n:nodes_)
        if (n.active) n.constraint_mask |= secondary::kCollideLegs;
      needed|=secondary::kCollideLegs;
    }
    if (secondary::IsHeadAttachment(socket)) {
      for (auto& n:nodes_)
        if (n.active) n.constraint_mask |= secondary::kCollideHead;
      needed|=secondary::kCollideHead;
    }
    if (secondary::IsArmAttachment(socket)) {
      for (std::size_t i=0;i<nodes_.size();++i)
        if (nodes_[i].active && !secondary::IsSleeve(names[i]))
          nodes_[i].constraint_mask |= secondary::kCollideArms;
      needed|=secondary::kCollideArms;
    }
    RefreshConstraintMasks();
    needed = secondary::EffectiveCollisionMask(
        needed, tuning_.extended_collision, tuning_.native_limits);
    const auto index=[&](std::string_view name) {
      const auto it=std::find(names.begin(),names.end(),name);
      return it==names.end() ? -1 : static_cast<int>(it-names.begin());
    };
    if ((needed & secondary::kCollideTorso) != 0) {
      for (const auto pair : {std::pair{"Bip001-Pelvis","Bip001-Spine"},
                              std::pair{"Bip001-Spine","Bip001-Spine1"},
                              std::pair{"Bip001-Spine1","Bip001-Spine2"}}) {
        const int a=index(pair.first),b=index(pair.second);
        if (a>=0 && b>=0)
          body_volumes_.push_back(
              {a,b,0,secondary::kTorsoCollisionRadiusCm,0,secondary::kCollideTorso,0});
      }
      const int chest=index("Bip001-Spine2"),neck=index("Bip001-Neck");
      if (chest>=0 && neck>=0)
        body_volumes_.push_back(
            {chest,neck,0,secondary::kChestCollisionRadiusCm,0,
             secondary::kCollideTorso,0});
    }
    if ((needed & secondary::kCollideHead) != 0) {
      const int neck=index("Bip001-Neck");
      const int head=index("Bip001-Head");
      if (neck>=0 && head>=0)
        body_volumes_.push_back(
            {neck,head,0,secondary::kNeckCollisionRadiusCm,0,
             secondary::kCollideHead,secondary::kNeckTopCollisionRadiusCm});
      if (head>=0)
        body_volumes_.push_back(
            {head,head,0,secondary::kHeadCollisionRadiusCm,0,
             secondary::kCollideHead,0});
    }
    if ((needed & secondary::kCollideArms) != 0) {
      const auto add_arm=[&](const char* first,const char* second,const double radius) {
        const int a=index(first),b=index(second);
        if (a>=0 && b>=0)
          body_volumes_.push_back(
              {a,b,0,radius,0,secondary::kCollideArms,0});
      };
      for (const char* side : {"L","R"}) {
        const std::string clavicle=std::string("Bip001-")+side+"-Clavicle";
        const std::string upper=std::string("Bip001-")+side+"-UpperArm";
        const std::string forearm=std::string("Bip001-")+side+"-Forearm";
        const std::string hand=std::string("Bip001-")+side+"-Hand";
        add_arm(clavicle.c_str(),upper.c_str(),secondary::kShoulderCollisionRadiusCm);
        add_arm(upper.c_str(),forearm.c_str(),secondary::kArmCollisionRadiusCm);
        add_arm(forearm.c_str(),hand.c_str(),secondary::kHandCollisionRadiusCm);
      }
    }
    const int left=index("Bip001-L-Thigh"),right=index("Bip001-R-Thigh");
    const double separation_cap =
        left>=0 && right>=0
            ? .5*Length(Position(body_bind[left])-Position(body_bind[right]))
            : 0.0;
    const double cap =
        tuning_.extended_collision && separation_cap > 0.0
            ? (std::min)(secondary::kLegCollisionRadiusCm, separation_cap)
            : separation_cap;
    if ((needed & secondary::kCollideLegs) != 0) {
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
              cap>0 ? (std::min)(tightest,cap) : tightest,tightest,
              secondary::kCollideLegs,0});
      }
    }
  }
  bool CollisionsConfigured() const { return collisions_configured_; }
  std::size_t ConfigureBoneOverrides(std::span<const BoneOverride> overrides) {
    std::size_t matched{};
    for (const auto& override : overrides) {
      if (override.name.empty() || !std::isfinite(override.spring.stiffness) ||
          !std::isfinite(override.spring.damping) ||
          !std::isfinite(override.spring.lag_degrees) ||
          override.spring.stiffness < 0.0 || override.spring.damping < 0.0)
        continue;
      for (auto& node : nodes_) {
        if (node.name != override.name)
          continue;
        if (!node.active) {
          const auto index =
              static_cast<std::size_t>(&node - nodes_.data());
          for (std::size_t child{}; child != nodes_.size(); ++child) {
            if (parents_[child] != static_cast<std::int32_t>(index))
              continue;
            const Vec offset =
                Rotate(Inverse(Rotation(bind_[index])),
                       Position(bind_[child]) - Position(bind_[index]));
            const double length = Length(offset);
            if (length > node.length) {
              node.length = length;
              node.axis = Unit(offset);
            }
          }
          if (node.length >= 1e-3) {
            node.active = true;
            ++driven_;
          }
        }
        node.spring = {
            std::clamp(override.spring.stiffness, 0.0, 1000000.0),
            std::clamp(override.spring.damping, 0.0, 1000.0),
            std::clamp(override.spring.lag_degrees, 0.0, 180.0)};
        node.collision_radius =
            std::isfinite(override.collision_radius)
                ? std::clamp(override.collision_radius, 0.0, 100.0)
                : 0.0;
        node.motion_follow =
            std::isfinite(override.motion_follow)
                ? std::clamp(override.motion_follow, 0.0, 1.0)
                : 1.0;
        node.native_gravity = override.native_gravity;
        node.forward_move_offset =
            std::isfinite(override.forward_move_offset)
                ? std::clamp(override.forward_move_offset, -1.0, 1.0)
                : 0.0;
        ++matched;
        break;
      }
    }
    RebuildBoneConstraints();
    return matched;
  }
  void ConfigureAir(bool enabled, double scale) {
    air_enabled_ = enabled && std::isfinite(scale) && scale > 0.0;
    air_scale_ = air_enabled_ ? std::clamp(scale, 0.0, 8.0) : 0.0;
  }
  bool AirConfigured() const { return air_enabled_; }
  void SetTuning(const secondary::DynamicsTuning& tuning) {
    const auto safe=secondary::SafeDynamicsTuning(tuning);
    const bool rebuild_self =
        !self_collision_tuning_initialized_ ||
        safe.self_collision_pairs != tuning_.self_collision_pairs ||
        safe.self_collision_radius != tuning_.self_collision_radius ||
        safe.self_collision_strength != tuning_.self_collision_strength ||
        safe.self_collision_damping != tuning_.self_collision_damping ||
        safe.self_collision_outer_priority !=
            tuning_.self_collision_outer_priority;
    if (safe.extended_collision != tuning_.extended_collision) {
      body_volumes_.clear();
      collisions_configured_=false;
      collision_debug_.capsules.clear();
      collision_debug_.contacts.clear();
      collision_debug_.distance_links.clear();
      collision_debug_.self_contacts.clear();
      collision_debug_.maximum_depth=0.0;
    }
    if (!safe.self_collision) {
      std::fill(self_contact_state_.begin(), self_contact_state_.end(),
                std::uint8_t{0});
      collision_debug_.self_contacts.clear();
    }
    tuning_=safe;
    self_collision_tuning_initialized_=true;
    RefreshConstraintMasks();
    if (rebuild_self)
      RebuildBoneConstraints();
  }
  std::size_t MaterialBoneCount(secondary::MotionMaterial material) const {
    return static_cast<std::size_t>(std::count_if(
        nodes_.begin(),nodes_.end(),[material](const Node& node) {
          return node.active && node.material==material;
        }));
  }
  void EnableConstraintMask(const std::uint8_t mask) {
    for (auto& node : nodes_) {
      if (!node.active)
        continue;
      node.constraint_mask |= mask;
      node.mask = secondary::EffectiveCollisionMask(
          node.constraint_mask, tuning_.extended_collision,
          tuning_.native_limits);
    }
  }
  bool EffectiveSpring(std::string_view name,
                       secondary::SpringSettings& result) const {
    for (const auto& node : nodes_) {
      if (node.name != name)
        continue;
      result = node.spring;
      return true;
    }
    return false;
  }
  bool EffectiveNativeGravity(std::string_view name, Vec& result) const {
    for (const auto& node : nodes_) {
      if (node.name != name)
        continue;
      result = node.native_gravity;
      return true;
    }
    return false;
  }
  std::size_t BodyCapsuleCount(std::uint8_t mask) const {
    return static_cast<std::size_t>(std::count_if(
        body_volumes_.begin(), body_volumes_.end(),
        [mask](const BodyVolume& volume) { return (volume.mask & mask) != 0; }));
  }
  template<class PositionAt>
  std::vector<Capsule> BodyCapsules(std::size_t count, PositionAt position, Frame body_world) const {
    std::vector<Capsule> result;
    for (const auto& v:body_volumes_) {
      if (static_cast<std::size_t>((std::max)(v.a,v.b))>=count) continue;
      const Vec a=position(v.a),b=position(v.b);
      result.push_back({body_world.Point(a+(b-a)*v.fraction),body_world.Point(b),
                        v.radius*body_world.scale,v.mask,
                        v.end_radius*body_world.scale});
    }
    return result;
  }

  void Reset() {
    seeded_=false;
    moving_=false;
    length_error_=0;
    corrections_=0;
    distance_corrections_=0;
    self_corrections_=0;
    collision_debug_.capsules.clear();
    collision_debug_.contacts.clear();
    collision_debug_.distance_links.clear();
    collision_debug_.self_contacts.clear();
    collision_debug_.corrections=0;
    collision_debug_.maximum_depth=0.0;
    pose_=bind_;
  }
  std::size_t DrivenBones() const { return driven_; }
  std::size_t DistanceConstraintCount() const {
    return distance_links_.size();
  }
  std::size_t DistanceConstraintsSolved() const {
    return distance_corrections_;
  }
  std::size_t SelfCollisionCount() const {
    return static_cast<std::size_t>(std::count_if(
        self_contact_state_.begin(), self_contact_state_.end(),
        [](const std::uint8_t value) { return value != 0; }));
  }
  std::size_t SelfCollisionLinkCount() const { return self_links_.size(); }
  std::size_t SelfCollisionCandidateCount(
      const secondary::MotionMaterial material) const {
    return static_cast<std::size_t>(std::count_if(
        self_links_.begin(), self_links_.end(),
        [&](const bone_constraints::Link &link) {
          return nodes_[link.a].material == material ||
                 nodes_[link.b].material == material;
        }));
  }
  std::size_t SelfCollisionContactCount(
      const secondary::MotionMaterial material) const {
    std::size_t count{};
    for (std::size_t index{};
         index < self_links_.size() && index < self_contact_state_.size();
         ++index) {
      if (self_contact_state_[index] == 0)
        continue;
      const auto &link = self_links_[index];
      if (nodes_[link.a].material == material ||
          nodes_[link.b].material == material)
        ++count;
    }
    return count;
  }
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
  std::size_t Corrections() const { return corrections_; }
  const CollisionDebugSnapshot& CollisionDebug() const { return collision_debug_; }
  void ClearCollisionDebug() {
    collision_debug_.capsules.clear();
    collision_debug_.contacts.clear();
    collision_debug_.distance_links.clear();
    collision_debug_.self_contacts.clear();
    collision_debug_.corrections=corrections_;
    collision_debug_.maximum_depth=0.0;
  }
  // The simulation only needs to publish the current capsule set. Contact
  // probing is an overlay diagnostic and must not run inside every solver
  // sample, where it multiplies the complete bone/capsule cross product.
  void PublishCollisionDebugCapsules(std::span<const Capsule> capsules) {
    collision_debug_.capsules.assign(capsules.begin(),capsules.end());
    collision_debug_.contacts.clear();
    collision_debug_.corrections=corrections_;
    collision_debug_.maximum_depth=0.0;
  }
  // Full contact probing is intentionally separate from publication above.
  // Production overlay code may call it only on its throttled diagnostic pass.
  void RecomputeCollisionDebug(Frame frame,std::span<const Capsule> capsules) {
    collision_debug_.capsules.assign(capsules.begin(),capsules.end());
    collision_debug_.contacts.clear();
    collision_debug_.corrections=corrections_;
    collision_debug_.maximum_depth=0.0;
    for (std::size_t i=0;i<nodes_.size() && i<pose_.size();++i) {
      const Node& node=nodes_[i];
      if (!node.active || !node.mask) continue;
      const Vec head=frame.Point(Position(pose_[i]));
      const Quat world_rotation=Normalize(Multiply(frame.rotation,Rotation(pose_[i])));
      const Vec direction=Rotate(world_rotation,node.axis);
      CollisionContact deepest;
      for (double slot:{.5,.75,1.0}) {
        const Vec point=head+direction*(node.length*frame.scale*slot);
        for (const Capsule& capsule:capsules) {
          if (!(capsule.mask & node.mask)) continue;
          const Vec closest=Closest(point,capsule.a,capsule.b);
          const Vec out=point-closest;
          const double distance=Length(out);
          if (distance<=.5*frame.scale) continue;
          const double radius=RadiusAt(capsule,closest)+node.collision_radius*frame.scale;
          const double depth=radius-distance;
          if (depth<=deepest.depth) continue;
          deepest.strand=point;
          deepest.surface=closest+out*(radius/distance);
          deepest.capsule_a=capsule.a;
          deepest.capsule_b=capsule.b;
          deepest.radius=radius;
          deepest.depth=depth;
          deepest.bone=static_cast<std::uint32_t>(i);
          deepest.mask=capsule.mask;
        }
      }
      if (deepest.depth>0.0) {
        collision_debug_.maximum_depth=
            (std::max)(collision_debug_.maximum_depth,deepest.depth);
        collision_debug_.contacts.push_back(deepest);
      }
    }
  }

  // Update and the mesh detour can submit the same timeline sample. Integrate it once.
  const std::vector<Bone>& Sample(Frame frame, double time, std::uint32_t seek_serial,
                                  bool playing, std::span<const Capsule> capsules={},
                                  std::span<const AttachmentDrive> attachments={}) {
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
      if (playing && !attachments.empty())
        ApplySeedAttachments(frame,attachments);
      if (playing && (!capsules.empty() || !attachments.empty())) {
        Finish(frame,capsules);
        moving_=true;
      }
      return pose_;
    }
    if (dt<=1e-9) return pose_;
    UpdateFrameMotion(frame,dt);
    for (std::size_t i=0;i<nodes_.size();++i) {
      auto& n=nodes_[i];
      const Frame parent=Parent(i,frame);
      const AttachmentDrive* attachment=nullptr;
      for (const auto& candidate:attachments) {
        if (candidate.bone==i) {
          attachment=&candidate;
          break;
        }
      }
      n.body_driven=false;
      n.offset=n.rest_offset;
      n.local=n.rest;
      if (attachment!=nullptr) {
        const Bone driven=
            BlendBone(bind_[i],attachment->local,attachment->weight);
        if (ValidBone(driven)) {
          const Frame wanted=Compose(frame,Transform(driven));
          const double parent_scale=parent.scale>1e-9 ? parent.scale : 1.0;
          n.offset=Rotate(Inverse(parent.rotation),wanted.position-parent.position)*
                   (1.0/parent_scale);
          n.local=Normalize(Multiply(Inverse(parent.rotation),wanted.rotation));
          n.body_driven=true;
        }
      }
      const Vec head=parent.Point(n.offset);
      if (n.active && !n.body_driven) {
        const bool breast=secondary::IsBreast(n.name);
        const Vec axis=Rotate(Multiply(parent.rotation,n.rest),n.axis);
        const double inertia_gain=InertiaGain(n);
        if (inertia_gain<=1e-6) {
          // A zero inertia setting is an explicit rest-pose hold, not merely a
          // disabled acceleration term. Gravity and spring stiffness must not
          // keep a supposedly stationary ribbon moving by itself.
          n.direction=axis;
          n.velocity={};
          n.filtered_accel={};
          n.held=false;
          n.collision_normal_ready=false;
        } else {
          const Vec head_velocity=(head-n.head)*(1/dt);
          Vec accel=n.samples>=2 ? (head_velocity-n.head_velocity)*(2/(dt+n.dt)) : Vec{};
          const Vec radius=head-frame.position;
          const double magnitude=Length(accel);
          if (magnitude>3*secondary::kGravity) accel=accel*(3*secondary::kGravity/magnitude);
          accel=accel*n.motion_follow*n.response.inertia*inertia_gain;
          const Vec gravity = Length(n.native_gravity) > 1e-6
                                  ? Rotate(frame.rotation, n.native_gravity)
                                  : Vec{0, 0, -secondary::kGravity};
          Vec air{};
          {
            const Vec flow =
                frame_velocity_ +
                Cross(frame_angular_velocity_,radius);
            const double response_scale=
                air_enabled_ ? air_scale_ : secondary::kMovingAirResponse;
            air=Limit(flow*(-0.55*response_scale*n.response.air*tuning_.air),
                      1.5*secondary::kGravity);
          }
          if (std::abs(n.forward_move_offset)>1e-6) {
            const double moving=std::clamp(Length(frame_velocity_)/300.0,0.0,1.0);
            const Vec forward=Rotate(frame.rotation,{1,0,0});
            air=air+forward*(n.forward_move_offset*moving*
                              secondary::kGravity*.45*n.response.air);
          }
          const Vec hang=Unit(gravity-accel*.5+air);
          const double gravity_weight=
              std::clamp(n.gravity*tuning_.gravity*
                             (breast ? tuning_.breast_gravity : 1.0),
                         0.0,1.0);
          Vec target=Unit(axis*(1-gravity_weight)+hang*gravity_weight,axis);
          auto spring=n.spring;
          spring.damping*=tuning_.damping;
          if (breast) {
            spring.stiffness*=tuning_.breast_stiffness;
            spring.damping*=tuning_.breast_damping;
          }
          double ribbon_raw_response=0.0;
          const Vec previous_ribbon_direction=n.direction;
          if (n.material==secondary::MotionMaterial::Ribbon) {
            ribbon_raw_response=std::clamp(
                inertia_gain*std::clamp(spring.stiffness/200.0,0.0,1.0),
                0.0,1.0);
            const double response=secondary::NormalizedChainResponse(
                ribbon_raw_response,n.ribbon_chain_length);
            spring.stiffness*=response;
            spring.damping*=tuning_.ribbon_damping;
          }
          const double step_rate=(std::max)(120.0,spring.damping*2.0);
          const int steps=std::clamp(
              static_cast<int>(std::ceil(dt*step_rate-1e-8)),4,128);
          const double h=dt/steps;
          for (int step=0;step<steps;++step) {
            n.velocity=n.velocity+(Cross(n.direction,target)*spring.stiffness-n.velocity*spring.damping)*h;
            n.direction=Unit(n.direction+Cross(n.velocity*h,n.direction),target);
          }
          const double breast_amplitude=
              breast ? tuning_.breast_amplitude : 1.0;
          if (breast && breast_amplitude<=1e-6) {
            n.direction=axis;
            target=axis;
            n.velocity={};
          } else if (breast && std::abs(breast_amplitude-1.0)>1e-6) {
            n.direction=Unit(
                axis+(n.direction-axis)*breast_amplitude,n.direction);
          }
          const double lag=std::acos(std::clamp(Dot(n.direction,target),-1.0,1.0))*180.0/3.14159265358979323846;
          const double maximum_lag=breast
              ? std::clamp(spring.lag_degrees*breast_amplitude,0.0,120.0)
              : spring.lag_degrees;
          if (lag>maximum_lag) {
            n.direction=Unit(
                n.direction+(target-n.direction)*(maximum_lag/lag),target);
            n.velocity=n.velocity*.5;
          }
          if (n.material==secondary::MotionMaterial::Ribbon) {
            const double angle_degrees=
                std::acos(std::clamp(
                    Dot(previous_ribbon_direction,n.direction),-1.0,1.0))*
                180.0/3.14159265358979323846;
            const double maximum_step_degrees=
                0.75+7.25*ribbon_raw_response;
            if (angle_degrees>maximum_step_degrees) {
              n.direction=Unit(
                  previous_ribbon_direction+
                      (n.direction-previous_ribbon_direction)*
                          (maximum_step_degrees/angle_degrees),
                  n.direction);
            }
            if (Length(n.velocity)<0.015)
              n.velocity={};
          }
          const Vec local_axis=Unit(Rotate(Inverse(parent.rotation),n.direction));
          n.local=Multiply(Swing(n.axis,local_axis),Twist(n.rest,n.axis));
          if (n.swing_only ||
              n.material==secondary::MotionMaterial::Ribbon) {
            const Quat deviation=Multiply(n.local,Inverse(n.rest));
            const Quat roll=Twist(deviation,Rotate(n.rest,n.axis));
            n.local=Multiply(Multiply(deviation,Inverse(roll)),n.rest);
          }
          n.local=Normalize(n.local);
        }
      }
      n.world={head,Normalize(Multiply(parent.rotation,n.local)),frame.scale};
    }
    if (tuning_.bone_constraints && !distance_links_.empty())
      RelaxDistanceConstraints(frame);
    if (tuning_.self_collision && !self_links_.empty())
      RelaxSelfCollisions(frame);
    Finish(frame,capsules,dt);
    previous_=frame; time_=time; moving_=true;
    return pose_;
  }
 private:
  struct Node {
    std::string name;
    Vec offset{},rest_offset{},axis{},direction{},velocity{},head{},head_velocity{},
        filtered_accel{},filtered_target{};
    Quat rest{},local{};
    Frame world{};
    double length{},gravity{},dt{},collision_radius{},motion_follow{1.0};
    std::uint32_t ribbon_chain_length{1};
    double forward_move_offset{};
    Vec native_gravity{};
    secondary::MotionMaterial material{secondary::MotionMaterial::General};
    secondary::MotionResponse response{secondary::ResponseFor(
        secondary::MotionMaterial::General)};
    secondary::SpringSettings spring{};
    Bone filtered_attachment{};
    std::uint64_t samples{};
    std::uint8_t constraint_mask{};
    std::uint8_t mask{};
    std::size_t collision_capsule{
        (std::numeric_limits<std::size_t>::max)()};
    Vec collision_normal{};
    bool active{},swing_only{},draped{},held{};
    bool body_driven{};
    bool collision_normal_ready{};
    bool filtered_attachment_ready{};
  };
  struct CollisionProbe {
    double depth{};
    Vec surface{};
    Vec normal{};
    std::size_t capsule{
        (std::numeric_limits<std::size_t>::max)()};
  };
  CollisionProbe ProbeCollision(
      const Node& n,Frame parent,Frame frame,Vec head,Vec candidate,
      std::span<const Capsule> capsules) const {
    CollisionProbe result;
    for (double slot:{.5,.75,1.0}) {
      const Vec point=head+candidate*(n.length*frame.scale*slot);
      for (std::size_t index{};index<capsules.size();++index) {
        const Capsule& capsule=capsules[index];
        if (!(capsule.mask & n.mask)) continue;
        const Vec closest=Closest(point,capsule.a,capsule.b);
        const Vec outward=point-closest;
        const double distance=Length(outward);
        const double radius=
            RadiusAt(capsule,closest)+n.collision_radius*frame.scale;
        const double depth=radius-distance;
        if (depth<=result.depth) continue;

        const Vec axis=Unit(capsule.b-capsule.a);
        Vec normal=Unit(outward);
        // A strand crossing near the capsule axis has no stable radial side.
        // Keep the previous side for the same capsule until Kawaii releases
        // the contact; otherwise the closest-point branch flips by 180 degrees
        // and the next correction looks like a teleport.
        if (n.held && n.collision_normal_ready &&
            n.collision_capsule==index) {
          const Vec previous=PerpendicularDirection(
              axis,Rotate(parent.rotation,n.collision_normal));
          if (Length(previous)>.5 &&
              (Length(normal)<.5 || Dot(normal,previous)<0.0))
            normal=previous;
        }
        if (Length(normal)<.5)
          normal=PerpendicularDirection(
              axis,Rotate(parent.rotation,n.axis));
        result.depth=depth;
        result.surface=closest+normal*radius;
        result.normal=normal;
        result.capsule=index;
      }
    }
    return result;
  }
  void RefreshConstraintMasks() {
    for (auto& node : nodes_)
      node.mask = secondary::EffectiveCollisionMask(
          node.constraint_mask, tuning_.extended_collision,
          tuning_.native_limits);
  }
  static bool ValidBone(const Bone& bone) {
    return std::all_of(bone.begin(),bone.end(),
                       [](double value) { return std::isfinite(value); });
  }
  static Bone BlendBone(const Bone& from,const Bone& to,double weight) {
    if (!ValidBone(from) || !ValidBone(to)) return from;
    const double t=std::clamp(std::isfinite(weight) ? weight : 1.0,0.0,1.0);
    Quat a=Normalize(Rotation(from)),b=Normalize(Rotation(to));
    const double dot=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
    if (dot<0.0) b={-b.x,-b.y,-b.z,-b.w};
    const Quat rotation=Normalize(
        {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,
         a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t});
    Bone result=from;
    result[0]=rotation.x; result[1]=rotation.y;
    result[2]=rotation.z; result[3]=rotation.w;
    for (std::size_t axis{};axis<3;++axis) {
      result[4+axis]=from[4+axis]+(to[4+axis]-from[4+axis])*t;
      result[8+axis]=from[8+axis]+(to[8+axis]-from[8+axis])*t;
    }
    return result;
  }
  void RebuildBoneConstraints() {
    distance_candidates_.assign(nodes_.size(), {});
    std::vector<bone_constraints::Vec> body_references;
    body_references.reserve(8);
    std::vector<std::uint32_t> root_of(nodes_.size());
    for (std::size_t i{}; i < nodes_.size(); ++i) {
      const std::string &name = nodes_[i].name;
      if (name.find("Pelvis") != std::string::npos ||
          name.find("Spine") != std::string::npos ||
          name.find("Neck") != std::string::npos ||
          name.find("Head") != std::string::npos)
      {
        const Vec position = Position(bind_[i]);
        body_references.push_back(
            {position.x, position.y, position.z});
      }
      std::size_t root = i;
      for (std::int32_t parent = parents_[i];
           parent >= 0 &&
           nodes_[static_cast<std::size_t>(parent)].active;
           parent = parents_[static_cast<std::size_t>(parent)])
        root = static_cast<std::size_t>(parent);
      root_of[i] = static_cast<std::uint32_t>(root);
    }
    for (std::size_t i{}; i < nodes_.size(); ++i) {
      Node &node = nodes_[i];
      if (!node.active)
        continue;
      std::size_t root = i;
      std::uint32_t depth{};
      for (std::int32_t parent = parents_[i];
           parent >= 0 &&
           nodes_[static_cast<std::size_t>(parent)].active;
           parent = parents_[static_cast<std::size_t>(parent)]) {
        root = static_cast<std::size_t>(parent);
        ++depth;
      }
      const Vec direction = Rotate(Rotation(bind_[i]), node.axis);
      const Vec head = Position(bind_[i]);
      const Vec tip = head + direction * node.length;
      auto &candidate = distance_candidates_[i];
      candidate.head = {head.x, head.y, head.z};
      candidate.tip = {tip.x, tip.y, tip.z};
      candidate.length = node.length;
      candidate.anchor = parents_[root];
      candidate.chain = static_cast<std::uint32_t>(root);
      candidate.depth = depth;
      candidate.material =
          static_cast<std::uint32_t>(node.material);
      candidate.collision_mask =
          secondary::SelfCollisionMask(node.material,
                                       tuning_.self_collision_pairs);
      candidate.radius =
          node.collision_radius *
          tuning_.self_collision_radius[
              secondary::MotionMaterialIndex(node.material)];
      const bone_constraints::Vec midpoint =
          (candidate.head + candidate.tip) * 0.5;
      double layer_depth =
          std::numeric_limits<double>::infinity();
      for (const auto reference : body_references)
        layer_depth =
            (std::min)(layer_depth, Length(midpoint - reference));
      if (!std::isfinite(layer_depth)) {
        const Vec anchor =
            parents_[root] >= 0
                ? Position(bind_[static_cast<std::size_t>(parents_[root])])
                : Vec{};
        layer_depth =
            bone_constraints::Length(
                midpoint -
                bone_constraints::Vec{anchor.x, anchor.y, anchor.z});
      }
      candidate.layer_depth = layer_depth;
      candidate.eligible = true;
      std::uint32_t ribbon_chain_length{};
      if (node.material == secondary::MotionMaterial::Ribbon) {
        for (std::size_t other{}; other < nodes_.size(); ++other)
          if (nodes_[other].active &&
              nodes_[other].material == secondary::MotionMaterial::Ribbon &&
              root_of[other] == root)
            ++ribbon_chain_length;
      }
      node.ribbon_chain_length = (std::max)(ribbon_chain_length, 1U);
    }
    distance_links_ =
        bone_constraints::Discover(distance_candidates_);
    self_links_ =
        bone_constraints::DiscoverSelfCollision(distance_candidates_);
    for (auto &link : self_links_) {
      const auto material_a = nodes_[link.a].material;
      const auto material_b = nodes_[link.b].material;
      const auto index_a = secondary::MotionMaterialIndex(material_a);
      const auto index_b = secondary::MotionMaterialIndex(material_b);
      link.correction_gain =
          std::clamp(0.22 *
                         std::sqrt(tuning_.self_collision_strength[index_a] *
                                   tuning_.self_collision_strength[index_b]),
                     0.02, 0.7);
      link.velocity_keep =
          std::clamp(0.5 /
                         std::sqrt(tuning_.self_collision_damping[index_a] *
                                   tuning_.self_collision_damping[index_b]),
                     0.03, 0.95);
      if (!tuning_.self_collision_outer_priority) {
        link.correction_share_a = 0.5;
        link.correction_share_b = 0.5;
      }
    }
    self_contact_state_.assign(self_links_.size(), 0);
  }
  void RelaxDistanceConstraints(Frame frame) {
    const auto rebuild = [&]() {
      for (std::size_t i{}; i < nodes_.size(); ++i) {
        auto &node = nodes_[i];
        node.world = Compose(Parent(i, frame),
                             {node.offset, node.local, 1});
      }
    };
    const auto head_at = [&](const std::uint32_t index) {
      return bone_constraints::Vec{
          nodes_[index].world.position.x,
          nodes_[index].world.position.y,
          nodes_[index].world.position.z};
    };
    const auto tip_at = [&](const std::uint32_t index) {
      Node &node = nodes_[index];
      const Vec direction = Rotate(node.world.rotation, node.axis);
      const Vec tip =
          node.world.position +
          direction * (node.length * frame.scale);
      return bone_constraints::Vec{tip.x, tip.y, tip.z};
    };
    const auto apply_direction =
        [&](const std::uint32_t index,
            const bone_constraints::Vec wanted) {
          Node &node = nodes_[index];
          if (node.body_driven)
            return;
          const Frame parent = Parent(index, frame);
          node.direction = Unit(Vec{wanted.x, wanted.y, wanted.z});
          const Vec local_axis =
              Unit(Rotate(Inverse(parent.rotation), node.direction));
          node.local =
              Multiply(Swing(node.axis, local_axis),
                       Twist(node.rest, node.axis));
          if (node.swing_only ||
              node.material==secondary::MotionMaterial::Ribbon) {
            const Quat deviation =
                Multiply(node.local, Inverse(node.rest));
            const Quat roll =
                Twist(deviation, Rotate(node.rest, node.axis));
            node.local =
                Multiply(Multiply(deviation, Inverse(roll)), node.rest);
          }
          node.local = Normalize(node.local);
          node.velocity = node.velocity * 0.5;
        };
    rebuild();
    distance_corrections_ +=
        bone_constraints::Relax(distance_links_, 2, head_at, tip_at,
                                apply_direction, rebuild);
    collision_debug_.distance_links.clear();
    collision_debug_.distance_links.reserve(distance_links_.size());
    for (const bone_constraints::Link &link : distance_links_) {
      const auto a = tip_at(link.a);
      const auto b = tip_at(link.b);
      const double current = bone_constraints::Length(b - a);
      collision_debug_.distance_links.push_back(
          {{a.x, a.y, a.z}, {b.x, b.y, b.z}, link.rest, current,
           current < link.rest * 0.75 || current > link.rest * 1.25});
    }
  }
  void RelaxSelfCollisions(Frame frame) {
    const auto rebuild = [&]() {
      for (std::size_t i{}; i < nodes_.size(); ++i) {
        auto &node = nodes_[i];
        node.world = Compose(Parent(i, frame),
                             {node.offset, node.local, 1});
      }
    };
    const auto head_at = [&](const std::uint32_t index) {
      return bone_constraints::Vec{
          nodes_[index].world.position.x,
          nodes_[index].world.position.y,
          nodes_[index].world.position.z};
    };
    const auto tip_at = [&](const std::uint32_t index) {
      const Node &node = nodes_[index];
      const Vec tip =
          node.world.position +
          Rotate(node.world.rotation, node.axis) *
              (node.length * frame.scale);
      return bone_constraints::Vec{tip.x, tip.y, tip.z};
    };
    const auto apply_direction =
        [&](const std::uint32_t index,
            const bone_constraints::Vec wanted,
            const double velocity_keep) {
          Node &node = nodes_[index];
          if (node.body_driven)
            return;
          const Frame parent = Parent(index, frame);
          node.direction = Unit(Vec{wanted.x, wanted.y, wanted.z});
          const Vec local_axis =
              Unit(Rotate(Inverse(parent.rotation), node.direction));
          node.local =
              Multiply(Swing(node.axis, local_axis),
                       Twist(node.rest, node.axis));
          if (node.swing_only ||
              node.material==secondary::MotionMaterial::Ribbon) {
            const Quat deviation =
                Multiply(node.local, Inverse(node.rest));
            const Quat roll =
                Twist(deviation, Rotate(node.rest, node.axis));
            node.local =
                Multiply(Multiply(deviation, Inverse(roll)), node.rest);
          }
          node.local = Normalize(node.local);
          node.velocity = node.velocity * velocity_keep;
        };
    rebuild();
    const auto relative_speed_at =
        [&](const bone_constraints::Link &link) {
          const Node &a = nodes_[link.a];
          const Node &b = nodes_[link.b];
          const Vec tip_velocity_a =
              Cross(a.velocity, a.direction) *
              (a.length * frame.scale);
          const Vec tip_velocity_b =
              Cross(b.velocity, b.direction) *
              (b.length * frame.scale);
          return Length(tip_velocity_a - tip_velocity_b);
        };
    self_corrections_ +=
        bone_constraints::RelaxSelfCollision(
            self_links_, self_contact_state_, 2, head_at, tip_at,
            apply_direction, rebuild, relative_speed_at, frame.scale);
    rebuild();
    collision_debug_.self_contacts.clear();
    collision_debug_.self_contacts.reserve(self_links_.size());
    for (std::size_t index{}; index < self_links_.size(); ++index) {
      const auto &link = self_links_[index];
      if (link.depth == 0 && link.shared_anchor)
        continue;
      const auto a = head_at(link.a);
      const auto b = head_at(link.b);
      const auto a_end = tip_at(link.a);
      const auto b_end = tip_at(link.b);
      const auto closest =
          bone_constraints::ClosestSegmentPoints(a, a_end, b, b_end);
      const double scale =
          frame.scale > 1e-6 ? frame.scale : 1.0;
      const double safety = (std::max)(
          0.2 * scale,
          (std::min)(link.segment_rest * 0.75,
                     link.radius * scale + 0.2 * scale));
      collision_debug_.self_contacts.push_back(
          {closest.a, closest.b, closest.distance, safety,
           self_contact_state_[index] != 0,
           link.correction_share_a > 0.5,
           link.correction_share_b > 0.5});
    }
  }
  double InertiaGain(const Node& node) const {
    const double material =
        tuning_.material_inertia[secondary::MotionMaterialIndex(node.material)];
    return tuning_.inertia * material *
           (secondary::IsBreast(node.name) ? tuning_.breast_inertia : 1.0);
  }
  double CollisionGain(const Node& node) const {
    return tuning_.collision *
           tuning_.material_collision[
               secondary::MotionMaterialIndex(node.material)];
  }
  Frame Parent(std::size_t i,Frame frame) const {
    return parents_[i]>=0 ? nodes_[static_cast<std::size_t>(parents_[i])].world : frame;
  }
  void Seed(Frame frame,double time,std::uint32_t serial) {
    for (std::size_t i=0;i<nodes_.size();++i) {
      auto& n=nodes_[i];
      n.offset=n.rest_offset;
      n.local=n.rest; n.world=Compose(Parent(i,frame),{n.offset,n.local,1});
      n.direction=Rotate(n.world.rotation,n.axis); n.velocity={};
      n.filtered_accel={}; n.filtered_target=n.direction;
      n.filtered_attachment={}; n.filtered_attachment_ready=false;
      n.head=n.world.position; n.head_velocity={}; n.samples=1; n.dt=0;
      n.held=false; n.body_driven=false;
      n.collision_normal_ready=false;
    }
    previous_=frame; time_=time; serial_=serial; seeded_=true; moving_=false;
    pose_=bind_; length_error_=0;
    frame_velocity_={}; frame_angular_velocity_={};
  }
  void ApplySeedAttachments(
      Frame frame,std::span<const AttachmentDrive> attachments) {
    for (std::size_t i=0;i<nodes_.size();++i) {
      auto& n=nodes_[i];
      const Frame parent=Parent(i,frame);
      const AttachmentDrive* attachment=nullptr;
      for (const auto& candidate:attachments) {
        if (candidate.bone==i) {
          attachment=&candidate;
          break;
        }
      }
      if (attachment==nullptr) continue;
      const Bone driven=BlendBone(bind_[i],attachment->local,attachment->weight);
      if (!ValidBone(driven)) continue;
      const Frame wanted=Compose(frame,Transform(driven));
      const double parent_scale=parent.scale>1e-9 ? parent.scale : 1.0;
      n.offset=Rotate(Inverse(parent.rotation),wanted.position-parent.position)*
               (1.0/parent_scale);
      n.local=Normalize(Multiply(Inverse(parent.rotation),wanted.rotation));
      n.world=Compose(parent,{n.offset,n.local,1});
      n.direction=Rotate(n.world.rotation,n.axis);
      n.head=n.world.position;
      n.body_driven=true;
    }
  }
  static Vec AngularVelocity(Quat from, Quat to, double dt) {
    Quat delta=Normalize(Multiply(to,Inverse(from)));
    if (delta.w<0.0) delta={-delta.x,-delta.y,-delta.z,-delta.w};
    const Vec xyz{delta.x,delta.y,delta.z};
    const double length=Length(xyz);
    if (length<=1e-9 || !(dt>1e-9)) return {};
    const double angle=2.0*std::atan2(length,delta.w);
    return xyz*(angle/(length*dt));
  }
  void UpdateFrameMotion(Frame frame,double dt) {
    frame_velocity_=(frame.position-previous_.position)*(1.0/dt);
    frame_angular_velocity_=
        AngularVelocity(previous_.rotation,frame.rotation,dt);
  }
  void Collide(Node& n,Frame parent,Frame frame,
               std::span<const Capsule> capsules,
               double& correction_budget) {
    double slack=(std::numeric_limits<double>::infinity)();
    for (const auto& capsule:capsules)
      if ((capsule.mask & n.mask)!=0)
        slack=(std::min)(slack,secondary::CollisionSlackCm(capsule.mask));
    slack*=frame.scale;
    const double threshold=slack;
    const Vec head=n.world.position;
    const Vec direction=Rotate(n.world.rotation,n.axis);
    const auto measure=[&](Vec candidate) {
      return ProbeCollision(n,parent,frame,head,candidate,capsules);
    };
    Vec slid=direction,best=direction;
    const CollisionProbe initial_probe=measure(direction);
    const double initial=initial_probe.depth;
    // Hysteresis with separated engage/release edges prevents boundary chatter.
    const double engage=slack*(n.draped ? 2.0 : 1.5);
    const double release=slack*(n.draped ? 0.75 : 0.5);
    if (initial<=release) {
      n.held=false;
      n.collision_normal_ready=false;
      n.direction=direction;
      return;
    }
    if (n.held && initial<=engage) {
      n.collision_normal=Rotate(
          Inverse(parent.rotation),initial_probe.normal);
      n.collision_capsule=initial_probe.capsule;
      n.collision_normal_ready=true;
      n.direction=direction;
      return;
    }
    double best_depth=initial;
    CollisionProbe best_probe=initial_probe;
    for (int attempt=0;attempt<12;++attempt) {
      const CollisionProbe contact=measure(slid);
      if (contact.depth<best_depth) {
        best_depth=contact.depth;
        best=slid;
        best_probe=contact;
      }
      if (contact.depth<=threshold) break;
      const Vec aim=Unit(contact.surface-head);
      if (Length(aim)<.5) break;
      slid=Unit(slid+(aim-slid)*.6);
    }
    n.held=best_depth<initial;
    if (n.held) {
      n.collision_normal=Rotate(
          Inverse(parent.rotation),best_probe.normal);
      n.collision_capsule=best_probe.capsule;
      n.collision_normal_ready=true;
      const double available_correction =
          (std::max)(0.0,correction_budget);
      const double kMaxCorrectionDegrees=
          (std::min)((n.draped ? 1.5 : 3.0)*CollisionGain(n),
                     available_correction);
      if (kMaxCorrectionDegrees<=1e-6) {
        n.direction=direction;
        return;
      }
      const Vec local_direction=Unit(Rotate(Inverse(parent.rotation),direction));
      Vec local_best=Unit(Rotate(Inverse(parent.rotation),best));
      const double sweep=std::acos(std::clamp(Dot(local_direction,local_best),-1.0,1.0))*
                         180.0/3.14159265358979323846;
      const double applied_correction=
          (std::min)(sweep,kMaxCorrectionDegrees);
      if (sweep>kMaxCorrectionDegrees)
        local_best=Unit(local_direction+
                        (local_best-local_direction)*(kMaxCorrectionDegrees/sweep));
      const Quat correction=Swing(local_direction,local_best);
      n.local=Normalize(Multiply(correction,n.local));
      n.world.rotation=Normalize(Multiply(parent.rotation,n.local));
      const Vec correction_axis=Rotate(
          parent.rotation,Unit({correction.x,correction.y,correction.z}));
      const double along=Dot(n.velocity,correction_axis);
      if (along<0) n.velocity=n.velocity-correction_axis*along;
      correction_budget=
          (std::max)(0.0,correction_budget-applied_correction);
      ++corrections_;
    }
    n.direction=Rotate(n.world.rotation,n.axis);
  }
  void Finish(Frame frame,std::span<const Capsule> capsules,double dt=0) {
    pose_=bind_; length_error_=0;
    if (collision_frame_budgets_.size()!=nodes_.size()) {
      collision_frame_budgets_.assign(nodes_.size(),0.0);
      collision_frame_budget_ready_.assign(nodes_.size(),0);
    } else {
      std::fill(collision_frame_budgets_.begin(),
                collision_frame_budgets_.end(),0.0);
      std::fill(collision_frame_budget_ready_.begin(),
                collision_frame_budget_ready_.end(),std::uint8_t{0});
    }
    // Recompute every descendant, including non-simulated leaves and ornaments,
    // after a parent correction. Only kinematic attachment offsets change; child
    // offsets stay authored, so the chains do not stretch.
    for (std::size_t i=0;i<nodes_.size();++i) {
      auto& n=nodes_[i]; const Frame parent=Parent(i,frame);
      n.world=Compose(parent,{n.offset,n.local,1});
      if (n.active && !n.body_driven && n.mask && !capsules.empty() &&
          CollisionGain(n)>1e-6) {
        const std::int32_t chain=collision_chains_[i];
        if (chain>=0) {
          const auto root=static_cast<std::size_t>(chain);
          if (collision_frame_budget_ready_[root]==0) {
            const double base_budget =
                n.material==secondary::MotionMaterial::Hair
                    ? 1.0
                    : (n.draped ? 0.75 : 1.5);
            collision_frame_budgets_[root]=
                base_budget*CollisionGain(n);
            collision_frame_budget_ready_[root]=1;
          }
          Collide(n,parent,frame,capsules,
                  collision_frame_budgets_[root]);
        }
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
  std::vector<std::int32_t> collision_chains_;
  std::vector<double> collision_frame_budgets_;
  std::vector<std::uint8_t> collision_frame_budget_ready_;
  std::vector<bone_constraints::Candidate> distance_candidates_;
  std::vector<bone_constraints::Link> distance_links_;
  std::vector<bone_constraints::Link> self_links_;
  std::vector<std::uint8_t> self_contact_state_;
  std::vector<BodyVolume> body_volumes_;
  Frame previous_{};
  std::size_t driven_{};
  double time_{},length_error_{};
  std::size_t corrections_{};
  std::size_t distance_corrections_{};
  std::size_t self_corrections_{};
  std::uint32_t serial_{};
  Vec frame_velocity_{},frame_angular_velocity_{};
  double air_scale_{};
  secondary::DynamicsTuning tuning_{};
  bool seeded_{},moving_{},collisions_configured_{};
  bool air_enabled_{};
  bool self_collision_tuning_initialized_{};
  CollisionDebugSnapshot collision_debug_;
};
} // namespace better_pose::accessory
