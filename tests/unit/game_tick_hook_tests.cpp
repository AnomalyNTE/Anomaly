#include "anomaly/game_tick_hook.hpp"

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
    bool Enable(void*) noexcept override { enabled = true; return true; }
    bool Disable(void*) noexcept override { enabled = false; return true; }
    bool Remove(void*) noexcept override { removed = true; return true; }

    void* detour{};
    bool enabled{};
    bool removed{};
};

std::atomic_uint32_t original_calls{};
std::atomic_bool original_before_callback{};

void __fastcall TickTarget(void*, float, bool) {
    original_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fixture = backend.get();
    std::atomic_uint32_t callbacks{};
    anomaly::GameTickHook hook(std::move(backend), [&](double delta) {
        original_before_callback = original_calls.load(std::memory_order_relaxed) == 1;
        if (delta > 0.49 && delta < 0.51) callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    if (!hook.Start(reinterpret_cast<void*>(&TickTarget)) || !fixture->enabled ||
        fixture->detour == nullptr) {
        std::cerr << "game tick hook did not start\n";
        return 1;
    }
    using Tick = void(__fastcall*)(void*, float, bool);
    reinterpret_cast<Tick>(fixture->detour)(nullptr, 0.5F, false);
    if (original_calls != 1 || callbacks != 1 || !original_before_callback) {
        std::cerr << "game tick hook order or delta forwarding failed\n";
        return 2;
    }
    if (!hook.Stop()) {
        std::cerr << "game tick hook clean stop timed out\n";
        return 3;
    }
    if (hook.Started() || fixture->enabled || !fixture->removed) {
        std::cerr << "game tick hook did not stop cleanly\n";
        return 4;
    }

    auto blocking_backend = std::make_unique<FakeBackend>();
    auto* blocking_fixture = blocking_backend.get();
    std::atomic_bool callback_entered{};
    std::atomic_bool release_callback{};
    anomaly::GameTickHook blocking_hook(
        std::move(blocking_backend), [&](double) {
            callback_entered.store(true, std::memory_order_release);
            while (!release_callback.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    if (!blocking_hook.Start(reinterpret_cast<void*>(&TickTarget))) {
        std::cerr << "blocking game tick hook did not start\n";
        return 5;
    }
    std::thread blocked_tick([detour = blocking_fixture->detour] {
        using Tick = void(__fastcall*)(void*, float, bool);
        reinterpret_cast<Tick>(detour)(nullptr, 0.25F, false);
    });
    const auto entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!callback_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entry_deadline) {
        std::this_thread::yield();
    }
    if (!callback_entered.load(std::memory_order_acquire)) {
        release_callback.store(true, std::memory_order_release);
        blocked_tick.join();
        std::cerr << "blocking game tick callback did not enter\n";
        return 6;
    }
    const auto stop_started = std::chrono::steady_clock::now();
    if (blocking_hook.Stop(std::chrono::milliseconds(5)) ||
        std::chrono::steady_clock::now() - stop_started > std::chrono::seconds(1) ||
        blocking_fixture->enabled || blocking_fixture->removed) {
        release_callback.store(true, std::memory_order_release);
        blocked_tick.join();
        std::cerr << "in-flight game tick generation did not remain quarantinable\n";
        return 7;
    }
    release_callback.store(true, std::memory_order_release);
    blocked_tick.join();
    if (!blocking_hook.Stop(std::chrono::milliseconds(100)) ||
        !blocking_fixture->removed) {
        std::cerr << "drained game tick generation was not removed on retry\n";
        return 8;
    }
    return 0;
}
