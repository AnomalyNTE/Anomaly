#pragma once
#include "engine.hpp"
#include "motion.hpp"
#include "physics.hpp"
#include "drivetrain.hpp"

namespace vehicle_flight {
class VehicleAccess {
    struct TeleportParameters {
        Vector location;
        Vector rotation;
        uint8_t result{};
    };
    static_assert(offsetof(TeleportParameters, result) == 48);
    struct ComponentParameters {
        uintptr_t type{}, result{};
    };
    struct FacingParameters {
        Vector rotation;
        uint8_t teleport{1}, result{};
    };

public:
    VehicleAccess() : physics_(engine), drivetrain_(engine) {}
    Engine engine;

    bool Bind() {
        if (location_.function && driver_.function && rotation_.function && teleport_.function && facing_.function && root_.function)
            return physics_.Bind() && drivetrain_.Bind();
        const auto location = engine.Bind("Engine", "Actor", "K2_GetActorLocation", 1, sizeof(Vector),
            {{"ReturnValue", 0, sizeof(Vector), "StructProperty"}});
        const auto driver = engine.Bind("HTGame", "HTPlayerController", "BP_GetCurrentDriveVehicle", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        if (!location.function || !driver.function) return false;
        location_ = location;
        driver_ = driver;
        rotation_ = engine.Bind("Engine", "Actor", "K2_GetActorRotation", 1, sizeof(Vector),
            {{"ReturnValue", 0, sizeof(Vector), "StructProperty"}});
        facing_ = engine.Bind("Engine", "Actor", "K2_SetActorRotation", 3, offsetof(FacingParameters, result) + 1,
            {{"NewRotation", 0, sizeof(Vector), "StructProperty"},
             {"bTeleportPhysics", offsetof(FacingParameters, teleport), 1, "BoolProperty"},
             {"ReturnValue", offsetof(FacingParameters, result), 1, "BoolProperty"}});
        teleport_ = engine.Bind("Engine", "Actor", "K2_TeleportTo", 3, offsetof(TeleportParameters, result) + 1,
            {{"DestLocation", 0, sizeof(Vector), "StructProperty"},
             {"DestRotation", offsetof(TeleportParameters, rotation), sizeof(Vector), "StructProperty"},
             {"ReturnValue", offsetof(TeleportParameters, result), 1, "BoolProperty"}});
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
        camera_manager_ = engine.Bind("HTGame", "HTPlayerController", "GetHTPlayerCameraManager", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        camera_rotation_ = engine.Bind("Engine", "PlayerCameraManager", "GetCameraRotation", 1, sizeof(Vector),
            {{"ReturnValue", 0, sizeof(Vector), "StructProperty"}});
        return rotation_.function && teleport_.function && facing_.function &&
            root_.function && physics_.Bind() && drivetrain_.Bind();
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
        return engine.Call(vehicle, rotation_, &rotation, sizeof(rotation)) && Finite(rotation);
    }

    bool ViewRotation(Ref controller, Vector& rotation) const {
        if (!camera_manager_.function || !camera_rotation_.function) return false;
        const auto manager = engine.Reference(ReturnedObject(controller, camera_manager_));
        return engine.Call(manager, camera_rotation_, &rotation, sizeof(rotation)) && Finite(rotation);
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

    bool SuspendDriving(Ref vehicle) {
        return drivetrain_.Hold(vehicle);
    }

    bool Move(Ref vehicle, Ref root, const Vector& position, const Vector* rotation, bool collision, bool& moved,
              const Vector* observed = nullptr) {
        moved = false;
        Vector current{}, original{};
        if (observed) current = *observed;
        else if (!Location(vehicle, current)) return false;
        if (!Rotation(vehicle, original)) return false;
        const Vector upright{0.0, rotation ? (*rotation)[1] : original[1], 0.0};
        const bool translate = Length(Difference(position, current)) > 0.02;
        const bool turn = std::abs(original[0]) > 0.02 || std::abs(original[2]) > 0.02 ||
            std::abs(std::remainder(upright[1] - original[1], 360.0)) > 0.02;
        if (turn) {
            FacingParameters facing{upright};
            if (!engine.Call(vehicle, facing_, &facing, facing_.size)) return false;
        }
        moved = true;
        if (translate) {
            // Match the observed implementation: native teleport performs destination
            // collision checks when collision is enabled on the vehicle.
            TeleportParameters parameters{position, upright};
            if (!engine.Call(vehicle, teleport_, &parameters, teleport_.size)) return false;
            moved = parameters.result != 0;
            if (!collision && !moved) {
                Vector actual{};
                if (!Location(vehicle, actual)) return false;
                moved = Length(Difference(position, actual)) <= 0.02;
            }
        }
        return physics_.Brake(root);
    }

    bool BeginFlightPhysics(Ref vehicle, Ref root, bool noclip = true) { return physics_.Begin(vehicle, root, noclip); }
    bool RestoreFlightPhysics() {
        if (drivetrain_.Active()) physics_.RebuildOnRestore();
        // Physics creation copies the drivetrain configuration. Restore it before
        // enabling chassis collision, including flight that already has collision.
        return drivetrain_.Restore() && physics_.Restore();
    }
    bool HasFlightPhysics() const { return physics_.Active() || drivetrain_.Active(); }


private:
    uintptr_t ReturnedObject(Ref object, const Bound& function) const {
        uintptr_t address{};
        return engine.Call(object, function, &address, sizeof(address)) ? address : 0;
    }

    Bound driver_, water_, water_driver_, location_, rotation_, teleport_, facing_, root_;
    Bound component_;
    Bound camera_manager_, camera_rotation_;
    Ref mesh_class_;
    PhysicsControl physics_;
    DrivetrainControl drivetrain_;
};
}
