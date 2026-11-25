#include "native_access.hpp"

namespace nte_vehicle {

bool NativeAccess::Load(const AnomalyHostApiV1* api) {
    host_api = api;
    const anomaly::sdk::Host host(api);
    storage.core = host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
    if (!storage.core || storage.core->struct_size < sizeof(*storage.core) || !storage.core->read_memory || !storage.core->write_memory ||
        !compatibility.Load(api, "compatibility.profile.json",
            {"nte", "HTGame.exe", ".text", "nte-vehicle-v1"})) return false;
    try {
        layout.Load(compatibility.Document());
        const auto instruction = compatibility.Address("Objects");
        std::int32_t displacement{};
        if (!instruction || !ReadValue(*memory,
                instruction + layout.objects_instructionDisplacement, displacement)) return false;
        registry_address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction) +
            layout.objects_instructionSize + displacement -
            layout.objects_registrySubtract);
        const auto world_instruction = compatibility.Address("World");
        if (!world_instruction || layout.world_instructionDisplacement + static_cast<std::int64_t>(sizeof(displacement)) !=
                layout.world_instructionSize ||
            !ReadValue(*memory, world_instruction + layout.world_instructionDisplacement, displacement)) return false;
        world_address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(world_instruction) +
            layout.world_instructionSize + displacement);
        process_event_invoker = [this](std::uintptr_t object, std::uintptr_t function,
                                      void* parameters, std::size_t size) {
            // The bridge services are bound lazily, so read them at call time.
            const auto* const bridge = events;
            if (!bridge) return false;
            AnomalyGenerationHandleV1 receiver{}, method{};
            return ObjectHandleLocked(object, receiver) && ObjectHandleLocked(function, method) &&
                bridge->invoke(bridge->user, receiver, method, parameters, size).code == ANOMALY_STATUS_V1_OK;
        };
    } catch (...) { return false; }
    static_cast<void>(BindGameServices());
    return true;
}

bool NativeAccess::BindGameServices() {
    if (names && objects && events && framework && player && session) return true;
    const anomaly::sdk::Host host(host_api);
    names = host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
    objects = host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
    events = host.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID).get();
    framework = host.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
    player = host.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get();
    session = host.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
    return names && names->struct_size >= offsetof(AnomalyUe5NamesServiceV1, find_utf8) &&
        names->resolve_utf8 && objects && objects->struct_size >= sizeof(*objects) &&
        objects->snapshot_at && objects->snapshot_by_handle && objects->find_exact && objects->generation &&
        events && events->struct_size >= sizeof(*events) && events->invoke &&
        framework && framework->struct_size >= sizeof(*framework) && framework->is_game_thread &&
        player && player->struct_size >= sizeof(*player) && player->get_object_handles &&
        session && session->struct_size >= sizeof(*session) && session->snapshot;
}

bool NativeAccess::LoadRegistry(ObjectRegistryState& output) const {
    ObjectRegistryState next{};
    if (!ReadPointerAt(*memory, registry_address, layout.objects_itemsOffset, next.items) ||
        !ReadValue(*memory, registry_address + layout.objects_countOffset, next.count) ||
        !ReadValue(*memory, registry_address + layout.objects_maxCountOffset, next.max_count) ||
        !next.count || next.count > next.max_count || next.max_count > 16 * 1024 * 1024) return false;
    next.chunk_size = static_cast<std::uint32_t>(layout.objects_chunkSize);
    next.item_stride = static_cast<std::uint32_t>(layout.objects_itemStride);
    next.object_offset = static_cast<std::uint32_t>(layout.objects_objectOffset);
    next.serial_offset = static_cast<std::uint32_t>(layout.objects_serialOffset);
    if (!next.chunk_size || next.item_stride < sizeof(void*) || next.item_stride > 256 ||
        next.object_offset > next.item_stride - sizeof(void*) ||
        next.serial_offset > next.item_stride - sizeof(std::uint32_t)) return false;
    output = next;
    return true;
}

bool NativeAccess::ResolveObjectHandleLocked(AnomalyGenerationHandleV1 handle, std::uintptr_t& object) const {
    object = 0;
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (!handle.id || objects->snapshot_by_handle(objects->user, handle, &snapshot).code != 0) return false;
    ObjectRegistryState registry;
    std::uint32_t serial{};
    std::int32_t index{};
    if (!LoadRegistry(registry) || !ReadObjectSlot(*memory, registry,
            ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle), object, serial) || !object ||
        serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
        !ReadValue(*memory, object + layout.object_internalIndex, index) ||
        index < 0 || static_cast<std::uint32_t>(index) != ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)) {
        object = 0;
        return false;
    }
    return true;
}

bool NativeAccess::ObjectHandleLocked(std::uintptr_t object, AnomalyGenerationHandleV1& handle) const {
    handle = {};
    std::int32_t index{};
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    std::uintptr_t resolved{};
    if (!object || !ReadValue(*memory, object + layout.object_internalIndex, index) || index < 0 ||
        objects->snapshot_at(objects->user, static_cast<std::uint32_t>(index), &snapshot).code != 0 ||
        !ResolveObjectHandleLocked(snapshot.handle, resolved) || resolved != object) return false;
    handle = snapshot.handle;
    return true;
}

bool NativeAccess::Refresh() {
    ready = false;
    player_pawn = player_controller = world_pointer = 0;
    if (!BindGameServices() || !framework->is_game_thread(framework->user)) return false;
    game_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
    if (!LoadRegistry(object_registry)) return false;
    object_generation = objects->generation(objects->user);
    if (layout_generation != object_generation) {
        if (!ResolveVehicleLayout()) return false;
        layout_generation = object_generation;
    }
    if (local_world_bindings.generation != object_generation &&
        !ResolveLocalWorldBindings()) return false;
    AnomalyNteSessionSnapshotV1 current{sizeof(current)};
    if (session->snapshot(session->user, &current).code != ANOMALY_STATUS_V1_OK ||
        current.state != ANOMALY_NTE_SESSION_V1_WORLD_READY ||
        !current.world.id || !current.world.generation ||
        !ReadValue(*memory, world_address, world_pointer) || !world_pointer) return false;
    AnomalyGenerationHandleV1 pawn{}, controller{};
    if (player->get_object_handles(player->user, &pawn, &controller).code != 0 ||
        !ResolveObjectHandleLocked(pawn, player_pawn) || !ResolveObjectHandleLocked(controller, player_controller))
        return false;
    // Resolve the current World through the pawn's Level owner on the Game thread.
    std::uintptr_t level{}, pawn_world{};
    ready = ReadPointerAt(*memory, player_pawn, layout.object_outer, level) &&
        ReadPointerAt(*memory, level, layout.object_outer, pawn_world) &&
        pawn_world == world_pointer && ObjectClassChainContainsLocked(world_pointer, "World") &&
        ReadLocalWorldPermission();
    if (!ready) player_pawn = player_controller = 0;
    return ready;
}

bool NativeAccess::ResolveLocalWorldBindings() {
    LocalWorldBindings next;
    std::uintptr_t util{}, world_class{}, state_class{}, owner{};
    if (!FindExactObjectLocked(L"/Script/HTGame.HTUtil", util) ||
        !FindExactObjectLocked(L"/Script/HTGame.HTUtil.IsHybridMode_ClientOrStandalone", next.function) ||
        !FindExactObjectLocked(L"/Script/Engine.World", world_class) ||
        !FindExactObjectLocked(L"/Script/HTGame.HTGameState", state_class) ||
        !ReadPointerAt(*memory, util, layout.uclass_classDefaultObject, next.receiver) ||
        !ReadPointerAt(*memory, next.function, layout.object_outer, owner) || owner != util ||
        !ObjectClassChainContainsLocked(next.function, "Function")) return false;

    // Dump 5.6.1: UObject* WorldContextObject at 0, native bool return at 8.
    // ParmsSize is 9, even though a C++ parameter struct would have tail padding.
    std::uint8_t count{};
    std::uint16_t size{}, returned{};
    ReflectedPropertyInfo context, result;
    ReflectedBoolParameter result_bool;
    if (!ReadValue(*memory, next.function + layout.ufunction_numParms, count) || count != 2 ||
        !ReadValue(*memory, next.function + layout.ufunction_parmsSize, size) || size != 9 ||
        !ReadValue(*memory, next.function + layout.ufunction_returnValueOffset, returned) || returned != 8 ||
        !FindReflectedPropertyLocked(next.function, "WorldContextObject", context, false) ||
        context.type != "ObjectProperty" || context.element_size != sizeof(void*) || context.offset != 0 ||
        !FindReflectedPropertyLocked(next.function, "ReturnValue", result, false) ||
        !ReadReflectedBoolParameterLocked(result, size, result_bool) ||
        result_bool.byte_offset != 8 || result_bool.byte_mask != 1 || result_bool.field_mask != 255)
        return false;

    const auto field = [&](std::uintptr_t type, std::string_view name,
                           std::string_view expected, std::int32_t bytes, std::int64_t& offset) {
        ReflectedPropertyInfo property;
        std::int32_t owner_size{};
        if (!ReadValue(*memory, type + layout.ustruct_propertiesSize, owner_size) ||
            !FindReflectedPropertyLocked(type, name, property, true) || property.type != expected ||
            property.element_size != bytes || property.offset % bytes != 0 ||
            property.offset + bytes > owner_size) return false;
        offset = property.offset;
        return true;
    };
    if (!field(world_class, "GameState", "ObjectProperty", sizeof(void*), next.world_game_state) ||
        !field(state_class, "CurrentRacingGameMode", "ByteProperty", 1, next.racing_mode)) return false;
    next.generation = object_generation;
    local_world_bindings = next;
    return true;
}

bool NativeAccess::ReadLocalWorldPermission() const {
    if (!world_pointer || !process_event_invoker) return false;
    std::uintptr_t state{};
    std::uint8_t racing{};
    if (!ReadPointerAt(*memory, world_pointer, local_world_bindings.world_game_state, state) ||
        !ObjectClassChainContainsLocked(state, "HTGameState") ||
        !ReadValue(*memory, state + local_world_bindings.racing_mode, racing) ||
        !IsLocalRacingMode(racing)) return false;
    std::array<std::uint8_t, 9> parameters{};
    std::memcpy(parameters.data(), &world_pointer, sizeof(world_pointer));
    return InvokeProcessEventGuarded(process_event_invoker, local_world_bindings.receiver,
        local_world_bindings.function, parameters.data(), parameters.size()) && parameters[8] == 1;
}

bool NativeAccess::ResolveVehicleLayout() {
    std::uintptr_t movement{}, vehicle{};
    if (!FindExactObjectLocked(L"/Script/HTGame.HTVehicleMovementComponent", movement) ||
        !FindExactObjectLocked(L"/Script/HTGame.HTWheeledVehicleBase", vehicle)) return false;
    auto fields = layout;
    const auto resolve = [&](std::uintptr_t owner, std::string_view name, std::string_view type,
                             std::int32_t size, std::int64_t& offset) {
        ReflectedPropertyInfo property;
        std::int32_t owner_size{};
        if (!ReadValue(*memory, owner + layout.ustruct_propertiesSize, owner_size) ||
            !FindReflectedPropertyLocked(owner, name, property, true) || property.type != type ||
            property.element_size != size || property.offset % size != 0 ||
            property.offset + size > owner_size) return false;
        offset = property.offset;
        return true;
    };
    if (!resolve(vehicle, "HTVehicleMovementComponent", "ObjectProperty", sizeof(void*),
            fields.vehicle_movementComponent)) return false;
    for (const auto& [name, offset] : std::array{
            std::pair{"MaxLinearVelocity", &fields.vehicle_maxLinearVelocity},
            std::pair{"TopSpeedInKmH", &fields.vehicle_topSpeedInKmH},
            std::pair{"ReverseTopSpeedInKmH", &fields.vehicle_reverseTopSpeedInKmH},
            std::pair{"TopSpeedRatio", &fields.vehicle_topSpeedRatio},
            std::pair{"ComfortTopSpeedInKmH", &fields.vehicle_comfortTopSpeedInKmH}}) {
        if (!resolve(movement, name, "FloatProperty", sizeof(float), *offset)) return false;
    }
    ReflectedPropertyInfo enabled;
    ReflectedBoolParameter enabled_bool;
    std::int32_t movement_size{};
    if (!ReadValue(*memory, movement + layout.ustruct_propertiesSize, movement_size) ||
        movement_size <= 0 || movement_size > 65535 ||
        !FindReflectedPropertyLocked(movement, "bEnableTopSpeedLimit", enabled, true) ||
        !ReadReflectedBoolParameterLocked(enabled, static_cast<std::uint16_t>(movement_size), enabled_bool) ||
        enabled_bool.byte_mask != 1 || enabled_bool.field_mask != 255) return false;
    fields.vehicle_enableTopSpeedLimit = enabled_bool.byte_offset;
    fields.vehicle_movementComponentSize = static_cast<std::uint32_t>(movement_size);
    for (const auto& [name, torque_offset, rpm_offset] : std::array{
            std::tuple{"EngineSetup", &fields.vehicle_maxEngineTorque, &fields.vehicle_maxEngineRPM},
            std::tuple{"ComfortEngineSetup", &fields.vehicle_comfortMaxEngineTorque, &fields.vehicle_comfortMaxEngineRPM}}) {
        ReflectedPropertyInfo config, torque, rpm;
        std::string structure;
        if (!FindReflectedPropertyLocked(movement, name, config, true) || config.type != "StructProperty" ||
            !config.structure || !ReadReflectedObjectNameLocked(config.structure, structure) ||
            structure != "VehicleEngineConfig" ||
            !FindReflectedPropertyLocked(config.structure, "MaxTorque", torque, true) ||
            !FindReflectedPropertyLocked(config.structure, "MaxRPM", rpm, true) ||
            torque.type != "FloatProperty" || torque.element_size != sizeof(float) ||
            rpm.type != "FloatProperty" || rpm.element_size != sizeof(float) ||
            torque.offset + torque.element_size > config.element_size ||
            rpm.offset + rpm.element_size > config.element_size ||
            config.offset + config.element_size > movement_size) return false;
        *torque_offset = config.offset + torque.offset;
        *rpm_offset = config.offset + rpm.offset;
    }
    // Reject overlapping or unaligned reflected fields before allowing raw writes.
    std::array offsets{fields.vehicle_maxLinearVelocity, fields.vehicle_topSpeedInKmH,
        fields.vehicle_reverseTopSpeedInKmH, fields.vehicle_topSpeedRatio,
        fields.vehicle_comfortTopSpeedInKmH, fields.vehicle_maxEngineTorque,
        fields.vehicle_maxEngineRPM, fields.vehicle_comfortMaxEngineTorque,
        fields.vehicle_comfortMaxEngineRPM};
    std::ranges::sort(offsets);
    constexpr std::int64_t float_size = sizeof(float);
    for (std::size_t i{}; i < offsets.size(); ++i) {
        const auto offset = offsets[i];
        if (offset < 0 || offset % alignof(float) != 0 || offset + float_size > movement_size ||
            (i != 0 && offset < offsets[i - 1] + float_size) ||
            (fields.vehicle_enableTopSpeedLimit >= offset &&
             fields.vehicle_enableTopSpeedLimit < offset + float_size)) return false;
    }
    layout = fields;
    return true;
}
} // namespace nte_vehicle
