#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

// Shared by the offline body converter and live modular accessory solver.
namespace better_pose::secondary {
inline constexpr double kGravity = 981.0;
inline constexpr double kStiffness = 200.0;
inline constexpr double kDamping = 16.0;
inline constexpr double kGravityWeight = 0.15;
inline constexpr double kSleeveGravityWeight = 0.45;
inline constexpr double kDrapedGravityWeight = 0.25;
inline constexpr double kMovingAirResponse = 1.25;
inline constexpr double kLagDegrees = 25.0;
inline constexpr double kTorsoCollisionRadiusCm = 8.0;
inline constexpr double kChestCollisionRadiusCm = 7.0;
inline constexpr double kNeckCollisionRadiusCm = 3.5;
inline constexpr double kNeckTopCollisionRadiusCm = 6.5;
inline constexpr double kHeadCollisionRadiusCm = 7.0;
inline constexpr double kShoulderCollisionRadiusCm = 5.0;
inline constexpr double kArmCollisionRadiusCm = 4.5;
inline constexpr double kHandCollisionRadiusCm = 3.5;
inline constexpr double kLegCollisionRadiusCm = 6.0;
inline constexpr double kTorsoCollisionSlackCm = 2.0;
inline constexpr double kHeadCollisionSlackCm = 1.0;
inline constexpr double kArmCollisionSlackCm = 0.75;
inline constexpr double kLegCollisionSlackCm = 0.5;
inline constexpr std::uint8_t kCollideTorso = 1;
inline constexpr std::uint8_t kCollideLegs = 2;
inline constexpr std::uint8_t kCollideHead = 4;
inline constexpr std::uint8_t kCollideArms = 8;
// KawaiiPhysics calls these CapsuleLimits. They constrain the native spring
// chain; they are not body-collision capsules.
inline constexpr std::uint8_t kKawaiiLimit = 16;

enum class MotionMaterial {
  General,
  Cloth,
  Hair,
  Ribbon,
  Belt,
  Accessory,
  Sleeve
};
inline constexpr std::size_t kMotionMaterialCount = 7;
inline constexpr std::array<std::string_view, kMotionMaterialCount>
    kMotionMaterialKeys{"general", "cloth", "hair", "ribbon",
                        "belt", "accessory", "sleeve"};
inline constexpr float kDynamicsInertiaMax = 10.0F;
inline constexpr float kDynamicsAirMax = 10.0F;
inline constexpr float kDynamicsDampingMin = 0.1F;
inline constexpr float kDynamicsDampingMax = 10.0F;
inline constexpr float kDynamicsGravityMax = 5.0F;
inline constexpr float kDynamicsCollisionMax = 5.0F;
inline constexpr float kDynamicsSelfRadiusMax = 5.0F;
inline constexpr float kDynamicsSelfStrengthMin = 0.1F;
inline constexpr float kDynamicsSelfStrengthMax = 10.0F;
inline constexpr float kDynamicsSelfDampingMin = 0.1F;
inline constexpr float kDynamicsSelfDampingMax = 10.0F;

inline constexpr std::size_t SelfCollisionPairIndex(
    const MotionMaterial first, const MotionMaterial second) {
  std::size_t a = static_cast<std::size_t>(first);
  std::size_t b = static_cast<std::size_t>(second);
  if (a > b)
    std::swap(a, b);
  return a * kMotionMaterialCount + b;
}

inline constexpr bool DefaultSelfCollisionPair(
    const MotionMaterial first, const MotionMaterial second) {
  const auto a = static_cast<int>(first);
  const auto b = static_cast<int>(second);
  if (a == b)
    return true;
  const auto pair = std::minmax(a, b);
  const int low = pair.first;
  const int high = pair.second;
  return (low == static_cast<int>(MotionMaterial::Hair) &&
          (high == static_cast<int>(MotionMaterial::Ribbon) ||
           high == static_cast<int>(MotionMaterial::Cloth) ||
           high == static_cast<int>(MotionMaterial::Sleeve))) ||
         (low == static_cast<int>(MotionMaterial::Ribbon) &&
          high == static_cast<int>(MotionMaterial::Cloth)) ||
         (low == static_cast<int>(MotionMaterial::Cloth) &&
          high == static_cast<int>(MotionMaterial::Sleeve)) ||
         (low == static_cast<int>(MotionMaterial::Belt) &&
          high == static_cast<int>(MotionMaterial::Cloth));
}

inline constexpr std::uint64_t DefaultSelfCollisionPairs() {
  std::uint64_t result{};
  for (int a{}; a <= static_cast<int>(MotionMaterial::Sleeve); ++a)
    for (int b = a; b <= static_cast<int>(MotionMaterial::Sleeve); ++b)
      if (DefaultSelfCollisionPair(static_cast<MotionMaterial>(a),
                                   static_cast<MotionMaterial>(b)))
        result |= std::uint64_t{1}
                  << SelfCollisionPairIndex(static_cast<MotionMaterial>(a),
                                            static_cast<MotionMaterial>(b));
  return result;
}

inline bool SelfCollisionPairEnabled(const std::uint64_t pairs,
                                     const MotionMaterial first,
                                     const MotionMaterial second) {
  return (pairs & (std::uint64_t{1} << SelfCollisionPairIndex(first, second))) !=
         0;
}

struct MotionResponse {
  double inertia;
  double angular;
  double air;
};

struct DynamicsTuning {
  float inertia{1.0F};
  float air{0.0F};
  float damping{1.0F};
  float ribbon_damping{1.0F};
  float gravity{1.0F};
  float collision{1.0F};
  float breast_inertia{1.0F};
  float breast_stiffness{1.0F};
  float breast_damping{1.0F};
  float breast_gravity{1.0F};
  float breast_amplitude{1.0F};
  // The v2.3.0 release used only torso and leg volumes. Keep that range as
  // the default; head/arm collision and native Kawaii spring limits are
  // opt-in because they materially change the constraint envelope.
  bool extended_collision{false};
  bool native_limits{false};
  bool bone_constraints{false};
  bool self_collision{false};
  bool self_collision_outer_priority{false};
  std::uint64_t self_collision_pairs{DefaultSelfCollisionPairs()};
  std::array<float, kMotionMaterialCount> self_collision_radius{
      1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, kMotionMaterialCount> self_collision_strength{
      1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, kMotionMaterialCount> self_collision_damping{
      1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, kMotionMaterialCount> material_inertia{
      1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, kMotionMaterialCount> material_collision{
      1.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F, 1.0F};
};

inline std::size_t MotionMaterialIndex(MotionMaterial material) {
  return static_cast<std::size_t>(material);
}
inline DynamicsTuning SafeDynamicsTuning(DynamicsTuning tuning) {
  const auto finite = [](float value, float fallback, float minimum,
                         float maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum)
                                : fallback;
  };
  tuning.inertia =
      finite(tuning.inertia, 1.0F, 0.0F, kDynamicsInertiaMax);
  tuning.air = finite(tuning.air, 0.0F, 0.0F, kDynamicsAirMax);
  tuning.damping =
      finite(tuning.damping, 1.0F, kDynamicsDampingMin, kDynamicsDampingMax);
  tuning.ribbon_damping =
      finite(tuning.ribbon_damping, 1.0F, kDynamicsDampingMin,
             kDynamicsDampingMax);
  tuning.gravity =
      finite(tuning.gravity, 1.0F, 0.0F, kDynamicsGravityMax);
  tuning.collision =
      finite(tuning.collision, 1.0F, 0.0F, kDynamicsCollisionMax);
  tuning.breast_inertia =
      finite(tuning.breast_inertia, 1.0F, 0.0F, kDynamicsInertiaMax);
  tuning.breast_stiffness =
      finite(tuning.breast_stiffness, 1.0F, 0.1F, kDynamicsDampingMax);
  tuning.breast_damping =
      finite(tuning.breast_damping, 1.0F, kDynamicsDampingMin,
             kDynamicsDampingMax);
  tuning.breast_gravity =
      finite(tuning.breast_gravity, 1.0F, 0.0F, kDynamicsGravityMax);
  tuning.breast_amplitude =
      finite(tuning.breast_amplitude, 1.0F, 0.0F, 5.0F);
  for (float& gain : tuning.material_inertia)
    gain = finite(gain, 1.0F, 0.0F, kDynamicsInertiaMax);
  for (float& gain : tuning.material_collision)
    gain = finite(gain, 1.0F, 0.0F, kDynamicsCollisionMax);
  for (float& gain : tuning.self_collision_radius)
    gain = finite(gain, 1.0F, 0.0F, kDynamicsSelfRadiusMax);
  for (float& gain : tuning.self_collision_strength)
    gain = finite(gain, 1.0F, kDynamicsSelfStrengthMin,
                  kDynamicsSelfStrengthMax);
  for (float& gain : tuning.self_collision_damping)
    gain = finite(gain, 1.0F, kDynamicsSelfDampingMin,
                  kDynamicsSelfDampingMax);
  return tuning;
}

inline double CollisionSlackCm(const std::uint8_t mask) {
  double slack = (std::numeric_limits<double>::infinity)();
  if ((mask & kCollideTorso) != 0) slack = (std::min)(slack, kTorsoCollisionSlackCm);
  if ((mask & kCollideLegs) != 0) slack = (std::min)(slack, kLegCollisionSlackCm);
  if ((mask & kCollideHead) != 0) slack = (std::min)(slack, kHeadCollisionSlackCm);
  if ((mask & kCollideArms) != 0) slack = (std::min)(slack, kArmCollisionSlackCm);
  if ((mask & kKawaiiLimit) != 0) slack = (std::min)(slack, 1.0);
  return std::isfinite(slack) ? slack : 0.0;
}

inline constexpr std::uint8_t kCompatibilityCollisionMask =
    kCollideTorso | kCollideLegs;

inline std::uint8_t EffectiveCollisionMask(const std::uint8_t mask,
                                           const bool extended_collision,
                                           const bool native_limits = false) {
  std::uint8_t result =
      extended_collision
          ? mask
          : static_cast<std::uint8_t>(mask & kCompatibilityCollisionMask);
  result = static_cast<std::uint8_t>(result & ~kKawaiiLimit);
  if (native_limits)
    result = static_cast<std::uint8_t>(result | (mask & kKawaiiLimit));
  return result;
}

struct SpringSettings { double stiffness, damping, lag_degrees; };
inline constexpr SpringSettings SpringFor(bool hair) {
  // Softer hair springs increase motion lag without changing the resting drape.
  return hair ? SpringSettings{120.0, 12.0, 35.0}
              : SpringSettings{kStiffness, kDamping, kLagDegrees};
}

struct KawaiiPhysicsSettings {
  double damping{};
  double stiffness{};
  double world_damping_location{};
  double world_damping_rotation{};
  double radius{};
  double limit_angle{};
  double forward_move_offset{};
};

// KawaiiPhysics stores per-step factors in [0, 1], while this solver integrates
// an angular spring in seconds. Preserve the authored relative feel by scaling
// around the common 0.18/0.05 values instead of copying those factors into a
// different equation. The native limit becomes this solver's hard lag angle.
inline SpringSettings SpringFromKawaii(const KawaiiPhysicsSettings& native,
                                       bool hair, bool draped_ribbon = false) {
  const auto base = SpringFor(hair);
  const double stiffness_scale =
      std::clamp(native.stiffness / 0.05, 0.25, 4.0);
  const double damping_scale =
      std::clamp(native.damping / 0.18, 0.25, 4.0);
  const double lag = native.limit_angle > 1e-4
                         ? std::clamp(native.limit_angle, 5.0, 75.0)
                         : base.lag_degrees;
  SpringSettings result{base.stiffness * stiffness_scale,
                        base.damping * damping_scale, lag};
  if (draped_ribbon) {
    result.stiffness *= 0.8;
    result.damping *= 1.25;
  }
  return result;
}

inline std::string Lower(std::string_view name) {
  std::string low(name);
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return low;
}
inline MotionMaterial ClassifyMotionMaterial(std::string_view name) {
  const auto low = Lower(name);
  // Hair wins over "tail" so a ponytail is not treated as a rigid animal tail.
  for (const char* key : {"hair", "hairline", "hari", "kami", "bang"})
    if (low.find(key) != std::string::npos) return MotionMaterial::Hair;
  if (low.find("xiu") != std::string::npos &&
      low.find("qun") == std::string::npos)
    return MotionMaterial::Sleeve;
  for (const char* key : {"ribbon", "piaodai", "robbin", "_robin",
                           "scarf", "streamer"})
    if (low.find(key) != std::string::npos) return MotionMaterial::Ribbon;
  for (const char* key : {"belt", "strap", "rope", "tie", "chain", "thread",
                           "lalian"})
    if (low.find(key) != std::string::npos) return MotionMaterial::Belt;
  for (const char* key : {"bag", "lantern", "pendant", "props", "acc_", "gongpai",
                           "wing", "armband", "earring", "ornament", "decorate",
                           "_chunk", "_hoop", "_card"})
    if (low.find(key) != std::string::npos) return MotionMaterial::Accessory;
  for (const char* key : {"qun", "cloth", "skirt", "apron", "coat", "cloak",
                           "cape", "dress", "pants", "b_l_cb", "b_r_cb"})
    if (low.find(key) != std::string::npos) return MotionMaterial::Cloth;
  return MotionMaterial::General;
}
inline MotionResponse ResponseFor(MotionMaterial material) {
  switch (material) {
    case MotionMaterial::Hair:
      return {1.00, 1.00, 1.00};
    case MotionMaterial::Ribbon:
      return {0.68, 0.65, 0.45};
    case MotionMaterial::Belt:
      return {0.55, 0.50, 0.25};
    case MotionMaterial::Accessory:
      return {0.40, 0.35, 0.10};
    case MotionMaterial::Sleeve:
      return {0.75, 0.70, 0.40};
    case MotionMaterial::Cloth:
      return {0.78, 0.75, 0.45};
    case MotionMaterial::General:
      return {0.90, 0.85, 0.50};
  }
  return {0.90, 0.85, 0.50};
}
inline double NormalizedChainResponse(const double response,
                                      const std::size_t chain_length) {
  const double value = std::clamp(response, 0.0, 1.0);
  if (value <= 0.0 || chain_length <= 1)
    return value;
  if (value >= 1.0)
    return 1.0;
  return 1.0 -
         std::exp(std::log1p(-value) / static_cast<double>(chain_length));
}
inline std::uint32_t SelfCollisionMask(
    const MotionMaterial material,
    const std::uint64_t pairs = DefaultSelfCollisionPairs()) {
  std::uint32_t result{};
  for (std::size_t index{}; index != kMotionMaterialCount; ++index) {
    const auto other = static_cast<MotionMaterial>(index);
    if (SelfCollisionPairEnabled(pairs, material, other))
      result |= std::uint32_t{1} << index;
  }
  return result;
}
inline bool IsSecondary(std::string_view name) {
  const auto low = Lower(name);
  for (const char* key : {"twist", "finger", "ik", "tail"})
    if (low.find(key) != std::string::npos) return false;
  if (name.rfind("Bn_", 0) == 0) return true;
  for (const char* key : {"hair", "qun", "cloth", "piao", "gongpai", "lalian",
                           "xiong", "breast", "bust", "boob", "tie", "skirt",
                           "ribbon", "b_l_cb", "b_r_cb"})
    if (low.find(key) != std::string::npos) return true;
  return false;
}
inline bool IsBreast(std::string_view name) {
  const auto low = Lower(name);
  for (const char* key : {"breast", "bust", "boob", "xiong"})
    if (low.find(key) != std::string::npos) return true;
  return false;
}
inline bool IsOrnament(std::string_view name) {
  const auto low = Lower(name);
  for (const char* key : {"bn_m_headprop", "bn_m_headproa", "bn_m_headprob",
                           "bn_m_headlinea", "bn_m_headlineb", "hat"})
    if (low.find(key) != std::string::npos) return true;
  return false;
}
inline bool IsSwingOnly(std::string_view name) {
  const auto low = Lower(name);
  for (const char* key : {"hair", "hairline", "hari", "kami"})
    if (low.find(key) != std::string::npos) return true;
  return false;
}
inline bool IsSleeve(std::string_view name) {
  return ClassifyMotionMaterial(name) == MotionMaterial::Sleeve;
}
inline bool IsDrapedRibbon(std::string_view name) {
  return ClassifyMotionMaterial(name) == MotionMaterial::Ribbon;
}
inline bool IsHair(std::string_view name) {
  return ClassifyMotionMaterial(name) == MotionMaterial::Hair;
}
inline bool IsLegAttachment(std::string_view name) {
  return name.find("Pelvis") != std::string::npos || name.find("Thigh") != std::string::npos;
}
inline bool IsHeadAttachment(std::string_view name) {
  const auto low = Lower(name);
  return low.find("head") != std::string::npos || low.find("neck") != std::string::npos ||
         low.find("hair") != std::string::npos;
}
inline bool IsArmAttachment(std::string_view name) {
  const auto low = Lower(name);
  return low.find("arm") != std::string::npos ||
         low.find("clavicle") != std::string::npos ||
         low.find("hand") != std::string::npos;
}
struct LegVolume {
  const char* first;
  const char* second;
  double fraction;
  const char* key;
};
inline constexpr LegVolume kLegVolumes[] = {
    {"Bip001-L-Thigh", "Bip001-L-Calf", .33, "Bip001-L-Thigh->Bip001-L-Calf"},
    {"Bip001-L-Calf", "Bip001-L-Foot", 0, "Bip001-L-Calf->Bip001-L-Foot"},
    {"Bip001-R-Thigh", "Bip001-R-Calf", .33, "Bip001-R-Thigh->Bip001-R-Calf"},
    {"Bip001-R-Calf", "Bip001-R-Foot", 0, "Bip001-R-Calf->Bip001-R-Foot"}};
} // namespace better_pose::secondary
