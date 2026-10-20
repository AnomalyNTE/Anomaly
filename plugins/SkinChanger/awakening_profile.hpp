#pragma once
#include "profile.hpp"
#include <cstdint>

namespace skin_awakening_profile {
// Check the fields against loaded reflection metadata before use. Unrelated
// Profile updates do not disable a compatible awakening layout.
inline constexpr std::uint32_t WorldGameInstance = 0x230;
inline constexpr std::uint32_t GameInstanceLocalPlayers = 0x38;
inline constexpr std::uint32_t LocalPlayerController = 0x30;
inline constexpr std::uint32_t ControllerPlayerState = 0x2D0;
inline constexpr std::uint32_t PlayerStateInventory = 0x25B8;
inline constexpr std::uint32_t InventoryContainers = 0x120;
inline constexpr std::uint32_t ItemId = 0x28;
inline constexpr std::uint32_t ItemUniqueId = 0x30;
inline constexpr std::uint32_t ItemAwakenLevel = 0xD4;
inline constexpr std::uint32_t ItemAwakenEffects = 0xD8;
inline constexpr std::uint32_t AwakenUiInfo = 0x12D8;
inline constexpr std::uint32_t AwakenUiInfoSize = 0x148;
inline constexpr std::uint32_t AwakenUiLevel = 0x1300;
inline constexpr std::uint32_t ActorUniqueId = 0x2A70;
inline constexpr std::uint32_t ActorPlayerState = 0x2C68;
inline constexpr std::uint32_t ActorFullAwaken = 0x2B5F;
inline constexpr std::uint32_t ActorAppearance = 0x2960;
inline constexpr std::uint32_t ActorDisplayedFashion = 0x29DC;
inline constexpr std::uint32_t DataTableRowStruct = 0x28;
inline constexpr std::uint32_t DataTableRowMap = 0x30;
inline constexpr std::uint32_t DataTableRowMapFreeCount = 0x64;
inline constexpr std::uint32_t ScriptStructSize = 0x58;
inline constexpr std::uint32_t AwakenEffectStride = 0x80;
inline constexpr std::uint32_t AwakenEffectType = 0x58;
}
