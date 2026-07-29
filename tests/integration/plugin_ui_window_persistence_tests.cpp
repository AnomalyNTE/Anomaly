#include "plugin_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

constexpr std::string_view kOwner = "anomaly.test.window-persistence";

bool Expect(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

anomaly::UiWindowRequest WindowRequest() {
    anomaly::UiWindowRequest request;
    request.id = "settings";
    request.title = "Persisted settings";
    request.initial_width = 480.0F;
    request.initial_height = 360.0F;
    request.constraints = {320.0F, 240.0F, 1200.0F, 900.0F};
    request.default_open = true;
    return request;
}

bool RemovePreviousState(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::create_directories(root / L"state", error);
    if (error) return false;
    std::filesystem::remove(root / L"state" / L"ui-window-state.json", error);
    if (error) return false;
    return !error;
}

bool VerifyWindowStateRoundTrip(const std::filesystem::path& root) {
    if (!RemovePreviousState(root)) return false;
    const std::filesystem::path state_file = root / L"state" / L"ui-window-state.json";

    {
        ue5mem::PluginManager manager(root, L"plugins");
        const auto ledger = std::make_shared<anomaly::ResourceLedger>();
        const auto scope = std::make_shared<anomaly::PluginScope>(
            ledger, std::string(kOwner), 1);
        const auto window = manager.UiResources().RegisterWindow(scope, WindowRequest());
        if (!Expect(static_cast<bool>(window), "window registration failed") ||
            !Expect(manager.UiResources().CloseWindow(scope, window), "window close failed") ||
            !Expect(
                manager.UiResources().SetWindowConstraints(
                    scope, window, {400.0F, 300.0F, 1000.0F, 800.0F}),
                "window constraints update failed") ||
            !Expect(
                manager.UiResources().SetWindowSize(scope, window, 700.0F, 500.0F),
                "window size update failed")) {
            return false;
        }
        manager.Maintenance();
        if (!Expect(std::filesystem::exists(state_file), "window state file was not published") ||
            !Expect(scope->RevokeAll() == 1, "window lease was not revoked")) {
            return false;
        }
    }

    std::ifstream input(state_file, std::ios::binary);
    const nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
    if (!Expect(!document.is_discarded() && document.value("schemaVersion", 0U) == 1U &&
                    document.contains("windows") && document["windows"].is_array() &&
                    document["windows"].size() == 1,
                "persisted window state schema is invalid")) {
        return false;
    }
    input.close();

    {
        ue5mem::PluginManager manager(root, L"plugins");
        const auto ledger = std::make_shared<anomaly::ResourceLedger>();
        const auto scope = std::make_shared<anomaly::PluginScope>(
            ledger, std::string(kOwner), 2);
        auto request = WindowRequest();
        request.default_open = true;
        const auto window = manager.UiResources().RegisterWindow(scope, std::move(request));
        const auto state = manager.UiResources().WindowState(scope, window);
        if (!Expect(state && !state->open && state->width == 700.0F && state->height == 500.0F &&
                        state->constraints.minimum_width == 400.0F &&
                        state->constraints.maximum_height == 800.0F,
                "persisted window state was not restored") ||
            !Expect(
                manager.UiResources().SetWindowSize(scope, window, 800.0F, 600.0F),
                "split persistence window resize failed")) {
            return false;
        }
        manager.MaintenancePluginState();
        manager.PersistUiWindowState();
        const auto updated = manager.UiResources().WindowState(scope, window);
        if (!Expect(updated && updated->width == 800.0F && updated->height == 600.0F,
                "split persistence window state was not retained") ||
            !Expect(scope->RevokeAll() == 1, "restored window lease was not revoked")) {
            return false;
        }
    }
    input.open(state_file, std::ios::binary);
    const nlohmann::json updated_document = nlohmann::json::parse(input, nullptr, false);
    if (!Expect(
            !updated_document.is_discarded() && updated_document.contains("windows") &&
                updated_document["windows"].is_array() && updated_document["windows"].size() == 1 &&
                updated_document["windows"][0].value("width", 0.0F) == 800.0F &&
                updated_document["windows"][0].value("height", 0.0F) == 600.0F,
            "split persistence did not publish the updated window state")) {
        return false;
    }
    return true;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    return VerifyWindowStateRoundTrip(std::filesystem::path(argv[1])) ? 0 : 1;
}
