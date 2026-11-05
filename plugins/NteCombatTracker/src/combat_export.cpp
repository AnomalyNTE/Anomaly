#include "combat_export.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <stdexcept>
#include <windows.h>

namespace nte::combat_stats::overlay {
namespace {

using Json = nlohmann::json;

template <typename Integer>
std::string Decimal(const Integer value) {
    return std::to_string(value);
}

Json HandleToJson(const Handle handle) {
    return {{"id", Decimal(handle.id)}, {"generation", Decimal(handle.generation)}};
}

Json PlayerToJson(const PlayerRow& player) {
    return {
        {"identity_key", player.identity_key},
        {"handle", HandleToJson(player.handle)},
        {"object_path", player.object_path},
        {"label", player.label},
        {"display_name", player.display_name},
        {"name_origin", player.name_origin},
        {"name_resource_version", player.name_resource_version},
        {"damage", Decimal(player.damage)},
        {"positive_hits", Decimal(player.positive_hits)},
        {"critical_hits", Decimal(player.critical_hits)},
        {"critical_rate", PlayerCriticalRate(player)},
        {"head_hits", Decimal(player.head_hits)},
        {"head_rate", PlayerHeadRate(player)},
        {"weak_hits", Decimal(player.weak_hits)},
        {"weak_rate", PlayerWeakRate(player)},
        {"maximum_hit", Decimal(player.maximum_hit)},
        {"maximum_ability", {
            {"name_id", Decimal(player.maximum_source_id)},
            {"name", player.maximum_source_name},
            {"display_name", player.maximum_source_display_name},
            {"name_origin", player.maximum_source_name_origin},
            {"name_resource_version", player.maximum_source_resource_version}}},
        {"damage_share", player.damage_share},
        {"encounter_dps", player.encounter_dps},
        {"active_dps", player.active_dps},
        {"rolling_dps_10s", player.rolling_dps},
        {"healing", {
            {"done", Decimal(player.healing_done)},
            {"done_events", Decimal(player.healing_done_events)},
            {"maximum", Decimal(player.maximum_heal)},
            {"share", player.healing_share},
            {"hps", player.encounter_hps},
            {"received", Decimal(player.healing_received)},
            {"received_events", Decimal(player.healing_received_events)},
            {"maximum_received", Decimal(player.maximum_heal_received)},
            {"received_share", player.healing_received_share},
            {"received_hps", player.healing_received_hps}}},
        {"incoming", {
            {"damage", Decimal(player.incoming_damage)},
            {"positive_hits", Decimal(player.incoming_positive_hits)},
            {"maximum_hit", Decimal(player.maximum_incoming_hit)},
            {"share", player.incoming_share}}}};
}

Json EventToJson(const CombatEventDetail& event) {
    return {
        {"event_type", CombatEventKindName(event.kind)},
        {"encounter_id", event.encounter_id},
        {"sequence", Decimal(event.sequence)},
        {"tick_sequence", Decimal(event.tick_sequence)},
        {"flags", event.flags},
        {"critical", event.kind == CombatEventKind::Damage &&
            (event.flags & kDamageFlagCritical) != 0},
        {"head_hit", event.kind == CombatEventKind::Damage &&
            (event.flags & kDamageFlagHeadHit) != 0},
        {"weak_unbalance", event.kind == CombatEventKind::Damage &&
            (event.flags & kDamageFlagWeakUnbalance) != 0},
        {"player_related", event.player_related},
        {"world", HandleToJson(event.world)},
        {"source_actor_id", event.attacker_actor_id},
        {"target_actor_id", event.victim_actor_id},
        {"ability_id", event.ability_id},
        {"name_id", Decimal(event.name_id)},
        {"source", HandleToJson(event.attacker)},
        {"target", HandleToJson(event.victim)},
        {"event_name", {
            {"technical_name", event.source},
            {"display_name", event.source_display_name},
            {"name_origin", event.source_name_origin},
            {"name_resource_version", event.source_resource_version}}},
        {"source_participant", {
            {"technical_name", event.attacker_name},
            {"display_name", event.attacker_display_name},
            {"name_origin", event.attacker_name_origin},
            {"name_resource_version", event.attacker_resource_version}}},
        {"target_participant", {
            {"technical_name", event.victim_name},
            {"display_name", event.victim_display_name},
            {"name_origin", event.victim_name_origin},
            {"name_resource_version", event.victim_resource_version}}},
        {"value", Decimal(event.value)},
        {"basic_value", Decimal(event.basic_value)},
        {"final_value", Decimal(event.final_value)},
        {"hit_location", event.hit_location},
        {"damage_type", event.damage_type},
        {"display_type", event.display_type},
        {"reaction_type", event.reaction_type},
        {"reaction_display_type", event.reaction_display_type},
        {"duration_seconds", event.duration_seconds},
        {"stack_count", event.stack_count},
        {"received_utc_ms", Decimal(event.received_utc_ms)},
        {"received_offset_us", Decimal(event.received_offset_us)},
        {"timestamp_basis", "client_receive"}};
}

}  // namespace

std::string SerializeCombatExport(
    const EncounterDetail& detail,
    const std::string& producer_version,
    const std::int64_t exported_utc_ms,
    const std::stop_token stop) {
    auto sorted_players = detail.players;
    std::ranges::sort(sorted_players, [](const auto& left, const auto& right) {
        return left.identity_key < right.identity_key;
    });
    Json players = Json::array();
    for (const auto& player : sorted_players) {
        if (stop.stop_requested()) throw std::runtime_error("export cancelled");
        players.push_back(PlayerToJson(player));
    }
    auto sorted_events = detail.events;
    std::ranges::sort(sorted_events, {}, &CombatEventDetail::sequence);
    sorted_events.erase(std::unique(sorted_events.begin(), sorted_events.end(),
        [](const auto& left, const auto& right) {
            return left.sequence == right.sequence;
        }), sorted_events.end());
    Json events = Json::array();
    for (const auto& event : sorted_events) {
        if (stop.stop_requested()) throw std::runtime_error("export cancelled");
        events.push_back(EventToJson(event));
    }
    Json histogram = Json::array();
    for (const auto& item : detail.summary.damage_flag_histogram) {
        histogram.push_back({{"flags", item.flags}, {"count", Decimal(item.count)}});
    }
    Json service_intervals = Json::array();
    for (const auto& [start, end] : detail.summary.service_unavailable_intervals) {
        service_intervals.push_back({
            {"start_offset_us", Decimal(start)}, {"end_offset_us", Decimal(end)}});
    }
    Json missing_ranges = Json::array();
    for (const auto& [first, last] : detail.summary.missing_sequence_ranges) {
        missing_ranges.push_back({
            {"first", Decimal(first)}, {"last", Decimal(last)}});
    }
    const auto& summary = detail.summary;
    Json document{
        {"format", "nte-combat-export"},
        {"schema_version", 3},
        {"metric_version", 1},
        {"account", {{"uid", summary.account.known()
                                  ? Json(summary.account.uid) : Json{}},
                     {"source", account::SourceName(summary.account.source)}}},
        {"producer", {{"name", "NTE Combat Tracker"},
                      {"version", producer_version},
                      {"exported_utc_ms", Decimal(exported_utc_ms)}}},
        {"capture", {{"event_scope", "player_and_engaged_actors"},
                     {"boundary_policy", "player_damage_inactivity_10s"},
                     {"healing_capture_status", "unavailable"}}},
        {"report", {{"report_id", summary.report_id},
                    {"game_build", summary.game_build}}},
        {"encounter", {
            {"encounter_id", summary.encounter_id},
            {"started_utc_ms", Decimal(summary.started_utc_ms)},
            {"closed_utc_ms", Decimal(summary.closed_utc_ms)},
            {"duration_seconds", summary.duration_seconds},
            {"end_reason", summary.end_reason},
            {"complete", summary.complete},
            {"event_count", Decimal(summary.event_count)},
            {"first_sequence", Decimal(summary.first_sequence)},
            {"last_sequence", Decimal(summary.last_sequence)},
            {"dropped_events", Decimal(summary.dropped_events)},
            {"dropped_first_sequence", Decimal(summary.dropped_first_sequence)},
            {"dropped_last_sequence", Decimal(summary.dropped_last_sequence)},
            {"write_error", summary.write_error},
            {"event_read_warnings", Decimal(detail.event_read_warnings)},
            {"service_unavailable_intervals", std::move(service_intervals)},
            {"missing_sequence_ranges", std::move(missing_ranges)},
            {"open_missing_sequence_start", summary.open_missing_sequence_start
                ? Json(Decimal(*summary.open_missing_sequence_start)) : Json{}},
            {"summary", {
                {"outgoing_damage", Decimal(summary.outgoing_damage)},
                {"incoming_damage", Decimal(summary.incoming_damage)},
                {"encounter_dps", summary.encounter_dps},
                {"outgoing_healing", Decimal(summary.outgoing_healing)},
                {"incoming_healing", Decimal(summary.incoming_healing)},
                {"outgoing_heal_events", Decimal(summary.outgoing_heal_events)},
                {"incoming_heal_events", Decimal(summary.incoming_heal_events)},
                {"encounter_hps", summary.encounter_hps},
                {"encounter_received_hps", summary.encounter_received_hps},
                {"event_type_counts", {
                    {"damage", Decimal(summary.event_type_counts.damage)},
                    {"heal", Decimal(summary.event_type_counts.heal)},
                    {"buff_add", Decimal(summary.event_type_counts.buff_add)},
                    {"buff_remove", Decimal(summary.event_type_counts.buff_remove)}}}}},
            {"quality", {
                {"partial", summary.quality.partial},
                {"overflow", summary.quality.overflow},
                {"cursor_gap", summary.quality.cursor_gap},
                {"aggregate_truncated", summary.quality.aggregate_truncated},
                {"service_intervals_truncated",
                    summary.service_intervals_truncated},
                {"missing_ranges_truncated", summary.missing_ranges_truncated},
                {"critical_data_status", CriticalDataStatusName(
                    summary.critical_data_status)},
                {"damage_flag_histogram", std::move(histogram)}}}}},
        {"players", std::move(players)},
        {"events", std::move(events)}};
    return document.dump(2) + '\n';
}

std::filesystem::path SuggestedCombatExportFilename(
    const HistoricalEncounter& encounter) {
    const std::time_t seconds = static_cast<std::time_t>(
        encounter.started_utc_ms / 1000);
    std::tm local{};
    localtime_s(&local, &seconds);
    std::wostringstream name;
    name << L"NTECombat-" << std::put_time(&local, L"%Y%m%d-%H%M%S") << L'-';
    const std::size_t id_length = (std::min)(std::size_t{8}, encounter.encounter_id.size());
    name << std::wstring(encounter.encounter_id.begin(),
        encounter.encounter_id.begin() + id_length) << L".json";
    return name.str();
}

bool WriteCombatExport(
    const std::filesystem::path& directory,
    const EncounterDetail& detail,
    const std::string& producer_version,
    const std::int64_t exported_utc_ms,
    std::filesystem::path& written_path,
    std::string& error,
    const std::stop_token stop) {
    error.clear();
    written_path.clear();
    if (directory.empty() || !directory.is_absolute()) {
        error = "export directory must be an absolute path";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(directory, filesystem_error)) {
        error = "export directory is unavailable: " + filesystem_error.message();
        return false;
    }
    const auto filename = SuggestedCombatExportFilename(detail.summary);
    const auto temporary = directory / (L".nte-export-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetCurrentThreadId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "export file could not be created";
            return false;
        }
        try {
            stream << SerializeCombatExport(
                detail, producer_version, exported_utc_ms, stop);
        } catch (const std::exception& exception) {
            error = exception.what();
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
        stream.flush();
        if (!stream) {
            error = "export file could not be written";
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
    }
    const auto stem = filename.stem().wstring();
    const auto extension = filename.extension().wstring();
    std::filesystem::path target;
    for (std::uint32_t suffix = 1; suffix != 0; ++suffix) {
        if (stop.stop_requested()) {
            std::filesystem::remove(temporary, filesystem_error);
            error = "export cancelled";
            return false;
        }
        target = directory / (suffix == 1 ? filename.wstring() :
            stem + L"-" + std::to_wstring(suffix) + extension);
        if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            written_path = target;
            return true;
        }
        const DWORD move_error = GetLastError();
        if (move_error != ERROR_FILE_EXISTS && move_error != ERROR_ALREADY_EXISTS) {
            std::filesystem::remove(temporary, filesystem_error);
            error = "export file could not be published: " +
                std::to_string(move_error);
            return false;
        }
    }
    std::filesystem::remove(temporary, filesystem_error);
    error = "no available export filename";
    return false;
}

}  // namespace nte::combat_stats::overlay
