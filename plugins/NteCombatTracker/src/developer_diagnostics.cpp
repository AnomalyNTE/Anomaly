#include "developer_diagnostics.hpp"

#include <unordered_set>
#include <utility>

namespace nte::developer {

bool IsDeveloperAccount(const account::AccountIdentity& account) noexcept {
    return account.known() && account.uid == kAuthorizedDeveloperUid;
}

bool EffectiveDeveloperMode(
    const bool saved_preference,
    const account::AccountIdentity& account) noexcept {
    return saved_preference && IsDeveloperAccount(account);
}

void AccessState::SetSavedPreference(const bool enabled) noexcept {
    saved_preference_ = enabled;
}

void AccessState::ObserveAccount(
    std::optional<account::AccountIdentity> account) noexcept {
    observed_account_ = std::move(account);
}

bool AccessState::available() const noexcept {
    return observed_account_ && IsDeveloperAccount(*observed_account_);
}

bool AccessState::enabled() const noexcept {
    return observed_account_ && EffectiveDeveloperMode(
        saved_preference_, *observed_account_);
}

std::optional<bool> AccessState::AuthorizePreferenceRequest(
    const bool requested) const noexcept {
    if (!available()) return std::nullopt;
    return requested;
}

void Diagnostics::SetEnabled(const bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    ClearAll();
}

void Diagnostics::SetCurrentMap(std::optional<MapIdentity> map) {
    if (!enabled_) return;
    current_map_ = std::move(map);
}

void Diagnostics::SetCurrentTarget(std::optional<ParticipantIdentity> target) {
    if (!enabled_) return;
    current_target_ = std::move(target);
}

void Diagnostics::RecordDamage(DamageRecord record) {
    if (!enabled_) return;
    if (damage_records_.size() == kMaximumDebugRecords) {
        damage_records_.pop_front();
        ++damage_overwritten_;
    }
    damage_records_.push_back(std::move(record));
}

void Diagnostics::RecordCombatEvent(CombatEventRecord record) {
    if (!enabled_) return;
    if (combat_event_records_.size() == kMaximumDebugRecords) {
        combat_event_records_.pop_front();
        ++combat_event_overwritten_;
    }
    combat_event_records_.push_back(std::move(record));
}

void Diagnostics::AddSkillRecord(SkillRecord record) {
    if (skill_records_.size() == kMaximumDebugRecords) {
        skill_records_.pop_front();
        ++skill_overwritten_;
    }
    skill_records_.push_back(std::move(record));
}

void Diagnostics::ObserveSkillFrame(SkillFrame frame) {
    if (!enabled_) return;
    if (skill_generation_ != frame.generation) {
        skill_generation_ = frame.generation;
        skill_baseline_.clear();
    }

    std::unordered_set<combat_stats::Handle, combat_stats::HandleHash> observed;
    observed.reserve(frame.skills.size());
    for (auto& skill : frame.skills) {
        observed.insert(skill.handle);
        const auto existing = skill_baseline_.find(skill.handle);
        if (existing == skill_baseline_.end()) {
            AddSkillRecord({SkillChangeKind::Added,
                frame.generation, frame.sequence, skill});
        } else if (existing->second != skill) {
            AddSkillRecord({SkillChangeKind::Changed,
                frame.generation, frame.sequence, skill});
        }
        skill_baseline_.insert_or_assign(skill.handle, std::move(skill));
    }

    if (!frame.complete) return;
    for (auto iterator = skill_baseline_.begin();
         iterator != skill_baseline_.end();) {
        if (observed.contains(iterator->first)) {
            ++iterator;
            continue;
        }
        AddSkillRecord({SkillChangeKind::Removed,
            frame.generation, frame.sequence, iterator->second});
        iterator = skill_baseline_.erase(iterator);
    }
}

void Diagnostics::ClearDamage() noexcept {
    damage_records_.clear();
    damage_overwritten_ = 0;
}

void Diagnostics::ClearSkills() noexcept {
    skill_records_.clear();
    skill_baseline_.clear();
    skill_generation_ = 0;
    skill_overwritten_ = 0;
}

void Diagnostics::ClearAll() noexcept {
    current_map_.reset();
    current_target_.reset();
    ClearDamage();
    combat_event_records_.clear();
    combat_event_overwritten_ = 0;
    ClearSkills();
}

Snapshot Diagnostics::BuildSnapshot() const {
    Snapshot result;
    result.enabled = enabled_;
    result.current_map = current_map_;
    result.current_target = current_target_;
    result.damage_records.assign(damage_records_.begin(), damage_records_.end());
    result.combat_event_records.assign(combat_event_records_.begin(), combat_event_records_.end());
    result.skill_records.assign(skill_records_.begin(), skill_records_.end());
    result.damage_overwritten = damage_overwritten_;
    result.combat_event_overwritten = combat_event_overwritten_;
    result.skill_overwritten = skill_overwritten_;
    return result;
}

}  // namespace nte::developer
