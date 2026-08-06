#include "anomaly/nte_navigation_input_policy.hpp"
#include "anomaly/thread_local_value.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace anomaly {
namespace {

constexpr std::string_view kOwner = "anomaly.nte.navigation";
constexpr std::uint64_t kGeneration = 1;
constexpr std::uint8_t kPatrolInputType = 0x0F;

using ClientIgnoreGameAndUiInputFn =
    void(__fastcall*)(void*, bool, std::uint8_t, bool);
using GetPlayerCharacterFn = void*(__fastcall*)(void*);
using SetCustomInputFn = void(__fastcall*)(void*, std::uint8_t, bool, bool);

ThreadLocalScalar<void*> g_scope;

template <typename Function>
Function VirtualFunction(void* object, const std::uint32_t byte_offset) noexcept {
    if (object == nullptr) return nullptr;
    auto** vtable = *static_cast<void***>(object);
    if (vtable == nullptr) return nullptr;
    return reinterpret_cast<Function>(
        *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(vtable) + byte_offset));
}

}  // namespace

class NteNavigationInputPolicy::Impl final {
public:
    Impl(
        std::unique_ptr<HookBackend> backend,
        const std::uint32_t controller_get_player_character_vtable_offset,
        const std::uint32_t character_set_custom_ignore_move_input_vtable_offset,
        const std::uint32_t character_set_custom_limit_input_vtable_offset)
        : hooks_(std::move(backend)),
          get_player_character_offset_(controller_get_player_character_vtable_offset),
          set_custom_ignore_move_input_offset_(
              character_set_custom_ignore_move_input_vtable_offset),
          set_custom_limit_input_offset_(character_set_custom_limit_input_vtable_offset) {}

    bool Start(void* target) {
        std::scoped_lock lock(mutex_);
        if (target == nullptr || started_.load(std::memory_order_acquire) ||
            owner_registered_) {
            return false;
        }
        std::scoped_lock process_lock(process_mutex_);
        if (active_.load(std::memory_order_acquire) != nullptr) return false;
        original_ = nullptr;
        if (!hooks_.Create(
                std::string(kOwner), kGeneration, "navigation-input-policy", target,
                reinterpret_cast<void*>(&Detour),
                reinterpret_cast<void**>(&original_))) {
            return false;
        }
        owner_registered_ = true;
        passthrough_.store(
            reinterpret_cast<ClientIgnoreGameAndUiInputFn>(target),
            std::memory_order_release);
        active_.store(this, std::memory_order_release);
        if (!hooks_.EnableOwner(kOwner, kGeneration)) {
            if (hooks_.RemoveOwner(kOwner, kGeneration)) {
                owner_registered_ = false;
                Impl* expected = this;
                static_cast<void>(active_.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel));
                original_ = nullptr;
            }
            return false;
        }
        target_ = target;
        started_.store(true, std::memory_order_release);
        return true;
    }

    bool Stop(std::chrono::milliseconds timeout) noexcept {
        std::scoped_lock lock(mutex_);
        if (!owner_registered_) {
            started_.store(false, std::memory_order_release);
            return true;
        }
        started_.store(false, std::memory_order_release);
        const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
        const bool disabled = hooks_.DisableOwner(kOwner, kGeneration);
        if (!disabled) {
            if (!hooks_.RemoveOwner(kOwner, kGeneration, bounded)) return false;
        }
        if (disabled) {
            std::scoped_lock process_lock(process_mutex_);
            Impl* expected = this;
            static_cast<void>(active_.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel));
        }
        if (disabled && !hooks_.RemoveOwner(kOwner, kGeneration, bounded)) return false;
        if (!disabled) {
            std::scoped_lock process_lock(process_mutex_);
            Impl* expected = this;
            static_cast<void>(active_.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel));
        }
        owner_registered_ = false;
        target_ = nullptr;
        original_ = nullptr;
        return true;
    }

    bool Started() const noexcept {
        return started_.load(std::memory_order_acquire);
    }

    void* Enter() noexcept {
        void* previous = g_scope.Get();
        g_scope.Set(this);
        return previous;
    }

    void Leave(void* previous) noexcept {
        g_scope.Set(previous);
    }

private:
    static void __fastcall Detour(
        void* controller,
        const bool ignore_move,
        const std::uint8_t input_type,
        const bool apply_all_characters) noexcept {
        Impl* self{};
        ClientIgnoreGameAndUiInputFn original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self != nullptr && self->original_ != nullptr) {
                lease = self->hooks_.AcquireCallback(kOwner, kGeneration);
                if (lease) original = self->original_;
                else self = nullptr;
            }
            if (self == nullptr) {
                original = passthrough_.load(std::memory_order_acquire);
            }
        }
        if (original == nullptr) return;
        const bool rewrite = self != nullptr && g_scope.Get() == self && ignore_move &&
            input_type == kPatrolInputType && apply_all_characters;
        if (!rewrite || !self->ApplyPatrolInputState(controller, input_type, apply_all_characters)) {
            original(controller, ignore_move, input_type, apply_all_characters);
        }
    }

    bool ApplyPatrolInputState(
        void* controller,
        const std::uint8_t input_type,
        const bool apply_all_characters) const noexcept {
        const auto get_player_character = VirtualFunction<GetPlayerCharacterFn>(
            controller, get_player_character_offset_);
        if (get_player_character == nullptr) return false;
        void* character = get_player_character(controller);
        if (character == nullptr) return false;
        const auto set_ignore_move = VirtualFunction<SetCustomInputFn>(
            character, set_custom_ignore_move_input_offset_);
        const auto set_limit_input = VirtualFunction<SetCustomInputFn>(
            character, set_custom_limit_input_offset_);
        if (set_ignore_move == nullptr || set_limit_input == nullptr) return false;
        set_ignore_move(character, input_type, true, apply_all_characters);
        set_limit_input(character, input_type, true, apply_all_characters);
        return true;
    }

    HookManager hooks_;
    const std::uint32_t get_player_character_offset_{};
    const std::uint32_t set_custom_ignore_move_input_offset_{};
    const std::uint32_t set_custom_limit_input_offset_{};
    ClientIgnoreGameAndUiInputFn original_{};
    void* target_{};
    bool owner_registered_{};
    std::atomic_bool started_{};
    std::mutex mutex_;
    static std::atomic<Impl*> active_;
    static std::atomic<ClientIgnoreGameAndUiInputFn> passthrough_;
    static std::mutex process_mutex_;
};

std::atomic<NteNavigationInputPolicy::Impl*> NteNavigationInputPolicy::Impl::active_{};
std::atomic<ClientIgnoreGameAndUiInputFn>
    NteNavigationInputPolicy::Impl::passthrough_{};
std::mutex NteNavigationInputPolicy::Impl::process_mutex_;

NteNavigationInputPolicy::NteNavigationInputPolicy(
    std::unique_ptr<HookBackend> backend,
    const std::uint32_t controller_get_player_character_vtable_offset,
    const std::uint32_t character_set_custom_ignore_move_input_vtable_offset,
    const std::uint32_t character_set_custom_limit_input_vtable_offset)
    : impl_(std::make_unique<Impl>(
          std::move(backend), controller_get_player_character_vtable_offset,
          character_set_custom_ignore_move_input_vtable_offset,
          character_set_custom_limit_input_vtable_offset)) {}

NteNavigationInputPolicy::~NteNavigationInputPolicy() {
    if (impl_ != nullptr && !impl_->Stop(std::chrono::milliseconds::zero())) {
        static_cast<void>(impl_.release());
    }
}

bool NteNavigationInputPolicy::Start(void* target) { return impl_->Start(target); }
bool NteNavigationInputPolicy::Stop(std::chrono::milliseconds timeout) noexcept {
    return impl_->Stop(timeout);
}
bool NteNavigationInputPolicy::Started() const noexcept { return impl_->Started(); }
void* NteNavigationInputPolicy::Enter() noexcept { return impl_->Enter(); }
void NteNavigationInputPolicy::Leave(void* previous) noexcept { impl_->Leave(previous); }

}  // namespace anomaly
