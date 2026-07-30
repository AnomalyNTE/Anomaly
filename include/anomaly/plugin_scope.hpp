#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anomaly {

enum class PluginResourceKind : std::uint8_t {
    Config,
    Subscription,
    Task,
    Ui,
    Hook,
    Patch,
    Texture,
    Ipc,
    Command,
    Window,
    Font,
    Input,
    Notification,
    Diagnostics,
    NteEscMenuButton,
};

struct PluginResourceRecord {
    std::uint64_t token{};
    std::string owner;
    std::uint64_t generation{};
    PluginResourceKind kind{PluginResourceKind::Task};
    std::string label;
};

struct PluginScopeSnapshot {
    std::string owner;
    std::uint64_t generation{};
    bool accepting_callbacks{};
    bool lifecycle_allowed{};
    std::size_t in_flight_callbacks{};
    std::size_t resources{};
};

class ResourceLedger final {
public:
    using Revoker = std::function<void()>;

    [[nodiscard]] std::uint64_t Register(
        std::string owner, std::uint64_t generation,
        PluginResourceKind kind, std::string label, Revoker revoker = {});
    bool Release(std::uint64_t token) noexcept;
    std::size_t Revoke(std::string_view owner, std::uint64_t generation) noexcept;
    std::size_t RevokeExcept(
        std::string_view owner, std::uint64_t generation,
        PluginResourceKind preserved_kind) noexcept;
    [[nodiscard]] std::vector<PluginResourceRecord> Snapshot(
        std::optional<std::string_view> owner = std::nullopt) const;

private:
    struct Entry {
        PluginResourceRecord record;
        Revoker revoker;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t next_token_{1};
};

class PluginScope final : public std::enable_shared_from_this<PluginScope> {
private:
    struct BarrierState;

public:
    class CallbackLease final {
    public:
        CallbackLease() = default;
        ~CallbackLease();
        CallbackLease(CallbackLease&& other) noexcept;
        CallbackLease& operator=(CallbackLease&& other) noexcept;
        CallbackLease(const CallbackLease&) = delete;
        CallbackLease& operator=(const CallbackLease&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        friend class PluginScope;
        explicit CallbackLease(std::shared_ptr<BarrierState> state) : state_(std::move(state)) {}
        void Reset() noexcept;
        std::shared_ptr<BarrierState> state_;
    };

    PluginScope(
        std::shared_ptr<ResourceLedger> ledger,
        std::string owner,
        std::uint64_t generation);

    [[nodiscard]] const std::string& Owner() const noexcept { return owner_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t Register(
        PluginResourceKind kind, std::string label, ResourceLedger::Revoker revoker = {});
    bool Release(std::uint64_t token) noexcept;
    [[nodiscard]] CallbackLease AcquireCallback(std::uint64_t generation) noexcept;
    // Freezes ordinary callback sources without waiting for callbacks already in flight.
    // A lifecycle lease can be acquired after BeginStop has drained those callbacks.
    bool FreezeCallbackSources() noexcept;
    [[nodiscard]] CallbackLease AcquireLifecycleLease(std::uint64_t generation) noexcept;
    [[nodiscard]] bool BeginStop(std::chrono::milliseconds timeout) noexcept;
    // Host staged shutdown can preserve one resource category through on_stop.
    // The caller must finish with RevokeAll before on_unload or DLL release.
    std::size_t RevokeAllExcept(PluginResourceKind preserved_kind) noexcept;
    std::size_t RevokeAll() noexcept;
    [[nodiscard]] std::vector<PluginResourceRecord> Resources() const;
    [[nodiscard]] PluginScopeSnapshot Snapshot() const;
    [[nodiscard]] std::size_t InFlightCallbacks() const noexcept;
    [[nodiscard]] bool SourcesFrozen() const noexcept;

private:
    std::shared_ptr<ResourceLedger> ledger_;
    std::string owner_;
    std::uint64_t generation_{};
    std::shared_ptr<BarrierState> barrier_;
};

}  // namespace anomaly
