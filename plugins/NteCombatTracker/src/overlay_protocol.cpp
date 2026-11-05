#include "overlay_protocol.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace nte::combat_stats {
namespace {

using Json = nlohmann::json;

template <typename Integer>
std::string Decimal(const Integer value) {
    return std::to_string(value);
}

template <typename Integer>
Integer ParseDecimal(const Json& value, const char* field) {
    if (!value.is_string()) throw std::runtime_error(std::string(field) + " must be a string");
    const std::string& text = value.get_ref<const std::string&>();
    Integer result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string(field) + " is not a decimal integer");
    }
    return result;
}

const char* StateName(const EncounterState state) noexcept {
    switch (state) {
    case EncounterState::Idle: return "idle";
    case EncounterState::Active: return "active";
    case EncounterState::Closing: return "closing";
    case EncounterState::Completed: return "completed";
    }
    return "idle";
}

const char* DirectionName(const EventDirection direction) noexcept {
    switch (direction) {
    case EventDirection::Visible: return "visible";
    case EventDirection::Outgoing: return "outgoing";
    case EventDirection::Incoming: return "incoming";
    case EventDirection::Both: return "both";
    }
    return "visible";
}

EventDirection ParseDirection(const std::string& direction) {
    if (direction == "visible") return EventDirection::Visible;
    if (direction == "outgoing") return EventDirection::Outgoing;
    if (direction == "incoming") return EventDirection::Incoming;
    if (direction == "both") return EventDirection::Both;
    throw std::runtime_error("recent_events.direction is invalid");
}

EncounterState ParseState(const std::string& state) {
    if (state == "idle") return EncounterState::Idle;
    if (state == "active") return EncounterState::Active;
    if (state == "closing") return EncounterState::Closing;
    if (state == "completed") return EncounterState::Completed;
    throw std::runtime_error("encounter.state is invalid");
}

CriticalDataStatus ParseCriticalDataStatus(const std::string& status) {
    if (status == "available") return CriticalDataStatus::Available;
    if (status == "partial") return CriticalDataStatus::Partial;
    if (status == "unavailable") return CriticalDataStatus::Unavailable;
    throw std::runtime_error("quality.critical_data_status is invalid");
}

void ValidateRate(const Json& value, const double expected, const char* field) {
    const double actual = value.get<double>();
    if (!std::isfinite(actual) || std::abs(actual - expected) > 0.000001) {
        throw std::runtime_error(std::string(field) + " is inconsistent with hit counts");
    }
}

Json PlayerToJson(const PlayerRow& row) {
    return Json{
        {"identity_key", row.identity_key},
        {"handle", {{"id", Decimal(row.handle.id)},
                    {"generation", Decimal(row.handle.generation)}}},
        {"object_path", row.object_path},
        {"label", row.label},
        {"attribute", row.attribute},
        {"display_name", row.display_name},
        {"name_origin", row.name_origin},
        {"name_resource_version", row.name_resource_version},
        {"damage", Decimal(row.damage)},
        {"positive_hits", Decimal(row.positive_hits)},
        {"critical_hits", Decimal(row.critical_hits)},
        {"critical_rate", PlayerCriticalRate(row)},
        {"head_hits", Decimal(row.head_hits)},
        {"head_rate", PlayerHeadRate(row)},
        {"weak_hits", Decimal(row.weak_hits)},
        {"weak_rate", PlayerWeakRate(row)},
        {"maximum_hit", Decimal(row.maximum_hit)},
        {"maximum_ability", {{"name_id", Decimal(row.maximum_source_id)},
                             {"name", row.maximum_source_name},
                             {"display_name", row.maximum_source_display_name},
                             {"name_origin", row.maximum_source_name_origin},
                             {"name_resource_version", row.maximum_source_resource_version}}},
        {"damage_share", row.damage_share},
        {"encounter_dps", row.encounter_dps},
        {"active_dps", row.active_dps},
        {"rolling_dps_10s", row.rolling_dps},
        {"incoming_metrics", {
            {"damage", Decimal(row.incoming_damage)},
            {"positive_hits", Decimal(row.incoming_positive_hits)},
            {"maximum_hit", Decimal(row.maximum_incoming_hit)},
            {"share", row.incoming_share}}},
        {"healing", {{"done", Decimal(row.healing_done)},
                     {"done_events", Decimal(row.healing_done_events)},
                     {"maximum", Decimal(row.maximum_heal)},
                     {"share", row.healing_share},
                     {"hps", row.encounter_hps},
                     {"received", Decimal(row.healing_received)},
                     {"received_events", Decimal(row.healing_received_events)},
                      {"maximum_received", Decimal(row.maximum_heal_received)},
                      {"received_share", row.healing_received_share},
                      {"received_hps", row.healing_received_hps}}}};
}

PlayerRow PlayerFromJson(const Json& value) {
    PlayerRow row;
    row.identity_key = value.at("identity_key").get<std::string>();
    const Json& handle = value.at("handle");
    row.handle.id = ParseDecimal<std::uint64_t>(handle.at("id"), "players.handle.id");
    row.handle.generation = ParseDecimal<std::uint64_t>(
        handle.at("generation"), "players.handle.generation");
    row.object_path = value.at("object_path").get<std::string>();
    row.label = value.at("label").get<std::string>();
    row.attribute = value.value("attribute", std::string{});
    row.display_name = value.value("display_name", row.label);
    row.name_origin = value.value("name_origin", std::string{"technical"});
    row.name_resource_version = value.value(
        "name_resource_version",
        value.value("name_catalog_version", std::string{}));
    row.damage = ParseDecimal<std::int64_t>(value.at("damage"), "players.damage");
    row.positive_hits = ParseDecimal<std::uint64_t>(
        value.at("positive_hits"), "players.positive_hits");
    row.critical_hits = ParseDecimal<std::uint64_t>(
        value.at("critical_hits"), "players.critical_hits");
    if (row.critical_hits > row.positive_hits) {
        throw std::runtime_error(
            "players.critical_hits must not exceed players.positive_hits");
    }
    row.head_hits = ParseDecimal<std::uint64_t>(
        value.at("head_hits"), "players.head_hits");
    row.weak_hits = ParseDecimal<std::uint64_t>(
        value.at("weak_hits"), "players.weak_hits");
    ValidateRate(value.at("critical_rate"), PlayerCriticalRate(row),
        "players.critical_rate");
    ValidateRate(value.at("head_rate"), PlayerHeadRate(row),
        "players.head_rate");
    ValidateRate(value.at("weak_rate"), PlayerWeakRate(row),
        "players.weak_rate");
    row.maximum_hit = ParseDecimal<std::int64_t>(
        value.at("maximum_hit"), "players.maximum_hit");
    const Json& ability = value.at("maximum_ability");
    row.maximum_source_id = ParseDecimal<std::uint64_t>(
        ability.at("name_id"), "players.maximum_ability.name_id");
    row.maximum_source_name = ability.at("name").get<std::string>();
    row.maximum_source_display_name = ability.value(
        "display_name", row.maximum_source_name);
    row.maximum_source_name_origin = ability.value(
        "name_origin", std::string{"technical"});
    row.maximum_source_resource_version = ability.value(
        "name_resource_version",
        ability.value("name_catalog_version",
            ability.value("catalog_version", std::string{})));
    row.damage_share = value.at("damage_share").get<double>();
    row.encounter_dps = value.at("encounter_dps").get<double>();
    row.active_dps = value.at("active_dps").get<double>();
    row.rolling_dps = value.at("rolling_dps_10s").get<double>();
    if (const auto incoming = value.find("incoming_metrics"); incoming != value.end()) {
        row.incoming_damage = ParseDecimal<std::int64_t>(
            incoming->at("damage"), "players.incoming_metrics.damage");
        row.incoming_positive_hits = ParseDecimal<std::uint64_t>(
            incoming->at("positive_hits"), "players.incoming_metrics.positive_hits");
        row.maximum_incoming_hit = ParseDecimal<std::int64_t>(
            incoming->at("maximum_hit"), "players.incoming_metrics.maximum_hit");
        row.incoming_share = incoming->at("share").get<double>();
    }
    if (const auto healing = value.find("healing"); healing != value.end()) {
        row.healing_done = ParseDecimal<std::int64_t>(healing->at("done"), "players.healing.done");
        row.healing_done_events = ParseDecimal<std::uint64_t>(healing->at("done_events"), "players.healing.done_events");
        row.maximum_heal = ParseDecimal<std::int64_t>(healing->at("maximum"), "players.healing.maximum");
        row.healing_share = healing->at("share").get<double>();
        row.encounter_hps = healing->at("hps").get<double>();
        row.healing_received = ParseDecimal<std::int64_t>(healing->at("received"), "players.healing.received");
        row.healing_received_events = ParseDecimal<std::uint64_t>(healing->at("received_events"), "players.healing.received_events");
        row.maximum_heal_received = ParseDecimal<std::int64_t>(healing->at("maximum_received"), "players.healing.maximum_received");
        row.healing_received_share = healing->at("received_share").get<double>();
        row.healing_received_hps = healing->value("received_hps", 0.0);
    }
    return row;
}

Json RecentEventToJson(const RecentEvent& event) {
    return Json{
        {"kind", CombatEventKindName(event.kind)},
        {"sequence", Decimal(event.sequence)},
        {"tick_sequence", Decimal(event.tick_sequence)},
        {"flags", event.flags},
        {"direction", DirectionName(event.direction)},
        {"name_id", Decimal(event.name_id)},
        {"source_handle", {{"id", Decimal(event.attacker.id)},
                           {"generation", Decimal(event.attacker.generation)}}},
        {"target_handle", {{"id", Decimal(event.victim.id)},
                           {"generation", Decimal(event.victim.generation)}}},
        {"final_value", Decimal(event.final_value)},
        {"value", Decimal(event.value)},
        {"basic_value", Decimal(event.basic_value)},
        {"hit_location", event.hit_location},
        {"damage_type", event.damage_type},
        {"display_type", event.display_type},
        {"reaction_type", event.reaction_type},
        {"reaction_display_type", event.reaction_display_type},
        {"duration_seconds", event.duration_seconds},
        {"stack_count", event.stack_count},
        {"received_utc_ms", Decimal(event.received_utc_ms)},
        {"received_offset_us", Decimal(event.received_offset_us)},
        {"event_name", event.source_name},
        {"event_display_name", event.source_display_name},
        {"source_path", event.attacker_name},
        {"source_display_name", event.attacker_display_name},
        {"target_path", event.victim_name},
        {"target_display_name", event.victim_display_name}};
}

RecentEvent RecentEventFromJson(const Json& value) {
    RecentEvent event;
    const auto kind = ParseCombatEventKind(value.at("kind").get<std::string>());
    if (!kind) throw std::runtime_error("recent_events.kind is invalid");
    event.kind = *kind;
    event.sequence = ParseDecimal<std::uint64_t>(value.at("sequence"),
        "recent_events.sequence");
    event.tick_sequence = ParseDecimal<std::uint64_t>(value.at("tick_sequence"),
        "recent_events.tick_sequence");
    event.flags = value.at("flags").get<std::uint32_t>();
    event.direction = ParseDirection(value.at("direction").get<std::string>());
    event.name_id = ParseDecimal<std::uint64_t>(
        value.at("name_id"), "recent_events.name_id");
    const auto parse_handle = [](const Json& handle, const char* prefix) {
        return Handle{
            ParseDecimal<std::uint64_t>(handle.at("id"), prefix),
            ParseDecimal<std::uint64_t>(handle.at("generation"), prefix)};
    };
    event.attacker = parse_handle(value.at("source_handle"),
        "recent_events.source_handle");
    event.victim = parse_handle(value.at("target_handle"),
        "recent_events.target_handle");
    event.final_value = ParseDecimal<std::int64_t>(value.at("final_value"),
        "recent_events.final_value");
    event.value = ParseDecimal<std::int64_t>(value.at("value"),
        "recent_events.value");
    event.basic_value = ParseDecimal<std::int64_t>(value.at("basic_value"),
        "recent_events.basic_value");
    if (const auto hit = value.find("hit_location"); hit != value.end() &&
        hit->is_array() && hit->size() == 3) {
        for (std::size_t index{}; index < 3; ++index) {
            event.hit_location[index] = hit->at(index).get<double>();
        }
    }
    event.damage_type = value.value("damage_type", 0U);
    event.display_type = value.value("display_type", 0U);
    event.reaction_type = value.value("reaction_type", 0U);
    event.reaction_display_type = value.value("reaction_display_type", 0U);
    event.duration_seconds = value.value("duration_seconds", 0.0F);
    event.stack_count = value.value("stack_count", 0);
    event.received_utc_ms = ParseDecimal<std::int64_t>(value.at("received_utc_ms"),
        "recent_events.received_utc_ms");
    event.received_offset_us = ParseDecimal<std::uint64_t>(value.at("received_offset_us"),
        "recent_events.received_offset_us");
    event.source_name = value.at("event_name").get<std::string>();
    event.source_display_name = value.value("event_display_name", event.source_name);
    event.attacker_name = value.at("source_path").get<std::string>();
    event.attacker_display_name = value.value(
        "source_display_name", event.attacker_name);
    event.victim_name = value.at("target_path").get<std::string>();
    event.victim_display_name = value.value(
        "target_display_name", event.victim_name);
    return event;
}

}  // namespace

NteOverlaySnapshot::NteOverlaySnapshot(Data data)
    : producer_instance_id(std::move(data.producer_instance_id)),
      snapshot_sequence(data.snapshot_sequence),
      latest_event_sequence(data.latest_event_sequence),
      sent_utc_ms(data.sent_utc_ms),
      producer_stopping(data.producer_stopping),
      capture_available(data.capture_available),
      capture_status(std::move(data.capture_status)),
      healing_capture_status(std::move(data.healing_capture_status)),
      control_status(std::move(data.control_status)),
      account(std::move(data.account)),
      report_id(std::move(data.report_id)),
      encounter_id(std::move(data.encounter_id)),
      primary_target(std::move(data.primary_target)),
      websocket_port(data.websocket_port),
      connected_clients(data.connected_clients),
      published_messages(data.published_messages),
      transport_dropped_messages(data.transport_dropped_messages),
      locale(std::move(data.locale)),
      combat(std::move(data.combat)) {}

std::string SerializeOverlaySnapshot(const NteOverlaySnapshot& snapshot) {
    Json players = Json::array();
    for (const PlayerRow& row : snapshot.combat.players) players.push_back(PlayerToJson(row));
    Json recent_events = Json::array();
    constexpr std::size_t kMaximumRecentEvents = 128;
    const std::size_t first = snapshot.combat.recent_events.size() > kMaximumRecentEvents
        ? snapshot.combat.recent_events.size() - kMaximumRecentEvents : 0;
    for (std::size_t index = first; index < snapshot.combat.recent_events.size(); ++index) {
        recent_events.push_back(RecentEventToJson(snapshot.combat.recent_events[index]));
    }
    Json damage_flag_histogram = Json::array();
    for (const auto& item : snapshot.combat.damage_flag_histogram) {
        damage_flag_histogram.push_back({
            {"flags", item.flags}, {"count", Decimal(item.count)}});
    }
    Json document{
        {"channel", kOverlayChannel},
        {"schema_version", kOverlaySchemaVersion},
        {"metric_version", kMetricVersion},
        {"message_type", snapshot.producer_stopping ? "producer_stopping" : "snapshot"},
        {"producer_instance_id", snapshot.producer_instance_id},
        {"snapshot_sequence", Decimal(snapshot.snapshot_sequence)},
        {"latest_event_sequence", Decimal(snapshot.latest_event_sequence)},
        {"sent_utc_ms", Decimal(snapshot.sent_utc_ms)},
        {"timestamp_basis", "client_receive"},
        {"capture_scope", "player_and_engaged_actors"},
        {"capture", {{"available", snapshot.capture_available},
                     {"status", snapshot.capture_status},
                     {"healing_capture_status", snapshot.healing_capture_status}}},
        {"control_status", snapshot.control_status},
        {"account", {{"uid", snapshot.account.known()
                            ? Json(snapshot.account.uid) : Json{}},
                     {"source", account::SourceName(snapshot.account.source)}}},
        {"report_id", snapshot.report_id},
        {"encounter_id", snapshot.encounter_id},
        {"encounter", {{"state", StateName(snapshot.combat.state)},
                       {"duration_seconds", snapshot.combat.duration_seconds},
                       {"confirmed_controlled_count",
                        Decimal(snapshot.combat.controlled_character_count)}}},
        {"summary", {{"outgoing_damage", Decimal(snapshot.combat.outgoing_damage)},
                     {"incoming_damage", Decimal(snapshot.combat.incoming_damage)},
                     {"outgoing_healing", Decimal(snapshot.combat.outgoing_healing)},
                     {"incoming_healing", Decimal(snapshot.combat.incoming_healing)},
                     {"outgoing_heal_events", Decimal(snapshot.combat.outgoing_heal_events)},
                     {"incoming_heal_events", Decimal(snapshot.combat.incoming_heal_events)},
                     {"encounter_hps", snapshot.combat.encounter_hps},
                     {"encounter_received_hps",
                      snapshot.combat.encounter_received_hps},
                     {"buff_add_events", Decimal(snapshot.combat.buff_add_events)},
                     {"buff_remove_events", Decimal(snapshot.combat.buff_remove_events)},
                     {"outgoing_positive_hits",
                      Decimal(snapshot.combat.outgoing_positive_hits)},
                     {"encounter_dps", snapshot.combat.encounter_dps},
                     {"rolling_dps_10s", snapshot.combat.rolling_dps},
                     {"hp", snapshot.combat.hp},
                     {"max_hp", snapshot.combat.max_hp},
                     {"shield", snapshot.combat.shield},
                     {"primary_target", snapshot.primary_target}}},
        {"players", std::move(players)},
        {"recent_events", std::move(recent_events)},
        {"quality", {{"partial", snapshot.combat.quality.partial},
                     {"overflow", snapshot.combat.quality.overflow},
                     {"cursor_gap", snapshot.combat.quality.cursor_gap},
                     {"aggregate_limit", snapshot.combat.quality.aggregate_truncated},
                     {"critical_data_status",
                      CriticalDataStatusName(snapshot.combat.critical_data_status)},
                     {"damage_flag_histogram", std::move(damage_flag_histogram)}}},
        {"transport", {{"port", snapshot.websocket_port},
                       {"connected_clients", snapshot.connected_clients},
                       {"published_messages", Decimal(snapshot.published_messages)},
                       {"dropped_messages", Decimal(snapshot.transport_dropped_messages)}}},
        {"locale", snapshot.locale}};
    std::string result = document.dump();
    while (result.size() > kMaximumOverlayMessageBytes &&
           !document["recent_events"].empty()) {
        document["recent_events"].erase(document["recent_events"].begin());
        result = document.dump();
    }
    return result;
}

std::optional<NteOverlaySnapshot> ParseOverlaySnapshot(
    const std::string_view text,
    std::string& error) {
    error.clear();
    if (text.size() > kMaximumOverlayMessageBytes) {
        error = "overlay message exceeds 64 KiB";
        return std::nullopt;
    }
    try {
        const Json document = Json::parse(text.begin(), text.end());
        if (document.at("channel").get<std::string>() != kOverlayChannel) {
            throw std::runtime_error("overlay channel is not supported");
        }
        if (document.at("schema_version").get<std::uint32_t>() !=
            kOverlaySchemaVersion) {
            throw std::runtime_error("overlay schema version is not supported");
        }
        if (document.at("metric_version").get<std::uint32_t>() != kMetricVersion) {
            throw std::runtime_error("metric version is not supported");
        }
        if (document.at("timestamp_basis").get<std::string>() != "client_receive") {
            throw std::runtime_error("timestamp basis is not supported");
        }
        const auto capture_scope = document.at("capture_scope").get<std::string>();
        if (capture_scope != "host_visible_during_player_encounter" &&
            capture_scope != "player_and_engaged_actors") {
            throw std::runtime_error("capture scope is not supported");
        }
        const std::string message_type = document.at("message_type").get<std::string>();
        if (message_type != "snapshot" && message_type != "producer_stopping") {
            throw std::runtime_error("overlay message type is not supported");
        }
        NteOverlaySnapshot::Data result;
        result.producer_stopping = message_type == "producer_stopping";
        result.producer_instance_id = document.at("producer_instance_id").get<std::string>();
        result.snapshot_sequence = ParseDecimal<std::uint64_t>(
            document.at("snapshot_sequence"), "snapshot_sequence");
        result.latest_event_sequence = ParseDecimal<std::uint64_t>(
            document.at("latest_event_sequence"), "latest_event_sequence");
        result.sent_utc_ms = ParseDecimal<std::int64_t>(
            document.at("sent_utc_ms"), "sent_utc_ms");
        const Json& capture = document.at("capture");
        result.capture_available = capture.at("available").get<bool>();
        result.capture_status = capture.at("status").get<std::string>();
        result.healing_capture_status = capture.value(
            "healing_capture_status", std::string{"unavailable"});
        if (const auto status = document.find("control_status"); status != document.end()) {
            result.control_status = status->get<std::string>();
        }
        if (const auto value = document.find("account"); value != document.end()) {
            const auto uid = value->find("uid");
            if (uid != value->end() && !uid->is_null()) {
                const auto parsed = account::FromUidText(uid->get<std::string>());
                if (!parsed) throw std::runtime_error("account uid is invalid");
                result.account = *parsed;
            }
        }
        result.report_id = document.at("report_id").get<std::string>();
        result.encounter_id = document.at("encounter_id").get<std::string>();
        const Json& encounter = document.at("encounter");
        result.combat.state = ParseState(encounter.at("state").get<std::string>());
        result.combat.duration_seconds = encounter.at("duration_seconds").get<double>();
        result.combat.controlled_character_count = ParseDecimal<std::size_t>(
            encounter.at("confirmed_controlled_count"),
            "encounter.confirmed_controlled_count");
        const Json& summary = document.at("summary");
        result.combat.outgoing_damage = ParseDecimal<std::int64_t>(
            summary.at("outgoing_damage"), "summary.outgoing_damage");
        result.combat.incoming_damage = ParseDecimal<std::int64_t>(
            summary.at("incoming_damage"), "summary.incoming_damage");
        result.combat.outgoing_healing = ParseDecimal<std::int64_t>(summary.value("outgoing_healing", Json{"0"}), "summary.outgoing_healing");
        result.combat.incoming_healing = ParseDecimal<std::int64_t>(summary.value("incoming_healing", Json{"0"}), "summary.incoming_healing");
        result.combat.outgoing_heal_events = ParseDecimal<std::uint64_t>(summary.value("outgoing_heal_events", Json{"0"}), "summary.outgoing_heal_events");
        result.combat.incoming_heal_events = ParseDecimal<std::uint64_t>(summary.value("incoming_heal_events", Json{"0"}), "summary.incoming_heal_events");
        result.combat.encounter_hps = summary.value("encounter_hps", 0.0);
        result.combat.encounter_received_hps = summary.value(
            "encounter_received_hps", 0.0);
        result.combat.buff_add_events = ParseDecimal<std::uint64_t>(summary.value("buff_add_events", Json{"0"}), "summary.buff_add_events");
        result.combat.buff_remove_events = ParseDecimal<std::uint64_t>(summary.value("buff_remove_events", Json{"0"}), "summary.buff_remove_events");
        result.combat.outgoing_positive_hits = ParseDecimal<std::uint64_t>(
            summary.at("outgoing_positive_hits"), "summary.outgoing_positive_hits");
        result.combat.encounter_dps = summary.at("encounter_dps").get<double>();
        result.combat.rolling_dps = summary.at("rolling_dps_10s").get<double>();
        result.combat.hp = summary.at("hp").get<double>();
        result.combat.max_hp = summary.at("max_hp").get<double>();
        result.combat.shield = summary.at("shield").get<double>();
        result.primary_target = summary.at("primary_target").get<std::string>();
        for (const Json& player : document.at("players")) {
            result.combat.players.push_back(PlayerFromJson(player));
        }
        if (const auto events = document.find("recent_events"); events != document.end()) {
            for (const Json& event : *events) {
                result.combat.recent_events.push_back(RecentEventFromJson(event));
            }
        }
        const Json& quality = document.at("quality");
        result.combat.quality.partial = quality.at("partial").get<bool>();
        result.combat.quality.overflow = quality.at("overflow").get<bool>();
        result.combat.quality.cursor_gap = quality.at("cursor_gap").get<bool>();
        result.combat.quality.aggregate_truncated =
            quality.at("aggregate_limit").get<bool>();
        if (const auto status = quality.find("critical_data_status");
            status != quality.end()) {
            result.combat.critical_data_status =
                ParseCriticalDataStatus(status->get<std::string>());
        }
        if (const auto histogram = quality.find("damage_flag_histogram");
            histogram != quality.end()) {
            for (const auto& item : *histogram) {
                result.combat.damage_flag_histogram.push_back({
                    item.at("flags").get<std::uint32_t>(),
                    ParseDecimal<std::uint64_t>(
                        item.at("count"), "quality.damage_flag_histogram.count")});
            }
        }
        const Json& transport = document.at("transport");
        result.websocket_port = transport.at("port").get<std::uint16_t>();
        result.connected_clients = transport.at("connected_clients").get<std::uint32_t>();
        result.published_messages = ParseDecimal<std::uint64_t>(
            transport.at("published_messages"), "transport.published_messages");
        result.transport_dropped_messages = ParseDecimal<std::uint64_t>(
            transport.at("dropped_messages"), "transport.dropped_messages");
        // Optional so an older producer still parses; the overlay then falls
        // back to its built-in catalog.
        result.locale = document.value("locale", std::string{"zh-CN"});
        return NteOverlaySnapshot(std::move(result));
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

}  // namespace nte::combat_stats
