#pragma once

#include "loot_catalog.generated.h"

#include <cstdint>

namespace pink_paw_heist_esp {

[[nodiscard]] constexpr bool IsAccessCard(
    const catalog::ItemDefinition* const item) noexcept {
    return item != nullptr && item->item_id.starts_with("RobBankItem_G");
}

[[nodiscard]] constexpr bool PassesItemValueFilter(
    const catalog::ItemDefinition* const item,
    const std::uint32_t minimum_value,
    const bool always_show_access_cards) noexcept {
    if (item == nullptr) return false;
    if (always_show_access_cards && IsAccessCard(item)) return true;
    return item->value.has_value() && *item->value >= minimum_value;
}

}  // namespace pink_paw_heist_esp
