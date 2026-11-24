#pragma once
#include "engine.hpp"
#include "motion.hpp"

namespace vehicle_flight {
class PhysicsControl {
    struct VelocityParameters {
        Vector value{};
        uint8_t add{};
        std::array<uint8_t, 3> padding{};
        std::array<uint32_t, 2> bone{};
    };

public:
    explicit PhysicsControl(Engine& engine) : engine_(engine) {}

    bool Bind() {
        if (velocity_.function && angular_.function && actor_collision_get_.function && actor_collision_set_.function &&
            gravity_get_.function && gravity_set_.function &&
            damping_get_.function && damping_set_.function) return true;
        constexpr auto size = offsetof(VelocityParameters, bone) + sizeof(VelocityParameters::bone);
        velocity_ = engine_.Bind("Engine", "PrimitiveComponent", "SetPhysicsLinearVelocity", 3, size,
            {{"NewVel", 0, sizeof(Vector), "StructProperty"}, {"bAddToCurrent", 24, 1, "BoolProperty"},
             {"BoneName", offsetof(VelocityParameters, bone), 8, "NameProperty"}});
        angular_ = engine_.Bind("Engine", "PrimitiveComponent", "SetPhysicsAngularVelocityInRadians", 3, size,
            {{"NewAngVel", 0, sizeof(Vector), "StructProperty"}, {"bAddToCurrent", 24, 1, "BoolProperty"},
             {"BoneName", offsetof(VelocityParameters, bone), 8, "NameProperty"}});
        actor_collision_get_ = engine_.Bind("Engine", "Actor", "GetActorEnableCollision", 1, 1,
            {{"ReturnValue", 0, 1, "BoolProperty"}});
        actor_collision_set_ = engine_.Bind("Engine", "Actor", "SetActorEnableCollision", 1, 1,
            {{"bNewActorEnableCollision", 0, 1, "BoolProperty"}});
        gravity_get_ = engine_.Bind("Engine", "PrimitiveComponent", "IsGravityEnabled", 1, 1,
            {{"ReturnValue", 0, 1, "BoolProperty"}});
        gravity_set_ = engine_.Bind("Engine", "PrimitiveComponent", "SetEnableGravity", 1, 1,
            {{"bGravityEnabled", 0, 1, "BoolProperty"}});
        damping_get_ = engine_.Bind("Engine", "PrimitiveComponent", "GetAngularDamping", 1, sizeof(float),
            {{"ReturnValue", 0, sizeof(float), "FloatProperty"}});
        damping_set_ = engine_.Bind("Engine", "PrimitiveComponent", "SetAngularDamping", 1, sizeof(float),
            {{"InDamping", 0, sizeof(float), "FloatProperty"}});
        return velocity_.function && angular_.function && actor_collision_get_.function && actor_collision_set_.function &&
            gravity_get_.function && gravity_set_.function &&
            damping_get_.function && damping_set_.function;
    }

    bool Begin(Ref actor, Ref component, bool noclip = true) {
        if (actor == actor_ && component == component_) return SetNoCollision(noclip);
        if (!Restore() || !actor || !component) return false;
        Snapshot saved{};
        // Preserve the component's collision mode and body setup throughout flight.
        if (!engine_.Call(actor, actor_collision_get_, &saved.actor_collision, 1) ||
            !engine_.Call(component, gravity_get_, &saved.gravity, 1) ||
            !engine_.Call(component, damping_get_, &saved.damping, sizeof(saved.damping)) ||
            !std::isfinite(saved.damping)) return false;
        original_ = saved;
        actor_ = actor;
        component_ = component;
        collision_disabled_ = false;
        uint8_t disabled{};
        float damping{100000.0f};
        if (!engine_.Call(component, gravity_set_, &disabled, 1) ||
            !engine_.Call(component, damping_set_, &damping, sizeof(damping)) || !SetNoCollision(noclip)) {
            static_cast<void>(Restore());
            return false;
        }
        return true;
    }

    bool SetNoCollision(bool enabled) {
        if (collision_disabled_ == enabled) return true;
        uint8_t actor_collision = enabled ? 0 : original_.actor_collision;
        const bool actor = engine_.Call(actor_, actor_collision_set_, &actor_collision, 1);
        if (!actor) return false;
        collision_disabled_ = enabled;
        return true;
    }

    bool Restore() {
        if (!Active()) return true;
        bool restored = true;
        if (const auto component = engine_.Resolve(component_.handle)) {
            // Stop residual chassis motion before handing its physics back.
            if (!Brake(component)) return false;
            const bool damping = engine_.Call(component, damping_set_, &original_.damping, sizeof(original_.damping));
            const bool gravity = engine_.Call(component, gravity_set_, &original_.gravity, 1);
            restored = damping && gravity;
        }
        if (!restored) return false;
        if (const auto actor = engine_.Resolve(actor_.handle))
            restored = engine_.Call(actor, actor_collision_set_, &original_.actor_collision, 1) && restored;
        if (restored) {
            actor_ = {};
            component_ = {};
            collision_disabled_ = false;
        }
        return restored;
    }

    bool Active() const { return static_cast<bool>(actor_); }

    bool Brake(Ref component) const {
        if (!component) return false;
        VelocityParameters parameters{};
        const bool linear = engine_.Call(component, velocity_, &parameters, sizeof(parameters));
        return engine_.Call(component, angular_, &parameters, sizeof(parameters)) && linear;
    }

private:
    struct Snapshot {
        uint8_t actor_collision{}, gravity{};
        float damping{};
    } original_;
    Engine& engine_;
    Bound velocity_, angular_;
    Bound actor_collision_get_, actor_collision_set_;
    Bound gravity_get_, gravity_set_, damping_get_, damping_set_;
    Ref actor_, component_;
    bool collision_disabled_{};
};
}
