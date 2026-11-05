#pragma once
#include <anomaly/sdk/profile.hpp>
namespace npc_profile {
inline anomaly::sdk::ProfileSnapshot Compatibility;
struct NativeLayout final {
    std::uint32_t L_propertyFlags{};
    std::uint32_t L_structPropertyStruct{};

    std::uint32_t L_world_gameInstance{};
    std::uint32_t L_gameInstance_localPlayers{};
    std::uint32_t L_localPlayer_controller{};
    std::uint32_t L_object_internalIndex{};
    std::uint32_t L_object_class{};
    std::uint32_t L_object_name{};
    std::uint32_t L_object_outer{};
    std::uint32_t L_actor_rootComponent{};
    std::uint32_t L_uclass_classDefaultObject{};
    std::uint32_t L_struct_superStruct{};
    std::uint32_t L_struct_children{};
    std::uint32_t L_field_next{};
    std::uint32_t L_field_name{};
    std::uint32_t L_function_flags{};
    std::uint32_t L_function_numParms{};
    std::uint32_t L_function_parmsSize{};
    std::uint32_t L_function_returnValueOffset{};
    std::uint32_t L_property_elementSize{};
    std::uint32_t L_property_offsetInternal{};
    std::uint32_t L_ffield_next{};
    std::uint32_t L_ffield_class{};
    std::uint32_t L_controller_pawn{};
    std::uint32_t L_objectsPerChunk{};
    std::uint32_t L_objectItemSize{};
    std::uint32_t L_objectsStructItems{};
    std::uint32_t L_ripDisplacement{};
    std::uint32_t L_ripInstructionSize{};
    std::uint32_t L_struct_childProperties{};
    std::uint32_t L_processEventEntryBack{};
    std::uint32_t L_transformTranslation{};
    std::uint32_t L_transformSize{};
    std::uint32_t L_functionNative{};
};
inline NativeLayout Layout;
inline bool Load(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!Compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "npc-summon-v1"}, {"ProcessEvent.entry"})) return false;
        NativeLayout next;
        next.L_propertyFlags=Compatibility.Value("L_propertyFlags",65536);
        next.L_structPropertyStruct=Compatibility.Value("L_structPropertyStruct",65536);

        next.L_world_gameInstance = Compatibility.Value("L_world_gameInstance", 65536);
        next.L_gameInstance_localPlayers = Compatibility.Value("L_gameInstance_localPlayers", 65536);
        next.L_localPlayer_controller = Compatibility.Value("L_localPlayer_controller", 65536);
        next.L_object_internalIndex = Compatibility.Value("L_object_internalIndex", 65536);
        next.L_object_class = Compatibility.Value("L_object_class", 65536);
        next.L_object_name = Compatibility.Value("L_object_name", 65536);
        next.L_object_outer = Compatibility.Value("L_object_outer", 65536);
        next.L_actor_rootComponent = Compatibility.Value("L_actor_rootComponent", 65536);
        next.L_uclass_classDefaultObject = Compatibility.Value("L_uclass_classDefaultObject", 65536);
        next.L_struct_superStruct = Compatibility.Value("L_struct_superStruct", 65536);
        next.L_struct_children = Compatibility.Value("L_struct_children", 65536);
        next.L_field_next = Compatibility.Value("L_field_next", 65536);
        next.L_field_name = Compatibility.Value("L_field_name", 65536);
        next.L_function_flags = Compatibility.Value("L_function_flags", 65536);
        next.L_function_numParms = Compatibility.Value("L_function_numParms", 65536);
        next.L_function_parmsSize = Compatibility.Value("L_function_parmsSize", 65536);
        next.L_function_returnValueOffset = Compatibility.Value("L_function_returnValueOffset", 65536);
        next.L_property_elementSize = Compatibility.Value("L_property_elementSize", 65536);
        next.L_property_offsetInternal = Compatibility.Value("L_property_offsetInternal", 65536);
        next.L_ffield_next = Compatibility.Value("L_ffield_next", 65536);
        next.L_ffield_class = Compatibility.Value("L_ffield_class", 65536);
        next.L_controller_pawn = Compatibility.Value("L_controller_pawn", 65536);
        next.L_objectsPerChunk = Compatibility.Value("L_objectsPerChunk", 65536);
        next.L_objectItemSize = Compatibility.Value("L_objectItemSize", 65536);
        next.L_objectsStructItems = Compatibility.Value("L_objectsStructItems", 65536);
        next.L_ripDisplacement = Compatibility.Value("L_ripDisplacement", 65536);
        next.L_ripInstructionSize = Compatibility.Value("L_ripInstructionSize", 65536);
        next.L_struct_childProperties = Compatibility.Value("L_struct_childProperties", 65536);
        next.L_processEventEntryBack = Compatibility.Value("L_processEventEntryBack", 65536);
        next.L_transformTranslation = Compatibility.Value("L_transformTranslation", 65536);
        next.L_transformSize = Compatibility.Value("L_transformSize", 65536);
        next.L_functionNative = Compatibility.Value("L_functionNative", 65536);
        if (!next.L_objectsPerChunk || next.L_objectItemSize < sizeof(void*) ||
            next.L_objectItemSize % alignof(void*) || next.L_transformTranslation % alignof(double) ||
            next.L_transformTranslation + sizeof(double) * 3 > next.L_transformSize) return false;
        Layout = next;
        return true;
    } catch (...) { return false; }
}
} // namespace npc_profile
