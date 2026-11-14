#pragma once
#include "map_teleport.hpp"
#include <anomaly/sdk/localization.hpp>
#include <charconv>
#include <mutex>

namespace collectible_map {
class TeleportHotkey {
    static constexpr std::string_view schema_id = "map-teleport-hotkey";
    static constexpr std::string_view setting = "key";
    static constexpr std::string_view schema =
        R"({"type":"object","additionalProperties":false,"required":["key"],"properties":{"key":{"type":"integer","minimum":0,"maximum":255}}})";
    enum class Result { None, Conflict, Unavailable };

public:
    explicit TeleportHotkey(MapTeleport& teleport) : teleport_(teleport) {}

    void Load(const anomaly::sdk::Host& host) {
        const auto input = host.Query<AnomalyInputServiceV1>(ANOMALY_INPUT_SERVICE_V1_ID);
        if (input && input->struct_size >= sizeof(AnomalyInputServiceV1) && input->was_pressed &&
            input->register_hotkey && input->release_hotkey) input_ = input.get();
        const auto scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
        if (scheduler && scheduler->struct_size >= sizeof(AnomalySchedulerServiceV1) && scheduler->schedule)
            scheduler_ = scheduler.get();
        const auto config = host.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID);
        if (!config || config->struct_size < sizeof(AnomalyConfigServiceV1) || !config->register_schema ||
            !config->read || !config->write_atomic) return;
        if (!anomaly::sdk::Succeeded(config->register_schema(config->user, anomaly::sdk::StringView(schema_id),
                1, Bytes(schema), &schema_handle_))) return;
        config_ = config.get();
        const auto json = host.Query<AnomalyJsonServiceV1>(ANOMALY_JSON_SERVICE_V1_ID);
        if (!json || json->struct_size < sizeof(AnomalyJsonServiceV1) || !json->parse || !json->object_find ||
            !json->number_value || !json->release) return;

        std::array<uint8_t, 128> document{};
        size_t size = document.size();
        uint32_t version{};
        if (!anomaly::sdk::Succeeded(config_->read(config_->user, anomaly::sdk::StringView(schema_id), &version,
                {document.data(), document.size()}, &size)) || version != 1 || !size || size > document.size()) return;
        AnomalyGenerationHandleV1 root{}, key{};
        if (!anomaly::sdk::Succeeded(json->parse(json->user,
                {reinterpret_cast<const char*>(document.data()), size}, &root))) return;
        if (anomaly::sdk::Succeeded(json->object_find(json->user, root, anomaly::sdk::StringView(setting), &key))) {
            double value{};
            if (anomaly::sdk::Succeeded(json->number_value(json->user, key, &value)) &&
                value >= 1 && value <= 255 && std::floor(value) == value && Usable(static_cast<uint32_t>(value)))
                key_ = static_cast<uint32_t>(value);
            static_cast<void>(json->release(json->user, key));
        }
        static_cast<void>(json->release(json->user, root));
    }

    void Start() {
        if (key_) Replace(key_, false);
    }

    void CancelCapture() noexcept { capturing_ = false; }

    void Stop() noexcept {
        CancelCapture();
        active_id_ = 0;
        if (handle_.id) static_cast<void>(input_->release_hotkey(input_->user, handle_));
        handle_ = {};
        Save();
    }

    void Draw(const AnomalyUiServiceV1* ui, const anomaly::sdk::Localizer& localizer) {
        using anomaly::sdk::StringView;
        if (input_ && ui->button && capturing_.load()) {
            ui->text(ui->user, StringView(localizer.Text("hotkey.capture", "Press a key. Esc cancels, Backspace restores the default.")));
            if (Pressed(VK_ESCAPE)) capturing_ = false;
            else if (Pressed(VK_BACK)) {
                Replace(0, true);
                capturing_ = false;
            } else {
                for (uint32_t key = 1; key <= 255; ++key) {
                    if (!Usable(key) || !Pressed(key)) continue;
                    Replace(key, true);
                    capturing_ = false;
                    break;
                }
            }
        } else {
            auto name = localizer.Text("hotkey.default", "Ctrl + left-click");
            if (key_ && handle_.id) name += " / " + KeyName(key_);
            const auto label = localizer.Format("hotkey.change", "Teleport shortcut · {0}",
                                             std::array<std::string_view, 1>{name});
            if (!input_ || !ui->button) {
                ui->text(ui->user, StringView(label));
            } else if (ui->button(ui->user, StringView(label), 0, 0)) {
                result_ = Result::None;
                capturing_ = true;
            }
        }

        switch (result_) {
        case Result::None: break;
        case Result::Conflict:
            ui->text(ui->user, StringView(localizer.Text("hotkey.conflict", "That key is already bound. Choose another.")));
            break;
        case Result::Unavailable:
            ui->text(ui->user, StringView(localizer.Text("hotkey.unavailable", "The teleport key could not be registered.")));
            break;
        }
        if (save_failed_.load())
            ui->text(ui->user, StringView(localizer.Text("hotkey.save_failed", "The teleport key could not be saved.")));
    }

private:
    static AnomalyByteSpanV1 Bytes(std::string_view text) {
        return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    }

    static bool Usable(uint32_t key) {
        return (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
               (key >= VK_F1 && key <= VK_F24) || (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) ||
               (key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE ||
               key == VK_SPACE || key == VK_TAB || key == VK_RETURN;
    }

    static std::string KeyName(uint32_t key) {
        if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) return std::string(1, static_cast<char>(key));
        if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return "Num" + std::to_string(key - VK_NUMPAD0);
        switch (key) {
        case VK_PRIOR: return "Page Up";
        case VK_NEXT: return "Page Down";
        case VK_END: return "End";
        case VK_HOME: return "Home";
        case VK_LEFT: return "←";
        case VK_UP: return "↑";
        case VK_RIGHT: return "→";
        case VK_DOWN: return "↓";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_RETURN: return "Enter";
        default: return {};
        }
    }

    bool Pressed(uint32_t key) const {
        int32_t pressed{};
        return anomaly::sdk::Succeeded(input_->was_pressed(input_->user, key, &pressed)) && pressed;
    }

    static void ANOMALY_CALL OnHotkey(void* raw, AnomalyGenerationHandleV1 handle,
                                    const AnomalyInputSnapshotV1* input) noexcept {
        auto& self = *static_cast<TeleportHotkey*>(raw);
        if (handle.id != self.active_id_.load() || self.capturing_.load() ||
            !input || (input->capture_flags & ANOMALY_INPUT_CAPTURE_V1_TEXT)) return;
        self.teleport_.RequestAtCursor();
    }

    static void ANOMALY_CALL SaveTask(void* raw, AnomalyGenerationHandleV1) noexcept {
        static_cast<TeleportHotkey*>(raw)->Save();
    }

    void Save() noexcept {
        std::scoped_lock lock(save_mutex_);
        const auto key = pending_save_.exchange(-1);
        if (key < 0 || !config_) return;
        constexpr std::string_view prefix = R"({"key":)";
        std::array<char, 24> document{};
        std::copy(prefix.begin(), prefix.end(), document.begin());
        const auto end = std::to_chars(document.data() + prefix.size(), document.data() + document.size() - 1, key);
        *end.ptr = '}';
        const auto bytes = Bytes({document.data(), static_cast<size_t>(end.ptr + 1 - document.data())});
        const auto result = config_->write_atomic(config_->user, anomaly::sdk::StringView(schema_id), 1, bytes);
        save_failed_ = !anomaly::sdk::Succeeded(result);
        if (save_failed_) {
            int empty = -1;
            pending_save_.compare_exchange_strong(empty, key);
        }
    }

    void Replace(uint32_t key, bool save) {
        using anomaly::sdk::StringView;
        if (!input_) return;
        AnomalyGenerationHandleV1 replacement{};
        if (key == key_ && handle_.id && !save_failed_.load()) {
            result_ = Result::None;
            return;
        }
        if (key && (key != key_ || !handle_.id)) {
            const auto id = "map-teleport-" + std::to_string(key);
            AnomalyHotkeySpecV1 spec{sizeof(spec)};
            spec.virtual_key = key;
            spec.flags = ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED;
            spec.id = StringView(id);
            const auto result = input_->register_hotkey(input_->user, &spec, OnHotkey, this, &replacement);
            if (!anomaly::sdk::Succeeded(result) || !replacement.id) {
                result_ = result.code == ANOMALY_STATUS_V1_CONFLICT ? Result::Conflict : Result::Unavailable;
                return;
            }
        }
        if (handle_.id && (replacement.id || key == 0)) {
            const auto result = input_->release_hotkey(input_->user, handle_);
            if (!anomaly::sdk::Succeeded(result) && result.code != ANOMALY_STATUS_V1_NOT_FOUND) {
                if (replacement.id) static_cast<void>(input_->release_hotkey(input_->user, replacement));
                result_ = Result::Unavailable;
                return;
            }
        }
        if (replacement.id || key == 0) handle_ = replacement;
        active_id_ = handle_.id;
        key_ = key;
        result_ = Result::None;
        if (save) {
            if (!config_) {
                save_failed_ = true;
                return;
            }
            pending_save_ = static_cast<int>(key);
            AnomalyGenerationHandleV1 task{};
            if (scheduler_ && !anomaly::sdk::Succeeded(scheduler_->schedule(scheduler_->user, 0, SaveTask, this, &task)))
                save_failed_ = true;
        }
    }

    MapTeleport& teleport_;
    const AnomalyInputServiceV1* input_{};
    const AnomalyConfigServiceV1* config_{};
    const AnomalySchedulerServiceV1* scheduler_{};
    AnomalyGenerationHandleV1 schema_handle_{}, handle_{};
    std::atomic<uint64_t> active_id_{};
    std::atomic<bool> capturing_{};
    std::atomic<bool> save_failed_{};
    std::atomic<int> pending_save_{-1};
    std::mutex save_mutex_;
    uint32_t key_{};
    Result result_{};
};
}
