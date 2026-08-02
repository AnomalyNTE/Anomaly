#include "anomaly/input_service.hpp"
#include "anomaly/plugin_scope.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr anomaly::InputKey kKeyA = 65;
constexpr anomaly::InputKey kKeyB = 66;
constexpr anomaly::InputKey kKeyG = 71;

struct QueuedDispatch final {
    std::string owner;
    std::uint64_t generation{};
    std::function<void()> callback;
};

class DeferredDispatcher final {
public:
    void Post(
        std::string owner, const std::uint64_t generation, std::function<void()> callback) {
        queued_.push_back({std::move(owner), generation, std::move(callback)});
    }

    void RunAll() {
        std::vector<QueuedDispatch> queued;
        queued.swap(queued_);
        for (QueuedDispatch& next : queued) next.callback();
    }

    [[nodiscard]] std::size_t Size() const noexcept { return queued_.size(); }
    [[nodiscard]] const QueuedDispatch& Front() const { return queued_.front(); }

private:
    std::vector<QueuedDispatch> queued_;
};

bool Check(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::shared_ptr<anomaly::PluginScope> MakeScope(
    const std::shared_ptr<anomaly::ResourceLedger>& ledger,
    std::string owner,
    const std::uint64_t generation) {
    return std::make_shared<anomaly::PluginScope>(ledger, std::move(owner), generation);
}

anomaly::HotkeySpec MakeHotkey(
    std::string id,
    const anomaly::InputKey key,
    const anomaly::InputModifierMask modifiers = 0) {
    anomaly::HotkeySpec spec;
    spec.id = std::move(id);
    spec.trigger = anomaly::InputControl::ForKey(key);
    spec.modifiers = modifiers;
    return spec;
}

bool TestSnapshotAndEdges() {
    anomaly::InputService input([](
        std::string, std::uint64_t, std::function<void()>) {});

    anomaly::InputFrameState state;
    state.timestamp_milliseconds = 41;
    state.modifiers = anomaly::InputModifier::Control | anomaly::InputModifier::Shift;
    state.mouse_x = 42.0F;
    state.mouse_y = 24.0F;
    state.mouse_delta_x = 3.0F;
    state.mouse_delta_y = -2.0F;
    state.mouse_wheel_delta = 120;
    state.keys[kKeyA] = true;
    state.mouse_buttons[static_cast<std::size_t>(anomaly::InputMouseButton::Left)] = true;

    const auto first = input.AdvanceFrame(state);
    const auto key_a = anomaly::InputControl::ForKey(kKeyA);
    const auto left_mouse = anomaly::InputControl::ForMouse(anomaly::InputMouseButton::Left);
    bool result = Check(first.snapshot.frame == 1 && first.snapshot.sequence == 1,
                        "first input frame did not advance frame and sequence") &&
        Check(first.snapshot.timestamp_milliseconds == 41,
              "snapshot did not retain host timestamp") &&
        Check(first.snapshot.IsDown(key_a) && first.snapshot.WasPressed(key_a),
              "key press edge was not derived") &&
        Check(first.snapshot.IsDown(left_mouse) && first.snapshot.WasPressed(left_mouse),
              "mouse press edge was not derived") &&
        Check(first.transitions.size() == 2,
              "key and mouse transitions were not recorded") &&
        Check(input.WasPressed(key_a), "service pressed query did not use coherent snapshot");

    const auto held = input.AdvanceFrame(state);
    result = Check(held.snapshot.frame == 2 && held.snapshot.sequence == 2,
                   "held frame did not advance monotonically") &&
        Check(!held.snapshot.WasPressed(key_a) && held.transitions.empty(),
              "held input retriggered a transition") && result;

    state.keys[kKeyA] = false;
    state.mouse_buttons[static_cast<std::size_t>(anomaly::InputMouseButton::Left)] = false;
    const auto released = input.AdvanceFrame(state);
    return Check(released.snapshot.WasReleased(key_a) &&
                     released.snapshot.WasReleased(left_mouse),
                 "release edges were not derived") &&
        Check(input.WasReleased(key_a), "service release query did not use coherent snapshot") && result;
}

bool TestUiCaptureIsFrameLocalAndSeparate() {
    DeferredDispatcher dispatcher;
    anomaly::InputService input([&dispatcher](
        std::string owner, const std::uint64_t generation, std::function<void()> callback) {
        dispatcher.Post(std::move(owner), generation, std::move(callback));
    });
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = MakeScope(ledger, "capture.plugin", 3);
    std::uint32_t callbacks{};
    const auto registration = input.RegisterHotkey(
        scope, MakeHotkey("capture", kKeyA),
        [&callbacks](const anomaly::HotkeyEvent&) { ++callbacks; });
    if (!Check(static_cast<bool>(registration), "capture hotkey registration failed")) return false;

    anomaly::InputFrameState state;
    state.keys[kKeyA] = true;
    anomaly::InputUiCaptureState capture;
    capture.flags = anomaly::ToFlags(anomaly::InputCaptureFlag::Keyboard) |
        anomaly::ToFlags(anomaly::InputCaptureFlag::Text);
    capture.item_hovered = true;
    const auto captured = input.AdvanceFrame(state, capture);
    const auto persisted = input.Snapshot();

    bool result = Check(captured.ui_capture.has_value() &&
                            captured.ui_capture->frame == captured.snapshot.frame,
                        "capture was not tied to the input frame") &&
        Check(captured.ui_capture->state.item_hovered,
              "frame-local UI signal was not recorded") &&
        Check(dispatcher.Size() == 0 && callbacks == 0,
              "keyboard-captured hotkey escaped input arbitration") &&
        Check(persisted.sequence == captured.snapshot.sequence &&
                  persisted.WasPressed(anomaly::InputControl::ForKey(kKeyA)),
              "capture unexpectedly mutated persistent input state");

    anomaly::InputUiCaptureState newer_capture;
    newer_capture.flags = anomaly::ToFlags(anomaly::InputCaptureFlag::Mouse);
    const auto recorded = input.RecordUiCapture(newer_capture);
    result = Check(recorded.has_value() &&
                       recorded->frame == captured.snapshot.frame &&
                       recorded->sequence > captured.ui_capture->sequence,
                   "UI capture update did not get its own frame-local sequence") &&
        Check(input.Snapshot().sequence == persisted.sequence,
              "recording capture changed persistent input sequence") && result;

    const auto held = input.AdvanceFrame(state);
    result = Check(held.ui_capture.has_value() && held.ui_capture->state.flags == 0,
                   "a capture decision leaked into the next input frame") && result;

    state.keys[kKeyA] = false;
    static_cast<void>(input.AdvanceFrame(state));
    state.keys[kKeyA] = true;
    static_cast<void>(input.AdvanceFrame(state));
    result = Check(dispatcher.Size() == 1,
                   "hotkey was not rearmed after release under uncaptured input") && result;
    dispatcher.RunAll();
    return Check(callbacks == 1, "deferred hotkey callback was not delivered") && result;
}

bool TestScopedHotkeysAndConflicts() {
    DeferredDispatcher dispatcher;
    anomaly::InputService input([&dispatcher](
        std::string owner, const std::uint64_t generation, std::function<void()> callback) {
        dispatcher.Post(std::move(owner), generation, std::move(callback));
    });
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto first_scope = MakeScope(ledger, "first.plugin", 7);
    const auto second_scope = MakeScope(ledger, "second.plugin", 9);
    std::uint32_t callbacks{};
    anomaly::HotkeyEvent callback_event;
    const auto first = input.RegisterHotkey(
        first_scope,
        MakeHotkey("toggle", kKeyG, anomaly::ToMask(anomaly::InputModifier::Control)),
        [&callbacks, &callback_event](const anomaly::HotkeyEvent& event) {
            ++callbacks;
            callback_event = event;
        });
    if (!Check(static_cast<bool>(first), "scoped hotkey registration failed")) return false;

    const auto same_binding = input.RegisterHotkey(
        second_scope,
        MakeHotkey("same-binding", kKeyG, anomaly::ToMask(anomaly::InputModifier::Control)),
        [](const anomaly::HotkeyEvent&) {});
    const auto same_identifier = input.RegisterHotkey(
        first_scope, MakeHotkey("toggle", kKeyB), [](const anomaly::HotkeyEvent&) {});

    bool result = Check(input.HotkeyCount() == 1,
                        "registered hotkey was not retained by the input service") &&
        Check(first_scope->Resources().size() == 1 &&
                  first_scope->Resources().front().kind == anomaly::PluginResourceKind::Input,
              "hotkey was not registered in the PluginScope input ledger") &&
        Check(same_binding.status == anomaly::HotkeyRegistrationStatus::Conflict &&
                  same_binding.conflict.has_value() &&
                  same_binding.conflict->kind == anomaly::HotkeyConflictKind::Binding &&
                  same_binding.conflict->owner == "first.plugin",
              "binding conflict did not report the current owner") &&
        Check(same_identifier.status == anomaly::HotkeyRegistrationStatus::Conflict &&
                  same_identifier.conflict.has_value() &&
                  same_identifier.conflict->kind == anomaly::HotkeyConflictKind::Identifier,
              "per-scope identifier conflict was not detected");

    anomaly::InputFrameState state;
    state.keys[kKeyG] = true;
    state.modifiers = anomaly::ToMask(anomaly::InputModifier::Control);
    const auto frame = input.AdvanceFrame(state);
    result = Check(dispatcher.Size() == 1 && callbacks == 0,
                   "input ingress invoked a plugin callback directly") &&
        Check(dispatcher.Front().owner == "first.plugin" && dispatcher.Front().generation == 7,
              "hotkey dispatcher did not receive owner and generation") && result;
    dispatcher.RunAll();
    result = Check(callbacks == 1 && callback_event.handle == first.handle &&
                       callback_event.snapshot.sequence == frame.snapshot.sequence,
                   "deferred callback did not receive a stable hotkey event") && result;

    const auto rejected_release = input.ReleaseHotkey(second_scope, first.handle);
    result = Check(!rejected_release && input.HotkeyCount() == 1,
                   "another scope released a foreign hotkey") && result;
    const auto released = input.ReleaseHotkey(first_scope, first.handle);
    return Check(released && input.HotkeyCount() == 0 && first_scope->Resources().empty(),
                 "explicit hotkey release did not revoke ledger state") && result;
}

bool TestScopeCleanupAndReset() {
    DeferredDispatcher dispatcher;
    anomaly::InputService input([&dispatcher](
        std::string owner, const std::uint64_t generation, std::function<void()> callback) {
        dispatcher.Post(std::move(owner), generation, std::move(callback));
    });
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = MakeScope(ledger, "cleanup.plugin", 11);
    std::uint32_t callbacks{};
    const auto registration = input.RegisterHotkey(
        scope, MakeHotkey("cleanup", kKeyA),
        [&callbacks](const anomaly::HotkeyEvent&) { ++callbacks; });
    if (!Check(static_cast<bool>(registration), "cleanup hotkey registration failed")) return false;

    anomaly::InputFrameState state;
    state.keys[kKeyA] = true;
    state.mouse_buttons[static_cast<std::size_t>(anomaly::InputMouseButton::Right)] = true;
    anomaly::InputUiCaptureState capture;
    capture.flags = anomaly::ToFlags(anomaly::InputCaptureFlag::Mouse);
    static_cast<void>(input.AdvanceFrame(state, capture));

    bool result = Check(dispatcher.Size() == 1,
                        "matching hotkey was not posted before cleanup") &&
        Check(scope->FreezeCallbackSources(),
              "scope did not freeze input callback sources");
    dispatcher.RunAll();
    result = Check(callbacks == 0,
                   "queued callback bypassed the PluginScope callback lease") && result;

    result = Check(scope->RevokeAll() == 1 && input.HotkeyCount() == 0,
                   "scope cleanup did not revoke the input registration") &&
        Check(scope->Resources().empty(), "scope cleanup left an input ledger record") && result;
    state.keys[kKeyA] = false;
    static_cast<void>(input.AdvanceFrame(state));
    state.keys[kKeyA] = true;
    static_cast<void>(input.AdvanceFrame(state));
    result = Check(dispatcher.Size() == 0,
                   "revoked scope still produced a queued hotkey callback") && result;

    const auto reset = input.OnDeviceReset();
    const auto key_a = anomaly::InputControl::ForKey(kKeyA);
    const auto right_mouse = anomaly::InputControl::ForMouse(anomaly::InputMouseButton::Right);
    return Check(reset.reset_reason == anomaly::InputResetReason::DeviceReset,
                 "device reset did not report its reason") &&
        Check(reset.snapshot.reset_generation == 1 && !reset.snapshot.IsDown(key_a) &&
                  !reset.snapshot.IsDown(right_mouse),
              "device reset left persistent key or mouse state down") &&
        Check(reset.snapshot.WasReleased(key_a) && reset.snapshot.WasReleased(right_mouse),
              "device reset did not publish release edges for held controls") &&
        Check(!reset.ui_capture.has_value() && !input.UiCapture().has_value(),
              "device reset left a stale UI capture arbitration record") && result;
}

}  // namespace

int main() {
    bool result = TestSnapshotAndEdges();
    result = TestUiCaptureIsFrameLocalAndSeparate() && result;
    result = TestScopedHotkeysAndConflicts() && result;
    result = TestScopeCleanupAndReset() && result;
    return result ? 0 : 1;
}
