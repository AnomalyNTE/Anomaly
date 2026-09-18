#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

// Shared by the offline body converter and live modular accessory solver.
namespace better_pose::secondary {
inline constexpr double kGravity = 981.0;
inline constexpr double kStiffness = 200.0;
inline constexpr double kDamping = 16.0;
inline constexpr double kGravityWeight = 0.15;
inline constexpr double kSleeveGravityWeight = 0.45;
inline constexpr double kLagDegrees = 25.0;
inline constexpr double kTorsoCollisionRadiusCm = 8.0;
inline constexpr double kTorsoCollisionSlackCm = 2.0;
inline constexpr double kLegCollisionSlackCm = 0.5;
inline constexpr std::uint8_t kCollideTorso = 1;
inline constexpr std::uint8_t kCollideLegs = 2;

struct SpringSettings { double stiffness, damping, lag_degrees; };
inline constexpr SpringSettings SpringFor(bool hair) {
  // Softer hair springs increase motion lag without changing the resting drape.
  return hair ? SpringSettings{120.0, 12.0, 35.0}
              : SpringSettings{kStiffness, kDamping, kLagDegrees};
}

inline std::string Lower(std::string_view name) {
  std::string low(name);
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return low;
}
inline bool IsSecondary(std::string_view name) {
  const auto low = Lower(name);
  for (const char* key : {"twist", "finger", "ik", "tail"})
    if (low.find(key) != std::string::npos) return false;
  if (name.rfind("Bn_", 0) == 0) return true;
  for (const char* key : {"hair", "qun", "cloth", "piao", "gongpai", "lalian",
                           "xiong", "tie", "skirt", "ribbon"})
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
  const auto low = Lower(name);
  return low.find("xiu") != std::string::npos && low.find("qun") == std::string::npos;
}
inline bool IsHair(std::string_view name) {
  return Lower(name).find("hair") != std::string::npos;
}
inline bool IsLegAttachment(std::string_view name) {
  return name.find("Pelvis") != std::string::npos || name.find("Thigh") != std::string::npos;
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
