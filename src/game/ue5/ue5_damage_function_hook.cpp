#include "anomaly/ue5_damage_function_hook.hpp"
#include "anomaly/thread_local_value.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <intrin.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace anomaly {
namespace {

inline constexpr std::string_view kOwner = "anomaly.nte.damage-native";
inline constexpr std::uint64_t kGeneration = 1;

}  // namespace

class Ue5DamageFunctionHook::Impl final {
public:
    using NativeFunction = void(__fastcall*)(
        void*, const void*, void*, void*, void*);
    using ExecFunction = void(__fastcall*)(void*, void*, void*);
    using ApplyFunction = std::uintptr_t(__fastcall*)(std::uintptr_t, std::uintptr_t,
        std::uintptr_t, float, std::uint8_t, std::uintptr_t, std::uintptr_t,
        std::uint8_t, std::uintptr_t);
    using FightResultFunction = std::uintptr_t(__fastcall*)(
        std::uintptr_t, std::uintptr_t, std::uintptr_t);
    using PendingFightHitFunction = std::uintptr_t(__fastcall*)(std::uintptr_t, std::uintptr_t);
    using SetHealthFunction = std::uintptr_t(__fastcall*)(std::uintptr_t, float);

    Impl(
        std::unique_ptr<HookBackend> backend,
        Callback callback,
        ExecCallback exec_callback, ReadHealth read_health, AppliedDamage applied_damage,
        ReplicatedDamage replicated_damage)
        : hooks_(std::move(backend)), callback_(std::move(callback)),
          exec_callback_(std::move(exec_callback)), read_health_(std::move(read_health)),
          applied_damage_(std::move(applied_damage)),
          replicated_damage_(std::move(replicated_damage)) {
        if (!callback_) {
            throw std::invalid_argument("Ue5DamageFunctionHook requires a callback");
        }
    }

    bool Start(const Ue5DamageFunctionTargets targets) {
        std::scoped_lock stop_lock(stop_mutex_);
        if (attempted_.exchange(true, std::memory_order_acq_rel) ||
            targets.character_on_damaged == nullptr ||
            targets.combat_exec_function_count > kUe5CombatExecFunctionCapacity ||
            started_.load(std::memory_order_acquire) || owner_registered_) {
            return false;
        }
        std::scoped_lock process_lock(process_mutex_);
        if (active_.load(std::memory_order_acquire) != nullptr) return false;

        target_ = targets.character_on_damaged;
        original_ = nullptr;
        if (!hooks_.Create(
                std::string(kOwner), kGeneration, "character-on-damaged",
                target_, reinterpret_cast<void*>(&Thunk),
                reinterpret_cast<void**>(&original_))) {
            target_ = nullptr;
            return false;
        }
        owner_registered_ = true;
        exec_count_ = targets.combat_exec_function_count;
        for (std::size_t index{}; index < exec_count_; ++index) {
            const auto& entry = targets.combat_exec_functions[index];
            if (entry.function == nullptr || entry.target == nullptr ||
                !hooks_.Create(
                    std::string(kOwner), kGeneration,
                    "combat-exec-" + std::to_string(index),
                    entry.target,
                    reinterpret_cast<void*>(ThunkForSlot(index)),
                    reinterpret_cast<void**>(&exec_originals_[index]))) {
                static_cast<void>(hooks_.RemoveOwner(kOwner, kGeneration));
                owner_registered_ = false;
                ClearTarget();
                return false;
            }
            exec_functions_[index] = entry.function;
            exec_targets_[index] = entry.target;
        }
        passthrough_.store(
            reinterpret_cast<NativeFunction>(target_), std::memory_order_release);
        for (std::size_t index{}; index < exec_count_; ++index) {
            exec_passthroughs_[index].store(
                reinterpret_cast<ExecFunction>(exec_targets_[index]),
                std::memory_order_release);
        }
        active_.store(this, std::memory_order_release);
        if (!hooks_.EnableOwner(kOwner, kGeneration)) {
            if (hooks_.RemoveOwner(kOwner, kGeneration)) {
                owner_registered_ = false;
                Impl* expected = this;
                static_cast<void>(active_.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel));
                ClearTarget();
            }
            return false;
        }
        if (targets.apply_damage != nullptr && read_health_ && applied_damage_ &&
            hooks_.Create(std::string(kOwner), kGeneration, "apply-health-damage",
                targets.apply_damage, reinterpret_cast<void*>(&ApplyThunk),
                reinterpret_cast<void**>(&apply_original_))) {
            apply_passthrough_.store(reinterpret_cast<ApplyFunction>(targets.apply_damage),
                std::memory_order_release);
            if (hooks_.Enable(kOwner, kGeneration, targets.apply_damage)) {
                apply_target_ = targets.apply_damage;
            } else {
                static_cast<void>(hooks_.Remove(kOwner, kGeneration, targets.apply_damage));
            }
        }
        const bool replicated_started = StartReplicated(targets);
        started_.store(true, std::memory_order_release);
        applied_started_.store(apply_target_ != nullptr || replicated_started, std::memory_order_release);
        return true;
    }

    bool Stop(const std::chrono::milliseconds timeout) noexcept {
        std::scoped_lock stop_lock(stop_mutex_);
        static_cast<void>(started_.exchange(false, std::memory_order_acq_rel));
        applied_started_.store(false, std::memory_order_release);
        if (!owner_registered_) return true;
        const auto bounded_timeout =
            (std::max)(timeout, std::chrono::milliseconds::zero());
        const bool detours_disabled = hooks_.DisableOwner(kOwner, kGeneration);
        if (!detours_disabled &&
            !hooks_.RemoveOwner(kOwner, kGeneration, bounded_timeout)) {
            return false;
        }
        {
            std::scoped_lock process_lock(process_mutex_);
            Impl* expected = this;
            static_cast<void>(active_.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel));
        }
        if (detours_disabled &&
            !hooks_.RemoveOwner(kOwner, kGeneration, bounded_timeout)) {
            return false;
        }
        owner_registered_ = false;
        ClearTarget();
        return true;
    }

    [[nodiscard]] bool Started() const noexcept {
        return started_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Attempted() const noexcept {
        return attempted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool AppliedDamageStarted() const noexcept {
        return applied_started_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<HookRecordView> Snapshot() const {
        return hooks_.Snapshot();
    }

private:
    bool StartReplicated(const Ue5DamageFunctionTargets& targets) {
        if (targets.replicated_fight_result == nullptr || targets.set_current_health == nullptr ||
            targets.take_pending_fight_hit == nullptr || targets.pending_hit_return == 0 ||
            targets.set_health_return == 0 ||
            !read_health_ || !replicated_damage_) {
            return false;
        }
        fight_result_target_ = targets.replicated_fight_result;
        pending_hit_target_ = targets.take_pending_fight_hit;
        set_health_target_ = targets.set_current_health;
        pending_hit_return_ = targets.pending_hit_return;
        set_health_return_ = targets.set_health_return;
        struct HookEntry {
            const char* label;
            void* target;
            void* detour;
            void** original;
        };
        const std::array entries{
            HookEntry{"take-pending-fight-hit", pending_hit_target_,
                reinterpret_cast<void*>(&PendingFightHitThunk),
                reinterpret_cast<void**>(&pending_hit_original_)},
            HookEntry{"set-current-health", set_health_target_,
                reinterpret_cast<void*>(&SetHealthThunk),
                reinterpret_cast<void**>(&set_health_original_)},
            HookEntry{"replicated-fight-result", fight_result_target_,
                reinterpret_cast<void*>(&FightResultThunk),
                reinterpret_cast<void**>(&fight_result_original_)}};
        std::size_t created{};
        while (created < entries.size()) {
            const auto& entry = entries[created];
            if (!hooks_.Create(std::string(kOwner), kGeneration, entry.label,
                    entry.target, entry.detour, entry.original)) break;
            ++created;
        }
        fight_result_passthrough_.store(reinterpret_cast<FightResultFunction>(fight_result_target_),
            std::memory_order_release);
        pending_hit_passthrough_.store(reinterpret_cast<PendingFightHitFunction>(pending_hit_target_),
            std::memory_order_release);
        set_health_passthrough_.store(reinterpret_cast<SetHealthFunction>(set_health_target_),
            std::memory_order_release);
        if (created == entries.size() && std::ranges::all_of(entries, [this](const HookEntry& entry) {
                return hooks_.Enable(kOwner, kGeneration, entry.target);
            })) {
            return true;
        }
        while (created != 0) {
            static_cast<void>(hooks_.Remove(kOwner, kGeneration, entries[--created].target));
        }
        return false;
    }

    void ClearTarget() noexcept {
        original_ = nullptr;
        target_ = nullptr;
        passthrough_.store(nullptr, std::memory_order_release);
        apply_original_ = nullptr;
        apply_target_ = nullptr;
        apply_passthrough_.store(nullptr, std::memory_order_release);
        fight_result_original_ = nullptr;
        fight_result_target_ = nullptr;
        fight_result_passthrough_.store(nullptr, std::memory_order_release);
        pending_hit_original_ = nullptr;
        pending_hit_target_ = nullptr;
        pending_hit_return_ = 0;
        pending_hit_passthrough_.store(nullptr, std::memory_order_release);
        set_health_original_ = nullptr;
        set_health_target_ = nullptr;
        set_health_return_ = 0;
        set_health_passthrough_.store(nullptr, std::memory_order_release);
        for (std::size_t index{}; index < kUe5CombatExecFunctionCapacity; ++index) {
            exec_originals_[index] = nullptr;
            exec_functions_[index] = nullptr;
            exec_targets_[index] = nullptr;
            exec_passthroughs_[index].store(nullptr, std::memory_order_release);
        }
        exec_count_ = 0;
    }

    static void Dispatch(
        void* const delegate,
        const void* const damage_event,
        void* const victim,
        void* const attacker,
        void* const damage_causer) noexcept {
        Impl* self{};
        NativeFunction original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self != nullptr) {
                original = self->original_;
                if (original != nullptr) {
                    lease = self->hooks_.AcquireCallback(
                        kOwner, kGeneration, self->target_);
                }
                if (!lease) self = nullptr;
            }
            if (self == nullptr) {
                original = passthrough_.load(std::memory_order_acquire);
            }
        }
        if (original == nullptr) return;
        original(delegate, damage_event, victim, attacker, damage_causer);
        if (self != nullptr) {
            try {
                self->callback_(
                    reinterpret_cast<std::uintptr_t>(damage_event),
                    reinterpret_cast<std::uintptr_t>(victim),
                    reinterpret_cast<std::uintptr_t>(attacker),
                    reinterpret_cast<std::uintptr_t>(damage_causer));
            } catch (...) {
            }
        }
    }

    static void __fastcall Thunk(
        void* delegate,
        const void* damage_event,
        void* victim,
        void* attacker,
        void* damage_causer) {
        Dispatch(delegate, damage_event, victim, attacker, damage_causer);
    }

    struct HealthSample {
        HealthSample* parent{};
        std::uintptr_t victim{};
        double nested_delta{};
        bool valid{true};
    };

    struct FightResultSample {
        std::uintptr_t context{};
        std::uintptr_t pending_hit{};
        std::uintptr_t victim{};
    };

    static std::uintptr_t __fastcall FightResultThunk(const std::uintptr_t controller,
        const std::uintptr_t context, const std::uintptr_t result_record) noexcept {
        Impl* self{};
        FightResultFunction original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self != nullptr) {
                lease = self->hooks_.AcquireCallback(kOwner, kGeneration, self->fight_result_target_);
            }
            original = lease ? self->fight_result_original_
                : fight_result_passthrough_.load(std::memory_order_acquire);
        }
        if (original == nullptr) return 0;
        if (!lease) return original(controller, context, result_record);

        FightResultSample sample{context};
        auto* const parent = self->fight_result_sample_.Get();
        self->fight_result_sample_.Set(&sample);
        const auto result = original(controller, context, result_record);
        self->fight_result_sample_.Set(parent);
        return result;
    }

    static std::uintptr_t __fastcall PendingFightHitThunk(
        const std::uintptr_t context, const std::uintptr_t victim) noexcept {
        Impl* self{};
        PendingFightHitFunction original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self != nullptr) {
                lease = self->hooks_.AcquireCallback(kOwner, kGeneration, self->pending_hit_target_);
            }
            original = lease ? self->pending_hit_original_
                : pending_hit_passthrough_.load(std::memory_order_acquire);
        }
        if (original == nullptr) return 0;
        const auto hit = original(context, victim);
        if (lease && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == self->pending_hit_return_) {
            auto* const sample = self->fight_result_sample_.Get();
            if (sample != nullptr && sample->context == context) {
                sample->pending_hit = hit;
                sample->victim = victim;
            }
        }
        return hit;
    }

    static std::uintptr_t __fastcall SetHealthThunk(
        const std::uintptr_t component, const float requested_hp) noexcept {
        Impl* self{};
        SetHealthFunction original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self != nullptr) {
                lease = self->hooks_.AcquireCallback(kOwner, kGeneration, self->set_health_target_);
            }
            original = lease ? self->set_health_original_
                : set_health_passthrough_.load(std::memory_order_acquire);
        }
        if (original == nullptr) return 0;
        auto* const sample = lease ? self->fight_result_sample_.Get() : nullptr;
        if (sample == nullptr || sample->pending_hit == 0 ||
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != self->set_health_return_) {
            return original(component, requested_hp);
        }

        const auto hit = std::exchange(sample->pending_hit, 0);
        const auto before = self->SampleHealth(0, component);
        const auto result = original(component, requested_hp);
        const auto after = before ? self->SampleHealth(0, component) : std::nullopt;
        if (before && after) {
            const double applied = RoundedHealthLoss(*before, *after);
            if (applied > 0.0) {
                try {
                    self->replicated_damage_(component, sample->context, hit, sample->victim,
                        static_cast<float>(applied));
                } catch (...) {
                }
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<float> SampleHealth(
        const std::uintptr_t attributes, const std::uintptr_t target) const noexcept {
        try {
            return read_health_(attributes, target);
        } catch (...) {
            return std::nullopt;
        }
    }

    static double RoundedHealthLoss(const float before, const float after) noexcept {
        // Round the endpoints once to preserve whole-HP totals across fractional hits.
        return std::round(static_cast<double>(before)) - std::round(static_cast<double>(after));
    }

    static std::uintptr_t __fastcall ApplyThunk(const std::uintptr_t attributes,
        const std::uintptr_t victim, const std::uintptr_t hit_source, const float nominal,
        const std::uint8_t force, const std::uintptr_t damage_event, const std::uintptr_t attacker,
        const std::uint8_t transferred, const std::uintptr_t definition) noexcept {
        Impl* self{};
        ApplyFunction original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self != nullptr && self->apply_target_ != nullptr) {
                original = self->apply_original_;
                lease = self->hooks_.AcquireCallback(kOwner, kGeneration, self->apply_target_);
            }
            if (!lease) {
                self = nullptr;
                original = apply_passthrough_.load(std::memory_order_acquire);
            }
        }
        if (original == nullptr) return 0;
        const auto invoke = [&] {
            return original(attributes, victim, hit_source, nominal, force, damage_event,
                attacker, transferred, definition);
        };
        if (self == nullptr || nominal <= 0.0F || damage_event == 0) return invoke();
        const auto before = self->SampleHealth(attributes, victim);
        auto* const enclosing = self->health_sample_.Get();
        if (!before && enclosing == nullptr) return invoke();
        HealthSample sample{enclosing, victim};
        self->health_sample_.Set(&sample);
        const auto result = invoke();
        self->health_sample_.Set(sample.parent);
        const auto after = before ? self->SampleHealth(attributes, victim) : std::nullopt;
        const double delta = before && after ? RoundedHealthLoss(*before, *after) : 0.0;
        // Attribute reentrant hits to their own attacker, even if they target
        // the same character before the outer application returns.
        for (auto* parent = sample.parent; parent != nullptr; parent = parent->parent) {
            if (parent->victim != victim) continue;
            if (before && after) parent->nested_delta += delta;
            else parent->valid = false;
            break;
        }
        if (sample.valid && before && after) {
            const double applied = delta - sample.nested_delta;
            if (applied > 0.0 && attacker != 0) {
                try {
                    self->applied_damage_(damage_event, victim, attacker, static_cast<float>(applied));
                } catch (...) {
                }
            }
        }
        return result;
    }

    static void DispatchExec(
        const std::size_t slot,
        void* const object,
        void* const stack,
        void* const result) noexcept {
        Impl* self{};
        ExecFunction original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self != nullptr && slot < self->exec_count_) {
                original = self->exec_originals_[slot];
                if (original != nullptr) {
                    lease = self->hooks_.AcquireCallback(
                        kOwner, kGeneration, self->exec_targets_[slot]);
                }
                if (!lease) self = nullptr;
            } else {
                self = nullptr;
            }
            if (self == nullptr) {
                original = exec_passthroughs_[slot].load(std::memory_order_acquire);
            }
        }
        if (original == nullptr) return;
        if (self != nullptr && self->exec_callback_) {
            try {
                self->exec_callback_(
                    reinterpret_cast<std::uintptr_t>(self->exec_functions_[slot]),
                    reinterpret_cast<std::uintptr_t>(object),
                    reinterpret_cast<std::uintptr_t>(stack));
            } catch (...) {
            }
        }
        original(object, stack, result);
    }

    template <std::size_t Slot>
    static void __fastcall ExecThunk(void* object, void* stack, void* result) {
        DispatchExec(Slot, object, stack, result);
    }

    static ExecFunction ThunkForSlot(const std::size_t slot) noexcept {
        switch (slot) {
        case 0: return &ExecThunk<0>;
        case 1: return &ExecThunk<1>;
        case 2: return &ExecThunk<2>;
        case 3: return &ExecThunk<3>;
        case 4: return &ExecThunk<4>;
        case 5: return &ExecThunk<5>;
        case 6: return &ExecThunk<6>;
        case 7: return &ExecThunk<7>;
        case 8: return &ExecThunk<8>;
        case 9: return &ExecThunk<9>;
        case 10: return &ExecThunk<10>;
        case 11: return &ExecThunk<11>;
        case 12: return &ExecThunk<12>;
        case 13: return &ExecThunk<13>;
        default: return nullptr;
        }
    }

    HookManager hooks_;
    Callback callback_;
    ExecCallback exec_callback_;
    ReadHealth read_health_;
    AppliedDamage applied_damage_;
    ReplicatedDamage replicated_damage_;
    ThreadLocalScalar<HealthSample*> health_sample_;
    ThreadLocalScalar<FightResultSample*> fight_result_sample_;
    ApplyFunction apply_original_{};
    void* apply_target_{};
    FightResultFunction fight_result_original_{};
    void* fight_result_target_{};
    PendingFightHitFunction pending_hit_original_{};
    void* pending_hit_target_{};
    std::uintptr_t pending_hit_return_{};
    SetHealthFunction set_health_original_{};
    void* set_health_target_{};
    std::uintptr_t set_health_return_{};
    std::atomic_bool applied_started_{};
    NativeFunction original_{};
    void* target_{};
    std::array<ExecFunction, kUe5CombatExecFunctionCapacity> exec_originals_{};
    std::array<void*, kUe5CombatExecFunctionCapacity> exec_functions_{};
    std::array<void*, kUe5CombatExecFunctionCapacity> exec_targets_{};
    std::size_t exec_count_{};
    std::atomic_bool started_{};
    std::atomic_bool attempted_{};
    bool owner_registered_{};
    std::mutex stop_mutex_;
    static std::atomic<Impl*> active_;
    static std::atomic<NativeFunction> passthrough_;
    static std::atomic<ApplyFunction> apply_passthrough_;
    static std::atomic<FightResultFunction> fight_result_passthrough_;
    static std::atomic<PendingFightHitFunction> pending_hit_passthrough_;
    static std::atomic<SetHealthFunction> set_health_passthrough_;
    static std::array<std::atomic<ExecFunction>, kUe5CombatExecFunctionCapacity>
        exec_passthroughs_;
    static std::mutex process_mutex_;
};

std::atomic<Ue5DamageFunctionHook::Impl*> Ue5DamageFunctionHook::Impl::active_{};
std::atomic<Ue5DamageFunctionHook::Impl::NativeFunction>
    Ue5DamageFunctionHook::Impl::passthrough_{};
std::atomic<Ue5DamageFunctionHook::Impl::ApplyFunction>
    Ue5DamageFunctionHook::Impl::apply_passthrough_{};
std::atomic<Ue5DamageFunctionHook::Impl::FightResultFunction>
    Ue5DamageFunctionHook::Impl::fight_result_passthrough_{};
std::atomic<Ue5DamageFunctionHook::Impl::PendingFightHitFunction>
    Ue5DamageFunctionHook::Impl::pending_hit_passthrough_{};
std::atomic<Ue5DamageFunctionHook::Impl::SetHealthFunction>
    Ue5DamageFunctionHook::Impl::set_health_passthrough_{};
std::array<
    std::atomic<Ue5DamageFunctionHook::Impl::ExecFunction>,
    kUe5CombatExecFunctionCapacity>
    Ue5DamageFunctionHook::Impl::exec_passthroughs_{};
std::mutex Ue5DamageFunctionHook::Impl::process_mutex_;

Ue5DamageFunctionHook::Ue5DamageFunctionHook(
    Callback callback, ExecCallback exec_callback, ReadHealth read_health,
    AppliedDamage applied_damage, ReplicatedDamage replicated_damage)
    : Ue5DamageFunctionHook(
          CreateMinHookBackend(), std::move(callback), std::move(exec_callback),
          std::move(read_health), std::move(applied_damage), std::move(replicated_damage)) {}

Ue5DamageFunctionHook::Ue5DamageFunctionHook(
    std::unique_ptr<HookBackend> backend,
    Callback callback,
    ExecCallback exec_callback, ReadHealth read_health, AppliedDamage applied_damage,
    ReplicatedDamage replicated_damage)
    : impl_(std::make_unique<Impl>(
          std::move(backend), std::move(callback), std::move(exec_callback),
          std::move(read_health), std::move(applied_damage), std::move(replicated_damage))) {}

Ue5DamageFunctionHook::~Ue5DamageFunctionHook() {
    if (impl_ != nullptr && !impl_->Stop(std::chrono::milliseconds::zero())) {
        static_cast<void>(impl_.release());
    }
}

bool Ue5DamageFunctionHook::Start(const Ue5DamageFunctionTargets targets) {
    return impl_->Start(targets);
}

bool Ue5DamageFunctionHook::Stop(
    const std::chrono::milliseconds timeout) noexcept {
    return impl_->Stop(timeout);
}

bool Ue5DamageFunctionHook::Attempted() const noexcept {
    return impl_->Attempted();
}

bool Ue5DamageFunctionHook::Started() const noexcept {
    return impl_->Started();
}

bool Ue5DamageFunctionHook::AppliedDamageStarted() const noexcept {
    return impl_->AppliedDamageStarted();
}

std::vector<HookRecordView> Ue5DamageFunctionHook::Snapshot() const {
    return impl_->Snapshot();
}

}  // namespace anomaly
