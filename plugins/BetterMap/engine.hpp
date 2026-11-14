#pragma once
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/profile.hpp>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

namespace collectible_map {
struct Ref {
    uintptr_t address{};
    AnomalyGenerationHandleV1 handle{};
    explicit operator bool() const { return address && handle.id; }
    bool operator==(const Ref& other) const {
        return address==other.address && handle.id==other.handle.id && handle.generation==other.handle.generation;
    }
};
struct Layout {
    uint32_t object_index{},object_class{},object_name{},super{},property_link{},field_class{},
        field_class_name{},field_name{},property_size{},property_offset{},property_next{},function_flags{},
        function_count{},function_size{},registry_count{},registry_items{},item_stride{},serial{},chunk_size{};
};
struct Param { const char* name; int offset,size; const char* kind; };
struct Bound { Ref function; uint16_t size{}; };
class Engine {
public:
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    Layout layout;
    uintptr_t registry{};
    using Free = void(ANOMALY_CALL*)(void*);
    Free free_memory{};

    bool ResolveProfile(const AnomalyHostApiV1* host, const anomaly::sdk::Profile& document) {
        const auto signatures = anomaly::sdk::Host(host).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        const auto objects_symbol = anomaly::sdk::ResolveProfileSymbol(signatures.get(), core, document, "Objects");
        const auto free_symbol = anomaly::sdk::ResolveProfileSymbol(signatures.get(), core, document, "FreeString");
        if (!objects_symbol || !free_symbol) return false;
        const auto& p = document;
        layout={p.Value("ObjectIndex",512),p.Value("ObjectClass",512),p.Value("ObjectName",512),
            p.Value("Super",512),p.Value("PropertyLink",512),p.Value("FieldClass",512),p.Value("FieldClassName",512),
            p.Value("FieldName",512),p.Value("PropertySize",512),p.Value("PropertyOffset",512),p.Value("PropertyNext",512),
            p.Value("FunctionFlags",512),p.Value("FunctionCount",512),p.Value("FunctionSize",512),
            p.Value("RegistryCount",512),p.Value("RegistryItems",512),p.Value("ItemStride",512),p.Value("Serial",512),p.Value("ChunkSize",65536)};
        if (!layout.chunk_size || layout.item_stride<sizeof(uintptr_t) || layout.serial+sizeof(uint32_t)>layout.item_stride) return false;
        const auto symbol = objects_symbol.address;
        std::array<uint8_t,7> instruction{};
        const auto at=symbol+p.Value("RegistryLoad",65536);
        if (!symbol || !Read(at,instruction) || instruction[0]!=0x48 || instruction[1]!=0x8B || instruction[2]!=0x05) return false;
        int32_t relative{};std::memcpy(&relative,instruction.data()+3,sizeof(relative));
        const auto target=static_cast<int64_t>(at)+instruction.size()+relative-p.Value("RegistrySubtract",65536);
        if (target<0x10000 || target>0x00007FFFFFFFFFF8ll || target%alignof(uintptr_t)) return false;
        registry = static_cast<uintptr_t>(target);
        free_memory = reinterpret_cast<Free>(free_symbol.address);
        return true;
    }
    bool ReadBytes(uintptr_t at,AnomalyMutableByteSpanV1 bytes) const {
        constexpr uintptr_t maximum=0x00007FFFFFFFFFFFull;
        return at>=0x10000 && at<=maximum && bytes.size<=maximum-at && core && core->read_memory &&
            anomaly::sdk::Succeeded(core->read_memory(core->user,at,bytes));
    }
    template<class T> bool Read(uintptr_t at,T& value) const {
        return ReadBytes(at,{reinterpret_cast<uint8_t*>(&value),sizeof(value)});
    }
    Ref Resolve(AnomalyGenerationHandleV1 handle) const {
        AnomalyUe5ObjectSnapshotV1 object{sizeof(object)};
        if (!registry || !layout.chunk_size || !handle.id || !objects || !objects->snapshot_by_handle ||
            !anomaly::sdk::Succeeded(objects->snapshot_by_handle(objects->user,handle,&object))) return {};
        const auto index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
        uintptr_t chunks{},chunk{},address{};uint32_t count{},serial{};int32_t actual{};
        if (!Read(registry+layout.registry_count,count) || index>=count || count>16*1024*1024 ||
            !Read(registry+layout.registry_items,chunks) || !Read(chunks+sizeof(uintptr_t)*(index/layout.chunk_size),chunk) ||
            !Read(chunk+layout.item_stride*(index%layout.chunk_size),address) ||
            !Read(chunk+layout.item_stride*(index%layout.chunk_size)+layout.serial,serial) || serial!=ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
            !Read(address+layout.object_index,actual) || actual!=static_cast<int32_t>(index)) return {};
        return {address,handle};
    }
    Ref Reference(uintptr_t address) const {
        int32_t index{};AnomalyUe5ObjectSnapshotV1 object{sizeof(object)};
        if (!address || !Read(address+layout.object_index,index) || index<0 || !objects || !objects->snapshot_at ||
            !anomaly::sdk::Succeeded(objects->snapshot_at(objects->user,static_cast<uint32_t>(index),&object))) return {};
        const auto result=Resolve(object.handle);
        return result.address==address ? result : Ref{};
    }
    Ref Find(std::string_view path) const {
        AnomalyGenerationHandleV1 handle{};
        if (!objects || !objects->find_exact || !anomaly::sdk::Succeeded(objects->find_exact(objects->user,anomaly::sdk::StringView(path),&handle))) return {};
        return Resolve(handle);
    }
    std::string Name(uintptr_t address) {
        uint32_t id{};if (!Read(address,id)) return {};
        return NameById(id);
    }
    std::string NameById(uint32_t id) {
        const auto cached=names_.find(id);if(cached!=names_.end())return cached->second;
        std::array<char,512> text{};size_t size=text.size();
        if (!names || !names->resolve_utf8 || !anomaly::sdk::Succeeded(names->resolve_utf8(names->user,id,text.data(),&size)) || !size || size>text.size()) return {};
        const auto length=std::find(text.begin(),text.begin()+size,'\0')-text.begin();
        return names_.emplace(id,std::string(text.data(),static_cast<size_t>(length))).first->second;
    }
    bool IsA(Ref object,std::string_view kind) {
        uintptr_t cls{};if(!object || !Read(object.address+layout.object_class,cls))return false;
        for(int depth=0;cls && depth<32;++depth) {
            if(Name(cls+layout.object_name)==kind)return true;
            uintptr_t parent{};if(!Read(cls+layout.super,parent) || cls==parent)break;cls=parent;
        }
        return false;
    }
    int Field(uintptr_t type,std::string_view name,int size,std::string_view kind) {
        const auto key=std::make_tuple(type,std::string(name),size,std::string(kind));
        const auto cached=fields_.find(key);if(cached!=fields_.end())return cached->second;
        for(int depth=0;type && depth<32;++depth) {
            uintptr_t field{};if(!Read(type+layout.property_link,field))return -1;
            for(int count=0;field && count<2048;++count) {
                if(Name(field+layout.field_name)==name) {
                    uintptr_t field_class{};int32_t actual_size{},offset{};
                    if(!Read(field+layout.field_class,field_class))return -1;
                    const auto actual=Name(field_class+layout.field_class_name);
                    if((actual!=kind && !(kind=="EnumProperty" && actual=="ByteProperty")) ||
                       !Read(field+layout.property_size,actual_size) || actual_size!=size ||
                       !Read(field+layout.property_offset,offset) || offset<0 || offset>0x20000)return -1;
                    fields_.emplace(key,offset);return offset;
                }
                uintptr_t next{};if(!Read(field+layout.property_next,next) || next==field)break;field=next;
            }
            uintptr_t parent{};if(!Read(type+layout.super,parent) || parent==type)break;type=parent;
        }
        return -1;
    }
    template<class T> bool Member(Ref object,const char* name,const char* kind,T& value) {
        uintptr_t type{};if(!object || !Read(object.address+layout.object_class,type))return false;
        const auto at=Field(type,name,sizeof(T),kind);return at>=0 && Read(object.address+at,value);
    }
    Bound Bind(const char* package,const char* owner,const char* name,uint8_t count,uint16_t size,
               std::initializer_list<Param> params) {
        const auto function=Find(std::string("/Script/")+package+"."+owner+"."+name);
        uint8_t actual_count{};uint16_t actual_size{};uint32_t flags{};
        if(!IsA(function,"Function") || Name(function.address+layout.object_name)!=name ||
           !Read(function.address+layout.function_count,actual_count) || actual_count!=count ||
           !Read(function.address+layout.function_size,actual_size) || actual_size!=size ||
           !Read(function.address+layout.function_flags,flags) || !(flags&0x400) || (flags&0x40))return {};
        for(const auto& p:params)if(p.offset+p.size>actual_size || Field(function.address,p.name,p.size,p.kind)!=p.offset)return {};
        return {function,actual_size};
    }
    bool Call(Ref object,const Bound& function,void* params,size_t size) const {
        return object && function.function && function.size<=size && events && events->invoke &&
            anomaly::sdk::Succeeded(events->invoke(events->user,object.handle,function.function.handle,params,size));
    }
private:
    std::map<uint32_t,std::string> names_;
    std::map<std::tuple<uintptr_t,std::string,int,std::string>,int> fields_;
};
}
