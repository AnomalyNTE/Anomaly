#include "anomaly/hook_manager.hpp"

#include <algorithm>
#include <utility>

namespace anomaly {

struct HookManager::HookRecord {
    std::string label;
    void* target{};
    bool enabled{};
    bool stopping{};
    std::uint64_t ledger_token{};
    std::shared_ptr<PluginScope> callback_scope;
};

struct HookManager::OwnerGroup {
    std::string owner;
    std::uint64_t generation{};
    std::shared_ptr<PluginScope> scope;
    std::vector<HookRecord> hooks;
};

HookManager::HookManager(
    std::unique_ptr<HookBackend> backend, std::shared_ptr<ResourceLedger> ledger)
    : backend_(std::move(backend)), ledger_(std::move(ledger)) {}

HookManager::~HookManager() {
    std::vector<std::pair<std::string, std::uint64_t>> owners;
    {
        std::scoped_lock lock(mutex_);
        owners.reserve(groups_.size());
        for (const auto& [key, group] : groups_) owners.emplace_back(group.owner, group.generation);
    }
    for (const auto& [owner, generation] : owners) {
        static_cast<void>(RemoveOwner(owner, generation, std::chrono::milliseconds(5000)));
    }
    std::scoped_lock lock(mutex_);
    if (initialized_) backend_->Uninitialize();
    initialized_ = false;
}

std::string HookManager::GroupKey(std::string_view owner, std::uint64_t generation) {
    return std::string(owner) + '#' + std::to_string(generation);
}

bool HookManager::Create(
    std::string owner, std::uint64_t generation, std::string label,
    void* target, void* detour, void** original) {
    if (owner.empty() || target == nullptr || detour == nullptr || original == nullptr) return false;
    std::scoped_lock lock(mutex_);
    if (targets_.contains(target)) return false;
    const std::string key = GroupKey(owner, generation);

    // Prepare all bookkeeping before touching the backend. This makes an
    // allocation/ledger failure a normal rejected Create rather than a live
    // detour with no owner record that the destructor can remove later.
    OwnerGroup* group{};
    bool group_inserted{};
    bool hook_inserted{};
    bool target_inserted{};
    std::uint64_t ledger_token{};
    bool initialized_here{};
    bool backend_created{};
    bool preserve_record{};
    const auto rollback = [&]() noexcept {
        if (backend_created) {
            // If removal itself fails, retain the complete owner record and
            // initialized backend so a later RemoveOwner/destructor can retry.
            preserve_record = !backend_->Remove(target);
        }
        if (preserve_record) return;
        if (target_inserted) targets_.erase(target);
        if (group != nullptr && hook_inserted) {
            HookRecord& hook = group->hooks.back();
            if (hook.callback_scope != nullptr && hook.ledger_token != 0) {
                static_cast<void>(hook.callback_scope->Release(hook.ledger_token));
            }
            group->hooks.pop_back();
        }
        if (group_inserted) groups_.erase(key);
        if (initialized_here && groups_.empty()) {
            backend_->Uninitialize();
            initialized_ = false;
        }
        *original = nullptr;
    };

    try {
        auto [position, inserted] = groups_.try_emplace(key);
        group = &position->second;
        group_inserted = inserted;
        if (inserted) {
            group->owner = owner;
            group->generation = generation;
            group->scope = std::make_shared<PluginScope>(ledger_, owner, generation);
        }
        auto callback_scope = std::make_shared<PluginScope>(ledger_, owner, generation);
        ledger_token = callback_scope->Register(
            PluginResourceKind::Hook, label.empty() ? "hook" : label);
        if (ledger_token == 0) {
            rollback();
            return false;
        }
        group->hooks.push_back(
            {std::move(label), target, false, false, ledger_token, std::move(callback_scope)});
        hook_inserted = true;
        const auto [target_position, inserted_target] = targets_.emplace(target, key);
        static_cast<void>(target_position);
        if (!inserted_target) {
            rollback();
            return false;
        }
        target_inserted = true;

        if (!initialized_) {
            if (!backend_ || !backend_->Initialize()) {
                rollback();
                return false;
            }
            initialized_ = true;
            initialized_here = true;
        }
        if (!backend_->Create(target, detour, original)) {
            rollback();
            return false;
        }
        backend_created = true;
        return true;
    } catch (...) {
        rollback();
        return false;
    }
}

bool HookManager::Enable(
    std::string_view owner, const std::uint64_t generation, void* target) noexcept {
    if (target == nullptr) return false;
    std::scoped_lock lock(mutex_);
    const auto found = groups_.find(GroupKey(owner, generation));
    if (found == groups_.end()) return false;
    const auto hook = std::find_if(
        found->second.hooks.begin(), found->second.hooks.end(),
        [&](const HookRecord& candidate) { return candidate.target == target; });
    if (hook == found->second.hooks.end() || hook->stopping) return false;
    if (hook->enabled) return true;
    if (backend_ == nullptr || !backend_->Enable(hook->target)) return false;
    hook->enabled = true;
    return true;
}

bool HookManager::EnableOwner(std::string_view owner, std::uint64_t generation) noexcept {
    std::scoped_lock lock(mutex_);
    const auto found = groups_.find(GroupKey(owner, generation));
    if (found == groups_.end()) return false;
    std::vector<HookRecord*> enabled_now;
    for (HookRecord& hook : found->second.hooks) {
        if (hook.enabled || hook.stopping) continue;
        if (backend_ == nullptr || !backend_->Enable(hook.target)) {
            for (HookRecord* rollback : enabled_now) {
                static_cast<void>(backend_->Disable(rollback->target));
                rollback->enabled = false;
            }
            return false;
        }
        hook.enabled = true;
        enabled_now.push_back(&hook);
    }
    return true;
}

bool HookManager::DisableOwner(std::string_view owner, std::uint64_t generation) noexcept {
    std::scoped_lock lock(mutex_);
    const auto found = groups_.find(GroupKey(owner, generation));
    if (found == groups_.end()) return false;
    bool success = true;
    for (auto iterator = found->second.hooks.rbegin(); iterator != found->second.hooks.rend(); ++iterator) {
        if (!iterator->enabled) continue;
        if (backend_ == nullptr || !backend_->Disable(iterator->target)) {
            success = false;
            continue;
        }
        iterator->enabled = false;
    }
    return success;
}

bool HookManager::Remove(
    std::string_view owner, const std::uint64_t generation, void* target,
    const std::chrono::milliseconds callback_timeout) noexcept {
    if (target == nullptr) return false;
    const std::string key = GroupKey(owner, generation);
    std::shared_ptr<PluginScope> callback_scope;
    {
        std::scoped_lock lock(mutex_);
        const auto found = groups_.find(key);
        if (found == groups_.end()) return false;
        const auto hook = std::find_if(
            found->second.hooks.begin(), found->second.hooks.end(),
            [&](const HookRecord& candidate) { return candidate.target == target; });
        if (hook == found->second.hooks.end()) return false;
        hook->stopping = true;
        if (hook->enabled) {
            if (backend_ == nullptr || !backend_->Disable(hook->target)) return false;
            hook->enabled = false;
        }
        callback_scope = hook->callback_scope;
    }
    if (callback_scope == nullptr || !callback_scope->BeginStop(callback_timeout)) return false;

    std::scoped_lock lock(mutex_);
    const auto found = groups_.find(key);
    if (found == groups_.end()) return false;
    const auto hook = std::find_if(
        found->second.hooks.begin(), found->second.hooks.end(),
        [&](const HookRecord& candidate) { return candidate.target == target; });
    if (hook == found->second.hooks.end()) return false;
    if (backend_ == nullptr || !backend_->Remove(hook->target)) return false;
    targets_.erase(hook->target);
    static_cast<void>(hook->callback_scope->Release(hook->ledger_token));
    found->second.hooks.erase(hook);
    if (found->second.hooks.empty()) groups_.erase(found);
    return true;
}

bool HookManager::RemoveOwner(
    std::string_view owner, std::uint64_t generation,
    std::chrono::milliseconds callback_timeout) noexcept {
    const std::string key = GroupKey(owner, generation);
    std::vector<std::shared_ptr<PluginScope>> callback_scopes;
    std::shared_ptr<PluginScope> owner_scope;
    bool disabled = true;
    {
        std::scoped_lock lock(mutex_);
        const auto found = groups_.find(key);
        if (found == groups_.end()) return false;
        callback_scopes.reserve(found->second.hooks.size());
        for (auto iterator = found->second.hooks.rbegin(); iterator != found->second.hooks.rend(); ++iterator) {
            iterator->stopping = true;
            if (iterator->enabled) {
                if (backend_ == nullptr || !backend_->Disable(iterator->target)) {
                    disabled = false;
                } else {
                    iterator->enabled = false;
                }
            }
            callback_scopes.push_back(iterator->callback_scope);
        }
        owner_scope = found->second.scope;
    }
    if (!disabled) return false;
    const auto deadline = callback_timeout == std::chrono::milliseconds::max()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + callback_timeout;
    const auto remaining = [&] {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            return std::chrono::milliseconds::max();
        }
        const auto now = std::chrono::steady_clock::now();
        return now >= deadline ? std::chrono::milliseconds::zero()
            : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    };
    if (owner_scope == nullptr || !owner_scope->BeginStop(remaining())) return false;
    for (const auto& callback_scope : callback_scopes) {
        if (callback_scope == nullptr || !callback_scope->BeginStop(remaining())) return false;
    }

    std::scoped_lock lock(mutex_);
    const auto found = groups_.find(key);
    if (found == groups_.end()) return false;
    bool success = true;
    std::vector<void*> removed_targets;
    removed_targets.reserve(found->second.hooks.size());
    for (auto iterator = found->second.hooks.rbegin(); iterator != found->second.hooks.rend(); ++iterator) {
        if (backend_ == nullptr || !backend_->Remove(iterator->target)) {
            success = false;
            continue;
        }
        removed_targets.push_back(iterator->target);
    }
    for (void* target : removed_targets) {
        const auto hook = std::find_if(
            found->second.hooks.begin(), found->second.hooks.end(),
            [&](const HookRecord& candidate) { return candidate.target == target; });
        if (hook == found->second.hooks.end()) continue;
        targets_.erase(hook->target);
        static_cast<void>(hook->callback_scope->Release(hook->ledger_token));
        found->second.hooks.erase(hook);
    }
    if (found->second.hooks.empty()) groups_.erase(found);
    return success;
}

PluginScope::CallbackLease HookManager::AcquireCallback(
    std::string_view owner, std::uint64_t generation) noexcept {
    std::shared_ptr<PluginScope> scope;
    {
        std::scoped_lock lock(mutex_);
        const auto found = groups_.find(GroupKey(owner, generation));
        if (found == groups_.end()) return {};
        scope = found->second.scope;
    }
    return scope->AcquireCallback(generation);
}

PluginScope::CallbackLease HookManager::AcquireCallback(
    std::string_view owner, const std::uint64_t generation, void* target) noexcept {
    if (target == nullptr) return {};
    std::shared_ptr<PluginScope> scope;
    {
        std::scoped_lock lock(mutex_);
        const auto found = groups_.find(GroupKey(owner, generation));
        if (found == groups_.end()) return {};
        const auto hook = std::find_if(
            found->second.hooks.begin(), found->second.hooks.end(),
            [&](const HookRecord& candidate) {
                return candidate.target == target && !candidate.stopping;
            });
        if (hook == found->second.hooks.end()) return {};
        scope = hook->callback_scope;
    }
    return scope == nullptr ? PluginScope::CallbackLease{} : scope->AcquireCallback(generation);
}

std::vector<HookRecordView> HookManager::Snapshot() const {
    std::vector<HookRecordView> result;
    std::scoped_lock lock(mutex_);
    for (const auto& [key, group] : groups_) {
        for (const HookRecord& hook : group.hooks) {
            result.push_back({group.owner, group.generation, hook.label, hook.target, hook.enabled});
        }
    }
    std::sort(result.begin(), result.end(), [](const HookRecordView& left, const HookRecordView& right) {
        if (left.owner != right.owner) return left.owner < right.owner;
        if (left.generation != right.generation) return left.generation < right.generation;
        return left.label < right.label;
    });
    return result;
}

}  // namespace anomaly
