#include "anomaly/sdk/cpp.hpp"
#include "localization.hpp"
#include "appearance.hpp"
#include "glider.hpp"
#include "profile.hpp"
#include "cosmetics.hpp"
#include "cosmetic_profiles.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <memory>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace accessory;
using anomaly::sdk::StringView;
using Address = std::uintptr_t;
using GetFn = bool(__fastcall*)(void*, Appearance*);
using CopyFn = NameArray*(__fastcall*)(NameArray*, const NameArray*);
using RefreshFn = void(__fastcall*)(void*);
struct Row {
    ItemKey key;
    std::string id, label, type;
    Address soft_class{};
};
std::string SlotKey(const Row& row) {
    return std::to_string(row.key.part)+":"+row.type;
}
enum class Request { None, Apply, Hide, Restore, RestoreSlot, Reapply, Reload };
enum class CatalogKind { Accessory, Glider };
struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalySignatureServiceV1* signatures{};
    const AnomalyHookServiceV1* hooks{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    AnomalyGenerationHandleV1 hook{}, table{};
    Address world{}, registry{}, refresh{}, copy{}, getter{};
    Address original{}; // Written by Host before it enables the hook, retained until release drains.
    AnomalyGenerationHandleV1 glider_hook{}, glider_table{};
    Address glider_spawn{}, glider_refresh{}, soft_assign{}, glider_original{}, glider_validated_class{};
    std::atomic<bool> glider_enabled{}, glider_fault{};
    std::atomic<unsigned> glider_replacements{};
    GliderPath glider_path;
    std::shared_ptr<const std::vector<Row>> gliders{std::make_shared<const std::vector<Row>>()};
    Request glider_request{};
    ItemKey glider_requested{};
    std::string glider_message{lite_i18n::localizer.Text("glider.wait_catalog", "Waiting for the glider catalog.")};
    bool glider_ready{};
    std::uint32_t glider_table_name{}, appearance_struct{}, glide_struct{};
    std::atomic<Address> pawn{};
    std::atomic<std::uint32_t> game_thread{};
    std::atomic<bool> enabled{}, applied{}, callback_fault{};
    std::atomic<unsigned> replacements{};
    // Keep the composed decoration list stable during appearance refreshes.
    std::array<Name,MaxAccessories> decoration_snapshot{};
    int decoration_snapshot_count{};
    bool decoration_snapshot_valid{};
    std::mutex mutex;
    cosmetic_profiles::Profiles profiles;
    std::string active_character;
    std::string last_character;
    bool auto_accessory_pending{}, auto_glider_pending{};
    bool auto_accessory_request{}, auto_glider_request{};
    unsigned auto_accessory_attempts{}, auto_glider_attempts{};
    std::chrono::steady_clock::time_point auto_accessory_due{}, auto_glider_due{};
    std::vector<ItemKey> accessory_choices;
    bool hide_all{};
    std::shared_ptr<const std::vector<Row>> rows{std::make_shared<const std::vector<Row>>()};
    Request request{};
    ItemKey requested{};
    std::string message{lite_i18n::localizer.Text("cosmetics.wait_services", "Waiting for game services.")}, player, entity_info;
    bool ready{}, waiting_message{true};
    // Fields below are accessed only from Game callbacks.
    std::uint32_t scan_index{}, table_name{}, player_class{}, table_class{}, row_struct{};
    Address validated_class{};
    double discovery_time{};
    // Render-only state.
    char filter[128]{};
    char glider_filter[128]{};
    int category{};
    ItemKey accessory_selection{}, glider_selection{};
};
std::atomic<Context*> active{};
AnomalyStatusV1 Status(std::uint32_t code, std::string message = {}) {
    thread_local std::string status_message;
    status_message=std::move(message);
    return {code, 0, StringView(status_message)};
}
template<class T> const T* Query(const AnomalyHostApiV1* host, const char* id, unsigned version=1) {
    const auto* value=anomaly::sdk::Host(host).Query<T>(id,version).get();
    return value && value->struct_size>=sizeof(T)?value:nullptr;
}
bool ReadBytes(Context& c, Address address, void* data, std::size_t size) {
    return address && c.core->read_memory(c.core->user,address,
        {static_cast<std::uint8_t*>(data),size}).code==ANOMALY_STATUS_V1_OK;
}
template<class T> bool Read(Context& c, Address address, T& value) { return ReadBytes(c,address,&value,sizeof(value)); }
bool Executable(Address address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)) && info.State==MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD|PAGE_NOACCESS)) && (info.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
Address Resolve(Context&, const char* pattern) {
    const auto result=profile::compatibility.Address(pattern);
    return Executable(result)?result:0;
}
Address Rip(Context& c, Address instruction, int displacement, int length, int addend=0) {
    std::int32_t rel{};
    return instruction && Read(c,instruction+displacement,rel)?instruction+length+rel+addend:0;
}
void Message(Context& c,std::string text,bool waiting=false) { std::scoped_lock lock(c.mutex);c.message=std::move(text);c.waiting_message=waiting; }
void LogCosmetics(Context& c,const std::string& text) {
    if(c.core&&c.core->log)c.core->log(c.core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        StringView("SkinChanger cosmetics: "+text));
}
std::string NameText(Context& c, Name name) {
    char text[512]{};std::size_t size=sizeof(text);
    if(!name.index || c.names->resolve_utf8(c.names->user,name.index,text,&size).code!=ANOMALY_STATUS_V1_OK) return {};
    std::string result(text);if(name.number)result+="_"+std::to_string(name.number-1);return result;
}
bool IsA(Context& c,Address object,std::uint32_t type) {
    Address cls{};if(!object||!type||!Read(c,object+profile::native_layout.ObjectClass,cls))return false;
    for(int depth=0;cls&&depth<64;++depth){
        Name name{};if(!Read(c,cls+accessory::profile::native_layout.ObjectName,name))return false;
        if(name.index==type)return true;
        if(!Read(c,cls+profile::native_layout.Super,cls))return false;
    }return false;
}
bool PropertyAt(Context& c,Address cls,const char* name,std::uint32_t offset) {
    Address field{};if(!Read(c,cls+profile::native_layout.PropertyLink,field))return false;
    for(int i=0;field&&i<2048;++i){
        Name id{};std::uint32_t found{};
        if(!Read(c,field+profile::native_layout.FieldName,id)||!Read(c,field+profile::native_layout.PropertyOffset,found))return false;
        if(NameText(c,id)==name)return found==offset;
        if(!Read(c,field+profile::native_layout.PropertyNext,field))return false;
    }return false;
}
Address ObjectAddress(Context& c,AnomalyGenerationHandleV1 handle) {
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if(!handle.id||!static_cast<std::uint32_t>(handle.id)||
       c.objects->snapshot_by_handle(c.objects->user,handle,&snapshot).code!=ANOMALY_STATUS_V1_OK)return 0;
    std::uint32_t index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle),count{},serial{};
    Address chunks{},chunk{},object{};
    if(!Read(c,c.registry+profile::native_layout.RegistryCount,count)||index>=count||
       !Read(c,c.registry+profile::native_layout.RegistryItems,chunks)||!chunks||
       !Read(c,chunks+(index/accessory::profile::native_layout.ChunkSize)*sizeof(uintptr_t),chunk)||!chunk)return 0;
    Address item=chunk+(index%accessory::profile::native_layout.ChunkSize)*profile::native_layout.ItemStride;
    if(!Read(c,item+profile::native_layout.Serial,serial)||serial!=ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle)||!Read(c,item,object))return 0;
    return object;
}
Address LocalPawn(Context& c) {
    Address world{},instance{},players{},local{},controller{},pawn{};std::int32_t count{};
    if(!Read(c,c.world,world)||!world||!Read(c,world+profile::native_layout.WorldGameInstance,instance)||!instance||
       !Read(c,instance+profile::native_layout.LocalPlayers,players)||!players||!Read(c,instance+profile::native_layout.LocalPlayers+profile::native_layout.ArrayCount,count)||count<1||count>4||
       !Read(c,players,local)||!local||!Read(c,local+profile::native_layout.Controller,controller)||!controller||
       !Read(c,controller+profile::native_layout.Pawn,pawn)||!IsA(c,pawn,c.player_class))return 0;
    Address cls{};if(!Read(c,pawn+profile::native_layout.ObjectClass,cls))return 0;
    if(cls!=c.validated_class){
        if(!PropertyAt(c,cls,"CurrentDisplayCharacterID",profile::native_layout.DisplayCharacter))return 0;
        c.validated_class=cls;
    }return pawn;
}
bool ReadCatalog(Context& c,Address table,std::vector<Row>& result,CatalogKind kind=CatalogKind::Accessory) {
    const bool appearance=kind!=CatalogKind::Accessory;
    const bool glider=kind==CatalogKind::Glider;
    Address rowtype{};Name type{};
    if(!IsA(c,table,c.table_class)||!Read(c,table+profile::native_layout.RowStruct,rowtype)||!rowtype||
       !Read(c,rowtype+accessory::profile::native_layout.ObjectName,type)||type.index!=(appearance?c.appearance_struct:c.row_struct))return false;
    if(appearance){
        if(!PropertyAt(c,rowtype,"AppearanceType",profile::native_layout.AppearanceType)||
           !PropertyAt(c,rowtype,"CharacterID",profile::native_layout.AppearanceCharacter)||
           !PropertyAt(c,rowtype,"Name",profile::native_layout.AppearanceName)||
           !PropertyAt(c,rowtype,"AppearanceData",profile::native_layout.AppearanceData)||
           !PropertyAt(c,rowtype,"IsCharacterShow",profile::native_layout.AppearanceCharacterShow))return false;
    }else if(!PropertyAt(c,rowtype,"Part",profile::native_layout.DecorationPart)||!PropertyAt(c,rowtype,"Type",profile::native_layout.DecorationType)||
             !PropertyAt(c,rowtype,"Name",profile::native_layout.DecorationName)||!PropertyAt(c,rowtype,"MeshDatas",profile::native_layout.DecorationMeshDatas))return false;
    struct Header{Address data;std::int32_t count,capacity;};Header h{};
    Address map=table+profile::native_layout.RowMap,flags{};std::int32_t bits{},maximum{},free{};
    if(!Read(c,map,h)||h.count<0||h.count>4096||h.capacity<h.count||h.capacity>65536||
       !Read(c,map+accessory::profile::native_layout.MapBitsData,flags)||!Read(c,map+accessory::profile::native_layout.MapBitCount,bits)||!Read(c,map+accessory::profile::native_layout.MapBitCapacity,maximum)||
       !Read(c,map+accessory::profile::native_layout.MapFreeCount,free)||bits<h.count||bits>65536||maximum<bits||free<0||free>h.count)return false;
    if(!flags){if(bits>128)return false;flags=map+accessory::profile::native_layout.MapInlineBits;}
    std::vector<std::uint32_t> allocation((h.count+31)/32);
    if(h.count&&(!h.data||!ReadBytes(c,flags,allocation.data(),allocation.size()*4)))return false;
    int allocated=0;
    for(int i=0;i<h.count;++i){
        if(!(allocation[i/32]&(1u<<(i%32))))continue;
        ++allocated;
        Name id{};Address row{};std::uint8_t part{};
        if(!Read(c,h.data+i*profile::native_layout.MapEntryStride,id)||!Read(c,h.data+i*profile::native_layout.MapEntryStride+profile::native_layout.MapValue,row)||!row||!Read(c,row+(appearance?profile::native_layout.AppearanceType:profile::native_layout.DecorationPart),part))return false;
        if(!id.index)continue;
        if(kind==CatalogKind::Accessory&&(part<1||part>4))continue;
        if(kind==CatalogKind::Glider&&part!=2)continue;
        Row entry{{id,static_cast<std::uint8_t>(appearance?0:part)},NameText(c,id),{}};
        if(!appearance){Name decoration_type{};if(!Read(c,row+profile::native_layout.DecorationType,decoration_type))return false;
            entry.key.type=decoration_type;entry.type=NameText(c,decoration_type);}
        if(appearance){
            std::uint8_t appearance_type{},show{};
            Name character{};
            if(!Read(c,row+profile::native_layout.AppearanceType,appearance_type)||
               !Read(c,row+profile::native_layout.AppearanceCharacter,character)||
               !Read(c,row+profile::native_layout.AppearanceCharacterShow,show))return false;
            // Shared gliders have no CharacterID. An empty character is valid here.
            if(!show||appearance_type!=2)continue;
        }
        if(glider){
            Address structure{},data{};Name structure_name{};
            if(!Read(c,row+profile::native_layout.AppearanceData,structure)||!structure||!Read(c,structure+accessory::profile::native_layout.ObjectName,structure_name)||
               structure_name.index!=c.glide_struct||!PropertyAt(c,structure,"ActorClass",profile::native_layout.GlideActorClass)||
               !Read(c,row+profile::native_layout.AppearanceData+profile::native_layout.InstancedStructData,data)||!data)continue;
            entry.soft_class=data+profile::native_layout.GlideActorClass;
        }
        char label[1024]{};std::size_t length=sizeof(label);
        if(c.names->resolve_ftext_utf8 && c.names->resolve_ftext_utf8(c.names->user,row+(appearance?profile::native_layout.AppearanceName:profile::native_layout.DecorationName),label,&length).code==ANOMALY_STATUS_V1_OK)entry.label=label;
        if(entry.label.empty())entry.label=entry.id;
        if(!entry.id.empty())result.push_back(std::move(entry));
    }
    return allocated==h.count-free;
}
// SEH boundaries contain no C++ objects requiring unwinding. A failed native call
// disables replacement; it is never treated as successful application.
bool NativeCopy(Address fn,NameArray* out,const NameArray* in) noexcept {
    __try{reinterpret_cast<CopyFn>(fn)(out,in);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRefresh(Address fn,Address pawn) noexcept {
    __try{reinterpret_cast<RefreshFn>(fn)(reinterpret_cast<void*>(pawn));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeSoftAssign(Address fn,SoftClass* out,const SoftClass* source) noexcept {
    __try{reinterpret_cast<SoftClass*(__fastcall*)(SoftClass*,const SoftClass*)>(fn)(out,source);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// The game owns this by-value argument and destroys its string after spawning.
// Assignment uses the engine allocator and touches only that temporary argument.
void __fastcall GliderDetour(void* object,SoftClass* appearance) noexcept {
    auto* c=active.load(std::memory_order_acquire);if(!c)return;
    AnomalyGenerationHandleV1 lease{};
    bool leased=c->hooks->begin_callback(c->hooks->user,c->glider_hook,&lease).code==ANOMALY_STATUS_V1_OK;
    try{
        if(leased&&appearance&&c->glider_enabled.load()&&reinterpret_cast<Address>(object)==c->pawn.load()&&
           GetCurrentThreadId()==c->game_thread.load()){
            std::scoped_lock lock(c->mutex);auto source=c->glider_path.View();
            if(!NativeSoftAssign(c->soft_assign,appearance,&source))throw 1;
            c->glider_replacements.fetch_add(1);
        }
    }catch(...){c->glider_enabled=false;c->glider_fault=true;}
    auto original=reinterpret_cast<void(__fastcall*)(void*,SoftClass*)>(c->glider_original);
    if(original)original(object,appearance);
    if(leased)c->hooks->end_callback(c->hooks->user,lease);
}
bool __fastcall GetDetour(void* object,Appearance* appearance) noexcept {
    auto* c=active.load(std::memory_order_acquire);if(!c)return false;
    auto original=reinterpret_cast<GetFn>(c->original);
    AnomalyGenerationHandleV1 lease{};
    bool leased=c->hooks->begin_callback(c->hooks->user,c->hook,&lease).code==ANOMALY_STATUS_V1_OK;
    bool result=false;
    try {
        if(original)result=original(object,appearance);
        const bool local_object=object&&reinterpret_cast<Address>(object)==c->pawn.load();
        const bool game_thread=GetCurrentThreadId()==c->game_thread.load();
        if(leased&&appearance&&local_object&&c->enabled.load()&&game_thread){
            std::vector<ItemKey> chosen,catalog;bool hide_all{};
            std::array<Name,MaxAccessories> snapshot{};int snapshot_count{};bool snapshot_valid{};
            {std::scoped_lock lock(c->mutex);chosen=c->accessory_choices;hide_all=c->hide_all;
                for(auto& row:*c->rows)catalog.push_back(row.key);
                snapshot_count=c->decoration_snapshot_count;snapshot_valid=c->decoration_snapshot_valid&&snapshot_count>=0&&snapshot_count<=static_cast<int>(MaxAccessories);
                if(snapshot_valid)std::copy_n(c->decoration_snapshot.begin(),snapshot_count,snapshot.begin());}
            if(c->enabled.load()){
                NameArray source=appearance->decorations;
                if(source.count<0||source.count>static_cast<int>(MaxAccessories)||source.capacity<source.count)throw 1;
                std::array<Name,MaxAccessories> old{},next{};int count{};
                const auto base_count=snapshot_valid?snapshot_count:source.count;
                if(snapshot_valid)std::copy_n(snapshot.begin(),base_count,old.begin());
                else if(source.count&&!ReadBytes(*c,reinterpret_cast<Address>(source.data),old.data(),source.count*sizeof(Name)))throw 1;
                if(!ComposeSelections({old.data(),static_cast<std::size_t>(base_count)},catalog,
                    chosen,hide_all,next,count))throw 1;
                NameArray replacement{next.data(),count,count};
                if(!NativeCopy(c->copy,&appearance->decorations,&replacement))throw 1;
                {std::scoped_lock lock(c->mutex);std::copy_n(next.begin(),count,c->decoration_snapshot.begin());c->decoration_snapshot_count=count;c->decoration_snapshot_valid=true;}
                c->replacements.fetch_add(1);
            }
        }
    }catch(...){c->enabled=false;c->callback_fault=true;}
    if(leased)c->hooks->end_callback(c->hooks->user,lease);
    return result;
}
bool Bind(Context& c) {
    c.getter=Resolve(c,profile::GetAppearance);c.refresh=Resolve(c,profile::Refresh);
    c.world=Rip(c,Resolve(c,profile::World),profile::native_layout.RipDisplacement,profile::native_layout.RipInstructionSize);c.registry=Rip(c,Resolve(c,profile::Objects),profile::native_layout.RipDisplacement,profile::native_layout.RipInstructionSize,-static_cast<int>(profile::native_layout.RegistrySubtract));
    if(!c.getter||!c.refresh||!c.world||!c.registry)return false;
    std::uint8_t opcode{};Address call=c.getter+profile::native_layout.CopyNamesCall;
    if(!Read(c,call,opcode)||opcode!=0xe8)return false;
    c.copy=Rip(c,call,1,5);
    const auto refresh_call=c.refresh+profile::native_layout.RefreshGetterCall;
    if(!Read(c,refresh_call,opcode)||opcode!=0xe8||Rip(c,refresh_call,1,5)!=c.getter)return false;
    return Executable(c.copy) && c.copy==profile::compatibility.Address("CopyNames");
}
bool BindGlider(Context& c) {
    c.glider_spawn=Resolve(c,profile::GliderSpawn);c.glider_refresh=Resolve(c,profile::GliderRefresh);
    c.soft_assign=Resolve(c,profile::SoftClassAssign);
    if(!c.glider_spawn||!c.glider_refresh||!c.soft_assign)return false;
    auto call=c.glider_refresh+profile::native_layout.GliderSpawnCall;std::uint8_t opcode{};
    return Read(c,call,opcode)&&opcode==0xe8&&Rip(c,call,1,5)==c.glider_spawn;
}
bool Services(Context& c) {
    // 1.11.1 publishes names v1. Only use its original two methods, never find_utf8.
    c.names=anomaly::sdk::Host(c.host).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID,1).get();
    constexpr auto names_v1_size=offsetof(AnomalyUe5NamesServiceV1,resolve_ftext_utf8)+sizeof(decltype(AnomalyUe5NamesServiceV1::resolve_ftext_utf8));
    if(c.names&&c.names->struct_size<names_v1_size)c.names=nullptr;
    c.objects=Query<AnomalyUe5ObjectsServiceV1>(c.host,ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
    c.actors=Query<AnomalyNteActorsServiceV1>(c.host,ANOMALY_NTE_ACTORS_SERVICE_V1_ID);
    c.framework=Query<AnomalyUe5FrameworkServiceV1>(c.host,ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID);
    // on_update is dispatched by Anomaly's GameUpdate callback, so the current
    // thread is already the game thread. The framework adapter may publish its
    // game-thread id later than the plugin starts; querying is_game_thread here
    // would keep the plugin stuck in its startup wait forever.
    return c.names&&c.names->resolve_utf8&&c.objects&&c.objects->snapshot_by_handle&&
        c.objects->snapshot_at&&c.objects->count&&c.framework&&c.framework->is_game_thread;
}
std::string LocalEntityInfo(Context& c) {
    if(!c.actors||!c.actors->frame||!c.actors->page||!c.actors->class_name_utf8||!c.actors->entity_name_utf8)
        return lite_i18n::localizer.Text("entity.unavailable", "Entity information: the Anomaly entity service is unavailable.");
    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if(c.actors->frame(c.actors->user,&frame).code!=ANOMALY_STATUS_V1_OK)
        return lite_i18n::localizer.Text("entity.wait_snapshot", "Entity information: waiting for a snapshot.");
    std::array<AnomalyNteEntitySnapshotV1,4> snapshots{};
    for(auto& snapshot:snapshots)snapshot.struct_size=sizeof(snapshot);
    AnomalyNteEntityPageRequestV1 request{sizeof(request),0,frame.generation,0,
        static_cast<std::uint32_t>(snapshots.size()),0,0,0,
        ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER,0};
    AnomalyNteEntityPageResultV1 result{sizeof(result)};
    if(c.actors->page(c.actors->user,&request,snapshots.data(),&result).code!=ANOMALY_STATUS_V1_OK||!result.returned)
        return lite_i18n::localizer.Text("entity.no_player", "Entity information: no local player found.");
    const auto& entity=snapshots.front();
    char class_name[256]{};std::size_t class_size=sizeof(class_name);
    char entity_name[256]{};std::size_t entity_size=sizeof(entity_name);
    if(c.actors->class_name_utf8(c.actors->user,entity.class_id,class_name,&class_size).code!=ANOMALY_STATUS_V1_OK)
        class_name[0]='\0';
    if(c.actors->entity_name_utf8(c.actors->user,entity.entity_id,entity_name,&entity_size).code!=ANOMALY_STATUS_V1_OK)
        entity_name[0]='\0';
    const auto unknown=lite_i18n::localizer.Text("common.unknown", "Unknown");
    const auto id=std::to_string(entity.entity_id);
    return lite_i18n::localizer.Format("entity.summary", "Entity: {0} #{1} - Type: {2}", std::array<std::string_view,3>{entity_name[0]?entity_name:unknown,id,class_name[0]?class_name:unknown});
}
void Discover(Context& c) {
    Address table=ObjectAddress(c,c.table);
    Address glider_table=ObjectAddress(c,c.glider_table);
    if(table&&glider_table)return;
    if(!table)c.table={};
    if(!glider_table)c.glider_table={};
    const auto count=c.objects->count(c.objects->user);
    const auto end=static_cast<std::uint32_t>(std::min<std::uint64_t>(count,static_cast<std::uint64_t>(c.scan_index)+4096));
    for(;c.scan_index<end;++c.scan_index){
        AnomalyUe5ObjectSnapshotV1 obj{sizeof(obj)};
        if(c.objects->snapshot_at(c.objects->user,c.scan_index,&obj).code!=ANOMALY_STATUS_V1_OK)continue;
        if(!c.table_name||!c.player_class||!c.table_class||!c.row_struct||!c.glider_table_name||!c.appearance_struct||!c.glide_struct){
            auto name=NameText(c,{obj.name_id,0});
            if(name=="DT_DecorationData")c.table_name=obj.name_id;
            else if(name=="HTPlayerCharacter")c.player_class=obj.name_id;
            else if(name=="DataTable")c.table_class=obj.name_id;
            else if(name=="DecorationData")c.row_struct=obj.name_id;
            else if(name=="DT_AppearanceData")c.glider_table_name=obj.name_id;
            else if(name=="StaticAppearanceData")c.appearance_struct=obj.name_id;
            else if(name=="GlideData")c.glide_struct=obj.name_id;
        }
        if(!glider_table&&c.table_class&&c.appearance_struct&&c.glide_struct&&obj.name_id==c.glider_table_name){
            auto candidate=ObjectAddress(c,obj.handle);std::vector<Row> gliders;
            if(candidate&&ReadCatalog(c,candidate,gliders,CatalogKind::Glider)){
                c.glider_table=obj.handle;glider_table=candidate;
                LogCosmetics(c,"loaded gliders="+std::to_string(gliders.size()));
                std::sort(gliders.begin(),gliders.end(),[](const Row& a,const Row& b){return a.id<b.id;});
                auto snapshot=std::make_shared<const std::vector<Row>>(std::move(gliders));
                std::scoped_lock lock(c.mutex);c.gliders=std::move(snapshot);
            }
        }
        if(table)continue;
        if(!c.player_class||!c.table_class||!c.row_struct||obj.name_id!=c.table_name)continue;
        auto candidate=ObjectAddress(c,obj.handle);std::vector<Row> rows;
        if(!candidate||!ReadCatalog(c,candidate,rows))continue;
        c.table=obj.handle;table=candidate;
        LogCosmetics(c,"loaded accessories="+std::to_string(rows.size()));
        std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){
            const auto group=[](const Row& row){
                if(row.key.part==1&&row.type=="EYE")return 0;
                if(row.key.part==1&&row.type=="OverHead")return 1;
                return 2+row.key.part;
            };
            return group(a)==group(b)?a.id<b.id:group(a)<group(b);
        });
        auto snapshot=std::make_shared<const std::vector<Row>>(std::move(rows));
        {std::scoped_lock lock(c.mutex);c.rows=std::move(snapshot);}
    }
    if(c.scan_index>=count)c.scan_index=0;
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host,void** output) noexcept {
    if(!host||!output)return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *output=nullptr;
    try{
        auto c=std::make_unique<Context>();c->host=host;
        c->core=Query<AnomalyCoreServiceV1>(host,ANOMALY_CORE_SERVICE_V1_ID);
        c->signatures=Query<AnomalySignatureServiceV1>(host,ANOMALY_SIGNATURE_SERVICE_V1_ID);
        c->hooks=Query<AnomalyHookServiceV1>(host,ANOMALY_HOOK_SERVICE_V1_ID);
        if(!c->core||!c->core->read_memory||!c->signatures||!c->signatures->resolve||!c->hooks||
           !c->hooks->create||!c->hooks->release||!c->hooks->begin_callback||!c->hooks->end_callback)
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,lite_i18n::localizer.Text("error.host_services", "Required host services unavailable"));
        *output=c.release();return anomaly::sdk::Ok();
    }catch(...){return Status(ANOMALY_STATUS_V1_FAILED,lite_i18n::localizer.Text("error.allocation", "Allocation failed"));}
}
AnomalyStatusV1 ANOMALY_CALL Start(void* context) noexcept {
    auto* c=static_cast<Context*>(context);if(!c)return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    try{
        if(!Bind(*c)){
            const auto message=lite_i18n::localizer.Text("error.signatures", "HT decoration signatures did not match; no hook installed");
            Message(*c,message);
            {std::scoped_lock lock(c->mutex);c->glider_message=message;}
            LogCosmetics(*c,message);
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,message);
        }
        Context* expected=nullptr;
        if(!active.compare_exchange_strong(expected,c))return Status(ANOMALY_STATUS_V1_FAILED,lite_i18n::localizer.Text("error.active", "Already active"));
        AnomalyHookRequestV1 request{sizeof(request),ANOMALY_HOOK_V1_FUNCTION,c->getter,reinterpret_cast<void*>(&GetDetour),StringView("accessory-display-data")};
        auto status=c->hooks->create(c->hooks->user,&request,&c->original,&c->hook);
        if(status.code!=ANOMALY_STATUS_V1_OK){active=nullptr;return status;}
        if(BindGlider(*c)){
            AnomalyHookRequestV1 glide_request{sizeof(glide_request),ANOMALY_HOOK_V1_FUNCTION,c->glider_spawn,reinterpret_cast<void*>(&GliderDetour),StringView("glider-display-data")};
            auto glider_status=c->hooks->create(c->hooks->user,&glide_request,&c->glider_original,&c->glider_hook);
            if(glider_status.code!=ANOMALY_STATUS_V1_OK)c->glider_message=lite_i18n::localizer.Text("glider.hook_failed", "The glider interface could not be enabled. Accessories remain available.");
        }else c->glider_message=lite_i18n::localizer.Text("glider.interface_missing", "The glider interface did not match this game build. Accessories remain available.");
        return status;
    }catch(...){return Status(ANOMALY_STATUS_V1_FAILED,lite_i18n::localizer.Text("error.binding", "Binding failed"));}
}
void UpdateGlider(Context& c,Address pawn) {
    if(!c.glider_hook.id)return;
    auto message=[&](std::string text){std::scoped_lock lock(c.mutex);c.glider_message=std::move(text);};
    Address cls{};
    bool layout=Read(c,pawn+profile::native_layout.ObjectClass,cls)&&cls;
    if(layout&&cls!=c.glider_validated_class){
        layout=PropertyAt(c,cls,"GlideFashionID",profile::native_layout.GliderId)&&PropertyAt(c,cls,"GlidingActor",profile::native_layout.GliderActor);
        if(layout)c.glider_validated_class=cls;
    }
    const auto table=ObjectAddress(c,c.glider_table);
    Request request;ItemKey selected;bool automatic{};
    {std::scoped_lock lock(c.mutex);c.glider_ready=layout&&table&&!c.gliders->empty();
        request=c.glider_request;selected=c.glider_requested;automatic=c.auto_glider_request;
        c.glider_request=Request::None;c.auto_glider_request=false;
        if(request!=Request::None&&!automatic)c.auto_glider_pending=false;}
    if(!layout){c.glider_enabled=false;message(lite_i18n::localizer.Text("glider.layout_mismatch", "This character has an incompatible glider layout. Replacement is off."));return;}
    if(!table){c.glider_enabled=false;message(lite_i18n::localizer.Text("glider.open_page", "Waiting for the glider catalog. Open Outfit > Glider in the game."));}
    if(c.glider_fault.exchange(false)){c.glider_enabled=false;message(lite_i18n::localizer.Text("glider.replacement_failed", "Glider replacement failed and stopped. Restore the original glider."));}
    if(request==Request::None)return;
    std::uint8_t loading{};
    if(!Read(c,pawn+profile::native_layout.GliderLoading,loading)||loading){message(lite_i18n::localizer.Text("glider.loading", "The glider is loading. Please retry shortly."));return;}
    if(request==Request::Restore||request==Request::Reload){
        c.glider_enabled=false;
        const bool refreshed=NativeRefresh(c.glider_refresh,pawn);
        if(refreshed&&request==Request::Restore){std::scoped_lock lock(c.mutex);
            if(!c.active_character.empty())cosmetic_profiles::ResetGlider(c.profiles,c.active_character);
            c.glider_selection={};}
        if(request==Request::Reload){
            c.glider_table={};c.scan_index=0;std::scoped_lock lock(c.mutex);
            c.gliders=std::make_shared<const std::vector<Row>>();c.glider_ready=false;
            const auto it=c.profiles.find(c.active_character);
            c.auto_glider_pending=it!=c.profiles.end()&&!it->second.glider.empty();
            c.auto_glider_attempts=0;c.auto_glider_due={};
        }
        message(refreshed?lite_i18n::localizer.Text("glider.restore_requested", "Original glider restoration requested. Deploy it to view the result."):lite_i18n::localizer.Text("glider.restore_failed", "Glider restoration failed. Please retry shortly."));return;
    }
    if(request!=Request::Apply||!table){message(lite_i18n::localizer.Text("glider.catalog_not_ready", "The glider catalog is not ready."));return;}
    // Re-read the selected row and own its path bytes before enabling the hook.
    std::vector<Row> rows;
    if(!ReadCatalog(c,table,rows,CatalogKind::Glider)){message(lite_i18n::localizer.Text("glider.catalog_stale", "The glider catalog changed. Read it again."));return;}
    auto row=std::find_if(rows.begin(),rows.end(),[&](const Row& x){return x.key.id==selected.id;});
    GliderPath path;
    if(row==rows.end()||!path.Capture(row->soft_class,[&](Address a,void* data,std::size_t size){return ReadBytes(c,a,data,size);})){message(lite_i18n::localizer.Text("glider.selection_invalid", "The selected glider resource is invalid. Nothing was applied."));return;}
    {std::scoped_lock lock(c.mutex);c.glider_path=std::move(path);}
    c.glider_enabled=true;const auto before=c.glider_replacements.load();
    if(!NativeRefresh(c.glider_refresh,pawn)||c.glider_replacements.load()==before){
        c.glider_enabled=false;message(lite_i18n::localizer.Text("glider.not_ready", "The current glider is not ready. Please retry shortly."));return;
    }
    {std::scoped_lock lock(c.mutex);
        if(!c.active_character.empty())cosmetic_profiles::SetGlider(c.profiles,c.active_character,row->id);
        c.glider_selection=row->key;c.auto_glider_pending=false;}
    message(lite_i18n::localizer.Text("glider.submitted", "Glider appearance submitted. Deploy it after the model loads."));
}
void ScheduleCharacterLook(Context& c,Address pawn) {
    const auto now=std::chrono::steady_clock::now();
    std::uint8_t glider_loading{1};
    if(c.glider_hook.id&&c.glider_validated_class)
        Read(c,pawn+profile::native_layout.GliderLoading,glider_loading);
    std::scoped_lock lock(c.mutex);
    const auto found=c.profiles.find(c.active_character);
    if(found==c.profiles.end())return;
    const auto& look=found->second;
    if(c.auto_glider_pending&&c.glider_hook.id&&c.glider_table.id&&!c.gliders->empty()&&
       !glider_loading&&c.glider_request==Request::None&&now>=c.auto_glider_due){
        const auto row=std::find_if(c.gliders->begin(),c.gliders->end(),[&](const Row& x){return x.id==look.glider;});
        if(row==c.gliders->end()){c.auto_glider_pending=false;c.glider_message=lite_i18n::localizer.Text("glider.saved_missing", "The saved glider is absent from the current catalog.");}
        else if(c.auto_glider_attempts>=5){c.auto_glider_pending=false;c.glider_message=lite_i18n::localizer.Text("glider.auto_failed", "Automatic glider application failed. Please retry manually.");}
        else{c.glider_request=Request::Apply;c.glider_requested=row->key;c.auto_glider_request=true;
            c.glider_selection=row->key;c.auto_glider_due=now+std::chrono::seconds(2);++c.auto_glider_attempts;}
    }
    if(c.auto_accessory_pending&&c.table.id&&!c.rows->empty()&&c.request==Request::None&&
       now>=c.auto_accessory_due){
        if(c.auto_accessory_attempts>=5){c.auto_accessory_pending=false;c.message=lite_i18n::localizer.Text("cosmetics.auto_failed", "Automatic accessory application failed. Please retry manually.");}
        else{
            std::vector<ItemKey> chosen;
            bool complete=true;
            for(const auto& [slot,id]:look.accessories){
                const auto row=std::find_if(c.rows->begin(),c.rows->end(),[&](const Row& x){
                    return x.id==id&&SlotKey(x)==slot;});
                if(row==c.rows->end()){complete=false;break;}
                chosen.push_back(row->key);
            }
            if(!complete){c.auto_accessory_pending=false;c.message=lite_i18n::localizer.Text("cosmetics.saved_missing", "Saved accessories are absent from the current catalog.");}
            else{c.accessory_choices=std::move(chosen);c.hide_all=look.hidden;
                c.request=Request::Reapply;c.auto_accessory_request=true;
                c.auto_accessory_due=now+std::chrono::seconds(2);++c.auto_accessory_attempts;}
        }
    }
}
void UpdateImpl(Context& c,double delta) {
    if(!Services(c)){
        c.enabled=false;c.glider_enabled=false;c.pawn=0;c.game_thread=0;
        std::scoped_lock lock(c.mutex);c.ready=false;c.glider_ready=false;c.waiting_message=true;c.message=lite_i18n::localizer.Text("cosmetics.wait_ue", "Waiting for UE5 services and appearance data.");return;
    }
    c.game_thread=GetCurrentThreadId();
    c.discovery_time+=delta;
    if(!c.table.id||!c.glider_table.id||c.discovery_time>=2.0){Discover(c);c.discovery_time=0;}
    auto pawn=LocalPawn(c);auto previous=c.pawn.exchange(pawn);
    Name character{};
    if(pawn)Read(c,pawn+profile::native_layout.DisplayCharacter,character);
    const auto character_id=NameText(c,character);
    if(previous!=pawn||c.last_character!=character_id){
        if(pawn)LogCosmetics(c,"local character="+character_id);
        c.glider_validated_class=0;
        c.last_character=character_id;c.enabled=false;c.applied=false;c.glider_enabled=false;
        std::scoped_lock lock(c.mutex);
        c.accessory_choices.clear();c.hide_all=false;
        c.decoration_snapshot_count=0;c.decoration_snapshot_valid=false;c.ready=false;c.glider_ready=false;
        c.player.clear();c.request=c.glider_request=Request::None;
        c.active_character=character_id;c.accessory_selection={};c.glider_selection={};
        c.auto_accessory_request=c.auto_glider_request=false;
        c.auto_accessory_attempts=c.auto_glider_attempts=0;
        c.auto_accessory_due=c.auto_glider_due={};
        const auto it=c.profiles.find(character_id);
        c.auto_accessory_pending=it!=c.profiles.end()&&(it->second.hidden||!it->second.accessories.empty());
        c.auto_glider_pending=it!=c.profiles.end()&&!it->second.glider.empty();
        if(c.glider_hook.id)c.glider_message=c.auto_glider_pending?lite_i18n::localizer.Text("glider.reapplying", "Restoring this character's saved glider."):lite_i18n::localizer.Text("glider.choose", "Choose a glider and apply it to the current character.");
        c.message=c.auto_accessory_pending?lite_i18n::localizer.Text("cosmetics.reapplying", "Restoring this character's saved accessories."):lite_i18n::localizer.Text("cosmetics.choose", "Choose accessories and apply them to the current character.");
        c.waiting_message=false;
    }
    if(!pawn){Message(c,lite_i18n::localizer.Text("cosmetics.no_player", "No valid local character found, or the character layout is incompatible."),true);return;}
    ScheduleCharacterLook(c,pawn);
    UpdateGlider(c,pawn);
    if(c.callback_fault.exchange(false)){c.enabled=false;Message(c,lite_i18n::localizer.Text("cosmetics.replacement_failed", "Accessory replacement failed and stopped. Restore the originals."));}
    Request request;ItemKey selected;bool automatic{};
    {std::scoped_lock lock(c.mutex);request=c.request;selected=c.requested;
        automatic=c.auto_accessory_request;c.request=Request::None;c.auto_accessory_request=false;
        if(request!=Request::None&&!automatic)c.auto_accessory_pending=false;}
    if(request==Request::Reload){
        c.enabled=false;
        if(c.applied.exchange(false))NativeRefresh(c.refresh,pawn);
        c.table={};c.scan_index=0;std::scoped_lock lock(c.mutex);
        c.rows=std::make_shared<const std::vector<Row>>();c.accessory_choices.clear();c.hide_all=false;
        c.decoration_snapshot_count=0;c.decoration_snapshot_valid=false;
        const auto it=c.profiles.find(c.active_character);
        c.auto_accessory_pending=it!=c.profiles.end()&&
            (it->second.hidden||!it->second.accessories.empty());
        c.auto_accessory_attempts=0;c.auto_accessory_due={};
    }
    auto player=NameText(c,character);
    bool ready=ObjectAddress(c,c.table)!=0;
    if(!ready)c.enabled=false;
    {std::scoped_lock lock(c.mutex);c.player=std::move(player);c.ready=ready&&!c.rows->empty();ready=c.ready;
      if(c.waiting_message){c.waiting_message=false;c.message=lite_i18n::localizer.Text("cosmetics.choose", "Choose accessories and apply them to the current character.");}}
    if(request==Request::None||request==Request::Reload)return;
    if(request!=Request::Restore&&!ready){Message(c,lite_i18n::localizer.Text("cosmetics.open_page", "The accessory catalog is not ready. Open Outfit in the game and retry."));return;}
    std::vector<ItemKey> previous_choices;
    bool previous_hide{},next_enabled{};
    const bool previous_enabled=c.enabled.load();
    {std::scoped_lock lock(c.mutex);
        previous_choices=c.accessory_choices;previous_hide=c.hide_all;
        if(request==Request::Apply){
            const auto row=std::find_if(c.rows->begin(),c.rows->end(),[&](const Row& x){
                return x.key.id==selected.id&&SameSlot(x.key,selected);});
            if(row==c.rows->end()){c.message=lite_i18n::localizer.Text("cosmetics.selection_stale", "The selected accessory changed. Select it again.");return;}
            c.accessory_choices.erase(std::remove_if(c.accessory_choices.begin(),c.accessory_choices.end(),
                [&](ItemKey x){return SameSlot(x,selected);}),c.accessory_choices.end());
            c.accessory_choices.push_back(row->key);c.hide_all=false;
        }else if(request==Request::RestoreSlot){
            c.accessory_choices.erase(std::remove_if(c.accessory_choices.begin(),c.accessory_choices.end(),
                [&](ItemKey x){return SameSlot(x,selected);}),c.accessory_choices.end());
        }else if(request==Request::Hide){c.accessory_choices.clear();c.hide_all=true;}
        else if(request==Request::Restore){c.accessory_choices.clear();c.hide_all=false;}
        next_enabled=c.hide_all||!c.accessory_choices.empty();
        c.decoration_snapshot_count=0;c.decoration_snapshot_valid=false;
    }
    c.enabled=next_enabled;
    const auto before=c.replacements.load();
    const bool refreshed=NativeRefresh(c.refresh,pawn);
    if(!refreshed||(next_enabled&&c.replacements.load()==before)){
        {std::scoped_lock lock(c.mutex);c.accessory_choices=std::move(previous_choices);
            c.hide_all=previous_hide;c.decoration_snapshot_count=0;c.decoration_snapshot_valid=false;}
        c.enabled=previous_enabled;
        if(refreshed&&previous_enabled)NativeRefresh(c.refresh,pawn);
        Message(c,lite_i18n::localizer.Text("cosmetics.not_ready", "The character appearance is not ready. Please retry shortly."));return;
    }
    c.applied=next_enabled;
    {std::scoped_lock lock(c.mutex);
        if(!c.active_character.empty()&&request!=Request::Reapply){
            if(request==Request::Restore)cosmetic_profiles::ResetAccessories(c.profiles,c.active_character);
            else if(request==Request::Hide)cosmetic_profiles::HideAccessories(c.profiles,c.active_character);
            else{
                const auto row=std::find_if(c.rows->begin(),c.rows->end(),[&](const Row& x){
                    return x.key.id==selected.id&&SameSlot(x.key,selected);});
                if(row!=c.rows->end()){
                    if(request==Request::Apply)cosmetic_profiles::SetAccessory(
                        c.profiles,c.active_character,SlotKey(*row),row->id);
                    else if(request==Request::RestoreSlot)cosmetic_profiles::ResetAccessorySlot(
                        c.profiles,c.active_character,SlotKey(*row));
                }
            }
        }
        c.auto_accessory_pending=false;
        if(request==Request::Restore||request==Request::Hide)c.accessory_selection={};
        else if(request==Request::Apply)c.accessory_selection=selected;
        else if(request==Request::RestoreSlot&&SameSlot(c.accessory_selection,selected))c.accessory_selection={};
    }
    Message(c,request==Request::Restore?lite_i18n::localizer.Text("cosmetics.restored", "Original accessories restored."):
        request==Request::RestoreSlot?lite_i18n::localizer.Text("cosmetics.slot_restored", "The selected accessory slot was restored."):
        request==Request::Reapply?lite_i18n::localizer.Text("cosmetics.reapplied", "This character's saved accessories were restored."):lite_i18n::localizer.Text("cosmetics.submitted", "Accessory appearance submitted. Waiting for the game to load the model."));
}
void ANOMALY_CALL Update(void* context,double delta) noexcept {
    if(!context)return;auto& c=*static_cast<Context*>(context);
    try{UpdateImpl(c,delta);}catch(...){c.enabled=false;c.glider_enabled=false;c.callback_fault=true;c.glider_fault=true;}
}
void DrawCosmetics(void* context,const AnomalyUiServiceV1* ui,int category) noexcept {
    if(!context||!ui||ui->struct_size<sizeof(AnomalyUiServiceV1))return;
    auto& c=*static_cast<Context*>(context);
    try{
        const bool glider=category==2;
        std::shared_ptr<const std::vector<Row>> snapshot;std::string message,player;bool ready;ItemKey selection{};
        cosmetic_profiles::Look saved_look;
        {std::scoped_lock lock(c.mutex);
            snapshot=glider?c.gliders:c.rows;
            message=glider?c.glider_message:c.message;
            player=c.player;ready=glider?c.glider_ready:c.ready;
            selection=glider?c.glider_selection:c.accessory_selection;
            const auto saved=c.profiles.find(c.active_character);
            if(saved!=c.profiles.end())saved_look=saved->second;
        }
        const auto& rows=*snapshot;
        ui->text(ui->user,StringView(message));ui->text(ui->user,StringView(player));
        if(glider&&!saved_look.glider.empty())ui->text(ui->user,StringView(lite_i18n::localizer.Format("glider.saved", "Saved for this character: {0}", std::array<std::string_view,1>{saved_look.glider})));
        if(!glider){
            if(saved_look.hidden)ui->text(ui->user,StringView(lite_i18n::localizer.Text("cosmetics.hidden", "Current character: all accessories hidden.")));
            for(const auto& [slot,id]:saved_look.accessories){
                const auto row=std::find_if(rows.begin(),rows.end(),[&](const Row& x){return SlotKey(x)==slot;});
                if(row!=rows.end())ui->text(ui->user,StringView(lite_i18n::localizer.Format("cosmetics.saved_slot", "{0}: {1}", std::array<std::string_view,2>{lite_i18n::PlacementLabel(row->type,row->key.part),id})));
            }
        }
        auto queue=[&](Request request,ItemKey item=ItemKey{}){std::scoped_lock lock(c.mutex);
            if(glider){c.glider_request=request;c.glider_requested=item;c.auto_glider_request=false;}
            else{c.request=request;c.requested=item;c.auto_accessory_request=false;}};
        if(ui->button(ui->user,StringView(glider?lite_i18n::localizer.Text("glider.restore", "Restore original glider"):lite_i18n::localizer.Text("cosmetics.restore", "Restore original accessories")),0,0))queue(Request::Restore);
        if(!glider){
            ui->same_line(ui->user,0,-1);
            if(ui->button_enabled(ui->user,StringView(lite_i18n::localizer.Text("cosmetics.hide", "Hide all accessories")),0,0,ready))queue(Request::Hide);
        }
        ui->same_line(ui->user,0,-1);
        if(ui->button(ui->user,StringView(lite_i18n::localizer.Text("catalog.reload", "Read catalog again")),0,0))queue(Request::Reload);
        if(!glider){
            for(const auto& [slot,id]:saved_look.accessories){
                const auto row=std::find_if(rows.begin(),rows.end(),[&](const Row& x){return SlotKey(x)==slot;});
                if(row!=rows.end()){
                    const auto label=lite_i18n::localizer.Format("cosmetics.restore_slot", "Restore {0}", std::array<std::string_view,1>{lite_i18n::PlacementLabel(row->type,row->key.part)})+"###restore-slot-"+slot;
                    if(ui->button(ui->user,StringView(label),0,0))queue(Request::RestoreSlot,row->key);
                }
            }
        }
        char* filter=glider?c.glider_filter:c.filter;
        ui->input_text(ui->user,StringView(glider?lite_i18n::localizer.Text("glider.search", "Search glider / ID"):lite_i18n::localizer.Text("cosmetics.search", "Search accessory / ID")),filter,128,0);
        ui->text(ui->user,StringView(glider?lite_i18n::localizer.Text("glider.instructions", "Select and apply a glider, then deploy it to view the result."):lite_i18n::localizer.Text("cosmetics.instructions", "Select an accessory and apply it. Eye and crown slots can be worn together.")));
        if(rows.empty())ui->text(ui->user,StringView(glider?lite_i18n::localizer.Text("glider.empty", "Waiting for the glider catalog. Open Outfit in the game."):lite_i18n::localizer.Text("cosmetics.empty", "Waiting for the accessory catalog. Open Outfit in the game.")));
        auto visible=ui->begin_child(ui->user,StringView(glider?"glider-list":"accessory-list"),0,300,0);
        if(visible){std::string last_group;for(auto& row:rows){
            if(filter[0]&&row.label.find(filter)==std::string::npos&&row.id.find(filter)==std::string::npos)continue;
            const auto group=glider?std::string{}:std::string(lite_i18n::PlacementLabel(row.type,row.key.part));
            if(!glider&&group!=last_group){ui->text(ui->user,StringView(group));last_group=group;}
            const auto label=lite_i18n::ResourceLabel(row.label,row.id,group,selection.id==row.key.id,"appearance-");
            if(ui->button_enabled(ui->user,StringView(label),0,0,ready)){
                selection=row.key;std::scoped_lock lock(c.mutex);
                if(glider)c.glider_selection=row.key;else c.accessory_selection=row.key;
            }
        }}ui->end_child(ui->user);
        const bool valid=std::any_of(rows.begin(),rows.end(),[&](const Row& row){
            return row.key.id==selection.id && row.key.part==selection.part;
        });
        if(ui->button_enabled(ui->user,StringView(glider?lite_i18n::localizer.Text("glider.apply", "Apply selected glider"):lite_i18n::localizer.Text("cosmetics.apply", "Apply selected accessory")),0,0,ready&&valid))
            queue(Request::Apply,selection);
    }catch(...){}
}
AnomalyStatusV1 ANOMALY_CALL Stop(void* context,std::uint32_t) noexcept {
    auto* c=static_cast<Context*>(context);if(!c)return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    c->enabled=false;c->glider_enabled=false;
    // Stop runs on Lifecycle, so it cannot invoke the game's refresh function.
    // No game-owned arrays were changed. The next native appearance refresh is original.
    if(c->glider_hook.id){auto status=c->hooks->release(c->hooks->user,c->glider_hook);if(status.code!=ANOMALY_STATUS_V1_OK&&status.code!=ANOMALY_STATUS_V1_NOT_FOUND)return status;c->glider_hook={};}
    if(c->hook.id){auto status=c->hooks->release(c->hooks->user,c->hook);if(status.code!=ANOMALY_STATUS_V1_OK&&status.code!=ANOMALY_STATUS_V1_NOT_FOUND)return status;c->hook={};}
    active=nullptr;c->original=0;c->pawn=0;
    return anomaly::sdk::Ok();
}
void ANOMALY_CALL Unload(void* context) noexcept {delete static_cast<Context*>(context);}
}
AnomalyStatusV1 CosmeticsLoad(const AnomalyHostApiV1* host,void** context) noexcept { return Load(host,context); }
std::uintptr_t CosmeticsNameArrayCopyAddress() noexcept {const auto c=active.load();return c?c->copy:0;}
AnomalyStatusV1 CosmeticsStart(void* context) noexcept { return Start(context); }
AnomalyStatusV1 CosmeticsStop(void* context,uint32_t reason) noexcept { return Stop(context,reason); }
void CosmeticsUnload(void* context) noexcept { Unload(context); }
void CosmeticsUpdate(void* context,double delta) noexcept { Update(context,delta); }
void CosmeticsDraw(void* context,const AnomalyUiServiceV1* ui,int category) noexcept { DrawCosmetics(context,ui,category); }
