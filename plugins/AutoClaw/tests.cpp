#include "profile_test_setup.hpp"
#include "plugin.cpp"
#include <cstdlib>
#include <iostream>
#include <thread>

namespace fixture {
void Check(bool value,const char* message) {if(!value){std::cerr<<message<<'\n';std::exit(1);}}
AnomalyStatusV1 Missing(){return {ANOMALY_STATUS_V1_NOT_FOUND,0,{}};}
AnomalyStatusV1 ReadMemory(void*,std::uintptr_t address,AnomalyMutableByteSpanV1 out) {
    __try {std::memcpy(out.data,reinterpret_cast<const void*>(address),out.size);return Ok();}
    __except(EXCEPTION_EXECUTE_HANDLER){return Missing();}
}
struct Game {
    std::vector<std::vector<std::uint8_t>> storage;
    std::vector<std::uintptr_t> objects;
    std::map<std::uint32_t,std::string> strings;
    std::map<std::string,std::uint32_t> ids;
    std::map<std::string,Handle> paths;
    std::map<std::wstring,std::uintptr_t> widget_names;
    anomaly::testing::ProfileHost profile_host{"AutoClaw"};
    AnomalyHostApiV1 host{};
    AnomalyCoreServiceV1 core{sizeof(core),1,this};
    AnomalyUe5NamesServiceV1 names{sizeof(names),1,this};
    AnomalyUe5ObjectsServiceV1 object_service{sizeof(object_service),1,this};
    AnomalyUe5WorldServiceV1 world_service{sizeof(world_service),1,this};
    AnomalyUe5FrameworkServiceV1 framework{sizeof(framework),1,this};
    AnomalyUe5BuildServiceV1 build{sizeof(build),1,this};
    AnomalyUe5ProcessEventServiceV1 events{sizeof(events),1,this};
    AnomalySignatureServiceV1 signatures{sizeof(signatures),1,this};
    AnomalySchedulerServiceV1 scheduler{sizeof(scheduler),1,this};
    AnomalyNteSessionServiceV1 session{sizeof(session),1,this};
    AnomalyNtePlayerServiceV1 player_service{sizeof(player_service),1,this};
    struct Task {Handle handle{};AnomalyTaskCallbackV1 callback{};void* user{};bool cancelled{},ran{};};
    std::vector<Task> tasks;
    std::uintptr_t registry{},chunk{},world{},level{},player{},controller{},hud{},machine{},panel{},panel_class{},doll_a{},doll_b{},doll_array{};
    std::uintptr_t get_ps{},get_pc{},get_ui{},get_owner{},server{},client{};
    std::uintptr_t current_world{};
    std::uint64_t world_epoch{77};
    std::uint64_t player_epoch{88},snapshot_sequence{100};
    std::uint32_t session_state{ANOMALY_NTE_SESSION_V1_WORLD_READY},player_flags{ANOMALY_NTE_SNAPSHOT_V1_VALID};
    bool worker{},reject_invoke{};
    std::uintptr_t text_address{};
    claw::Name a{},b{};
    Context* observed{};
    unsigned requests{},unsubscribed{},translations{},signature_calls{},reads{},object_counts{},snapshots{},finds{},native_calls{},player_queries{},subscriptions{},scheduled{},cancelled{},game_queries{},semantic_calls{},thread_queries{};
    std::string requested_machine,requested_doll;

    std::uintptr_t Buffer(std::size_t size){storage.emplace_back(size);return reinterpret_cast<std::uintptr_t>(storage.back().data());}
    void Put(std::uintptr_t at,const auto& value){std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(value));}
    claw::Name Name(const std::string& text) {
        if(const auto it=ids.find(text);it!=ids.end())return {it->second,0};
        const auto id=static_cast<std::uint32_t>(strings.size()+1);strings[id]=text;ids[text]=id;return {id,0};
    }
    Handle H(std::uint32_t index)const{return {(std::uint64_t{7}<<32)|(std::uint64_t{index}+1),11};}
    Handle H(std::uintptr_t object)const{int index{};std::memcpy(&index,reinterpret_cast<void*>(object+12),4);return H(static_cast<std::uint32_t>(index));}
    void Register(std::uintptr_t object) {
        const auto index=static_cast<std::uint32_t>(objects.size());objects.push_back(object);
        Put(object+12,static_cast<std::int32_t>(index));Put(chunk+24*index,object);Put(chunk+24*index+16,std::uint32_t{7});Put(registry+36,static_cast<std::uint32_t>(objects.size()));
    }
    std::pair<std::uintptr_t,std::uintptr_t> Object(const char* type,std::size_t size,const std::string& path={}) {
        const auto cls=Buffer(128);Register(cls);Put(cls+24,Name(type));
        const auto object=Buffer(size);Register(object);Put(object+16,cls);
        if(!path.empty())paths[path]=H(object);
        return {object,cls};
    }
    std::uintptr_t Property(std::uintptr_t cls,const char* name,int offset,int size,const char* kind="ObjectProperty") {
        const auto p=Buffer(80);std::uintptr_t previous{};std::memcpy(&previous,reinterpret_cast<void*>(cls+112),8);
        const auto field_class=Buffer(40);Put(field_class,Name(kind));Put(p+8,field_class);
        Put(p+32,Name(name));Put(p+52,size);Put(p+68,offset);Put(p+72,previous);Put(cls+112,p);return p;
    }
    std::uintptr_t Function(const char* package,const char* owner,const char* name,int count,int size,std::initializer_list<ue::Parameter> parameters,std::uint32_t flags=0x400) {
        const auto [fn,cls]=Object("Function",224,std::string("/Script/")+package+"."+owner+"."+name);
        static_cast<void>(cls);Put(fn+24,Name(name));Put(fn+176,flags);Put(fn+180,static_cast<std::uint8_t>(count));Put(fn+182,static_cast<std::uint16_t>(size));
        for(const auto& p:parameters)Property(fn,p.name,p.offset,p.size,p.kind);return fn;
    }
    Game() {
        PrepareClawProfile(profile_host);registry=Buffer(512);
        const auto recorded=RecordedClawCode();Put(registry,recorded);
        chunk=Buffer(24*2048);const auto chunks=Buffer(8);Put(registry+16,chunks);Put(chunks,chunk);
        core.read_memory=[](void* user,std::uintptr_t at,AnomalyMutableByteSpanV1 out){
            auto& f=*static_cast<Game*>(user);++f.reads;
            return ReadMemory(user,at,out);
        };
        names.resolve_utf8=[](void* user,std::uint32_t id,char* out,std::size_t* size){
            auto& f=*static_cast<Game*>(user);const auto it=f.strings.find(id);if(it==f.strings.end())return Missing();
            const auto bytes=it->second.size()+1;if(!out||*size<bytes){*size=bytes;return AnomalyStatusV1{ANOMALY_STATUS_V1_BUFFER_TOO_SMALL,0,{}};}
            std::memcpy(out,it->second.c_str(),bytes);*size=bytes;return Ok();
        };
        object_service.count=[](void* user){auto& f=*static_cast<Game*>(user);++f.object_counts;return static_cast<std::uint32_t>(f.objects.size());};
        object_service.snapshot_at=[](void* user,std::uint32_t index,AnomalyUe5ObjectSnapshotV1* out){auto& f=*static_cast<Game*>(user);++f.snapshots;if(index>=f.objects.size())return Missing();out->handle=f.H(index);return Ok();};
        object_service.snapshot_by_handle=[](void* user,Handle h,AnomalyUe5ObjectSnapshotV1* out){
            auto& f=*static_cast<Game*>(user);++f.snapshots;const auto index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(h);
            if(index>=f.objects.size()||!ue::Same(h,f.H(index)))return Missing();out->handle=h;return Ok();
        };
        object_service.find_exact=[](void* user,AnomalyStringViewV1 name,Handle* out){
            auto& f=*static_cast<Game*>(user);++f.finds;const auto it=f.paths.find(std::string(name.data,name.size));if(it==f.paths.end())return Missing();*out=it->second;return Ok();
        };
        // Real host contract: world-service tokens are independent of object
        // index/serial handles, even when they describe the same UE world.
        world_service.current=[](void* user,Handle* out){auto& f=*static_cast<Game*>(user);if(!f.current_world)return Missing();*out={1,f.world_epoch};return Ok();};
        world_service.snapshot=[](void* user,Handle h,AnomalyUe5WorldSnapshotV1* out){
            auto& f=*static_cast<Game*>(user);if(!f.current_world||h.id!=1||h.generation!=f.world_epoch)return Missing();
            out->handle=h;out->change_sequence=f.world_epoch;std::uint32_t id{};std::memcpy(&id,reinterpret_cast<void*>(f.current_world+24),4);out->name_id=id;return Ok();
        };
        framework.is_game_thread=[](void* user){auto& f=*static_cast<Game*>(user);++f.thread_queries;return f.worker?0:1;};
        build.feature_state=[](void*,AnomalyStringViewV1){return static_cast<std::uint32_t>(ANOMALY_FEATURE_V1_AVAILABLE);};
        events.invoke=[](void* user,Handle receiver,Handle function,void* parameters,std::size_t size) {
            auto& f=*static_cast<Game*>(user);
            Check(!f.worker,"native invocation ran on the Worker");
            const auto receiver_index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(receiver),function_index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(function);
            if(f.reject_invoke||receiver_index>=f.objects.size()||function_index>=f.objects.size()||
                !ue::Same(receiver,f.H(receiver_index))||!ue::Same(function,f.H(function_index)))return Missing();
            std::uint16_t expected{};std::memcpy(&expected,reinterpret_cast<void*>(f.objects[function_index]+182),sizeof(expected));
            Check(size==expected&&(!size||parameters),"public ProcessEvent invocation received an invalid parameter capacity");
            f.Call(f.objects[receiver_index],f.objects[function_index],parameters);return Ok();
        };
        events.subscribe=[](void* user,AnomalyUe5ProcessEventCallbackV1,void* observer,Handle* h){auto& f=*static_cast<Game*>(user);++f.subscriptions;f.observed=static_cast<Context*>(observer);*h={9,1};return Ok();};
        events.unsubscribe=[](void* user,Handle h){auto& f=*static_cast<Game*>(user);Check(h.id==9,"unsubscribe used a different service's handle");++f.unsubscribed;f.observed=nullptr;return Ok();};
        scheduler.schedule=[](void* user,std::uint32_t,AnomalyTaskCallbackV1 callback,void* data,Handle* h) {
            auto& f=*static_cast<Game*>(user);*h={++f.scheduled,1};f.tasks.push_back({*h,callback,data});return Ok();
        };
        scheduler.cancel=[](void* user,Handle h) {
            auto& f=*static_cast<Game*>(user);
            for(auto& task:f.tasks)if(ue::Same(task.handle,h)&&!task.ran&&!task.cancelled) {
                task.cancelled=true;++f.cancelled;return AnomalyStatusV1{ANOMALY_STATUS_V1_CANCELLED,0,{}};
            }
            return Missing();
        };
        session.snapshot=[](void* user,AnomalyNteSessionSnapshotV1* out) {
            auto& f=*static_cast<Game*>(user);++f.semantic_calls;out->state=f.session_state;out->sequence=f.world_epoch;out->world={1,f.world_epoch};return Ok();
        };
        player_service.snapshot=[](void* user,AnomalyNtePlayerSnapshotV1* out) {
            auto& f=*static_cast<Game*>(user);++f.semantic_calls;out->flags=f.player_flags;out->sequence=f.snapshot_sequence;out->handle={2,f.player_epoch};return Ok();
        };
        signatures.resolve=[](void* user,AnomalyStringViewV1 module,AnomalyStringViewV1 section,AnomalyStringViewV1 pattern,std::uintptr_t* out){
            auto& f=*static_cast<Game*>(user);++f.signature_calls;
            Check(f.worker,"signature scanning ran outside the host Worker");
            Check(std::string_view(module.data,module.size)=="HTGame.exe"&&std::string_view(section.data,section.size)==".text","wrong startup signature domain");
            try {
                const auto hits=ue5mem::Pattern::Parse({pattern.data,pattern.size}).FindAll(
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(f.registry),512),2);
                if(hits.size()!=1)return Missing();*out=f.registry+hits[0];return Ok();
            }catch(...){return Missing();}
        };
        host.struct_size=sizeof(host);host.api_major=ANOMALY_PLUGIN_API_V1_MAJOR;host.api_minor=ANOMALY_PLUGIN_API_V1_MINOR;host.host_context=this;
        host.query_service=[](void* user,AnomalyStringViewV1 key,std::uint32_t,const void** out){
            auto& f=*static_cast<Game*>(user);const std::string_view id(key.data,key.size);*out=nullptr;
            if(id.starts_with("anomaly.ue5.")||id.starts_with("anomaly.nte.")||id==ANOMALY_SIGNATURE_SERVICE_V1_ID)++f.game_queries;
            if(id==ANOMALY_CORE_SERVICE_V1_ID)*out=&f.core;
            else if(id==ANOMALY_UE5_NAMES_SERVICE_V1_ID)*out=&f.names;
            else if(id==ANOMALY_UE5_OBJECTS_SERVICE_V1_ID)*out=&f.object_service;
            else if(id==ANOMALY_UE5_WORLD_SERVICE_V1_ID)*out=&f.world_service;
            else if(id==ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID)*out=&f.framework;
            else if(id==ANOMALY_UE5_BUILD_SERVICE_V1_ID)*out=&f.build;
            else if(id==ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID)*out=&f.events;
            else if(id==ANOMALY_SIGNATURE_SERVICE_V1_ID)*out=&f.signatures;
            else if(id==ANOMALY_SCHEDULER_SERVICE_V1_ID)*out=&f.scheduler;
            else if(id==ANOMALY_NTE_SESSION_SERVICE_V1_ID)*out=&f.session;
            else if(id==ANOMALY_NTE_PLAYER_SERVICE_V1_ID)*out=&f.player_service;
            else if(id==ANOMALY_LOCALIZATION_SERVICE_V1_ID)++f.translations;
            else if(id==ANOMALY_PROFILE_SERVICE_V1_ID||id==ANOMALY_JSON_SERVICE_V1_ID)
                return anomaly::testing::ProfileHost::Query(&f.profile_host,key,1,out);
            return *out?Ok():Missing();
        };
        const auto [w,wc]=Object("World",128);world=current_world=w;static_cast<void>(wc);Put(w+24,Name("FixtureWorld"));Put(registry+64,w);
        const auto [l,lc]=Object("Level",192);level=l;Property(lc,"OwningWorld",64,8);Put(l+64,w);
        const auto [ps,psc]=Object("HTPlayerState",128);player=ps;static_cast<void>(psc);Put(ps+32,l);
        const auto [pc,pcc]=Object("HTPlayerController",128);controller=pc;Put(pc+32,l);Property(pcc,"PlayerState",64,8);Put(pc+64,ps);
        const auto [h,hc]=Object("HTHUD",192);hud=h;Property(hc,"PlayerOwner",64,8);Put(h+64,pc);Property(hc,"ActivedWidgetMap",80,80,"MapProperty");
        Property(pcc,"MyHUD",72,8);Put(pc+72,h);
        const auto [p,cl]=Object("HTUI_ClawMachine",128);panel=p;panel_class=cl;widget_names[L"ClawMachine"]=panel;Property(cl,"bIsActive",64,1,"BoolProperty");Property(cl,"Visibility",65,1,"EnumProperty");Put(p+64,std::uint8_t{1});
        const auto slots=Buffer(24);Put(slots,panel_class);Put(slots+8,Name("ClawMachine"));
        Put(h+80,claw::Array{slots,1,1});Put(h+96,std::uint32_t{1});Put(h+120,std::int32_t{1});Put(h+124,std::int32_t{128});
        const auto [m,mc]=Object("HTClawMachineActor",160);machine=m;Put(m+32,l);Property(mc,"machineID",80,8,"NameProperty");Put(m+80,Name("Machine_01"));
        Property(mc,"bIsInteracting",88,1,"BoolProperty");Put(m+88,std::uint8_t{1});Property(mc,"DollActors",96,16,"ArrayProperty");
        const auto [da,dac]=Object("HTDollActor",128);doll_a=da;Put(da+32,l);Property(dac,"DollID",64,8,"NameProperty");a=Name("Doll_A");Put(da+64,a);
        const auto [db,dbc]=Object("HTDollActor",128);doll_b=db;Put(db+32,l);Property(dbc,"DollID",64,8,"NameProperty");b=Name("Doll_B");Put(db+64,b);
        const auto levels=Buffer(8);Put(levels,level);Property(wc,"Levels",64,16,"ArrayProperty");Put(world+64,claw::Array{levels,1,1});
        const auto actors=Buffer(8);Put(actors,machine);Put(level+160,claw::Array{actors,1,1});
        doll_array=Buffer(16);Put(doll_array,da);Put(doll_array+8,db);Put(m+96,claw::Array{doll_array,2,2});
        Object("HTUtil",64,"/Script/HTGame.Default__HTUtil");
        get_ps=Function("HTGame","HTUtil","GetMainPlayerState",2,16,{{"WorldContextObject",0,8,"ObjectProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
        get_pc=Function("HTGame","HTUtil","GetMainPlayerController",2,16,{{"WorldContextObject",0,8,"ObjectProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
        get_ui=Function("HTGame","HTHUD","BP_GetUIByClass",2,24,{{"UIClass",0,16,"StrProperty"},{"ReturnValue",16,8,"ObjectProperty"}});
        get_owner=Function("UMG","Widget","GetOwningPlayer",1,8,{{"ReturnValue",0,8,"ObjectProperty"}});
        server=Function("HTGame","HTPlayerState","ServerClawMachineRPC",2,32,{{"MsgType",0,4,"IntProperty"},{"WrapperArray",8,24,"StructProperty"}},0x00200440);
        client=Function("HTGame","HTPlayerState","ClientClawMachineRPC",2,32,{{"MsgType",0,4,"IntProperty"},{"WrapperArray",8,24,"StructProperty"}},0x01000440);
        Function("HTGame","HTClawMachineActor","OpenClawMachine",0,0,{});
        const auto [wrapper,ct]=Object("ScriptStruct",128,"/Script/HottaFramework.ParameterWrapper");static_cast<void>(ct);
        Property(wrapper,"TypeInfo",0,12,"StructProperty");Property(wrapper,"BinaryData",16,16,"ArrayProperty");
        const auto [array,at]=Object("ScriptStruct",128,"/Script/HottaFramework.ParameterWrapperArray");static_cast<void>(at);
        Property(array,"ParameterWrappers",0,16,"ArrayProperty");Property(array,"ReadParameterIndex",16,4,"IntProperty");
        const auto [type,tt]=Object("ScriptStruct",128,"/Script/HottaFramework.ParameterTypeInfo");static_cast<void>(tt);
        Property(type,"Type",0,1,"EnumProperty");Property(type,"CustomTypeName",4,8,"NameProperty");
    }
    void Bind(Context& c) {
        c.engine.api=&host;c.engine.profile=claw::compat::Load(&host);Check(static_cast<bool>(c.engine.profile),"test Profile did not load");c.engine.Refresh();c.engine.registry=registry;c.engine.world_slot=registry+64;c.localizer=anomaly::sdk::Localizer(&host);c.running=c.detection=true;c.prepared=true;c.init_serial=1;
        c.callback_framework=&framework;c.scheduler=&scheduler;c.gate.identity={1,world_epoch,2,player_epoch};c.gate.observed=true;c.gate.sequence=snapshot_sequence;
        c.gate.progress=ue::Now();c.gate.since=c.gate.progress-2000;
        c.candidate=H(machine);c.subscription={9,1};c.event_owner=&events;
        Check(ResolveFunctions(c),"reflected RPC contract did not match");
        Check(BaseScope(c.engine,c.scope)&&MachineScope(c,c.scope),"local player, UI or machine was not bound");observed=&c;
    }
    void Begin(Context& c) {
        claw::Request request("Machine_01","Doll_A");request.rpc.message=0;request.rpc.parameters.items.count=request.rpc.parameters.items.capacity=1;
        Observe(&c,player,server,&request.rpc);Check(c.round.state==claw::State::WaitingBegin,"paid request not observed");
        Confirm(c);Check(c.round.state==claw::State::Ready,"server round confirmation not observed");
    }
    void Confirm(Context& c) {
        const std::int32_t state=1;claw::Wrapper wrapper{};wrapper.type=6;wrapper.bytes={reinterpret_cast<std::uintptr_t>(&state),4,4};
        claw::Rpc reply{};reply.parameters.items={reinterpret_cast<std::uintptr_t>(&wrapper),1,1};Observe(&c,player,client,&reply);
    }
    Command Choose(Context& c){return {c.scope.identity,c.round.sequence,H(doll_b),b,c.applied_revision};}
    void RunScheduled(bool late=false) {
        for(auto& task:tasks)if(!task.ran&&(!task.cancelled||late)) {
            task.ran=true;std::thread run([&]{worker=true;task.callback(task.user,task.handle);worker=false;});run.join();
        }
    }
    void Reply(Context& c,bool failed,const std::string& doll="Doll_B") {
        const std::int32_t flag=failed?1:0;
        auto doll_bytes=claw::EncodeName(doll),item_bytes=claw::EncodeName("Item_Confirmed");
        std::array<claw::Wrapper,3> wrappers{};wrappers[0].type=1;wrappers[0].bytes={reinterpret_cast<std::uintptr_t>(&flag),4,4};
        wrappers[1].type=15;wrappers[1].bytes={reinterpret_cast<std::uintptr_t>(doll_bytes.data()),static_cast<std::int32_t>(doll_bytes.size()),static_cast<std::int32_t>(doll_bytes.size())};
        wrappers[2].type=15;wrappers[2].bytes={reinterpret_cast<std::uintptr_t>(item_bytes.data()),static_cast<std::int32_t>(item_bytes.size()),static_cast<std::int32_t>(item_bytes.size())};
        claw::Rpc reply{1};reply.parameters.items={reinterpret_cast<std::uintptr_t>(wrappers.data()),3,3};Observe(&c,player,client,&reply);
    }
    void Call(std::uintptr_t object,std::uintptr_t fn,void* parameters) {
        ++native_calls;if(fn==get_ps)++player_queries;
        if(observed)Observe(observed,object,fn,parameters);
        if(fn==get_ps)Put(reinterpret_cast<std::uintptr_t>(parameters)+8,player);
        else if(fn==get_pc)Put(reinterpret_cast<std::uintptr_t>(parameters)+8,controller);
        else if(fn==get_ui) {
            const auto& key=*static_cast<const claw::Array*>(parameters);
            const std::wstring label(reinterpret_cast<const wchar_t*>(key.data),static_cast<std::size_t>(key.count-1));
            const auto found=widget_names.find(label);Put(reinterpret_cast<std::uintptr_t>(parameters)+16,found==widget_names.end()?std::uintptr_t{}:found->second);
        }
        else if(fn==get_owner)Put(reinterpret_cast<std::uintptr_t>(parameters),controller);
        else if(fn==server) {
            const auto& rpc=*static_cast<claw::Rpc*>(parameters);Check(rpc.message==1&&rpc.parameters.items.count==2,"wrong capture RPC shape");
            const auto* w=reinterpret_cast<const claw::Wrapper*>(rpc.parameters.items.data);
            Check(w[0].type==15&&w[1].type==15&&w[0].custom_type==claw::Name{}&&w[1].custom_type==claw::Name{},"wrong Name wrapper type");
            const auto* m=reinterpret_cast<const std::uint8_t*>(w[0].bytes.data);const auto* d=reinterpret_cast<const std::uint8_t*>(w[1].bytes.data);
            // An independent wire check: positive FString length, exact ASCII
            // spelling including NUL, and machine-first ordering.
            std::int32_t ml{},dl{};std::memcpy(&ml,m,4);std::memcpy(&dl,d,4);
            Check(ml==11&&w[0].bytes.count==15&&std::memcmp(m+4,"Machine_01",11)==0,"machine wire bytes differ from native FString contract");
            Check(dl==7&&w[1].bytes.count==11&&std::memcmp(d+4,"Doll_B",7)==0,"selected doll wire bytes differ from native FString contract");
            requested_machine=reinterpret_cast<const char*>(m+4);requested_doll=reinterpret_cast<const char*>(d+4);++requests;
        }
    }
};

void ReflectedRowsAndTypes() {
    Game f;Context c;f.Bind(c);
    f.Property(f.panel_class,"bIsActive",64,1,"ByteProperty");c.engine.fields.clear();
    Check(!Active(c.engine,f.panel,"HTUI_ClawMachine"),"same-sized non-boolean active field was accepted");
    f.Property(f.panel_class,"bIsActive",64,1,"BoolProperty");c.engine.fields.clear();
    Check(Active(c.engine,f.panel,"HTUI_ClawMachine"),"restored valid property could not be resolved");
    f.Property(f.server,"MsgType",0,4,"FloatProperty");
    Check(!ResolveFunctions(c)&&!f.requests,"same-sized wrong RPC parameter type was accepted");
    f.Property(f.server,"MsgType",0,4,"IntProperty");Check(ResolveFunctions(c),"valid RPC type did not recover");
    claw::Request request("Machine_01","Doll_A");const auto calls=f.native_calls;f.reject_invoke=true;
    Check(!ue::Invoke(c.engine,f.player,f.server,&request.rpc)&&f.native_calls==calls,"host rejection was bypassed by a raw native call");
    f.reject_invoke=false;
    const auto invoke=f.events.invoke;f.events.invoke=nullptr;
    Check(!ue::Invoke(c.engine,f.player,f.server,&request.rpc)&&f.native_calls==calls,"missing host invocation fell back to a raw virtual call");
    f.events.invoke=invoke;const auto service_size=f.events.struct_size;f.events.struct_size=offsetof(AnomalyUe5ProcessEventServiceV1,invoke);
    Check(!ue::Invoke(c.engine,f.player,f.server,&request.rpc)&&f.native_calls==calls,"truncated ProcessEvent table was accepted");
    f.events.struct_size=service_size;

    const auto [doll_type,doll_class]=f.Object("ScriptStruct",160);
    const auto [item_type,item_class]=f.Object("ScriptStruct",160);
    static_cast<void>(doll_class);static_cast<void>(item_class);
    f.Put(doll_type+24,f.Name("StaticDollData"));f.Put(item_type+24,f.Name("StaticItemData"));
    // Deliberately relocated reflected fields, independent of the old offsets.
    f.Property(doll_type,"ItemID",40,8,"NameProperty");f.Property(item_type,"ItemName",184,16,"TextProperty");
    const auto [dolls,dc]=f.Object("DataTable",160);f.Property(dc,"RowStruct",40,8);f.Put(dolls+40,doll_type);
    const auto [items,ic]=f.Object("DataTable",160);f.Property(ic,"RowStruct",40,8);f.Put(items+40,item_type);
    const auto item_id=f.Name("Item_For_Doll_B");const auto doll_row=f.Buffer(64),item_row=f.Buffer(224);
    f.Put(doll_row+40,item_id);f.text_address=item_row+184;
    const auto doll_slots=f.Buffer(48),item_slots=f.Buffer(24);
    f.Put(doll_slots,f.b);f.Put(doll_slots+8,std::uintptr_t{123}); // sparse hole must be skipped
    f.Put(doll_slots+24,f.b);f.Put(doll_slots+32,doll_row);f.Put(item_slots,item_id);f.Put(item_slots+8,item_row);
    f.Put(dolls+48,claw::Array{doll_slots,2,2});f.Put(dolls+64,std::uint32_t{2});f.Put(dolls+88,std::int32_t{2});f.Put(dolls+92,std::int32_t{128});
    f.Put(items+48,claw::Array{item_slots,1,1});f.Put(items+64,std::uint32_t{1});f.Put(items+88,std::int32_t{1});f.Put(items+92,std::int32_t{128});
    const auto [data,data_class]=f.Object("HTGameData",128);f.Property(data_class,"DollDataTable",64,8);f.Property(data_class,"ItemDataTable",72,8);
    f.Put(data+64,dolls);f.Put(data+72,items);c.game_data=f.H(data);
    f.names.resolve_ftext_utf8=[](void* user,std::uintptr_t address,char* output,std::size_t* size) {
        if(address!=static_cast<Game*>(user)->text_address)return Missing();
        constexpr char text[]="Recorded reward";
        if(!output||*size<sizeof(text)){*size=sizeof(text);return AnomalyStatusV1{ANOMALY_STATUS_V1_BUFFER_TOO_SMALL,0,{}};}
        std::memcpy(output,text,sizeof(text));*size=sizeof(text);return Ok();
    };
    const Item target{f.H(f.doll_b),f.b,"Doll_B",{}};
    Check(Row(c.engine,dolls,"StaticDollData",f.b)==doll_row,"sparse DataTable hole was interpreted as a row");
    Check(ItemLabel(c,target)=="Recorded reward (Doll_B)","item label retained a compiled row offset");
    f.Property(item_type,"ItemName",184,16,"ArrayProperty");
    Check(ItemLabel(c,target)=="Doll_B","wrong FText type did not fall back to the game row ID");
    Check(Stop(&c,0).code==0,"type fixture did not release its observer");
}
void SceneMachineLookup() {
    Game f;Context c;f.Bind(c);c.candidate={};
    const auto counts=f.object_counts;
    // Registry enumeration is unavailable: the active level still contains the
    // paid machine. Detection must not depend on an asset/object-table walk.
    f.object_service.count=[](void*)->std::uint32_t{Check(false,"machine search enumerated the global object registry");return 0;};
    f.object_service.snapshot_at=[](void* user,std::uint32_t index,AnomalyUe5ObjectSnapshotV1* out){
        auto& game=*static_cast<Game*>(user);if(index>=game.objects.size())return Missing();out->handle=game.H(index);return Ok();
    };
    Scout(c,c.scope);Check(ue::Same(c.candidate,f.H(f.machine))&&f.object_counts==counts,"active scene machine was not found");
    c.candidate={};f.Put(f.machine+88,std::uint8_t{0});Scout(c,c.scope);Check(!c.candidate.id,"non-interacting machine was selected");
    f.Put(f.machine+88,std::uint8_t{1});f.Put(f.level+64,std::uintptr_t{0});Scout(c,c.scope);Check(!c.candidate.id,"foreign level supplied a machine");
    Check(Stop(&c,0).code==0,"scene machine lookup leaked its observer");
}
void BatchedSceneLookup() {
    Game f;Context c;f.Bind(c);c.candidate={};
    const auto [sample,shared_class]=f.Object("OtherSceneActor",64);static_cast<void>(sample);
    const auto actors=f.Buffer(257*sizeof(std::uintptr_t));
    for(std::size_t i=0;i<256;++i) {
        const auto actor=f.Buffer(64);f.Register(actor);f.Put(actor+16,shared_class);f.Put(actors+i*sizeof(std::uintptr_t),actor);
    }
    f.Put(actors+256*sizeof(std::uintptr_t),f.machine);f.Put(f.level+160,claw::Array{actors,257,257});
    const auto before=f.reads;
    for(int attempts=0;attempts<10&&!c.candidate.id;++attempts)Scout(c,c.scope);
    Check(ue::Same(c.candidate,f.H(f.machine)),"batched lookup skipped the active machine after unrelated actors");
    Check(f.reads-before<1024,"scene search repeated metadata reads for every actor of the same class");
    Check(Stop(&c,0).code==0,"batched lookup leaked its observer");
}
void LiveMainWindowRegistry() {
    Game f;Context c;f.Bind(c);
    const auto [native,native_class]=f.Object("HTUI_ClawMachineMain",128);static_cast<void>(native);
    f.Property(native_class,"bIsActive",64,1,"BoolProperty");f.Property(native_class,"Visibility",65,1,"EnumProperty");
    const auto [main,blueprint]=f.Object("BPUI_ClawMachineMain_C",128);f.Put(blueprint+64,native_class);f.Put(main+64,std::uint8_t{1});
    f.widget_names[L"ClawMachineMain"]=main;
    // Independently captured HUD registry shape: 9 sparse slots, occupied bits
    // 1..6, with the blueprint main window in slot 6 under ClawMachineMain.
    const auto slots=f.Buffer(9*24);f.Put(slots+6*24,blueprint);f.Put(slots+6*24+8,f.Name("ClawMachineMain"));
    f.Put(f.hud+80,claw::Array{slots,9,23});f.Put(f.hud+96,std::uint32_t{126});
    f.Put(f.hud+120,std::int32_t{9});f.Put(f.hud+124,std::int32_t{128});
    Scope scope;Check(BaseScope(c.engine,scope)&&scope.main==main&&!scope.operation,"live blueprint main-window registry was not recognized");
    f.Put(main+64,std::uint8_t{0});Check(BaseScope(c.engine,scope)&&!scope.main,"inactive main window remained recognized");
    Check(Stop(&c,0).code==0,"main-window fixture leaked its observer");
}
void PolicyAndCodec() {
    const std::vector<std::uint8_t> golden{4,0,0,0,'A','_','1',0};
    Check(claw::EncodeName("A_1")==golden,"Name wire format contains an index instead of FString bytes");
    Check(claw::DecodeName(golden)=="A_1","cannot read ASCII server Name");
    const std::vector<std::uint8_t> wide{252,255,255,255,'A',0,'_',0,'1',0,0,0};
    Check(claw::DecodeName(wide)=="A_1","cannot read UTF16 server Name");
    auto invalid=golden;invalid.back()='x';Check(!claw::DecodeName(invalid),"unterminated FString accepted");
    invalid=golden;invalid[0]=3;Check(!claw::DecodeName(invalid),"mismatched FString length accepted");
    Check(claw::EncodeName(std::string("A\0B",3)).empty()&&claw::EncodeName(std::string(256,'A')).empty(),"invalid identifier accepted");
    const claw::Identity id{{1,11,2,11,3,11,1,77}},other{{1,11,2,11,4,11,1,77}};
    claw::Round r;Check(!r.CanSubmit(id,0),"unpaid round accepted");
    r.BeginRequest(id,0);r.BeginResponse(other,1);Check(r.state==claw::State::WaitingBegin,"other machine armed a round");
    r.BeginResponse(id,1);const auto token=r.sequence;Check(r.Submit(id,token,"Doll_B",2),"confirmed round rejected");
    Check(!r.Submit(id,token,"Doll_B",3),"duplicate request accepted");
    r.Result(other,false,"Doll_B","Item");Check(r.state==claw::State::Pending,"other machine confirmed a reward");
    r.Result(id,false,"Doll_A","Item");Check(r.state==claw::State::Unconfirmed,"wrong doll counted as success");
    r.BeginRequest(id,20);r.BeginResponse(id,21);Check(!r.CanSubmit(id,token),"old UI command spent a new round");
    r.ManualCapture();Check(!r.CanSubmit(id,r.sequence),"manual capture permitted a second capture");
    r.BeginRequest(id,30);r.Tick(15030);r.BeginResponse(id,15031);Check(r.state==claw::State::Unconfirmed,"late confirmation rearmed timed-out round");
}
void Runtime() {
    Game f;Context c;f.Bind(c);const auto list=Items(c,c.scope);Check(list.size()==2,"machine-local item discovery failed");
    Check(c.engine.Resolve({1,f.world_epoch})==0&&c.engine.World()==f.world,"world token was treated as an object handle");
    auto command=f.Choose(c);Execute(c,command);Check(f.requests==0,"unpaid capture dispatched");
    f.Begin(c);command=f.Choose(c);Execute(c,command);Check(f.requests==1&&c.round.state==claw::State::Pending,"selected capture not dispatched once");
    Execute(c,command);Check(f.requests==1,"repeated UI command dispatched twice");
    f.Reply(c,false);Check(c.round.state==claw::State::Success&&c.round.item=="Item_Confirmed","matching server result not confirmed");
    f.Begin(c);command=f.Choose(c);f.Put(f.panel+64,std::uint8_t{0});Execute(c,command);Check(f.requests==1,"closed game UI dispatched a capture");f.Put(f.panel+64,std::uint8_t{1});
    auto forged=command;forged.actor=f.H(f.doll_a);Execute(c,forged);Check(f.requests==1,"mismatched actor and doll accepted");
    f.Put(f.machine+96,claw::Array{f.doll_array,1,2});Execute(c,command);Check(f.requests==1,"removed array member dispatched");f.Put(f.machine+96,claw::Array{f.doll_array,2,2});
    const auto index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(command.actor);f.Put(f.chunk+24*index+16,std::uint32_t{8});Execute(c,command);Check(f.requests==1,"stale actor serial dispatched");f.Put(f.chunk+24*index+16,std::uint32_t{7});
    const auto saved_world=f.current_world;f.current_world=0;Execute(c,command);Check(f.requests==1,"missing current world dispatched");f.current_world=saved_world;
    ++f.world_epoch;Execute(c,command);Check(f.requests==1,"new world epoch spent an old command");--f.world_epoch;
    f.Put(f.registry+64,std::uintptr_t{0});Execute(c,command);Check(f.requests==1,"null GWorld slot dispatched");f.Put(f.registry+64,f.world);
    f.Put(f.server+182,std::uint16_t{24});Execute(c,command);Check(f.requests==1,"incompatible RPC parameter layout dispatched");f.Put(f.server+182,std::uint16_t{32});
    Execute(c,command);Check(f.requests==2,"valid command failed after read-only rejections");f.Reply(c,true);Check(c.round.state==claw::State::Rejected,"server rejection presented as success");
    f.Begin(c);claw::Request manual("Machine_01","Doll_A");Observe(&c,f.player,f.server,&manual.rpc);Execute(c,f.Choose(c));Check(f.requests==2&&c.round.state==claw::State::Used,"manual round was spent twice");
    c.engine.events=nullptr;Check(Stop(&c,0).code==0&&f.unsubscribed==1&&!c.subscription.id,"subscription owner was lost during service refresh");Check(!c.running.load()&&!c.command.load()&&!c.engine.registry,"stop retained game actions");f.observed=nullptr;
}
void LifecycleAndWindow() {
    Game f;void* context{};Check(Load(&f.host,&context).code==0&&context,"load with optional game interfaces failed");
    Check(Start(context).code==0&&f.requests==0,"start performed game work");
    for(int i=0;i<1000;++i){++f.snapshot_sequence;Update(context,0.016);}
    Check(!static_cast<Context*>(context)->detection.load()&&!static_cast<Context*>(context)->engine.registry,"restored enable did not start idle");
    Check(f.game_queries==1&&!f.signature_calls&&!f.reads&&!f.object_counts&&!f.snapshots&&!f.finds&&!f.native_calls&&!f.subscriptions&&!f.scheduled&&!f.semantic_calls&&!f.thread_queries,"idle startup exceeded the SDK signature-service query or touched game objects");
    struct Probe{unsigned begin{},end{},text{};} probe;
    AnomalyUiServiceV1 ui{sizeof(ui),1,&probe};
    ui.begin_window=[](void* user,AnomalyStringViewV1,int* open,std::uint32_t){auto& p=*static_cast<Probe*>(user);++p.begin;Check(open,"window has no close state");*open=0;return 0;};
    ui.end_window=[](void* user){++static_cast<Probe*>(user)->end;};
    ui.text=[](void* user,AnomalyStringViewV1){++static_cast<Probe*>(user)->text;};
    Draw(context,&ui);Draw(context,&ui);Check(probe.begin==1&&probe.end==1,"collapsed or closed window has unbalanced scopes");
    Check(f.translations==1,"localizer was queried more than once");
    Check(Stop(context,0).code==0,"stop failed");Unload(context);
}
void ReadinessPolicy() {
    claw::ReadyGate gate;claw::ReadinessSample sample{{1,77,2,88},100,true};
    for(std::uint64_t now=0;now<=2000;now+=500) {
        ++sample.sequence;Check(gate.Observe(sample,now)==(now==2000),"readiness did not require two stable seconds");
    }
    Check(!gate.Observe(sample,3101),"frozen stale player snapshot passed readiness");
    ++sample.sequence;Check(!gate.Observe(sample,3200),"one new sample after a freeze skipped stabilization");
    for(std::uint64_t now=3700;now<=5200;now+=500) {++sample.sequence;Check(gate.Observe(sample,now)==(now==5200),"snapshot recovery did not restabilize");}
    ++sample.identity[3];++sample.sequence;Check(!gate.Observe(sample,5300),"changed player was immediately considered ready");
    sample.valid=false;Check(!gate.Observe(sample,5400)&&!gate.observed,"invalid snapshot retained readiness");
    sample.valid=true;sample.identity[0]=0;Check(!gate.Observe(sample,5500),"empty world identity passed readiness");
}
void StartupAndLoading() {
    Game f;void* user{};Check(Load(&f.host,&user).code==0&&Start(user).code==0,"startup fixture failed");auto& c=*static_cast<Context*>(user);
    // Optional tails are not needed to consume the semantic snapshot prefix.
    f.player_service.struct_size=static_cast<std::uint32_t>(offsetof(AnomalyNtePlayerServiceV1,snapshot)+sizeof(f.player_service.snapshot));
    f.session.struct_size=static_cast<std::uint32_t>(offsetof(AnomalyNteSessionServiceV1,snapshot)+sizeof(f.session.snapshot));
    SetDetection(c,true);f.session_state=ANOMALY_NTE_SESSION_V1_LOADING;
    for(std::uint64_t t=1000;t<=4000;t+=250){++f.snapshot_sequence;Tick(c,t);}
    f.session_state=ANOMALY_NTE_SESSION_V1_WORLD_READY;f.player_flags|=ANOMALY_NTE_SNAPSHOT_V1_PARTIAL;
    for(std::uint64_t t=4250;t<=7000;t+=250){++f.snapshot_sequence;Tick(c,t);}
    f.player_flags=ANOMALY_NTE_SNAPSHOT_V1_VALID|ANOMALY_NTE_SNAPSHOT_V1_STALE;
    for(std::uint64_t t=7250;t<=10000;t+=250)Tick(c,t);
    Check(!f.reads&&!f.finds&&!f.native_calls&&!f.object_counts&&!f.snapshots&&!f.scheduled&&!f.subscriptions,"loading, partial or frozen snapshots triggered game work");
    for(std::uint64_t t=10250;t<=12250;t+=250){++f.snapshot_sequence;Tick(c,t);}
    Check(f.scheduled==1&&!f.signature_calls&&!f.native_calls&&!f.subscriptions,"stable world did not defer initialization to one Worker");
    f.RunScheduled();Check(f.signature_calls==2&&!f.native_calls&&!f.object_counts,"Worker initialization performed unrelated game work");
    f.Put(f.panel+64,std::uint8_t{0});++f.snapshot_sequence;Tick(c,12300);
    Check(c.prepared&&c.engine.registry==f.registry&&c.engine.world_slot==f.registry+64,"Worker did not publish both validated signature domains");
    Check(!f.subscriptions&&!f.object_counts,"detection outside claw UI subscribed or scanned objects");
    const auto calls=f.player_queries;
    for(std::uint64_t t=12301;t<12550;++t){++f.snapshot_sequence;Tick(c,t);}
    Check(f.player_queries==calls,"native scope was polled more than every 250 ms");
    for(std::uint64_t t=12550;t<13300;++t){++f.snapshot_sequence;Tick(c,t);}
    Check(f.player_queries-calls<=3&&!f.subscriptions&&!f.object_counts,"background scope polling or UI subscription exceeded its limit");
    f.Put(f.panel+64,std::uint8_t{1});++f.snapshot_sequence;Tick(c,13600);
    Check(f.subscriptions==1&&c.subscription.id&&c.scope.machine==f.machine,"active claw UI did not acquire observation");
    const auto thread_queries=f.thread_queries;Observe(&c,f.player,f.get_pc,nullptr);
    Check(f.thread_queries==thread_queries,"unrelated ProcessEvent called back into host services");
    const auto reads=f.reads,native=f.native_calls;
    f.session_state=ANOMALY_NTE_SESSION_V1_LOADING;++f.snapshot_sequence;Tick(c,13650);
    Check(!c.detection.load()&&!c.command.load()&&!c.subscription.id&&!c.engine.registry&&f.unsubscribed==1,"loading retained active detection, requests or subscription");
    Check(f.reads==reads&&f.native_calls==native,"loading transition touched old game objects");
    for(std::uint64_t t=13700;t<16000;t+=33)Tick(c,t);
    Check(f.reads==reads&&f.native_calls==native&&f.scheduled==1,"automatic stop restarted game work");
    Check(Stop(user,0).code==0,"loading stop failed");Unload(user);
}
void CancelledInitialization() {
    Game f;void* user{};Check(Load(&f.host,&user).code==0&&Start(user).code==0,"cancel fixture failed");auto& c=*static_cast<Context*>(user);
    SetDetection(c,true);
    for(std::uint64_t t=1000;t<=3000;t+=250){++f.snapshot_sequence;Tick(c,t);}
    Check(f.scheduled==1&&c.init_task.id,"initialization was not queued");
    SetDetection(c,false);Tick(c,3050);Check(f.cancelled==1&&!c.init_task.id,"turning off did not cancel the scoped task");
    f.RunScheduled(true);Check(!f.signature_calls&&!c.initialized.load()&&!c.detection.load(),"cancelled Worker revived detection");
    SetDetection(c,true);
    for(std::uint64_t t=4000;t<=6000;t+=250){++f.snapshot_sequence;Tick(c,t);}
    Check(f.scheduled==2,"new activation did not schedule a fresh task");
    ++f.player_epoch;++f.snapshot_sequence;Tick(c,6050);f.RunScheduled(true);
    Check(f.cancelled==2&&!c.detection.load()&&!c.initialized.load()&&!f.signature_calls,"player change did not invalidate a queued Worker");
    Check(Stop(user,0).code==0,"cancel stop failed");Unload(user);
}
void AutomaticPaidCapture() {
    Game f;Context c;f.Bind(c);Publish(c,RoundStatus(c),Items(c,c.scope),true);
    struct Probe{std::string target;unsigned buttons{};} probe{"###autoclaw-doll-"+std::to_string(claw::Pack(f.b))};
    AnomalyUiServiceV1 ui{sizeof(ui),1,&probe};ui.text=[](void*,AnomalyStringViewV1){};
    ui.checkbox=[](void* user,AnomalyStringViewV1 label,int* selected) {
        auto& p=*static_cast<Probe*>(user);const std::string_view text(label.data,label.size);
        if(text.ends_with(p.target)){*selected=1;return 1;}return 0;
    };
    ui.button=[](void* user,AnomalyStringViewV1,float,float){++static_cast<Probe*>(user)->buttons;return 1;};
    Controls(c,&ui);Check(c.selection.load()&&c.selection.load()->doll==f.b&&!probe.buttons&&!f.requests,"selection required or sent a capture-button request");
    f.Begin(c);Check(c.command.load()&&c.command.load()->doll==f.b&&!f.requests,"paid confirmation did not queue the selected target");
    const auto queued=c.command.load();const auto sequence=c.round.sequence;f.Confirm(c);
    Check(c.command.load()==queued&&c.round.sequence==sequence,"duplicate paid confirmation queued another capture");
    const auto now=ue::Now();f.Put(f.panel+64,std::uint8_t{0});++f.snapshot_sequence;Tick(c,now);
    Check(c.command.load()&&!f.requests,"temporary operation UI transition lost or dispatched the automatic request");
    f.Put(f.panel+64,std::uint8_t{1});++f.snapshot_sequence;Tick(c,now+300);
    Check(f.requests==1&&c.round.state==claw::State::Pending&&!c.command.load(),"paid round did not automatically capture once");
    ++f.snapshot_sequence;Tick(c,now+600);Check(f.requests==1,"automatic capture retried without another paid round");
    f.Reply(c,false);Check(c.round.state==claw::State::Success,"automatic result did not retain server confirmation");
    // The paid target is immutable even if the next selection changes.
    f.Begin(c);c.selection.store(std::make_shared<Command>(Command{c.scope.identity,0,f.H(f.doll_a),f.a,c.applied_revision}));
    ++f.snapshot_sequence;Tick(c,now+900);Check(f.requests==2&&f.requested_doll=="Doll_B","selection changed the target after payment");f.Reply(c,false);
    c.selection.store({});f.Begin(c);++f.snapshot_sequence;Tick(c,now+1200);
    Check(f.requests==2&&!c.command.load(),"round without a prior target auto-captured");
    c.selection.store(std::make_shared<Command>(f.Choose(c)));f.Begin(c);Check(c.command.load()!=nullptr,"new selected paid round was not armed");
    claw::Request manual("Machine_01","Doll_A");Observe(&c,f.player,f.server,&manual.rpc);
    ++f.snapshot_sequence;Tick(c,now+1500);Check(f.requests==2&&!c.command.load()&&c.round.state==claw::State::Used,"manual claw input duplicated the automatic request");
    f.Begin(c);auto expired=*c.command.load();expired.expires=now+1550;c.command.store(std::make_shared<Command>(expired));
    ++f.snapshot_sequence;Tick(c,now+1800);Check(f.requests==2&&c.round.state==claw::State::Unconfirmed&&!c.command.load(),"expired automatic request was dispatched");
    f.Begin(c);const auto old=*c.command.load();SetDetection(c,false);SetDetection(c,true);
    Execute(c,old);Check(f.requests==2,"request from a previous activation was dispatched");
    Check(Stop(&c,0).code==0,"automatic capture stop failed");
}
}
int main(){fixture::PolicyAndCodec();fixture::ReadinessPolicy();fixture::Runtime();fixture::ReflectedRowsAndTypes();fixture::LiveMainWindowRegistry();fixture::SceneMachineLookup();fixture::BatchedSceneLookup();fixture::LifecycleAndWindow();fixture::StartupAndLoading();fixture::CancelledInitialization();fixture::AutomaticPaidCapture();std::cout<<"autoclaw: idle startup, readiness, Worker cancellation, automatic paid capture, reflected rows, typed ABI, dispatch guards and lifecycle passed\n";}
