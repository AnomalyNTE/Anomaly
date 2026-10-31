#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "plugins/common/localization.hpp"
#include "access.hpp"
#include "model.hpp"
namespace {
// English fallbacks live in the code; the zh-CN catalog is plugins/SceneLoader/locales/zh-CN.json.
anomaly::plugins::Localizer localizer;
std::string T(const char* key,const char* fallback) {return localizer.Text(key,fallback);}
std::string T(const scene::Text& text) {return text.key?localizer.Text(text.key,text.fallback):std::string{};}
std::string F(const char* key,const char* fallback,std::initializer_list<std::string_view> arguments) {
    return localizer.Format(key,fallback,std::span<const std::string_view>(arguments.begin(),arguments.size()));
}
const AnomalyUe5NamesServiceV1* names{};
const AnomalyUe5ObjectsServiceV1* objects{};
const AnomalyUe5FrameworkServiceV1* framework{};
const AnomalyUe5WorldServiceV1* world_service{};
const AnomalyWindowServiceV1* windows{};
const AnomalyUe5ProcessEventServiceV1* events{};
AnomalyGenerationHandleV1 subscription{},cached_ps{},cached_pc{};
std::atomic_uintptr_t observed_ps{};
std::atomic_uint64_t return_entrance{},return_ack{};
std::atomic_bool force_return{},dismiss_error{};
std::string big_world;
std::array<double,3> last_world_position{};
bool have_world_position{},only_verified{true};
uint64_t return_queued_at{};
struct ReturnPoint {FName id;std::array<double,3> position;};
std::vector<ReturnPoint> return_points;
AnomalyGenerationHandleV1 window{};
std::map<std::string,AnomalyGenerationHandleV1> functions;
std::map<std::string,Clock::time_point> missing_functions;
std::map<std::string,int> offsets;
uintptr_t world_slot{},text_string{};
uintptr_t error_exit_exec{},delegate_remove{},deactivate_signal{};
#include "ue.hpp"
struct Route {FName id{};scene::RouteKind kind{};uint8_t clone_type{};int32_t level{1};std::string label;};
struct Entry {FName id{};std::string key,title;uint8_t type{};std::vector<Route> routes;std::array<double,3> exit_position{};bool exit_to_world{};};
struct Catalog {std::vector<Entry> scenes;};
struct LayerRow {std::string instance,asset;uint8_t state{0xFF};};
struct LayerSnapshot {uint64_t generation{};std::string scene,message;std::vector<LayerRow> rows;bool working{};};
struct View {std::shared_ptr<const Catalog> catalog;std::shared_ptr<const LayerSnapshot> layers;std::string current,message;bool busy{},ready{},in_big_world{};};
struct Request {std::string from,target;Route route;};
std::shared_ptr<Catalog> building;
std::shared_ptr<const Catalog> catalog;
std::atomic<std::shared_ptr<const View>> published;
std::atomic<std::shared_ptr<const Request>> pending;
std::atomic_bool shown{},refresh{};
scene::Transition transition;
Clock::time_point next_tick{},next_catalog{};
std::string message,current_scene;
int selected{},selected_route{},category{},fallback_open{1};
bool only_available{true};
std::array<char,128> filter{};
// Scene data layers: the game thread owns the snapshot and the jobs; the UI only reads published
// snapshots and hands over requests through the atomics.
std::shared_ptr<const LayerSnapshot> layer_snapshot;
uint64_t layer_generation{},layer_recheck_at{};
std::atomic_bool layer_refresh{};
// One layer message to send: the instance name and the message type (open or close).
struct LayerCommand {std::string instance;int32_t type{};};
std::atomic<std::shared_ptr<const std::vector<LayerCommand>>> layer_request;
enum class LayerJob {None,Read,Send};
LayerJob layer_job{};
std::vector<LayerCommand> layer_commands;
AnomalyGenerationHandleV1 layer_rpc{};
int32_t rpc_scan_cursor{};
// Holding a scene's own exit request (scene::HoldsExit): game thread, except the watched pointer
// and the counter, which the ProcessEvent observer reads and bumps.
AnomalyGenerationHandleV1 exit_rpc{};
bool exit_held{},exit_hold_unavailable{};
uint32_t exit_saved_flags{};
uintptr_t exit_saved_func{},exit_held_fn{};
std::atomic<uintptr_t> exit_watch_fn{};
std::atomic_int exit_swallowed{};
int exit_reported{};
// FNativeFuncPtr signature (context, stack frame, result); does nothing.
void __fastcall NoOpNative(void*,void*,void*) {}
void ReleaseExitHold();
struct Row {FName key{};uintptr_t data{};uint32_t hash{},next{};};
struct MapHeader {
    uintptr_t data{};int32_t count{},capacity{};uint32_t inline_bits[4]{};uintptr_t flags{};
    int32_t bits{},max_bits{},first_free{},free_count{};uint32_t hash{},padding{};
    uintptr_t hash_data{};int32_t hash_count{},tail{};
};
static_assert(sizeof(MapHeader)==80 && sizeof(Row)==24);
std::vector<Row> level_rows,transfer_rows,clone_rows;
size_t level_cursor{},transfer_cursor{},clone_cursor{};
AnomalyGenerationHandleV1 level_table{},transfer_table{},clone_table{};
uint64_t NameKey(FName id) {return static_cast<uint64_t>(id.number)<<32 | id.id;}
void ANOMALY_CALL Observe(void*,uintptr_t object,uintptr_t function,void* parameters) {
    try {
        if(function && function==exit_watch_fn.load()) {exit_swallowed.fetch_add(1);return;}
        if(object!=observed_ps.load() || !return_entrance.load() || !parameters) return;
        const Reader r{names};const auto name=r.NameAt(function+24);
        if((name=="OnMainPlayerTransferFinish" || name=="OnFinishTransfered") && r.Function(function,name,1,8)) {
            FName id{};std::memcpy(&id,parameters,8);if(NameKey(id)==return_entrance.load()) return_ack=NameKey(id);
        }
    } catch(...) {}
}
bool Rows(const Reader& r,uintptr_t table,const char* type,std::vector<Row>& result) {
    uintptr_t structure{};MapHeader header{};
    if(!table || !r.IsA(table,"DataTable") || !r.Read(table+40,structure) || r.NameAt(structure+24)!=type ||
       !r.Read(table+48,header) || header.count<0 || header.count>8192 || header.capacity<header.count ||
       header.bits<header.count || header.bits>65536 || header.max_bits<header.bits ||
       (!header.flags && header.count>128) || (header.count && !header.data)) return false;
    std::vector<Row> all(header.count);std::vector<uint32_t> bits((header.count+31)/32);
    if(header.count && (core->read_memory(core->user,header.data,{reinterpret_cast<uint8_t*>(all.data()),all.size()*24}).code!=0 ||
       core->read_memory(core->user,header.flags?header.flags:table+64,{reinterpret_cast<uint8_t*>(bits.data()),bits.size()*4}).code!=0)) return false;
    result.clear();
    for(int i=0;i<header.count;++i) if(bits[i/32]&(1u<<(i%32))) {if(all[i].key.id && all[i].data) result.push_back(all[i]);}
    return true;
}
std::string WideString(const Reader& r,uintptr_t address) {
    RowArray value{};if(!r.Read(address,value) || !value.data || value.count<1 || value.count>1024 || value.capacity<value.count) return {};
    std::vector<wchar_t> wide(value.count);
    if(core->read_memory(core->user,value.data,{reinterpret_cast<uint8_t*>(wide.data()),wide.size()*2}).code!=0 || wide.back()!=0) return {};
    const auto bytes=WideCharToMultiByte(CP_UTF8,0,wide.data(),value.count-1,nullptr,0,nullptr,nullptr);
    if(bytes<=0) return {};
    std::string text(bytes,'\0');WideCharToMultiByte(CP_UTF8,0,wide.data(),value.count-1,text.data(),bytes,nullptr,nullptr);return text;
}
uintptr_t TextString(uintptr_t function,uintptr_t text) {
    __try {return reinterpret_cast<uintptr_t(__fastcall*)(const void*)>(function)(reinterpret_cast<const void*>(text));}
    __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
std::string Text(const Reader& r,uintptr_t address) {
    char buffer[1024]{};size_t bytes=sizeof(buffer);
    if(names->resolve_ftext_utf8 && names->resolve_ftext_utf8(names->user,address,buffer,&bytes).code==0 && buffer[0]) return buffer;
    if(!text_string) {
        const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        if(sig && sig->resolve) sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
            StringView("40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 0B 48 8B 01 48 83 C4 20"),&text_string);
    }
    const auto string=text_string?TextString(text_string,address):0;return string?WideString(r,string):std::string{};
}
bool Layout(const Reader& r,uintptr_t table,const char* field,int offset,int size) {
    uintptr_t structure{};return r.Read(table+40,structure) && r.Property(structure,field,offset,size);
}
Entry* Scene(std::string_view key) {
    if(!building) return nullptr;
    for(auto& entry:building->scenes) if(entry.key==key) return &entry;
    return nullptr;
}
bool StartCatalog(const Reader& r,uintptr_t util) {
    const auto gd_cdo=Find(r,"/Script/HTGame.Default__HTGameData"),get=Fn(r,"HTGame","HTGameData","GetGameData",1,8);
    uintptr_t gd{};if(!gd_cdo || !get || !Invoke(r,gd_cdo,get,&gd) || !r.IsA(gd,"HTGameData")) return false;
    FName home{};const auto home_offset=Field(r,gd,"BigWorldLevelID",8);
    if(home_offset>=0 && r.Read(gd+home_offset,home)) big_world=r.Name(home);
    const auto levels=Child(r,gd,"LevelConfigDataTable"),transfers=Child(r,Child(r,gd,"TeleportDataAsset"),"TransferDataTable");
    if(!Layout(r,levels,"SceneType",8,1) || !Layout(r,levels,"Title",16,16) ||
       !Layout(r,levels,"SingleExtensionData",112,16) || !Rows(r,levels,"LevelConfigData",level_rows)) return false;
    if(big_world.empty()) {
        // The loaded SDK's BigWorldLevelID is unset. Use the world verified in
        // the current NTE build only when that outdoor map exists in its catalog.
        for(const auto& row:level_rows) {
            uint8_t type{};
            if(r.Name(row.key)=="XL_map_bigworld_test" && r.Read(row.data+8,type) && type==0) {
                big_world=r.Name(row.key);Log("scene-loader big-world fallback="+big_world);break;
            }
        }
    }
    if(transfers && Layout(r,transfers,"BelongLevelName",112,16) && Layout(r,transfers,"Transform",16,96) &&
       Layout(r,transfers,"TransferCondition",176,16)) Rows(r,transfers,"StaticTransformData",transfer_rows);
    uintptr_t clones{};const auto clone_get=Fn(r,"HTGame","HTUtil","GetCloneSystemDataAsset",1,8);
    if(clone_get && Invoke(r,util,clone_get,&clones)) {
        clones=Child(r,clones,"DT_CloneSystemData");
        if(clones && Layout(r,clones,"CloneID",8,8) && Layout(r,clones,"CloneType",0x49,1) &&
           Layout(r,clones,"CloneLevelInstance",0x68,40)) Rows(r,clones,"CloneSystemData",clone_rows);
    }
    level_table=Handle(r,levels);transfer_table=transfers?Handle(r,transfers):AnomalyGenerationHandleV1{};
    clone_table=clones?Handle(r,clones):AnomalyGenerationHandleV1{};
    building=std::make_shared<Catalog>();return_points.clear();level_cursor=transfer_cursor=clone_cursor=0;return true;
}
void BuildCatalog(const Reader& r,uintptr_t util) {
    if(catalog || Clock::now()<next_catalog) return;
    next_catalog=Clock::now()+std::chrono::milliseconds(100);
    if(!building && !StartCatalog(r,util)) {next_catalog=Clock::now()+std::chrono::seconds(5);return;}
    if(!Resolve(r,objects,level_table)) {building={};next_catalog=Clock::now()+std::chrono::seconds(5);return;}
    if(level_cursor<level_rows.size()) {
        const auto end=std::min(level_cursor+8,level_rows.size());
        for(;level_cursor<end;++level_cursor) {
            const auto row=level_rows[level_cursor];Entry entry{};entry.id=row.key;entry.key=r.Name(row.key);
            if(!r.Read(row.data+8,entry.type)) continue;
            entry.title=Text(r,row.data+16);if(entry.title.empty()) entry.title=entry.key;
            uintptr_t extension{},data{};
            if(entry.type==2 && r.Read(row.data+112,extension) && r.Read(row.data+120,data) && extension && data &&
               r.NameAt(extension+24)=="SingleLevelExtensionData" && r.Property(extension,"EnterTransformID",80,8)) {
                FName entrance{};
                if(r.Read(data+80,entrance) && entrance.id) entry.routes.push_back({row.key,scene::RouteKind::Single,0,1,T("route.default","Default entrance")});
                if(r.Property(extension,"ExitMapName",0,16) && r.Property(extension,"Translation",40,24) &&
                   WideString(r,data)==big_world && r.Read(data+40,entry.exit_position)) entry.exit_to_world=true;
            }
            building->scenes.push_back(std::move(entry));
        }
        return;
    }
    if(transfer_cursor<transfer_rows.size()) {
        if(!Resolve(r,objects,transfer_table)) transfer_cursor=transfer_rows.size();
        const auto end=std::min(transfer_cursor+64,transfer_rows.size());
        for(;transfer_cursor<end;++transfer_cursor) {
            const auto row=transfer_rows[transfer_cursor];const auto target=WideString(r,row.data+112);
            if(auto* entry=Scene(target)) entry->routes.push_back({row.key,scene::RouteKind::Transfer,0,1,r.Name(row.key)});
            uintptr_t condition{};std::array<double,3> position{};
            if(target==big_world && r.Read(row.data+176,condition) && !condition && r.Read(row.data+48,position))
                return_points.push_back({row.key,position});
        }
        return;
    }
    if(clone_cursor<clone_rows.size()) {
        if(!Resolve(r,objects,clone_table)) clone_cursor=clone_rows.size();
        const auto end=std::min(clone_cursor+8,clone_rows.size());
        for(;clone_cursor<end;++clone_cursor) {
            const auto row=clone_rows[clone_cursor];uint8_t cross{},type{};FName id{},map{};
            if(!r.Read(row.data+16,cross) || !cross || !r.Read(row.data+8,id) || !id.id ||
               !r.Read(row.data+0x49,type) || type>=27 || !r.Read(row.data+0x68+16,map)) continue;
            auto* entry=Scene(r.Name(map));if(!entry) continue;
            int32_t difficulty{1};RowArray nodes{};
            const auto node_type=Find(r,"/Script/HTGame.CloneSystemSubNodeData");
            if(node_type && r.Property(node_type,"DifficultyLevel",124,4) && r.Read(row.data+0x58,nodes) && nodes.data && nodes.count>0)
                r.Read(nodes.data+124,difficulty);
            if(difficulty<1) difficulty=1;
            auto label=Text(r,row.data+0x38);if(label.empty()) label=r.Name(id);
            entry->routes.push_back({id,scene::RouteKind::Clone,type,difficulty,
                F("route.difficulty","{0} / Difficulty {1}",{label,std::to_string(difficulty)})});
        }
        return;
    }
    std::sort(building->scenes.begin(),building->scenes.end(),[](const auto& a,const auto& b) {
        if(a.type!=b.type) return a.type<b.type;return a.key<b.key;
    });
    size_t available{};for(const auto& entry:building->scenes) if(scene::CanEnter(entry.type,!entry.routes.empty())) ++available;
    Log("scene-loader catalog scenes="+std::to_string(building->scenes.size())+" available="+std::to_string(available));
    std::string listed;
    for(const auto& entry:building->scenes) if(scene::CanEnter(entry.type,!entry.routes.empty()))
        listed+=(listed.empty()?"":", ")+entry.key+"|"+scene::TypeName(entry.type).fallback+"|"+entry.title;
    Log("scene-loader available scenes: "+listed);
    message=T("status.ready","Select a scene and an entrance, then switch.");catalog=building;building={};
}
const Entry* CurrentEntry() {
    const auto data=catalog?catalog:building;
    if(data) for(const auto& entry:data->scenes) if(entry.key==current_scene) return &entry;
    return nullptr;
}
bool RemoveDelegate(uintptr_t function,uintptr_t list,int index) {
    __try {reinterpret_cast<void(__fastcall*)(void*,int)>(function)(reinterpret_cast<void*>(list),index);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool DetachErrorExit(const Reader& r,uintptr_t widget,uintptr_t hud) {
    const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
    if(!sig || !sig->resolve) return false;
    const auto resolve=[&](uintptr_t& address,const char* pattern) {
        return address || sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),StringView(pattern),&address).code==0;
    };
    if(!resolve(error_exit_exec,"40 53 48 83 EC 20 48 8D 59 ?? 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? C7 80 ?? ?? ?? ?? ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 72 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B C8 E8") ||
       !resolve(delegate_remove,"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 8B 29 48 8B F9 48 63 F2 48 8B DE 48 C1 E3 04") ||
       !resolve(deactivate_signal,"40 57 48 83 EC 40 80 B9 ?? ?? ?? ?? ?? 48 8B F9 0F 85 ?? ?? ?? ?? 80 B9 ?? ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 48 8B 01")) return false;
    // The validated deactivation implementation broadcasts this native delegate
    // list. Derive its offset from LEA RCX,[RDI+offset], rather than assuming a UI layout.
    std::array<uint8_t,7> lea{};int32_t offset{};
    if(!r.Read(deactivate_signal+0x125,lea) || lea[0]!=0x48 || lea[1]!=0x8d || lea[2]!=0x8f) return false;
    std::memcpy(&offset,lea.data()+3,4);
    if(offset<0x3e0 || offset>0x800) return false;
    struct List {uintptr_t data;int32_t count,capacity,compact_threshold,lock_depth;} list{};
    if(!r.Read(widget+offset,list) || !list.data || list.count<1 || list.count>32 ||
       list.capacity<list.count || list.capacity>128 || list.lock_depth) return false;
    const auto owner=Handle(r,hud);if(!owner.id) return false;
    for(int i=list.count-1;i>=0;--i) {
        uintptr_t instance{},vtable{},execute{};uint32_t units{},index{},serial{};
        if(!r.Read(list.data+16*i,instance) || !r.Read(list.data+16*i+8,units) || units!=2 ||
           !r.Read(instance,vtable) || !r.Read(vtable+96,execute) || execute!=error_exit_exec ||
           !r.Read(instance+24,index) || !r.Read(instance+28,serial) ||
           index!=ANOMALY_UE5_OBJECT_HANDLE_INDEX(owner) || serial!=ANOMALY_UE5_OBJECT_HANDLE_SERIAL(owner)) continue;
        // Use the game's delegate removal/destructor, preserving its allocator and
        // all other listeners (input focus, UI stack, widget pooling).
        if(!RemoveDelegate(delegate_remove,widget+offset,i)) return false;
        int32_t remaining{};
        if(!r.Read(widget+offset+8,remaining) || remaining!=list.count-1) return false;
        Log("scene-loader detached scene error return-to-login listener");return true;
    }
    return false;
}
bool DismissSceneError(const Reader& r,uintptr_t pc) {
    const auto hud=Child(r,pc,"MyHUD");
    const auto offset=Field(r,hud,"ActivedWidgetMap",80);
    const auto get=Fn(r,"HTGame","HTHUD","BP_GetUIByClass",2,24);
    const auto deactivate=Fn(r,"CommonUI","CommonActivatableWidget","DeactivateWidget",0,0);
    MapHeader map{};
    if(!r.IsA(hud,"HTHUD") || offset<0 || !get || !deactivate || !r.Read(hud+offset,map) ||
       map.count<0 || map.count>256 || map.capacity<map.count || map.bits<map.count ||
       (!map.flags && map.count>128) || (map.count && !map.data)) return false;
    for(int i=0;i<map.count;++i) {
        uint32_t allocated{};
        if(map.flags) {if(!r.Read(map.flags+4*(i/32),allocated)) continue;}
        else allocated=map.inline_bits[i/32];
        if(!(allocated&(1u<<(i%32)))) continue;
        uintptr_t type{};FName key{};
        if(!r.Read(map.data+24*i,type) || !r.Read(map.data+24*i+8,key)) continue;
        bool message_box=false;
        for(int depth=0;type && depth<32;++depth) {
            if(r.NameAt(type+24)=="HTUI_MessageBox") {message_box=true;break;}
            if(!r.Read(type+64,type)) break;
        }
        if(!message_box) continue;
        const auto label=r.Name(key);if(label.empty() || label.size()>128) continue;
        std::wstring wide(label.begin(),label.end());
        struct Query {uintptr_t text;int32_t count,capacity;uintptr_t widget;} q{
            reinterpret_cast<uintptr_t>(wide.c_str()),static_cast<int32_t>(wide.size()+1),static_cast<int32_t>(wide.size()+1),0};
        if(!Invoke(r,hud,get,&q) || !r.IsA(q.widget,"HTUI_MessageBox")) continue;
        uint8_t active{};const auto active_offset=Field(r,q.widget,"bIsActive",1);
        if(active_offset<0 || !r.Read(q.widget+active_offset,active) || !active) continue;
        const auto content=Child(r,q.widget,"Content");
        const auto text_offset=Field(r,content,"TypingText",16);
        auto text=text_offset>=0?Text(r,content+text_offset):std::string{};
        if(!scene::SceneConnectionError(text)) {
            const auto visible_offset=Field(r,content,"Text",16);
            if(visible_offset>=0) text=Text(r,content+visible_offset);
        }
        if(!scene::SceneConnectionError(text)) continue;
        if(!DetachErrorExit(r,q.widget,hud)) {Log("scene-loader could not detach scene error exit listener");return false;}
        // The error has a native deactivation listener that logs out even without
        // pressing Confirm. Remove that one listener before normal stack cleanup.
        if(!Invoke(r,q.widget,deactivate,nullptr)) return false;
        if(!r.Read(q.widget+active_offset,active) || active) return false;
        Log("scene-loader dismissed scene connection error");return true;
    }
    return false;
}
bool ForceReturn(const Reader& r,uintptr_t ps,uintptr_t pc,uint64_t now) {
    const auto* entry=CurrentEntry();
    if(entry && (entry->type==1 || entry->type==4)) {
        force_return=false;return_queued_at=0;message=T("status.at_login","Back at the main menu; sign in to the game again first.");return false;
    }
    if(!r.IsA(pc,"HTPlayerController") || !r.IsA(ps,"HTPlayerState") || big_world.empty() || return_points.empty()) {
        message=T("status.return_waiting","Return request kept; waiting for the player controller and a big-world entrance.");return false;
    }
    std::array<double,3> anchor=last_world_position;
    if(!have_world_position && entry && entry->exit_to_world) anchor=entry->exit_position;
    std::vector<std::array<double,3>> positions;for(const auto& point:return_points) positions.push_back(point.position);
    size_t pick=have_world_position || (entry && entry->exit_to_world)?scene::Nearest(positions,anchor):0;
    if(pick>=return_points.size()) {force_return=false;message=T("status.return_no_point","No valid big-world return position was found.");return false;}
    // Use the game's ForceTransfer_FadeOut mode. Single-level exit also clears
    // its scene-specific state before invoking the configured destination transfer.
    struct Args {FName id;uint8_t loading;} args{return_points[pick].id,3};
    const bool single=entry && entry->type==2;
    const auto fn=Fn(r,"HTGame",single?"HTPlayerState":"HTPlayerController",
        single?"RequestExitSingleLevelScene":"K2_ServerRequestCustomTransfer",2,9);
    if(!fn) {force_return=false;message=T("status.return_unavailable","The return function is not available in this game version.");return false;}
    pending.store({});return_ack=0;return_entrance=NameKey(args.id);
    transition.Recover(current_scene,big_world,now);force_return=false;return_queued_at=0;
    // The single-level exit goes through the very RPC that may be held; let it through.
    ReleaseExitHold();
    if(!Invoke(r,single?ps:pc,fn,&args)) {transition.busy=false;return_entrance=0;message=T("status.return_send_failed","The return request could not be sent.");return false;}
    message=T("status.return_sent","Force return to the big world requested.");
    Log("scene-loader force return from="+current_scene+" target="+big_world+" entrance="+r.Name(args.id));return true;
}
#pragma pack(push,1)
struct LayerStateQuery {uintptr_t instance;uint8_t state;};
#pragma pack(pop)
static_assert(sizeof(LayerStateQuery)==9);
// One ClientDataLayerRPC message, laid out as captured from server traffic:
//   envelope: ParameterWrappers {data, count=1, capacity=1} @0, ReadParameterIndex=0 @16, Type @24
//   wrapper : tag 0x0F @0, TArray<uint8> {data, count, capacity} @16
// The bytes live in plugin-owned static storage, valid for as long as the plugin is loaded.
struct LayerMessageStorage {uint8_t buffer[160];uint8_t wrapper[32];uint8_t envelope[32];};
LayerMessageStorage layer_message_storage{};
void SetLayerSnapshot(std::vector<LayerRow> rows,std::string text) {
    auto snapshot=std::make_shared<LayerSnapshot>();snapshot->generation=++layer_generation;snapshot->scene=current_scene;
    snapshot->message=std::move(text);snapshot->rows=std::move(rows);layer_snapshot=snapshot;
}
void SetLayerMessage(std::string text,bool working) {
    // Same generation, so the UI keeps its checkbox choices while a job reports progress.
    auto snapshot=std::make_shared<LayerSnapshot>(layer_snapshot?*layer_snapshot:LayerSnapshot{});
    if(!layer_snapshot) snapshot->scene=current_scene;
    snapshot->message=std::move(text);snapshot->working=working;layer_snapshot=snapshot;
}
uintptr_t LayerManager(const Reader& r,uintptr_t world) {
    const auto fn=Fn(r,"Engine","World","GetDataLayerManager",1,8);uintptr_t manager{};
    return fn && Invoke(r,world,fn,&manager)?manager:0;
}
void ReadLayers(const Reader& r,uintptr_t world) {
    const auto manager=LayerManager(r,world);
    const auto list_fn=Fn(r,"Engine","DataLayerManager","GetDataLayerInstances",1,16);
    const auto state_fn=Fn(r,"Engine","DataLayerManager","GetDataLayerInstanceRuntimeState",2,9);
    if(!manager || !list_fn || !state_fn) {SetLayerSnapshot({},T("layers.no_manager","This scene has no usable data layer manager."));return;}
    // The returned TArray is a copy allocated by the game and is not freed here: one small
    // allocation per manual refresh. The zeroed buffer keeps the game's assignment from freeing garbage.
    RowArray list{};
    if(!Invoke(r,manager,list_fn,&list) || list.count<0 || list.count>1024 || (list.count && !list.data)) {
        SetLayerSnapshot({},T("layers.read_failed","Could not read the data layers."));return;
    }
    std::vector<uintptr_t> instances(static_cast<size_t>(list.count));
    if(list.count && core->read_memory(core->user,list.data,{reinterpret_cast<uint8_t*>(instances.data()),instances.size()*sizeof(uintptr_t)}).code!=0) {
        SetLayerSnapshot({},T("layers.read_failed","Could not read the data layers."));return;
    }
    std::vector<LayerRow> rows;
    for(const auto instance:instances) {
        if(!instance) continue;
        LayerRow row{};row.instance=r.NameAt(instance+24);if(row.instance.empty()) continue;
        const auto asset_offset=Field(r,instance,"DataLayerAsset",8);uintptr_t asset{};
        if(asset_offset>=0 && r.Read(instance+asset_offset,asset) && asset) row.asset=r.NameAt(asset+24);
        if(row.asset.empty()) row.asset=row.instance;
        LayerStateQuery query{instance,0xFF};if(Invoke(r,manager,state_fn,&query)) row.state=query.state;
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b) {return a.asset<b.asset;});
    size_t active{};for(const auto& row:rows) if(row.state==2) ++active;
    const auto total=rows.size();
    SetLayerSnapshot(std::move(rows),F("layers.summary","{0} data layers, {1} activated.",{std::to_string(total),std::to_string(active)}));
}
// Neither HTPlayerState RPC is reachable through find_exact in this build, so walk the object
// table for them by name, chunked across ticks. One pass resolves both. Returns true when done.
bool ScanPlayerStateRpcs(const Reader& r) {
    const auto function_class=Find(r,"/Script/CoreUObject.Function");
    uint32_t count{};uintptr_t chunks{};
    if(!function_class || !r.Read(registry+36,count) || count>16*1024*1024 || !r.Read(registry+16,chunks) || !chunks) return true;
    const auto end=std::min<int32_t>(rpc_scan_cursor+6000,static_cast<int32_t>(count));
    for(;rpc_scan_cursor<end;++rpc_scan_cursor) {
        uintptr_t chunk{},object{},cls{},owner{};
        if(!r.Read(chunks+8*(rpc_scan_cursor/65536),chunk) || !chunk || !r.Read(chunk+24*(rpc_scan_cursor%65536),object) ||
           !object || !r.Read(object+16,cls) || cls!=function_class) continue;
        const auto name=r.NameAt(object+24);
        if(name!="ClientDataLayerRPC" && name!="ServerRequestExitSingleLevelSceneRPC") continue;
        if(!r.Read(object+32,owner) || r.NameAt(owner+24)!="HTPlayerState") continue;
        if(name=="ClientDataLayerRPC" && r.Function(object,name,1,32)) layer_rpc=Handle(r,object);
        if(name=="ServerRequestExitSingleLevelSceneRPC" && r.Function(object,name,2,9)) exit_rpc=Handle(r,object);
        if(Resolve(r,objects,layer_rpc) && Resolve(r,objects,exit_rpc)) {rpc_scan_cursor=0;return true;}
    }
    if(rpc_scan_cursor<static_cast<int32_t>(count)) return false;
    rpc_scan_cursor=0;return true;
}
uintptr_t LayerRpc(const Reader& r,bool& scanning) {
    scanning=false;
    if(const auto cached=Resolve(r,objects,layer_rpc)) return cached;
    if(const auto direct=Fn(r,"HTGame","HTPlayerState","ClientDataLayerRPC",1,32)) {layer_rpc=Handle(r,direct);return direct;}
    if(!ScanPlayerStateRpcs(r)) {scanning=true;return 0;}
    return Resolve(r,objects,layer_rpc);
}
uintptr_t ExitRpc(const Reader& r) {
    if(const auto cached=Resolve(r,objects,exit_rpc)) return cached;
    if(const auto direct=Fn(r,"HTGame","HTPlayerState","ServerRequestExitSingleLevelSceneRPC",2,9)) {exit_rpc=Handle(r,direct);return direct;}
    ScanPlayerStateRpcs(r);return Resolve(r,objects,exit_rpc);
}
// Holding the exit request: Func first, then the flag (while FUNC_Net is still set the no-op is
// never reached); release in the opposite order. Every write is read back.
void ReleaseExitHold() {
    if(!exit_held) return;
    exit_watch_fn=0;
    const bool restored=WriteMemory(exit_held_fn+0xB0,&exit_saved_flags,sizeof(exit_saved_flags)) &&
        WriteMemory(exit_held_fn+0xD8,&exit_saved_func,sizeof(exit_saved_func));
    exit_held=false;
    Log(std::string("scene-loader released exit request")+(restored?"":" (restore failed)"));
}
bool HoldExit(const Reader& r) {
    if(exit_held) return true;
    const auto fn=ExitRpc(r);if(!fn) return false;  // still scanning
    uint32_t flags{},check_flags{};uintptr_t func{},check_func{};
    const auto stub=reinterpret_cast<uintptr_t>(&NoOpNative);
    if(!r.Read(fn+0xB0,flags) || !r.Read(fn+0xD8,func) || !scene::CanHoldExit(flags) || !Executable(func)) {
        exit_hold_unavailable=true;Log("scene-loader exit hold refused flags="+std::to_string(flags));return false;
    }
    const auto held_flags=scene::HeldExitFlags(flags);
    if(!WriteMemory(fn+0xD8,&stub,sizeof(stub)) || !WriteMemory(fn+0xB0,&held_flags,sizeof(held_flags)) ||
       !r.Read(fn+0xD8,check_func) || !r.Read(fn+0xB0,check_flags) || check_func!=stub || check_flags!=held_flags) {
        WriteMemory(fn+0xB0,&flags,sizeof(flags));WriteMemory(fn+0xD8,&func,sizeof(func));
        exit_hold_unavailable=true;Log("scene-loader exit hold write failed");return false;
    }
    exit_saved_flags=flags;exit_saved_func=func;exit_held_fn=fn;exit_held=true;exit_watch_fn=fn;
    Log("scene-loader holding exit request scene="+current_scene);
    return true;
}
bool SendLayerMessage(const Reader& r,uintptr_t ps,uintptr_t rpc,const std::string& instance,int32_t type) {
    const auto payload=scene::LayerNamePayload(instance);
    auto& s=layer_message_storage;
    if(payload.size()>sizeof(s.buffer)) return false;
    std::memset(&s,0,sizeof(s));std::memcpy(s.buffer,payload.data(),payload.size());
    const uint64_t tag=scene::kStringParameterTag;const auto buffer=reinterpret_cast<uintptr_t>(s.buffer);const auto size=static_cast<int32_t>(payload.size());
    std::memcpy(s.wrapper,&tag,8);std::memcpy(s.wrapper+16,&buffer,8);std::memcpy(s.wrapper+24,&size,4);std::memcpy(s.wrapper+28,&size,4);
    const auto wrapper=reinterpret_cast<uintptr_t>(s.wrapper);const int32_t one=1;
    std::memcpy(s.envelope,&wrapper,8);std::memcpy(s.envelope+8,&one,4);std::memcpy(s.envelope+12,&one,4);std::memcpy(s.envelope+24,&type,4);
    return Invoke(r,ps,rpc,s.envelope);
}
void StepLayers(const Reader& r,uintptr_t world,uintptr_t ps,uint64_t now) {
    if(layer_job==LayerJob::Read) {layer_job=LayerJob::None;ReadLayers(r,world);return;}
    if(layer_job!=LayerJob::Send) return;
    if(!ps) {SetLayerMessage(T("layers.waiting_player","Waiting for the player state…"),true);return;}
    bool scanning=false;const auto rpc=LayerRpc(r,scanning);
    if(!rpc) {
        if(scanning) {SetLayerMessage(T("layers.locating_rpc","Locating ClientDataLayerRPC (about ten seconds the first time)…"),true);return;}
        layer_job=LayerJob::None;layer_commands.clear();SetLayerMessage(T("layers.rpc_missing","ClientDataLayerRPC is not available in this game version."),false);return;
    }
    size_t opened{},closed{};
    for(const auto& command:layer_commands) if(SendLayerMessage(r,ps,rpc,command.instance,command.type))
        ++(command.type==scene::kActivateLayerMessage?opened:closed);
    Log("scene-loader layer messages scene="+current_scene+" open="+std::to_string(opened)+" close="+std::to_string(closed)+
        " of "+std::to_string(layer_commands.size()));
    layer_job=LayerJob::None;layer_commands.clear();layer_recheck_at=now+1000;
    SetLayerMessage(F("layers.sent","Sent: {0} opened, {1} closed; refreshing…",{std::to_string(opened),std::to_string(closed)}),true);
}
void Publish(bool ready) {
    auto view=std::make_shared<View>();view->catalog=catalog;view->layers=layer_snapshot;view->current=current_scene;view->message=message;
    view->busy=transition.busy;view->ready=ready;view->in_big_world=!big_world.empty() && current_scene==big_world;published.store(view);
}
bool EnsureWindow() {
    if(window.id) return true;
    if(!windows || !windows->register_window || !windows->begin || !windows->end || !windows->state) return false;
    const auto title=T("window.title","Scene Loader");
    AnomalyWindowSpecV1 spec{};spec.struct_size=sizeof(spec);spec.id=StringView("scene-loader-main");spec.title=StringView(title);
    spec.initial_width=560;spec.initial_height=570;spec.minimum_width=360;spec.minimum_height=350;spec.maximum_width=950;spec.maximum_height=950;spec.default_open=1;
    return windows->register_window(windows->user,&spec,&window).code==0 && window.id;
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host,void** context) noexcept {
    api=host;localizer=anomaly::plugins::Localizer(host);core=Host(host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
    names=Host(host).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID,2).get();
    objects=Host(host).Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
    framework=Host(host).Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
    world_service=Host(host).Query<AnomalyUe5WorldServiceV1>(ANOMALY_UE5_WORLD_SERVICE_V1_ID).get();
    windows=Host(host).Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID).get();
    events=Host(host).Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID).get();
    if(context) *context=nullptr;return Ok();
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) noexcept {
    registry=world_slot=text_string=error_exit_exec=delegate_remove=deactivate_signal=0;retry_init=next_tick=next_catalog={};functions.clear();missing_functions.clear();offsets.clear();
    window=level_table=transfer_table=clone_table={};building={};catalog={};level_rows.clear();transfer_rows.clear();clone_rows.clear();
    pending.store({});shown=refresh=force_return=dismiss_error=false;transition={};current_scene.clear();selected=selected_route=category=0;
    cached_ps=cached_pc=subscription={};observed_ps=0;return_entrance=return_ack=0;return_queued_at=0;big_world.clear();have_world_position=false;return_points.clear();only_verified=true;
    if(events && events->subscribe) events->subscribe(events->user,Observe,nullptr,&subscription);
    layer_snapshot={};layer_generation=layer_recheck_at=0;layer_refresh=false;layer_request.store({});layer_job=LayerJob::None;
    layer_commands.clear();layer_rpc={};rpc_scan_cursor=0;
    exit_rpc={};exit_hold_unavailable=false;exit_swallowed=0;exit_reported=0;
    fallback_open=1;filter={};only_available=true;message=T("status.reading","Reading the game's scenes.");Publish(false);EnsureWindow();return Ok();
}
AnomalyStatusV1 ANOMALY_CALL Stop(void*,uint32_t) noexcept {
    ReleaseExitHold();
    pending.store({});shown=false;
    force_return=dismiss_error=false;observed_ps=0;
    if(events && subscription.id && events->unsubscribe) events->unsubscribe(events->user,subscription);
    subscription={};
    if(window.id && windows && windows->release_window) windows->release_window(windows->user,window);
    window={};return Ok();
}
void ANOMALY_CALL Unload(void*) noexcept {}
void ANOMALY_CALL Update(void*,double) noexcept {
    try {
        if(!shown.load() && !transition.busy && !pending.load() && !force_return.load() && !dismiss_error.load() &&
           layer_job==LayerJob::None && !layer_recheck_at && !layer_refresh.load() && !layer_request.load() && !exit_held) return;
        if(Clock::now()<next_tick) return;next_tick=Clock::now()+std::chrono::milliseconds(100);
        if(!core || !names || !objects || !framework || !framework->is_game_thread || !framework->is_game_thread(framework->user)) return;
        const Reader r{names};if(!Initialize(r,objects)) {message=T("status.waiting_game","Waiting for the game interfaces.");Publish(false);return;}
        const auto world=World(r),util=Find(r,"/Script/HTGame.Default__HTUtil");
        const auto now=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        if(force_return.load()) {
            if(!return_queued_at) {pending.store({});transition.busy=false;return_queued_at=now;}
            if(now-return_queued_at>=5000) {force_return=false;return_queued_at=0;message=T("status.return_timeout","The return entrance is not ready, so the wait was cancelled; normal scene switching still works.");}
            if(force_return.load() && ForceReturn(r,Resolve(r,objects,cached_ps),Resolve(r,objects,cached_pc),now)) {Publish(false);return;}
        } else return_queued_at=0;
        if(!world || !util) {Publish(false);return;}
        FName world_name{};r.Read(world+24,world_name);world_name.number=0;current_scene=r.Name(world_name);
        if(layer_snapshot && layer_snapshot->scene!=current_scene) {
            // Layers belong to one world; drop the list and any job when the scene changes.
            layer_snapshot={};layer_job=LayerJob::None;layer_commands.clear();layer_recheck_at=0;
        }
        if(refresh.exchange(false) && !transition.busy) {catalog={};building={};next_catalog={};level_rows.clear();transfer_rows.clear();clone_rows.clear();}
        BuildCatalog(r,util);
        const auto ps_get=Fn(r,"HTGame","HTUtil","GetMainPlayerState",2,16),pc_get=Fn(r,"HTGame","HTUtil","GetMainPlayerController",2,16);
        std::array<uintptr_t,2> ps{world,0},pc{world,0};
        const bool ready=ps_get && pc_get && Invoke(r,util,ps_get,&ps) && Invoke(r,util,pc_get,&pc) && r.IsA(ps[1],"HTPlayerState") && r.IsA(pc[1],"HTPlayerController");
        if(ready) {
            cached_ps=Handle(r,ps[1]);cached_pc=Handle(r,pc[1]);observed_ps=ps[1];
            if(current_scene==big_world && !transition.busy && !force_return.load()) {
                const auto character_get=Fn(r,"HTGame","HTUtil","GetMainPlayerCharacter",2,16),location=Fn(r,"Engine","Actor","K2_GetActorLocation",1,24);
                std::array<uintptr_t,2> character{world,0};
                if(character_get && location && Invoke(r,util,character_get,&character) && character[1] &&
                   Invoke(r,character[1],location,&last_world_position)) have_world_position=true;
            }
        }
        if(dismiss_error.exchange(false)) {
            if(ready && DismissSceneError(r,pc[1])) {
                pending.store({});force_return=false;return_queued_at=0;transition.busy=false;return_entrance=return_ack=0;
                message=T("status.error_dismissed","Closed the scene error dialog; check that the character can move.");Publish(ready);return;
            }
            message=T("status.error_not_found","There is no scene connection error dialog to close.");Publish(ready);return;
        }
        const auto* current=CurrentEntry();
        if(current && (current->type==1 || current->type==4)) {cached_ps=cached_pc={};observed_ps=0;}
        if(force_return.load()) {
            if(ForceReturn(r,ps[1],pc[1],now)) {Publish(ready);return;}
        }
        if(transition.busy) {
            const auto state=transition.Check(current_scene,now,return_ack.load()!=0);
            if(state==scene::Transition::Result::Arrived) {message=F("status.arrived","Switched to: {0}",{current_scene});Log("scene-loader arrived "+current_scene);}
            else if(state==scene::Transition::Result::Different) message=F("status.changed","The current scene changed to: {0}",{current_scene});
            else if(state==scene::Transition::Result::Timeout) message=T("status.timeout","Switch not confirmed; check whether this entrance needs a quest or an unlock.");
            if(!transition.busy) return_entrance=return_ack=0;
        }
        const auto send_route=[&](const Request& request)->bool {
            if(request.route.kind==scene::RouteKind::Clone) {
                struct Args {FName id;int32_t level;uint8_t type;uint8_t padding[3]{};FName extra{};} args{request.route.id,request.route.level,request.route.clone_type};
                const auto fn=Fn(r,"HTGame","HTPlayerState","EnterCloneSceneRequest",4,24);return fn && Invoke(r,ps[1],fn,&args);
            }
            struct Args {FName id;uint8_t loading{};} args{request.route.id,0};
            const bool single=request.route.kind==scene::RouteKind::Single;
            const auto fn=Fn(r,"HTGame",single?"HTPlayerState":"HTPlayerController",single?"RequestEnterSingleLevelScene":"K2_ServerRequestCustomTransfer",2,9);
            return fn && Invoke(r,single?ps[1]:pc[1],fn,&args);
        };
        if(const auto request=pending.exchange({})) {
            if(!ready || request->from!=current_scene || transition.busy) message=T("status.stale_request","The scene changed or is loading; select again.");
            else if(request->target==current_scene) {
                // Same scene: another entrance is an in-scene teleport, so no transition is
                // tracked (Begin would refuse current==next anyway).
                if(send_route(*request)) {
                    message=F("status.in_scene_sent","In-scene entrance switch sent: {0}",{request->route.label});
                    Log("scene-loader in-scene entrance="+r.Name(request->route.id)+" scene="+current_scene);
                } else message=T("status.entrance_unavailable","This entrance's switch function is not available.");
            }
            else if(transition.Begin(current_scene,request->target,now)) {
                if(send_route(*request)) {message=F("status.requesting","Requesting a switch to: {0}",{request->target});Log("scene-loader request target="+request->target+" entrance="+r.Name(request->route.id));}
                else {transition.busy=false;message=T("status.scene_unavailable","This scene's switch function is not available.");}
            }
        }
        // Hold the exit request while heading to or staying in a scene that sends the player back
        // by itself; release it for anything else, including the loader's own force return.
        const bool want_hold=transition.busy?scene::HoldsExit(transition.target):scene::HoldsExit(current_scene);
        if(want_hold && !exit_held && !exit_hold_unavailable) HoldExit(r);
        else if(!want_hold && exit_held) ReleaseExitHold();
        if(const int swallowed=exit_swallowed.load(); swallowed!=exit_reported) {
            exit_reported=swallowed;
            message=F("status.exit_held","Held the scene's automatic send-back (time {0}). Use \"Force return to big world\" to leave.",
                {std::to_string(swallowed)});
            Log("scene-loader held exit request count="+std::to_string(swallowed)+" scene="+current_scene);
        }
        if(layer_refresh.exchange(false) && layer_job==LayerJob::None) layer_job=LayerJob::Read;
        if(const auto request=layer_request.exchange({}); request && layer_job==LayerJob::None && !request->empty()) {
            layer_commands=*request;layer_job=LayerJob::Send;SetLayerMessage(T("layers.sending","Sending…"),true);
        }
        if(layer_recheck_at && now>=layer_recheck_at) {layer_recheck_at=0;if(layer_job==LayerJob::None) layer_job=LayerJob::Read;}
        if(layer_job!=LayerJob::None) StepLayers(r,world,ready?ps[1]:0,now);
        Publish(ready);
    } catch(...) {message=T("status.refreshing","Scene data is refreshing; try again shortly.");transition.busy=false;layer_job=LayerJob::None;layer_commands.clear();Publish(false);}
}
void LayerContent(const AnomalyUiServiceV1* ui,const View& view) {
    const auto button=[&](const std::string& label,bool enabled) {
        const bool clicked=ui->button_enabled?ui->button_enabled(ui->user,StringView(label),0,0,enabled):ui->button(ui->user,StringView(label),0,0);
        return clicked && enabled;
    };
    if(ui->separator) ui->separator(ui->user);
    ui->text(ui->user,StringView(T("layers.title","Scene data layers (experimental)")));
    const auto layers=view.layers;
    const bool here=layers && layers->scene==view.current;
    const bool idle=!here || !layers->working;
    if(here && !layers->message.empty()) ui->text(ui->user,StringView(layers->message));
    if(button(localizer.Label("layers.read","Read this scene's data layers","layers-read"),view.ready && idle)) layer_refresh=true;
    if(!here || layers->rows.empty()) return;
    // Each checkbox mirrors the layer's real state (checked = activated). Ticking sends the open
    // message, unticking the close message; the list is re-read right after, so a change the game
    // did not apply springs back.
    // Single layers can be toggled in the big world too (live-tested); the bulk buttons stay off
    // there, since the big world mixes in layers that must stay closed (W_Unload_*) and quest stages.
    const bool can_send=view.ready && idle;
    const bool can_bulk=can_send && !view.in_big_world;
    auto commands=std::make_shared<std::vector<LayerCommand>>();
    const bool child=ui->begin_child && ui->end_child;
    if(!child || ui->begin_child(ui->user,StringView("layer-list"),0,180,0)) {
        for(const auto& row:layers->rows) {
            const bool active=row.state==2;int value=active?1:0;
            const auto label=row.asset+"  ["+T(scene::LayerStateName(row.state))+"]##layer-"+row.instance;
            if(!ui->checkbox) {ui->text(ui->user,StringView(label));continue;}
            if(ui->checkbox(ui->user,StringView(label),&value) && can_send && (value!=0)!=active)
                commands->push_back({row.instance,value?scene::kActivateLayerMessage:scene::kDeactivateLayerMessage});
        }
    }
    if(child) ui->end_child(ui->user);
    size_t inactive{},active{};for(const auto& row:layers->rows) ++(row.state==2?active:inactive);
    if(button(F("layers.open_all","Open all ({0})",{std::to_string(inactive)})+"###layers-open-all",can_bulk && inactive>0))
        for(const auto& row:layers->rows) if(row.state!=2) commands->push_back({row.instance,scene::kActivateLayerMessage});
    if(button(F("layers.close_all","Close all ({0})",{std::to_string(active)})+"###layers-close-all",can_bulk && active>0))
        for(const auto& row:layers->rows) if(row.state==2) commands->push_back({row.instance,scene::kDeactivateLayerMessage});
    if(!commands->empty()) layer_request.store(commands);
    if(view.in_big_world) ui->text(ui->user,StringView(T("layers.big_world",
        "Big world: single layers can be toggled, the bulk buttons are off; the server resends the layers after you leave the area or teleport.")));
    ui->text(ui->user,StringView(T("layers.local_only",
        "Local only: the server does not know about these layer changes, so some interactions may not work; reopen them after leaving the scene.")));
}
void Content(const AnomalyUiServiceV1* ui) {
    const auto view=published.load();if(!view) return;
    ui->text(ui->user,StringView(F("ui.current","Current scene: {0}",{view->current.empty()?T("ui.waiting_game","waiting for the game"):view->current})));
    if(ui->button(ui->user,StringView(localizer.Label("ui.force_return","Force return to big world","force-return")),0,0)) {pending.store({});force_return=true;}
    if(ui->button(ui->user,StringView(localizer.Label("ui.dismiss_error","Close scene error dialog","dismiss-error")),0,0)) dismiss_error=true;
    if(ui->input_text) ui->input_text(ui->user,StringView(localizer.Label("ui.search","Search scenes","search")),filter.data(),filter.size(),0);
    int available=only_available;
    if(ui->checkbox && ui->checkbox(ui->user,StringView(localizer.Label("ui.only_available","Only switchable scenes","only-available")),&available)) only_available=available!=0;
    int verified=only_verified;
    if(ui->checkbox && ui->checkbox(ui->user,StringView(localizer.Label("ui.only_verified","Only verified scenes","only-verified")),&verified)) only_verified=verified!=0;
    if(ui->button(ui->user,StringView(localizer.Label("ui.refresh","Refresh list","refresh")),0,0)) refresh=true;
    if(ui->separator) ui->separator(ui->user);
    const auto data=view->catalog;
    if(data) {
        if(only_verified && (selected<0 || static_cast<size_t>(selected)>=data->scenes.size() || !scene::Verified(data->scenes[selected].key))) {
            for(size_t i=0;i<data->scenes.size();++i) if(scene::Verified(data->scenes[i].key)) {selected=static_cast<int>(i);selected_route=0;break;}
        }
        ui->text(ui->user,StringView(F("ui.total","Scenes: {0}",{std::to_string(data->scenes.size())})));
        const bool child=ui->begin_child && ui->end_child;
        const bool display=!child || ui->begin_child(ui->user,StringView("scene-list"),0,270,0);
        if(display) for(size_t i=0;i<data->scenes.size();++i) {
            const auto& entry=data->scenes[i];const bool can=scene::CanEnter(entry.type,!entry.routes.empty());
            if(only_available && !can) continue;
            if(only_verified && !scene::Verified(entry.key)) continue;
            const auto label=entry.title+" / "+T(scene::TypeName(entry.type))+" / "+T(scene::StatusLabel(entry.key));
            const std::string_view query(filter.data());
            if(!query.empty() && label.find(query)==std::string::npos && entry.key.find(query)==std::string::npos) continue;
            if(ui->button(ui->user,StringView((selected==static_cast<int>(i)?"● ":"")+label+"##scene-"+entry.key),0,0)) {selected=static_cast<int>(i);selected_route=0;}
        }
        if(child) ui->end_child(ui->user);
        if(selected>=0 && static_cast<size_t>(selected)<data->scenes.size()) {
            const auto& entry=data->scenes[selected];
            if(ui->separator) ui->separator(ui->user);
            ui->text(ui->user,StringView(F("ui.selected","Selected: {0}",{entry.title})));ui->text(ui->user,StringView(entry.key));
            if(scene::HoldsExit(entry.key)) ui->text(ui->user,StringView(T("ui.holds_exit",
                "Entering holds the scene's own send-back request; use \"Force return to big world\" to leave.")));
            if(scene::Unusable(entry.key)) ui->text(ui->user,StringView(F("ui.unusable","This scene is unusable: {0}",{T(scene::UnusableReason(entry.key))})));
            else if(!scene::Verified(entry.key)) ui->text(ui->user,StringView(T("ui.unverified",
                "This scene is not verified; the connection may fail and return you to the main menu.")));
            if(!entry.routes.empty()) {
                if(selected_route<0 || static_cast<size_t>(selected_route)>=entry.routes.size()) selected_route=0;
                if(ui->begin_menu && ui->end_menu && ui->begin_menu(ui->user,StringView(F("ui.entrance","Entrance: {0}",{entry.routes[selected_route].label})),1)) {
                    for(size_t i=0;i<entry.routes.size();++i) if(ui->button(ui->user,StringView(entry.routes[i].label+"##route-"+std::to_string(i)),0,0)) selected_route=static_cast<int>(i);
                    ui->end_menu(ui->user);
                }
            } else ui->text(ui->user,StringView(T("ui.no_entrance","No usable game entrance.")));
            const bool same=entry.key==view->current;
            const bool can=view->ready && !view->busy && !pending.load() && !scene::Unusable(entry.key) &&
                (!only_verified || scene::Verified(entry.key)) && scene::CanEnter(entry.type,!entry.routes.empty()) &&
                (!same || scene::SameSceneSwitch(view->current,entry.key,!entry.routes.empty()));
            const auto action_label=same?localizer.Label("ui.switch_in_scene","Switch entrance in this scene","switch")
                                       :localizer.Label("ui.switch","Switch to the selected scene","switch");
            const auto action=StringView(action_label);
            const bool clicked=ui->button_enabled?ui->button_enabled(ui->user,action,0,0,can):ui->button(ui->user,action,0,0);
            if(clicked && can) {force_return=false;pending.store(std::make_shared<const Request>(Request{view->current,entry.key,entry.routes[selected_route]}));}
        }
    }
    ui->text(ui->user,StringView(view->message));
    LayerContent(ui,*view);
}
void ANOMALY_CALL Draw(void*,const AnomalyUiServiceV1* ui) noexcept {
    try {
        shown=false;if(!ui || !ui->text || !ui->button) return;
        if(EnsureWindow()) {
            AnomalyWindowStateV1 state{sizeof(state)};if(windows->state(windows->user,window,&state).code!=0 || !state.open) return;
            int32_t open{};if(windows->begin(windows->user,window,0,&open).code!=0) return;
            if(open) {shown=true;Content(ui);}windows->end(windows->user,window);return;
        }
        const auto title=localizer.Label("window.title","Scene Loader","scene-loader-main");
        UiWindow panel(ui,title,&fallback_open);if(panel) {shown=true;Content(ui);}
    } catch(...) {}
}
}
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if(!descriptor || descriptor->struct_size<sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT,0,{}};
    *descriptor={sizeof(*descriptor),ANOMALY_PLUGIN_API_V1_MAJOR,ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("b1ank.scene-loader"),StringView("Scene Loader"),StringView("b1ank, CCYellowStar"),StringView("0.1.7"),Load,Start,Stop,Unload,Update,Draw};return Ok();
}
