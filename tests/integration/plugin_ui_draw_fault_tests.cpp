#include "plugin_manager.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kPluginId = "anomaly.fixture.ui-draw-fault";

enum class HostStackEntry : std::uint8_t {
    Window,
    Child,
    Table,
    Menu,
    Popup,
};

struct HostUiProbe {
    std::vector<HostStackEntry> stack;
    std::uint32_t unsafe_end_calls{};
    std::uint32_t child_end_calls{};
    std::uint32_t table_begin_calls{};
    std::uint32_t table_end_calls{};
    std::uint32_t menu_end_calls{};
    std::uint32_t popup_end_calls{};
    std::uint32_t scoped_window_begin_calls{};
    bool close_next_scoped_window{};
    bool child_returns_false{};
    bool table_returns_false{};
    bool menu_returns_false{};
    bool popup_returns_false{};

    void Begin(const HostStackEntry entry) { stack.push_back(entry); }

    void BeginWindow(const AnomalyStringViewV1 title, int* open) {
        Begin(HostStackEntry::Window);
        const std::string_view label(title.data, title.size);
        if (label.find("Scoped Window") != std::string_view::npos) {
            ++scoped_window_begin_calls;
        }
        if (close_next_scoped_window && open != nullptr &&
            label.find("Scoped Window") != std::string_view::npos) {
            *open = 0;
            close_next_scoped_window = false;
        }
    }

    void End(const HostStackEntry entry) {
        if (stack.empty() || stack.back() != entry) {
            ++unsafe_end_calls;
            return;
        }
        stack.pop_back();
        switch (entry) {
        case HostStackEntry::Child:
            ++child_end_calls;
            break;
        case HostStackEntry::Table:
            ++table_end_calls;
            break;
        case HostStackEntry::Menu:
            ++menu_end_calls;
            break;
        case HostStackEntry::Popup:
            ++popup_end_calls;
            break;
        case HostStackEntry::Window:
            break;
        }
    }
};

class TestUiResourceRenderBackend final : public anomaly::UiResourceRenderBackend {
public:
    std::uint32_t font_pushes{};
    std::uint32_t font_pops{};

    bool PushFont(
        anomaly::UiResourceRegistry& registry, const std::shared_ptr<anomaly::PluginScope>& scope,
        const anomaly::UiResourceHandle handle) noexcept override {
        const auto state = registry.ResourceState(scope, handle);
        if (!state || state->state != anomaly::UiResourceState::Ready) return false;
        ++font_pushes;
        return true;
    }

    bool PopFont() noexcept override {
        ++font_pops;
        return true;
    }

    bool DrawTexture(
        anomaly::UiResourceRegistry&, const std::shared_ptr<anomaly::PluginScope>&,
        anomaly::UiResourceHandle, float, float, std::uint32_t) noexcept override {
        return false;
    }

    void PrepareFont(
        anomaly::UiResourceRegistry& registry, const std::shared_ptr<anomaly::PluginScope>& scope,
        const anomaly::UiResourceHandle handle) noexcept override {
        static_cast<void>(registry.MarkFontReady(scope, handle, registry.DeviceGeneration()));
    }

    void PrepareTexture(
        anomaly::UiResourceRegistry&, const std::shared_ptr<anomaly::PluginScope>&,
        anomaly::UiResourceHandle) noexcept override {}
    void CollectGarbage(anomaly::UiResourceRegistry&) noexcept override {}
    void OnDeviceLost() noexcept override {}
    bool OnDeviceRebuilt(std::uint64_t) noexcept override { return true; }
};

void ANOMALY_CALL SetNextWindowSize(void*, float, float, std::uint32_t) {}
void ANOMALY_CALL SetNextWindowSizeConstraints(void*, float, float, float, float) {}
void ANOMALY_CALL GetWindowSize(void*, float* width, float* height) {
    if (width != nullptr) *width = 320.0F;
    if (height != nullptr) *height = 240.0F;
}

int ANOMALY_CALL BeginWindow(
    void* user, const AnomalyStringViewV1 title, int* open, std::uint32_t) {
    static_cast<HostUiProbe*>(user)->BeginWindow(title, open);
    return 1;
}

void ANOMALY_CALL EndWindow(void* user) {
    static_cast<HostUiProbe*>(user)->End(HostStackEntry::Window);
}

int ANOMALY_CALL BeginChild(
    void* user, AnomalyStringViewV1, float, float, std::uint32_t) {
    auto* probe = static_cast<HostUiProbe*>(user);
    probe->Begin(HostStackEntry::Child);
    return probe->child_returns_false ? 0 : 1;
}

void ANOMALY_CALL EndChild(void* user) {
    static_cast<HostUiProbe*>(user)->End(HostStackEntry::Child);
}

int ANOMALY_CALL BeginTable(
    void* user, AnomalyStringViewV1, std::int32_t, std::uint32_t, float, float) {
    auto* probe = static_cast<HostUiProbe*>(user);
    ++probe->table_begin_calls;
    if (!probe->table_returns_false) probe->Begin(HostStackEntry::Table);
    return probe->table_returns_false ? 0 : 1;
}

void ANOMALY_CALL EndTable(void* user) {
    static_cast<HostUiProbe*>(user)->End(HostStackEntry::Table);
}

int ANOMALY_CALL BeginMenu(void* user, AnomalyStringViewV1, int) {
    auto* probe = static_cast<HostUiProbe*>(user);
    if (!probe->menu_returns_false) probe->Begin(HostStackEntry::Menu);
    return probe->menu_returns_false ? 0 : 1;
}

void ANOMALY_CALL EndMenu(void* user) {
    static_cast<HostUiProbe*>(user)->End(HostStackEntry::Menu);
}

void ANOMALY_CALL OpenPopup(void*, AnomalyStringViewV1) {}

int ANOMALY_CALL BeginPopup(void* user, AnomalyStringViewV1, int*, std::uint32_t) {
    auto* probe = static_cast<HostUiProbe*>(user);
    if (!probe->popup_returns_false) probe->Begin(HostStackEntry::Popup);
    return probe->popup_returns_false ? 0 : 1;
}

void ANOMALY_CALL EndPopup(void* user) {
    static_cast<HostUiProbe*>(user)->End(HostStackEntry::Popup);
}

AnomalyUiServiceV1 MakeUiService(HostUiProbe* probe) {
    AnomalyUiServiceV1 ui{};
    ui.struct_size = sizeof(ui);
    ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
    ui.user = probe;
    ui.set_next_window_size = SetNextWindowSize;
    ui.begin_window = BeginWindow;
    ui.end_window = EndWindow;
    ui.begin_child = BeginChild;
    ui.end_child = EndChild;
    ui.begin_table = BeginTable;
    ui.end_table = EndTable;
    ui.begin_menu = BeginMenu;
    ui.end_menu = EndMenu;
    ui.open_popup = OpenPopup;
    ui.begin_popup_modal = BeginPopup;
    ui.end_popup = EndPopup;
    ui.set_next_window_size_constraints = SetNextWindowSizeConstraints;
    ui.get_window_size = GetWindowSize;
    return ui;
}

struct EvidenceCopy {
    ue5mem::PluginCallbackEvidenceKind kind{};
    std::string plugin_id;
    std::uint64_t generation{};
    std::uint32_t thread_id{};
    double duration_micros{};
    bool fault{};
};

bool Expect(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
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
    std::filesystem::create_directories(package / L"assets", error);
    if (error) return false;
    std::ofstream font(package / L"assets" / L"test-font.bin", std::ios::binary | std::ios::trunc);
    const std::array<char, 4> font_bytes{{'t', 'e', 's', 't'}};
    font.write(font_bytes.data(), static_cast<std::streamsize>(font_bytes.size()));
    if (!font) return false;
    std::ofstream manifest(package / L"manifest.json", std::ios::binary | std::ios::trunc);
    manifest << R"({"schemaVersion":2,"id":"anomaly.fixture.ui-draw-fault",)"
                R"("name":"UI Draw Fault Fixture","author":"Anomaly","version":"1.0.0",)"
                R"("entry":"plugin.dll","api":{"major":1,"minMinor":0,"maxMinor":0},)"
                R"("games":["nte"],"builds":["nte-win64-*"],"loadPhase":"game-ready",)"
                R"("services":[{"id":"anomaly.ui","minVersion":1},)"
                R"({"id":"anomaly.window","minVersion":1},)"
                R"({"id":"anomaly.font","minVersion":1}],)"
                R"("capabilities":["ui","ui-window","ui-font"]})";
    return static_cast<bool>(manifest);
}

std::size_t StackFaultLogCount(const std::vector<std::string>& events) {
    return static_cast<std::size_t>(std::count_if(
        events.begin(), events.end(), [](const std::string& event) {
            return event.find("unbalanced UI stack: anomaly.fixture.ui-draw-fault") !=
                std::string::npos;
        }));
}

bool VerifyMissingTableEndGuard(
    const std::filesystem::path& root, const std::filesystem::path& plugins) {
    HostUiProbe probe;
    AnomalyUiServiceV1 ui = MakeUiService(&probe);
    ui.end_table = nullptr;
    const auto worker_dispatch = [](
                                     std::string, std::uint64_t,
                                     std::function<void()> callback) {
        callback();
        return true;
    };
    ue5mem::PluginManager manager(root, plugins, {}, {}, {}, {}, worker_dispatch);
    manager.SetUiResourceRenderBackend(std::make_shared<TestUiResourceRenderBackend>());
    manager.SetUiService(reinterpret_cast<const AnomalyUiServiceV1*>(&ui));
    manager.LoadAll();
    static_cast<void>(manager.SetEnabled(kPluginId, true));
    manager.PrepareUiResources();

    bool result = true;
    auto views = manager.Plugins();
    result = Expect(
                 views.size() == 1 && views.front().state == "active",
                 "missing-end fixture did not activate") &&
        result;
    manager.Draw(nullptr);
    manager.Draw(nullptr);
    views = manager.Plugins();
    result = Expect(
                 views.size() == 1 && views.front().state == "faulted" &&
                     probe.table_begin_calls == 0 && probe.table_end_calls == 0 &&
                     probe.stack.empty() && probe.unsafe_end_calls == 0,
                 "proxy reached the host for a table without a matching end callback") &&
        result;
    manager.UnloadAll();
    return result;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-ui-draw-fault-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const std::filesystem::path plugins = root / L"plugins";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (!WritePackage(plugins / L"DrawFault", argv[1])) return 3;

    bool result = true;
    {
        HostUiProbe probe;
        AnomalyUiServiceV1 ui = MakeUiService(&probe);
        ue5mem::PluginCallbackBudgets budgets;
        budgets.draw_slow_milliseconds = 1.0;
        const auto worker_dispatch = [](
                                         std::string, std::uint64_t,
                                         std::function<void()> callback) {
            callback();
            return true;
        };
        ue5mem::PluginManager manager(root, plugins, {}, budgets, {}, {}, worker_dispatch);
        const auto resource_backend = std::make_shared<TestUiResourceRenderBackend>();
        manager.SetUiResourceRenderBackend(resource_backend);
        std::vector<EvidenceCopy> evidence;
        manager.SetCallbackEvidenceObserver([&](const ue5mem::PluginCallbackEvidence& item) {
            evidence.push_back({
                item.kind, std::string(item.plugin_id), item.generation, item.thread_id,
                item.duration_micros, item.fault});
        });
        manager.SetUiService(reinterpret_cast<const AnomalyUiServiceV1*>(&ui));
        manager.LoadAll();
        static_cast<void>(manager.SetEnabled(kPluginId, true));
        manager.PrepareUiResources();

        auto views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().id == kPluginId &&
                         views.front().state == "active",
                     "draw-fault fixture did not activate") &&
            result;
        const std::uint64_t first_generation = views.empty() ? 0 : views.front().generation;

        manager.Draw(nullptr);
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "active" &&
                         views.front().draw_metrics.calls == 1 &&
                          views.front().draw_metrics.faults == 0 &&
                          views.front().draw_metrics.slow_calls == 0,
                     "scoped window did not complete a balanced normal draw") &&
            result;
        result = Expect(
                     probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "normal scoped window draw did not preserve host stack pairing") &&
            result;
        result = Expect(
                     resource_backend->font_pushes == 1 && resource_backend->font_pops == 1,
                     "normal draw did not balance the scoped font stack") &&
            result;
        result = Expect(
                     evidence.size() == 1 &&
                         evidence.front().kind == ue5mem::PluginCallbackEvidenceKind::Draw &&
                         evidence.front().plugin_id == kPluginId &&
                         evidence.front().generation == first_generation &&
                         evidence.front().thread_id == GetCurrentThreadId() && !evidence.front().fault,
                     "normal draw evidence did not preserve generation and thread") &&
            result;

        manager.Draw(nullptr);
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "faulted" &&
                         views.front().draw_metrics.calls == 2 &&
                         views.front().draw_metrics.faults == 1 &&
                         views.front().draw_metrics.slow_calls == 1,
                     "draw fault or slow callback metric was not recorded") &&
            result;
        result = Expect(
                     probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "UI recovery did not preserve strict host stack pairing") &&
            result;
        result = Expect(
                     resource_backend->font_pushes == 2 && resource_backend->font_pops == 2,
                     "UI recovery did not unwind the scoped font stack") &&
            result;
        result = Expect(
                     evidence.size() == 2 && evidence.back().kind == ue5mem::PluginCallbackEvidenceKind::Draw &&
                         evidence.back().plugin_id == kPluginId &&
                         evidence.back().generation == first_generation &&
                         evidence.back().thread_id == GetCurrentThreadId() &&
                         evidence.back().duration_micros >= 1000.0 && evidence.back().fault,
                     "faulting draw evidence did not preserve generation, thread, duration, and fault") &&
            result;
        const std::size_t first_log_count = StackFaultLogCount(manager.Events());
        result = Expect(first_log_count == 1, "draw stack fault did not emit exactly one event") && result;

        manager.Draw(nullptr);
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().draw_metrics.calls == 2 &&
                         evidence.size() == 2 && StackFaultLogCount(manager.Events()) == first_log_count &&
                         probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "faulted draw callback was retried or contaminated the host stack") &&
            result;

        result = Expect(manager.Reload(kPluginId), "explicit reload of faulted plugin failed") && result;
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "active" &&
                         views.front().generation > first_generation,
                     "explicit reload did not create an active new generation") &&
            result;
        const std::uint64_t second_generation = views.empty() ? 0 : views.front().generation;

        probe.close_next_scoped_window = true;
        manager.PrepareUiResources();
        manager.Draw(nullptr);
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "active" &&
                         !views.front().visible && views.front().draw_metrics.calls == 1 && evidence.size() == 3 &&
                         !evidence.back().fault && evidence.back().generation == second_generation &&
                         probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "closing the last scoped window did not hide the plugin cleanly") &&
            result;
        result = Expect(
                     resource_backend->font_pushes == 3 && resource_backend->font_pops == 3,
                     "scoped window close did not retain a balanced font stack") &&
            result;

        const std::uint32_t scoped_begins_before_reopen = probe.scoped_window_begin_calls;
        result = Expect(
                     manager.SetVisible(kPluginId, true),
                     "plugin list could not reopen a closed managed-window group") &&
            result;
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().visible,
                     "managed-window reopen did not update plugin visibility") &&
            result;
        manager.Draw(nullptr);
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "active" &&
                         views.front().visible && views.front().draw_metrics.calls == 2 &&
                         evidence.size() == 4 && !evidence.back().fault &&
                         evidence.back().generation == second_generation &&
                         probe.scoped_window_begin_calls == scoped_begins_before_reopen + 1 &&
                         probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "reopened managed window did not complete the next draw") &&
            result;
        result = Expect(
                     resource_backend->font_pushes == 4 && resource_backend->font_pops == 4,
                     "reopened managed window did not retain a balanced font stack") &&
            result;

        result = Expect(manager.Reload(kPluginId), "explicit reload after scoped window close failed") && result;
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "active" &&
                         views.front().generation > second_generation,
                     "reload after scoped window close did not create a new active generation") &&
            result;
        const std::uint64_t third_generation = views.empty() ? 0 : views.front().generation;

        manager.PrepareUiResources();
        manager.Draw(nullptr);
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "active" &&
                         views.front().draw_metrics.calls == 1 && evidence.size() == 5 &&
                         !evidence.back().fault && evidence.back().generation == third_generation &&
                         probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "third generation did not complete a balanced normal scoped window draw") &&
            result;

        const std::uint32_t child_end_calls = probe.child_end_calls;
        const std::uint32_t table_end_calls = probe.table_end_calls;
        const std::uint32_t menu_end_calls = probe.menu_end_calls;
        const std::uint32_t popup_end_calls = probe.popup_end_calls;
        probe.child_returns_false = true;
        probe.table_returns_false = true;
        probe.menu_returns_false = true;
        probe.popup_returns_false = true;
        manager.Draw(nullptr);
        views = manager.Plugins();
        result = Expect(
                     views.size() == 1 && views.front().state == "faulted" &&
                         views.front().draw_metrics.calls == 2 && evidence.size() == 6 && evidence.back().fault &&
                         evidence.back().generation == third_generation &&
                         evidence.back().generation != first_generation &&
                         probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "reloaded generation did not fault and recover independently") &&
            result;
        result = Expect(
                     resource_backend->font_pushes == 6 && resource_backend->font_pops == 6 &&
                         probe.child_end_calls == child_end_calls + 1 &&
                         probe.table_end_calls == table_end_calls &&
                         probe.menu_end_calls == menu_end_calls &&
                         probe.popup_end_calls == popup_end_calls,
                     "false V2 begin results did not follow their required unwind contracts") &&
            result;

        manager.UnloadAll();
        result = Expect(
                     probe.stack.empty() && probe.unsafe_end_calls == 0,
                     "plugin unload left a host UI stack entry behind") &&
            result;
    }

    result = VerifyMissingTableEndGuard(root, plugins) && result;

    std::filesystem::remove_all(root, cleanup_error);
    return result ? 0 : 1;
}
