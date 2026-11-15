#include "durability_guard.hpp"
#include "anomaly/sdk/services/interop.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nte_vehicle {
namespace {

// 原始签名取自 dump：载具受伤入口返回 float 伤害值（0 = 无伤害），撞击处理器返回 void。
using TakeDamageFn = float(ANOMALY_CALL*)(void* self, float damage, void* a3, void* a4, void* a5);
using ImpactFn = void(ANOMALY_CALL*)(void* self, int a2, void* a3, void* a4, int a5, void* a6);

// detour 只碰原子量：不调宿主服务、不打日志、不分配内存。
std::atomic_bool g_enabled{};
std::atomic_bool g_installed{};
std::atomic<std::uintptr_t> g_take_damage_original{};
std::atomic<std::uintptr_t> g_impact_original{};
std::atomic<std::uint64_t> g_take_damage_handle_id{};
std::atomic<std::uint64_t> g_take_damage_handle_generation{};
std::atomic<std::uint64_t> g_impact_handle_id{};
std::atomic<std::uint64_t> g_impact_handle_generation{};
const AnomalyHookServiceV1* g_hook{};

bool AcquireLease(const std::atomic<std::uint64_t>& handle_id,
                  const std::atomic<std::uint64_t>& handle_generation,
                  AnomalyGenerationHandleV1& lease) noexcept {
    if (g_hook == nullptr || g_hook->begin_callback == nullptr) return false;
    const std::uint64_t id = handle_id.load(std::memory_order_acquire);
    if (id == 0) return false;
    AnomalyGenerationHandleV1 handle{};
    handle.id = id;
    handle.generation = handle_generation.load(std::memory_order_acquire);
    __try {
        return g_hook->begin_callback(g_hook->user, handle, &lease).code == ANOMALY_STATUS_V1_OK;
    } __except (1) {
        return false;
    }
}

void ReleaseLease(AnomalyGenerationHandleV1& lease) noexcept {  // NOLINT(readability-non-const-parameter)
    if (g_hook == nullptr || g_hook->end_callback == nullptr) return;
    __try {
        static_cast<void>(g_hook->end_callback(g_hook->user, lease));
    } __except (1) {
    }
}

float ANOMALY_CALL TakeDamageDetour(void* self, float damage, void* a3, void* a4,
                                    void* a5) noexcept {
    if (g_enabled.load(std::memory_order_relaxed)) return 0.0F;
    const auto original =
        reinterpret_cast<TakeDamageFn>(g_take_damage_original.load(std::memory_order_acquire));
    if (original == nullptr) return 0.0F;
    AnomalyGenerationHandleV1 lease{};
    if (!AcquireLease(g_take_damage_handle_id, g_take_damage_handle_generation, lease))
        return 0.0F;
    float result = 0.0F;
    __try {
        result = original(self, damage, a3, a4, a5);
    } __except (1) {
        result = 0.0F;
    }
    ReleaseLease(lease);
    return result;
}

void ANOMALY_CALL ImpactDetour(void* self, int a2, void* a3, void* a4, int a5, void* a6) noexcept {
    if (g_enabled.load(std::memory_order_relaxed)) return;
    const auto original =
        reinterpret_cast<ImpactFn>(g_impact_original.load(std::memory_order_acquire));
    if (original == nullptr) return;
    AnomalyGenerationHandleV1 lease{};
    if (!AcquireLease(g_impact_handle_id, g_impact_handle_generation, lease)) return;
    __try {
        original(self, a2, a3, a4, a5, a6);
    } __except (1) {
    }
    ReleaseLease(lease);
}

bool HookReady(const AnomalyHookServiceV1* hook) noexcept {
    return hook != nullptr && hook->struct_size >= sizeof(AnomalyHookServiceV1) &&
        hook->service_version >= ANOMALY_HOOK_SERVICE_V1_VERSION && hook->create != nullptr &&
        hook->release != nullptr && hook->begin_callback != nullptr &&
        hook->end_callback != nullptr;
}

bool InstallOne(const char* label, std::uintptr_t target, void* detour,
                std::atomic<std::uintptr_t>& original_out,
                std::atomic<std::uint64_t>& handle_id_out,
                std::atomic<std::uint64_t>& handle_generation_out, std::string& error) {
    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = detour;
    request.label = anomaly::sdk::StringView(label);
    std::uintptr_t original = 0;
    AnomalyGenerationHandleV1 handle{};
    const auto status = g_hook->create(g_hook->user, &request, &original, &handle);
    if (status.code != ANOMALY_STATUS_V1_OK || original == 0 || handle.id == 0) {
        error = label;
        return false;
    }
    original_out.store(original, std::memory_order_release);
    handle_id_out.store(handle.id, std::memory_order_release);
    handle_generation_out.store(handle.generation, std::memory_order_release);
    return true;
}

void ReleaseOne(std::atomic<std::uintptr_t>& original,
                std::atomic<std::uint64_t>& handle_id,
                std::atomic<std::uint64_t>& handle_generation) noexcept {
    if (g_hook == nullptr || g_hook->release == nullptr) return;
    const std::uint64_t id = handle_id.load(std::memory_order_acquire);
    if (id == 0) return;
    // 先摘 trampoline 再释放，detour 立刻退化为空实现。
    original.store(0, std::memory_order_release);
    AnomalyGenerationHandleV1 handle{};
    handle.id = id;
    handle.generation = handle_generation.load(std::memory_order_acquire);
    __try {
        static_cast<void>(g_hook->release(g_hook->user, handle));
    } __except (1) {
    }
    handle_id.store(0, std::memory_order_release);
}

} // namespace

bool InstallDurabilityGuard(const AnomalyHostApiV1* host, std::uintptr_t take_damage,
                            std::uintptr_t impact, std::string& error) {
    error.clear();
    if (host == nullptr || (take_damage == 0 && impact == 0)) {
        error = "missing-entry";
        return false;
    }
    const anomaly::sdk::Host sdk(host);
    g_hook = sdk.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID,
        ANOMALY_HOOK_SERVICE_V1_VERSION).get();
    if (!HookReady(g_hook)) {
        error = "hook-service-unavailable";
        g_hook = nullptr;
        return false;
    }
    // 两个入口各自独立安装，任一成功即可保护耐久。
    std::string take_damage_error;
    const bool take_damage_installed = take_damage != 0 &&
        InstallOne("nte-vehicle.durability.take-damage", take_damage,
            reinterpret_cast<void*>(&TakeDamageDetour), g_take_damage_original,
            g_take_damage_handle_id, g_take_damage_handle_generation, take_damage_error);
    std::string impact_error;
    const bool impact_installed = impact != 0 &&
        InstallOne("nte-vehicle.durability.impact", impact,
            reinterpret_cast<void*>(&ImpactDetour), g_impact_original, g_impact_handle_id,
            g_impact_handle_generation, impact_error);
    if (!take_damage_installed && !impact_installed) {
        ReleaseDurabilityGuard();
        error = "hook-install-failed";
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    return true;
}

void ReleaseDurabilityGuard() {
    g_enabled.store(false, std::memory_order_release);
    ReleaseOne(g_take_damage_original, g_take_damage_handle_id, g_take_damage_handle_generation);
    ReleaseOne(g_impact_original, g_impact_handle_id, g_impact_handle_generation);
    g_installed.store(false, std::memory_order_release);
    g_hook = nullptr;
}

void SetDurabilityGuardEnabled(bool enabled) noexcept {
    g_enabled.store(enabled && g_installed.load(std::memory_order_acquire),
        std::memory_order_release);
}

bool DurabilityGuardEnabled() noexcept { return g_enabled.load(std::memory_order_acquire); }

} // namespace nte_vehicle
