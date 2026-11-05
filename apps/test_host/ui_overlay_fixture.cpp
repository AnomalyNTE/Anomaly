#include "plugin_manager.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
constexpr auto kPluginId = "b1ank.status-bar";

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Probe final {
    bool close{}, collapse{};
    unsigned begins{}, ends{}, glass{};
    AnomalyUiServiceV1 ui{};

    Probe() {
        ui.struct_size = sizeof(ui);
        ui.service_version = 1;
        ui.user = this;
        ui.begin_window = [](void* user, AnomalyStringViewV1, int* open, uint32_t) {
            auto& p = *static_cast<Probe*>(user);
            Require(open && *open, "host did not receive a live/reopened window flag");
            ++p.begins;
            if (p.close) { *open = 0; p.close = false; return 0; }
            return p.collapse ? 0 : 1;
        };
        ui.end_window = [](void* user) { ++static_cast<Probe*>(user)->ends; };
        ui.text = [](void*, AnomalyStringViewV1) {};
        ui.separator = [](void*) {};
        ui.same_line = [](void*, float, float) {};
        ui.set_cursor_pos_x = [](void*, float) {};
        ui.text_link = [](void*, AnomalyStringViewV1, AnomalyStringViewV1) { return 0; };
        ui.button_enabled = [](void*, AnomalyStringViewV1, float, float, int) { return 0; };
        ui.checkbox = [](void*, AnomalyStringViewV1, int*) { return 0; };
        ui.slider_float = [](void*, AnomalyStringViewV1, float*, float, float) { return 0; };
        ui.begin_tab_bar = [](void*, AnomalyStringViewV1, uint32_t) { return 0; };
        ui.begin_tab_item = [](void*, AnomalyStringViewV1, int*, uint32_t, int) { return 0; };
        ui.end_tab_item = [](void*) {};
        ui.end_tab_bar = [](void*) {};
        ui.set_next_item_width = [](void*, float) {};
        ui.overlay_viewport_size = [](void*, float* width, float* height) { *width = 1920; *height = 1080; };
        ui.overlay_glass = [](void* user, float, float, float, float, const AnomalyUiGlassStyleV1*) {
            ++static_cast<Probe*>(user)->glass; return 1;
        };
        ui.overlay_rect_filled = [](void*, float, float, float, float, uint32_t, float) {};
        ui.overlay_line = [](void*, float, float, float, float, uint32_t, float) {};
        ui.overlay_circle = [](void*, float, float, float, uint32_t, float) {};
        ui.overlay_ring_sector = [](void*, float, float, float, float, float, float, uint32_t) {};
        ui.overlay_measure_text = [](void*, AnomalyStringViewV1 value, float, float* width, float* height) {
            *width = static_cast<float>(value.size) * 8; *height = 16;
        };
        ui.overlay_text = [](void*, float, float, AnomalyStringViewV1, uint32_t, float) {};
    }
};

ue5mem::PluginView View(const ue5mem::PluginManager& manager) {
    for (const auto& plugin : manager.Plugins()) if (plugin.id == kPluginId) return plugin;
    throw std::runtime_error("status bar was not discovered");
}

void Run(const std::filesystem::path& root, bool legacy) {
    Probe probe;
    if (legacy) {
        // An incomplete optional tail must be hidden from the consumer. The
        // proxy should advertise only the published pre-overlay V1 prefix,
        // even when some later native function pointers are non-null.
        probe.ui.overlay_text = nullptr;
    }
    const auto dispatch = [](std::string, uint64_t, std::function<void()> callback) {
        callback(); return true;
    };
    ue5mem::PluginManager manager(root, root, {}, {}, {}, dispatch, dispatch);
    manager.SetUiService(&probe.ui);
    manager.LoadAll();
    if (!View(manager).enabled) Require(manager.SetEnabled(kPluginId, true), "could not enable status bar");
    Require(manager.SetVisible(kPluginId, true), "could not open settings");
    manager.GameUpdate(1.0 / 60.0);
    manager.Draw(nullptr);
    if (probe.begins != 1 || probe.ends != 1) {
        const auto current = View(manager);
        std::cerr << "begins=" << probe.begins << " ends=" << probe.ends << " glass=" << probe.glass
                  << " state=" << current.state << " reason=" << current.status_reason << '\n';
    }
    Require(probe.begins == 1 && probe.ends == 1, "initial settings scope was not balanced");
    Require(probe.glass == (legacy ? 0u : 1u), "overlay publication did not respect the host table size");

    probe.close = true;
    manager.Draw(nullptr);
    Require(!View(manager).visible, "the real UI proxy did not apply the close request");
    const auto closed_begins = probe.begins;
    const auto closed_glass = probe.glass;
    manager.Draw(nullptr);
    Require(probe.begins == closed_begins && probe.ends == probe.begins,
            "hidden overlay callback acquired a real window scope");
    Require(probe.glass == closed_glass + (legacy ? 0u : 1u), "the hidden plugin lost its overlay callback");

    Require(manager.SetVisible(kPluginId, true), "could not request reopening");
    manager.Draw(nullptr);
    Require(View(manager).visible && probe.begins == closed_begins + 1,
            "the real UI proxy did not reopen the plugin's close flag");
    probe.collapse = true;
    manager.Draw(nullptr);
    Require(View(manager).visible && probe.begins == probe.ends, "collapsed settings lost visibility or unbalanced the stack");
    Require(manager.SetEnabled(kPluginId, false), "could not disable status bar");
    const auto stopped_glass = probe.glass;
    manager.Draw(nullptr);
    Require(!View(manager).enabled && probe.glass == stopped_glass, "disabled plugin continued drawing");
    manager.UnloadAll();
    Require(manager.UiResources().ResourceLeaseCount() == 0 && manager.UiResources().StagingBytesInUse() == 0,
            "UI resource leases or staging survived unload");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto temporary = std::filesystem::temp_directory_path() /
        ("anomaly-overlay-fixture-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    try {
        Require(std::filesystem::create_directory(temporary), "could not create owned fixture directory");
        std::filesystem::copy(argv[1], temporary / "StatusBar", std::filesystem::copy_options::recursive);
        Run(temporary, false);
        Run(temporary, true);
        std::filesystem::remove_all(temporary, error);
        std::cout << "ok real_host_overlay close reopen collapse disable legacy_prefix\n";
        return 0;
    } catch (const std::exception& exception) {
        std::filesystem::remove_all(temporary, error);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
