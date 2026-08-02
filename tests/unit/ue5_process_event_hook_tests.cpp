#include "anomaly/ue5_process_event_hook.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

namespace {

class FakeBackend final : public anomaly::HookBackend {
public:
    bool Initialize() noexcept override { return true; }
    void Uninitialize() noexcept override {}
    bool Create(void* target, void* detour_value, void** original) noexcept override {
        detour = detour_value;
        *original = target;
        return true;
    }
    bool Enable(void*) noexcept override {
        enabled = true;
        return true;
    }
    bool Disable(void*) noexcept override {
        enabled = false;
        return true;
    }
    bool Remove(void*) noexcept override {
        removed = true;
        return true;
    }

    void* detour{};
    bool enabled{};
    bool removed{};
};

std::atomic_uint32_t g_original_calls{};

void __fastcall ProcessEventTarget(void*, void*, void*) {
    g_original_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fixture = backend.get();
    std::atomic_uint32_t callbacks{};
    std::atomic_bool original_before_callback{};
    std::atomic_bool bypassed_detour{};
    anomaly::Ue5ProcessEventHook hook(
        std::move(backend),
        [&](const std::uintptr_t object,
            const std::uintptr_t function,
            void* const parameters,
            const anomaly::Ue5ProcessEventInvoker& original) {
            original_before_callback.store(
                g_original_calls.load(std::memory_order_relaxed) == 1,
                std::memory_order_release);
            callbacks.fetch_add(1, std::memory_order_relaxed);
            bypassed_detour.store(
                original(object, function, parameters, 0) &&
                    callbacks.load(std::memory_order_relaxed) == 1 &&
                    g_original_calls.load(std::memory_order_relaxed) == 2,
                std::memory_order_release);
        });
    if (!hook.Start(reinterpret_cast<void*>(&ProcessEventTarget)) ||
        !fixture->enabled || fixture->detour == nullptr) {
        std::cerr << "ProcessEvent hook did not start\n";
        return 1;
    }

    using ProcessEvent = void(__fastcall*)(void*, void*, void*);
    int object{};
    int function{};
    int parameters{};
    reinterpret_cast<ProcessEvent>(fixture->detour)(
        &object, &function, &parameters);
    if (g_original_calls.load(std::memory_order_relaxed) != 2 ||
        callbacks.load(std::memory_order_relaxed) != 1 ||
        !original_before_callback.load(std::memory_order_acquire) ||
        !bypassed_detour.load(std::memory_order_acquire)) {
        std::cerr << "ProcessEvent hook order or trampoline forwarding failed\n";
        return 2;
    }
    const auto stopped_detour = reinterpret_cast<ProcessEvent>(fixture->detour);
    if (!hook.Stop() || hook.Started() || fixture->enabled || !fixture->removed) {
        std::cerr << "ProcessEvent hook did not stop cleanly\n";
        return 3;
    }
    stopped_detour(&object, &function, &parameters);
    if (g_original_calls.load(std::memory_order_relaxed) != 3 ||
        callbacks.load(std::memory_order_relaxed) != 1) {
        std::cerr << "stopping ProcessEvent race dropped original forwarding\n";
        return 4;
    }

    auto blocking_backend = std::make_unique<FakeBackend>();
    auto* blocking_fixture = blocking_backend.get();
    std::atomic_bool callback_entered{};
    std::atomic_bool release_callback{};
    anomaly::Ue5ProcessEventHook blocking_hook(
        std::move(blocking_backend),
        [&](std::uintptr_t, std::uintptr_t, void*,
            const anomaly::Ue5ProcessEventInvoker&) {
            callback_entered.store(true, std::memory_order_release);
            while (!release_callback.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    if (!blocking_hook.Start(reinterpret_cast<void*>(&ProcessEventTarget))) {
        std::cerr << "blocking ProcessEvent hook did not start\n";
        return 5;
    }
    std::thread blocked_event([detour = blocking_fixture->detour, &object, &function] {
        reinterpret_cast<ProcessEvent>(detour)(&object, &function, nullptr);
    });
    const auto entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!callback_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entry_deadline) {
        std::this_thread::yield();
    }
    if (!callback_entered.load(std::memory_order_acquire)) {
        release_callback.store(true, std::memory_order_release);
        blocked_event.join();
        std::cerr << "blocking ProcessEvent callback did not enter\n";
        return 6;
    }
    if (blocking_hook.Stop(std::chrono::milliseconds(5)) ||
        blocking_fixture->enabled || blocking_fixture->removed) {
        release_callback.store(true, std::memory_order_release);
        blocked_event.join();
        std::cerr << "in-flight ProcessEvent generation was not retained\n";
        return 7;
    }
    release_callback.store(true, std::memory_order_release);
    blocked_event.join();
    if (!blocking_hook.Stop(std::chrono::milliseconds(100)) ||
        !blocking_fixture->removed) {
        std::cerr << "drained ProcessEvent generation was not removed\n";
        return 8;
    }
    return 0;
}
