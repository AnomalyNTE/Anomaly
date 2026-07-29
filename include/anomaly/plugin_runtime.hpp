#pragma once

#include "anomaly/plugin_scope.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anomaly {

enum class PluginRuntimeState : std::uint8_t {
    Discovered,
    Validated,
    Shadowed,
    Loaded,
    Starting,
    Running,
    Stopping,
    Unloaded,
    Faulted,
    Quarantined,
};

class PluginModule {
public:
    virtual ~PluginModule() = default;
    [[nodiscard]] virtual bool Prepare() = 0;
    [[nodiscard]] virtual bool Load(const std::shared_ptr<PluginScope>& scope) = 0;
    [[nodiscard]] virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual void Unload() noexcept = 0;
    virtual void Update(double delta_seconds) = 0;
    virtual void Draw() = 0;
};

struct PluginRuntimeSnapshot {
    std::string id;
    PluginRuntimeState state{PluginRuntimeState::Discovered};
    std::uint64_t generation{};
    std::string last_error;
    std::size_t resources{};
    std::size_t quarantined_modules{};
};

class PluginRuntime final {
public:
    using Factory = std::function<std::shared_ptr<PluginModule>()>;

    PluginRuntime(
        std::string id,
        std::shared_ptr<ResourceLedger> ledger = std::make_shared<ResourceLedger>(),
        std::chrono::milliseconds callback_timeout = std::chrono::milliseconds(1000));
    ~PluginRuntime();

    bool Activate(Factory factory);
    bool Reload(Factory factory);
    bool Stop();
    void Update(double delta_seconds) noexcept;
    void Draw() noexcept;

    [[nodiscard]] PluginRuntimeSnapshot Snapshot() const;
    [[nodiscard]] std::shared_ptr<PluginScope> Scope() const;
    [[nodiscard]] std::shared_ptr<ResourceLedger> Ledger() const noexcept { return ledger_; }

private:
    bool StopCurrent();
    void SetFailure(PluginRuntimeState state, std::string message) noexcept;

    std::string id_;
    std::shared_ptr<ResourceLedger> ledger_;
    std::chrono::milliseconds callback_timeout_;
    mutable std::mutex mutex_;
    PluginRuntimeState state_{PluginRuntimeState::Discovered};
    std::uint64_t generation_{};
    std::string last_error_;
    std::shared_ptr<PluginModule> module_;
    std::shared_ptr<PluginScope> scope_;
    std::vector<std::shared_ptr<PluginModule>> quarantined_;
};

}  // namespace anomaly
