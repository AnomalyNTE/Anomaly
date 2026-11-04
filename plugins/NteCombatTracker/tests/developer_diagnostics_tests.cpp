#include "developer_diagnostics.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void DeveloperModeRequiresTheAuthorizedAccountAndSavedPreference() {
    const auto authorized = nte::account::FromUidText("215017018089");
    const auto other = nte::account::FromUidText("216065736008");
    Check(authorized && other,
        "developer access fixtures should contain valid account identities");
    Check(nte::developer::IsDeveloperAccount(*authorized) &&
            !nte::developer::IsDeveloperAccount(*other) &&
            !nte::developer::IsDeveloperAccount(nte::account::Unknown()),
        "only the configured account should be eligible for developer mode");
    Check(nte::developer::EffectiveDeveloperMode(true, *authorized) &&
            !nte::developer::EffectiveDeveloperMode(false, *authorized) &&
            !nte::developer::EffectiveDeveloperMode(true, *other) &&
            !nte::developer::EffectiveDeveloperMode(
                true, nte::account::Unknown()),
        "developer mode should require both saved preference and account eligibility");

    nte::developer::AccessState access;
    access.SetSavedPreference(true);
    access.ObserveAccount(authorized);
    Check(access.available() && access.enabled(),
        "the authorized current observation should restore the saved preference");
    access.ObserveAccount(std::nullopt);
    Check(!access.available() && !access.enabled() && access.saved_preference(),
        "an unknown current observation should revoke access without clearing preference");
    access.SetSavedPreference(true);
    Check(!access.enabled() && !access.AuthorizePreferenceRequest(true),
        "config refresh and queued requests should not enable an unknown account");
    access.ObserveAccount(other);
    Check(!access.available() && !access.enabled() &&
            !access.AuthorizePreferenceRequest(false),
        "another current account should remain unauthorized for queued requests");
    access.ObserveAccount(authorized);
    const auto authorized_request = access.AuthorizePreferenceRequest(false);
    Check(access.available() && access.enabled() && authorized_request &&
            !*authorized_request,
        "returning to the authorized account should restore preference and authorize control");
}

nte::developer::SkillSnapshot Skill(
    const std::uint64_t id, const std::uint32_t flags,
    const float remaining = 0.0F) {
    nte::developer::SkillSnapshot skill;
    skill.handle = {id, 1};
    skill.character = {100, 1};
    skill.ability_class = {id + 1000, 1};
    skill.flags = flags;
    skill.sequence = id;
    skill.level = 2;
    skill.input_id = 3;
    skill.cooldown_valid = remaining > 0.0F;
    skill.cooldown_remaining_seconds = remaining;
    skill.cooldown_duration_seconds = remaining > 0.0F ? 10.0F : 0.0F;
    skill.ability_path = "/Game/Abilities/Test" + std::to_string(id);
    return skill;
}

void DeveloperBuffersAreBoundedAndClearedWhenDisabled() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    for (std::uint64_t index{}; index < 300; ++index) {
        nte::developer::DamageRecord record;
        record.sequence = index + 1;
        diagnostics.RecordDamage(std::move(record));
    }
    const auto snapshot = diagnostics.BuildSnapshot();
    Check(snapshot.enabled && snapshot.damage_records.size() == 256 &&
            snapshot.damage_records.front().sequence == 45 &&
            snapshot.damage_overwritten == 44,
        "damage diagnostics should retain the newest 256 records and count overwrites");

    diagnostics.SetEnabled(false);
    const auto disabled = diagnostics.BuildSnapshot();
    Check(!disabled.enabled && disabled.damage_records.empty() &&
            disabled.combat_event_records.empty() &&
            disabled.skill_records.empty() && disabled.damage_overwritten == 0,
        "disabling developer mode should clear all in-memory diagnostics");
}

void UnifiedCombatEventsRetainBuffDetails() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    nte::developer::CombatEventRecord record;
    record.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    record.sequence = 42;
    record.name_id = 9001;
    record.duration_seconds = 12.5F;
    record.stack_count = 3;
    record.accepted = true;
    diagnostics.RecordCombatEvent(record);
    const auto snapshot = diagnostics.BuildSnapshot();
    Check(snapshot.combat_event_records.size() == 1 &&
            snapshot.combat_event_records.front().kind ==
                nte::combat_stats::CombatEventKind::BuffAdd &&
            snapshot.combat_event_records.front().duration_seconds == 12.5F &&
            snapshot.combat_event_records.front().stack_count == 3,
        "unified diagnostics should retain complete BUFF event details");
}

void MapTargetAndEventParticipantStateClearsWhenDisabled() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    diagnostics.SetCurrentMap(nte::developer::MapIdentity{
        {10, 2}, 42, "World_City", "available"});
    diagnostics.SetCurrentTarget(nte::developer::ParticipantIdentity{
        {20, 3}, "/Game/Enemy/Boss_07_BP_WorldBoss_C", "塞润尼缇",
        "Boss_07_BP_WorldBoss", "Boss_07"});
    nte::developer::DamageRecord damage;
    damage.attacker = {{30, 4}, "/Game/Enemy/Boss_05_BP_C", "墨菲克斯",
        "boss_05_BP", "Boss_05"};
    damage.victim = {{40, 5}, "/Game/Player/player_076_shinku_C", "真红", {}, {}};
    diagnostics.RecordDamage(std::move(damage));

    const auto active = diagnostics.BuildSnapshot();
    Check(active.current_map && active.current_map->handle ==
            nte::combat_stats::Handle{10, 2} &&
            active.current_map->name_id == 42 &&
            active.current_map->technical_name == "World_City" &&
            active.current_target &&
            active.current_target->monster_id == "Boss_07" &&
            active.damage_records.size() == 1 &&
            active.damage_records[0].attacker.enemy_config_id == "boss_05_BP" &&
            active.damage_records[0].victim.technical_path ==
                "/Game/Player/player_076_shinku_C",
        "developer snapshots should expose map, target, and event participant identities");

    diagnostics.SetEnabled(false);
    const auto disabled = diagnostics.BuildSnapshot();
    Check(!disabled.current_map && !disabled.current_target &&
            disabled.damage_records.empty(),
        "disabling developer mode should clear map, target, and participant diagnostics");
}

void SkillFramesProduceChangesWithoutInventingPartialRemovals() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    diagnostics.ObserveSkillFrame({1, 10, true, {Skill(1, 1), Skill(2, 0)}});
    auto snapshot = diagnostics.BuildSnapshot();
    Check(snapshot.skill_records.size() == 2 &&
            snapshot.skill_records[0].kind == nte::developer::SkillChangeKind::Added,
        "the first complete frame should establish an observable added baseline");

    diagnostics.ObserveSkillFrame({1, 11, false, {Skill(1, 3, 4.0F)}});
    snapshot = diagnostics.BuildSnapshot();
    Check(snapshot.skill_records.size() == 3 &&
            snapshot.skill_records.back().kind ==
                nte::developer::SkillChangeKind::Changed,
        "partial frames should record present changes");

    diagnostics.ObserveSkillFrame({1, 12, true, {Skill(1, 3, 4.0F)}});
    snapshot = diagnostics.BuildSnapshot();
    Check(snapshot.skill_records.size() == 4 &&
            snapshot.skill_records.back().kind ==
                nte::developer::SkillChangeKind::Removed &&
            snapshot.skill_records.back().skill.handle.id == 2,
        "only a complete frame should confirm a removed skill");

    diagnostics.ObserveSkillFrame({2, 1, true, {Skill(9, 0)}});
    snapshot = diagnostics.BuildSnapshot();
    Check(snapshot.skill_records.back().kind ==
            nte::developer::SkillChangeKind::Added,
        "a generation change should start a new baseline without fabricated removals");
}

void SkillBufferIsBoundedAndCooldownValidityIsExplicit() {
    nte::developer::Diagnostics diagnostics;
    diagnostics.SetEnabled(true);
    nte::developer::SkillFrame frame{3, 90, true, {}};
    frame.skills.reserve(300);
    for (std::uint64_t index{}; index < 300; ++index) {
        auto skill = Skill(index + 1, 0, 5.0F);
        skill.cooldown_valid = false;
        frame.skills.push_back(std::move(skill));
    }
    diagnostics.ObserveSkillFrame(std::move(frame));
    const auto snapshot = diagnostics.BuildSnapshot();
    Check(snapshot.skill_records.size() == 256 &&
            snapshot.skill_overwritten == 44 &&
            snapshot.skill_records.front().skill.handle.id == 45,
        "skill diagnostics should retain the newest 256 changes and count overwrites");
    Check(!snapshot.skill_records.back().skill.cooldown_valid &&
            snapshot.skill_records.back().skill.cooldown_remaining_seconds == 5.0F,
        "cooldown availability should follow COOLDOWN_VALID rather than numeric values");
}

}  // namespace

int main() {
    DeveloperModeRequiresTheAuthorizedAccountAndSavedPreference();
    DeveloperBuffersAreBoundedAndClearedWhenDisabled();
    UnifiedCombatEventsRetainBuffDetails();
    MapTargetAndEventParticipantStateClearsWhenDisabled();
    SkillFramesProduceChangesWithoutInventingPartialRemovals();
    SkillBufferIsBoundedAndCooldownValidityIsExplicit();
    std::cout << "All developer diagnostics tests passed\n";
}
