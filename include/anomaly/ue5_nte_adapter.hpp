#pragma once

#include "anomaly/adapter_service_registry.hpp"
#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/symbol_resolver.hpp"
#include "anomaly/ue5_object_lookup.hpp"
#include "anomaly/ue5_process_event.hpp"
#include "anomaly/nte_navigation_input_policy.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace anomaly {

struct NteSnapshotSamplingOptions {
    std::uint32_t player_tick_interval{1};
    std::uint32_t entity_tick_interval{1};
};

class Ue5NteAdapter final {
public:
    using TickCallback = std::function<void(double)>;
    // Invocation boundary for the separately verified generic UE5 ProcessEvent capability.
    // Tests inject this seam; production derives it from the active Profile and ABI validator.
    // It must never fall back to a Pawn vtable slot.
    using ProcessEventInvoker = Ue5ProcessEventInvoker;
    using ObjectLookup = Ue5ObjectLookup;

    Ue5NteAdapter(
        BuildFingerprint fingerprint,
        BuildProfile profile,
        ProfileResolutionSnapshot resolution,
        std::shared_ptr<const SymbolMemory> memory,
        AdapterServiceRegistry& services = ProcessAdapterServices(),
        NteSnapshotSamplingOptions sampling = {},
        FeatureLayoutValidatorRegistry feature_layout_validators = {},
        // Mutation services remain default-deny until the current module's ABI
        // and reflection validators supply an invocation bridge.
        ProcessEventInvoker process_event_invoker = {},
        ObjectLookup object_lookup = {},
        std::shared_ptr<NteNavigationInputPolicy> navigation_input_policy = {});
    ~Ue5NteAdapter();

    Ue5NteAdapter(const Ue5NteAdapter&) = delete;
    Ue5NteAdapter& operator=(const Ue5NteAdapter&) = delete;

    [[nodiscard]] bool Start(bool framework_hook_ready, bool ahud_hook_ready = false);
    // Closes cached service tables, detaches callbacks, and revokes registry
    // entries before draining state/callback work. Callback target destruction
    // is deferred off the lifecycle caller. A false result keeps the generation
    // in stopping state until a later successful Stop call.
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    void SetTickCallback(TickCallback callback);
    // Removes the callback immediately, then waits for already-entered game
    // ticks. A finite timeout returns false while the callback is still in
    // flight; the callback has nevertheless been detached and will not be
    // entered by a later tick. Target destruction is deferred off the caller.
    bool ClearTickCallback(
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    void OnGameTick(double delta_seconds) noexcept;
    // Called only by the separately owned Actor ProcessEvent wrapper detour
    // after the wrapper has completed. AHUD calls use that wrapper's original
    // trampoline so native HUD dispatch matches the object's virtual path.
    void OnProcessEvent(
        std::uintptr_t object,
        std::uintptr_t function,
        void* parameters,
        const ProcessEventInvoker& actor_process_event) noexcept;

    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] DWORD GameThreadId() const noexcept;
    [[nodiscard]] std::uint64_t TickSequence() const noexcept;
    [[nodiscard]] std::uint64_t RejectedThreadTicks() const noexcept;
    [[nodiscard]] bool AhudBindingReady() const noexcept;
    [[nodiscard]] std::uint64_t AhudFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t AhudProcessEventCallCount() const noexcept;
    [[nodiscard]] ProfileResolutionSnapshot Resolution() const;

private:
    struct State;
    // A game tick may still be unwinding after the public adapter owner is
    // released. Each entry point takes a local shared owner before touching
    // State so teardown cannot invalidate the callback's bookkeeping.
    std::shared_ptr<State> state_;
};

}  // namespace anomaly
