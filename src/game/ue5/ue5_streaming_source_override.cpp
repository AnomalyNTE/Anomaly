#include "anomaly/ue5_streaming_source_override.hpp"

#include "anomaly/hook_manager.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace anomaly {
namespace {

inline constexpr std::string_view kOwner = "anomaly.ue5.streaming-source";
inline constexpr std::uint64_t kGeneration = 1;
inline constexpr std::string_view kLabel = "ue5-streaming-source";

// PlayerController streaming source update: void(object, double location[3], double rotation[3]).
using StreamingSourceFn = void(__fastcall*)(void*, double*, double*);

}  // namespace

struct Ue5StreamingSourceOverride::Impl final {
public:
    Impl() : hooks_(CreateMinHookBackend()) {}

    ~Impl() { Remove(); }

    bool Install(std::uintptr_t controller, std::uintptr_t streaming_source) {
        if (controller == 0 || streaming_source == 0) return false;
        std::scoped_lock stop_lock(stop_mutex_);
        const auto target = reinterpret_cast<void*>(streaming_source);
        if (owner_registered_) {
            // A different function is a different streaming source implementation and
            // the caller has to decide; the existing hook stays untouched.
            if (target_ != target) return false;
            controller_.store(controller, std::memory_order_release);
            return true;
        }
        std::scoped_lock process_lock(process_mutex_);
        if (active_.load(std::memory_order_acquire) != nullptr) return false;
        original_ = nullptr;
        if (!hooks_.Create(
                std::string(kOwner), kGeneration, std::string(kLabel), target,
                reinterpret_cast<void*>(&StreamingSourceThunk),
                reinterpret_cast<void**>(&original_))) {
            return false;
        }
        owner_registered_ = true;
        controller_.store(controller, std::memory_order_release);
        active_.store(this, std::memory_order_release);
        if (!hooks_.EnableOwner(kOwner, kGeneration)) {
            active_.store(nullptr, std::memory_order_release);
            if (hooks_.RemoveOwner(kOwner, kGeneration)) owner_registered_ = false;
            original_ = nullptr;
            return false;
        }
        target_ = target;
        return true;
    }

    void Remove() noexcept {
        std::scoped_lock stop_lock(stop_mutex_);
        active_flag_.store(false, std::memory_order_release);
        if (!owner_registered_) return;
        if (hooks_.DisableOwner(kOwner, kGeneration)) {
            static_cast<void>(hooks_.RemoveOwner(kOwner, kGeneration));
            owner_registered_ = false;
        }
        {
            std::scoped_lock process_lock(process_mutex_);
            Impl* expected = this;
            static_cast<void>(active_.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel));
        }
        original_ = nullptr;
        target_ = nullptr;
        controller_.store(0, std::memory_order_release);
    }

    [[nodiscard]] bool Installed() const noexcept {
        return owner_registered_ && target_ != nullptr;
    }

    [[nodiscard]] std::uintptr_t Controller() const noexcept {
        return controller_.load(std::memory_order_acquire);
    }

    void SetOverride(
        const Position& position, const Position& rotation, bool redirect_rotation,
        std::chrono::milliseconds duration) {
        std::scoped_lock lock(override_mutex_);
        position_ = position;
        rotation_ = rotation;
        redirect_rotation_ = redirect_rotation;
        deadline_ = duration.count() > 0
            ? std::chrono::steady_clock::now() + duration
            : std::chrono::steady_clock::time_point::max();
        active_flag_.store(true, std::memory_order_release);
    }

    void ClearOverride() noexcept {
        active_flag_.store(false, std::memory_order_release);
        std::scoped_lock lock(override_mutex_);
        deadline_ = std::chrono::steady_clock::time_point::min();
    }

    [[nodiscard]] bool Active() const noexcept {
        return active_flag_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::chrono::milliseconds Remaining(
        std::chrono::steady_clock::time_point now) const noexcept {
        if (!Active()) return std::chrono::milliseconds::zero();
        std::scoped_lock lock(override_mutex_);
        if (deadline_ == std::chrono::steady_clock::time_point::max()) {
            return std::chrono::milliseconds::max();
        }
        const auto remaining = deadline_ - now;
        return remaining <= std::chrono::steady_clock::duration::zero()
            ? std::chrono::milliseconds::zero()
            : std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    }

    void Expire(std::chrono::steady_clock::time_point now) noexcept {
        if (!Active()) return;
        std::scoped_lock lock(override_mutex_);
        if (deadline_ != std::chrono::steady_clock::time_point::max() && now >= deadline_) {
            active_flag_.store(false, std::memory_order_release);
        }
    }

private:
    static void __fastcall StreamingSourceThunk(
        void* object, double* location, double* rotation) {
        Impl* self{};
        StreamingSourceFn original{};
        PluginScope::CallbackLease lease;
        {
            // Remove clears active_ under the same lock before draining callbacks, so
            // every self pointer that leaves this block owns a lease on its generation.
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self == nullptr || self->original_ == nullptr) return;
            lease = self->hooks_.AcquireCallback(kOwner, kGeneration);
            if (!lease) return;
            original = self->original_;
        }
        Position position{};
        Position override_rotation{};
        bool redirect = false;
        bool redirect_rotation = false;
        {
            std::scoped_lock lock(self->override_mutex_);
            redirect = self->active_flag_.load(std::memory_order_acquire) &&
                object == reinterpret_cast<void*>(
                    self->controller_.load(std::memory_order_acquire));
            if (redirect) {
                position = self->position_;
                override_rotation = self->rotation_;
                redirect_rotation = self->redirect_rotation_;
            }
        }
        try {
            original(object, location, rotation);
        } catch (...) {
        }
        // The game keeps its own bookkeeping from the original call; only the location
        // the world streams around is rewritten.
        if (redirect && location != nullptr && rotation != nullptr) {
            for (std::size_t axis = 0; axis != position.size(); ++axis) {
                location[axis] = position[axis];
                if (redirect_rotation) rotation[axis] = override_rotation[axis];
            }
        }
    }

    HookManager hooks_;
    StreamingSourceFn original_{};
    void* target_{};
    std::atomic<std::uintptr_t> controller_{};
    std::atomic_bool active_flag_{};
    mutable std::mutex override_mutex_;
    Position position_{};
    Position rotation_{};
    bool redirect_rotation_{};
    std::chrono::steady_clock::time_point deadline_{
        std::chrono::steady_clock::time_point::min()};
    bool owner_registered_{};
    std::mutex stop_mutex_;
    static std::atomic<Impl*> active_;
    static std::mutex process_mutex_;
};

std::atomic<Ue5StreamingSourceOverride::Impl*> Ue5StreamingSourceOverride::Impl::active_{};
std::mutex Ue5StreamingSourceOverride::Impl::process_mutex_;

Ue5StreamingSourceOverride::Ue5StreamingSourceOverride()
    : impl_(std::make_unique<Impl>()) {}

Ue5StreamingSourceOverride::~Ue5StreamingSourceOverride() = default;

bool Ue5StreamingSourceOverride::Install(
    std::uintptr_t controller, std::uintptr_t streaming_source) {
    return impl_->Install(controller, streaming_source);
}

void Ue5StreamingSourceOverride::Remove() noexcept { impl_->Remove(); }

bool Ue5StreamingSourceOverride::Installed() const noexcept { return impl_->Installed(); }

std::uintptr_t Ue5StreamingSourceOverride::Controller() const noexcept {
    return impl_->Controller();
}

void Ue5StreamingSourceOverride::SetOverride(
    const Position& position, const Position& rotation, bool redirect_rotation,
    std::chrono::milliseconds duration) {
    impl_->SetOverride(position, rotation, redirect_rotation, duration);
}

void Ue5StreamingSourceOverride::ClearOverride() noexcept { impl_->ClearOverride(); }

bool Ue5StreamingSourceOverride::Active() const noexcept { return impl_->Active(); }

std::chrono::milliseconds Ue5StreamingSourceOverride::Remaining(
    std::chrono::steady_clock::time_point now) const noexcept {
    return impl_->Remaining(now);
}

void Ue5StreamingSourceOverride::Expire(std::chrono::steady_clock::time_point now) noexcept {
    impl_->Expire(now);
}

}  // namespace anomaly