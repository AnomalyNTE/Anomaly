#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace camera_blur_fix_profile {
struct Layout {
    std::uint32_t kObjectInternalIndexOffset{};
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kUStructSuperOffset{};
    std::uint32_t kUStructPropertyLinkOffset{};
    std::uint32_t kFFieldClassOffset{};
    std::uint32_t kFFieldNextOffset{};
    std::uint32_t kFFieldNameOffset{};
    std::uint32_t kFPropertyElementSizeOffset{};
    std::uint32_t kFPropertyOffsetOffset{};
    std::uint32_t kUFunctionExecOffset{};
    std::uint32_t kUFunctionNumParmsOffset{};
    std::uint32_t kUFunctionParmsSizeOffset{};
    std::ptrdiff_t kGObjectsAddend{};
    std::uint32_t kObjectChunksOffset{};
    std::uint32_t kObjectCountOffset{};
    std::uint32_t kObjectChunkBits{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kObjectItemPointerOffset{};
    std::uint32_t kObjectItemSerialOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "camera-blur-fix-v1"})) return false;
        Layout parsed;
        parsed.kObjectInternalIndexOffset = compatibility.Value("kObjectInternalIndexOffset", 16384);
        parsed.kObjectClassOffset = compatibility.Value("kObjectClassOffset", 16384);
        parsed.kUStructSuperOffset = compatibility.Value("kUStructSuperOffset", 16384);
        parsed.kUStructPropertyLinkOffset = compatibility.Value("kUStructPropertyLinkOffset", 16384);
        parsed.kFFieldClassOffset = compatibility.Value("kFFieldClassOffset", 16384);
        parsed.kFFieldNextOffset = compatibility.Value("kFFieldNextOffset", 16384);
        parsed.kFFieldNameOffset = compatibility.Value("kFFieldNameOffset", 16384);
        parsed.kFPropertyElementSizeOffset = compatibility.Value("kFPropertyElementSizeOffset", 16384);
        parsed.kFPropertyOffsetOffset = compatibility.Value("kFPropertyOffsetOffset", 16384);
        parsed.kUFunctionExecOffset = compatibility.Value("kUFunctionExecOffset", 16384);
        parsed.kUFunctionNumParmsOffset = compatibility.Value("kUFunctionNumParmsOffset", 16384);
        parsed.kUFunctionParmsSizeOffset = compatibility.Value("kUFunctionParmsSizeOffset", 16384);
        parsed.kGObjectsAddend = -static_cast<std::ptrdiff_t>(compatibility.Value("kGObjectsAddendMagnitude", 16384));
        parsed.kObjectChunksOffset = compatibility.Value("kObjectChunksOffset", 65536);
        parsed.kObjectCountOffset = compatibility.Value("kObjectCountOffset", 16384);
        parsed.kObjectChunkBits = compatibility.Value("kObjectChunkBits", 65536);
        parsed.kObjectItemStride = compatibility.Value("kObjectItemStride", 16384);
        parsed.kObjectItemPointerOffset = compatibility.Value("kObjectItemPointerOffset", 16384);
        parsed.kObjectItemSerialOffset = compatibility.Value("kObjectItemSerialOffset", 16384);
        native_layout = parsed;
        return true;
    } catch (...) { return false; }
}


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
inline constexpr std::string_view kGWorldPattern = "kGWorldPattern";

// Existing UE5 reflection metadata layout from the active profile. Game-specific
// camera offsets are resolved by property name/type, not copied from the dump.
// That indirection is load-bearing: AHTPlayerCameraManager grew in 1.5a, so the fade
// block moved (1.4b -> 1.5a: PlayerFadeDistanceSquare 0x3B54 -> 0x3B5C,
// PlayerHideDistanceSquare 0x3B58 -> 0x3B60, PlayerPitchFadeCurve 0x3B60 -> 0x3B68)
// and NormalCameraSettings moved further still (0xBAC8 -> 0xBAD8). FCameraSettings
// itself did not move, so the 0x570 field size it is validated against still holds.

// UE5 reflection layout: UFunction::Func and the {numParms, parmsSize} pair.

// A UHT exec thunk loads `this` and then calls the native implementation, so
// the last "mov rcx, rdi; call rel32" in the thunk is the function to hook.
inline constexpr std::uint8_t kNativeCallMarker[] = {0x48, 0x8B, 0xCF, 0xE8};
inline constexpr std::uint32_t kThunkScanBytes = 0xC0;

// GObjects registry, used to turn an objects-service handle into an address.
inline constexpr std::string_view kGObjectsPattern = "kGObjectsPattern";
inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;

inline constexpr std::uint32_t kMaximumObjectCount = 16U * 1024U * 1024U;

// Retry missing hook targets or camera reflection metadata at a slow interval.
inline constexpr double kRetrySeconds = 5.0;
inline constexpr std::size_t kMaximumPathBytes = 128;

} // namespace camera_blur_fix_profile
