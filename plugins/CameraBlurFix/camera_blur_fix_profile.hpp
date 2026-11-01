#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace camera_blur_fix_profile {

// Keep the NPC/crowd mask setters from ea22766. The local player's independent
// distance-hide and pitch-fade settings also need disabling; a material mask
// alone cannot prevent the camera manager from hiding that player.
inline constexpr std::string_view kMaskSetterPath =
    "/Script/HTGame.HTAbilityCharacter.UpdateOpacityMaskVal";
inline constexpr std::string_view kFadeSetterPath =
    "/Script/HTGame.HTFadeComponent.UpdateFade";
inline constexpr std::uint32_t kMaskSetterNumParams = 1;
inline constexpr std::uint16_t kMaskSetterParamBytes = 4;
inline constexpr std::uint32_t kFadeSetterNumParams = 2;
inline constexpr std::uint16_t kFadeSetterParamBytes = 8;
inline constexpr float kVisibleMask = 1.0F;

// Same validated GWorld signature as the active NTE profile. A world-service
// handle identifies a lifecycle generation, not a GObjects slot.
inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";

// Existing UE5 reflection metadata layout from the active profile. Game-specific
// camera offsets are resolved by property name/type, not copied from the dump.
inline constexpr std::uint32_t kObjectInternalIndexOffset = 0xC;
inline constexpr std::uint32_t kObjectClassOffset = 0x10;
inline constexpr std::uint32_t kUStructSuperOffset = 0x40;
inline constexpr std::uint32_t kUStructPropertyLinkOffset = 0x70;
inline constexpr std::uint32_t kFFieldClassOffset = 0x8;
inline constexpr std::uint32_t kFFieldNextOffset = 0x18;
inline constexpr std::uint32_t kFFieldNameOffset = 0x20;
inline constexpr std::uint32_t kFPropertyElementSizeOffset = 0x34;
inline constexpr std::uint32_t kFPropertyOffsetOffset = 0x44;

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

// Retry missing hook targets or camera reflection metadata at a slow interval.
inline constexpr double kRetrySeconds = 5.0;
inline constexpr std::size_t kMaximumPathBytes = 128;

} // namespace camera_blur_fix_profile
