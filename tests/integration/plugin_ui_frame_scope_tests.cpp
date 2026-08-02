#include "plugin_manager.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

constexpr std::string_view kPluginId = "anomaly.fixture.ui-frame-scope";

struct FrameStateProbe {
    std::uint32_t calls{};
    std::uint32_t text_calls{};
    std::uint32_t double_input_calls{};
    double double_input_value{};
    double double_input_step{};
    double double_input_step_fast{};
    std::uint32_t developer_mode_calls{};
    int developer_mode_enabled{};
};

std::uint32_t ANOMALY_CALL FrameState(void* user) {
    auto* probe = static_cast<FrameStateProbe*>(user);
    if (probe != nullptr) ++probe->calls;
    return ANOMALY_UI_FRAME_V1_ITEM_HOVERED;
}

void ANOMALY_CALL Text(void* user, AnomalyStringViewV1) {
    auto* probe = static_cast<FrameStateProbe*>(user);
    if (probe != nullptr) ++probe->text_calls;
}

int ANOMALY_CALL InputDouble(
    void* user, AnomalyStringViewV1, double* value, double step, double step_fast) {
    auto* probe = static_cast<FrameStateProbe*>(user);
    if (probe != nullptr) {
        ++probe->double_input_calls;
        probe->double_input_value = value == nullptr ? 0.0 : *value;
        probe->double_input_step = step;
        probe->double_input_step_fast = step_fast;
    }
    return value == nullptr ? 0 : 1;
}

int ANOMALY_CALL DeveloperModeEnabled(void* user) {
    auto* probe = static_cast<FrameStateProbe*>(user);
    if (probe == nullptr) return 0;
    ++probe->developer_mode_calls;
    return probe->developer_mode_enabled;
}

bool Expect(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool WritePackage(const std::filesystem::path& package, const std::filesystem::path& plugin) {
    std::error_code error;
    std::filesystem::create_directories(package, error);
    if (error || !std::filesystem::copy_file(
            plugin, package / L"plugin.dll",
            std::filesystem::copy_options::overwrite_existing, error) || error) {
        return false;
    }
    std::ofstream manifest(package / L"manifest.json", std::ios::binary | std::ios::trunc);
    manifest << R"({"schemaVersion":2,"id":"anomaly.fixture.ui-frame-scope",)"
                R"("name":"UI Frame Scope Fixture","author":"Anomaly","version":"1.0.0",)"
                R"("entry":"plugin.dll","api":{"major":1,"minMinor":0,"maxMinor":0},)"
                R"("games":["nte"],"builds":["nte-win64-*"],"loadPhase":"game-ready",)"
                R"("services":[{"id":"anomaly.ui","minVersion":1},)"
                R"({"id":"anomaly.window","minVersion":1},{"id":"anomaly.input","minVersion":1}],)"
                R"("capabilities":["ui","ui-window","input"]})";
    return static_cast<bool>(manifest);
}

void LoadFixture(ue5mem::PluginManager& manager) {
    manager.LoadAll();
    static_cast<void>(manager.SetEnabled(kPluginId, true));
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-ui-frame-scope-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const std::filesystem::path plugins = root / L"plugins";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (!WritePackage(plugins / L"FrameScope", argv[1])) return 3;

    bool result = true;
    {
        FrameStateProbe probe;
        probe.developer_mode_enabled = 1;
        AnomalyUiServiceV1 ui{};
        ui.struct_size = sizeof(ui);
        ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
        ui.user = &probe;
        ui.text = Text;
        ui.frame_state = FrameState;
        ui.input_double = InputDouble;
        ui.developer_mode_enabled = DeveloperModeEnabled;

        ue5mem::PluginManager manager(root, plugins);
        manager.SetUiService(reinterpret_cast<const AnomalyUiServiceV1*>(&ui));
        LoadFixture(manager);
        const auto loaded = manager.Plugins();
        const bool activated = loaded.size() == 1 && loaded.front().id == kPluginId &&
            loaded.front().state == "active";
        if (!activated) {
            for (const std::string& event : manager.Events()) {
                std::cerr << "frame-scope activation event: " << event << '\n';
            }
        }
        result = Expect(activated, "complete UI V1 fixture did not activate") && result;

        manager.GameUpdate(1.0 / 60.0);
        result = Expect(
                     probe.calls == 0 && probe.developer_mode_calls == 1 &&
                         probe.double_input_calls == 0 && probe.text_calls == 0,
                     "UI V1 update exposed draw-only state or hid the developer-mode gate") &&
            result;
        manager.Draw(nullptr);
        result = Expect(
                     probe.calls == 1 && probe.developer_mode_calls == 2 &&
                         probe.double_input_calls == 1 &&
                         probe.double_input_value == -10000000.125 &&
                         probe.double_input_step == 0.0 && probe.double_input_step_fast == 0.0 &&
                         probe.text_calls == 1,
                     "UI V1 draw callbacks or frame-local input capture were not forwarded") &&
            result;
        manager.PersistUiWindowState();
        std::ifstream state_input(root / L"state" / L"ui-window-state.json", std::ios::binary);
        const nlohmann::json state_document = nlohmann::json::parse(state_input, nullptr, false);
        result = Expect(
                     !state_document.is_discarded() && state_document.contains("windows") &&
                         state_document["windows"].is_array() &&
                         state_document["windows"].size() == 1 &&
                         state_document["windows"][0].value("width", 0.0F) == 320.0F &&
                         state_document["windows"][0].value("height", 0.0F) == 240.0F,
                     "UI V1 window dimensions were not persisted") &&
            result;
        const auto after_callbacks = manager.Plugins();
        result = Expect(
                     after_callbacks.size() == 1 && after_callbacks.front().draw_metrics.calls == 1 &&
                         after_callbacks.front().update_metrics.calls == 1 &&
                         after_callbacks.front().state == "active",
                     "UI V1 fixture callbacks were not both observed") &&
            result;
        manager.UnloadAll();
    }
    {
        FrameStateProbe probe;
        AnomalyUiServiceV1 ui{};
        ui.struct_size = offsetof(AnomalyUiServiceV1, developer_mode_enabled);
        ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
        ui.user = &probe;
        ui.text = Text;
        ui.frame_state = FrameState;
        ui.input_double = InputDouble;
        ui.developer_mode_enabled = DeveloperModeEnabled;

        ue5mem::PluginManager manager(root, plugins);
        manager.SetUiService(reinterpret_cast<const AnomalyUiServiceV1*>(&ui));
        LoadFixture(manager);
        const auto loaded = manager.Plugins();
        result = Expect(
                     loaded.size() == 1 && loaded.front().id == kPluginId &&
                         loaded.front().state != "active" && probe.calls == 0 &&
                         probe.double_input_calls == 0 && probe.developer_mode_calls == 0,
                     "undersized UI V1 table activated the fixture") &&
            result;
        manager.UnloadAll();
    }

    std::filesystem::remove_all(root, cleanup_error);
    return result ? 0 : 1;
}
