#pragma once

#include "rob_bank_runtime.hpp"

#include <chrono>
#include <cstdint>

namespace pink_paw_heist_esp {

class LootRefreshPolicy final {
public:
    using Clock = std::chrono::steady_clock;

    explicit LootRefreshPolicy(const Clock::duration interval) noexcept
        : interval_(interval) {}

    [[nodiscard]] bool Begin(
        const Clock::time_point now,
        const bool requested) noexcept {
        if (requested) stable_ = false;
        return requested || (!stable_ && now >= next_refresh_);
    }

    void Complete(
        const Clock::time_point now,
        const bool unchanged,
        const bool has_loot) noexcept {
        stable_ = unchanged && has_loot;
        next_refresh_ = now + interval_;
    }

    void Fail(const Clock::time_point now) noexcept {
        stable_ = false;
        next_refresh_ = now + interval_;
    }

    void Reset() noexcept {
        stable_ = false;
        next_refresh_ = {};
    }

private:
    Clock::duration interval_;
    Clock::time_point next_refresh_{};
    bool stable_{};
};

[[nodiscard]] constexpr bool PassesPickabilityFilter(
    const RobBankPickability pickability,
    const bool pickable_only) noexcept {
    return !pickable_only || pickability != RobBankPickability::blocked;
}

enum class KnownLootValidationAction {
    unchanged,
    updated,
    remove,
};

struct KnownLootValidationState final {
    RobBankInspection inspection;
    std::uint8_t missing_observations{};
};

[[nodiscard]] inline KnownLootValidationAction ApplyKnownLootObservation(
    KnownLootValidationState& state,
    const RobBankInspection& observation) {
    constexpr std::uint8_t kMissingObservationLimit = 2;

    if (!observation.entity.Valid()) {
        if (state.missing_observations < kMissingObservationLimit) {
            ++state.missing_observations;
        }
        return state.missing_observations >= kMissingObservationLimit
            ? KnownLootValidationAction::remove
            : KnownLootValidationAction::updated;
    }

    if (state.inspection.entity.Valid() &&
        (state.inspection.entity.object_index != observation.entity.object_index ||
         state.inspection.entity.object_serial != observation.entity.object_serial)) {
        return KnownLootValidationAction::remove;
    }

    if (state.inspection.pickability == RobBankPickability::candidate &&
        observation.pickability == RobBankPickability::blocked) {
        return KnownLootValidationAction::remove;
    }

    RobBankInspection next = state.inspection;
    next.entity = observation.entity;
    if (observation.pickability != RobBankPickability::unavailable) {
        next.pickability = observation.pickability;
    }
    next.name_utf8 = observation.name_utf8;
    next.fons_value = observation.fons_value;
    next.pink_paw_coin_value = observation.pink_paw_coin_value;
    next.item_resolved = observation.item_resolved;
    const bool changed = state.missing_observations != 0 ||
        next.entity.object_index != state.inspection.entity.object_index ||
        next.entity.object_serial != state.inspection.entity.object_serial ||
        next.pickability != state.inspection.pickability ||
        next.item_resolved != state.inspection.item_resolved ||
        next.name_utf8 != state.inspection.name_utf8 ||
        next.fons_value != state.inspection.fons_value ||
        next.pink_paw_coin_value != state.inspection.pink_paw_coin_value;
    state.inspection = next;
    state.missing_observations = 0;
    return changed
        ? KnownLootValidationAction::updated
        : KnownLootValidationAction::unchanged;
}

}  // namespace pink_paw_heist_esp
