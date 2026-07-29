#pragma once

#include "anomaly/plugin_scope.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anomaly {

class HookBackend {
public:
    virtual ~HookBackend() = default;
    virtual bool Initialize() noexcept = 0;
    virtual void Uninitialize() noexcept = 0;
    virtual bool Create(void* target, void* detour, void** original) noexcept = 0;
    virtual bool Enable(void* target) noexcept = 0;
    virtual bool Disable(void* target) noexcept = 0;
    virtual bool Remove(void* target) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<HookBackend> CreateMinHookBackend();

struct HookRecordView {
    std::string owner;
    std::uint64_t generation{};
    std::string label;
    void* target{};
    bool enabled{};
};

class HookManager final {
public:
    HookManager(
        std::unique_ptr<HookBackend> backend,
        std::shared_ptr<ResourceLedger> ledger = std::make_shared<ResourceLedger>());
    ~HookManager();

    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    bool Create(
        std::string owner, std::uint64_t generation, std::string label,
        void* target, void* detour, void** original);
    bool Enable(std::string_view owner, std::uint64_t generation, void* target) noexcept;
    bool EnableOwner(std::string_view owner, std::uint64_t generation) noexcept;
    bool DisableOwner(std::string_view owner, std::uint64_t generation) noexcept;
    bool Remove(
        std::string_view owner, std::uint64_t generation, void* target,
        std::chrono::milliseconds callback_timeout = std::chrono::milliseconds(5000)) noexcept;
    bool RemoveOwner(
        std::string_view owner, std::uint64_t generation,
        std::chrono::milliseconds callback_timeout = std::chrono::milliseconds(5000)) noexcept;
    [[nodiscard]] PluginScope::CallbackLease AcquireCallback(
        std::string_view owner, std::uint64_t generation) noexcept;
    [[nodiscard]] PluginScope::CallbackLease AcquireCallback(
        std::string_view owner, std::uint64_t generation, void* target) noexcept;
    [[nodiscard]] std::vector<HookRecordView> Snapshot() const;

private:
    struct HookRecord;
    struct OwnerGroup;
    [[nodiscard]] static std::string GroupKey(std::string_view owner, std::uint64_t generation);

    std::unique_ptr<HookBackend> backend_;
    std::shared_ptr<ResourceLedger> ledger_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, OwnerGroup> groups_;
    std::unordered_map<void*, std::string> targets_;
    bool initialized_{};
};

}  // namespace anomaly
