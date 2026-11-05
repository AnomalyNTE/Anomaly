#pragma once

#include <anomaly/sdk/anomaly_sdk.h>

#include <cstddef>

namespace nte::sdk_compat {

template <typename Struct, typename Field>
[[nodiscard]] bool HasField(
    const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

[[nodiscard]] inline bool CombatServiceV18Available(
    const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
               decltype(AnomalyNteCombatServiceV1::next_event)>(
               service,
               offsetof(AnomalyNteCombatServiceV1, next_event)) &&
        service->current_combatant != nullptr &&
        service->participant_path_utf8 != nullptr &&
        service->latest_event_sequence != nullptr &&
        service->next_event != nullptr;
}

[[nodiscard]] inline bool CombatEventNameResolverAvailable(
    const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
               decltype(AnomalyNteCombatServiceV1::event_name_utf8)>(
               service, offsetof(AnomalyNteCombatServiceV1, event_name_utf8)) &&
        service->event_name_utf8 != nullptr;
}

[[nodiscard]] inline bool ParticipantDisplayNameResolverAvailable(
    const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
               decltype(AnomalyNteCombatServiceV1::participant_display_name_utf8)>(
               service,
               offsetof(AnomalyNteCombatServiceV1,
                   participant_display_name_utf8)) &&
        service->participant_display_name_utf8 != nullptr;
}

[[nodiscard]] inline bool CombatEventV18Available(
    const AnomalyNteCombatEventV1* event) noexcept {
    return HasField<AnomalyNteCombatEventV1,
        decltype(AnomalyNteCombatEventV1::reaction_display_type)>(
        event, offsetof(AnomalyNteCombatEventV1, reaction_display_type));
}

[[nodiscard]] inline bool CombatEventNameAvailable(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyNteCombatEventV1* event) noexcept {
    return CombatEventNameResolverAvailable(service) &&
        CombatEventV18Available(event) &&
        event->name_id != 0 &&
        (event->flags & ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID) != 0;
}

[[nodiscard]] inline bool SkillsServiceV18Available(
    const AnomalyNteSkillsServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillsServiceV1,
               decltype(AnomalyNteSkillsServiceV1::ability_display_name_utf8)>(
               service,
               offsetof(AnomalyNteSkillsServiceV1, ability_display_name_utf8)) &&
        service->frame != nullptr && service->page != nullptr &&
        service->ability_path_utf8 != nullptr &&
        service->ability_display_name_utf8 != nullptr;
}

}  // namespace nte::sdk_compat
