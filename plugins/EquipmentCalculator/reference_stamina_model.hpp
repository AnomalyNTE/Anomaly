#pragma once
#include <nlohmann/json_fwd.hpp>

namespace equipment_calculator {
// A frozen reference model, not a live observation or a game-version gate.
// The returned JSON owns all values and performs no I/O or game access.
nlohmann::json ReferenceStaminaModel();
} // namespace equipment_calculator
