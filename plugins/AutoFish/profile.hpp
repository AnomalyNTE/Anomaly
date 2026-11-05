#pragma once
#include <anomaly/sdk/profile.hpp>
namespace fishing::profile {
struct Layout {
    std::uint32_t ObjectClass{};
    std::uint32_t ObjectName{};
    std::uint32_t ObjectOuter{};
    std::uint32_t ObjectIndex{};
    std::uint32_t StructSuper{};
    std::uint32_t StructProperties{};
    std::uint32_t PropertyName{};
    std::uint32_t PropertyOffset{};
    std::uint32_t PropertySize{};
    std::uint32_t PropertyNext{};
    std::uint32_t FunctionCount{};
    std::uint32_t FunctionSize{};
    std::uint32_t RegistryCount{};
    std::uint32_t RegistryItems{};
    std::uint32_t ChunkSize{};
    std::uint32_t ItemStride{};
    std::uint32_t ItemSerial{};
    std::uint32_t RegistrySubtract{};
    std::uint32_t ProcessEventSlot{};
    std::uint32_t RodSpeedOffset{};
    std::uint32_t DataTableType{};
    std::uint32_t DataTableMap{};
    std::uint32_t GoodsShop{};
    std::uint32_t GoodsItem{};
    std::uint32_t GoodsName{};
    std::uint32_t GoodsCurrency{};
    std::uint32_t GoodsPrice{};
    std::uint32_t GoodsMinimum{};
    std::uint32_t GoodsMaximum{};
    std::uint32_t GoodsSelector{};
    std::uint32_t ItemName{};
    std::uint32_t ItemType{};
    std::uint32_t SoftAssetName{};
    std::uint32_t SoftPackageName{};
    std::uint32_t MapEntryStride{};
    std::uint32_t MapValue{};
    std::uint32_t MapSize{};
    std::uint32_t MapInlineBits{};
    std::uint32_t MapBitsData{};
    std::uint32_t MapBitCount{};
    std::uint32_t MapFreeIndex{};
    std::uint32_t RowStride{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "auto-fish-v1"}, {"ProcessEvent.entry"})) return false;
        Layout parsed;
        parsed.ObjectClass=compatibility.Value("ObjectClass",65536,4);
        parsed.ObjectName=compatibility.Value("ObjectName",65536,4);
        parsed.ObjectOuter=compatibility.Value("ObjectOuter",65536,4);
        parsed.ObjectIndex=compatibility.Value("ObjectIndex",65536,4);
        parsed.StructSuper=compatibility.Value("StructSuper",65536,4);
        parsed.StructProperties=compatibility.Value("StructProperties",65536,4);
        parsed.PropertyName=compatibility.Value("PropertyName",65536,4);
        parsed.PropertyOffset=compatibility.Value("PropertyOffset",65536,4);
        parsed.PropertySize=compatibility.Value("PropertySize",65536,4);
        parsed.PropertyNext=compatibility.Value("PropertyNext",65536,4);
        parsed.FunctionCount=compatibility.Value("FunctionCount",65536,1);
        parsed.FunctionSize=compatibility.Value("FunctionSize",65536,1);
        parsed.RegistryCount=compatibility.Value("RegistryCount",65536,4);
        parsed.RegistryItems=compatibility.Value("RegistryItems",65536,4);
        parsed.ChunkSize=compatibility.Value("ChunkSize",65536,4);
        parsed.ItemStride=compatibility.Value("ItemStride",65536,4);
        parsed.ItemSerial=compatibility.Value("ItemSerial",65536,4);
        parsed.RegistrySubtract=compatibility.Value("RegistrySubtract",65536,4);
        parsed.ProcessEventSlot=compatibility.Value("ProcessEventSlot",65536,4);
        parsed.RodSpeedOffset=compatibility.Value("RodSpeedOffset",65536,4);
        parsed.DataTableType=compatibility.Value("DataTableType",65536,4);
        parsed.DataTableMap=compatibility.Value("DataTableMap",65536,4);
        parsed.GoodsShop=compatibility.Value("GoodsShop",65536,4);
        parsed.GoodsItem=compatibility.Value("GoodsItem",65536,4);
        parsed.GoodsName=compatibility.Value("GoodsName",65536,4);
        parsed.GoodsCurrency=compatibility.Value("GoodsCurrency",65536,1);
        parsed.GoodsPrice=compatibility.Value("GoodsPrice",65536,4);
        parsed.GoodsMinimum=compatibility.Value("GoodsMinimum",65536,4);
        parsed.GoodsMaximum=compatibility.Value("GoodsMaximum",65536,4);
        parsed.GoodsSelector=compatibility.Value("GoodsSelector",65536,4);
        parsed.ItemName=compatibility.Value("ItemName",65536,4);
        parsed.ItemType=compatibility.Value("ItemType",65536,1);
        parsed.SoftAssetName=compatibility.Value("SoftAssetName",65536,4);
        parsed.SoftPackageName=compatibility.Value("SoftPackageName",65536,4);
        parsed.MapEntryStride=compatibility.Value("MapEntryStride",65536,4);
        parsed.MapValue=compatibility.Value("MapValue",65536,4);
        parsed.MapSize=compatibility.Value("MapSize",65536,4);
        parsed.MapInlineBits=compatibility.Value("MapInlineBits",65536,4);
        parsed.MapBitsData=compatibility.Value("MapBitsData",65536,4);
        parsed.MapBitCount=compatibility.Value("MapBitCount",65536,4);
        parsed.MapFreeIndex=compatibility.Value("MapFreeIndex",65536,4);
        parsed.RowStride=compatibility.Value("RowStride",65536,4);
        if (!parsed.ChunkSize || parsed.ItemStride < sizeof(void*) ||
            parsed.ItemSerial + sizeof(std::uint32_t)>parsed.ItemStride ||
            parsed.ProcessEventSlot % sizeof(void*) != 0 || parsed.MapEntryStride < sizeof(void*) ||
            parsed.MapValue + sizeof(void*)>parsed.MapEntryStride) return false;
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
