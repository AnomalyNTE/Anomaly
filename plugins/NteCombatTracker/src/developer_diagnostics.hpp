#pragma once

#include "account_identity.hpp"
#include "combat_stats_engine.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nte::developer {

inline constexpr std::size_t kMaximumDebugRecords = 256;
inline constexpr std::string_view kAuthorizedDeveloperUid = "215017018089";

[[nodiscard]] bool IsDeveloperAccount(
    const account::AccountIdentity& account) noexcept;
[[nodiscard]] bool EffectiveDeveloperMode(
    bool saved_preference,
    const account::AccountIdentity& account) noexcept;

class AccessState {
public:
    void SetSavedPreference(bool enabled) noexcept;
    void ObserveAccount(std::optional<account::AccountIdentity> account) noexcept;
    [[nodiscard]] bool saved_preference() const noexcept {
        return saved_preference_;
    }
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] std::optional<bool> AuthorizePreferenceRequest(
        bool requested) const noexcept;

private:
    bool saved_preference_{};
    std::optional<account::AccountIdentity> observed_account_;
};

struct MapIdentity {
    combat_stats::Handle handle;
    std::uint32_t name_id{};
    std::string technical_name;
    std::string status;
};

struct ParticipantIdentity {
    combat_stats::Handle handle;
    std::string technical_path;
    std::string display_name;
    std::string enemy_config_id;
    std::string monster_id;
};

struct DamageRecord {
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t flags{};
    combat_stats::Handle world;
    ParticipantIdentity attacker;
    ParticipantIdentity victim;
    std::uint64_t source_id{};
    std::int64_t value{};
    std::int64_t basic_value{};
    std::int64_t final_value{};
    double hit_location[3]{};
    std::uint32_t damage_type{};
    std::uint32_t display_type{};
    std::uint32_t reaction_type{};
    std::uint32_t reaction_display_type{};
    bool player_related{};
    bool accepted{};
};

struct CombatEventRecord {
    combat_stats::CombatEventKind kind{combat_stats::CombatEventKind::Damage};
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t flags{};
    combat_stats::Handle world;
    ParticipantIdentity attacker;
    ParticipantIdentity victim;
    std::uint64_t name_id{};
    std::string event_name;
    std::string event_display_name;
    std::int64_t value{};
    std::int64_t basic_value{};
    std::int64_t final_value{};
    std::uint32_t damage_type{};
    std::uint32_t display_type{};
    std::uint32_t reaction_type{};
    std::uint32_t reaction_display_type{};
    float duration_seconds{};
    std::int32_t stack_count{};
    bool player_related{};
    bool accepted{};
};

struct SkillSnapshot {
    std::uint32_t flags{};
    combat_stats::Handle handle;
    combat_stats::Handle character;
    combat_stats::Handle ability_class;
    std::uint64_t sequence{};
    std::int32_t level{};
    std::int32_t input_id{};
    bool cooldown_valid{};
    float cooldown_remaining_seconds{};
    float cooldown_duration_seconds{};
    std::string ability_path;
    std::string ability_display_name;

    friend bool operator==(const SkillSnapshot&, const SkillSnapshot&) = default;
};

enum class SkillChangeKind : std::uint8_t { Added, Changed, Removed };

struct SkillRecord {
    SkillChangeKind kind{SkillChangeKind::Added};
    std::uint64_t frame_generation{};
    std::uint64_t frame_sequence{};
    SkillSnapshot skill;
};

struct SkillFrame {
    std::uint64_t generation{};
    std::uint64_t sequence{};
    bool complete{};
    std::vector<SkillSnapshot> skills;
};

struct Snapshot {
    bool enabled{};
    std::optional<MapIdentity> current_map;
    std::optional<ParticipantIdentity> current_target;
    std::vector<DamageRecord> damage_records;
    std::vector<CombatEventRecord> combat_event_records;
    std::vector<SkillRecord> skill_records;
    std::uint64_t damage_overwritten{};
    std::uint64_t combat_event_overwritten{};
    std::uint64_t skill_overwritten{};
};

class Diagnostics {
public:
    void SetEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void SetCurrentMap(std::optional<MapIdentity> map);
    void SetCurrentTarget(std::optional<ParticipantIdentity> target);
    void RecordDamage(DamageRecord record);
    void RecordCombatEvent(CombatEventRecord record);
    void ObserveSkillFrame(SkillFrame frame);
    void ClearDamage() noexcept;
    void ClearSkills() noexcept;
    void ClearAll() noexcept;
    [[nodiscard]] Snapshot BuildSnapshot() const;

private:
    void AddSkillRecord(SkillRecord record);

    bool enabled_{};
    std::optional<MapIdentity> current_map_;
    std::optional<ParticipantIdentity> current_target_;
    std::deque<DamageRecord> damage_records_;
    std::deque<CombatEventRecord> combat_event_records_;
    std::deque<SkillRecord> skill_records_;
    std::unordered_map<combat_stats::Handle, SkillSnapshot,
        combat_stats::HandleHash> skill_baseline_;
    std::uint64_t skill_generation_{};
    std::uint64_t damage_overwritten_{};
    std::uint64_t combat_event_overwritten_{};
    std::uint64_t skill_overwritten_{};
};

}  // namespace nte::developer
