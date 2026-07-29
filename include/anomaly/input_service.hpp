#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anomaly {

class PluginScope;

// Input adapters normalize platform-specific scan codes before publishing them
// here. Values are host-defined canonical key codes, not Win32 virtual keys.
using InputKey = std::uint16_t;
inline constexpr std::size_t kInputKeyCount = 256;
inline constexpr std::size_t kInputMouseButtonCount = 8;

enum class InputMouseButton : std::uint8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
    X1 = 3,
    X2 = 4,
    Button6 = 5,
    Button7 = 6,
    Button8 = 7,
};

enum class InputModifier : std::uint32_t {
    None = 0,
    Shift = 1U << 0U,
    Control = 1U << 1U,
    Alt = 1U << 2U,
    Super = 1U << 3U,
};

using InputModifierMask = std::uint32_t;

[[nodiscard]] constexpr InputModifierMask ToMask(InputModifier modifier) noexcept {
    return static_cast<InputModifierMask>(modifier);
}

[[nodiscard]] constexpr InputModifierMask operator|(
    InputModifier left, InputModifier right) noexcept {
    return ToMask(left) | ToMask(right);
}

[[nodiscard]] constexpr bool HasModifier(
    InputModifierMask modifiers, InputModifier modifier) noexcept {
    return (modifiers & ToMask(modifier)) != 0;
}

enum class InputCaptureFlag : std::uint32_t {
    None = 0,
    Mouse = 1U << 0U,
    Keyboard = 1U << 1U,
    Text = 1U << 2U,
};

using InputCaptureFlags = std::uint32_t;

[[nodiscard]] constexpr InputCaptureFlags ToFlags(InputCaptureFlag flag) noexcept {
    return static_cast<InputCaptureFlags>(flag);
}

[[nodiscard]] constexpr InputCaptureFlags operator|(
    InputCaptureFlag left, InputCaptureFlag right) noexcept {
    return ToFlags(left) | ToFlags(right);
}

[[nodiscard]] constexpr bool HasCaptureFlag(
    InputCaptureFlags flags, InputCaptureFlag flag) noexcept {
    return (flags & ToFlags(flag)) != 0;
}

enum class InputControlKind : std::uint8_t {
    Key,
    MouseButton,
};

struct InputControl final {
    InputControlKind kind{InputControlKind::Key};
    std::uint16_t code{};

    [[nodiscard]] static constexpr InputControl ForKey(InputKey key) noexcept {
        return {InputControlKind::Key, key};
    }

    [[nodiscard]] static constexpr InputControl ForMouse(
        InputMouseButton button) noexcept {
        return {InputControlKind::MouseButton, static_cast<std::uint16_t>(button)};
    }

    [[nodiscard]] friend constexpr bool operator==(
        InputControl left, InputControl right) noexcept {
        return left.kind == right.kind && left.code == right.code;
    }

    [[nodiscard]] friend constexpr bool operator!=(
        InputControl left, InputControl right) noexcept {
        return !(left == right);
    }
};

[[nodiscard]] constexpr bool IsValidInputControl(InputControl control) noexcept {
    return (control.kind == InputControlKind::Key && control.code != 0 &&
            control.code < kInputKeyCount) ||
        (control.kind == InputControlKind::MouseButton &&
            control.code < kInputMouseButtonCount);
}

struct InputSnapshot final {
    std::uint64_t frame{};
    std::uint64_t sequence{};
    std::uint64_t reset_generation{};
    std::uint64_t timestamp_milliseconds{};
    InputModifierMask modifiers{};
    float mouse_x{};
    float mouse_y{};
    float mouse_delta_x{};
    float mouse_delta_y{};
    std::int32_t mouse_wheel_delta{};
    std::array<bool, kInputKeyCount> keys{};
    std::array<bool, kInputKeyCount> pressed_keys{};
    std::array<bool, kInputKeyCount> released_keys{};
    std::array<bool, kInputMouseButtonCount> mouse_buttons{};
    std::array<bool, kInputMouseButtonCount> pressed_mouse_buttons{};
    std::array<bool, kInputMouseButtonCount> released_mouse_buttons{};

    [[nodiscard]] bool IsDown(InputControl control) const noexcept;
    [[nodiscard]] bool WasPressed(InputControl control) const noexcept;
    [[nodiscard]] bool WasReleased(InputControl control) const noexcept;
};

// This is the host-normalized state for one input ingress. Pressed and released
// edges are derived by comparing it with the preceding InputSnapshot.
struct InputFrameState final {
    std::uint64_t timestamp_milliseconds{};
    InputModifierMask modifiers{};
    float mouse_x{};
    float mouse_y{};
    float mouse_delta_x{};
    float mouse_delta_y{};
    std::int32_t mouse_wheel_delta{};
    std::array<bool, kInputKeyCount> keys{};
    std::array<bool, kInputMouseButtonCount> mouse_buttons{};
};

// UI data is only an ingress-time routing decision. It is intentionally absent
// from InputSnapshot and expires when the next input frame is published.
struct InputUiCaptureState final {
    InputCaptureFlags flags{};
    bool item_hovered{};
    bool window_focused{};
    bool item_active{};
};

struct InputUiCaptureSnapshot final {
    std::uint64_t frame{};
    std::uint64_t sequence{};
    InputUiCaptureState state;
};

struct InputTransition final {
    InputControl control;
    bool pressed{};
    std::uint64_t frame{};
    std::uint64_t sequence{};
};

enum class InputResetReason : std::uint8_t {
    None,
    Explicit,
    FocusLost,
    DeviceReset,
};

struct InputFrameResult final {
    InputSnapshot snapshot;
    std::optional<InputUiCaptureSnapshot> ui_capture;
    std::vector<InputTransition> transitions;
    InputResetReason reset_reason{InputResetReason::None};
};

struct HotkeyHandle final {
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] friend constexpr bool operator==(
        HotkeyHandle left, HotkeyHandle right) noexcept {
        return left.value == right.value;
    }

    [[nodiscard]] friend constexpr bool operator!=(
        HotkeyHandle left, HotkeyHandle right) noexcept {
        return !(left == right);
    }
};

enum class HotkeyCapturePolicy : std::uint8_t {
    RespectUiCapture,
    AllowWhileUiCaptured,
    OnlyWhileUiCaptured,
};

struct HotkeySpec final {
    std::string id;
    InputControl trigger;
    InputModifierMask modifiers{};
    bool exact_modifiers{true};
    HotkeyCapturePolicy capture_policy{HotkeyCapturePolicy::RespectUiCapture};
};

struct HotkeyEvent final {
    HotkeyHandle handle;
    InputSnapshot snapshot;
    InputTransition transition;
    InputUiCaptureSnapshot ui_capture;
};

using HotkeyCallback = std::function<void(const HotkeyEvent&)>;

// Implementations must enqueue callback on their chosen execution domain. The
// InputService never invokes plugin callbacks itself or waits for this function.
using HotkeyDispatcher = std::function<void(
    std::string owner, std::uint64_t generation, std::function<void()> callback)>;

enum class HotkeyRegistrationStatus : std::uint8_t {
    Registered,
    InvalidSpec,
    ScopeUnavailable,
    DispatcherUnavailable,
    Conflict,
};

enum class HotkeyConflictKind : std::uint8_t {
    Binding,
    Identifier,
};

struct HotkeyConflict final {
    HotkeyConflictKind kind{HotkeyConflictKind::Binding};
    HotkeyHandle existing;
    std::string owner;
    std::uint64_t generation{};
    std::string id;
    HotkeySpec spec;
};

struct HotkeyRegistrationResult final {
    HotkeyRegistrationStatus status{HotkeyRegistrationStatus::InvalidSpec};
    HotkeyHandle handle;
    std::optional<HotkeyConflict> conflict;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == HotkeyRegistrationStatus::Registered;
    }
};

// Host-owned input state. The constructor receives a non-blocking dispatcher
// adapter, typically a RuntimeDispatchers::Post wrapper targeting Lifecycle.
class InputService final {
public:
    explicit InputService(HotkeyDispatcher dispatcher);
    ~InputService();

    InputService(const InputService&) = delete;
    InputService& operator=(const InputService&) = delete;
    InputService(InputService&&) = delete;
    InputService& operator=(InputService&&) = delete;

    // Publishes one complete normalized state and derives the frame's edges.
    // The capture argument is evaluated against hotkeys synchronously with this
    // ingress; a later RecordUiCapture call never changes already queued work.
    [[nodiscard]] InputFrameResult AdvanceFrame(
        const InputFrameState& state, InputUiCaptureState capture = {});
    [[nodiscard]] InputFrameResult Reset(
        InputResetReason reason = InputResetReason::Explicit);
    [[nodiscard]] InputFrameResult OnDeviceReset();

    // Render/UI code may publish a newer frame-local arbitration result without
    // mutating persistent key or mouse state. It returns nullopt before input
    // has published its first frame.
    [[nodiscard]] std::optional<InputUiCaptureSnapshot> RecordUiCapture(
        InputUiCaptureState capture);
    [[nodiscard]] InputSnapshot Snapshot() const;
    [[nodiscard]] std::vector<InputTransition> Transitions() const;
    [[nodiscard]] std::optional<InputUiCaptureSnapshot> UiCapture() const;
    [[nodiscard]] bool WasPressed(InputControl control) const;
    [[nodiscard]] bool WasReleased(InputControl control) const;

    [[nodiscard]] HotkeyRegistrationResult RegisterHotkey(
        const std::shared_ptr<PluginScope>& scope,
        HotkeySpec spec,
        HotkeyCallback callback);
    [[nodiscard]] bool ReleaseHotkey(
        const std::shared_ptr<PluginScope>& scope, HotkeyHandle handle);
    [[nodiscard]] std::size_t HotkeyCount() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace anomaly
