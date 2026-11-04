#include "combat_export.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void ExportContainsEveryUniqueEventInSequenceOrder() {
    nte::combat_stats::overlay::EncounterDetail detail;
    detail.summary.report_id = "11111111-1111-1111-1111-111111111111";
    detail.summary.encounter_id = "22222222-2222-2222-2222-222222222222";
    detail.summary.complete = true;
    for (std::uint64_t sequence = 130; sequence >= 1; --sequence) {
        nte::combat_stats::overlay::CombatEventDetail event;
        event.encounter_id = detail.summary.encounter_id;
        event.sequence = sequence;
        detail.events.push_back(event);
    }
    detail.events.push_back(detail.events.back());

    const auto document = nlohmann::json::parse(
        nte::combat_stats::overlay::SerializeCombatExport(detail, "0.5.0", 1));
    Check(document.at("schema_version") == 3 &&
            document.at("account").at("uid").is_null() &&
            document.at("account").at("source") == "unavailable",
        "unknown account exports should use schema v3 with a null UID");
    const auto& events = document.at("events");
    Check(events.size() == 130,
        "export should include every unique event beyond the overlay 128-event limit");
    Check(events.front().at("sequence") == "1" &&
            events.back().at("sequence") == "130",
        "export events should be sorted by sequence");
}

void ExportWriterUsesGeneratedCollisionSafeFilenames() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-combat-export-test-" + std::to_string(nonce));
    nte::combat_stats::overlay::EncounterDetail detail;
    detail.summary.report_id = "11111111-1111-1111-1111-111111111111";
    detail.summary.encounter_id = "22222222-2222-2222-2222-222222222222";
    detail.summary.started_utc_ms = 1'700'000'000'000;
    detail.summary.complete = true;
    std::filesystem::path first;
    std::filesystem::path second;
    std::string error;
    Check(nte::combat_stats::overlay::WriteCombatExport(
            root, detail, "0.5.0", 1, first, error) && error.empty(),
        "writer should create a JSON export in a missing absolute directory");
    Check(nte::combat_stats::overlay::WriteCombatExport(
            root, detail, "0.5.0", 2, second, error) && error.empty(),
        "writer should avoid overwriting an existing export");
    Check(first != second && first.extension() == ".json" &&
            first.filename().wstring().starts_with(L"NTECombat-") &&
            first.filename().wstring().find(L"22222222") != std::wstring::npos &&
            second.stem().wstring().ends_with(L"-2"),
        "generated names should carry time, encounter id, and collision suffix");
    Check(std::filesystem::is_regular_file(first) &&
            std::filesystem::is_regular_file(second) &&
            !std::filesystem::exists(first.wstring() + L".tmp"),
        "atomic export should publish both files without leaving temporary files");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void CompletedEncounterSerializesAsVersionedNteLogsImportDocument() {
    nte::combat_stats::overlay::EncounterDetail detail;
    detail.summary.report_id = "11111111-1111-1111-1111-111111111111";
    detail.summary.encounter_id = "22222222-2222-2222-2222-222222222222";
    detail.summary.game_build = "2026.08";
    detail.summary.account = *nte::account::FromUidText("216065736008");
    detail.summary.started_utc_ms = 1'700'000'000'000;
    detail.summary.closed_utc_ms = 1'700'000'012'500;
    detail.summary.duration_seconds = 12.5;
    detail.summary.outgoing_damage = 9'007'199'254'740'993LL;
    detail.summary.incoming_damage = 321;
    detail.summary.encounter_dps = 720'575'940'379'279.4;
    detail.summary.outgoing_healing = 450;
    detail.summary.incoming_healing = 200;
    detail.summary.outgoing_heal_events = 2;
    detail.summary.incoming_heal_events = 1;
    detail.summary.encounter_hps = 36.0;
    detail.summary.encounter_received_hps = 16.0;
    detail.summary.event_type_counts = {1, 1, 1, 1};
    detail.summary.end_reason = "inactivity";
    detail.summary.complete = true;
    detail.summary.service_unavailable_intervals.push_back({100, 200});
    detail.summary.missing_sequence_ranges.push_back({7, 9});
    detail.summary.open_missing_sequence_start = 11;
    detail.summary.service_intervals_truncated = true;
    detail.summary.missing_ranges_truncated = true;

    nte::combat_stats::PlayerRow player;
    player.identity_key = "path:/Game/Player_中文";
    player.handle = {9'007'199'254'740'993ULL, 4};
    player.object_path = "/Game/Player_中文";
    player.label = "角色\"甲";
    player.display_name = "角色甲";
    player.damage = 9'007'199'254'740'993LL;
    player.positive_hits = 1;
    player.maximum_hit = 9'007'199'254'740'993LL;
    detail.players.push_back(player);

    nte::combat_stats::overlay::CombatEventDetail event;
    event.encounter_id = detail.summary.encounter_id;
    event.sequence = 9'007'199'254'740'993ULL;
    event.tick_sequence = 8;
    event.flags = nte::combat_stats::kDamageFlagCritical;
    event.world = {10, 11};
    event.attacker_actor_id = 12;
    event.victim_actor_id = 13;
    event.ability_id = 14;
    event.name_id = 9'007'199'254'740'995ULL;
    event.attacker = player.handle;
    event.victim = {15, 16};
    event.player_related = true;
    event.final_value = 99;
    event.basic_value = 100;
    event.value = 101;
    event.hit_location = {1.25, 2.5, 3.75};
    event.damage_type = 17;
    event.display_type = 18;
    event.reaction_type = 19;
    event.reaction_display_type = 20;
    event.received_utc_ms = 1'700'000'001'000;
    event.received_offset_us = 1'234'567;
    event.source = "/Game/Ability/Test";
    event.source_display_name = "测试技能";
    event.attacker_name = player.object_path;
    event.attacker_display_name = player.display_name;
    event.victim_name = "/Game/Enemy/Test";
    event.victim_display_name = "测试敌人";
    detail.events.push_back(event);
    auto heal = event;
    heal.sequence += 1;
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    detail.events.push_back(heal);
    auto buff_add = event;
    buff_add.sequence += 2;
    buff_add.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    buff_add.duration_seconds = 9.5F;
    buff_add.stack_count = 4;
    detail.events.push_back(buff_add);
    auto buff_remove = buff_add;
    buff_remove.sequence += 1;
    buff_remove.kind = nte::combat_stats::CombatEventKind::BuffRemove;
    detail.events.push_back(buff_remove);

    const std::string json = nte::combat_stats::overlay::SerializeCombatExport(
        detail, "0.5.0", 1'700'000'020'000);

    Check(json.find("\"format\": \"nte-combat-export\"") != std::string::npos,
        "export should identify the stable document format");
    Check(json.find("\"schema_version\": 3") != std::string::npos &&
            json.find("\"metric_version\": 1") != std::string::npos,
        "export should version both schema and metrics");
    Check(json.find("\"9007199254740993\"") != std::string::npos,
        "64-bit integers should be emitted as decimal strings");
    Check(json.find("角色甲") != std::string::npos &&
            json.find("\"hit_location\": [") != std::string::npos &&
            json.find("\"reaction_display_type\": 20") != std::string::npos,
        "export should retain Unicode names and complete event metadata");
    const auto document = nlohmann::json::parse(json);
    Check(document.at("account").at("uid") == "216065736008" &&
            document.at("account").at("source") == "ui-role-id",
        "export should carry the encounter account UID once at document scope");
    Check(document.at("capture").at("healing_capture_status") == "unavailable",
        "export should distinguish unavailable healing capture from zero healing");
    const auto& encounter = document.at("encounter");
    Check(encounter.at("service_unavailable_intervals").at(0)
                .at("start_offset_us") == "100" &&
            encounter.at("missing_sequence_ranges").at(0).at("first") == "7" &&
            encounter.at("open_missing_sequence_start") == "11" &&
            encounter.at("quality").at("missing_ranges_truncated") == true,
        "export should retain capture gaps and truncation quality metadata");
    Check(!json.empty() && json.back() == '\n',
        "pretty-printed export should end with a newline");
    Check(document.at("encounter").at("summary").at("outgoing_healing") == "450" &&
              document.at("encounter").at("summary").at("event_type_counts")
                  .at("buff_remove") == "1" &&
              document.at("events").at(1).at("event_type") == "heal" &&
              document.at("events").at(2).at("event_type") == "buff_add" &&
              document.at("events").at(3).at("event_type") == "buff_remove",
          "schema v3 exports should include healing summaries and every event kind");
}

}  // namespace

int main() {
    CompletedEncounterSerializesAsVersionedNteLogsImportDocument();
    ExportContainsEveryUniqueEventInSequenceOrder();
    ExportWriterUsesGeneratedCollisionSafeFilenames();
    std::cout << "All combat export tests passed\n";
}
