#pragma once

#include "anomaly/hook_manager.hpp"
#include "anomaly/ue5_process_event.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace anomaly {

class Ue5ProcessEventHook final {
public:
    // The original invoker is valid only for the synchronous callback. It
    // bypasses this detour so callback-owned UE calls cannot recursively enter
    // the AHUD event ingress.
    using Callback = std::function<void(
        std::uintptr_t object,
        std::uintptr_t function,
        void* parameters,
        const Ue5ProcessEventInvoker& original)>;

    explicit Ue5ProcessEventHook(Callback callback);
    Ue5ProcessEventHook(std::unique_ptr<HookBackend> backend, Callback callback);
    ~Ue5ProcessEventHook();

    Ue5ProcessEventHook(const Ue5ProcessEventHook&) = delete;
    Ue5ProcessEventHook& operator=(const Ue5ProcessEventHook&) = delete;

    [[nodiscard]] bool Start(void* target);
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] std::vector<HookRecordView> Snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
