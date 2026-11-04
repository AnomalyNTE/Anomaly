#include "damage_log.hpp"
#include "history_reader.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

using nte::combat_stats::DamageEvent;
using nte::combat_stats::Handle;
using nte::combat_stats::PlayerRow;
using nte::damage_log::DamageLogRepository;
using nte::damage_log::EncounterClose;
using nte::damage_log::EncounterOpen;
using nte::damage_log::LoggedCombatEvent;
using nte::damage_log::ReportClose;
using nte::damage_log::ReportOpen;
using nte::combat_stats::overlay::CombatLogReader;
using nte::combat_stats::overlay::DetailDirection;

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::filesystem::path UnknownReports(const std::filesystem::path& root) {
    return root / "accounts" / "_unknown" / "reports";
}

void ClosedLogsCanBeBrowsedAndDrilledDown() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-history-reader-test-" + std::to_string(nonce));
    const std::string report_id = "11111111-2222-4333-8444-555555555555";
    const std::string encounter_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const Handle world{1, 1};
    const Handle player{2, 1};
    const Handle enemy{3, 1};
    {
        DamageLogRepository repository(root);
        std::string error;
        Check(repository.Recover(error), "reader fixture repository should initialize");
        Check(repository.BeginReport(ReportOpen{
            report_id, world, 1'700'000'000'000, "build"}, error),
            "reader fixture report should open");
        Check(repository.BeginEncounter(EncounterOpen{
            encounter_id, 1'700'000'001'000, 1'000'000}, error),
            "reader fixture encounter should open");
        DamageEvent first;
        first.sequence = 10;
        first.tick_sequence = 20;
        first.flags = nte::combat_stats::kDamageFlagCritical;
        first.world = world;
        first.attacker = player;
        first.victim = enemy;
        first.name_id = 77;
        first.final_value = 120;
        first.basic_value = 100;
        first.value = 121;
        first.hit_location = {1.5, 2.5, 3.5};
        first.damage_type = 4;
        first.display_type = 5;
        first.reaction_type = 6;
        first.reaction_display_type = 7;
        first.source_name = "SkillA";
        first.source_display_name = "已记录技能";
        first.source_name_origin = "resource";
        first.name_resource_version = "fixture-v1";
        first.attacker_name = "/Game/Player/player_C";
        first.attacker_display_name = "已记录角色";
        first.attacker_name_origin = "resource";
        first.victim_name = "/Game/Enemy/enemy_C";
        Check(repository.Append(LoggedCombatEvent{
            first, 1'700'000'001'250, 1'250'000, true}, error),
            "first fixture event should append");
        auto second = first;
        second.sequence = 11;
        second.attacker = {player.id, 2};
        second.final_value = 80;
        second.flags = 0;
        second.source_display_name = "SkillA";
        second.source_name_origin = "technical";
        second.attacker_display_name = "player_C";
        second.attacker_name_origin = "technical";
        Check(repository.Append(LoggedCombatEvent{
            second, 1'700'000'001'750, 1'750'000, true}, error),
            "second fixture event should append");
        auto immune = second;
        immune.sequence = 12;
        immune.final_value = 0;
        immune.flags = nte::combat_stats::kDamageFlagCritical;
        Check(repository.Append(LoggedCombatEvent{
            immune, 1'700'000'001'850, 1'850'000, true}, error),
            "zero-damage fixture event should append");
        auto negative = immune;
        negative.sequence = 13;
        negative.final_value = -10;
        Check(repository.Append(LoggedCombatEvent{
            negative, 1'700'000'001'950, 1'950'000, true}, error),
            "negative fixture event should append");
        PlayerRow row;
        row.handle = player;
        row.object_path = first.attacker_name;
        row.label = "player_C";
        row.display_name = "已记录角色";
        row.name_origin = "resource";
        row.name_resource_version = "fixture-v1";
        row.damage = 200;
        row.positive_hits = 99;
        row.critical_hits = 99;
        row.maximum_hit = 120;
        row.maximum_source_id = 77;
        row.maximum_source_name = "SkillA";
        row.damage_share = 100.0;
        row.encounter_dps = 100.0;
        EncounterClose close{encounter_id, "manual_end",
            1'700'000'003'000, 3'000'000, 200, 0, 100.0, 2.0, {}};
        close.confirmed_controlled_players.push_back(row);
        Check(repository.EndEncounter(close, error), "fixture encounter should close");
        Check(repository.CloseReport(ReportClose{
            "plugin_stop", 1'700'000'003'100, false}, error),
            "fixture report should close");
    }

    const auto event_path = UnknownReports(root) / report_id / "events-000001.jsonl";
    {
        std::ifstream input(event_path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        const std::string source = "\"source_path\":\"/Game/Player/player_C\"";
        const std::string target = "\"target_path\":\"/Game/Enemy/enemy_C\"";
        const auto source_at = content.find(source);
        Check(source_at != std::string::npos && content.find(target) != std::string::npos,
            "fixture should contain source and target technical paths");
        if (source_at != std::string::npos) {
            content.replace(source_at, source.size(),
                "\"source_path\":\"RecordedAttacker\"");
        }
        if (const auto target_at = content.find(target);
            target_at != std::string::npos) {
            content.replace(target_at, target.size(),
                "\"target_path\":\"RecordedVictim\"");
        }
        std::ofstream output(event_path, std::ios::binary | std::ios::trunc);
        output << content;
    }
    {
        std::ofstream events(event_path, std::ios::binary | std::ios::app);
        events << "{\"encounter_id\":\"ffffffff-ffff-4fff-8fff-ffffffffffff\"}\n";
    }
    {
        std::ofstream encounters(
            UnknownReports(root) / report_id / "encounters.jsonl",
            std::ios::binary | std::ios::app);
        encounters << "{\"malformed\":true}\n";
    }

    CombatLogReader reader(root);
    std::string error;
    const auto history = reader.History(error);
    Check(!error.empty() && history.size() == 1 &&
            history[0].encounters.size() == 1,
        "valid history should remain visible when one summary row is malformed");
    if (!history.empty()) {
        const auto& encounter = history[0].encounters[0];
        Check(history[0].account == nte::account::Unknown() &&
                  encounter.encounter_id == encounter_id &&
                  encounter.end_reason == "manual_end" &&
                  encounter.outgoing_damage == 200 &&
                  encounter.game_build == "build",
              "history should expose encounter identity and summary");
        Check(encounter.critical_data_status ==
                  nte::combat_stats::CriticalDataStatus::Unavailable,
              "old encounter summaries default critical quality to unavailable");
    }
    const auto collapsed = nte::combat_stats::overlay::FlattenHistoryGroups(
        history, {});
    const auto expanded = nte::combat_stats::overlay::FlattenHistoryGroups(
        history, {"_unknown"});
    Check(collapsed.size() == 1 &&
            collapsed[0].kind ==
                nte::combat_stats::overlay::HistoryListRow::Kind::Account &&
            expanded.size() == 2 && expanded[1].encounter &&
            expanded[1].encounter->encounter_id == encounter_id,
        "history groups should flatten into collapsible account folders");
    const auto detail = reader.Encounter(
        nte::account::Unknown(), report_id, encounter_id, error);
    Check(detail.has_value() && error.empty(), "closed encounter detail should load");
    if (detail) {
        Check(detail->summary.critical_data_status ==
                  nte::combat_stats::CriticalDataStatus::Available,
              "complete unified events should reveal available critical metadata");
        Check(detail->players.size() == 1 && detail->players[0].handle == player,
            "report-local actor ids should resolve back to player handles");
        Check(detail->summary.game_build == "build" &&
                  detail->players[0].display_name == "已记录角色" &&
                  detail->players[0].name_origin == "resource" &&
                  detail->events[0].source_display_name == "已记录技能" &&
                  detail->events[0].source_name_origin == "resource",
            "history should retain report build and recorded display-name priority metadata");
        Check(detail->players.size() == 1 && detail->players[0].positive_hits == 2 &&
                  detail->players[0].critical_hits == 1,
            "history should recompute hit and critical counts from raw events across generations");
        Check(detail->events.size() == 4 && detail->events[0].sequence == 10,
            "all matching raw events should load in sequence order");
        Check(detail->events[0].world == world &&
                  detail->events[0].attacker_actor_id != 0 &&
                  detail->events[0].victim_actor_id != 0 &&
                  detail->events[0].ability_id != 0 &&
                  detail->events[0].player_related &&
                  detail->events[0].hit_location[1] == 2.5 &&
                  detail->events[0].damage_type == 4 &&
                  detail->events[0].display_type == 5 &&
                  detail->events[0].reaction_type == 6 &&
                  detail->events[0].reaction_display_type == 7,
            "history events should retain complete raw metadata for export");
        const auto sources = nte::combat_stats::overlay::GroupEventSources(
            detail->events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage);
        Check(sources.size() == 1 && sources[0].name_id == 77 &&
                  sources[0].amount == 200 && sources[0].count == 2 &&
                  sources[0].maximum == 120,
              "player events should aggregate by damage source");
        const auto hits = nte::combat_stats::overlay::FilterEvents(
            detail->events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage, sources[0]);
        Check(hits.size() == 2 && hits[1].received_utc_ms == 1'700'000'001'750,
            "source selection should expose timestamped individual hits");

        auto renamed_events = detail->events;
        renamed_events[1].source = "SkillB";
        const auto renamed_sources = nte::combat_stats::overlay::GroupEventSources(
            renamed_events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage);
        Check(renamed_sources.size() == 2,
            "the same source id with different names should remain distinct");
        if (renamed_sources.size() == 2) {
            const auto renamed_hits = nte::combat_stats::overlay::FilterEvents(
                renamed_events, player, DetailDirection::Outgoing,
                nte::combat_stats::CombatEventKind::Damage, renamed_sources[0]);
            Check(renamed_hits.size() == 1,
                "source drill-down should retain the selected source name");
        }

        auto renumbered_events = detail->events;
        renumbered_events[1].name_id = 88;
        const auto renumbered_sources = nte::combat_stats::overlay::GroupEventSources(
            renumbered_events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage);
        Check(renumbered_sources.size() == 1 && renumbered_sources[0].amount == 200,
            "the same technical source path should aggregate across source id generations");
        if (renumbered_sources.size() == 1) {
            const auto renumbered_hits = nte::combat_stats::overlay::FilterEvents(
                renumbered_events, player, DetailDirection::Outgoing,
                nte::combat_stats::CombatEventKind::Damage, renumbered_sources[0]);
            Check(renumbered_hits.size() == 2,
                "source drill-down should follow technical identity across generations");
        }

        auto incomplete_players = detail->players;
        incomplete_players[0].positive_hits = 7;
        incomplete_players[0].critical_hits = 4;
        nte::combat_stats::overlay::RecomputePlayerHitCounts(
            detail->events, incomplete_players, false);
        Check(incomplete_players[0].positive_hits == 7 &&
                  incomplete_players[0].critical_hits == 4,
            "incomplete history must retain persisted hit counts instead of using partial events");
        Check(nte::combat_stats::overlay::CriticalDataStatusForEvents(
                  {}, incomplete_players, false) ==
                  nte::combat_stats::CriticalDataStatus::Partial,
            "an incomplete history remains partial even when its event segments are empty");
    }

    {
        std::ofstream events(UnknownReports(root) / report_id /
                "events-000001.jsonl", std::ios::binary | std::ios::app);
        events << "{\"encounter_id\":\"" << encounter_id << "\"}\n";
    }
    const auto export_detail = reader.ExportEncounter(
        nte::account::Unknown(), report_id, encounter_id, error);
    Check(export_detail.has_value() && error.empty(),
        "export encounter should load the persisted final record");
    if (export_detail) {
        Check(export_detail->players[0].positive_hits == 99 &&
                  export_detail->players[0].critical_hits == 99 &&
                  export_detail->summary.critical_data_status ==
                      nte::combat_stats::CriticalDataStatus::Unavailable,
            "export should preserve persisted final player and critical metadata");
        Check(export_detail->events[1].attacker.generation == 2,
            "export should preserve raw event handles without display canonicalization");
        Check(export_detail->events[1].source_display_name == "SkillA" &&
                  export_detail->events[1].source_name_origin == "technical" &&
                  export_detail->events[1].attacker_display_name == "player_C" &&
                  export_detail->events[1].attacker_name_origin == "technical",
            "export should preserve event-level recorded names and origins");
        Check(export_detail->events[0].attacker_name == "RecordedAttacker" &&
                  export_detail->events[0].victim_name == "RecordedVictim",
            "export should not replace event-level recorded technical names");
        Check(export_detail->events.size() == 4 &&
                  export_detail->event_read_warnings == 1,
            "export should retain captured events and report malformed target rows");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void EventAbilityIdDisambiguatesReusedSourceIds() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-history-source-reuse-test-" + std::to_string(nonce));
    const std::string report_id = "33333333-4444-4555-8666-777777777777";
    const std::string encounter_id = "cccccccc-dddd-4eee-8fff-000000000000";
    const auto report = UnknownReports(root) / report_id;
    std::filesystem::create_directories(report);
    {
        std::ofstream actors(report / "actors.jsonl", std::ios::binary);
        actors << "{\"schema_version\":2,\"actor_id\":1,"
                  "\"handle\":{\"id\":\"2\",\"generation\":\"1\"},"
                  "\"path\":\"Player\"}\n"
                  "{\"schema_version\":2,\"actor_id\":2,"
                  "\"handle\":{\"id\":\"3\",\"generation\":\"1\"},"
                  "\"path\":\"Enemy\"}\n";
    }
    {
        std::ofstream abilities(report / "abilities.jsonl", std::ios::binary);
        abilities << "{\"schema_version\":2,\"ability_id\":1,"
                     "\"source_id\":\"9223372036854775813\","
                     "\"name\":\"GE_Player_Shinku_Melee1_1_Damage\","
                     "\"display_name\":\"普通攻击：特种格斗术\","
                     "\"name_origin\":\"resource\"}\n"
                     "{\"schema_version\":2,\"ability_id\":2,"
                     "\"source_id\":\"9223372036854775813\","
                     "\"name\":\"GE_Player_Sagiri_Melee1_Damage\","
                     "\"display_name\":\"鬼郎丸挥击\","
                     "\"name_origin\":\"resource\"}\n";
    }
    {
        std::ofstream events(report / "events-000001.jsonl", std::ios::binary);
        const auto write_event = [&](const int sequence, const int ability_id,
                                     const char* source) {
            events << "{\"schema_version\":2,\"event_type\":\"damage\","
                   << "\"encounter_id\":\"" << encounter_id << "\","
                   << "\"sequence\":\"" << sequence << "\","
                   << "\"tick_sequence\":\"" << sequence << "\","
                   << "\"flags\":0,\"ability_id\":" << ability_id << ','
                   << "\"name_id\":\"9223372036854775813\","
                   << "\"source\":{\"id\":\"2\",\"generation\":\"1\"},"
                   << "\"target\":{\"id\":\"3\",\"generation\":\"1\"},"
                   << "\"final_value\":\"100\",\"basic_value\":\"0\","
                   << "\"value\":\"0\",\"received_utc_ms\":1,"
                   << "\"received_offset_us\":\"1\",\"event_name\":\""
                   << source << "\",\"source_display_name\":\"" << source
                   << "\",\"event_name_origin\":\"technical\"}\n";
        };
        write_event(1, 1, "GE_Player_Shinku_Melee1_1_Damage");
        write_event(2, 2, "GE_Player_Sagiri_Melee1_Damage");
    }

    CombatLogReader reader(root);
    std::string error;
    const auto events = reader.Events(
        nte::account::Unknown(), report_id, encounter_id, error);
    Check(error.empty() && events.size() == 2,
        "history should load events that reuse a source id");
    if (events.size() == 2) {
        Check(events[0].ability_id == 1 &&
                  events[0].source_display_name == "普通攻击：特种格斗术" &&
                  events[1].ability_id == 2 &&
                  events[1].source_display_name == "鬼郎丸挥击",
            "event ability ids must select the matching localized dictionary row");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void HealingDrilldownSeparatesDirectionsAndEventKinds() {
    const Handle healer{40, 1};
    const Handle target{41, 1};
    nte::combat_stats::overlay::CombatEventDetail heal;
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    heal.sequence = 1;
    heal.attacker = healer;
    heal.victim = target;
    heal.name_id = 90;
    heal.source = "HealSkill";
    heal.value = 300;
    heal.final_value = 0;
    auto damage = heal;
    damage.kind = nte::combat_stats::CombatEventKind::Damage;
    damage.sequence = 2;
    damage.final_value = 900;
    const std::vector events{heal, damage};

    const auto done = nte::combat_stats::overlay::GroupEventSources(
        events, healer, DetailDirection::Outgoing,
        nte::combat_stats::CombatEventKind::Heal);
    const auto received = nte::combat_stats::overlay::GroupEventSources(
        events, target, DetailDirection::Incoming,
        nte::combat_stats::CombatEventKind::Heal);
    Check(done.size() == 1 && done.front().amount == 300 &&
              received.size() == 1 && received.front().amount == 300,
        "healing drilldown should support output and received directions without damage");
    if (!done.empty()) {
        const auto rows = nte::combat_stats::overlay::FilterEvents(
            events, healer, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Heal, done.front());
        Check(rows.size() == 1 && rows.front().kind ==
                  nte::combat_stats::CombatEventKind::Heal,
            "healing source selection should expose only individual heal events");
    }
}

void LegacyV1LogsAreNotReadOrConverted() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-history-v1-rejection-test-" + std::to_string(nonce));
    const auto account = root / "accounts" / "_unknown";
    std::filesystem::create_directories(account);
    {
        std::ofstream catalog(account / "catalog.json", std::ios::binary);
        catalog << "{\"schema_version\":1,\"reports\":[]}";
    }
    CombatLogReader reader(root);
    std::string error;
    const auto history = reader.History(error);
    Check(history.empty() && error.find("schema is not supported") != std::string::npos,
        "legacy v1 logs should be rejected instead of read or converted");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    ClosedLogsCanBeBrowsedAndDrilledDown();
    EventAbilityIdDisambiguatesReusedSourceIds();
    HealingDrilldownSeparatesDirectionsAndEventKinds();
    LegacyV1LogsAreNotReadOrConverted();
    if (failures != 0) return 1;
    std::cout << "All history reader tests passed\n";
    return 0;
}
