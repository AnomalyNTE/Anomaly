#include "anomaly/i18n.hpp"

#include "config.hpp"
#include "platform_host.hpp"
#include "plugin_manager.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/platform_settings.hpp"

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace {

std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::string PreviewPaletteArgument() {
    const std::wstring command_line = GetCommandLineW();
    constexpr std::wstring_view prefix = L"--palette=";
    const std::size_t start = command_line.find(prefix);
    if (start == std::wstring::npos) return "anomalyhub";
    const std::size_t value_start = start + prefix.size();
    const std::size_t value_end = command_line.find_first_of(L" \t\r\n", value_start);
    const std::wstring value = command_line.substr(
        value_start, value_end == std::wstring::npos ? std::wstring::npos : value_end - value_start);
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character >= L'A' && character <= L'Z') {
            result.push_back(static_cast<char>(character - L'A' + L'a'));
        } else if (character >= L'a' && character <= L'z') {
            result.push_back(static_cast<char>(character));
        }
    }
    return result.empty() ? "anomalyhub" : result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto palette = ue5mem::ParsePlatformUiPalette(PreviewPaletteArgument());
    ue5mem::SetPlatformUiPalette(palette);
    const auto root = ExecutableDirectory() / L"Anomaly";
    auto config = ue5mem::AnalyzerConfig::Load(root / L"anomaly.ini");
    const auto locale = anomaly::ResolveUserLocale(config.platform_language);
    ue5mem::PlatformDiagnostics diagnostics;
    diagnostics.runtime_root = root;
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
