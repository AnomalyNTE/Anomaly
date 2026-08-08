#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nte_interactbox_scanner_profile {

// These are the validated GObjects contracts used by the UE5 object service.
// The plugin reads only immutable object and data-table fields.
inline constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr std::string_view kGetRecordOwnerPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 "
    "48 8B 99 60 01 00 00 48 8B E9";
inline constexpr std::ptrdiff_t kGObjectsAddend = -16;
inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;
inline constexpr std::ptrdiff_t kObjectItemsOffset = 16;
inline constexpr std::ptrdiff_t kObjectMaxCountOffset = 32;
inline constexpr std::ptrdiff_t kObjectCountOffset = 36;
inline constexpr std::ptrdiff_t kObjectMaxChunksOffset = 40;
inline constexpr std::ptrdiff_t kObjectNumChunksOffset = 44;
inline constexpr std::uint32_t kObjectChunkSize = 65536;
inline constexpr std::uint32_t kObjectItemStride = 24;
inline constexpr std::uint32_t kDataAssetRandomItemTableOffset = 0x48;
inline constexpr std::uint32_t kDataTableRowMapOffset = 0x30;
inline constexpr std::uint32_t kDataTableRowStride = 24;
inline constexpr std::uint32_t kDataTableRowPointerOffset = 8;
inline constexpr std::uint32_t kRandomItemLevelNameOffset = 8;
inline constexpr std::uint32_t kRandomItemTypeNameOffset = 0x20;
inline constexpr std::uint32_t kRandomItemTransformOffset = 0x30;
inline constexpr std::uint32_t kTransformTranslationOffset = 0x20;
inline constexpr std::uint32_t kWorldGameInstanceOffset = 0x230;
inline constexpr std::uint32_t kGameInstanceLocalPlayersOffset = 0x38;
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;
inline constexpr std::uint32_t kControllerPlayerStateOffset = 0x2D0;
inline constexpr std::uint32_t kRecordOwnerFindRecordVtableOffset = 0x118;
inline constexpr std::uint32_t kRecordOwnerOffset = 0x8;
inline constexpr std::uint32_t kRecordIndexOffset = 0x10;
inline constexpr std::uint32_t kRecordDescriptorTableOffset = 0x18;
inline constexpr std::uint32_t kRecordDescriptorStride = 0x28;
inline constexpr std::uint32_t kRecordDescriptorStoreOffset = 0x20;
inline constexpr std::uint32_t kRecordStoreRowsOffset = 0x20;
inline constexpr std::uint32_t kRecordGetStringVtableOffset = 0xE0;

} // namespace nte_interactbox_scanner_profile
