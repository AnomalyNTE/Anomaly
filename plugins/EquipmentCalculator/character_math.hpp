#pragma once
#include "algorithm.hpp"

namespace equipment_calculator {
// Raw native property units. Sources are frozen, complete, unconditional
// contributions assembled by the caller from profile and verified catalogs.
nlohmann::json CalculateCharacter(const nlohmann::json& request,
    const AlgorithmCancelCheck& cancel_check);
// The same frozen panel model, with real unique equipment UIDs. Removal gains
// compare against that same model; replacement ranking uses one frozen snapshot.
nlohmann::json CalculateEquipmentContributions(const nlohmann::json& request,
    const AlgorithmCancelCheck& cancel_check);
nlohmann::json RankEquipmentReplacements(const nlohmann::json& request,
    const nlohmann::json& frozen_inventory, const AlgorithmCancelCheck& cancel_check);
// Explicit transition costs, experience denominations, stock and deterministic
// farming yields. These pure functions do not read a catalog or mutate stock.
nlohmann::json CalculateCultivation(const nlohmann::json& request,
    const AlgorithmCancelCheck& cancel_check);
nlohmann::json CalculateStamina(const nlohmann::json& request,
    const AlgorithmCancelCheck& cancel_check);
nlohmann::json CalculateExperienceMaterials(const nlohmann::json& request,
    const AlgorithmCancelCheck& cancel_check);
} // namespace equipment_calculator
