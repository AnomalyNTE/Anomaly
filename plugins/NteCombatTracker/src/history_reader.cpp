#include "history_reader.hpp"
#include "event_damage_source_resolver.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <map>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <tuple>

namespace nte::combat_stats::overlay {
namespace {

using Json = nlohmann::json;

template <typename Integer>
Integer Decimal(const Json& value) {
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<Integer>();
    }
    if (!value.is_string()) throw std::runtime_error("decimal field is not a string");
    const auto& text = value.get_ref<const std::string&>();
    Integer result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error("decimal field is invalid");
    }
    return result;
}

bool IsUuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t index{}; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

bool IsEventSegment(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    if (name.size() != 19 || !name.starts_with("events-") || !name.ends_with(".jsonl")) {
        return false;
    }
    return std::ranges::all_of(name.substr(7, 6), [](const char character) {
        return character >= '0' && character <= '9';
    });
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream content;
    content << stream.rdbuf();
    if (!stream.eof() && stream.fail()) return std::nullopt;
    return content.str();
}

void RequireLogSchemaV2(const Json& value, const char* context) {
    const auto schema_version = value.value("schema_version", 0U);
    if (schema_version != 2U && schema_version != 3U) {
        throw std::runtime_error(std::string(context) + " schema is not supported");
    }
}

HistoricalEncounter ParseSummary(
    const Json& value,
    const account::AccountIdentity& account_identity,
    const std::string& report_id,
    const std::string& game_build = {}) {
    RequireLogSchemaV2(value, "encounter");
    HistoricalEncounter result;
    result.account = account_identity;
    result.report_id = report_id;
    result.encounter_id = value.at("encounter_id").get<std::string>();
    result.game_build = game_build;
    result.started_utc_ms = value.at("started_utc_ms").get<std::int64_t>();
    result.closed_utc_ms = value.at("closed_utc_ms").get<std::int64_t>();
    result.duration_seconds = value.at("duration_seconds").get<double>();
    result.outgoing_damage = Decimal<std::int64_t>(value.at("outgoing_damage"));
    result.incoming_damage = Decimal<std::int64_t>(value.at("incoming_damage"));
    result.encounter_dps = value.at("encounter_dps").get<double>();
    result.outgoing_healing = Decimal<std::int64_t>(
        value.value("outgoing_healing", Json{"0"}));
    result.incoming_healing = Decimal<std::int64_t>(
        value.value("incoming_healing", Json{"0"}));
    result.outgoing_heal_events = Decimal<std::uint64_t>(
        value.value("outgoing_heal_events", Json{"0"}));
    result.incoming_heal_events = Decimal<std::uint64_t>(
        value.value("incoming_heal_events", Json{"0"}));
    result.encounter_hps = value.value("encounter_hps", 0.0);
    result.encounter_received_hps = value.value("encounter_received_hps", 0.0);
    if (const auto counts = value.find("event_type_counts"); counts != value.end()) {
        result.event_type_counts.damage = Decimal<std::uint64_t>(
            counts->value("damage", Json{"0"}));
        result.event_type_counts.heal = Decimal<std::uint64_t>(
            counts->value("heal", Json{"0"}));
        result.event_type_counts.buff_add = Decimal<std::uint64_t>(
            counts->value("buff_add", Json{"0"}));
        result.event_type_counts.buff_remove = Decimal<std::uint64_t>(
            counts->value("buff_remove", Json{"0"}));
    }
    result.end_reason = value.at("end_reason").get<std::string>();
    result.complete = value.value("complete", false);
    result.event_count = Decimal<std::uint64_t>(value.value("event_count", Json{"0"}));
    result.first_sequence = Decimal<std::uint64_t>(value.value("first_sequence", Json{"0"}));
    result.last_sequence = Decimal<std::uint64_t>(value.value("last_sequence", Json{"0"}));
    result.dropped_events = Decimal<std::uint64_t>(value.value("dropped_events", Json{"0"}));
    result.dropped_first_sequence = Decimal<std::uint64_t>(
        value.value("dropped_first_sequence", Json{"0"}));
    result.dropped_last_sequence = Decimal<std::uint64_t>(
        value.value("dropped_last_sequence", Json{"0"}));
    result.write_error = value.value("write_error", std::string{});
    if (const auto intervals = value.find("service_unavailable_intervals");
        intervals != value.end()) {
        for (const auto& interval : *intervals) {
            result.service_unavailable_intervals.emplace_back(
                Decimal<std::uint64_t>(interval.at("start_offset_us")),
                Decimal<std::uint64_t>(interval.at("end_offset_us")));
        }
    }
    if (const auto ranges = value.find("missing_sequence_ranges");
        ranges != value.end()) {
        for (const auto& range : *ranges) {
            result.missing_sequence_ranges.emplace_back(
                Decimal<std::uint64_t>(range.at("first")),
                Decimal<std::uint64_t>(range.at("last")));
        }
    }
    if (const auto open = value.find("open_missing_sequence_start");
        open != value.end() && !open->is_null()) {
        result.open_missing_sequence_start = Decimal<std::uint64_t>(*open);
    }
    if (const auto quality = value.find("quality"); quality != value.end()) {
        result.quality.partial = quality->value("partial", false);
        result.quality.overflow = quality->value("overflow", false);
        result.quality.cursor_gap = quality->value("cursor_gap", false);
        result.quality.aggregate_truncated = quality->value("aggregate_truncated", false);
        result.service_intervals_truncated = quality->value(
            "service_intervals_truncated", false);
        result.missing_ranges_truncated = quality->value(
            "missing_ranges_truncated", false);
    }
    const std::string critical_status = value.value(
        "critical_data_status", std::string{"unavailable"});
    if (critical_status == "available") {
        result.critical_data_status = CriticalDataStatus::Available;
    } else if (critical_status == "partial") {
        result.critical_data_status = CriticalDataStatus::Partial;
    } else {
        result.critical_data_status = CriticalDataStatus::Unavailable;
    }
    if (const auto histogram = value.find("damage_flag_histogram");
        histogram != value.end()) {
        for (const auto& item : *histogram) {
            result.damage_flag_histogram.push_back({
                item.at("flags").get<std::uint32_t>(),
                Decimal<std::uint64_t>(item.at("count"))});
        }
    }
    return result;
}

std::string ShortName(const std::string& value);

struct ActorName {
    Handle handle;
    std::string technical_name;
    std::string display_name;
    std::string name_origin;
    std::string resource_version;
};

struct AbilityName {
    std::uint64_t source_id{};
    std::string technical_name;
    std::string display_name;
    std::string name_origin;
    std::string resource_version;
};

std::unordered_map<std::uint32_t, ActorName> LoadActors(
    const std::filesystem::path& report) {
    std::unordered_map<std::uint32_t, ActorName> result;
    std::ifstream stream(report / "actors.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        const Json value = Json::parse(line);
        RequireLogSchemaV2(value, "actor");
        const std::uint32_t id = value.at("actor_id").get<std::uint32_t>();
        const auto& handle = value.at("handle");
        const std::string technical = value.value(
            "technical_name", value.at("path").get<std::string>());
        result.emplace(id, ActorName{
            Handle{Decimal<std::uint64_t>(handle.at("id")),
                   Decimal<std::uint64_t>(handle.at("generation"))},
            technical, value.value("display_name", ShortName(technical)),
            value.value("name_origin", std::string{"technical"}),
            value.value("name_resource_version",
                value.value("catalog_version", std::string{}))});
    }
    return result;
}

std::unordered_map<std::uint32_t, AbilityName> LoadAbilities(
    const std::filesystem::path& report) {
    std::unordered_map<std::uint32_t, AbilityName> result;
    std::ifstream stream(report / "abilities.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        const Json value = Json::parse(line);
        RequireLogSchemaV2(value, "ability");
        const std::string technical = value.value(
            "technical_name", value.at("name").get<std::string>());
        result.emplace(value.at("ability_id").get<std::uint32_t>(), AbilityName{
            Decimal<std::uint64_t>(value.at("source_id")),
            technical, value.value("display_name", ShortName(technical)),
            value.value("name_origin", std::string{"technical"}),
            value.value("name_resource_version",
                value.value("catalog_version", std::string{}))});
    }
    return result;
}

CombatEventDetail ParseEvent(const Json& value) {
    RequireLogSchemaV2(value, "event");
    CombatEventDetail result;
    const auto kind = value.at("event_type").get<std::string>();
    const auto parsed_kind = ParseCombatEventKind(kind);
    if (!parsed_kind) throw std::runtime_error("event type is not supported");
    result.kind = *parsed_kind;
    result.encounter_id = value.at("encounter_id").get<std::string>();
    result.sequence = Decimal<std::uint64_t>(value.at("sequence"));
    result.tick_sequence = Decimal<std::uint64_t>(value.at("tick_sequence"));
    result.flags = value.at("flags").get<std::uint32_t>();
    if (const auto world = value.find("world"); world != value.end()) {
        result.world = {Decimal<std::uint64_t>(world->at("id")),
            Decimal<std::uint64_t>(world->at("generation"))};
    }
    result.attacker_actor_id = value.value("source_actor_id", 0U);
    result.victim_actor_id = value.value("target_actor_id", 0U);
    result.ability_id = value.value("ability_id", 0U);
    result.name_id = Decimal<std::uint64_t>(value.at("name_id"));
    const auto& attacker = value.at("source");
    const auto& victim = value.at("target");
    result.attacker = {Decimal<std::uint64_t>(attacker.at("id")),
        Decimal<std::uint64_t>(attacker.at("generation"))};
    result.victim = {Decimal<std::uint64_t>(victim.at("id")),
        Decimal<std::uint64_t>(victim.at("generation"))};
    result.player_related = value.value("player_related", false);
    result.final_value = Decimal<std::int64_t>(value.at("final_value"));
    result.basic_value = Decimal<std::int64_t>(value.at("basic_value"));
    result.value = Decimal<std::int64_t>(value.at("value"));
    if (const auto hit = value.find("hit_location"); hit != value.end() &&
        hit->is_array() && hit->size() == 3) {
        for (std::size_t index{}; index < 3; ++index) {
            result.hit_location[index] = hit->at(index).get<double>();
        }
    }
    result.damage_type = value.value("damage_type", 0U);
    result.display_type = value.value("display_type", 0U);
    result.reaction_type = value.value("reaction_type", 0U);
    result.reaction_display_type = value.value("reaction_display_type", 0U);
    result.duration_seconds = value.value("duration_seconds", 0.0F);
    result.stack_count = value.value("stack_count", 0);
    result.received_utc_ms = value.at("received_utc_ms").get<std::int64_t>();
    result.received_offset_us = Decimal<std::uint64_t>(value.at("received_offset_us"));
    result.source = value.at("event_name").get<std::string>();
    result.source_display_name = value.value(
        "event_display_name", ShortName(result.source));
    result.source_name_origin = value.value(
        "event_name_origin", std::string{"technical"});
    result.source_resource_version = value.value(
        "name_resource_version",
        value.value("name_catalog_version", std::string{}));
    result.attacker_name = value.value("source_path", std::string{});
    result.attacker_display_name = value.value(
        "source_display_name", ShortName(result.attacker_name));
    result.attacker_name_origin = value.value(
        "source_name_origin", std::string{"technical"});
    result.attacker_resource_version = value.value(
        "name_resource_version",
        value.value("name_catalog_version", std::string{}));
    result.victim_name = value.value("target_path", std::string{});
    result.victim_display_name = value.value(
        "target_display_name", ShortName(result.victim_name));
    result.victim_name_origin = value.value(
        "target_name_origin", std::string{"technical"});
    result.victim_resource_version = value.value(
        "name_resource_version",
        value.value("name_catalog_version", std::string{}));
    return result;
}

std::string ShortName(const std::string& value) {
    const auto slash = value.find_last_of("/\\");
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

}  // namespace

CombatLogReader::CombatLogReader(std::filesystem::path root) : root_(std::move(root)) {}

std::vector<AccountHistoryGroup> CombatLogReader::History(std::string& error) const {
    error.clear();
    std::vector<AccountHistoryGroup> result;
    try {
        std::error_code filesystem_error;
        const auto accounts_root = root_ / "accounts";
        if (!std::filesystem::is_directory(accounts_root, filesystem_error)) return result;
        for (const auto& account_entry : std::filesystem::directory_iterator(
                 accounts_root, filesystem_error)) {
            if (filesystem_error) throw std::runtime_error(filesystem_error.message());
            if (!account_entry.is_directory()) continue;
            const auto account_identity = account::FromDirectoryName(
                account_entry.path().filename().string());
            if (!account_identity) continue;
            const auto catalog_text = ReadFile(account_entry.path() / "catalog.json");
            if (!catalog_text) {
                error = "history catalog is temporarily unavailable";
                continue;
            }
            AccountHistoryGroup group;
            group.account = *account_identity;
            try {
                const Json catalog = Json::parse(*catalog_text);
                RequireLogSchemaV2(catalog, "catalog");
                for (const auto& report : catalog.at("reports")) {
                    try {
                        const std::string report_id = report.at("report_id").get<std::string>();
                        if (!IsUuid(report_id)) continue;
                        const auto report_directory = account_entry.path() / "reports" / report_id;
                        std::string game_build;
                        if (const auto report_text = ReadFile(report_directory / "report.json")) {
                            try {
                                game_build = Json::parse(*report_text).value(
                                    "game_build", std::string{});
                            } catch (const std::exception& exception) {
                                error = exception.what();
                            }
                        }
                        std::ifstream encounters(
                            report_directory / "encounters.jsonl", std::ios::binary);
                        std::string line;
                        while (std::getline(encounters, line)) {
                            if (line.empty()) continue;
                            try {
                                auto summary = ParseSummary(Json::parse(line),
                                    *account_identity, report_id, game_build);
                                if (IsUuid(summary.encounter_id)) {
                                    group.encounters.push_back(std::move(summary));
                                }
                            } catch (const std::exception& exception) {
                                error = exception.what();
                            }
                        }
                    } catch (const std::exception& exception) {
                        error = exception.what();
                    }
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            }
            std::ranges::sort(group.encounters, [](const auto& left, const auto& right) {
                if (left.started_utc_ms != right.started_utc_ms) {
                    return left.started_utc_ms > right.started_utc_ms;
                }
                return left.encounter_id < right.encounter_id;
            });
            if (!group.encounters.empty()) result.push_back(std::move(group));
        }
        std::ranges::sort(result, [](const auto& left, const auto& right) {
            return left.encounters.front().started_utc_ms >
                right.encounters.front().started_utc_ms;
        });
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return result;
}

std::vector<CombatEventDetail> CombatLogReader::Events(
    const account::AccountIdentity& account_identity,
    const std::string& report_id,
    const std::string& encounter_id,
    std::string& error,
    const std::stop_token stop) const {
    return ReadEvents(account_identity, report_id, encounter_id,
        true, false, nullptr, error, stop);
}

std::vector<CombatEventDetail> CombatLogReader::ReadEvents(
    const account::AccountIdentity& account_identity,
    const std::string& report_id,
    const std::string& encounter_id,
    const bool enrich_recorded_names,
    const bool tolerate_malformed,
    std::uint64_t* const read_warnings,
    std::string& error,
    const std::stop_token stop) const {
    error.clear();
    std::vector<CombatEventDetail> result;
    if (!account::IsValid(account_identity) ||
        !IsUuid(report_id) || !IsUuid(encounter_id)) {
        error = "invalid report or encounter id";
        return result;
    }
    try {
        const auto directory = root_ / "accounts" /
            account::DirectoryName(account_identity) / "reports" / report_id;
        const auto actors = LoadActors(directory);
        const auto abilities = LoadAbilities(directory);
        std::unordered_map<Handle, ActorName, HandleHash> actor_names;
        for (const auto& [id, actor] : actors) {
            static_cast<void>(id);
            actor_names.insert_or_assign(actor.handle, actor);
        }
        std::map<std::pair<std::uint64_t, std::string>, const AbilityName*>
            abilities_by_identity;
        std::unordered_map<std::uint64_t, const AbilityName*> unique_abilities_by_source;
        for (const auto& [id, ability] : abilities) {
            static_cast<void>(id);
            abilities_by_identity.emplace(
                std::pair{ability.source_id, ability.technical_name}, &ability);
            const auto [found, inserted] = unique_abilities_by_source.emplace(
                ability.source_id, &ability);
            if (!inserted) found->second = nullptr;
        }
        std::error_code filesystem_error;
        std::vector<std::filesystem::path> segments;
        for (const auto& entry : std::filesystem::directory_iterator(
                 directory, filesystem_error)) {
            if (!filesystem_error && entry.is_regular_file() && IsEventSegment(entry.path())) {
                segments.push_back(entry.path());
            }
        }
        if (filesystem_error) throw std::runtime_error(filesystem_error.message());
        std::ranges::sort(segments);
        for (const auto& segment : segments) {
            if (stop.stop_requested()) {
                error = "operation cancelled";
                return {};
            }
            std::ifstream stream(segment, std::ios::binary);
            std::string line;
            while (std::getline(stream, line)) {
                if (stop.stop_requested()) {
                    error = "operation cancelled";
                    return {};
                }
                if (line.empty()) continue;
                try {
                    const Json value = Json::parse(line);
                    if (value.value("encounter_id", std::string{}) != encounter_id) {
                        continue;
                    }
                    RequireLogSchemaV2(value, "event");
                    auto event = ParseEvent(value);
                    {
                        if (const auto attacker = actor_names.find(event.attacker);
                            attacker != actor_names.end()) {
                            if (event.attacker_name.empty() || enrich_recorded_names) {
                                event.attacker_name = attacker->second.technical_name;
                            }
                            if (enrich_recorded_names &&
                                event.attacker_name_origin == "technical" &&
                                attacker->second.name_origin != "technical") {
                                event.attacker_display_name = attacker->second.display_name;
                                event.attacker_name_origin = attacker->second.name_origin;
                                event.attacker_resource_version =
                                    attacker->second.resource_version;
                            }
                        }
                        if (const auto victim = actor_names.find(event.victim);
                            victim != actor_names.end()) {
                            if (event.victim_name.empty() || enrich_recorded_names) {
                                event.victim_name = victim->second.technical_name;
                            }
                            if (enrich_recorded_names &&
                                event.victim_name_origin == "technical" &&
                                victim->second.name_origin != "technical") {
                                event.victim_display_name = victim->second.display_name;
                                event.victim_name_origin = victim->second.name_origin;
                                event.victim_resource_version = victim->second.resource_version;
                            }
                        }
                        const AbilityName* source{};
                        if (event.ability_id != 0) {
                            if (const auto found = abilities.find(event.ability_id);
                                found != abilities.end() &&
                                found->second.source_id == event.name_id &&
                                found->second.technical_name == event.source) {
                                source = &found->second;
                            }
                        }
                        if (source == nullptr) {
                            if (const auto found = abilities_by_identity.find(
                                    {event.name_id, event.source});
                                found != abilities_by_identity.end()) {
                                source = found->second;
                            }
                        }
                        if (source == nullptr) {
                            if (const auto found = unique_abilities_by_source.find(
                                    event.name_id);
                                found != unique_abilities_by_source.end()) {
                                source = found->second;
                            }
                        }
                        if (enrich_recorded_names && source != nullptr &&
                            event.source_name_origin == "technical" &&
                            source->name_origin != "technical") {
                            event.source_display_name = source->display_name;
                            event.source_name_origin = source->name_origin;
                            event.source_resource_version = source->resource_version;
                        }
                        result.push_back(std::move(event));
                    }
                } catch (const std::exception& exception) {
                    if (read_warnings != nullptr) ++*read_warnings;
                    if (!tolerate_malformed) error = exception.what();
                }
            }
        }
        std::ranges::sort(result, {}, &CombatEventDetail::sequence);
        result.erase(std::unique(result.begin(), result.end(), [](const auto& left,
                         const auto& right) { return left.sequence == right.sequence; }),
            result.end());
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return result;
}

std::optional<EncounterDetail> CombatLogReader::Encounter(
    const account::AccountIdentity& account_identity,
    const std::string& report_id,
    const std::string& encounter_id,
    std::string& error) const {
    return ReadEncounter(account_identity, report_id, encounter_id, true, error, {});
}

std::optional<EncounterDetail> CombatLogReader::ExportEncounter(
    const account::AccountIdentity& account_identity,
    const std::string& report_id,
    const std::string& encounter_id,
    std::string& error,
    const std::stop_token stop) const {
    return ReadEncounter(
        account_identity, report_id, encounter_id, false, error, stop);
}

std::optional<EncounterDetail> CombatLogReader::ReadEncounter(
    const account::AccountIdentity& account_identity,
    const std::string& report_id,
    const std::string& encounter_id,
    const bool normalize_for_display,
    std::string& error,
    const std::stop_token stop) const {
    error.clear();
    if (!account::IsValid(account_identity) ||
        !IsUuid(report_id) || !IsUuid(encounter_id)) {
        error = "invalid report or encounter id";
        return std::nullopt;
    }
    try {
        const auto directory = root_ / "accounts" /
            account::DirectoryName(account_identity) / "reports" / report_id;
        std::ifstream stream(directory / "encounters.jsonl", std::ios::binary);
        std::string line;
        Json selected;
        while (std::getline(stream, line)) {
            if (stop.stop_requested()) {
                error = "operation cancelled";
                return std::nullopt;
            }
            if (line.empty()) continue;
            try {
                const Json value = Json::parse(line);
                if (value.value("encounter_id", std::string{}) == encounter_id) {
                    selected = value;
                    break;
                }
            } catch (const std::exception&) {
                // Ignore malformed summaries when looking for a valid encounter.
            }
        }
        if (selected.is_null()) {
            error = "encounter not found";
            return std::nullopt;
        }
        EncounterDetail detail;
        std::string game_build;
        if (const auto report_text = ReadFile(directory / "report.json")) {
            const Json report = Json::parse(*report_text);
            RequireLogSchemaV2(report, "report");
            game_build = report.value("game_build", std::string{});
        }
        detail.summary = ParseSummary(
            selected, account_identity, report_id, game_build);
        const auto actors = LoadActors(directory);
        const auto abilities = LoadAbilities(directory);
        std::unordered_map<std::uint32_t, std::size_t> player_indices;
        for (const auto& item : selected.at("confirmed_controlled_players")) {
            PlayerRow player;
            const auto actor_id = item.at("actor_id").get<std::uint32_t>();
            if (const auto actor = actors.find(actor_id); actor != actors.end()) {
                player.handle = actor->second.handle;
                player.object_path = actor->second.technical_name;
                player.display_name = actor->second.display_name;
                player.name_origin = actor->second.name_origin;
                player.name_resource_version = actor->second.resource_version;
            }
            player.identity_key = "path:" + player.object_path;
            player.label = item.value("label", player.display_name.empty()
                ? ShortName(player.object_path) : player.display_name);
            if (player.display_name.empty()) player.display_name = player.label;
            player.name_origin = item.value("name_origin", player.name_origin.empty()
                ? std::string{"recorded"} : player.name_origin);
            player.name_resource_version = item.value(
                "name_resource_version",
                item.value("name_catalog_version", player.name_resource_version));
            player.damage = Decimal<std::int64_t>(item.at("damage"));
            player.positive_hits = Decimal<std::uint64_t>(item.at("positive_hits"));
            player.critical_hits = Decimal<std::uint64_t>(item.at("critical_hits"));
            player.head_hits = Decimal<std::uint64_t>(item.at("head_hits"));
            player.weak_hits = Decimal<std::uint64_t>(item.at("weak_hits"));
            player.damage_share = item.at("damage_share").get<double>();
            player.encounter_dps = item.at("encounter_dps").get<double>();
            player.active_dps = item.at("active_dps").get<double>();
            player.rolling_dps = item.at("rolling_dps_10s").get<double>();
            player.healing_done = Decimal<std::int64_t>(
                item.value("healing_done", Json{"0"}));
            player.healing_done_events = Decimal<std::uint64_t>(
                item.value("healing_done_events", Json{"0"}));
            player.maximum_heal = Decimal<std::int64_t>(
                item.value("maximum_heal", Json{"0"}));
            player.healing_share = item.value("healing_share", 0.0);
            player.encounter_hps = item.value("encounter_hps", 0.0);
            player.healing_received = Decimal<std::int64_t>(
                item.value("healing_received", Json{"0"}));
            player.healing_received_events = Decimal<std::uint64_t>(
                item.value("healing_received_events", Json{"0"}));
            player.maximum_heal_received = Decimal<std::int64_t>(
                item.value("maximum_heal_received", Json{"0"}));
            player.healing_received_share = item.value("healing_received_share", 0.0);
            player.healing_received_hps = item.value("healing_received_hps", 0.0);
            player.maximum_hit = Decimal<std::int64_t>(item.at("maximum_hit"));
            const auto ability_id = item.at("maximum_ability_id").get<std::uint32_t>();
            if (const auto ability = abilities.find(ability_id); ability != abilities.end()) {
                player.maximum_source_id = ability->second.source_id;
                player.maximum_source_name = ability->second.technical_name;
                player.maximum_source_display_name = ability->second.display_name;
                player.maximum_source_name_origin = ability->second.name_origin;
                player.maximum_source_resource_version =
                    ability->second.resource_version;
            }
            player_indices.emplace(actor_id, detail.players.size());
            detail.players.push_back(std::move(player));
        }
        if (const auto incoming = selected.find("actor_incoming"); incoming != selected.end()) {
            for (const auto& item : *incoming) {
                const auto actor_id = item.at("actor_id").get<std::uint32_t>();
                const auto player = player_indices.find(actor_id);
                if (player == player_indices.end()) continue;
                auto& row = detail.players[player->second];
                row.incoming_damage = Decimal<std::int64_t>(item.at("damage"));
                row.incoming_positive_hits = Decimal<std::uint64_t>(item.at("hits"));
                row.maximum_incoming_hit = Decimal<std::int64_t>(item.at("maximum_hit"));
                row.incoming_share = detail.summary.incoming_damage > 0
                    ? static_cast<double>(row.incoming_damage) * 100.0 /
                        static_cast<double>(detail.summary.incoming_damage)
                    : 0.0;
            }
        }
        detail.events = ReadEvents(account_identity, report_id, encounter_id,
            normalize_for_display, !normalize_for_display,
            &detail.event_read_warnings, error, stop);
        if (normalize_for_display && error.empty() && !detail.events.empty()) {
            CanonicalizePlayerEvents(detail.events, detail.players);
        }
        if (normalize_for_display && error.empty() &&
            detail.summary.critical_data_status != CriticalDataStatus::Available) {
            detail.summary.critical_data_status = CriticalDataStatusForEvents(
                detail.events, detail.players, detail.summary.complete);
        }
        if (normalize_for_display && error.empty()) RecomputePlayerHitCounts(
            detail.events, detail.players, detail.summary.complete);
        return detail;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::vector<HistoryListRow> FlattenHistoryGroups(
    const std::vector<AccountHistoryGroup>& groups,
    const std::unordered_set<std::string>& expanded_accounts) {
    std::vector<HistoryListRow> rows;
    for (const auto& group : groups) {
        rows.push_back({HistoryListRow::Kind::Account,
            group.account, group.encounters.size(), std::nullopt});
        if (!expanded_accounts.contains(account::DirectoryName(group.account))) continue;
        for (const auto& encounter : group.encounters) {
            rows.push_back({HistoryListRow::Kind::Encounter,
                group.account, 0, encounter});
        }
    }
    return rows;
}

CriticalDataStatus CriticalDataStatusForEvents(
    const std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players,
    const bool complete) noexcept {
    std::uint64_t positive_hits{};
    std::uint64_t partial_hits{};
    for (const auto& event : events) {
        if (event.kind != CombatEventKind::Damage ||
            !IsPositiveDamage(event.final_value)) continue;
        const bool outgoing = std::ranges::any_of(players, [&](const PlayerRow& player) {
            return event.attacker == player.handle;
        });
        if (!outgoing) continue;
        ++positive_hits;
        if ((event.flags & kCombatFlagPartial) != 0) ++partial_hits;
    }
    if (!complete) return CriticalDataStatus::Partial;
    if (positive_hits == 0) return CriticalDataStatus::Unavailable;
    return partial_hits == 0
        ? CriticalDataStatus::Available
        : CriticalDataStatus::Partial;
}

void CanonicalizePlayerEvents(
    std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players) {
    for (auto& event : events) {
        for (const auto& player : players) {
            if (!player.object_path.empty() && event.attacker_name == player.object_path) {
                event.attacker = player.handle;
            }
            if (!player.object_path.empty() && event.victim_name == player.object_path) {
                event.victim = player.handle;
            }
        }
    }
}

void RecomputePlayerHitCounts(
    const std::vector<CombatEventDetail>& events,
    std::vector<PlayerRow>& players,
    const bool complete) {
    if (!complete || events.empty()) return;
    for (auto& player : players) {
        std::uint64_t positive_hits{};
        std::uint64_t critical_hits{};
        for (const auto& event : events) {
            if (event.kind != CombatEventKind::Damage ||
                event.attacker != player.handle ||
                !IsPositiveDamage(event.final_value)) continue;
            ++positive_hits;
            if (IsCriticalPositiveDamage(event.final_value, event.flags)) ++critical_hits;
        }
        player.positive_hits = positive_hits;
        player.critical_hits = (std::min)(critical_hits, positive_hits);
    }
}

std::vector<EventSourceSummary> GroupEventSources(
    const std::vector<CombatEventDetail>& events,
    const Handle player,
    const DetailDirection direction,
    const CombatEventKind kind) {
    struct Key {
        Handle participant;
        std::string source_identity;
        bool operator<(const Key& right) const {
            return std::tie(participant.id, participant.generation, source_identity) <
                std::tie(right.participant.id,
                    right.participant.generation, right.source_identity);
        }
    };
    std::map<Key, EventSourceSummary> rows;
    for (const auto& event : events) {
        const bool matches = direction == DetailDirection::Outgoing
            ? event.attacker == player : event.victim == player;
        const auto amount = PositiveCombatEventAmount(event);
        if (event.kind != kind || !matches || amount <= 0) continue;
        const Handle participant = direction == DetailDirection::Incoming
            ? event.attacker : Handle{};
        auto& row = rows[{participant,
            DamageSourceIdentity(event.name_id, event.source)}];
        row.name_id = event.name_id;
        row.participant = participant;
        row.source_name = event.source;
        row.label = direction == DetailDirection::Incoming
            ? (event.attacker_display_name.empty()
                ? ShortName(event.attacker_name) : event.attacker_display_name) + " / " +
              (event.source_display_name.empty()
                ? ShortName(event.source) : event.source_display_name)
            : (event.source_display_name.empty()
                ? ShortName(event.source) : event.source_display_name);
        row.amount += amount;
        ++row.count;
        row.maximum = (std::max)(row.maximum, amount);
    }
    std::vector<EventSourceSummary> result;
    for (auto& [key, row] : rows) {
        static_cast<void>(key);
        result.push_back(std::move(row));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.amount != right.amount) return left.amount > right.amount;
        return left.label < right.label;
    });
    return result;
}

std::vector<CombatEventDetail> FilterEvents(
    const std::vector<CombatEventDetail>& events,
    const Handle player,
    const DetailDirection direction,
    const CombatEventKind kind,
    const EventSourceSummary& source) {
    std::vector<CombatEventDetail> result;
    for (const auto& event : events) {
        const auto amount = PositiveCombatEventAmount(event);
        const bool participant_matches = direction == DetailDirection::Outgoing
            ? event.attacker == player
            : event.victim == player && event.attacker == source.participant;
        const bool source_matches = event.source == source.source_name &&
            (DamageSourceIdentity(event.name_id, event.source) == event.source ||
                event.name_id == source.name_id);
        if (event.kind == kind && participant_matches && source_matches && amount > 0) {
            result.push_back(event);
        }
    }
    std::ranges::sort(result, {}, &CombatEventDetail::sequence);
    return result;
}

}  // namespace nte::combat_stats::overlay
