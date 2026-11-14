#pragma once
#include "map_teleport.hpp"
#include <intrin.h>

namespace collectible_map {
class MapPinGuard {
    using CanCreatePin = bool(ANOMALY_CALL*)(uintptr_t, uintptr_t, uintptr_t);

public:
    void Resolve(const anomaly::sdk::Host& host, Engine& engine, const anomaly::sdk::Profile& profile) {
        const auto hook = host.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID);
        if (!hook || hook->struct_size < sizeof(AnomalyHookServiceV1) || !hook->create ||
            !hook->release || !hook->begin_callback || !hook->end_callback) return;
        const auto signatures = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        const auto site = anomaly::sdk::ResolveProfileSymbol(signatures.get(), engine.core, profile, "MapPinCreateCall");
        const auto callee = anomaly::sdk::ResolveProfileSymbol(signatures.get(), engine.core, profile, "MapPinCreateTarget");
        if (!site || !callee || !callee.candidate->call_offset) return;
        const auto call_offset = *callee.candidate->call_offset;
        const auto tag_offset = profile.Value("PinDialogTagOffset", 0x1000);
        const auto call = site.address + call_offset;
        const auto load_tag = call + tag_offset;
        std::array<uint8_t, 7> tag_load{};
        if (!engine.Read(load_tag, tag_load) || tag_load[0] != 0x48 ||
            tag_load[1] != 0x8d || tag_load[2] != 0x15) return;
        int32_t tag_relative{};
        std::memcpy(&tag_relative, tag_load.data() + 3, sizeof(tag_relative));
        // The next branch in this caller constructs only the MiniMapMarkIcon dialog.
        constexpr char tag[] = "MiniMapMarkIcon";
        std::array<char, sizeof(tag)> actual_tag{};
        if (!engine.Read(load_tag + tag_load.size() + tag_relative, actual_tag) ||
            std::memcmp(tag, actual_tag.data(), actual_tag.size()) != 0) return;
        hook_ = hook.get();
        return_address_ = call + 5; // E8 rel32 instruction length.
        target_ = callee.address;
    }

    bool Start(const std::atomic<bool>& enabled) noexcept {
        if (!hook_ || !target_) return false;
        enabled_ = &enabled;
        AnomalyHookRequestV1 request{sizeof(request), ANOMALY_HOOK_V1_FUNCTION, target_,
            reinterpret_cast<void*>(&Detour), anomaly::sdk::StringView("collectible-map-pin-dialog")};
        current_.store(this, std::memory_order_release);
        if (anomaly::sdk::Succeeded(hook_->create(hook_->user, &request, &original_, &handle_)) &&
            handle_.id && original_) {
            armed_.store(true, std::memory_order_release);
            return true;
        }
        current_.store(nullptr, std::memory_order_release);
        return false;
    }

    AnomalyStatusV1 Stop() noexcept {
        if (!armed_.load(std::memory_order_acquire)) return anomaly::sdk::Ok();
        const auto result = hook_->release(hook_->user, handle_);
        if (!anomaly::sdk::Succeeded(result) && result.code != ANOMALY_STATUS_V1_NOT_FOUND) return result;
        armed_.store(false, std::memory_order_release);
        current_.store(nullptr, std::memory_order_release);
        return anomaly::sdk::Ok();
    }

private:
    static bool ANOMALY_CALL Detour(uintptr_t object, uintptr_t level, uintptr_t gameplay_area) noexcept {
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const auto self = current_.load(std::memory_order_acquire);
        if (!self || !self->armed_.load(std::memory_order_acquire)) return false;
        const auto* hook = self->hook_;
        AnomalyGenerationHandleV1 lease{};
        if (!anomaly::sdk::Succeeded(hook->begin_callback(hook->user, self->handle_, &lease)) || !lease.id)
            return false;
        const bool allowed = !(caller == self->return_address_ && self->enabled_->load()) &&
            reinterpret_cast<CanCreatePin>(self->original_)(object, level, gameplay_area);
        static_cast<void>(hook->end_callback(hook->user, lease));
        return allowed;
    }

    static inline std::atomic<MapPinGuard*> current_{};
    const AnomalyHookServiceV1* hook_{};
    const std::atomic<bool>* enabled_{};
    std::atomic<bool> armed_{};
    uintptr_t target_{}, return_address_{}, original_{};
    AnomalyGenerationHandleV1 handle_{};
};
}
