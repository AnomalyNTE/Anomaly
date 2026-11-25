#include "runtime.hpp"
#include "backend.hpp"
#include "durability_guard.hpp"
#include <memory>

namespace nte_vehicle {
void Runtime::RestoreLocalVehicleChangesLocked(std::uintptr_t previous_world) {
    // Undo only changes on still-live objects owned by the previously approved
    // world. Never resolve a new driving vehicle after permission was lost.
    const auto owned = [&](std::uintptr_t object, AnomalyGenerationHandleV1 handle) {
        if (!previous_world || !IsCurrentObject(handle, object)) return false;
        for (unsigned depth = 0; object && depth < 8; ++depth) {
            if (!ReadPointerAt(*memory, object, layout.object_outer, object)) return false;
            if (object == previous_world) return true;
        }
        return false;
    };
    const auto reset_torque = [&](std::uintptr_t movement, float base, bool has_base) {
        const auto invoke = [&](const VehicleFunctionBinding& binding, float value) {
            if (!binding.function || binding.parms_size != sizeof(float) || binding.parameter_offset != 0)
                return;
            static_cast<void>(InvokeProcessEventGuarded(process_event_invoker, movement,
                binding.function, &value, sizeof(value)));
        };
        invoke(vehicle_bindings.set_external_torque_ratio, 1.0F);
        if (has_base) invoke(vehicle_bindings.set_max_engine_torque, base);
    };
    std::string failure;
    if (owned(vehicle_movement_component, movement_handle)) {
        if (vehicle_speed_fields_applied && vehicle_speed_field_baseline.valid &&
            vehicle_speed_field_baseline_component == vehicle_movement_component)
            static_cast<void>(WriteVehicleSpeedFieldSnapshotLocked(
                vehicle_movement_component, vehicle_speed_field_baseline, failure));
        if (vehicle_speed_fields_applied || vehicle_requested_engine_torque_ratio != 1.0F)
            reset_torque(vehicle_movement_component, vehicle_base_engine_torque,
                vehicle_base_engine_torque_valid);
    }
    for (const auto& [movement, handle] : other_vehicle_handles) {
        if (!owned(movement, handle)) continue;
        if (const auto base = other_vehicle_speed_field_baselines.find(movement);
            base != other_vehicle_speed_field_baselines.end())
            static_cast<void>(WriteVehicleSpeedFieldSnapshotLocked(movement, base->second, failure));
        const auto torque = other_vehicle_base_torques.find(movement);
        reset_torque(movement, torque == other_vehicle_base_torques.end() ? 0.0F : torque->second,
            torque != other_vehicle_base_torques.end());
    }
}

void Runtime::Tick() {
    std::scoped_lock lock(mutex);
    const auto previous_generation = object_generation;
    const auto previous_world = world_pointer;
    const bool previously_allowed = ready;
    SetDurabilityGuardWorld({});
    const bool allowed = Refresh();
    if (previously_allowed && (!allowed || previous_world != world_pointer)) {
        RestoreLocalVehicleChangesLocked(previous_world);
    }
    if (!allowed || previous_generation != object_generation || previous_world != world_pointer)
        ClearObjectCaches();
    if (!allowed) return;
    SetDurabilityGuardWorld({world_pointer, world_address, layout.object_outer,
        local_world_bindings.world_game_state, local_world_bindings.racing_mode});
    MaintainVehicleSpeedFieldsLocked();
}

void Runtime::ClearObjectCaches() {
    RestoreWheelFrictionLocked();
    vehicle_bindings = {};
    current_vehicle_object = vehicle_movement_component = 0;
    current_vehicle_handle = movement_handle = {};
    vehicle_speed_maintenance.Reset();
    vehicle_valid = false;
    vehicle_speed_fields_applied = false;
    vehicle_speed_field_baseline = {};
    vehicle_speed_field_baseline_component = 0;
    vehicle_base_engine_torque_valid = false;
    vehicle_top_speed_ratio = vehicle_current_velocity_ratio = 1.0F;
    vehicle_engine_torque_ratio = vehicle_requested_engine_torque_ratio = 1.0F;
    advanced_tuning_cache.clear();
    other_vehicle_handles.clear();
    other_vehicle_velocity_ratios.clear();
    other_vehicle_base_torques.clear();
    other_vehicle_torque_fallback_ratios.clear();
    other_vehicle_requested_torque_ratios.clear();
    other_vehicle_speed_field_baselines.clear();
    other_vehicle_speed_field_ratios.clear();
    vehicle_catalog_valid = false;
    vehicle_catalog_table = vehicle_data_asset_object = 0;
    vehicle_catalog_handle = vehicle_data_asset_handle = {};
    vehicle_display_name_cache.clear();
    vehicle_customization_generation = 0;
}

namespace { std::unique_ptr<Runtime> backend; }
bool LoadBackend(const AnomalyHostApiV1* host, const anomaly::sdk::Localizer& localizer) {
    backend_localizer = &localizer;
    backend_messages.clear();
    backend = std::make_unique<Runtime>();
    if (backend->Load(host)) return true;
    backend.reset();
    return false;
}
Runtime* Backend() { return backend.get(); }
void TickBackend() { if (backend) backend->Tick(); }
void StopBackend() {
    SetDurabilityGuardWorld({});
    if (backend) {
        std::scoped_lock lock(backend->mutex);
        backend->vehicle_speed_maintenance.Reset();
        backend->ready = false;
    }
}
void RestoreWheelFrictionBackend() {
    if (backend) backend->VehicleRestoreWheelFriction();
}
void UnloadBackend() {
    backend.reset();
    backend_messages.clear();
    backend_localizer = nullptr;
}
} // namespace nte_vehicle
