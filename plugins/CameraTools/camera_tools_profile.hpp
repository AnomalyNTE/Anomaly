#pragma once

#include <cstdint>
#include <string_view>

namespace camera_tools_profile {

// These contracts match the currently validated NTE camera-manager path used
// by FreeCamera. The active manager's vtable must match the resolved hook target
// before the returned camera location is adjusted.
inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";

inline constexpr std::string_view kCameraViewPointPattern =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 49 8B F8 48 8B DA "
    "FF 90 A0 07 00 00 0F 10 00 0F 11 03 F2 0F 10 48 10 F2 0F "
    "11 4B 10 0F 10 40 18 48 8B 5C 24 30 0F 11 07 F2 0F 10 48 "
    "28 F2 0F 11 4F 10 48 83 C4 20 5F C3";

inline constexpr std::string_view kFNamePoolPattern =
    "48 8D 05 ?? ?? ?? ?? EB 13 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "C6 05 ?? ?? ?? ?? 01 0F 10 07 4C 8D 44 24 20 48 8B C8 48 8D 54 24 40";

inline constexpr std::string_view kPlayerInputKeyPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
    "41 54 41 56 41 57 48 83 EC 30 48 8B 01 4C 8B FA 4C 8B E1 "
    "FF 90 80 01 00 00 48 85 C0";

inline constexpr std::uint32_t kGWorldResolveOffset = 3;
inline constexpr std::uint32_t kGWorldInstructionSize = 7;
inline constexpr std::uint32_t kFNamePoolResolveOffset = 3;
inline constexpr std::uint32_t kFNamePoolInstructionSize = 7;

inline constexpr std::uint32_t kWorldGameInstanceOffset = 0x230;
inline constexpr std::uint32_t kGameInstanceLocalPlayersOffset = 0x38;
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;
inline constexpr std::uint32_t kControllerCameraManagerOffset = 0x380;
inline constexpr std::uint32_t kControllerPlayerInputOffset = 0x440;
inline constexpr std::uint32_t kControllerStreamingSourceVtableOffset = 0xC60;
inline constexpr std::uint32_t kCameraViewPointVtableOffset = 0x850;
inline constexpr std::uint32_t kPlayerInputKeyVtableOffset = 0x2B8;
inline constexpr std::uint32_t kInputKeyEventArgsKeyOffset = 0x10;

inline constexpr std::uint32_t kNamePoolBlocksOffset = 0x10;
inline constexpr std::uint32_t kNameBlockBits = 16;
inline constexpr std::uint32_t kNameEntryStride = 2;
inline constexpr std::uint32_t kNameLengthShift = 6;
inline constexpr std::uint32_t kMouseXNameId = 0x12CF;
inline constexpr std::uint32_t kMouseYNameId = 0x12D3;
inline constexpr std::uint32_t kMouse2DNameId = 0x12D7;

} // namespace camera_tools_profile
