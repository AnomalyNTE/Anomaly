#pragma once

#include <nlohmann/json.hpp>
#include <vector>

namespace equipment_calculator {

using Json = nlohmann::json;

enum class CommandFailure {
    invalid_request, missing_character, missing_equipment, wrong_type,
    invalid_layout, occupied, duplicate_uid, unsupported_action
};
struct InvalidCommand { CommandFailure reason; };

// Pure planning over a frozen, complete inventory. These functions never read
// game memory and never turn a requested result into an observed inventory.
std::vector<Json> PrepareOperation(const Json& request, const Json& inventory);
Json ResolveStep(const Json& step, const Json& inventory);
bool StepSatisfied(const Json& command, const Json& inventory);
bool SameUid(const Json& left, const Json& right);
Json ValidateSavedLoadout(const Json& plan, const Json& inventory, const Json& allowed_cells);

} // namespace equipment_calculator
