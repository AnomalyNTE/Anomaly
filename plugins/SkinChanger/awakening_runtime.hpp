// Local awakening appearance preview. Inventory writes run on the Game thread.
namespace awakening {
using namespace skin_awakening_profile;
using Effects = awakening_data::Effects;
struct Target { AnomalyGenerationHandleV1 item{}, inventory{}; uint64_t uid{}; FName character{}; };
struct Backup { Target target; Effects before{}, applied{}; int32_t level{}; };
struct VisualBackup {
    AnomalyGenerationHandleV1 actor{};
    uint64_t uid{}, fashion{};
    uintptr_t appearance{};
    uint8_t before{};
};
std::vector<VisualBackup> visual_backups;
Clock::time_point next_visual{};
struct View {
    std::string status{lite_i18n::localizer.Text("awakening.wait_inventory", "Waiting for the local character inventory.")}; size_t owned{}, changed{}, processed{}, rejected{}, already{};
    bool ready{}, active{}, busy{}, restore_failed{};
};
std::atomic<std::shared_ptr<const View>> published;
std::atomic_int requested{};
std::atomic_bool interested{};
std::vector<Target> owned, queue;
std::vector<Backup> backups;
size_t cursor{}, rejected{}, already{};
bool enabled{}, restore_failed{}, initial_complete{};
uintptr_t world_pointer{}, item_class{}, ui_class{};
AnomalyGenerationHandleV1 inventory_handle{}, ui_handle{};
uint32_t ui_scan_index{};
Clock::time_point next_tick{}, next_inventory{}, next_ui_scan{};
std::string result;
struct CachedObject { std::string path; AnomalyGenerationHandleV1 handle{}; Clock::time_point retry{}; };
std::vector<CachedObject> cached_objects;
struct CheckedLayout { uintptr_t type{}; std::string name; int32_t offset{}, size{}; bool valid{}; };
std::vector<CheckedLayout> layouts;

bool Layout(const Reader& r, uintptr_t type, const char* name, int32_t offset, int32_t size) {
    const auto found = std::find_if(layouts.begin(),layouts.end(),[&](const CheckedLayout& field) {
        return field.type == type && field.name == name && field.offset == offset && field.size == size;
    });
    if (found != layouts.end()) return found->valid;
    const bool valid = r.Property(type,name,offset,size);
    layouts.push_back({type,name,offset,size,valid}); return valid;
}
uintptr_t FindCached(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const char* path) {
    auto found = std::find_if(cached_objects.begin(),cached_objects.end(),
        [&](const CachedObject& entry){return entry.path == path;});
    if (found == cached_objects.end()) {
        cached_objects.push_back({path,{},{}}); found = std::prev(cached_objects.end());
    }
    if (const auto object = Resolve(r,objects,found->handle)) return object;
    if (Clock::now() < found->retry) return 0;
    found->retry = Clock::now()+std::chrono::seconds(5); found->handle = {};
    return objects->find_exact(objects->user,StringView(path),&found->handle).code == 0
        ? Resolve(r,objects,found->handle) : 0;
}
bool Write(uintptr_t address, const auto& data) {
    return core && core->write_memory && core->write_memory(core->user,address,
        {reinterpret_cast<const uint8_t*>(&data),sizeof(data)}).code == 0;
}
bool Bytes(const Reader&, uintptr_t address, void* data, size_t size) {
    return address > 0x10000 && core && core->read_memory && core->read_memory(core->user,address,
        {reinterpret_cast<uint8_t*>(data),size}).code == 0;
}
AnomalyGenerationHandleV1 Handle(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, uintptr_t object) {
    int32_t index{}; AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (!r.Read(object+accessory::profile::native_layout.ObjectIndex,index) || index < 0 ||
        objects->snapshot_at(objects->user,static_cast<uint32_t>(index),&snapshot).code != 0 ||
        Resolve(r,objects,snapshot.handle) != object) return {};
    return snapshot.handle;
}
bool PrepareWorldPointer(const Reader& r) {
    if (!world_pointer) {
        const auto instruction=accessory::profile::compatibility.Address("World");
        int32_t displacement{};
        if (!instruction || !r.Read(instruction+accessory::profile::native_layout.RipDisplacement,displacement)) return false;
        world_pointer = static_cast<uintptr_t>(static_cast<intptr_t>(instruction)+accessory::profile::native_layout.RipInstructionSize+displacement);
    }
    return true;
}
uintptr_t LocalInventory(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    if (!PrepareWorldPointer(r)) return 0;
    uintptr_t world{}, instance{}, local{}, controller{}, state{}, inventory{}, cls{}; RowArray players{};
    if (!r.Read(world_pointer,world) || !r.Read(world+accessory::profile::native_layout.ObjectClass,cls) ||
        !Layout(r,cls,"OwningGameInstance",skin_awakening_profile::native_layout.WorldGameInstance,8) || !r.Read(world+skin_awakening_profile::native_layout.WorldGameInstance,instance) ||
        !r.Read(instance+accessory::profile::native_layout.ObjectClass,cls) || !Layout(r,cls,"LocalPlayers",skin_awakening_profile::native_layout.GameInstanceLocalPlayers,16) ||
        !r.Read(instance+skin_awakening_profile::native_layout.GameInstanceLocalPlayers,players) || players.count != 1 || players.capacity < 1 || players.capacity > 16 ||
        !r.Read(players.data,local) || !r.Read(local+accessory::profile::native_layout.ObjectClass,cls) || !Layout(r,cls,"PlayerController",skin_awakening_profile::native_layout.LocalPlayerController,8) ||
        !r.Read(local+skin_awakening_profile::native_layout.LocalPlayerController,controller) || !r.Read(controller+accessory::profile::native_layout.ObjectClass,cls) || !Layout(r,cls,"PlayerState",skin_awakening_profile::native_layout.ControllerPlayerState,8) ||
        !r.Read(controller+skin_awakening_profile::native_layout.ControllerPlayerState,state) || !r.Read(state+accessory::profile::native_layout.ObjectClass,cls) || !Layout(r,cls,"InventoryComponent",skin_awakening_profile::native_layout.PlayerStateInventory,8) ||
        !r.Read(state+skin_awakening_profile::native_layout.PlayerStateInventory,inventory) || !r.Read(inventory+accessory::profile::native_layout.ObjectClass,cls) ||
        !Layout(r,cls,"InventoryContainerMap",skin_awakening_profile::native_layout.InventoryContainers,0x50) || !Handle(r,objects,inventory).id) return 0;
    return inventory;
}
// Character items are in CARD (4). Respect the allocation flags of sparse maps.
std::optional<std::vector<uintptr_t>> Slots(const Reader& r, uintptr_t map, size_t stride, int32_t limit) {
    RowArray header{}; uintptr_t flags{}; int32_t bits{}, maximum{}, free{};
    if (!r.Read(map,header) || header.count < 0 || header.count > limit || header.capacity < header.count ||
        header.capacity > limit*4 || !r.Read(map+accessory::profile::native_layout.MapBitsData,flags) || !r.Read(map+accessory::profile::native_layout.MapBitCount,bits) ||
        !r.Read(map+accessory::profile::native_layout.MapBitCapacity,maximum) || !r.Read(map+accessory::profile::native_layout.MapFreeCount,free) || bits < header.count || maximum < bits ||
        free < 0 || free > header.count || bits > limit*4) return {};
    if (!flags) { if (bits > 128) return {}; flags = map+accessory::profile::native_layout.MapInlineBits; }
    std::vector<uint32_t> allocated((header.count+31)/32);
    if (header.count && (!header.data || !Bytes(r,flags,allocated.data(),allocated.size()*4))) return {};
    std::vector<uintptr_t> result_slots;
    for (int32_t i = 0; i < header.count; ++i)
        if (allocated[i/32] & (1u << (i%32))) result_slots.push_back(header.data+stride*i);
    if (result_slots.size() != static_cast<size_t>(header.count-free)) return {};
    return result_slots;
}
bool ReadOwned(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, uintptr_t inventory) {
    const auto containers = Slots(r,inventory+skin_awakening_profile::native_layout.InventoryContainers,accessory::profile::native_layout.InventoryContainerStride,64);
    if (!containers) return false;
    if (!item_class) item_class = FindCached(r,objects,"/Script/HTGame.HTCharacterItem");
    if (!item_class || !Layout(r,item_class,"AwakenLevel",skin_awakening_profile::native_layout.ItemAwakenLevel,4) ||
        !Layout(r,item_class,"AwakenEffectDataLists",skin_awakening_profile::native_layout.ItemAwakenEffects,16) || !Layout(r,item_class,"ItemID",skin_awakening_profile::native_layout.ItemId,8) ||
        !Layout(r,item_class,"UniqueID",skin_awakening_profile::native_layout.ItemUniqueId,8)) return false;
    std::vector<Target> found;
    const auto inventory_id = Handle(r,objects,inventory);
    for (const auto entry : *containers) {
        uint8_t kind{}; if (!r.Read(entry,kind)) return false;
        if (kind != 4) continue;
        const auto cards = Slots(r,entry+accessory::profile::native_layout.InventoryContainerMap,accessory::profile::native_layout.MapEntryStride,512); if (!cards) return false;
        for (const auto card : *cards) {
            uintptr_t item{}, cls{}; Target target; target.inventory = inventory_id;
            if (!r.Read(card,target.uid) || !r.Read(card+accessory::profile::native_layout.MapValue,item) || !r.Read(item+accessory::profile::native_layout.ObjectClass,cls) || cls != item_class ||
                !r.Read(item+skin_awakening_profile::native_layout.ItemId,target.character) || !target.character.id) continue;
            uint64_t uid{}; if (!r.Read(item+skin_awakening_profile::native_layout.ItemUniqueId,uid) || uid != target.uid) continue;
            target.item = Handle(r,objects,item); if (target.item.id) found.push_back(target);
        }
    }
    if (found.empty()) return false;
    owned = std::move(found); return true;
}
bool Item(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const Target& target,
          uintptr_t& item, uintptr_t& effects, int32_t& level, Effects& data) {
    item = Resolve(r,objects,target.item); const auto inventory = Resolve(r,objects,target.inventory);
    uintptr_t cls{}, owner{}, actual_inventory{}; uint64_t uid{}; FName character{}; RowArray array{};
    if (!item || !inventory || !r.Read(item+accessory::profile::native_layout.ObjectClass,cls) || cls != item_class ||
        !r.Read(item+skin_awakening_profile::native_layout.ItemUniqueId,uid) || uid != target.uid || !r.Read(item+skin_awakening_profile::native_layout.ItemId,character) ||
        Pack(character) != Pack(target.character) || !r.Read(item+accessory::profile::native_layout.ObjectOuter,owner) ||
        !r.Read(owner+skin_awakening_profile::native_layout.PlayerStateInventory,actual_inventory) || actual_inventory != inventory ||
        !r.Read(item+skin_awakening_profile::native_layout.ItemAwakenLevel,level) || level < 0 || level > 6 || !r.Read(item+skin_awakening_profile::native_layout.ItemAwakenEffects,array) ||
        array.count != 6 || array.capacity < 6 || array.capacity > 64 || !r.Read(array.data,data)) return false;
    effects = array.data; return true;
}
void DiscoverUi(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    if (Resolve(r,objects,ui_handle) || Clock::now() < next_ui_scan) return;
    if (ui_handle.id) ui_scan_index = objects->count(objects->user);
    ui_handle = {};
    if (!ui_class) ui_class = FindCached(r,objects,
        "/Game/UI/Blueprints/Character/BPUI_CharacterAwaken.BPUI_CharacterAwaken_C");
    if (!ui_class || !Layout(r,ui_class,"m_CharacterInfoStruct",skin_awakening_profile::native_layout.AwakenUiInfo,skin_awakening_profile::native_layout.AwakenUiInfoSize)) return;
    const auto count = objects->count(objects->user);
    if (!ui_scan_index || ui_scan_index > count) ui_scan_index = count;
    uintptr_t chunks{};
    if (!r.Read(registry+accessory::profile::native_layout.RegistryItems,chunks)) return;
    // Find the recently created widget once. No per-object name resolution.
    const auto until = Clock::now()+std::chrono::microseconds(500);
    for (size_t n = 0; ui_scan_index && n < 1024 && Clock::now() < until; ++n) {
        const auto index = --ui_scan_index; uintptr_t chunk{}, object{}, cls{};
        if (!r.Read(chunks+sizeof(uintptr_t)*(index/accessory::profile::native_layout.ChunkSize),chunk) || !r.Read(chunk+accessory::profile::native_layout.ItemStride*(index%accessory::profile::native_layout.ChunkSize),object) ||
            !r.Read(object+accessory::profile::native_layout.ObjectClass,cls) || cls != ui_class) continue;
        uint64_t uid{};
        if (!r.Read(object+skin_awakening_profile::native_layout.AwakenUiInfo,uid) || !uid) continue;
        ui_handle = Handle(r,objects,object);
        if (ui_handle.id) return;
    }
    if (!ui_scan_index) next_ui_scan = Clock::now()+std::chrono::seconds(5);
}
bool Refresh(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const Target& target) {
    const auto ui = Resolve(r,objects,ui_handle); const auto inventory = Resolve(r,objects,target.inventory);
    uint64_t uid{};
    if (!ui || !inventory || !r.Read(ui+skin_awakening_profile::native_layout.AwakenUiInfo,uid) || uid != target.uid) return false;
    const auto fn = FindCached(r,objects,"/Script/HTGame.HTUI_CharacterAwaken.OnInventoryUpdateCharacterCallback");
    if (!fn || !r.Function(fn,"OnInventoryUpdateCharacterCallback",3,24) ||
        !Layout(r,fn,"NotifyType",0,1) || !Layout(r,fn,"InventoryComponent",8,8) ||
        !Layout(r,fn,"TheUniqueId",16,8)) return false;
    alignas(8) std::array<uint8_t,24> params{}; params[0] = 22;
    std::memcpy(params.data()+8,&inventory,8); std::memcpy(params.data()+16,&target.uid,8);
    return Invoke(r,ui,fn,params.data());
}
bool RestoreOne(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const Backup& saved, bool refresh) {
    if (!Resolve(r,objects,saved.target.item)) return true;
    uintptr_t item{}, effects{}; Effects current{}; int32_t level{};
    if (!Item(r,objects,saved.target,item,effects,level,current)) return false;
    const auto restored = awakening_data::Restore(current,saved.applied,saved.before);
    if (!Write(effects,restored) || (level == 6 && !Write(item+skin_awakening_profile::native_layout.ItemAwakenLevel,saved.level))) return false;
    if (refresh) Refresh(r,objects,saved.target);
    return true;
}
bool RefreshVisual(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, uintptr_t actor) {
    const auto fn = FindCached(r,objects,"/Script/HTGame.HTPlayerCharacter.OnRep_bReplicatedFullAwaken");
    if (!fn || !r.Function(fn,"OnRep_bReplicatedFullAwaken",0,0)) return false;
    return Invoke(r,actor,fn,nullptr);
}
bool VisualIdentity(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects,
                    const VisualBackup& saved, uintptr_t& actor, uint8_t& current) {
    actor = Resolve(r,objects,saved.actor); uint64_t uid{};
    return actor && r.Read(actor+skin_awakening_profile::native_layout.ActorUniqueId,uid) && uid == saved.uid &&
        r.Read(actor+skin_awakening_profile::native_layout.ActorFullAwaken,current) && current <= 1;
}
bool RestoreVisuals(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, bool refresh) {
    bool restored = true;
    for (size_t i = visual_backups.size(); i > 0; --i) {
        const auto& saved = visual_backups[i-1]; uintptr_t actor{}; uint8_t current{};
        // Weak handles and item IDs prevent restoring a replacement avatar.
        if (!VisualIdentity(r,objects,saved,actor,current)) {
            visual_backups.erase(visual_backups.begin()+static_cast<std::ptrdiff_t>(i-1)); continue;
        }
        if (current == 1 && saved.before != 1) {
            if (!Write(actor+skin_awakening_profile::native_layout.ActorFullAwaken,saved.before) || (refresh && !RefreshVisual(r,objects,actor))) {
                restored = false; continue;
            }
        }
        visual_backups.erase(visual_backups.begin()+static_cast<std::ptrdiff_t>(i-1));
    }
    return restored;
}
void UpdateVisual(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    if (!enabled || Clock::now() < next_visual) return;
    next_visual = Clock::now()+std::chrono::milliseconds(500);
    const auto combat = Host(api).Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID).get();
    AnomalyNteCombatantSnapshotV1 player{sizeof(player)};
    if (!combat || !combat->current_combatant || combat->current_combatant(combat->user,&player).code != 0 ||
        !(player.flags & ANOMALY_NTE_COMBATANT_V1_VALID)) return;
    const auto actor = Resolve(r,objects,player.character);
    const auto inventory = Resolve(r,objects,inventory_handle);
    uintptr_t cls{}, state{}, owner{}, appearance{}; uint64_t uid{}; uint8_t current{}; FName fashion{};
    if (!actor || !inventory || !r.Read(actor+accessory::profile::native_layout.ObjectClass,cls) ||
        !Layout(r,cls,"UniqueID",skin_awakening_profile::native_layout.ActorUniqueId,8) || !Layout(r,cls,"SavedPlayerState",skin_awakening_profile::native_layout.ActorPlayerState,8) ||
        !Layout(r,cls,"bReplicatedFullAwaken",skin_awakening_profile::native_layout.ActorFullAwaken,1) ||
        !Layout(r,cls,"CurPlayerAppearance",skin_awakening_profile::native_layout.ActorAppearance,8) ||
        !Layout(r,cls,"CurrentDisplayFashionID",skin_awakening_profile::native_layout.ActorDisplayedFashion,8) ||
        !r.Read(actor+skin_awakening_profile::native_layout.ActorUniqueId,uid) || !r.Read(actor+skin_awakening_profile::native_layout.ActorPlayerState,state) || !r.Read(inventory+accessory::profile::native_layout.ObjectOuter,owner) ||
        !state || state != owner || !r.Read(actor+skin_awakening_profile::native_layout.ActorFullAwaken,current) || current > 1 ||
        !r.Read(actor+skin_awakening_profile::native_layout.ActorAppearance,appearance) || !appearance || !r.Read(actor+skin_awakening_profile::native_layout.ActorDisplayedFashion,fashion)) return;
    const auto target = std::find_if(owned.begin(),owned.end(),[&](const Target& value){return value.uid == uid;});
    uintptr_t item{}, effects{}; Effects nodes{}; int32_t level{};
    if (target == owned.end() || !Item(r,objects,*target,item,effects,level,nodes) || level != 6) return;
    auto saved = std::find_if(visual_backups.begin(),visual_backups.end(),[&](const VisualBackup& value) {
        return value.actor.id == player.character.id && value.uid == uid;
    });
    if (saved == visual_backups.end()) {
        visual_backups.push_back({player.character,uid,0,0,current}); saved = std::prev(visual_backups.end());
    }
    // Refresh once on activation, switching avatar, replacing a skin, or a replicated flag change.
    // No UObject search, repeated mesh rebuild, or awakening buff application.
    if (current == 1 && saved->fashion == Pack(fashion) && saved->appearance == appearance) return;
    const uint8_t full = 1;
    if (!Write(actor+skin_awakening_profile::native_layout.ActorFullAwaken,full) || !RefreshVisual(r,objects,actor)) {
        Write(actor+skin_awakening_profile::native_layout.ActorFullAwaken,current); return;
    }
    saved->fashion = Pack(fashion); saved->appearance = appearance;
    Log("Awakening: refreshed full-awakening world appearance for "+r.Name(target->character));
}
bool Restore(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, bool refresh) {
    bool restored = RestoreVisuals(r,objects,refresh);
    for (size_t i = backups.size(); i > 0; --i)
        if (RestoreOne(r,objects,backups[i-1],refresh)) backups.erase(backups.begin()+static_cast<std::ptrdiff_t>(i-1));
        else restored = false;
    enabled = false; queue.clear(); cursor = 0; return restored;
}

bool EffectNames(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, FName character,
                 std::array<awakening_data::Name,6>& names) {
    const auto id = r.Name(character);
    if (id.empty() || id.size() > 12 || id.find_first_not_of("0123456789") != std::string::npos) return false;
    const std::string path = "/Game/DataTable/Character/Awaken/"+id+"AwakenEffect."+id+"AwakenEffect";
    auto table = FindCached(r, objects, path.c_str());
    if(!table) {
        const auto variant="/Game/DataTable/Character/Awaken/"+id+"AwakenEffect1."+id+"AwakenEffect1";
        table=FindCached(r,objects,variant.c_str());
    }
    uintptr_t row_type{}; RowArray entries{}; int32_t free{};
    if (!table || !r.IsA(table, "DataTable") || !r.Read(table+skin_awakening_profile::native_layout.DataTableRowStruct, row_type) ||
        r.NameAt(row_type+accessory::profile::native_layout.ObjectName) != "CharacterAwakenEffectData" ||
        !Layout(r,row_type, "AwakenEffectStructList", accessory::profile::native_layout.AwakenList, 16) || !r.Read(table+skin_awakening_profile::native_layout.DataTableRowMap, entries) ||
        entries.count < 1 || entries.count > 128 || entries.capacity < entries.count ||
        !r.Read(table+skin_awakening_profile::native_layout.DataTableRowMapFreeCount, free) || free != 0) return false;
    const auto type = FindCached(r, objects, "/Script/HTGame.AwakenEffectStruct");
    const auto item_type = FindCached(r, objects, "/Script/HTGame.AwakenEffectData");
    int32_t stride{}, item_stride{};
    if (!type || !r.Read(type+skin_awakening_profile::native_layout.ScriptStructSize, stride) || stride != static_cast<int32_t>(skin_awakening_profile::native_layout.AwakenEffectStride) || !Layout(r,type,"EffectID",skin_awakening_profile::native_layout.AwakenEffectId,8) ||
        !Layout(r,type,"AwakenType",skin_awakening_profile::native_layout.AwakenEffectType,1) || !item_type || !r.Read(item_type+skin_awakening_profile::native_layout.ScriptStructSize,item_stride) ||
        item_stride != static_cast<int32_t>(skin_awakening_profile::native_layout.AwakenDataStride) || !Layout(r,item_type,"Unlocked",skin_awakening_profile::native_layout.AwakenDataUnlocked,1) || !Layout(r,item_type,"EffectID",skin_awakening_profile::native_layout.AwakenDataId,8)) return false;
    for (int32_t i = 0; i < entries.count; ++i) {
        FName key{}; uintptr_t row{}; RowArray effects{};
        if (!r.Read(entries.data+accessory::profile::native_layout.MapEntryStride*i, key) || Pack(key) != Pack(character)) continue;
        if (!r.Read(entries.data+accessory::profile::native_layout.MapEntryStride*i+accessory::profile::native_layout.MapValue, row) || !r.Read(row+accessory::profile::native_layout.AwakenList, effects) ||
            effects.count < 6 || effects.count > 64 || effects.capacity < effects.count) return false;
        std::array<bool,6> found{};
        for (int32_t j = 0; j < effects.count; ++j) {
            FName name{}; uint8_t kind{};
            if (!r.Read(effects.data+skin_awakening_profile::native_layout.AwakenEffectStride*j+skin_awakening_profile::native_layout.AwakenEffectId, name) || !r.Read(effects.data+skin_awakening_profile::native_layout.AwakenEffectStride*j+skin_awakening_profile::native_layout.AwakenEffectType, kind)) return false;
            if (kind != 0) continue;
            const auto label = r.Name(name);
            for (size_t k = 0; k < 6; ++k) if (label == "Effect"+std::to_string(k+1)) {
                if (found[k]) return false;
                found[k] = true; names[k] = {name.id, name.number};
            }
        }
        return std::all_of(found.begin(), found.end(), [](bool v){return v;});
    }
    return false;
}
void Apply(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const Target& target) {
    const auto existing=std::find_if(backups.begin(),backups.end(),[&](const Backup& saved){return saved.target.item.id == target.item.id;});
    uintptr_t item{}, effects{}; Effects before{}; int32_t level{};
    std::array<awakening_data::Name,6> names{};
    if (!Item(r,objects,target,item,effects,level,before)) {
        ++rejected; Log("Awakening: rejected item layout for "+r.Name(target.character)); return;
    }
    if (level == 6 && std::all_of(before.begin(),before.end(),[](const auto& e){return e.unlocked == 1;})) {
        ++already; return;
    }
    if (!EffectNames(r,objects,target.character,names)) {
        ++rejected; Log("Awakening: rejected effect catalog for "+r.Name(target.character)); return;
    }
    const auto applied = awakening_data::Unlock(before,names);
    if (!applied) { ++rejected; return; }
    Backup saved{target,before,*applied,level};
    const bool had_backup=existing!=backups.end();
    const auto backup_index=had_backup?static_cast<size_t>(existing-backups.begin()):backups.size();
    if(had_backup) {
        saved.before=awakening_data::Rebase(before,existing->applied,existing->before);
        if(level==6) saved.level=existing->level;
        backups[backup_index]=saved;
    } else backups.push_back(saved);
    const int32_t six = 6; Effects verified{}; int32_t verified_level{};
    if (!Write(effects,*applied) || !Write(item+skin_awakening_profile::native_layout.ItemAwakenLevel,six) || !r.Read(effects,verified) || verified != *applied ||
        !r.Read(item+skin_awakening_profile::native_layout.ItemAwakenLevel,verified_level) || verified_level != 6) {
        if (RestoreOne(r,objects,saved,true)) backups.erase(backups.begin()+static_cast<std::ptrdiff_t>(backup_index));
        ++rejected;
        Log("Awakening: failed local preview for "+r.Name(target.character)); return;
    }
    Refresh(r,objects,target);
    Log("Awakening: six-node local preview applied for "+r.Name(target.character));
}
void Publish() {
    auto data = std::make_shared<View>(); data->owned = owned.size(); data->changed = backups.size();
    data->rejected = rejected; data->already = already;
    data->processed = cursor; data->ready = !owned.empty(); data->active = enabled || !backups.empty() || !visual_backups.empty();
    data->busy = !initial_complete && cursor < queue.size();
    data->restore_failed = restore_failed;
    if (!result.empty()) data->status = result;
    else if (data->ready) data->status = lite_i18n::localizer.Text("awakening.inventory_ready", "All owned characters have been read.");
    published.store(std::move(data));
}
void Tick(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    if ((!interested && !enabled && backups.empty() && visual_backups.empty() && !requested.load()) || Clock::now() < next_tick) return;
    next_tick = Clock::now()+std::chrono::milliseconds(100);
    const auto begin = Clock::now();
    // Turning off cancels the remaining queue immediately, even if the current
    // inventory is unavailable while loading or changing scenes.
    int off = 2;
    if (requested.compare_exchange_strong(off,0)) {
        restore_failed = !Restore(r,objects,true);
        result = restore_failed ? lite_i18n::localizer.Text("awakening.restore_partial", "Some characters could not be restored. Please retry.") : lite_i18n::localizer.Text("awakening.restored", "Original awakening restored for all characters.");
        Publish(); return;
    }
    if (Clock::now() >= next_inventory) {
        next_inventory = Clock::now()+std::chrono::seconds(2);
        const auto inventory = LocalInventory(r,objects);
        const auto handle = inventory ? Handle(r,objects,inventory) : AnomalyGenerationHandleV1{};
        if (inventory_handle.id && inventory_handle.id != handle.id) {
            restore_failed = !Restore(r,objects,false);
            result = restore_failed ? lite_i18n::localizer.Text("awakening.restore_failed", "Original awakening could not be fully restored.") : lite_i18n::localizer.Text("awakening.inventory_changed", "The character inventory changed. Original awakening restored.");
            owned.clear(); ui_handle = {}; ui_scan_index = 0;
        }
        inventory_handle = handle;
        if (!inventory || !ReadOwned(r,objects,inventory)) { owned.clear(); Publish(); return; }
        // Revisit late-loaded catalogs and replicated inventory changes.
        if(enabled && cursor>=queue.size()) {queue=owned;cursor=0;rejected=already=0;}
        if (enabled) for (const auto& target : owned) {
            if (std::none_of(queue.begin(),queue.end(),[&](const Target& queued) {
                    return queued.item.id == target.item.id && queued.uid == target.uid;
                })) queue.push_back(target);
        }
    }
    // Keep an enable request pending until the character inventory is ready.
    if (owned.empty()) { Publish(); return; }
    DiscoverUi(r,objects);
    if (const auto command = requested.exchange(0)) {
        if (command == 2) {
            restore_failed = !Restore(r,objects,true);
            result = restore_failed ? lite_i18n::localizer.Text("awakening.restore_partial", "Some characters could not be restored. Please retry.") : lite_i18n::localizer.Text("awakening.restored", "Original awakening restored for all characters.");
        }
        else if (command == 1 && !owned.empty()) {
            enabled = true; initial_complete=false; restore_failed = false; queue = owned; cursor = rejected = already = 0; result = lite_i18n::localizer.Text("awakening.processing", "Applying previews to owned characters in batches.");
        }
    }
    if (enabled && cursor < queue.size()) {
        Apply(r,objects,queue[cursor++]);
        if (cursor == queue.size()) {initial_complete=true;result = rejected ? lite_i18n::localizer.Text("awakening.rejected", "Some characters failed validation and were left unchanged.") : lite_i18n::localizer.Text("awakening.applied", "Awakening nodes enabled for all characters (local preview).");}
    }
    // A selected page is refreshed once; no continuous rewriting or RPCs.
    if (enabled) {
        const auto ui = Resolve(r,objects,ui_handle); uint64_t uid{}; int32_t ui_level{};
        if (ui && r.Read(ui+skin_awakening_profile::native_layout.AwakenUiInfo,uid) && r.Read(ui+skin_awakening_profile::native_layout.AwakenUiLevel,ui_level) && ui_level != 6) {
            const auto found = std::find_if(backups.begin(),backups.end(),[&](const Backup& saved){return saved.target.uid == uid;});
            if (found != backups.end()) {
                const auto item = Resolve(r,objects,found->target.item); int32_t actual{};
                if (r.Read(item+skin_awakening_profile::native_layout.ItemAwakenLevel,actual) && actual == 6) Refresh(r,objects,found->target);
            }
        }
    }
    UpdateVisual(r,objects);
    Publish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count();
    if (elapsed > 3000) Log("Awakening: update took "+std::to_string(elapsed)+" us");
}
void Reset() {
    visual_backups.clear(); next_visual = {};
    owned.clear(); queue.clear(); backups.clear(); cached_objects.clear(); layouts.clear(); cursor = rejected = already = initial_complete = 0; enabled = restore_failed = false;
    world_pointer = item_class = ui_class = 0; inventory_handle = ui_handle = {}; ui_scan_index = 0;
    next_tick = next_inventory = next_ui_scan = {}; result.clear(); interested = false; requested = 0; Publish();
    // Signature discovery belongs to plugin startup, not an interactive frame.
    const Reader r{Host(api).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get()};
    PrepareWorldPointer(r);
}
bool Active() { return enabled || !backups.empty() || !visual_backups.empty(); }
void Draw(const AnomalyUiServiceV1* ui) {
    const auto data = published.load(); if (!data) return;
    const auto command = requested.load();
    int checked = command ? command == 1 : data->active;
    if (ui->checkbox && ui->checkbox(ui->user,StringView(lite_i18n::localizer.Text("awakening.enabled", "Unlock awakening")),&checked)) requested = checked ? 1 : 2;
    if (data->restore_failed) ui->text(ui->user,StringView(lite_i18n::localizer.Text("awakening.restore_retry", "Restoration is incomplete. Turn the option off to retry.")));
    else if (!data->ready) ui->text(ui->user,StringView(lite_i18n::localizer.Text("awakening.wait_character", "Waiting for characters to load...")));
    else if (data->busy) ui->text(ui->user,StringView(lite_i18n::localizer.Text("awakening.enabling", "Enabling...")));
    else if (data->active && data->rejected) ui->text(ui->user,StringView(lite_i18n::localizer.Text("awakening.partial", "Some characters could not be enabled.")));
}
}
