#pragma once
#include "native_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oracle_stone_locator_profile {

// Validated for NTE win64 builds tracked by the Anomaly project.
inline constexpr std::string_view kModuleName = "HTGame.exe";
inline constexpr std::string_view kTextSection = ".text";

inline constexpr std::string_view kGWorldPattern = "kGWorldPattern";
inline constexpr std::string_view kFNamePoolPattern = "kFNamePoolPattern";
inline constexpr std::string_view kGObjectsPattern = "kGObjectsPattern";
// No oracle-stone state query: the pattern that used to live here matched
// AHTTreasureBoxActor::TryOpen's open step on the 9/26+ builds, which
// writes the treasure-box opened bitmap. Do not reintroduce a call to it.

inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;


inline constexpr std::string_view kTreasureboxDataAssetPath =
    "/Game/DataAssets/TreasureboxDataAsset.TreasureboxDataAsset";
inline constexpr std::string_view kOracleRowStructName = "OracleStoneConfigData";
inline constexpr std::string_view kOracleAssetClassName = "HTTreasureboxDataAsset";

// Object registry.

// UE reflection layout (from the validated NTE profile).

// World / player state chain.

// DataTable layout.

inline constexpr std::int32_t kDataTableMaxRows = 4096;

// Oracle stone row layout (FOracleStoneConfigData, re-read from the 1.5a SDK: the row
// struct is 0x60 bytes, the four offsets the scan below actually uses are
// Index 0x08, LevelName 0x10, floor 0x30, Location 0x40, bIsMapExploreValue 0x58).

// Oracle icon map layout.

// MiniMapIconAddParams layout.

} // namespace oracle_stone_locator_profile
