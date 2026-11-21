#pragma once

// 所有用户可见文案都过这里：中文只存在于 locales/zh-CN.json，
// 代码里只写英文回退（宿主缺目录时才用得到回退）。

#include "anomaly/sdk/services/localization.h"

#include <cstddef>
#include <string>

namespace daily {

void BindLocalization(const AnomalyLocalizationServiceV1* service) noexcept;

std::string TextLocalized(const char* key, const char* fallback);
std::string TextLocalized(const char* key, const char* fallback, const std::string* arguments,
               std::size_t count);

}  // namespace daily
