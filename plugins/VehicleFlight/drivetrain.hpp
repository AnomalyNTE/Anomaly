#pragma once
#include "engine.hpp"

namespace vehicle_flight {
class DrivetrainControl {
public:
    explicit DrivetrainControl(Engine& engine) : engine_(engine) {}

    bool Bind() {
        if (movement_get_.function && clear_.function && mechanical_set_.function &&
            parked_get_.function && parked_set_.function) return true;
        movement_get_ = engine_.Bind("HTGame", "HTWheeledVehicleBase", "GetWheeledVehicleMovement", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        clear_ = engine_.Bind("HTGame", "HTVehicleMovementComponent", "ClearAllInputs", 0, 0, {});
        mechanical_set_ = engine_.Bind("ChaosVehicles", "ChaosWheeledVehicleMovementComponent", "EnableMechanicalSim", 1, 1,
            {{"InState", 0, 1, "BoolProperty"}});
        parked_get_ = engine_.Bind("ChaosVehicles", "ChaosVehicleMovementComponent", "IsParked", 1, 1,
            {{"ReturnValue", 0, 1, "BoolProperty"}});
        parked_set_ = engine_.Bind("ChaosVehicles", "ChaosVehicleMovementComponent", "SetParked", 1, 1,
            {{"bParked", 0, 1, "BoolProperty"}});
        return movement_get_.function && clear_.function && mechanical_set_.function &&
            parked_get_.function && parked_set_.function;
    }

    bool Hold(Ref vehicle) {
        if (vehicle == actor_) return Clear();
        if (!Restore()) return false;
        if (!engine_.IsA(vehicle, "HTWheeledVehicleBase")) return true;
        uintptr_t address{};
        if (!engine_.Call(vehicle, movement_get_, &address, sizeof(address))) return false;
        const auto movement = engine_.Reference(address);
        if (!engine_.IsA(movement, "HTVehicleMovementComponent")) return false;
        uintptr_t type{};
        if (!engine_.Read(movement.address + engine_.layout.object_class, type)) return false;
        const int offset = engine_.Field(type, "bMechanicalSimEnabled", 1, "BoolProperty");
        uint8_t mechanical{}, parked{};
        if (offset < 0 || !engine_.Read(movement.address + offset, mechanical) || mechanical > 1 ||
            !engine_.Call(movement, parked_get_, &parked, 1)) return false;
        actor_ = vehicle;
        movement_ = movement;
        original_mechanical_ = mechanical;
        original_parked_ = parked;
        uint8_t enabled{1}, disabled{};
        if (!Clear() || !engine_.Call(movement_, parked_set_, &enabled, 1) ||
            !engine_.Call(movement_, mechanical_set_, &disabled, 1)) {
            static_cast<void>(Restore());
            return false;
        }
        return true;
    }

    bool Restore() {
        if (!Active()) return true;
        if (const auto movement = engine_.Resolve(movement_.handle)) {
            // Clear flight-time throttle before restoring the parked and drivetrain states.
            if (!engine_.Call(movement, clear_, nullptr, 0) ||
                !engine_.Call(movement, mechanical_set_, &original_mechanical_, 1) ||
                !engine_.Call(movement, parked_set_, &original_parked_, 1)) return false;
        }
        actor_ = {};
        movement_ = {};
        return true;
    }

    bool Active() const { return static_cast<bool>(actor_); }

private:
    bool Clear() const { return engine_.Call(movement_, clear_, nullptr, 0); }

    Engine& engine_;
    Bound movement_get_, clear_, mechanical_set_, parked_get_, parked_set_;
    Ref actor_, movement_;
    uint8_t original_mechanical_{}, original_parked_{};
};
}
