#include "anomaly/plugin_scope.hpp"

#include <algorithm>
#include <utility>

namespace anomaly {

struct PluginScope::BarrierState {
    std::mutex mutex;
    std::condition_variable condition;
    bool accepting{true};
    bool lifecycle_allowed{};
    std::size_t in_flight{};
};

std::uint64_t ResourceLedger::Register(
    std::string owner, std::uint64_t generation,
    PluginResourceKind kind, std::string label, Revoker revoker) {
    std::scoped_lock lock(mutex_);
    const std::uint64_t token = next_token_++;
    entries_.emplace(token, Entry{
        PluginResourceRecord{token, std::move(owner), generation, kind, std::move(label)},
        std::move(revoker)});
    return token;
}

bool ResourceLedger::Release(std::uint64_t token) noexcept {
    Revoker revoker;
    {
        std::scoped_lock lock(mutex_);
        const auto found = entries_.find(token);
        if (found == entries_.end()) return false;
        revoker = std::move(found->second.revoker);
        entries_.erase(found);
    }
    if (revoker) {
        try { revoker(); } catch (...) {}
    }
    return true;
}

std::size_t ResourceLedger::Revoke(std::string_view owner, std::uint64_t generation) noexcept {
    std::vector<Entry> revoked;
    {
        std::scoped_lock lock(mutex_);
        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            if (iterator->second.record.owner == owner &&
                iterator->second.record.generation == generation) {
                revoked.push_back(std::move(iterator->second));
                iterator = entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    std::sort(revoked.begin(), revoked.end(), [](const Entry& left, const Entry& right) {
        return left.record.token > right.record.token;
    });
    for (Entry& entry : revoked) {
        if (!entry.revoker) continue;
        try { entry.revoker(); } catch (...) {}
    }
    return revoked.size();
}

std::size_t ResourceLedger::RevokeExcept(
    const std::string_view owner, const std::uint64_t generation,
    const PluginResourceKind preserved_kind) noexcept {
    std::vector<Entry> revoked;
    {
        std::scoped_lock lock(mutex_);
        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            if (iterator->second.record.owner == owner &&
                iterator->second.record.generation == generation &&
                iterator->second.record.kind != preserved_kind) {
                revoked.push_back(std::move(iterator->second));
                iterator = entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    std::sort(revoked.begin(), revoked.end(), [](const Entry& left, const Entry& right) {
        return left.record.token > right.record.token;
    });
    for (Entry& entry : revoked) {
        if (!entry.revoker) continue;
        try { entry.revoker(); } catch (...) {}
    }
    return revoked.size();
}

std::vector<PluginResourceRecord> ResourceLedger::Snapshot(
    std::optional<std::string_view> owner) const {
    std::vector<PluginResourceRecord> result;
    std::scoped_lock lock(mutex_);
    result.reserve(entries_.size());
    for (const auto& [token, entry] : entries_) {
        if (!owner || entry.record.owner == *owner) result.push_back(entry.record);
    }
    std::sort(result.begin(), result.end(),
        [](const PluginResourceRecord& left, const PluginResourceRecord& right) {
            return left.token < right.token;
        });
    return result;
}

PluginScope::CallbackLease::~CallbackLease() { Reset(); }

PluginScope::CallbackLease::CallbackLease(CallbackLease&& other) noexcept
    : state_(std::move(other.state_)) {}

PluginScope::CallbackLease& PluginScope::CallbackLease::operator=(CallbackLease&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = std::move(other.state_);
    }
    return *this;
}

void PluginScope::CallbackLease::Reset() noexcept {
    if (!state_) return;
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->in_flight != 0) --state_->in_flight;
    }
    state_->condition.notify_all();
    state_.reset();
}

PluginScope::PluginScope(
    std::shared_ptr<ResourceLedger> ledger, std::string owner, std::uint64_t generation)
    : ledger_(std::move(ledger)), owner_(std::move(owner)), generation_(generation),
      barrier_(std::make_shared<BarrierState>()) {}

std::uint64_t PluginScope::Register(
    PluginResourceKind kind, std::string label, ResourceLedger::Revoker revoker) {
    std::scoped_lock barrier_lock(barrier_->mutex);
    if (!barrier_->accepting) return 0;
    return ledger_->Register(owner_, generation_, kind, std::move(label), std::move(revoker));
}

bool PluginScope::Release(std::uint64_t token) noexcept { return ledger_->Release(token); }

PluginScope::CallbackLease PluginScope::AcquireCallback(std::uint64_t generation) noexcept {
    std::scoped_lock lock(barrier_->mutex);
    if (!barrier_->accepting || generation != generation_) return {};
    ++barrier_->in_flight;
    return CallbackLease(barrier_);
}

bool PluginScope::FreezeCallbackSources() noexcept {
    std::scoped_lock lock(barrier_->mutex);
    const bool was_accepting = barrier_->accepting;
    barrier_->accepting = false;
    return was_accepting;
}

PluginScope::CallbackLease PluginScope::AcquireLifecycleLease(
    std::uint64_t generation) noexcept {
    std::scoped_lock lock(barrier_->mutex);
    if (!barrier_->lifecycle_allowed || generation != generation_) return {};
    ++barrier_->in_flight;
    return CallbackLease(barrier_);
}

bool PluginScope::BeginStop(std::chrono::milliseconds timeout) noexcept {
    std::unique_lock lock(barrier_->mutex);
    barrier_->accepting = false;
    bool drained{};
    if (timeout == std::chrono::milliseconds::max()) {
        barrier_->condition.wait(lock, [&] { return barrier_->in_flight == 0; });
        drained = true;
    } else {
        drained = barrier_->condition.wait_for(
            lock, timeout, [&] { return barrier_->in_flight == 0; });
    }
    if (drained) barrier_->lifecycle_allowed = true;
    return drained;
}

std::size_t PluginScope::RevokeAllExcept(const PluginResourceKind preserved_kind) noexcept {
    return ledger_->RevokeExcept(owner_, generation_, preserved_kind);
}

std::size_t PluginScope::RevokeAll() noexcept { return ledger_->Revoke(owner_, generation_); }

std::vector<PluginResourceRecord> PluginScope::Resources() const {
    std::vector<PluginResourceRecord> resources = ledger_->Snapshot(owner_);
    resources.erase(std::remove_if(resources.begin(), resources.end(), [&](const auto& record) {
        return record.generation != generation_;
    }), resources.end());
    return resources;
}

PluginScopeSnapshot PluginScope::Snapshot() const {
    PluginScopeSnapshot result;
    result.owner = owner_;
    result.generation = generation_;
    {
        std::scoped_lock lock(barrier_->mutex);
        result.accepting_callbacks = barrier_->accepting;
        result.lifecycle_allowed = barrier_->lifecycle_allowed;
        result.in_flight_callbacks = barrier_->in_flight;
    }
    result.resources = Resources().size();
    return result;
}

std::size_t PluginScope::InFlightCallbacks() const noexcept {
    std::scoped_lock lock(barrier_->mutex);
    return barrier_->in_flight;
}

bool PluginScope::SourcesFrozen() const noexcept {
    std::scoped_lock lock(barrier_->mutex);
    return !barrier_->accepting;
}

}  // namespace anomaly
