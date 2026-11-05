#pragma once
#include <anomaly/sdk/profile.hpp>
#include <algorithm>
#include <array>
#include <memory>

namespace claw::compat {
struct Layout {
    std::uint32_t FieldClass{},FieldClassName{},ObjectFlags{},ObjectIndex{},ObjectClass{},ObjectName{},ObjectOuter{};
    std::uint32_t Super{},PropertyLink{},FieldName{},PropertySize{},PropertyOffset{},PropertyNext{};
    std::uint32_t FunctionFlags{},FunctionCount{},FunctionSize{},RegistryCount{},RegistryItems{},ItemStride{},Serial{},ChunkSize{};
    std::uint32_t RegistryLoad{},RegistrySubtract{};
    std::uint32_t MapSize{},MapSlots{},MapEntryStride{},MapValue{},MapInlineBits{},MapBitsData{},MapBitCount{},MapBitCapacity{},DataTableRows{},LevelActors{};
};
struct Snapshot {
    Layout layout;
    anomaly::sdk::ProfileSnapshot compatibility;
};
inline std::shared_ptr<const Snapshot> Load(const AnomalyHostApiV1* api) noexcept {
    try {
        auto parsed=std::make_shared<Snapshot>();
        // These symbols are resolved only by the user-requested Worker after
        // readiness. Loading the SDK snapshot performs no signature scan.
        if(!parsed->compatibility.Load(api,"compatibility.profile.json",
            {"nte","HTGame.exe",".text","autoclaw-v1"},{"Objects","World"}))return {};
        const auto& doc=parsed->compatibility.Document();
        const auto value=[&](std::string_view key){return doc.Value(key,65536);};
        auto& v=parsed->layout;
        v.FieldClass=value("FieldClass");v.FieldClassName=value("FieldClassName");
        v.ObjectFlags=value("ObjectFlags");v.ObjectIndex=value("ObjectIndex");v.ObjectClass=value("ObjectClass");
        v.ObjectName=value("ObjectName");v.ObjectOuter=value("ObjectOuter");v.Super=value("Super");
        v.PropertyLink=value("PropertyLink");v.FieldName=value("FieldName");v.PropertySize=value("PropertySize");
        v.PropertyOffset=value("PropertyOffset");v.PropertyNext=value("PropertyNext");
        v.FunctionFlags=value("FunctionFlags");v.FunctionCount=value("FunctionCount");v.FunctionSize=value("FunctionSize");
        v.RegistryCount=value("RegistryCount");v.RegistryItems=value("RegistryItems");v.ItemStride=value("ItemStride");
        v.Serial=value("Serial");v.ChunkSize=value("ChunkSize");
        v.RegistryLoad=value("RegistryLoad");v.RegistrySubtract=value("RegistrySubtract");
        v.MapSize=value("MapSize");v.MapSlots=value("MapSlots");v.MapEntryStride=value("MapEntryStride");v.MapValue=value("MapValue");
        v.MapInlineBits=value("MapInlineBits");v.MapBitsData=value("MapBitsData");
        v.MapBitCount=value("MapBitCount");v.MapBitCapacity=value("MapBitCapacity");v.DataTableRows=value("DataTableRows");v.LevelActors=value("LevelActors");
        const auto fits=[](std::uint32_t offset,std::size_t bytes,std::uint32_t size){return offset<=size&&bytes<=size-offset;};
        if(v.LevelActors<v.ObjectOuter+sizeof(std::uintptr_t)||v.LevelActors%alignof(std::uintptr_t)||!v.ChunkSize||v.ItemStride<sizeof(std::uintptr_t)||!fits(v.Serial,sizeof(std::uint32_t),v.ItemStride)||
           v.MapSize<sizeof(std::uintptr_t)+2*sizeof(std::int32_t)||
           v.MapEntryStride>256||v.MapValue<sizeof(std::uintptr_t)||!fits(v.MapValue,sizeof(std::uintptr_t),v.MapEntryStride)||
           !fits(v.MapInlineBits,4*sizeof(std::uint32_t),v.MapSize)||!fits(v.MapBitsData,sizeof(std::uintptr_t),v.MapSize)||
           !fits(v.MapBitCount,sizeof(std::int32_t),v.MapSize)||!fits(v.MapBitCapacity,sizeof(std::int32_t),v.MapSize))return {};
        if(doc.symbol("Objects").candidates.empty()||doc.symbol("World").candidates.empty())return {};
        const std::array<std::pair<std::uint32_t,std::size_t>,5> map_fields{{
            {v.MapSlots,sizeof(std::uintptr_t)+2*sizeof(std::int32_t)},{v.MapInlineBits,4*sizeof(std::uint32_t)},
            {v.MapBitsData,sizeof(std::uintptr_t)},{v.MapBitCount,sizeof(std::int32_t)},{v.MapBitCapacity,sizeof(std::int32_t)}}};
        for(std::size_t i=0;i<map_fields.size();++i) {
            const auto [offset,bytes]=map_fields[i];
            if(!fits(offset,bytes,v.MapSize))return {};
            for(std::size_t j=0;j<i;++j)if(offset<map_fields[j].first+map_fields[j].second&&map_fields[j].first<offset+bytes)return {};
        }
        return parsed;
    }catch(...){return {};}
}
// The x64 MOV r64,[RIP+rel32] decoder is an instruction contract. Its location
// within a candidate and the registry adjustment come from the Profile.
inline std::uintptr_t RipTarget(const AnomalyCoreServiceV1* core,std::uintptr_t instruction,std::uint8_t modrm,std::uint32_t subtract) {
    std::array<std::uint8_t,7> bytes{};
    if(instruction<0x10000||instruction>0x00007FFFFFFFFFF8ull||
       !anomaly::sdk::Succeeded(core->read_memory(core->user,instruction,{bytes.data(),bytes.size()}))||
       bytes[0]!=0x48||bytes[1]!=0x8B||bytes[2]!=modrm)return 0;
    std::int32_t displacement{};std::memcpy(&displacement,bytes.data()+3,sizeof(displacement));
    const auto result=static_cast<std::int64_t>(instruction)+static_cast<std::int64_t>(bytes.size())+displacement-subtract;
    return result>=0x10000&&result<=0x00007FFFFFFFFFF8ll&&result%alignof(std::uintptr_t)==0?static_cast<std::uintptr_t>(result):0;
}
inline bool Resolve(const AnomalyHostApiV1* api,const AnomalyCoreServiceV1* core,const Snapshot& snapshot,
                    std::uintptr_t& registry,std::uintptr_t& world) {
    registry=world=0;
    if(!core||!core->read_memory)return false;
    const auto& document=snapshot.compatibility.Document();
    const std::array<std::string_view,2> symbols{"Objects","World"};
    anomaly::sdk::ProfileScanCache scans;
    scans.Prepare(anomaly::sdk::Host(api),document,symbols);
    const auto objects=scans.Resolve(core,document,"Objects");
    const auto slot=scans.Resolve(core,document,"World");
    if(!objects||!slot)return false;
    const auto object_registry=RipTarget(core,objects.address+snapshot.layout.RegistryLoad,0x05,snapshot.layout.RegistrySubtract);
    const auto world_slot=RipTarget(core,slot.address,0x1D,0);
    if(!object_registry||!world_slot)return false;
    registry=object_registry;world=world_slot;return true;
}
}
