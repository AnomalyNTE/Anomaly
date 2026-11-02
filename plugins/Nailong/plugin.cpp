// 奶龙原生角色替换：UE ProceduralMeshComponent，不是 D3D12 画面叠加。

#include "ue_model.hpp"

#include "ue_pose_driver.hpp"

#include "ue_update_route.hpp"

#include "local_actor_select.hpp"

#include "npc_select.hpp"

#include "ue_native_bridge.hpp"

#include <anomaly/sdk/services/nte.h>

#include <anomaly/sdk/services/ui.h>

#include <anomaly/sdk/services/plugin_state.h>

#include <atomic>

#include <cmath>

#include <cstdio>

#include <filesystem>

#include <mutex>

#include <string>

#include <vector>

namespace {

constexpr char kId[]="com.nailong.costume";

constexpr char kVersion[]="1.7.7";

const AnomalyHostApiV1* host=nullptr;

const AnomalyNteActorsServiceV1* actors=nullptr;

const AnomalyNtePlayerServiceV1* player=nullptr;

std::mutex state_mutex;

struct Settings {bool enabled=true,bone_follow=true,nearby_npcs=true;float height=1.8f,offset=90,yaw=0,pose_fps=30,npc_radius=60;int npc_limit=256;int npc_pose_fps=15;std::uint64_t revision=0;};

Settings settings;

std::string status="未启用；使用 UE 原生场景模型替换";

nailong::ModelAsset model;

nailong::SourceRig source_rig;

std::atomic<std::uint64_t> skin_frames{0};

std::atomic<std::uint32_t> mapped_bones{0};

std::atomic<std::uint32_t> actor_total{0},actor_returned{0},actor_flags{0};

std::mutex actor_diag_mutex; std::string actor_class_diag;

std::atomic<bool> active{false};

struct Hidden {ue::Ref object;bool was_visible=false;std::int32_t tick_offset=-1;std::uint8_t tick_original=0;bool tick_changed=false;};

struct Record {ue::Ref actor,component;std::vector<Hidden> hidden;std::uint64_t revision=0;

    std::uint64_t player_generation=0;bool player_generation_valid=false;

    bool bone_follow=false;nailong::PoseDriver driver;std::vector<nailong::EngineSection> prepared;double animation_time=0;
    double next_pose_time=0;
    double distance_m=0;
    bool source_pose_offscreen=false;

} record;

// NPC bookkeeping lives apart from `record` so the 1.7.6 player path keeps its
// exact semantics; every NPC owns its own hidden/component/driver state.
std::vector<Record> npc_records;

// Actor slot index -> the component we created for it. Cleared when the binding
// is restored. This is the authoritative "already dressed" test.
std::unordered_map<std::uintptr_t,std::uintptr_t> dressed_actors;

// Actor pointer -> time when a rebind is allowed again. Prevents restore/rebind
// churn from stacking two ProceduralMeshComponents on the same actor.
std::unordered_map<std::uintptr_t,double> rebind_cooldown;
constexpr double kRebindCooldownSeconds=3.0;

// Components whose K2_DestroyComponent call succeeded. UE may leave these in the
// actor's instance-component array until it is reconstructed, so a subsequent
// K2_GetComponentsByClass can still enumerate them. Keep them hidden and never
// mistake them for a live replacement on the next bind.
std::vector<ue::Ref> retired_components;

bool AlreadyDressed(std::uintptr_t actor){return actor!=0&&dressed_actors.count(actor)!=0;}

void RememberDressed(std::uintptr_t actor,std::uintptr_t component){
    if(actor!=0)dressed_actors[actor]=component;
}

void ForgetDressed(std::uintptr_t actor){
    if(actor!=0)dressed_actors.erase(actor);
}

// ---------------------------------------------------------------------------
// Deferred restore across a plugin stop.
//
// on_stop runs on the host's Lifecycle worker, and every UE reflection call is
// gated on ue::InGame(), which is false outside the Game domain. A stop can
// therefore never put the original meshes back itself.
//
// The previous implementation answered that impossibility with
// ANOMALY_STATUS_V1_CONFLICT. The host treats a non-OK on_stop as a failed stop
// and quarantines the generation (src/plugin/plugin_manager.cpp,
// UnloadIndicesWithDeadline), and quarantined plugins can never be enabled again
// in that process -- so closing the plugin without pressing the in-panel restore
// button forced a game restart.
//
// Instead of failing, remember which actors are still dressed and finish the job
// in the Game domain at the next start. The host gives every plugin a durable
// state directory that survives both a disable and a hot reload.
// Everything RestoreOne() needs, kept in a flat integer layout so it can be
// written from the Lifecycle domain and replayed in the Game domain later.
struct PendingHidden {
    std::uintptr_t object=0;std::uint32_t index=0,serial=0;
    bool was_visible=false;std::int32_t tick_offset=-1;
    std::uint8_t tick_original=0;bool tick_changed=false;
};

struct PendingRecord {
    std::uintptr_t actor=0;std::uint32_t actor_index=0,actor_serial=0;
    std::uintptr_t component=0;std::uint32_t component_index=0,component_serial=0;
    std::vector<PendingHidden> hidden;
};

std::vector<PendingRecord> deferred_restore;

std::atomic<std::uint32_t> npc_count{0};
std::atomic<std::uint32_t> npc_scan_seen{0},npc_scan_range{0},npc_scan_slots{0},npc_scan_class{0},npc_scan_meshes{0},npc_scan_candidates{0};
std::atomic<double> npc_scan_nearest{0};
std::atomic<std::uint32_t> npc_scan_total{0},npc_scan_pages{0};
std::mutex npc_diag_mutex;std::string npc_scan_rejected;
std::uint64_t npc_observed_revision=0;double npc_scan_time=0;

// Persistent knowledge about every actor we have ever inspected. The actor table
// is huge (4343 actors in one city block) and its contents barely change, so the
// expensive part -- resolving the UObject, walking the class chain and probing
// for a mesh -- is done once per actor and cached by (entity_id, serial).
// A rescan then costs one page walk and a distance check per entry.
struct ActorProbe {
    std::uint64_t entity_id=0;
    std::uint32_t serial=0;
    std::uintptr_t ptr=0;
    std::uint8_t kind=0;   // 0 unknown, 1 character, 2 not a character
    std::uint8_t has_mesh=0;
};
std::unordered_map<std::uint64_t,ActorProbe> actor_cache;

// Full scans are slow; keep them rare and make them incremental.
constexpr std::uint32_t kActorsPerPage=64;
constexpr std::uint32_t kPagesPerTick=6;      // ~384 actors / tick, not 4343
// A city block held 4262 actors -- more than 64 pages of 64 -- so the old
// ceiling truncated the table and the tail was never visited.
constexpr double kNpcRescanSeconds=3.0;       // full table sweep cadence
double npc_scan_cursor_time=0;std::uint32_t npc_scan_cursor=0;bool npc_scan_wrapped=false;
double npc_pose_accumulator=0;
std::uint32_t npc_object_scan_cursor=0;
double plugin_time_seconds=0;
double now_seconds(){return plugin_time_seconds;}
// Set by the panel's "bind all now" button. The sweep normally advances a few
// pages per tick to stay off the game thread; a one-shot request runs a complete
// pass immediately so the user does not wait through the incremental walk.
bool npc_bind_now_requested=false;
bool npc_auto_bound=false;

std::uint64_t observed_revision=0;double settle_time=0, retry_time=0, actor_verify_time=0;

bool pending_rebind=false;ue::Ref previous_actor{};

bool model_loaded=false;

// Title-bar close state. Reset to open on every load so a fresh generation shows
// the panel; the Host owns the value after that and toggles it when the user
// reopens the window from the plugin list.
int context_window_open=1;

AnomalyStringViewV1 Sv(const char* s){return {s,std::strlen(s)};}

AnomalyStatusV1 Ok(){return {ANOMALY_STATUS_V1_OK,0,{}};}

void Log(std::uint32_t level,const std::string& s){if(ue::g_core&&ue::g_core->log)ue::g_core->log(ue::g_core->user,level,{s.data(),s.size()});}

void Status(const std::string& s){std::lock_guard lock(state_mutex);status=s;}

// Host-owned, per-plugin directory that outlives a disable and a hot reload.
const AnomalyPluginStateServiceV1* state_dir_service=nullptr;

// The Host refuses plugin-state I/O from the Game and Render domains, so the
// directory is resolved once during on_load and cached as plain text. Game-domain
// code then only touches the cache.
std::string state_directory;

void CacheStateDirectory(){

    state_directory.clear();

    if(!state_dir_service||!state_dir_service->directory)return;

    char dir[1024]{};std::size_t size=sizeof(dir);

    if(!ue::nl_good(state_dir_service->directory(state_dir_service->user,dir,&size)))return;

    state_directory.assign(dir,size>0?size-1:0);

}

bool StateFilePath(const char* name,std::string& out){

    out.clear();

    if(state_directory.empty())return false;

    out=state_directory;out+="/";out+=name;return true;

}

// Called from on_stop (Lifecycle domain): integer file layout only, no UE access.
void SaveDeferredRestore(){

    std::string path;

    if(!StateFilePath("deferred-restore.bin",path))return;

    FILE* f=nullptr;

    if(fopen_s(&f,path.c_str(),"wb")!=0||!f){Status("待恢复记录不可写");return;}

    const std::uint32_t count=std::uint32_t(deferred_restore.size());

    bool ok=fwrite(&count,sizeof(count),1,f)==1;

    for(const auto& pending:deferred_restore){

        if(!ok)break;

        const std::uint32_t hidden=std::uint32_t(pending.hidden.size());

        ok=fwrite(&pending.actor,sizeof(pending.actor),1,f)==1&&
           fwrite(&pending.actor_index,sizeof(pending.actor_index),1,f)==1&&
           fwrite(&pending.actor_serial,sizeof(pending.actor_serial),1,f)==1&&
           fwrite(&pending.component,sizeof(pending.component),1,f)==1&&
           fwrite(&pending.component_index,sizeof(pending.component_index),1,f)==1&&
           fwrite(&pending.component_serial,sizeof(pending.component_serial),1,f)==1&&
           fwrite(&hidden,sizeof(hidden),1,f)==1;

        for(const auto& item:pending.hidden){

            if(!ok)break;

            ok=fwrite(&item.object,sizeof(item.object),1,f)==1&&
               fwrite(&item.index,sizeof(item.index),1,f)==1&&
               fwrite(&item.serial,sizeof(item.serial),1,f)==1&&
               fwrite(&item.was_visible,sizeof(item.was_visible),1,f)==1&&
               fwrite(&item.tick_offset,sizeof(item.tick_offset),1,f)==1&&
               fwrite(&item.tick_original,sizeof(item.tick_original),1,f)==1&&
               fwrite(&item.tick_changed,sizeof(item.tick_changed),1,f)==1;

        }

    }

    fclose(f);

    if(!ok)Status("待恢复记录写入不完整");

}

// Called from on_load (Lifecycle domain). A missing or truncated file simply
// means nothing was left dressed.
void LoadDeferredRestore(){

    deferred_restore.clear();

    std::string path;

    if(!StateFilePath("deferred-restore.bin",path))return;

    FILE* f=nullptr;

    if(fopen_s(&f,path.c_str(),"rb")!=0||!f)return;

    std::uint32_t count=0;

    if(fread(&count,sizeof(count),1,f)!=1||count>4096){fclose(f);return;}

    for(std::uint32_t i=0;i<count;++i){

        PendingRecord pending{};

        std::uint32_t hidden=0;

        if(fread(&pending.actor,sizeof(pending.actor),1,f)!=1||
           fread(&pending.actor_index,sizeof(pending.actor_index),1,f)!=1||
           fread(&pending.actor_serial,sizeof(pending.actor_serial),1,f)!=1||
           fread(&pending.component,sizeof(pending.component),1,f)!=1||
           fread(&pending.component_index,sizeof(pending.component_index),1,f)!=1||
           fread(&pending.component_serial,sizeof(pending.component_serial),1,f)!=1||
           fread(&hidden,sizeof(hidden),1,f)!=1){break;}

        if(hidden>8192)break;

        bool complete=true;

        for(std::uint32_t h=0;h<hidden;++h){

            PendingHidden item{};

            if(fread(&item.object,sizeof(item.object),1,f)!=1||
               fread(&item.index,sizeof(item.index),1,f)!=1||
               fread(&item.serial,sizeof(item.serial),1,f)!=1||
               fread(&item.was_visible,sizeof(item.was_visible),1,f)!=1||
               fread(&item.tick_offset,sizeof(item.tick_offset),1,f)!=1||
               fread(&item.tick_original,sizeof(item.tick_original),1,f)!=1||
               fread(&item.tick_changed,sizeof(item.tick_changed),1,f)!=1){complete=false;break;}

            pending.hidden.push_back(item);

        }

        if(!complete)break;

        if(pending.actor&&pending.actor_serial)deferred_restore.push_back(std::move(pending));

    }

    fclose(f);

    if(!deferred_restore.empty()){

        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong: "+std::to_string(deferred_restore.size())+
            " actor(s) were left dressed by the previous run");

    }

}

void DropDeferredRestoreFile(){

    std::string path;

    if(!StateFilePath("deferred-restore.bin",path))return;

    std::error_code ignored;

    std::filesystem::remove(std::filesystem::path(path),ignored);

}

template<typename T> const T* Query(const char* id){const void* p=nullptr;return host&&host->query_service&&ue::nl_good(host->query_service(host->host_context,Sv(id),1,&p))?static_cast<const T*>(p):nullptr;}

void Services(){

    if(!ue::g_core)ue::g_core=Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);

    if(!state_dir_service)state_dir_service=Query<AnomalyPluginStateServiceV1>(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID);

    if(!ue::g_signature)ue::g_signature=Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);

    if(!ue::g_objects)ue::g_objects=Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);

    if(!ue::g_names)ue::g_names=Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID);

    if(!ue::g_framework)ue::g_framework=Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID);

    if(!actors)actors=Query<AnomalyNteActorsServiceV1>(ANOMALY_NTE_ACTORS_SERVICE_V1_ID);

    if(!player)player=Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID);

}

bool AssetBytes(const char* relative,std::vector<std::uint8_t>& bytes){

    char path[2048]{};std::size_t n=sizeof(path);

    if(!ue::g_core||!ue::g_core->plugin_directory||!ue::nl_good(ue::g_core->plugin_directory(ue::g_core->user,path,&n))){Status("无法取得插件资产目录");return false;}

    std::string utf8=std::string(path)+"/"+relative;

    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),int(utf8.size()),nullptr,0);

    if(count<=0){Status("资产路径 UTF-8 无效");return false;}

    std::wstring wide(std::size_t(count),L'\0');MultiByteToWideChar(CP_UTF8,0,utf8.data(),int(utf8.size()),wide.data(),count);

    HANDLE f=CreateFileW(wide.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);

    if(f==INVALID_HANDLE_VALUE){Status(std::string("资产未找到: ")+relative);return false;}

    LARGE_INTEGER size{};bool valid=GetFileSizeEx(f,&size)&&size.QuadPart>=12&&size.QuadPart<=32*1024*1024;

    bytes.resize(valid?std::size_t(size.QuadPart):0);DWORD read=0;

    valid=valid&&ReadFile(f,bytes.data(),DWORD(bytes.size()),&read,nullptr)&&read==bytes.size();CloseHandle(f);

    if(!valid)Status(std::string("读取资产失败: ")+relative);return valid;

}

bool LoadModel(){

    std::vector<std::uint8_t> bytes;std::string error;

    if(!AssetBytes("assets/mesh/lod0_skinned.nlm",bytes))return false;

    if(!nailong::ParseModel(bytes,model,error)){Status(error);return false;}

    if(!AssetBytes("assets/mesh/nailong_bind.nlbr",bytes))return false;

    if(!nailong::ParseRig(bytes,source_rig,error)||!nailong::ValidateWeights(model,source_rig,error)){Status(error);return false;}

    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong UE: supplied model and full "+std::to_string(source_rig.bones.size())+"-bone bind rig loaded");return true;

}

void SyncActive(){active.store(record.actor.ptr!=0||!npc_records.empty());}

// Per-actor restore. Split out of Restore() so NPCs reuse identical semantics;
// the player wrapper below keeps 1.7.6 bookkeeping byte-for-byte.
bool RestoreOne(Record& target){

    if(!target.actor.ptr)return true;

    const int alive=ue::Alive(target.actor);

    if(alive<0){Status("对象身份读取失败；保留恢复记录");return false;}

    if(alive==0){
        // A dead/streamed-out actor must ALSO get a rebind cooldown. Without it,
        // the next scan re-binds the moment the actor streams back in, while the
        // old ProceduralMesh may still be attached -- two NaiLong bodies.
        if(target.actor.ptr!=0){
            rebind_cooldown[target.actor.ptr]=now_seconds()+kRebindCooldownSeconds;
        }
        ForgetDressed(target.actor.ptr);target={};return true;
    }

    bool complete=true;

    for(auto& h:target.hidden){

        if(!h.was_visible&&!h.tick_changed)continue;

        const int live=ue::Alive(h.object);

        if(live==0){h.was_visible=false;h.tick_changed=false;continue;}

        if(live<0){complete=false;continue;}

        if(h.was_visible){

            if(!ue::SetVisible(h.object.ptr,true)){complete=false;continue;}

            bool visible=false;if(!ue::Visible(h.object.ptr,visible)||!visible){complete=false;continue;}h.was_visible=false;

        }

        if(h.tick_changed){

            auto address=h.object.ptr+std::uintptr_t(h.tick_offset);std::uint8_t restored=0;

            if(!ue::WriteByte(address,h.tick_original)||!ue::ReadByte(address,restored)||restored!=h.tick_original){complete=false;continue;}

            h.tick_changed=false;

        }

    }

    if(!complete){Status("原模型尚未完全恢复，未丢弃恢复记录");return false;}

    if(target.component.ptr){

        const int live=ue::Alive(target.component);

        if(live<0){Status("奶龙组件身份读取失败，保留恢复记录");return false;}

        if(live==1){
            if(std::find_if(retired_components.begin(),retired_components.end(),[&](const ue::Ref& r){return r.ptr==target.component.ptr;})==retired_components.end()){
                retired_components.push_back(target.component);
            }

            // Destruction alone was not removing the renderable component. Clear
            // its geometry and verify the result: a PMC with zero sections cannot
            // leave an old NaiLong body behind even if UE keeps the UObject alive.
            const bool cleared=ue::ClearAllMeshSections(target.component.ptr);
            std::int32_t sections=-1;
            const bool section_read=ue::NumSections(target.component.ptr,sections);
            const bool hidden=ue::SetVisible(target.component.ptr,false);
            ue::Deactivate(target.component.ptr);
            ue::DetachFromParent(target.component.ptr);
            ue::MarkRenderStateDirty(target.component.ptr);
            const bool destroyed=ue::Destroy(target.component.ptr);
            Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "NaiLong retire: component="+std::to_string(target.component.ptr)+
                " cleared="+std::to_string(cleared)+" section_read="+std::to_string(section_read)+
                " sections="+std::to_string(sections)+" hidden="+std::to_string(hidden)+
                " destroy_call="+std::to_string(destroyed));
            if(!cleared||!section_read||sections!=0||!destroyed){
                Status("奶龙网格尚未清空，保留恢复记录");return false;
            }
        }

    }

    // Ownership is released only now: the original meshes are visible again AND the
    // owned component has been destroyed (the block above returned early otherwise).
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong restore: actor="+std::to_string(target.actor.ptr)+
        " component="+std::to_string(target.component.ptr));

    if(target.actor.ptr!=0){
        rebind_cooldown[target.actor.ptr]=now_seconds()+kRebindCooldownSeconds;
    }

    ForgetDressed(target.actor.ptr);

    target={};Status("原角色已恢复；奶龙组件已移除");

    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong UE: original meshes restored; owned PMC removed");return true;

}

bool Restore(){const bool ok=RestoreOne(record);SyncActive();mapped_bones.store(record.actor.ptr?std::uint32_t(record.driver.mapped):0);return ok;}

bool RestoreNpcs(){bool ok=true;for(auto& item:npc_records)ok=RestoreOne(item)&&ok;npc_records.erase(std::remove_if(npc_records.begin(),npc_records.end(),[](const Record& r){return r.actor.ptr==0;}),npc_records.end());npc_count.store(std::uint32_t(npc_records.size()));SyncActive();return ok;}

// Game-domain replay of a record left behind by a previous run. Nothing here
// touches the in-memory `record`/`npc_records`: those describe the current
// session only, and a replayed record may name a pawn that no longer exists.
bool RestoreDeferredOne(PendingRecord& pending){

    if(!pending.actor)return true;

    Record target;

    target.actor={pending.actor,pending.actor_index,pending.actor_serial};
    target.component={pending.component,pending.component_index,pending.component_serial};

    for(const auto& item:pending.hidden){

        Hidden hidden;

        hidden.object={item.object,item.index,item.serial};
        hidden.was_visible=item.was_visible;
        hidden.tick_offset=item.tick_offset;
        hidden.tick_original=item.tick_original;
        hidden.tick_changed=item.tick_changed;

        target.hidden.push_back(hidden);

    }

    const int alive=ue::Alive(target.actor);

    // A streamed-out or recycled actor has nothing left to un-hide.
    if(alive==0){ForgetDressed(pending.actor);return true;}

    if(alive<0)return false;

    return RestoreOne(target);

}

// Runs once per loaded generation, on the first Game tick where the UE bridge
// is usable -- that is the earliest moment a deferred restore can succeed.
double deferred_retry_time=0;

void ReplayDeferredRestore(double dt){

    deferred_retry_time-=dt;

    if(deferred_retry_time>0)return;

    deferred_retry_time=1.0;

    std::size_t remaining=0;

    for(auto& pending:deferred_restore){

        if(!RestoreDeferredOne(pending)){
            // Keep the record: a later tick may find the actor alive again.
            deferred_restore[remaining++]=std::move(pending);
        }

    }

    deferred_restore.resize(remaining);

    if(deferred_restore.empty()){

        DropDeferredRestoreFile();

        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong: deferred restore from the previous run completed");

        Status("已自动恢复上次遗留的原模型");

    }else{

        // Re-persist the survivors so a second stop does not lose them.
        SaveDeferredRestore();

        Status("仍有 "+std::to_string(deferred_restore.size())+" 个角色的原模型待恢复，正在重试");

    }

}

bool Upload(std::uintptr_t component,Record& target,const Settings& cfg,std::uintptr_t material){

    target.bone_follow=cfg.bone_follow;



    const double scale=double(cfg.height)*100/(model.zmax-model.zmin);std::vector<nailong::M4> pose;std::string error;

    if(target.bone_follow&&!target.driver.Sample(source_rig,double(cfg.height)*100,pose,error)){ue::last_error=error;return false;}

    for(std::size_t i=0;i<model.sections.size();++i){

        target.prepared.push_back(nailong::PrepareSection(model.sections[i],scale,model.zmin,cfg.yaw));auto& section=target.prepared.back();

        if(target.bone_follow)nailong::DeformDualQuaternion(model.sections[i],source_rig,pose,scale,cfg.yaw,section);

        // This client has UE5 LWC enabled. The reflected parameter types are:

        // FVector = double[3] (24 bytes), FVector2D = double[2] (16 bytes),

        // FProcMeshTangent = FVector + flip flag (32-byte element stride).

        // Keep the native double layouts. Converting to float makes UE read each

        // TArray element with the wrong stride inside CreateMeshSection.

        alignas(16) std::array<std::uint8_t,105> p{};const auto index=std::uint32_t(i);std::memcpy(p.data(),&index,4);

        auto set_array=[&](std::size_t offset,void* data,std::size_t count){ue::Array a{data,std::int32_t(count),std::int32_t(count)};std::memcpy(p.data()+offset,&a,16);};

        set_array(8,section.positions.data(),section.positions.size());set_array(24,section.indices.data(),section.indices.size());

        set_array(40,section.normals.data(),section.normals.size());set_array(56,section.uv.data(),section.uv.size());set_array(72,section.bgra.data(),section.bgra.size());set_array(88,section.tangents.data(),section.tangents.size());

        p[104]=0; // bCreateCollision=false

        if(!ue::Call(component,"/Script/ProceduralMeshComponent.ProceduralMeshComponent.CreateMeshSection",p.data(),p.size())||!ue::SetMaterial(component,index,material))return false;

        std::int32_t section_count=0;

        if(!ue::NumSections(component,section_count)||section_count<=std::int32_t(index)){

            ue::last_error="CreateMeshSection returned without a live mesh section";

            return false;

        }

    }

    return true;

}

// Per-record skinning update. The player wrapper keeps 1.7.6 behaviour
// (including Restore() on a dead component); NPCs never touch player bookkeeping.
bool AnimateOne(Record& target,const Settings& cfg,double dt){

    (void)dt; // pacing is decided by the caller (player vs NPC rate)

    if(!target.component.ptr||ue::Alive(target.component)!=1){

        Status("奶龙组件已失效，准备恢复原角色并重试");

        return false;

    }

    // Live UpdateMeshSection enabled for bone following

    if(!target.bone_follow)return true;

    // The caller already decides whether this tick produces a pose for this record;
    // re-gating on cfg.pose_fps here silently reused the player's 30 Hz for NPCs
    // and made the NPC rate setting meaningless.
    target.animation_time=0;

    std::vector<nailong::M4> pose;std::string error;

    if(ue::Alive(target.component)!=1||!target.driver.Sample(source_rig,double(cfg.height)*100,pose,error)){Status("骨骼驱动中断，准备恢复原角色: "+error);return false;}

    if(!target.driver.diagnostic.empty()&&(skin_frames.load(std::memory_order_relaxed)%30)==0)

        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong pose: "+target.driver.diagnostic);

    const double scale=double(cfg.height)*100/(model.zmax-model.zmin);

    for(std::size_t i=0;i<target.prepared.size();++i){auto& section=target.prepared[i];nailong::DeformDualQuaternion(model.sections[i],source_rig,pose,scale,cfg.yaw,section);

        alignas(16) std::array<std::uint8_t,88> p{};const auto index=std::uint32_t(i);std::memcpy(p.data(),&index,4);

        auto set_array=[&](std::size_t offset,void* data,std::size_t count){ue::Array a{data,std::int32_t(count),std::int32_t(count)};std::memcpy(p.data()+offset,&a,16);};

        set_array(8,section.positions.data(),section.positions.size());set_array(24,section.normals.data(),section.normals.size());

        set_array(40,section.uv.data(),section.uv.size());set_array(56,section.bgra.data(),section.bgra.size());set_array(72,section.tangents.data(),section.tangents.size());

        if(!ue::Call(target.component.ptr,"/Script/ProceduralMeshComponent.ProceduralMeshComponent.UpdateMeshSection",p.data(),p.size())){Status("UE 蒙皮网格更新失败: "+ue::last_error);return false;}

    }

    skin_frames.fetch_add(1);return true;

}

bool Animate(const Settings& cfg,double dt){if(!AnimateOne(record,cfg,dt)){Restore();return false;}return true;}

bool ReadPlayerGeneration(std::uint64_t& generation){

    if(!player||!player->snapshot)return false;

    AnomalyNtePlayerSnapshotV1 snapshot{};snapshot.struct_size=sizeof(snapshot);

    if(!ue::nl_good(player->snapshot(player->user,&snapshot))||

       (snapshot.flags&ANOMALY_NTE_SNAPSHOT_V1_VALID)==0||

       !nailong::ValidHandle(snapshot.handle)||snapshot.handle.id!=1)return false;

    generation=snapshot.handle.generation;return true;

}

bool ReadCurrentActor(ue::Ref& current,AnomalyNteEntitySnapshotV1& selected,std::string& class_name){

    if(!actors||!actors->frame||!actors->page){Status("等待 NTE Actor 服务");return false;}

    AnomalyNteEntityFrameV1 frame{};frame.struct_size=sizeof(frame);

    if(!ue::nl_good(actors->frame(actors->user,&frame))){Status("等待玩家 Actor 快照建立");return false;}

    constexpr std::uint32_t kCapacity=4;std::array<AnomalyNteEntitySnapshotV1,kCapacity> page_data{};

    for(auto& item:page_data)item.struct_size=sizeof(item);

    AnomalyNteEntityPageRequestV1 request{};request.struct_size=sizeof(request);request.generation=frame.generation;

    request.offset=0;request.capacity=kCapacity;request.required_flags=ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER;request.excluded_flags=0;

    AnomalyNteEntityPageResultV1 result{};result.struct_size=sizeof(result);

    const auto page_status=actors->page(actors->user,&request,page_data.data(),&result);

    if(!ue::nl_good(page_status)||result.returned>kCapacity||result.total_matches>kCapacity||result.returned!=result.total_matches){

        actor_total.store(result.total_matches,std::memory_order_relaxed);actor_returned.store(result.returned,std::memory_order_relaxed);

        {std::lock_guard lock(actor_diag_mutex);actor_class_diag="当前玩家身份不唯一或 Actor 快照无效";}return false;

    }

    actor_total.store(result.total_matches,std::memory_order_relaxed);actor_returned.store(result.returned,std::memory_order_relaxed);

    const auto index=nailong::SelectLocalActor(page_data.data(),result.returned);

    if(index==UINT32_MAX){

        {std::lock_guard lock(actor_diag_mutex);actor_class_diag=result.total_matches?"当前玩家身份不唯一或快照无效":"等待当前玩家身份";}return false;

    }

    selected=page_data[index];

    if(!selected.entity_id||selected.entity_id>UINT32_MAX){Status("当前玩家实体身份无效");return false;}

    std::uintptr_t ptr=0;std::uint32_t serial=0;

    if(!ue::nl_object_slot(std::uint32_t(selected.entity_id)-1,&ptr,&serial)||!ptr||!serial){Status("当前玩家 UObject 尚未就绪");return false;}

    current={ptr,std::uint32_t(selected.entity_id)-1,serial};

    char name[256]{};std::size_t n=sizeof(name);

    if(actors->class_name_utf8&&ue::nl_good(actors->class_name_utf8(actors->user,selected.class_id,name,&n)))class_name.assign(name,n);else class_name.clear();

    {std::lock_guard lock(actor_diag_mutex);actor_class_diag=class_name;}

    return true;

}

bool EnableSourcePoseTick(Record& target){

    const auto offset=ue::AnimationTickOffset();

    if(!target.driver.mesh.ptr||offset<0){ue::last_error="源骨骼动画 Tick 属性不可用";return false;}

    for(auto& h:target.hidden){

        if(h.object.ptr!=target.driver.mesh.ptr)continue;

        h.tick_offset=offset;std::uint8_t before=0;

        if(!ue::ReadByte(h.object.ptr+std::uintptr_t(offset),before)){ue::last_error="读取源骨骼动画 Tick 失败";return false;}

        h.tick_original=before;

        if(before==0)return true;

        const auto address=h.object.ptr+std::uintptr_t(offset);

        if(!ue::WriteByte(address,0)){ue::last_error="开启源骨骼动画 Tick 失败";return false;}

        h.tick_changed=true;std::uint8_t after=0;

        if(!ue::ReadByte(address,after)||after!=0){ue::last_error="源骨骼动画 Tick 写入校验失败";return false;}

        return true;

    }

    ue::last_error="已绑定的源骨骼组件不在原模型恢复记录中";

    return false;

}

bool SetSourcePoseOffscreen(Record& target,bool offscreen){
    const auto offset=ue::AnimationTickOffset();
    if(!target.driver.mesh.ptr||offset<0)return false;
    for(auto& h:target.hidden){
        if(h.object.ptr!=target.driver.mesh.ptr||h.tick_offset!=offset)continue;
        const auto address=h.object.ptr+std::uintptr_t(offset);
        const std::uint8_t desired=offscreen?4:0; // OnlyTickPoseWhenRendered / full pose
        std::uint8_t current=0;
        if(!ue::ReadByte(address,current))return false;
        if(current!=desired&&!ue::WriteByte(address,desired))return false;
        // Even when the original value was already zero, RestoreOne must put it
        // back after our temporary off-screen mode.
        if(offscreen)h.tick_changed=true;
        return true;
    }
    return false;
}

// ---- character identification ------------------------------------------------
// Derived from the Dumper-7 dump at C:\Dumper-7\5.6.1-0+UE5-HT1.4b. Hierarchy:
//   Character -> ModularCharacter -> HottaCharacter -> HTCharacter
//             -> HTAbilityCharacter -> HTAICharacter
//             -> { HTMonsterCharacter, HTNPCCharacter, HTPlayerNPCCharacter }
// HTCharacterActor is a SEPARATE branch: it derives straight from AActor and owns
// its SkeletalMeshComponents as object properties (Mesh 0x02C8, ForkMesh 0x02D0),
// which is why K2_GetComponentsByClass reports nothing for it.
std::uintptr_t CharacterBaseClass(){
    static std::uintptr_t cached=0;
    if(!cached)cached=ue::Find("/Script/ModularGameplayActors.ModularCharacter");
    return cached;
}

std::uintptr_t HtCharacterClass(){
    static std::uintptr_t cached=0;
    if(!cached)cached=ue::Find("/Script/HTGame.HTCharacter");
    return cached;
}

std::uintptr_t HtCharacterActorClass(){
    static std::uintptr_t cached=0;
    if(!cached)cached=ue::Find("/Script/HTGame.HTCharacterActor");
    return cached;
}

std::uintptr_t PlayerCharacterClass(){
    static std::uintptr_t cached=0;
    if(!cached)cached=ue::Find("/Script/HTGame.HTPlayerCharacter");
    return cached;
}

bool IsPlayerCharacterActor(std::uintptr_t actor){
    const auto player_class=PlayerCharacterClass();
    return player_class!=0&&ue::IsClass(actor,player_class);
}

std::int32_t ActorComponentOffset(const char* name){
    static std::int32_t mesh=-2,fork=-2;
    std::int32_t& cached=std::strcmp(name,"Mesh")==0?mesh:fork;
    if(cached==-2)cached=ue::PropertyOffset("/Script/HTGame.HTCharacterActor",name,8);
    return cached;
}

bool ActorOwnedMeshes(std::uintptr_t actor,std::vector<ue::Ref>& out){
    const auto cls=HtCharacterActorClass();
    if(!cls||!ue::IsClass(actor,cls))return false;
    for(const char* name:{"Mesh","ForkMesh"}){
        const auto offset=ActorComponentOffset(name);
        if(offset<0)continue;
        std::uintptr_t component=0;
        if(!ue::nl_read_ptr(actor+std::uintptr_t(offset),&component)||!component)continue;
        ue::Ref r;
        if(ue::Capture(component,r))out.push_back(r);
    }
    return true;
}

bool HasActorOwnedMesh(std::uintptr_t actor){
    const auto cls=HtCharacterActorClass();
    if(!cls||!ue::IsClass(actor,cls))return false;
    const auto offset=ActorComponentOffset("Mesh");
    if(offset<0)return false;
    std::uintptr_t component=0;
    return ue::nl_read_ptr(actor+std::uintptr_t(offset),&component)&&component!=0;
}

std::string ClassNameOf(std::uint64_t class_id){
    if(!actors||!actors->class_name_utf8)return {};
    char name[256]{};std::size_t n=sizeof(name);
    return ue::nl_good(actors->class_name_utf8(actors->user,class_id,name,&n))?std::string(name,n):std::string();
}

// Admission, in decreasing order of proof:
//   1. ancestry on the playable character chain (Character -> ... -> HTCharacter)
//   2. a live owned mesh on the AActor-derived branch (HTCharacterActor)
//   3. an actual skeletal mesh component on the actor
//   4. the name gate, only when neither class object resolved in this build
//
// Rule 3 matters because the world contains NPC variants whose class sits outside
// the HTCharacter chain (AHTSimpleSceneActor props, and NPC_Sub assemblies that
// wrap a budgeted skeletal mesh). Class names alone both over- and under-admit:
// NPC_Sub_* looks like an NPC but is scenery, while a hand-placed animated NPC can
// have no recognisable name at all. A live USkeletalMeshComponent is the one
// signal that actually means "this can be skinned".
// Reflection offsets on AHTSimpleSceneActor, from the Dumper-7 dump
// (C:\Dumper-7\5.6.1-0+UE5-HT1.4b). Resolved once via PropertyOffset so a build
// with reshuffled fields still works.
std::uintptr_t SimpleSceneActorClass(){
    static std::uintptr_t cached=0;
    if(!cached)cached=ue::Find("/Script/HTGame.HTSimpleSceneActor");
    return cached;
}

std::int32_t SimpleSceneOffset(const char* name){
    static std::int32_t slot[4]={-2,-2,-2,-2};
    const int idx=std::strcmp(name,"SkeletalMeshComponent")==0?0:
                  std::strcmp(name,"AdditionalHeadMeshComponents")==0?1:
                  std::strcmp(name,"AdditionalTopMeshComponents")==0?2:3;
    if(slot[idx]==-2)slot[idx]=ue::PropertyOffset("/Script/HTGame.HTSimpleSceneActor",name,8);
    return slot[idx];
}

// Collects the budgeted mesh components this actor owns directly.
bool SimpleSceneMeshes(std::uintptr_t actor,std::vector<ue::Ref>& out){
    const auto cls=SimpleSceneActorClass();
    if(!cls||!ue::IsClass(actor,cls))return false;
    for(const char* name:{"SkeletalMeshComponent","AdditionalHeadMeshComponents",
                          "AdditionalTopMeshComponents","AdditionalBottomMeshComponents"}){
        const auto offset=SimpleSceneOffset(name);
        if(offset<0)continue;
        std::uintptr_t component=0;
        if(!ue::nl_read_ptr(actor+std::uintptr_t(offset),&component)||!component)continue;
        ue::Ref r;
        if(ue::Capture(component,r))out.push_back(r);
    }
    return true;
}

bool IsSkeletalMeshOnlyActor(std::uintptr_t actor){
    static std::uintptr_t cached=0;
    if(!cached)cached=ue::Find("/Script/Engine.SkeletalMeshActor");
    return cached!=0&&ue::IsClass(actor,cached);
}

bool IsAnimalClassName(std::string name){
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    return name.find("animal")!=std::string::npos;
}

std::string ActorClassName(std::uintptr_t actor){
    std::uintptr_t cls=0;std::uint32_t name_id=0;
    if(!ue::nl_read_ptr(actor+16,&cls)||!ue::nl_read_u32(cls+24,&name_id)||!ue::g_names)return {};
    char name[256]{};std::size_t size=sizeof(name);
    if(!ue::nl_good(ue::g_names->resolve_utf8(ue::g_names->user,name_id,name,&size)))return {};
    return std::string(name,size);
}

bool IsAnimalActor(std::uintptr_t actor){return IsAnimalClassName(ActorClassName(actor));}

bool AdmitActor(std::uintptr_t ptr,const std::string& item_class,std::string& first_rejected){
    if(IsAnimalClassName(item_class)){
        if(first_rejected.empty())first_rejected=item_class;
        return false;
    }
    // Every playable character derives from HTPlayerCharacter, not only the
    // currently controlled pawn. Party/display characters can overlap the local
    // pawn; treating them as NPCs adds the second NaiLong body reported by the
    // user. Exclude the entire playable branch before any mesh-based admission.
    if(IsPlayerCharacterActor(ptr)){
        if(first_rejected.empty())first_rejected=item_class;
        return false;
    }
    // ASkeletalMeshActor (including HTSkeletalMeshActor) is a generic mesh
    // presenter, not an NPC. It can be attached to a character and overlap it.
    // Mesh ownership alone is therefore not a valid NPC admission signal.
    if(IsSkeletalMeshOnlyActor(ptr)){
        if(first_rejected.empty())first_rejected=item_class;
        return false;
    }
    const auto base=CharacterBaseClass();
    if(base!=0&&ue::IsClass(ptr,base))return true;
    const auto ht=HtCharacterClass();
    if(ht!=0&&ue::IsClass(ptr,ht))return true;
    if(HasActorOwnedMesh(ptr))return true;
    {
        std::vector<ue::Ref> scene_meshes;
        if(SimpleSceneMeshes(ptr,scene_meshes)&&!scene_meshes.empty())return true;
    }
    const bool name_fallback=nailong::IsCharacterClassName(item_class)&&base==0&&ht==0&&
        !nailong::IsActorOwnedMeshBranch(item_class);
    if(name_fallback)return true;
    if(first_rejected.empty())first_rejected=item_class;
    return false;
}

// The actor page table is the bottleneck: one page call is a reflected Host
// round-trip, and a city block holds thousands of actors. Scanning it every
// second stalled the game thread. Instead the sweep is incremental (a few pages
// per tick, resuming where it stopped) and every actor's classification is cached
// by (entity_id, serial), so the costly UObject/class/mesh probing happens once
// per actor rather than once per scan.
struct CandidateHit {ue::Ref ref;double distance_m;};

void nps_scan_note_seen(std::uint32_t seen,std::uint32_t total,std::uint32_t pages,const std::string& rejected){
    npc_scan_seen.store(seen,std::memory_order_relaxed);
    npc_scan_total.store(total,std::memory_order_relaxed);
    npc_scan_pages.store(pages,std::memory_order_relaxed);
    if(!rejected.empty()){std::lock_guard lock(npc_diag_mutex);npc_scan_rejected=rejected;}
}

// Classify one actor, consulting the cache first. Returns true when this actor is
// a character that can carry a replacement.
//
// The cache key is the entity id, but a slot can be recycled: the same entity id
// later names a different UObject. The stored ptr+serial pair is the real
// identity, so a hit is only trusted when both still match the live slot. A
// previous version compared serial against entity_id, which can never be equal --
// the cache never hit and every sweep repaid the full probe cost.
bool ClassifyActor(const AnomalyNteEntitySnapshotV1& item,std::string& first_rejected){
    const auto key=item.entity_id;
    const std::uint32_t index=std::uint32_t(item.entity_id)-1;

    std::uintptr_t ptr=0;std::uint32_t serial=0;
    if(!ue::nl_object_slot(index,&ptr,&serial)||!ptr||!serial)return false;

    auto it=actor_cache.find(key);
    if(it!=actor_cache.end()&&it->second.ptr==ptr&&it->second.serial==serial){
        if(it->second.kind==1)return true;
        if(it->second.kind==2)return false;
    }

    ActorProbe probe;probe.entity_id=item.entity_id;probe.serial=serial;
    probe.ptr=ptr;probe.kind=2;probe.has_mesh=0;

    const auto item_class=ClassNameOf(item.class_id);
    if(AdmitActor(ptr,item_class,first_rejected)){
        probe.kind=1;
        const auto root=ue::Root(ptr);
        if(root){
            std::vector<ue::Ref> meshes;
            probe.has_mesh=(ue::Meshes(ptr,root,meshes)&&!meshes.empty())?1:0;
        }
    }
    actor_cache[key]=probe;
    return probe.kind==1;
}

// Incremental sweep: reads up to kPagesPerTick pages starting at the persisted
// cursor, records what is in range, and advances. When the table has been walked
// completely the cursor wraps and the next full sweep is scheduled.
bool ScanActorPages(double radius,const AnomalyNtePlayerSnapshotV1& player_snapshot,
                    std::uint64_t player_entity_id,std::vector<CandidateHit>& hits,
                    std::uint32_t& seen,std::uint32_t& total,std::uint32_t& pages_this_tick,
                    std::uint64_t class_filter=0){
    hits.clear();seen=0;total=0;pages_this_tick=0;
    if(!actors||!actors->frame||!actors->page)return false;
    AnomalyNteEntityFrameV1 frame{};frame.struct_size=sizeof(frame);
    if(!ue::nl_good(actors->frame(actors->user,&frame)))return false;

    std::uint32_t offset=npc_scan_cursor;
    std::string first_rejected;
    std::vector<std::string> admitted_names,rejected_names;
    bool finished=false;
    for(std::uint32_t page=0;page<kPagesPerTick;++page){
        std::array<AnomalyNteEntitySnapshotV1,kActorsPerPage> page_data{};
        for(auto& item:page_data)item.struct_size=sizeof(item);
        AnomalyNteEntityPageRequestV1 request{};request.struct_size=sizeof(request);
        request.generation=frame.generation;request.offset=offset;request.capacity=kActorsPerPage;
        // SimpleSceneNPC actors can be STATIC too. Excluding that flag here made
        // those NPCs invisible to discovery even though their class and mesh were
        // supported. Keep the pages incremental and reject scenery by class below.
        request.excluded_flags=ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER;
        request.class_id=class_filter;
        AnomalyNteEntityPageResultV1 result{};result.struct_size=sizeof(result);
        if(!ue::nl_good(actors->page(actors->user,&request,page_data.data(),&result))){
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,"NaiLong NPC scan: actor page failed");
            return pages_this_tick>0;
        }
        ++pages_this_tick;total=result.total_matches;seen+=result.returned;

        for(std::uint32_t i=0;i<result.returned;++i){
            const auto& item=page_data[i];
            if(!nailong::IsNpcInRadius(item,player_entity_id,
                    std::span<const double,3>(player_snapshot.position),radius))continue;
            const auto item_class_name=ClassNameOf(item.class_id);
            if(!ClassifyActor(item,first_rejected)){
                if(rejected_names.size()<10&&std::find(rejected_names.begin(),rejected_names.end(),item_class_name)==rejected_names.end()){
                    rejected_names.push_back(item_class_name);
                }
                continue;
            }
            if(admitted_names.size()<16&&std::find(admitted_names.begin(),admitted_names.end(),item_class_name)==admitted_names.end()){
                admitted_names.push_back(item_class_name);
            }
            std::uint32_t index=std::uint32_t(item.entity_id)-1;
            std::uintptr_t ptr=0;std::uint32_t serial=0;
            if(!ue::nl_object_slot(index,&ptr,&serial)||!ptr||!serial)continue;
            const double dx=item.bounds_center[0]-player_snapshot.position[0];
            const double dy=item.bounds_center[1]-player_snapshot.position[1];
            const double dz=item.bounds_center[2]-player_snapshot.position[2];
            hits.push_back({{ptr,index,serial},std::hypot(dx,dy,dz)*0.01});
        }

        if(result.returned==0||result.next_offset<=offset||result.next_offset>=result.total_matches){
            finished=true;break;
        }
        offset=result.next_offset;
    }

    npc_scan_cursor=finished?0:offset;
    npc_scan_wrapped=finished;
    if(finished)npc_scan_cursor_time=0.0;

    {
        std::string admitted_all,rejected_all;
        for(const auto& n:admitted_names)admitted_all+=" "+n;
        for(const auto& n:rejected_names)rejected_all+=" "+n;
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong NPC names: admitted="+(admitted_all.empty()?std::string("(none)"):admitted_all)+
            " | rejected="+(rejected_all.empty()?std::string("(none)"):rejected_all));
    }

    nps_scan_note_seen(seen,total,pages_this_tick,first_rejected);
    return true;
}

bool ReplaceInto(Record& target,const ue::Ref& actor,const Settings& cfg,std::uint64_t player_generation,bool player_generation_valid,bool allow_static_fallback=false){

    // Refuse to dress an actor twice.
    if(AlreadyDressed(actor.ptr)){

        Status("该角色已替换，跳过重复装配");
        return false;

    }

    // A recently restored actor must not be re-bound yet: the old component is
    // still pending kill, and attaching a new one now produces two bodies.
    {
        const auto cd=rebind_cooldown.find(actor.ptr);
        if(cd!=rebind_cooldown.end()){
            const double now=now_seconds();
            if(now<cd->second){
                Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong cooldown: blocked actor="+std::to_string(actor.ptr)+
                    " now="+std::to_string(now)+" until="+std::to_string(cd->second));
                return false;
            }else{
                Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong cooldown: expired actor="+std::to_string(actor.ptr)+
                    " now="+std::to_string(now)+" until="+std::to_string(cd->second));
            }
        }
    }

    // Direct census of components already on this actor. This is the ground truth
    // for the "two NaiLong bodies" report: if the actor already carries PMCs that
    // are NOT in our ownership map, they came from a previous session's binding
    // and must be destroyed before we add another one.
    {
        const auto pmc_class=ue::Find("/Script/ProceduralMeshComponent.ProceduralMeshComponent");
        if(pmc_class){
            std::vector<ue::Ref> existing;
            if(ue::ComponentsByClass(actor.ptr,pmc_class,existing)&&!existing.empty()){
                Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    "NaiLong: actor already has "+std::to_string(existing.size())+
                    " PMC(s) before binding; destroying them");
                for(auto& c:existing){
                    const int alive=ue::Alive(c);
                    if(alive!=1)continue;
                    const bool retired=std::any_of(retired_components.begin(),retired_components.end(),
                        [&](const ue::Ref& r){return r.ptr==c.ptr;});
                    if(retired){
                        // It was already destroyed; it is still in the actor array.
                        // Re-hiding prevents it rendering, but it is not evidence of
                        // a second live body and must not cancel the new binding.
                        ue::SetVisible(c.ptr,false);
                        continue;
                    }
                    ue::SetVisible(c.ptr,false);
                    ue::ClearAllMeshSections(c.ptr);
                    ue::Deactivate(c.ptr);
                    ue::DetachFromParent(c.ptr);
                    if(!ue::Destroy(c.ptr)){
                        Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                            "NaiLong: failed to destroy stray PMC; refusing to bind");
                        return false;
                    }
                    retired_components.push_back(c);
                }
            }
        }
    }

    auto root=ue::Root(actor.ptr), cls=ue::Find("/Script/ProceduralMeshComponent.ProceduralMeshComponent");

    auto mat=ue::VertexMaterial();

    if(!root||!cls){Status("UE 原生根组件或 ProceduralMeshComponent 未就绪");return false;}

    // Do not substitute the player's material: it cannot reproduce the source model colors.

    if(!mat){Status("VertexColorMaterial 未加载，保留原角色；不降级为叠加层或基础体");return false;}

    std::vector<ue::Ref> originals;

    if(SimpleSceneMeshes(actor.ptr,originals)){
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong: SimpleSceneActor branch; owned meshes="+std::to_string(originals.size()));
    }

    if(ActorOwnedMeshes(actor.ptr,originals)){

        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong: HTCharacterActor branch; owned meshes="+std::to_string(originals.size()));

    }

    std::vector<ue::Ref> enumerated;

    if(ue::Meshes(actor.ptr,root,enumerated)){

        for(auto& r:enumerated){

            const bool dup=std::any_of(originals.begin(),originals.end(),[&](const ue::Ref& e){return e.ptr==r.ptr;});

            if(!dup)originals.push_back(r);

        }

    }

    if(originals.empty()&&!allow_static_fallback){Status("原角色网格枚举失败；没有隐藏任何原组件");return false;}

    Record next;next.actor=actor;next.revision=cfg.revision;next.player_generation=player_generation;

    next.player_generation_valid=player_generation_valid;next.bone_follow=false;

    for(auto r:originals){bool v=false;if(!ue::Visible(r.ptr,v)){Status(ue::last_error);return false;}next.hidden.push_back({r,v});}

    if(cfg.bone_follow){

        const auto sk=ue::Find("/Script/Engine.SkeletalMeshComponent");bool bound=false;std::string reason;

        for(auto r:originals)if(ue::IsClass(r.ptr,sk)){nailong::PoseDriver candidate;

            if(candidate.Init(r.ptr,root,source_rig,reason)){next.driver=std::move(candidate);bound=true;break;}}

        if(!bound){
            if(!allow_static_fallback){Status("没有可靠的源骨架驱动；保留原角色: "+reason);return false;}
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,"NaiLong NPC: incompatible source rig; using static replacement: "+reason);
        }

    }

    // Gate on the driver this assembly actually built. Reading `target` here would
    // carry a stale driver from a previous binding and silence NPC skinning, whose
    // target is a fresh local Record.
    Settings upload_cfg=cfg;upload_cfg.bone_follow=cfg.bone_follow&&next.driver.mesh.ptr!=0;

    double half=ue::CapsuleHalfHeight(root);

    if(half<=0||!std::isfinite(half)){
        if(!allow_static_fallback){Status("胶囊半高不可用，取消装配以免模型漂移");return false;}
        half=90;
    }

    auto component=ue::Add(actor.ptr,cls,-half+cfg.offset);

    if(!component){Status(ue::last_error);return false;}

    if(!ue::Capture(component,next.component)){ue::Destroy(component);Status("新组件身份验证失败");return false;}

    // Keep the mesh invisible until attachment, upload and all original mesh
    // changes have completed. This also prevents a partially-created component
    // from showing for one frame during a restore/rebuild.
    if(!ue::SetVisible(component,false)){
        ue::Destroy(component);Status("新组件初始隐藏失败");return false;
    }

    RememberDressed(actor.ptr,component);

    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong bind: actor="+std::to_string(actor.ptr)+
        " component="+std::to_string(component)+" index="+std::to_string(actor.index));

    target=std::move(next); // Keep ownership even if a later operation fails.

    if(!ue::NoCollision(component)||!ue::Attach(component,root)||!Upload(component,target,upload_cfg,mat)){

        const auto reason=ue::last_error;RestoreOne(target);Status("装配失败，已尝试恢复："+reason);return false;

    }

    // Keep hidden source skeletal meshes evaluating pose. The reflected property offset

    // is resolved from the live UE class, not hard-coded; Restore() puts the original

    // byte back before destroying our component.

    if(upload_cfg.bone_follow&&!EnableSourcePoseTick(target)){

        const auto reason=ue::last_error;RestoreOne(target);Status("源骨骼动画刷新准备失败，已尝试恢复："+reason);return false;

    }

    // Hide only original visual meshes AFTER the new native mesh and pose ticking are ready.

    for(auto& h:target.hidden){

        if(!h.was_visible)continue;

        bool visible=true;

        if(!ue::SetVisible(h.object.ptr,false)||!ue::Visible(h.object.ptr,visible)||visible){

            const auto reason=ue::last_error;RestoreOne(target);Status("隐藏原模型失败，已尝试恢复："+reason);return false;

        }

    }

    if(!ue::SetVisible(component,true)){
        const auto reason=ue::last_error;RestoreOne(target);Status("奶龙组件显示失败，已尝试恢复："+reason);return false;
    }

    Status(upload_cfg.bone_follow?"已替换：源骨架蒙皮跟随原角色；可读取 BetterPose/MMD 姿势":"已替换：使用静态整模");

    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong UE: actor replaced; original meshes="+std::to_string(target.hidden.size())+
        "; native PMC="+std::to_string(component)+"; map="+target.driver.mapping_summary+"; ref="+target.driver.reference_source);return true;

}

bool Replace(const ue::Ref& actor,const Settings& cfg,std::uint64_t player_generation,bool player_generation_valid){const bool ok=ReplaceInto(record,actor,cfg,player_generation,player_generation_valid);SyncActive();mapped_bones.store(record.actor.ptr?std::uint32_t(record.driver.mapped):0);return ok;}

// One incompatible NPC must not be re-assembled (and re-logged) every scan.
struct NpcFailure {std::uintptr_t ptr;std::uint32_t serial;double cooldown;};
std::vector<NpcFailure> npc_failures;

bool NpcBlocked(const ue::Ref& r){
    return std::any_of(npc_failures.begin(),npc_failures.end(),[&](const NpcFailure& f){
        return f.ptr==r.ptr&&f.serial==r.serial&&f.cooldown>0;});
}

void NpcCoolDown(double dt){
    for(auto& f:npc_failures)f.cooldown-=dt;
    npc_failures.erase(std::remove_if(npc_failures.begin(),npc_failures.end(),
        [](const NpcFailure& f){return f.cooldown<=0;}),npc_failures.end());
}

void NpcRememberFailure(const ue::Ref& r){
    if(npc_failures.size()>=64)npc_failures.erase(npc_failures.begin());
    npc_failures.push_back({r.ptr,r.serial,15.0});
}

// Live distance from the player to a bound actor, in metres. Read from the
// actor's root component rather than from a page entry, so it stays valid
// between incremental sweeps.
bool ActorDistanceM(const ue::Ref& actor,const AnomalyNtePlayerSnapshotV1& player_snapshot,double& out_m){
    if(ue::Alive(actor)!=1)return false;
    const auto root=ue::Root(actor.ptr);
    if(!root)return false;
    double location[3]{};
    if(!ue::ComponentLocation(root,location))return false;
    const double dx=location[0]-player_snapshot.position[0];
    const double dy=location[1]-player_snapshot.position[1];
    const double dz=location[2]-player_snapshot.position[2];
    const double d=std::hypot(dx,dy,dz)*0.01;
    if(!std::isfinite(d))return false;
    out_m=d;return true;
}

// Fallback for NPCs that have a live UObject but no Host entity-page entry.
// The registry is large, so walk only a bounded slice per discovery pass.
bool EnumerateCharacterActors(double radius,const AnomalyNtePlayerSnapshotV1& player_snapshot,
                              std::vector<CandidateHit>& hits,std::uint32_t& scanned,std::uint32_t& matched){
    hits.clear();scanned=0;matched=0;
    if(!ue::g_reg.ready&&!ue::nl_refresh_objects())return false;

    const auto base=CharacterBaseClass();
    const auto ht=HtCharacterClass();
    const auto simple=SimpleSceneActorClass();
    const auto actor_branch=HtCharacterActorClass();
    if(base==0&&ht==0&&actor_branch==0&&simple==0)return false;

    constexpr std::uint32_t kObjectSlotsPerPass=4096;
    const std::uint32_t total=ue::g_reg.count;
    if(npc_object_scan_cursor>=total)npc_object_scan_cursor=0;
    const std::uint32_t begin=npc_object_scan_cursor;
    const std::uint32_t end=std::min(total,begin+kObjectSlotsPerPass);
    for(std::uint32_t index=begin;index<end;++index){
        std::uintptr_t obj=0;std::uint32_t serial=0;
        if(!ue::nl_object_slot(index,&obj,&serial)||!obj||!serial)continue;
        ++scanned;

        std::uint32_t flags=0;
        if(!ue::nl_read_u32(obj+8,&flags)||(flags&0x18000))continue;

        const bool is_character =
            (base!=0&&ue::IsClass(obj,base))||
            (ht!=0&&ue::IsClass(obj,ht))||
            (actor_branch!=0&&ue::IsClass(obj,actor_branch))||
            (simple!=0&&ue::IsClass(obj,simple));
        if(!is_character||IsPlayerCharacterActor(obj)||IsAnimalActor(obj))continue;

        const auto root=ue::Root(obj);
        if(!root)continue;
        double location[3]{};
        if(!ue::ComponentLocation(root,location))continue;
        const double dx=location[0]-player_snapshot.position[0];
        const double dy=location[1]-player_snapshot.position[1];
        const double dz=location[2]-player_snapshot.position[2];
        const double d=std::hypot(dx,dy,dz)*0.01;
        if(!std::isfinite(d)||d>radius)continue;

        ++matched;
        hits.push_back({{obj,index,serial},d});
    }
    npc_object_scan_cursor=end>=total?0:end;
    return true;
}

void UpdateNearbyNpcs(const Settings& cfg,double dt){
    if(!cfg.nearby_npcs){
        if(!npc_records.empty())RestoreNpcs();
        npc_observed_revision=cfg.revision;npc_scan_time=0;npc_scan_cursor=0;npc_object_scan_cursor=0;
        npc_failures.clear();actor_cache.clear();return;
    }
    if(cfg.revision!=npc_observed_revision){
        RestoreNpcs();npc_observed_revision=cfg.revision;npc_scan_time=0;npc_scan_cursor=0;npc_object_scan_cursor=0;
        npc_failures.clear();return;
    }

    NpcCoolDown(dt);

    // DQS deformation and UpdateMeshSection dominate NPC cost. Stagger each NPC
    // over the interval instead of updating the whole crowd on the same frame,
    // and skip geometry work entirely while the component is off screen.
    const double npc_interval=cfg.npc_pose_fps>0?1.0/double(cfg.npc_pose_fps):1.0/15.0;
    const double now=now_seconds();
    std::size_t schedule_index=0,skin_updates=0;
    constexpr std::size_t kMaxSkinUpdatesPerFrame=4;
    for(auto& item:npc_records){
        if(!item.bone_follow)continue;
        if(item.next_pose_time<=0){
            item.next_pose_time=now+npc_interval*double(schedule_index%16)/16.0;
        }
        ++schedule_index;
        if(now<item.next_pose_time)continue;

        bool rendered=true;
        if(item.component.ptr&&ue::WasRecentlyRendered(item.component.ptr,0.25f,rendered)&&!rendered){
            // Off-screen NPCs need neither DQS nor a fully refreshed hidden source
            // skeleton. Check visibility again twice a second.
            if(!item.source_pose_offscreen&&SetSourcePoseOffscreen(item,true))item.source_pose_offscreen=true;
            item.next_pose_time=now+0.5;
            continue;
        }
        if(item.source_pose_offscreen&&SetSourcePoseOffscreen(item,false))item.source_pose_offscreen=false;

        if(skin_updates>=kMaxSkinUpdatesPerFrame){
            item.next_pose_time=now+0.005; // deferred to the next frame, not dropped
            continue;
        }
        ++skin_updates;
        const double distance_lod=item.distance_m>35.0?4.0:(item.distance_m>18.0?2.0:1.0);
        item.next_pose_time=now+npc_interval*distance_lod;
        if(!AnimateOne(item,cfg,dt))RestoreOne(item);
    }
    npc_records.erase(std::remove_if(npc_records.begin(),npc_records.end(),
        [](const Record& r){return r.actor.ptr==0;}),npc_records.end());

    // Binding pass. The Host actor page is the only view that actually contains
    // the world's actors: walking the raw object registry found 85k UObjects but
    // only 6 with character ancestry, because world actors are reachable through
    // the level, not through a flat registry scan. The page is therefore the
    // discovery source, with the STATIC flag excluded host-side so the walk is
    // small, and every actor's classification cached so probing happens once.
    npc_scan_time-=dt;
    if(npc_scan_time<=0||npc_bind_now_requested){
        npc_scan_time=1.0;
        npc_bind_now_requested=false;

        if(!actors||!player||!player->snapshot)return;
        AnomalyNtePlayerSnapshotV1 player_snapshot{};player_snapshot.struct_size=sizeof(player_snapshot);
        if(!ue::nl_good(player->snapshot(player->user,&player_snapshot))||
           (player_snapshot.flags&ANOMALY_NTE_SNAPSHOT_V1_VALID)==0)return;
        ue::Ref player_actor;AnomalyNteEntitySnapshotV1 player_entity{};std::string class_name;
        if(!ReadCurrentActor(player_actor,player_entity,class_name))return;
        const std::uint32_t player_entity_id_minus_one=player_actor.index;

        std::vector<CandidateHit> hits;
        std::uint32_t seen=0,total=0,pages=0;
        // The one-click button schedules this same incremental pass. Draining the
        // whole actor table in one frame caused a large opening hitch even though
        // binding itself was already capped.
        ScanActorPages(cfg.npc_radius,player_snapshot,player_entity.entity_id,hits,seen,total,pages);

        std::vector<CandidateHit> object_hits;
        std::uint32_t object_scanned=0,object_matched=0;
        if(EnumerateCharacterActors(cfg.npc_radius,player_snapshot,object_hits,object_scanned,object_matched)){
            hits.insert(hits.end(),object_hits.begin(),object_hits.end());
            Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "NaiLong NPC objects: scanned="+std::to_string(object_scanned)+
                " matched="+std::to_string(object_matched)+" registry="+std::to_string(ue::g_reg.count));
        }

        // Eviction from live distance: a partial page slice is not a full view,
        // so it must never decide that an NPC left.
        const double evict_radius=cfg.npc_radius+5.0;
        for(auto& item:npc_records){
            double distance=0;
            if(!ActorDistanceM(item.actor,player_snapshot,distance)){RestoreOne(item);continue;}
            item.distance_m=distance;
            if(distance>evict_radius)RestoreOne(item);
        }
        npc_records.erase(std::remove_if(npc_records.begin(),npc_records.end(),
            [](const Record& r){return r.actor.ptr==0;}),npc_records.end());

        // Purge any record that points at the controlled player. Such a record
        // would be a second NaiLong on the same actor, and RestoreOne() would then
        // destroy only one of the two components.
        const std::size_t before_purge=npc_records.size();
        for(auto& item:npc_records){
            const bool is_player=(record.actor.ptr!=0&&item.actor.ptr==record.actor.ptr)||
                                 (player_actor.ptr!=0&&item.actor.ptr==player_actor.ptr)||
                                 IsPlayerCharacterActor(item.actor.ptr)||
                                 IsSkeletalMeshOnlyActor(item.actor.ptr)||
                                 IsAnimalActor(item.actor.ptr);
            if(is_player)RestoreOne(item);
        }
        npc_records.erase(std::remove_if(npc_records.begin(),npc_records.end(),
            [](const Record& r){return r.actor.ptr==0;}),npc_records.end());
        const std::size_t purged=before_purge-npc_records.size();

        const std::size_t limit=std::size_t(cfg.npc_limit<0?0:cfg.npc_limit);
        std::sort(hits.begin(),hits.end(),[](const CandidateHit& a,const CandidateHit& b){return a.distance_m<b.distance_m;});
        hits.erase(std::unique(hits.begin(),hits.end(),
            [](const CandidateHit& a,const CandidateHit& b){return a.ref.ptr==b.ref.ptr;}),hits.end());

        std::size_t bound_this_pass=0, skipped_player=0;
        constexpr std::size_t kMaxBindsPerPass=4;
        for(const auto& h:hits){
            if(npc_records.size()>=limit||bound_this_pass>=kMaxBindsPerPass)break;
            // The controlled player is already dressed by Replace(); binding it a
            // second time from the NPC pass added a second ProceduralMeshComponent
            // to the same actor, which is why two NaiLong bodies appeared on the
            // player -- one static, one skinned. Never touch the player actor here.
            if(IsPlayerCharacterActor(h.ref.ptr)){++skipped_player;continue;}
            if(IsSkeletalMeshOnlyActor(h.ref.ptr))continue;
            if(IsAnimalActor(h.ref.ptr))continue;
            if(record.actor.ptr!=0&&h.ref.ptr==record.actor.ptr){++skipped_player;continue;}
            if(player_actor.ptr!=0&&h.ref.ptr==player_actor.ptr)continue;
            // The player can also be reached through the entity id carried by the
            // page, which may differ from both pointers above if the actor was
            // re-spawned. Compare that too before binding anything.
            if(h.ref.index==player_entity_id_minus_one){++skipped_player;continue;}
            if(record.actor.ptr!=0&&h.ref.index==record.actor.index){++skipped_player;continue;}
            const bool exists=std::any_of(npc_records.begin(),npc_records.end(),[&](const Record& r){
                return r.actor.ptr==h.ref.ptr;});
            if(exists)continue;
            if(NpcBlocked(h.ref))continue;
            Record next;next.distance_m=h.distance_m;
            if(ReplaceInto(next,h.ref,cfg,0,false,true)){
                npc_records.push_back(std::move(next));
                ++bound_this_pass;
            }else{
                NpcRememberFailure(h.ref);
            }
        }

        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong NPC: pass; actors="+std::to_string(seen)+
            "/"+std::to_string(total)+" in_range="+std::to_string(hits.size())+
            " bound_now="+std::to_string(bound_this_pass)+" total="+std::to_string(npc_records.size())+
            " player_skipped="+std::to_string(skipped_player)+" player_purged="+std::to_string(purged)+
            " dressed="+std::to_string(dressed_actors.size())+
            " player_idx="+std::to_string(player_actor.index)+" record_idx="+std::to_string(record.actor.index));

        npc_scan_seen.store(seen,std::memory_order_relaxed);
        npc_scan_total.store(total,std::memory_order_relaxed);
        npc_scan_range.store(std::uint32_t(hits.size()),std::memory_order_relaxed);
        npc_scan_candidates.store(std::uint32_t(hits.size()),std::memory_order_relaxed);
        npc_scan_pages.store(pages,std::memory_order_relaxed);
        npc_scan_nearest.store(hits.empty()?0.0:hits.front().distance_m,std::memory_order_relaxed);
    }
    npc_count.store(std::uint32_t(npc_records.size()));SyncActive();
}

void ANOMALY_CALL Update(void*,double dt){
    plugin_time_seconds+=dt;

    Settings cfg;{std::lock_guard lock(state_mutex);cfg=settings;}

    // Retired components already have zero mesh sections and are hidden at
    // restore time. Do not perform a reflected IsVisible call for every one of
    // them every frame; prune their registry entries only during the 1 Hz service
    // refresh below.
    retry_time-=dt;if(retry_time<=0){
        Services();retry_time=1;
        retired_components.erase(std::remove_if(retired_components.begin(),retired_components.end(),
            [](const ue::Ref& r){return ue::Alive(r)==0;}),retired_components.end());
    }

    // A restore left over from the previous run does not need the model assets,
    // so it runs before the model gate below.
    if(!ue::InGame())return;

    if(!ue::Ready()){Status(ue::last_error.empty()?"等待 UE 反射桥接就绪":ue::last_error);return;}

    if(!deferred_restore.empty())ReplayDeferredRestore(dt);

    if(!model_loaded)return;

    if(!cfg.enabled){pending_rebind=false;previous_actor={};Restore();RestoreNpcs();return;}

    // NPC pass runs before the player settle gate so a radius/checkbox change
    // restores and rescans immediately instead of waiting out the settle window.
    UpdateNearbyNpcs(cfg,dt);

    if(cfg.revision!=observed_revision){observed_revision=cfg.revision;settle_time=0.25;}else settle_time-=dt;

    if(settle_time>0)return;



    std::uint64_t player_generation=0;const bool player_generation_valid=ReadPlayerGeneration(player_generation);

    if(record.actor.ptr&&record.player_generation_valid&&player_generation_valid&&

       player_generation!=record.player_generation){

        // The player-service generation and actor-service handle are different

        // identity domains. Use this only to force an immediate actor check;

        // never replace from the player handle itself.

        actor_verify_time=0;

    }



    const int bound_actor_alive=record.actor.ptr?ue::Alive(record.actor):0;

    switch(nailong::RouteBoundUpdate(cfg.enabled,record.actor.ptr!=0,record.revision==cfg.revision,bound_actor_alive)){

    case nailong::BoundUpdateRoute::Restore:

        Restore();return;

    case nailong::BoundUpdateRoute::Animate:

        if(!Animate(cfg,dt)){

            // A mesh/pose read failure can happen after the same pawn swaps its

            // skeletal asset. Rebuild that actor; only identity changes set the

            // pending-rebind gate below.

            pending_rebind=false;previous_actor={};

            Restore();settle_time=0.25;

            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,"NaiLong: bound pose update failed; rebuilding the current actor");

            return;

        }

        actor_verify_time-=dt;

        if(actor_verify_time<=0){

            actor_verify_time=0.25;

            ue::Ref observed;AnomalyNteEntitySnapshotV1 snapshot{};std::string class_name;

            if(ReadCurrentActor(observed,snapshot,class_name)&&

               (observed.ptr!=record.actor.ptr||observed.serial!=record.actor.serial)){

                pending_rebind=true;previous_actor=record.actor;

                Restore();settle_time=0.25;

                Status("检测到当前角色 Actor 已切换，正在重新绑定");return;

            }

        }

        return;

    case nailong::BoundUpdateRoute::RestoreThenDiscover:

        Restore();settle_time=0.25;return;

    case nailong::BoundUpdateRoute::Retry:

        Status("当前玩家 UObject 身份暂不可验证；暂停骨骼采样并重试");return;

    case nailong::BoundUpdateRoute::Unbound:

        break;

    }



    ue::Ref current;AnomalyNteEntitySnapshotV1 snapshot{};std::string class_name;

    if(!ReadCurrentActor(current,snapshot,class_name)){

        Status("等待唯一的当前玩家 Actor");return;

    }

    actor_flags.store(snapshot.flags,std::memory_order_relaxed);

    if(pending_rebind&&previous_actor.ptr&&

       (current.ptr==previous_actor.ptr&&current.serial==previous_actor.serial)){

        Status("等待角色切换后的新 Actor 快照");return;

    }

    pending_rebind=false;previous_actor={};

    if(!Replace(current,cfg,player_generation,player_generation_valid)){

        settle_time=0.5;Status("当前角色绑定失败，保留启用状态并重试");return;

    }

    actor_verify_time=0.25;

    if(record.component.ptr){
        const auto pmc=ue::Find("/Script/ProceduralMeshComponent.ProceduralMeshComponent");
        std::vector<ue::Ref> components;
        if(pmc&&ue::ComponentsByClass(record.actor.ptr,pmc,components)){
            std::size_t visible_count=0;
            for(const auto& c:components){
                bool visible=false;
                if(ue::Visible(c.ptr,visible)&&visible)++visible_count;
            }
            Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "NaiLong player census: actor="+std::to_string(record.actor.ptr)+
                " owned="+std::to_string(record.component.ptr)+
                " components="+std::to_string(components.size())+
                " visible="+std::to_string(visible_count));
        }
    }

}

void ANOMALY_CALL Draw(void*,const AnomalyUiServiceV1* ui){

    if(!ui||!ui->begin_window||!ui->end_window)return;

    ui->set_next_window_size(ui->user,420,330,4);

    // The Host draws the title-bar close button only when it is handed a writable
    // flag, and writes the click back into it; a null pointer makes the button
    // inert ("This window cannot be closed"). Keep the state here and let the Host
    // set it back to 1 when the window is reopened from the plugin list.
    int open=context_window_open;

    const int visible=ui->begin_window(ui->user,Sv("奶龙"),&open,0);

    // Written unconditionally: the click that closes the window returns 0 here,
    // and committing only on the open branch would reopen the panel next frame.
    context_window_open=open;

    if(visible!=0){

        std::lock_guard lock(state_mutex);

        int enabled=settings.enabled;bool changed=ui->checkbox(ui->user,Sv("替换当前可控角色（切换自动跟随）"),&enabled);settings.enabled=enabled!=0;

        int bone_follow=settings.bone_follow;changed = changed || ui->checkbox(ui->user,Sv("骨骼跟随 / BetterPose MMD"),&bone_follow);settings.bone_follow=bone_follow!=0;

        int nearby_npcs=settings.nearby_npcs;changed = changed || ui->checkbox(ui->user,Sv("附近 NPC 也换上奶龙"),&nearby_npcs);settings.nearby_npcs=nearby_npcs!=0;

        changed = changed || ui->slider_float(ui->user,Sv("NPC 范围（米）"),&settings.npc_radius,5,300);

        float npc_fps=float(settings.npc_pose_fps);
        if(ui->slider_float(ui->user,Sv("NPC 骨骼更新 FPS（越低越省）"),&npc_fps,5,30)){
            settings.npc_pose_fps=int(npc_fps+0.5f);changed=true;
        }

        float npc_limit=float(settings.npc_limit);
        if(ui->slider_float(ui->user,Sv("NPC 数量上限（越少越省）"),&npc_limit,1,512)){
            settings.npc_limit=int(npc_limit+0.5f);changed=true;
        }

        changed = changed || ui->slider_float(ui->user,Sv("蒙皮更新 FPS"),&settings.pose_fps,15,60);

        changed = changed || ui->slider_float(ui->user,Sv("奶龙高度（米）"),&settings.height,0.5f,3.0f);

        changed = changed || ui->slider_float(ui->user,Sv("脚底偏移（cm）"),&settings.offset,-100,100);

        changed = changed || ui->slider_float(ui->user,Sv("朝向修正（度）"),&settings.yaw,-180,180);

        if(ui->button(ui->user,Sv("一键替换全部 NPC（当前范围）"),0,0)){
            // Deliberately does NOT touch settings.revision: a revision bump re-arms
            // the player path, which would re-run Replace() on the already dressed
            // pawn and leave two NaiLong bodies on it. The one-shot flag is enough.
            settings.nearby_npcs=true;npc_bind_now_requested=true;
        }

        if(ui->button(ui->user,Sv("恢复玩家与 NPC 并移除奶龙"),0,0)){settings.enabled=false;settings.nearby_npcs=false;changed=true;}

        if(changed)++settings.revision;

        ui->separator(ui->user);

        std::string summary="已替换 NPC："+std::to_string(npc_count.load())+" 个（离开范围自动恢复）";
        ui->text(ui->user,{summary.data(),summary.size()});

        ui->text(ui->user,Sv("可直接停用插件：残留的奶龙会在下次启用时自动还原。"));

        if(!status.empty())ui->text(ui->user,{status.data(),status.size()});

    }

    ui->end_window(ui->user);

}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* h,void** context){

    host=h;Services();CacheStateDirectory();

    // A previous run may have stopped while actors were still dressed; the file
    // is replayed from Update() as soon as the Game domain is live.
    LoadDeferredRestore();

    context_window_open=1;

    model_loaded=LoadModel();*context=&settings;return Ok();

}

AnomalyStatusV1 ANOMALY_CALL Start(void*){Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong UE 1.7.7: incremental GObjects fallback for NPCs without entity pages");return Ok();}

// Snapshot everything still dressed into the flat layout that survives a stop.
// Only plugin-owned memory is touched here: the Game callbacks are already
// drained, and ue::* calls are unusable from the Lifecycle domain.
// Merge the current session's dressings into whatever the previous run left
// unfinished: a stop that happens before a deferred replay succeeds must not
// drop the older records.
void CaptureDeferredRestore(){

    std::vector<PendingRecord> merged;

    const auto remember=[&](PendingRecord& pending){
        const bool known=std::any_of(merged.begin(),merged.end(),
            [&](const PendingRecord& other){return other.actor==pending.actor;});
        if(!known)merged.push_back(std::move(pending));
    };

    for(auto& pending:deferred_restore)remember(pending);

    deferred_restore.clear();

    const auto capture=[&](const Record& source){

        if(!source.actor.ptr)return;

        PendingRecord pending;

        pending.actor=source.actor.ptr;

        pending.actor_index=source.actor.index;

        pending.actor_serial=source.actor.serial;

        pending.component=source.component.ptr;

        pending.component_index=source.component.index;

        pending.component_serial=source.component.serial;

        for(const auto& hidden:source.hidden){

            if(!hidden.object.ptr)continue;

            if(!hidden.was_visible&&!hidden.tick_changed)continue;

            PendingHidden item;

            item.object=hidden.object.ptr;

            item.index=hidden.object.index;

            item.serial=hidden.object.serial;

            item.was_visible=hidden.was_visible;

            item.tick_offset=hidden.tick_offset;

            item.tick_original=hidden.tick_original;

            item.tick_changed=hidden.tick_changed;

            pending.hidden.push_back(item);

        }

        remember(pending);

    };

    capture(record);

    for(const auto& item:npc_records)capture(item);

    deferred_restore=std::move(merged);

}

AnomalyStatusV1 ANOMALY_CALL Stop(void*,std::uint32_t){

    // The host freezes Game callbacks before on_stop, which runs on a lifecycle
    // worker, so the original meshes cannot be put back here. Failing the stop
    // only costs the user the plugin: the host quarantines a plugin whose
    // on_stop is not OK and then rejects every later enable. Hand the work to
    // the next start instead, and always report a clean stop.
    if(!active.load()&&deferred_restore.empty()){DropDeferredRestoreFile();return Ok();}

    CaptureDeferredRestore();

    if(deferred_restore.empty()){DropDeferredRestoreFile();return Ok();}

    SaveDeferredRestore();

    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,"NaiLong: "+std::to_string(deferred_restore.size())+
        " actor(s) still dressed at stop; restore deferred to the next start");

    return Ok();

}

void ANOMALY_CALL Unload(void*){

    // Do not clear `deferred_restore` here: on_stop may not have run (a hot
    // reload of the same generation still unloads), and the next Load needs the
    // list that Stop persisted.

    model={};source_rig={};ue::paths.clear();actors=nullptr;player=nullptr;host=nullptr;

    state_dir_service=nullptr;state_directory.clear();

    ue::g_core=nullptr;ue::g_objects=nullptr;ue::g_names=nullptr;ue::g_signature=nullptr;ue::g_framework=nullptr;

}

}

extern "C" __declspec(dllexport) AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* d){

    if(!d||d->struct_size<sizeof(*d))return {ANOMALY_STATUS_V1_INVALID_ARGUMENT,0,{}};

    d->struct_size=sizeof(*d);d->api_major=ANOMALY_PLUGIN_API_V1_MAJOR;d->api_minor=ANOMALY_PLUGIN_API_V1_MINOR;

    d->id=Sv(kId);d->name=Sv("奶龙");d->author=Sv("yi621yi");d->version=Sv(kVersion);

    d->on_load=Load;d->on_start=Start;d->on_stop=Stop;d->on_unload=Unload;d->on_update=Update;d->on_draw=Draw;return Ok();

}
