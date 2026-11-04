#pragma once

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nte::combat_stats {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct Handle {
    std::uint64_t id{};
    std::uint64_t generation{};

    friend bool operator==(const Handle&, const Handle&) = default;
};

struct HandleHash {
    std::size_t operator()(const Handle& value) const noexcept;
};

enum class EncounterState { Idle, Active, Closing, Completed };
enum class CriticalDataStatus : std::uint8_t { Unavailable, Available, Partial };
enum class CombatEventKind : std::uint8_t { Damage, Heal, BuffAdd, BuffRemove };

[[nodiscard]] inline std::string_view CombatEventKindName(
    const CombatEventKind kind) noexcept {
    switch (kind) {
    case CombatEventKind::Damage: return "damage";
    case CombatEventKind::Heal: return "heal";
    case CombatEventKind::BuffAdd: return "buff_add";
    case CombatEventKind::BuffRemove: return "buff_remove";
    }
    return "damage";
}

[[nodiscard]] inline std::optional<CombatEventKind> ParseCombatEventKind(
    const std::string_view name) noexcept {
    if (name == "damage") return CombatEventKind::Damage;
    if (name == "heal") return CombatEventKind::Heal;
    if (name == "buff_add") return CombatEventKind::BuffAdd;
    if (name == "buff_remove") return CombatEventKind::BuffRemove;
    return std::nullopt;
}

[[nodiscard]] inline std::string_view CriticalDataStatusName(
    const CriticalDataStatus status) noexcept {
    switch (status) {
    case CriticalDataStatus::Available: return "available";
    case CriticalDataStatus::Partial: return "partial";
    case CriticalDataStatus::Unavailable: return "unavailable";
    }
    return "unavailable";
}

struct Combatant {
    Handle world;
    Handle character;
    double hp{};
    double max_hp{};
    double shield{};
};

struct CombatEventData {
    CombatEventKind kind{CombatEventKind::Damage};
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t flags{};
    Handle world;
    Handle attacker;
    Handle victim;
    std::uint64_t name_id{};
    std::int64_t value{};
    std::int64_t basic_value{};
    std::int64_t final_value{};
    std::array<double, 3> hit_location{};
    std::uint32_t damage_type{};
    std::uint32_t display_type{};
    std::uint32_t reaction_type{};
    std::uint32_t reaction_display_type{};
    float duration_seconds{};
    std::int32_t stack_count{};
    std::int64_t received_utc_ms{};
    std::uint64_t received_offset_us{};
};

struct CombatEvent : CombatEventData {
    std::string source_name;
    std::string source_display_name;
    std::string source_name_origin;
    std::string name_resource_version;
    std::string attacker_name;
    std::string attacker_display_name;
    std::string attacker_name_origin;
    std::string victim_name;
    std::string victim_display_name;
    std::string victim_name_origin;
    // Raw object paths, kept alongside the display names because the character
    // attribute lookup keys off the player_NNN segment rather than the name.
    std::string attacker_path;
    std::string victim_path;
};

struct DamageEvent : CombatEvent {
    DamageEvent() { kind = CombatEventKind::Damage; }
};

// Mirrors AnomalyNteCombatEventFlagsV1. Keeping the constants SDK-independent
// lets the engine and log readers remain testable without loading the plugin ABI.
inline constexpr std::uint32_t kDamageFlagCritical = 1U << 0U;
inline constexpr std::uint32_t kDamageFlagHeadHit = 1U << 1U;
inline constexpr std::uint32_t kDamageFlagWeakUnbalance = 1U << 2U;
inline constexpr std::uint32_t kCombatFlagDisplayValid = 1U << 3U;
inline constexpr std::uint32_t kCombatFlagNameValid = 1U << 4U;
inline constexpr std::uint32_t kCombatFlagPartial = 1U << 5U;

[[nodiscard]] inline bool IsPositiveDamage(const std::int64_t final_value) noexcept {
    return final_value > 0;
}

[[nodiscard]] inline bool IsPositiveDamage(const CombatEvent& event) noexcept {
    return IsPositiveDamage(event.final_value);
}

// The v1.8 unified stream reports healing in `value`; some producers also
// populate `final_value`, so retain it as a compatibility fallback.
[[nodiscard]] inline std::int64_t PositiveCombatEventAmount(
    const CombatEventData& event) noexcept {
    if (event.kind == CombatEventKind::Heal) {
        if (event.value > 0) return event.value;
        return event.final_value > 0 ? event.final_value : 0;
    }
    if (event.kind == CombatEventKind::Damage) {
        return event.final_value > 0 ? event.final_value : 0;
    }
    return 0;
}

[[nodiscard]] inline bool IsCriticalPositiveDamage(
    const std::int64_t final_value,
    const std::uint32_t flags) noexcept {
    return IsPositiveDamage(final_value) && (flags & kDamageFlagCritical) != 0;
}

[[nodiscard]] inline bool IsCriticalPositiveDamage(const CombatEvent& event) noexcept {
    return IsCriticalPositiveDamage(event.final_value, event.flags);
}

struct AggregateRow {
    std::string label;
    std::int64_t damage{};
    std::uint64_t positive_hits{};
    std::int64_t maximum_hit{};
};

struct PlayerRow {
    std::string identity_key;
    Handle handle;
    std::string object_path;
    std::string label;
    // ECharacterElementType suffix resolved from the live DT_Character row, or
    // empty when the character could not be matched.
    std::string attribute;
    std::string display_name;
    std::string name_origin;
    std::string name_resource_version;
    std::int64_t damage{};
    std::uint64_t positive_hits{};
    std::uint64_t critical_hits{};
    std::uint64_t head_hits{};
    std::uint64_t weak_hits{};
    std::int64_t maximum_hit{};
    std::uint64_t maximum_source_id{};
    std::string maximum_source_name;
    std::string maximum_source_display_name;
    std::string maximum_source_name_origin;
    std::string maximum_source_resource_version;
    double damage_share{};
    double encounter_dps{};
    double active_dps{};
    double rolling_dps{};
    std::int64_t incoming_damage{};
    std::uint64_t incoming_positive_hits{};
    std::int64_t maximum_incoming_hit{};
    double incoming_share{};
    std::int64_t healing_done{};
    std::uint64_t healing_done_events{};
    std::int64_t maximum_heal{};
    double healing_share{};
    double encounter_hps{};
    std::int64_t healing_received{};
    std::uint64_t healing_received_events{};
    std::int64_t maximum_heal_received{};
    double healing_received_share{};
    double healing_received_hps{};
};

[[nodiscard]] inline double PlayerRate(
    const std::uint64_t count, const std::uint64_t positive_hits) noexcept {
    return positive_hits == 0 ? 0.0 : static_cast<double>(count) * 100.0 /
        static_cast<double>(positive_hits);
}

[[nodiscard]] inline double PlayerCriticalRate(const PlayerRow& row) noexcept {
    return PlayerRate(row.critical_hits, row.positive_hits);
}

[[nodiscard]] inline double PlayerHeadRate(const PlayerRow& row) noexcept {
    return PlayerRate(row.head_hits, row.positive_hits);
}

[[nodiscard]] inline double PlayerWeakRate(const PlayerRow& row) noexcept {
    return PlayerRate(row.weak_hits, row.positive_hits);
}

enum class EventDirection { Visible, Outgoing, Incoming, Both };

struct RecentEvent : CombatEventData {
    EventDirection direction{EventDirection::Outgoing};
    std::string source_name;
    std::string source_display_name;
    std::string attacker_name;
    std::string attacker_display_name;
    std::string victim_name;
    std::string victim_display_name;
};

struct Quality {
    bool partial{};
    bool overflow{};
    bool cursor_gap{};
    bool aggregate_truncated{};
};

struct DamageFlagCount {
    std::uint32_t flags{};
    std::uint64_t count{};
};

struct EventTypeCounts {
    std::uint64_t damage{};
    std::uint64_t heal{};
    std::uint64_t buff_add{};
    std::uint64_t buff_remove{};
};

struct TimedDamage {
    TimePoint time;
    std::int64_t damage{};
};

struct SourceKey {
    std::string source_identity;
    Handle participant;
    friend bool operator==(const SourceKey&, const SourceKey&) = default;
};

struct SourceKeyHash {
    std::size_t operator()(const SourceKey& value) const noexcept;
};

struct Snapshot {
    EncounterState state{EncounterState::Idle};
    double duration_seconds{};
    std::size_t controlled_character_count{};
    std::int64_t outgoing_damage{};
    std::int64_t incoming_damage{};
    std::uint64_t outgoing_positive_hits{};
    std::uint64_t incoming_positive_hits{};
    std::uint64_t non_positive_events{};
    std::int64_t outgoing_healing{};
    std::int64_t incoming_healing{};
    std::uint64_t outgoing_heal_events{};
    std::uint64_t incoming_heal_events{};
    double encounter_hps{};
    double encounter_received_hps{};
    std::uint64_t buff_add_events{};
    std::uint64_t buff_remove_events{};
    std::int64_t maximum_outgoing_hit{};
    double encounter_dps{};
    double rolling_dps{};
    double hp{};
    double max_hp{};
    double shield{};
    Quality quality;
    CriticalDataStatus critical_data_status{CriticalDataStatus::Unavailable};
    std::vector<DamageFlagCount> damage_flag_histogram;
    std::vector<AggregateRow> outgoing_sources;
    std::vector<AggregateRow> outgoing_targets;
    std::vector<AggregateRow> incoming_sources;
    std::vector<PlayerRow> players;
    std::vector<RecentEvent> recent_events;
};

class CombatStatsEngine {
public:
    // Resolves a participant's element from its object path. Supplied by the
    // plugin, which owns the live DT_Character lookup; the engine stays free of
    // host services and of any character table of its own.
    using AttributeResolver = std::function<std::string(const std::string& object_path)>;

    void SetAttributeResolver(AttributeResolver resolver);

    void ObserveCombatant(const Combatant& combatant, TimePoint now);
    [[nodiscard]] bool IsRelevant(Handle world, Handle attacker, Handle victim) const;
    bool AcceptDamage(const DamageEvent& event, TimePoint now);
    bool AcceptEvent(const CombatEvent& event, TimePoint now);
    void Advance(TimePoint now);
    void CompleteEncounter() noexcept;
    void Reset();
    void MarkPartial(bool cursor_gap);
    void MarkOverflow();
    [[nodiscard]] EncounterState state() const noexcept { return state_; }
    [[nodiscard]] Snapshot BuildSnapshot(TimePoint now) const;

private:
    struct PlayerAggregate {
        Handle handle;
        std::string object_path;
        // Object path used for the attribute lookup; object_path may hold a
        // display name once the Host resolves one.
        std::string attribute_path;
        std::string label;
        std::string display_name;
        std::string name_origin;
        std::string name_resource_version;
        std::int64_t damage{};
        std::uint64_t positive_hits{};
        std::uint64_t critical_hits{};
        std::uint64_t head_hits{};
        std::uint64_t weak_hits{};
        std::int64_t maximum_hit{};
        std::uint64_t maximum_source_id{};
        std::string maximum_source_name;
        std::string maximum_source_display_name;
        std::string maximum_source_name_origin;
        std::string maximum_source_resource_version;
        TimePoint first_positive_damage{};
        TimePoint last_positive_damage{};
        std::vector<TimedDamage> rolling_damage;
        std::int64_t incoming_damage{};
        std::uint64_t incoming_positive_hits{};
        std::int64_t maximum_incoming_hit{};
        std::int64_t healing_done{};
        std::uint64_t healing_done_events{};
        std::int64_t maximum_heal{};
        std::int64_t healing_received{};
        std::uint64_t healing_received_events{};
        std::int64_t maximum_heal_received{};
    };

    PlayerAggregate& ConfirmControlledActor(
        Handle handle,
        const std::string& object_path,
        const std::string& display_name,
        const std::string& name_origin,
        const std::string& resource_version,
        const std::string& attribute_path);
    void ClearEncounter();

    EncounterState state_{EncounterState::Idle};
    AttributeResolver attribute_resolver_;
    Combatant combatant_;
    std::unordered_set<Handle, HandleHash> controlled_characters_;
    std::unordered_set<Handle, HandleHash> engaged_actors_;
    TimePoint encounter_started_{};
    TimePoint last_activity_{};
    TimePoint first_positive_outgoing_{};
    TimePoint last_positive_outgoing_{};
    std::int64_t outgoing_damage_{};
    std::int64_t incoming_damage_{};
    std::uint64_t outgoing_positive_hits_{};
    std::uint64_t incoming_positive_hits_{};
    std::uint64_t non_positive_events_{};
    std::int64_t outgoing_healing_{};
    std::int64_t incoming_healing_{};
    std::uint64_t outgoing_heal_events_{};
    std::uint64_t incoming_heal_events_{};
    std::uint64_t buff_add_events_{};
    std::uint64_t buff_remove_events_{};
    std::uint64_t complete_damage_metadata_positive_hits_{};
    std::int64_t maximum_outgoing_hit_{};
    std::vector<RecentEvent> recent_events_;
    std::unordered_map<std::uint32_t, std::uint64_t> damage_flag_histogram_;
    std::vector<TimedDamage> rolling_outgoing_;
    std::unordered_map<std::string, AggregateRow> outgoing_sources_;
    std::unordered_map<Handle, AggregateRow, HandleHash> outgoing_targets_;
    std::unordered_map<SourceKey, AggregateRow, SourceKeyHash> incoming_sources_;
    std::unordered_map<std::string, PlayerAggregate> confirmed_controlled_actors_;
    std::unordered_map<Handle, std::string, HandleHash> confirmed_actor_keys_;
    AggregateRow other_outgoing_source_;
    AggregateRow other_outgoing_target_;
    AggregateRow other_incoming_source_;
    Quality quality_;
    Quality pending_quality_;
};

}  // namespace nte::combat_stats
