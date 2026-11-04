#include "damage_log.hpp"
#include "log_worker.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using nte::combat_stats::DamageEvent;
using nte::combat_stats::Handle;
using nte::damage_log::DamageLogRepository;
using nte::damage_log::EncounterClose;
using nte::damage_log::EncounterOpen;
using nte::damage_log::LoggedCombatEvent;
using nte::damage_log::ReportClose;
using nte::damage_log::ReportOpen;

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
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
        Check(repository.Recover(error), "multi-account repository should recover");
        Check(repository.BeginReport(ReportOpen{
            report_a, {1, 1}, 1, "build", "plugin", "runtime", account_a}, error),
            "first account report should open");
        Check(repository.CloseReport(ReportClose{"done", 2, false}, error),
            "first account report should close");
        Check(repository.BeginReport(ReportOpen{
            report_b, {2, 1}, 3, "build", "plugin", "runtime", account_b}, error),
            "second account report should open");
        Check(repository.CloseReport(ReportClose{"done", 4, false}, error),
            "second account report should close");
        const auto history = repository.History();
        Check(history.size() == 2 && history[0].account == account_b &&
                history[1].account == account_a,
            "history should retain immutable account ownership");
    }
    Check(std::filesystem::is_regular_file(root / "accounts" / "10001" /
              "reports" / report_a / "report.json") &&
            std::filesystem::is_regular_file(root / "accounts" / "20002" /
              "reports" / report_b / "report.json") &&
            !std::filesystem::is_regular_file(root / "reports" / report_a /
              "report.json"),
        "reports should be physically isolated without adopting legacy roots");
    const auto account_catalog = ReadAll(root / "accounts" / "10001" / "catalog.json");
    const auto account_report = ReadAll(root / "accounts" / "10001" /
        "reports" / report_a / "report.json");
    Check(account_catalog.find("\"uid\":\"10001\"") != std::string::npos &&
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
        Check(repository.Recover(error), "empty repository should recover");
        Check(repository.BeginReport(ReportOpen{
                  report_id, Handle{9, 2}, 1'700'000'000'000, "nte-build-test"}, error),
              "report should open");
        std::string active_delete_error;
        Check(!repository.DeleteReport(
                  nte::account::Unknown(), report_id, active_delete_error),
              "an active report must not be deleted");
        Check(repository.BeginEncounter(EncounterOpen{
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
        Check(repository.Append(LoggedCombatEvent{
                  event, 1'700'000'000'200, 200'000, true}, error),
              "damage should append");
        auto heal = event;
        heal.kind = nte::combat_stats::CombatEventKind::Heal;
        heal.sequence += 1;
        heal.name_id = 7001;
        heal.final_value = 250;
        Check(repository.Append(LoggedCombatEvent{
                  heal, 1'700'000'000'300, 300'000, true}, error),
              "heal should append");
        auto buff_add = event;
        buff_add.kind = nte::combat_stats::CombatEventKind::BuffAdd;
        buff_add.sequence += 2;
        buff_add.name_id = 7002;
        buff_add.final_value = 0;
        buff_add.duration_seconds = 12.5F;
        buff_add.stack_count = 3;
        Check(repository.Append(LoggedCombatEvent{
                  buff_add, 1'700'000'000'400, 400'000, true}, error),
              "buff add should append");
        auto buff_remove = buff_add;
        buff_remove.kind = nte::combat_stats::CombatEventKind::BuffRemove;
        buff_remove.sequence += 1;
        buff_remove.stack_count = 2;
        Check(repository.Append(LoggedCombatEvent{
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
        Check(repository.EndEncounter(close, error),
              "encounter should close");
        Check(repository.CloseReport(ReportClose{
                  "plugin_stop", 1'700'000'010'200, false}, error),
              "report should close");
    }

    const auto report_path = UnknownReports(root) / report_id;
    Check(std::filesystem::exists(
              root / "accounts" / "_unknown" / "catalog.json"),
        "account catalog should exist");
    Check(std::filesystem::exists(report_path / "report.json"), "report metadata should exist");
    Check(std::filesystem::exists(report_path / "actors.jsonl"), "actor dictionary should exist");
    Check(std::filesystem::exists(report_path / "abilities.jsonl"), "ability dictionary should exist");
    Check(std::filesystem::exists(report_path / "encounters.jsonl"), "encounter summary should exist");
    Check(std::filesystem::exists(report_path / "events-000001.jsonl"), "event segment should exist");
    Check(std::filesystem::exists(report_path / "index.json"), "segment index should exist");
    const std::string report_index = ReadAll(report_path / "index.json");
    Check(report_index.find("\"actors_sha256\":\"") != std::string::npos &&
              report_index.find("\"abilities_sha256\":\"") != std::string::npos &&
              report_index.find("\"encounters_sha256\":\"") != std::string::npos,
          "report content hash should cover dictionaries and encounter summaries");
    const std::string report = ReadAll(report_path / "report.json");
    Check(report.find("\"schema_version\":3") != std::string::npos &&
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
    Check(events.find("\"sequence\":\"18446744073709551000\"") != std::string::npos,
          "64-bit sequence should be encoded as a decimal string");
    Check(events.find("\"name_id\":\"18446744073709550999\"") != std::string::npos,
          "64-bit name id should be encoded as a decimal string");
    Check(events.find("\"event_name\":\"Skill \\\"A\\\"\"") != std::string::npos,
          "JSON strings should be escaped");
    Check(events.find("\"event_name_origin\":\"resource\"") != std::string::npos &&
              events.find("\"source_name_origin\":\"resource\"") != std::string::npos &&
              events.find("\"target_name_origin\":\"resource\"") != std::string::npos &&
              events.find("\"name_resource_version\":\"2026.08.28\"") !=
                  std::string::npos &&
              events.find("name_catalog_version") == std::string::npos,
          "events should retain display-name origins for historical priority");
    Check(!events.empty() && events.back() == '\n', "JSONL should end with a newline");
    Check(events.find("\"event_type\":\"heal\"") != std::string::npos &&
              events.find("\"event_type\":\"buff_add\"") != std::string::npos &&
              events.find("\"event_type\":\"buff_remove\"") != std::string::npos &&
              events.find("\"duration_seconds\":12.5") != std::string::npos &&
              events.find("\"stack_count\":3") != std::string::npos,
          "all unified event kinds and buff snapshots should round-trip through JSONL");
    const std::string encounters = ReadAll(report_path / "encounters.jsonl");
    Check(encounters.find("\"actor_outgoing\"") != std::string::npos &&
              encounters.find("\"ability_stats\"") != std::string::npos &&
              encounters.find("\"actor_ability\"") != std::string::npos &&
              encounters.find("\"actor_target\"") != std::string::npos &&
              encounters.find("\"ability_target\"") != std::string::npos,
          "encounter summary should contain all participant aggregate dimensions");
    Check(encounters.find("\"critical_hits\":\"1\"") != std::string::npos,
          "encounter aggregates should retain critical hit counts");
    Check(encounters.find("\"schema_version\":3") != std::string::npos &&
              encounters.find("\"outgoing_healing\":\"250\"") != std::string::npos &&
              encounters.find("\"outgoing_heal_events\":\"1\"") != std::string::npos &&
              encounters.find("\"event_type_counts\":{\"damage\":\"1\",\"heal\":\"1\",\"buff_add\":\"1\",\"buff_remove\":\"1\"}") != std::string::npos,
          "schema v3 encounter summaries should persist healing and event type counts");
    Check(events.find("\"schema_version\":3") != std::string::npos,
          "schema v3 event records should be emitted");
    Check(encounters.find("\"critical_data_status\":\"unavailable\"") !=
              std::string::npos &&
          encounters.find("\"damage_flag_histogram\":[{\"flags\":16,\"count\":\"1\"}]") !=
              std::string::npos,
          "encounter metadata should explain unavailable critical data with raw flags");
    Check(encounters.find("\"confirmed_controlled_players\":[{\"actor_id\":1") !=
              std::string::npos &&
          encounters.find("\"maximum_ability_id\":1") != std::string::npos &&
          encounters.find("\"damage_share\":100") != std::string::npos,
          "encounter summary should persist report-local controlled player and ability ids");
    Check(encounters.find("\"service_unavailable_intervals\":[{\"start_offset_us\":\"3000000\"") !=
              std::string::npos && encounters.find("\"complete\":false") != std::string::npos,
          "service gaps should be persisted and make the encounter incomplete");
    Check(encounters.find("\"missing_sequence_ranges\":[{\"first\":\"92\",\"last\":\"94\"}]") !=
              std::string::npos,
          "missing sequence ranges should be persisted exactly");
    Check(encounters.find("\"open_missing_sequence_start\":\"95\"") !=
              std::string::npos &&
          encounters.find("\"service_intervals_truncated\":true") !=
              std::string::npos,
          "open cursor gaps and truncated interval lists should be explicit");
    Check(ReadAll(report_path / "actors.jsonl").find(
              "\"resolution_status\":\"resolved\"") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"resolution_status\":\"resolved\"") != std::string::npos,
          "actor and ability dictionaries should persist resolution state");
    Check(ReadAll(report_path / "actors.jsonl").find(
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
    Check(ReadAll(report_path / "actors.jsonl").find(
              "\"actor_id\":0") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"ability_id\":0") != std::string::npos,
          "dictionary id zero should declare the synthetic Other bucket");

    DamageLogRepository reopened(root);
    std::string recover_error;
    Check(reopened.Recover(recover_error), "closed report should recover");
    const auto history = reopened.History();
    Check(history.size() == 1, "history should contain the report");
    if (!history.empty()) {
        Check(history[0].report_id == report_id && !history[0].open,
              "history should preserve report identity and closed state");
        Check(history[0].world == Handle{9, 2},
              "history should preserve the report World identity");
        Check(history[0].encounter_count == 1 && history[0].event_count == 4,
              "history should expose encounter and event counts");
        Check(history[0].encounters.size() == 1 &&
                  history[0].encounters[0].end_reason == "inactivity" &&
                  history[0].encounters[0].outgoing_damage == 100,
              "recovered history should include encounter summaries");
    }
    {
        std::ofstream marker(root / "keep.txt");
        marker << "keep";
    }
    std::string delete_error;
    Check(!reopened.DeleteReport(
              nte::account::Unknown(), "../outside", delete_error) &&
              std::filesystem::exists(root / "keep.txt"),
          "invalid report ids must not escape the log root");
    delete_error.clear();
    Check(reopened.DeleteReport(
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
    Check(repository.Recover(error), "source reuse fixture repository should initialize");
    Check(repository.BeginReport(ReportOpen{
              report_id, world, 1'700'000'100'000, "nte-build-test"}, error),
        "source reuse fixture report should open");
    Check(repository.BeginEncounter(EncounterOpen{
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
    Check(repository.Append(LoggedCombatEvent{
              first, 1'700'000'100'200, 200'000, true}, error),
        "unresolved event name should append");

    auto second = first;
    second.sequence = 2;
    second.final_value = 200;
    second.source_name = "GE_Player_Shinku_Melee1_1_Damage";
    second.source_display_name = "普通攻击：特种格斗术";
    second.source_name_origin = "sdk";
    Check(repository.Append(LoggedCombatEvent{
              second, 1'700'000'100'300, 300'000, true}, error),
        "later SDK name completion should append");

    Check(repository.EndEncounter(EncounterClose{encounter_id, "manual_end",
              1'700'000'101'000, 1'000'000, 300, 0, 300.0, 1.0, {}}, error),
        "source reuse fixture encounter should close");
    Check(repository.CloseReport(ReportClose{
              "plugin_stop", 1'700'000'101'100, false}, error),
        "source reuse fixture report should close");

    const auto report = UnknownReports(root) / report_id;
    const std::string abilities = ReadAll(report / "abilities.jsonl");
    const std::string events = ReadAll(report / "events-000001.jsonl");
    const std::string encounters = ReadAll(report / "encounters.jsonl");
    Check(abilities.find("\"ability_id\":1") != std::string::npos &&
              abilities.find("\"ability_id\":2") == std::string::npos &&
              abilities.find("GE_Player_Shinku_Melee1_1_Damage") != std::string::npos &&
              abilities.find("\"name_origin\":\"sdk\"") != std::string::npos,
        "the local ability index should be completed in place by the SDK name");
    Check(events.find("\"ability_id\":1") != std::string::npos &&
              events.find("\"ability_id\":2") == std::string::npos,
        "events before and after name completion should retain one local ability id");
    Check(encounters.find(
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
    Check(worker.Start(), "log worker should start");
    Check(worker.Submit(ReportOpen{report_id, {1, 1}, 1000, "build"}),
          "report command should enqueue");
    Check(worker.Submit(EncounterOpen{encounter_id, 1100, 100}),
          "encounter command should enqueue");
    DamageEvent event;
    event.sequence = 1;
    event.world = {1, 1};
    event.attacker = {2, 1};
    event.victim = {3, 1};
    event.name_id = 4;
    event.final_value = 5;
    Check(worker.Submit(LoggedCombatEvent{event, 1200, 200, true, encounter_id}),
          "event command should enqueue");
    Check(worker.Submit(EncounterClose{
              encounter_id, "inactivity", 2100, 1100, 5, 0, 5.0, 1.0, {}}),
          "encounter close should enqueue");
    event.sequence = 2;
    Check(!worker.Submit(LoggedCombatEvent{
              event, 2200, 1200, true, encounter_id}),
          "worker should close the event gate before processing the close command");
    std::string flush_error;
    Check(worker.FlushAndWait(std::chrono::seconds(2), flush_error),
          "encounter close should be durably flushed");
    Check(std::filesystem::exists(
              UnknownReports(root) / report_id / "encounters.jsonl"),
          "flush barrier should persist the encounter summary before returning");
    const auto event_path = UnknownReports(root) / report_id /
        "events-000001.jsonl";
    const auto closed_events = ReadAll(event_path);
    Check(worker.Submit(ReportClose{"plugin_stop", 2200, false}),
          "report close should enqueue");
    worker.Stop();
    Check(std::filesystem::exists(
              event_path),
          "worker stop should drain queued log commands");
    Check(ReadAll(event_path) == closed_events,
          "event segment must remain immutable after the encounter close barrier");
    const auto status = worker.Status();
    Check(!status.running && status.queue_depth == 0 && status.last_error.empty(),
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
        Check(repository.Recover(error), "recovery test repository should initialize");
        Check(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
              "recovery test report should open");
    }
    {
        DamageLogRepository repository(root);
        std::string error;
        Check(repository.Recover(error), "open report should recover as unclean");
        const auto history = repository.History();
        Check(history.size() == 1 && !history[0].open && !history[0].complete &&
                  history[0].close_reason == "unclean_shutdown",
              "recovery should expose unclean shutdown in history");
    }
    const auto report = ReadAll(UnknownReports(root) / report_id / "report.json");
    Check(report.find("\"open\":false") != std::string::npos &&
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
    Check(repository.Recover(error), "volume repository should initialize");
    Check(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
          "volume report should open");
    Check(repository.BeginEncounter(EncounterOpen{encounter_id, 1000, 0}, error),
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
    Check(appended, "100,000 healthy events should append without loss");
    Check(repository.EndEncounter(EncounterClose{
              encounter_id, "inactivity", 101000, 100'000'000,
              500'000, 0, 5000.0, 100.0, {}}, error),
          "volume encounter should close");
    Check(repository.CloseReport(ReportClose{"plugin_stop", 101001, false}, error),
          "volume report should close");
    const auto history = repository.History();
    Check(history.size() == 1 && history[0].event_count == 100'000,
          "history should retain the exact event count");
    const auto report_path = UnknownReports(root) / report_id;
    Check(std::filesystem::exists(report_path / "events-000002.jsonl"),
          "large capture should create multiple event segments");
    const std::string index = ReadAll(report_path / "index.json");
    Check(index.find("\"content_sha256\":\"") != std::string::npos &&
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
        Check(repository.Recover(error), "switch repository should initialize");
        Check(repository.BeginReport(
            ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
            "switch report should open");
        Check(repository.BeginEncounter(
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
        Check(repository.Append(
            LoggedCombatEvent{event, 1100, 100, true}, error),
            "switch event should append");
        Check(repository.EndEncounter(EncounterClose{
            encounter_id, close_reason, 1200, 200,
            event.final_value, 0, 1.0, 0.2, {}}, error),
            "switch encounter should close");
        Check(repository.CloseReport(
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
    Check(old_events.find("\"sequence\":\"101\"") != std::string::npos &&
            old_events.find("\"sequence\":\"202\"") == std::string::npos,
        "old root should contain only pre-switch events");
    Check(new_events.find("\"sequence\":\"202\"") != std::string::npos &&
            new_events.find("\"sequence\":\"101\"") == std::string::npos,
        "new root should contain only post-switch events");
    Check(ReadAll(UnknownReports(old_root) / old_report / "report.json").find(
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
    Check(repository.Recover(error), "single-encounter repository should initialize");
    Check(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
        "single-encounter report should open");
    Check(repository.BeginEncounter(EncounterOpen{encounter_id, 1000, 0}, error),
        "single encounter should open");

    DamageEvent event;
    event.sequence = 1;
    event.world = {1, 1};
    event.attacker = {2, 1};
    event.victim = {3, 1};
    event.final_value = 10;
    LoggedCombatEvent logged{event, 1100, 100, true};
    logged.encounter_id = encounter_id;
    Check(repository.Append(logged, error),
        "event carrying the active encounter token should append");
    logged.encounter_id = other_encounter;
    Check(!repository.Append(logged, error),
        "event carrying a stale encounter token must be rejected");

    Check(repository.EndEncounter(EncounterClose{
        encounter_id, "inactivity", 1200, 200, 10, 0, 10.0, 0.2, {}}, error),
        "single encounter should close");
    logged.encounter_id = encounter_id;
    Check(!repository.Append(logged, error),
        "closed encounter must reject late events");
    Check(!repository.BeginEncounter(EncounterOpen{other_encounter, 1300, 300}, error),
        "a report must not accept a second encounter");
    Check(repository.CloseReport(ReportClose{"inactivity", 1200, false}, error),
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
    Check(repository.Recover(error), "rollback repository should initialize");
    Check(!repository.BeginReport(
            ReportOpen{blocked_report, {1, 1}, 1000, "build"}, error),
        "invalid report filesystem should fail initialization");
    Check(!repository.HasOpenReport(),
        "failed report initialization must not leave an active repository state");
    Check(repository.BeginReport(
            ReportOpen{replacement_report, {1, 1}, 1100, "build"}, error),
        "repository should accept a replacement report after rollback");
    Check(repository.CloseReport(ReportClose{"test", 1200, false}, error),
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
    Check(repository.Recover(error), "abort repository should initialize");
    Check(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error) &&
            repository.BeginEncounter(EncounterOpen{encounter_id, 1000, 0}, error),
        "abort fixture should open an active encounter");
    Check(repository.CloseReport(ReportClose{
              "write_failure", 1200, true, true, "encounter close failed"}, error),
        "incomplete report close should forcibly seal an active encounter");
    const auto report = ReadAll(UnknownReports(root) / report_id / "report.json");
    const auto encounters = ReadAll(
        UnknownReports(root) / report_id / "encounters.jsonl");
    Check(report.find("\"open\":false") != std::string::npos &&
            report.find("\"complete\":false") != std::string::npos &&
            report.find("\"close_reason\":\"unclean_shutdown\"") !=
                std::string::npos,
        "aborted report should persist a closed incomplete state");
    Check(encounters.find("\"encounter_id\":\"" + encounter_id + "\"") !=
              std::string::npos &&
            encounters.find("\"complete\":false") != std::string::npos,
        "aborted report should retain a visible incomplete encounter summary");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
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
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All damage log tests passed\n";
    return 0;
}
