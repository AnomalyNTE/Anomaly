#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/profile.hpp>
#include <array>
#include <cstring>

namespace taxi::encounter {
// Optional native capability. Resolving it never enables an encounter or changes
// a player; callers must use these addresses only on the game's thread.
struct Resolved {
    std::uintptr_t exec{}, getter{}, selector{}, free{};
    std::uint32_t offset{};
    std::uint32_t result_size{},subpath_offset{};
    explicit operator bool() const noexcept { return getter && selector && free && offset; }
};

inline bool Read(const AnomalyCoreServiceV1* core, std::uintptr_t address,
                 void* destination, std::size_t size) noexcept {
    return core && core->read_memory && address >= 0x10000 &&
        address <= 0x00007FFFFFFFFFFFull-size &&
        anomaly::sdk::Succeeded(core->read_memory(core->user,address,
            {static_cast<std::uint8_t*>(destination),size}));
}
template<class T> inline bool Read(const AnomalyCoreServiceV1* core,
                                    std::uintptr_t address, T& value) noexcept {
    return Read(core,address,&value,sizeof(value));
}
inline std::uintptr_t DirectCall(const AnomalyCoreServiceV1* core,
                                std::uintptr_t address) noexcept {
    std::array<std::uint8_t,5> bytes{};
    if (!Read(core,address,bytes) || bytes[0]!=0xE8) return 0;
    std::int32_t relative{};std::memcpy(&relative,bytes.data()+1,sizeof(relative));
    const auto target=static_cast<std::int64_t>(address)+5+relative;
    return target>=0x10000 && target<=0x00007FFFFFFFFFFFull
        ? static_cast<std::uintptr_t>(target) : 0;
}
inline bool Displacement(const AnomalyCoreServiceV1* core,std::uintptr_t address,
                         std::array<std::uint8_t,3> opcode,std::uint32_t expected,
                         bool trailing_zero=false) noexcept {
    std::array<std::uint8_t,8> bytes{};
    if(!Read(core,address,bytes) || !std::equal(opcode.begin(),opcode.end(),bytes.begin())) return false;
    std::uint32_t value{};std::memcpy(&value,bytes.data()+3,sizeof(value));
    return value==expected && (!trailing_zero || bytes[7]==0);
}
inline std::uintptr_t RipSlot(const AnomalyCoreServiceV1* core,std::uintptr_t instruction) noexcept {
    std::array<std::uint8_t,7> bytes{};
    if(!Read(core,instruction,bytes) || bytes[0]!=0x48 || bytes[1]!=0x8B || bytes[2]!=0x0D) return 0;
    std::int32_t relative{};std::memcpy(&relative,bytes.data()+3,sizeof(relative));
    const auto result=static_cast<std::int64_t>(instruction)+7+relative;
    return result>=0x10000 && result<=0x00007FFFFFFFFFFFull && result%8==0
        ? static_cast<std::uintptr_t>(result) : 0;
}
// Validate the native call/allocator chain after the public SDK has resolved
// unique getter and selector candidates. This routine never scans code.
inline Resolved ValidateSelection(const AnomalyCoreServiceV1* core,const anomaly::sdk::Profile& doc,
                                  std::uintptr_t getter,std::uintptr_t selector) noexcept {
    try {
        if(doc.game!="nte" || doc.module!="HTGame.exe" || doc.section!=".text" || doc.contract!="autotaxi-v1") return {};
        if(!getter || !selector ||
            !anomaly::sdk::MatchesProfileEntry(core,doc,"Encounter.getter",getter) ||
            !anomaly::sdk::MatchesProfileEntry(core,doc,"Encounter.selector",selector)) return {};
        const auto value=[&](std::string_view name) {return doc.Value(name,65536);};
        if(!anomaly::sdk::MatchesProfileEntry(core,doc,"Encounter.getterCleanup",getter+value("EncounterGetterCleanup"))) return {};
        const auto free=DirectCall(core,getter+value("EncounterGetterFreeCall"));
        if(!free ||
            !anomaly::sdk::MatchesProfileEntry(core,doc,"Encounter.free",free) ||
            !anomaly::sdk::MatchesProfileEntry(core,doc,"Encounter.freeAllocatorCheck",free+value("EncounterFreeAllocatorCheck")) ||
            !anomaly::sdk::MatchesProfileEntry(core,doc,"Encounter.freeDispatch",free+value("EncounterFreeDispatch"))) return {};
        const auto slot=RipSlot(core,free+value("EncounterFreeAllocatorLoadA"));
        if(!slot || RipSlot(core,free+value("EncounterFreeAllocatorLoadB"))!=slot) return {};
        const auto offset=doc.Value("EncounterOverride",0x20000,alignof(std::uint64_t));
        if(!offset || !Displacement(core,selector+value("EncounterOffsetProbeA"),{0x41,0x8B,0x86},offset) ||
            !Displacement(core,selector+value("EncounterOffsetProbeB"),{0x4D,0x39,0xA6},offset) ||
            !Displacement(core,selector+value("EncounterOffsetProbeC"),{0x49,0x83,0xBE},offset,true) ||
            !Displacement(core,selector+value("EncounterOffsetProbeD"),{0x49,0x89,0x86},offset)) return {};
        Resolved resolved{0,getter,selector,free,offset};
        // Optional direct selection ABI: the selector constructs a soft-class
        // path at this LEA, matching the native caller's temporary and cleanup.
        const auto size=doc.layout.find("EncounterResultSize"),path=doc.layout.find("EncounterSubPathOffset"),witness=doc.layout.find("EncounterResultPathWitness");
        if(size!=doc.layout.end() && path!=doc.layout.end() && witness!=doc.layout.end() && witness->second<=65536 && size->second<=256 &&
           path->second+16<=size->second && path->second<=127) {
            std::array<std::uint8_t,4> lea{};
            if(Read(core,selector+witness->second,lea) &&
               lea==std::array<std::uint8_t,4>{0x48,0x8D,0x4A,static_cast<std::uint8_t>(path->second)}) {
                resolved.result_size=size->second;resolved.subpath_offset=path->second;
            }
        }
        return resolved;
    } catch(...) { return {}; }
}
// Run after the normal reflection checks for this exact getter and its return
// property. No signatures are scanned in this per-player confirmation.
inline bool ValidateGetter(const AnomalyCoreServiceV1* core,const anomaly::sdk::Profile& doc,
                           const Resolved& resolved,std::uintptr_t function) noexcept {
    try {
        if(!resolved || !function) return false;
        std::uintptr_t native{};std::uint32_t flags{};std::uint8_t count{};std::uint16_t size{};
        return Read(core,function+doc.Value("EncounterFunctionNative",512,8),native) &&
            anomaly::sdk::MatchesProfileEntry(core,doc,"Encounter.exec",native) &&
            DirectCall(core,native+doc.Value("EncounterGetterCall",4096))==resolved.getter &&
            DirectCall(core,native+doc.Value("EncounterFreeCallA",4096))==resolved.free &&
            DirectCall(core,native+doc.Value("EncounterFreeCallB",4096))==resolved.free &&
            Read(core,function+doc.Value("FunctionFlags",512),flags) && (flags&0x400) && !(flags&0x40) &&
            Read(core,function+doc.Value("FunctionCount",512),count) && count==1 &&
            Read(core,function+doc.Value("FunctionSize",512),size) && size==16;
    } catch(...) {return false;}
}
// The exec wrapper frees TArray buffers through this exact game allocator.
// Do not use CRT free/delete or pass caller-owned storage to the native getter:
// its first instructions zero the output TArray and discard that storage.
inline bool FreeArray(const Resolved& resolved,std::uintptr_t memory) noexcept {
    if(!memory) return true;
    if(!resolved || memory<0x10000 || memory>0x00007FFFFFFFFFFFull) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*)>(resolved.free)(reinterpret_cast<void*>(memory));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
inline bool Select(const Resolved& resolved,std::uintptr_t player,void* output,std::size_t size) noexcept {
    if(!resolved || !player || !output || !resolved.result_size || size!=resolved.result_size) return false;
    __try {
        return reinterpret_cast<void*(__fastcall*)(void*,void*)>(resolved.selector)(reinterpret_cast<void*>(player),output)==output;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
