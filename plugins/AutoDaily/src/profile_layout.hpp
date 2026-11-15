#pragma once

#include "anomaly/sdk/anomaly_sdk.h"
#include "native_layout.hpp"
#include "profile_service.h"

#include <cstddef>
#include <string>

namespace daily {

// profile/compatibility.profile.json 解析结果：UE5 偏移 + 两条要扫描的签名。
struct ProfileSnapshot {
    Layout layout{};
    std::string gobjects_pattern;
    std::string gobjects_entry;
    std::string gworld_pattern;
    std::string gworld_entry;
};

// 只读兼容数据文件：contract 必须等于 kProfileContract，layout 每个字段都要存在且在
// 合理范围内，两个符号各自至少一个带 pattern/entry 的候选。任何一项缺失即失败，
// 不回落任何编译期数值。
bool LoadCompatibilityProfile(const AnomalyProfileServiceV1* profile,
                              const AnomalyJsonServiceV1* json, ProfileSnapshot& out,
                              char* error, std::size_t capacity) noexcept;

// 用 anomaly.interop.signature 解析匹配地址，再按 profile 里声明的入口字节校验。
bool ResolveProfileSymbol(const AnomalySignatureServiceV1* signature,
                          const AnomalyCoreServiceV1* core, const char* pattern,
                          const char* entry, std::uintptr_t& instruction) noexcept;

}  // namespace daily
