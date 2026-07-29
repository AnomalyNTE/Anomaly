#include "anomaly/i18n.hpp"

#include "config.hpp"
#include "platform_host.hpp"
#include "plugin_manager.hpp"
#include "anomaly/platform_settings.hpp"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace {

std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto root = ExecutableDirectory() / L"Anomaly";
    auto config = ue5mem::AnalyzerConfig::Load(root / L"anomaly.ini");
    const auto locale = anomaly::ResolveUserLocale(config.platform_language);
    ue5mem::PlatformDiagnostics diagnostics;
    diagnostics.translator = anomaly::LoadHostCatalog(
        locale.locale, root / L"locales" / L"host").translator;
    config.platform_enabled = true;
    config.platform_visible = true;
    config.platform_embedded = false;
    config.platform_attach_to_process_window = false;
    auto plugins = std::make_shared<ue5mem::PluginManager>(root, config.plugin_directory);
    plugins->SetTranslator(diagnostics.translator);
    plugins->LoadAll();
    std::filesystem::create_directories(root / L"config");
    auto settings = std::make_shared<anomaly::PlatformSettingsStore>(root);
    static_cast<void>(settings->Start());
    diagnostics.settings_snapshot = [settings] { return settings->Snapshot(); };
    diagnostics.settings_apply = [settings](const anomaly::PlatformSettingsApplyRequest& request) {
        return settings->Apply(request);
    };
    diagnostics.settings_record_route = [settings](const std::string_view route) {
        return settings->RecordLastRoute(route);
    };
    ue5mem::RunPlatform(root, config, {}, {}, {}, std::move(diagnostics), plugins);
    static_cast<void>(plugins->StopForRuntime());
    return 0;
}
