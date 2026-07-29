#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

namespace test {
class AdapterServiceRegistryTestAccess;
}

struct AdapterServiceView {
    std::string id;
    std::uint32_t version{};
    const void* table{};
};

class AdapterServiceRegistry final {
public:
    using QueryObserver = std::function<void()>;
    enum class RevokeResult {
        Revoked,
        Missing,
        TimedOut,
    };

    [[nodiscard]] bool Publish(
        std::string id,
        std::uint32_t version,
        const void* table,
        QueryObserver query_observer = {},
        std::shared_ptr<const void> lifetime = {});
    [[nodiscard]] bool Revoke(std::string_view id, const void* table) noexcept;
    [[nodiscard]] RevokeResult RevokeUntil(
        std::string_view id,
        const void* table,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] const void* Query(
        std::string_view id,
        std::uint32_t minimum_version,
        bool observe = true) const noexcept;
    [[nodiscard]] std::vector<AdapterServiceView> Snapshot() const;
    void Clear() noexcept;

private:
    friend class test::AdapterServiceRegistryTestAccess;

    struct Entry {
        AdapterServiceView view;
        std::shared_ptr<const QueryObserver> query_observer;
        std::shared_ptr<const void> lifetime;
    };

    mutable std::timed_mutex mutex_;
    std::map<std::string, Entry, std::less<>> services_;
    // Providers that supply a lifetime can keep a queried C ABI table valid
    // after revocation so cached callbacks can fail cleanly.
    std::vector<std::shared_ptr<const void>> retired_lifetimes_;
};

[[nodiscard]] AdapterServiceRegistry& ProcessAdapterServices() noexcept;

}  // namespace anomaly
