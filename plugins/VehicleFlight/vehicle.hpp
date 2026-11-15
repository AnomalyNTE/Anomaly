#pragma once
#include "engine.hpp"
#include "motion.hpp"

namespace vehicle_flight {
class VehicleAccess {
    struct alignas(8) MoveParameters {
        Vector location;
        uint8_t sweep{1};
        std::array<uint8_t, 7> alignment{};
        std::array<uint8_t, 264> hit{};
        uint8_t teleport{1};
        uint8_t result{};
    };
    static_assert(offsetof(MoveParameters, result) == 297);

    struct alignas(8) SteerParameters {
        Vector location;
        Vector rotation;
        uint8_t sweep{};
        std::array<uint8_t, 7> alignment{};
        std::array<uint8_t, 264> hit{};
        uint8_t teleport{1};
        uint8_t result{};
    };
    static_assert(offsetof(SteerParameters, result) == 321);

    struct VelocityParameters {
        Vector velocity{};
        uint8_t add{};
        std::array<uint8_t, 3> alignment{};
        std::array<uint32_t, 2> bone{};
    };

public:
    Engine engine;

    bool Bind() {
        if (move_.function) return true;
        const auto location = engine.Bind("Engine", "Actor", "K2_GetActorLocation", 1, sizeof(Vector),
            {{"ReturnValue", 0, sizeof(Vector), "StructProperty"}});
        const auto driver = engine.Bind("HTGame", "HTPlayerController", "BP_GetCurrentDriveVehicle", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        const auto move = engine.Bind("Engine", "Actor", "K2_SetActorLocation", 5, offsetof(MoveParameters, result) + 1,
            {{"NewLocation", 0, sizeof(Vector), "StructProperty"},
             {"bSweep", offsetof(MoveParameters, sweep), 1, "BoolProperty"},
             {"SweepHitResult", offsetof(MoveParameters, hit), sizeof(MoveParameters::hit), "StructProperty"},
             {"bTeleport", offsetof(MoveParameters, teleport), 1, "BoolProperty"},
             {"ReturnValue", offsetof(MoveParameters, result), 1, "BoolProperty"}});
        if (!location.function || !driver.function || !move.function) return false;
        location_ = location;
        driver_ = driver;
        move_ = move;
        rotation_ = engine.Bind("Engine", "Actor", "K2_GetActorRotation", 1, sizeof(Vector),
            {{"ReturnValue", 0, sizeof(Vector), "StructProperty"}});
        steer_ = engine.Bind("Engine", "Actor", "K2_SetActorLocationAndRotation", 6, offsetof(SteerParameters, result) + 1,
            {{"NewLocation", 0, sizeof(Vector), "StructProperty"},
             {"NewRotation", offsetof(SteerParameters, rotation), sizeof(Vector), "StructProperty"},
             {"bSweep", offsetof(SteerParameters, sweep), 1, "BoolProperty"},
             {"SweepHitResult", offsetof(SteerParameters, hit), sizeof(SteerParameters::hit), "StructProperty"},
             {"bTeleport", offsetof(SteerParameters, teleport), 1, "BoolProperty"},
             {"ReturnValue", offsetof(SteerParameters, result), 1, "BoolProperty"}});
        water_ = engine.Bind("HTGame", "HTPlayerCharacter", "GetBoardWaterVehicle", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        water_driver_ = engine.Bind("HTGame", "HTVirtualWaterVehicleBasePawn", "GetDriver", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        root_ = engine.Bind("Engine", "Actor", "K2_GetRootComponent", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        velocity_ = engine.Bind("Engine", "PrimitiveComponent", "SetPhysicsLinearVelocity", 3,
            offsetof(VelocityParameters, bone) + sizeof(VelocityParameters::bone),
            {{"NewVel", 0, sizeof(Vector), "StructProperty"},
             {"bAddToCurrent", offsetof(VelocityParameters, add), 1, "BoolProperty"},
             {"BoneName", offsetof(VelocityParameters, bone), sizeof(VelocityParameters::bone), "NameProperty"}});
        return true;
    }

    Ref DrivenVehicle(AnomalyGenerationHandleV1 controller, AnomalyGenerationHandleV1 pawn, Ref current) {
        const auto local_controller = engine.Resolve(controller);
        if (!local_controller) return {};
        const auto address = ReturnedObject(local_controller, driver_);
        if (address) {
            const auto vehicle = engine.Reference(address);
            if (vehicle && (vehicle == current || engine.IsA(vehicle, "HTWheeledVehicleBase"))) return vehicle;
        }
        if (!water_.function || !water_driver_.function) return {};
        const auto rider = engine.Resolve(pawn);
        if (!engine.IsA(rider, "HTPlayerCharacter")) return {};
        const auto boat = engine.Reference(ReturnedObject(rider, water_));
        if (!boat || (!(boat == current) && !engine.IsA(boat, "HTVirtualWaterVehicleBasePawn"))) return {};
        return ReturnedObject(boat, water_driver_) == rider.address ? boat : Ref{};
    }

    bool Location(Ref vehicle, Vector& position) const {
        return engine.Call(vehicle, location_, &position, sizeof(position)) && Finite(position);
    }

    bool Rotation(Ref vehicle, Vector& rotation) const {
        return steer_.function && engine.Call(vehicle, rotation_, &rotation, sizeof(rotation)) && Finite(rotation);
    }

    Ref PhysicsRoot(Ref vehicle) {
        if (!velocity_.function) return {};
        const auto root = engine.Reference(ReturnedObject(vehicle, root_));
        return engine.IsA(root, "PrimitiveComponent") ? root : Ref{};
    }

    bool Move(Ref vehicle, Ref physics, const Vector& position, const Vector* rotation, bool sweep, bool& moved) const {
        if (rotation) {
            SteerParameters params{position, *rotation, static_cast<uint8_t>(sweep)};
            if (!engine.Call(vehicle, steer_, &params, steer_.size)) return false;
            moved = params.result != 0;
        } else {
            MoveParameters params{position, static_cast<uint8_t>(sweep)};
            if (!engine.Call(vehicle, move_, &params, move_.size)) return false;
            moved = params.result != 0;
        }
        Brake(physics);
        return true;
    }

    void Brake(Ref physics) const {
        if (!physics) return;
        VelocityParameters velocity{};
        static_cast<void>(engine.Call(physics, velocity_, &velocity, velocity_.size));
    }

private:
    uintptr_t ReturnedObject(Ref object, const Bound& function) const {
        uintptr_t address{};
        return engine.Call(object, function, &address, sizeof(address)) ? address : 0;
    }

    Bound driver_, water_, water_driver_, location_, move_, rotation_, steer_, root_, velocity_;
};
}
