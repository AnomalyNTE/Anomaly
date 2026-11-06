#pragma once
#include <anomaly/sdk/profile.hpp>
#include <Windows.h>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace mod_manager {
using Handle = AnomalyGenerationHandleV1;
// All addresses come from a profile-validated registry and serial-bearing SDK handles.
// UObject flags are never changed. Orphaned Mass cache entries use the validated
// native destructor/hash removal and the same relocation rules as UE TArray.
class NativeAccess {
    friend class VehicleTemplates;
    struct Layout {
        uint32_t ObjectClass{}, ObjectName{}, ObjectIndex{}, ObjectOuter{}, ObjectFlags{}, StructSuper{}, StructProperties{};
        uint32_t PropertyName{}, PropertyNext{}, PropertyClass{}, FieldClassName{}, PropertyOffset{}, PropertySize{}, PropertyDim{}, ObjectPropertyClass{};
        uint32_t FunctionCount{}, FunctionSize{}, FunctionReturn{}, RegistryCount{}, RegistryItems{}, ChunkSize{}, ItemStride{}, ItemSerial{}, RegistrySubtract{};
        uint32_t PoolOffset{}, PoolElementStride{};
        uint32_t TemplateActors{}, TemplateActorStride{}, TemplateActorReferences{}, TemplateMap{}, TemplateStride{}, TemplateValue{}, TemplateController{}, SharedReferences{};
        uint32_t TemplateFragments{}, FragmentValueStride{}, RepresentationSize{}, RepresentationHigh{}, RepresentationLow{}, RepresentationVisual{};
        uint32_t RepresentationManager{}, ManagerArchetypes{}, ArchetypeFragments{}, ArchetypeFragmentData{}, ArchetypeFragmentCount{}, ArchetypeChunks{}, ArchetypeCapacity{};
        uint32_t FragmentConfigStride{}, FragmentConfigOffset{}, MassChunkStride{}, MassChunkBytes{}, MassChunkCount{}, StructSize{};
        uint32_t ManagerConstShared{}, SharedHash{}, SharedTypes{}, SharedTypeStride{}, SharedHashStride{}, SharedMemoryData{};
        uint32_t SharedTypeInstances{}, SharedHashIndex{}, SharedValueController{};
        uint32_t SimulationSize{}, SimulationClass{}, StructAlignment{};
        uint32_t TrafficSubsystemSize{}, PhysicsTemplates{}, PhysicsTemplateClass{};
        uint32_t VisualizationComponent{}, VisualInfos{}, VisualFree{}, VisualComponentMap{}, VisualShared{}, VisualSharedMap{}, VisualDirty{}, VisualSharedFree{}, VisualMeshMap{}, VisualPending{}, VisualSize{};
        uint32_t VisualInfoSize{}, VisualDesc{}, VisualDescSize{}, VisualMeshes{}, VisualComponents{}, VisualRanges{}, VisualMeshSize{}, VisualMeshObject{};
        uint32_t VisualMapStride{}, VisualMapValue{}, VisualMapNext{}, VisualMapBucket{}, VisualMeshMapKey{};
        uint32_t VisualSharedSize{}, VisualSharedTransform{}, VisualSharedUpdates{}, VisualSharedRemoves{}, VisualSharedKey{}, VisualSharedReferences{}, VisualSharedInnerMap{};
    } l;
    // Borrowed, single-element TSet input to the native pool-release call. The engine only
    // reads this input; its layout is checked against Profile before enabling the operation.
    struct ClassElement { uintptr_t object{}; int32_t next{-1}, hash{}; };
    struct ClassSet {
        uintptr_t data{};
        int32_t count{1}, capacity{1};
        std::array<uint32_t, 4> bits{1};
        uintptr_t indirect_bits{};
        int32_t bit_count{1}, bit_capacity{128}, first_free{-1}, free_count{};
        int32_t hash{}, padding{};
        uintptr_t indirect_hash{};
        int32_t hash_count{1}, padding2{};
    };
    struct Parameter {
        std::string_view name, type;
        uint16_t offset{}, size{};
        std::string_view object_type;
    };
    anomaly::sdk::ProfileSnapshot profile;
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyNtePlayerServiceV1* player{};
    uintptr_t registry{}, release_pooled{}, remove_template{}, release_template{}, remove_shared_hash{}, destroy_struct{};
    uintptr_t initialize_struct{}, remove_visual_hash{}, destroy_visual_shared{}, initialize_instance_map{}, grow_array{};
    bool ready{};

    template<class T> bool Read(uintptr_t at, T& out) const {
        return at > 65536 && core->read_memory(core->user, at,
            {reinterpret_cast<uint8_t*>(&out), sizeof(out)}).code == 0;
    }
    std::string Name(uintptr_t at) const {
        struct NameValue { uint32_t index, number; } value{};
        std::array<char, 512> text{}; size_t size = text.size();
        if (!Read(at, value) || names->resolve_utf8(names->user, value.index, text.data(), &size).code ||
            size == 0 || size > text.size() || text[size - 1] != 0) return {};
        auto result = std::string(text.data());
        if (value.number) result += "_" + std::to_string(value.number - 1);
        return result;
    }
    uintptr_t Resolve(Handle handle) const {
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (!ready || !handle.id || objects->snapshot_by_handle(objects->user, handle, &snapshot).code) return 0;
        const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
        uintptr_t chunks{}, chunk{}, object{}; uint32_t count{}, serial{}, actual{}, flags{};
        if (!Read(registry + l.RegistryCount, count) || index >= count || count > 16 * 1024 * 1024 ||
            !Read(registry + l.RegistryItems, chunks) || !Read(chunks + sizeof(uintptr_t) * (index / l.ChunkSize), chunk)) return 0;
        const auto item = chunk + uint64_t(l.ItemStride) * (index % l.ChunkSize);
        if (!Read(item, object) || !Read(item + l.ItemSerial, serial) || serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
            !Read(object + l.ObjectIndex, actual) || actual != index || !Read(object + l.ObjectFlags, flags) ||
            (flags & 0x18000u)) return 0; // RF_BeginDestroyed | RF_FinishDestroyed, UE EObjectFlags contract.
        return object;
    }
    bool IsA(uintptr_t object, uintptr_t required) const {
        uintptr_t cls{};
        if (!required || !Read(object + l.ObjectClass, cls)) return false;
        for (unsigned i = 0; cls && i < 64; ++i) {
            if (cls == required) return true;
            uintptr_t next{};
            if (!Read(cls + l.StructSuper, next) || next == cls) return false;
            cls = next;
        }
        return false;
    }
    Handle Find(std::string_view path) const {
        Handle h{};
        if (objects->find_exact(objects->user, anomaly::sdk::StringView(path), &h).code) return {};
        return h;
    }
    // Parameter shapes are C++ call contracts; offsets to UE metadata live only in Profile.
    bool InvokeParameters(Handle receiver, std::string_view path, void* params, uint16_t size,
                          std::span<const Parameter> parameters, uint16_t expected_return = UINT16_MAX) const {
        const auto function = Find(path);
        const auto fn = Resolve(function), object = Resolve(receiver);
        const auto function_class = Resolve(Find("/Script/CoreUObject.Function"));
        uintptr_t owner{}; uint8_t count{}; uint16_t actual_size{}, return_offset{};
        if (!fn || !object || !IsA(fn, function_class) || !Read(fn + l.ObjectOuter, owner) || !IsA(object, owner) ||
            !Read(fn + l.FunctionCount, count) || count != parameters.size() ||
            !Read(fn + l.FunctionSize, actual_size) || actual_size != size ||
            !Read(fn + l.FunctionReturn, return_offset) || return_offset != expected_return) return false;
        uintptr_t field{};
        if (!Read(fn + l.StructProperties, field)) return false;
        for (const auto& parameter : parameters) {
            uintptr_t cls{}, next{}; int32_t offset{}, element{}, dim{};
            if (!field || Name(field + l.PropertyName) != parameter.name || !Read(field + l.PropertyClass, cls) ||
                Name(cls + l.FieldClassName) != parameter.type || !Read(field + l.PropertyOffset, offset) || offset != parameter.offset ||
                !Read(field + l.PropertySize, element) || element != parameter.size || !Read(field + l.PropertyDim, dim) || dim != 1 ||
                !Read(field + l.PropertyNext, next)) return false;
            if (!parameter.object_type.empty()) {
                uintptr_t property_class{};
                if (!Read(field + l.ObjectPropertyClass, property_class) || property_class != Resolve(Find(parameter.object_type))) return false;
            }
            field = next;
        }
        if (field) return false;
        return events->invoke(events->user, receiver, function, params, size).code == 0;
    }
    bool Invoke(Handle receiver, std::string_view path, void* params, uint16_t size,
                std::string_view parameter = {}, std::string_view type = {}, bool returns = false,
                std::string_view object_type = {}) const {
        const Parameter shape{parameter, type, 0, size, object_type};
        return InvokeParameters(receiver, path, params, size, {&shape, size ? 1u : 0u}, returns ? 0 : UINT16_MAX);
    }
    bool Level(Handle actor, uintptr_t& level) const {
        return Invoke(actor, "/Script/Engine.Actor:GetLevel", &level, sizeof(level), "ReturnValue", "ObjectProperty", true,
            "/Script/Engine.Level") && level && IsA(level, Resolve(Find("/Script/Engine.Level")));
    }
    uintptr_t WorldSubsystem(std::string_view type) const {
        Handle pawn{}; uintptr_t level{}, world{}, outer{};
        struct Params { uintptr_t context{}, type{}, result{}; } params;
        if (player->get_object_handles(player->user, &pawn, nullptr).code || !Level(pawn, level) ||
            !Read(level + l.ObjectOuter, world) || !IsA(world, Resolve(Find("/Script/Engine.World")))) return 0;
        params.context = level;
        params.type = Resolve(Find(type));
        if (!params.type) return 0;
        const std::array<Parameter, 3> shape{{
            {"ContextObject", "ObjectProperty", offsetof(Params, context), sizeof(uintptr_t), "/Script/CoreUObject.Object"},
            {"Class", "ClassProperty", offsetof(Params, type), sizeof(uintptr_t), "/Script/CoreUObject.Class"},
            {"ReturnValue", "ObjectProperty", offsetof(Params, result), sizeof(uintptr_t), "/Script/Engine.WorldSubsystem"}
        }};
        if (!InvokeParameters(Find("/Script/Engine.Default__SubsystemBlueprintLibrary"),
                "/Script/Engine.SubsystemBlueprintLibrary:GetWorldSubsystem", &params, sizeof(params), shape, offsetof(Params, result)) ||
            !IsA(params.result, params.type) || !Read(params.result + l.ObjectOuter, outer) || outer != world) return 0;
        return params.result;
    }
    static bool ReleasePoolCall(uintptr_t function, uintptr_t subsystem, const ClassSet& classes) noexcept {
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, const ClassSet*)>(function)(subsystem, &classes);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
public:
    static bool MatchesVehicle(std::string_view name, std::string_view tag) {
        const auto at = name.find(tag);
        return at != std::string_view::npos && (at == 0 || name[at - 1] == '_') &&
            (at + tag.size() == name.size() || name[at + tag.size()] == '_');
    }
    bool Load(const AnomalyHostApiV1* api) {
        ready = false;
        const anomaly::sdk::Host host(api);
        core = host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        if (!core || core->struct_size < offsetof(AnomalyCoreServiceV1, read_memory) + sizeof(core->read_memory) || !core->read_memory)
            return false;
        if (!profile.Load(api, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "mod-manager-v1"})) return false;
#define FIELD(name) l.name = profile.Value(#name, 65536)
        FIELD(ObjectClass); FIELD(ObjectName); FIELD(ObjectIndex); FIELD(ObjectOuter); FIELD(ObjectFlags); FIELD(StructSuper); FIELD(StructProperties);
        FIELD(PropertyName); FIELD(PropertyNext); FIELD(PropertyClass); FIELD(FieldClassName); FIELD(PropertyOffset); FIELD(PropertySize); FIELD(PropertyDim); FIELD(ObjectPropertyClass);
        FIELD(FunctionCount); FIELD(FunctionSize); FIELD(FunctionReturn); FIELD(RegistryCount); FIELD(RegistryItems); FIELD(ChunkSize); FIELD(ItemStride); FIELD(ItemSerial); FIELD(RegistrySubtract);
        FIELD(PoolOffset); FIELD(PoolElementStride);
        FIELD(TemplateActors); FIELD(TemplateActorStride); FIELD(TemplateActorReferences); FIELD(TemplateMap); FIELD(TemplateStride); FIELD(TemplateValue); FIELD(TemplateController); FIELD(SharedReferences);
        FIELD(TemplateFragments); FIELD(FragmentValueStride); FIELD(RepresentationSize); FIELD(RepresentationHigh); FIELD(RepresentationLow); FIELD(RepresentationVisual);
        FIELD(RepresentationManager); FIELD(ManagerArchetypes); FIELD(ArchetypeFragments); FIELD(ArchetypeFragmentData); FIELD(ArchetypeFragmentCount); FIELD(ArchetypeChunks); FIELD(ArchetypeCapacity);
        FIELD(FragmentConfigStride); FIELD(FragmentConfigOffset); FIELD(MassChunkStride); FIELD(MassChunkBytes); FIELD(MassChunkCount); FIELD(StructSize);
        FIELD(ManagerConstShared); FIELD(SharedHash); FIELD(SharedTypes); FIELD(SharedTypeStride); FIELD(SharedHashStride); FIELD(SharedMemoryData);
        FIELD(SharedTypeInstances); FIELD(SharedHashIndex); FIELD(SharedValueController);
        FIELD(SimulationSize); FIELD(SimulationClass); FIELD(StructAlignment);
        FIELD(TrafficSubsystemSize); FIELD(PhysicsTemplates); FIELD(PhysicsTemplateClass);
        FIELD(VisualizationComponent); FIELD(VisualInfos); FIELD(VisualFree); FIELD(VisualComponentMap); FIELD(VisualShared); FIELD(VisualSharedMap); FIELD(VisualDirty); FIELD(VisualSharedFree); FIELD(VisualMeshMap); FIELD(VisualPending); FIELD(VisualSize);
        FIELD(VisualInfoSize); FIELD(VisualDesc); FIELD(VisualDescSize); FIELD(VisualMeshes); FIELD(VisualComponents); FIELD(VisualRanges); FIELD(VisualMeshSize); FIELD(VisualMeshObject);
        FIELD(VisualMapStride); FIELD(VisualMapValue); FIELD(VisualMapNext); FIELD(VisualMapBucket); FIELD(VisualMeshMapKey);
        FIELD(VisualSharedSize); FIELD(VisualSharedTransform); FIELD(VisualSharedUpdates); FIELD(VisualSharedRemoves); FIELD(VisualSharedKey); FIELD(VisualSharedReferences); FIELD(VisualSharedInnerMap);
#undef FIELD
        if (profile.Value("ClassSetSize", 256) != sizeof(ClassSet) ||
            profile.Value("ClassElementStride", 256) != sizeof(ClassElement) ||
            profile.Value("SetDataCount", 256) != offsetof(ClassSet, count) ||
            profile.Value("SetAllocationBits", 256) != offsetof(ClassSet, bits) ||
            profile.Value("SetIndirectBits", 256) != offsetof(ClassSet, indirect_bits) ||
            profile.Value("SetBitCount", 256) != offsetof(ClassSet, bit_count) ||
            profile.Value("SetFreeCount", 256) != offsetof(ClassSet, free_count) ||
            profile.Value("SetHashInline", 256) != offsetof(ClassSet, hash) ||
            profile.Value("SetHashAllocation", 256) != offsetof(ClassSet, indirect_hash) ||
            profile.Value("SetHashCount", 256) != offsetof(ClassSet, hash_count) ||
            l.PoolOffset % alignof(uintptr_t) || l.PoolElementStride < sizeof(ClassElement) || l.PoolElementStride > 256) return false;
        if (!l.ChunkSize || l.ItemStride < sizeof(uintptr_t) || l.ItemSerial + sizeof(uint32_t) > l.ItemStride ||
            l.ObjectClass % alignof(uintptr_t) || l.StructSuper % alignof(uintptr_t) || l.PropertyNext % alignof(uintptr_t)) return false;
        const auto instruction = profile.Address("Objects"); int32_t displacement{};
        // x64 MOV reg, [RIP+rel32] decoding; instruction shape is verified by Profile.
        if (!instruction || !Read(instruction + 3, displacement)) return false;
        registry = static_cast<uintptr_t>(static_cast<intptr_t>(instruction) + 7 + displacement - l.RegistrySubtract);
        release_pooled = profile.Address("ReleasePooledVehicleClasses");
        remove_template = profile.Address("RemoveEntityTemplate");
        release_template = profile.Address("ReleaseActorTemplate");
        remove_shared_hash = profile.Address("RemoveSharedHash");
        destroy_struct = profile.Address("DestroyScriptStruct");
        initialize_struct = profile.Address("InitializeScriptStruct");
        remove_visual_hash = profile.Address("RemoveVisualHash");
        destroy_visual_shared = profile.Address("DestroyVisualShared");
        initialize_instance_map = profile.Address("InitializeInstanceMap");
        grow_array = profile.Address("GrowArray");
        const auto physics_lookup = profile.Address("PhysicsTemplateLookup");
        int32_t physics_templates{};
        // The validated LEA r13,[rcx+disp32] selects the cache array. Its lookup
        // reads the class from the first pointer-sized field and skips null keys.
        if (!physics_lookup || !Read(physics_lookup + 3, physics_templates) ||
            physics_templates < 0 || static_cast<uint32_t>(physics_templates) != l.PhysicsTemplates ||
            l.PhysicsTemplateClass != 0 || l.PhysicsTemplates % alignof(uintptr_t) ||
            l.PhysicsTemplates + 2 * sizeof(uintptr_t) > l.TrafficSubsystemSize) return false;
        return registry && release_pooled && remove_template && release_template && remove_shared_hash && destroy_struct &&
            initialize_struct && remove_visual_hash && destroy_visual_shared && initialize_instance_map && grow_array &&
            l.VisualInfos % alignof(uintptr_t) == 0 && l.VisualFree == l.VisualInfos + 2 * sizeof(uintptr_t) &&
            l.VisualComponentMap == l.VisualFree + 2 * sizeof(uintptr_t) && l.VisualShared == l.VisualComponentMap + sizeof(ClassSet) &&
            l.VisualSharedMap == l.VisualShared + 2 * sizeof(uintptr_t) && l.VisualDirty == l.VisualSharedMap + sizeof(ClassSet) &&
            l.VisualSharedFree == l.VisualDirty + 4 * sizeof(uintptr_t) && l.VisualMeshMap == l.VisualSharedFree + 2 * sizeof(uintptr_t) &&
            l.VisualPending == l.VisualMeshMap + sizeof(ClassSet) && l.VisualSize == l.VisualPending + 2 * sizeof(uintptr_t) &&
            l.VisualSharedSize <= 1024 && l.VisualSharedSize % 16 == 0 && l.VisualSharedTransform == 0 &&
            l.VisualSharedKey + 2 * sizeof(uint32_t) == l.VisualSharedReferences &&
            l.VisualSharedInnerMap > l.VisualSharedReferences && l.VisualSharedInnerMap < l.VisualSharedSize &&
            l.VisualInfoSize <= 1024 && l.VisualInfoSize % 16 == 0 && l.VisualDesc + l.VisualDescSize == l.VisualComponents &&
            l.VisualRanges == l.VisualComponents + 2 * sizeof(uintptr_t) && l.VisualInfoSize == l.VisualRanges + 2 * sizeof(uintptr_t) &&
            l.VisualMeshSize <= 1024 && l.VisualMeshSize % 16 == 0 && l.VisualMeshObject + sizeof(uintptr_t) <= l.VisualMeshSize &&
            l.ManagerConstShared % alignof(uintptr_t) == 0 && l.SharedHash == 2 * sizeof(uintptr_t) &&
            l.SharedTypes == l.SharedHash + sizeof(ClassSet) && l.SimulationSize <= 1024 &&
            l.SimulationClass + sizeof(uintptr_t) <= l.SimulationSize &&
            l.TemplateActorStride >= sizeof(uintptr_t) + sizeof(uint32_t) && l.TemplateActorStride <= 64 &&
            l.TemplateActorReferences + sizeof(uint32_t) <= l.TemplateActorStride &&
            l.TemplateStride <= 128 && l.TemplateValue + sizeof(uintptr_t) <= l.TemplateStride &&
            l.TemplateController + sizeof(uintptr_t) <= l.TemplateStride && l.SharedReferences < 64 &&
            l.FragmentValueStride == 2 * sizeof(uintptr_t) && l.FragmentConfigStride == 2 * sizeof(uintptr_t) &&
            l.FragmentConfigOffset == sizeof(uintptr_t) && l.RepresentationSize <= 1024 &&
            l.RepresentationHigh + sizeof(int16_t) <= l.RepresentationSize &&
            l.RepresentationLow + sizeof(int16_t) <= l.RepresentationSize &&
            l.RepresentationVisual + sizeof(uint16_t) <= l.RepresentationSize &&
            l.ArchetypeFragmentData > l.ArchetypeFragments &&
            l.ArchetypeFragmentCount == l.ArchetypeFragmentData + sizeof(uintptr_t) &&
            l.MassChunkStride <= 1024 && l.MassChunkBytes + sizeof(uint64_t) <= l.MassChunkStride &&
            l.MassChunkCount + sizeof(int32_t) <= l.MassChunkStride;
    }
    // Service publication can lag plugin loading. Retry only these cheap queries, never the
    // profile scan. The caller publishes this object to Game only after all bindings are ready.
    bool BindServices(const AnomalyHostApiV1* api) {
        const anomaly::sdk::Host host(api);
        names = host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
        objects = host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        events = host.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID).get();
        player = host.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get();
        return ready = registry && names &&
            names->struct_size >= offsetof(AnomalyUe5NamesServiceV1, resolve_utf8) + sizeof(names->resolve_utf8) && names->resolve_utf8 &&
            objects && objects->struct_size >= sizeof(*objects) && events && events->struct_size >= sizeof(*events) && events->invoke &&
            player && player->struct_size >= offsetof(AnomalyNtePlayerServiceV1, get_object_handles) + sizeof(player->get_object_handles) && player->get_object_handles;
    }
    bool PlayerVehicle(Handle& pawn, Handle& vehicle) const {
        vehicle = {};
        Handle controller{};
        if (!ready || player->get_object_handles(player->user, &pawn, &controller).code) return false;
        // Use the same reflected getters as NteVehicle, without depending on that plugin.
        // A successful null return means unoccupied; failed calls cannot establish that.
        struct Getter { Handle receiver; std::string_view path; };
        const std::array<Getter, 3> getters{{
            {controller, "/Script/HTGame.HTPlayerController:BP_GetCurrentDriveVehicle"},
            {pawn, "/Script/HTGame.HTPlayerCharacter:BP_GetCurrentDirvingVehicle"},
            {pawn, "/Script/HTGame.HTPlayerCharacter:GetBoardWaterVehicle"}
        }};
        bool queried{};
        for (const auto& getter : getters) {
            uintptr_t object{};
            if (!Invoke(getter.receiver, getter.path, &object, sizeof(object), "ReturnValue", "ObjectProperty", true)) continue;
            queried = true;
            if (!object) continue;
            uint32_t index{};
            AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
            if (!IsA(object, Resolve(Find("/Script/Engine.Actor"))) || !Read(object + l.ObjectIndex, index) ||
                objects->snapshot_at(objects->user, index, &snapshot).code || Resolve(snapshot.handle) != object) return false;
            vehicle = snapshot.handle;
            return true;
        }
        return queried;
    }
    bool Actor(Handle handle, std::string_view expected_class) {
        const auto object = Resolve(handle); uintptr_t cls{}, level{}, player_level{}, world{}, player_world{}; Handle pawn{}; uint32_t flags{};
        return object && Read(object + l.ObjectFlags, flags) && !(flags & 0x30u) && // CDO / archetype cannot be a world instance.
            Read(object + l.ObjectClass, cls) && Name(cls + l.ObjectName) == expected_class &&
            IsA(object, Resolve(Find("/Script/Engine.Actor"))) &&
            player->get_object_handles(player->user, &pawn, nullptr).code == 0 &&
            Level(handle, level) && Level(pawn, player_level) &&
            Read(level + l.ObjectOuter, world) && Read(player_level + l.ObjectOuter, player_world) &&
            world == player_world && IsA(world, Resolve(Find("/Script/Engine.World")));
    }
    bool Destroy(Handle handle) {
        // The game owns Mass vehicle entities as well as their Actor representations.
        // Its safe-destroy route retires that entity; destroying only the Actor leaves it active.
        // Keep the delay positive: the non-Mass route uses SetLifeSpan, where zero cancels expiry.
        float life_span = 0.1f;
        return Invoke(handle, "/Script/HTGame.HTWheeledVehicleDrivable:SafeDestroyVehicle",
            &life_span, sizeof(life_span), "LifeSpanTime", "FloatProperty");
    }
    bool ReleasePools(const std::vector<std::string>& tags) const {
        const auto subsystem = WorldSubsystem("/Script/MassActors.MassActorSpawnerSubsystem");
        ClassSet pool{};
        if (!subsystem || !Read(subsystem + l.PoolOffset, pool) || pool.count < 0 || pool.count > 4096 ||
            pool.capacity < pool.count || pool.bit_count != pool.count || pool.free_count < 0 || pool.free_count > pool.count ||
            (pool.count && !pool.data) || pool.bit_capacity < pool.bit_count ||
            (!pool.indirect_bits && pool.bit_count > sizeof(pool.bits) * 8)) return false;
        const auto class_type = Resolve(Find("/Script/CoreUObject.Class"));
        const auto bits = pool.indirect_bits ? pool.indirect_bits : subsystem + l.PoolOffset + offsetof(ClassSet, bits);
        std::vector<uintptr_t> classes;
        uint32_t allocated{};
        for (int32_t i = 0; i < pool.count; ++i) {
            if (i % 32 == 0 && !Read(bits + sizeof(uint32_t) * (i / 32), allocated)) return false;
            if (!(allocated & (uint32_t{1} << (i % 32)))) continue;
            uintptr_t cls{};
            if (!Read(pool.data + uint64_t(l.PoolElementStride) * i, cls) || !IsA(cls, class_type)) return false;
            const auto name = Name(cls + l.ObjectName);
            if (std::any_of(tags.begin(), tags.end(), [&](const auto& tag) { return MatchesVehicle(name, tag); })) classes.push_back(cls);
        }
        // The native routine removes only these class keys, drains their pending-destroy
        // entries, updates pool counters, and calls UWorld::DestroyActor for their instances.
        // Snapshot the keys first because each call mutates the pool's sparse map.
        for (const auto cls : classes) {
            const ClassElement element{cls};
            ClassSet selection{};
            selection.data = reinterpret_cast<uintptr_t>(&element);
            if (!ReleasePoolCall(release_pooled, subsystem, selection)) return false;
        }
        return true;
    }
    bool Clear() {
        Handle pawn{}; uintptr_t level{};
        return ready && player->get_object_handles(player->user, &pawn, nullptr).code == 0 && Level(pawn, level) &&
            Invoke(Find("/Script/HTGame.Default__HTGameUserSettings"),
                "/Script/HTGame.HTGameUserSettings:ClearCacheUIAndGC", &level, sizeof(level),
                "WorldContextObject", "ObjectProperty", false, "/Script/CoreUObject.Object");
    }
};
} // namespace mod_manager
