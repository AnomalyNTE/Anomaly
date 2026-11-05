#pragma once
#include <anomaly/sdk/profile.hpp>
#include <algorithm>
#include <array>

namespace taxi::profile {
struct Layout {
    std::uint32_t FieldClass{},FieldClassName{};
    std::uint32_t ObjectFlags{};
    std::uint32_t ObjectIndex{};
    std::uint32_t ObjectClass{};
    std::uint32_t ObjectName{};
    std::uint32_t ObjectOuter{};
    std::uint32_t Super{};
    std::uint32_t PropertyLink{};
    std::uint32_t FieldName{};
    std::uint32_t PropertySize{};
    std::uint32_t PropertyOffset{};
    std::uint32_t PropertyNext{};
    std::uint32_t FunctionFlags{};
    std::uint32_t FunctionCount{};
    std::uint32_t FunctionSize{};
    std::uint32_t RegistryCount{};
    std::uint32_t RegistryItems{};
    std::uint32_t ItemStride{};
    std::uint32_t Serial{};
    std::uint32_t ChunkSize{};
    std::uint32_t ProcessEventSlot{};
    std::uint32_t RegistryLoad{};
    std::uint32_t RegistrySubtract{};
    std::uint32_t MapSize{};
    std::uint32_t MapEntryStride{};
    std::uint32_t MapValue{};
    std::uint32_t MapInlineBits{};
    std::uint32_t MapBitsData{};
    std::uint32_t MapBitCount{};
    std::uint32_t MapBitCapacity{};
    std::uint32_t PlayerSaveSize{};
    std::uint32_t PlayerFinished{};
    std::uint32_t PlayerInitialized{};
};
struct Snapshot {
    Layout layout;
    anomaly::sdk::Profile document;
    std::uintptr_t registry{}, world{};
};
inline Snapshot snapshot;

// Decode the supported x64 MOV r64,[RIP+rel32] instruction, not a versioned
// function layout. Recipes and instruction locations come from the Profile.
inline std::uintptr_t RipTarget(const AnomalyCoreServiceV1* core,
        std::uintptr_t instruction, std::uint8_t modrm, std::uint32_t subtract) {
    std::array<std::uint8_t,7> bytes{};
    if (instruction < 0x10000 || instruction > 0x00007FFFFFFFFFF8ull ||
        !anomaly::sdk::Succeeded(core->read_memory(core->user,instruction,{bytes.data(),bytes.size()})) ||
        bytes[0]!=0x48 || bytes[1]!=0x8B || bytes[2]!=modrm) return 0;
    std::int32_t displacement{};
    std::memcpy(&displacement,bytes.data()+3,sizeof(displacement));
    const auto value=static_cast<std::int64_t>(instruction)+static_cast<std::int64_t>(bytes.size())+displacement-subtract;
    return value >= 0x10000 && value <= 0x00007FFFFFFFFFF8ll && value%alignof(std::uintptr_t)==0
        ? static_cast<std::uintptr_t>(value) : 0;
}
inline bool Load(const AnomalyHostApiV1* api) noexcept {
    snapshot={};
    try {
        const anomaly::sdk::Host host(api);
        auto loaded=anomaly::sdk::LoadProfile(host,"compatibility.profile.json");
        if (!loaded) return false;
        Snapshot parsed;
        parsed.document=std::move(loaded).value();
        const auto& doc=parsed.document;
        if (doc.game!="nte" || doc.module!="HTGame.exe" || doc.section!=".text" || doc.contract!="autotaxi-v1") return false;
        const auto value=[&](std::string_view key) {
            const auto found=doc.layout.find(key);
            if (found==doc.layout.end() || found->second>65536) throw std::runtime_error("unsupported taxi layout field");
            return found->second;
        };
        parsed.layout.ObjectFlags=value("ObjectFlags");
        parsed.layout.ObjectIndex=value("ObjectIndex");
        parsed.layout.ObjectClass=value("ObjectClass");
        parsed.layout.ObjectName=value("ObjectName");
        parsed.layout.ObjectOuter=value("ObjectOuter");
        parsed.layout.Super=value("Super");
        parsed.layout.PropertyLink=value("PropertyLink");
        parsed.layout.FieldName=value("FieldName");
        parsed.layout.PropertySize=value("PropertySize");
        parsed.layout.PropertyOffset=value("PropertyOffset");
        parsed.layout.PropertyNext=value("PropertyNext");
        parsed.layout.FunctionFlags=value("FunctionFlags");
        parsed.layout.FunctionCount=value("FunctionCount");
        parsed.layout.FunctionSize=value("FunctionSize");
        parsed.layout.RegistryCount=value("RegistryCount");
        parsed.layout.RegistryItems=value("RegistryItems");
        parsed.layout.ItemStride=value("ItemStride");
        parsed.layout.Serial=value("Serial");
        parsed.layout.ChunkSize=value("ChunkSize");
        parsed.layout.ProcessEventSlot=value("ProcessEventSlot");
        parsed.layout.RegistryLoad=value("RegistryLoad");
        parsed.layout.RegistrySubtract=value("RegistrySubtract");
        parsed.layout.MapSize=value("MapSize");
        parsed.layout.MapEntryStride=value("MapEntryStride");
        parsed.layout.MapValue=value("MapValue");
        parsed.layout.MapInlineBits=value("MapInlineBits");
        parsed.layout.MapBitsData=value("MapBitsData");
        parsed.layout.MapBitCount=value("MapBitCount");
        parsed.layout.MapBitCapacity=value("MapBitCapacity");
        parsed.layout.PlayerSaveSize=value("PlayerSaveSize");
        parsed.layout.PlayerFinished=value("PlayerFinished");
        parsed.layout.PlayerInitialized=value("PlayerInitialized");
        parsed.layout.FieldClass=value("FieldClass");
        parsed.layout.FieldClassName=value("FieldClassName");
        const auto& v=parsed.layout;
        if (!v.ChunkSize || v.ItemStride<sizeof(std::uintptr_t) || v.Serial+sizeof(std::uint32_t)>v.ItemStride ||
            !v.ProcessEventSlot || v.ProcessEventSlot%alignof(std::uintptr_t) || !v.MapEntryStride ||
            v.MapValue+sizeof(std::uint64_t)>v.MapEntryStride || v.MapSize<v.MapBitCapacity+sizeof(std::int32_t) ||
            v.MapBitsData+sizeof(std::uintptr_t)>v.MapSize || v.MapInlineBits+sizeof(std::uint32_t)*4>v.MapSize ||
            v.PlayerFinished+sizeof(std::int32_t)>v.PlayerSaveSize || v.PlayerInitialized>=v.PlayerSaveSize) return false;
        if (doc.symbol("ProcessEvent.entry").candidates.empty()) return false;
        const auto core=host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);
        const auto sig=host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        if (!core || core->struct_size<offsetof(AnomalyCoreServiceV1,read_memory)+sizeof(core->read_memory) || !core->read_memory) return false;
        const auto objects=anomaly::sdk::ResolveProfileSymbol(sig.get(),core.get(),doc,"Objects");
        const auto world=anomaly::sdk::ResolveProfileSymbol(sig.get(),core.get(),doc,"World");
        if (!objects || !world) return false;
        parsed.registry=RipTarget(core.get(),objects.address+v.RegistryLoad,0x05,v.RegistrySubtract);
        parsed.world=RipTarget(core.get(),world.address,0x1D,0);
        if (!parsed.registry || !parsed.world) return false;
        snapshot=std::move(parsed);
        return true;
    } catch (...) { return false; }
}
inline bool ProcessEventEntry(const AnomalyCoreServiceV1* core,std::uintptr_t address) noexcept {
    try {
        for (const auto& candidate:snapshot.document.symbol("ProcessEvent.entry").candidates) {
            std::vector<std::uint8_t> bytes(candidate.entry.size());
            if (bytes.empty() || !anomaly::sdk::Succeeded(core->read_memory(core->user,address,{bytes.data(),bytes.size()}))) continue;
            if (bytes==candidate.entry) return true;
            // The host observer may replace five entry bytes with an E9 jump.
            if (bytes.size()>5 && bytes[0]==0xE9 && std::equal(bytes.begin()+5,bytes.end(),candidate.entry.begin()+5)) return true;
        }
    } catch (...) {}
    return false;
}
}
