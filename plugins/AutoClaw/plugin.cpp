#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/localization.hpp>
#include <atomic>
#include <memory>
#include <new>
#include <vector>
#include "engine.hpp"
#include "readiness.hpp"

namespace {
using namespace anomaly::sdk;
using Handle=AnomalyGenerationHandleV1;
struct Item { Handle actor{}; claw::Name doll{}; std::string id,label; };
struct Scope {
    std::uintptr_t world{},player{},controller{},hud{},main{},operation{},machine{};
    Handle world_epoch{};
    claw::Identity identity{};
    std::string machine_id;
};
struct View {
    claw::Identity identity{};
    std::uint64_t round{},revision{};
    std::vector<Item> items;
    std::string machine,status,result;
    bool ready{};
};
struct Command { claw::Identity identity{}; std::uint64_t round{}; Handle actor{}; claw::Name doll{}; std::uint64_t revision{},expires{}; };
struct Context;
struct InitResult {std::uint64_t serial{};std::uintptr_t registry{},world_slot{};};
struct InitWork {
    Context* context{};const AnomalyHostApiV1* api{};const AnomalyCoreServiceV1* core{};
    std::shared_ptr<const claw::compat::Snapshot> profile;
    std::uint64_t serial{};std::atomic_bool cancelled{};
};
struct Context {
    ue::Engine engine;
    anomaly::sdk::Localizer localizer;
    const AnomalyWindowServiceV1* windows{};
    const AnomalyUe5ProcessEventServiceV1* event_owner{};
    const AnomalySchedulerServiceV1* scheduler{};
    Handle window{},subscription{},candidate{},game_data{};
    Handle init_task{};
    std::atomic_bool running{};
    std::atomic_bool detection{};
    std::atomic<const AnomalyUe5FrameworkServiceV1*> callback_framework{};
    std::atomic_uint64_t revision{},init_serial{},init_failed{};
    std::uint64_t applied_revision{},next_poll{},next_scan{};
    std::atomic<std::shared_ptr<const InitResult>> initialized;
    std::vector<std::unique_ptr<InitWork>> init_work;
    claw::ReadyGate gate;
    bool prepared{},init_requested{};
    std::atomic<std::shared_ptr<const View>> view;
    std::atomic<std::shared_ptr<const Command>> command;
    std::atomic<std::shared_ptr<const Command>> selection;
    std::shared_ptr<const Command> paid_target;
    Scope scope;
    Scope scan_scope;
    claw::Round round;
    std::atomic_uintptr_t server_fn{},client_fn{},open_fn{};
    std::uint64_t next_services{},next_view{},last_world{};
    std::uint32_t scan{},scan_level{};
    std::uint64_t next_data{};
    int fallback_open{1};
    std::uint64_t selected{};
    claw::Identity selected_scope{};
};
bool Active(ue::Engine& e,std::uintptr_t widget,const char* type) {
    if(!e.Live(widget,type))return false;
    const auto at=e.Field(widget,"bIsActive",1,"BoolProperty"),visible=e.Field(widget,"Visibility",1,"EnumProperty");
    std::uint8_t active{},visibility{};
    return at>=0&&visible>=0&&e.Read(widget+at,active)&&active==1&&e.Read(widget+visible,visibility)&&
        (visibility==0||visibility==3||visibility==4);
}
std::uintptr_t ActiveWidget(ue::Engine& e,const Scope& scope,const char* type) {
    struct Query {claw::Array name{};std::uintptr_t result{};};
    static_assert(sizeof(Query)==24&&offsetof(Query,result)==16);
    if(!e.Live(scope.hud,"HTHUD")||e.Child(scope.hud,"PlayerOwner")!=scope.controller)return 0;
    const auto at=e.Field(scope.hud,"ActivedWidgetMap",e.layout().MapSize,"MapProperty");
    const auto get=e.Function("HTGame","HTHUD","BP_GetUIByClass",2,24,{{"UIClass",0,16,"StrProperty"},{"ReturnValue",16,8,"ObjectProperty"}});
    const auto owner=e.Function("UMG","Widget","GetOwningPlayer",1,8,{{"ReturnValue",0,8,"ObjectProperty"}});
    ue::SparseMap map{};
    if(at<0||!get||!owner||!ue::ReadMap(e,scope.hud+at,map,512,4096))return 0;
    for(int i=0;i<map.slots.count;++i) {
        auto bits=map.bits[(i/32)%4];
        if(map.extra&&!e.Read(map.extra+4*(i/32),bits))return 0;
        if(!(bits&(1u<<(i%32))))continue;
        std::uintptr_t cls{};claw::Name key{};
        if(!e.Read(map.slots.data+e.layout().MapEntryStride*i,cls)||!e.Read(map.slots.data+e.layout().MapEntryStride*i+e.layout().MapValue,key))continue;
        // The map key can be a blueprint-derived widget class.
        bool matches=false;
        for(int depth=0;cls&&depth<32;++depth) {if(e.NameAt(cls+e.layout().ObjectName)==type){matches=true;break;}if(!e.Read(cls+e.layout().Super,cls))break;}
        if(!matches)continue;
        const auto label=e.Name(key);if(label.empty()||label.size()>255)continue;
        const auto length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,label.data(),static_cast<int>(label.size()),nullptr,0);
        if(length<=0)continue;
        std::wstring wide(static_cast<std::size_t>(length),L'\0');
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,label.data(),static_cast<int>(label.size()),wide.data(),length);
        Query query{{reinterpret_cast<std::uintptr_t>(wide.c_str()),length+1,length+1}};std::uintptr_t pc{};
        if(ue::Invoke(e,scope.hud,get,&query)&&Active(e,query.result,type)&&ue::Invoke(e,query.result,owner,&pc)&&pc==scope.controller)return query.result;
    }
    return 0;
}
bool BaseScope(ue::Engine& e,Scope& scope) {
    struct Query {std::uintptr_t context{},value{};};
    scope.world=e.World(&scope.world_epoch);if(!scope.world)return false;
    const auto utility=e.Find("/Script/HTGame.Default__HTUtil");
    const auto get_ps=e.Function("HTGame","HTUtil","GetMainPlayerState",2,16,{{"WorldContextObject",0,8,"ObjectProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
    const auto get_pc=e.Function("HTGame","HTUtil","GetMainPlayerController",2,16,{{"WorldContextObject",0,8,"ObjectProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
    Query ps{scope.world},pc{scope.world};
    if(!utility||!get_ps||!get_pc||!ue::Invoke(e,utility,get_ps,&ps)||!ue::Invoke(e,utility,get_pc,&pc))return false;
    scope.player=ps.value;scope.controller=pc.value;
    if(!e.Live(scope.player,"HTPlayerState")||!e.Live(scope.controller,"HTPlayerController")||e.Child(scope.controller,"PlayerState")!=scope.player||
       e.OwningWorld(scope.player)!=scope.world||e.OwningWorld(scope.controller)!=scope.world)return false;
    scope.hud=e.Child(scope.controller,"MyHUD");
    scope.main=ActiveWidget(e,scope,"HTUI_ClawMachineMain");
    scope.operation=ActiveWidget(e,scope,"HTUI_ClawMachine");
    return true;
}
bool Interacting(ue::Engine& e,std::uintptr_t machine,std::uintptr_t world) {
    if(!e.Live(machine,"HTClawMachineActor")||e.OwningWorld(machine)!=world)return false;
    const auto at=e.Field(machine,"bIsInteracting",1,"BoolProperty");std::uint8_t active{};
    return at>=0&&e.Read(machine+at,active)&&active==1;
}
void Scout(Context& c,const Scope& scope) {
    auto& e=c.engine;
    if(!e.Resolve(c.game_data)&&ue::Now()>=c.next_data) {
        c.next_data=ue::Now()+2000;
        const auto utility=e.Find("/Script/HTGame.Default__HTGameData");
        const auto getter=e.Function("HTGame","HTGameData","GetGameData",1,8,{{"ReturnValue",0,8,"ObjectProperty"}});
        std::uintptr_t data{};
        if(utility&&getter&&ue::Invoke(e,utility,getter,&data)&&e.IsA(data,"HTGameData"))c.game_data=e.Handle(data);
    }
    const auto candidate=e.Resolve(c.candidate);
    if(candidate&&Interacting(e,candidate,scope.world))return;
    const auto at=e.Field(scope.world,"Levels",sizeof(claw::Array),"ArrayProperty");claw::Array levels{};
    if(at<0||!e.Read(scope.world+at,levels)||!ue::ValidArray(levels,4096)||!levels.count)return;
    const auto until=ue::Clock::now()+std::chrono::milliseconds(3);
    std::array<std::uintptr_t,128> pointers{};claw::Array actors{};
    std::uint32_t cached_level=UINT32_MAX;std::size_t visited{};
    // The cache lives only in this Game callback. No UE call runs while it is
    // populated, so class pointers cannot outlive the current scan slice.
    std::map<std::uintptr_t,bool> machine_types;
    while(visited<1024&&ue::Clock::now()<until) {
        if(c.scan_level>=static_cast<std::uint32_t>(levels.count)){c.scan_level=0;c.scan=0;cached_level=UINT32_MAX;}
        if(cached_level!=c.scan_level) {
            std::uintptr_t level{};
            if(!e.Read(levels.data+sizeof(std::uintptr_t)*c.scan_level,level)||!e.IsA(level,"Level")||
               e.Child(level,"OwningWorld")!=scope.world||!e.Read(level+e.layout().LevelActors,actors)||!ue::ValidArray(actors,65536)||
               c.scan>=static_cast<std::uint32_t>(actors.count)) {++c.scan_level;c.scan=0;++visited;continue;}
            cached_level=c.scan_level;
        }
        const auto batch=std::min({pointers.size(),static_cast<std::size_t>(actors.count)-c.scan,std::size_t{1024}-visited});
        if(!e.ReadBytes(actors.data+sizeof(std::uintptr_t)*c.scan,pointers.data(),batch*sizeof(std::uintptr_t))) {
            ++c.scan_level;c.scan=0;cached_level=UINT32_MAX;++visited;continue;
        }
        for(std::size_t i=0;i<batch&&ue::Clock::now()<until;++i) {
            ++c.scan;++visited;const auto actor=pointers[i];std::uintptr_t cls{};
            if(!actor||!e.Read(actor+e.layout().ObjectClass,cls))continue;
            auto kind=machine_types.find(cls);
            if(kind==machine_types.end())kind=machine_types.emplace(cls,e.IsA(actor,"HTClawMachineActor")).first;
            if(kind->second&&Interacting(e,actor,scope.world)){c.candidate=e.Handle(actor);return;}
        }
        if(c.scan>=static_cast<std::uint32_t>(actors.count)){++c.scan_level;c.scan=0;cached_level=UINT32_MAX;}
    }
}

bool MachineScope(Context& c,Scope& scope) {
    auto& e=c.engine;scope.machine=e.Resolve(c.candidate);
    if((!scope.main&&!scope.operation)||!Interacting(e,scope.machine,scope.world))return false;
    const auto at=e.Field(scope.machine,"machineID",8,"NameProperty");claw::Name id{};
    if(at<0||!e.Read(scope.machine+at,id)||(scope.machine_id=e.Name(id)).empty()||scope.machine_id=="None")return false;
    const auto world=e.Handle(scope.world),player=e.Handle(scope.player),machine=e.Handle(scope.machine);
    scope.identity.handles={world.id,world.generation,player.id,player.generation,machine.id,machine.generation,scope.world_epoch.id,scope.world_epoch.generation};
    return scope.identity.Valid();
}

std::uintptr_t Row(ue::Engine& e,std::uintptr_t table,const char* type,claw::Name id) {
    // Read-only DataTable sparse map. The row type and fields are validated
    // before interpreting its contents; sparse holes are never treated as rows.
    const auto row=e.Child(table,"RowStruct");
    if(!e.IsA(table,"DataTable")||!row||e.NameAt(row+e.layout().ObjectName)!=type)return 0;
    ue::SparseMap map{};
    if(!ue::ReadMap(e,table+e.layout().DataTableRows,map,65536,1048576)||!map.slots.count)return 0;
    std::vector<std::uint8_t> slots(static_cast<std::size_t>(map.slots.count)*e.layout().MapEntryStride);
    std::vector<std::uint32_t> bits(static_cast<std::size_t>((map.count+31)/32));
    if(!e.ReadBytes(map.slots.data,slots.data(),slots.size()))return 0;
    if(map.extra) {if(!e.ReadBytes(map.extra,bits.data(),bits.size()*4))return 0;}
    else std::copy_n(map.bits.begin(),bits.size(),bits.begin());
    for(int i=0;i<map.slots.count;++i) {
        if(!(bits[static_cast<std::size_t>(i/32)]&(1u<<(i%32))))continue;
        claw::Name key{};std::uintptr_t value{};
        std::memcpy(&key,slots.data()+e.layout().MapEntryStride*i,sizeof(key));
        if(key==id){std::memcpy(&value,slots.data()+e.layout().MapEntryStride*i+e.layout().MapValue,sizeof(value));return value;}
    }
    return 0;
}
std::string ItemLabel(Context& c,const Item& item) {
    auto& e=c.engine;const auto data=e.Resolve(c.game_data);
    if(!data)return item.id;
    const auto dolls=e.Child(data,"DollDataTable"),items=e.Child(data,"ItemDataTable");
    const auto doll_type=e.Child(dolls,"RowStruct"),item_type=e.Child(items,"RowStruct");
    const auto item_at=e.Offset(doll_type,"ItemID",sizeof(claw::Name),"NameProperty");
    const auto text_at=e.Offset(item_type,"ItemName",16,"TextProperty");
    if(item_at<0||text_at<0)return item.id;
    const auto doll=Row(e,dolls,"StaticDollData",item.doll);claw::Name item_id{};
    if(!doll||!e.Read(doll+item_at,item_id))return item.id;
    const auto row=Row(e,items,"StaticItemData",item_id);
    std::array<char,1024> name{};std::size_t size=name.size();
    if(row&&e.names&&e.names->resolve_ftext_utf8&&e.names->resolve_ftext_utf8(e.names->user,row+text_at,name.data(),&size).code==0&&size&&size<=name.size())
        return c.localizer.Format("item.label","{0} ({1})",std::array<std::string_view,2>{name.data(),item.id});
    return item.id;
}
std::vector<Item> Items(Context& c,const Scope& scope) {
    auto& e=c.engine;const auto at=e.Field(scope.machine,"DollActors",16,"ArrayProperty");claw::Array array{};
    std::vector<Item> result;
    if(at<0||!e.Read(scope.machine+at,array)||!ue::ValidArray(array,256))return result;
    for(int i=0;i<array.count;++i) {
        std::uintptr_t actor{};
        if(!e.Read(array.data+8*i,actor)||!e.Live(actor,"HTDollActor")||e.OwningWorld(actor)!=scope.world)continue;
        const auto offset=e.Field(actor,"DollID",8,"NameProperty");claw::Name doll{};
        if(offset<0||!e.Read(actor+offset,doll))continue;
        const auto id=e.Name(doll);if(id.empty()||id=="None"||claw::EncodeName(id).empty())continue;
        if(std::any_of(result.begin(),result.end(),[&](const Item& v){return v.doll==doll;}))continue;
        Item item{e.Handle(actor),doll,id,{}};item.label=ItemLabel(c,item);result.push_back(std::move(item));
    }
    return result;
}
bool Bound(Context& c,const claw::Identity& id,std::uintptr_t player=0) {
    auto& e=c.engine;
    const auto world=e.Resolve({id.handles[0],id.handles[1]}),ps=e.Resolve({id.handles[2],id.handles[3]}),machine=e.Resolve({id.handles[4],id.handles[5]});
    Handle epoch{};
    return id.Valid()&&world&&ps&&machine&&e.World(&epoch)==world&&epoch.id==id.handles[6]&&epoch.generation==id.handles[7]&&(!player||ps==player)&&e.Live(ps,"HTPlayerState")&&
        e.Live(machine,"HTClawMachineActor")&&e.OwningWorld(ps)==world&&e.OwningWorld(machine)==world;
}
void ANOMALY_CALL Observe(void* user,std::uintptr_t object,std::uintptr_t function,void* parameters) noexcept {
    auto& c=*static_cast<Context*>(user);
    // The shared dispatcher sees all ProcessEvents, including loading traffic
    // on other threads. Ignore unrelated functions before any host call.
    const auto server=c.server_fn.load(),client=c.client_fn.load(),open=c.open_fn.load();
    if(!function||(function!=server&&function!=client&&function!=open)||!c.running.load()||!c.detection.load())return;
    try {
        const auto* framework=c.callback_framework.load();
        if(!framework||!framework->is_game_thread||!framework->is_game_thread(framework->user)||c.applied_revision!=c.revision.load())return;
        auto& e=c.engine;
        if(function==open&&e.Live(object,"HTClawMachineActor")) {c.candidate=e.Handle(object);return;}
        if(!parameters||(function!=server&&function!=client))return;
        claw::Rpc rpc{};if(!e.Read(reinterpret_cast<std::uintptr_t>(parameters),rpc)||!ue::ValidArray(rpc.parameters.items,16)||rpc.parameters.cursor!=0)return;
        if(function==server&&object==c.scope.player&&Bound(c,c.scope.identity,object)) {
            const auto machine=ue::WrapperName(e,rpc.parameters,0);
            if(!machine||*machine!=c.scope.machine_id)return;
            if(rpc.message==0&&rpc.parameters.items.count==1) {
                const bool fresh=c.round.state!=claw::State::WaitingBegin&&c.round.state!=claw::State::Pending;
                c.round.BeginRequest(c.scope.identity,ue::Now());
                if(fresh&&c.round.state==claw::State::WaitingBegin) {
                    const auto target=c.selection.load();
                    c.paid_target=target&&target->identity==c.scope.identity&&target->revision==c.applied_revision?target:nullptr;
                }
            } else if(rpc.message==1&&rpc.parameters.items.count==2) {
                c.round.ManualCapture();c.command.store({});
            }
        } else if(function==client&&Bound(c,c.round.identity,object)) {
            if(rpc.message==0) {
                const auto state=ue::WrapperInteger(e,rpc.parameters,0,6);
                if(state&&*state>=0) {
                    const bool waiting=c.round.state==claw::State::WaitingBegin;
                    const auto now=ue::Now();c.round.BeginResponse(c.round.identity,now);
                    if(waiting&&c.round.state==claw::State::Ready&&c.paid_target) {
                        auto command=*c.paid_target;command.round=c.round.sequence;command.expires=now+5000;
                        c.command.store(std::make_shared<Command>(command));
                    }
                    c.next_view=0;
                }
            } else if(rpc.message==1) {
                const auto failed=ue::WrapperInteger(e,rpc.parameters,0,1);
                if(!failed||(*failed!=0&&*failed!=1))return;
                if(*failed)c.round.Result(c.round.identity,true,{},{});
                else {
                    const auto doll=ue::WrapperName(e,rpc.parameters,1),item=ue::WrapperName(e,rpc.parameters,2);
                    if(doll&&item)c.round.Result(c.round.identity,false,*doll,*item);
                }
            }
        }
    } catch(...) {}
}
bool ResolveFunctions(Context& c) {
    auto& e=c.engine;
    c.server_fn=e.Function("HTGame","HTPlayerState","ServerClawMachineRPC",2,32,{{"MsgType",0,4,"IntProperty"},{"WrapperArray",8,24,"StructProperty"}},0x00200440);
    c.client_fn=e.Function("HTGame","HTPlayerState","ClientClawMachineRPC",2,32,{{"MsgType",0,4,"IntProperty"},{"WrapperArray",8,24,"StructProperty"}},0x01000440);
    c.open_fn=e.Function("HTGame","HTClawMachineActor","OpenClawMachine",0,0,{});
    const auto wrapper=e.Find("/Script/HottaFramework.ParameterWrapper"),array=e.Find("/Script/HottaFramework.ParameterWrapperArray");
    const auto type=e.Find("/Script/HottaFramework.ParameterTypeInfo");
    return c.server_fn&&c.client_fn&&e.Property(wrapper,"TypeInfo",0,12,"StructProperty")&&e.Property(wrapper,"BinaryData",16,16,"ArrayProperty")&&
        e.Property(array,"ParameterWrappers",0,16,"ArrayProperty")&&e.Property(array,"ReadParameterIndex",16,4,"IntProperty")&&
        e.Property(type,"Type",0,1,"EnumProperty")&&e.Property(type,"CustomTypeName",4,8,"NameProperty");
}
std::string RoundStatus(Context& c) {
    switch(c.round.state) {
    case claw::State::WaitingBegin:return c.localizer.Text("status.begin_pending","Waiting for the game to confirm the manually paid round.");
    case claw::State::Ready:return c.paid_target?c.localizer.Text("status.ready","Round confirmed. Capturing the selected doll automatically."):
        c.localizer.Text("status.no_target","No target was selected before inserting the coin. Finish this round in the game, then select a doll before the next round.");
    case claw::State::Used:return c.localizer.Text("status.used","This round already sent a capture request in the game.");
    case claw::State::Pending:return c.localizer.Text("status.pending","Capture requested. Waiting for the server result.");
    case claw::State::Success:return c.localizer.Text("status.success","The server confirmed the selected doll.");
    case claw::State::Rejected:return c.localizer.Text("status.rejected","The server returned a failed capture. No automatic retry.");
    case claw::State::Unconfirmed:return c.localizer.Text("status.unconfirmed","The selected reward was not confirmed. No automatic retry.");
    default:return c.localizer.Text("status.coin","Select a doll, then insert a coin manually in the game. Capture starts automatically after round confirmation.");
    }
}
void Publish(Context& c,const std::string& status,std::vector<Item> items={},bool actions=false) {
    auto v=std::make_shared<View>();v->identity=c.scope.identity;v->round=c.round.sequence;v->revision=c.applied_revision;v->machine=c.scope.machine_id;
    v->status=status;v->items=std::move(items);v->ready=actions&&c.running.load()&&c.detection.load()&&c.subscription.id&&c.scope.operation&&c.round.CanSubmit(c.scope.identity,c.round.sequence);
    if(c.round.state==claw::State::Success)v->result=c.localizer.Format("label.result","Confirmed item: {0}",std::array<std::string_view,1>{c.round.item});
    c.view.store(std::move(v));
}
claw::ReadinessSample Readiness(ue::Engine& e) {
    AnomalyNteSessionSnapshotV1 session{sizeof(session)};
    AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
    if(!e.session||!e.session->snapshot||!e.player||!e.player->snapshot||
       e.session->snapshot(e.session->user,&session).code!=0||session.state!=ANOMALY_NTE_SESSION_V1_WORLD_READY||
       e.player->snapshot(e.player->user,&player).code!=0||!(player.flags&ANOMALY_NTE_SNAPSHOT_V1_VALID)||
       (player.flags&ANOMALY_NTE_SNAPSHOT_V1_PARTIAL))return {};
    return {{session.world.id,session.world.generation,player.handle.id,player.handle.generation},player.sequence,true};
}
void SetDetection(Context& c,bool enabled) {
    if(c.detection.exchange(enabled)!=enabled)c.revision.fetch_add(1);
    c.command.store({});
}
AnomalyStatusV1 Unsubscribe(Context& c) {
    if(!c.subscription.id)return Ok();
    if(!c.event_owner||!c.event_owner->unsubscribe)return {ANOMALY_STATUS_V1_UNAVAILABLE,0,{}};
    const auto status=c.event_owner->unsubscribe(c.event_owner->user,c.subscription);
    if(status.code!=0&&status.code!=ANOMALY_STATUS_V1_NOT_FOUND)return status;
    c.subscription={};c.event_owner=nullptr;return Ok();
}
AnomalyStatusV1 ResetActivation(Context& c) {
    c.init_serial.fetch_add(1);
    for(const auto& work:c.init_work)work->cancelled=true;
    c.initialized.store({});c.init_failed=0;
    AnomalyStatusV1 cancelled=Ok();
    if(c.init_task.id) {
        cancelled=c.scheduler&&c.scheduler->cancel?c.scheduler->cancel(c.scheduler->user,c.init_task):AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE,0,{}};
        if(cancelled.code==0||cancelled.code==ANOMALY_STATUS_V1_CANCELLED||cancelled.code==ANOMALY_STATUS_V1_NOT_FOUND){c.init_task={};cancelled=Ok();}
    }
    const auto unsubscribed=Unsubscribe(c);
    c.command.store({});c.selection.store({});c.paid_target.reset();c.gate.Reset();c.prepared=c.init_requested=false;
    c.round.Reset();c.scope={};c.scan_scope={};c.candidate={};c.game_data={};c.scan=c.scan_level=0;c.next_data=0;c.last_world=0;
    c.next_poll=c.next_scan=c.next_services=c.next_view=0;
    c.server_fn=c.client_fn=c.open_fn=0;c.engine.registry=c.engine.world_slot=0;
    c.engine.functions.clear();c.engine.fields.clear();
    return cancelled.code!=0?cancelled:unsubscribed;
}
void Deactivate(Context& c,const std::string& status) {
    SetDetection(c,false);c.applied_revision=c.revision.load();
    static_cast<void>(ResetActivation(c));Publish(c,status);
}
void ANOMALY_CALL Initialize(void* user,Handle) noexcept {
    auto& work=*static_cast<InitWork*>(user);auto& c=*work.context;
    try {
        if(work.cancelled.load()||!c.running.load()||!c.detection.load()||work.serial!=c.init_serial.load())return;
        // Only the host Worker touches signature learning/scanning. The Game
        // Engine and its caches are not shared with this callback.
        ue::Engine engine;engine.api=work.api;engine.core=work.core;engine.profile=work.profile;
        const bool valid=engine.Signatures();
        if(work.cancelled.load()||!c.running.load()||!c.detection.load()||work.serial!=c.init_serial.load())return;
        c.initialized.store(std::make_shared<InitResult>(InitResult{work.serial,valid?engine.registry:0,valid?engine.world_slot:0}));
    }catch(...) {
        c.init_failed=work.serial;
    }
}
bool ScheduleInitialization(Context& c) {
    if(c.init_requested)return true;
    if(!c.scheduler||!c.scheduler->schedule||!c.scheduler->cancel)return false;
    auto work=std::make_unique<InitWork>();work->context=&c;work->api=c.engine.api;work->core=c.engine.core;work->profile=c.engine.profile;work->serial=c.init_serial.load();
    auto* user=work.get();c.init_work.push_back(std::move(work));c.init_requested=true;
    // callback_user remains alive until Unload, after the host drains scoped
    // Worker callbacks. Cancellation also rejects any late completion by serial.
    if(c.scheduler->schedule(c.scheduler->user,0,Initialize,user,&c.init_task).code!=0) {
        user->cancelled=true;c.init_task={};return false;
    }
    return true;
}
void Execute(Context& c,const Command& command,std::uint64_t now=ue::Now()) {
    auto& e=c.engine;
    if(!c.running.load()||!c.detection.load()||(command.expires&&now>=command.expires)||command.revision!=c.applied_revision||command.revision!=c.revision.load()||
       !c.prepared||!c.subscription.id)return;
    const auto sample=Readiness(e);
    if(!c.gate.Same(sample)||sample.sequence<c.gate.sequence||!e.Features()||
       !c.round.CanSubmit(command.identity,command.round)||!Bound(c,command.identity))return;
    Scope current;
    if(!BaseScope(e,current)||!MachineScope(c,current)||!current.operation||current.identity!=command.identity)return;
    // Re-read actual membership and serial numbers immediately before dispatch.
    const auto items=Items(c,current);
    const auto selected=std::find_if(items.begin(),items.end(),[&](const Item& i){return i.doll==command.doll&&ue::Same(i.actor,command.actor);});
    if(selected==items.end()||!e.Resolve(command.actor)||!ResolveFunctions(c))return;
    claw::Request request(current.machine_id,selected->id);
    if(!request.Valid()||!c.round.Submit(command.identity,command.round,selected->id,now))return;
    c.scope=std::move(current);
    if(!c.running.load()||!ue::Invoke(e,c.scope.player,c.server_fn,&request.rpc))c.round.state=claw::State::Unconfirmed;
    c.next_view=0;
}
void ReleaseWindow(Context& c) {
    if(c.window.id&&c.windows&&c.windows->release_window&&c.windows->release_window(c.windows->user,c.window).code==0)c.window={};
}
bool EnsureWindow(Context& c) {
    if(c.window.id)return true;
    if(!c.windows||!c.windows->register_window||!c.windows->begin||!c.windows->end||!c.windows->state||!c.windows->release_window)return false;
    AnomalyWindowSpecV1 spec{};spec.struct_size=sizeof(spec);spec.id=StringView("autoclaw-main");
    const auto title=c.localizer.Text("window.title","AutoClaw");spec.title=StringView(title);
    spec.initial_width=560;spec.initial_height=480;spec.minimum_width=380;spec.minimum_height=220;spec.maximum_width=1100;spec.maximum_height=1000;spec.default_open=1;
    return c.windows->register_window(c.windows->user,&spec,&c.window).code==0&&c.window.id;
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host,void** out) noexcept {
    if(!host||!out)return {ANOMALY_STATUS_V1_INVALID_ARGUMENT,0,{}};
    *out=nullptr;
    try {
        auto c=std::make_unique<Context>();c->engine.api=host;c->localizer=anomaly::sdk::Localizer(host);
        c->engine.profile=claw::compat::Load(host);
        if(!c->engine.profile) {
            static thread_local std::string message;
            message=c->localizer.Text("error.profile","The claw-machine compatibility profile is unavailable or incompatible. Detection was not started.");
            return {ANOMALY_STATUS_V1_UNAVAILABLE,0,StringView(message)};
        }
        c->engine.core=c->engine.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);
        c->windows=c->engine.Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID);
        c->scheduler=c->engine.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
        if(!c->engine.core)return {ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
        *out=c.release();return Ok();
    }catch(...){return {ANOMALY_STATUS_V1_FAILED,0,{}};}
}
AnomalyStatusV1 ANOMALY_CALL Start(void* user) noexcept {
    if(!user)return {ANOMALY_STATUS_V1_INVALID_ARGUMENT,0,{}};
    auto& c=*static_cast<Context*>(user);
    try {
        SetDetection(c,false);c.applied_revision=c.revision.load();
        const auto reset=ResetActivation(c);if(reset.code!=0)return reset;
        c.view.store({});c.fallback_open=1;c.selected=0;c.selected_scope={};
        c.running=true;static_cast<void>(EnsureWindow(c));
        Publish(c,c.localizer.Text("status.idle","Detection is off. Enter the game, then enable claw-machine detection here."));
        return Ok();
    }catch(...){c.running=false;return {ANOMALY_STATUS_V1_FAILED,0,{}};}
}
AnomalyStatusV1 ANOMALY_CALL Stop(void* user,std::uint32_t) noexcept {
    if(!user)return Ok();auto& c=*static_cast<Context*>(user);c.running=false;SetDetection(c,false);
    const auto reset=ResetActivation(c);ReleaseWindow(c);return reset;
}
void ANOMALY_CALL Unload(void* user) noexcept {
    if(!user)return;auto& c=*static_cast<Context*>(user);
    if(Stop(user,0).code!=0)return;
    delete &c;
}
void Tick(Context& c,std::uint64_t now) {
        auto& e=c.engine;
        if(!c.running.load())return;
        if(c.applied_revision!=c.revision.load()) {
            c.applied_revision=c.revision.load();
            if(ResetActivation(c).code!=0) {
                Deactivate(c,c.localizer.Text("status.cleanup","A previous callback could not be released. Disable and reload the plugin."));return;
            }
            Publish(c,c.detection.load()?c.localizer.Text("status.player","Waiting for stable local player and world snapshots."):
                c.localizer.Text("status.idle","Detection is off. Enter the game, then enable claw-machine detection here."));
        }
        // A restored plugin never touches UE objects, hooks or signatures until
        // the user explicitly enables detection in this process.
        if(!c.detection.load())return;
        if(now>=c.next_services) {
            c.next_services=now+2000;e.Refresh();c.callback_framework=e.framework;
            if(!c.init_task.id)c.scheduler=e.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
        }
        if(!e.framework||!e.framework->is_game_thread||!e.framework->is_game_thread(e.framework->user))return;
        if(now<c.next_scan)return;c.next_scan=now+33;
        const auto sample=Readiness(e);
        const bool same=c.gate.Same(sample),ready=c.gate.Observe(sample,now);
        if((c.prepared||c.init_requested)&&(!same||!ready)) {
            Deactivate(c,c.localizer.Text("status.scene","Player or scene changed. Detection was stopped; enable it again after loading finishes."));return;
        }
        if(!ready) {
            if(now>=c.next_view){c.next_view=now+2000;Publish(c,c.localizer.Text("status.player","Waiting for stable local player and world snapshots."));}return;
        }
        if(const auto result=c.initialized.exchange({});result&&result->serial==c.init_serial.load()) {
            c.init_task={};
            if(!result->registry||!result->world_slot) {
                Deactivate(c,c.localizer.Text("status.signature","Object or world signature unavailable. Enable detection again after the framework is ready."));return;
            }
            e.registry=result->registry;e.world_slot=result->world_slot;c.prepared=true;c.next_poll=0;
        }
        if(c.init_failed.load()==c.init_serial.load()) {
            Deactivate(c,c.localizer.Text("status.signature","Object or world signature unavailable. Enable detection again after the framework is ready."));return;
        }
        if(!c.prepared) {
            if(!ScheduleInitialization(c)) {
                Deactivate(c,c.localizer.Text("status.scheduler","The initialization worker is unavailable. Reload the plugin after the framework is ready."));return;
            }
            if(now>=c.next_view){c.next_view=now+2000;Publish(c,c.localizer.Text("status.initializing","Preparing claw-machine detection in the background."));}return;
        }
        if(!e.names||!e.objects||!e.worlds||!e.events||!e.events->subscribe||!e.events->unsubscribe||!e.events->invoke||!e.Features()) {
            if(now>=c.next_view){c.next_view=now+2000;Publish(c,c.localizer.Text("status.services","Waiting for game reflection and ProcessEvent services."));}return;
        }
        if(now>=c.next_poll) {
            c.next_poll=now+250;Scope scope;
            if(!BaseScope(e,scope)||scope.world_epoch.id!=sample.identity[0]||scope.world_epoch.generation!=sample.identity[1]) {
                Deactivate(c,c.localizer.Text("status.scene","Player or scene changed. Detection was stopped; enable it again after loading finishes."));return;
            }
            const auto world_handle=e.Handle(scope.world);
            if((c.last_world&&c.last_world!=world_handle.id)||(c.scan_scope.player&&c.scan_scope.player!=scope.player)||
               (c.scan_scope.world_epoch.id&&!ue::Same(c.scan_scope.world_epoch,scope.world_epoch))) {
                Deactivate(c,c.localizer.Text("status.scene","Player or scene changed. Detection was stopped; enable it again after loading finishes."));return;
            }
            c.last_world=world_handle.id;c.scan_scope=scope;
            if(scope.main||scope.operation) {
                if(!c.subscription.id) {
                    if(!ResolveFunctions(c)||e.events->subscribe(e.events->user,Observe,&c,&c.subscription).code!=0||!c.subscription.id) {
                        Deactivate(c,c.localizer.Text("status.layout","The claw-machine interface did not match. Actions are unavailable."));return;
                    }
                    c.event_owner=e.events;
                }
            } else {
                c.scope.main=c.scope.operation=0;
                const auto state=c.round.state;
                if(state!=claw::State::WaitingBegin&&state!=claw::State::Ready&&state!=claw::State::Pending)static_cast<void>(Unsubscribe(c));
            }
        }
        Scope scope=c.scan_scope;
        if(scope.main||scope.operation)Scout(c,scope);
        const bool active=(scope.main||scope.operation)&&MachineScope(c,scope);
        if(active) {
            if(c.round.identity.Valid()&&c.round.identity!=scope.identity) {
                c.round.Reset();c.command.store({});c.selection.store({});c.paid_target.reset();
            }
            c.scope=scope;
        } else c.scope.operation=c.scope.main=0;
        c.round.Tick(now);
        if(const auto command=c.command.load()) {
            if(command->expires&&now>=command->expires) {
                c.command.store({});if(c.round.state==claw::State::Ready)c.round.state=claw::State::Unconfirmed;c.next_view=0;
            } else if(active&&scope.operation) {
                c.command.store({});Execute(c,*command,now);
                if(c.round.state==claw::State::Ready)c.round.state=claw::State::Unconfirmed;c.next_view=0;
            }
        }
        if(now<c.next_view)return;c.next_view=now+500;
        if(active)Publish(c,RoundStatus(c),Items(c,c.scope),true);
        else Publish(c,c.round.state==claw::State::Pending?RoundStatus(c):
            (scope.main||scope.operation)?c.localizer.Text("status.machine","Reading the active claw machine. Please wait."):
            c.localizer.Text("status.open","Open a claw machine in the game. Wait for its item list before inserting a coin."));
}
void ANOMALY_CALL Update(void* user,double) noexcept {
    if(!user)return;auto& c=*static_cast<Context*>(user);
    try {Tick(c,ue::Now());}
    catch(...) {
        try {Deactivate(c,c.localizer.Text("status.scene","Player or scene changed. Detection was stopped; enable it again after loading finishes."));}
        catch(...) {SetDetection(c,false);}
    }
}
void Controls(Context& c,const AnomalyUiServiceV1* ui) {
    int enabled=c.detection.load()?1:0;
    if(ui->checkbox&&ui->checkbox(ui->user,StringView(c.localizer.Label("option.detection","Enable claw-machine detection","autoclaw-detection")),&enabled))SetDetection(c,enabled!=0);
    ui->text(ui->user,StringView(c.localizer.Text("instructions.flow","Enable detection, choose a doll, then insert a coin manually. Capture starts automatically after the game confirms the round.")));
    ui->text(ui->user,StringView(c.localizer.Text("instructions.result","Capture results and rewards follow the game settlement.")));
    const auto view=c.view.load();if(!view)return;
    ui->text(ui->user,StringView(view->status));
    if(!c.detection.load()||view->revision!=c.revision.load())return;
    if(!view->machine.empty())ui->text(ui->user,StringView(c.localizer.Format("label.machine","Machine: {0}",std::array<std::string_view,1>{view->machine})));
    if(!view->result.empty())ui->text(ui->user,StringView(view->result));
    if(c.selected_scope!=view->identity){c.selected_scope=view->identity;c.selected=0;}
    if(ui->separator)ui->separator(ui->user);
    if(view->items.empty())ui->text(ui->user,StringView(c.localizer.Text("label.no_items","No selectable dolls found in the active machine yet.")));
    const auto count=std::to_string(view->items.size());
    ui->text(ui->user,StringView(c.localizer.Format("label.count","Dolls in this machine: {0}",std::array<std::string_view,1>{count})));
    if(ui->checkbox)for(const auto& item:view->items) {
        int selected=c.selected==claw::Pack(item.doll)?1:0;
        const auto text=c.localizer.Format("item.choice","{0}",std::array<std::string_view,1>{item.label});
        const auto id=text+"###autoclaw-doll-"+std::to_string(claw::Pack(item.doll));
        if(ui->checkbox(ui->user,StringView(id),&selected)&&selected)c.selected=claw::Pack(item.doll);
    }
    const auto chosen=std::find_if(view->items.begin(),view->items.end(),[&](const Item& i){return claw::Pack(i.doll)==c.selected;});
    if(chosen!=view->items.end())c.selection.store(std::make_shared<Command>(Command{view->identity,0,chosen->actor,chosen->doll,view->revision}));
}
void ANOMALY_CALL Draw(void* user,const AnomalyUiServiceV1* ui) noexcept {
    if(!user||!ui||!ui->text)return;auto& c=*static_cast<Context*>(user);
    try {
        if(EnsureWindow(c)) {
            AnomalyWindowStateV1 state{sizeof(state)};
            if(c.windows->state(c.windows->user,c.window,&state).code!=0||!state.open)return;
            std::int32_t visible{};if(c.windows->begin(c.windows->user,c.window,0,&visible).code!=0)return;
            try {if(visible)Controls(c,ui);}catch(...) {}
            static_cast<void>(c.windows->end(c.windows->user,c.window));return;
        }
        if(!c.fallback_open||!ui->begin_window||!ui->end_window)return;
        UiWindow window(ui,c.localizer.Text("window.title","AutoClaw"),&c.fallback_open);
        if(window)Controls(c,ui);
    }catch(...) {}
}
}
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if(!descriptor||descriptor->struct_size<sizeof(*descriptor))return {ANOMALY_STATUS_V1_INVALID_ARGUMENT,0,{}};
    *descriptor={sizeof(*descriptor),ANOMALY_PLUGIN_API_V1_MAJOR,ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("b1ank.autoclaw"),StringView("AutoClaw"),StringView("b1ank"),StringView("0.1.6"),Load,Start,Stop,Unload,Update,Draw};
    return Ok();
}
