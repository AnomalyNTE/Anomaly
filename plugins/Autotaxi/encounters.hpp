#pragma once

struct EncounterChoice { FName character{}; std::string label; };
// SDK FSoftObjectPtr: weak object, top-level asset path, owned UTF-8 subpath.
struct EncounterSelectionResult {uint64_t weak{};FName package{},asset{};RowArray subpath{};};
static_assert(sizeof(EncounterSelectionResult)==40 && offsetof(EncounterSelectionResult,subpath)==24);
struct EncounterPreference { taxi::Identity identity{}; FName character{}; std::string label; };
struct AssignedEncounter {taxi::Identity identity{};taxi::Order order{};FName character{};EncounterSelectionResult appearance{};};
std::shared_ptr<const AssignedEncounter> assigned_encounter; // Game-owned, cleared after hook drain.
struct EncounterView {
    taxi::Identity identity{};
    std::vector<EncounterChoice> choices;
    std::string status;
};
enum class EncounterCommandKind { Refresh, Select, Cancel };
struct EncounterCommand {
    EncounterCommandKind kind{};
    taxi::Identity identity{};
    FName character{};
};
std::atomic<std::shared_ptr<const EncounterPreference>> encounter_pending;
std::atomic<std::shared_ptr<const EncounterCommand>> encounter_commands;
std::atomic<std::shared_ptr<const EncounterView>> encounter_published;
const AnomalyHookServiceV1* encounter_hooks{};
AnomalyGenerationHandleV1 encounter_hook{};
uintptr_t encounter_original{};
Clock::time_point encounter_next_poll{};
// Render-owned controls; gameplay selection remains in the Game-thread command.
std::array<char,128> encounter_search{};
size_t encounter_page{};

void EncounterStatus(std::string text, const Runtime* runtime=nullptr,
                     const std::vector<EncounterChoice>* choices=nullptr) {
    const auto previous=encounter_published.load();
    auto view=previous ? std::make_shared<EncounterView>(*previous) : std::make_shared<EncounterView>();
    view->status=std::move(text);
    if(runtime) view->identity=runtime->observation.identity;
    if(choices) view->choices=*choices;
    encounter_published.store(std::move(view));
}
void EncounterLog(const std::string& text) {
    if(core && core->log) core->log(core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,StringView(text));
}
bool EncounterReadFailed(std::string_view step) {
    EncounterLog("Encounter list unavailable: "+std::string(step));return false;
}
bool WriteEncounterName(uintptr_t address,FName value) {
    return core && core->write_memory && core->write_memory(core->user,address,
        {reinterpret_cast<const uint8_t*>(&value),sizeof(value)}).code==ANOMALY_STATUS_V1_OK;
}
// Only native-returned TArray storage is owned here; its allocator belongs to the game.
struct EncounterArray {
    RowArray value{};
    ~EncounterArray() { taxi::encounter::FreeArray(taxi::profile::snapshot.encounter,value.data); }
};
bool EncounterTableRows(const Reader& r,uintptr_t table,int32_t maximum,
                        std::vector<std::pair<FName,uintptr_t>>& result) {
    const auto row_struct=Field(r,table,"RowStruct",sizeof(uintptr_t),"ObjectProperty");
    const auto& layout=taxi::profile::snapshot.layout;
    const auto rows=taxi::profile::snapshot.compatibility.Document().layout.find("DataTableRows");
    if(row_struct<0 || rows==taxi::profile::snapshot.compatibility.Document().layout.end() ||
       rows->second!=static_cast<uint32_t>(row_struct)+sizeof(uintptr_t)) return false;
    RowArray map{};uintptr_t extra{};int32_t bits{},capacity{};
    const auto address=table+rows->second;
    if(!r.Read(address,map) || map.count<0 || map.count>maximum || map.capacity<map.count ||
       !r.Read(address+layout.MapBitsData,extra) || !r.Read(address+layout.MapBitCount,bits) ||
       !r.Read(address+layout.MapBitCapacity,capacity) || bits<map.count || bits>maximum || capacity<bits ||
       (!extra && bits>128) || (map.count && !map.data)) return false;
    if(!map.count) return true;
    // Snapshot the sparse entries in two reads, including for the large item
    // table. A label refresh must not issue one host memory call per item.
    std::vector<uint8_t> entries(static_cast<size_t>(map.count)*layout.MapEntryStride);
    std::vector<uint32_t> allocated((map.count+31)/32);
    if(!taxi::encounter::Read(core,map.data,entries.data(),entries.size()) ||
       !taxi::encounter::Read(core,extra?extra:address+layout.MapInlineBits,allocated.data(),allocated.size()*sizeof(uint32_t))) return false;
    for(int i=0;i<map.count;++i) {
        if(!(allocated[i/32]&(1u<<(i%32)))) continue;
        FName key{};uintptr_t row{};
        const auto entry=entries.data()+static_cast<size_t>(i)*layout.MapEntryStride;
        std::memcpy(&key,entry,sizeof(key));std::memcpy(&row,entry+layout.MapValue,sizeof(row));
        if(!row) return false;
        result.emplace_back(key,row);
    }
    return true;
}
uintptr_t CurrentTaxiAsset(const Reader& r) {
    // The native passenger selector gets this key from the current session's
    // HTDataAssetSet. A package-path object cached before relog can be obsolete.
    const auto library=Find(r,"/Script/Engine.Default__KismetStringLibrary");
    const auto convert=Checked(r,"Engine","KismetStringLibrary","Conv_StringToName",2,24,
        {{"InString",0,16,"StrProperty"},{"ReturnValue",16,8,"NameProperty"}});
    const auto cdo=Find(r,"/Script/HTGame.Default__HTDataAssetSet");
    const auto getter=Checked(r,"HTGame","HTDataAssetSet","K2_GetDataAsset",2,16,
        {{"AssetName",0,8,"NameProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
    if(!r.IsA(library,"KismetStringLibrary") || !convert || !r.IsA(cdo,"HTDataAssetSet") || !getter) return 0;
    // Game data identifier, not UI text.
    constexpr wchar_t key[]=L"DA_TaxiGameplay";
    struct NameQuery {RowArray input{};FName result{};} name{
        {reinterpret_cast<uintptr_t>(key),static_cast<int32_t>(std::size(key)),static_cast<int32_t>(std::size(key))}};
    static_assert(sizeof(NameQuery)==24);
    if(!Invoke(r,library,convert,&name) || r.Name(name.result)!="DA_TaxiGameplay") return 0;
    struct AssetQuery {FName name{};uintptr_t result{};} query{name.result};
    return Invoke(r,cdo,getter,&query)?query.result:0;
}
bool EncounterNpcCharacters(const Reader& r,std::set<uint64_t>& result) {
    const auto asset=CurrentTaxiAsset(r);
    if(!r.IsA(asset,"TaxiGameplayDataAsset")) return EncounterReadFailed("taxi asset lookup");
    const auto table=Child(r,asset,"DT_TaxiGameplayPassengerNPCTable");
    if(!r.IsA(table,"DataTable")) return EncounterReadFailed("passenger table");
    const auto type=Child(r,table,"RowStruct");
    if(r.NameAt(type+taxi::profile::snapshot.layout.ObjectName)!="TaxiGameplayPassengerNPCData") return EncounterReadFailed("passenger row type");
    const auto character=StructField(r,type,"CharacterID",sizeof(FName),"NameProperty");
    const auto is_character=StructField(r,type,"bIsCharacterNPC",1,"BoolProperty");
    std::vector<std::pair<FName,uintptr_t>> rows;
    if(character<0 || is_character<0 || !EncounterTableRows(r,table,4096,rows)) return EncounterReadFailed("passenger table layout");
    for(const auto& [key,row]:rows) {
        FName id{};uint8_t character_npc{};
        if(!r.Read(row+character,id) || !r.Read(row+is_character,character_npc) || character_npc>1) return EncounterReadFailed("passenger row read");
        if(character_npc && id.id) result.insert(Pack(id));
    }
    return true;
}
void ResolveEncounterLabels(const Reader& r,std::vector<EncounterChoice>& choices) {
    if(choices.empty() || !names || names->struct_size<offsetof(AnomalyUe5NamesServiceV1,resolve_ftext_utf8)+sizeof(names->resolve_ftext_utf8) || !names->resolve_ftext_utf8) return;
    const auto cdo=Find(r,"/Script/HTGame.Default__HTGameData");
    const auto getter=Checked(r,"HTGame","HTGameData","GetGameData",1,8,{{"ReturnValue",0,8,"ObjectProperty"}});
    uintptr_t data{};
    if(!cdo || !getter || !Invoke(r,cdo,getter,&data) || !r.IsA(data,"HTGameData")) return;
    // Characters live in their own table; the generic ItemDataTable contains
    // consumables/currencies and does not provide the numeric character rows.
    const auto table=Child(r,data,"CharacterDataTable");
    if(!r.IsA(table,"DataTable")) return;
    const auto type=Child(r,table,"RowStruct");
    if(r.NameAt(type+taxi::profile::snapshot.layout.ObjectName)!="StaticItemData") return;
    const auto item_name=StructField(r,type,"ItemName",16,"TextProperty");
    std::vector<std::pair<FName,uintptr_t>> rows;
    if(item_name<0 || !EncounterTableRows(r,table,262144,rows)) return;
    std::map<uint64_t,EncounterChoice*> wanted;
    for(auto& choice:choices) wanted.emplace(Pack(choice.character),&choice);
    for(const auto& [id,row]:rows) {
        const auto found=wanted.find(Pack(id));
        if(found==wanted.end()) continue;
        // ItemName is a persistent table FText. The service reads it without
        // constructing a temporary UE value or retaining an engine reference.
        size_t size{};
        const auto status=names->resolve_ftext_utf8(names->user,row+item_name,nullptr,&size);
        if((status.code==ANOMALY_STATUS_V1_OK || status.code==ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) && size>1 && size<=4096) {
            std::string label(size,'\0');
            if(names->resolve_ftext_utf8(names->user,row+item_name,label.data(),&size).code==ANOMALY_STATUS_V1_OK && size<=label.size()) {
                label.resize(size);
                if(const auto nul=label.find('\0');nul!=std::string::npos) label.resize(nul);
                if(!label.empty()) found->second->label=std::move(label);
            }
        }
        wanted.erase(found);
        if(wanted.empty()) break;
    }
}
bool ReadEncounterCandidates(const Reader& r,const Runtime& runtime,std::vector<EncounterChoice>& choices) {
    const auto& profile=taxi::profile::snapshot;
    const auto getter=Checked(r,"HTGame","HTPlayerState","GetTaxiGameplayFortuitousMeetingCharacter",1,16,
        {{"ReturnValue",0,16,"ArrayProperty"}});
    const auto inventory=Child(r,runtime.player,"InventoryComponent");
    const auto amount=Checked(r,"HTGame","HTInventoryComponent","FastGetItemTotalAmount",4,24,
        {{"ItemID",0,8,"NameProperty"},{"ItemType",8,1,"EnumProperty"},
         {"ContainsBind",9,1,"BoolProperty"},{"ReturnValue",16,8,"Int64Property"}});
    if(!taxi::encounter::ValidateGetter(core,profile.compatibility.Document(),profile.encounter,getter)) return EncounterReadFailed("candidate getter ABI");
    if(!r.IsA(inventory,"HTInventoryComponent")) return EncounterReadFailed("current inventory");
    if(!amount) return EncounterReadFailed("ownership getter ABI");
    std::set<uint64_t> npcs;
    if(!EncounterNpcCharacters(r,npcs)) return false;
    EncounterArray returned;
    if(!Invoke(r,runtime.player,getter,&returned.value)) return EncounterReadFailed("candidate getter invocation");
    const auto& array=returned.value;
    if(array.count<0 || array.count>256 || array.capacity<array.count || array.capacity>4096 || (array.count && !array.data)) return EncounterReadFailed("candidate array layout");
    std::set<uint64_t> seen;
    for(int i=0;i<array.count;++i) {
        FName id{};
        if(!r.Read(array.data+sizeof(FName)*i,id)) return EncounterReadFailed("candidate array read");
        if(!id.id || !npcs.contains(Pack(id)) || !seen.insert(Pack(id)).second) continue;
        struct Amount { FName item{};uint8_t type{3},bound{1},pad[6]{};int64_t total{}; } query{id};
        static_assert(sizeof(Amount)==24);
        if(!Invoke(r,inventory,amount,&query)) return EncounterReadFailed("ownership getter invocation");
        if(query.total>0) choices.push_back({id,r.Name(id)});
    }
    ResolveEncounterLabels(r,choices);
    return true;
}
bool ArmEncounter(const Reader& r,const Runtime& runtime,FName character) {
    if(runtime.data.stage!=taxi::Stage::Idle && runtime.data.stage!=taxi::Stage::Waiting) {
        EncounterStatus(localizer.Text("encounter.too_late","Finish the current order, then select a character before the next passenger is generated."));return false;
    }
    std::vector<EncounterChoice> choices;
    if(!ReadEncounterCandidates(r,runtime,choices)) {
        const std::vector<EncounterChoice> empty;
        EncounterStatus(localizer.Text("encounter.read_failed","Could not verify the encounter list. No character preference was applied."),&runtime,&empty);return false;
    }
    const auto found=std::find_if(choices.begin(),choices.end(),[&](const auto& item){return Pack(item.character)==Pack(character);});
    if(found==choices.end()) {
        EncounterStatus(localizer.Text("encounter.ineligible","This character is no longer eligible. Refresh the list."),&runtime,&choices);return false;
    }
    encounter_pending.store(std::make_shared<const EncounterPreference>(EncounterPreference{runtime.observation.identity,character,found->label}));
    EncounterStatus(localizer.Format("encounter.armed","Next passenger preference: {0}. Start taking orders in the game.",
        std::array<std::string_view,1>{found->label}),&runtime,&choices);
    EncounterLog("Encounter preference armed: "+found->label);
    return true;
}
// The native selector owns the actual NPC class result, order updates and RPCs.
// This hook supplies a preference for one local invocation only, never a reward.
void* __fastcall EncounterDetour(void* player,void* output) noexcept {
    const auto original=reinterpret_cast<void*(__fastcall*)(void*,void*)>(encounter_original);
    AnomalyGenerationHandleV1 lease{};
    const auto admitted=encounter_hooks && encounter_hooks->begin_callback &&
        encounter_hooks->begin_callback(encounter_hooks->user,encounter_hook,&lease).code==ANOMALY_STATUS_V1_OK;
    // A denied lease skips plugin work but must preserve the game's selector.
    if(!original) {
        if(admitted) encounter_hooks->end_callback(encounter_hooks->user,lease);
        return output;
    }
    if(!admitted) return original(player,output);
    // Automated pickup may prepare the role before the actual NPC loads. That
    // later native spawn needs the same class, without rerolling or recording it
    // a second time. This cached return contains no owned subpath allocation.
    try {
        if(assigned_encounter && !encounter_pending.load() && framework && framework->is_game_thread && framework->is_game_thread(framework->user)) {
            const Reader r{names};Runtime current;
            if(ReadRuntime(r,current) && current.player==reinterpret_cast<uintptr_t>(player) &&
               current.observation.identity==assigned_encounter->identity && current.observation.order==assigned_encounter->order &&
               current.data.stage>=taxi::Stage::Pickup && Pack(current.data.character)==Pack(assigned_encounter->character)) {
                std::memcpy(output,&assigned_encounter->appearance,sizeof(EncounterSelectionResult));
                encounter_hooks->end_callback(encounter_hooks->user,lease);return output;
            }
        }
    } catch(...) {}
    std::shared_ptr<const EncounterPreference> request;
    uintptr_t slot{};FName previous{};bool written{};
    try {
        request=encounter_pending.load();
        if(request && framework && framework->is_game_thread && framework->is_game_thread(framework->user)) {
            const Reader r{names};Runtime runtime;
            if(ReadRuntime(r,runtime) && runtime.player==reinterpret_cast<uintptr_t>(player) &&
               runtime.observation.identity==request->identity) {
                std::vector<EncounterChoice> candidates;
                slot=runtime.player+taxi::profile::snapshot.encounter.offset;
                if(ReadEncounterCandidates(r,runtime,candidates) &&
                   std::any_of(candidates.begin(),candidates.end(),[&](const auto& c){return Pack(c.character)==Pack(request->character);}) &&
                   r.Read(slot,previous) && !Pack(previous)) {
                    if(encounter_pending.compare_exchange_strong(request,{})) {
                        written=WriteEncounterName(slot,request->character);
                        if(!written) EncounterStatus(localizer.Text("encounter.read_failed","Could not verify the encounter list. No character preference was applied."));
                    }
                } else {
                    encounter_pending.compare_exchange_strong(request,{});
                    EncounterStatus(localizer.Text("encounter.unavailable_now","The selected character is unavailable or another preference is active. Refresh before trying again."));
                }
            }
        }
    } catch(...) {encounter_pending.store({});}
    const auto result=original(player,output);
    try {
        if(written) {
            const Reader r{names};FName remaining{};
            // Do not overwrite a different value left by the game or another tool.
            if(r.Read(slot,remaining) && Pack(remaining)==Pack(request->character)) WriteEncounterName(slot,previous);
            const auto data_offset=Field(r,reinterpret_cast<uintptr_t>(player),"TaxiGamePlaySaveData",sizeof(SaveData),"StructProperty");
            SaveData actual{};
            if(data_offset>=0 && r.Read(reinterpret_cast<uintptr_t>(player)+data_offset,actual) && Pack(actual.character)==Pack(request->character)) {
                const auto label=request->label.empty()?r.Name(actual.character):request->label;
                EncounterStatus(localizer.Format("encounter.generated","Game selected passenger: {0}. Confirm the NPC and finish the encounter in the game.",std::array<std::string_view,1>{label}));
                EncounterLog("Native passenger selected: "+label+"; encounter completion remains unverified");
            } else EncounterStatus(localizer.Text("encounter.not_selected","The game did not select the requested character. No encounter completion has been confirmed."));
        }
    } catch(...) {encounter_pending.store({});}
    encounter_hooks->end_callback(encounter_hooks->user,lease);
    return result;
}
enum class EncounterPreparation { None, Prepared, Failed, Stale };
EncounterPreparation PrepareEncounterPickup(const Reader& r,const Runtime& runtime,uint64_t token) {
    const auto request=encounter_pending.load();
    if(!request || runtime.data.stage!=taxi::Stage::Pickup || runtime.data.character.id) return EncounterPreparation::None;
    if(!CurrentIntent(token)) return EncounterPreparation::Stale;
    if(request->identity!=runtime.observation.identity) {
        encounter_pending.store({});return EncounterPreparation::Stale;
    }
    const auto& native=taxi::profile::snapshot.encounter;
    const auto type=Find(r,"/Script/HTGame.TaxiGameplayPassengerNPCData");
    if(!encounter_hook.id || native.result_size!=sizeof(EncounterSelectionResult) ||
       native.subpath_offset!=offsetof(EncounterSelectionResult,subpath) || !type ||
       StructField(r,type,"AITemplate",sizeof(EncounterSelectionResult),"SoftClassProperty")<0) return EncounterPreparation::Failed;
    EncounterSelectionResult selected;
    struct Cleanup {
        EncounterSelectionResult& value;
        ~Cleanup() {taxi::encounter::FreeArray(taxi::profile::snapshot.encounter,value.subpath.data);}
    } cleanup{selected};
    if(!CurrentIntent(token)) return EncounterPreparation::Stale;
    // Use the same selector and record RPC as native passenger spawning, before
    // the automated pickup can advance past that normal spawning trigger.
    if(!taxi::encounter::Select(native,runtime.player,&selected,sizeof(selected))) return EncounterPreparation::Failed;
    Runtime current;
    if(!selected.package.id || !selected.asset.id || !ReadRuntime(r,current) ||
       current.observation.identity!=runtime.observation.identity || current.observation.order!=runtime.observation.order ||
       current.data.stage!=taxi::Stage::Pickup || Pack(current.data.character)!=Pack(request->character)) return EncounterPreparation::Failed;
    if(selected.subpath.count<0 || selected.subpath.count>1) return EncounterPreparation::Failed;
    if(selected.subpath.count==1) {
        uint8_t terminator{};
        if(!selected.subpath.data || !r.Read(selected.subpath.data,terminator) || terminator) return EncounterPreparation::Failed;
    }
    auto appearance=selected;appearance.subpath={};
    assigned_encounter=std::make_shared<const AssignedEncounter>(AssignedEncounter{current.observation.identity,current.observation.order,request->character,appearance});
    EncounterLog("Preferred passenger assigned before automatic pickup: "+request->label);
    return EncounterPreparation::Prepared;
}
void StartEncounters() {
    encounter_commands.store({});encounter_pending.store({});encounter_published.store({});encounter_next_poll={};
    encounter_search.fill('\0');encounter_page=0;
    assigned_encounter.reset();
    if(!encounter_hooks || !encounter_hooks->create || !encounter_hooks->release ||
       !encounter_hooks->begin_callback || !encounter_hooks->end_callback || !core || !core->write_memory ||
       !taxi::profile::snapshot.encounter) {
        EncounterStatus(localizer.Text("encounter.unsupported","Character selection is unavailable for this game interface. Automatic taxi completion remains available."));return;
    }
    AnomalyHookRequestV1 spec{sizeof(spec),ANOMALY_HOOK_V1_FUNCTION,
        taxi::profile::snapshot.encounter.selector,reinterpret_cast<void*>(&EncounterDetour),StringView("autotaxi-passenger-preference")};
    if(encounter_hooks->create(encounter_hooks->user,&spec,&encounter_original,&encounter_hook).code!=ANOMALY_STATUS_V1_OK) {
        encounter_hook={};encounter_original=0;
        EncounterStatus(localizer.Text("encounter.unsupported","Character selection is unavailable for this game interface. Automatic taxi completion remains available."));return;
    }
    EncounterStatus(localizer.Text("encounter.instructions","Drive a taxi, refresh and select a character before taking orders. Works with automatic completion; each preference is used once."));
}
bool StopEncounters() {
    encounter_commands.store({});encounter_pending.store({});
    if(encounter_hook.id && encounter_hooks && encounter_hooks->release(encounter_hooks->user,encounter_hook).code!=ANOMALY_STATUS_V1_OK) return false;
    encounter_hook={};encounter_original=0;assigned_encounter.reset();return true;
}
void UpdateEncounters(const Reader& r) {
    auto command=encounter_commands.exchange({});
    if(command && command->kind==EncounterCommandKind::Cancel) {
        encounter_pending.store({});EncounterStatus(localizer.Text("encounter.cancelled","Passenger preference cancelled."));return;
    }
    if(!command && !encounter_pending.load()) return;
    if(!encounter_hook.id) return;
    if(command && command->kind==EncounterCommandKind::Refresh) {
        encounter_pending.store({});
        // Refresh must also recover from replaced session objects and an
        // earlier lookup made while their functions/assets were still loading.
        ResetBindings();
    }
    if(!command && Clock::now()<encounter_next_poll) return;
    encounter_next_poll=Clock::now()+std::chrono::milliseconds(500);
    Runtime runtime;
    if(!ReadRuntime(r,runtime)) {
        encounter_pending.store({});
        const std::vector<EncounterChoice> empty;
        EncounterStatus(localizer.Text("encounter.driver","Drive a taxi in the main scene, then refresh the encounter list."),nullptr,&empty);return;
    }
    if(!command) {
        const auto pending=encounter_pending.load();
        if(pending && pending->identity!=runtime.observation.identity) {
            encounter_pending.store({});EncounterStatus(localizer.Text("encounter.scope_changed","Player, scene or taxi changed. The preference was cancelled; refresh the list."));
        }
        return;
    }
    if(command->kind==EncounterCommandKind::Select) {
        if(command->identity!=runtime.observation.identity) {
            encounter_pending.store({});
            EncounterStatus(localizer.Text("encounter.scope_changed","Player, scene or taxi changed. The preference was cancelled; refresh the list."));return;
        }
        if(!ArmEncounter(r,runtime,command->character)) EncounterLog("Encounter preference was not armed");
        return;
    }
    std::vector<EncounterChoice> candidates;
    if(!ReadEncounterCandidates(r,runtime,candidates)) {
        candidates.clear();
        EncounterStatus(localizer.Text("encounter.read_failed","Could not verify the encounter list. No character preference was applied."),&runtime,&candidates);return;
    }
    EncounterStatus(candidates.empty() ? localizer.Text("encounter.empty","No owned characters are currently eligible for an encounter today.") :
        localizer.Text("encounter.ready","Select a character below before taking orders. Automatic pickup and delivery can stay enabled; finish dialogues in the game."),&runtime,&candidates);
    EncounterLog("Eligible encounter characters: "+std::to_string(candidates.size()));
}
void DrawEncounters(const AnomalyUiServiceV1* ui) {
    if(ui->separator) ui->separator(ui->user);
    ui->text(ui->user,StringView(localizer.Text("encounter.title","Encounter character")));
    const bool line=ui->struct_size>=offsetof(AnomalyUiServiceV1,same_line)+sizeof(ui->same_line) && ui->same_line;
    const bool input=ui->struct_size>=offsetof(AnomalyUiServiceV1,input_text)+sizeof(ui->input_text) && ui->input_text;
    if(input) {
        if(line && ui->struct_size>=offsetof(AnomalyUiServiceV1,set_next_item_width)+sizeof(ui->set_next_item_width) && ui->set_next_item_width)
            ui->set_next_item_width(ui->user,-138.f);
        if(ui->input_text(ui->user,StringView(localizer.Label("encounter.search","Search","encounter-search")),encounter_search.data(),encounter_search.size(),0)) encounter_page=0;
        if(line) ui->same_line(ui->user,0,8);
    }
    if(encounter_hook.id && ui->button && ui->button(ui->user,StringView(localizer.Label("encounter.refresh","Refresh","encounter-refresh")),0,0)) {
        encounter_page=0;
        encounter_commands.store(std::make_shared<const EncounterCommand>(EncounterCommand{EncounterCommandKind::Refresh}));
    }
    const auto pending=encounter_pending.load();
    if(const auto view=encounter_published.load()) {
        std::vector<const EncounterChoice*> matches;
        const std::string_view filter(encounter_search.data());
        for(const auto& choice:view->choices) {
            if(filter.empty() || (ui->filter_match ? ui->filter_match(ui->user,StringView(filter),StringView(choice.label))!=0 : choice.label.find(filter)!=std::string::npos)) matches.push_back(&choice);
        }
        constexpr size_t page_size=12,columns=3;
        const auto pages=std::max(size_t{1},(matches.size()+page_size-1)/page_size);
        encounter_page=std::min(encounter_page,pages-1);
        const auto page_button=[&](const std::string& label,bool active) {
            const bool extended=ui->struct_size>=offsetof(AnomalyUiServiceV1,button_enabled)+sizeof(ui->button_enabled) && ui->button_enabled;
            const bool clicked=extended ? ui->button_enabled(ui->user,StringView(label),0,0,active?1:0) :
                ui->button && ui->button(ui->user,StringView(label),0,0);
            return clicked && active;
        };
        if(page_button(localizer.Label("encounter.previous","Previous","encounter-previous"),encounter_page>0)) --encounter_page;
        if(line) ui->same_line(ui->user,0,10);
        const auto current=std::to_string(encounter_page+1),total=std::to_string(pages);
        ui->text(ui->user,StringView(localizer.Format("encounter.page","{0} / {1}",std::array<std::string_view,2>{current,total})));
        if(line) ui->same_line(ui->user,0,10);
        if(page_button(localizer.Label("encounter.next","Next","encounter-next"),encounter_page+1<pages)) ++encounter_page;
        const bool tables=ui->begin_table && ui->table_next_row && ui->table_next_column && ui->end_table;
        const bool table=tables && ui->begin_table(ui->user,StringView("encounter-role-grid"),static_cast<int32_t>(columns),ANOMALY_UI_TABLE_V1_NONE,0,0);
        if(!tables || table) {
            const auto begin=encounter_page*page_size,end=std::min(begin+page_size,matches.size());
            for(auto i=begin;i<end;++i) {
                if(table) {
                    if((i-begin)%columns==0) ui->table_next_row(ui->user);
                    ui->table_next_column(ui->user);
                }
                const auto& choice=*matches[i];
                int checked=pending && pending->identity==view->identity && Pack(pending->character)==Pack(choice.character);
                const auto label=choice.label+"##encounter-role-"+std::to_string(Pack(choice.character));
                if(ui->checkbox && ui->checkbox(ui->user,StringView(label),&checked))
                    encounter_commands.store(std::make_shared<const EncounterCommand>(EncounterCommand{checked?EncounterCommandKind::Select:EncounterCommandKind::Cancel,view->identity,choice.character}));
            }
        }
        if(table) ui->end_table(ui->user);
        if(matches.empty() && !view->choices.empty()) ui->text(ui->user,StringView(localizer.Text("encounter.no_matches","No matching characters.")));
        const auto selected=pending ? pending->label : localizer.Text("encounter.none","None");
        ui->text(ui->user,StringView(localizer.Format("encounter.selected","Selected: {0}",std::array<std::string_view,1>{selected})));
        if(ui->struct_size>=offsetof(AnomalyUiServiceV1,text_wrapped)+sizeof(ui->text_wrapped) && ui->text_wrapped) ui->text_wrapped(ui->user,StringView(view->status));
        else ui->text(ui->user,StringView(view->status));
    }
    if(pending && ui->button && ui->button(ui->user,StringView(localizer.Label("encounter.cancel","Cancel passenger preference","encounter-cancel")),0,0))
        encounter_commands.store(std::make_shared<const EncounterCommand>(EncounterCommand{EncounterCommandKind::Cancel}));
}
