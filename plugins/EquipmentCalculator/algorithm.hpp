#pragma once

#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <functional>
#include <string_view>

namespace equipment_calculator {

namespace algorithm_limits {
inline constexpr int kDefaultTimeLimitMs = 10000;
inline constexpr int kMaximumTimeLimitMs = 120000;
inline constexpr int kDefaultNodes = 2000000;
inline constexpr int kMaximumNodes = 100000000;
inline constexpr int kDefaultCombinations = 20000;
inline constexpr int kMaximumCombinations = 5000000;
inline constexpr int kMaximumTopK = 20;
} // namespace algorithm_limits

enum class AlgorithmFailure {
    invalid_request,
    inventory_unavailable,
    stale_snapshot,
    missing_character,
    invalid_inventory,
    incomplete_attributes,
    incomplete_main_stat,
    incomplete_shape,
    incomplete_set,
    unsupported_quality,
    cancelled,
    budget_exhausted,
};

struct InvalidAlgorithm { AlgorithmFailure reason; };

// Stable machine identifiers. The API owner localizes them at the UI boundary.
std::string_view AlgorithmErrorCode(AlgorithmFailure failure) noexcept;
const char* GradeLoadout(double score);

using AlgorithmCancelCheck = std::function<bool()>;

enum class AlgorithmProgressPhase { preparing, searching, coverage, improving, alternatives, blueprints, computing };

// Sampled work and budget usage, never a percentage of the search space.
// All callbacks run synchronously on the caller's pure computation thread.
struct AlgorithmProgressSnapshot {
    AlgorithmProgressPhase phase{AlgorithmProgressPhase::preparing};
    std::int64_t elapsed_ms{}, time_limit_ms{};
    std::uint64_t nodes{}, max_nodes{}, combinations{}, combination_budget{};
    std::uint32_t current_group{}, groups_total{}, groups_visited{}, roles_completed{}, roles_total{};
    bool combinations_available{}, multi_role{};
};
using AlgorithmProgressCallback = std::function<void(const AlgorithmProgressSnapshot&)>;

inline std::string_view AlgorithmProgressPhaseName(AlgorithmProgressPhase phase) noexcept {
    switch (phase) {
    case AlgorithmProgressPhase::preparing: return "preparing";
    case AlgorithmProgressPhase::searching: return "searching";
    case AlgorithmProgressPhase::coverage: return "coverage";
    case AlgorithmProgressPhase::improving: return "improving";
    case AlgorithmProgressPhase::alternatives: return "alternatives";
    case AlgorithmProgressPhase::blueprints: return "blueprints";
    case AlgorithmProgressPhase::computing: return "computing";
    }
    return "computing";
}

// Pure computation over caller-owned immutable JSON. No game access or I/O.
// Preferences use exact native property IDs; scores follow the reference
// ScoringEngine's sub-stat identity and maximum-level core-main rules.
nlohmann::json ScoreEquipment(const nlohmann::json& item, const nlohmann::json& preferences);

// Returns one complete-board preview, never an equipment command. The caller
// runs this on an owned pure-computation thread without SDK access or the
// inventory mutex. Cancellation throws
// InvalidAlgorithm{cancelled}; a budget stop may return a valid non-optimal
// preview, explicitly marked search.exhaustive=false. See algorithm.md.
nlohmann::json OptimizeLoadout(const nlohmann::json& request,
    const nlohmann::json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
    const AlgorithmProgressCallback& progress = {});

nlohmann::json OptimizeTopK(const nlohmann::json& request,
    const nlohmann::json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
    const AlgorithmProgressCallback& progress = {});
nlohmann::json OptimizeLoadouts(const nlohmann::json& request,
    const nlohmann::json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
    const AlgorithmProgressCallback& progress = {});
nlohmann::json AssessInventory(const nlohmann::json& request,
    const nlohmann::json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
    const AlgorithmProgressCallback& progress = {});
nlohmann::json GenerateBlueprints(const nlohmann::json& request,
    const AlgorithmCancelCheck& cancel_check, const AlgorithmProgressCallback& progress = {});
nlohmann::json RecommendRewind(const nlohmann::json& request);

} // namespace equipment_calculator
