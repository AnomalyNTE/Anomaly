#pragma once
#include "anomaly/sdk/cpp.hpp"

#include <cstdint>
#include <string>

namespace nte_vehicle {

// 载具耐久保护：拦下碰撞伤害链路的两个入口，让碰撞伤害事件不再继续，
// 因此不会产生耐久扣除。插件不写耐久数值——耐久由服务器持有，客户端只能
// 阻止「扣耐久」的前置伤害事件。
//
// 两个入口地址都由 profile 签名扫描得到，ProfileSnapshot::Load 已经校验过
// 唯一命中与入口字节；这里只负责安装钩子和维护开关状态。
struct DurabilityGuardCounters {
    std::uint32_t take_damage_calls{};
    std::uint32_t take_damage_blocked{};
    std::uint32_t impact_calls{};
    std::uint32_t impact_blocked{};
};

// 只在 Lifecycle 回调里调用。两个入口各自独立安装：任何一个装不上都不影响另一个，
// 只要至少装上一个保护就成立。实机观察表明真正止住耐久扣除的是撞击处理器一侧，
// 受伤入口是另一条通向同一伤害施加点的路径，因此两者都安装、由 UI 计数器报告实际命中。
bool InstallDurabilityGuard(const AnomalyHostApiV1* host, std::uintptr_t take_damage,
                            std::uintptr_t impact, std::string& error);
void ReleaseDurabilityGuard();
void SetDurabilityGuardEnabled(bool enabled) noexcept;
[[nodiscard]] bool DurabilityGuardEnabled() noexcept;
[[nodiscard]] bool DurabilityGuardInstalled() noexcept;
[[nodiscard]] bool DurabilityGuardImpactHooked() noexcept;

// 载具受伤入口是否已挂上；未挂上时该入口保持原样，不产生任何拦截。
bool DurabilityGuardTakeDamageHooked() noexcept;
[[nodiscard]] DurabilityGuardCounters DurabilityGuardCounts() noexcept;

} // namespace nte_vehicle
