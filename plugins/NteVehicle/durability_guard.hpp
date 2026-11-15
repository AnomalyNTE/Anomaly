#pragma once
#include "anomaly/sdk/cpp.hpp"

#include <cstdint>
#include <string>

namespace nte_vehicle {

// 载具耐久保护：拦截碰撞伤害入口，让伤害事件不再推进，因此不产生耐久扣除。
// 耐久数值由服务器持有，插件不写入。两个地址来自 profile 签名扫描，
// ProfileSnapshot::Load 已校验唯一命中与入口字节。
bool InstallDurabilityGuard(const AnomalyHostApiV1* host, std::uintptr_t take_damage,
                            std::uintptr_t impact, std::string& error);
void ReleaseDurabilityGuard();
void SetDurabilityGuardEnabled(bool enabled) noexcept;
[[nodiscard]] bool DurabilityGuardEnabled() noexcept;

} // namespace nte_vehicle
