#include "overlay_protocol.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {

using nte::combat_stats::NteOverlaySnapshot;

static_assert(std::is_copy_constructible_v<NteOverlaySnapshot>);
static_assert(!std::is_copy_assignable_v<NteOverlaySnapshot>);

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

NteOverlaySnapshot ExampleSnapshot() {
    NteOverlaySnapshot::Data message;
    message.producer_instance_id = "6ee28b4d-7039-4bc4-b068-1468f280745d";
    message.snapshot_sequence = std::numeric_limits<std::uint64_t>::max();
    message.latest_event_sequence = std::numeric_limits<std::uint64_t>::max() - 1;
    message.sent_utc_ms = 1'725'000'000'123;
    message.capture_available = true;
    message.capture_status = "available";
    message.control_status = "manual_end_completed";
    message.account = *nte::account::FromUidText("216065736008");
    message.report_id = "report-id";
    message.encounter_id = "encounter-id";
    message.primary_target = "训练目标·甲";
    message.websocket_port = 14514;
    message.connected_clients = 1;
    message.published_messages = 22;
    message.transport_dropped_messages = 3;
    message.combat.state = nte::combat_stats::EncounterState::Active;
    message.combat.duration_seconds = 12.5;
    message.combat.outgoing_damage = std::numeric_limits<std::int64_t>::max();
    message.combat.incoming_damage = 45;
    message.combat.outgoing_healing = 600;
    message.combat.incoming_healing = 200;
    message.combat.outgoing_heal_events = 3;
    message.combat.incoming_heal_events = 2;
    message.combat.encounter_hps = 48.0;
    message.combat.encounter_received_hps = 16.0;
    message.combat.outgoing_positive_hits = 2;
    message.combat.encounter_dps = 1234.5;
    message.combat.rolling_dps = 500.25;
    message.combat.hp = 88.0;
    message.combat.max_hp = 100.0;
    message.combat.shield = 9.0;
    message.combat.quality.partial = true;
    message.combat.critical_data_status = nte::combat_stats::CriticalDataStatus::Partial;
    message.combat.damage_flag_histogram = {{0, 1}, {16, 2}};
    nte::combat_stats::PlayerRow player;
    player.identity_key = "path:/Game/角色/player_001_C";
    player.handle = {std::numeric_limits<std::uint64_t>::max(), 7};
    player.object_path = "/Game/角色/player_001_C";
    player.attribute = "INCANTATION";
    player.label = "角色甲";
    player.display_name = "角色甲";
    player.name_origin = "resource";
    player.name_resource_version = "2026.08.28";
    player.damage = 700;
    player.positive_hits = 2;
    player.critical_hits = 1;
    player.head_hits = 1;
    player.weak_hits = 0;
    player.maximum_hit = 500;
    player.maximum_source_id = std::numeric_limits<std::uint64_t>::max() - 2;
    player.maximum_source_name = "终结技\"A\"";
    player.maximum_source_display_name = "终结技甲";
    player.maximum_source_resource_version = "2026.08.28";
    player.damage_share = 70.0;
    player.encounter_dps = 56.0;
    player.active_dps = 70.0;
    player.rolling_dps = 40.0;
    player.incoming_damage = 45;
    player.incoming_positive_hits = 3;
    player.maximum_incoming_hit = 20;
    player.incoming_share = 100.0;
    player.healing_done = 600;
    player.healing_done_events = 3;
    player.maximum_heal = 300;
    player.healing_share = 75.0;
    player.encounter_hps = 48.0;
    player.healing_received = 200;
    player.healing_received_events = 2;
    player.maximum_heal_received = 120;
    player.healing_received_share = 25.0;
    player.healing_received_hps = 16.0;
    message.combat.players.push_back(std::move(player));
    nte::combat_stats::RecentEvent event;
    event.sequence = 99;
    event.tick_sequence = 100;
    event.flags = 0x0f;
    event.direction = nte::combat_stats::EventDirection::Outgoing;
    event.name_id = std::numeric_limits<std::uint64_t>::max() - 3;
    event.attacker = {91, 2};
    event.victim = {92, 3};
    event.final_value = 500;
    event.received_utc_ms = message.sent_utc_ms;
    event.received_offset_us = 2'500'000;
    event.source_name = "Skill";
    event.source_display_name = "技能甲";
    event.attacker_name = "Player";
    event.attacker_display_name = "角色甲";
    event.victim_name = "Enemy";
    message.combat.recent_events.push_back(std::move(event));
    for (const auto kind : {nte::combat_stats::CombatEventKind::Heal,
             nte::combat_stats::CombatEventKind::BuffAdd,
             nte::combat_stats::CombatEventKind::BuffRemove}) {
        auto extra = message.combat.recent_events.front();
        extra.sequence += message.combat.recent_events.size();
        extra.kind = kind;
        extra.duration_seconds = 8.5F;
        extra.stack_count = 2;
        message.combat.recent_events.push_back(std::move(extra));
    }
    message.combat.controlled_character_count = 1;
    return NteOverlaySnapshot(std::move(message));
}

void RoundTripPreservesVersionedNteSnapshot() {
    const auto expected = ExampleSnapshot();
    const std::string json = nte::combat_stats::SerializeOverlaySnapshot(expected);
    Check(json.find("CombatData") == std::string::npos,
          "NTE protocol must not emit ACT CombatData");
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    Check(parsed.has_value(), "valid snapshot should parse");
    Check(error.empty(), "valid snapshot should not produce an error");
    if (!parsed) return;
    Check(parsed->snapshot_sequence == expected.snapshot_sequence,
          "uint64 snapshot sequence must retain decimal precision");
    Check(parsed->combat.outgoing_damage == expected.combat.outgoing_damage,
          "int64 damage must retain decimal precision");
    Check(parsed->combat.outgoing_healing == 600 &&
              parsed->combat.incoming_healing == 200 &&
              parsed->combat.encounter_received_hps == 16.0,
          "healing output and received summaries should round-trip");
    Check(parsed->combat.critical_data_status ==
              nte::combat_stats::CriticalDataStatus::Partial &&
              parsed->combat.damage_flag_histogram.size() == 2,
          "critical data quality and raw flag histogram should round-trip");
    Check(parsed->primary_target == expected.primary_target,
          "Unicode target names must round-trip");
    Check(parsed->control_status == "manual_end_completed",
          "optional control status should round-trip");
    Check(parsed->account == expected.account,
          "account identity should round-trip through overlay v1");
    Check(parsed->combat.players.size() == 1,
          "player rows should round-trip");
    if (!parsed->combat.players.empty()) {
        const auto& player = parsed->combat.players.front();
        Check(player.handle.id == std::numeric_limits<std::uint64_t>::max(),
              "generation handle ids must retain precision");
        Check(player.maximum_source_name == "终结技\"A\"",
              "ability names must use correct JSON escaping");
        Check(player.display_name == "角色甲" &&
                  player.maximum_source_display_name == "终结技甲" &&
                  player.name_origin == "resource" &&
                  player.name_resource_version == "2026.08.28",
              "optional localized player and source names should round-trip");
        Check(player.incoming_damage == 45 && player.incoming_positive_hits == 3 &&
                  player.maximum_incoming_hit == 20 && player.incoming_share == 100.0,
              "optional incoming player metrics should round-trip");
        Check(player.attribute == "INCANTATION",
              "the resolved element attribute should round-trip with the row");
        Check(player.healing_done == 600 && player.healing_done_events == 3 &&
                  player.healing_received == 200 &&
                  player.healing_received_events == 2 &&
                  player.healing_received_hps == 16.0,
              "healing output and received metrics should round-trip");
    }
    Check(parsed->combat.recent_events.size() == 4 &&
              parsed->combat.recent_events[0].name_id ==
                  std::numeric_limits<std::uint64_t>::max() - 3 &&
              parsed->combat.recent_events[0].attacker ==
                  nte::combat_stats::Handle{91, 2} &&
              parsed->combat.recent_events[0].source_display_name == "技能甲",
          "recent event identifiers should round-trip");
    if (parsed->combat.recent_events.size() == 4) {
        Check(parsed->combat.recent_events[1].kind ==
                  nte::combat_stats::CombatEventKind::Heal &&
                  parsed->combat.recent_events[2].kind ==
                  nte::combat_stats::CombatEventKind::BuffAdd &&
                  parsed->combat.recent_events[3].kind ==
                  nte::combat_stats::CombatEventKind::BuffRemove &&
                  parsed->combat.recent_events[2].duration_seconds == 8.5F &&
                  parsed->combat.recent_events[2].stack_count == 2,
              "all unified event kinds and buff snapshots should round-trip");
    }
}

void ParserAcceptsLegacyNameCatalogVersion() {
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string current = "name_resource_version";
    std::size_t position{};
    while ((position = json.find(current, position)) != std::string::npos) {
        json.replace(position, current.size(), "name_catalog_version");
        position += std::string_view("name_catalog_version").size();
    }
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    Check(parsed.has_value() && error.empty() &&
              parsed->combat.players[0].name_resource_version == "2026.08.28" &&
              parsed->combat.players[0].maximum_source_resource_version ==
                  "2026.08.28",
        "protocol v1 should continue reading the legacy name catalog version field");
}

void ClosingEncounterStateRoundTrips() {
    nte::combat_stats::NteOverlaySnapshot::Data data;
    data.producer_instance_id = "closing-test";
    data.capture_available = true;
    data.capture_status = "available";
    data.healing_capture_status = "unavailable";
    data.combat.state = nte::combat_stats::EncounterState::Closing;
    const nte::combat_stats::NteOverlaySnapshot snapshot(std::move(data));
    const auto json = nte::combat_stats::SerializeOverlaySnapshot(snapshot);
    Check(json.find("\"state\":\"closing\"") != std::string::npos,
        "closing state should be serialized explicitly");
    Check(json.find("\"healing_capture_status\":\"unavailable\"") !=
              std::string::npos,
        "overlay protocol should disclose unavailable healing capture");
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    Check(parsed && parsed->combat.state == nte::combat_stats::EncounterState::Closing,
        "closing state should round-trip through the overlay protocol");
    Check(parsed && parsed->healing_capture_status == "unavailable",
        "healing capture status should round-trip through the overlay protocol");
}

void ParserDefaultsMissingOptionalExtensions() {
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const auto incoming = json.find("\"incoming_metrics\"");
    Check(incoming != std::string::npos, "fixture should contain incoming extension");
    if (incoming != std::string::npos) {
        const auto begin = json.rfind(',', incoming);
        const auto end = json.find('}', incoming);
        json.erase(begin, end - begin + 1);
    }
    const auto events = json.find(",\"recent_events\"");
    Check(events != std::string::npos, "fixture should contain recent event extension");
    if (events != std::string::npos) {
        const auto end = json.find(",\"report_id\"", events);
        json.erase(events, end - events);
    }
    const std::string control = ",\"control_status\":\"manual_end_completed\"";
    const auto control_at = json.find(control);
    Check(control_at != std::string::npos, "fixture should contain control status extension");
    if (control_at != std::string::npos) json.erase(control_at, control.size());
    const std::string account =
        "\"account\":{\"source\":\"ui-role-id\",\"uid\":\"216065736008\"},";
    const auto account_at = json.find(account);
    Check(account_at != std::string::npos, "fixture should contain account extension");
    if (account_at != std::string::npos) json.erase(account_at, account.size());
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    Check(parsed.has_value(), "snapshots without optional extensions should remain compatible");
    if (parsed) {
        Check(parsed->combat.players[0].incoming_damage == 0 &&
                  parsed->combat.recent_events.empty() && parsed->control_status.empty() &&
                  parsed->account == nte::account::Unknown(),
              "missing optional extensions should use zero and empty defaults");
    }
}

void ParserRejectsWrongProtocolAndAcceptsUnknownFields() {
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::size_t insertion = json.find('{') + 1;
    json.insert(insertion, "\"future_field\":{\"ignored\":true},");
    std::string error;
    Check(nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unknown fields should be ignored");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string channel = "local.nte.nte-combat-tracker.overlay";
    json.replace(json.find(channel), channel.size(), "foreign.channel");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "foreign channels must be rejected");
}

void ParserRejectsOversizedMessages() {
    std::string oversized(64U * 1024U + 1U, 'x');
    std::string error;
    Check(!nte::combat_stats::ParseOverlaySnapshot(oversized, error).has_value(),
          "messages over 64 KiB must be rejected");
}

void ParserRejectsForeignSemantics() {
    std::string error;
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string valid_type = "\"message_type\":\"snapshot\"";
    json.replace(json.find(valid_type), valid_type.size(),
        "\"message_type\":\"foreign\"");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unknown message types should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string basis = "\"timestamp_basis\":\"client_receive\"";
    json.replace(json.find(basis), basis.size(),
        "\"timestamp_basis\":\"server\"");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "non-client timestamp semantics should be rejected");
}

void ParserRejectsMissingRequiredFieldsAndUnsupportedVersions() {
    std::string error;
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string required = "\"producer_instance_id\":\"6ee28b4d-7039-4bc4-b068-1468f280745d\",";
    json.erase(json.find(required), required.size());
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "missing required fields should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string schema = "\"schema_version\":2";
    json.replace(json.find(schema), schema.size(), "\"schema_version\":1");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unsupported schema major versions should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string metric = "\"metric_version\":1";
    json.replace(json.find(metric), metric.size(), "\"metric_version\":2");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unsupported metric major versions should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string required_rate = "\"critical_rate\":50.0,";
    json.erase(json.find(required_rate), required_rate.size());
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "missing required derived player metrics should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string valid_rate = "\"critical_rate\":50.0";
    json.replace(json.find(valid_rate), valid_rate.size(), "\"critical_rate\":49.0");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "inconsistent derived player metrics should be rejected");
}

void DerivedCriticalRateCannotExceedPositiveHits() {
    auto snapshot = ExampleSnapshot();
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(snapshot);
    const std::string hits = "\"positive_hits\":\"2\"";
    json.replace(json.find(hits), hits.size(), "\"positive_hits\":\"0\"");
    std::string error;
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "critical count with zero positive hits should fail derived-rate validation");

    json = nte::combat_stats::SerializeOverlaySnapshot(snapshot);
    const std::string crits = "\"critical_hits\":\"1\"";
    json.replace(json.find(crits), crits.size(), "\"critical_hits\":\"3\"");
    const std::string rate = "\"critical_rate\":50.0";
    json.replace(json.find(rate), rate.size(), "\"critical_rate\":150.0");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "critical hit count greater than positive hits must be rejected explicitly");
}

}  // namespace

int main() {
    RoundTripPreservesVersionedNteSnapshot();
    ParserRejectsWrongProtocolAndAcceptsUnknownFields();
    ParserRejectsOversizedMessages();
    ParserRejectsForeignSemantics();
    ParserRejectsMissingRequiredFieldsAndUnsupportedVersions();
    ParserDefaultsMissingOptionalExtensions();
    ParserAcceptsLegacyNameCatalogVersion();
    ClosingEncounterStateRoundTrips();
    DerivedCriticalRateCannotExceedPositiveHits();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All overlay protocol tests passed\n";
    return 0;
}
