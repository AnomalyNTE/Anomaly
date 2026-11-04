#include "combat_stats_engine.hpp"
#include "combat_boundary_policy.hpp"
#include "combat_cursor.hpp"

#include <chrono>
#include <iostream>
#include <limits>

namespace {

using namespace std::chrono_literals;
using nte::combat_stats::CombatStatsEngine;
using nte::combat_stats::Combatant;
using nte::combat_stats::DamageEvent;
using nte::combat_stats::EncounterState;
using nte::combat_stats::Handle;
using nte::combat_stats::TimePoint;
using nte::combat_stats::CombatBoundaryAction;

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

DamageEvent Outgoing(
    std::uint64_t sequence,
    Handle world,
    Handle attacker,
    std::int64_t damage);

void RelevantDamageStartsEncounter() {
    CombatStatsEngine engine;
    const TimePoint start{100s};
    const Handle world{1, 1};
    const Handle player{2, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    DamageEvent event;
    event.sequence = 1;
    event.world = world;
    event.attacker = player;
    event.victim = {3, 1};
    event.name_id = 10;
    event.final_value = 125;
    event.source_name = "GE_Test";
    event.victim_name = "Enemy";

    Check(engine.AcceptDamage(event, start), "player event should be accepted");
    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.state == EncounterState::Active, "first player event should start encounter");
    Check(snapshot.outgoing_damage == 125, "outgoing damage should be accumulated");
    Check(snapshot.outgoing_positive_hits == 1, "positive outgoing hit should be counted");
}

void NonDamageEventsDoNotStartOrExtendEncounter() {
    CombatStatsEngine engine;
    const TimePoint start{125s};
    const Handle world{3, 1};
    const Handle player{4, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto heal = Outgoing(1, world, player, 100);
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    Check(!engine.AcceptEvent(heal, start), "healing must not start an encounter");
    auto buff = Outgoing(2, world, player, 0);
    buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    Check(!engine.AcceptEvent(buff, start + 1s), "BUFF must not start an encounter");
    Check(engine.BuildSnapshot(start + 1s).state == EncounterState::Idle,
        "non-damage events must leave an idle engine idle");

    Check(engine.AcceptDamage(Outgoing(3, world, player, 50), start + 2s),
        "positive damage should start the encounter");
    auto late_heal = Outgoing(4, world, player, 100);
    late_heal.kind = nte::combat_stats::CombatEventKind::Heal;
    Check(engine.AcceptEvent(late_heal, start + 11s),
        "healing in an active encounter should remain visible");
    engine.Advance(start + 12s);
    Check(engine.BuildSnapshot(start + 12s).state == EncounterState::Completed,
        "healing must not extend the damage inactivity timeout");
}

void IncomingDamageStartsAndExtendsEncounter() {
    CombatStatsEngine engine;
    const TimePoint start{1150s};
    const Handle world{16, 1};
    const Handle player{17, 1};
    const Handle enemy{18, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto incoming = Outgoing(1, world, enemy, 80);
    incoming.victim = player;
    incoming.attacker_name = "Enemy";
    incoming.victim_name = "/Game/Player/player_incoming_C";
    Check(engine.AcceptDamage(incoming, start),
        "incoming positive damage should start the encounter");
    auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.state == EncounterState::Active,
        "first incoming positive damage should activate combat");
    Check(snapshot.incoming_damage == 80 && snapshot.incoming_positive_hits == 1,
        "incoming positive damage should aggregate on the controlled victim");

    auto boundary_hit = incoming;
    boundary_hit.sequence = 2;
    boundary_hit.final_value = 30;
    Check(engine.AcceptDamage(boundary_hit, start + 10s),
        "a fresh incoming hit at the inactivity boundary should be accepted");
    engine.Advance(start + 10s);
    snapshot = engine.BuildSnapshot(start + 10s);
    Check(snapshot.state == EncounterState::Active,
        "accepting incoming damage before Advance should keep the encounter active");
    Check(snapshot.incoming_damage == 110,
        "incoming damage should accumulate across the extended encounter");

    engine.Advance(start + 20s);
    snapshot = engine.BuildSnapshot(start + 20s);
    Check(snapshot.state == EncounterState::Completed,
        "encounter should complete only after 10 seconds without damage");
}

void CombatBoundaryPolicyDistinguishesServiceLossFromStaleCombatant() {
    Check(PlanCombatBoundaryAction(EncounterState::Active, true, true) ==
              CombatBoundaryAction::AdvanceForInactivity,
        "valid combat methods with an open recording encounter should only advance for inactivity");
    Check(PlanCombatBoundaryAction(EncounterState::Active, true, false) ==
              CombatBoundaryAction::CompleteForServiceLoss,
        "missing combat methods while recording should force service-loss close");
    Check(PlanCombatBoundaryAction(EncounterState::Idle, false, true) ==
              CombatBoundaryAction::None,
        "idle non-recording state should not advance or close");
}

void ExplicitCompletionClosesTheActiveCaptureGate() {
    CombatStatsEngine engine;
    const TimePoint start{140s};
    const Handle world{31, 1};
    const Handle player{32, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    Check(engine.AcceptDamage(Outgoing(1, world, player, 25), start),
        "damage should start an encounter before explicit completion");
    engine.CompleteEncounter();
    Check(engine.state() == EncounterState::Completed,
        "explicit lifecycle boundaries should complete the active encounter");
    auto buff = Outgoing(2, world, player, 0);
    buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    Check(!engine.AcceptEvent(buff, start + 1s),
        "completed encounters must reject non-starting events");
}

void ActiveEncounterCapturesEveryVisibleDamageField() {
    CombatStatsEngine engine;
    const TimePoint start{150s};
    const Handle world{5, 1};
    const Handle player{6, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    Check(engine.AcceptDamage(Outgoing(1, world, player, 10), start),
          "player damage should trigger the encounter");

    DamageEvent visible;
    visible.sequence = 2;
    visible.tick_sequence = 77;
    visible.flags = 0x0f;
    visible.world = world;
    visible.attacker = {901, 1};
    visible.victim = {80, 3};
    visible.name_id = 999;
    visible.value = 31;
    visible.basic_value = 29;
    visible.final_value = 25;
    visible.hit_location = {1.5, 2.5, 3.5};
    visible.damage_type = 4;
    visible.display_type = 5;
    visible.reaction_type = 6;
    visible.reaction_display_type = 7;
    visible.source_name = "NpcSkill";
    visible.attacker_name = "NpcA";
    visible.victim_name = "NpcB";

    Check(engine.AcceptDamage(visible, start + 1s),
          "an active encounter should capture damage involving an engaged actor");
    const auto snapshot = engine.BuildSnapshot(start + 1s);
    Check(snapshot.outgoing_damage == 10,
          "engaged-actor damage should not enter controlled-character output");
    Check(snapshot.recent_events.size() == 2,
          "visible damage should be retained in the recent event stream");
    if (snapshot.recent_events.size() == 2) {
        const auto& recent = snapshot.recent_events.back();
        Check(recent.name_id == visible.name_id &&
                  recent.attacker == visible.attacker &&
                  recent.victim == visible.victim,
              "recent events should retain stable source and participant identifiers");
        Check(recent.tick_sequence == 77 && recent.flags == 0x0f,
              "sequence metadata should be retained");
        Check(recent.value == 31 && recent.basic_value == 29 &&
                  recent.final_value == 25,
              "all three damage values should be retained");
        Check(recent.hit_location[0] == 1.5 && recent.damage_type == 4 &&
                  recent.reaction_display_type == 7,
              "damage classification and hit location should be retained");
    }

    engine.Advance(start + 10s);
    Check(engine.BuildSnapshot(start + 10s).state == EncounterState::Completed,
          "unrelated visible damage must not extend player activity timeout");
}

DamageEvent Outgoing(
    const std::uint64_t sequence,
    const Handle world,
    const Handle attacker,
    const std::int64_t damage) {
    DamageEvent event;
    event.sequence = sequence;
    event.world = world;
    event.attacker = attacker;
    event.victim = {900 + sequence, 1};
    event.name_id = 10;
    event.final_value = damage;
    event.source_name = "Source";
    event.victim_name = "Enemy";
    return event;
}

void EncounterLifecycleAndCharacterRotation() {
    CombatStatsEngine engine;
    const TimePoint start{200s};
    const Handle world{10, 1};
    const Handle first{11, 1};
    const Handle second{12, 1};
    engine.ObserveCombatant(Combatant{world, first, 80.0, 100.0, 5.0}, start);

    Check(!engine.AcceptDamage(Outgoing(1, world, {99, 1}, 50), start),
          "unrelated event should be ignored");
    Check(!engine.AcceptDamage(Outgoing(1, {999, 1}, first, 50), start),
          "event from another world should be ignored");
    Check(engine.AcceptDamage(Outgoing(2, world, first, 100), start),
          "first controlled character should be accepted");
    engine.ObserveCombatant(Combatant{world, second, 75.0, 100.0, 3.0}, start + 1s);
    Check(engine.AcceptDamage(Outgoing(3, world, second, 200), start + 1s),
          "rotated controlled character should join encounter");
    auto snapshot = engine.BuildSnapshot(start + 1s);
    Check(snapshot.controlled_character_count == 2, "character rotation should be merged");
    Check(snapshot.outgoing_damage == 300, "rotated character damage should share total");

    engine.Advance(start + 11s);
    snapshot = engine.BuildSnapshot(start + 11s);
    Check(snapshot.state == EncounterState::Completed, "ten idle seconds should complete encounter");

    Check(engine.AcceptDamage(Outgoing(4, world, first, 25), start + 12s),
          "new event after completion should start next encounter");
    snapshot = engine.BuildSnapshot(start + 12s);
    Check(snapshot.state == EncounterState::Active, "next encounter should be active");
    Check(snapshot.outgoing_damage == 25, "next encounter should start with clean totals");

    const Handle next_world{20, 1};
    engine.ObserveCombatant(Combatant{next_world, first, 100.0, 100.0, 0.0}, start + 13s);
    snapshot = engine.BuildSnapshot(start + 13s);
    Check(snapshot.state == EncounterState::Idle, "world change should reset encounter");
    Check(snapshot.outgoing_damage == 0, "world change should clear totals");
    Check(snapshot.controlled_character_count == 0,
          "world change should wait for actual encounter participation");
}

void ObservedCharactersRequireEncounterParticipation() {
    CombatStatsEngine engine;
    const TimePoint start{250s};
    const Handle world{21, 1};
    const Handle participating{22, 1};
    const Handle observed_only{23, 1};
    engine.ObserveCombatant(Combatant{world, participating, 100.0, 100.0, 0.0}, start);
    engine.ObserveCombatant(Combatant{world, observed_only, 100.0, 100.0, 0.0}, start);

    auto hit = Outgoing(1, world, participating, 120);
    hit.attacker_name = "/Game/Player/player_001_C";
    Check(engine.AcceptDamage(hit, start), "participating controlled character should start encounter");

    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.controlled_character_count == 1,
          "observed characters without encounter events must not be counted");
    Check(snapshot.players.size() == 1,
          "only confirmed controlled characters should produce player rows");
}

void ConfirmedCharactersMergeGenerationByResolvedPath() {
    CombatStatsEngine engine;
    const TimePoint start{275s};
    const Handle world{24, 1};
    const Handle first_generation{25, 1};
    const Handle second_generation{25, 2};
    engine.ObserveCombatant(
        Combatant{world, first_generation, 100.0, 100.0, 0.0}, start);

    auto first = Outgoing(1, world, first_generation, 100);
    first.attacker_name = "/Game/Player/player_076_shinku_C";
    first.name_id = 501;
    first.source_name = "SkillA";
    first.flags = nte::combat_stats::kDamageFlagCritical |
        nte::combat_stats::kDamageFlagHeadHit;
    engine.AcceptDamage(first, start);

    engine.ObserveCombatant(
        Combatant{world, second_generation, 100.0, 100.0, 0.0}, start + 2s);
    auto second = Outgoing(2, world, second_generation, 300);
    second.attacker_name = first.attacker_name;
    second.name_id = 502;
    second.source_name = "SkillB";
    second.flags = nte::combat_stats::kDamageFlagCritical |
        nte::combat_stats::kDamageFlagWeakUnbalance;
    engine.AcceptDamage(second, start + 2s);

    const auto snapshot = engine.BuildSnapshot(start + 2s);
    Check(snapshot.controlled_character_count == 1,
          "same resolved character path should merge generation handles");
    Check(snapshot.players.size() == 1, "merged character should have one player row");
    if (snapshot.players.size() == 1) {
        const auto& row = snapshot.players.front();
        Check(row.damage == 400 && row.positive_hits == 2,
              "player row should aggregate positive damage and hits");
        Check(row.critical_hits == 2 && row.head_hits == 1 && row.weak_hits == 1,
              "player row should aggregate NTE hit flags");
        Check(row.maximum_hit == 300 && row.maximum_source_id == 502 &&
                  row.maximum_source_name == "SkillB",
              "player row should retain maximum hit ability");
        Check(row.encounter_dps == 200.0 && row.active_dps == 200.0,
              "player row should expose encounter and active DPS");
        Check(row.damage_share == 100.0,
              "single player should own one hundred percent of outgoing damage");
    }
}

void DistinctParticipatingCharactersProduceDistinctRows() {
    CombatStatsEngine engine;
    const TimePoint start{290s};
    const Handle world{30, 1};
    const Handle first{31, 1};
    const Handle second{32, 1};
    engine.ObserveCombatant(Combatant{world, first, 100.0, 100.0, 0.0}, start);
    engine.ObserveCombatant(Combatant{world, second, 100.0, 100.0, 0.0}, start);

    auto first_hit = Outgoing(1, world, first, 100);
    first_hit.attacker_name = "/Game/Player/player_first_C";
    engine.AcceptDamage(first_hit, start);
    auto second_hit = Outgoing(2, world, second, 300);
    second_hit.attacker_name = "/Game/Player/player_second_C";
    engine.AcceptDamage(second_hit, start + 4s);

    const auto snapshot = engine.BuildSnapshot(start + 4s);
    Check(snapshot.players.size() == 2 && snapshot.controlled_character_count == 2,
          "two distinct controlled characters that deal damage should produce two rows");
    Check(snapshot.players[0].damage + snapshot.players[1].damage ==
              snapshot.outgoing_damage,
          "confirmed character damage rows should reconcile to encounter output");
    Check(snapshot.players[0].encounter_dps + snapshot.players[1].encounter_dps ==
              snapshot.encounter_dps,
          "character encounter DPS should use the encounter denominator");
}

void SpectatorAndCrossWorldEventsDoNotConfirmCharacters() {
    CombatStatsEngine engine;
    const TimePoint start{295s};
    const Handle world{40, 1};
    const Handle player{41, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto spectator = Outgoing(1, world, Handle{42, 1}, 100);
    spectator.attacker_name = "/Game/Npc/spectator_a_C";
    spectator.victim = {43, 1};
    Check(!engine.AcceptDamage(spectator, start),
          "a bystander fight should not start an encounter");
    auto cross_world = Outgoing(2, Handle{99, 1}, player, 100);
    cross_world.attacker_name = "/Game/Player/player_C";
    Check(!engine.AcceptDamage(cross_world, start),
          "a controlled handle event from another world should be ignored");
    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.players.empty() && snapshot.controlled_character_count == 0,
          "ignored events should not confirm controlled characters");
}

void ActiveEncounterCapturesOnlyPlayersAndEngagedActors() {
    CombatStatsEngine engine;
    const TimePoint start{275s};
    const Handle world{45, 1};
    const Handle player{46, 1};
    const Handle enemy{47, 1};
    const Handle bystander{48, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto opening = Outgoing(1, world, player, 100);
    opening.victim = enemy;
    Check(engine.AcceptEvent(opening, start),
        "player damage should open an encounter and engage its opponent");

    auto enemy_buff = Outgoing(2, world, {}, 0);
    enemy_buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    enemy_buff.victim = enemy;
    Check(engine.AcceptEvent(enemy_buff, start + 1s),
        "events involving an engaged actor should remain in the encounter");

    auto unrelated = enemy_buff;
    unrelated.sequence = 3;
    unrelated.victim = bystander;
    Check(!engine.AcceptEvent(unrelated, start + 2s),
        "same-world events unrelated to players or engaged actors must be rejected");

    engine.Advance(start + 10s);
    auto replacement = Outgoing(4, world, player, 50);
    replacement.victim = bystander;
    Check(engine.AcceptEvent(replacement, start + 11s),
        "new player damage should open a replacement encounter");
    enemy_buff.sequence = 5;
    Check(!engine.AcceptEvent(enemy_buff, start + 12s),
        "engaged actors must not leak into a replacement encounter");
}

void BothControlledParticipantsStoreOneVisibleEvent() {
    CombatStatsEngine engine;
    const TimePoint start{298s};
    const Handle world{50, 1};
    const Handle attacker{51, 1};
    const Handle victim{52, 1};
    engine.ObserveCombatant(Combatant{world, attacker, 100.0, 100.0, 0.0}, start);
    engine.ObserveCombatant(Combatant{world, victim, 100.0, 100.0, 0.0}, start);
    auto hit = Outgoing(1, world, attacker, 100);
    hit.attacker_name = "/Game/Player/attacker_C";
    hit.victim = victim;
    hit.victim_name = "/Game/Player/victim_C";
    engine.AcceptDamage(hit, start);

    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.recent_events.size() == 1,
          "an event whose two participants are controlled should be stored once");
    Check(snapshot.outgoing_damage == 100 && snapshot.incoming_damage == 100,
          "a both-controlled event should contribute once to each direction");
    Check(snapshot.players.size() == 2,
          "both controlled participants should be confirmed by the event");
}

void DirectionAndNonPositiveRules() {
    CombatStatsEngine engine;
    const TimePoint start{300s};
    const Handle world{30, 1};
    const Handle first{31, 1};
    const Handle second{32, 1};
    engine.ObserveCombatant(Combatant{world, first, 90.0, 100.0, 7.0}, start);
    engine.ObserveCombatant(Combatant{world, second, 70.0, 100.0, 2.0}, start);

    auto incoming = Outgoing(1, world, {40, 1}, 80);
    incoming.victim = first;
    incoming.attacker_name = "EnemyA";
    incoming.victim_name = "/Game/Player/player_first_C";
    engine.AcceptDamage(incoming, start);

    auto both = Outgoing(2, world, first, 30);
    both.victim = second;
    both.attacker_name = "/Game/Player/player_first_C";
    both.victim_name = "/Game/Player/player_second_C";
    engine.AcceptDamage(both, start + 1s);

    engine.AcceptDamage(Outgoing(3, world, first, 0), start + 2s);
    engine.AcceptDamage(Outgoing(4, world, first, -15), start + 3s);

    const auto snapshot = engine.BuildSnapshot(start + 3s);
    Check(snapshot.outgoing_damage == 30, "only positive outgoing damage should accumulate");
    Check(snapshot.incoming_damage == 110, "incoming should include enemy and controlled-to-controlled damage");
    Check(snapshot.outgoing_positive_hits == 1, "only positive outgoing hit should count");
    Check(snapshot.incoming_positive_hits == 2, "positive incoming hits should count");
    Check(snapshot.non_positive_events == 2, "non-positive events should be counted separately");
    Check(snapshot.recent_events.size() == 4, "each accepted event should produce one log row");
    if (snapshot.recent_events.size() > 1) {
        Check(snapshot.recent_events[1].direction == nte::combat_stats::EventDirection::Both,
              "controlled-to-controlled event should be marked both");
    }
    const auto first_row = std::ranges::find_if(snapshot.players,
        [&](const auto& row) { return row.handle == first; });
    const auto second_row = std::ranges::find_if(snapshot.players,
        [&](const auto& row) { return row.handle == second; });
    Check(first_row != snapshot.players.end() && first_row->incoming_damage == 80 &&
              first_row->incoming_positive_hits == 1 &&
              first_row->maximum_incoming_hit == 80,
          "incoming damage should aggregate on the controlled victim row");
    Check(second_row != snapshot.players.end() && second_row->incoming_damage == 30 &&
              second_row->incoming_positive_hits == 1,
          "controlled-to-controlled damage should aggregate on the victim only");
    Check(first_row != snapshot.players.end() && first_row->incoming_share > 72.7 &&
              first_row->incoming_share < 72.8,
          "incoming share should use total encounter incoming damage");
}

void CriticalRateCountsOnlyPositiveOutgoingHits() {
    CombatStatsEngine engine;
    const TimePoint start{320s};
    const Handle world{60, 1};
    const Handle first_generation{61, 1};
    const Handle second_generation{61, 2};
    engine.ObserveCombatant(
        Combatant{world, first_generation, 100.0, 100.0, 0.0}, start);

    auto critical = Outgoing(1, world, first_generation, 100);
    critical.attacker_name = "/Game/Player/player_critical_C";
    critical.flags = nte::combat_stats::kDamageFlagCritical;
    engine.AcceptDamage(critical, start);
    auto normal = Outgoing(2, world, first_generation, 80);
    normal.attacker_name = critical.attacker_name;
    engine.AcceptDamage(normal, start + 1s);
    engine.ObserveCombatant(
        Combatant{world, second_generation, 100.0, 100.0, 0.0}, start + 2s);
    auto rotated = Outgoing(3, world, second_generation, 60);
    rotated.attacker_name = critical.attacker_name;
    engine.AcceptDamage(rotated, start + 2s);
    auto immune_critical = Outgoing(4, world, second_generation, 0);
    immune_critical.attacker_name = critical.attacker_name;
    immune_critical.flags = nte::combat_stats::kDamageFlagCritical;
    engine.AcceptDamage(immune_critical, start + 3s);
    auto negative_critical = Outgoing(5, world, second_generation, -10);
    negative_critical.attacker_name = critical.attacker_name;
    negative_critical.flags = nte::combat_stats::kDamageFlagCritical;
    engine.AcceptDamage(negative_critical, start + 4s);

    const auto snapshot = engine.BuildSnapshot(start + 4s);
    Check(snapshot.critical_data_status ==
              nte::combat_stats::CriticalDataStatus::Available,
        "complete unified damage events should make critical metadata available");
    Check(snapshot.damage_flag_histogram.size() == 2,
        "encounter snapshots retain the raw damage flag distribution");
    Check(snapshot.players.size() == 1, "rotated character should remain one row");
    if (!snapshot.players.empty()) {
        const auto& row = snapshot.players.front();
        Check(row.positive_hits == 3 && row.critical_hits == 1,
            "only positive outgoing hits should enter the critical denominator");
        Check(std::abs(nte::combat_stats::PlayerCriticalRate(row) - 100.0 / 3.0) < 0.001,
            "one critical among three positive hits should be 33.33 percent");
    }
}

void UnifiedDamageCriticalMetadataUsesPartialFlag() {
    CombatStatsEngine engine;
    const TimePoint start{350s};
    const Handle world{62, 1};
    const Handle player{63, 1};
    engine.ObserveCombatant(
        Combatant{world, player, 100.0, 100.0, 0.0}, start);
    auto first = Outgoing(1, world, player, 100);
    first.flags = 1U << 4U;
    engine.AcceptDamage(first, start);
    auto second = Outgoing(2, world, player, 80);
    second.flags = 1U << 4U;
    engine.AcceptDamage(second, start + 1s);

    const auto available = engine.BuildSnapshot(start + 1s);
    Check(available.critical_data_status ==
              nte::combat_stats::CriticalDataStatus::Available,
        "complete unified damage events make a zero critical count authoritative");
    Check(available.players.front().critical_hits == 0,
        "available critical metadata retains the zero-percent display value");

    engine.Reset();
    engine.ObserveCombatant(
        Combatant{world, player, 100.0, 100.0, 0.0}, start + 2s);
    first.flags = nte::combat_stats::kCombatFlagPartial |
        nte::combat_stats::kDamageFlagCritical;
    second.flags = nte::combat_stats::kCombatFlagPartial;
    engine.AcceptDamage(first, start + 2s);
    engine.AcceptDamage(second, start + 3s);
    Check(engine.BuildSnapshot(start + 3s).critical_data_status ==
              nte::combat_stats::CriticalDataStatus::Partial,
        "partial unified events must make critical metadata partial");
}

void EncounterAndRollingDps() {
    CombatStatsEngine engine;
    const TimePoint start{400s};
    const Handle world{50, 1};
    const Handle player{51, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 100), start);
    auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.encounter_dps == 100.0, "single hit encounter DPS should use one-second floor");
    Check(snapshot.rolling_dps == 100.0, "single hit rolling DPS should use one-second floor");

    engine.AcceptDamage(Outgoing(2, world, player, 300), start + 1s);
    snapshot = engine.BuildSnapshot(start + 1s);
    Check(snapshot.encounter_dps == 400.0, "encounter DPS should use first-to-last positive hit");
    Check(snapshot.rolling_dps == 400.0, "rolling DPS should use elapsed window before ten seconds");

    engine.Advance(start + 11s);
    snapshot = engine.BuildSnapshot(start + 11s);
    Check(snapshot.rolling_dps == 0.0, "rolling DPS should expire old hits");
    Check(snapshot.duration_seconds == 1.0, "completed encounter duration should stop at last activity");
}

void EncounterDpsUsesRealDamageActivityWindow() {
    CombatStatsEngine engine;
    const TimePoint start{450s};
    const Handle world{55, 1};
    const Handle player{56, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 100), start);
    auto activity = Outgoing(2, world, player, 0);
    engine.AcceptDamage(activity, start + 5s);
    const auto snapshot = engine.BuildSnapshot(start + 5s);
    Check(snapshot.encounter_dps == 100.0,
          "encounter DPS should use trigger-to-last-real-damage duration");
}

void AggregatesAndQualityFlags() {
    CombatStatsEngine engine;
    const TimePoint start{500s};
    const Handle world{60, 1};
    const Handle player{61, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto first = Outgoing(1, world, player, 100);
    first.name_id = 101;
    first.source_name = "SkillA";
    first.victim_name = "EnemyA";
    first.victim_display_name = "敌人甲";
    first.victim = {70, 1};
    engine.AcceptDamage(first, start);

    auto second = first;
    second.sequence = 2;
    second.final_value = 50;
    engine.AcceptDamage(second, start + 1s);

    auto third = Outgoing(3, world, {80, 1}, 30);
    third.victim = player;
    third.name_id = 202;
    third.source_name = "EnemySkill";
    third.attacker_name = "EnemyB";
    engine.AcceptDamage(third, start + 2s);

    engine.MarkPartial(true);
    engine.MarkOverflow();
    const auto snapshot = engine.BuildSnapshot(start + 2s);
    Check(snapshot.outgoing_sources.size() == 1, "same source should aggregate into one row");
    if (!snapshot.outgoing_sources.empty()) {
        Check(snapshot.outgoing_sources[0].damage == 150, "source damage should aggregate");
        Check(snapshot.outgoing_sources[0].positive_hits == 2, "source hits should aggregate");
        Check(snapshot.outgoing_sources[0].maximum_hit == 100, "source maximum should aggregate");
    }
    Check(snapshot.outgoing_targets.size() == 1, "same target should aggregate");
    if (!snapshot.outgoing_targets.empty()) {
        Check(snapshot.outgoing_targets[0].label == "敌人甲",
            "outgoing targets should use the resolved participant display name");
    }
    Check(snapshot.incoming_sources.size() == 1, "incoming attacker/source should aggregate");
    Check(snapshot.quality.partial && snapshot.quality.cursor_gap,
          "partial cursor gap flags should propagate");
    Check(snapshot.quality.overflow, "overflow flag should propagate");
}

void DamageSourceAggregatesUseTechnicalPathIdentity() {
    CombatStatsEngine engine;
    const TimePoint start{550s};
    const Handle world{65, 1};
    const Handle player{66, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto shinku = Outgoing(1, world, player, 100);
    shinku.name_id = 0x8000000000000005ULL;
    shinku.source_name = "GE_Player_Shinku_Melee1_1_Damage";
    shinku.source_display_name = "普通攻击：特种格斗术";
    engine.AcceptDamage(shinku, start);

    auto sagiri = shinku;
    sagiri.sequence = 2;
    sagiri.final_value = 200;
    sagiri.source_name = "GE_Player_Sagiri_Melee1_Damage";
    sagiri.source_display_name = "鬼郎丸挥击";
    engine.AcceptDamage(sagiri, start + 1s);

    auto sagiri_after_reset = sagiri;
    sagiri_after_reset.sequence = 3;
    sagiri_after_reset.name_id = 0x8000000000000042ULL;
    sagiri_after_reset.final_value = 50;
    engine.AcceptDamage(sagiri_after_reset, start + 2s);

    const auto snapshot = engine.BuildSnapshot(start + 2s);
    Check(snapshot.outgoing_sources.size() == 2,
        "reused source ids with different technical paths must remain distinct");
    const auto sagiri_row = std::ranges::find_if(snapshot.outgoing_sources,
        [](const auto& row) { return row.label == "鬼郎丸挥击"; });
    Check(sagiri_row != snapshot.outgoing_sources.end() &&
              sagiri_row->damage == 250 && sagiri_row->positive_hits == 2,
        "the same technical path should aggregate across source id generations");
}

void AggregateLimitUsesOtherBucket() {
    CombatStatsEngine engine;
    const TimePoint start{600s};
    const Handle world{90, 1};
    const Handle player{91, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    for (std::uint64_t index{}; index < 257; ++index) {
        auto event = Outgoing(index + 1, world, player, 1);
        event.name_id = 1000 + index;
        event.source_name = "Source" + std::to_string(index);
        event.victim = {2000 + index, 1};
        event.victim_name = "Target" + std::to_string(index);
        engine.AcceptDamage(event, start);
    }
    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.quality.aggregate_truncated, "aggregate limit should set quality flag");
    Check(snapshot.outgoing_sources.size() == 256, "source rows should stay bounded with Other bucket");
    Check(snapshot.outgoing_targets.size() == 256, "target rows should stay bounded with Other bucket");
    if (!snapshot.outgoing_sources.empty()) {
        Check(snapshot.outgoing_sources.front().label == "Other",
              "overflow source row should be named Other");
        Check(snapshot.outgoing_sources.front().damage == 2,
              "overflow source damage should be retained");
    }
}

void DamageTotalsSaturateOnOverflow() {
    CombatStatsEngine engine;
    const TimePoint start{700s};
    const Handle world{100, 1};
    const Handle player{101, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(
        1, world, player, std::numeric_limits<std::int64_t>::max()), start);
    engine.AcceptDamage(Outgoing(2, world, player, 1), start);
    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.outgoing_damage == std::numeric_limits<std::int64_t>::max(),
          "damage total should saturate instead of wrapping");
    Check(snapshot.quality.overflow, "local total overflow should set quality flag");
}

void CursorDefersResetAndRecoversRetainedWindow() {
    nte::combat_stats::CombatCursor cursor;
    static_cast<void>(cursor.Accept(40));
    cursor.RequestReset();
    const auto unavailable = cursor.BeginFrame(std::nullopt);
    Check(!unavailable.reset_applied && cursor.reset_pending(),
          "reset should remain pending while service is unavailable");
    const auto reset = cursor.BeginFrame(55);
    Check(reset.reset_applied && cursor.value() == 55,
          "available service should reset cursor to latest sequence");
    Check(!cursor.reset_pending(), "applied reset should clear pending state");

    static_cast<void>(cursor.Accept(70));
    const auto rewind = cursor.BeginFrame(10);
    Check(rewind.cursor_gap && cursor.value() == 0,
          "latest sequence rollback should rewind and report a gap");
    static_cast<void>(cursor.Accept(5));
    static_cast<void>(cursor.BeginFrame(20));
    Check(cursor.RecoverFromNotFound(20) && cursor.value() == 0,
          "stale retained-window cursor should recover once per frame");
    Check(!cursor.RecoverFromNotFound(20), "cursor recovery should be bounded to one attempt");
}

void CursorReportsDirectSequenceJumps() {
    nte::combat_stats::CombatCursor cursor;
    static_cast<void>(cursor.Accept(10));
    const auto jump = cursor.AcceptNext(15);
    Check(jump.accepted && jump.cursor_gap && jump.missing_first == 11 &&
              jump.missing_last == 14 && cursor.value() == 15,
        "direct next_event sequence jumps should expose their missing range");
    const auto contiguous = cursor.AcceptNext(16);
    Check(contiguous.accepted && !contiguous.cursor_gap,
        "contiguous events should not report a cursor gap");
}

void RuntimeNameCompletionUpdatesRecentTimelineEntries() {
    CombatStatsEngine engine;
    const TimePoint start{1050s};
    const Handle world{145, 1};
    const Handle player{146, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    auto unresolved = Outgoing(1, world, player, 10);
    unresolved.name_id = 9001;
    unresolved.source_name = "#9001";
    unresolved.source_display_name = unresolved.source_name;
    unresolved.source_name_origin = "technical";
    Check(engine.AcceptEvent(unresolved, start), "unresolved event should be retained");
    auto resolved = unresolved;
    resolved.sequence = 2;
    resolved.source_name = "Skill.Localized";
    resolved.source_display_name = "本地化技能";
    resolved.source_name_origin = "sdk";
    Check(engine.AcceptEvent(resolved, start + 1s), "resolved event should be retained");
    const auto events = engine.BuildSnapshot(start + 1s).recent_events;
    Check(events.size() == 2 && events.front().source_name == "Skill.Localized" &&
              events.front().source_display_name == "本地化技能",
        "later SDK event names should complete earlier timeline placeholders");
}

void CursorGapQualityAppliesToReplacementEncounter() {
    CombatStatsEngine engine;
    nte::combat_stats::CombatCursor cursor;
    const TimePoint start{800s};
    const Handle world{110, 1};
    const Handle player{111, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 10), start);
    engine.Advance(start + 10s);
    static_cast<void>(cursor.Accept(50));
    const auto decision = cursor.BeginFrame(5);
    engine.AcceptDamage(Outgoing(2, world, player, 20), start + 11s);
    if (decision.cursor_gap) engine.MarkPartial(true);
    const auto snapshot = engine.BuildSnapshot(start + 11s);
    Check(snapshot.outgoing_damage == 20, "replacement encounter should start clean");
    Check(snapshot.quality.partial && snapshot.quality.cursor_gap,
          "cursor gap should apply after replacement encounter is created");
}

void CursorGapWaitsForLaterReplacementEncounter() {
    CombatStatsEngine engine;
    const TimePoint start{900s};
    const Handle world{120, 1};
    const Handle player{121, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 10), start);
    engine.Advance(start + 10s);
    engine.MarkPartial(true);
    auto snapshot = engine.BuildSnapshot(start + 10s);
    Check(snapshot.quality.cursor_gap, "completed encounter should show detected cursor gap");

    engine.Advance(start + 11s);
    engine.AcceptDamage(Outgoing(2, world, player, 20), start + 12s);
    snapshot = engine.BuildSnapshot(start + 12s);
    Check(snapshot.outgoing_damage == 20, "later replacement encounter should start clean");
    Check(snapshot.quality.partial && snapshot.quality.cursor_gap,
          "pending cursor gap should survive until a later relevant event");
}

void WorldChangeDropsPendingQuality() {
    CombatStatsEngine engine;
    const TimePoint start{1000s};
    const Handle first_world{130, 1};
    const Handle next_world{140, 1};
    const Handle player{131, 1};
    engine.ObserveCombatant(Combatant{first_world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, first_world, player, 10), start);
    engine.Advance(start + 10s);
    engine.MarkPartial(true);

    engine.ObserveCombatant(Combatant{next_world, player, 100.0, 100.0, 0.0}, start + 11s);
    engine.AcceptDamage(Outgoing(2, next_world, player, 20), start + 12s);
    const auto snapshot = engine.BuildSnapshot(start + 12s);
    Check(!snapshot.quality.partial && !snapshot.quality.cursor_gap,
          "world change should discard pending quality from the previous world");
}

void HealingAndBuffEventsDoNotPolluteDamageMetrics() {
    CombatStatsEngine engine;
    const TimePoint start{1100s};
    const Handle world{150, 1};
    const Handle player{151, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto damage = Outgoing(1, world, player, 100);
    damage.flags = nte::combat_stats::kDamageFlagCritical;
    Check(engine.AcceptEvent(damage, start), "damage should start the encounter");

    auto heal = Outgoing(2, world, player, 1'000);
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    heal.value = 1'000;
    heal.final_value = 0;
    heal.flags = nte::combat_stats::kCombatFlagDisplayValid;
    Check(engine.AcceptEvent(heal, start + 1s), "positive healing should be accepted");

    auto buff = Outgoing(3, world, player, 0);
    buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    buff.duration_seconds = 12.5F;
    buff.stack_count = 3;
    Check(engine.AcceptEvent(buff, start + 2s), "buff add should be accepted");

    auto received = Outgoing(4, world, {900, 1}, 400);
    received.kind = nte::combat_stats::CombatEventKind::Heal;
    received.victim = player;
    received.value = 400;
    received.final_value = 0;
    Check(engine.AcceptEvent(received, start + 3s), "received healing should be accepted");

    const auto snapshot = engine.BuildSnapshot(start + 3s);
    Check(snapshot.outgoing_damage == 100 && snapshot.maximum_outgoing_hit == 100,
          "healing must not change outgoing damage or maximum hit");
    Check(snapshot.outgoing_healing == 1'000 && snapshot.outgoing_heal_events == 1,
           "healing should use its own amount and event counters");
    Check(snapshot.incoming_healing == 400 && snapshot.incoming_heal_events == 1,
          "received healing should use the SDK value field and its own counters");
    Check(snapshot.buff_add_events == 1 && snapshot.buff_remove_events == 0,
          "buff events should use independent type counters");
    Check(snapshot.damage_flag_histogram.size() == 1 &&
              snapshot.damage_flag_histogram.front().flags ==
                  nte::combat_stats::kDamageFlagCritical,
          "damage flag histograms must exclude healing and buff flags");
    Check(snapshot.players.size() == 1 && snapshot.players.front().damage == 100 &&
              snapshot.players.front().healing_done == 1'000 &&
              snapshot.players.front().healing_received == 400,
           "player damage and healing aggregates should remain independent");
}

}  // namespace


int main() {
    RelevantDamageStartsEncounter();
    NonDamageEventsDoNotStartOrExtendEncounter();
    IncomingDamageStartsAndExtendsEncounter();
    CombatBoundaryPolicyDistinguishesServiceLossFromStaleCombatant();
    ExplicitCompletionClosesTheActiveCaptureGate();
    ActiveEncounterCapturesEveryVisibleDamageField();
    EncounterLifecycleAndCharacterRotation();
    ObservedCharactersRequireEncounterParticipation();
    ConfirmedCharactersMergeGenerationByResolvedPath();
    DistinctParticipatingCharactersProduceDistinctRows();
    SpectatorAndCrossWorldEventsDoNotConfirmCharacters();
    ActiveEncounterCapturesOnlyPlayersAndEngagedActors();
    BothControlledParticipantsStoreOneVisibleEvent();
    DirectionAndNonPositiveRules();
    CriticalRateCountsOnlyPositiveOutgoingHits();
    UnifiedDamageCriticalMetadataUsesPartialFlag();
    EncounterAndRollingDps();
    EncounterDpsUsesRealDamageActivityWindow();
    AggregatesAndQualityFlags();
    DamageSourceAggregatesUseTechnicalPathIdentity();
    AggregateLimitUsesOtherBucket();
    DamageTotalsSaturateOnOverflow();
    CursorDefersResetAndRecoversRetainedWindow();
    CursorReportsDirectSequenceJumps();
    RuntimeNameCompletionUpdatesRecentTimelineEntries();
    CursorGapQualityAppliesToReplacementEncounter();
    CursorGapWaitsForLaterReplacementEncounter();
    WorldChangeDropsPendingQuality();
    HealingAndBuffEventsDoNotPolluteDamageMetrics();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All combat stats tests passed\n";
    return 0;
}
