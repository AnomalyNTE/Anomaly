#include "anomaly/input_service.hpp"

#include "anomaly/plugin_scope.hpp"

#include <atomic>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace anomaly {
namespace {

constexpr InputModifierMask kKnownModifiers =
    ToMask(InputModifier::Shift) |
    ToMask(InputModifier::Control) |
    ToMask(InputModifier::Alt) |
    ToMask(InputModifier::Super);

constexpr InputCaptureFlags kKnownCaptureFlags =
    ToFlags(InputCaptureFlag::Mouse) |
    ToFlags(InputCaptureFlag::Keyboard) |
    ToFlags(InputCaptureFlag::Text);

void IncrementMonotonic(std::uint64_t& value) noexcept {
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
}

[[nodiscard]] InputModifierMask NormalizeModifiers(InputModifierMask modifiers) noexcept {
    return modifiers & kKnownModifiers;
}

[[nodiscard]] InputUiCaptureState NormalizeCapture(InputUiCaptureState capture) noexcept {
    capture.flags &= kKnownCaptureFlags;
    return capture;
}

[[nodiscard]] bool MatchesModifiers(
    const InputModifierMask required,
    const bool exact,
    const InputModifierMask actual) noexcept {
    return exact ? required == actual : (actual & required) == required;
}

[[nodiscard]] bool Conflicts(const HotkeySpec& left, const HotkeySpec& right) noexcept {
    if (left.trigger != right.trigger) return false;
    for (InputModifierMask modifiers{}; modifiers <= kKnownModifiers; ++modifiers) {
        if (MatchesModifiers(left.modifiers, left.exact_modifiers, modifiers) &&
            MatchesModifiers(right.modifiers, right.exact_modifiers, modifiers)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool IsUiCaptured(
    const InputUiCaptureState& capture, const InputControl control) noexcept {
    if (control.kind == InputControlKind::MouseButton) {
        return HasCaptureFlag(capture.flags, InputCaptureFlag::Mouse);
    }
    return HasCaptureFlag(capture.flags, InputCaptureFlag::Keyboard) ||
        HasCaptureFlag(capture.flags, InputCaptureFlag::Text);
}

[[nodiscard]] bool AllowsCapture(
    const HotkeyCapturePolicy policy,
    const InputUiCaptureState& capture,
    const InputControl control) noexcept {
    const bool captured = IsUiCaptured(capture, control);
    switch (policy) {
    case HotkeyCapturePolicy::RespectUiCapture:
        return !captured;
    case HotkeyCapturePolicy::AllowWhileUiCaptured:
        return true;
    case HotkeyCapturePolicy::OnlyWhileUiCaptured:
        return captured;
    }
    return false;
}

[[nodiscard]] bool ValidHotkeySpec(
    const HotkeySpec& spec, const HotkeyCallback& callback) noexcept {
    return !spec.id.empty() && IsValidInputControl(spec.trigger) &&
        (spec.modifiers & ~kKnownModifiers) == 0 && static_cast<bool>(callback);
}

void AppendTransitions(
    const InputSnapshot& previous,
    InputSnapshot& next,
    std::vector<InputTransition>& transitions) {
    next.pressed_keys.fill(false);
    next.released_keys.fill(false);
    next.pressed_mouse_buttons.fill(false);
    next.released_mouse_buttons.fill(false);

    for (std::size_t index = 1; index < kInputKeyCount; ++index) {
        if (previous.keys[index] == next.keys[index]) continue;
        if (next.keys[index]) {
            next.pressed_keys[index] = true;
        } else {
            next.released_keys[index] = true;
        }
        transitions.push_back({
            InputControl::ForKey(static_cast<InputKey>(index)), next.keys[index],
            next.frame, next.sequence});
    }

    for (std::size_t index{}; index < kInputMouseButtonCount; ++index) {
        if (previous.mouse_buttons[index] == next.mouse_buttons[index]) continue;
        if (next.mouse_buttons[index]) {
            next.pressed_mouse_buttons[index] = true;
        } else {
            next.released_mouse_buttons[index] = true;
        }
        transitions.push_back({
            InputControl::ForMouse(static_cast<InputMouseButton>(index)),
            next.mouse_buttons[index], next.frame, next.sequence});
    }
}

}  // namespace

bool InputSnapshot::IsDown(const InputControl control) const noexcept {
    if (!IsValidInputControl(control)) return false;
    const std::size_t index = control.code;
    return control.kind == InputControlKind::Key ? keys[index] : mouse_buttons[index];
}

bool InputSnapshot::WasPressed(const InputControl control) const noexcept {
    if (!IsValidInputControl(control)) return false;
    const std::size_t index = control.code;
    return control.kind == InputControlKind::Key
        ? pressed_keys[index] : pressed_mouse_buttons[index];
}

bool InputSnapshot::WasReleased(const InputControl control) const noexcept {
    if (!IsValidInputControl(control)) return false;
    const std::size_t index = control.code;
    return control.kind == InputControlKind::Key
        ? released_keys[index] : released_mouse_buttons[index];
}

class InputService::Impl final : public std::enable_shared_from_this<InputService::Impl> {
public:
    explicit Impl(HotkeyDispatcher dispatcher) : dispatcher_(std::move(dispatcher)) {}

    [[nodiscard]] InputFrameResult Advance(
        const InputFrameState& state, InputUiCaptureState capture) {
        std::vector<PendingDispatch> pending;
        InputFrameResult result;
        {
            std::scoped_lock lock(mutex_);

            InputSnapshot next = snapshot_;
            IncrementMonotonic(next.frame);
            IncrementMonotonic(next.sequence);
            next.timestamp_milliseconds = state.timestamp_milliseconds;
            next.modifiers = NormalizeModifiers(state.modifiers);
            next.mouse_x = state.mouse_x;
            next.mouse_y = state.mouse_y;
            next.mouse_delta_x = state.mouse_delta_x;
            next.mouse_delta_y = state.mouse_delta_y;
            next.mouse_wheel_delta = state.mouse_wheel_delta;
            next.keys = state.keys;
            next.keys[0] = false;
            next.mouse_buttons = state.mouse_buttons;

            std::vector<InputTransition> transitions;
            transitions.reserve(kInputKeyCount + kInputMouseButtonCount);
            AppendTransitions(snapshot_, next, transitions);

            snapshot_ = next;
            transitions_ = transitions;
            capture_ = InputUiCaptureSnapshot{
                snapshot_.frame, NextCaptureSequenceLocked(), NormalizeCapture(capture)};

            result.snapshot = snapshot_;
            result.ui_capture = capture_;
            result.transitions = transitions_;
            CollectDispatchesLocked(*capture_, result.transitions, pending);
        }
        Dispatch(std::move(pending));
        return result;
    }

    [[nodiscard]] InputFrameResult Reset(const InputResetReason reason) {
        std::scoped_lock lock(mutex_);

        InputSnapshot next = snapshot_;
        IncrementMonotonic(next.frame);
        IncrementMonotonic(next.sequence);
        IncrementMonotonic(next.reset_generation);
        next.modifiers = 0;
        next.mouse_x = 0.0F;
        next.mouse_y = 0.0F;
        next.mouse_delta_x = 0.0F;
        next.mouse_delta_y = 0.0F;
        next.mouse_wheel_delta = 0;
        next.keys.fill(false);
        next.mouse_buttons.fill(false);

        std::vector<InputTransition> transitions;
        transitions.reserve(kInputKeyCount + kInputMouseButtonCount);
        AppendTransitions(snapshot_, next, transitions);
        snapshot_ = next;
        transitions_ = transitions;
        capture_.reset();
        IncrementMonotonic(capture_sequence_);

        return {snapshot_, std::nullopt, transitions_, reason};
    }

    [[nodiscard]] std::optional<InputUiCaptureSnapshot> RecordCapture(
        InputUiCaptureState capture) {
        std::scoped_lock lock(mutex_);
        if (snapshot_.frame == 0) return std::nullopt;
        capture_ = InputUiCaptureSnapshot{
            snapshot_.frame, NextCaptureSequenceLocked(), NormalizeCapture(capture)};
        return capture_;
    }

    [[nodiscard]] InputSnapshot Snapshot() const {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    [[nodiscard]] std::vector<InputTransition> Transitions() const {
        std::scoped_lock lock(mutex_);
        return transitions_;
    }

    [[nodiscard]] std::optional<InputUiCaptureSnapshot> Capture() const {
        std::scoped_lock lock(mutex_);
        return capture_;
    }

    [[nodiscard]] bool WasPressed(const InputControl control) const {
        std::scoped_lock lock(mutex_);
        return snapshot_.WasPressed(control);
    }

    [[nodiscard]] bool WasReleased(const InputControl control) const {
        std::scoped_lock lock(mutex_);
        return snapshot_.WasReleased(control);
    }

    [[nodiscard]] HotkeyRegistrationResult Register(
        const std::shared_ptr<PluginScope>& scope,
        HotkeySpec spec,
        HotkeyCallback callback) {
        if (scope == nullptr) {
            return {HotkeyRegistrationStatus::ScopeUnavailable, {}, std::nullopt};
        }
        if (!ValidHotkeySpec(spec, callback)) {
            return {HotkeyRegistrationStatus::InvalidSpec, {}, std::nullopt};
        }

        const std::string resource_label = "input.hotkey:" + spec.id;
        auto entry = std::make_shared<Entry>();
        {
            std::scoped_lock lock(mutex_);
            if (!dispatcher_) {
                return {HotkeyRegistrationStatus::DispatcherUnavailable, {}, std::nullopt};
            }
            for (const auto& [unused_handle, existing] : hotkeys_) {
                static_cast<void>(unused_handle);
                if (!existing->active.load(std::memory_order_acquire)) continue;
                if (existing->owner == scope->Owner() &&
                    existing->generation == scope->Generation() &&
                    existing->spec.id == spec.id) {
                    return {HotkeyRegistrationStatus::Conflict, {}, HotkeyConflict{
                        HotkeyConflictKind::Identifier, existing->handle, existing->owner,
                        existing->generation, existing->spec.id, existing->spec}};
                }
                if (Conflicts(existing->spec, spec)) {
                    return {HotkeyRegistrationStatus::Conflict, {}, HotkeyConflict{
                        HotkeyConflictKind::Binding, existing->handle, existing->owner,
                        existing->generation, existing->spec.id, existing->spec}};
                }
            }

            entry->handle = {next_hotkey_++};
            entry->scope = scope;
            entry->owner = scope->Owner();
            entry->generation = scope->Generation();
            entry->spec = std::move(spec);
            entry->callback = std::move(callback);
            hotkeys_.emplace(entry->handle.value, entry);
        }

        const std::weak_ptr<Impl> weak_impl = weak_from_this();
        const std::weak_ptr<Entry> weak_entry = entry;
        std::uint64_t token{};
        try {
            token = scope->Register(
                PluginResourceKind::Input, resource_label,
                [weak_impl, weak_entry, handle = entry->handle] {
                    if (const auto locked_entry = weak_entry.lock()) {
                        locked_entry->active.store(false, std::memory_order_release);
                        locked_entry->registered.store(false, std::memory_order_release);
                    }
                    if (const auto locked_impl = weak_impl.lock()) {
                        locked_impl->Deactivate(handle);
                    }
                });
        } catch (...) {
            Deactivate(entry->handle);
            return {HotkeyRegistrationStatus::ScopeUnavailable, {}, std::nullopt};
        }

        if (token == 0) {
            Deactivate(entry->handle);
            return {HotkeyRegistrationStatus::ScopeUnavailable, {}, std::nullopt};
        }

        entry->resource_token.store(token, std::memory_order_release);
        if (!entry->active.load(std::memory_order_acquire)) {
            Deactivate(entry->handle);
            return {HotkeyRegistrationStatus::ScopeUnavailable, {}, std::nullopt};
        }
        entry->registered.store(true, std::memory_order_release);
        if (!entry->active.load(std::memory_order_acquire)) {
            Deactivate(entry->handle);
            return {HotkeyRegistrationStatus::ScopeUnavailable, {}, std::nullopt};
        }

        return {HotkeyRegistrationStatus::Registered, entry->handle, std::nullopt};
    }

    [[nodiscard]] bool Release(
        const std::shared_ptr<PluginScope>& scope, const HotkeyHandle handle) {
        if (scope == nullptr || !handle) return false;

        std::shared_ptr<Entry> entry;
        {
            std::scoped_lock lock(mutex_);
            const auto found = hotkeys_.find(handle.value);
            if (found == hotkeys_.end()) return false;
            entry = found->second;
            const auto registered_scope = entry->scope.lock();
            if (registered_scope == nullptr || registered_scope.get() != scope.get() ||
                entry->owner != scope->Owner() || entry->generation != scope->Generation()) {
                return false;
            }
        }

        const std::uint64_t token = entry->resource_token.load(std::memory_order_acquire);
        if (token == 0) return false;
        const bool released = scope->Release(token);
        Deactivate(handle);
        return released;
    }

    [[nodiscard]] std::size_t HotkeyCount() const {
        std::scoped_lock lock(mutex_);
        std::size_t count{};
        for (const auto& [unused_handle, entry] : hotkeys_) {
            static_cast<void>(unused_handle);
            if (entry->active.load(std::memory_order_acquire) &&
                entry->registered.load(std::memory_order_acquire)) {
                ++count;
            }
        }
        return count;
    }

    void DeactivateAll() noexcept {
        std::unordered_map<std::uint64_t, std::shared_ptr<Entry>> entries;
        {
            std::scoped_lock lock(mutex_);
            for (const auto& [unused_handle, entry] : hotkeys_) {
                static_cast<void>(unused_handle);
                entry->active.store(false, std::memory_order_release);
                entry->registered.store(false, std::memory_order_release);
            }
            entries.swap(hotkeys_);
        }
    }

private:
    struct Entry final {
        HotkeyHandle handle;
        std::weak_ptr<PluginScope> scope;
        std::string owner;
        std::uint64_t generation{};
        HotkeySpec spec;
        HotkeyCallback callback;
        std::atomic_bool active{true};
        std::atomic_bool registered{};
        std::atomic<std::uint64_t> resource_token{};
    };

    struct PendingDispatch final {
        std::string owner;
        std::uint64_t generation{};
        std::weak_ptr<Entry> entry;
        HotkeyEvent event;
    };

    [[nodiscard]] std::uint64_t NextCaptureSequenceLocked() noexcept {
        IncrementMonotonic(capture_sequence_);
        return capture_sequence_;
    }

    void CollectDispatchesLocked(
        const InputUiCaptureSnapshot& capture,
        const std::vector<InputTransition>& transitions,
        std::vector<PendingDispatch>& pending) const {
        for (const InputTransition& transition : transitions) {
            if (!transition.pressed) continue;
            for (const auto& [unused_handle, entry] : hotkeys_) {
                static_cast<void>(unused_handle);
                if (!entry->active.load(std::memory_order_acquire) ||
                    !entry->registered.load(std::memory_order_acquire) ||
                    entry->spec.trigger != transition.control ||
                    !MatchesModifiers(
                        entry->spec.modifiers, entry->spec.exact_modifiers,
                        snapshot_.modifiers) ||
                    !AllowsCapture(
                        entry->spec.capture_policy, capture.state, transition.control)) {
                    continue;
                }
                pending.push_back({
                    entry->owner, entry->generation, entry,
                    {entry->handle, snapshot_, transition, capture}});
            }
        }
    }

    void Dispatch(std::vector<PendingDispatch> pending) const {
        if (!dispatcher_) return;
        for (PendingDispatch& next : pending) {
            try {
                dispatcher_(
                    next.owner, next.generation,
                    [entry = next.entry, event = std::move(next.event)]() mutable {
                        const auto locked_entry = entry.lock();
                        if (locked_entry == nullptr ||
                            !locked_entry->active.load(std::memory_order_acquire) ||
                            !locked_entry->registered.load(std::memory_order_acquire)) {
                            return;
                        }
                        const auto scope = locked_entry->scope.lock();
                        if (scope == nullptr || scope->Owner() != locked_entry->owner ||
                            scope->Generation() != locked_entry->generation) {
                            return;
                        }
                        auto lease = scope->AcquireCallback(locked_entry->generation);
                        if (!lease ||
                            !locked_entry->active.load(std::memory_order_acquire) ||
                            !locked_entry->registered.load(std::memory_order_acquire)) {
                            return;
                        }
                        try {
                            locked_entry->callback(event);
                        } catch (...) {
                        }
                    });
            } catch (...) {
                // Input ingress must never fall back to a direct plugin callback.
            }
        }
    }

    void Deactivate(const HotkeyHandle handle) noexcept {
        std::shared_ptr<Entry> entry;
        {
            std::scoped_lock lock(mutex_);
            const auto found = hotkeys_.find(handle.value);
            if (found == hotkeys_.end()) return;
            entry = std::move(found->second);
            entry->active.store(false, std::memory_order_release);
            entry->registered.store(false, std::memory_order_release);
            hotkeys_.erase(found);
        }
    }

    mutable std::mutex mutex_;
    HotkeyDispatcher dispatcher_;
    InputSnapshot snapshot_;
    std::vector<InputTransition> transitions_;
    std::optional<InputUiCaptureSnapshot> capture_;
    std::uint64_t capture_sequence_{};
    std::unordered_map<std::uint64_t, std::shared_ptr<Entry>> hotkeys_;
    std::uint64_t next_hotkey_{1};
};

InputService::InputService(HotkeyDispatcher dispatcher)
    : impl_(std::make_shared<Impl>(std::move(dispatcher))) {}

InputService::~InputService() {
    if (impl_ != nullptr) impl_->DeactivateAll();
}

InputFrameResult InputService::AdvanceFrame(
    const InputFrameState& state, InputUiCaptureState capture) {
    return impl_->Advance(state, capture);
}

InputFrameResult InputService::Reset(const InputResetReason reason) {
    return impl_->Reset(reason);
}

InputFrameResult InputService::OnDeviceReset() {
    return impl_->Reset(InputResetReason::DeviceReset);
}

std::optional<InputUiCaptureSnapshot> InputService::RecordUiCapture(
    InputUiCaptureState capture) {
    return impl_->RecordCapture(capture);
}

InputSnapshot InputService::Snapshot() const {
    return impl_->Snapshot();
}

std::vector<InputTransition> InputService::Transitions() const {
    return impl_->Transitions();
}

std::optional<InputUiCaptureSnapshot> InputService::UiCapture() const {
    return impl_->Capture();
}

bool InputService::WasPressed(const InputControl control) const {
    return impl_->WasPressed(control);
}

bool InputService::WasReleased(const InputControl control) const {
    return impl_->WasReleased(control);
}

HotkeyRegistrationResult InputService::RegisterHotkey(
    const std::shared_ptr<PluginScope>& scope,
    HotkeySpec spec,
    HotkeyCallback callback) {
    return impl_->Register(scope, std::move(spec), std::move(callback));
}

bool InputService::ReleaseHotkey(
    const std::shared_ptr<PluginScope>& scope, const HotkeyHandle handle) {
    return impl_->Release(scope, handle);
}

std::size_t InputService::HotkeyCount() const {
    return impl_->HotkeyCount();
}

}  // namespace anomaly
