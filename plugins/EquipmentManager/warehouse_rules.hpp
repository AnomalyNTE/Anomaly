#pragma once
#include "algorithm.hpp"

namespace equipment_manager {
// Pure ordered-rule evaluation. Caller injects locked-plan exclude_uids and
// freezes this preview before explicitly applying its changes through the
// normal operation queue. Unknown scores never become zero or a discard.
nlohmann::json PreviewWarehouseRules(const nlohmann::json& request,
    const nlohmann::json& frozen_inventory, const AlgorithmCancelCheck& cancel_check);
} // namespace equipment_manager
