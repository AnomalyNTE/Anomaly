#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pink_paw_heist_esp::bank_profile {

// Plugin-local reflection contract, using the same UE field layout as the
// active nte-current Profile. NTE data field offsets are resolved by name and
// type at runtime; bank requests are disabled on any contract mismatch.
inline constexpr std::ptrdiff_t kPropertyLink = 112;
inline constexpr std::ptrdiff_t kFieldClass = 8;
inline constexpr std::ptrdiff_t kFieldName = 32;
inline constexpr std::ptrdiff_t kPropertyArrayDim = 48;
inline constexpr std::ptrdiff_t kPropertySize = 52;
inline constexpr std::ptrdiff_t kPropertyOffset = 68;
inline constexpr std::ptrdiff_t kPropertyNext = 72;
inline constexpr std::ptrdiff_t kStructPropertyStruct = 112;
inline constexpr std::ptrdiff_t kArrayPropertyInner = 120;
inline constexpr std::ptrdiff_t kFunctionNumParms = 180;
inline constexpr std::ptrdiff_t kFunctionParmsSize = 182;
inline constexpr std::size_t kProcessEventSlot = 0x4C;
inline constexpr std::string_view kCloneTablePath =
    "/Game/DataTable/CloneSystem/CloneSystemDataTable.CloneSystemDataTable";
inline constexpr std::string_view kEnterFunction = "/Script/HTGame.HTPlayerState.EnterCloneSceneRequest";
inline constexpr std::string_view kCanEnterFunction = "/Script/HTGame.HTPlayerState.CanEntryRobBankClone";
inline constexpr std::string_view kEvacuateFunction = "/Script/HTGame.HTPlayerState.RobBankRequestEvacuation";
// Observed through the bank-entry controller's LevelSequenceActor on 2026-10-20.
inline constexpr std::string_view kBankIntroSequence =
    "/Game/Blueprints/Level/Quest/JYF/2p_xiaozhi01.2p_xiaozhi01";

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
inline constexpr std::array kEvacuateParameters{Parameter{"ID", "NameProperty", 0, 8}};
inline constexpr std::array kBoolResultParameters{Parameter{"ReturnValue", "BoolProperty", 0, 1}};
inline constexpr std::array kParentResultParameters{Parameter{"ReturnValue", "ObjectProperty", 0, 8}};

}  // namespace pink_paw_heist_esp::bank_profile
