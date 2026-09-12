#pragma once

#include "anomaly/adapter_service_registry.hpp"
#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/symbol_resolver.hpp"
#include "anomaly/ue5_object_lookup.hpp"
#include "anomaly/ue5_process_event.hpp"
#include "anomaly/nte_navigation_input_policy.hpp"

#include <Windows.h>

#include <array>
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
    std::uint32_t combat_tick_interval{6};
    std::uint32_t skill_tick_interval{6};
    std::uint32_t actor_tick_interval{60};
};

struct NteCombatDiagnosticsSnapshot {
    bool damage_event_layout_ready{};
    bool combat_available{};
    bool combat_partial{};
    std::uint32_t process_event_binding_mask{};
    std::uint64_t damage_floaties_call_count{};
    std::uint64_t monster_damage_call_count{};
    std::uint64_t player_damage_queue_call_count{};
    std::uint64_t damage_widget_call_count{};
    std::uint64_t buff_call_count{};
    std::uint64_t crit_query_call_count{};
    std::uint64_t crit_query_success_count{};
    std::uint64_t crit_true_count{};
    std::uint64_t native_call_count{};
    std::uint64_t captured_event_count{};
    std::uint64_t dropped_count{};
    std::uint64_t attacker_resolution_failure_count{};
    std::uint64_t victim_resolution_failure_count{};
    std::uint64_t source_resolution_failure_count{};
    std::uint64_t saved_trigger_skill_mapping_count{};
    std::uint64_t trigger_ability_handle_mapping_count{};
    std::uint64_t damage_source_mapping_failure_count{};
    std::uint64_t delayed_damage_name_completion_count{};
    std::uint64_t combat_sample_sequence{};
    std::uint64_t world_pointer{};
    std::uint64_t player_pawn{};
    std::uint64_t combat_character_id{};
    std::uint64_t combat_character_generation{};
    std::uint32_t combat_refresh_failure{};
    std::uint64_t reflection_fault_count{};
    std::uintptr_t last_reflection_fault_function{};
    std::uint32_t last_reflection_fault_code{};
};

inline constexpr std::size_t kNteCombatExecFunctionCapacity = 14;

struct NteCombatExecFunctionTarget {
    std::uintptr_t function{};
    std::uintptr_t target{};
};

struct NteCombatExecFunctionTargetsSnapshot {
    std::array<NteCombatExecFunctionTarget, kNteCombatExecFunctionCapacity> entries{};
    std::size_t count{};
    bool discovery_complete{};
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

    [[nodiscard]] bool Start(
        bool framework_hook_ready,
        bool ahud_hook_ready = false,
        bool process_event_hook_ready = false);
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
    void OnDamageEvent(
        std::uintptr_t damage_event,
        std::uintptr_t victim,
        std::uintptr_t attacker,
        std::uintptr_t damage_causer) noexcept;
    void OnCombatExecFunction(
        std::uintptr_t receiver,
        std::uintptr_t function,
        std::uintptr_t stack) noexcept;
    // Called by the shared UObject ProcessEvent detour after the original
    // function completes. Damage capture uses the exact native
    // CharacterOnDamaged broadcast and never enters through this broad hook.
    void OnProcessEvent(
        std::uintptr_t object,
        std::uintptr_t function,
        void* parameters,
        const ProcessEventInvoker& process_event) noexcept;
    void OnProcessEventPre(
        std::uintptr_t object, std::uintptr_t function,
        void* parameters) noexcept;

    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] DWORD GameThreadId() const noexcept;
    [[nodiscard]] std::uint64_t TickSequence() const noexcept;
    [[nodiscard]] std::uint64_t RejectedThreadTicks() const noexcept;
    [[nodiscard]] bool AhudBindingReady() const noexcept;
    [[nodiscard]] std::uint64_t AhudFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t AhudProcessEventCallCount() const noexcept;
    // Native combat capture stays dormant until the combat reflection gate
    // has completed on the game thread.
    [[nodiscard]] bool CombatFeatureAvailable() const noexcept;
    [[nodiscard]] NteCombatExecFunctionTargetsSnapshot
        CombatExecFunctionTargets() const noexcept;
    [[nodiscard]] NteCombatDiagnosticsSnapshot CombatDiagnostics() const noexcept;
    // Read-only diagnostic view used by anomaly-cli's ue combat/buffs commands.
    [[nodiscard]] std::string CombatEventsJson(bool buffs_only = false) const;
    [[nodiscard]] ProfileResolutionSnapshot Resolution() const;

private:
    struct State;
    // A game tick may still be unwinding after the public adapter owner is
    // released. Each entry point takes a local shared owner before touching
    // State so teardown cannot invalidate the callback's bookkeeping.
    std::shared_ptr<State> state_;
};

}  // namespace anomaly
