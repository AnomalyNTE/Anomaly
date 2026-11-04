#include "sdk_compat.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

AnomalyStatusV1 ANOMALY_CALL StatusCombatant(
    void*, AnomalyNteCombatantSnapshotV1*) { return {ANOMALY_STATUS_V1_OK}; }
std::uint64_t ANOMALY_CALL Latest(void*) { return 0; }
AnomalyStatusV1 ANOMALY_CALL Next(
    void*, std::uint64_t, AnomalyNteCombatEventV1*) { return {ANOMALY_STATUS_V1_OK}; }
AnomalyStatusV1 ANOMALY_CALL EventName(
    void*, const AnomalyNteCombatEventV1*, char*, std::size_t*) {
    return {ANOMALY_STATUS_V1_OK};
}
AnomalyStatusV1 ANOMALY_CALL ParticipantName(
    void*, AnomalyGenerationHandleV1, char*, std::size_t*) {
    return {ANOMALY_STATUS_V1_OK};
}

void CombatServiceRequiresTheCompleteV18Tail() {
    AnomalyNteCombatServiceV1 service{};
    service.current_combatant = StatusCombatant;
    service.participant_path_utf8 = ParticipantName;
    service.latest_event_sequence = Latest;
    service.next_event = Next;
    service.event_name_utf8 = EventName;
    service.participant_display_name_utf8 = ParticipantName;

    service.struct_size = offsetof(AnomalyNteCombatServiceV1, latest_event_sequence);
    Check(!nte::sdk_compat::CombatServiceV18Available(&service),
        "a v1.7-sized combat service table must be rejected");
    service.struct_size = sizeof(service);
    Check(nte::sdk_compat::CombatServiceV18Available(&service),
        "a complete v1.8 combat service table should be accepted");
    service.event_name_utf8 = nullptr;
    service.participant_display_name_utf8 = nullptr;
    Check(nte::sdk_compat::CombatServiceV18Available(&service),
        "optional name resolvers must not block unified event capture");
    Check(!nte::sdk_compat::CombatEventNameResolverAvailable(&service) &&
              !nte::sdk_compat::ParticipantDisplayNameResolverAvailable(&service),
        "missing name resolvers should independently fall back to technical names");
}

void EventNamesRequireTheV18ShapeAndNameValidFlag() {
    AnomalyNteCombatServiceV1 service{};
    service.struct_size = sizeof(service);
    service.current_combatant = StatusCombatant;
    service.participant_path_utf8 = ParticipantName;
    service.latest_event_sequence = Latest;
    service.next_event = Next;
    service.event_name_utf8 = EventName;
    service.participant_display_name_utf8 = ParticipantName;
    AnomalyNteCombatEventV1 event{};
    event.struct_size = sizeof(event);
    event.name_id = 42;
    Check(!nte::sdk_compat::CombatEventNameAvailable(&service, &event),
        "event names must not be queried before NAME_VALID is set");
    event.flags = ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
    Check(nte::sdk_compat::CombatEventNameAvailable(&service, &event),
        "valid v1.8 events with NAME_VALID should allow runtime name resolution");
}

}  // namespace

int main() {
    CombatServiceRequiresTheCompleteV18Tail();
    EventNamesRequireTheV18ShapeAndNameValidFlag();
    std::cout << "All SDK compatibility tests passed\n";
}
