#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/services/plugin_state.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <set>
#include <limits>
#include "profile.hpp"
#include "access.hpp"
#include "flow.hpp"
#include "hud_map.hpp"
#include "cast_point.hpp"
#include "restock.hpp"
#include "sell_policy.hpp"
#include "persistence.hpp"

namespace {
const AnomalyUe5NamesServiceV1* names{};
const AnomalyUe5ObjectsServiceV1* objects{};
const AnomalyUe5FrameworkServiceV1* framework{};
const AnomalyUe5WorldServiceV1* world_service{};
const AnomalyUe5ProcessEventServiceV1* events{};
AnomalyGenerationHandleV1 event_subscription{};
std::atomic_uintptr_t observed_ps{},observed_ui{};
std::array<double,3> last_cast{};
bool has_cast{};
int trace_budget{100},traced_phase{-1};
int event_phase{-1};
Clock::time_point result_time{};
bool result_pending{};
bool result_success{};
uint64_t result_serial{},skipped_intro{},continued_result{};
uint64_t skipped_cinematic{};
AnomalyGenerationHandleV1 skipped_close{};
const AnomalyPluginStateServiceV1* plugin_state{};
CastPoint calibration{};
bool calibration_dirty{},paused{};
std::string current_level;
std::filesystem::path point_file;
const AnomalyWindowServiceV1* windows{};
AnomalyGenerationHandleV1 window{},epoch{},main_widget{},result_widget{},player_handle{};
std::atomic_bool enabled{},repeat{true},shown{};
uintptr_t rod_update{};
uintptr_t cast_begin{};
uintptr_t catch_continue{};
uint64_t cast_requests{};
Clock::time_point next_rod_bind{};
Clock::time_point last_rod_update{},next_pull_report{};
std::atomic<std::shared_ptr<const std::string>> status;
std::atomic_uint caught{};
int fallback_open{1};
Clock::time_point next_tick{},next_ui{};
std::map<std::string,AnomalyGenerationHandleV1> functions;
std::map<std::string,Clock::time_point> missing_functions;
std::map<std::string,int> offsets;
uintptr_t world_slot{};
fishing::Flow flow;
bool was_enabled{},was_result{};
fishing::Persistence persisted_files;
void Publish(std::string value) {status.store(std::make_shared<const std::string>(std::move(value)));}
void ANOMALY_CALL Observe(void*,uintptr_t object,uintptr_t fn,void* parameters) {
    if(object!=observed_ps.load() && object!=observed_ui.load()) return;
    const Reader r{names};const auto label=r.NameAt(fn+24);
    if(label.find("Fish")==std::string::npos && label.find("Throw")==std::string::npos &&
       label.find("Handle")==std::string::npos && label.find("Pressed")==std::string::npos) return;
    const auto phase=fishing::EventPhase(label);if(phase>=0) event_phase=phase;
    if(label=="Server_TryThrowRod") result_pending=false;
    if(label=="Client_ThrowUpResult") {
        result_pending=true;result_time=Clock::now();++result_serial;skipped_close={};result_success=false;
        if(parameters && r.Function(fn,label,2,36)) result_success=*static_cast<uint8_t*>(parameters)!=0;
        if(enabled.load() && !result_success) {paused=true;Publish("本轮钓鱼失败，已暂停。请检查鱼线耐久后重新开启。");}
        Log(std::string("autofish result success=")+(result_success?"1":"0"));
    }
    if(label=="Server_TryExitFishingState") result_pending=false;
    std::string message="autofish observed "+label;
    if(parameters && label=="Server_TryThrowRod" && r.Function(fn,label,1,24)) {
        ++cast_requests;
        std::memcpy(last_cast.data(),parameters,24);has_cast=true;
        for(const auto v:last_cast) if(!std::isfinite(v)) has_cast=false;
        if(has_cast) message+=" point="+std::to_string(last_cast[0])+","+std::to_string(last_cast[1])+","+std::to_string(last_cast[2]);
        if(has_cast && current_level.size()<calibration.world.size()) {
            calibration={};calibration.point=last_cast;
            std::memcpy(calibration.world.data(),current_level.data(),current_level.size());calibration_dirty=true;
        }
    }
    if(parameters && label=="BP_RefreshFishEndurance" && r.Function(fn,label,2,8)) {
        std::array<int32_t,2> values{};std::memcpy(values.data(),parameters,8);
        message+=" hp="+std::to_string(values[0])+"/"+std::to_string(values[1]);
    }
    if(trace_budget>0) {--trace_budget;Log(message);}
}
uintptr_t Find(const Reader& r,const std::string& path) {
    auto& h=functions[path];const auto old=Resolve(r,objects,h);if(old) return old;
    if(Clock::now()<missing_functions[path]) return 0;
    if(objects->find_exact(objects->user,StringView(path),&h).code!=0) {h={};missing_functions[path]=Clock::now()+std::chrono::seconds(5);return 0;}
    const auto found=Resolve(r,objects,h);
    if(!found) missing_functions[path]=Clock::now()+std::chrono::seconds(5);
    return found;
}
uintptr_t Fn(const Reader& r,const char* package,const char* owner,const char* name,int count,int size) {
    const auto f=Find(r,std::string("/Script/")+package+"."+owner+"."+name);
    return f && r.Function(f,name,static_cast<uint8_t>(count),static_cast<uint16_t>(size))?f:0;
}
int Field(const Reader& r,uintptr_t object,const char* name,int size) {
    uintptr_t cls{};if(!r.Read(object+16,cls)) return -1;
    const auto key=std::to_string(cls)+":"+name+":"+std::to_string(size);
    const auto cached=offsets.find(key);if(cached!=offsets.end()) return cached->second;
    int found=-1;
    for(int depth=0;cls && depth<32;++depth) {
        uintptr_t p{};if(!r.Read(cls+112,p)) break;
        for(int i=0;p && i<2048;++i) {
            if(r.NameAt(p+32)==name) {
                int bytes{},offset{};
                if(r.Read(p+52,bytes) && bytes==size && r.Read(p+68,offset) && offset>=0 && offset<0x20000) found=offset;
                break;
            }
            if(!r.Read(p+72,p)) break;
        }
        if(found>=0 || !r.Read(cls+64,cls)) break;
    }
    offsets[key]=found;return found;
}
uintptr_t Child(const Reader& r,uintptr_t parent,const char* field) {
    const auto offset=Field(r,parent,field,8);uintptr_t value{};
    if(offset>=0) r.Read(parent+offset,value);return value;
}
AnomalyGenerationHandleV1 Handle(const Reader& r,uintptr_t address) {
    int index{};AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if(!r.Read(address+12,index) || index<0 || objects->snapshot_at(objects->user,index,&snapshot).code!=0 ||
       Resolve(r,objects,snapshot.handle)!=address) return {};
    return snapshot.handle;
}
uintptr_t World(const Reader& r) {
    if(!world_slot) {
        const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        uintptr_t instruction{};int32_t displacement{};
        if(!sig || sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
           StringView(fishing::profile::World),&instruction).code!=0 || !r.Read(instruction+3,displacement)) return 0;
        world_slot=static_cast<uintptr_t>(static_cast<intptr_t>(instruction)+7+displacement);
    }
    uintptr_t value{};return r.Read(world_slot,value) && r.IsA(value,"World")?value:0;
}
bool ActiveWidget(const Reader& r,uintptr_t widget) {
    if(!widget || !r.IsA(widget,"CommonActivatableWidget")) return false;
    const auto offset=Field(r,widget,"bIsActive",1);uint8_t active{};
    return offset>=0 && r.Read(widget+offset,active) && active!=0;
}
bool ClassDerives(const Reader& r,uintptr_t type,const char* base) {
    for(int depth=0;type && depth<32;++depth) {
        if(r.NameAt(type+24)==base) return true;
        if(!r.Read(type+64,type)) break;
    }
    return false;
}
void ReadHudWidgets(const Reader& r,uintptr_t world,uintptr_t util) {
    if(Clock::now()<next_ui) return;next_ui=Clock::now()+std::chrono::milliseconds(500);
    const auto get_pc=Fn(r,"HTGame","HTUtil","GetMainPlayerController",2,16);
    std::array<uintptr_t,2> pc{world,0};
    if(!get_pc || !Invoke(r,util,get_pc,pc.data()) || !r.IsA(pc[1],"PlayerController")) return;
    const auto hud=Child(r,pc[1],"MyHUD");if(!hud || !r.IsA(hud,"HTHUD")) return;
    const auto offset=Field(r,hud,"ActivedWidgetMap",80);if(offset<0) return;
    HudMapHeader map{};if(!r.Read(hud+offset,map) || !map.Valid()) return;
    const auto get_ui=Fn(r,"HTGame","HTHUD","BP_GetUIByClass",2,24);if(!get_ui) return;
    for(int i=0;i<map.count;++i) {
        uint32_t allocated{};
        if(map.extra_bits) {if(!r.Read(map.extra_bits+4*(i/32),allocated)) continue;}
        else allocated=map.inline_bits[i/32];
        if(!(allocated&(1u<<(i%32)))) continue;
        uintptr_t type{};FName key{};
        if(!r.Read(map.data+24*i,type) || !type || !r.Read(map.data+24*i+8,key)) continue;
        const bool main=ClassDerives(r,type,"HTUI_FishMain"),result=ClassDerives(r,type,"HTUI_FishingSuccess");
        if(!main && !result) continue;
        const auto label=r.Name(key);if(label.empty() || label.size()>128) continue;
        std::wstring wide(label.begin(),label.end());
        struct Query {uintptr_t text;int32_t count,capacity;uintptr_t widget;} q{
            reinterpret_cast<uintptr_t>(wide.c_str()),static_cast<int32_t>(wide.size()+1),static_cast<int32_t>(wide.size()+1),0};
        if(!Invoke(r,hud,get_ui,&q) || !ActiveWidget(r,q.widget)) continue;
        const auto handle=Handle(r,q.widget);
        if(main && r.IsA(q.widget,"HTUI_FishMain")) main_widget=handle;
        if(result && r.IsA(q.widget,"HTUI_FishingSuccess")) result_widget=handle;
    }
}
bool ImageX(const Reader& r,uintptr_t widget,const char* field,double& x) {
    const auto image=Child(r,widget,field),slot=Child(r,image,"Slot");
    const auto get=Fn(r,"UMG","CanvasPanelSlot","GetPosition",1,16);
    const auto size_fn=Fn(r,"UMG","CanvasPanelSlot","GetSize",1,16);
    const auto align_fn=Fn(r,"UMG","CanvasPanelSlot","GetAlignment",1,16);
    std::array<double,2> position{},size{},alignment{};
    if(!slot || !r.IsA(slot,"CanvasPanelSlot") || !get || !size_fn || !align_fn ||
       !Invoke(r,slot,get,&position) || !Invoke(r,slot,size_fn,&size) || !Invoke(r,slot,align_fn,&alignment) ||
       !std::isfinite(position[0]) || !std::isfinite(size[0]) || size[0]<0 || !std::isfinite(alignment[0])) return false;
    x=fishing::MarkerCenter(position[0],size[0],alignment[0]);return true;
}
bool InvokeRodUpdate(uintptr_t function,uintptr_t widget,int direction,float delta) {
    __try {reinterpret_cast<void(__fastcall*)(void*,int,uint8_t,float)>(function)(reinterpret_cast<void*>(widget),direction,0,delta);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool LockRod(const Reader& r,uintptr_t widget,double fish,double rod) {
    if(!rod_update && Clock::now()>=next_rod_bind) {
        next_rod_bind=Clock::now()+std::chrono::seconds(5);
        const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        if(sig && sig->resolve) sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
            StringView(fishing::profile::RodUpdate),&rod_update);
    }
    const auto offset=Field(r,widget,"RodSpeed",4);float speed{};
    if(!rod_update || offset!=fishing::profile::RodSpeedOffset || !r.Read(widget+offset,speed) || speed<=0 || !std::isfinite(speed)) return false;
    const auto now=Clock::now();const float elapsed=last_rod_update.time_since_epoch().count()?std::chrono::duration<float>(now-last_rod_update).count():0.02F;
    last_rod_update=now;
    const auto delta=fishing::LockDelta(fish,rod,speed,elapsed);
    if(now>=next_pull_report) {
        next_pull_report=now+std::chrono::seconds(1);
        // Read-only diagnostics for the native minigame, guarded by its unique code signature.
        Log("autofish follow target="+std::to_string(fish)+" rod="+std::to_string(rod)+" step="+std::to_string(delta*speed));
    }
    // Keep the native rod angle synchronized even when its visual position is already correct.
    if(!delta || std::abs(fish-rod)<0.5) return InvokeRodUpdate(rod_update,widget,0,0);
    return InvokeRodUpdate(rod_update,widget,fish>rod?1:-1,delta);
}
bool Pull(const Reader& r,uintptr_t widget) {
    double fish{},rod{};
    if(!ImageX(r,widget,"Img_Fish",fish) || !ImageX(r,widget,"Img_Rod",rod)) {return false;}
    return LockRod(r,widget,fish,rod);
}
bool FinishAnimation(const Reader& r,uintptr_t widget,const char* field) {
    auto animation=Child(r,widget,field);
    const auto playing=Fn(r,"UMG","UserWidget","IsAnimationPlaying",2,9);
    struct Playing {uintptr_t animation;uint8_t active;} query{animation,0};
    if(!animation || !r.IsA(animation,"WidgetAnimation") || !playing ||
       !Invoke(r,widget,playing,&query) || !query.active) return false;
    const auto speed=Fn(r,"UMG","UserWidget","SetPlaybackSpeed",2,12);
    struct Speed {uintptr_t animation;float rate;} rate{animation,100.0F};
    // Let the real animation finish on its next tick so its completion delegates still run.
    return speed && Invoke(r,widget,speed,&rate);
}
void SkipFishingCinematic(const Reader& r,uintptr_t ps) {
    if(!enabled.load() || !result_pending || !result_success || skipped_cinematic==result_serial) return;
    const auto actor=Child(r,ps,"LevelSequenceActor"),player=Child(r,actor,"SequencePlayer");
    const auto sequence=Child(r,player,"Sequence");
    if(!actor || !player || !sequence || !r.IsA(actor,"LevelSequenceActor") || !r.IsA(player,"MovieSceneSequencePlayer")) return;
    const auto cdo=Find(r,"/Script/HTGame.Default__HTGameData"),get=Fn(r,"HTGame","HTGameData","GetFishDataAsset",1,8);
    uintptr_t asset{};if(!cdo || !get || !Invoke(r,cdo,get,&asset) || !r.IsA(asset,"FishingDataAsset")) return;
    // Match both the asset name and package to the game's fishing-success assets.
    // Other cutscenes owned by the player state must never be shortened.
    const auto sequence_name=r.NameAt(sequence+24);uintptr_t outer{};r.Read(sequence+32,outer);
    const auto package=r.NameAt(outer+24);bool match=false;
    for(const auto field:{"FemaleFishingSuccessSequence","MaleFishingSuccessSequence"}) {
        const auto offset=Field(r,asset,field,40);
        if(offset>=0 && sequence_name==r.NameAt(asset+offset+16) && package==r.NameAt(asset+offset+8)) match=true;
    }
    if(!match) return;
    const auto playing=Fn(r,"MovieScene","MovieSceneSequencePlayer","IsPlaying",1,1);
    const auto finish=Fn(r,"MovieScene","MovieSceneSequencePlayer","GoToEndAndStop",0,0);uint8_t active{};
    if(playing && finish && Invoke(r,player,playing,&active) && active && Invoke(r,player,finish,nullptr)) {
        skipped_cinematic=result_serial;Log("autofish fishing cinematic finished");next_ui={};
    }
}
void SkipCatchDisplay(const Reader& r,uintptr_t result) {
    if(!enabled.load() || !result_serial || (!result_pending && !ActiveWidget(r,result))) return;
    if(!ActiveWidget(r,result) || !r.IsA(result,"HTUI_FishingSuccess")) return;
    if(skipped_intro!=result_serial && FinishAnimation(r,result,"CurrentFishSuccessAnim")) {
        skipped_intro=result_serial;Log("autofish catch intro accelerated");
    }
    uint8_t closing{};const auto offset=Field(r,result,"bIsClosing",1);
    auto animation=Child(r,result,"CurrentFishCloseAnim");
    if(offset>=0 && r.Read(result+offset,closing) && closing && animation) {
        const auto handle=Handle(r,animation);
        if(handle.id && handle.id!=skipped_close.id && FinishAnimation(r,result,"CurrentFishCloseAnim")) {
            skipped_close=handle;Log("autofish catch close accelerated");
        }
    }
}
bool InvokeCatchContinue(uintptr_t function,uintptr_t widget) {
    __try {reinterpret_cast<void(__fastcall*)(void*,uint8_t)>(function)(reinterpret_cast<void*>(widget),1);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ContinueFishing(const Reader& r,uintptr_t widget) {
    if(!result_serial || continued_result==result_serial || !ActiveWidget(r,widget) || !r.IsA(widget,"HTUI_FishingSuccess")) return false;
    // The real continue handler refuses requests while the intro is still playing.
    // Do not mark such a no-op as a completed continuation.
    auto intro=Child(r,widget,"CurrentFishSuccessAnim");
    if(intro) {
        const auto playing=Fn(r,"UMG","UserWidget","IsAnimationPlaying",2,9);
        struct Playing {uintptr_t animation;uint8_t active;} query{intro,0};
        if(!playing || !Invoke(r,widget,playing,&query) || query.active) return false;
    }
    if(!catch_continue) {
        const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        if(!sig || !sig->resolve || sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
            StringView(fishing::profile::CatchContinue),&catch_continue).code!=0) return false;
    }
    const auto ready=Fn(r,"HTGame","HTUI_FishingSuccess","EnableCloseButton",0,0);
    if(!ready || !Invoke(r,widget,ready,nullptr) || !InvokeCatchContinue(catch_continue,widget)) return false;
    continued_result=result_serial;return true;
}
void LoadPoint() {
    point_file.clear();calibration={};has_cast=false;calibration_dirty=false;
    if(!plugin_state || !plugin_state->directory) return;
    size_t bytes{};plugin_state->directory(plugin_state->user,nullptr,&bytes);
    if(!bytes || bytes>32768) return;
    std::vector<char> directory(bytes);
    if(plugin_state->directory(plugin_state->user,directory.data(),&bytes).code!=0) return;
    point_file=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(directory.data())))/"cast-point-v1.bin";
    std::ifstream in(point_file,std::ios::binary);CastPoint saved{};
    if(in.read(reinterpret_cast<char*>(&saved),sizeof(saved)) && saved.magic==0x41465348 && saved.version==1) {
        calibration=saved;last_cast=saved.point;has_cast=true;
    }
}
bool InvokeCastBegin(uintptr_t function,uintptr_t widget) {
    __try {reinterpret_cast<void(__fastcall*)(void*)>(function)(reinterpret_cast<void*>(widget));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool Cast(const Reader& r,uintptr_t widget,uintptr_t world,uintptr_t util) {
    const auto character_fn=Fn(r,"HTGame","HTUtil","GetMainPlayerCharacter",2,16);
    std::array<uintptr_t,2> character{world,0};
    const auto position_fn=Fn(r,"Engine","Actor","K2_GetActorLocation",1,24);
    std::array<double,3> position{};
    if(!has_cast || !character_fn || !position_fn || !Invoke(r,util,character_fn,character.data()) ||
       !r.IsA(character[1],"Actor") || !Invoke(r,character[1],position_fn,&position) || !calibration.Valid(current_level,position)) {
        has_cast=false;Publish("请在此钓鱼点手动按 F 抛竿一次，记录抛竿位置。");return false;
    }
    if(!ActiveWidget(r,widget) || !r.IsA(widget,"HTUI_FishMain")) return false;
    if(!cast_begin) {
        const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        if(!sig || !sig->resolve || sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
            StringView(fishing::profile::CastBegin),&cast_begin).code!=0) return false;
    }
    // The F-button's complete cast path validates gear and aiming, resets line endurance,
    // sets local fishing state, then sends the request. A bare RPC skips those steps.
    const auto before=cast_requests;
    return InvokeCastBegin(cast_begin,widget) && cast_requests!=before;
}
#include "bait_runtime.hpp"
#include "sell_runtime.hpp"
bool EnsureWindow() {
    if(window.id) return true;
    if(!windows || !windows->register_window || !windows->begin || !windows->end || !windows->state) return false;
    AnomalyWindowSpecV1 spec{};spec.struct_size=sizeof(spec);spec.id=StringView("autofish-main");spec.title=StringView("autofish");
    spec.initial_width=420;spec.initial_height=530;spec.minimum_width=280;spec.minimum_height=180;
    spec.maximum_width=650;spec.maximum_height=600;spec.default_open=1;
    return windows->register_window(windows->user,&spec,&window).code==0 && window.id;
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host,void** context) noexcept {
    api=host;core=Host(host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
    names=Host(host).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID,2).get();
    objects=Host(host).Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
    framework=Host(host).Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
    world_service=Host(host).Query<AnomalyUe5WorldServiceV1>(ANOMALY_UE5_WORLD_SERVICE_V1_ID).get();
    plugin_state=Host(host).Query<AnomalyPluginStateServiceV1>(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID).get();
    events=Host(host).Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID).get();
    windows=Host(host).Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID).get();
    if(context) *context=nullptr;return Ok();
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) noexcept {
    enabled=false;repeat=true;shown=false;caught=0;fallback_open=1;flow.Reset();was_enabled=was_result=false;
    rod_update=cast_begin=catch_continue=0;cast_requests=0;next_rod_bind=last_rod_update=next_pull_report={};
    observed_ps=observed_ui=0;has_cast=false;last_cast={};trace_budget=100;traced_phase=-1;event_subscription={};
    event_phase=-1;result_pending=false;result_success=false;result_time={};
    result_serial=skipped_intro=continued_result=skipped_cinematic=0;skipped_close={};
    paused=false;current_level.clear();LoadPoint();LoadBaitSettings();LoadSellSettings();
    persisted_files.Start(Host(api).Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get());
    bait_catalog.store({});bait_stock=-1;catalog_build={};next_catalog=next_stock={};
    restock={};pending_bait={};equip_pending=false;equip_sent=0;text_to_string=0;
    if(events && events->subscribe) events->subscribe(events->user,Observe,nullptr,&event_subscription);
    registry=world_slot=0;retry_init=next_tick=next_ui={};functions.clear();missing_functions.clear();offsets.clear();
    window=epoch=main_widget=result_widget=player_handle={};Publish("请打开钓鱼界面，再开启自动钓鱼。");EnsureWindow();return Ok();
}
AnomalyStatusV1 ANOMALY_CALL Stop(void*,uint32_t) noexcept {
    enabled=false;shown=false;
    if(events && event_subscription.id && events->unsubscribe) events->unsubscribe(events->user,event_subscription);
    event_subscription={};observed_ps=observed_ui=0;
    try {
        persisted_files.BeginStop();
        if(!point_file.empty() && calibration.world[0]) persisted_files.Store(point_file,&calibration,sizeof(calibration));
        saved_bait_settings={};saved_sell_settings={};SyncBaitSettings();SyncSellSettings();
        persisted_files.FinishStop();
    } catch(...) {Log("autofish settings could not be saved during stop");}
    if(window.id && windows && windows->release_window) windows->release_window(windows->user,window);
    window={};return Ok();
}
void ANOMALY_CALL Unload(void*) noexcept {}
void ANOMALY_CALL Update(void*,double) noexcept {
    try {
        if(!enabled.load() && !shown.load() && !AutoSellEnabled()) return;
        if(Clock::now()<next_tick) return;
        next_tick=Clock::now()+std::chrono::milliseconds(enabled.load() && event_phase==5?20:100);
        if(!core || !names || !objects || !framework || !world_service || !framework->is_game_thread(framework->user)) return;
        const Reader r{names};if(!Initialize(r,objects)) {Publish("正在等待游戏接口。");return;}
        SyncBaitSettings();SyncSellSettings();UpdateBaitCatalog(r);
        AnomalyGenerationHandleV1 current{};
        if(world_service->current(world_service->user,&current).code!=0 || !current.id) return;
        if(epoch.id && (current.id!=epoch.id || current.generation!=epoch.generation)) {
                    enabled=false;flow.Reset();main_widget=result_widget=player_handle={};functions.clear();missing_functions.clear();offsets.clear();next_ui={};
            event_phase=-1;result_pending=false;has_cast=false;
            Publish("场景已切换，自动钓鱼已停止。");
        }
        epoch=current;
        const auto world=World(r),util=Find(r,"/Script/HTGame.Default__HTUtil");
        if(world) current_level=r.NameAt(world+24);
        if(calibration_dirty && !point_file.empty()) {
            if(persisted_files.Store(point_file,&calibration,sizeof(calibration))) calibration_dirty=false;
        }
        const auto get=Fn(r,"HTGame","HTUtil","GetMainPlayerState",2,16);
        std::array<uintptr_t,2> p{world,0};
        if(!world || !util || !get || !Invoke(r,util,get,p.data()) || !r.IsA(p[1],"HTPlayerState")) {Publish("正在等待玩家。");return;}
        const auto ps=p[1];const auto handle=Handle(r,ps);
        if(!handle.id) return;
        if(player_handle.id && player_handle.id!=handle.id) {enabled=false;flow.Reset();Publish("玩家已变化，自动钓鱼已停止。");}
        player_handle=handle;
        RefreshBaitStock(r,ps);
        observed_ps=ps;
        const auto phase_offset=Field(r,ps,"CurrFishingState",1);uint8_t phase{};
        if(phase_offset<0 || !r.Read(ps+phase_offset,phase)) {enabled=false;Publish("当前钓鱼接口暂不可用。");return;}
        auto main=Resolve(r,objects,main_widget),result=Resolve(r,objects,result_widget);
        bool main_open=ActiveWidget(r,main),result_open=ActiveWidget(r,result);
        if((!main_open && !result_open) || result_pending) {ReadHudWidgets(r,world,util);main=Resolve(r,objects,main_widget);result=Resolve(r,objects,result_widget);main_open=ActiveWidget(r,main);result_open=ActiveWidget(r,result);}
        SkipFishingCinematic(r,ps);SkipCatchDisplay(r,result);
        observed_ui=main;
        if(event_phase>=0) phase=static_cast<uint8_t>(event_phase);
        if(result_pending && !result_open && main_open && Clock::now()-result_time>std::chrono::seconds(2)) {phase=1;event_phase=1;result_pending=false;}
        if(traced_phase!=phase) {Log("autofish phase="+std::to_string(phase));traced_phase=phase;}
        const bool completed=result_open || result_pending;
        if(completed && !was_result && result_success) ++caught;was_result=completed;
        const bool on=enabled.load();if(on!=was_enabled) {flow.Reset();paused=false;restock.Retry();equip_pending=false;was_enabled=on;}
        const auto now=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        const bool selling=ServiceAutoSell(r,ps,phase<=1 && (!result_pending || result_open),now);
        if(on && paused) return;
        if(phase!=5) last_rod_update={};
        static constexpr std::array<const char*,6> labels{"请打开钓鱼界面。","准备抛竿。","正在抛竿。","等待鱼咬钩。","鱼已咬钩，准备提竿。","正在自动遛鱼。"};
        Publish(completed?"钓鱼结算中。":phase<labels.size()?labels[phase]:"未知钓鱼状态，已暂停。");
        if(!on) return;
        if(selling) {Publish("正在等待卖鱼完成。");return;}
        const auto action=flow.Next(phase,main_open,completed,repeat.load(),now,result_open && continued_result!=result_serial);bool sent=false;
        if((action==fishing::Action::Cast || action==fishing::Action::Continue) && !EnsureBait(r,ps,util,now)) return;
        if(action==fishing::Action::Cast) {
            sent=Cast(r,main,world,util);
            if(!sent && !has_cast) return;
        } else if(action==fishing::Action::Hook) {
            const auto fn=Fn(r,"HTGame","HTPlayerState","ServerPlayerSureFishing",0,0);sent=fn && Invoke(r,ps,fn,nullptr);
        }
        else if(action==fishing::Action::Pull) {
            if(!Pull(r,main)) {enabled=false;Publish("遛鱼控制条暂不可用，自动操作已停止。");}
            return;
        } else if(action==fishing::Action::Continue) sent=ContinueFishing(r,result);
        else if(action==fishing::Action::Stop) {paused=true;Publish(completed&&!repeat.load()?"单次钓鱼已完成。":"未收到钓鱼响应，自动操作已暂停。关闭再开启可重试。");}
        if(action!=fishing::Action::None && action!=fishing::Action::Stop) {
            // Waiting for the intro or a result widget is not a sent continuation.
            if(action!=fishing::Action::Continue || sent) flow.Sent(now);
            if(sent) Log("autofish action="+std::to_string(static_cast<int>(action))+" phase="+std::to_string(phase));
            else Publish(action==fishing::Action::Cast?"请把抛竿点对准水面，正在等待按钮可用。":"钓鱼按钮暂不可用，正在等待。");
        }
    } catch(...) {enabled=false;Publish("钓鱼界面刷新中，自动操作已停止。");}
}
void Content(const AnomalyUiServiceV1* ui) {
    int on=enabled.load(),loop=repeat.load();
    if(ui->checkbox(ui->user,StringView("启用自动钓鱼"),&on)) enabled=on!=0;
    if(ui->checkbox(ui->user,StringView("连续钓鱼"),&loop)) repeat=loop!=0;
    if(ui->separator) ui->separator(ui->user);
    BaitContent(ui);
    if(ui->separator) ui->separator(ui->user);
    SellContent(ui);
    if(ui->separator) ui->separator(ui->user);
    ui->text(ui->user,StringView("更换钓鱼点后，先手动抛竿一次。"));
    if(const auto s=status.load()) ui->text(ui->user,StringView(*s));
    ui->text(ui->user,StringView(std::string("本次收鱼：")+std::to_string(caught.load())));
}
void ANOMALY_CALL Draw(void*,const AnomalyUiServiceV1* ui) noexcept {
    try {
        shown=false;if(!ui || !ui->checkbox || !ui->text) return;
        if(EnsureWindow()) {
            AnomalyWindowStateV1 state{sizeof(state)};if(windows->state(windows->user,window,&state).code!=0 || !state.open) return;
            int32_t open{};if(windows->begin(windows->user,window,0,&open).code!=0) return;
            if(open) {shown=true;Content(ui);}windows->end(windows->user,window);return;
        }
        UiWindow panel(ui,"autofish",&fallback_open);if(panel) {shown=true;Content(ui);}
    } catch(...) {}
}
}
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if(!descriptor || descriptor->struct_size<sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT,0,{}};
    *descriptor={sizeof(*descriptor),ANOMALY_PLUGIN_API_V1_MAJOR,ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("b1ank.autofish"),StringView("autofish"),StringView("b1ank"),StringView("0.1.15"),Load,Start,Stop,Unload,Update,Draw};return Ok();
}
