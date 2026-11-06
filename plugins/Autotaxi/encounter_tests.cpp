#include "profile_test_setup.hpp"
#define TAXI_RUNTIME_FIXTURE_ONLY
#include "runtime_tests.cpp"
#undef TAXI_RUNTIME_FIXTURE_ONLY
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
constexpr std::uintptr_t base = 0x10000;
constexpr std::size_t exec_at = 0x200, getter_at = 0x400, selector_at = 0x800,
    free_at = 0x1800, allocator_at = 0x2000, function_at = 0x3000;

template<class T> void Put(anomaly::testing::ProfileHost& host, std::size_t at, const T& value) {
    Check(at <= host.image.size() && sizeof(value) <= host.image.size() - at, "fixture write overflow");
    std::memcpy(host.image.data() + at, &value, sizeof(value));
}
void Bytes(anomaly::testing::ProfileHost& host, std::size_t at,
           std::initializer_list<std::uint8_t> bytes) {
    Check(at <= host.image.size() && bytes.size() <= host.image.size() - at, "fixture bytes overflow");
    std::copy(bytes.begin(), bytes.end(), host.image.begin() + at);
}
void Call(anomaly::testing::ProfileHost& host, std::size_t at, std::size_t target) {
    Put(host, at, std::uint8_t{0xE8});
    Put(host, at + 1, static_cast<std::int32_t>(target - (at + 5)));
}
void AllocatorLoad(anomaly::testing::ProfileHost& host, std::size_t at, std::size_t target) {
    Bytes(host, at, {0x48,0x8B,0x0D});
    Put(host, at + 3, static_cast<std::int32_t>(target - (at + 7)));
}
void Replay(anomaly::testing::ProfileHost& host) {
    PrepareProfileHost(host);
    host.image.resize(0x5000);
    // Machine instructions independently observed in the game. Only relative
    // addresses are relocated into this controlled image; no Profile patterns
    // are used to generate the fixture or its expected targets.
    Bytes(host, getter_at, {
        0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xEC,0x30,
        0x33,0xC0,0x48,0x8B,0xE9,0x48,0x89,0x02,0x48,0x8B,0xFA,0x48,0x89,0x42,0x08,
        0x48,0x8B,0x89,0x68,0x27,0x00,0x00,0x48,0x85,0xC9});
    Bytes(host, getter_at + 0xCD, {0x48,0x85,0xDB,0x74,0x08,0x48,0x8B,0xCB});
    Call(host, getter_at + 0xD5, free_at);
    Bytes(host, selector_at, {
        0x48,0x89,0x54,0x24,0x10,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x8D,0x6C,0x24,0xD0,0x48,0x81,0xEC,0x30,0x01,0x00,0x00,0x45,0x33,0xE4,
        0x4C,0x8B,0xF1,0x48,0x8D,0x4A,0x18,0x4C,0x89,0x22});
    Bytes(host, selector_at + 0x41E, {0x41,0x8B,0x86,0x68,0xAE,0x00,0x00});
    Bytes(host, selector_at + 0x483, {0x4D,0x39,0xA6,0x68,0xAE,0x00,0x00});
    Bytes(host, selector_at + 0x621, {0x49,0x83,0xBE,0x68,0xAE,0x00,0x00,0x00});
    Bytes(host, selector_at + 0x672, {0x49,0x89,0x86,0x68,0xAE,0x00,0x00});
    Bytes(host, free_at, {
        0x48,0x85,0xC9,0x74,0x2E,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,
        0x48,0x8B,0x0D,0,0,0,0,0x48,0x85,0xC9,0x75,0x0C,0xE8,0,0,0,0,
        0x48,0x8B,0x0D,0,0,0,0,0x48,0x8B,0x01,0x48,0x8B,0xD3,0xFF,0x50,0x48,
        0x48,0x83,0xC4,0x20,0x5B,0xC3});
    AllocatorLoad(host, free_at + 0xD, allocator_at);
    AllocatorLoad(host, free_at + 0x1E, allocator_at);
    Bytes(host, exec_at, {
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x30,0x4C,0x8B,0x4A,0x20,
        0x33,0xC0,0x4D,0x85,0xC9,0x49,0x8B,0xF8,0x0F,0x95,0xC0,0x49,0x03,0xC1,
        0x48,0x89,0x42,0x20,0x48,0x8D,0x54,0x24,0x20});
    Call(host, exec_at + 0x25, getter_at);
    Bytes(host, exec_at + 0x2A, {0x48,0x8B,0xD8,0x48,0x3B,0xF8,0x74,0x2E,
        0x48,0x8B,0x0F,0x48,0x85,0xC9,0x74,0x05});
    Call(host, exec_at + 0x3A, free_at);
    Bytes(host, exec_at + 0x60, {0x48,0x8B,0x4C,0x24,0x20,0x48,0x85,0xC9,0x74,0x05});
    Call(host, exec_at + 0x6A, free_at);
    Bytes(host, exec_at + 0x6F, {0x48,0x8B,0x5C,0x24,0x40,0x48,0x83,0xC4,0x30,0x5F,0xC3});
    Put(host, function_at + 176, std::uint32_t{0x400});
    Put(host, function_at + 180, std::uint8_t{1});
    Put(host, function_at + 182, std::uint16_t{16});
    Put(host, function_at + 216, base + exec_at);
}
void ProfileReplay() {
    anomaly::testing::ProfileHost host("Autotaxi");
    Replay(host);
    Check(taxi::profile::Load(&host.api), "base taxi Profile rejected independent encounter replay");
    const auto doc = taxi::profile::snapshot.compatibility.Document();
    const auto resolve = [&](const anomaly::sdk::Profile& current) {
        const auto getter=anomaly::sdk::ResolveProfileSymbol(&host.signatures,&host.core,current,"Encounter.getter");
        const auto selector=anomaly::sdk::ResolveProfileSymbol(&host.signatures,&host.core,current,"Encounter.selector");
        return taxi::encounter::ValidateSelection(&host.core,current,getter.address,selector.address);
    };
    const auto resolved = resolve(doc);
    Check(static_cast<bool>(resolved), "encounter replay did not resolve");
    Check(resolved.getter == base + getter_at && resolved.selector == base + selector_at &&
        resolved.free == base + free_at && resolved.offset == 0xAE68 && resolved.result_size==40 && resolved.subpath_offset==24,
        "encounter targets differed from independently relocated instructions");
    Check(taxi::encounter::ValidateGetter(&host.core, doc, resolved, base + function_at),
        "reflected candidate getter did not match its reviewed native/allocator chain");
    Put(host, function_at + 182, std::uint16_t{24});
    Check(!taxi::encounter::ValidateGetter(&host.core, doc, resolved, base + function_at),
        "changed candidate array ABI was accepted");
    Put(host, function_at + 182, std::uint16_t{16});
    Call(host, exec_at + 0x6A, free_at + 1);
    Check(!taxi::encounter::ValidateGetter(&host.core, doc, resolved, base + function_at),
        "mismatched exec array allocator was accepted");
    Call(host, exec_at + 0x6A, free_at);
    Call(host, exec_at + 0x25, getter_at + 1);
    Check(!taxi::encounter::ValidateGetter(&host.core, doc, resolved, base + function_at),
        "another reflected getter reached encounter dispatch");
    Call(host, exec_at + 0x25, getter_at);
    Put(host, selector_at + 0x483 + 3, std::uint32_t{0xAE70});
    Check(!resolve(doc), "disagreeing private-member witnesses were accepted");
    Put(host, selector_at + 0x483 + 3, std::uint32_t{0xAE68});
    AllocatorLoad(host, free_at + 0x1E, allocator_at + 8);
    Check(!resolve(doc), "free allocator reads did not need the same allocator slot");
    AllocatorLoad(host, free_at + 0x1E, allocator_at);
    Put(host, free_at + 0x18, std::uint8_t{0x0D});
    Check(!resolve(doc), "changed allocator initialization branch was accepted");
    Put(host, free_at + 0x18, std::uint8_t{0x0C});
    Put(host, free_at + 0x2D, std::uint8_t{0x50});
    Check(!resolve(doc), "changed allocator dispatch slot was accepted");
    Put(host, free_at + 0x2D, std::uint8_t{0x48});
    std::copy_n(host.image.begin() + getter_at, 40, host.image.begin() + 0x4000);
    Check(!resolve(doc), "ambiguous candidate getter was accepted");
    Check(taxi::profile::Load(&host.api) && !taxi::profile::snapshot.encounter,
        "optional encounter mismatch broke ordinary AutoTaxi loading");
    std::fill_n(host.image.begin() + 0x4000, 40, std::uint8_t{});
    auto unsupported = doc;
    unsupported.layout.erase("EncounterOverride");
    Check(!resolve(unsupported),
        "missing private-field recipe fell back to a code offset");
    host.signatures.resolve = [](void*, AnomalyStringViewV1, AnomalyStringViewV1,
                                AnomalyStringViewV1, std::uintptr_t*) {
        Check(false, "Game-thread getter validation rescanned code");
        return anomaly::sdk::Ok();
    };
    Check(taxi::encounter::ValidateGetter(&host.core, doc, resolved, base + function_at),
        "cached reflection verification failed without scanning");
    Check(host.OutstandingHandles() == 0, "encounter Profile replay leaked document handles");
}

struct EncounterGame;
EncounterGame* encounter_game{};
void __fastcall CandidateNative(void* object, void* function, void* parameters);
void* __fastcall PassengerNative(void* player, void* output);
void __fastcall ReleaseCandidateArray(void* memory);
struct EncounterGame {
    fixture::Game game;
    std::vector<FName> native_candidates;
    std::map<uint64_t, int64_t> owned;
    FName first{}, second{}, unowned{}, ordinary{}, third{};
    uintptr_t getter{}, amount{}, inventory{}, candidate_data{}, native_image{};
    uintptr_t dialogue_getter{},selection_subpath{};
    uintptr_t convert_name{}, name_cdo{}, get_asset{}, asset_cdo{}, current_asset{};
    FName asset_name{};
    uintptr_t get_game_data{}, game_data{}, data_cdo{}, item_type{};
    uintptr_t first_text{}, second_text{}, failed_text{};
    std::map<uintptr_t,std::string> display_text;
    std::set<uintptr_t> text_failures;
    unsigned text_queries{}, game_data_queries{};
    unsigned getter_calls{}, releases{}, original_calls{}, writes{}, hook_releases{}, begins{}, ends{};
    unsigned string_releases{};
    uintptr_t last_freed{};
    FName observed_preference{};
    enum class NativeResult { Consume, Leave, Replace } result{NativeResult::Consume};
    AnomalyHookServiceV1 hooks{sizeof(hooks), 1, this};
    AnomalyUe5FrameworkServiceV1 game_thread{sizeof(game_thread), 1, this};
    AnomalyUe5BuildServiceV1 build{sizeof(build),1,this};
    decltype(AnomalyCoreServiceV1::read_memory) old_read{};
    bool on_game_thread{true}, deny_lease{},dialogue{},return_subpath{};
    static constexpr size_t override_offset = 0x4A0, save_offset = 0x420;

    EncounterGame() {
        LoadTestProfile();
        encounter_game = this;
        old_read = game.memory.read_memory;
        game.memory.read_memory = [](void* user, uintptr_t address, AnomalyMutableByteSpanV1 output) {
            const auto& self = *encounter_game;
            if (address == reinterpret_cast<uintptr_t>(&CandidateNative))
                return anomaly::testing::RecordedProcessEventEntry(output);
            if (address >= self.native_image && address - self.native_image <= 0x5000 &&
                output.size <= 0x5000 - (address - self.native_image)) {
                std::memcpy(output.data, reinterpret_cast<void*>(address), output.size);
                return anomaly::sdk::Ok();
            }
            return self.old_read(user, address, output);
        };
        game.memory.write_memory = [](void* user, uintptr_t address, AnomalyByteSpanV1 source) {
            auto& self = *encounter_game;
            for (const auto& block : static_cast<fixture::Game*>(user)->storage) {
                const auto start = reinterpret_cast<uintptr_t>(block.data());
                if (address >= start && address - start <= block.size() && source.size <= block.size() - (address - start)) {
                    std::memcpy(reinterpret_cast<void*>(address), source.data, source.size);
                    ++self.writes;
                    return anomaly::sdk::Ok();
                }
            }
            return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND, 0, {}};
        };
        game_thread.is_game_thread = [](void* user) { return static_cast<EncounterGame*>(user)->on_game_thread ? 1 : 0; };
        framework = &game_thread;
        build.feature_state=[](void*,AnomalyStringViewV1) {return uint32_t{ANOMALY_FEATURE_V1_AVAILABLE};};build_service=&build;
        game.Put(game.native_table + 0x4C * 8, reinterpret_cast<uintptr_t>(&CandidateNative));

        anomaly::testing::ProfileHost recorded("Autotaxi");
        Replay(recorded);
        native_image = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 0x5000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        Check(native_image != 0, "could not allocate independent native fixture");
        std::memcpy(reinterpret_cast<void*>(native_image), recorded.image.data(), recorded.image.size());
        // A controlled executable release callback records allocator ownership.
        // The reviewed exec/free relative targets stay inside the same image.
        std::array<uint8_t,14> jump{0xFF,0x25,0,0,0,0};
        const auto release = reinterpret_cast<uintptr_t>(&ReleaseCandidateArray);
        std::memcpy(jump.data()+6, &release, sizeof(release));
        std::memcpy(reinterpret_cast<void*>(native_image + free_at), jump.data(), jump.size());
        const auto detour=reinterpret_cast<uintptr_t>(&EncounterDetour);
        std::memcpy(jump.data()+6,&detour,sizeof(detour));
        std::memcpy(reinterpret_cast<void*>(native_image+selector_at),jump.data(),jump.size());
        DWORD old_protect{};
        Check(VirtualProtect(reinterpret_cast<void*>(native_image), 0x5000, PAGE_EXECUTE_READ, &old_protect) != 0,
            "could not activate controlled release callback");
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(native_image), 0x5000);
        taxi::profile::snapshot.encounter = {native_image + exec_at, native_image + getter_at,
            native_image + selector_at, native_image + free_at, override_offset,40,24};

        first = game.Name("Character_A"); second = game.Name("Character_B");
        unowned = game.Name("Character_Unowned"); ordinary = game.Name("Ordinary_NPC"); third = game.Name("Other_Preference");
        owned = {{Pack(first),1}, {Pack(second),1}, {Pack(ordinary),1}};
        native_candidates = {first, first, unowned, ordinary, second, {}};
        candidate_data = game.Buffer(256 * sizeof(FName));
        selection_subpath=game.Buffer(8);
        dialogue_getter=game.Function("HTGame","HTPlayerController","IsInDialogue",1,1,{{"ReturnValue",0,1,"BoolProperty"}});
        uintptr_t ps_class{};
        std::memcpy(&ps_class, reinterpret_cast<void*>(game.ps + 16), sizeof(ps_class));
        const auto inv = game.Object("HTInventoryComponent", 128);
        inventory = inv.first;
        game.Property(ps_class,"InventoryComponent",0x400,8); game.Put(game.ps+0x400,inventory);
        game.Property(ps_class,"TaxiGamePlaySaveData",save_offset,40,"StructProperty");

        const auto asset = game.Object("TaxiGameplayDataAsset",0x100,
            "/Game/DataAssets/DataAssetSet/TaxiGameplay/DA_TaxiGameplayDataAsset.DA_TaxiGameplayDataAsset");
        current_asset=asset.first;
        asset_name=game.Name("DA_TaxiGameplay");
        name_cdo=game.Object("KismetStringLibrary",64,"/Script/Engine.Default__KismetStringLibrary").first;
        asset_cdo=game.Object("HTDataAssetSet",64,"/Script/HTGame.Default__HTDataAssetSet").first;
        convert_name=game.Function("Engine","KismetStringLibrary","Conv_StringToName",2,24,
            {{"InString",0,16,"StrProperty"},{"ReturnValue",16,8,"NameProperty"}});
        get_asset=game.Function("HTGame","HTDataAssetSet","K2_GetDataAsset",2,16,
            {{"AssetName",0,8,"NameProperty"},{"ReturnValue",8,8,"ObjectProperty"}});
        const auto table = game.Object("DataTable",0x100);
        const auto type = game.Object("ScriptStruct",128,"/Script/HTGame.TaxiGameplayPassengerNPCData");
        game.Put(type.first+24,game.Name("TaxiGameplayPassengerNPCData"));
        game.Property(type.first,"CharacterID",8,8,"NameProperty");
        game.Property(type.first,"AITemplate",16,40,"SoftClassProperty");
        game.Property(type.first,"bIsCharacterNPC",56,1,"BoolProperty");
        game.Property(table.second,"RowStruct",40,8); game.Put(table.first+40,type.first);
        game.Property(asset.second,"DT_TaxiGameplayPassengerNPCTable",0x80,8); game.Put(asset.first+0x80,table.first);
        const auto slots = game.Buffer(5*24);
        const std::array ids{first,unowned,ordinary,second};
        for (size_t i=0;i<ids.size();++i) {
            const auto row = game.Buffer(64);
            game.Put(row+8,ids[i]); game.Put(row+56,uint8_t(i==2?0:1));
            game.Put(slots+24*(i+1)+8,row);
        }
        fixture::WidgetMap rows{}; rows.slots={slots,5,5}; rows.bits[0]=0x1E;
        rows.bit_count=5; rows.max_bits=128; rows.free_count=1; game.Put(table.first+48,rows);

        getter=game.Function("HTGame","HTPlayerState","GetTaxiGameplayFortuitousMeetingCharacter",1,16,
            {{"ReturnValue",0,16,"ArrayProperty"}});
        game.Put(getter+216,native_image+exec_at);
        amount=game.Function("HTGame","HTInventoryComponent","FastGetItemTotalAmount",4,24,
            {{"ItemID",0,8,"NameProperty"},{"ItemType",8,1,"EnumProperty"},
             {"ContainsBind",9,1,"BoolProperty"},{"ReturnValue",16,8,"Int64Property"}});
        game.data.stage=taxi::Stage::Idle;
        game.Put(game.ps+save_offset,game.data);
        hooks.create=[](void*,const AnomalyHookRequestV1* request,uintptr_t* original,AnomalyGenerationHandleV1* handle) {
            Check(request->target==encounter_game->native_image+selector_at &&
                request->detour==reinterpret_cast<void*>(&EncounterDetour),"wrong native selector was hooked");
            *original=reinterpret_cast<uintptr_t>(&PassengerNative); *handle={456,1}; return anomaly::sdk::Ok();
        };
        hooks.release=[](void* user,AnomalyGenerationHandleV1 handle) {
            auto& self=*static_cast<EncounterGame*>(user);
            Check(handle.id==456,"wrong encounter hook was released"); ++self.hook_releases; return anomaly::sdk::Ok();
        };
        hooks.begin_callback=[](void* user,AnomalyGenerationHandleV1 hook,AnomalyGenerationHandleV1* lease) {
            auto& self=*static_cast<EncounterGame*>(user);
            Check(hook.id==456,"callback used another hook scope");
            if(self.deny_lease) return AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE,0,{}};
            ++self.begins; *lease={789,1}; return anomaly::sdk::Ok();
        };
        hooks.end_callback=[](void* user,AnomalyGenerationHandleV1 lease) {
            auto& self=*static_cast<EncounterGame*>(user);
            Check(lease.id==789,"wrong encounter callback lease was drained"); ++self.ends; return anomaly::sdk::Ok();
        };
        encounter_hooks=&hooks;
        StartEncounters();
    }
    ~EncounterGame() {
        StopEncounters(); framework=nullptr; build_service=nullptr; encounter_hooks=nullptr;
        if(native_image) VirtualFree(reinterpret_cast<void*>(native_image),0,MEM_RELEASE);
        encounter_game=nullptr;
    }
    Runtime RuntimeNow() {
        Runtime runtime;
        Check(ReadRuntime(Reader{names},runtime),"local taxi runtime was unavailable to encounter fixture");
        return runtime;
    }
    void EnableLabels() {
        const auto cdo=game.Object("HTGameData",0x100,"/Script/HTGame.Default__HTGameData");
        data_cdo=cdo.first;
        const auto actual=game.Object("HTGameData",0x100);
        game_data=actual.first;
        const auto table=game.Object("DataTable",0x100);
        const auto type=game.Object("ScriptStruct",128);
        item_type=type.first;
        game.Put(type.first+24,game.Name("StaticItemData"));
        // Reflected offsets deliberately differ from the current game's dump.
        game.Property(type.first,"ItemName",0x30,16,"TextProperty");
        game.Property(table.second,"RowStruct",40,8);game.Put(table.first+40,type.first);
        game.Property(actual.second,"CharacterDataTable",0x80,8);game.Put(game_data+0x80,table.first);
        // The observed generic item table has no numeric character rows. A
        // regression to ItemDataTable must retain IDs and fail name assertions.
        const auto generic=game.Object("DataTable",0x100);
        game.Property(generic.second,"RowStruct",40,8);game.Put(generic.first+40,type.first);
        fixture::WidgetMap empty_items{};empty_items.max_bits=128;game.Put(generic.first+48,empty_items);
        game.Property(actual.second,"ItemDataTable",0x88,8);game.Put(game_data+0x88,generic.first);
        // The CDO has no useful item data; the native return object must be used.
        game.Property(cdo.second,"CharacterDataTable",0x80,8);
        const auto rows=game.Buffer(5*24);
        const auto row=[&](size_t slot,FName id,const std::string& text) {
            const auto value=game.Buffer(0x50);
            game.Put(rows+slot*24,id);game.Put(rows+slot*24+8,value);
            display_text[value+0x30]=text;
            return value+0x30;
        };
        row(0,first,"unallocated-hole");
        first_text=row(1,first,"\xE5\xA8\x9C\xE5\xA8\x9C\xE8\x8E\x89");
        row(2,second,"another-unallocated-hole");
        second_text=row(3,second,"\xC3\x89lys / \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88");
        failed_text=row(4,third,"unreadable-label");text_failures.insert(failed_text);
        fixture::WidgetMap map{};map.slots={rows,5,5};map.bits[0]=0x1A;
        map.bit_count=5;map.max_bits=128;map.free_count=2;game.Put(table.first+48,map);
        get_game_data=game.Function("HTGame","HTGameData","GetGameData",1,8,
            {{"ReturnValue",0,8,"ObjectProperty"}});
        game.name_service.resolve_ftext_utf8=[](void*,uintptr_t address,char* out,size_t* size) {
            auto& self=*encounter_game;++self.text_queries;
            const auto found=self.display_text.find(address);
            if(found==self.display_text.end() || self.text_failures.contains(address))
                return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
            const auto required=found->second.size()+1;
            if(!out) {*size=required;return anomaly::sdk::Ok();}
            if(*size<required) {*size=required;return AnomalyStatusV1{ANOMALY_STATUS_V1_BUFFER_TOO_SMALL,0,{}};}
            std::memcpy(out,found->second.c_str(),required);*size=required;return anomaly::sdk::Ok();
        };
    }
    FName Preference() const {
        FName value{}; std::memcpy(&value,reinterpret_cast<void*>(game.ps+override_offset),sizeof(value)); return value;
    }
};
void __fastcall ReleaseCandidateArray(void* memory) {
    if(reinterpret_cast<uintptr_t>(memory)==encounter_game->selection_subpath) {++encounter_game->string_releases;return;}
    ++encounter_game->releases;
    encounter_game->last_freed=reinterpret_cast<uintptr_t>(memory);
}
void __fastcall CandidateNative(void* object,void* function,void* parameters) {
    auto& self=*encounter_game;
    const auto fn=reinterpret_cast<uintptr_t>(function);
    if(fn==self.dialogue_getter) {
        Check(reinterpret_cast<uintptr_t>(object)==self.game.pc,"dialogue queried another controller");
        *static_cast<uint8_t*>(parameters)=self.dialogue?1:0;
    } else if(fn==self.convert_name) {
        struct NameQuery {RowArray input{};FName result{};};
        auto& query=*static_cast<NameQuery*>(parameters);
        Check(reinterpret_cast<uintptr_t>(object)==self.name_cdo && query.input.count==16 &&
            query.input.capacity==16 && std::wstring(reinterpret_cast<const wchar_t*>(query.input.data))==L"DA_TaxiGameplay",
            "taxi asset key conversion used the wrong receiver or game identifier");
        query.result=self.asset_name;
    } else if(fn==self.get_asset) {
        struct AssetQuery {FName name{};uintptr_t result{};};auto& query=*static_cast<AssetQuery*>(parameters);
        Check(reinterpret_cast<uintptr_t>(object)==self.asset_cdo && Pack(query.name)==Pack(self.asset_name),
            "session taxi asset lookup used the wrong data-asset key");
        query.result=self.current_asset;
    } else if(fn==self.get_game_data && self.get_game_data) {
        Check(reinterpret_cast<uintptr_t>(object)==self.data_cdo,"game data lookup bypassed its static CDO");
        ++self.game_data_queries;*static_cast<uintptr_t*>(parameters)=self.game_data;
    } else if(fn==self.getter) {
        Check(reinterpret_cast<uintptr_t>(object)==self.game.ps,"candidate getter queried another player");
        ++self.getter_calls;
        std::memcpy(reinterpret_cast<void*>(self.candidate_data),self.native_candidates.data(),self.native_candidates.size()*sizeof(FName));
        *static_cast<RowArray*>(parameters)={self.candidate_data,static_cast<int32_t>(self.native_candidates.size()),256};
    } else if(fn==self.amount) {
        Check(reinterpret_cast<uintptr_t>(object)==self.inventory,"ownership query used another inventory");
        const auto* bytes=static_cast<uint8_t*>(parameters);
        Check(bytes[8]==3 && bytes[9]==1,"character ownership query used another item category");
        FName id{};std::memcpy(&id,bytes,sizeof(id));
        const auto found=self.owned.find(Pack(id));
        const int64_t total=found==self.owned.end()?0:found->second;
        std::memcpy(static_cast<uint8_t*>(parameters)+16,&total,sizeof(total));
    } else fixture::Native(object,function,parameters);
}
void* __fastcall PassengerNative(void* player,void* output) {
    auto& self=*encounter_game; ++self.original_calls;
    self.observed_preference=self.Preference();
    if(reinterpret_cast<uintptr_t>(player)==self.game.ps && Pack(self.observed_preference)) {
        if(self.result==EncounterGame::NativeResult::Consume) {
            self.game.data.character=self.observed_preference;
            self.game.Put(self.game.ps+EncounterGame::save_offset,self.game.data);
            self.game.Put(self.game.ps+EncounterGame::override_offset,FName{});
        } else if(self.result==EncounterGame::NativeResult::Replace)
            self.game.Put(self.game.ps+EncounterGame::override_offset,self.third);
    }
    auto& selected=*static_cast<EncounterSelectionResult*>(output);
    selected.weak=0xFEED1234;selected.package=self.first;selected.asset=self.second;
    if(self.return_subpath) selected.subpath={self.selection_subpath,4,8};
    return output;
}
void NativePreferenceContracts() {
    EncounterGame self; const Reader reader{names}; auto runtime=self.RuntimeNow();
    Check(encounter_hook.id && !encounter_pending.load() && self.writes==0,
        "startup armed or wrote an encounter preference");
    std::vector<EncounterChoice> choices;
    Check(ReadEncounterCandidates(reader,runtime,choices) && choices.size()==2 &&
        Pack(choices[0].character)==Pack(self.first) && Pack(choices[1].character)==Pack(self.second),
        "native candidates were not deduplicated and intersected with owned character NPCs");
    Check(self.getter_calls==1 && self.releases==1 && self.last_freed==self.candidate_data,
        "native candidate array was not released once through the game allocator");
    Check(!ArmEncounter(reader,runtime,self.unowned) && !encounter_pending.load(),
        "unowned role could be selected by stale or arbitrary ID");
    self.game.data.stage=taxi::Stage::Pickup; runtime=self.RuntimeNow();
    Check(!ArmEncounter(reader,runtime,self.first),"preference armed after the passenger was generated");
    self.game.data.stage=taxi::Stage::Idle; runtime=self.RuntimeNow();
    Check(ArmEncounter(reader,runtime,self.first) && encounter_pending.load() && self.writes==0,
        "arming wrote a long-lived player preference");
    EncounterSelectionResult output{};
    Check(EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output)==&output && output.weak==0xFEED1234 &&
        self.original_calls==1 && Pack(self.observed_preference)==Pack(self.first) && !Pack(self.Preference()) &&
        !encounter_pending.load(),"native selector did not consume one local preference exactly once");
    Check(self.game.requests.empty() && self.game.settlements==0 && confirmed.added==0,
        "character selection fabricated taxi completion or settlement");
    Check(self.begins==self.ends,"selector callback lease was not drained");

    self.game.data.character={}; self.result=EncounterGame::NativeResult::Leave;
    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"second one-shot preference did not arm");
    const auto calls=self.original_calls; EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output);
    Check(self.original_calls==calls+1 && !Pack(self.Preference()) && !encounter_pending.load(),
        "unconsumed preference survived its native invocation or repeated the selector");

    self.result=EncounterGame::NativeResult::Replace;
    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"replacement-result preference did not arm");
    EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output);
    Check(Pack(self.Preference())==Pack(self.third),"cleanup overwrote another value left by the native selector");
    self.game.Put(self.game.ps+EncounterGame::override_offset,self.third);
    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"conflicting native preference test did not arm");
    const auto writes=self.writes; self.result=EncounterGame::NativeResult::Leave;
    EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output);
    Check(self.writes==writes && Pack(self.Preference())==Pack(self.third) && !encounter_pending.load(),
        "an existing native preference was overwritten");
    self.game.Put(self.game.ps+EncounterGame::override_offset,FName{});

    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"scope test did not arm");
    const auto foreign_calls=self.original_calls;
    EncounterDetour(reinterpret_cast<void*>(0x12340),&output);
    Check(self.original_calls==foreign_calls+1 && encounter_pending.load() && !Pack(self.Preference()),
        "another player's selector consumed or wrote the local preference");
    self.deny_lease=true; const auto denied_calls=self.original_calls;
    const auto denied_writes=self.writes;
    Check(EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output)==&output &&
        self.original_calls==denied_calls+1 && encounter_pending.load() && self.writes==denied_writes,
        "denied callback lease blocked the native selector or applied plugin work");
    self.deny_lease=false;
    self.on_game_thread=false;
    EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output);
    Check(encounter_pending.load() && !Pack(self.Preference()),"non-game thread applied the preference");
    self.on_game_thread=true;
    // The native getter's next reply represents a character becoming ineligible
    // (including the game removing a character already encountered today).
    self.native_candidates={self.second};
    EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output);
    Check(!encounter_pending.load() && !Pack(self.Preference()),"stale eligibility was not rechecked before NPC selection");

    self.native_candidates={self.first,self.second};
    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"cancel test did not arm");
    encounter_commands.store(std::make_shared<const EncounterCommand>(EncounterCommand{EncounterCommandKind::Cancel}));
    UpdateEncounters(reader);
    Check(!encounter_pending.load() && !Pack(self.Preference()),"cancel left a preference in the player");
    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"identity-change test did not arm");
    auto stale=std::make_shared<EncounterPreference>(*encounter_pending.load()); ++stale->identity[7];
    encounter_pending.store(stale); encounter_next_poll={}; UpdateEncounters(reader);
    Check(!encounter_pending.load() && !Pack(self.Preference()),"world/player/vehicle identity change retained the preference");
    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"stop test did not arm");
    Check(StopEncounters() && !encounter_pending.load() && !encounter_hook.id && !encounter_original && self.hook_releases==1,
        "plugin stop retained selection intent or its hook");
    Check(self.getter_calls==self.releases,"candidate queries leaked an array on a rejected selection path");
    self.game.data.stage=taxi::Stage::Pickup; self.game.data.character=self.first;
    runtime=self.RuntimeNow(); auto ordinary_order=runtime; ordinary_order.data.character={};
    Check(!SameOrder(runtime,ordinary_order),"a newly assigned character passenger was ignored by dispatch identity");
    Check(Submit(reader,runtime,taxi::Action::Pickup,intent.load())==Dispatch::Sent && self.game.requests.size()==1,
        "assigned character prevented native automatic pickup");
    self.dialogue=true;
    Check(Submit(reader,runtime,taxi::Action::Pickup,intent.load())==Dispatch::Dialogue && self.game.requests.size()==1,
        "automatic pickup advanced a character dialogue");
    Check(self.begins==self.ends,"selector callback lease remained live after rejection paths");
}
void LocalizedLabelContracts() {
    EncounterGame self;self.EnableLabels();const Reader reader{names};
    const auto unknown=self.game.Name("Unknown_Character_ID");
    const auto raw=[&] {
        return std::vector<EncounterChoice>{{self.first,"Character_A"},{self.second,"Character_B"},
            {unknown,"Unknown_Character_ID"},{self.third,"Other_Preference"}};
    };
    auto labels=raw();ResolveEncounterLabels(reader,labels);
    Check(labels.size()==4 && Pack(labels[0].character)==Pack(self.first) &&
        Pack(labels[1].character)==Pack(self.second),"resolving labels changed candidate identity or count");
    Check(labels[0].label==self.display_text.at(self.first_text) &&
        labels[1].label==self.display_text.at(self.second_text),
        "game character names lost UTF-8 or came from an unallocated sparse slot");
    Check(labels[2].label=="Unknown_Character_ID" && labels[3].label=="Other_Preference",
        "unknown or unreadable item names did not retain the original role ID");
    Check(self.game_data_queries==1 && self.releases==0,
        "persistent data-table text created an owned game array or skipped the GameData getter");
    const auto resolver=self.game.name_service.resolve_ftext_utf8;
    self.game.name_service.resolve_ftext_utf8=nullptr;
    labels=raw();const auto before=self.text_queries;ResolveEncounterLabels(reader,labels);
    Check(labels[0].label=="Character_A" && labels[1].label=="Character_B" && self.text_queries==before,
        "missing FText service changed fallback labels or invoked a null callback");
    self.game.name_service.resolve_ftext_utf8=resolver;
    const auto full_size=self.game.name_service.struct_size;
    self.game.name_service.struct_size=offsetof(AnomalyUe5NamesServiceV1,resolve_ftext_utf8);
    labels=raw();ResolveEncounterLabels(reader,labels);
    Check(labels[0].label=="Character_A" && self.text_queries==before,
        "truncated Names service ABI reached FText resolution");
    self.game.name_service.struct_size=full_size;
    self.game.Put(self.item_type+24,self.game.Name("WrongItemStruct"));
    labels=raw();ResolveEncounterLabels(reader,labels);
    Check(labels[0].label=="Character_A","another data-table row struct was parsed as StaticItemData");
    self.game.Put(self.item_type+24,self.game.Name("StaticItemData"));

    std::vector<EncounterChoice> candidates;
    Check(ReadEncounterCandidates(reader,self.RuntimeNow(),candidates) && candidates.size()==2 &&
        candidates[0].label==self.display_text.at(self.first_text) &&
        candidates[1].label==self.display_text.at(self.second_text),
        "eligible candidate refresh did not resolve actual game character names");
    // Identical translated names must not redirect a choice to another role.
    self.display_text[self.first_text]="Shared display name";
    self.display_text[self.second_text]="Shared display name";
    Check(ArmEncounter(reader,self.RuntimeNow(),self.second),"localized role choice did not arm");
    const auto pending=encounter_pending.load();
    Check(pending && Pack(pending->character)==Pack(self.second) && pending->label=="Shared display name",
        "arming a display label changed the underlying selected FName");
    EncounterSelectionResult output{};EncounterDetour(reinterpret_cast<void*>(self.game.ps),&output);
    Check(Pack(self.observed_preference)==Pack(self.second) && !encounter_pending.load() &&
        !Pack(self.Preference()),"duplicate display names selected the wrong role or retained preference state");
    Check(encounter_published.load()->status.find("Shared display name")!=std::string::npos,
        "native passenger confirmation lost the selected game's display name");
}
void AutomaticEncounterContracts() {
    {
        EncounterGame self;const Reader reader{names};
        Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"automatic encounter did not arm before taking orders");
        self.game.data.stage=taxi::Stage::Pickup;
        const auto tick=[] {next_poll={};Update(nullptr,.5);};
        tick();
        Check(self.original_calls==1 && self.game.requests.empty() && Pack(self.game.data.character)==Pack(self.first) &&
            !encounter_pending.load() && assigned_encounter,"automatic pickup skipped preferred role assignment");
        EncounterSelectionResult spawn{};
        EncounterDetour(reinterpret_cast<void*>(self.game.ps),&spawn);
        Check(self.original_calls==1 && Pack(spawn.package)==Pack(self.first) && !spawn.subpath.data,
            "a later NPC spawn rerolled or re-recorded the assigned character");
        tick();tick();
        Check(self.game.requests.size()==1 && self.game.requests[0].state==3,"assigned role did not allow exactly one automatic pickup request");
        self.game.data.stage=taxi::Stage::Destination;self.dialogue=true;tick();
        Check(self.game.requests.size()==1 && !run.failed,"pickup dialogue was skipped or timed out");
        self.dialogue=false;tick();
        Check(self.game.requests.size()==2 && self.game.requests.back().state==5,"delivery did not resume after role dialogue");
        self.game.data.stage=taxi::Stage::SpecialEvent;self.dialogue=true;tick();
        Check(self.game.requests.size()==2 && !run.failed,"special passenger event was advanced by automation");
        self.dialogue=false;self.game.data.stage=taxi::Stage::Destination;tick();
        Check(self.game.requests.size()==3 && self.game.requests.back().state==5 && !run.failed,
            "returning from a passenger event did not resume delivery");
        self.game.data.stage=taxi::Stage::Settle;tick();tick();
        Check(self.game.settlements==1 && self.original_calls==1,"character settlement or assignment was repeated");
        self.game.data.stage=taxi::Stage::Waiting;self.game.data.character={};self.game.Put(self.game.ps+0x380,int32_t{11});tick();
        Check(published.load()->completed==1 && self.getter_calls==self.releases,"native completion acknowledgement or candidate cleanup was lost");
    }
    {
        EncounterGame self;const Reader reader{names};
        Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"failed assignment fixture could not arm");
        self.game.data.stage=taxi::Stage::Pickup;self.result=EncounterGame::NativeResult::Leave;
        const auto tick=[] {next_poll={};Update(nullptr,.5);};tick();tick();
        Check(run.failed && self.original_calls==1 && self.game.requests.empty() && !Pack(self.Preference()),
            "unconfirmed character assignment fell through to ordinary automatic pickup or retried");
    }
    {
        EncounterGame self;const Reader reader{names};
        Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"temporary-path fixture could not arm");
        self.return_subpath=true;self.game.data.stage=taxi::Stage::Pickup;
        next_poll={};Update(nullptr,.5);
        Check(self.string_releases==1 && self.game.requests.empty() && run.failed,
            "unsupported nested class path leaked its engine buffer or advanced the order");
    }
}
void RelogRefreshContracts() {
    EncounterGame self;const Reader reader{names};
    const auto refresh=[] {
        encounter_commands.store(std::make_shared<const EncounterCommand>(EncounterCommand{EncounterCommandKind::Refresh}));
        UpdateEncounters(Reader{names});
    };
    // A first attempt during login can see a native wrapper before it is
    // available. A deliberate retry must not remain in the old miss cache.
    const std::string asset_getter="/Script/HTGame.HTDataAssetSet.K2_GetDataAsset";
    const auto getter_handle=self.game.paths.at(asset_getter);
    self.game.paths.erase(asset_getter);
    refresh();
    Check(encounter_published.load()->choices.empty(),"unavailable session asset exposed a partial candidate list");
    self.game.paths[asset_getter]=getter_handle;
    refresh();
    Check(encounter_published.load()->choices.size()==2,"refresh remained stuck on an earlier login-time lookup failure");
    const auto before=encounter_published.load()->identity;
    Check(ArmEncounter(reader,self.RuntimeNow(),self.first),"relog fixture could not arm the previous player's choice");

    const std::string asset_path="/Game/DataAssets/DataAssetSet/TaxiGameplay/DA_TaxiGameplayDataAsset.DA_TaxiGameplayDataAsset";
    const auto old_asset=Find(reader,asset_path);uintptr_t table{};
    Check(reader.Read(old_asset+0x80,table) && table,"previous-session passenger table was missing");
    const auto replacement=self.game.Object("TaxiGameplayDataAsset",0x100);
    self.game.Property(replacement.second,"DT_TaxiGameplayPassengerNPCTable",0x80,8);
    self.game.Put(replacement.first+0x80,table);self.current_asset=replacement.first;
    self.game.Put(old_asset+0x80,uintptr_t{});
    // The old package-path object deliberately remains resolvable. The public
    // session asset getter, rather than its address, identifies current data.
    Check(Find(reader,asset_path)==old_asset,"fixture did not retain its obsolete package-path object");

    const auto old_player=self.game.ps;
    const auto player=self.game.Object("HTPlayerState",0x600).first;
    int32_t new_index{};std::memcpy(&new_index,reinterpret_cast<void*>(player+12),sizeof(new_index));
    std::memcpy(reinterpret_cast<void*>(player),reinterpret_cast<void*>(old_player),0x600);
    self.game.Put(player+12,new_index);self.game.ps=player;self.game.Put(self.game.pc+0x300,player);
    refresh();
    const auto current=encounter_published.load();
    Check(current->choices.size()==2 && current->identity!=before && current->identity==self.RuntimeNow().observation.identity,
        "relog refresh did not use the new player and current session asset");
    Check(!encounter_pending.load() && self.writes==0 && self.original_calls==0 && self.getter_calls==self.releases,
        "relog refresh retained a previous-session choice, changed gameplay or leaked a returned array");
}
void PickerUiContracts() {
    EncounterGame self;
    auto view=std::make_shared<EncounterView>();view->identity={10,1,20,1,30,1,40,1};view->status="Ready";
    for(int i=1;i<=16;++i) view->choices.push_back({self.game.Name("id_"+std::to_string(i)),"Role "+std::to_string(i)});
    encounter_published.store(view);
    struct Probe {
        std::vector<std::pair<std::string,int>> toggles;
        std::vector<std::string> texts;
        std::string click_button,click_role,search;
        int columns{},rows{},tables{},ends{};bool change_search{},inside{};
        bool Click(const std::string& label) {
            if(click_button.empty() || !label.ends_with(click_button)) return false;
            click_button.clear();return true;
        }
        void Frame() {toggles.clear();texts.clear();columns=rows=tables=ends=0;inside=false;}
    } probe;
    AnomalyUiServiceV1 ui{sizeof(ui),1,&probe};
    ui.text=[](void* user,AnomalyStringViewV1 value) {static_cast<Probe*>(user)->texts.emplace_back(value.data,value.size);};
    ui.text_wrapped=ui.text;
    ui.input_text=[](void* user,AnomalyStringViewV1,char* buffer,size_t capacity,uint32_t) {
        auto& p=*static_cast<Probe*>(user);if(!p.change_search)return 0;p.change_search=false;
        Check(p.search.size()<capacity,"search fixture exceeded its public input buffer");
        std::memcpy(buffer,p.search.c_str(),p.search.size()+1);return 1;
    };
    ui.button=[](void* user,AnomalyStringViewV1 label,float,float) {return static_cast<Probe*>(user)->Click({label.data,label.size})?1:0;};
    ui.button_enabled=[](void* user,AnomalyStringViewV1 label,float,float,int active) {
        return active && static_cast<Probe*>(user)->Click({label.data,label.size})?1:0;
    };
    ui.same_line=[](void*,float,float) {};
    ui.set_next_item_width=[](void*,float) {};
    ui.begin_table=[](void* user,AnomalyStringViewV1,int32_t columns,uint32_t,float,float) {
        auto& p=*static_cast<Probe*>(user);p.columns=columns;++p.tables;p.inside=true;return 1;
    };
    ui.table_next_row=[](void* user) {++static_cast<Probe*>(user)->rows;};
    ui.table_next_column=[](void*) {return 1;};
    ui.end_table=[](void* user) {auto& p=*static_cast<Probe*>(user);Check(p.inside,"picker ended an unopened table");p.inside=false;++p.ends;};
    ui.checkbox=[](void* user,AnomalyStringViewV1 label,int* checked) {
        auto& p=*static_cast<Probe*>(user);Check(p.inside,"role switch was drawn outside the grid");
        std::string value(label.data,label.size);p.toggles.emplace_back(value,*checked);
        if(p.click_role.empty() || !value.ends_with(p.click_role)) return 0;
        p.click_role.clear();*checked=!*checked;return 1;
    };
    const auto draw=[&] {probe.Frame();DrawEncounters(&ui);};
    draw();
    Check(probe.columns==3 && probe.rows==4 && probe.toggles.size()==12 && probe.tables==probe.ends,
        "the first picker page was not twelve switches in three columns");
    Check(probe.toggles.front().first.starts_with("Role 1##") && probe.toggles.back().first.starts_with("Role 12##"),
        "the picker skipped or duplicated a role on its first page");
    probe.click_button="##encounter-next";draw();
    Check(probe.toggles.size()==4 && probe.toggles.front().first.starts_with("Role 13##") && probe.toggles.back().first.starts_with("Role 16##"),
        "the next picker page did not contain the remaining four roles");
    probe.search="Role 16";probe.change_search=true;draw();
    Check(encounter_page==0 && probe.toggles.size()==1 && probe.toggles[0].first.starts_with("Role 16##"),
        "searching from a later page did not reset pagination or filter role names");
    const auto selected=view->choices.back().character;
    probe.click_role="##encounter-role-"+std::to_string(Pack(selected));draw();
    auto command=encounter_commands.exchange({});
    Check(command && command->kind==EncounterCommandKind::Select && Pack(command->character)==Pack(selected) && command->identity==view->identity,
        "the role switch did not queue the displayed role's original identity");
    Check(!encounter_pending.load() && self.getter_calls==0 && self.original_calls==0 && self.writes==0,
        "Render performed gameplay work instead of queuing the selection");
    encounter_pending.store(std::make_shared<const EncounterPreference>(EncounterPreference{view->identity,selected,"Role 16"}));
    probe.click_role="##encounter-role-"+std::to_string(Pack(selected));draw();
    command=encounter_commands.exchange({});
    Check(probe.toggles[0].second==1 && command && command->kind==EncounterCommandKind::Cancel && encounter_pending.load(),
        "turning off the selected switch did not queue cancellation for Game");
    probe.search="no matching role";probe.change_search=true;draw();
    Check(probe.toggles.empty() && encounter_pending.load() && !encounter_commands.load() &&
        std::any_of(probe.texts.begin(),probe.texts.end(),[](const auto& text){return text=="Selected: Role 16";}),
        "filtering the selected role out cleared the preference or hid the selected summary");
    probe.click_button="##encounter-refresh";draw();command=encounter_commands.exchange({});
    Check(command && command->kind==EncounterCommandKind::Refresh,"refresh no longer queued a fresh candidate list");
}
}

int main() {
    try {
        ProfileReplay();
        NativePreferenceContracts();
        LocalizedLabelContracts();
        AutomaticEncounterContracts();
        RelogRefreshContracts();
        PickerUiContracts();
        std::cout << "Taxi encounter native replay, candidate eligibility, one-shot selection and lifecycle guards passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
