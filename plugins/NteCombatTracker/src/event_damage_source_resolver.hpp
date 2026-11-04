#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nte::combat_stats {

// 伤害来源的聚合身份。优先用技术名（同一技能的多个 name_id 会解析到同一名字），
// 解析不出来才退回 name_id，保证两个未解析来源不会被并成同一行。
// 这里只决定身份，不产生用户可见文本——显示名由宿主 ABI 提供。
[[nodiscard]] std::string DamageSourceIdentity(
    std::uint64_t source_id, std::string_view technical_name);

}  // namespace nte::combat_stats
