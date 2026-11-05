#pragma once

#include "account_identity.hpp"
#include "combat_stats_engine.hpp"
#include "plugin_version.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nte::damage_log {

inline constexpr std::uint32_t kSchemaVersion = 3;
inline constexpr std::uint32_t kMetricVersion = 1;

struct ReportOpen {
    std::string report_id;
    combat_stats::Handle world;
    std::int64_t started_utc_ms{};
    std::string game_build;
    std::string plugin_version{nte::build::kPluginVersion};
    std::string runtime_version;
    account::AccountIdentity account;
};

struct EncounterOpen {
    std::string encounter_id;
    std::int64_t started_utc_ms{};
    std::uint64_t started_offset_us{};
};

struct LoggedCombatEvent {
    combat_stats::CombatEvent event;
    std::int64_t received_utc_ms{};
    std::uint64_t received_offset_us{};
    bool player_related{};
    std::string encounter_id;
};

struct EncounterClose {
    std::string encounter_id;
    std::string reason;
    std::int64_t closed_utc_ms{};
    std::uint64_t closed_offset_us{};
    std::int64_t outgoing_damage{};
    std::int64_t incoming_damage{};
    double encounter_dps{};
    double duration_seconds{};
    combat_stats::Quality quality;
    std::uint64_t dropped_events{};
    std::uint64_t dropped_first_sequence{};
    std::uint64_t dropped_last_sequence{};
    std::string write_error;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> service_unavailable_intervals;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> missing_sequence_ranges;
    std::optional<std::uint64_t> open_missing_sequence_start;
    bool service_intervals_truncated{};
    bool missing_ranges_truncated{};
    std::vector<combat_stats::PlayerRow> confirmed_controlled_players;
    combat_stats::CriticalDataStatus critical_data_status{
        combat_stats::CriticalDataStatus::Unavailable};
    std::vector<combat_stats::DamageFlagCount> damage_flag_histogram;
    std::int64_t outgoing_healing{};
    std::int64_t incoming_healing{};
    std::uint64_t outgoing_heal_events{};
    std::uint64_t incoming_heal_events{};
    double encounter_hps{};
    double encounter_received_hps{};
    std::uint64_t cutoff_sequence{};
};

struct ReportClose {
    std::string reason;
    std::int64_t closed_utc_ms{};
    bool unclean_shutdown{};
    bool incomplete{};
    std::string write_error;
};

struct EncounterHistory {
    std::string encounter_id;
    std::string end_reason;
    double duration_seconds{};
    std::int64_t outgoing_damage{};
    std::int64_t incoming_damage{};
    double encounter_dps{};
    bool complete{true};
};

struct ReportHistory {
    std::string report_id;
    combat_stats::Handle world;
    std::int64_t started_utc_ms{};
    std::int64_t closed_utc_ms{};
    std::uint64_t event_count{};
    std::uint64_t encounter_count{};
    std::uint64_t total_bytes{};
    bool open{};
    bool complete{true};
    std::string close_reason;
    std::vector<EncounterHistory> encounters;
    account::AccountIdentity account;
};

class DamageLogRepository {
public:
    explicit DamageLogRepository(std::filesystem::path root);
    ~DamageLogRepository();
    DamageLogRepository(DamageLogRepository&&) noexcept;
    DamageLogRepository& operator=(DamageLogRepository&&) noexcept;
    DamageLogRepository(const DamageLogRepository&) = delete;
    DamageLogRepository& operator=(const DamageLogRepository&) = delete;

    bool Recover(std::string& error);
    bool BeginReport(const ReportOpen& report, std::string& error);
    bool BeginEncounter(const EncounterOpen& encounter, std::string& error);
    bool Append(const LoggedCombatEvent& event, std::string& error);
    bool Flush(std::string& error);
    bool EndEncounter(const EncounterClose& encounter, std::string& error);
    bool CloseReport(const ReportClose& report, std::string& error);
    bool DeleteReport(const account::AccountIdentity& account,
        const std::string& report_id, std::string& error);

    [[nodiscard]] std::vector<ReportHistory> History() const;
    [[nodiscard]] bool HasOpenReport() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nte::damage_log
