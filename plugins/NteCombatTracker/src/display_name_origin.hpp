#pragma once

#include <string_view>

namespace nte::combat_stats {

// 一条名字是从哪来的。写进日志的 actor/ability/event 行，供历史页和导出判断可信度。
enum class DisplayNameOrigin { Sdk, Technical, Unknown };

[[nodiscard]] inline std::string_view DisplayNameOriginName(
    const DisplayNameOrigin origin) noexcept {
    switch (origin) {
    case DisplayNameOrigin::Sdk: return "sdk";
    case DisplayNameOrigin::Technical: return "technical";
    case DisplayNameOrigin::Unknown: return "unknown";
    }
    return "unknown";
}

}  // namespace nte::combat_stats
