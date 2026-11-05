#pragma once
#include <anomaly/sdk/profile.hpp>
namespace nailong_profile {
struct Layout {
    std::uint32_t NL_OBJ_ITEMS_OFFSET{};
    std::uint32_t NL_OBJ_MAX_COUNT_OFFSET{};
    std::uint32_t NL_OBJ_COUNT_OFFSET{};
    std::uint32_t NL_OBJ_MAX_CHUNKS_OFFSET{};
    std::uint32_t NL_OBJ_NUM_CHUNKS_OFFSET{};
    std::uint32_t NL_OBJ_CHUNK_SIZE{};
    std::uint32_t NL_OBJ_ITEM_STRIDE{};
    std::uint32_t NL_OBJ_OBJECT_OFFSET{};
    std::uint32_t NL_OBJ_SERIAL_OFFSET{};
    std::uint32_t RegistrySubtract{};
    std::uint32_t ObjectFlags{};
    std::uint32_t ObjectIndex{};
    std::uint32_t ClassDefaultObject{};
    std::uint32_t ProcessEventSlot{};
    std::uint32_t FunctionFlags{};
    std::uint32_t FunctionSize{};
    std::uint32_t ObjectClass{};
    std::uint32_t StructSuper{};
    std::uint32_t StructProperties{};
    std::uint32_t PropertyName{};
    std::uint32_t PropertySize{};
    std::uint32_t PropertyOffset{};
    std::uint32_t PropertyNext{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "nailong-native-v1"}, {"ProcessEvent.entry"})) return false;
        Layout parsed;
        parsed.NL_OBJ_ITEMS_OFFSET=compatibility.Value("NL_OBJ_ITEMS_OFFSET",65536);
        parsed.NL_OBJ_MAX_COUNT_OFFSET=compatibility.Value("NL_OBJ_MAX_COUNT_OFFSET",65536);
        parsed.NL_OBJ_COUNT_OFFSET=compatibility.Value("NL_OBJ_COUNT_OFFSET",65536);
        parsed.NL_OBJ_MAX_CHUNKS_OFFSET=compatibility.Value("NL_OBJ_MAX_CHUNKS_OFFSET",65536);
        parsed.NL_OBJ_NUM_CHUNKS_OFFSET=compatibility.Value("NL_OBJ_NUM_CHUNKS_OFFSET",65536);
        parsed.NL_OBJ_CHUNK_SIZE=compatibility.Value("NL_OBJ_CHUNK_SIZE",65536);
        parsed.NL_OBJ_ITEM_STRIDE=compatibility.Value("NL_OBJ_ITEM_STRIDE",65536);
        parsed.NL_OBJ_OBJECT_OFFSET=compatibility.Value("NL_OBJ_OBJECT_OFFSET",65536);
        parsed.NL_OBJ_SERIAL_OFFSET=compatibility.Value("NL_OBJ_SERIAL_OFFSET",65536);
        parsed.RegistrySubtract=compatibility.Value("RegistrySubtract",65536);
        parsed.ObjectFlags=compatibility.Value("ObjectFlags",65536);
        parsed.ObjectIndex=compatibility.Value("ObjectIndex",65536);
        parsed.ClassDefaultObject=compatibility.Value("ClassDefaultObject",65536);
        parsed.ProcessEventSlot=compatibility.Value("ProcessEventSlot",65536);
        parsed.FunctionFlags=compatibility.Value("FunctionFlags",65536);
        parsed.FunctionSize=compatibility.Value("FunctionSize",65536);
        parsed.ObjectClass=compatibility.Value("ObjectClass",65536);
        parsed.StructSuper=compatibility.Value("StructSuper",65536);
        parsed.StructProperties=compatibility.Value("StructProperties",65536);
        parsed.PropertyName=compatibility.Value("PropertyName",65536);
        parsed.PropertySize=compatibility.Value("PropertySize",65536);
        parsed.PropertyOffset=compatibility.Value("PropertyOffset",65536);
        parsed.PropertyNext=compatibility.Value("PropertyNext",65536);
        if (!parsed.NL_OBJ_CHUNK_SIZE || parsed.NL_OBJ_ITEM_STRIDE<sizeof(void*) || parsed.NL_OBJ_SERIAL_OFFSET+sizeof(std::uint32_t)>parsed.NL_OBJ_ITEM_STRIDE ||
            parsed.ProcessEventSlot%sizeof(void*) || parsed.NL_OBJ_OBJECT_OFFSET+sizeof(void*)>parsed.NL_OBJ_ITEM_STRIDE) return false;
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
