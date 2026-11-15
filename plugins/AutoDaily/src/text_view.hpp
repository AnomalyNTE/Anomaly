#pragma once

// anomaly::sdk::StringView 是函数而不是类型；插件侧统一用这个可隐式转成
// AnomalyStringViewV1 的轻量视图类型。

#include "anomaly/sdk/base.h"

#include <cstddef>
#include <cstring>
#include <string>

namespace daily {

struct Str {
    AnomalyStringViewV1 view{};
    Str() = default;
    Str(const char* text) : view{text, text != nullptr ? std::strlen(text) : 0} {}
    Str(const char* data, std::size_t size) : view{data, size} {}
    Str(const std::string& text) : view{text.data(), text.size()} {}
    operator AnomalyStringViewV1() const { return view; }
};

}  // namespace daily
