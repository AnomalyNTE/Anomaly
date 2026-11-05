#pragma once
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include "round.hpp"
#include "profile.hpp"

namespace ue {
using namespace anomaly::sdk;
using Clock=std::chrono::steady_clock;
inline std::uint64_t Now() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); }
struct Parameter { const char* name; std::int32_t offset,size; const char* kind; };
struct Engine {
    const AnomalyHostApiV1* api{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5WorldServiceV1* worlds{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyUe5BuildServiceV1* build{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    std::shared_ptr<const claw::compat::Snapshot> profile;
    const claw::compat::Layout& layout()const{return profile->layout;}
    std::uintptr_t registry{},world_slot{};
    std::map<std::string,AnomalyGenerationHandleV1> functions;
    std::map<std::tuple<std::uint64_t,std::uint64_t,std::string,int,std::string>,int> fields;

    template<class T> const T* Query(const char* id) {
        const auto* p=Host(api).Query<T>(id).get(); return p && p->struct_size>=sizeof(T)?p:nullptr;
    }
    void Refresh() {
        core=Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);
        names=Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID);
        objects=Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
        worlds=Query<AnomalyUe5WorldServiceV1>(ANOMALY_UE5_WORLD_SERVICE_V1_ID);
        framework=Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID);
        build=Query<AnomalyUe5BuildServiceV1>(ANOMALY_UE5_BUILD_SERVICE_V1_ID);
        events=Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID);
        session=Host(api).Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
        if(session&&session->struct_size<offsetof(AnomalyNteSessionServiceV1,snapshot)+sizeof(session->snapshot))session=nullptr;
        player=Host(api).Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get();
        if(player&&player->struct_size<offsetof(AnomalyNtePlayerServiceV1,snapshot)+sizeof(player->snapshot))player=nullptr;
    }
    bool ReadBytes(std::uintptr_t address,void* data,std::size_t size) const {
        return address>0x10000 && size && size<=64*1024*1024 && address<=0x00007FFFFFFFFFFFull-size && core && core->read_memory &&
            core->read_memory(core->user,address,{static_cast<std::uint8_t*>(data),size}).code==ANOMALY_STATUS_V1_OK;
    }
    template<class T> bool Read(std::uintptr_t at,T& value)const { return ReadBytes(at,&value,sizeof(value)); }
    std::string Name(claw::Name value)const {
        if(!value.id || !names || !names->resolve_utf8)return {};
        std::array<char,512> out{}; std::size_t size=out.size();
        if(names->resolve_utf8(names->user,value.id,out.data(),&size).code!=ANOMALY_STATUS_V1_OK || !size || size>out.size())return {};
        std::string text(out.data(),std::find(out.begin(),out.end(),'\0')-out.begin());
        if(value.number)text+="_"+std::to_string(value.number-1);
        return text;
    }
    std::string NameAt(std::uintptr_t at)const {claw::Name name{};return Read(at,name)?Name(name):std::string{};}
    bool IsA(std::uintptr_t object,std::string_view name)const {
        std::uintptr_t cls{}; if(!Read(object+layout().ObjectClass,cls))return false;
        for(int depth=0;cls && depth<48;++depth) {
            if(NameAt(cls+layout().ObjectName)==name)return true;
            std::uintptr_t parent{};if(!Read(cls+layout().Super,parent) || parent==cls)break; cls=parent;
        }
        return false;
    }
    std::uintptr_t Resolve(AnomalyGenerationHandleV1 h)const {
        if(!registry || !objects || !objects->snapshot_by_handle || !h.id)return 0;
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if(objects->snapshot_by_handle(objects->user,h,&snapshot).code!=ANOMALY_STATUS_V1_OK)return 0;
        const auto index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(h);
        std::uintptr_t chunks{},chunk{},object{};std::uint32_t count{},serial{};std::int32_t actual{};
        if(!Read(registry+layout().RegistryCount,count) || count>16*1024*1024 || index>=count || !Read(registry+layout().RegistryItems,chunks) ||
           !Read(chunks+sizeof(std::uintptr_t)*(index/layout().ChunkSize),chunk) || !Read(chunk+layout().ItemStride*(index%layout().ChunkSize),object) ||
           !Read(chunk+layout().ItemStride*(index%layout().ChunkSize)+layout().Serial,serial) || serial!=ANOMALY_UE5_OBJECT_HANDLE_SERIAL(h) ||
           !Read(object+layout().ObjectIndex,actual) || actual!=static_cast<std::int32_t>(index))return 0;
        return object;
    }
    AnomalyGenerationHandleV1 Handle(std::uintptr_t address)const {
        std::int32_t index{}; AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if(!objects || !objects->snapshot_at || !Read(address+layout().ObjectIndex,index) || index<0 ||
           objects->snapshot_at(objects->user,static_cast<std::uint32_t>(index),&snapshot).code!=ANOMALY_STATUS_V1_OK || Resolve(snapshot.handle)!=address)return {};
        return snapshot.handle;
    }
    std::uintptr_t Find(const std::string& path) {
        if(!objects || !objects->find_exact)return 0;
        auto& h=functions[path];if(const auto v=Resolve(h))return v;
        h={};return objects->find_exact(objects->user,StringView(path),&h).code==ANOMALY_STATUS_V1_OK?Resolve(h):0;
    }
    bool Kind(std::uintptr_t field,std::string_view expected)const {
        std::uintptr_t cls{};
        if(!Read(field+layout().FieldClass,cls))return false;
        const auto actual=NameAt(cls+layout().FieldClassName);
        return actual==expected||(expected=="EnumProperty"&&actual=="ByteProperty");
    }
    int Offset(std::uintptr_t cls,std::string_view name,int bytes,std::string_view kind)const {
        for(int depth=0;cls&&depth<48;++depth) {
            std::uintptr_t field{};if(!Read(cls+layout().PropertyLink,field))return -1;
            for(int n=0;field&&n<2048;++n) {
                if(NameAt(field+layout().FieldName)==name) {
                    int size{},offset{};
                    return Kind(field,kind)&&Read(field+layout().PropertySize,size)&&size==bytes&&
                        Read(field+layout().PropertyOffset,offset)&&offset>=0&&offset<0x20000?offset:-1;
                }
                std::uintptr_t next{};if(!Read(field+layout().PropertyNext,next)||next==field)break;field=next;
            }
            if(!Read(cls+layout().Super,cls))break;
        }
        return -1;
    }
    bool Property(std::uintptr_t cls,std::string_view name,int offset,int size,std::string_view kind)const {
        return offset>=0&&Offset(cls,name,size,kind)==offset;
    }
    int Field(std::uintptr_t object,const char* name,int bytes,std::string_view kind) {
        std::uintptr_t cls{};if(!Read(object+layout().ObjectClass,cls))return -1;
        const auto h=Handle(cls);if(!h.id)return -1;
        const auto key=std::tuple(h.id,h.generation,std::string(name),bytes,std::string(kind));
        if(const auto it=fields.find(key);it!=fields.end())return it->second;
        const auto offset=Offset(cls,name,bytes,kind);
        if(offset>=0)fields[key]=offset;
        return offset;
    }
    std::uintptr_t Child(std::uintptr_t object,const char* name) {
        const auto at=Field(object,name,sizeof(std::uintptr_t),"ObjectProperty");std::uintptr_t p{};
        return at>=0&&Read(object+at,p)?p:0;
    }
    std::uintptr_t Function(const char* package,const char* owner,const char* name,int count,int size,std::initializer_list<Parameter> parameters,std::uint32_t flags=0x400) {
        const auto fn=Find(std::string("/Script/")+package+"."+owner+"."+name);
        std::uint8_t actual_count{};std::uint16_t actual_size{};std::uint32_t actual_flags{};
        if(!fn||!IsA(fn,"Function")||NameAt(fn+layout().ObjectName)!=name||!Read(fn+layout().FunctionCount,actual_count)||actual_count!=count||
           !Read(fn+layout().FunctionSize,actual_size)||actual_size!=size||!Read(fn+layout().FunctionFlags,actual_flags)||(actual_flags&flags)!=flags)return 0;
        for(const auto& p:parameters)if(!Property(fn,p.name,p.offset,p.size,p.kind))return 0;
        return fn;
    }
    bool Live(std::uintptr_t object,const char* type)const {
        std::uint32_t flags{};return IsA(object,type) && Read(object+layout().ObjectFlags,flags) && !(flags&0x30) && Handle(object).id;
    }
    std::uintptr_t World(AnomalyGenerationHandleV1* epoch=nullptr)const {
        if(epoch)*epoch={};
        if(!world_slot||!worlds||!worlds->current||!worlds->snapshot)return 0;
        AnomalyGenerationHandleV1 h{},after{};
        AnomalyUe5WorldSnapshotV1 snapshot{sizeof(snapshot)};
        std::uintptr_t world{},check{};claw::Name name{};
        // The public world handle is a service token (id=1 in the NTE
        // adapter), not an encoded object index/serial. Resolve the pointer
        // through the learned GWorld recipe, then cross-check both domains.
        if(worlds->current(worlds->user,&h).code!=0||!h.id||!h.generation||
           worlds->snapshot(worlds->user,h,&snapshot).code!=0||snapshot.handle.id!=h.id||snapshot.handle.generation!=h.generation||
           !Read(world_slot,world)||!Live(world,"World")||!Read(world+layout().ObjectName,name)||
           (snapshot.name_id&&snapshot.name_id!=name.id)||!Read(world_slot,check)||check!=world||
           worlds->current(worlds->user,&after).code!=0||after.id!=h.id||after.generation!=h.generation)return 0;
        if(epoch)*epoch=h;
        return world;
    }
    std::uintptr_t OwningWorld(std::uintptr_t object) {
        for(int depth=0;object && depth<12;++depth) {
            if(IsA(object,"World"))return object;
            if(IsA(object,"Level"))return Child(object,"OwningWorld");
            if(!Read(object+layout().ObjectOuter,object))break;
        }
        return 0;
    }
    bool Features()const {
        if(!build || !build->feature_state)return false;
        for(const auto id:{"ue5.objects","ue5.names","ue5.functions","ue5.process-event"})
            if(build->feature_state(build->user,StringView(id))!=ANOMALY_FEATURE_V1_AVAILABLE)return false;
        return true;
    }
    // The document is loaded once in Load; only a requested Worker resolves it.
    bool Signatures() {
        registry=world_slot=0;
        return profile&&claw::compat::Resolve(api,core,*profile,registry,world_slot);
    }
};

inline bool Invoke(const Engine& e,std::uintptr_t object,std::uintptr_t function,void* parameters) {
    if(!object||!function||!e.events||e.events->struct_size<offsetof(AnomalyUe5ProcessEventServiceV1,invoke)+sizeof(e.events->invoke)||!e.events->invoke)return false;
    const auto receiver=e.Handle(object),method=e.Handle(function);std::uint16_t size{};
    if(!receiver.id||!method.id||!e.Read(function+e.layout().FunctionSize,size))return false;
    return e.events->invoke(e.events->user,receiver,method,parameters,size).code==ANOMALY_STATUS_V1_OK;
}
inline bool Same(AnomalyGenerationHandleV1 a,AnomalyGenerationHandleV1 b) {return a.id==b.id&&a.generation==b.generation;}
inline bool ValidArray(const claw::Array& a,int maximum) {return a.count>=0&&a.count<=maximum&&a.capacity>=a.count&&a.capacity<=maximum&&(!a.count||a.data>0x10000);}
struct SparseMap {
    claw::Array slots{};std::array<std::uint32_t,4> bits{};std::uintptr_t extra{};
    std::int32_t count{},maximum{};
};
inline bool ReadMap(const Engine& e,std::uintptr_t address,SparseMap& map,int maximum_entries,int maximum_bits) {
    const auto& v=e.layout();
    return e.Read(address+v.MapSlots,map.slots)&&e.Read(address+v.MapInlineBits,map.bits)&&
        e.Read(address+v.MapBitsData,map.extra)&&e.Read(address+v.MapBitCount,map.count)&&e.Read(address+v.MapBitCapacity,map.maximum)&&
        ValidArray(map.slots,maximum_entries)&&map.count>=map.slots.count&&map.count<=maximum_entries&&
        map.maximum>=map.count&&map.maximum<=maximum_bits&&(map.extra||map.count<=static_cast<int>(map.bits.size()*32));
}
inline bool ReadWrapper(const Engine& e,const claw::WrapperArray& array,int index,claw::Wrapper& wrapper) {
    return ValidArray(array.items,16)&&index>=0&&index<array.items.count && e.Read(array.items.data+sizeof(claw::Wrapper)*index,wrapper)&&
        ValidArray(wrapper.bytes,4096)&&wrapper.bytes.count>0;
}
inline std::optional<std::string> WrapperName(const Engine& e,const claw::WrapperArray& array,int index) {
    claw::Wrapper wrapper{};
    if(!ReadWrapper(e,array,index,wrapper)||wrapper.type!=15||wrapper.bytes.count>516)return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(wrapper.bytes.count));
    return e.ReadBytes(wrapper.bytes.data,bytes.data(),bytes.size())?claw::DecodeName(bytes):std::nullopt;
}
inline std::optional<std::int32_t> WrapperInteger(const Engine& e,const claw::WrapperArray& array,int index,std::uint8_t type) {
    claw::Wrapper wrapper{};std::int32_t value{};
    if(!ReadWrapper(e,array,index,wrapper)||wrapper.type!=type||wrapper.bytes.count!=4||!e.Read(wrapper.bytes.data,value))return {};
    return value;
}
}
