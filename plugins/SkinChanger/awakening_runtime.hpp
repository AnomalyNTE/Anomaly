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
    std::string status{"等待本地角色背包。"}; size_t owned{}, changed{}, processed{}, rejected{}, already{};
    bool ready{}, active{}, busy{}, restore_failed{};
};
std::atomic<std::shared_ptr<const View>> published;
std::atomic_int requested{};
std::atomic_bool interested{};
std::vector<Target> owned, queue;
std::vector<Backup> backups;
size_t cursor{}, rejected{}, already{};
bool enabled{}, restore_failed{};
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
    if (!r.Read(object+12,index) || index < 0 ||
        objects->snapshot_at(objects->user,static_cast<uint32_t>(index),&snapshot).code != 0 ||
        Resolve(r,objects,snapshot.handle) != object) return {};
    return snapshot.handle;
}
bool PrepareWorldPointer(const Reader& r) {
    if (!world_pointer) {
        const auto sig = Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        uintptr_t instruction{}; int32_t displacement{};
        if (!sig || !sig->resolve || sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
            StringView(accessory::profile::World),&instruction).code != 0 ||
            !r.Read(instruction+3,displacement)) return false;
        world_pointer = static_cast<uintptr_t>(static_cast<intptr_t>(instruction)+7+displacement);
    }
    return true;
}
uintptr_t LocalInventory(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    if (!PrepareWorldPointer(r)) return 0;
    uintptr_t world{}, instance{}, local{}, controller{}, state{}, inventory{}, cls{}; RowArray players{};
    if (!r.Read(world_pointer,world) || !r.Read(world+16,cls) ||
        !Layout(r,cls,"OwningGameInstance",WorldGameInstance,8) || !r.Read(world+WorldGameInstance,instance) ||
        !r.Read(instance+16,cls) || !Layout(r,cls,"LocalPlayers",GameInstanceLocalPlayers,16) ||
        !r.Read(instance+GameInstanceLocalPlayers,players) || players.count != 1 || players.capacity < 1 || players.capacity > 16 ||
        !r.Read(players.data,local) || !r.Read(local+16,cls) || !Layout(r,cls,"PlayerController",LocalPlayerController,8) ||
        !r.Read(local+LocalPlayerController,controller) || !r.Read(controller+16,cls) || !Layout(r,cls,"PlayerState",ControllerPlayerState,8) ||
        !r.Read(controller+ControllerPlayerState,state) || !r.Read(state+16,cls) || !Layout(r,cls,"InventoryComponent",PlayerStateInventory,8) ||
        !r.Read(state+PlayerStateInventory,inventory) || !r.Read(inventory+16,cls) ||
        !Layout(r,cls,"InventoryContainerMap",InventoryContainers,0x50) || !Handle(r,objects,inventory).id) return 0;
    return inventory;
}
// Character items are in CARD (4). Respect the allocation flags of sparse maps.
std::optional<std::vector<uintptr_t>> Slots(const Reader& r, uintptr_t map, size_t stride, int32_t limit) {
    RowArray header{}; uintptr_t flags{}; int32_t bits{}, maximum{}, free{};
    if (!r.Read(map,header) || header.count < 0 || header.count > limit || header.capacity < header.count ||
        header.capacity > limit*4 || !r.Read(map+32,flags) || !r.Read(map+40,bits) ||
        !r.Read(map+44,maximum) || !r.Read(map+52,free) || bits < header.count || maximum < bits ||
        free < 0 || free > header.count || bits > limit*4) return {};
    if (!flags) { if (bits > 128) return {}; flags = map+16; }
    std::vector<uint32_t> allocated((header.count+31)/32);
    if (header.count && (!header.data || !Bytes(r,flags,allocated.data(),allocated.size()*4))) return {};
    std::vector<uintptr_t> result_slots;
    for (int32_t i = 0; i < header.count; ++i)
        if (allocated[i/32] & (1u << (i%32))) result_slots.push_back(header.data+stride*i);
    if (result_slots.size() != static_cast<size_t>(header.count-free)) return {};
    return result_slots;
}
bool ReadOwned(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, uintptr_t inventory) {
    const auto containers = Slots(r,inventory+InventoryContainers,176,64);
    if (!containers) return false;
    if (!item_class) item_class = FindCached(r,objects,"/Script/HTGame.HTCharacterItem");
    if (!item_class || !Layout(r,item_class,"AwakenLevel",ItemAwakenLevel,4) ||
        !Layout(r,item_class,"AwakenEffectDataLists",ItemAwakenEffects,16) || !Layout(r,item_class,"ItemID",ItemId,8) ||
        !Layout(r,item_class,"UniqueID",ItemUniqueId,8)) return false;
    std::vector<Target> found;
    const auto inventory_id = Handle(r,objects,inventory);
    for (const auto entry : *containers) {
        uint8_t kind{}; if (!r.Read(entry,kind)) return false;
        if (kind != 4) continue;
        const auto cards = Slots(r,entry+8,24,512); if (!cards) return false;
        for (const auto card : *cards) {
            uintptr_t item{}, cls{}; Target target; target.inventory = inventory_id;
            if (!r.Read(card,target.uid) || !r.Read(card+8,item) || !r.Read(item+16,cls) || cls != item_class ||
                !r.Read(item+ItemId,target.character) || !target.character.id) continue;
            uint64_t uid{}; if (!r.Read(item+ItemUniqueId,uid) || uid != target.uid) continue;
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
    if (!item || !inventory || !r.Read(item+16,cls) || cls != item_class ||
        !r.Read(item+ItemUniqueId,uid) || uid != target.uid || !r.Read(item+ItemId,character) ||
        Pack(character) != Pack(target.character) || !r.Read(item+32,owner) ||
        !r.Read(owner+PlayerStateInventory,actual_inventory) || actual_inventory != inventory ||
        !r.Read(item+ItemAwakenLevel,level) || level < 0 || level > 6 || !r.Read(item+ItemAwakenEffects,array) ||
        array.count != 6 || array.capacity < 6 || array.capacity > 64 || !r.Read(array.data,data)) return false;
    effects = array.data; return true;
}
void DiscoverUi(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    if (Resolve(r,objects,ui_handle) || Clock::now() < next_ui_scan) return;
    if (ui_handle.id) ui_scan_index = objects->count(objects->user);
    ui_handle = {};
    if (!ui_class) ui_class = FindCached(r,objects,
        "/Game/UI/Blueprints/Character/BPUI_CharacterAwaken.BPUI_CharacterAwaken_C");
    if (!ui_class || !Layout(r,ui_class,"m_CharacterInfoStruct",AwakenUiInfo,AwakenUiInfoSize)) return;
    const auto count = objects->count(objects->user);
    if (!ui_scan_index || ui_scan_index > count) ui_scan_index = count;
    uintptr_t chunks{};
    if (!r.Read(registry+16,chunks)) return;
    // Find the recently created widget once. No per-object name resolution.
    const auto until = Clock::now()+std::chrono::microseconds(500);
    for (size_t n = 0; ui_scan_index && n < 1024 && Clock::now() < until; ++n) {
        const auto index = --ui_scan_index; uintptr_t chunk{}, object{}, cls{};
        if (!r.Read(chunks+8*(index/65536),chunk) || !r.Read(chunk+24*(index%65536),object) ||
            !r.Read(object+16,cls) || cls != ui_class) continue;
        uint64_t uid{};
        if (!r.Read(object+AwakenUiInfo,uid) || !uid) continue;
        ui_handle = Handle(r,objects,object);
        if (ui_handle.id) return;
    }
    if (!ui_scan_index) next_ui_scan = Clock::now()+std::chrono::seconds(5);
}
bool Refresh(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const Target& target) {
    const auto ui = Resolve(r,objects,ui_handle); const auto inventory = Resolve(r,objects,target.inventory);
    uint64_t uid{};
    if (!ui || !inventory || !r.Read(ui+AwakenUiInfo,uid) || uid != target.uid) return false;
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
    if (!Write(effects,restored) || (level == 6 && !Write(item+ItemAwakenLevel,saved.level))) return false;
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
    return actor && r.Read(actor+ActorUniqueId,uid) && uid == saved.uid &&
        r.Read(actor+ActorFullAwaken,current) && current <= 1;
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
            if (!Write(actor+ActorFullAwaken,saved.before) || (refresh && !RefreshVisual(r,objects,actor))) {
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
    if (!actor || !inventory || !r.Read(actor+16,cls) ||
        !Layout(r,cls,"UniqueID",ActorUniqueId,8) || !Layout(r,cls,"SavedPlayerState",ActorPlayerState,8) ||
        !Layout(r,cls,"bReplicatedFullAwaken",ActorFullAwaken,1) ||
        !Layout(r,cls,"CurPlayerAppearance",ActorAppearance,8) ||
        !Layout(r,cls,"CurrentDisplayFashionID",ActorDisplayedFashion,8) ||
        !r.Read(actor+ActorUniqueId,uid) || !r.Read(actor+ActorPlayerState,state) || !r.Read(inventory+32,owner) ||
        !state || state != owner || !r.Read(actor+ActorFullAwaken,current) || current > 1 ||
        !r.Read(actor+ActorAppearance,appearance) || !appearance || !r.Read(actor+ActorDisplayedFashion,fashion)) return;
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
    if (!Write(actor+ActorFullAwaken,full) || !RefreshVisual(r,objects,actor)) {
        Write(actor+ActorFullAwaken,current); return;
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
    const auto table = FindCached(r, objects, path.c_str()); uintptr_t row_type{}; RowArray entries{}; int32_t free{};
    if (!table || !r.IsA(table, "DataTable") || !r.Read(table+DataTableRowStruct, row_type) ||
        r.NameAt(row_type+24) != "CharacterAwakenEffectData" ||
        !Layout(r,row_type, "AwakenEffectStructList", 8, 16) || !r.Read(table+DataTableRowMap, entries) ||
        entries.count < 1 || entries.count > 128 || entries.capacity < entries.count ||
        !r.Read(table+DataTableRowMapFreeCount, free) || free != 0) return false;
    const auto type = FindCached(r, objects, "/Script/HTGame.AwakenEffectStruct");
    const auto item_type = FindCached(r, objects, "/Script/HTGame.AwakenEffectData");
    int32_t stride{}, item_stride{};
    if (!type || !r.Read(type+ScriptStructSize, stride) || stride != AwakenEffectStride || !Layout(r,type,"EffectID",0,8) ||
        !Layout(r,type,"AwakenType",AwakenEffectType,1) || !item_type || !r.Read(item_type+ScriptStructSize,item_stride) ||
        item_stride != 12 || !Layout(r,item_type,"Unlocked",0,1) || !Layout(r,item_type,"EffectID",4,8)) return false;
    for (int32_t i = 0; i < entries.count; ++i) {
        FName key{}; uintptr_t row{}; RowArray effects{};
        if (!r.Read(entries.data+24*i, key) || Pack(key) != Pack(character)) continue;
        if (!r.Read(entries.data+24*i+8, row) || !r.Read(row+8, effects) ||
            effects.count < 6 || effects.count > 64 || effects.capacity < effects.count) return false;
        std::array<bool,6> found{};
        for (int32_t j = 0; j < effects.count; ++j) {
            FName name{}; uint8_t kind{};
            if (!r.Read(effects.data+AwakenEffectStride*j, name) || !r.Read(effects.data+AwakenEffectStride*j+AwakenEffectType, kind)) return false;
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
    if (std::any_of(backups.begin(),backups.end(),[&](const Backup& saved){return saved.target.item.id == target.item.id;})) return;
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
    const Backup saved{target,before,*applied,level}; backups.push_back(saved);
    const int32_t six = 6; Effects verified{}; int32_t verified_level{};
    if (!Write(effects,*applied) || !Write(item+ItemAwakenLevel,six) || !r.Read(effects,verified) || verified != *applied ||
        !r.Read(item+ItemAwakenLevel,verified_level) || verified_level != 6) {
        if (RestoreOne(r,objects,saved,true)) backups.pop_back();
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
    data->busy = cursor < queue.size();
    data->restore_failed = restore_failed;
    if (!result.empty()) data->status = result;
    else if (data->ready) data->status = "已读取全部已持有角色。";
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
        result = restore_failed ? "部分角色恢复未完成，请重试。" : "已恢复全部角色原觉醒。";
        Publish(); return;
    }
    if (Clock::now() >= next_inventory) {
        next_inventory = Clock::now()+std::chrono::seconds(2);
        const auto inventory = LocalInventory(r,objects);
        const auto handle = inventory ? Handle(r,objects,inventory) : AnomalyGenerationHandleV1{};
        if (inventory_handle.id && inventory_handle.id != handle.id) {
            restore_failed = !Restore(r,objects,false);
            result = restore_failed ? "原觉醒恢复未完成。" : "角色背包已变更，已恢复原觉醒。";
            owned.clear(); ui_handle = {}; ui_scan_index = 0;
        }
        inventory_handle = handle;
        if (!inventory || !ReadOwned(r,objects,inventory)) { owned.clear(); Publish(); return; }
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
            result = restore_failed ? "部分角色恢复未完成，请重试。" : "已恢复全部角色原觉醒。";
        }
        else if (command == 1 && !owned.empty()) {
            enabled = true; restore_failed = false; queue = owned; cursor = rejected = already = 0; result = "正在分批预览全部已持有角色。";
        }
    }
    if (enabled && cursor < queue.size()) {
        Apply(r,objects,queue[cursor++]);
        if (cursor == queue.size()) result = rejected ? "部分角色未通过校验，未修改。" : "全部角色的觉醒节点已解锁（本机预览）。";
    }
    // A selected page is refreshed once; no continuous rewriting or RPCs.
    if (enabled) {
        const auto ui = Resolve(r,objects,ui_handle); uint64_t uid{}; int32_t ui_level{};
        if (ui && r.Read(ui+AwakenUiInfo,uid) && r.Read(ui+AwakenUiLevel,ui_level) && ui_level != 6) {
            const auto found = std::find_if(backups.begin(),backups.end(),[&](const Backup& saved){return saved.target.uid == uid;});
            if (found != backups.end()) {
                const auto item = Resolve(r,objects,found->target.item); int32_t actual{};
                if (r.Read(item+ItemAwakenLevel,actual) && actual == 6) Refresh(r,objects,found->target);
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
    owned.clear(); queue.clear(); backups.clear(); cached_objects.clear(); layouts.clear(); cursor = rejected = already = 0; enabled = restore_failed = false;
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
    if (ui->checkbox && ui->checkbox(ui->user,StringView("解锁觉醒"),&checked)) requested = checked ? 1 : 2;
    if (data->restore_failed) ui->text(ui->user,StringView("恢复未完成，请关闭开关重试。"));
    else if (!data->ready) ui->text(ui->user,StringView("等待角色加载…"));
    else if (data->busy) ui->text(ui->user,StringView("正在开启…"));
    else if (data->active && data->rejected) ui->text(ui->user,StringView("部分角色未能开启。"));
}
}
