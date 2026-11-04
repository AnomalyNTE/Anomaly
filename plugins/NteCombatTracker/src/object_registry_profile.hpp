#pragma once

// Validated GObjects contract for turning a ue5.objects handle into the object
// address. The Host publishes handles, never addresses, so a plugin that must
// dereference an object resolves the registry itself exactly as the built-in
// scanners do.
//
// This plugin reads only immutable DataTable fields.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nte::combat_stats::object_registry {

inline constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;
inline constexpr std::ptrdiff_t kGObjectsAddend = -16;

inline constexpr std::ptrdiff_t kObjectItemsOffset = 16;
inline constexpr std::ptrdiff_t kObjectMaxCountOffset = 32;
inline constexpr std::ptrdiff_t kObjectCountOffset = 36;
inline constexpr std::ptrdiff_t kObjectMaxChunksOffset = 40;
inline constexpr std::ptrdiff_t kObjectNumChunksOffset = 44;
inline constexpr std::uint32_t kObjectChunkSize = 65536;
inline constexpr std::uint32_t kObjectItemStride = 24;

// FInstancedStruct: a UScriptStruct pointer followed by the payload pointer.
inline constexpr std::int32_t kInstancedStructType = 0;
inline constexpr std::int32_t kInstancedStructData = 8;

// UStruct / FProperty layout on this build.
inline constexpr std::int32_t kObjectClass = 16;
inline constexpr std::int32_t kObjectName = 24;
inline constexpr std::int32_t kStructSuperStruct = 64;
inline constexpr std::int32_t kStructChildren = 72;
inline constexpr std::int32_t kStructChildProperties = 112;
inline constexpr std::int32_t kFieldName = 32;
// FField->ClassPrivate: the class object naming the property kind.
inline constexpr std::int32_t kFieldClass = 8;
inline constexpr std::int32_t kFieldNext = 72;
inline constexpr std::int32_t kPropertyElementSize = 52;
inline constexpr std::int32_t kPropertyOffsetInternal = 68;
// FStructProperty->Struct: the UScriptStruct a StructProperty points at.
// 0x70 matches the offset the built-in plugins already read on this build; the
// alternate is probed because the field sits after the FProperty tail and moved
// between engine revisions.
inline constexpr std::int32_t kPropertyStruct = 0x70;
inline constexpr std::int32_t kPropertyStructAlt = 0x78;

// UDataTable::RowMap is a TMap<FName, std::uint8_t*>, and RowStruct names the
// struct whose property chain declares each row's fields.
inline constexpr std::int32_t kDataTableRowStruct = 40;
inline constexpr std::int32_t kDataTableRowMap = 48;
inline constexpr std::int32_t kRowMapCount = 8;
inline constexpr std::int32_t kRowMapMax = 12;
inline constexpr std::int32_t kRowStride = 24;
inline constexpr std::int32_t kRowKeyNumber = 4;
inline constexpr std::int32_t kRowData = 8;
inline constexpr std::int32_t kMaximumRows = 8192;

}  // namespace nte::combat_stats::object_registry
