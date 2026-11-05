// Offline engine fixtures: combat aggregation, the damage log repository,
// developer diagnostics, account identity, and the attribute color table.
//
// One executable per dependency group; the group entry point runs every case so
// a single run reports all broken assertions instead of stopping at the first.
#include "test_support.hpp"

#include "account_identity.hpp"
#include "attribute_color_map.hpp"
#include "character_identity.hpp"
#include "combat_boundary_policy.hpp"
#include "combat_cursor.hpp"
#include "combat_stats_engine.hpp"
#include "damage_log.hpp"
#include "developer_diagnostics.hpp"
#include "log_worker.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using nte::combat_stats::CombatBoundaryAction;
using nte::combat_stats::Combatant;
using nte::combat_stats::CombatStatsEngine;
using nte::combat_stats::DamageEvent;
using nte::combat_stats::EncounterState;
using nte::combat_stats::Handle;
using nte::combat_stats::TimePoint;
using nte::damage_log::DamageLogRepository;
using nte::damage_log::EncounterClose;
using nte::damage_log::EncounterOpen;
using nte::damage_log::LoggedCombatEvent;
using nte::damage_log::ReportClose;
using nte::damage_log::ReportOpen;
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

    NTE_CHECK(engine.AcceptDamage(event, start), "player event should be accepted");
    const auto snapshot = engine.BuildSnapshot(start);
    NTE_CHECK(snapshot.state == EncounterState::Active, "first player event should start encounter");
    NTE_CHECK(snapshot.outgoing_damage == 125, "outgoing damage should be accumulated");
    NTE_CHECK(snapshot.outgoing_positive_hits == 1, "positive outgoing hit should be counted");
}

void NonDamageEventsDoNotStartOrExtendEncounter() {
    CombatStatsEngine engine;
    const TimePoint start{125s};
    const Handle world{3, 1};
    const Handle player{4, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto heal = Outgoing(1, world, player, 100);
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    NTE_CHECK(!engine.AcceptEvent(heal, start), "healing must not start an encounter");
    auto buff = Outgoing(2, world, player, 0);
    buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    NTE_CHECK(!engine.AcceptEvent(buff, start + 1s), "BUFF must not start an encounter");
    NTE_CHECK(engine.BuildSnapshot(start + 1s).state == EncounterState::Idle,
        "non-damage events must leave an idle engine idle");

    NTE_CHECK(engine.AcceptDamage(Outgoing(3, world, player, 50), start + 2s),
        "positive damage should start the encounter");
    auto late_heal = Outgoing(4, world, player, 100);
    late_heal.kind = nte::combat_stats::CombatEventKind::Heal;
    NTE_CHECK(engine.AcceptEvent(late_heal, start + 11s),
        "healing in an active encounter should remain visible");
    engine.Advance(start + 12s);
    NTE_CHECK(engine.BuildSnapshot(start + 12s).state == EncounterState::Completed,
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
    NTE_CHECK(engine.AcceptDamage(incoming, start),
        "incoming positive damage should start the encounter");
    auto snapshot = engine.BuildSnapshot(start);
    NTE_CHECK(snapshot.state == EncounterState::Active,
        "first incoming positive damage should activate combat");
    NTE_CHECK(snapshot.incoming_damage == 80 && snapshot.incoming_positive_hits == 1,
        "incoming positive damage should aggregate on the controlled victim");

    auto boundary_hit = incoming;
    boundary_hit.sequence = 2;
    boundary_hit.final_value = 30;
    NTE_CHECK(engine.AcceptDamage(boundary_hit, start + 10s),
        "a fresh incoming hit at the inactivity boundary should be accepted");
    engine.Advance(start + 10s);
    snapshot = engine.BuildSnapshot(start + 10s);
    NTE_CHECK(snapshot.state == EncounterState::Active,
        "accepting incoming damage before Advance should keep the encounter active");
    NTE_CHECK(snapshot.incoming_damage == 110,
        "incoming damage should accumulate across the extended encounter");

    engine.Advance(start + 20s);
    snapshot = engine.BuildSnapshot(start + 20s);
    NTE_CHECK(snapshot.state == EncounterState::Completed,
        "encounter should complete only after 10 seconds without damage");
}

void CombatBoundaryPolicyDistinguishesServiceLossFromStaleCombatant() {
    NTE_CHECK(PlanCombatBoundaryAction(EncounterState::Active, true, true) ==
              CombatBoundaryAction::AdvanceForInactivity,
        "valid combat methods with an open recording encounter should only advance for inactivity");
    NTE_CHECK(PlanCombatBoundaryAction(EncounterState::Active, true, false) ==
              CombatBoundaryAction::CompleteForServiceLoss,
        "missing combat methods while recording should force service-loss close");
    NTE_CHECK(PlanCombatBoundaryAction(EncounterState::Idle, false, true) ==
              CombatBoundaryAction::None,
        "idle non-recording state should not advance or close");
}

void ExplicitCompletionClosesTheActiveCaptureGate() {
    CombatStatsEngine engine;
    const TimePoint start{140s};
    const Handle world{31, 1};
    const Handle player{32, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    NTE_CHECK(engine.AcceptDamage(Outgoing(1, world, player, 25), start),
        "damage should start an encounter before explicit completion");
    engine.CompleteEncounter();
    NTE_CHECK(engine.state() == EncounterState::Completed,
        "explicit lifecycle boundaries should complete the active encounter");
    auto buff = Outgoing(2, world, player, 0);
    buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    NTE_CHECK(!engine.AcceptEvent(buff, start + 1s),
        "completed encounters must reject non-starting events");
}

void ActiveEncounterCapturesEveryVisibleDamageField() {
    CombatStatsEngine engine;
    const TimePoint start{150s};
    const Handle world{5, 1};
    const Handle player{6, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    NTE_CHECK(engine.AcceptDamage(Outgoing(1, world, player, 10), start),
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

    NTE_CHECK(engine.AcceptDamage(visible, start + 1s),
          "an active encounter should capture damage involving an engaged actor");
    const auto snapshot = engine.BuildSnapshot(start + 1s);
    NTE_CHECK(snapshot.outgoing_damage == 10,
          "engaged-actor damage should not enter controlled-character output");
    NTE_CHECK(snapshot.recent_events.size() == 2,
          "visible damage should be retained in the recent event stream");
    if (snapshot.recent_events.size() == 2) {
        const auto& recent = snapshot.recent_events.back();
        NTE_CHECK(recent.name_id == visible.name_id &&
                  recent.attacker == visible.attacker &&
                  recent.victim == visible.victim,
              "recent events should retain stable source and participant identifiers");
        NTE_CHECK(recent.tick_sequence == 77 && recent.flags == 0x0f,
              "sequence metadata should be retained");
        NTE_CHECK(recent.value == 31 && recent.basic_value == 29 &&
                  recent.final_value == 25,
              "all three damage values should be retained");
        NTE_CHECK(recent.hit_location[0] == 1.5 && recent.damage_type == 4 &&
                  recent.reaction_display_type == 7,
              "damage classification and hit location should be retained");
    }

    engine.Advance(start + 10s);
    NTE_CHECK(engine.BuildSnapshot(start + 10s).state == EncounterState::Completed,
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

    NTE_CHECK(!engine.AcceptDamage(Outgoing(1, world, {99, 1}, 50), start),
          "unrelated event should be ignored");
    NTE_CHECK(!engine.AcceptDamage(Outgoing(1, {999, 1}, first, 50), start),
          "event from another world should be ignored");
    NTE_CHECK(engine.AcceptDamage(Outgoing(2, world, first, 100), start),
          "first controlled character should be accepted");
    engine.ObserveCombatant(Combatant{world, second, 75.0, 100.0, 3.0}, start + 1s);
    NTE_CHECK(engine.AcceptDamage(Outgoing(3, world, second, 200), start + 1s),
          "rotated controlled character should join encounter");
    auto snapshot = engine.BuildSnapshot(start + 1s);
    NTE_CHECK(snapshot.controlled_character_count == 2, "character rotation should be merged");
    NTE_CHECK(snapshot.outgoing_damage == 300, "rotated character damage should share total");

    engine.Advance(start + 11s);
    snapshot = engine.BuildSnapshot(start + 11s);
    NTE_CHECK(snapshot.state == EncounterState::Completed, "ten idle seconds should complete encounter");

    NTE_CHECK(engine.AcceptDamage(Outgoing(4, world, first, 25), start + 12s),
          "new event after completion should start next encounter");
    snapshot = engine.BuildSnapshot(start + 12s);
    NTE_CHECK(snapshot.state == EncounterState::Active, "next encounter should be active");
    NTE_CHECK(snapshot.outgoing_damage == 25, "next encounter should start with clean totals");

    const Handle next_world{20, 1};
    engine.ObserveCombatant(Combatant{next_world, first, 100.0, 100.0, 0.0}, start + 13s);
    snapshot = engine.BuildSnapshot(start + 13s);
    NTE_CHECK(snapshot.state == EncounterState::Idle, "world change should reset encounter");
    NTE_CHECK(snapshot.outgoing_damage == 0, "world change should clear totals");
    NTE_CHECK(snapshot.controlled_character_count == 0,
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
    NTE_CHECK(engine.AcceptDamage(hit, start), "participating controlled character should start encounter");

    const auto snapshot = engine.BuildSnapshot(start);
    NTE_CHECK(snapshot.controlled_character_count == 1,
          "observed characters without encounter events must not be counted");
    NTE_CHECK(snapshot.players.size() == 1,
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
    NTE_CHECK(snapshot.controlled_character_count == 1,
          "same resolved character path should merge generation handles");
    NTE_CHECK(snapshot.players.size() == 1, "merged character should have one player row");
    if (snapshot.players.size() == 1) {
        const auto& row = snapshot.players.front();
        NTE_CHECK(row.damage == 400 && row.positive_hits == 2,
              "player row should aggregate positive damage and hits");
        NTE_CHECK(row.critical_hits == 2 && row.head_hits == 1 && row.weak_hits == 1,
              "player row should aggregate NTE hit flags");
        NTE_CHECK(row.maximum_hit == 300 && row.maximum_source_id == 502 &&
                  row.maximum_source_name == "SkillB",
              "player row should retain maximum hit ability");
        NTE_CHECK(row.encounter_dps == 200.0 && row.active_dps == 200.0,
              "player row should expose encounter and active DPS");
        NTE_CHECK(row.damage_share == 100.0,
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
    NTE_CHECK(snapshot.players.size() == 2 && snapshot.controlled_character_count == 2,
          "two distinct controlled characters that deal damage should produce two rows");
    NTE_CHECK(snapshot.players[0].damage + snapshot.players[1].damage ==
              snapshot.outgoing_damage,
          "confirmed character damage rows should reconcile to encounter output");
    NTE_CHECK(snapshot.players[0].encounter_dps + snapshot.players[1].encounter_dps ==
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
    NTE_CHECK(!engine.AcceptDamage(spectator, start),
          "a bystander fight should not start an encounter");
    auto cross_world = Outgoing(2, Handle{99, 1}, player, 100);
    cross_world.attacker_name = "/Game/Player/player_C";
    NTE_CHECK(!engine.AcceptDamage(cross_world, start),
          "a controlled handle event from another world should be ignored");
    const auto snapshot = engine.BuildSnapshot(start);
    NTE_CHECK(snapshot.players.empty() && snapshot.controlled_character_count == 0,
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
    NTE_CHECK(engine.AcceptEvent(opening, start),
        "player damage should open an encounter and engage its opponent");

    auto enemy_buff = Outgoing(2, world, {}, 0);
    enemy_buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    enemy_buff.victim = enemy;
    NTE_CHECK(engine.AcceptEvent(enemy_buff, start + 1s),
        "events involving an engaged actor should remain in the encounter");

    auto unrelated = enemy_buff;
    unrelated.sequence = 3;
    unrelated.victim = bystander;
    NTE_CHECK(!engine.AcceptEvent(unrelated, start + 2s),
        "same-world events unrelated to players or engaged actors must be rejected");

    engine.Advance(start + 10s);
    auto replacement = Outgoing(4, world, player, 50);
    replacement.victim = bystander;
    NTE_CHECK(engine.AcceptEvent(replacement, start + 11s),
        "new player damage should open a replacement encounter");
    enemy_buff.sequence = 5;
    NTE_CHECK(!engine.AcceptEvent(enemy_buff, start + 12s),
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
    NTE_CHECK(snapshot.recent_events.size() == 1,
          "an event whose two participants are controlled should be stored once");
    NTE_CHECK(snapshot.outgoing_damage == 100 && snapshot.incoming_damage == 100,
          "a both-controlled event should contribute once to each direction");
    NTE_CHECK(snapshot.players.size() == 2,
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
    NTE_CHECK(snapshot.outgoing_damage == 30, "only positive outgoing damage should accumulate");
    NTE_CHECK(snapshot.incoming_damage == 110, "incoming should include enemy and controlled-to-controlled damage");
    NTE_CHECK(snapshot.outgoing_positive_hits == 1, "only positive outgoing hit should count");
    NTE_CHECK(snapshot.incoming_positive_hits == 2, "positive incoming hits should count");
    NTE_CHECK(snapshot.non_positive_events == 2, "non-positive events should be counted separately");
    NTE_CHECK(snapshot.recent_events.size() == 4, "each accepted event should produce one log row");
    if (snapshot.recent_events.size() > 1) {
        NTE_CHECK(snapshot.recent_events[1].direction == nte::combat_stats::EventDirection::Both,
              "controlled-to-controlled event should be marked both");
    }
    const auto first_row = std::ranges::find_if(snapshot.players,
        [&](const auto& row) { return row.handle == first; });
    const auto second_row = std::ranges::find_if(snapshot.players,
        [&](const auto& row) { return row.handle == second; });
    NTE_CHECK(first_row != snapshot.players.end() && first_row->incoming_damage == 80 &&
              first_row->incoming_positive_hits == 1 &&
              first_row->maximum_incoming_hit == 80,
          "incoming damage should aggregate on the controlled victim row");
    NTE_CHECK(second_row != snapshot.players.end() && second_row->incoming_damage == 30 &&
              second_row->incoming_positive_hits == 1,
          "controlled-to-controlled damage should aggregate on the victim only");
    NTE_CHECK(first_row != snapshot.players.end() && first_row->incoming_share > 72.7 &&
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
    NTE_CHECK(snapshot.critical_data_status ==
              nte::combat_stats::CriticalDataStatus::Available,
        "complete unified damage events should make critical metadata available");
    NTE_CHECK(snapshot.damage_flag_histogram.size() == 2,
        "encounter snapshots retain the raw damage flag distribution");
    NTE_CHECK(snapshot.players.size() == 1, "rotated character should remain one row");
    if (!snapshot.players.empty()) {
        const auto& row = snapshot.players.front();
        NTE_CHECK(row.positive_hits == 3 && row.critical_hits == 1,
            "only positive outgoing hits should enter the critical denominator");
        NTE_CHECK(std::abs(nte::combat_stats::PlayerCriticalRate(row) - 100.0 / 3.0) < 0.001,
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
    NTE_CHECK(available.critical_data_status ==
              nte::combat_stats::CriticalDataStatus::Available,
        "complete unified damage events make a zero critical count authoritative");
    NTE_CHECK(available.players.front().critical_hits == 0,
        "available critical metadata retains the zero-percent display value");

    engine.Reset();
    engine.ObserveCombatant(
        Combatant{world, player, 100.0, 100.0, 0.0}, start + 2s);
    first.flags = nte::combat_stats::kCombatFlagPartial |
        nte::combat_stats::kDamageFlagCritical;
    second.flags = nte::combat_stats::kCombatFlagPartial;
    engine.AcceptDamage(first, start + 2s);
    engine.AcceptDamage(second, start + 3s);
    NTE_CHECK(engine.BuildSnapshot(start + 3s).critical_data_status ==
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
    NTE_CHECK(snapshot.encounter_dps == 100.0, "single hit encounter DPS should use one-second floor");
    NTE_CHECK(snapshot.rolling_dps == 100.0, "single hit rolling DPS should use one-second floor");

    engine.AcceptDamage(Outgoing(2, world, player, 300), start + 1s);
    snapshot = engine.BuildSnapshot(start + 1s);
    NTE_CHECK(snapshot.encounter_dps == 400.0, "encounter DPS should use first-to-last positive hit");
    NTE_CHECK(snapshot.rolling_dps == 400.0, "rolling DPS should use elapsed window before ten seconds");

    engine.Advance(start + 11s);
    snapshot = engine.BuildSnapshot(start + 11s);
    NTE_CHECK(snapshot.rolling_dps == 0.0, "rolling DPS should expire old hits");
    NTE_CHECK(snapshot.duration_seconds == 1.0, "completed encounter duration should stop at last activity");
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
    NTE_CHECK(snapshot.encounter_dps == 100.0,
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
    NTE_CHECK(snapshot.outgoing_sources.size() == 1, "same source should aggregate into one row");
    if (!snapshot.outgoing_sources.empty()) {
        NTE_CHECK(snapshot.outgoing_sources[0].damage == 150, "source damage should aggregate");
        NTE_CHECK(snapshot.outgoing_sources[0].positive_hits == 2, "source hits should aggregate");
        NTE_CHECK(snapshot.outgoing_sources[0].maximum_hit == 100, "source maximum should aggregate");
    }
    NTE_CHECK(snapshot.outgoing_targets.size() == 1, "same target should aggregate");
    if (!snapshot.outgoing_targets.empty()) {
        NTE_CHECK(snapshot.outgoing_targets[0].label == "敌人甲",
            "outgoing targets should use the resolved participant display name");
    }
    NTE_CHECK(snapshot.incoming_sources.size() == 1, "incoming attacker/source should aggregate");
    NTE_CHECK(snapshot.quality.partial && snapshot.quality.cursor_gap,
          "partial cursor gap flags should propagate");
    NTE_CHECK(snapshot.quality.overflow, "overflow flag should propagate");
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
    NTE_CHECK(snapshot.outgoing_sources.size() == 2,
        "reused source ids with different technical paths must remain distinct");
    const auto sagiri_row = std::ranges::find_if(snapshot.outgoing_sources,
        [](const auto& row) { return row.label == "鬼郎丸挥击"; });
    NTE_CHECK(sagiri_row != snapshot.outgoing_sources.end() &&
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
    NTE_CHECK(snapshot.quality.aggregate_truncated, "aggregate limit should set quality flag");
    NTE_CHECK(snapshot.outgoing_sources.size() == 256, "source rows should stay bounded with Other bucket");
    NTE_CHECK(snapshot.outgoing_targets.size() == 256, "target rows should stay bounded with Other bucket");
    if (!snapshot.outgoing_sources.empty()) {
        NTE_CHECK(snapshot.outgoing_sources.front().label == "Other",
              "overflow source row should be named Other");
        NTE_CHECK(snapshot.outgoing_sources.front().damage == 2,
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
    NTE_CHECK(snapshot.outgoing_damage == std::numeric_limits<std::int64_t>::max(),
          "damage total should saturate instead of wrapping");
    NTE_CHECK(snapshot.quality.overflow, "local total overflow should set quality flag");
}

void CursorDefersResetAndRecoversRetainedWindow() {
    nte::combat_stats::CombatCursor cursor;
    static_cast<void>(cursor.Accept(40));
    cursor.RequestReset();
    const auto unavailable = cursor.BeginFrame(std::nullopt);
    NTE_CHECK(!unavailable.reset_applied && cursor.reset_pending(),
          "reset should remain pending while service is unavailable");
    const auto reset = cursor.BeginFrame(55);
    NTE_CHECK(reset.reset_applied && cursor.value() == 55,
          "available service should reset cursor to latest sequence");
    NTE_CHECK(!cursor.reset_pending(), "applied reset should clear pending state");

    static_cast<void>(cursor.Accept(70));
    const auto rewind = cursor.BeginFrame(10);
    NTE_CHECK(rewind.cursor_gap && cursor.value() == 0,
          "latest sequence rollback should rewind and report a gap");
    static_cast<void>(cursor.Accept(5));
    static_cast<void>(cursor.BeginFrame(20));
    NTE_CHECK(cursor.RecoverFromNotFound(20) && cursor.value() == 0,
          "stale retained-window cursor should recover once per frame");
    NTE_CHECK(!cursor.RecoverFromNotFound(20), "cursor recovery should be bounded to one attempt");
}

void CursorReportsDirectSequenceJumps() {
    nte::combat_stats::CombatCursor cursor;
    static_cast<void>(cursor.Accept(10));
    const auto jump = cursor.AcceptNext(15);
    NTE_CHECK(jump.accepted && jump.cursor_gap && jump.missing_first == 11 &&
              jump.missing_last == 14 && cursor.value() == 15,
        "direct next_event sequence jumps should expose their missing range");
    const auto contiguous = cursor.AcceptNext(16);
    NTE_CHECK(contiguous.accepted && !contiguous.cursor_gap,
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
    NTE_CHECK(engine.AcceptEvent(unresolved, start), "unresolved event should be retained");
    auto resolved = unresolved;
    resolved.sequence = 2;
    resolved.source_name = "Skill.Localized";
    resolved.source_display_name = "本地化技能";
    resolved.source_name_origin = "sdk";
    NTE_CHECK(engine.AcceptEvent(resolved, start + 1s), "resolved event should be retained");
    const auto events = engine.BuildSnapshot(start + 1s).recent_events;
    NTE_CHECK(events.size() == 2 && events.front().source_name == "Skill.Localized" &&
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
    NTE_CHECK(snapshot.outgoing_damage == 20, "replacement encounter should start clean");
    NTE_CHECK(snapshot.quality.partial && snapshot.quality.cursor_gap,
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
    NTE_CHECK(snapshot.quality.cursor_gap, "completed encounter should show detected cursor gap");

    engine.Advance(start + 11s);
    engine.AcceptDamage(Outgoing(2, world, player, 20), start + 12s);
    snapshot = engine.BuildSnapshot(start + 12s);
    NTE_CHECK(snapshot.outgoing_damage == 20, "later replacement encounter should start clean");
    NTE_CHECK(snapshot.quality.partial && snapshot.quality.cursor_gap,
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
    NTE_CHECK(!snapshot.quality.partial && !snapshot.quality.cursor_gap,
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
    NTE_CHECK(engine.AcceptEvent(damage, start), "damage should start the encounter");

    auto heal = Outgoing(2, world, player, 1'000);
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    heal.value = 1'000;
    heal.final_value = 0;
    heal.flags = nte::combat_stats::kCombatFlagDisplayValid;
    NTE_CHECK(engine.AcceptEvent(heal, start + 1s), "positive healing should be accepted");

    auto buff = Outgoing(3, world, player, 0);
    buff.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    buff.duration_seconds = 12.5F;
    buff.stack_count = 3;
    NTE_CHECK(engine.AcceptEvent(buff, start + 2s), "buff add should be accepted");

    auto received = Outgoing(4, world, {900, 1}, 400);
    received.kind = nte::combat_stats::CombatEventKind::Heal;
    received.victim = player;
    received.value = 400;
    received.final_value = 0;
    NTE_CHECK(engine.AcceptEvent(received, start + 3s), "received healing should be accepted");

    const auto snapshot = engine.BuildSnapshot(start + 3s);
    NTE_CHECK(snapshot.outgoing_damage == 100 && snapshot.maximum_outgoing_hit == 100,
          "healing must not change outgoing damage or maximum hit");
    NTE_CHECK(snapshot.outgoing_healing == 1'000 && snapshot.outgoing_heal_events == 1,
           "healing should use its own amount and event counters");
    NTE_CHECK(snapshot.incoming_healing == 400 && snapshot.incoming_heal_events == 1,
          "received healing should use the SDK value field and its own counters");
    NTE_CHECK(snapshot.buff_add_events == 1 && snapshot.buff_remove_events == 0,
          "buff events should use independent type counters");
    NTE_CHECK(snapshot.damage_flag_histogram.size() == 1 &&
              snapshot.damage_flag_histogram.front().flags ==
                  nte::combat_stats::kDamageFlagCritical,
          "damage flag histograms must exclude healing and buff flags");
    NTE_CHECK(snapshot.players.size() == 1 && snapshot.players.front().damage == 100 &&
              snapshot.players.front().healing_done == 1'000 &&
              snapshot.players.front().healing_received == 400,
           "player damage and healing aggregates should remain independent");
}

std::filesystem::path UnknownReports(const std::filesystem::path& root) {
    return root / "accounts" / "_unknown" / "reports";
}

std::string ReadAll(const std::filesystem::path& path);

void AccountsUseIndependentPhysicalRepositories() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-account-log-test-" + std::to_string(nonce));
    const auto account_a = *nte::account::FromUidText("10001");
    const auto account_b = *nte::account::FromUidText("20002");
    const std::string report_a = "10000000-0000-4000-8000-000000000001";
    const std::string report_b = "20000000-0000-4000-8000-000000000002";
    std::filesystem::create_directories(root / "reports" / report_a);
    {
        DamageLogRepository repository(root);
        std::string error;
        NTE_CHECK(repository.Recover(error), "multi-account repository should recover");
        NTE_CHECK(repository.BeginReport(ReportOpen{
            report_a, {1, 1}, 1, "build", "plugin", "runtime", account_a}, error),
            "first account report should open");
        NTE_CHECK(repository.CloseReport(ReportClose{"done", 2, false}, error),
            "first account report should close");
        NTE_CHECK(repository.BeginReport(ReportOpen{
            report_b, {2, 1}, 3, "build", "plugin", "runtime", account_b}, error),
            "second account report should open");
        NTE_CHECK(repository.CloseReport(ReportClose{"done", 4, false}, error),
            "second account report should close");
        const auto history = repository.History();
        NTE_CHECK(history.size() == 2 && history[0].account == account_b &&
                history[1].account == account_a,
            "history should retain immutable account ownership");
    }
    NTE_CHECK(std::filesystem::is_regular_file(root / "accounts" / "10001" /
              "reports" / report_a / "report.json") &&
            std::filesystem::is_regular_file(root / "accounts" / "20002" /
              "reports" / report_b / "report.json") &&
            !std::filesystem::is_regular_file(root / "reports" / report_a /
              "report.json"),
        "reports should be physically isolated without adopting legacy roots");
    const auto account_catalog = ReadAll(root / "accounts" / "10001" / "catalog.json");
    const auto account_report = ReadAll(root / "accounts" / "10001" /
        "reports" / report_a / "report.json");
    NTE_CHECK(account_catalog.find("\"uid\":\"10001\"") != std::string::npos &&
            account_report.find("\"source\":\"ui-role-id\"") != std::string::npos,
        "catalog and report metadata should record immutable account identity");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

void ClosedEncounterProducesRecoverableJsonlReport() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-log-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const std::string report_id = "11111111-2222-4333-8444-555555555555";
    const std::string encounter_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

    {
        DamageLogRepository repository(root);
        std::string error;
        NTE_CHECK(repository.Recover(error), "empty repository should recover");
        NTE_CHECK(repository.BeginReport(ReportOpen{
                  report_id, Handle{9, 2}, 1'700'000'000'000, "nte-build-test"}, error),
              "report should open");
        std::string active_delete_error;
        NTE_CHECK(!repository.DeleteReport(
                  nte::account::Unknown(), report_id, active_delete_error),
              "an active report must not be deleted");
        NTE_CHECK(repository.BeginEncounter(EncounterOpen{
                  encounter_id, 1'700'000'000'100, 100'000}, error),
              "encounter should open");

        DamageEvent event;
        event.sequence = 18'446'744'073'709'551'000ULL;
        event.tick_sequence = 91;
        event.flags = 0x0f;
        event.world = {9, 2};
        event.attacker = {10, 3};
        event.victim = {11, 4};
        event.name_id = 18'446'744'073'709'550'999ULL;
        event.value = 102;
        event.basic_value = 101;
        event.final_value = 100;
        event.hit_location = {1.25, 2.5, 3.75};
        event.damage_type = 4;
        event.display_type = 5;
        event.reaction_type = 6;
        event.reaction_display_type = 7;
        event.source_name = "Skill \"A\"";
        event.source_display_name = "技能甲";
        event.source_name_origin = "resource";
        event.name_resource_version = "2026.08.28";
        event.attacker_name = "Actor/A";
        event.attacker_display_name = "角色甲";
        event.attacker_name_origin = "resource";
        event.victim_name = "Actor/B";
        event.victim_display_name = "目标乙";
        event.victim_name_origin = "resource";
        NTE_CHECK(repository.Append(LoggedCombatEvent{
                  event, 1'700'000'000'200, 200'000, true}, error),
              "damage should append");
        auto heal = event;
        heal.kind = nte::combat_stats::CombatEventKind::Heal;
        heal.sequence += 1;
        heal.name_id = 7001;
        heal.final_value = 250;
        NTE_CHECK(repository.Append(LoggedCombatEvent{
                  heal, 1'700'000'000'300, 300'000, true}, error),
              "heal should append");
        auto buff_add = event;
        buff_add.kind = nte::combat_stats::CombatEventKind::BuffAdd;
        buff_add.sequence += 2;
        buff_add.name_id = 7002;
        buff_add.final_value = 0;
        buff_add.duration_seconds = 12.5F;
        buff_add.stack_count = 3;
        NTE_CHECK(repository.Append(LoggedCombatEvent{
                  buff_add, 1'700'000'000'400, 400'000, true}, error),
              "buff add should append");
        auto buff_remove = buff_add;
        buff_remove.kind = nte::combat_stats::CombatEventKind::BuffRemove;
        buff_remove.sequence += 1;
        buff_remove.stack_count = 2;
        NTE_CHECK(repository.Append(LoggedCombatEvent{
                  buff_remove, 1'700'000'000'500, 500'000, true}, error),
              "buff remove should append");
        EncounterClose close{encounter_id, "inactivity",
            1'700'000'010'100, 10'100'000, 100, 0, 10.0, 10.0, {}};
        close.critical_data_status =
            nte::combat_stats::CriticalDataStatus::Unavailable;
        close.damage_flag_histogram = {{16, 1}};
        close.outgoing_healing = 250;
        close.outgoing_heal_events = 1;
        close.encounter_hps = 25.0;
        close.encounter_received_hps = 5.0;
        close.service_unavailable_intervals.emplace_back(3'000'000, 4'000'000);
        close.missing_sequence_ranges.emplace_back(92, 94);
        close.open_missing_sequence_start = 95;
        close.service_intervals_truncated = true;
        nte::combat_stats::PlayerRow player;
        player.handle = event.attacker;
        player.object_path = event.attacker_name;
        player.label = "Actor/A";
        player.display_name = "角色甲";
        player.name_origin = "resource";
        player.name_resource_version = "2026.08.28";
        player.damage = 100;
        player.positive_hits = 1;
        player.critical_hits = 1;
        player.maximum_hit = 100;
        player.maximum_source_id = event.name_id;
        player.maximum_source_name = event.source_name;
        player.maximum_source_display_name = event.source_display_name;
        player.damage_share = 100.0;
        player.encounter_dps = 10.0;
        player.active_dps = 100.0;
        player.rolling_dps = 10.0;
        close.confirmed_controlled_players.push_back(player);
        NTE_CHECK(repository.EndEncounter(close, error),
              "encounter should close");
        NTE_CHECK(repository.CloseReport(ReportClose{
                  "plugin_stop", 1'700'000'010'200, false}, error),
              "report should close");
    }

    const auto report_path = UnknownReports(root) / report_id;
    NTE_CHECK(std::filesystem::exists(
              root / "accounts" / "_unknown" / "catalog.json"),
        "account catalog should exist");
    NTE_CHECK(std::filesystem::exists(report_path / "report.json"), "report metadata should exist");
    NTE_CHECK(std::filesystem::exists(report_path / "actors.jsonl"), "actor dictionary should exist");
    NTE_CHECK(std::filesystem::exists(report_path / "abilities.jsonl"), "ability dictionary should exist");
    NTE_CHECK(std::filesystem::exists(report_path / "encounters.jsonl"), "encounter summary should exist");
    NTE_CHECK(std::filesystem::exists(report_path / "events-000001.jsonl"), "event segment should exist");
    NTE_CHECK(std::filesystem::exists(report_path / "index.json"), "segment index should exist");
    const std::string report_index = ReadAll(report_path / "index.json");
    NTE_CHECK(report_index.find("\"actors_sha256\":\"") != std::string::npos &&
              report_index.find("\"abilities_sha256\":\"") != std::string::npos &&
              report_index.find("\"encounters_sha256\":\"") != std::string::npos,
          "report content hash should cover dictionaries and encounter summaries");
    const std::string report = ReadAll(report_path / "report.json");
    NTE_CHECK(report.find("\"schema_version\":3") != std::string::npos &&
              report.find("\"encounter_id\":\"" + encounter_id + "\"") !=
                  std::string::npos &&
              report.find("\"event_scope\":\"player_and_engaged_actors\"") !=
                  std::string::npos &&
              report.find("\"boundary_policy\":\"player_damage_inactivity_10s\"") !=
                  std::string::npos &&
              report.find("\"healing_capture_status\":\"unavailable\"") !=
                  std::string::npos,
          "schema v3 report should describe its single encounter and capture policy");

    const std::string events = ReadAll(report_path / "events-000001.jsonl");
    NTE_CHECK(events.find("\"sequence\":\"18446744073709551000\"") != std::string::npos,
          "64-bit sequence should be encoded as a decimal string");
    NTE_CHECK(events.find("\"name_id\":\"18446744073709550999\"") != std::string::npos,
          "64-bit name id should be encoded as a decimal string");
    NTE_CHECK(events.find("\"event_name\":\"Skill \\\"A\\\"\"") != std::string::npos,
          "JSON strings should be escaped");
    NTE_CHECK(events.find("\"event_name_origin\":\"resource\"") != std::string::npos &&
              events.find("\"source_name_origin\":\"resource\"") != std::string::npos &&
              events.find("\"target_name_origin\":\"resource\"") != std::string::npos &&
              events.find("\"name_resource_version\":\"2026.08.28\"") !=
                  std::string::npos &&
              events.find("name_catalog_version") == std::string::npos,
          "events should retain display-name origins for historical priority");
    NTE_CHECK(!events.empty() && events.back() == '\n', "JSONL should end with a newline");
    NTE_CHECK(events.find("\"event_type\":\"heal\"") != std::string::npos &&
              events.find("\"event_type\":\"buff_add\"") != std::string::npos &&
              events.find("\"event_type\":\"buff_remove\"") != std::string::npos &&
              events.find("\"duration_seconds\":12.5") != std::string::npos &&
              events.find("\"stack_count\":3") != std::string::npos,
          "all unified event kinds and buff snapshots should round-trip through JSONL");
    const std::string encounters = ReadAll(report_path / "encounters.jsonl");
    NTE_CHECK(encounters.find("\"actor_outgoing\"") != std::string::npos &&
              encounters.find("\"ability_stats\"") != std::string::npos &&
              encounters.find("\"actor_ability\"") != std::string::npos &&
              encounters.find("\"actor_target\"") != std::string::npos &&
              encounters.find("\"ability_target\"") != std::string::npos,
          "encounter summary should contain all participant aggregate dimensions");
    NTE_CHECK(encounters.find("\"critical_hits\":\"1\"") != std::string::npos,
          "encounter aggregates should retain critical hit counts");
    NTE_CHECK(encounters.find("\"schema_version\":3") != std::string::npos &&
              encounters.find("\"outgoing_healing\":\"250\"") != std::string::npos &&
              encounters.find("\"outgoing_heal_events\":\"1\"") != std::string::npos &&
              encounters.find("\"event_type_counts\":{\"damage\":\"1\",\"heal\":\"1\",\"buff_add\":\"1\",\"buff_remove\":\"1\"}") != std::string::npos,
          "schema v3 encounter summaries should persist healing and event type counts");
    NTE_CHECK(events.find("\"schema_version\":3") != std::string::npos,
          "schema v3 event records should be emitted");
    NTE_CHECK(encounters.find("\"critical_data_status\":\"unavailable\"") !=
              std::string::npos &&
          encounters.find("\"damage_flag_histogram\":[{\"flags\":16,\"count\":\"1\"}]") !=
              std::string::npos,
          "encounter metadata should explain unavailable critical data with raw flags");
    NTE_CHECK(encounters.find("\"confirmed_controlled_players\":[{\"actor_id\":1") !=
              std::string::npos &&
          encounters.find("\"maximum_ability_id\":1") != std::string::npos &&
          encounters.find("\"damage_share\":100") != std::string::npos,
          "encounter summary should persist report-local controlled player and ability ids");
    NTE_CHECK(encounters.find("\"service_unavailable_intervals\":[{\"start_offset_us\":\"3000000\"") !=
              std::string::npos && encounters.find("\"complete\":false") != std::string::npos,
          "service gaps should be persisted and make the encounter incomplete");
    NTE_CHECK(encounters.find("\"missing_sequence_ranges\":[{\"first\":\"92\",\"last\":\"94\"}]") !=
              std::string::npos,
          "missing sequence ranges should be persisted exactly");
    NTE_CHECK(encounters.find("\"open_missing_sequence_start\":\"95\"") !=
              std::string::npos &&
          encounters.find("\"service_intervals_truncated\":true") !=
              std::string::npos,
          "open cursor gaps and truncated interval lists should be explicit");
    NTE_CHECK(ReadAll(report_path / "actors.jsonl").find(
              "\"resolution_status\":\"resolved\"") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"resolution_status\":\"resolved\"") != std::string::npos,
          "actor and ability dictionaries should persist resolution state");
    NTE_CHECK(ReadAll(report_path / "actors.jsonl").find(
              "\"display_name\":\"角色甲\"") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"display_name\":\"技能甲\"") != std::string::npos &&
          ReadAll(report_path / "actors.jsonl").find(
              "\"technical_name\":\"Actor/A\"") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"technical_name\":\"Skill \\\"A\\\"\"") != std::string::npos &&
          ReadAll(report_path / "actors.jsonl").find(
              "\"name_resource_version\":\"2026.08.28\"") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"name_resource_version\":\"2026.08.28\"") != std::string::npos,
          "actor and ability dictionaries should persist localized display metadata");
    NTE_CHECK(ReadAll(report_path / "actors.jsonl").find(
              "\"actor_id\":0") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"ability_id\":0") != std::string::npos,
          "dictionary id zero should declare the synthetic Other bucket");

    DamageLogRepository reopened(root);
    std::string recover_error;
    NTE_CHECK(reopened.Recover(recover_error), "closed report should recover");
    const auto history = reopened.History();
    NTE_CHECK(history.size() == 1, "history should contain the report");
    if (!history.empty()) {
        NTE_CHECK(history[0].report_id == report_id && !history[0].open,
              "history should preserve report identity and closed state");
        // 先算成 bool 再断言：宏参数里的 `==` 与花括号初始化会被预处理器当作分隔符。
        const bool world_preserved = history[0].world == Handle{9, 2};
        NTE_CHECK(world_preserved,
              "history should preserve the report World identity");
        NTE_CHECK(history[0].encounter_count == 1 && history[0].event_count == 4,
              "history should expose encounter and event counts");
        NTE_CHECK(history[0].encounters.size() == 1 &&
                  history[0].encounters[0].end_reason == "inactivity" &&
                  history[0].encounters[0].outgoing_damage == 100,
              "recovered history should include encounter summaries");
    }
    {
        std::ofstream marker(root / "keep.txt");
        marker << "keep";
    }
    std::string delete_error;
    NTE_CHECK(!reopened.DeleteReport(
              nte::account::Unknown(), "../outside", delete_error) &&
              std::filesystem::exists(root / "keep.txt"),
          "invalid report ids must not escape the log root");
    delete_error.clear();
    NTE_CHECK(reopened.DeleteReport(
              nte::account::Unknown(), report_id, delete_error) &&
              !std::filesystem::exists(report_path),
          "closed report should be deleted by exact id");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void DelayedEventNameCompletionUpdatesTheLocalAbilityIndex() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-source-reuse-test-" + std::to_string(nonce));
    const std::string report_id = "22222222-3333-4444-8555-666666666666";
    const std::string encounter_id = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
    const Handle world{12, 3};
    DamageLogRepository repository(root);
    std::string error;
    NTE_CHECK(repository.Recover(error), "source reuse fixture repository should initialize");
    NTE_CHECK(repository.BeginReport(ReportOpen{
              report_id, world, 1'700'000'100'000, "nte-build-test"}, error),
        "source reuse fixture report should open");
    NTE_CHECK(repository.BeginEncounter(EncounterOpen{
              encounter_id, 1'700'000'100'100, 100'000}, error),
        "source reuse fixture encounter should open");

    DamageEvent first;
    first.sequence = 1;
    first.world = world;
    first.attacker = {20, 3};
    first.victim = {21, 3};
    first.name_id = 0x8000000000000005ULL;
    first.final_value = 100;
    first.source_name = "#9223372036854775813";
    first.source_display_name = first.source_name;
    first.source_name_origin = "technical";
    first.attacker_name = "Player";
    first.victim_name = "Enemy";
    NTE_CHECK(repository.Append(LoggedCombatEvent{
              first, 1'700'000'100'200, 200'000, true}, error),
        "unresolved event name should append");

    auto second = first;
    second.sequence = 2;
    second.final_value = 200;
    second.source_name = "GE_Player_Shinku_Melee1_1_Damage";
    second.source_display_name = "普通攻击：特种格斗术";
    second.source_name_origin = "sdk";
    NTE_CHECK(repository.Append(LoggedCombatEvent{
              second, 1'700'000'100'300, 300'000, true}, error),
        "later SDK name completion should append");

    NTE_CHECK(repository.EndEncounter(EncounterClose{encounter_id, "manual_end",
              1'700'000'101'000, 1'000'000, 300, 0, 300.0, 1.0, {}}, error),
        "source reuse fixture encounter should close");
    NTE_CHECK(repository.CloseReport(ReportClose{
              "plugin_stop", 1'700'000'101'100, false}, error),
        "source reuse fixture report should close");

    const auto report = UnknownReports(root) / report_id;
    const std::string abilities = ReadAll(report / "abilities.jsonl");
    const std::string events = ReadAll(report / "events-000001.jsonl");
    const std::string encounters = ReadAll(report / "encounters.jsonl");
    NTE_CHECK(abilities.find("\"ability_id\":1") != std::string::npos &&
              abilities.find("\"ability_id\":2") == std::string::npos &&
              abilities.find("GE_Player_Shinku_Melee1_1_Damage") != std::string::npos &&
              abilities.find("\"name_origin\":\"sdk\"") != std::string::npos,
        "the local ability index should be completed in place by the SDK name");
    NTE_CHECK(events.find("\"ability_id\":1") != std::string::npos &&
              events.find("\"ability_id\":2") == std::string::npos,
        "events before and after name completion should retain one local ability id");
    NTE_CHECK(encounters.find(
              "\"ability_stats\":[{\"ability_id\":1,\"damage\":\"300\"") !=
              std::string::npos,
        "name completion should preserve the accumulated ability aggregate");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void WorkerDrainsCommandsBeforeStopping() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-worker-test-" + std::to_string(nonce));
    const std::string report_id = "12345678-1234-4abc-8def-123456789abc";
    const std::string encounter_id = "87654321-4321-4cba-8fed-cba987654321";
    nte::damage_log::DamageLogWorker worker(root);
    NTE_CHECK(worker.Start(), "log worker should start");
    NTE_CHECK(worker.Submit(ReportOpen{report_id, {1, 1}, 1000, "build"}),
          "report command should enqueue");
    NTE_CHECK(worker.Submit(EncounterOpen{encounter_id, 1100, 100}),
          "encounter command should enqueue");
    DamageEvent event;
    event.sequence = 1;
    event.world = {1, 1};
    event.attacker = {2, 1};
    event.victim = {3, 1};
    event.name_id = 4;
    event.final_value = 5;
    NTE_CHECK(worker.Submit(LoggedCombatEvent{event, 1200, 200, true, encounter_id}),
          "event command should enqueue");
    NTE_CHECK(worker.Submit(EncounterClose{
              encounter_id, "inactivity", 2100, 1100, 5, 0, 5.0, 1.0, {}}),
          "encounter close should enqueue");
    event.sequence = 2;
    NTE_CHECK(!worker.Submit(LoggedCombatEvent{
              event, 2200, 1200, true, encounter_id}),
          "worker should close the event gate before processing the close command");
    std::string flush_error;
    NTE_CHECK(worker.FlushAndWait(std::chrono::seconds(2), flush_error),
          "encounter close should be durably flushed");
    NTE_CHECK(std::filesystem::exists(
              UnknownReports(root) / report_id / "encounters.jsonl"),
          "flush barrier should persist the encounter summary before returning");
    const auto event_path = UnknownReports(root) / report_id /
        "events-000001.jsonl";
    const auto closed_events = ReadAll(event_path);
    NTE_CHECK(worker.Submit(ReportClose{"plugin_stop", 2200, false}),
          "report close should enqueue");
    worker.Stop();
    NTE_CHECK(std::filesystem::exists(
              event_path),
          "worker stop should drain queued log commands");
    NTE_CHECK(ReadAll(event_path) == closed_events,
          "event segment must remain immutable after the encounter close barrier");
    const auto status = worker.Status();
    NTE_CHECK(!status.running && status.queue_depth == 0 && status.last_error.empty(),
          "stopped worker should report a clean drained state");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void RecoveryPersistsUncleanShutdownState() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-recovery-test-" + std::to_string(nonce));
    const std::string report_id = "99999999-8888-4777-8666-555555555555";
    {
        DamageLogRepository repository(root);
        std::string error;
        NTE_CHECK(repository.Recover(error), "recovery test repository should initialize");
        NTE_CHECK(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
              "recovery test report should open");
    }
    {
        DamageLogRepository repository(root);
        std::string error;
        NTE_CHECK(repository.Recover(error), "open report should recover as unclean");
        const auto history = repository.History();
        NTE_CHECK(history.size() == 1 && !history[0].open && !history[0].complete &&
                  history[0].close_reason == "unclean_shutdown",
              "recovery should expose unclean shutdown in history");
    }
    const auto report = ReadAll(UnknownReports(root) / report_id / "report.json");
    NTE_CHECK(report.find("\"open\":false") != std::string::npos &&
              report.find("\"close_reason\":\"unclean_shutdown\"") != std::string::npos,
          "unclean state should be persisted to report metadata");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void HundredThousandEventsAreSegmentedWithoutLoss() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-volume-test-" + std::to_string(nonce));
    const std::string report_id = "abcdefab-cdef-4abc-8def-abcdefabcdef";
    const std::string encounter_id = "fedcbafe-dcba-4fed-8cba-fedcbafedcba";
    DamageLogRepository repository(root);
    std::string error;
    NTE_CHECK(repository.Recover(error), "volume repository should initialize");
    NTE_CHECK(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
          "volume report should open");
    NTE_CHECK(repository.BeginEncounter(EncounterOpen{encounter_id, 1000, 0}, error),
          "volume encounter should open");
    DamageEvent event;
    event.world = {1, 1};
    event.attacker = {2, 1};
    event.victim = {3, 1};
    event.name_id = 4;
    event.final_value = 5;
    event.source_name = "Skill";
    event.attacker_name = "Attacker";
    event.victim_name = "Victim";
    bool appended = true;
    for (std::uint64_t sequence = 1; sequence <= 100'000; ++sequence) {
        event.sequence = sequence;
        event.tick_sequence = sequence / 4;
        appended = repository.Append(LoggedCombatEvent{
            event, 1000 + static_cast<std::int64_t>(sequence), sequence * 1000, true}, error);
        if (!appended) break;
    }
    NTE_CHECK(appended, "100,000 healthy events should append without loss");
    NTE_CHECK(repository.EndEncounter(EncounterClose{
              encounter_id, "inactivity", 101000, 100'000'000,
              500'000, 0, 5000.0, 100.0, {}}, error),
          "volume encounter should close");
    NTE_CHECK(repository.CloseReport(ReportClose{"plugin_stop", 101001, false}, error),
          "volume report should close");
    const auto history = repository.History();
    NTE_CHECK(history.size() == 1 && history[0].event_count == 100'000,
          "history should retain the exact event count");
    const auto report_path = UnknownReports(root) / report_id;
    NTE_CHECK(std::filesystem::exists(report_path / "events-000002.jsonl"),
          "large capture should create multiple event segments");
    const std::string index = ReadAll(report_path / "index.json");
    NTE_CHECK(index.find("\"content_sha256\":\"") != std::string::npos &&
              index.find("events-000002.jsonl") != std::string::npos,
          "segment index should contain hashes and multiple segments");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void StoragePathBoundaryKeepsReportsSeparated() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
        ("nte-log-root-switch-test-" + std::to_string(nonce));
    const auto old_root = base / "old";
    const auto new_root = base / "new";
    const std::string old_report = "11111111-2222-4333-8444-555555555555";
    const std::string new_report = "66666666-7777-4888-8999-aaaaaaaaaaaa";
    const std::string old_encounter = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const std::string new_encounter = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";

    const auto write_report = [](const std::filesystem::path& root,
                                  const std::string& report_id,
                                  const std::string& encounter_id,
                                  const std::uint64_t sequence,
                                  const char* close_reason) {
        DamageLogRepository repository(root);
        std::string error;
        NTE_CHECK(repository.Recover(error), "switch repository should initialize");
        NTE_CHECK(repository.BeginReport(
            ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
            "switch report should open");
        NTE_CHECK(repository.BeginEncounter(
            EncounterOpen{encounter_id, 1000, 0}, error),
            "switch encounter should open");
        DamageEvent event;
        event.sequence = sequence;
        event.world = {1, 1};
        event.attacker = {2, 1};
        event.victim = {3, 1};
        event.name_id = 4;
        event.final_value = static_cast<std::int64_t>(sequence);
        event.source_name = "Skill";
        event.attacker_name = "Attacker";
        event.victim_name = "Victim";
        NTE_CHECK(repository.Append(
            LoggedCombatEvent{event, 1100, 100, true}, error),
            "switch event should append");
        NTE_CHECK(repository.EndEncounter(EncounterClose{
            encounter_id, close_reason, 1200, 200,
            event.final_value, 0, 1.0, 0.2, {}}, error),
            "switch encounter should close");
        NTE_CHECK(repository.CloseReport(
            ReportClose{close_reason, 1200, false}, error),
            "switch report should close");
    };

    write_report(old_root, old_report, old_encounter, 101,
        "storage_path_changed");
    write_report(new_root, new_report, new_encounter, 202,
        "plugin_stop");

    const auto old_events = ReadAll(
        UnknownReports(old_root) / old_report / "events-000001.jsonl");
    const auto new_events = ReadAll(
        UnknownReports(new_root) / new_report / "events-000001.jsonl");
    NTE_CHECK(old_events.find("\"sequence\":\"101\"") != std::string::npos &&
            old_events.find("\"sequence\":\"202\"") == std::string::npos,
        "old root should contain only pre-switch events");
    NTE_CHECK(new_events.find("\"sequence\":\"202\"") != std::string::npos &&
            new_events.find("\"sequence\":\"101\"") == std::string::npos,
        "new root should contain only post-switch events");
    NTE_CHECK(ReadAll(UnknownReports(old_root) / old_report / "report.json").find(
            "\"close_reason\":\"storage_path_changed\"") != std::string::npos,
        "old report should record the storage-path boundary");
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
}

void ReportAcceptsExactlyOneEncounterAndRejectsLateEvents() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-single-encounter-report-test-" + std::to_string(nonce));
    const std::string report_id = "33333333-4444-4555-8666-777777777777";
    const std::string encounter_id = "cccccccc-dddd-4eee-8fff-000000000000";
    const std::string other_encounter = "dddddddd-eeee-4fff-8000-111111111111";
    DamageLogRepository repository(root);
    std::string error;
    NTE_CHECK(repository.Recover(error), "single-encounter repository should initialize");
    NTE_CHECK(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
        "single-encounter report should open");
    NTE_CHECK(repository.BeginEncounter(EncounterOpen{encounter_id, 1000, 0}, error),
        "single encounter should open");

    DamageEvent event;
    event.sequence = 1;
    event.world = {1, 1};
    event.attacker = {2, 1};
    event.victim = {3, 1};
    event.final_value = 10;
    LoggedCombatEvent logged{event, 1100, 100, true};
    logged.encounter_id = encounter_id;
    NTE_CHECK(repository.Append(logged, error),
        "event carrying the active encounter token should append");
    logged.encounter_id = other_encounter;
    NTE_CHECK(!repository.Append(logged, error),
        "event carrying a stale encounter token must be rejected");

    NTE_CHECK(repository.EndEncounter(EncounterClose{
        encounter_id, "inactivity", 1200, 200, 10, 0, 10.0, 0.2, {}}, error),
        "single encounter should close");
    logged.encounter_id = encounter_id;
    NTE_CHECK(!repository.Append(logged, error),
        "closed encounter must reject late events");
    NTE_CHECK(!repository.BeginEncounter(EncounterOpen{other_encounter, 1300, 300}, error),
        "a report must not accept a second encounter");
    NTE_CHECK(repository.CloseReport(ReportClose{"inactivity", 1200, false}, error),
        "single-encounter report should close");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void FailedReportInitializationRollsBackActiveState() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-report-rollback-test-" + std::to_string(nonce));
    const std::string blocked_report = "44444444-5555-4666-8777-888888888888";
    const std::string replacement_report = "55555555-6666-4777-8888-999999999999";
    std::filesystem::create_directories(
        UnknownReports(root) / blocked_report / "actors.jsonl");
    DamageLogRepository repository(root);
    std::string error;
    NTE_CHECK(repository.Recover(error), "rollback repository should initialize");
    NTE_CHECK(!repository.BeginReport(
            ReportOpen{blocked_report, {1, 1}, 1000, "build"}, error),
        "invalid report filesystem should fail initialization");
    NTE_CHECK(!repository.HasOpenReport(),
        "failed report initialization must not leave an active repository state");
    NTE_CHECK(repository.BeginReport(
            ReportOpen{replacement_report, {1, 1}, 1100, "build"}, error),
        "repository should accept a replacement report after rollback");
    NTE_CHECK(repository.CloseReport(ReportClose{"test", 1200, false}, error),
        "replacement report should close normally");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ActiveEncounterCanBeAbortedAsIncompleteReport() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-abort-active-report-test-" + std::to_string(nonce));
    const std::string report_id = "66666666-7777-4888-8999-aaaaaaaaaaaa";
    const std::string encounter_id = "eeeeeeee-ffff-4000-8111-222222222222";
    DamageLogRepository repository(root);
    std::string error;
    NTE_CHECK(repository.Recover(error), "abort repository should initialize");
    NTE_CHECK(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error) &&
            repository.BeginEncounter(EncounterOpen{encounter_id, 1000, 0}, error),
        "abort fixture should open an active encounter");
    NTE_CHECK(repository.CloseReport(ReportClose{
              "write_failure", 1200, true, true, "encounter close failed"}, error),
        "incomplete report close should forcibly seal an active encounter");
    const auto report = ReadAll(UnknownReports(root) / report_id / "report.json");
    const auto encounters = ReadAll(
        UnknownReports(root) / report_id / "encounters.jsonl");
    NTE_CHECK(report.find("\"open\":false") != std::string::npos &&
            report.find("\"complete\":false") != std::string::npos &&
            report.find("\"close_reason\":\"unclean_shutdown\"") !=
                std::string::npos,
        "aborted report should persist a closed incomplete state");
    NTE_CHECK(encounters.find("\"encounter_id\":\"" + encounter_id + "\"") !=
              std::string::npos &&
            encounters.find("\"complete\":false") != std::string::npos,
        "aborted report should retain a visible incomplete encounter summary");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void DeveloperModeRequiresTheAuthorizedAccountAndSavedPreference() {
    const auto authorized = nte::account::FromUidText("215017018089");
    const auto other = nte::account::FromUidText("216065736008");
    NTE_CHECK(authorized && other,
        "developer access fixtures should contain valid account identities");
    NTE_CHECK(nte::developer::IsDeveloperAccount(*authorized) &&
            !nte::developer::IsDeveloperAccount(*other) &&
            !nte::developer::IsDeveloperAccount(nte::account::Unknown()),
        "only the configured account should be eligible for developer mode");
    NTE_CHECK(nte::developer::EffectiveDeveloperMode(true, *authorized) &&
            !nte::developer::EffectiveDeveloperMode(false, *authorized) &&
            !nte::developer::EffectiveDeveloperMode(true, *other) &&
            !nte::developer::EffectiveDeveloperMode(
                true, nte::account::Unknown()),
        "developer mode should require both saved preference and account eligibility");

    nte::developer::AccessState access;
    access.SetSavedPreference(true);
    access.ObserveAccount(authorized);
    NTE_CHECK(access.available() && access.enabled(),
        "the authorized current observation should restore the saved preference");
    access.ObserveAccount(std::nullopt);
    NTE_CHECK(!access.available() && !access.enabled() && access.saved_preference(),
        "an unknown current observation should revoke access without clearing preference");
    access.SetSavedPreference(true);
    NTE_CHECK(!access.enabled() && !access.AuthorizePreferenceRequest(true),
        "config refresh and queued requests should not enable an unknown account");
    access.ObserveAccount(other);
    NTE_CHECK(!access.available() && !access.enabled() &&
            !access.AuthorizePreferenceRequest(false),
        "another current account should remain unauthorized for queued requests");
    access.ObserveAccount(authorized);
    const auto authorized_request = access.AuthorizePreferenceRequest(false);
    NTE_CHECK(access.available() && access.enabled() && authorized_request &&
            !*authorized_request,
        "returning to the authorized account should restore preference and authorize control");
}

nte::developer::SkillSnapshot Skill(
    const std::uint64_t id, const std::uint32_t flags,
    const float remaining = 0.0F) {
    nte::developer::SkillSnapshot skill;
    skill.handle = {id, 1};
    skill.character = {100, 1};
    skill.ability_class = {id + 1000, 1};
    skill.flags = flags;
    skill.sequence = id;
    skill.level = 2;
    skill.input_id = 3;
    skill.cooldown_valid = remaining > 0.0F;
    skill.cooldown_remaining_seconds = remaining;
    skill.cooldown_duration_seconds = remaining > 0.0F ? 10.0F : 0.0F;
    skill.ability_path = "/Game/Abilities/Test" + std::to_string(id);
    return skill;
}

void DeveloperBuffersAreBoundedAndClearedWhenDisabled() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    for (std::uint64_t index{}; index < 300; ++index) {
        nte::developer::DamageRecord record;
        record.sequence = index + 1;
        diagnostics.RecordDamage(std::move(record));
    }
    const auto snapshot = diagnostics.BuildSnapshot();
    NTE_CHECK(snapshot.enabled && snapshot.damage_records.size() == 256 &&
            snapshot.damage_records.front().sequence == 45 &&
            snapshot.damage_overwritten == 44,
        "damage diagnostics should retain the newest 256 records and count overwrites");

    diagnostics.SetEnabled(false);
    const auto disabled = diagnostics.BuildSnapshot();
    NTE_CHECK(!disabled.enabled && disabled.damage_records.empty() &&
            disabled.combat_event_records.empty() &&
            disabled.skill_records.empty() && disabled.damage_overwritten == 0,
        "disabling developer mode should clear all in-memory diagnostics");
}

void UnifiedCombatEventsRetainBuffDetails() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    nte::developer::CombatEventRecord record;
    record.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    record.sequence = 42;
    record.name_id = 9001;
    record.duration_seconds = 12.5F;
    record.stack_count = 3;
    record.accepted = true;
    diagnostics.RecordCombatEvent(record);
    const auto snapshot = diagnostics.BuildSnapshot();
    NTE_CHECK(snapshot.combat_event_records.size() == 1 &&
            snapshot.combat_event_records.front().kind ==
                nte::combat_stats::CombatEventKind::BuffAdd &&
            snapshot.combat_event_records.front().duration_seconds == 12.5F &&
            snapshot.combat_event_records.front().stack_count == 3,
        "unified diagnostics should retain complete BUFF event details");
}

void MapTargetAndEventParticipantStateClearsWhenDisabled() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    diagnostics.SetCurrentMap(nte::developer::MapIdentity{
        {10, 2}, 42, "World_City", "available"});
    diagnostics.SetCurrentTarget(nte::developer::ParticipantIdentity{
        {20, 3}, "/Game/Enemy/Boss_07_BP_WorldBoss_C", "塞润尼缇",
        "Boss_07_BP_WorldBoss", "Boss_07"});
    nte::developer::DamageRecord damage;
    damage.attacker = {{30, 4}, "/Game/Enemy/Boss_05_BP_C", "墨菲克斯",
        "boss_05_BP", "Boss_05"};
    damage.victim = {{40, 5}, "/Game/Player/player_076_shinku_C", "真红", {}, {}};
    diagnostics.RecordDamage(std::move(damage));

    const auto active = diagnostics.BuildSnapshot();
    // 先算成 bool 再断言，理由同 EncounterLifecycle 里的世界身份检查。
    const bool map_target_present = active.current_map && active.current_map->handle ==
            nte::combat_stats::Handle{10, 2} &&
            active.current_map->name_id == 42 &&
            active.current_map->technical_name == "World_City" &&
            active.current_target &&
            active.current_target->monster_id == "Boss_07" &&
            active.damage_records.size() == 1 &&
            active.damage_records[0].attacker.enemy_config_id == "boss_05_BP" &&
            active.damage_records[0].victim.technical_path ==
                "/Game/Player/player_076_shinku_C";
    NTE_CHECK(map_target_present,
        "developer snapshots should expose map, target, and event participant identities");

    diagnostics.SetEnabled(false);
    const auto disabled = diagnostics.BuildSnapshot();
    NTE_CHECK(!disabled.current_map && !disabled.current_target &&
            disabled.damage_records.empty(),
        "disabling developer mode should clear map, target, and participant diagnostics");
}

void SkillFramesProduceChangesWithoutInventingPartialRemovals() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    diagnostics.ObserveSkillFrame({1, 10, true, {Skill(1, 1), Skill(2, 0)}});
    auto snapshot = diagnostics.BuildSnapshot();
    NTE_CHECK(snapshot.skill_records.size() == 2 &&
            snapshot.skill_records[0].kind == nte::developer::SkillChangeKind::Added,
        "the first complete frame should establish an observable added baseline");

    diagnostics.ObserveSkillFrame({1, 11, false, {Skill(1, 3, 4.0F)}});
    snapshot = diagnostics.BuildSnapshot();
    NTE_CHECK(snapshot.skill_records.size() == 3 &&
            snapshot.skill_records.back().kind ==
                nte::developer::SkillChangeKind::Changed,
        "partial frames should record present changes");

    diagnostics.ObserveSkillFrame({1, 12, true, {Skill(1, 3, 4.0F)}});
    snapshot = diagnostics.BuildSnapshot();
    NTE_CHECK(snapshot.skill_records.size() == 4 &&
            snapshot.skill_records.back().kind ==
                nte::developer::SkillChangeKind::Removed &&
            snapshot.skill_records.back().skill.handle.id == 2,
        "only a complete frame should confirm a removed skill");

    diagnostics.ObserveSkillFrame({2, 1, true, {Skill(9, 0)}});
    snapshot = diagnostics.BuildSnapshot();
    NTE_CHECK(snapshot.skill_records.back().kind ==
            nte::developer::SkillChangeKind::Added,
        "a generation change should start a new baseline without fabricated removals");
}

void SkillBufferIsBoundedAndCooldownValidityIsExplicit() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    nte::developer::SkillFrame frame{3, 90, true, {}};
    frame.skills.reserve(300);
    for (std::uint64_t index{}; index < 300; ++index) {
        auto skill = Skill(index + 1, 0, 5.0F);
        skill.cooldown_valid = false;
        frame.skills.push_back(std::move(skill));
    }
    diagnostics.ObserveSkillFrame(std::move(frame));
    const auto snapshot = diagnostics.BuildSnapshot();
    NTE_CHECK(snapshot.skill_records.size() == 256 &&
            snapshot.skill_overwritten == 44 &&
            snapshot.skill_records.front().skill.handle.id == 45,
        "skill diagnostics should retain the newest 256 changes and count overwrites");
    NTE_CHECK(!snapshot.skill_records.back().skill.cooldown_valid &&
            snapshot.skill_records.back().skill.cooldown_remaining_seconds == 5.0F,
        "cooldown availability should follow COOLDOWN_VALID rather than numeric values");
}

void AccountUidIsCanonicalAndPathSafe() {
    const auto known = nte::account::FromUidText("000216065736008");
    NTE_CHECK(known.has_value() && known->uid == "216065736008" &&
            known->source == nte::account::Source::UiRoleId,
        "numeric UI text should become a canonical known account");
    NTE_CHECK(!nte::account::FromUidText("0") &&
            !nte::account::FromUidText("123456789012345678901") &&
            !nte::account::FromUidText("12/34") &&
            !nte::account::FromUidText(""),
        "zero, overlong, non-numeric, and empty UIDs should be rejected");
    NTE_CHECK(nte::account::DirectoryName(*known) == "216065736008" &&
            nte::account::DirectoryName(nte::account::Unknown()) == "_unknown",
        "account directory names should be deterministic and path safe");
    NTE_CHECK(nte::account::FromDirectoryName("216065736008") == known &&
            nte::account::FromDirectoryName("_unknown") ==
                std::optional{nte::account::Unknown()} &&
            !nte::account::FromDirectoryName("../outside"),
        "only known UID and unknown sentinel directories should parse");
    NTE_CHECK(nte::account::ShouldTransition(nte::account::Unknown(), known) &&
            !nte::account::ShouldTransition(*known, known) &&
            !nte::account::ShouldTransition(*known, std::nullopt),
        "only a newly detected different UID should create an account boundary");
}

void RepositoryAssetProvidesAllAttributeColors() {
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/attribute_colors.json",
        error);

    NTE_CHECK(error.empty(), "the checked-in attribute color asset should load");
    NTE_CHECK(colors.Find("NATURE") == 0x3EBDA1U, "NATURE should use its teal color");
    NTE_CHECK(colors.Find("LAKSHANA") == 0xCBBD39U, "LAKSHANA should use its gold color");
    NTE_CHECK(colors.Find("CHAOS") == 0x5E58BDU, "CHAOS should use its violet color");
    NTE_CHECK(colors.Find("COSMOS") == 0xBCBCBCU, "COSMOS should use its silver color");
    NTE_CHECK(colors.Find("PSYCHE") == 0x3E98C5U, "PSYCHE should use its blue color");
    NTE_CHECK(colors.Find("INCANTATION") == 0xD0455EU, "INCANTATION should use its crimson color");
    NTE_CHECK(!colors.Find("UNKNOWN").has_value(),
        "an unknown attribute should not invent a color");
}

// The character id is derived from the participant object path, which is the
// only identity the combat stream carries. Guard the mapping the runtime lookup
// depends on.
void CharacterIdIsRecoveredFromParticipantPath() {
    NTE_CHECK(nte::combat_stats::CharacterIdFromPath(
              "/Game/Blueprints/Character/Player/Player_003_Sagiri.Player_003_Sagiri_C") ==
              "1003",
        "player_003 should map to the DT_Character row 1003");
    NTE_CHECK(nte::combat_stats::CharacterIdFromPath("player_021_edgar") == "1021",
        "player_021 should map to the DT_Character row 1021");
    NTE_CHECK(nte::combat_stats::CharacterIdFromPath("/Game/Enemy/monster_C").empty(),
        "a non-character path has no character id");
    NTE_CHECK(nte::combat_stats::CharacterIdFromPath("player_CC_001").empty(),
        "a malformed player segment has no character id");
}

std::filesystem::path TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-attribute-colors-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void Write(const std::filesystem::path& path, const std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

void LowercaseHexIsAccepted() {
    const auto root = TemporaryRoot();
    const auto path = root / "attribute_colors.json";
    Write(path, R"({"NATURE":"#2f9f89","LAKSHANA":"#b67817","CHAOS":"#625daf","COSMOS":"#699a87","PSYCHE":"#326fbf","INCANTATION":"#b61e3a"})");
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    NTE_CHECK(error.empty() && colors.Find("NATURE") == 0x2F9F89U &&
              colors.Find("INCANTATION") == 0xB61E3AU,
        "lowercase hexadecimal colors should be accepted");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void InvalidDictionariesDegradeSafely() {
    const auto root = TemporaryRoot();
    const auto path = root / "attribute_colors.json";
    std::string error;

    auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    NTE_CHECK(!error.empty() && !colors.Find("NATURE").has_value(),
        "a missing dictionary should return an empty map with an error");

    Write(path, R"({"NATURE":"#2F9F89","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    NTE_CHECK(!error.empty() && !colors.Find("NATURE").has_value(),
        "a dictionary missing a required attribute should be rejected atomically");

    Write(path, R"({"NATURE":"#2F9F89","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF","INCANTATION":""})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    NTE_CHECK(!error.empty() && !colors.Find("NATURE").has_value(),
        "an empty color should reject the complete dictionary");

    Write(path, R"({"NATURE":"#2F9F89","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF","INCANTATION":"#B61E3Z"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    NTE_CHECK(!error.empty() && !colors.Find("NATURE").has_value(),
        "a non-hexadecimal color should reject the complete dictionary");

    Write(path, R"({"NATURE":"#2F9F89","NATURE":"#000000","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF","INCANTATION":"#B61E3A"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    NTE_CHECK(!error.empty() && !colors.Find("NATURE").has_value(),
        "duplicate attribute keys should be rejected");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

void RunCombatStatsEngine() {
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
}

void RunDamageLog() {
    AccountsUseIndependentPhysicalRepositories();
    ClosedEncounterProducesRecoverableJsonlReport();
    DelayedEventNameCompletionUpdatesTheLocalAbilityIndex();
    WorkerDrainsCommandsBeforeStopping();
    RecoveryPersistsUncleanShutdownState();
    HundredThousandEventsAreSegmentedWithoutLoss();
    StoragePathBoundaryKeepsReportsSeparated();
    ReportAcceptsExactlyOneEncounterAndRejectsLateEvents();
    FailedReportInitializationRollsBackActiveState();
    ActiveEncounterCanBeAbortedAsIncompleteReport();
}

void RunDeveloperDiagnostics() {
    DeveloperModeRequiresTheAuthorizedAccountAndSavedPreference();
    DeveloperBuffersAreBoundedAndClearedWhenDisabled();
    UnifiedCombatEventsRetainBuffDetails();
    MapTargetAndEventParticipantStateClearsWhenDisabled();
    SkillFramesProduceChangesWithoutInventingPartialRemovals();
    SkillBufferIsBoundedAndCooldownValidityIsExplicit();
}

void RunAccountIdentity() {
    AccountUidIsCanonicalAndPathSafe();
}

void RunAttributeColorMap() {
    RepositoryAssetProvidesAllAttributeColors();
    CharacterIdIsRecoveredFromParticipantPath();
    LowercaseHexIsAccepted();
    InvalidDictionariesDegradeSafely();
}
NTE_TEST_GROUP("nte-engine",
    RunCombatStatsEngine,
    RunDamageLog,
    RunDeveloperDiagnostics,
    RunAccountIdentity,
    RunAttributeColorMap)
