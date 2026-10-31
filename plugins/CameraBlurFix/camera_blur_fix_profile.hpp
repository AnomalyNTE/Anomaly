#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace camera_blur_fix_profile {

// Character camera fade, all of it: the game writes the "OpacityMaskVal" mask
// that decides how transparent an actor is drawn through two native setters.
// Forcing the mask argument to 1.0 inside those two functions removes the fade
// for the view target, NPCs, monsters and the crowd at once, and costs nothing
// per frame.  Both functions are located through reflection, so no actor scan
// and no offset hunting happens here.
inline constexpr std::string_view kMaskSetterPath =
    "/Script/HTGame.HTAbilityCharacter.UpdateOpacityMaskVal";
inline constexpr std::string_view kFadeSetterPath =
    "/Script/HTGame.HTFadeComponent.UpdateFade";
inline constexpr std::uint32_t kMaskSetterNumParams = 1;
inline constexpr std::uint16_t kMaskSetterParamBytes = 4;
inline constexpr std::uint32_t kFadeSetterNumParams = 2;
inline constexpr std::uint16_t kFadeSetterParamBytes = 8;
inline constexpr float kVisibleMask = 1.0F;

// UE5 reflection layout: UFunction::Func and the {numParms, parmsSize} pair.
inline constexpr std::uint32_t kUFunctionExecOffset = 0xD8;
inline constexpr std::uint32_t kUFunctionNumParmsOffset = 0xB4;
inline constexpr std::uint32_t kUFunctionParmsSizeOffset = 0xB6;
// A UHT exec thunk loads `this` and then calls the native implementation, so
// the last "mov rcx, rdi; call rel32" in the thunk is the function to hook.
inline constexpr std::uint8_t kNativeCallMarker[] = {0x48, 0x8B, 0xCF, 0xE8};
inline constexpr std::uint32_t kThunkScanBytes = 0xC0;

// GObjects registry, used to turn an objects-service handle into an address.
inline constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;
inline constexpr std::ptrdiff_t kGObjectsAddend = -16;
inline constexpr std::uint32_t kObjectChunksOffset = 16;
inline constexpr std::uint32_t kObjectCountOffset = 36;
inline constexpr std::uint32_t kObjectChunkBits = 16;
inline constexpr std::uint32_t kObjectItemStride = 24;
inline constexpr std::uint32_t kObjectItemPointerOffset = 0;
inline constexpr std::uint32_t kObjectItemSerialOffset = 16;
inline constexpr std::uint32_t kMaximumObjectCount = 16U * 1024U * 1024U;

// Hook installation is retried at this interval while a target is missing,
// which is the only work the plugin does once its hooks are live.
inline constexpr double kRetrySeconds = 5.0;
inline constexpr std::size_t kMaximumPathBytes = 128;

} // namespace camera_blur_fix_profile
