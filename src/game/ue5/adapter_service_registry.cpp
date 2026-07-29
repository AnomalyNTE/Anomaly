#include "anomaly/adapter_service_registry.hpp"

#include <cstddef>
#include <cstring>
#include <utility>

namespace anomaly {
namespace {

struct ServiceTableHeader {
    std::uint32_t struct_size;
    std::uint32_t service_version;
};

struct ServiceTableCallablePrefix {
    std::uint32_t struct_size;
    std::uint32_t service_version;
    void* user;
};

constexpr std::size_t kServiceTableCallablePrefixSize =
    offsetof(ServiceTableCallablePrefix, user) + sizeof(void*);

bool HasRetainedLifetime(
    const std::vector<std::shared_ptr<const void>>& retired,
    const std::shared_ptr<const void>& lifetime) noexcept {
    for (const auto& retained : retired) {
        if (retained.get() == lifetime.get()) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool AdapterServiceRegistry::Publish(
    std::string id,
    std::uint32_t version,
    const void* table,
    QueryObserver query_observer,
    std::shared_ptr<const void> lifetime) {
    if (id.empty() || version == 0 || table == nullptr) return false;
    ServiceTableHeader header{};
    std::memcpy(&header, table, sizeof(header));
    if (header.struct_size < kServiceTableCallablePrefixSize ||
        header.service_version != version) {
        return false;
    }
    std::shared_ptr<const QueryObserver> observer;
    if (query_observer) {
        observer = std::make_shared<const QueryObserver>(std::move(query_observer));
    }
    std::scoped_lock lock(mutex_);
    if (services_.contains(id)) return false;
    if (lifetime) {
        retired_lifetimes_.reserve(retired_lifetimes_.size() + services_.size() + 1U);
    }
    AdapterServiceView view{id, version, table};
    services_.emplace(
        std::move(id), Entry{std::move(view), std::move(observer), std::move(lifetime)});
    return true;
}

bool AdapterServiceRegistry::Revoke(std::string_view id, const void* table) noexcept {
    return RevokeUntil(id, table, std::chrono::steady_clock::time_point::max()) ==
        RevokeResult::Revoked;
}

AdapterServiceRegistry::RevokeResult AdapterServiceRegistry::RevokeUntil(
    std::string_view id,
    const void* table,
    std::chrono::steady_clock::time_point deadline) noexcept {
    std::map<std::string, Entry, std::less<>>::node_type removed;
    {
        std::unique_lock lock(mutex_, std::defer_lock);
        const bool locked = deadline == std::chrono::steady_clock::time_point::max()
            ? (lock.lock(), true)
            : lock.try_lock_until(deadline);
        if (!locked) return RevokeResult::TimedOut;

        const auto found = services_.find(id);
        if (found == services_.end() || found->second.view.table != table) {
            return RevokeResult::Missing;
        }
        removed = services_.extract(found);
        auto& lifetime = removed.mapped().lifetime;
        if (lifetime && !HasRetainedLifetime(retired_lifetimes_, lifetime)) {
            // Publish reserves for every live entry before it becomes revocable.
            retired_lifetimes_.push_back(std::move(lifetime));
        }
    }
    // The extracted observer and any duplicate lifetime are released after
    // dropping the registry lock; both can own caller-provided work.
    return RevokeResult::Revoked;
}

const void* AdapterServiceRegistry::Query(
    std::string_view id,
    std::uint32_t minimum_version,
    bool observe) const noexcept {
    const void* table{};
    std::shared_ptr<const QueryObserver> observer;
    {
        std::scoped_lock lock(mutex_);
        const auto found = services_.find(id);
        if (found == services_.end() || found->second.view.version < minimum_version) {
            return nullptr;
        }
        table = found->second.view.table;
        if (observe) observer = found->second.query_observer;
    }
    if (observer) {
        try {
            (*observer)();
        } catch (...) {
        }
    }
    return table;
}

std::vector<AdapterServiceView> AdapterServiceRegistry::Snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<AdapterServiceView> result;
    result.reserve(services_.size());
    for (const auto& [id, service] : services_) {
        static_cast<void>(id);
        result.push_back(service.view);
    }
    return result;
}

void AdapterServiceRegistry::Clear() noexcept {
    std::map<std::string, Entry, std::less<>> removed;
    {
        std::scoped_lock lock(mutex_);
        removed.swap(services_);
        for (auto& [id, service] : removed) {
            static_cast<void>(id);
            if (service.lifetime &&
                !HasRetainedLifetime(retired_lifetimes_, service.lifetime)) {
                retired_lifetimes_.push_back(std::move(service.lifetime));
            }
        }
    }
    // Keep removed entries alive until after the lock is released.
}

AdapterServiceRegistry& ProcessAdapterServices() noexcept {
    static AdapterServiceRegistry registry;
    return registry;
}

}  // namespace anomaly
