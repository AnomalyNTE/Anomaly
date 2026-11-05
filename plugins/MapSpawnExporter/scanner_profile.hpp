#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace anomaly_map_spawn_exporter {
struct Layout {
    std::uint32_t kMapInlineBits{};
    std::uint32_t kMapBitsData{};
    std::uint32_t kMapBitCount{};
    std::uint32_t kMapBitCapacity{};
    std::uint32_t kStructPropertyLink{};
    std::uint32_t kStructSize{};
    std::uint32_t kPropertyName{};
    std::uint32_t kPropertyOffset{};
    std::uint32_t kPropertyClass{};
    std::uint32_t kPropertyElementSize{};
    std::uint32_t kPropertyNext{};
    std::uint32_t kStructPropertyStruct{};

    std::ptrdiff_t kGObjectsAddend{};
    std::uint32_t kObjectItemsOffset{};
    std::uint32_t kObjectCountOffset{};
    std::uint32_t kObjectMaxCountOffset{};
    std::uint32_t kObjectMaxChunksOffset{};
    std::uint32_t kObjectNumChunksOffset{};
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kDataTableRowMapOffset{};
    std::uint32_t kDataTableRowStride{};
    std::uint32_t kDataTableRowPointerOffset{};
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kObjectOuterOffset{};
    std::uint32_t kDataTableRowStructOffset{};
    std::uint32_t kTeleportBelongsLevelOffset{};
    std::uint32_t kTeleportFloorOffset{};
    std::uint32_t kTeleportTransformTranslationOffset{};
    std::uint32_t kTeleportTypeOffset{};
    std::uint32_t kTeleportCanTeleportOffset{};
    std::uint32_t kTeleportOverrideTransformOffset{};
    std::uint32_t kTeleportOverrideTranslationOffset{};
    std::uint32_t kRandomItemTableOffset{};
    std::uint32_t kOracleStoneDataAssetTableOffset{};
    std::uint32_t kOracleStoneLevelOffset{};
    std::uint32_t kOracleStoneFloorOffset{};
    std::uint32_t kOracleStoneLocationOffset{};
    std::uint32_t kRandomItemLevelNameOffset{};
    std::uint32_t kRandomItemTypeOffset{};
    std::uint32_t kRandomItemTransformOffset{};
    std::uint32_t kTransformTranslationOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "map-spawn-exporter-v1"})) return false;
        Layout parsed;
        parsed.kMapInlineBits=compatibility.Value("kMapInlineBits",16384);
        parsed.kMapBitsData=compatibility.Value("kMapBitsData",16384);
        parsed.kMapBitCount=compatibility.Value("kMapBitCount",16384);
        parsed.kMapBitCapacity=compatibility.Value("kMapBitCapacity",16384);
        parsed.kStructPropertyLink=compatibility.Value("kStructPropertyLink",16384);
        parsed.kStructSize=compatibility.Value("kStructSize",16384);
        parsed.kPropertyName=compatibility.Value("kPropertyName",16384);
        parsed.kPropertyOffset=compatibility.Value("kPropertyOffset",16384);
        parsed.kPropertyClass=compatibility.Value("kPropertyClass",16384);
        parsed.kPropertyElementSize=compatibility.Value("kPropertyElementSize",16384);
        parsed.kPropertyNext=compatibility.Value("kPropertyNext",16384);
        parsed.kStructPropertyStruct=compatibility.Value("kStructPropertyStruct",16384);

        parsed.kGObjectsAddend = -static_cast<std::ptrdiff_t>(compatibility.Value("kGObjectsAddendMagnitude", 16384));
        parsed.kObjectItemsOffset = compatibility.Value("kObjectItemsOffset", 16384);
        parsed.kObjectCountOffset = compatibility.Value("kObjectCountOffset", 16384);
        parsed.kObjectMaxCountOffset = compatibility.Value("kObjectMaxCountOffset", 16384);
        parsed.kObjectMaxChunksOffset = compatibility.Value("kObjectMaxChunksOffset", 65536);
        parsed.kObjectNumChunksOffset = compatibility.Value("kObjectNumChunksOffset", 65536);
        parsed.kObjectChunkSize = compatibility.Value("kObjectChunkSize", 65536);
        parsed.kObjectItemStride = compatibility.Value("kObjectItemStride", 16384);
        parsed.kDataTableRowMapOffset = compatibility.Value("kDataTableRowMapOffset", 16384);
        parsed.kDataTableRowStride = compatibility.Value("kDataTableRowStride", 16384);
        parsed.kDataTableRowPointerOffset = compatibility.Value("kDataTableRowPointerOffset", 16384);
        parsed.kObjectClassOffset = compatibility.Value("kObjectClassOffset", 16384);
        parsed.kObjectNameOffset = compatibility.Value("kObjectNameOffset", 16384);
        parsed.kObjectOuterOffset = compatibility.Value("kObjectOuterOffset", 16384);
        parsed.kDataTableRowStructOffset = compatibility.Value("kDataTableRowStructOffset", 16384);
        parsed.kTeleportBelongsLevelOffset = compatibility.Value("kTeleportBelongsLevelOffset", 16384);
        parsed.kTeleportFloorOffset = compatibility.Value("kTeleportFloorOffset", 16384);
        parsed.kTeleportTransformTranslationOffset = compatibility.Value("kTeleportTransformTranslationOffset", 16384);
        parsed.kTeleportTypeOffset = compatibility.Value("kTeleportTypeOffset", 16384);
        parsed.kTeleportCanTeleportOffset = compatibility.Value("kTeleportCanTeleportOffset", 16384);
        parsed.kTeleportOverrideTransformOffset = compatibility.Value("kTeleportOverrideTransformOffset", 16384);
        parsed.kTeleportOverrideTranslationOffset = compatibility.Value("kTeleportOverrideTranslationOffset", 16384);
        parsed.kRandomItemTableOffset = compatibility.Value("kRandomItemTableOffset", 16384);
        parsed.kOracleStoneDataAssetTableOffset = compatibility.Value("kOracleStoneDataAssetTableOffset", 16384);
        parsed.kOracleStoneLevelOffset = compatibility.Value("kOracleStoneLevelOffset", 16384);
        parsed.kOracleStoneFloorOffset = compatibility.Value("kOracleStoneFloorOffset", 16384);
        parsed.kOracleStoneLocationOffset = compatibility.Value("kOracleStoneLocationOffset", 16384);
        parsed.kRandomItemLevelNameOffset = compatibility.Value("kRandomItemLevelNameOffset", 16384);
        parsed.kRandomItemTypeOffset = compatibility.Value("kRandomItemTypeOffset", 16384);
        parsed.kRandomItemTransformOffset = compatibility.Value("kRandomItemTransformOffset", 16384);
        parsed.kTransformTranslationOffset = compatibility.Value("kTransformTranslationOffset", 16384);
        native_layout = parsed;
        return true;
    } catch (...) { return false; }
}


// These values mirror the validated UE5 object/DataTable contract currently used
// by the NTE profile. The exporter only reads immutable table rows and never
// walks UWorld levels or actor/entity snapshots.
inline constexpr std::string_view kGObjectsPattern = "kGObjectsPattern";

inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;

inline constexpr std::uint32_t kMaximumDataTableRows = 32768;
inline constexpr std::uint32_t kMaximumRowBytes = 0x200;

inline constexpr std::size_t kMaximumNameBytes = 1024;
inline constexpr std::uint32_t kMaximumObjectCount = 16U * 1024U * 1024U;
inline constexpr std::uint32_t kMaximumObjectChunks = 4096;

}  // namespace anomaly_map_spawn_exporter
