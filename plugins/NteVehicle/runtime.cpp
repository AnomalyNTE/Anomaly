#include "runtime.hpp"
#include "backend.hpp"
#include <memory>

namespace nte_vehicle {
void Runtime::Tick() {
    std::scoped_lock lock(mutex);
    const auto previous_generation = object_generation;
    const auto previous_world = world_pointer;
    if (!Refresh()) {
        vehicle_speed_maintenance.Reset();
        vehicle_valid = false;
        return;
    }
    if (previous_generation != object_generation || previous_world != world_pointer)
        ClearObjectCaches();
    MaintainVehicleSpeedFieldsLocked();
}

void Runtime::ClearObjectCaches() {
    vehicle_bindings = {};
    current_vehicle_object = vehicle_movement_component = 0;
    current_vehicle_handle = movement_handle = {};
    vehicle_speed_maintenance.Reset();
    vehicle_speed_fields_applied = false;
    vehicle_speed_field_baseline = {};
    vehicle_speed_field_baseline_component = 0;
    vehicle_base_engine_torque_valid = false;
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
    if (backend) {
        std::scoped_lock lock(backend->mutex);
        backend->vehicle_speed_maintenance.Reset();
        backend->ready = false;
    }
}
void UnloadBackend() {
    backend.reset();
    backend_messages.clear();
    backend_localizer = nullptr;
}
} // namespace nte_vehicle
