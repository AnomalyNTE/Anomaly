#include "native_layout.hpp"
// Minimal UE Native bridge adapted from the local AGPL NaiLong bridge.

// All invocation occurs in Anomaly's Game callback domain; no D3D hooks.

#pragma once

#include <anomaly/sdk/plugin.h>

#include <anomaly/sdk/services/core.h>

#include <anomaly/sdk/services/interop.h>

#include <anomaly/sdk/services/ue5.h>

#include <windows.h>

#include <array>

#include <algorithm>

#include <cstdint>

#include <cstring>

#include <string>

#include <unordered_map>

#include <vector>

namespace ue {
using nailong_profile::native_layout;
using nailong_profile::compatibility;

inline const AnomalyCoreServiceV1* g_core=nullptr;

inline const AnomalySignatureServiceV1* g_signature=nullptr;

inline const AnomalyUe5ObjectsServiceV1* g_objects=nullptr;

inline const AnomalyUe5NamesServiceV1* g_names=nullptr;

inline const AnomalyUe5FrameworkServiceV1* g_framework=nullptr;

inline AnomalyStringViewV1 nl_sv(const char* s){return {s,std::strlen(s)};}

inline bool nl_good(AnomalyStatusV1 s){return s.code==ANOMALY_STATUS_V1_OK;}



#define NL_GOBJECTS_RESOLVE_OFFSET 3u

#define NL_GOBJECTS_INSTRUCTION_SIZE 7u





















typedef struct NlObjectRegistry {

    uintptr_t gobjects;

    uintptr_t items;

    uint32_t count;

    uint32_t max_count;

    uint32_t num_chunks;

    uint32_t max_chunks;

    int ready;

} NlObjectRegistry;

static NlObjectRegistry g_reg;



static int nl_read(uintptr_t address, void* out, size_t size) {

    AnomalyMutableByteSpanV1 span;

    if (g_core == NULL || g_core->read_memory == NULL || out == NULL || size == 0u) return 0;

    if (address < 0x10000u) return 0;

    span.data = (uint8_t*)out;

    span.size = size;

    return nl_good(g_core->read_memory(g_core->user, address, span));

}



static int nl_read_ptr(uintptr_t address, uintptr_t* out) {

    return nl_read(address, out, sizeof(*out));

}



static int nl_read_u32(uintptr_t address, uint32_t* out) {

    return nl_read(address, out, sizeof(*out));

}



static int nl_resolve_gobjects(void) {

    uintptr_t instruction = 0u;

    uint32_t raw = 0u;

    int32_t displacement;

    intptr_t resolved;

    instruction=compatibility.Address("Objects");
    if (!instruction) return 0;

    if (!nl_read_u32(instruction + NL_GOBJECTS_RESOLVE_OFFSET, &raw)) return 0;

    memcpy(&displacement, &raw, sizeof(displacement));

    resolved = (intptr_t)instruction + (intptr_t)NL_GOBJECTS_INSTRUCTION_SIZE +

        (intptr_t)displacement + -static_cast<intptr_t>(native_layout.RegistrySubtract);

    if (resolved <= 0) return 0;

    g_reg.gobjects = (uintptr_t)resolved;

    return 1;

}



static int nl_refresh_objects(void) {

    uintptr_t items = 0u;

    uint32_t count = 0u;

    uint32_t max_count = 0u;

    uint32_t num_chunks = 0u;

    uint32_t max_chunks = 0u;

    if (g_reg.gobjects == 0u && !nl_resolve_gobjects()) return 0;

    if (!nl_read_ptr(g_reg.gobjects + native_layout.NL_OBJ_ITEMS_OFFSET, &items)) return 0;

    if (!nl_read_u32(g_reg.gobjects + native_layout.NL_OBJ_COUNT_OFFSET, &count)) return 0;

    if (!nl_read_u32(g_reg.gobjects + native_layout.NL_OBJ_MAX_COUNT_OFFSET, &max_count)) return 0;

    if (!nl_read_u32(g_reg.gobjects + native_layout.NL_OBJ_MAX_CHUNKS_OFFSET, &max_chunks)) return 0;

    if (!nl_read_u32(g_reg.gobjects + native_layout.NL_OBJ_NUM_CHUNKS_OFFSET, &num_chunks)) return 0;

    if (items == 0u || count == 0u || max_count < count ||

        num_chunks == 0u || num_chunks > 4096u || max_chunks < num_chunks) {

        return 0;

    }

    g_reg.items = items;

    g_reg.count = count;

    g_reg.max_count = max_count;

    g_reg.num_chunks = num_chunks;

    g_reg.max_chunks = max_chunks;

    g_reg.ready = 1;

    return 1;

}



static int nl_object_slot(uint32_t index, uintptr_t* object, uint32_t* serial) {

    uint32_t page;

    uint32_t slot;

    uintptr_t chunk = 0u;

    uintptr_t item;

    uintptr_t value = 0u;

    uint32_t number = 0u;

    if (object == NULL) return 0;

    *object = 0u;

    if (serial != NULL) *serial = 0u;

    if (!g_reg.ready || index >= g_reg.count) return 0;

    page = index / native_layout.NL_OBJ_CHUNK_SIZE;

    slot = index % native_layout.NL_OBJ_CHUNK_SIZE;

    if (page >= g_reg.num_chunks) return 0;

    if (!nl_read_ptr(g_reg.items + ((uintptr_t)page * sizeof(uintptr_t)), &chunk)) return 0;

    if (chunk == 0u || (chunk & 7u) != 0u) return 0;

    item = chunk + ((uintptr_t)slot * native_layout.NL_OBJ_ITEM_STRIDE);

    if (!nl_read_ptr(item + native_layout.NL_OBJ_OBJECT_OFFSET, &value)) return 0;

    if (!nl_read_u32(item + native_layout.NL_OBJ_SERIAL_OFFSET, &number)) return 0;

    *object = value;

    if (serial != NULL) *serial = number;

    return 1;

}

struct Ref { std::uintptr_t ptr=0; std::uint32_t index=0,serial=0; };

inline std::uintptr_t process_event=0;

inline std::unordered_map<std::string,Ref> paths;

inline std::string last_error;

inline bool InGame(){return g_framework && g_framework->is_game_thread && g_framework->is_game_thread(g_framework->user);}

inline bool Capture(std::uintptr_t p,Ref& out){

    std::uint32_t i=0,s=0,flags=0; std::uintptr_t v=0;

    if(!p||!nl_read_u32(p+native_layout.ObjectFlags,&flags)||(flags&0x18000)||!nl_read_u32(p+native_layout.ObjectIndex,&i)) return false;

    if(!nl_object_slot(i,&v,&s)){

        // AddComponentByClass can append a UObject after the previous registry snapshot.

        // Refresh once before rejecting the new component identity.

        if(!nl_refresh_objects()||!nl_object_slot(i,&v,&s)) return false;

    }

    if(v!=p||!s) return false;

    out={p,i,s};return true;

}

// Unknown registry reads are not interpreted as proof that an actor was destroyed.

inline int Alive(const Ref& r){

    if(!r.ptr)return 0;

    std::uintptr_t p=0; std::uint32_t s=0;

    if(r.index>=g_reg.count) return 0;

    if(!nl_object_slot(r.index,&p,&s))return -1;

    if(p!=r.ptr||s!=r.serial)return 0;

    std::uint32_t flags=0;if(!nl_read_u32(p+native_layout.ObjectFlags,&flags))return -1;

    return (flags&0x18000)?0:1;

}

inline std::uintptr_t Find(const char* path){

    auto it=paths.find(path); if(it!=paths.end()&&Alive(it->second)==1)return it->second.ptr;

    if(!g_objects||!g_objects->find_exact)return 0;

    AnomalyGenerationHandleV1 h{};

    if(!nl_good(g_objects->find_exact(g_objects->user,nl_sv(path),&h))){

        std::string alt(path);auto dot=alt.find_last_of('.');if(dot==std::string::npos)return 0;alt[dot]=':';

        if(!nl_good(g_objects->find_exact(g_objects->user,nl_sv(alt.c_str()),&h)))return 0;

    }

    std::uintptr_t p=0;std::uint32_t serial=0;

    if(!std::uint32_t(h.id)||!nl_object_slot(ANOMALY_UE5_OBJECT_HANDLE_INDEX(h),&p,&serial))return 0;

    if(ANOMALY_UE5_OBJECT_HANDLE_SERIAL(h)&&serial!=ANOMALY_UE5_OBJECT_HANDLE_SERIAL(h))return 0;

    paths[path]={p,ANOMALY_UE5_OBJECT_HANDLE_INDEX(h),serial};return p;

}

inline bool ExecutableAddress(std::uintptr_t address) {

    if(address < 0x10000u)return false;

    MEMORY_BASIC_INFORMATION info{};

    if(VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))!=sizeof(info))return false;

    if(info.State!=MEM_COMMIT||(info.Protect&PAGE_GUARD))return false;

    const auto p=info.Protect&0xffu;

    return p==PAGE_EXECUTE||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;

}

inline bool Ready(){

    if(!InGame()||!g_signature||!nl_refresh_objects())return false;

    if(ExecutableAddress(process_event))return true;

    process_event=0;

    // Resolve from the controlled UObject vtable; validate its complete prologue
    // or the host observer detour and surviving entry bytes against the Profile.
    auto cls=Find("/Script/CoreUObject.Object");std::uintptr_t cdo=0,vt=0,target=0;

    if(!cls||!nl_read_ptr(cls+native_layout.ClassDefaultObject,&cdo)||!nl_read_ptr(cdo,&vt)||!nl_read_ptr(vt+native_layout.ProcessEventSlot,&target)||!ExecutableAddress(target)||!compatibility.MatchesEntry(g_core,"ProcessEvent.entry",target,true)){

        last_error="ProcessEvent unavailable: UObject vtable entry validation failed";return false;

    }

    process_event=target;return true;

}inline bool Call(std::uintptr_t obj,const char* path,void* parameters,std::size_t n){

    if(!InGame()||!process_event){last_error="Native call rejected outside Game domain";return false;}

    Ref r;if(!Capture(obj,r)){last_error="stale UObject";return false;}

    auto f=Find(path);std::uint32_t flags=0;std::uint16_t size=0;

    if(!f){last_error=std::string("function unavailable: ")+path;return false;}

    if(!nl_read_u32(f+native_layout.FunctionFlags,&flags)||!nl_read(f+native_layout.FunctionSize,&size,sizeof(size))||!(flags&0x400)||size!=n){

        last_error=std::string("Native ABI mismatch: ")+path+" expected="+std::to_string(n)+" actual="+std::to_string(size);return false;

    }

    using Fn=void(__fastcall*)(void*,void*,void*);

    reinterpret_cast<Fn>(process_event)(reinterpret_cast<void*>(obj),reinterpret_cast<void*>(f),parameters);return true;

}

inline bool IsClass(std::uintptr_t obj,std::uintptr_t base){

    std::uintptr_t cls=0;if(!base||!nl_read_ptr(obj+native_layout.ObjectClass,&cls))return false;

    for(int i=0;cls&&i<40;++i){if(cls==base)return true;if(!nl_read_ptr(cls+native_layout.StructSuper,&cls))return false;}return false;

}

inline bool Visible(std::uintptr_t c,bool& v){std::uint8_t p=0;if(!Call(c,"/Script/Engine.SceneComponent.IsVisible",&p,1))return false;v=p!=0;return true;}

inline bool SetVisible(std::uintptr_t c,bool v){std::array<std::uint8_t,2> p{std::uint8_t(v),0};return Call(c,"/Script/Engine.SceneComponent.SetVisibility",p.data(),p.size());}

inline bool Destroy(std::uintptr_t c){auto p=c;return Call(c,"/Script/Engine.ActorComponent.K2_DestroyComponent",&p,8);}

inline bool IsComponentDestroyed(std::uintptr_t c){
    std::uint8_t result=0;
    return Call(c,"/Script/Engine.ActorComponent.IsBeingDestroyed",&result,1)&&result!=0;
}

inline bool Deactivate(std::uintptr_t c){return Call(c,"/Script/Engine.ActorComponent.Deactivate",nullptr,0);}

inline bool DetachFromParent(std::uintptr_t c){
    std::array<std::uint8_t,2> p{1,0}; // bMaintainWorldPosition=true
    return Call(c,"/Script/Engine.SceneComponent.DetachFromParent",p.data(),p.size());
}

inline bool MarkRenderStateDirty(std::uintptr_t c){
    return Call(c,"/Script/Engine.ActorComponent.MarkRenderStateDirty",nullptr,0);
}

inline bool ClearAllMeshSections(std::uintptr_t c){
    return Call(c,"/Script/ProceduralMeshComponent.ProceduralMeshComponent.ClearAllMeshSections",nullptr,0);
}

inline bool WasRecentlyRendered(std::uintptr_t c,float tolerance,bool& rendered){
    std::array<std::uint8_t,8> p{};
    std::memcpy(p.data(),&tolerance,4);
    if(!Call(c,"/Script/Engine.PrimitiveComponent.WasRecentlyRendered",p.data(),p.size()))return false;
    rendered=p[4]!=0;return true;
}

inline std::uintptr_t Root(std::uintptr_t actor){std::uintptr_t p=0;return Call(actor,"/Script/Engine.Actor.K2_GetRootComponent",&p,8)?p:0;}

struct alignas(16) Transform {double rotation[4]={0,0,0,1};double translation[3]={0,0,0};double pad0=0;double scale[3]={1,1,1};double pad1=0;};

static_assert(sizeof(Transform)==96);

inline std::uintptr_t Add(std::uintptr_t actor,std::uintptr_t cls,double offset){

    alignas(16) std::array<std::uint8_t,128> p{};Transform t;t.translation[2]=offset;

    std::memcpy(p.data(),&cls,8);p[8]=1;std::memcpy(p.data()+16,&t,96);

    if(!Call(actor,"/Script/Engine.Actor.AddComponentByClass",p.data(),p.size()))return 0;

    std::uintptr_t c=0;std::memcpy(&c,p.data()+120,8);return c;

}

inline bool Attach(std::uintptr_t component,std::uintptr_t root){

    std::array<std::uint8_t,21> p{};std::memcpy(p.data(),&root,8);

    return Call(component,"/Script/Engine.SceneComponent.K2_AttachToComponent",p.data(),p.size())&&p[20];

}

inline bool NoCollision(std::uintptr_t c){std::uint8_t off=0;return Call(c,"/Script/Engine.PrimitiveComponent.SetCollisionEnabled",&off,1)&&Call(c,"/Script/Engine.PrimitiveComponent.SetGenerateOverlapEvents",&off,1);}

struct Array {void* data=nullptr;std::int32_t num=0,max=0;};static_assert(sizeof(Array)==16);

// Finds a reflected property offset by name.
//
// The search walks UP the UClass hierarchy. A field declared on a base class is not
// present in the derived class's own property list, so looking only at the class
// named by `owner` misses everything inherited: `SkeletalMesh` lives on
// USkinnedMeshComponent while callers asked for USkinnedMeshComponent's subclass
// SkinnedMeshComponent and got -1, which surfaced at runtime as
// "skeletal asset identity unavailable" and dropped those NPCs to static.
inline std::int32_t PropertyOffsetOnClass(std::uintptr_t cls,const char* wanted,std::int32_t expectedSize){

    if(!cls||!g_names||!g_names->resolve_utf8)return -1;

    std::uintptr_t property=0;

    if(!nl_read_ptr(cls+native_layout.StructProperties,&property))return -1;

    for(int i=0;property&&i<1024;++i){

        std::uint32_t name=0;if(!nl_read_u32(property+native_layout.PropertyName,&name))return -1;

        char text[128]{};std::size_t n=sizeof(text);

        if(nl_good(g_names->resolve_utf8(g_names->user,name,text,&n))&&std::strcmp(text,wanted)==0){

            std::int32_t size=0,offset=0;

            if(!nl_read(property+native_layout.PropertySize,&size,4)||size!=expectedSize||!nl_read(property+native_layout.PropertyOffset,&offset,4)||offset<40||offset>65536)return -1;

            return offset;

        }

        if(!nl_read_ptr(property+native_layout.PropertyNext,&property))return -1;

    }

    return -1;

}

inline std::int32_t PropertyOffset(const char* owner,const char* wanted,std::int32_t expectedSize){

    std::uintptr_t cls=Find(owner);

    if(!cls)return -1;

    // Climb the super chain (UStruct::SuperStruct at +64).
    for(int depth=0;cls&&depth<32;++depth){

        const auto offset=PropertyOffsetOnClass(cls,wanted,expectedSize);

        if(offset>=0)return offset;

        std::uintptr_t super=0;

        if(!nl_read_ptr(cls+native_layout.StructSuper,&super))break;

        cls=super;

    }

    return -1;

}

inline std::int32_t AttachChildrenOffset(){

    static std::int32_t cached=-1;

    if(cached<0)cached=PropertyOffset("/Script/Engine.SceneComponent","AttachChildren",16);

    return cached;

}

inline std::int32_t AnimationTickOffset(){

    static std::int32_t cached=-1;

    if(cached<0)cached=PropertyOffset("/Script/Engine.SkinnedMeshComponent","VisibilityBasedAnimTickOption",1);

    return cached;

}

inline bool ReadByte(std::uintptr_t addr,std::uint8_t& value){

    return InGame()&&nl_read(addr,&value,1);

}

inline bool WriteByte(std::uintptr_t addr,std::uint8_t value){

    if(!InGame()||!g_core||!g_core->write_memory)return false;

    return nl_good(g_core->write_memory(g_core->user,addr,{&value,1}));

}

inline bool Children(std::uintptr_t c,std::vector<std::uintptr_t>& result){

    const auto offset=AttachChildrenOffset();if(offset<0){last_error="AttachChildren reflection layout unavailable";return false;}

    Array a;if(!nl_read(c+std::uintptr_t(offset),&a,sizeof(a))||a.num<0||a.num>128||a.max<a.num)return false;

    result.resize(a.num);return !a.num||nl_read(reinterpret_cast<std::uintptr_t>(a.data),result.data(),std::size_t(a.num)*8);

}

inline bool ComponentsByClass(std::uintptr_t actor,std::uintptr_t cls,std::vector<Ref>& out){

    if(!actor||!cls)return false;

    alignas(16) std::array<std::uint8_t,24> p{};std::memcpy(p.data(),&cls,8);

    if(!Call(actor,"/Script/Engine.Actor.K2_GetComponentsByClass",p.data(),p.size()))return false;

    Array a{};std::memcpy(&a,p.data()+8,16);

    if(a.num<0||a.num>128||a.max<a.num)return false;

    for(std::int32_t i=0;i<a.num;++i){std::uintptr_t c=0;if(!nl_read_ptr(reinterpret_cast<std::uintptr_t>(a.data)+std::uintptr_t(i)*8,&c))return false;Ref r;if(Capture(c,r))out.push_back(r);}

    return true;

}

inline bool Meshes(std::uintptr_t actor,std::uintptr_t root,std::vector<Ref>& out){

    (void)root;

    auto sk=Find("/Script/Engine.SkeletalMeshComponent"), st=Find("/Script/Engine.StaticMeshComponent");

    if(!sk||!st){last_error="mesh component classes unavailable";return false;}

    if(!ComponentsByClass(actor,sk,out)||!ComponentsByClass(actor,st,out)){last_error="K2_GetComponentsByClass failed";return false;}

    return !out.empty();

}

inline double CapsuleHalfHeight(std::uintptr_t root){

    if(!IsClass(root,Find("/Script/Engine.CapsuleComponent")))return 0;

    float value=0;return Call(root,"/Script/Engine.CapsuleComponent.GetScaledCapsuleHalfHeight",&value,4)?double(value):0;

}

inline bool NumSections(std::uintptr_t c,std::int32_t& out){

    std::array<std::uint8_t,4> p{};if(!Call(c,"/Script/ProceduralMeshComponent.ProceduralMeshComponent.GetNumSections",p.data(),p.size()))return false;std::memcpy(&out,p.data(),4);return true;

}

inline bool SectionVisible(std::uintptr_t c,std::int32_t index,bool& out){

    std::array<std::uint8_t,8> p{};std::memcpy(p.data(),&index,4);if(!Call(c,"/Script/ProceduralMeshComponent.ProceduralMeshComponent.IsMeshSectionVisible",p.data(),p.size()))return false;out=p[4]!=0;return true;

}

inline bool ComponentLocation(std::uintptr_t c,double out[3]){

    std::array<std::uint8_t,24> p{};if(!Call(c,"/Script/Engine.SceneComponent.K2_GetComponentLocation",p.data(),p.size()))return false;std::memcpy(out,p.data(),24);return true;

}inline bool SetMaterial(std::uintptr_t c,std::uint32_t index,std::uintptr_t mat){

    std::array<std::uint8_t,16> p{};std::memcpy(p.data(),&index,4);std::memcpy(p.data()+8,&mat,8);

    return Call(c,"/Script/Engine.PrimitiveComponent.SetMaterial",p.data(),p.size());

}



inline std::uintptr_t VertexMaterial(){

    constexpr char path[]="/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial";

    if(auto m=Find(path))return m;

    auto cls=Find("/Script/Engine.KismetSystemLibrary");std::uintptr_t cdo=0;

    if(!cls||!nl_read_ptr(cls+native_layout.ClassDefaultObject,&cdo)||!cdo)return 0;

    // Exact ABI from the supplied Engine_parameters.hpp. No UObject loading on Render/worker threads.

    std::wstring text=L"/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial";

    Array string{text.data(),std::int32_t(text.size()+1),std::int32_t(text.size()+1)};

    alignas(16) std::array<std::uint8_t,48> make{};std::memcpy(make.data(),&string,16);

    if(!Call(cdo,"/Script/Engine.KismetSystemLibrary.MakeSoftObjectPath",make.data(),make.size()))return 0;

    // This top-level path has no subobject string, hence no owned output FString to leak/free.

    Array subpath{};std::memcpy(&subpath,make.data()+32,16);if(subpath.data||subpath.num||subpath.max){last_error="unexpected material subobject path";return 0;}

    alignas(16) std::array<std::uint8_t,72> convert{};std::memcpy(convert.data(),make.data()+16,32);

    if(!Call(cdo,"/Script/Engine.KismetSystemLibrary.Conv_SoftObjPathToSoftObjRef",convert.data(),convert.size()))return 0;

    alignas(16) std::array<std::uint8_t,48> load{};std::memcpy(load.data(),convert.data()+32,40);

    if(!Call(cdo,"/Script/Engine.KismetSystemLibrary.LoadAsset_Blocking",load.data(),load.size()))return 0;

    std::uintptr_t material=0;std::memcpy(&material,load.data()+40,8);

    return material;

}

} // namespace ue
