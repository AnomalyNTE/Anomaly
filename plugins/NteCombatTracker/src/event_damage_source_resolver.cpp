#include "event_damage_source_resolver.hpp"

namespace nte::combat_stats {

std::string DamageSourceIdentity(
    const std::uint64_t source_id,
    const std::string_view technical_name) {
    if (!technical_name.empty() &&
        !technical_name.starts_with("Unknown") &&
        !technical_name.starts_with("unknown")) {
        return std::string(technical_name);
    }
    return "#" + std::to_string(source_id);
}

}  // namespace nte::combat_stats
