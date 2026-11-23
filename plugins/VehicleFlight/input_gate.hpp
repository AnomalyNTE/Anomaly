#pragma once
#include "engine.hpp"
#include <atomic>

namespace vehicle_flight {
class InputGate {
    using Getter = bool(*)(void*);

public:
    explicit InputGate(Engine& engine) : engine_(engine) {}

    void Load(const anomaly::sdk::Profile& profile) {
        profile_ = profile;
        getter_slot_ = profile.Value("ControllerIgnoreMoveInputGetterSlot", 65536, sizeof(uintptr_t));
    }

    bool Start(const AnomalyHookServiceV1* hooks, const AnomalyHostApiV1* host = nullptr) {
        if (!engine_.registry && host && !engine_.ResolveProfile(host, profile_))
            return Fail("Vehicle Flight: object registry is not ready");
        hooks_ = hooks;
        if (!hooks_ || hooks_->struct_size < sizeof(*hooks_) || !hooks_->create || !hooks_->release ||
            !hooks_->begin_callback || !hooks_->end_callback) return Fail("Vehicle Flight: hook service is unavailable");
        if (!engine_.names || !engine_.names->resolve_utf8 ||
            !engine_.objects || !engine_.objects->count || !engine_.objects->snapshot_at)
            return Fail("Vehicle Flight: controller enumeration is unavailable");
        Ref controller;
        const auto count = engine_.objects->count(engine_.objects->user);
        for (uint32_t index = 0; index < count; ++index) {
            AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
            if (!anomaly::sdk::Succeeded(engine_.objects->snapshot_at(engine_.objects->user, index, &snapshot)) ||
                engine_.NameById(snapshot.name_id) != "Default__Controller") continue;
            const auto candidate = engine_.Resolve(snapshot.handle);
            if (!engine_.IsA(candidate, "Controller")) continue;
            controller = candidate;
            break;
        }
        if (!controller) return Fail("Vehicle Flight: default controller was not found");
        uintptr_t vtable{};
        if (!engine_.Read(controller.address, vtable) || !engine_.Read(vtable + getter_slot_, target_))
            return Fail("Vehicle Flight: controller input getter is unreadable");
        if (!anomaly::sdk::MatchesProfileEntry(engine_.core, profile_, "ControllerIgnoreMoveInputGetter", target_))
            return Fail("Vehicle Flight: controller input getter does not match the compatibility profile");
        owner_.store(this);
        const AnomalyHookRequestV1 request{sizeof(request), ANOMALY_HOOK_V1_FUNCTION, target_,
            reinterpret_cast<void*>(Ignored), anomaly::sdk::StringView("vehicle-flight-driving-input")};
        uintptr_t original{};
        const auto result = hooks_->create(hooks_->user, &request, &original, &handle_);
        if (!anomaly::sdk::Succeeded(result) || !handle_.id || !original) {
            owner_.store(nullptr);
            return Fail("Vehicle Flight: controller input hook could not be installed");
        }
        original_.store(reinterpret_cast<Getter>(original));
        return true;
    }

    bool Hold(Ref controller) {
        if (!handle_.id || !controller) return false;
        if (controller_.load() == controller.address) return true;
        uintptr_t vtable{}, getter{};
        if (!engine_.Read(controller.address, vtable) || !engine_.Read(vtable + getter_slot_, getter) ||
            getter != target_) return false;
        controller_.store(controller.address);
        return true;
    }

    void Release() { controller_.store(0); }

    AnomalyStatusV1 Stop() noexcept {
        Release();
        if (handle_.id) {
            const auto result = hooks_->release(hooks_->user, handle_);
            if (!anomaly::sdk::Succeeded(result) && result.code != ANOMALY_STATUS_V1_NOT_FOUND) return result;
            handle_ = {};
        }
        owner_.store(nullptr);
        return anomaly::sdk::Ok();
    }

private:
    bool Fail(std::string_view reason) const {
        if (engine_.core && engine_.core->log)
            engine_.core->log(engine_.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(reason));
        return false;
    }

    static bool Ignored(void* controller) noexcept {
        auto* self = owner_.load();
        const auto original = original_.load();
        if (!self || !original || self->controller_.load() != reinterpret_cast<uintptr_t>(controller))
            return original ? original(controller) : false;
        AnomalyGenerationHandleV1 lease{};
        if (!anomaly::sdk::Succeeded(self->hooks_->begin_callback(self->hooks_->user, self->handle_, &lease)))
            return original(controller);
        const bool ignored = self->controller_.load() == reinterpret_cast<uintptr_t>(controller) || original(controller);
        static_cast<void>(self->hooks_->end_callback(self->hooks_->user, lease));
        return ignored;
    }

    Engine& engine_;
    anomaly::sdk::Profile profile_;
    uint32_t getter_slot_{};
    uintptr_t target_{};
    const AnomalyHookServiceV1* hooks_{};
    AnomalyGenerationHandleV1 handle_{};
    std::atomic<uintptr_t> controller_{};
    inline static std::atomic<InputGate*> owner_{};
    inline static std::atomic<Getter> original_{};
};
}
