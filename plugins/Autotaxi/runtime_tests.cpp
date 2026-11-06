#include "profile_test_setup.hpp"
#include "plugin.cpp"
#include "capability_tests.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>

namespace fixture {
void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct WidgetMap {
    RowArray slots{}; uint32_t bits[4]{}; uintptr_t extra{};
    int32_t bit_count{},max_bits{},first_free{},free_count{}; uint8_t tail[24]{};
};
static_assert(sizeof(WidgetMap)==80);
struct Game;
Game* game{};
void __fastcall Native(void* object, void* function, void* parameters);
struct Game {
    std::vector<std::vector<uint8_t>> storage;
    std::vector<uintptr_t> registered;
    std::map<uint32_t,std::string> strings;
    std::map<std::string,AnomalyGenerationHandleV1> paths;
    AnomalyCoreServiceV1 memory{sizeof(memory),1,this};
    AnomalyUe5NamesServiceV1 name_service{sizeof(name_service),1,this};
    AnomalyUe5ObjectsServiceV1 object_service{sizeof(object_service),1,this};
    uintptr_t items{},chunk{},native_table{};
    uintptr_t world{},level{},ps{},pc{},car{},hud{},panel{},button{},panel_class{},car_class{},map_data{};
    uintptr_t get_ps{},get_pc{},get_car{},get_data{},get_ui{},get_owner{},get_enabled{},get_interaction{},set_state{},continue_order{};
    SaveData data{};
    std::vector<StateParameters> requests;
    unsigned settlements{},reads{};
    bool button_enabled{true},interaction{true},cancel_on_read{},return_other_driver{};
    unsigned localization_queries{},translations{};
    AnomalyLocalizationServiceV1 language{sizeof(language),1,this};
    uintptr_t Buffer(size_t size) { storage.emplace_back(size);return reinterpret_cast<uintptr_t>(storage.back().data()); }
    void Put(uintptr_t at,const auto& value) { std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(value)); }
    FName Name(std::string text) {
        const auto id=static_cast<uint32_t>(strings.size()+1);strings[id]=std::move(text);return {id,0};
    }
    AnomalyGenerationHandleV1 HandleFor(uint32_t index) { return {(uint64_t{7}<<32)|(uint64_t{index}+1),11}; }
    uintptr_t Property(uintptr_t cls,const char* name,int32_t offset,int32_t size,const char* kind="ObjectProperty") {
        const auto p=Buffer(80);uintptr_t previous{};std::memcpy(&previous,reinterpret_cast<void*>(cls+112),8);
        const auto field_class=Buffer(40);Put(field_class,Name(kind));Put(p+8,field_class);
        Put(p+32,Name(name));Put(p+52,size);Put(p+68,offset);Put(p+72,previous);Put(cls+112,p);return p;
    }
    std::pair<uintptr_t,uintptr_t> Object(const char* type,size_t size,const std::string& path={}) {
        const auto value=Buffer(size),cls=Buffer(128);
        Put(value,native_table);Put(value+16,cls);Put(cls+24,Name(type));
        const auto index=static_cast<uint32_t>(registered.size());registered.push_back(value);
        Put(value+12,static_cast<int32_t>(index));Put(chunk+24*index,value);Put(chunk+24*index+16,uint32_t{7});
        Put(items+36,static_cast<uint32_t>(registered.size()));
        if(!path.empty()) paths[path]=HandleFor(index);
        return {value,cls};
    }
    uintptr_t Function(const char* package,const char* owner,const char* name,int count,int bytes,std::initializer_list<Parameter> params) {
        const auto [value,cls]=Object("Function",224,std::string("/Script/")+package+"."+owner+"."+name);
        Put(value+24,Name(name));Put(value+176,uint32_t{0x400});Put(value+180,static_cast<uint8_t>(count));Put(value+182,static_cast<uint16_t>(bytes));
        for(const auto& p:params) Property(value,p.name,p.offset,p.size,p.kind);
        return value;
    }
    Game() {
        game=this;
        language.translate=[](void* user,AnomalyStringViewV1 key,AnomalyStringViewV1 fallback,
            const AnomalyStringViewV1* arguments,size_t count,char* output,size_t* size) {
            auto& f=*static_cast<Game*>(user);++f.translations;
            const std::string_view id(key.data,key.size);
            std::string value(fallback.data,fallback.size);
            if(id=="window.title") value="AutoTaxi";
            if(id=="status.startup") value="localized startup";
            if(id=="label.completed") {
                Check(count==1 && arguments,"completion text did not pass a localization argument");
                value="confirmed="+std::string(arguments[0].data,arguments[0].size);
            }
            const auto required=value.size()+1;
            if(!output || *size<required) {*size=required;return AnomalyStatusV1{ANOMALY_STATUS_V1_BUFFER_TOO_SMALL,0,{}};}
            std::memcpy(output,value.c_str(),required);*size=required;return Ok();
        };
        memory.read_memory=[](void* user,uintptr_t address,AnomalyMutableByteSpanV1 out) {
            if(address==reinterpret_cast<uintptr_t>(&Native))
                return anomaly::testing::RecordedProcessEventEntry(out);
            for(const auto& block:static_cast<Game*>(user)->storage) {
                const auto first=reinterpret_cast<uintptr_t>(block.data());
                if(address>=first && address-first<=block.size() && out.size<=block.size()-(address-first)) {
                    std::memcpy(out.data,reinterpret_cast<void*>(address),out.size);return Ok();
                }
            }
            return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
        };
        name_service.resolve_utf8=[](void* user,uint32_t id,char* out,size_t* size) {
            const auto& strings=static_cast<Game*>(user)->strings;const auto it=strings.find(id);
            if(it==strings.end()) return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
            const auto need=it->second.size()+1;
            if(out) { if(*size<need) return AnomalyStatusV1{ANOMALY_STATUS_V1_BUFFER_TOO_SMALL,0,{}};std::memcpy(out,it->second.c_str(),need); }
            *size=need;return Ok();
        };
        object_service.snapshot_at=[](void* user,uint32_t index,AnomalyUe5ObjectSnapshotV1* out) {
            auto& value=*static_cast<Game*>(user);
            if(index>=value.registered.size()) return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
            out->handle=value.HandleFor(index);return Ok();
        };
        object_service.snapshot_by_handle=[](void* user,AnomalyGenerationHandleV1 h,AnomalyUe5ObjectSnapshotV1* out) {
            auto& value=*static_cast<Game*>(user);const auto index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(h);
            if(index>=value.registered.size() || h.id!=value.HandleFor(index).id || h.generation!=11)
                return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
            out->handle=h;return Ok();
        };
        object_service.find_exact=[](void* user,AnomalyStringViewV1 path,AnomalyGenerationHandleV1* out) {
            const auto& paths=static_cast<Game*>(user)->paths;const auto it=paths.find(std::string(path.data,path.size));
            if(it==paths.end()) return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
            *out=it->second;return Ok();
        };
        core=&memory;names=&name_service;objects=&object_service;
        items=Buffer(64);chunk=Buffer(128*24);const auto chunks=Buffer(8);
        Put(chunks,chunk);Put(items+16,chunks);registry=items;
        native_table=Buffer(0x4d*8);Put(native_table+0x4c*8,reinterpret_cast<uintptr_t>(&Native));
        world=Object("World",64).first;
        const auto level_pair=Object("Level",0x100);level=level_pair.first;
        Property(level_pair.second,"OwningWorld",0xC0,8);Put(level+0xC0,world);
        const auto player_pair=Object("HTPlayerState",0x600);ps=player_pair.first;
        // Intentionally differs from the dump: player field offsets must be reflected.
        Property(player_pair.second,"TaxiGameplayPlayerSaveData",0x380,28,"StructProperty");Put(ps+0x380,int32_t{10});Put(ps+0x398,uint8_t{1});
        const auto controller_pair=Object("HTPlayerController",0x600);pc=controller_pair.first;
        Property(controller_pair.second,"PlayerState",0x300,8);Put(pc+0x300,ps);
        Property(controller_pair.second,"MyHUD",0x310,8);
        const auto car_pair=Object("HTWheeledVehicleDrivable",0x400);car=car_pair.first;car_class=car_pair.second;
        Property(car_class,"Owner",0x160,8);Put(car+0x160,pc);
        for(const auto actor:{ps,pc,car}) Put(actor+32,level);
        const auto hud_pair=Object("HTHUD",0x600);hud=hud_pair.first;Put(pc+0x310,hud);
        Property(hud_pair.second,"PlayerOwner",0x300,8);Put(hud+0x300,pc);
        Property(hud_pair.second,"ActivedWidgetMap",0x380,80,"MapProperty");
        const auto panel_pair=Object("HTUI_TaxiGameplaySettleMain",0x1100);panel=panel_pair.first;panel_class=panel_pair.second;
        Property(panel_class,"bIsActive",0x3e0,1,"BoolProperty");Put(panel+0x3e0,uint8_t{1});
        Property(panel_class,"Visibility",0x100,1,"EnumProperty");Put(panel+0x100,uint8_t{});
        Property(panel_class,"BtnContinueTaxiGameplay",0xF98,8);
        button=Object("CommonButtonBase",0x200).first;Put(panel+0xF98,button);
        map_data=Buffer(3*24);Put(map_data+2*24,panel_class);Put(map_data+2*24+8,Name("TaxiGameplaySettle"));
        WidgetMap map{};map.slots={map_data,3,3};map.bit_count=3;map.max_bits=128;map.bits[0]=4;map.free_count=2;Put(hud+0x380,map);
        const auto save_type=Object("ScriptStruct",128,"/Script/HTGame.TaxiGamePlaySaveData").first;
        for(const auto& p:std::array<Parameter,6>{{{"ExecutingTaxiOrderID",0,8,"NameProperty"},{"GuestLocationID",8,8,"NameProperty"},{"GuestTargetLocationID",16,8,"NameProperty"},
            {"TaxiGameplayState",24,1,"EnumProperty"},{"FortuitousMeetingCharacterID",28,8,"NameProperty"},{"TaxiGameplaySatisfaction",36,4,"FloatProperty"}}}) Property(save_type,p.name,p.offset,p.size,p.kind);
        const auto player_type=Object("ScriptStruct",128,"/Script/HTGame.TaxiGameplayPlayerSaveData").first;
        Property(player_type,"TaxiFinishedCnt",0,4,"IntProperty");Property(player_type,"bIsInit",24,1,"BoolProperty");
        Object("HTUtil",64,"/Script/HTGame.Default__HTUtil");
        get_ps=Function("HTGame","HTUtil","GetMainPlayerState",2,16,{{"WorldContextObject",0,8,"ObjectProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
        get_pc=Function("HTGame","HTUtil","GetMainPlayerController",2,16,{{"WorldContextObject",0,8,"ObjectProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
        get_car=Function("HTGame","HTPlayerController","BP_GetCurrentDriveVehicle",1,8,{{"ReturnValue",0,8,"ObjectProperty"}});
        get_data=Function("HTGame","HTPlayerState","GetTaxiGameplayCurData",1,40,{{"ReturnValue",0,40,"StructProperty"}});
        set_state=Function("HTGame","HTPlayerState","SetTaxiGameplayOrderState",2,2,{{"InOrderState",0,1,"EnumProperty"},{"InEndType",1,1,"EnumProperty"}});
        get_ui=Function("HTGame","HTHUD","BP_GetUIByClass",2,24,{{"UIClass",0,16,"StrProperty"},{"ReturnValue",16,8,"ObjectProperty"}});
        get_owner=Function("UMG","Widget","GetOwningPlayer",1,8,{{"ReturnValue",0,8,"ObjectProperty"}});
        get_enabled=Function("UMG","Widget","GetIsEnabled",1,1,{{"ReturnValue",0,1,"BoolProperty"}});
        get_interaction=Function("CommonUI","CommonButtonBase","IsInteractionEnabled",1,1,{{"ReturnValue",0,1,"BoolProperty"}});
        continue_order=Function("HTGame","HTUI_TaxiGameplaySettleMain","OnBtnContinueTaxiGameplayClicked",1,8,{{"Button",0,8,"ObjectProperty"}});
        data.order=Name("order-template");data.pickup=Name("pickup-point");data.destination=Name("destination-point");data.stage=taxi::Stage::Pickup;
        world_slot=Buffer(8);Put(world_slot,world);
        functions.clear();missing_functions.clear();offsets.clear();run.Reset();confirmed.Reset();
        enabled=true;applied_intent=++intent;next_poll={};
    }
};
void __fastcall Native(void* object,void* function,void* parameters) {
    auto& f=*game;const auto fn=reinterpret_cast<uintptr_t>(function),self=reinterpret_cast<uintptr_t>(object);
    if(fn==f.get_ps || fn==f.get_pc) {
        auto& q=*static_cast<ObjectQuery*>(parameters);Check(q.context==f.world,"query did not use the current world");q.result=fn==f.get_ps?f.ps:f.pc;
    } else if(fn==f.get_car) *static_cast<uintptr_t*>(parameters)=f.return_other_driver?0:f.car;
    else if(fn==f.get_data) {
        *static_cast<SaveData*>(parameters)=f.data;++f.reads;
        if(f.cancel_on_read && f.reads%2==0) {enabled=false;++intent;}
    } else if(fn==f.get_ui) {
        auto& q=*static_cast<UiQuery*>(parameters);
        const auto* label=reinterpret_cast<const wchar_t*>(q.label.data);
        Check(q.label.count==19 && std::wstring(label)==L"TaxiGameplaySettle","HUD lookup used a guessed or incorrect UI key");
        q.widget=f.panel;
    } else if(fn==f.get_owner) *static_cast<uintptr_t*>(parameters)=f.pc;
    else if(fn==f.get_enabled) *static_cast<uint8_t*>(parameters)=f.button_enabled?1:0;
    else if(fn==f.get_interaction) *static_cast<uint8_t*>(parameters)=f.interaction?1:0;
    else if(fn==f.set_state) {
        Check(self==f.ps,"taxi state request was sent on another player");f.requests.push_back(*static_cast<StateParameters*>(parameters));
    } else if(fn==f.continue_order) {
        Check(self==f.panel && *static_cast<uintptr_t*>(parameters)==f.button,"settlement bypassed the actual native button callback");++f.settlements;
    } else Check(false,"unregistered native call");
}

}

#ifndef TAXI_RUNTIME_FIXTURE_ONLY
int main() {
    using fixture::Check;
    LoadTestProfile();
    if(!compatibility_test::Run(MissingRuntimeFeature,{"ue5.objects","ue5.names","ue5.functions","ue5.process-event"})) return 1;
    fixture::Game f;Reader r{names};Runtime current;
    uintptr_t pc_class{},properties{};
    Check(r.Read(f.pc+16,pc_class) && r.Read(pc_class+112,properties),"fixture controller class missing");
    f.Put(pc_class+112,uintptr_t{});
    Check(Field(r,f.pc,"MyHUD",8,"ObjectProperty")<0,"temporarily absent metadata returned a field");
    f.Put(pc_class+112,properties);
    Check(Field(r,f.pc,"MyHUD",8,"ObjectProperty")==0x310,"negative field cache prevented recovery after metadata arrived");
    offsets.clear();
    uintptr_t field_kind{};Check(r.Read(properties+8,field_kind),"fixture field type missing");
    f.Put(field_kind,f.Name("IntProperty"));
    Check(Field(r,f.pc,"MyHUD",8,"ObjectProperty")<0,"same-sized wrong property type was accepted");
    f.Put(field_kind,f.Name("ObjectProperty"));
    Check(SaveLayout(r) && ReadRuntime(r,current),"reflected native taxi state could not be read");
    const std::array<Parameter,2> state_params{{{"InOrderState",0,1,"EnumProperty"},{"InEndType",1,1,"EnumProperty"}}};
    Check(ParametersMatch(r,f.set_state,state_params),"native taxi wrapper ABI rejected");
    f.Put(f.set_state+176,uint32_t{0x440});Check(!ParametersMatch(r,f.set_state,state_params),"raw outbound RPC was accepted as a native gameplay wrapper");
    f.Put(f.set_state+176,uint32_t{});Check(!ParametersMatch(r,f.set_state,state_params),"Blueprint callback was confused with the reviewed native wrapper");
    f.Put(f.set_state+176,uint32_t{0x400});
    auto mismatched=state_params;mismatched[1].offset=2;
    Check(!ParametersMatch(r,f.set_state,mismatched),"changed end-type argument offset was accepted");
    f.Put(f.set_state+182,uint16_t{8});
    Check(!r.Function(f.set_state,"SetTaxiGameplayOrderState",2,2),"padded request size was confused with parameter size");f.Put(f.set_state+182,uint16_t{2});
    f.Put(f.car+0x160,uintptr_t{});Check(!ReadRuntime(r,current),"a vehicle owned by another driver reached automation");f.Put(f.car+0x160,f.pc);
    f.Put(f.car+8,uint32_t{0x10});Check(!ReadRuntime(r,current),"a vehicle template reached automation");f.Put(f.car+8,uint32_t{});
    const auto [asset,asset_cls]=f.Object("World",64);
    f.Put(f.level+32,asset);
    Check(ReadRuntime(r,current),"streamed level asset Outer was confused with active OwningWorld");
    f.Put(f.level+0xC0,asset);Check(!ReadRuntime(r,current),"a car in a different world was accepted");f.Put(f.level+0xC0,f.world);
    Check(ReadRuntime(r,current),"current local driver was not restored");
    Check(FindActiveWidget(r,current,"HTUI_TaxiGameplaySettleMain")==f.panel,"sparse HUD slots could not resolve the active local settlement UI");
    f.Put(f.panel+0x3e0,uint8_t{});Check(!FindActiveWidget(r,current,"HTUI_TaxiGameplaySettleMain"),"inactive cached settlement UI was accepted");f.Put(f.panel+0x3e0,uint8_t{1});
    f.Put(f.panel+0x100,uint8_t{1});Check(!FindActiveWidget(r,current,"HTUI_TaxiGameplaySettleMain"),"collapsed settlement UI was accepted");f.Put(f.panel+0x100,uint8_t{});
    Check(EnabledButton(r,f.button),"normal continuation button was rejected");
    f.interaction=false;Check(!EnabledButton(r,f.button),"interaction-disabled continuation button was forced");f.interaction=true;

    AnomalyUe5BuildServiceV1 build{sizeof(build),1};
    build.feature_state=[](void*,AnomalyStringViewV1){return uint32_t{ANOMALY_FEATURE_V1_AVAILABLE};};build_service=&build;
    AnomalyUe5FrameworkServiceV1 game_thread{sizeof(game_thread),1};game_thread.is_game_thread=[](void*){return 1;};framework=&game_thread;
    const auto tick=[] {next_poll={};Update(nullptr,0.5);};
    // Native responses are replayed independently. The dispatcher does not synthesize
    // any stage, completion counter, reward or satisfaction changes.
    tick();Check(f.requests.size()==1 && f.requests[0].state==3 && f.requests[0].end_type==0,"pickup did not submit the reviewed native parameters");
    tick();Check(f.requests.size()==1,"unacknowledged pickup was repeated");
    f.return_other_driver=true;tick();f.return_other_driver=false;tick();
    Check(f.requests.size()==1,"temporary runtime unavailability caused duplicate pickup submission");
    f.data.stage=taxi::Stage::Destination;tick();
    Check(f.requests.size()==2 && f.requests.back().state==5 && f.requests.back().end_type==0,"delivery skipped the native completion request");
    tick();Check(f.requests.size()==2,"unacknowledged delivery was repeated");
    Check(published.load()->completed==0,"submitted delivery was displayed as confirmed completion");
    f.data.stage=taxi::Stage::Settle;f.button_enabled=false;tick();
    Check(f.settlements==0,"disabled settlement button was forced");
    f.button_enabled=true;tick();tick();
    Check(f.settlements==1 && published.load()->completed==0,"settlement was repeated or falsely counted");
    f.data.stage=taxi::Stage::Waiting;f.Put(f.ps+0x380,int32_t{11});tick();tick();
    Check(published.load()->completed==1,"replicated completed-order count was not confirmed exactly once");
    f.data.stage=taxi::Stage::Pickup;f.reads=0;f.cancel_on_read=true;tick();
    Check(f.requests.size()==2,"disable between observation and dispatch still sent a taxi request");
    f.cancel_on_read=false;enabled=true;++intent;tick();
    Check(f.requests.size()==3,"explicit re-enable did not acquire a new automation intent");
    const auto count=f.requests.size();
    game_thread.is_game_thread=[](void*){return 0;};tick();Check(f.requests.size()==count,"non-game thread performed native gameplay work");
    game_thread.is_game_thread=[](void*){return 1;};
    Check(Stop(nullptr,0).code==0 && !enabled.load(),"stop retained taxi auto intent");tick();
    Check(f.requests.size()==count && !run.bound,"disabled module retained pending actions");
    Check(Start(nullptr).code==0 && !enabled.load() && confirmed.added==0,"module reload did not default off/reset confirmation count");
    AnomalyPluginDescriptorV1 descriptor{sizeof(descriptor)};
    Check(AnomalyPluginEntryV1(&descriptor).code==0 && descriptor.on_draw==Draw && descriptor.on_update==Update,
        "standalone entry did not expose the taxi lifecycle");
    Check(std::string_view(descriptor.id.data,descriptor.id.size)=="b1ank.autotaxi" &&
        std::string_view(descriptor.version.data,descriptor.version.size)=="0.1.11" &&
        std::string_view(descriptor.name.data,descriptor.name.size)=="AutoTaxi","standalone metadata did not match its manifest");
    anomaly::testing::ProfileHost profile_host("Autotaxi");PrepareProfileHost(profile_host);
    struct WaitingServices {fixture::Game* game;anomaly::testing::ProfileHost* profile;} waiting_services{&f,&profile_host};
    AnomalyHostApiV1 waiting_host{};waiting_host.struct_size=sizeof(waiting_host);waiting_host.api_major=1;waiting_host.host_context=&waiting_services;
    waiting_host.query_service=[](void* user,AnomalyStringViewV1 id,uint32_t,const void** table) {
        *table=nullptr;
        if(std::string_view(id.data,id.size)==ANOMALY_LOCALIZATION_SERVICE_V1_ID) {
            auto& game=*static_cast<WaitingServices*>(user)->game;++game.localization_queries;*table=&game.language;return Ok();
        }
        return anomaly::testing::ProfileHost::Query(static_cast<WaitingServices*>(user)->profile,id,1,table);
    };
    void* waiting_context{};
    Check(Load(&waiting_host,&waiting_context).code==0 && Start(nullptr).code==0,"unavailable game services prevented the standalone window from loading");
    Check(f.localization_queries==1 && published.load()->status=="localized startup","localization was not acquired once at load and used for status text");
    Update(nullptr,0.5);
    Check(!enabled.load() && f.requests.size()==count && !names && !framework,"waiting standalone plugin performed native actions without game services");
    struct WindowProbe {unsigned registrations{},releases{},begins{},ends{},texts{};int open{1};} window_probe;
    AnomalyWindowServiceV1 windows{sizeof(windows),1,&window_probe};
    windows.register_window=[](void* user,const AnomalyWindowSpecV1* spec,AnomalyGenerationHandleV1* out) {
        auto& probe=*static_cast<WindowProbe*>(user);++probe.registrations;
        Check(std::string_view(spec->id.data,spec->id.size)=="b1ank-autotaxi-main" && spec->default_open==1,"standalone window identity/default was incorrect");
        Check(std::string_view(spec->title.data,spec->title.size)=="AutoTaxi","standalone window title was not renamed");
        *out={123,1};return Ok();
    };
    windows.release_window=[](void* user,AnomalyGenerationHandleV1 h) {
        Check(h.id==123,"wrong standalone window released");++static_cast<WindowProbe*>(user)->releases;return Ok();
    };
    windows.state=[](void* user,AnomalyGenerationHandleV1,AnomalyWindowStateV1* state) {
        state->open=static_cast<WindowProbe*>(user)->open;return Ok();
    };
    windows.begin=[](void* user,AnomalyGenerationHandleV1,uint32_t,int32_t* visible) {
        ++static_cast<WindowProbe*>(user)->begins;*visible=1;return Ok();
    };
    windows.end=[](void* user,AnomalyGenerationHandleV1) {++static_cast<WindowProbe*>(user)->ends;return Ok();};
    AnomalyUiServiceV1 ui{sizeof(ui),1,&window_probe};
    ui.text=[](void* user,AnomalyStringViewV1) {++static_cast<WindowProbe*>(user)->texts;};
    window_service=&windows;
    Check(Start(nullptr).code==0 && !enabled.load() && window_handle.id==123 && window_probe.registrations==1,"standalone startup failed to create its own window");
    Draw(nullptr,&ui);
    Check(window_probe.begins==1 && window_probe.ends==1 && window_probe.texts>0 && window_probe.registrations==1,"standalone draw did not balance its registered window");
    const auto text_count=window_probe.texts;window_probe.open=0;Draw(nullptr,&ui);
    Check(window_probe.begins==1 && window_probe.texts==text_count,"closed standalone window still drew controls");
    Check(Stop(nullptr,0).code==0 && !window_handle.id && window_probe.releases==1,"standalone stop leaked the window");
    Unload(nullptr);Check(window_probe.releases==1,"standalone unload released the same window twice");
    window_service=nullptr;
    ui.begin_window=[](void* user,AnomalyStringViewV1 title,int*,uint32_t) {
        Check(std::string_view(title.data,title.size)=="AutoTaxi","fallback window title was not renamed");
        ++static_cast<WindowProbe*>(user)->begins;return 1;
    };
    ui.end_window=[](void* user) {++static_cast<WindowProbe*>(user)->ends;};
    Draw(nullptr,&ui);
    Check(window_probe.begins==2 && window_probe.ends==2 && f.localization_queries==1,"fallback draw re-queried localization or left its window unbalanced");
    localizer=anomaly::sdk::Localizer{};
    Check(localizer.Format("fixture","Order {0} {{ready}}",std::array<std::string_view,1>{"A"})=="Order A {ready}","English fallback placeholders/braces did not format correctly");
    std::cout << "Taxi runtime ABI, driver/world guards, real callback dispatch and disabled lifecycle checks passed\n";
}
#endif
