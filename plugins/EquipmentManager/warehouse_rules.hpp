#pragma once
#include "algorithm.hpp"

namespace equipment_manager {
// Resolve the same per-character scoring presets as automatic loadouts. Includes
// named catalog characters without an owned character; missing weights stay unknown.
nlohmann::json ResolveWarehouseScoringRoles(const nlohmann::json& workspace,
    const nlohmann::json& filters, const nlohmann::json& recommended_catalog);

// Pure ordered-rule evaluation. Caller injects locked-plan exclude_uids and
// freezes this preview before explicitly applying its changes through the
// normal operation queue. Unknown scores never become zero or a discard.
nlohmann::json PreviewWarehouseRules(const nlohmann::json& request,
    const nlohmann::json& frozen_inventory, const AlgorithmCancelCheck& cancel_check);
} // namespace equipment_manager
