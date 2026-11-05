#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nte_interactbox_scanner_profile {
struct Layout {
    std::ptrdiff_t kGObjectsAddend{};
    std::ptrdiff_t kObjectItemsOffset{};
    std::ptrdiff_t kObjectMaxCountOffset{};
    std::ptrdiff_t kObjectCountOffset{};
    std::ptrdiff_t kObjectMaxChunksOffset{};
    std::ptrdiff_t kObjectNumChunksOffset{};
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kDataAssetRandomItemTableOffset{};
    std::uint32_t kDataTableRowMapOffset{};
    std::uint32_t kDataTableRowStride{};
    std::uint32_t kDataTableRowPointerOffset{};
    std::uint32_t kRandomItemLevelNameOffset{};
    std::uint32_t kRandomItemTypeNameOffset{};
    std::uint32_t kRandomItemTransformOffset{};
    std::uint32_t kTransformTranslationOffset{};
    std::uint32_t kWorldGameInstanceOffset{};
    std::uint32_t kGameInstanceLocalPlayersOffset{};
    std::uint32_t kLocalPlayerControllerOffset{};
    std::uint32_t kControllerPlayerStateOffset{};
    std::uint32_t kRecordOwnerFindRecordVtableOffset{};
    std::uint32_t kRecordOwnerOffset{};
    std::uint32_t kRecordIndexOffset{};
    std::uint32_t kRecordDescriptorTableOffset{};
    std::uint32_t kRecordDescriptorStride{};
    std::uint32_t kRecordDescriptorStoreOffset{};
    std::uint32_t kRecordStoreRowsOffset{};
    std::uint32_t kRecordGetStringVtableOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "wallet-collector-v1"})) return false;
        Layout parsed;
        parsed.kGObjectsAddend = -static_cast<std::ptrdiff_t>(compatibility.Value("kGObjectsAddendMagnitude", 16384));
        parsed.kObjectItemsOffset = compatibility.Value("kObjectItemsOffset", 16384);
        parsed.kObjectMaxCountOffset = compatibility.Value("kObjectMaxCountOffset", 16384);
        parsed.kObjectCountOffset = compatibility.Value("kObjectCountOffset", 16384);
        parsed.kObjectMaxChunksOffset = compatibility.Value("kObjectMaxChunksOffset", 65536);
        parsed.kObjectNumChunksOffset = compatibility.Value("kObjectNumChunksOffset", 65536);
        parsed.kObjectChunkSize = compatibility.Value("kObjectChunkSize", 65536);
        parsed.kObjectItemStride = compatibility.Value("kObjectItemStride", 16384);
        parsed.kDataAssetRandomItemTableOffset = compatibility.Value("kDataAssetRandomItemTableOffset", 16384);
        parsed.kDataTableRowMapOffset = compatibility.Value("kDataTableRowMapOffset", 16384);
        parsed.kDataTableRowStride = compatibility.Value("kDataTableRowStride", 16384);
        parsed.kDataTableRowPointerOffset = compatibility.Value("kDataTableRowPointerOffset", 16384);
        parsed.kRandomItemLevelNameOffset = compatibility.Value("kRandomItemLevelNameOffset", 16384);
        parsed.kRandomItemTypeNameOffset = compatibility.Value("kRandomItemTypeNameOffset", 16384);
        parsed.kRandomItemTransformOffset = compatibility.Value("kRandomItemTransformOffset", 16384);
        parsed.kTransformTranslationOffset = compatibility.Value("kTransformTranslationOffset", 16384);
        parsed.kWorldGameInstanceOffset = compatibility.Value("kWorldGameInstanceOffset", 16384);
        parsed.kGameInstanceLocalPlayersOffset = compatibility.Value("kGameInstanceLocalPlayersOffset", 16384);
        parsed.kLocalPlayerControllerOffset = compatibility.Value("kLocalPlayerControllerOffset", 16384);
        parsed.kControllerPlayerStateOffset = compatibility.Value("kControllerPlayerStateOffset", 16384);
        parsed.kRecordOwnerFindRecordVtableOffset = compatibility.Value("kRecordOwnerFindRecordVtableOffset", 16384);
        parsed.kRecordOwnerOffset = compatibility.Value("kRecordOwnerOffset", 16384);
        parsed.kRecordIndexOffset = compatibility.Value("kRecordIndexOffset", 16384);
        parsed.kRecordDescriptorTableOffset = compatibility.Value("kRecordDescriptorTableOffset", 16384);
        parsed.kRecordDescriptorStride = compatibility.Value("kRecordDescriptorStride", 16384);
        parsed.kRecordDescriptorStoreOffset = compatibility.Value("kRecordDescriptorStoreOffset", 16384);
        parsed.kRecordStoreRowsOffset = compatibility.Value("kRecordStoreRowsOffset", 16384);
        parsed.kRecordGetStringVtableOffset = compatibility.Value("kRecordGetStringVtableOffset", 16384);
        native_layout = parsed;
        return true;
    } catch (...) { return false; }
}


// These are the validated GObjects contracts used by the UE5 object service.
// The plugin reads only immutable object and data-table fields.
inline constexpr std::string_view kGObjectsPattern = "kGObjectsPattern";
inline constexpr std::string_view kGWorldPattern = "kGWorldPattern";
inline constexpr std::string_view kGetRecordOwnerPattern = "kGetRecordOwnerPattern";

inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;

} // namespace nte_interactbox_scanner_profile
