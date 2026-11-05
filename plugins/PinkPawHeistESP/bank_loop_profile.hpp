#pragma once
#include <anomaly/sdk/profile.hpp>
namespace pink_paw_heist_esp::bank_profile {
struct Layout {
    std::uint32_t kWorldGameInstanceOffset{};
    std::uint32_t kGameInstanceLocalPlayersOffset{};
    std::uint32_t kLocalPlayerControllerOffset{};
    std::uint32_t kControllerPlayerStateOffset{};
    std::uint32_t kActorRootComponentOffset{};
    std::uint32_t kRobBankContainerInterfaceOffset{};
    std::uint32_t kRobBankPickupVtableSlot{};
    std::uint32_t kObjectInternalIndexOffset{};
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kObjectOuterOffset{};
    std::uint32_t kStructSuperOffset{};
    std::uint32_t kObjectItemsOffset{};
    std::uint32_t kObjectMaxCountOffset{};
    std::uint32_t kObjectCountOffset{};
    std::uint32_t kObjectMaxChunksOffset{};
    std::uint32_t kObjectNumChunksOffset{};
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kObjectPointerOffset{};
    std::uint32_t kObjectSerialOffset{};
    std::uint32_t kRobBankPointKeyDoorIdOffset{};
    std::uint32_t kRobBankCloneDataAssetItemOffset{};
    std::uint32_t kDataTableRowMapOffset{};
    std::uint32_t kDataTableRowStride{};
    std::uint32_t kDataTableRowPointerOffset{};
    std::uint32_t kStaticItemNameOffset{};
    std::uint32_t kInstancedStructDataOffset{};
    std::uint32_t kRobBankItemValueOffset{};
    std::uint32_t kRobBankItemCoinOffset{};
    std::uint32_t kStructChildren{};
    std::uint32_t kFieldNext{};
    std::uint32_t kPropertyLink{};
    std::uint32_t kFieldClass{};
    std::uint32_t kFieldName{};
    std::uint32_t kPropertyArrayDim{};
    std::uint32_t kPropertySize{};
    std::uint32_t kPropertyOffset{};
    std::uint32_t kPropertyNext{};
    std::uint32_t kStructPropertyStruct{};
    std::uint32_t kArrayPropertyInner{};
    std::uint32_t kFunctionNumParms{};
    std::uint32_t kFunctionParmsSize{};
    std::uint32_t kProcessEventSlot{};
    std::uint32_t kGObjectsSubtract{};
    std::uint32_t kPickupSuccessOffset{};
    std::uint32_t kPickupFailureOffset{};
    std::uint32_t kArrayNum{};
    std::uint32_t kArrayMax{};
    std::uint32_t kRobBankCanInteractOffset{};
    std::uint32_t kRobBankPointUidOffset{};
    std::uint32_t kRobBankAwardDropIdOffset{};
    std::uint32_t kStaticItemElementDataOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "pink-paw-v1"}, {"kPickupPrologue","kPickupSuccessReturn","kPickupFailureReturn","ProcessEvent.entry"}, false, "layout")) return false;
        Layout parsed;
        parsed.kWorldGameInstanceOffset=compatibility.Value("kWorldGameInstanceOffset",65536);
        parsed.kGameInstanceLocalPlayersOffset=compatibility.Value("kGameInstanceLocalPlayersOffset",65536);
        parsed.kLocalPlayerControllerOffset=compatibility.Value("kLocalPlayerControllerOffset",65536);
        parsed.kControllerPlayerStateOffset=compatibility.Value("kControllerPlayerStateOffset",65536);
        parsed.kActorRootComponentOffset=compatibility.Value("kActorRootComponentOffset",65536);
        parsed.kRobBankContainerInterfaceOffset=compatibility.Value("kRobBankContainerInterfaceOffset",65536);
        parsed.kRobBankPickupVtableSlot=compatibility.Value("kRobBankPickupVtableSlot",65536);
        parsed.kObjectInternalIndexOffset=compatibility.Value("kObjectInternalIndexOffset",65536);
        parsed.kObjectClassOffset=compatibility.Value("kObjectClassOffset",65536);
        parsed.kObjectNameOffset=compatibility.Value("kObjectNameOffset",65536);
        parsed.kObjectOuterOffset=compatibility.Value("kObjectOuterOffset",65536);
        parsed.kStructSuperOffset=compatibility.Value("kStructSuperOffset",65536);
        parsed.kObjectItemsOffset=compatibility.Value("kObjectItemsOffset",65536);
        parsed.kObjectMaxCountOffset=compatibility.Value("kObjectMaxCountOffset",65536);
        parsed.kObjectCountOffset=compatibility.Value("kObjectCountOffset",65536);
        parsed.kObjectMaxChunksOffset=compatibility.Value("kObjectMaxChunksOffset",65536);
        parsed.kObjectNumChunksOffset=compatibility.Value("kObjectNumChunksOffset",65536);
        parsed.kObjectChunkSize=compatibility.Value("kObjectChunkSize",65536);
        parsed.kObjectItemStride=compatibility.Value("kObjectItemStride",65536);
        parsed.kObjectPointerOffset=compatibility.Value("kObjectPointerOffset",65536);
        parsed.kObjectSerialOffset=compatibility.Value("kObjectSerialOffset",65536);
        parsed.kRobBankPointKeyDoorIdOffset=compatibility.Value("kRobBankPointKeyDoorIdOffset",65536);
        parsed.kRobBankCloneDataAssetItemOffset=compatibility.Value("kRobBankCloneDataAssetItemOffset",65536);
        parsed.kDataTableRowMapOffset=compatibility.Value("kDataTableRowMapOffset",65536);
        parsed.kDataTableRowStride=compatibility.Value("kDataTableRowStride",65536);
        parsed.kDataTableRowPointerOffset=compatibility.Value("kDataTableRowPointerOffset",65536);
        parsed.kStaticItemNameOffset=compatibility.Value("kStaticItemNameOffset",65536);
        parsed.kInstancedStructDataOffset=compatibility.Value("kInstancedStructDataOffset",65536);
        parsed.kRobBankItemValueOffset=compatibility.Value("kRobBankItemValueOffset",65536);
        parsed.kRobBankItemCoinOffset=compatibility.Value("kRobBankItemCoinOffset",65536);
        parsed.kStructChildren=compatibility.Value("kStructChildren",65536);
        parsed.kFieldNext=compatibility.Value("kFieldNext",65536);
        parsed.kPropertyLink=compatibility.Value("kPropertyLink",65536);
        parsed.kFieldClass=compatibility.Value("kFieldClass",65536);
        parsed.kFieldName=compatibility.Value("kFieldName",65536);
        parsed.kPropertyArrayDim=compatibility.Value("kPropertyArrayDim",65536);
        parsed.kPropertySize=compatibility.Value("kPropertySize",65536);
        parsed.kPropertyOffset=compatibility.Value("kPropertyOffset",65536);
        parsed.kPropertyNext=compatibility.Value("kPropertyNext",65536);
        parsed.kStructPropertyStruct=compatibility.Value("kStructPropertyStruct",65536);
        parsed.kArrayPropertyInner=compatibility.Value("kArrayPropertyInner",65536);
        parsed.kFunctionNumParms=compatibility.Value("kFunctionNumParms",65536);
        parsed.kFunctionParmsSize=compatibility.Value("kFunctionParmsSize",65536);
        parsed.kProcessEventSlot=compatibility.Value("kProcessEventSlot",65536);
        parsed.kGObjectsSubtract=compatibility.Value("kGObjectsSubtract",65536);
        parsed.kPickupSuccessOffset=compatibility.Value("kPickupSuccessOffset",65536);
        parsed.kPickupFailureOffset=compatibility.Value("kPickupFailureOffset",65536);
        parsed.kArrayNum=compatibility.Value("kArrayNum",65536);
        parsed.kArrayMax=compatibility.Value("kArrayMax",65536);
        parsed.kRobBankCanInteractOffset=compatibility.Value("kRobBankCanInteractOffset",65536);
        parsed.kRobBankPointUidOffset=compatibility.Value("kRobBankPointUidOffset",65536);
        parsed.kRobBankAwardDropIdOffset=compatibility.Value("kRobBankAwardDropIdOffset",65536);
        parsed.kStaticItemElementDataOffset=compatibility.Value("kStaticItemElementDataOffset",65536);
        if (!parsed.kObjectChunkSize || parsed.kObjectItemStride<sizeof(void*) || parsed.kObjectSerialOffset+sizeof(std::uint32_t)>parsed.kObjectItemStride ||
            !parsed.kDataTableRowStride || parsed.kDataTableRowPointerOffset+sizeof(void*)>parsed.kDataTableRowStride) return false;
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pink_paw_heist_esp::bank_profile {

// Plugin-local reflection contract, using the same UE field layout as the
// active nte-current Profile. NTE data field offsets are resolved by name and
// type at runtime; bank requests are disabled on any contract mismatch.

inline constexpr std::string_view kCloneTablePath =
    "/Game/DataTable/CloneSystem/CloneSystemDataTable.CloneSystemDataTable";
// Observed through the bank-entry controller's LevelSequenceActor on 2026-10-20.
inline constexpr std::string_view kBankIntroSequence =
    "/Game/Blueprints/Level/Quest/JYF/2p_xiaozhi01.2p_xiaozhi01";
inline constexpr std::string_view kEnterFunction = "/Script/HTGame.HTPlayerState.EnterCloneSceneRequest";
inline constexpr std::string_view kCanEnterFunction = "/Script/HTGame.HTPlayerState.CanEntryRobBankClone";

struct Parameter final {
    std::string_view name;
    std::string_view type;
    std::int32_t offset;
    std::int32_t size;
};
inline constexpr std::array kEnterParameters{
    Parameter{"InCloneID", "NameProperty", 0, 8},
    Parameter{"InCloneLevel", "IntProperty", 8, 4},
    Parameter{"InCloneType", "EnumProperty", 12, 1},
    Parameter{"InCloneCustomArgs", "NameProperty", 16, 8},
};
inline constexpr std::array kCanEnterParameters{Parameter{"ReturnValue", "BoolProperty", 0, 1}};
inline constexpr std::array kBoolResultParameters{Parameter{"ReturnValue", "BoolProperty", 0, 1}};
inline constexpr std::array kParentResultParameters{Parameter{"ReturnValue", "ObjectProperty", 0, 8}};

}  // namespace pink_paw_heist_esp::bank_profile
