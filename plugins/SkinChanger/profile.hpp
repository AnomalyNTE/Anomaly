#pragma once
#include <cstdint>

namespace accessory::profile {
// Verified against HTGame_dump.exe supplied on 2026-09-29. Signatures must resolve
// uniquely; reflection layout checks are required before enabling replacement.
inline constexpr auto GetAppearance = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 80 B9 50 2A 00 00 00 48 8B F2 48 8B F9 0F 84 A1 00 00 00";
inline constexpr auto Refresh = "40 55 41 55 48 8D AC 24 88 FD FF FF 48 81 EC 78 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 50 02 00 00 48 FF 81 58 2A 00 00 4C 8B E9 48 8B 81 58 2A 00 00 48 89 44 24 70 48 89 4D 90";
inline constexpr auto World = "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr auto Objects = "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr auto GliderSpawn = "40 55 53 57 48 8D 6C 24 B9 48 81 EC A0 00 00 00 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 0F 84 2E 01 00 00 48 83 7F 08 00";
inline constexpr auto GliderRefresh = "48 8B C4 55 48 8D 68 A1 48 81 EC A0 00 00 00 48 89 58 08 48 8B D9 4C 89 70 E8 E8 ?? ?? ?? ?? 45 33 F6 48 85 C0 74 7E 48 8B 88 98 00 00 00 48 85 C9 74 72 48 8B 93 A0 2A 00 00";
inline constexpr auto SoftClassAssign = "48 89 5C 24 18 56 48 83 EC 40 48 8B 02 48 8D 71 18 48 89 01 48 8B D9 0F 10 42 08 48 83 C2 18 0F 11 41 08 48 3B F2 74 62";
inline constexpr std::uint32_t GliderSpawnCall=0x156, GliderId=0x2aa0, GliderActor=0x3b30, GliderLoading=0x3b38;
inline constexpr std::uint32_t CopyNamesCall = 0x134;
inline constexpr std::uint32_t WorldGameInstance = 0x230, LocalPlayers = 0x38;
inline constexpr std::uint32_t Controller = 0x30, Pawn = 0x308;
inline constexpr std::uint32_t ObjectClass = 0x10, Super = 0x40, PropertyLink = 0x70;
inline constexpr std::uint32_t FieldName = 0x20, PropertyOffset = 0x44, PropertyNext = 0x48;
inline constexpr std::uint32_t DisplayCharacter = 0x29e4;
inline constexpr std::uint32_t AppearanceType = 0x08, AppearanceCharacter = 0x0c;
inline constexpr std::uint32_t AppearanceName = 0x30, AppearanceData = 0x1b8;
inline constexpr std::uint32_t AppearanceCharacterShow = 0x211;
inline constexpr std::uint32_t RowStruct = 0x28, RowMap = 0x30;
inline constexpr std::uint32_t RegistryItems = 0x10, RegistryCount = 0x24, ItemStride = 24, Serial = 16;
}
