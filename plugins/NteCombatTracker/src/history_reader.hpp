#pragma once

#include "account_identity.hpp"
#include "combat_stats_engine.hpp"

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <stop_token>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nte::combat_stats::overlay {

enum class DetailDirection : std::uint8_t { Outgoing, Incoming };

struct HistoricalEncounter {
    account::AccountIdentity account;
    std::string report_id;
    std::string encounter_id;
    std::string game_build;
    std::int64_t started_utc_ms{};
    std::int64_t closed_utc_ms{};
    double duration_seconds{};
    std::int64_t outgoing_damage{};
    std::int64_t incoming_damage{};
    double encounter_dps{};
    std::int64_t outgoing_healing{};
    std::int64_t incoming_healing{};
    std::uint64_t outgoing_heal_events{};
    std::uint64_t incoming_heal_events{};
    double encounter_hps{};
    double encounter_received_hps{};
    EventTypeCounts event_type_counts;
    std::string end_reason;
    bool complete{true};
    std::uint64_t event_count{};
    std::uint64_t first_sequence{};
    std::uint64_t last_sequence{};
    std::uint64_t dropped_events{};
    std::uint64_t dropped_first_sequence{};
    std::uint64_t dropped_last_sequence{};
    std::string write_error;
    std::vector<std::pair<std::uint64_t, std::uint64_t>>
        service_unavailable_intervals;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> missing_sequence_ranges;
    std::optional<std::uint64_t> open_missing_sequence_start;
    bool service_intervals_truncated{};
    bool missing_ranges_truncated{};
    Quality quality;
    CriticalDataStatus critical_data_status{CriticalDataStatus::Unavailable};
    std::vector<DamageFlagCount> damage_flag_histogram;
};

struct AccountHistoryGroup {
    account::AccountIdentity account;
    std::vector<HistoricalEncounter> encounters;
};

struct HistoryListRow {
    enum class Kind : std::uint8_t { Account, Encounter };
    Kind kind{Kind::Account};
    account::AccountIdentity account;
    std::size_t encounter_count{};
    std::optional<HistoricalEncounter> encounter;
};

struct CombatEventDetail : CombatEventData {
    std::string encounter_id;
    std::uint32_t attacker_actor_id{};
    std::uint32_t victim_actor_id{};
    std::uint32_t ability_id{};
    bool player_related{};
    std::string source;
    std::string source_display_name;
    std::string source_name_origin;
    std::string source_resource_version;
    std::string attacker_name;
    std::string attacker_display_name;
    std::string attacker_name_origin;
    std::string attacker_resource_version;
    std::string victim_name;
    std::string victim_display_name;
    std::string victim_name_origin;
    std::string victim_resource_version;
};

struct EventSourceSummary {
    std::uint64_t name_id{};
    Handle participant;
    std::string source_name;
    std::string label;
    std::int64_t amount{};
    std::uint64_t count{};
    std::int64_t maximum{};
};

struct EncounterDetail {
    HistoricalEncounter summary;
    std::vector<PlayerRow> players;
    std::vector<CombatEventDetail> events;
    std::uint64_t event_read_warnings{};
};

class CombatLogReader {
public:
    explicit CombatLogReader(std::filesystem::path root);

    [[nodiscard]] std::vector<AccountHistoryGroup> History(std::string& error) const;
    [[nodiscard]] std::optional<EncounterDetail> Encounter(
        const account::AccountIdentity& account,
        const std::string& report_id,
        const std::string& encounter_id,
        std::string& error) const;
    [[nodiscard]] std::optional<EncounterDetail> ExportEncounter(
        const account::AccountIdentity& account,
        const std::string& report_id,
        const std::string& encounter_id,
        std::string& error,
        std::stop_token stop = {}) const;
    [[nodiscard]] std::vector<CombatEventDetail> Events(
        const account::AccountIdentity& account,
        const std::string& report_id,
        const std::string& encounter_id,
        std::string& error,
        std::stop_token stop = {}) const;

private:
    [[nodiscard]] std::vector<CombatEventDetail> ReadEvents(
        const account::AccountIdentity& account,
        const std::string& report_id,
        const std::string& encounter_id,
        bool enrich_recorded_names,
        bool tolerate_malformed,
        std::uint64_t* read_warnings,
        std::string& error,
        std::stop_token stop) const;
    [[nodiscard]] std::optional<EncounterDetail> ReadEncounter(
        const account::AccountIdentity& account,
        const std::string& report_id,
        const std::string& encounter_id,
        bool normalize_for_display,
        std::string& error,
        std::stop_token stop) const;
    std::filesystem::path root_;
};

[[nodiscard]] std::vector<EventSourceSummary> GroupEventSources(
    const std::vector<CombatEventDetail>& events,
    Handle player,
    DetailDirection direction,
    CombatEventKind kind);
[[nodiscard]] std::vector<CombatEventDetail> FilterEvents(
    const std::vector<CombatEventDetail>& events,
    Handle player,
    DetailDirection direction,
    CombatEventKind kind,
    const EventSourceSummary& source);
void CanonicalizePlayerEvents(
    std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players);
void RecomputePlayerHitCounts(
    const std::vector<CombatEventDetail>& events,
    std::vector<PlayerRow>& players,
    bool complete);
[[nodiscard]] CriticalDataStatus CriticalDataStatusForEvents(
    const std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players,
    bool complete) noexcept;
[[nodiscard]] std::vector<HistoryListRow> FlattenHistoryGroups(
    const std::vector<AccountHistoryGroup>& groups,
    const std::unordered_set<std::string>& expanded_accounts);

}  // namespace nte::combat_stats::overlay
