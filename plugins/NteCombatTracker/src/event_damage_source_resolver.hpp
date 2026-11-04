#pragma once

#include "resource_name_resolver.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace nte::combat_stats {

using DamageSourceNameLookup = std::function<std::string(std::uint64_t)>;

[[nodiscard]] ResolvedDamageName ResolveEventDamageSource(
    std::uint64_t source_id,
    const DamageSourceNameLookup& lookup,
    const ResourceNameResolver& resolver);
[[nodiscard]] std::string DamageSourceIdentity(
    std::uint64_t source_id,
    std::string_view technical_name);

}  // namespace nte::combat_stats
