#include "anomaly/i18n.hpp"

#include "config.hpp"
#include "platform_host.hpp"
#include "plugin_manager.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/platform_settings.hpp"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <objbase.h>
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
    if (start == std::wstring::npos) return "naiwa";
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
    return result.empty() ? "naiwa" : result;
}

std::array<float, 4> LoadBackground(const std::filesystem::path& configuration) {
    const UINT rgb = GetPrivateProfileIntW(
        L"Preview", L"BackgroundColor", 0x00FF00, configuration.c_str());
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f};
}

void SaveBackground(
    const std::filesystem::path& configuration, const std::array<float, 4>& color) {
    const auto channel = [](const float value) {
        return static_cast<UINT>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const UINT rgb = (channel(color[0]) << 16) | (channel(color[1]) << 8) | channel(color[2]);
    const std::wstring value = std::to_wstring(rgb);
    static_cast<void>(WritePrivateProfileStringW(
        L"Preview", L"BackgroundColor", value.c_str(), configuration.c_str()));
}

std::wstring MenuText(const anomaly::Translator& translator, anomaly::MessageId id) {
    const auto text = translator.Text(id);
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

class PreviewControls {
public:
    enum Command : UINT { Green = 1001, White, Custom, AlwaysOnTop };

    std::array<float, 4> background;
    bool always_on_top{};

    void Attach(HWND window, const anomaly::Translator& translator) {
        const HMENU menu = CreateMenu();
        background_menu_ = CreatePopupMenu();
        window_menu_ = CreatePopupMenu();
        const auto append = [&](HMENU parent, UINT flags, UINT_PTR command,
                                anomaly::MessageId id) {
            const auto text = MenuText(translator, id);
            AppendMenuW(parent, flags, command, text.c_str());
        };
        append(background_menu_, MF_STRING, Green, anomaly::MessageId::PreviewBackgroundGreen);
        append(background_menu_, MF_STRING, White, anomaly::MessageId::PreviewBackgroundWhite);
        append(background_menu_, MF_STRING, Custom, anomaly::MessageId::PreviewBackgroundCustom);
        append(window_menu_, MF_STRING, AlwaysOnTop, anomaly::MessageId::PreviewAlwaysOnTop);
        append(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(background_menu_),
            anomaly::MessageId::PreviewBackgroundTitle);
        append(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(window_menu_),
            anomaly::MessageId::PreviewWindowMenu);
        // The native window owns the menu and destroys it with the window.
        SetMenu(window, menu);
        RefreshMenu();
        ApplyTopmost(window);
        DrawMenuBar(window);
    }

    void HandleCommand(HWND window, UINT command) {
        switch (command) {
        case Green:
            background = {0.0f, 1.0f, 0.0f, 1.0f};
            break;
        case White:
            background = {1.0f, 1.0f, 1.0f, 1.0f};
            break;
        case Custom: {
            CHOOSECOLORW picker{sizeof(CHOOSECOLORW)};
            picker.hwndOwner = window;
            picker.rgbResult = RGB(Channel(background[0]), Channel(background[1]),
                Channel(background[2]));
            picker.lpCustColors = custom_colors_.data();
            picker.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&picker)) {
                background = {GetRValue(picker.rgbResult) / 255.0f,
                    GetGValue(picker.rgbResult) / 255.0f,
                    GetBValue(picker.rgbResult) / 255.0f, 1.0f};
            }
            break;
        }
        case AlwaysOnTop:
            always_on_top = !always_on_top;
            ApplyTopmost(window);
            break;
        default:
            return;
        }
        RefreshMenu();
    }

private:
    static BYTE Channel(float value) {
        return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    void ApplyTopmost(HWND window) const {
        SetWindowPos(window, always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void RefreshMenu() const {
        const UINT selected = background == std::array<float, 4>{0, 1, 0, 1} ? Green
            : background == std::array<float, 4>{1, 1, 1, 1} ? White : Custom;
        CheckMenuRadioItem(background_menu_, Green, Custom, selected, MF_BYCOMMAND);
        CheckMenuItem(window_menu_, AlwaysOnTop,
            MF_BYCOMMAND | (always_on_top ? MF_CHECKED : MF_UNCHECKED));
    }

    HMENU background_menu_{};
    HMENU window_menu_{};
    std::array<COLORREF, 16> custom_colors_{};
};

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // The launcher and the injected host both own a single-threaded apartment on
    // the thread that drives the platform UI, and the shell file dialog only
    // builds its view there. Entering it first keeps this fixture faithful to
    // the hosts it stands in for.
    const HRESULT com =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    static_cast<void>(com);
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
    diagnostics.settings_record_notice = [settings](const std::uint32_t version) {
        return settings->RecordNoticeAcceptance(version);
    };
    const auto configuration = root / L"anomaly.ini";
    auto controls = std::make_shared<PreviewControls>();
    controls->background = LoadBackground(configuration);
    controls->always_on_top = GetPrivateProfileIntW(
        L"Preview", L"AlwaysOnTop", 0, configuration.c_str()) != 0;
    const auto translator = diagnostics.translator;
    ue5mem::PlatformWindowAppearance appearance;
    appearance.clear_color = controls->background;
    appearance.show_in_taskbar = true;
    appearance.close_exits = true;
    appearance.setup_window = [controls, translator](void* window) {
        controls->Attach(static_cast<HWND>(window), *translator);
    };
    appearance.menu_command = [controls](void* window, std::uint32_t command) {
        controls->HandleCommand(static_cast<HWND>(window), command);
    };
    appearance.background_color = [controls] { return controls->background; };
    ue5mem::RunPlatform(root, config, {}, {}, {}, std::move(diagnostics), plugins,
        std::move(appearance));
    SaveBackground(configuration, controls->background);
    static_cast<void>(WritePrivateProfileStringW(L"Preview", L"AlwaysOnTop",
        controls->always_on_top ? L"1" : L"0", configuration.c_str()));
    static_cast<void>(plugins->StopForRuntime());
    return 0;
}
