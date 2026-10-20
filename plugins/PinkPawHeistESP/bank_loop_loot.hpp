#pragma once

#include "rob_bank_runtime.hpp"

#include <cstdint>

namespace pink_paw_heist_esp {

struct BankLoopLootObservation final {
    bool ready{};
    bool present{};
    bool has_targets{};
    bool has_unattempted_targets{};
    std::uint32_t observed_count{};
    std::uint32_t ready_count{};
    std::uint32_t target_count{};
};

// Readiness belongs to the collected frame and the inspection context. Item
// metadata is independent: an unresolved price excludes that item from value
// filtering, but cannot block other identified loot in the same bank.
template <typename Entries, typename IsTarget, typename IsAttempted>
[[nodiscard]] BankLoopLootObservation ObserveBankLoopLoot(
    const Entries& entries, const bool cache_available, const bool context_ready,
    const bool discovery_pending, IsTarget is_target, IsAttempted is_attempted) {
    BankLoopLootObservation result;
    result.ready = cache_available && context_ready && !discovery_pending;
    if (!cache_available) return result;
    for (const auto& entry : entries) {
        ++result.observed_count;
        const bool complete = (entry.snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
            (entry.snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) == 0;
        const bool inspected = complete && entry.rob_bank.entity.Valid() &&
            entry.rob_bank.pickability == RobBankPickability::candidate &&
            (entry.rob_bank.item_resolved || entry.rob_bank.is_access_card);
        if (inspected) ++result.ready_count;
        if (is_target(entry)) {
            result.has_unattempted_targets |= !is_attempted(entry.rob_bank.entity);
            // Cards are supporting interactions, not value-filtered loot.
            // Try each once to unlock loot, but a persistent zero-value card
            // actor must not prevent evacuation after the valued loot is gone.
            if (!entry.rob_bank.is_access_card) {
                ++result.target_count;
                result.has_targets = true;
            }
        }
    }
    // Count actual usable loot before the user's minimum-value filter. A bank
    // with zero matching values can finish, but an early empty frame cannot.
    result.present = result.ready_count != 0;
    return result;
}

}  // namespace pink_paw_heist_esp
