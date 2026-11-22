#pragma once

namespace anomaly {

// Window messages retain short taps between frames. The async fallback is only
// used when the focused window's WndProc is unavailable. Never use
// GetAsyncKeyState's shared, consumable low bit to detect a menu press.
class PlatformMenuHotkey final {
public:
    void Configure(unsigned virtual_key, int asynchronous_key_state) noexcept {
        if (virtual_key_ == virtual_key) return;
        virtual_key_ = virtual_key;
        pending_toggle_ = false;
        down_ = (asynchronous_key_state & 0x8000) != 0;
    }

    void RecordKeyDown(unsigned virtual_key, bool repeated) noexcept {
        if (virtual_key == virtual_key_ && !repeated) {
            pending_toggle_ = !pending_toggle_;
        }
    }

    void Reset() noexcept { pending_toggle_ = false; }

    [[nodiscard]] bool Consume(
        bool recording_menu_key, bool focused, int asynchronous_key_state,
        bool use_window_messages) noexcept {
        const bool down = (asynchronous_key_state & 0x8000) != 0;
        const bool pressed = use_window_messages
            ? pending_toggle_
            : down && !down_;
        pending_toggle_ = false;
        // Keep sampling even outside focus/while recording so a held key does
        // not become a new press when either gate opens again.
        down_ = down;
        return !recording_menu_key && focused && pressed;
    }

private:
    unsigned virtual_key_{};
    bool pending_toggle_{};
    bool down_{};
};

}  // namespace anomaly
