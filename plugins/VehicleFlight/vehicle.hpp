#pragma once
#include "engine.hpp"
#include "motion.hpp"
#include "physics.hpp"

namespace vehicle_flight {
class VehicleAccess {
    struct alignas(8) LocationParameters {
        Vector location;
        uint8_t sweep{};
        std::array<uint8_t, 7> alignment{};
        std::array<uint8_t, 264> hit{};
        uint8_t teleport{1}, result{};
    };
    static_assert(offsetof(LocationParameters, result) == 297);
    struct ComponentParameters {
        uintptr_t type{}, result{};
    };

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

public:
    VehicleAccess() : physics_(engine) {}
    Engine engine;

    bool Bind() {
        if (location_.function && driver_.function && rotation_.function && steer_.function && root_.function)
            return physics_.Bind();
        const auto location = engine.Bind("Engine", "Actor", "K2_GetActorLocation", 1, sizeof(Vector),
            {{"ReturnValue", 0, sizeof(Vector), "StructProperty"}});
        const auto driver = engine.Bind("HTGame", "HTPlayerController", "BP_GetCurrentDriveVehicle", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        if (!location.function || !driver.function) return false;
        location_ = location;
        driver_ = driver;
        rotation_ = engine.Bind("Engine", "Actor", "K2_GetActorRotation", 1, sizeof(Vector),
            {{"ReturnValue", 0, sizeof(Vector), "StructProperty"}});
        steer_ = engine.Bind("Engine", "Actor", "K2_SetActorLocationAndRotation", 6, offsetof(SteerParameters, result) + 1,
            {{"NewLocation", 0, sizeof(Vector), "StructProperty"},
             {"NewRotation", offsetof(SteerParameters, rotation), sizeof(Vector), "StructProperty"},
             {"bSweep", offsetof(SteerParameters, sweep), 1, "BoolProperty"},
             {"SweepHitResult", offsetof(SteerParameters, hit), sizeof(SteerParameters::hit), "StructProperty"},
             {"bTeleport", offsetof(SteerParameters, teleport), 1, "BoolProperty"},
             {"ReturnValue", offsetof(SteerParameters, result), 1, "BoolProperty"}});
        position_ = engine.Bind("Engine", "Actor", "K2_SetActorLocation", 5, offsetof(LocationParameters, result) + 1,
            {{"NewLocation", 0, sizeof(Vector), "StructProperty"},
             {"bSweep", offsetof(LocationParameters, sweep), 1, "BoolProperty"},
             {"SweepHitResult", offsetof(LocationParameters, hit), sizeof(LocationParameters::hit), "StructProperty"},
             {"bTeleport", offsetof(LocationParameters, teleport), 1, "BoolProperty"},
             {"ReturnValue", offsetof(LocationParameters, result), 1, "BoolProperty"}});
        const auto hit = engine.Find("/Script/Engine.HitResult");
        if (engine.IsA(hit, "ScriptStruct")) {
            const int offset = engine.Field(hit.address, "Normal", sizeof(Vector), "StructProperty");
            if (offset >= 0 && offset + sizeof(Vector) <= sizeof(SteerParameters::hit)) normal_offset_ = offset;
        }
        water_ = engine.Bind("HTGame", "HTPlayerCharacter", "GetBoardWaterVehicle", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        water_driver_ = engine.Bind("HTGame", "HTVirtualWaterVehicleBasePawn", "GetDriver", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        root_ = engine.Bind("Engine", "Actor", "K2_GetRootComponent", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        component_ = engine.Bind("Engine", "Actor", "GetComponentByClass", 2, sizeof(ComponentParameters),
            {{"ComponentClass", 0, sizeof(uintptr_t), "ClassProperty"},
             {"ReturnValue", offsetof(ComponentParameters, result), sizeof(uintptr_t), "ObjectProperty"}});
        mesh_class_ = engine.Find("/Script/Engine.SkeletalMeshComponent");
        movement_ = engine.Bind("Engine", "Pawn", "GetMovementComponent", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        clear_inputs_ = engine.Bind("HTGame", "HTVehicleMovementComponent", "ClearAllInputs", 0, 0, {});
        return rotation_.function && steer_.function && root_.function && physics_.Bind();
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
        const auto root = engine.Reference(ReturnedObject(vehicle, root_));
        if (engine.IsA(root, "PrimitiveComponent")) return root;
        // A generic first PrimitiveComponent can be a wheel or decoration.
        // Only use a skeletal chassis when the actor root is non-physical.
        if (!component_.function || !mesh_class_) return {};
        ComponentParameters parameters{mesh_class_.address};
        if (!engine.Call(vehicle, component_, &parameters, sizeof(parameters))) return {};
        const auto component = engine.Reference(parameters.result);
        return engine.IsA(component, "SkeletalMeshComponent") ? component : Ref{};
    }

    bool ClearDrivingInput(Ref vehicle) {
        if (!movement_.function || !clear_inputs_.function) return true;
        if (!(vehicle == input_vehicle_)) {
            input_vehicle_ = vehicle;
            movement_component_ = engine.Reference(ReturnedObject(vehicle, movement_));
            if (!engine.IsA(movement_component_, "HTVehicleMovementComponent")) movement_component_ = {};
        }
        return !movement_component_ || engine.Call(movement_component_, clear_inputs_, nullptr, 0);
    }

    bool Move(Ref vehicle, Ref root, const Vector& position, const Vector* rotation, bool sweep, bool& moved,
              const Vector* observed = nullptr) {
        moved = false;
        Vector current{}, original{};
        if (observed) current = *observed;
        else if (!Location(vehicle, current)) return false;
        if (!Rotation(vehicle, original)) return false;
        const double yaw = rotation ? (*rotation)[1] : original[1];
        const Vector upright{0.0, yaw, 0.0};
        if (Length(Difference(position, current)) > 0.02 ||
            std::abs(original[0]) > 0.02 || std::abs(original[2]) > 0.02 ||
            std::abs(std::remainder(upright[1] - original[1], 360.0)) > 0.02) {
            SteerParameters params{position, upright, static_cast<uint8_t>(sweep)};
            if (!engine.Call(vehicle, steer_, &params, steer_.size)) return false;
            moved = params.result != 0;
            if (!sweep && !moved) {
                Vector actual{};
                if (!Location(vehicle, actual)) return false;
                if (Length(Difference(position, actual)) > 0.02 && position_.function) {
                    LocationParameters retry{position};
                    if (!engine.Call(vehicle, position_, &retry, position_.size) || !Location(vehicle, actual)) return false;
                }
                // A false pose return need not mean the translation failed.
                moved = Length(Difference(position, actual)) <= 0.02;
            } else if (sweep && normal_offset_ >= 0) {
                Vector normal{};
                std::memcpy(normal.data(), params.hit.data() + normal_offset_, sizeof(normal));
                const double length = Length(normal);
                if (Finite(normal) && length > 0.5 && length < 1.5) {
                    Vector actual{};
                    if (!Location(vehicle, actual)) return false;
                    for (auto& axis : normal) axis /= length;
                    const auto remaining = Difference(position, actual);
                    const double into = remaining[0]*normal[0] + remaining[1]*normal[1] + remaining[2]*normal[2];
                    Vector slide{};
                    for (size_t axis=0; axis<slide.size(); ++axis) slide[axis] = remaining[axis] - into*normal[axis];
                    if (into < 0.0 && Length(slide) > 0.02) {
                        for (size_t axis=0; axis<slide.size(); ++axis) slide[axis] += actual[axis];
                        SteerParameters tangent{slide, upright, 1};
                        if (!engine.Call(vehicle, steer_, &tangent, steer_.size)) return false;
                        moved = moved || tangent.result != 0;
                    }
                }
            }
        } else moved = true;
        return physics_.Brake(root);
    }

    void ResetPhysics() {
        input_vehicle_ = {};
        movement_component_ = {};
    }

    bool BeginFlightPhysics(Ref vehicle, Ref root, bool noclip = true) { return physics_.Begin(vehicle, root, noclip); }
    bool RestoreFlightPhysics() { return physics_.Restore(); }
    bool HasFlightPhysics() const { return physics_.Active(); }


private:
    uintptr_t ReturnedObject(Ref object, const Bound& function) const {
        uintptr_t address{};
        return engine.Call(object, function, &address, sizeof(address)) ? address : 0;
    }

    Bound driver_, water_, water_driver_, location_, rotation_, steer_, root_;
    Bound position_;
    int normal_offset_{-1};
    Bound movement_, clear_inputs_, component_;
    Ref input_vehicle_, movement_component_, mesh_class_;
    PhysicsControl physics_;
};
}
