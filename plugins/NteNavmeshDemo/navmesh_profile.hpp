#pragma once

#include <cstdint>
#include <string_view>

namespace navmesh_profile {

inline constexpr std::string_view kModuleName = "HTGame.exe";
inline constexpr std::string_view kTextSection = ".text";

// These values are pinned to the current NTE build and are intentionally kept
// inside this research plugin rather than becoming a host Profile contract.
inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr std::string_view kFNamePoolPattern =
    "48 8D 05 ?? ?? ?? ?? EB 13 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "C6 05 ?? ?? ?? ?? 01 0F 10 07 4C 8D 44 24 20 48 8B C8 48 8D 54 24 40";
inline constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr std::string_view kProcessEventPattern =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 "
    "48 89 9D 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 C0 00 00 00 "
    "8B 41 08 4D 8B F0 C1 E8 1E 48 8B FA F6 D0 4C 8B F9 A8 01 0F 84 ?? ?? ?? ?? "
    "33 F6 F7 82 B0 00 00 00 00 04 00 00";

inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;
inline constexpr std::int32_t kGObjectsAddend = -16;

inline constexpr std::uint32_t kWorldGameInstanceOffset = 0x230;
inline constexpr std::uint32_t kGameInstanceLocalPlayersOffset = 0x38;
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;

inline constexpr std::uint32_t kObjectClassOffset = 0x10;
inline constexpr std::uint32_t kObjectNameOffset = 0x18;
inline constexpr std::uint32_t kObjectOuterOffset = 0x20;
inline constexpr std::uint32_t kUStructSuperStructOffset = 0x40;
inline constexpr std::uint32_t kUStructPropertyLinkOffset = 0x70;
inline constexpr std::uint32_t kUFunctionNumParmsOffset = 0xB4;
inline constexpr std::uint32_t kUFunctionParmsSizeOffset = 0xB6;
inline constexpr std::uint32_t kUFunctionReturnValueOffset = 0xB8;

inline constexpr std::uint32_t kFFieldClassOffset = 0x8;
inline constexpr std::uint32_t kFFieldNameOffset = 0x20;
inline constexpr std::uint32_t kFFieldClassNameOffset = 0x0;
inline constexpr std::uint32_t kFPropertyArrayDimOffset = 0x30;
inline constexpr std::uint32_t kFPropertyElementSizeOffset = 0x34;
inline constexpr std::uint32_t kFPropertyOffsetInternalOffset = 0x44;
inline constexpr std::uint32_t kFPropertyLinkNextOffset = 0x48;
inline constexpr std::uint32_t kFStructPropertyStructOffset = 0x70;
inline constexpr std::uint32_t kFBoolPropertyFieldSizeOffset = 0x70;
inline constexpr std::uint32_t kFBoolPropertyByteOffsetOffset = 0x71;
inline constexpr std::uint32_t kFBoolPropertyByteMaskOffset = 0x72;
inline constexpr std::uint32_t kFBoolPropertyFieldMaskOffset = 0x73;

inline constexpr std::uint32_t kNamePoolBlocksOffset = 0x10;
inline constexpr std::uint32_t kNameBlockBits = 16;
inline constexpr std::uint32_t kNameEntryStride = 2;
inline constexpr std::uint32_t kNameLengthShift = 6;

inline constexpr std::uint32_t kObjectRegistryItemsOffset = 0x10;
inline constexpr std::uint32_t kObjectRegistryMaxCountOffset = 0x20;
inline constexpr std::uint32_t kObjectRegistryCountOffset = 0x24;
inline constexpr std::uint32_t kObjectRegistryMaxChunksOffset = 0x28;
inline constexpr std::uint32_t kObjectRegistryNumChunksOffset = 0x2C;
inline constexpr std::uint32_t kObjectRegistryChunkSize = 65536;
inline constexpr std::uint32_t kObjectRegistryItemStride = 24;
inline constexpr std::uint32_t kObjectRegistryObjectOffset = 0;
inline constexpr std::uint32_t kObjectRegistrySerialOffset = 16;

inline constexpr std::uint8_t kMoveToLocationParameterCount = 10;
inline constexpr std::uint16_t kMoveToLocationParmsSize = 0x38;
inline constexpr std::uint8_t kPathFollowingReasonCommon = 2;
inline constexpr std::uint8_t kPathRequestFailed = 0;
inline constexpr std::uint8_t kPathRequestAlreadyAtGoal = 1;
inline constexpr std::uint8_t kPathRequestSuccessful = 2;

}  // namespace navmesh_profile
