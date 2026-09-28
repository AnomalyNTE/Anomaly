#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oracle_stone_locator_profile {

// Validated for NTE win64 builds tracked by the Anomaly project.
inline constexpr std::string_view kModuleName = "HTGame.exe";
inline constexpr std::string_view kTextSection = ".text";

inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr std::string_view kFNamePoolPattern =
    "48 8D 05 ?? ?? ?? ?? EB 13 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "C6 05 ?? ?? ?? ?? 01 0F 10 07 4C 8D 44 24 20 48 8B C8 48 8D 54 24 40";
inline constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr std::string_view kProcessEventPattern =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 "
    "48 8D 6C 24 30 48 89 9D 28 01 00 00 48 8B 05 ?? ?? ?? ?? "
    "48 33 C5 48 89 85 C0 00 00 00 8B 41 08 4D 8B F0 C1 E8 1E "
    "48 8B FA F6 D0 4C 8B F9 A8 01 0F 84 ?? ?? ?? ?? 33 F6 F7 82 "
    "B0 00 00 00 00 04 00 00";
// No oracle-stone state query: the pattern that used to live here matched
// AHTTreasureBoxActor::TryOpen's open step on the 9/26+ builds, which
// writes the treasure-box opened bitmap. Do not reintroduce a call to it.

inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;
inline constexpr std::ptrdiff_t kGObjectsAddend = -16;

inline constexpr std::string_view kTreasureboxDataAssetPath =
    "/Game/DataAssets/TreasureboxDataAsset.TreasureboxDataAsset";
inline constexpr std::string_view kOracleRowStructName = "OracleStoneConfigData";
inline constexpr std::string_view kOracleAssetClassName = "HTTreasureboxDataAsset";

// Object registry.
inline constexpr std::uintptr_t kObjectItemsOffset = 16;
inline constexpr std::uintptr_t kObjectMaxCountOffset = 32;
inline constexpr std::uintptr_t kObjectCountOffset = 36;
inline constexpr std::uintptr_t kObjectMaxChunksOffset = 40;
inline constexpr std::uintptr_t kObjectNumChunksOffset = 44;
inline constexpr std::uint32_t kObjectChunkSize = 65536;
inline constexpr std::uint32_t kObjectItemStride = 24;

// UE reflection layout (from the validated NTE profile).
inline constexpr std::int64_t kObjectInternalIndexOffset = 12;
inline constexpr std::int64_t kObjectClassOffset = 16;
inline constexpr std::int64_t kObjectNameOffset = 24;
inline constexpr std::int64_t kObjectOuterOffset = 32;
inline constexpr std::int64_t kUClassClassDefaultObjectOffset = 272;
inline constexpr std::int64_t kUStructPropertyLinkOffset = 112;
inline constexpr std::int64_t kUStructChildrenOffset = 72;
inline constexpr std::int64_t kUFieldNextOffset = 40;
inline constexpr std::int64_t kFFieldNameOffset = 32;
inline constexpr std::int64_t kFFieldClassOffset = 8;
inline constexpr std::int64_t kFPropertyArrayDimOffset = 48;
inline constexpr std::int64_t kFPropertyElementSizeOffset = 52;
inline constexpr std::int64_t kFPropertyOffsetInternalOffset = 68;
inline constexpr std::int64_t kFPropertyPropertyLinkNextOffset = 72;
inline constexpr std::int64_t kFStructPropertyStructOffset = 112;
inline constexpr std::int64_t kFArrayPropertyInnerOffset = 120;
inline constexpr std::int64_t kFMapPropertyKeyOffset = 112;
inline constexpr std::int64_t kFMapPropertyValueOffset = 120;
inline constexpr std::int64_t kFEnumPropertyUnderlyingOffset = 112;
inline constexpr std::int64_t kFEnumPropertyEnumOffset = 120;
inline constexpr std::int64_t kFBoolPropertyFieldSizeOffset = 112;
inline constexpr std::int64_t kFBoolPropertyByteOffset = 113;
inline constexpr std::int64_t kFBoolPropertyByteMask = 114;
inline constexpr std::int64_t kFBoolPropertyFieldMask = 115;

// World / player state chain.
inline constexpr std::int64_t kWorldGameInstanceOffset = 560;
inline constexpr std::int64_t kGameInstanceLocalPlayersOffset = 56;
inline constexpr std::int64_t kLocalPlayerControllerOffset = 48;
inline constexpr std::int64_t kControllerPlayerStateOffset = 720;

// DataTable layout.
inline constexpr std::int64_t kDataTableRowMapOffset = 48;
inline constexpr std::int64_t kDataTableRowMapElementStride = 24;
inline constexpr std::int64_t kDataTableRowMapRowOffset = 8;
inline constexpr std::int64_t kDataTableRowMapData = 0;
inline constexpr std::int64_t kDataTableRowMapNum = 8;
inline constexpr std::int64_t kDataTableRowMapMax = 12;
inline constexpr std::int64_t kDataTableRowMapInlineFlags = 16;
inline constexpr std::int64_t kDataTableRowMapFlagsData = 32;
inline constexpr std::int64_t kDataTableRowMapFlagsNum = 40;
inline constexpr std::int64_t kDataTableRowMapFlagsMax = 44;
inline constexpr std::int64_t kDataTableRowStruct = 40;
inline constexpr std::int32_t kDataTableMaxRows = 4096;

// Oracle stone row layout.
inline constexpr std::int64_t kOracleStoneDataAssetTableOffset = 64;
inline constexpr std::int64_t kOracleStoneRowSize = 96;
inline constexpr std::int64_t kOracleStoneIndexOffset = 8;
inline constexpr std::int64_t kOracleStoneLevelOffset = 16;
inline constexpr std::int64_t kOracleStoneGameplayAreaOffset = 32;
inline constexpr std::int64_t kOracleStoneFloorAreaOffset = 40;
inline constexpr std::int64_t kOracleStoneFloorOffset = 48;
inline constexpr std::int64_t kOracleStoneAreaOffset = 52;
inline constexpr std::int64_t kOracleStoneLocationOffset = 64;
inline constexpr std::int64_t kOracleStoneMapExploreOffset = 88;

// Oracle icon map layout.
inline constexpr std::int64_t kOracleStoneIconMapOffset = 96;
inline constexpr std::int64_t kOracleStoneIconMapSize = 80;
inline constexpr std::int64_t kOracleStoneIconMapElementStride = 20;
inline constexpr std::int64_t kOracleStoneIconMapKeyOffset = 0;
inline constexpr std::int64_t kOracleStoneIconMapValueOffset = 4;
inline constexpr std::int64_t kOracleStoneIconMapOracleKey = 4;

// MiniMapIconAddParams layout.
inline constexpr std::int64_t kOracleAddParamsSize = 352;
inline constexpr std::int64_t kEditableTextTextOffset = 360;
inline constexpr std::int64_t kOracleAddMapIconParmsSize = 376;
inline constexpr std::int64_t kOracleRemoveMapIconParmsSize = 18;
inline constexpr std::int64_t kOracleTextIsEmptyParmsSize = 17;
inline constexpr std::int64_t kOracleStringLeftParmsSize = 40;
inline constexpr std::int64_t kOracleAddMapIconIdOffset = 0;
inline constexpr std::int64_t kOracleAddIconTypeOffset = 8;
inline constexpr std::int64_t kOracleAddTransformTypeOffset = 16;
inline constexpr std::int64_t kOracleAddTransformQuatWOffset = 56;
inline constexpr std::int64_t kOracleAddTransformTranslationOffset = 64;
inline constexpr std::int64_t kOracleAddTransformScaleOffset = 96;
inline constexpr std::int64_t kOracleAddBelongsLevelOffset = 160;
inline constexpr std::int64_t kOracleAddGameplayAreaOffset = 176;
inline constexpr std::int64_t kOracleAddFloorAreaOffset = 184;
inline constexpr std::int64_t kOracleAddFloorOffset = 192;
inline constexpr std::int64_t kOracleAddOnlyShowBelongsFloorOffset = 196;
inline constexpr std::int64_t kOracleAddAreaOffset = 200;
inline constexpr std::int64_t kOracleAddOverrideNameOffset = 208;
inline constexpr std::int64_t kOracleAddOverrideDescOffset = 224;
inline constexpr std::int64_t kOracleAddIconRegionOffset = 240;
inline constexpr std::int64_t kOracleAddUniqueIdOffset = 256;
inline constexpr std::int64_t kOracleAddKeywordsOffset = 272;
inline constexpr std::int64_t kOracleAddIconInfoTypeOffset = 288;
inline constexpr std::int64_t kOracleAddShowTypesOffset = 296;
inline constexpr std::int64_t kOracleAddShowModesOffset = 312;
inline constexpr std::int64_t kOracleAddDrawRangeOffset = 328;
inline constexpr std::int64_t kOracleAddIconStateOffset = 344;
inline constexpr std::int64_t kOracleAddUnderFogOffset = 345;

} // namespace oracle_stone_locator_profile
