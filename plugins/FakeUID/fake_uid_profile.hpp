#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fake_uid_profile {

// FakeUID owns this NTE-specific compatibility contract. The host supplies the
// generic signature scanner and UE services, but does not publish these details.
inline constexpr std::string_view kSetTextPattern =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B F9 48 8B F2 48 8B 0A "
    "48 8B 9F 88 01 00 00 48 3B D9 74 25 48 89 8F 88 01 00 00 48 85 C9 74 0B "
    "48 8B 01 48 8D 54 24 60 FF 50 08 48 85 DB 74 09 48 8B 03 48 8B CB FF 50 10 "
    "8B 46 08 48 8D 54 24 60";
inline constexpr std::string_view kAssignStringPattern =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 41 57 48 83 "
    "EC 30 33 C0 4C 8D 79 08 48 89 01 4C 8D 71 0C 41 89 07 48 8B FA 41 89 06 "
    "48 8B F1 48 85 D2 74 66 66 39 02 74 61 48 C7 C3 FF FF FF FF 48 FF C3 66 39 "
    "04 5A 75 F7 83 C3 01 78 6A 85 DB 7E 1B BA 02 00 00 00";
inline constexpr std::string_view kFromStringPattern =
    "40 53 48 83 EC 30 48 8B C2 48 8B D9 48 8B C8 48 8D 54 24 20 E8 ?? ?? ?? ?? "
    "48 8B D0 48 8B CB E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9 74 ?? E8 ?? ?? ?? "
    "?? 48 8B C3 48 83 C4 30 5B C3 CC CC CC CC 40 53 57 48 83 EC 38 48 89 6C 24 "
    "60";
inline constexpr std::uint32_t kFromStringResolveOffset = 32;
inline constexpr std::uint32_t kFromStringInstructionSize = 36;
inline constexpr std::string_view kFreeStringPattern =
    "48 85 C9 74 2E 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C "
    "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 48 48 83 C4 20 "
    "5B C3";
inline constexpr std::string_view kTextToStringPattern =
    "40 53 48 83 EC 20 48 8B D9 48 8B CA E8 ?? ?? ?? ?? 48 8B D0 48 8B CB E8 ?? "
    "?? ?? ?? 48 8B C3 48 83 C4 20 5B C3";
inline constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";

inline constexpr std::uint32_t kObjectNameOffset = 24;
inline constexpr std::uint32_t kObjectRegistryItemsOffset = 16;
inline constexpr std::uint32_t kObjectChunkSize = 65536;
inline constexpr std::uint32_t kObjectItemStride = 24;
inline constexpr std::uint32_t kObjectItemSerialOffset = 16;
inline constexpr std::uint32_t kTextFieldOffset = 392;
inline constexpr std::uintptr_t kSetTextVtableOffset = 856;
inline constexpr std::uint32_t kGObjectsResolveOffset = 3;
inline constexpr std::uint32_t kGObjectsInstructionSize = 7;
inline constexpr std::int32_t kGObjectsAddend = -16;

}  // namespace fake_uid_profile
