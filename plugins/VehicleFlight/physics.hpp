#pragma once
#include "engine.hpp"
#include "motion.hpp"
#include <vector>

namespace vehicle_flight {
class PhysicsControl {
    struct VelocityParameters {
        Vector value{};
        uint8_t add{};
        std::array<uint8_t, 3> padding{};
        std::array<uint32_t, 2> bone{};
    };
    struct ChildParameters {
        int32_t index{};
        uint32_t padding{};
        uintptr_t result{};
    };
    struct Snapshot {
        Ref component;
        uint8_t gravity{};
        float damping{};
    };

public:
    explicit PhysicsControl(Engine& engine) : engine_(engine) {}

    bool Bind() {
        if (velocity_.function && angular_.function && actor_collision_get_.function && actor_collision_set_.function &&
            gravity_get_.function && gravity_set_.function &&
            damping_get_.function && damping_set_.function && owner_.function &&
            child_count_.function && child_.function && collision_get_.function && collision_set_.function) return true;
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
        collision_get_ = engine_.Bind("Engine", "PrimitiveComponent", "GetCollisionEnabled", 1, 1,
            {{"ReturnValue", 0, 1, "EnumProperty"}});
        collision_set_ = engine_.Bind("Engine", "PrimitiveComponent", "SetCollisionEnabled", 1, 1,
            {{"NewType", 0, 1, "EnumProperty"}});
        gravity_get_ = engine_.Bind("Engine", "PrimitiveComponent", "IsGravityEnabled", 1, 1,
            {{"ReturnValue", 0, 1, "BoolProperty"}});
        gravity_set_ = engine_.Bind("Engine", "PrimitiveComponent", "SetEnableGravity", 1, 1,
            {{"bGravityEnabled", 0, 1, "BoolProperty"}});
        damping_get_ = engine_.Bind("Engine", "PrimitiveComponent", "GetAngularDamping", 1, sizeof(float),
            {{"ReturnValue", 0, sizeof(float), "FloatProperty"}});
        damping_set_ = engine_.Bind("Engine", "PrimitiveComponent", "SetAngularDamping", 1, sizeof(float),
            {{"InDamping", 0, sizeof(float), "FloatProperty"}});
        owner_ = engine_.Bind("Engine", "ActorComponent", "GetOwner", 1, sizeof(uintptr_t),
            {{"ReturnValue", 0, sizeof(uintptr_t), "ObjectProperty"}});
        child_count_ = engine_.Bind("Engine", "SceneComponent", "GetNumChildrenComponents", 1, sizeof(int32_t),
            {{"ReturnValue", 0, sizeof(int32_t), "IntProperty"}});
        child_ = engine_.Bind("Engine", "SceneComponent", "GetChildComponent", 2, sizeof(ChildParameters),
            {{"ChildIndex", 0, sizeof(int32_t), "IntProperty"},
             {"ReturnValue", offsetof(ChildParameters, result), sizeof(uintptr_t), "ObjectProperty"}});
        return velocity_.function && angular_.function && actor_collision_get_.function && actor_collision_set_.function &&
            gravity_get_.function && gravity_set_.function &&
            damping_get_.function && damping_set_.function && owner_.function &&
            child_count_.function && child_.function && collision_get_.function && collision_set_.function;
    }

    bool Begin(Ref actor, Ref component, bool noclip = true) {
        if (actor == actor_ && component == component_) return SetNoCollision(noclip);
        if (!Restore() || !actor || !component) return false;
        uint8_t collision{};
        std::vector<Snapshot> saved;
        if (!engine_.Call(actor, actor_collision_get_, &collision, 1) ||
            !Capture(actor, component, saved)) return false;
        uint8_t root_collision{};
        // The getter can report NoCollision when the actor already disables it.
        // In that case leave the component's own mode untouched.
        if (collision && !engine_.Call(component, collision_get_, &root_collision, 1)) return false;
        original_collision_ = collision;
        original_root_collision_ = root_collision;
        original_ = std::move(saved);
        actor_ = actor;
        component_ = component;
        collision_disabled_ = false;
        root_collision_overridden_ = false;
        uint8_t disabled{};
        float damping{100000.0f};
        for (const auto& state : original_) {
            if (!engine_.Call(state.component, gravity_set_, &disabled, 1) ||
                !engine_.Call(state.component, damping_set_, &damping, sizeof(damping))) {
                static_cast<void>(Restore());
                return false;
            }
        }
        if (!SetNoCollision(noclip)) {
            static_cast<void>(Restore());
            return false;
        }
        return true;
    }

    bool SetNoCollision(bool enabled) {
        if (collision_disabled_ == enabled) return true;
        uint8_t actor_collision = enabled ? 0 : original_collision_;
        if (enabled) {
            if (!engine_.Call(actor_, actor_collision_set_, &actor_collision, 1)) return false;
            if (original_collision_ && original_root_collision_) {
                uint8_t disabled{};
                root_collision_overridden_ = true;
                if (!engine_.Call(component_, collision_set_, &disabled, 1)) return false;
            }
        } else {
            if (!RestoreRootCollision() ||
                !engine_.Call(actor_, actor_collision_set_, &actor_collision, 1)) return false;
        }
        collision_disabled_ = enabled;
        return true;
    }

    bool Restore() {
        if (!Active()) return true;
        bool restored = true;
        for (auto& state : original_) {
            if (const auto component = engine_.Resolve(state.component.handle)) {
                if (!Brake(component)) { restored = false; continue; }
                const bool damping = engine_.Call(component, damping_set_, &state.damping, sizeof(state.damping));
                const bool gravity = engine_.Call(component, gravity_set_, &state.gravity, 1);
                restored = damping && gravity && restored;
            }
        }
        if (!restored) return false;
        if (rebuild_on_restore_ && original_collision_ && original_root_collision_ && !root_collision_overridden_) {
            if (const auto component = engine_.Resolve(component_.handle)) {
                uint8_t disabled{};
                if (!engine_.Call(component, collision_set_, &disabled, 1)) return false;
                root_collision_overridden_ = true;
            }
        }
        if (!RestoreRootCollision()) return false;
        if (const auto actor = engine_.Resolve(actor_.handle))
            restored = engine_.Call(actor, actor_collision_set_, &original_collision_, 1) && restored;
        if (restored) {
            actor_ = {};
            component_ = {};
            collision_disabled_ = false;
            original_.clear();
            rebuild_on_restore_ = false;
        }
        return restored;
    }

    bool Active() const { return static_cast<bool>(actor_); }
    void RebuildOnRestore() { if (Active()) rebuild_on_restore_ = true; }

    bool Brake(Ref component) const {
        if (!component) return false;
        VelocityParameters parameters{};
        const bool linear = engine_.Call(component, velocity_, &parameters, sizeof(parameters));
        return engine_.Call(component, angular_, &parameters, sizeof(parameters)) && linear;
    }

private:
    bool RestoreRootCollision() {
        if (!root_collision_overridden_) return true;
        if (const auto component = engine_.Resolve(component_.handle)) {
            if (!engine_.Call(component, collision_set_, &original_root_collision_, 1)) return false;
        }
        root_collision_overridden_ = false;
        return true;
    }

    bool Capture(Ref actor, Ref root, std::vector<Snapshot>& saved) {
        // Walk once on entry; an attached passenger belongs to a different actor.
        std::vector<Ref> pending{root};
        for (size_t index = 0; index < pending.size(); ++index) {
            const auto component = pending[index];
            uintptr_t owner{};
            if (!engine_.Call(component, owner_, &owner, sizeof(owner))) return false;
            if (owner != actor.address) continue;
            if (engine_.IsA(component, "PrimitiveComponent")) {
                Snapshot state{component};
                if (!engine_.Call(component, gravity_get_, &state.gravity, 1) ||
                    !engine_.Call(component, damping_get_, &state.damping, sizeof(state.damping)) ||
                    !std::isfinite(state.damping)) return false;
                saved.push_back(state);
            }
            int32_t count{};
            if (!engine_.Call(component, child_count_, &count, sizeof(count)) || count < 0 || count > 256) return false;
            for (int32_t child_index = 0; child_index < count; ++child_index) {
                ChildParameters parameters{child_index};
                if (!engine_.Call(component, child_, &parameters, sizeof(parameters))) return false;
                const auto child = engine_.Reference(parameters.result);
                if (!child) return false;
                if (std::find(pending.begin(), pending.end(), child) != pending.end()) continue;
                if (pending.size() >= 256) return false;
                pending.push_back(child);
            }
        }
        return !saved.empty();
    }
    std::vector<Snapshot> original_;
    uint8_t original_collision_{}, original_root_collision_{};
    Engine& engine_;
    Bound velocity_, angular_;
    Bound actor_collision_get_, actor_collision_set_;
    Bound collision_get_, collision_set_;
    Bound gravity_get_, gravity_set_, damping_get_, damping_set_;
    Bound owner_, child_count_, child_;
    Ref actor_, component_;
    bool collision_disabled_{}, root_collision_overridden_{}, rebuild_on_restore_{};
};
}
