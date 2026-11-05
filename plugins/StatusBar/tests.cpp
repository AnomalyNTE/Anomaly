#include "plugin.cpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace fixture {
void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string Text(AnomalyStringViewV1 value) { return {value.data, value.size}; }

struct Probe final {
    bool hidden{}, close_click{}, reopen{}, collapsed{}, rendering{};
    unsigned begins{}, ends{}, glass{}, settings_text{}, localization_queries{};
    unsigned reads{}, writes{}, long_retries{};
    std::vector<std::string> overlay_text;
    std::string title;
    std::string long_help = std::string(700, 'h') + " complete translation";
    AnomalyUiServiceV1 ui{sizeof(ui), 1, this};
    AnomalyLocalizationServiceV1 language{sizeof(language), 1, this};
    AnomalyStorageServiceV1 storage{sizeof(storage), 1, this};
    AnomalyHostApiV1 host{};

    Probe() {
        ui.begin_window = [](void* user, AnomalyStringViewV1 title, int* open, uint32_t) {
            auto& p = *static_cast<Probe*>(user);
            Check(open != nullptr, "the settings window supplied no close flag to the host");
            ++p.begins;
            p.title = Text(title);
            if (p.hidden) return 0;
            // The host resets the plugin's close flag when its list reopens the window.
            if (p.reopen) { *open = 1; p.reopen = false; }
            if (p.close_click) { *open = 0; p.hidden = true; p.close_click = false; }
            return *open && !p.collapsed ? 1 : 0;
        };
        ui.end_window = [](void* user) { ++static_cast<Probe*>(user)->ends; };
        ui.text = [](void* user, AnomalyStringViewV1) { ++static_cast<Probe*>(user)->settings_text; };
        ui.separator = [](void*) {};
        ui.same_line = [](void*, float, float) {};
        ui.button_enabled = [](void*, AnomalyStringViewV1, float, float, int) { return 0; };
        ui.checkbox = [](void*, AnomalyStringViewV1, int*) { return 0; };
        ui.slider_float = [](void*, AnomalyStringViewV1, float*, float, float) { return 0; };
        ui.overlay_viewport_size = [](void*, float* width, float* height) { *width = 1920; *height = 1080; };
        ui.overlay_measure_text = [](void*, AnomalyStringViewV1 value, float, float* width, float* height) {
            *width = static_cast<float>(value.size) * 8.0F; *height = 16;
        };
        ui.overlay_glass = [](void* user, float, float, float, float, const AnomalyUiGlassStyleV1*) {
            ++static_cast<Probe*>(user)->glass;
            return 1;
        };
        ui.overlay_text = [](void* user, float, float, AnomalyStringViewV1 value, uint32_t, float) {
            static_cast<Probe*>(user)->overlay_text.push_back(Text(value));
        };
        language.translate = [](void* user, AnomalyStringViewV1 key, AnomalyStringViewV1,
                                const AnomalyStringViewV1* args, size_t count,
                                char* output, size_t* size) {
            auto& p = *static_cast<Probe*>(user);
            const auto id = Text(key);
            std::string value;
            if (id == "window.title") value = "\u72b6\u6001\u6761";
            else if (id == "help") value = p.long_help;
            else if (id == "bar.memory") {
                Check(count == 1, "memory text did not pass its numeric argument");
                value = "\u5185\u5b58 " + Text(args[0]) + "%";
            } else if (id == "bar.gpu_temperature") {
                Check(count == 1, "temperature text did not pass its numeric argument");
                value = "GPU " + Text(args[0]) + "\u00B0C";
            } else return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND, 0, {}};
            const auto required = value.size() + 1;
            if (!output || *size < required) {
                if (id == "help") ++p.long_retries;
                *size = required;
                return AnomalyStatusV1{ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, 0, {}};
            }
            std::memcpy(output, value.c_str(), required); *size = required;
            return anomaly::sdk::Ok();
        };
        storage.read = [](void* user, AnomalyStringViewV1 path, AnomalyMutableByteSpanV1 output, size_t* size) {
            auto& p = *static_cast<Probe*>(user);
            Check(!p.rendering && Text(path) == "settings.txt", "settings read occurred on Draw or changed the state path");
            ++p.reads;
            const std::string original = "position=5\nspacing=130\ncpu=0\ngpu=1\n";
            Check(output.size >= original.size(), "settings fixture buffer is too small");
            std::memcpy(output.data, original.data(), original.size()); *size = original.size();
            return anomaly::sdk::Ok();
        };
        storage.write_atomic = [](void* user, AnomalyStringViewV1, AnomalyByteSpanV1) {
            auto& p = *static_cast<Probe*>(user);
            Check(!p.rendering, "settings were written from Draw"); ++p.writes;
            return anomaly::sdk::Ok();
        };
        host.struct_size = sizeof(host); host.api_major = 1; host.host_context = this;
        host.query_service = [](void* user, AnomalyStringViewV1 id, uint32_t, const void** table) {
            auto& p = *static_cast<Probe*>(user); *table = nullptr;
            const auto key = Text(id);
            if (key == ANOMALY_UI_SERVICE_V1_ID) *table = &p.ui;
            else if (key == ANOMALY_LOCALIZATION_SERVICE_V1_ID) { ++p.localization_queries; *table = &p.language; }
            else if (key == ANOMALY_STORAGE_SERVICE_V1_ID) *table = &p.storage;
            return *table ? anomaly::sdk::Ok() : AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND, 0, {}};
        };
    }

    void DrawFrame(Context& context, const AnomalyUiServiceV1* service = nullptr) {
        rendering = true;
        Draw(&context, service);
        rendering = false;
        Check(begins == ends, "window scopes were not balanced after Draw");
    }
};
}

int main() {
    using fixture::Check;
    fixture::Probe p;
    AnomalyPluginDescriptorV1 descriptor{sizeof(descriptor)};
    Check(Succeeded(AnomalyPluginEntryV1(&descriptor)), "plugin entry failed");
    Check(fixture::Text(descriptor.id) == "b1ank.status-bar" && fixture::Text(descriptor.version) == "0.3.2",
          "plugin identity or version differs from its manifest");
    void* loaded{};
    Check(Succeeded(descriptor.on_load(&p.host, &loaded)) && loaded, "load failed without optional font/texture services");
    auto& context = *static_cast<Context*>(loaded);
    Check(p.localization_queries == 1 && p.reads == 1, "load did not cache one localizer and read the existing settings");
    Check(context.texts.help == p.long_help && p.long_retries == 1, "long translation was truncated or fell back to English");
    Check(context.settings.position == Position::BottomRight && context.settings.spacing == 130 && !context.settings.shown[kCpu],
          "the existing settings document was not preserved");
    const auto original_settings = context.settings;
    context.readings.memory_percent = 42; context.readings.gpu_celsius = 62;
    p.DrawFrame(context);
    Check(p.title == "\u72b6\u6001\u6761###status-bar" && p.settings_text > 0 && p.glass == 1,
          "the localized settings and overlay did not render together");
    Check(std::ranges::find(p.overlay_text, "\u5185\u5b58 42%") != p.overlay_text.end() &&
          std::ranges::find(p.overlay_text, "GPU 62\u00B0C") != p.overlay_text.end(),
          "localized metric patterns did not receive/reorder the real readings");
    const auto before_close_text = p.settings_text;
    p.close_click = true; p.DrawFrame(context);
    Check(context.window_open == 0 && p.hidden && p.settings_text == before_close_text,
          "clicking X failed to close the settings body");
    Check(p.glass == 2, "closing the settings also stopped the overlay");
    p.DrawFrame(context);
    Check(p.glass == 3 && p.settings_text == before_close_text && context.window_open == 0,
          "a hidden settings window reopened itself or suppressed the overlay");
    p.hidden = false; p.reopen = true; p.DrawFrame(context);
    Check(context.window_open == 1 && p.settings_text > before_close_text && p.glass == 4,
          "the host could not reopen settings after closing them");
    const auto before_collapse_text = p.settings_text;
    p.collapsed = true; p.DrawFrame(context);
    Check(context.window_open == 1 && p.glass == 5 && p.settings_text == before_collapse_text,
          "collapsing settings was treated as closing the overlay");
    p.collapsed = false;
    auto legacy_ui = p.ui;
    legacy_ui.struct_size = static_cast<uint32_t>(offsetof(AnomalyUiServiceV1, overlay_viewport_size));
    p.DrawFrame(context, &legacy_ui);
    Check(p.glass == 5 && p.settings_text > before_collapse_text, "missing overlay fields broke the settings window");
    Check(context.settings == original_settings && !context.save_pending && p.writes == 0 && p.reads == 1,
          "closing/reopening settings changed user preferences or performed render I/O");
    Check(p.localization_queries == 1, "Draw re-queried the localization service");

    anomaly::sdk::Localizer english;
    Texts fallback;
    BuildTexts(fallback, english);
    Check(fallback.window_title == "Status Bar###status-bar" && context.texts.window_title.ends_with("###status-bar"),
          "English fallback changed the window identity");
    Check(context.texts.toggles[kFps].ends_with("###fps") && fallback.positions[0].ends_with("###position-0"),
          "widget IDs depend on translated text");
    Check(FrameText(english, 60, 40, true, true) == "60 FPS  40 (1% low)" &&
          FrameText(english, 60, -1, true, true) == "60 FPS" &&
          FrameText(english, -1, -1, false, true) == "-- (1% low)",
          "frame text lost its English fallback, warmup behavior or unavailable reading");
    const auto parsed = ParseSettings("position=0\nspacing=201\nunknown=1\nfps=0\n");
    Check(parsed.position == Position::TopLeft && parsed.spacing == 100 && !parsed.shown[kFps] &&
          ParseSettings(FormatSettings(original_settings)) == original_settings,
          "settings defaults or format compatibility changed");
    Check(Succeeded(descriptor.on_stop(loaded, 1000)) && p.writes == 0, "stop changed settings without a user edit");
    descriptor.on_unload(loaded);
    std::cout << "Settings close/reopen, persistent overlay, localization and legacy settings passed\n";
}
