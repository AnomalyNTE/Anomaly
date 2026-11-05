#pragma once
#include "profile.hpp"
#include "awakening_data.hpp"
#include <cstddef>
namespace skin_awakening_profile {
struct Layout {
    std::uint32_t AwakenEffectId{};
    std::uint32_t AwakenDataStride{};
    std::uint32_t AwakenDataUnlocked{};
    std::uint32_t AwakenDataId{};

    std::uint32_t WorldGameInstance{};
    std::uint32_t GameInstanceLocalPlayers{};
    std::uint32_t LocalPlayerController{};
    std::uint32_t ControllerPlayerState{};
    std::uint32_t PlayerStateInventory{};
    std::uint32_t InventoryContainers{};
    std::uint32_t ItemId{};
    std::uint32_t ItemUniqueId{};
    std::uint32_t ItemAwakenLevel{};
    std::uint32_t ItemAwakenEffects{};
    std::uint32_t AwakenUiInfo{};
    std::uint32_t AwakenUiInfoSize{};
    std::uint32_t AwakenUiLevel{};
    std::uint32_t ActorUniqueId{};
    std::uint32_t ActorPlayerState{};
    std::uint32_t ActorFullAwaken{};
    std::uint32_t ActorAppearance{};
    std::uint32_t ActorDisplayedFashion{};
    std::uint32_t DataTableRowStruct{};
    std::uint32_t DataTableRowMap{};
    std::uint32_t DataTableRowMapFreeCount{};
    std::uint32_t ScriptStructSize{};
    std::uint32_t AwakenEffectStride{};
    std::uint32_t AwakenEffectType{};
};
inline Layout native_layout;
inline bool LoadLayout() noexcept {
    native_layout={};
    try {
        Layout parsed;
        parsed.AwakenEffectId=accessory::profile::compatibility.Value("awakening.AwakenEffectId",65536);
        parsed.AwakenDataStride=accessory::profile::compatibility.Value("awakening.AwakenDataStride",65536);
        parsed.AwakenDataUnlocked=accessory::profile::compatibility.Value("awakening.AwakenDataUnlocked",65536);
        parsed.AwakenDataId=accessory::profile::compatibility.Value("awakening.AwakenDataId",65536);

        parsed.WorldGameInstance=accessory::profile::compatibility.Value("awakening.WorldGameInstance",65536);
        parsed.GameInstanceLocalPlayers=accessory::profile::compatibility.Value("awakening.GameInstanceLocalPlayers",65536);
        parsed.LocalPlayerController=accessory::profile::compatibility.Value("awakening.LocalPlayerController",65536);
        parsed.ControllerPlayerState=accessory::profile::compatibility.Value("awakening.ControllerPlayerState",65536);
        parsed.PlayerStateInventory=accessory::profile::compatibility.Value("awakening.PlayerStateInventory",65536);
        parsed.InventoryContainers=accessory::profile::compatibility.Value("awakening.InventoryContainers",65536);
        parsed.ItemId=accessory::profile::compatibility.Value("awakening.ItemId",65536);
        parsed.ItemUniqueId=accessory::profile::compatibility.Value("awakening.ItemUniqueId",65536);
        parsed.ItemAwakenLevel=accessory::profile::compatibility.Value("awakening.ItemAwakenLevel",65536);
        parsed.ItemAwakenEffects=accessory::profile::compatibility.Value("awakening.ItemAwakenEffects",65536);
        parsed.AwakenUiInfo=accessory::profile::compatibility.Value("awakening.AwakenUiInfo",65536);
        parsed.AwakenUiInfoSize=accessory::profile::compatibility.Value("awakening.AwakenUiInfoSize",65536);
        parsed.AwakenUiLevel=accessory::profile::compatibility.Value("awakening.AwakenUiLevel",65536);
        parsed.ActorUniqueId=accessory::profile::compatibility.Value("awakening.ActorUniqueId",65536);
        parsed.ActorPlayerState=accessory::profile::compatibility.Value("awakening.ActorPlayerState",65536);
        parsed.ActorFullAwaken=accessory::profile::compatibility.Value("awakening.ActorFullAwaken",65536);
        parsed.ActorAppearance=accessory::profile::compatibility.Value("awakening.ActorAppearance",65536);
        parsed.ActorDisplayedFashion=accessory::profile::compatibility.Value("awakening.ActorDisplayedFashion",65536);
        parsed.DataTableRowStruct=accessory::profile::compatibility.Value("awakening.DataTableRowStruct",65536);
        parsed.DataTableRowMap=accessory::profile::compatibility.Value("awakening.DataTableRowMap",65536);
        parsed.DataTableRowMapFreeCount=accessory::profile::compatibility.Value("awakening.DataTableRowMapFreeCount",65536);
        parsed.ScriptStructSize=accessory::profile::compatibility.Value("awakening.ScriptStructSize",65536);
        parsed.AwakenEffectStride=accessory::profile::compatibility.Value("awakening.AwakenEffectStride",65536);
        parsed.AwakenEffectType=accessory::profile::compatibility.Value("awakening.AwakenEffectType",65536);
        // Effects are read/restored as the named representation in awakening_data.
        // Reject a profile outside that implementation contract before any access.
        if (parsed.AwakenDataStride != sizeof(awakening_data::Effect) ||
            parsed.AwakenDataUnlocked != offsetof(awakening_data::Effect,unlocked) ||
            parsed.AwakenDataId != offsetof(awakening_data::Effect,name) ||
            parsed.AwakenEffectId + sizeof(awakening_data::Name) > parsed.AwakenEffectStride) return false;
        if (!parsed.AwakenEffectStride || parsed.AwakenEffectType>=parsed.AwakenEffectStride ||
            parsed.AwakenUiLevel<parsed.AwakenUiInfo || parsed.AwakenUiLevel+sizeof(std::int32_t)>parsed.AwakenUiInfo+parsed.AwakenUiInfoSize) return false;
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
