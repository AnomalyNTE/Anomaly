#pragma once

#include "anomaly/hook_manager.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace anomaly {

class GameTickHook final {
public:
    using Callback = std::function<void(double)>;

    explicit GameTickHook(Callback callback);
    GameTickHook(std::unique_ptr<HookBackend> backend, Callback callback);
    ~GameTickHook();

    GameTickHook(const GameTickHook&) = delete;
    GameTickHook& operator=(const GameTickHook&) = delete;

    [[nodiscard]] bool Start(void* target);
    // Disables new detour entries, then drains callbacks within timeout. A
    // false result leaves the disabled hook generation intact for quarantine
    // and may be retried after the in-flight callback exits.
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] std::vector<HookRecordView> Snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
