#pragma once
#include <anomaly/sdk/localization.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <Windows.h>

namespace vehicle_flight {
struct Binding {
    uint32_t key{}, modifiers{};
    bool operator==(const Binding&) const = default;
};

inline constexpr uint32_t Modifiers = ANOMALY_INPUT_MODIFIER_V1_CONTROL |
    ANOMALY_INPUT_MODIFIER_V1_SHIFT | ANOMALY_INPUT_MODIFIER_V1_ALT;

inline bool Bindable(uint32_t key) {
    if (key == 'W' || key == 'A' || key == 'S' || key == 'D') return false;
    return (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
        (key >= VK_F1 && key <= VK_F24) || (key >= VK_NUMPAD0 && key <= VK_NUMPAD9);
}

class FlightHotkey {
    using Toggle = void(*)(void*) noexcept;
    enum class Result { Ok, Conflict, Unavailable, MovementKey };
public:
    FlightHotkey(void* owner, Toggle toggle) : owner_(owner), toggle_(toggle) {}

    void Start(const AnomalyInputServiceV1* input, Binding saved) {
        input_ = input;
        binding_ = saved;
        if (saved.key) Replace(saved);
    }

    Binding Current() const { return binding_; }
    void CancelCapture() { capturing_ = false; }

    AnomalyStatusV1 Stop() noexcept {
        CancelCapture();
        active_id_ = 0;
        if (handle_.id) {
            const auto result = input_->release_hotkey(input_->user, handle_);
            if (!anomaly::sdk::Succeeded(result) && result.code != ANOMALY_STATUS_V1_NOT_FOUND) return result;
            handle_ = {};
        }
        return anomaly::sdk::Ok();
    }

    bool Draw(const AnomalyUiServiceV1& ui, const anomaly::sdk::Localizer& localizer) {
        using namespace anomaly::sdk;
        if (!input_ || !input_->snapshot || !input_->register_hotkey || !input_->release_hotkey || !ui.button)
            return false;
        bool changed{};
        if (capturing_.load()) {
            ui.text(ui.user, StringView(localizer.Text("hotkey.capture",
                "Press a key or combination. Esc cancels and Backspace clears.")));
            changed = Capture();
        } else {
            const auto name = binding_.key ? Name(binding_) : localizer.Text("hotkey.unbound", "Unbound");
            const auto label = localizer.Format("hotkey.button", "Flight toggle · {0}", std::array<std::string_view, 1>{name});
            if (ui.button(ui.user, StringView(label), 0, 0)) {
                AnomalyInputSnapshotV1 snapshot{sizeof(snapshot)};
                if (Succeeded(input_->snapshot(input_->user, &snapshot))) {
                    std::ranges::copy(snapshot.keys, previous_.begin());
                    result_ = Result::Ok;
                    capturing_ = true;
                } else result_ = Result::Unavailable;
            }
        }
        switch (result_) {
        case Result::Ok: break;
        case Result::Conflict:
            ui.text(ui.user, StringView(localizer.Text("hotkey.conflict", "This shortcut is already in use.")));
            break;
        case Result::Unavailable:
            ui.text(ui.user, StringView(localizer.Text("hotkey.unavailable", "The shortcut could not be registered.")));
            break;
        case Result::MovementKey:
            ui.text(ui.user, StringView(localizer.Text("hotkey.movement_key", "Choose a letter, number or function key outside the flight controls.")));
            break;
        }
        return changed;
    }

private:
    static std::string Name(Binding binding) {
        std::string text;
        if (binding.modifiers & ANOMALY_INPUT_MODIFIER_V1_CONTROL) text += "Ctrl + ";
        if (binding.modifiers & ANOMALY_INPUT_MODIFIER_V1_SHIFT) text += "Shift + ";
        if (binding.modifiers & ANOMALY_INPUT_MODIFIER_V1_ALT) text += "Alt + ";
        if (binding.key >= VK_F1 && binding.key <= VK_F24) return text + "F" + std::to_string(binding.key - VK_F1 + 1);
        if (binding.key >= VK_NUMPAD0 && binding.key <= VK_NUMPAD9) return text + "Num" + std::to_string(binding.key - VK_NUMPAD0);
        return text + static_cast<char>(binding.key);
    }

    bool Capture() {
        AnomalyInputSnapshotV1 snapshot{sizeof(snapshot)};
        if (!anomaly::sdk::Succeeded(input_->snapshot(input_->user, &snapshot))) return false;
        const auto pressed = [&](uint32_t key) {
            const auto bit = 1u << (key % 8);
            return (snapshot.keys[key / 8] & bit) && !(previous_[key / 8] & bit);
        };
        if (pressed(VK_ESCAPE)) {
            CancelCapture();
            return false;
        }
        if (pressed(VK_BACK)) {
            const bool changed = Replace({});
            CancelCapture();
            return changed;
        }
        for (uint32_t key = 0; key < 256; ++key) {
            if (!pressed(key)) continue;
            if (Bindable(key) && !(snapshot.modifiers & ~Modifiers)) {
                const bool changed = Replace({key, snapshot.modifiers});
                CancelCapture();
                return changed;
            }
            if (key >= VK_SPACE && key != VK_LSHIFT && key != VK_RSHIFT && key != VK_LCONTROL &&
                key != VK_RCONTROL && key != VK_LMENU && key != VK_RMENU) result_ = Result::MovementKey;
        }
        std::ranges::copy(snapshot.keys, previous_.begin());
        return false;
    }

    bool Replace(Binding binding) {
        using namespace anomaly::sdk;
        if (!input_ || !input_->register_hotkey || !input_->release_hotkey) {
            result_ = Result::Unavailable;
            return false;
        }
        if (binding == binding_ && handle_.id) {
            result_ = Result::Ok;
            return false;
        }
        AnomalyGenerationHandleV1 next{};
        if (binding.key) {
            const auto id = "flight-toggle-" + std::to_string(binding.modifiers) + "-" + std::to_string(binding.key);
            const AnomalyHotkeySpecV1 spec{sizeof(spec), binding.modifiers, binding.key, ANOMALY_HOTKEY_V1_NONE, StringView(id)};
            const auto result = input_->register_hotkey(input_->user, &spec, Pressed, this, &next);
            if (!Succeeded(result) || !next.id) {
                result_ = result.code == ANOMALY_STATUS_V1_CONFLICT ? Result::Conflict : Result::Unavailable;
                return false;
            }
        }
        if (handle_.id) {
            const auto result = input_->release_hotkey(input_->user, handle_);
            if (!Succeeded(result) && result.code != ANOMALY_STATUS_V1_NOT_FOUND) {
                if (next.id) static_cast<void>(input_->release_hotkey(input_->user, next));
                result_ = Result::Unavailable;
                return false;
            }
        }
        active_id_ = next.id;
        handle_ = next;
        binding_ = binding;
        result_ = Result::Ok;
        return true;
    }

    static void ANOMALY_CALL Pressed(void* raw, AnomalyGenerationHandleV1 handle, const AnomalyInputSnapshotV1* input) noexcept {
        auto& self = *static_cast<FlightHotkey*>(raw);
        if (input && handle.id == self.active_id_.load() && handle.id && !self.capturing_.load() &&
            !(input->capture_flags & (ANOMALY_INPUT_CAPTURE_V1_KEYBOARD | ANOMALY_INPUT_CAPTURE_V1_TEXT)))
            self.toggle_(self.owner_);
    }

    void* owner_;
    Toggle toggle_;
    const AnomalyInputServiceV1* input_{};
    Binding binding_{};
    AnomalyGenerationHandleV1 handle_{};
    std::array<uint8_t, 32> previous_{};
    std::atomic<uint64_t> active_id_{};
    std::atomic<bool> capturing_{};
    Result result_{};
};
}
