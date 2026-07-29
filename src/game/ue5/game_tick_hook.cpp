#include "anomaly/game_tick_hook.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace anomaly {
namespace {

inline constexpr std::string_view kOwner = "anomaly.ue5.framework";
inline constexpr std::uint64_t kGeneration = 1;

}  // namespace

class GameTickHook::Impl final {
public:
    using TickFunction = void(__fastcall*)(void*, float, bool);

    Impl(std::unique_ptr<HookBackend> backend, Callback callback)
        : hooks_(std::move(backend)), callback_(std::move(callback)) {
        if (!callback_) throw std::invalid_argument("GameTickHook requires callback");
    }

    bool Start(void* target) {
        std::scoped_lock stop_lock(stop_mutex_);
        if (target == nullptr || started_.load(std::memory_order_acquire)) return false;
        if (owner_registered_) return false;
        std::scoped_lock process_lock(process_mutex_);
        if (active_.load(std::memory_order_acquire) != nullptr) return false;
        original_ = nullptr;
        if (!hooks_.Create(
                std::string(kOwner), kGeneration, "game-tick", target,
                reinterpret_cast<void*>(&TickThunk),
                reinterpret_cast<void**>(&original_))) {
            return false;
        }
        owner_registered_ = true;
        active_.store(this, std::memory_order_release);
        if (!hooks_.EnableOwner(kOwner, kGeneration)) {
            active_.store(nullptr, std::memory_order_release);
            if (hooks_.RemoveOwner(kOwner, kGeneration)) owner_registered_ = false;
            original_ = nullptr;
            return false;
        }
        target_ = target;
        started_.store(true, std::memory_order_release);
        return true;
    }

    bool Stop(std::chrono::milliseconds timeout) noexcept {
        std::scoped_lock stop_lock(stop_mutex_);
        const bool was_started = started_.exchange(false, std::memory_order_acq_rel);
        if (!owner_registered_) return true;
        if (was_started) {
            static_cast<void>(hooks_.DisableOwner(kOwner, kGeneration));
            std::scoped_lock process_lock(process_mutex_);
            Impl* expected = this;
            static_cast<void>(active_.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel));
        }
        const auto bounded_timeout =
            (std::max)(timeout, std::chrono::milliseconds::zero());
        if (!hooks_.RemoveOwner(kOwner, kGeneration, bounded_timeout)) return false;
        owner_registered_ = false;
        original_ = nullptr;
        target_ = nullptr;
        return true;
    }

    bool Started() const noexcept { return started_.load(std::memory_order_acquire); }
    std::vector<HookRecordView> Snapshot() const { return hooks_.Snapshot(); }

private:
    static void __fastcall TickThunk(void* self_pointer, float delta_seconds, bool idle) {
        Impl* self{};
        TickFunction original{};
        PluginScope::CallbackLease lease;
        {
            // Stop clears active_ under the same lock before beginning the
            // callback drain. Therefore every self pointer that leaves this
            // block owns a lease which keeps its generation quarantinable.
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self == nullptr || self->original_ == nullptr) return;
            lease = self->hooks_.AcquireCallback(kOwner, kGeneration);
            if (!lease) return;
            original = self->original_;
        }
        original(self_pointer, delta_seconds, idle);
        try {
            self->callback_(static_cast<double>(delta_seconds));
        } catch (...) {
        }
    }

    HookManager hooks_;
    Callback callback_;
    TickFunction original_{};
    void* target_{};
    std::atomic_bool started_{};
    bool owner_registered_{};
    std::mutex stop_mutex_;
    static std::atomic<Impl*> active_;
    static std::mutex process_mutex_;
};

std::atomic<GameTickHook::Impl*> GameTickHook::Impl::active_{};
std::mutex GameTickHook::Impl::process_mutex_;

GameTickHook::GameTickHook(Callback callback)
    : GameTickHook(CreateMinHookBackend(), std::move(callback)) {}

GameTickHook::GameTickHook(std::unique_ptr<HookBackend> backend, Callback callback)
    : impl_(std::make_unique<Impl>(std::move(backend), std::move(callback))) {}

GameTickHook::~GameTickHook() {
    if (impl_ != nullptr && !impl_->Stop(std::chrono::milliseconds::zero())) {
        // The detour is disabled and detached from active_, but its callback
        // frame still uses Impl. Keep that generation mapped for process life.
        static_cast<void>(impl_.release());
    }
}

bool GameTickHook::Start(void* target) { return impl_->Start(target); }
bool GameTickHook::Stop(std::chrono::milliseconds timeout) noexcept {
    return impl_->Stop(timeout);
}
bool GameTickHook::Started() const noexcept { return impl_->Started(); }
std::vector<HookRecordView> GameTickHook::Snapshot() const { return impl_->Snapshot(); }

}  // namespace anomaly
