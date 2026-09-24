#pragma once

#include <cstdint>
#include <string_view>

namespace camera_blur_fix_profile {

// The pattern resolves the validated UE5 GWorld pointer used by the NTE
// profile. The plugin only consumes the pointer and the profile's camera
// manager layout; it does not include generated game SDK types.
inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr std::uint32_t kGWorldDisplacementOffset = 3;
inline constexpr std::uint32_t kGWorldInstructionSize = 7;

inline constexpr std::uint32_t kWorldGameInstanceOffset = 0x230;
inline constexpr std::uint32_t kGameInstanceLocalPlayersOffset = 0x38;
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;
inline constexpr std::uint32_t kControllerCameraManagerOffset = 0x380;
// AHTPlayerCameraManager::PlayerFade* fields from the active HT profile.
// These are the data path used by the camera pitch fade, not ProcessEvent
// candidates. Keep the offsets profile-local and validate them at runtime.
inline constexpr std::uint32_t kPlayerFadeSpeedOffset = 0x3B50;
inline constexpr std::uint32_t kPlayerFadeDistanceSquareOffset = 0x3B54;
inline constexpr std::uint32_t kPlayerHideDistanceSquareOffset = 0x3B58;
inline constexpr std::uint32_t kPlayerPitchFadeCurveOffset = 0x3B60;
// AHTPlayerCameraManager::NormalCameraSettings is an inline FCameraSettings
// value.  The two FTargetableFloat TargetValue members are the source values
// copied into PlayerFadeDistance/PlayerHideDistance during camera update.
inline constexpr std::uint32_t kNormalCameraSettingsOffset = 0xBAC8;
inline constexpr std::uint32_t kNormalPlayerFadeDistanceTargetOffset = 0x55C;
inline constexpr std::uint32_t kNormalPlayerHideDistanceTargetOffset = 0x564;
} // namespace camera_blur_fix_profile
