#include "anomaly/liquid_glass.hpp"
#include "anomaly/launcher/manual_map.hpp"
#include "anomaly/launcher/mod_archive.hpp"
#include "anomaly/launcher/mod_groups.hpp"
#include "anomaly/launcher/configuration.hpp"
#include "anomaly/launcher/proxy_installation.hpp"
#include "anomaly/launcher/release_update.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/platform_notice.hpp"
#include "anomaly/sdk/version.h"
#include "anomaly/platform_ui_layout.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/telemetry.hpp"
#include "anomaly/runtime_launch.hpp"
#include "anomaly/reliable_storage.hpp"
#include "anomaly/pak_mod_settings.hpp"
#include "anomaly/pak_mod_catalog.hpp"
#include "anomaly/pak_mod_layout.hpp"
#include "anomaly/mod_group_names.hpp"
#include "anomaly/plugin_package.hpp"
#include "anomaly/ui_resource_decoder.hpp"
#include "config.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <winhttp.h>
#include <winreg.h>
#include <TlHelp32.h>
#include <wincrypt.h>
#include <commdlg.h>
#include <d3d11.h>
// The window's content is a composition swap chain rather than a window-bound one,
// which is what gives its pixels an alpha channel. IDXGIFactory2 comes from the 1.2
// header, IDCompositionDevice from dcomp.
#include <dcomp.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kLogoResourceId = 101;
constexpr int kModPlaceholderResourceId = 102;
// The support buttons. They are pictures rather than drawn controls because they are the platforms'
// own artwork, and they are embedded rather than fetched so the buttons work on a machine that never
// reaches the networks those pages live on.
constexpr int kSponsorResourceId = 103;
constexpr int kKofiResourceId = 104;
constexpr int kIconResourceId = 201;
// Where the support buttons point. Data, not copy -- a translated address leads nowhere.
constexpr std::wstring_view kSponsorUrl{L"https://afdian.com/a/bananachicken"};
constexpr std::wstring_view kKofiUrl{L"https://ko-fi.com/bananachicken"};
// The width each support button is drawn at, in canvas pixels before the DPI scale. It is the width
// the platforms publish their artwork for.
constexpr float kSupportButtonWidth = 200.0f;
// Sized for the launcher's 17px font. These were laid out for 13px text, and a
// child that cannot fit its own contents grows a scrollbar instead of clipping,
// which is what a mode row of 48px did to a 30px button plus its padding.
// The launcher draws on a themed surface with a wallpaper and its veil behind it,
// where the palette's own muted ink is too pale to read on a light body. Every
// secondary label here uses this tone instead whenever the palette is one of the
// light ones, which is also what ImGuiCol_TextDisabled is set to, so disabled and
// secondary text stay consistent.
const ImVec4 kLauncherMutedInkLightSurface(0.24f, 0.21f, 0.15f, 1.0f);
constexpr float kHeaderHeight = 44.0f;
// The shell canvas. It is the whole window except for one transparent band above it, and nothing
// may be laid out past its edges: a sticker that overhangs is clipped at the canvas, which is why
// the band exists only where a sticker actually reaches.
constexpr float kLauncherCanvasWidth = 1280.0f;
constexpr float kLauncherCanvasHeight = 860.0f;
// The transparent band above the canvas, in canvas pixels, for the launcher's own sticker. The
// geometry comes from config/themes/<palette>.launcher.json, checked against what the runtime
// actually places (the layout drops its `height` and derives it from the artwork's aspect):
//   width  0.614291 * 1280 = 786.29
//   height 786.29 * 2722 / 1956 = 1094.15
//   centre 0.421821 * 860 = 362.77  ->  image top 362.77 - 547.08 = -184.31, i.e. 184.31 px above
//   the canvas; its first opaque row is 301 of 2722, 121.03 px below that edge, so the artwork
//   starts 63.28 px above the canvas.
// Six more pixels keep that top edge off the window border, where a cut sticker looks clipped.
constexpr float kLauncherBandTop = 69.3f;
constexpr float kModeHeight = 76.0f;
// The lower command row both launch sub-modes anchor their own buttons to: this far above the page's
// bottom edge, and this tall. The sponsor button shares that line, which is why the two numbers live
// here rather than in each of the three places that place something on it.
constexpr float kLaunchActionRowBottom = 82.0f;
constexpr float kLaunchActionRowHeight = 62.0f;
constexpr float kProxyActionLeftPadding = 4.0f;
// The band's "a newer release exists" chip is drawn on the modes row, which is a child window of the
// root, while the update dialog belongs to the root. The chip therefore leaves a request here and the
// dialog takes it: the answer to "do you want to update" is the same dialog the check opens, wherever
// the user asked for it from.
bool g_update_prompt_requested{};
constexpr float kLauncherFontScale = 20.0f / 13.0f;
constexpr float kDefaultDpi = 96.0f;

ImVec4 ThemeColor(const ue5mem::PlatformUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

ImVec4 ThemeColorWithAlpha(
    const ue5mem::PlatformUiColor& color, const float alpha) noexcept {
    return {color.red, color.green, color.blue, alpha};
}

// The launcher paints its own body from the palette's surface colour, under a wallpaper
// and its veil, and the palettes it can wear are not all light: Naiwa and Paper are cream
// and paper with dark cocoa ink, the other presets are dark with light ink. The surface
// therefore decides which side of that line every label, chip and button lands on, so a
// theme switch in the settings page cannot leave black text on a dark body.
bool LauncherSurfaceIsLight() noexcept {
    const auto surface = ue5mem::PlatformUiSurfaceColor();
    return 0.2126f * surface.red + 0.7152f * surface.green +
        0.0722f * surface.blue >= 0.5f;
}

ImVec4 LauncherInk() noexcept {
    // Plain black on a light surface: the palette's own ink is tuned for the shell's
    // panels, and those are empty while a wallpaper is showing.
    return LauncherSurfaceIsLight()
        ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
        : ThemeColor(ue5mem::PlatformUiTheme().text);
}

ImVec4 LauncherMutedInk() noexcept {
    return LauncherSurfaceIsLight()
        ? kLauncherMutedInkLightSurface
        : ThemeColor(ue5mem::PlatformUiTheme().text_muted);
}

enum class LauncherMode : std::uint8_t { Launch, Mods, Settings };
enum class MessageKind : std::uint8_t { Neutral, Success, Error };

// The state of the launcher's own update, as the settings page reports it. The phases are the
// steps the update actually takes, so the page can only ever show a step that happened.
enum class LauncherUpdatePhase : std::uint8_t {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Installing,
    Installed,
    Failed,
};

// What a failed update step was. The launcher names the ones it can explain -- an unreachable host,
// a rate-limited API -- and leaves the rest to the installer, whose diagnostic says what could not
// be written.
enum class LauncherUpdateFailure : std::uint8_t {
    None,
    Network,
    RateLimited,
    Http,
    NotRelease,
    Check,
    Install,
};

// What the last check found and, once it is asked for, how far the update got. Everything here is
// read from the snapshot; the phase is the only field the page branches on.
struct LauncherUpdateStatus final {
    LauncherUpdatePhase phase{LauncherUpdatePhase::Idle};
    bool auto_check{true};
    // The release the user answered "skip this version" to: the check keeps reporting it, but the
    // launcher does not ask about that one version again.
    std::string skipped_version;
    std::string remote_tag;
    std::string remote_version;
    std::string page_url;
    // The package the check chose: its address, its published length and its published digest, which
    // is what a download is verified against before anything is extracted.
    anomaly::launcher::ReleaseAsset asset;
    // The release notes the dialog shows before the user decides.
    std::string notes;
    LauncherUpdateFailure failure{LauncherUpdateFailure::None};
    // The HTTP status of a failed lookup, and the diagnostic a failed install reported.
    int http_status{};
    std::string detail;

    [[nodiscard]] bool Busy() const noexcept {
        return phase == LauncherUpdatePhase::Checking ||
            phase == LauncherUpdatePhase::Downloading ||
            phase == LauncherUpdatePhase::Installing;
    }

    [[nodiscard]] bool CanInstall() const noexcept {
        return !asset.download_url.empty() &&
            (phase == LauncherUpdatePhase::Available ||
             (phase == LauncherUpdatePhase::Failed && failure == LauncherUpdateFailure::Install));
    }
};

// One decoded theme image. The worker decodes and the render thread only
// uploads, so a large background never stalls on WIC during a frame.
struct LauncherThemeImage final {
    // Only the upload reads these; they are dropped as soon as the texture owns the
    // picture, which is why the field is mutable behind a shared const theme.
    mutable std::vector<std::uint8_t> pixels;
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool Ok() const noexcept {
        // Dimensions are set together with a successful decode, so this keeps saying
        // yes after the pixels are gone.
        return width != 0 && height != 0;
    }
};

// Everything the launcher needs to dress itself: the palette chosen in its own
// settings page, plus the background and stickers from config/themes.
struct LauncherTheme final {
    std::uint64_t version{};
    anomaly::PlatformUiPalette palette{anomaly::PlatformUiPalette::Naiwa};
    anomaly::PlatformUiLayoutDocument layout;
    LauncherThemeImage background;
    std::vector<LauncherThemeImage> stickers;

    [[nodiscard]] bool HasBackground() const noexcept {
        return background.Ok() &&
            layout.background.mode != anomaly::PlatformUiBackgroundMode::Disabled;
    }
};

struct LauncherMessage final {
    anomaly::MessageId id{anomaly::MessageId::LauncherStateReady};
    std::vector<std::string> arguments;
    std::string detail;
};

// Which mode a status message belongs to. The controller keeps a single message, so
// without this an attach outcome stayed on screen after switching to the proxy mode,
// where it described something that mode cannot do. Shared covers what neither mode
// owns: the initial scan, settings and hotkey writes.
enum class MessageScope { Both, Proxy, Attach };

// When a process rescan is allowed to write the status line: a background poll must not overwrite an
// outcome, but it does have to correct a status that has become untrue.
enum class ProcessAnnounce { Never, Always, OnChange };

struct LauncherSnapshot final {
    anomaly::launcher::NteClient selected_client{
        anomaly::launcher::NteClient::MainlandChina};
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
    anomaly::launcher::ProxyInstallationStatus proxy;
    std::vector<anomaly::launcher::AttachableProcess> processes;
    DWORD attached_process{};
    bool core_available{};
    std::string runtime_version;
    std::string runtime_message;
    std::uint32_t toggle_key{VK_INSERT};
    // The launcher's own language and page preferences, straight from its configuration.
    std::string language{"auto"};
    // The pictures and the custom colours this window was dressed with: the theme carries the
    // artwork, the colours are applied on top of it every frame so dragging a colour updates the
    // window without decoding a wallpaper again.
    anomaly::launcher::LauncherThemeArtwork artwork;
    anomaly::PlatformUiCustomColors custom_colors;
    anomaly::launcher::LauncherViewSettings view;
    bool busy{};
    LauncherMessage message;
    MessageKind message_kind{MessageKind::Neutral};
    MessageScope message_scope{MessageScope::Both};
    LauncherUpdateStatus update;
    // Set once the new image is in place and has been started: the window is done, because it is
    // drawn by the image that was replaced.
    bool restart_pending{};
    std::shared_ptr<const LauncherTheme> theme;
};

// The launcher wears whichever palette the settings page stored, and Naiwa -- the
// shipped default -- when nothing was chosen. It deliberately does not follow the
// palette picked in the in-game settings: the two windows are separate surfaces, and a
// launcher that changed colour with every launch would lose the identity it has as the
// first thing a user sees. The layout is resolved for the launcher rather than for the
// game: config/themes/<palette>.launcher.json when the theme ships one, which is how a
// sticker can sit correctly on a 1100x860 window and on the in-game shell at once.
// The fill mode travels through the configuration as an int, the way the in-game settings keep it, so
// it round-trips without a separate string enum. Anything unknown means "do not draw a wallpaper".
anomaly::PlatformUiBackgroundMode BackgroundModeFromInt(const int value) noexcept {
    switch (value) {
    case static_cast<int>(anomaly::PlatformUiBackgroundMode::Center):
        return anomaly::PlatformUiBackgroundMode::Center;
    case static_cast<int>(anomaly::PlatformUiBackgroundMode::Stretch):
        return anomaly::PlatformUiBackgroundMode::Stretch;
    case static_cast<int>(anomaly::PlatformUiBackgroundMode::Crop):
        return anomaly::PlatformUiBackgroundMode::Crop;
    default:
        return anomaly::PlatformUiBackgroundMode::Disabled;
    }
}

std::shared_ptr<const LauncherTheme> LoadLauncherTheme(
    const std::filesystem::path& runtime_root, const std::string_view palette_name,
    const anomaly::launcher::LauncherThemeArtwork& artwork, const std::uint64_t version) {
    auto theme = std::make_shared<LauncherTheme>();
    theme->version = version;
    theme->palette = anomaly::ParsePlatformUiPalette(palette_name);
    try {
        const auto read = anomaly::ReadUiResourceBytes(
            anomaly::ResolveLauncherUiLayoutFile(
                runtime_root, anomaly::ToString(theme->palette)),
            anomaly::kPlatformUiLayoutMaximumBytes);
        if (read) {
            const std::string text(
                reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
            auto parsed = anomaly::ParsePlatformUiLayout(text);
            if (parsed.ok) theme->layout = std::move(parsed.layout);
        }

        // The picture the launcher was pointed at replaces the one the layout names, with the fill
        // mode and the veil that sit beside it. A layout that ships its wallpaper turned off would
        // otherwise load a picture and never draw it, so an unpicked mode falls back to crop.
        if (!artwork.background.empty()) {
            theme->layout.background.path = artwork.background;
            if (theme->layout.background.mode == anomaly::PlatformUiBackgroundMode::Disabled &&
                artwork.background_mode < 0)
                theme->layout.background.mode = anomaly::PlatformUiBackgroundMode::Crop;
            if (theme->layout.background.opacity <= 0.0f)
                theme->layout.background.opacity = 1.0f;
        }
        if (artwork.background_mode >= 0)
            theme->layout.background.mode = BackgroundModeFromInt(artwork.background_mode);
        if (artwork.surface_opacity >= 0.0f)
            theme->layout.surface_opacity = artwork.surface_opacity;

        // Identical files decode once; a repeated sticker reuses the pixels.
        std::map<std::string, LauncherThemeImage> decoded;
        const auto decode = [&](const std::string& path) -> const LauncherThemeImage& {
            const auto found = decoded.find(path);
            if (found != decoded.end()) return found->second;
            LauncherThemeImage image;
            std::filesystem::path resolved;
            if (anomaly::ResolvePlatformUiLayoutPath(runtime_root, path, resolved)) {
                const auto bytes = anomaly::ReadUiResourceBytes(
                    resolved, anomaly::kDefaultUiResourceEncodedByteLimit);
                if (bytes) {
                    auto result = anomaly::DecodeUiImageRgba8(bytes.bytes);
                    if (result) {
                        image.pixels = std::move(result.image.pixels);
                        image.width = result.image.width;
                        image.height = result.image.height;
                    }
                }
            }
            return decoded.emplace(path, std::move(image)).first->second;
        };
        if (theme->layout.background.mode != anomaly::PlatformUiBackgroundMode::Disabled) {
            theme->background = decode(theme->layout.background.path);
        }
        theme->stickers.reserve(theme->layout.stickers.size());
        for (const auto& sticker : theme->layout.stickers) {
            theme->stickers.push_back(decode(sticker.path));
        }
    } catch (...) {
        // A theme is decoration: a failed load keeps the plain launcher.
    }
    return theme;
}

std::filesystem::path ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path);
}

std::filesystem::path ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

// The hook that goes into the official launcher. Its source still lives in this tree
// (src/launcher/hook/nte_game_hook.cpp) but nothing builds or ships it, so the file is absent from an
// installed package. The path is absolute because the launcher it is injected into runs from an
// install location this process does not own.
std::filesystem::path GameHookLibrary() {
    return ExecutableDirectory() / L"Anomaly" / L"NTEGameHook.dll";
}

// Auto-starting the official client is off. The launcher is started the way its own shortcut starts
// it, and the player presses start game in the window that opens, so capture waits for the HTGame.exe
// that press produces. Everything auto-starting needs -- resolving the client out of the bootstrap's
// update section and injecting the hook -- stays compiled behind this switch, but the hook library is
// no longer built or shipped, so turning it back on also means building it again.
constexpr bool kAutoStartOfficialClient = false;

struct AdministratorLaunchResult final {
    bool run_current_process{};
    int exit_code{};
};

AdministratorLaunchResult EnsureAdministrator(PWSTR command_line) noexcept {
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    const BOOL queried = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    const DWORD query_error = queried == FALSE ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(token);
    if (queried == FALSE) return {false, static_cast<int>(query_error)};
    if (elevation.TokenIsElevated != 0) return {true, ERROR_SUCCESS};

    const auto executable = ExecutablePath();
    if (executable.empty()) return {false, ERROR_FILE_NOT_FOUND};
    const std::wstring working_directory = executable.parent_path().wstring();
    SHELLEXECUTEINFOW launch{
        .cbSize = sizeof(launch),
        .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
        .lpVerb = L"runas",
        .lpFile = executable.c_str(),
        .lpParameters = command_line != nullptr && command_line[0] != L'\0'
            ? command_line : nullptr,
        .lpDirectory = working_directory.c_str(),
        .nShow = SW_SHOWNORMAL,
    };
    if (ShellExecuteExW(&launch) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    if (launch.hProcess != nullptr) CloseHandle(launch.hProcess);
    return {false, ERROR_SUCCESS};
}

HMODULE LoadSystemDwmapi() {
    std::wstring directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(
        directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) return nullptr;
    directory.resize(length);
    return LoadLibraryExW(
        (std::filesystem::path(directory) / L"dwmapi.dll").c_str(),
        nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

std::string WideUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring Utf8Wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        return {};
    }
    return result;
}

std::string PathUtf8(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    return WideUtf8(value);
}

std::string VirtualKeyName(const std::uint32_t key) {
    const std::string fallback = "Key " + std::to_string(key);
    const UINT scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    wchar_t buffer[64]{};
    const LONG parameter = static_cast<LONG>(scan_code << 16U);
    if (GetKeyNameTextW(parameter, buffer, static_cast<int>(std::size(buffer))) <= 0) {
        return fallback;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return fallback;
    std::string result(static_cast<std::size_t>(size), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, result.data(), size, nullptr, nullptr));
    result.pop_back();
    return result;
}

bool PathsEqual(
    const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    try {
        const std::wstring left_value = left.lexically_normal().wstring();
        const std::wstring right_value = right.lexically_normal().wstring();
        return !left_value.empty() && !right_value.empty() &&
            CompareStringOrdinal(
                left_value.c_str(), -1, right_value.c_str(), -1, TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

std::string EncodeUtf8(char32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7fU) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return result;
}

std::string Ellipsize(std::string_view value, float width) {
    if (ImGui::CalcTextSize(value.data(), value.data() + value.size()).x <= width) {
        return std::string(value);
    }
    constexpr std::string_view suffix{"..."};
    if (ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x > width) {
        return {};
    }
    std::size_t end = value.size();
    while (end > 0) {
        --end;
        while (end > 0 &&
               (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        const auto candidate = std::string(value.substr(0, end)) + std::string(suffix);
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
    }
    return std::string(suffix);
}

// Every icon the launcher draws, and therefore the whole icon font it bakes. A glyph
// that is not in this list cannot be drawn, so adding one here is what makes it
// available as well as visible. 0xeb9f is the picture mark a card without a cover wears.
constexpr std::array<char32_t, 9> kLauncherIcons{
    0xe838, 0xe72c, 0xe768, 0xe73e, 0xe7ba, 0xe711, 0xe8b7, 0xe8e5, 0xeb9f};

// The launcher draws its body text at this multiple of the platform base size and its
// title with an explicit size from the same font, so one bake serves every string on
// screen. Another bake would only be a second copy of a 15.7 MB font.
constexpr std::array kLauncherFontScales{2.00f};

const char* Glyph(char32_t codepoint) {
    struct Entry final { char32_t codepoint; std::string text; };
    static const std::array<Entry, kLauncherIcons.size()> entries = [] {
        std::array<Entry, kLauncherIcons.size()> built{};
        for (std::size_t index = 0; index < built.size(); ++index) {
            built[index] = Entry{kLauncherIcons[index], EncodeUtf8(kLauncherIcons[index])};
        }
        return built;
    }();
    const auto found = std::find_if(entries.begin(), entries.end(), [codepoint](const auto& entry) {
        return entry.codepoint == codepoint;
    });
    return found == entries.end() ? "?" : found->text.c_str();
}

LauncherMessage MakeLauncherMessage(
    anomaly::MessageId id,
    std::initializer_list<std::string_view> arguments = {},
    std::string detail = {}) {
    LauncherMessage message;
    message.id = id;
    message.arguments.reserve(arguments.size());
    for (const auto argument : arguments) message.arguments.emplace_back(argument);
    message.detail = std::move(detail);
    return message;
}

anomaly::MessageId ProxyStateMessageId(
    anomaly::launcher::ProxyInstallationState state) noexcept {
    using State = anomaly::launcher::ProxyInstallationState;
    switch (state) {
    case State::NotInstalled: return anomaly::MessageId::LauncherProxyStateNotInstalled;
    case State::Enabled: return anomaly::MessageId::LauncherProxyStateEnabled;
    case State::Disabled: return anomaly::MessageId::LauncherProxyStateDisabled;
    case State::UpdateAvailable:
        return anomaly::MessageId::LauncherProxyStateUpdateAvailable;
    case State::Conflict: return anomaly::MessageId::LauncherProxyStateConflict;
    case State::Unavailable: return anomaly::MessageId::LauncherProxyStateUnavailable;
    }
    return anomaly::MessageId::LauncherProxyStateUnavailable;
}

std::filesystem::path LauncherModsDirectory(const LauncherSnapshot& snapshot);

// The launcher's network helpers live with the GameBanana pages further down; the update check on
// this side of them needs their declarations only. The status code is reported as well as the body
// because the update check has to tell a rate-limited API answer from a release that is not there.
struct LauncherDownloadProgress final {
    std::atomic<long long> bytes{};
    std::atomic<long long> total{};
    std::atomic<bool> cancel{};
};

std::string LauncherHttpGet(const std::wstring& host, const std::wstring& path,
    bool use_system_proxy = false, int* status_code = nullptr);
std::string LauncherHttpGetUrl(const std::string& url, bool use_system_proxy = false,
    int* status_code = nullptr, DWORD timeout_ms = 8000);
bool LauncherDownloadUrl(const std::string& url, const std::filesystem::path& destination,
    LauncherDownloadProgress* progress = nullptr);
void LauncherUpdateLog(const std::string& line);

// The addresses one github.com URL is asked at, best-known first: the mirror that answered last time,
// then the direct address, then the rest. A launcher started on a network where github.com does not
// answer would otherwise spend its whole timeout budget on the direct attempt before it ever reaches
// the mirror that works.
std::vector<std::string> LauncherLookupCandidates(
    const std::string& url, const std::string& endpoint) {
    std::vector<std::string> candidates;
    candidates.reserve(1 + anomaly::launcher::kGithubProxyPrefixes.size());
    if (!endpoint.empty()) candidates.push_back(endpoint + url);
    candidates.push_back(url);
    for (const auto prefix : anomaly::launcher::kGithubProxyPrefixes) {
        if (std::string(prefix) != endpoint) candidates.push_back(std::string(prefix) + url);
    }
    return candidates;
}

// Which mirror a URL was asked through: the prefix it starts with, or nothing for the direct address.
std::string LauncherEndpointOfUrl(const std::string& url) {
    for (const auto prefix : anomaly::launcher::kGithubProxyPrefixes) {
        if (url.starts_with(prefix)) return std::string(prefix);
    }
    return {};
}



// Mod installs and launcher updates run on different workers and may overlap.
LauncherDownloadProgress g_mod_download;
LauncherDownloadProgress g_update_download;

class LauncherController final {
public:
    explicit LauncherController(std::filesystem::path payload_root)
        : payload_root_(std::move(payload_root)),
          source_{payload_root_ / L"dwmapi.dll", payload_root_ / L"Anomaly"},
          configuration_path_(anomaly::launcher::LauncherConfigurationPath(payload_root_)),
          worker_([this](std::stop_token stop) { WorkerMain(stop); }) {
        Queue(anomaly::MessageId::LauncherStatusScanningLocal, MessageScope::Both, [this] {
            InitializePathsImpl();
        });
    }

    ~LauncherController() {
        g_update_download.cancel.store(true);
        update_check_worker_.request_stop();
        worker_.request_stop();
        queue_changed_.notify_all();
        if (update_check_worker_.joinable()) update_check_worker_.join();
        worker_.join();
        if (telemetry_worker_.joinable()) {
            telemetry_worker_.request_stop();
            telemetry_worker_.join();
        }
        // Closing the window may cancel a queued preference write. Once initialization completed,
        // persist the final in-memory configuration after the worker has stopped.
        if (state_.theme != nullptr) static_cast<void>(PersistConfigurationImpl());
    }

    LauncherController(const LauncherController&) = delete;
    LauncherController& operator=(const LauncherController&) = delete;

    [[nodiscard]] LauncherSnapshot Snapshot() const {
        std::scoped_lock lock(state_mutex_);
        return state_;
    }

    void SelectGameDirectory(std::filesystem::path directory) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.game_directory = std::move(directory);
            configuration_.Selected().game_directory = state_.game_directory;
        }
        Queue(anomaly::MessageId::LauncherStatusInspectingProxy, MessageScope::Proxy, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshHotkeyImpl();
            RefreshProxyImpl();
            RefreshProcessesImpl(ProcessAnnounce::Never);
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void RefreshProxy() {
        Queue(anomaly::MessageId::LauncherStatusInspectingProxy, MessageScope::Proxy, [this] {
            RefreshProxyImpl();
            RefreshProcessesImpl(ProcessAnnounce::Never);
        });
    }

    void InstallProxy() {
        Queue(anomaly::MessageId::LauncherStatusInstallingRuntime, MessageScope::Proxy, [this] {
            StartTelemetryImpl();
            const auto game = GameDirectory();
            const auto result = anomaly::launcher::InstallProxyRuntime(game, source_);
            PublishProxy(result);
            RefreshProcessesImpl(ProcessAnnounce::Never);
        });
    }

    void SetProxyEnabled(bool enabled) {
        Queue(enabled ? anomaly::MessageId::LauncherStatusEnablingProxy
                      : anomaly::MessageId::LauncherStatusDisablingProxy,
            MessageScope::Proxy, [this, enabled] {
            const auto game = GameDirectory();
            const auto result = anomaly::launcher::SetProxyEnabled(
                game, source_, enabled);
            PublishProxy(result);
        });
    }

    void UninstallProxy() {
        Queue(anomaly::MessageId::LauncherStatusUninstallingProxy, MessageScope::Proxy, [this] {
            StartTelemetryImpl();
            const auto result = anomaly::launcher::UninstallProxy(GameDirectory(), source_);
            PublishProxy(result);
            RefreshProcessesImpl(ProcessAnnounce::Never);
            if (result.Ok() && result.state == anomaly::launcher::ProxyInstallationState::NotInstalled)
                PublishMessage(anomaly::MessageId::LauncherStatusProxyUninstalled, MessageKind::Success);
        });
    }

    void RefreshProcesses() {
        Queue(anomaly::MessageId::LauncherStatusScanningProcesses,
            MessageScope::Attach, [this] { RefreshProcessesImpl(); });
    }

    // A game that starts after this window is up would otherwise leave the list empty until the user asks
    // for a scan. Polling keeps it current: the work runs on the worker like every other operation, but it
    // is silent -- it never takes the busy flag, so buttons do not flicker, and it never writes the status
    // line, so an outcome stays readable while the list behind it catches up.
    void PollProcesses() {
        constexpr auto kInterval = std::chrono::seconds(2);
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            const auto now = std::chrono::steady_clock::now();
            if (last_process_poll_ != std::chrono::steady_clock::time_point{} &&
                now - last_process_poll_ < kInterval) {
                return;
            }
            last_process_poll_ = now;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back([this] { RefreshProcessesImpl(ProcessAnnounce::OnChange); });
        }
        queue_changed_.notify_one();
    }

    void SelectLauncherExecutable(std::filesystem::path executable) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.launcher_executable = std::move(executable);
            configuration_.Selected().launcher_executable = state_.launcher_executable;
        }
        Queue(anomaly::MessageId::LauncherStatusScanningLocal, MessageScope::Attach, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshHotkeyImpl();
            RefreshProxyImpl();
            RefreshProcessesImpl();
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void SelectClient(const anomaly::launcher::NteClient client) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || configuration_.selected_client == client) return;
            configuration_.selected_client = client;
            const auto& selected = configuration_.Selected();
            state_.selected_client = client;
            state_.game_directory = selected.game_directory;
            state_.launcher_executable = selected.launcher_executable;
            state_.proxy = {};
            state_.attached_process = 0;
        }
        Queue(anomaly::MessageId::LauncherStatusScanningLocal, MessageScope::Both, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshHotkeyImpl();
            RefreshProxyImpl();
            RefreshProcessesImpl();
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void LaunchAndAttach() {
        Queue(anomaly::MessageId::LauncherStatusLaunchingAttach, MessageScope::Attach, [this] {
            StartTelemetryImpl();
            const auto launcher = LauncherExecutable();
            const auto selected = ResolveAttachRuntime();
            if (!selected.Ok()) {
                PublishRuntimeFailure(selected);
                return;
            }
            anomaly::DisableRestrictedPakMods(LauncherModsDirectory(Snapshot()));
            anomaly::launcher::ManualMapLaunchOptions options;
            // NTELauncher.exe is what the player's own shortcut starts: it opens the launcher window,
            // and the press of its start button is what creates HTGame.exe.
            options.launcher_path = launcher;
            options.working_directory = launcher.parent_path();
            if constexpr (kAutoStartOfficialClient) {
                // NTELauncher.exe is only the self-updating bootstrap. The launcher window and the
                // platform pipe server live in the client it starts, so that client is started
                // directly: it is the process that has to stay hidden, and it is the one that spawns
                // HTGame.exe.
                const auto command =
                    anomaly::launcher::ResolveClientLaunchCommand(launcher);
                if (command.executable.empty()) {
                    PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                        MessageKind::Error, command.failure);
                    return;
                }
                options.launcher_path = command.executable;
                options.launcher_arguments = command.arguments;
                options.working_directory = command.executable.parent_path();
                // The launcher is never asked to act by a user, so a hook goes into it: it keeps the
                // launcher's window hidden from the inside and asks the launcher to start the game,
                // which is what leaves the platform session -- and with it the login and support
                // buttons -- exactly as a normal launch produces it.
                options.hook_path = GameHookLibrary();
            }
            options.manual_map.core_path = selected.core_path;
            options.manual_map.runtime_root = selected.runtime_root;
            options.manual_map.log_directory = options.manual_map.runtime_root / L"logs";
            const auto result =
                anomaly::launcher::LaunchAndManualMapRuntimeCore(options);
            if (result.Ok()) RefreshProcessesImpl();

            std::scoped_lock lock(state_mutex_);
            if (result.Ok()) {
                state_.attached_process = result.process_id;
                const std::string process_id = std::to_string(result.process_id);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusLaunchAttached, {process_id});
                state_.message_kind = MessageKind::Success;
            } else {
                const std::string error = std::to_string(result.mapping.win32_error);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusLaunchAttachFailed, {error},
                    result.mapping.message);
                state_.message_kind = MessageKind::Error;
            }
        });
    }

    // Live attach: map the core into a game the player started from its own shortcut, at whatever point
    // they start the launcher. Nothing is relaunched and nothing is terminated, so a failed attach leaves
    // the running game exactly as it was.
    void InjectIntoRunningProcess(const DWORD process_id) {
        if (process_id == 0) return;
        Queue(anomaly::MessageId::LauncherStatusInjectingProcess, MessageScope::Attach,
            [this, process_id] {
            StartTelemetryImpl();
            const auto selected = ResolveAttachRuntime();
            if (!selected.Ok()) {
                PublishRuntimeFailure(selected);
                return;
            }
            anomaly::launcher::ManualMapOptions options;
            options.process_id = process_id;
            options.core_path = selected.core_path;
            options.runtime_root = selected.runtime_root;
            options.log_directory = options.runtime_root / L"logs";
            // Which way into a game that is already running can work depends on what that game is doing
            // at this moment: whether a thread reaches an alertable wait, whether one is parked in a
            // system call. The mapper tries them in turn rather than making the user guess.
            options.delivery = anomaly::launcher::ManualMapDelivery::Automatic;
            const auto result = anomaly::launcher::AttachToRunningProcess(options);
            // Announce is off: the rescan would report the attachment in the launched wording and
            // replace the message this action is about to publish.
            if (result.Ok()) RefreshProcessesImpl(ProcessAnnounce::Never);

            std::scoped_lock lock(state_mutex_);
            if (result.Ok()) {
                state_.attached_process = process_id;
                const std::string target = std::to_string(process_id);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusInjected, {target});
                state_.message_kind = MessageKind::Success;
            } else {
                const std::string error = std::to_string(result.win32_error);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusInjectFailed, {error}, result.message);
                state_.message_kind = MessageKind::Error;
            }
        });
    }

    void SetToggleKey(const std::uint32_t key) {
        Queue(anomaly::MessageId::LauncherStatusSavingSettings, MessageScope::Both, [this, key] {
            if (!SaveToggleKeyImpl(key)) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, "menu toggle preference could not be written");
                return;
            }
            std::scoped_lock lock(state_mutex_);
            state_.toggle_key = key;
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusSettingsSaved);
            state_.message_kind = MessageKind::Success;
        });
    }

    // The launcher's own theme. It is stored in the launcher configuration rather than
    // in the game's settings, because it dresses this window alone, and the reload runs
    // on the worker like every other file read: decoding a wallpaper on the render
    // thread would stutter the window it is painting.
    void SetLauncherTheme(const anomaly::PlatformUiPalette palette) {
        const std::string name(anomaly::ToString(palette));
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || configuration_.theme == name) return;
            configuration_.theme = name;
        }
        Queue(anomaly::MessageId::LauncherStatusSavingSettings, MessageScope::Both, [this] {
            RefreshThemeImpl();
            const auto saved = PersistConfigurationImpl();
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
                return;
            }
            PublishMessage(anomaly::MessageId::LauncherStatusSettingsSaved,
                MessageKind::Success);
        });
    }

    // The colours behind the "custom" palette. They are applied on the frame that stores them, so a
    // colour being dragged is visible while it moves, and written with the rest of the settings.
    void SetLauncherCustomColors(const anomaly::PlatformUiCustomColors& colors) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.custom_colors == colors) return;
            configuration_.custom_colors = colors;
            state_.custom_colors = colors;
            settings_dirty_ = true;
        }
    }

    // The launcher's own background: the picture and the fill mode beside it. Both are stored with
    // the launcher configuration, and a change reloads the theme on the worker -- decoding a wallpaper
    // on the render thread would stutter the window that is painting it.
    void SetLauncherBackground(const std::string& path, const int mode) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.artwork.background == path &&
                configuration_.artwork.background_mode == mode) return;
            configuration_.artwork.background = path;
            configuration_.artwork.background_mode = mode;
            state_.artwork = configuration_.artwork;
            settings_dirty_ = true;
            theme_dirty_ = true;
        }
    }

    // The veil over that picture: the same value the in-game settings call the background's
    // visibility, and the same one the layout calls surfaceOpacity.
    void SetLauncherSurfaceOpacity(const float opacity) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.artwork.surface_opacity == opacity) return;
            configuration_.artwork.surface_opacity = opacity;
            state_.artwork = configuration_.artwork;
            settings_dirty_ = true;
            theme_dirty_ = true;
        }
    }

    // The launcher's own language. It is stored in the launcher configuration, not in the ini
    // the runtime reads, because it dresses this window alone -- and it takes effect on the
    // next frame rather than on the next launch, see LoadLauncherCatalog.
    void SetLanguage(const anomaly::LanguagePreference preference) {
        const std::string name(anomaly::LanguagePreferenceName(preference));
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.language == name) return;
            configuration_.language = name;
            state_.language = name;
            settings_dirty_ = true;
        }
    }

    // The launcher's own update. "Check" is the read of the newest release and its comparison with
    // this build; "install" downloads that release's runtime package, publishes it over this
    // installation and starts the new image. Both run on the worker: neither may block a frame.
    // AutoCheckUpdates is what the window calls every frame, and it decides for itself whether this
    // launch still owes a check.
    void SetAutoCheckUpdates(const bool enabled) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.update.auto_check == enabled) return;
            configuration_.update.auto_check = enabled;
            state_.update.auto_check = enabled;
            settings_dirty_ = true;
        }
    }

    // "Skip this version": the release stays known and installable from the settings page, but the
    // prompt stops asking about it, this session and every later one.
    void SkipUpdateVersion(std::string version) {
        {
            std::scoped_lock lock(state_mutex_);
            if (version.empty() || configuration_.update.skipped_version == version) return;
            configuration_.update.skipped_version = std::move(version);
            state_.update.skipped_version = configuration_.update.skipped_version;
            settings_dirty_ = true;
        }
    }

    void AutoCheckUpdates() {
        {
            std::scoped_lock lock(state_mutex_);
            if (auto_check_done_ || state_.busy || !configuration_.update.auto_check) return;
            auto_check_done_ = true;
        }
        if (!StartUpdateCheck()) {
            std::scoped_lock lock(state_mutex_);
            auto_check_done_ = false;  // the worker was busy; the next frame tries again
        }
    }

    void CheckForUpdates() {
        StartUpdateCheck();
    }

    void InstallUpdate() {
        const auto snapshot = Snapshot();
        if (snapshot.busy || !snapshot.update.CanInstall()) return;
        g_update_download.cancel.store(false);
        g_update_download.bytes.store(0);
        g_update_download.total.store(0);
        QueueUpdate([this] { InstallUpdateImpl(); }, LauncherUpdateFailure::Install);
    }

    // The mods page and the download page own no configuration of their own: their view
    // choices are stored here and read back through the snapshot, which is what keeps the
    // settings page and the pages in step. The write itself is deferred to FlushSettings.
    void SetModsView(const bool grid, const bool pagination, const int page_size) {
        {
            std::scoped_lock lock(state_mutex_);
            configuration_.view.mods_grid = grid;
            configuration_.view.mods_pagination = pagination;
            configuration_.view.mods_page_size =
                anomaly::launcher::LauncherNearestPageSize(page_size);
            state_.view = configuration_.view;
            settings_dirty_ = true;
        }
    }

    void SetBrowseView(std::string sort, std::string category, const bool hide_mature) {
        {
            std::scoped_lock lock(state_mutex_);
            configuration_.view.browse_sort = std::move(sort);
            configuration_.view.browse_category = std::move(category);
            configuration_.view.browse_hide_mature = hide_mature;
            state_.view = configuration_.view;
            settings_dirty_ = true;
        }
    }

    // Called once a frame by the UI. A preference change is held in memory until the worker is
    // free, so flipping a switch or dragging the page-size slider costs one configuration write
    // instead of one per frame, and nothing announces itself: the control the user just moved is
    // its own feedback.
    void FlushSettings() {
        bool reload_theme{};
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || !settings_dirty_) return;
            settings_dirty_ = false;
            reload_theme = std::exchange(theme_dirty_, false);
        }
        if (!QueueSilent([this, reload_theme] {
                if (reload_theme) RefreshThemeImpl();
                const auto saved = PersistConfigurationImpl();
                if (!saved.Ok()) PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            })) {
            std::scoped_lock lock(state_mutex_);
            settings_dirty_ = true;
            theme_dirty_ = theme_dirty_ || reload_theme;
        }
    }

private:
    using Work = std::function<void()>;

    bool StartUpdateCheck() {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || state_.update.Busy()) return false;
            state_.update.phase = LauncherUpdatePhase::Checking;
            auto_check_done_ = true;
        }
        // A release lookup must not reserve the launch/install worker for every network timeout.
        // The previous lookup has published its result before another check becomes available.
        if (update_check_worker_.joinable()) update_check_worker_.join();
        update_check_worker_ = std::jthread([this](std::stop_token stop) {
            RunUpdate([this, stop] { CheckForUpdatesImpl(stop); }, LauncherUpdateFailure::Check);
        });
        return true;
    }

    bool QueueUpdate(Work work, LauncherUpdateFailure failure) {
        return QueueSilent([this, work = std::move(work), failure] {
            RunUpdate(work, failure);
        });
    }

    void RunUpdate(const Work& work, LauncherUpdateFailure failure) {
        try {
            work();
        } catch (...) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Failed;
            state_.update.failure = failure;
            state_.update.detail = "update operation could not be completed";
        }
    }

    [[nodiscard]] anomaly::RuntimeLaunchResult ResolveAttachRuntime() const {
        auto bundled = anomaly::ResolveRuntimeLaunch({source_.runtime_directory});
        if (bundled.Ok()) return bundled;
        auto installed = anomaly::ResolveRuntimeLaunch({GameDirectory() / L"Anomaly"});
        return installed.Ok() ? installed : bundled;
    }

    void PublishRuntimeFailure(const anomaly::RuntimeLaunchResult& selected) {
        std::scoped_lock lock(state_mutex_);
        state_.runtime_version.clear();
        state_.runtime_message = selected.message;
        state_.core_available = false;
        state_.message = MakeLauncherMessage(
            anomaly::MessageId::LauncherStatusCoreUnavailable, {}, selected.message);
        state_.message_kind = MessageKind::Error;
    }

    void PublishMessage(anomaly::MessageId id, MessageKind kind, std::string detail = {}) {
        std::scoped_lock lock(state_mutex_);
        state_.message = MakeLauncherMessage(id, {}, std::move(detail));
        state_.message_kind = kind;
    }

    [[nodiscard]] std::filesystem::path GameDirectory() const {
        std::scoped_lock lock(state_mutex_);
        return state_.game_directory;
    }

    [[nodiscard]] std::filesystem::path LauncherExecutable() const {
        std::scoped_lock lock(state_mutex_);
        return state_.launcher_executable;
    }

    // Which runtime the game ends up loading depends on the mode: a proxy installation keeps its
    // runtime inside the game folder, while the core this launcher maps itself runs from the
    // bundled payload. Both read their own anomaly.ini, so both have to carry the startup hotkey --
    // writing only the installed one left an attached game toggling on the payload's shipped
    // default, and writing only the payload one left a proxy-installed game on its old key.
    // The installed runtime comes first because that is the file the row reads back from.
    [[nodiscard]] std::vector<std::filesystem::path> RuntimeSettingsRoots() const {
        std::vector<std::filesystem::path> roots;
        std::vector<std::filesystem::path> game_directories;
        {
            std::scoped_lock lock(state_mutex_);
            game_directories.push_back(state_.game_directory);
            game_directories.push_back(
                configuration_.selected_client == anomaly::launcher::NteClient::Global
                    ? configuration_.mainland_china.game_directory
                    : configuration_.global.game_directory);
        }
        const auto add = [&roots](const std::filesystem::path& root) {
            std::error_code error;
            if (root.empty() || !std::filesystem::is_directory(root, error) || error) return;
            if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
        };
        for (const auto& game_directory : game_directories) {
            if (game_directory.empty()) continue;
            const auto installed = game_directory / L"Anomaly";
            std::error_code error;
            // Only an installation the game folder really holds: creating the runtime directory
            // from a settings write would make an empty one look installed.
            if (std::filesystem::is_regular_file(
                    installed / L"Anomaly.Core.dll", error) && !error) {
                add(installed);
            }
        }
        // The launcher's own runtime last: a manual map starts the core from here, and this ini is
        // the file the launcher itself reads at start-up.
        add(source_.runtime_directory);
        return roots;
    }

    void RefreshHotkeyImpl() {
        const auto roots = RuntimeSettingsRoots();
        const auto root = roots.empty() ? source_.runtime_directory : roots.front();
        const auto config = ue5mem::AnalyzerConfig::Load(root / L"anomaly.ini");
        std::scoped_lock lock(state_mutex_);
        state_.toggle_key = config.platform_toggle_key;
    }

    [[nodiscard]] bool SaveToggleKeyImpl(const std::uint32_t key) const {
        const std::wstring value = std::to_wstring(key);
        const auto roots = RuntimeSettingsRoots();
        // Nothing written is not a saved preference, so an empty list starts out failed instead of
        // reporting a success the game never sees.
        bool saved = !roots.empty();
        for (const auto& root : roots) {
            const bool written = WritePrivateProfileStringW(
                L"Platform", L"ToggleKey", value.c_str(),
                (root / L"anomaly.ini").c_str()) != FALSE;
            saved = written && saved;
        }
        return saved;
    }

    // The launcher's own ini, which is the file this process reads at start-up. The runtime's
    // copy next to the game holds the same keys for the in-game window; this one is the
    // launcher's, exactly like its configuration next to the executable.
    [[nodiscard]] std::filesystem::path LauncherSettingsPath() const {
        return payload_root_ / L"Anomaly" / L"anomaly.ini";
    }

    // A queued unit of work that does not replace the status line: preferences are written
    // silently, because a "saving" line under a switch the user just flipped is noise.
    bool QueueSilent(Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back(std::move(work));
        }
        queue_changed_.notify_one();
        return true;
    }

    bool Queue(anomaly::MessageId activity, MessageScope scope, Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
            state_.message = MakeLauncherMessage(activity);
            state_.message_kind = MessageKind::Neutral;
            // The scope is the operation's, so every message the work publishes --
            // including the ones written deeper in, such as a failed settings save --
            // stays attributed to the mode that asked for it.
            state_.message_scope = scope;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back(std::move(work));
        }
        queue_changed_.notify_one();
        return true;
    }

    void WorkerMain(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Work work;
            {
                std::unique_lock lock(queue_mutex_);
                queue_changed_.wait(lock, stop, [this] { return !queue_.empty(); });
                if (stop.stop_requested()) break;
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                work();
            } catch (...) {
                std::scoped_lock lock(state_mutex_);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusUnexpectedFailure);
                state_.message_kind = MessageKind::Error;
            }
            std::scoped_lock lock(state_mutex_);
            state_.busy = false;
        }
    }

    void RefreshProxyImpl() {
        const auto game = GameDirectory();
        if (game.empty()) {
            std::scoped_lock lock(state_mutex_);
            state_.proxy = {};
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusSelectGameDirectory);
            state_.message_kind = MessageKind::Neutral;
            return;
        }
        PublishProxy(anomaly::launcher::InspectProxyInstallation(game, source_));
    }

    void PublishProxy(anomaly::launcher::ProxyInstallationStatus result) {
        std::scoped_lock lock(state_mutex_);
        state_.proxy = std::move(result);
        state_.message_scope = MessageScope::Proxy;
        state_.message = state_.proxy.Ok()
            ? MakeLauncherMessage(
                ProxyStateMessageId(state_.proxy.state), {}, state_.proxy.message)
            : MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusProxyOperationFailed, {},
                state_.proxy.message);
        state_.message_kind = state_.proxy.Ok()
            ? (state_.proxy.state == anomaly::launcher::ProxyInstallationState::Enabled ||
                state_.proxy.state == anomaly::launcher::ProxyInstallationState::Disabled
                ? MessageKind::Success : MessageKind::Neutral)
            : MessageKind::Error;
    }

    void InitializePathsImpl() {
        const auto loaded = anomaly::launcher::LoadLauncherConfiguration(configuration_path_);
        const auto game_processes = anomaly::launcher::EnumerateAttachableProcesses();
        const auto mainland_launcher_processes =
            anomaly::launcher::EnumerateAttachableProcesses(L"NTELauncher.exe");
        const auto global_launcher_processes =
            anomaly::launcher::EnumerateAttachableProcesses(L"NTEGlobalLauncher.exe");
        auto configuration = loaded.configuration;
        if (!configuration.mainland_china.game_directory.empty() &&
            PathsEqual(configuration.mainland_china.game_directory, configuration.global.game_directory)) {
            // Older discovery could save the same running game for both clients.
            const bool keep_mainland = anomaly::launcher::IsNteLauncherExecutable(
                configuration.mainland_china.launcher_executable,
                anomaly::launcher::NteClient::MainlandChina) ||
                !anomaly::launcher::IsNteLauncherExecutable(configuration.global.launcher_executable,
                    anomaly::launcher::NteClient::Global);
            (keep_mainland ? configuration.global : configuration.mainland_china).game_directory.clear();
        }
        const auto discover = [this, &game_processes, &configuration](
                                  const anomaly::launcher::NteClient client,
                                  const auto& launcher_processes) {
            anomaly::launcher::LauncherDiscoveryOptions options;
            options.client = client;
            options.payload_root = payload_root_;
            options.allow_unpaired_game_discovery = true;
            options.other_game_directory = client == anomaly::launcher::NteClient::Global
                ? configuration.mainland_china.game_directory : configuration.global.game_directory;
            if (configuration.selected_client == client) {
                for (const auto& process : game_processes) {
                    if (!process.executable_path.empty()) {
                        options.running_game_executables.push_back(process.executable_path);
                    }
                }
            }
            for (const auto& process : launcher_processes) {
                if (!process.executable_path.empty()) {
                    options.running_launcher_executables.push_back(process.executable_path);
                }
            }
            return anomaly::launcher::DiscoverLauncherConfiguration(
                client == anomaly::launcher::NteClient::Global
                    ? configuration.global : configuration.mainland_china,
                options);
        };
        configuration.mainland_china = discover(
            anomaly::launcher::NteClient::MainlandChina, mainland_launcher_processes);
        configuration.global = discover(
            anomaly::launcher::NteClient::Global, global_launcher_processes);
        // A launcher that stores no language yet follows the one its runtime ini already names,
        // which is what an installation upgraded from an older build has. From here on the
        // configuration owns the value and the ini is only the seed.
        if (configuration.language.empty()) {
            const ue5mem::AnalyzerConfig ini =
                ue5mem::AnalyzerConfig::Load(LauncherSettingsPath());
            configuration.language =
                std::string(anomaly::LanguagePreferenceName(ini.platform_language));
        }
        {
            std::scoped_lock lock(state_mutex_);
            configuration_ = std::move(configuration);
            const auto& selected = configuration_.Selected();
            state_.selected_client = configuration_.selected_client;
            state_.game_directory = selected.game_directory;
            state_.launcher_executable = selected.launcher_executable;
            state_.language = configuration_.language;
            state_.view = configuration_.view;
            state_.artwork = configuration_.artwork;
            state_.custom_colors = configuration_.custom_colors;
            state_.update.auto_check = configuration_.update.auto_check;
            state_.update.skipped_version = configuration_.update.skipped_version;
        }
        const auto saved = PersistConfigurationImpl();
        // A previous update left the image it replaced behind, which this process can now delete:
        // while that image was running it could only be renamed.
        anomaly::launcher::RemoveUpdateLeftovers(payload_root_);
        RefreshHotkeyImpl();
        RefreshProxyImpl();
        RefreshProcessesImpl();
        RefreshThemeImpl();
        if (!saved.Ok()) {
            PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                MessageKind::Error, saved.message);
        }
    }

    void ReconcileRelatedPathsImpl() {
        anomaly::launcher::LauncherClientConfiguration preferred;
        std::filesystem::path other_game_directory;
        anomaly::launcher::NteClient client{};
        {
            std::scoped_lock lock(state_mutex_);
            preferred.game_directory = state_.game_directory;
            preferred.launcher_executable = state_.launcher_executable;
            client = configuration_.selected_client;
            other_game_directory = client == anomaly::launcher::NteClient::Global
                ? configuration_.mainland_china.game_directory : configuration_.global.game_directory;
        }
        anomaly::launcher::LauncherDiscoveryOptions options;
        options.client = client;
        options.payload_root = payload_root_;
        options.allow_unpaired_game_discovery = true;
        options.other_game_directory = std::move(other_game_directory);
        // A game that is already running is the surest pairing there is: its own directory is where the client
        // the user is switching to is installed, and it is the directory the process list filters on next. It
        // only fills a directory that is not known yet -- a stored one always wins in the discovery below.
        for (const auto& process : anomaly::launcher::EnumerateAttachableProcesses()) {
            if (!process.executable_path.empty()) {
                options.running_game_executables.push_back(process.executable_path);
            }
        }
        const auto discovered = anomaly::launcher::DiscoverLauncherConfiguration(
            preferred, options);
        std::scoped_lock lock(state_mutex_);
        configuration_.Selected() = discovered;
        state_.game_directory = discovered.game_directory;
        state_.launcher_executable = discovered.launcher_executable;
    }

    [[nodiscard]] anomaly::launcher::LauncherConfigurationSaveResult
    PersistConfigurationImpl() const {
        anomaly::launcher::LauncherConfiguration configuration;
        {
            std::scoped_lock lock(state_mutex_);
            configuration = configuration_;
        }
        return anomaly::launcher::SaveLauncherConfiguration(
            configuration_path_, configuration);
    }

    void StartTelemetryImpl() noexcept {
        try {
            const auto day = anomaly::TelemetryDay(std::chrono::system_clock::now());
            if (telemetry_attempt_day_ == day) return;
            anomaly::TelemetryOptions options;
            options.enabled = ue5mem::AnalyzerConfig::Load(LauncherSettingsPath()).telemetry_enabled;
            if (!options.enabled) return;
            const auto snapshot = Snapshot();
            if (!snapshot.theme) return;
            options.language = anomaly::LocaleName(anomaly::ResolveUserLocale(
                anomaly::ParseLanguagePreference(snapshot.language).preference).locale);
            anomaly::LauncherTelemetryUsage usage;
            usage.theme = anomaly::ToString(snapshot.theme->palette);
            usage.custom_background = anomaly::launcher::IsCustomLauncherBackground(
                snapshot.theme->palette, snapshot.theme->layout.background.path, snapshot.theme->HasBackground());
            telemetry_attempt_day_ = day;
            telemetry_worker_ = std::jthread([options = std::move(options), usage = std::move(usage)](
                std::stop_token stop) { anomaly::ReportLauncherUsage(options, usage, stop); });
        } catch (...) {}
    }

    void RefreshThemeImpl() {
        // The launcher reads the theme next to itself, exactly like its locales:
        // payload_root_ points at wherever the payload is installed, which is not
        // necessarily where this launcher keeps config/themes and assets.
        std::string palette;
        {
            std::scoped_lock lock(state_mutex_);
            palette = configuration_.theme;
        }
        anomaly::launcher::LauncherThemeArtwork artwork;
        {
            std::scoped_lock lock(state_mutex_);
            artwork = configuration_.artwork;
        }
        auto theme = LoadLauncherTheme(
            ExecutableDirectory() / L"Anomaly", palette, artwork, theme_version_ + 1);
        ++theme_version_;
        std::scoped_lock lock(state_mutex_);
        state_.theme = std::move(theme);
    }

    // GET /repos/<repository>/releases/latest, asked at the endpoint that answered last before any
    // other: the answer is the same JSON wherever it comes from, so the first endpoint that returns a
    // release ends the search and is remembered for the next launch. The status the API itself
    // reported is what a failure is described with, because that is the one that says "rate limited".
    void CheckForUpdatesImpl(std::stop_token stop) {
        // The mirror that answered last time, if any: asked first, so a launch on a network that
        // cannot reach api.github.com does not spend its budget on an address known not to answer.
        std::string endpoint;
        {
            std::scoped_lock lock(state_mutex_);
            endpoint = configuration_.update.endpoint;
            state_.update.phase = LauncherUpdatePhase::Checking;
            state_.update.failure = LauncherUpdateFailure::None;
            state_.update.detail.clear();
            state_.update.http_status = 0;
        }
        const std::string api_url = "https://api.github.com/repos/" +
            std::string(anomaly::launcher::kReleaseRepository) + "/releases/latest";
        std::optional<anomaly::launcher::ReleaseInfo> release;
        std::string answered_by;
        int status{};
        bool answered{};
        bool first = true;
        for (const std::string& url : LauncherLookupCandidates(api_url, endpoint)) {
            if (stop.stop_requested()) return;
            // The first endpoint gets both routes -- the system proxy the machine is configured with,
            // and a direct connection -- because a stale proxy is a common reason for a lookup to
            // return nothing. The rest are tried once: whoever answers, answers.
            int attempt_status{};
            std::string body = LauncherHttpGetUrl(url, false, &attempt_status, 3000);
            if (body.empty() && first && !stop.stop_requested())
                body = LauncherHttpGetUrl(url, true, &attempt_status, 3000);
            first = false;
            if (attempt_status != 0) status = attempt_status;
            if (body.empty()) continue;
            answered = true;
            if (auto parsed = anomaly::launcher::ParseLatestRelease(body); parsed.has_value()) {
                release = std::move(parsed);
                answered_by = url;
                break;
            }
        }

        {
            std::scoped_lock lock(state_mutex_);
            if (release.has_value() && !answered_by.empty()) {
                // Remember which endpoint answered, so the next launch starts there instead of
                // waiting out the addresses this network cannot reach.
                const std::string used = LauncherEndpointOfUrl(answered_by);
                if (configuration_.update.endpoint != used) {
                    configuration_.update.endpoint = used;
                    settings_dirty_ = true;
                }
            }
            state_.update.http_status = status;
            if (!release.has_value()) {
                state_.update.phase = LauncherUpdatePhase::Failed;
                if (status == 0 && !answered) {
                    state_.update.failure = LauncherUpdateFailure::Network;
                } else if (status == 403 || status == 429) {
                    state_.update.failure = LauncherUpdateFailure::RateLimited;
                } else if (status != 0 && status != 200) {
                    state_.update.failure = LauncherUpdateFailure::Http;
                } else {
                    state_.update.failure = LauncherUpdateFailure::NotRelease;
                }
                return;
            }
            state_.update.remote_tag = release->tag;
            state_.update.remote_version = release->version;
            state_.update.page_url = release->page_url;
            state_.update.notes = release->notes;
            const auto* const asset = anomaly::launcher::FindRuntimeAsset(*release);
            state_.update.asset = asset != nullptr
                ? *asset : anomaly::launcher::ReleaseAsset{};
            switch (anomaly::launcher::CompareRelease(ANOMALY_SDK_VERSION_STRING, *release)) {
            case anomaly::launcher::ReleaseComparison::Newer:
                if (asset != nullptr) {
                    state_.update.phase = LauncherUpdatePhase::Available;
                } else {
                    state_.update.phase = LauncherUpdatePhase::Failed;
                    state_.update.failure = LauncherUpdateFailure::Check;
                    state_.update.detail = "release publishes no verified runtime package for this version";
                }
                break;
            case anomaly::launcher::ReleaseComparison::UpToDate:
                state_.update.phase = LauncherUpdatePhase::UpToDate;
                break;
            case anomaly::launcher::ReleaseComparison::Unknown:
                state_.update.phase = LauncherUpdatePhase::Failed;
                state_.update.failure = LauncherUpdateFailure::Check;
                state_.update.detail = "release version could not be compared with this build";
                break;
            }
        }
    }

    // Downloads the runtime package the check found and publishes it over this installation. The
    // download goes to a directory of its own: two updates never share a half-written archive, and
    // the archive itself is thrown away as soon as it is extracted. The package comes from the
    // release's own address and is verified against the length and digest the release published,
    // because a truncated transfer or someone else's bytes must not reach an extraction.
    void InstallUpdateImpl() {
        anomaly::launcher::ReleaseAsset asset;
        {
            std::scoped_lock lock(state_mutex_);
            if (!state_.update.CanInstall()) return;
            asset = state_.update.asset;
            if (asset.download_url.empty()) return;
            state_.update.phase = LauncherUpdatePhase::Downloading;
            state_.update.failure = LauncherUpdateFailure::None;
            state_.update.detail.clear();
        }
        const auto fail = [this](std::string detail) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Failed;
            state_.update.failure = LauncherUpdateFailure::Install;
            state_.update.detail = std::move(detail);
        };
        std::error_code error;
        GUID download_id{};
        wchar_t download_name[40]{};
        if (FAILED(CoCreateGuid(&download_id)) ||
            StringFromGUID2(download_id, download_name, static_cast<int>(std::size(download_name))) == 0) {
            fail("update download directory could not be allocated");
            return;
        }
        const auto directory = std::filesystem::temp_directory_path(error) /
            (std::wstring(L"anomaly-launcher-update-") + download_name);
        if (error) {
            fail("temporary directory is unavailable");
            return;
        }
        if (!std::filesystem::create_directory(directory, error)) {
            fail("update download directory could not be created");
            return;
        }
        struct DownloadDirectory final {
            std::filesystem::path path;
            ~DownloadDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{directory};
        const auto archive = directory / L"runtime.zip";

        // The package is fetched from the release's own address, once: the gh-proxy mirrors exist to
        // reach the API when github.com does not answer, not to carry the payload, and a package that
        // arrived through a third party would be one more thing to trust. A user whose network cannot
        // reach github.com at all has the netdisk buttons in this dialog for exactly that case.
        anomaly::launcher::PackageVerification verification;
        std::string rejected;
        bool downloaded = LauncherDownloadUrl(asset.download_url, archive, &g_update_download);
        if (downloaded) {
            verification = anomaly::launcher::VerifyReleasePackage(archive, asset);
            if (!verification.Ok()) {
                rejected = std::string("package failed verification (") +
                    (verification.check == anomaly::launcher::PackageCheck::Size
                        ? "size " + std::to_string(verification.size) + " != " +
                            std::to_string(asset.size)
                        : verification.check == anomaly::launcher::PackageCheck::Digest
                            ? "sha256 " + verification.sha256
                            : std::string("unreadable")) + ')';
                LauncherUpdateLog(rejected);
                std::filesystem::remove(archive, error);
                downloaded = false;
            }
        } else {
            LauncherUpdateLog("package download failed: " + asset.download_url);
        }
        if (!downloaded || g_update_download.cancel.load()) {
            if (g_update_download.cancel.load()) {
                // Cancelling is not a failure: the release is still there to install, and the dialog
                // goes back to offering it.
                std::scoped_lock lock(state_mutex_);
                state_.update.phase = LauncherUpdatePhase::Available;
                state_.update.failure = LauncherUpdateFailure::None;
                state_.update.detail.clear();
                return;
            }
            fail(rejected.empty()
                ? std::string("update package could not be downloaded")
                : rejected);
            return;
        }
        LauncherUpdateLog("package verified: " + asset.name + " size=" +
            std::to_string(verification.size) + " sha256=" + verification.sha256);
        {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Installing;
        }
        const auto extracted = anomaly::launcher::ExtractModArchive(archive, &g_update_download.cancel);
        if (!extracted.Ok()) {
            if (extracted.error == anomaly::launcher::ModArchiveError::Cancelled) {
                std::scoped_lock lock(state_mutex_);
                state_.update.phase = LauncherUpdatePhase::Available;
                return;
            }
            fail("update package could not be extracted");
            return;
        }
        const auto published =
            anomaly::launcher::PublishStagedRelease(extracted.directory, payload_root_);
        if (!published.Ok()) {
            fail(published.failure);
            return;
        }
        std::string restart_failure;
        if (!RelaunchUpdatedLauncher(restart_failure)) {
            fail(std::move(restart_failure));
            return;
        }
        std::scoped_lock lock(state_mutex_);
        state_.update.phase = LauncherUpdatePhase::Installed;
        state_.restart_pending = true;
    }

    // The new image is in place, so this process now runs the image that was replaced: it starts
    // the new one and lets the window close. The child inherits this token, which is what keeps an
    // elevated launcher elevated without a second prompt.
    bool RelaunchUpdatedLauncher(std::string& failure) {
        const auto executable = payload_root_ / anomaly::launcher::kLauncherExecutableName;
        // Wait before reading configuration or removing the renamed image. The old controller
        // persists its final settings during destruction, after the window closes.
        std::wstring command = L"\"" + executable.wstring() + L"\" --wait-for-update " +
            std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{.cb = sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_UNICODE_ENVIRONMENT, nullptr, executable.parent_path().c_str(),
                &startup, &process) == FALSE) {
            failure = "new launcher could not be started (Win32 " +
                std::to_string(GetLastError()) + ')';
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    void RefreshProcessesImpl(ProcessAnnounce announce = ProcessAnnounce::Always) {
        auto processes = anomaly::launcher::EnumerateAttachableProcesses();
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            std::erase_if(processes, [&game_directory](const auto& process) {
                return process.executable_path.empty() ||
                    !PathsEqual(process.executable_path.parent_path(), game_directory);
            });
        }
        const auto runtime = ResolveAttachRuntime();
        const bool core_available = runtime.Ok();
        std::scoped_lock lock(state_mutex_);
        const bool had_processes = !state_.processes.empty();
        state_.processes = std::move(processes);
        state_.core_available = core_available;
        state_.runtime_version = runtime.version;
        state_.runtime_message = runtime.message;
        if (std::none_of(
                state_.processes.begin(), state_.processes.end(),
                [this](const auto& process) {
                    return process.process_id == state_.attached_process;
                })) {
            state_.attached_process = 0;
        }
        if (announce == ProcessAnnounce::Never) {
            return;
        }
        // The background poll only speaks when the answer changed, so a list that fills in does not keep
        // claiming that nothing is running.
        if (announce == ProcessAnnounce::OnChange && had_processes == !state_.processes.empty()) {
            return;
        }
        state_.message_scope = MessageScope::Attach;
        if (!core_available) {
            state_.message = runtime.message.empty()
                ? MakeLauncherMessage(anomaly::MessageId::LauncherStatusCoreUnavailable)
                : MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusCoreUnavailable, {},
                    runtime.message);
            state_.message_kind = MessageKind::Error;
        } else if (state_.processes.empty()) {
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusNoProcesses);
            state_.message_kind = MessageKind::Neutral;
        } else if (state_.attached_process != 0) {
            // The status describes the state, not the act of scanning, so a scan that
            // finds the attached process keeps reporting the attachment instead of
            // replacing it with a note that the list was rebuilt.
            const std::string process_id = std::to_string(state_.attached_process);
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusLaunchAttached, {process_id});
            state_.message_kind = MessageKind::Success;
        } else {
            // Opening the launcher while the game already runs lands here. The process
            // and its id are what the mode has to report; a note that the list was
            // rebuilt says nothing about the state it just read.
            const std::string process_id =
                std::to_string(state_.processes.front().process_id);
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusProcessFound, {process_id});
            state_.message_kind = MessageKind::Neutral;
        }
    }

    std::filesystem::path payload_root_;
    std::uint64_t theme_version_{};
    anomaly::launcher::ProxyInstallationSource source_;
    std::filesystem::path configuration_path_;
    mutable std::mutex state_mutex_;
    anomaly::launcher::LauncherConfiguration configuration_;
    LauncherSnapshot state_;
    // Set when a preference changed in memory and the configuration still has to be written.
    // Guarded by state_mutex_, flushed by FlushSettings once the worker is free.
    bool settings_dirty_{};
    bool theme_dirty_{};
    // Set when this launch has done its automatic update check, whether or not it found anything.
    bool auto_check_done_{};
    std::mutex queue_mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Work> queue_;
    // Throttles PollProcesses: a per-frame caller must not turn into a per-frame process scan.
    std::chrono::steady_clock::time_point last_process_poll_{};
    std::jthread update_check_worker_;
    std::string telemetry_attempt_day_;
    std::jthread telemetry_worker_;
    std::jthread worker_;
};

struct Graphics final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    // The window is created without a redirection bitmap, so it draws nothing of its
    // own: the swap chain is bound to a composition visual, and that visual is the
    // window's content. The indirection is what lets the back buffer's alpha reach
    // the desktop, which is what its rounded corners are cut out of.
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<IDCompositionDevice> composition_device;
    ComPtr<IDCompositionTarget> composition_target;
    ComPtr<IDCompositionVisual> composition_visual;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11ShaderResourceView> logo;
    ComPtr<ID3D11ShaderResourceView> mod_placeholder;
    // The placeholder's own pixel size: a grid slot fits it like a real cover, so a 16:9
    // drawing is not stretched to whatever shape the slot happens to have.
    std::uint32_t mod_placeholder_width{};
    std::uint32_t mod_placeholder_height{};
    // The sponsor button's artwork. Its own aspect fills the height the caption gives it, so the
    // button keeps the proportions the platform publishes it with.
    ComPtr<ID3D11ShaderResourceView> sponsor;
    std::uint32_t sponsor_width{};
    std::uint32_t sponsor_height{};
    ComPtr<ID3D11ShaderResourceView> kofi;
    std::uint32_t kofi_width{};
    std::uint32_t kofi_height{};
    ComPtr<ID3D11ShaderResourceView> theme_background;
    std::uint32_t theme_background_width{};
    std::uint32_t theme_background_height{};
    std::vector<ComPtr<ID3D11ShaderResourceView>> theme_stickers;
    std::vector<ImVec2> theme_sticker_sizes;
    std::uint64_t theme_version{};
};

Graphics* g_graphics{};
float g_launcher_dpi_scale{1.0f};
bool g_launcher_dpi_changed{};
bool g_launcher_hotkey_capture{};
std::array<bool, 256> g_launcher_hotkey_down{};

bool IsLauncherHotkeyModifier(const std::uint32_t key) noexcept {
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        key == VK_LCONTROL ||
        key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU;
}

void BeginLauncherHotkeyCapture() {
    g_launcher_hotkey_capture = true;
    for (std::uint32_t key = 0; key <= 0xff; ++key) {
        g_launcher_hotkey_down[key] =
            (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
}

std::optional<std::uint32_t> CaptureLauncherHotkey() {
    if (!g_launcher_hotkey_capture) return std::nullopt;
    for (std::uint32_t key = 8; key <= 0xff; ++key) {
        if (key >= VK_LBUTTON && key <= VK_XBUTTON2) continue;
        // The generic aliases report both physical Shift keys. Skip them so
        // the left/right virtual key, especially VK_RSHIFT, can be captured.
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
        const bool down =
            (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        const bool pressed = down && !g_launcher_hotkey_down[key];
        g_launcher_hotkey_down[key] = down;
        if (!pressed) continue;
        if (key == VK_ESCAPE) {
            g_launcher_hotkey_capture = false;
            return std::nullopt;
        }
        if (IsLauncherHotkeyModifier(key)) continue;
        g_launcher_hotkey_capture = false;
        return key;
    }
    return std::nullopt;
}

float DpiScale(UINT dpi) noexcept {
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / kDefaultDpi;
}

float Scale(float value) noexcept {
    return std::round(value * g_launcher_dpi_scale);
}

ImVec2 Scale(float x, float y) noexcept {
    return ImVec2(Scale(x), Scale(y));
}

float ButtonHeight(float logical_height) noexcept {
    return (std::max)(Scale(logical_height), ImGui::GetFrameHeight());
}

// The content layer of the page: the glass the launcher is built on -- a lens over the pane with the
// material's own rim, gloss and refraction, which is what a card here has always looked like. Its
// glow is switched off, because around a shape with a radius this large the shadow only reads as a
// box in the corner.
anomaly::glass::SurfaceOptions LauncherCardSurface(const float rounding) {
    anomaly::glass::SurfaceOptions surface;
    surface.variant = anomaly::glass::Variant::Regular;
    surface.rounding = rounding;
    surface.scale = g_launcher_dpi_scale;
    surface.shadow = false;
    return surface;
}

void ApplyLauncherDpiScale() noexcept {
    ImGuiIO& io = ImGui::GetIO();
    // This launcher has one baked font. Minimal atlas retention discards its source
    // name, so the host's name-based font selection cannot be used after upload.
    if (io.FontDefault != nullptr && io.FontDefault->FontSize > 0.0f) {
        io.FontDefault->Scale = ue5mem::PlatformUiBaseFontSize() / io.FontDefault->FontSize;
    }
    io.FontGlobalScale = g_launcher_dpi_scale * kLauncherFontScale;
    // The shared style already scales its metrics by FontGlobalScale.
    ue5mem::ApplyPlatformUiStyle();
    // The launcher writes with its own two ink tones rather than the palette's: it
    // paints the body itself under the wallpaper and its veil, and both are chosen for
    // the surface the palette actually has -- see LauncherInk.
    ImGui::GetStyle().Colors[ImGuiCol_Text] = LauncherInk();
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = LauncherMutedInk();
    // No popup in this launcher dims what is behind it. The launcher is a themed surface with a
    // wallpaper and stickers, and a dim veil over all of it reads as the window switching off
    // rather than as a question being asked; the popup itself is frosted glass and already
    // separates from the page. Set here rather than per popup, so a popup added later cannot
    // reintroduce the veil by accident.
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    g_launcher_dpi_changed = false;
}

bool CreateRenderTarget(Graphics& graphics) {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(graphics.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    return SUCCEEDED(graphics.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &graphics.render_target));
}

bool CreateGraphics(HWND window, Graphics& graphics) {
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    // The device is created on its own rather than through
    // D3D11CreateDeviceAndSwapChain: DXGI_SWAP_CHAIN_DESC carries no AlphaMode field,
    // and an alpha channel is what keeps the window's rounded corners transparent.
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &graphics.device, &selected, &graphics.context))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(graphics.device.As(&dxgi_device))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    // A composition swap chain has no output window to inherit a size from, so that
    // pair cannot be left at zero the way a window-bound one allows: DXGI rejects it
    // with DXGI_ERROR_INVALID_CALL and the launcher never reaches a first frame. The
    // size is taken here, and the resize handler keeps it in step from then on.
    RECT client{};
    if (GetClientRect(window, &client) == FALSE) return false;
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>(client.right - client.left);
    description.Height = static_cast<UINT>(client.bottom - client.top);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    // Composition takes sequential rather than discard, and premultiplied is the only
    // alpha mode it accepts.
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> swap_chain;
    if (FAILED(factory->CreateSwapChainForComposition(
            graphics.device.Get(), &description, nullptr, &swap_chain))) {
        return false;
    }
    if (FAILED(DCompositionCreateDevice(
            dxgi_device.Get(), IID_PPV_ARGS(&graphics.composition_device)))) {
        return false;
    }
    if (FAILED(graphics.composition_device->CreateTargetForHwnd(
            window, TRUE, &graphics.composition_target))) {
        return false;
    }
    if (FAILED(graphics.composition_device->CreateVisual(&graphics.composition_visual))) {
        return false;
    }
    if (FAILED(graphics.composition_visual->SetContent(swap_chain.Get()))) return false;
    if (FAILED(graphics.composition_target->SetRoot(graphics.composition_visual.Get()))) {
        return false;
    }
    if (FAILED(graphics.composition_device->Commit())) return false;
    if (FAILED(swap_chain.As(&graphics.swap_chain))) return false;
    return CreateRenderTarget(graphics);
}

// Uploads decoded RGBA8 pixels. The launcher owns its device directly, so there
// is no resource registry to go through.
ComPtr<ID3D11ShaderResourceView> CreateTextureFromPixels(
    Graphics& graphics, const std::span<const std::uint8_t> pixels,
    const std::uint32_t width, const std::uint32_t height) {
    if (graphics.device == nullptr || width == 0 || height == 0 ||
        pixels.size() < static_cast<std::size_t>(width) * height * 4U) {
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * 4U;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(graphics.device->CreateTexture2D(&description, &initial, &texture))) return {};
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(graphics.device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
        return {};
    }
    return view;
}

// Theme artwork arrives at whatever resolution the artist had: the launcher's own sticker is
// 1956 x 2722 and is drawn into roughly 795 x 1106, so it is always *minified*. A single-mip texture
// would be sampled with bilinear minification and sparkle as the window moves, wasting the extra
// resolution, so the full mip chain is generated once here and the renderer picks the level.
ComPtr<ID3D11ShaderResourceView> CreateTextureFromThemeImage(
    Graphics& graphics, const LauncherThemeImage& image, std::uint32_t& width,
    std::uint32_t& height) {
    width = 0;
    height = 0;
    if (graphics.device == nullptr || graphics.context == nullptr || image.width == 0 ||
        image.height == 0 ||
        image.pixels.size() < static_cast<std::size_t>(image.width) * image.height * 4U) {
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.width;
    description.Height = image.height;
    description.MipLevels = 0;  // the whole chain
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(graphics.device->CreateTexture2D(&description, nullptr, &texture))) return {};
    graphics.context->UpdateSubresource(texture.Get(), 0, nullptr, image.pixels.data(),
        image.width * 4U, 0);
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(graphics.device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
        return {};
    }
    graphics.context->GenerateMips(view.Get());
    width = image.width;
    height = image.height;
    return view;
}

// Decodes one embedded picture and uploads it. The decoded size is reported through the
// optional out parameters: a caller that draws the picture into a slot it did not author
// needs the pixel size to crop rather than stretch it.
ComPtr<ID3D11ShaderResourceView> LoadEmbeddedTexture(
    Graphics& graphics, const int resource_id, std::uint32_t* width = nullptr,
    std::uint32_t* height = nullptr) {
    if (width != nullptr) *width = 0;
    if (height != nullptr) *height = 0;
    const HRSRC resource = FindResourceW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (resource == nullptr) return {};
    const HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const void* data = loaded == nullptr ? nullptr : LockResource(loaded);
    if (data == nullptr || size == 0) return {};
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto decoded = anomaly::DecodeUiImageRgba8(std::span(bytes, size));
    if (!decoded) return {};
    auto view = CreateTextureFromPixels(graphics, decoded.image.pixels,
        decoded.image.width, decoded.image.height);
    if (view != nullptr) {
        if (width != nullptr) *width = decoded.image.width;
        if (height != nullptr) *height = decoded.image.height;
    }
    return view;
}

// Uploads a newly published theme payload. Decoding already happened on the
// worker, so the render thread only copies pixels into a device texture.
void UploadLauncherTheme(
    Graphics& graphics, const std::shared_ptr<const LauncherTheme>& theme) {
    if (theme == nullptr || theme->version == graphics.theme_version) return;
    graphics.theme_version = theme->version;
    graphics.theme_background.Reset();
    graphics.theme_background_width = 0;
    graphics.theme_background_height = 0;
    graphics.theme_stickers.clear();
    graphics.theme_sticker_sizes.clear();
    if (theme->HasBackground()) {
        graphics.theme_background = CreateTextureFromThemeImage(
            graphics, theme->background, graphics.theme_background_width,
            graphics.theme_background_height);
    }
    graphics.theme_stickers.reserve(theme->stickers.size());
    graphics.theme_sticker_sizes.reserve(theme->stickers.size());
    for (const LauncherThemeImage& image : theme->stickers) {
        std::uint32_t width{};
        std::uint32_t height{};
        graphics.theme_stickers.push_back(
            CreateTextureFromThemeImage(graphics, image, width, height));
        graphics.theme_sticker_sizes.emplace_back(
            static_cast<float>(width), static_cast<float>(height));
    }
    // The textures are the picture now. Keeping the decoded RGBA8 copies would hold
    // the wallpaper and both stickers resident -- 15 MB -- for a frame that draws
    // from the textures, so nothing reads them again.
    const auto release_pixels = [](const LauncherThemeImage& image) {
        std::vector<std::uint8_t>().swap(image.pixels);
    };
    release_pixels(theme->background);
    for (const LauncherThemeImage& image : theme->stickers) release_pixels(image);
}

// The launcher shares the palette and layout the in-game shell uses, so a theme
// only has to be authored once. The palette is the one the settings page stored, so
// switching it there repaints the window on the next frame rather than on the next
// launch. A palette of "custom" is built from the colours the launcher stores, which are applied
// here every frame: a colour being dragged updates the window without reloading its artwork.
void ApplyLauncherTheme(const std::shared_ptr<const LauncherTheme>& theme,
    const anomaly::PlatformUiCustomColors& custom_colors) {
    if (theme == nullptr) return;
    if (theme->palette == anomaly::PlatformUiPalette::Custom)
        ue5mem::SetPlatformUiCustomColors(custom_colors);
    ue5mem::SetPlatformUiPalette(theme->palette);
    ue5mem::SetPlatformUiSurfaceAlpha(
        theme->HasBackground() ? theme->layout.surface_opacity : 1.0f);
    // Rebuilds the style from the palette just stored, so every control follows the
    // theme rather than the one the previous frame drew with.
    ApplyLauncherDpiScale();
    // These two tones are the launcher's own: it paints the body itself instead of
    // using the shell's panels, so its ink follows the surface lightness.
    ImGui::GetStyle().Colors[ImGuiCol_Text] = LauncherInk();
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = LauncherMutedInk();
}

void DrawLauncherThemeImages(
    Graphics& graphics, const std::shared_ptr<const LauncherTheme>& theme,
    const ImVec2 origin, const ImVec2 size, const bool front) {
    if (theme == nullptr) return;
    const auto& layout = theme->layout;
    const auto tint = [](const anomaly::PlatformUiColor& color, const float alpha) {
        return ImGui::ColorConvertFloat4ToU32(
            ImVec4(color.red, color.green, color.blue, color.alpha * alpha));
    };
    ImDrawList* const list = front ? ImGui::GetForegroundDrawList()
                                   : ImGui::GetWindowDrawList();
    if (list == nullptr) return;
    // The wallpaper and the veil are the shell's own body and stop at its edges, so nothing paints
    // past the canvas and the window's corners stay transparent.
    list->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    const float body_rounding = ImGui::GetStyle().WindowRounding;
    if (!front) {
        // The window contributes no body of its own: it has no redirection bitmap, and
        // every panel fill is empty while a theme is active. The shell therefore paints
        // its own opaque base first -- the layer the in-game shell gets for free from
        // its opaque window. Without it the veil would composite against the desktop
        // instead of against the surface colour, and whatever sits behind the launcher
        // would show through the window.
        list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
            tint(ue5mem::PlatformUiSurfaceColor(), 1.0f), body_rounding);
    }
    if (!front && graphics.theme_background != nullptr &&
        graphics.theme_background_width != 0 && graphics.theme_background_height != 0) {
        const auto placement = anomaly::ComputePlatformUiBackgroundPlacement(
            layout.background.mode,
            static_cast<float>(graphics.theme_background_width),
            static_cast<float>(graphics.theme_background_height),
            origin.x, origin.y, size.x, size.y);
        if (placement.Visible()) {
            const bool covers_body = placement.x <= origin.x + 0.5f &&
                placement.y <= origin.y + 0.5f &&
                placement.x + placement.width >= origin.x + size.x - 0.5f &&
                placement.y + placement.height >= origin.y + size.y - 0.5f;
            list->AddImageRounded(
                static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(graphics.theme_background.Get())),
                ImVec2(placement.x, placement.y),
                ImVec2(placement.x + placement.width, placement.y + placement.height),
                ImVec2(placement.uv0_x, placement.uv0_y),
                ImVec2(placement.uv1_x, placement.uv1_y),
                tint(layout.background.tint, layout.background.opacity),
                covers_body ? body_rounding : 0.0f);
        }
    }
    // The shell paints one translucent veil between its wallpaper and its
    // stickers and leaves every panel fill empty while a theme is active, so the
    // launcher has to paint that same veil: without it nothing stands in for the
    // panels and the window loses its palette instead of showing the theme.
    if (!front) {
        const bool wallpaper = graphics.theme_background != nullptr &&
            layout.background.mode != anomaly::PlatformUiBackgroundMode::Disabled;
        const float veil_alpha =
            wallpaper ? ue5mem::GetPlatformUiSurfaceAlpha() : 1.0f;
        if (veil_alpha > 0.0f) {
            list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                tint(ue5mem::PlatformUiSurfaceColor(), veil_alpha), body_rounding);
        }
    }
    list->PopClipRect();
    // Stickers, unlike the body, are meant to spill: the root window's own clip rect is inset by its
    // padding, which would slice off whatever lies past the canvas and leave the plain surface
    // showing instead. This is what lets a sticker reach the canvas edge.
    list->PushClipRect(
        ImVec2(-8192.0f, -8192.0f), ImVec2(8192.0f, 8192.0f), false);
    if (front) {
        list->PushClipRect(
            origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    }
    for (std::size_t index = 0;
         index < layout.stickers.size() && index < graphics.theme_stickers.size();
         ++index) {
        const auto& sticker = layout.stickers[index];
        if ((sticker.layer == anomaly::PlatformUiStickerLayer::Front) != front) continue;
        if (graphics.theme_stickers[index] == nullptr) continue;
        const ImVec2 image_size = graphics.theme_sticker_sizes[index];
        // The layout stores a width and a height for every sticker, both
        // normalised to a viewport the launcher does not share. Using both scales
        // the two axes independently, which stretches the artwork. Keeping only
        // the width lets the placement derive the height from the image's own
        // aspect, so a sticker keeps the proportions it has in the shell.
        anomaly::PlatformUiSticker fitted = sticker;
        if (fitted.width > 0.0f && fitted.height > 0.0f) fitted.height = 0.0f;
        const auto placement = anomaly::ComputePlatformUiStickerPlacement(
            fitted, image_size.x, image_size.y, size.x, size.y);
        if (!placement.Visible()) continue;
        const ImTextureID texture = static_cast<ImTextureID>(
            reinterpret_cast<std::uintptr_t>(graphics.theme_stickers[index].Get()));
        const ImU32 color = tint(sticker.tint, sticker.opacity);
        const ImVec2 position(origin.x + placement.x, origin.y + placement.y);
        if (placement.rotation == 0.0f) {
            list->AddImage(texture, position,
                ImVec2(position.x + placement.width, position.y + placement.height),
                ImVec2(placement.uv0_x, placement.uv0_y),
                ImVec2(placement.uv1_x, placement.uv1_y), color);
            continue;
        }
        const auto quad = anomaly::ComputePlatformUiImageQuad(placement);
        list->AddImageQuad(texture,
            ImVec2(origin.x + quad.x[0], origin.y + quad.y[0]),
            ImVec2(origin.x + quad.x[1], origin.y + quad.y[1]),
            ImVec2(origin.x + quad.x[2], origin.y + quad.y[2]),
            ImVec2(origin.x + quad.x[3], origin.y + quad.y[3]),
            ImVec2(quad.u[0], quad.v[0]), ImVec2(quad.u[1], quad.v[1]),
            ImVec2(quad.u[2], quad.v[2]), ImVec2(quad.u[3], quad.v[3]), color);
    }
    if (front) list->PopClipRect();
    list->PopClipRect();
}

LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_NCCALCSIZE:
        // The window is frameless and the launcher draws its own title bar, so
        // the client area has to cover the whole window. Leaving the frame in
        // place would show a strip of window background above the caption.
        if (wparam != 0) return 0;
        break;
    case WM_SIZE:
        if (g_graphics != nullptr && g_graphics->swap_chain != nullptr &&
            wparam != SIZE_MINIMIZED) {
            g_graphics->render_target.Reset();
            if (SUCCEEDED(g_graphics->swap_chain->ResizeBuffers(
                    0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0))) {
                static_cast<void>(CreateRenderTarget(*g_graphics));
            }
        }
        return 0;
    case WM_DPICHANGED: {
        g_launcher_dpi_scale = DpiScale(HIWORD(wparam));
        g_launcher_dpi_changed = true;
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            static_cast<void>(SetWindowPos(
                window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER));
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {
            static_cast<LONG>(Scale(720.0f)), static_cast<LONG>(Scale(500.0f))};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::optional<std::filesystem::path> ChooseGameDirectory(
    HWND owner, const std::filesystem::path& current,
    const anomaly::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    const std::wstring title = Utf8Wide(
        translator.Text(anomaly::MessageId::LauncherDialogGameDirectory));
    dialog->SetTitle(title.c_str());
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

// The file name the selected client's official launcher ships under. The two installs are separate programs
// with separate launchers, so a label that names one of them has to name the one this client actually starts.
const char* ClientLauncherFileName(const anomaly::launcher::NteClient client) noexcept {
    return client == anomaly::launcher::NteClient::Global
        ? "NTEGlobalLauncher.exe" : "NTELauncher.exe";
}


// File and network helpers run on worker threads and have no translator parameter, but they still
// produce user-visible text. The UI stores the active translator here each frame; lookups are
// read-only after start-up, so concurrent reads are safe.
// Background downloads also translate their results. Each reader retains its catalog while a
// language switch publishes the replacement.
std::atomic<std::shared_ptr<const anomaly::Translator>> g_translator;
// The language the catalog in hand was built for, so the render loop only reloads on a real change.
anomaly::LanguagePreference g_translator_language{anomaly::LanguagePreference::Auto};

// Loads the host catalog for one language into the launcher's globals. Called at start-up and again
// whenever the settings page changes the language: both locales are already baked into the atlas,
// so a switch costs one JSON parse and nothing else.
bool LoadLauncherCatalog(const anomaly::LanguagePreference preference,
    const std::filesystem::path& locale_directory) {
    const auto locale = anomaly::ResolveUserLocale(preference);
    const auto loaded = anomaly::LoadHostCatalog(locale.locale, locale_directory);
    if (loaded.translator == nullptr) return false;
    g_translator.store(loaded.translator);
    g_translator_language = preference;
    return true;
}

std::string Num(const double value, const int precision) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

std::string Tr(const anomaly::MessageId id) {
    const auto translator = g_translator.load();
    return translator != nullptr ? std::string(translator->Text(id)) : std::string{};
}

std::string Tr(const anomaly::MessageId id, std::initializer_list<std::string_view> arguments) {
    const auto translator = g_translator.load();
    if (translator == nullptr) return {};
    return translator->Format(id, std::span<const std::string_view>(arguments.begin(),
        arguments.size()));
}

std::string Format(
    const anomaly::Translator& translator, anomaly::MessageId id,
    std::initializer_list<std::string_view> arguments) {
    return translator.Format(id, std::span<const std::string_view>(
        arguments.begin(), arguments.size()));
}

std::optional<std::filesystem::path> ChooseLauncherExecutable(
    HWND owner, const std::filesystem::path& current,
    const anomaly::launcher::NteClient client, const anomaly::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST);
    }
    const std::wstring executable_filter = Utf8Wide(
        translator.Text(anomaly::MessageId::LauncherDialogExecutableFilter));
    const wchar_t* launcher_name = client == anomaly::launcher::NteClient::Global
        ? L"NTEGlobalLauncher.exe" : L"NTELauncher.exe";
    const wchar_t* launcher_label = client == anomaly::launcher::NteClient::Global
        ? L"NTE Global Launcher" : L"NTE Launcher";
    const COMDLG_FILTERSPEC filters[]{
        {launcher_label, launcher_name},
        {executable_filter.c_str(), L"*.exe"},
    };
    static_cast<void>(dialog->SetFileTypes(
        static_cast<UINT>(std::size(filters)), filters));
    const std::wstring title = Utf8Wide(Format(
        translator, anomaly::MessageId::LauncherDialogNteLauncher,
        {ClientLauncherFileName(client)}));
    dialog->SetTitle(title.c_str());
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

// Every command in the launcher is one chip of the same material: an invisible item for the hit box,
// the liquid-glass surface behind it and the label centred on top. A command that carries a message
// (destructive, primary) tints that surface instead of falling back to a flat fill, so a row of
// commands still reads as one material rather than as glass next to paint.
struct LauncherChipStyle final {
    ImVec4 tint{};             // alpha 0 keeps the palette's own tint colour
    ImVec4 ink{};              // alpha 0 keeps the palette's text colour
    float tint_strength{-1.0f};
    float rounding{-1.0f};     // negative is a capsule
};

// The part of an ImGui label that is drawn: anything after "##" only names the widget.
std::string_view LauncherChipText(const char* label) noexcept {
    const std::string_view text(label);
    const auto hidden = text.find("##");
    return hidden == std::string_view::npos ? text : text.substr(0, hidden);
}

bool LauncherChip(const char* label, const ImVec2 requested = {}, const bool enabled = true,
    const LauncherChipStyle& style = {}) {
    const std::string caption = Ellipsize(
        LauncherChipText(label), (requested.x > 0.0f ? requested.x : 4096.0f) - Scale(16.0f));
    const ImVec2 caption_size = ImGui::CalcTextSize(caption.c_str());
    ImVec2 size = requested;
    if (size.x <= 0.0f) size.x = caption_size.x + Scale(30.0f);
    if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    // The invisible item is the button: it owns the id, the hit box and the pressed state, which is
    // released on mouse up exactly like the themed button it replaces.
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    // The same glass the panes are made of, one size down: a capsule with the material's rim, gloss
    // and refraction. Its glow is off -- around a shape this small it reads as a box -- and the tint
    // rises while the pointer is on it, which is what tells a control from the pane behind it.
    anomaly::glass::SurfaceOptions surface;
    surface.variant = anomaly::glass::Variant::Regular;
    surface.rounding = style.rounding >= 0.0f ? Scale(style.rounding) : size.y * 0.5f;
    surface.scale = g_launcher_dpi_scale;
    surface.shadow = false;
    surface.minimum_alpha = enabled ? 0.82f : 0.56f;
    float strength = style.tint_strength >= 0.0f
        ? style.tint_strength : (LauncherSurfaceIsLight() ? 0.24f : 0.20f);
    if (ImGui::IsItemActive()) strength += 0.16f;
    else if (hovered && enabled) strength += 0.10f;
    if (!enabled) strength *= 0.55f;
    surface.tint_strength = strength;
    if (style.tint.w > 0.0f) surface.tint = style.tint;
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    anomaly::glass::DrawSurface(draw, min, max, surface);
    if (!caption.empty()) {
        const ImVec4 ink = style.ink.w > 0.0f ? style.ink : LauncherInk();
        const ImVec4 colour = enabled ? ink : ImVec4(ink.x, ink.y, ink.z, ink.w * 0.45f);
        draw->AddText(ImVec2(min.x + (size.x - caption_size.x) * 0.5f,
                          min.y + (size.y - caption_size.y) * 0.5f),
            ImGui::GetColorU32(colour), caption.c_str());
    }
    return pressed && enabled;
}

bool IconButton(const char* id, char32_t glyph, const char* tooltip, bool enabled = true) {
    ImGui::PushID(id);
    const float extent = ButtonHeight(38.0f);
    LauncherChipStyle style;
    style.rounding = extent * 0.5f;
    const bool pressed = LauncherChip("##icon", ImVec2(extent, extent), enabled, style);
    // The mark is centred by hand. These are private-use icon glyphs whose bearing
    // and advance are not symmetric, so leaving the centring to the text layout
    // puts them visibly off centre inside the frame.
    const ImVec2 frame_min = ImGui::GetItemRectMin();
    const ImVec2 frame_max = ImGui::GetItemRectMax();
    const char* const mark = Glyph(glyph);
    const ImVec2 mark_size = ImGui::CalcTextSize(mark);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(frame_min.x + (frame_max.x - frame_min.x - mark_size.x) * 0.5f,
            frame_min.y + (frame_max.y - frame_min.y - mark_size.y) * 0.5f),
        ImGui::GetColorU32(enabled ? LauncherInk() : ImVec4(
            LauncherInk().x, LauncherInk().y, LauncherInk().z, 0.45f)), mark);
    Tooltip(tooltip);
    ImGui::PopID();
    return pressed && enabled;
}

bool CommandButton(
    const char* id, char32_t glyph, const char* label,
    bool primary, bool enabled, ImVec2 size = {}) {
    std::string text = std::string(Glyph(glyph)) + "  " + label;
    ImVec2 scaled_size(
        size.x > 0.0f ? Scale(size.x) : 0.0f,
        ButtonHeight(size.y > 0.0f ? size.y : 30.0f));
    if (scaled_size.x > 0.0f) {
        text = Ellipsize(text, (std::max)(0.0f, scaled_size.x - Scale(16.0f)));
    }
    const auto& theme = ue5mem::PlatformUiTheme();
    LauncherChipStyle style;
    // A primary command keeps the accent colour inside the glass; a secondary one is the plain
    // material, so the two differ by tint rather than by being made of different things.
    if (primary) {
        style.tint = ThemeColor(theme.accent);
        style.tint.w = 1.0f;
        style.tint_strength = 0.86f;
        style.ink = ThemeColor(theme.inverse_text);
    } else {
        style.ink = LauncherInk();
    }
    ImGui::PushID(id);
    const bool pressed = LauncherChip((text + "##command").c_str(), scaled_size, enabled, style);
    ImGui::PopID();
    return pressed && enabled;
}

// A command that destroys something. It is the only button in a row that carries the palette's
// danger colour, so a list of grey commands never gets a destructive click by mistake -- on a
// grid card the same applies, where every other command is a plain chip.
bool DestructiveButton(const char* label, const ImVec2 size, const bool enabled) {
    const auto& theme = ue5mem::PlatformUiTheme();
    LauncherChipStyle style;
    style.tint = ThemeColor(theme.danger);
    style.tint.w = 1.0f;
    style.tint_strength = 0.34f;
    style.ink = ThemeColor(theme.danger);
    return LauncherChip(label, size, enabled, style);
}

// The width a command button needs for its own label, in the same unscaled units CommandButton takes. A
// translation is as long as it is: a fixed width ellipsizes the ones that grew.
float CommandButtonWidth(char32_t glyph, const char* label) {
    const std::string text = std::string(Glyph(glyph)) + "  " + label;
    const float pixels = ImGui::CalcTextSize(text.c_str()).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    const float scale = g_launcher_dpi_scale > 0.0f ? g_launcher_dpi_scale : 1.0f;
    return std::ceil(pixels / scale) + 6.0f;
}

const char* Text(const anomaly::Translator& translator, anomaly::MessageId id) noexcept {
    return translator.Text(id).data();
}

std::string StableLabel(
    const anomaly::Translator& translator, anomaly::MessageId id,
    std::string_view stable_id) {
    return anomaly::StableDisplayLabel(translator.Text(id), stable_id);
}

std::string RenderMessage(
    const anomaly::Translator& translator, const LauncherMessage& message) {
    std::vector<std::string_view> arguments;
    arguments.reserve(message.arguments.size());
    for (const auto& argument : message.arguments) arguments.push_back(argument);
    std::string result = message.arguments.empty()
        ? std::string(translator.Text(message.id))
        : translator.Format(message.id, arguments);
    if (!message.detail.empty()) {
        if (!result.empty()) result.append(": ");
        result.append(message.detail);
    }
    return result;
}

ImVec4 ProxyStateColor(anomaly::launcher::ProxyInstallationState state) {
    const auto& theme = ue5mem::PlatformUiTheme();
    using State = anomaly::launcher::ProxyInstallationState;
    switch (state) {
    case State::Enabled: return ThemeColor(theme.success);
    case State::UpdateAvailable: return ThemeColor(theme.warning);
    case State::Disabled:
    case State::NotInstalled: return LauncherMutedInk();
    case State::Conflict:
    case State::Unavailable: return ThemeColor(theme.danger);
    }
    return LauncherMutedInk();
}

void DrawModes(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    LauncherMode& mode, const anomaly::Translator& translator) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "LauncherModes", ImVec2(0.0f, Scale(kModeHeight)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    const std::array<const char*, 3> labels{
        Text(translator, anomaly::MessageId::LauncherModeLaunch),
        Text(translator, anomaly::MessageId::LauncherModeMods),
        Text(translator, anomaly::MessageId::LauncherModeSettings)};
    anomaly::glass::SegmentedOptions options;
    options.height = 54.0f;
    options.padding_x = 32.0f;
    options.scale = g_launcher_dpi_scale;
    int selected = static_cast<int>(mode);
    if (anomaly::glass::SegmentedControl("launcher-modes", labels, selected, options)) {
        mode = static_cast<LauncherMode>(selected);
    }
    // The client sits right beside the mode, on the same band: it decides which game directory the process
    // list, the proxy install and the launch all read from, and the two installs live in different
    // directories -- a game under the client that is not selected is not in the list at all. The labels are
    // the clients' own names in both locales, so they are not translated.
    ImGui::SameLine(0.0f, Scale(12.0f));
    const std::array<const char*, 2> clients{"CN", "Global"};
    anomaly::glass::SegmentedOptions client_options;
    client_options.height = 54.0f;
    client_options.padding_x = 26.0f;
    client_options.scale = g_launcher_dpi_scale;
    client_options.disabled_mask = snapshot.busy ? 0b11U : 0U;
    int client = snapshot.selected_client == anomaly::launcher::NteClient::Global ? 1 : 0;
    if (anomaly::glass::SegmentedControl("launcher-client", clients, client, client_options)) {
        controller.SelectClient(client == 1
            ? anomaly::launcher::NteClient::Global
            : anomaly::launcher::NteClient::MainlandChina);
    }
    // A release this launcher can install is worth saying on the band, which is the one row that is
    // on screen in every mode. Clicking it asks the same question the automatic check asks -- the
    // dialog with the notes and the two answers -- rather than sending the user off to another page.
    if (snapshot.update.phase == LauncherUpdatePhase::Available &&
        !snapshot.update.remote_version.empty()) {
        ImGui::SameLine(0.0f, Scale(12.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
            (options.height * g_launcher_dpi_scale - ButtonHeight(38.0f)) * 0.5f);
        LauncherChipStyle style;
        // #ffa31a, filled rather than tinted: the badge is the one place in this row that is a colour
        // rather than a material, and the palette's inverse ink keeps the label readable on it.
        style.tint = ImVec4(1.0f, 0.639f, 0.102f, 1.0f);
        style.tint.w = 1.0f;
        style.tint_strength = 0.86f;
        style.ink = ThemeColor(ue5mem::PlatformUiTheme().inverse_text);
        if (LauncherChip(Format(translator, anomaly::MessageId::LauncherUpdateAvailable,
                {snapshot.update.remote_version}).c_str(),
                ImVec2(0.0f, ButtonHeight(38.0f)), true, style)) {
            g_update_prompt_requested = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DrawReadOnlyPath(
    const char* id, const std::filesystem::path& path, float trailing_width,
    const anomaly::Translator& translator) {
    std::string value = path.empty()
        ? std::string(translator.Text(anomaly::MessageId::LauncherNoDirectorySelected))
        : PathUtf8(path);
    ImGui::SetNextItemWidth((std::max)(
        Scale(120.0f), ImGui::GetContentRegionAvail().x - Scale(trailing_width)));
    ImGui::InputText(
        id, value.data(), value.size() + 1,
        ImGuiInputTextFlags_ReadOnly);
}

void DrawStartupSettings(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionSettings));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingMenuToggle));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(190.0f));
    const auto captured = CaptureLauncherHotkey();
    if (captured) controller.SetToggleKey(*captured);
    const std::string label = g_launcher_hotkey_capture
        ? std::string(Text(translator, anomaly::MessageId::LauncherSettingPressKey))
        : VirtualKeyName(snapshot.toggle_key);
    ImGui::BeginDisabled(snapshot.busy);
    if (LauncherChip(label.c_str(), ImVec2(Scale(180.0f), ButtonHeight(38.0f)))) {
        BeginLauncherHotkeyCapture();
    }
    ImGui::EndDisabled();
    if (g_launcher_hotkey_capture) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSettingEscapeHint));
    }
}

// The width a status line has to leave free for the refresh at its right edge, so
// the text beside it wraps before the icon instead of running underneath it.
float StatusRefreshReserve() {
    return ButtonHeight(38.0f) + Scale(16.0f) + Scale(8.0f);
}

// The refresh belongs on a mode's status line, at its right edge: it acts on exactly
// the state shown beside it. On an action row below it followed a right-aligned
// button and was pushed off the panel entirely. Both modes carry one, so the
// placement and the centring live here rather than being repeated.
bool StatusRefreshButton(
    const char* id, const char* tooltip, const bool enabled, const float status_top) {
    const float icon_extent = ButtonHeight(38.0f);
    ImGui::SameLine();
    // Centred against the status line rather than sitting on its baseline, which a
    // frame-height button always overflows.
    ImGui::SetCursorPosY(
        status_top - (icon_extent - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::SetCursorPosX(
        ImGui::GetWindowWidth() - icon_extent - Scale(16.0f));
    return IconButton(id, 0xe72c, tooltip, enabled);
}

void DrawProxyMode(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    using State = anomaly::launcher::ProxyInstallationState;

    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionInstallation));
    ImGui::TextColored(ProxyStateColor(snapshot.proxy.state), "%s",
        Text(translator, ProxyStateMessageId(snapshot.proxy.state)));
    const float status_top = ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y;
    if (StatusRefreshButton(
            "refresh-proxy", Text(translator, anomaly::MessageId::LauncherProxyRefresh),
            !snapshot.busy && !snapshot.game_directory.empty(), status_top)) {
        controller.RefreshProxy();
    }
    if (snapshot.proxy.state == State::UpdateAvailable) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(LauncherMutedInk(), "%s", Text(
            translator, anomaly::MessageId::LauncherProxyUpdateDescription));
        ImGui::PopTextWrapPos();
    }
    if (snapshot.message_kind != MessageKind::Neutral &&
        snapshot.message_scope != MessageScope::Attach) {
        // Install and enable failures used to be reported by the footer, which is
        // gone, so the status block carries them now. Attach outcomes are left out:
        // the controller keeps one message, and without this an attach result
        // described itself here, under a mode that cannot attach.
        const ImVec4 proxy_ink = snapshot.message_kind == MessageKind::Error
            ? ThemeColor(theme.danger)
            : ThemeColor(theme.success);
        const std::string proxy_status = RenderMessage(translator, snapshot.message);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(proxy_ink, "%s", proxy_status.c_str());
        ImGui::PopTextWrapPos();
    }

    const bool installed = snapshot.proxy.state == State::Enabled ||
        snapshot.proxy.state == State::Disabled || snapshot.proxy.state == State::UpdateAvailable;
    const bool can_uninstall = !snapshot.busy && installed && snapshot.processes.empty();
    const bool can_install = !snapshot.busy && !snapshot.game_directory.empty() &&
        (snapshot.proxy.state == State::NotInstalled || snapshot.proxy.state == State::UpdateAvailable);
    ImGui::SetCursorPosY((std::max)(ImGui::GetCursorPosY() + Scale(20.0f),
        ImGui::GetWindowHeight() - Scale(kLaunchActionRowBottom)));
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
        ImGui::GetWindowWidth() - Scale(180.0f * 3.0f + 24.0f) - Scale(16.0f)));
    const std::string popup_name = std::string(Text(
        translator, anomaly::MessageId::LauncherProxyCompleteUninstall)) + "###complete-uninstall-proxy";
    static std::filesystem::path pending_directory;
    if (DestructiveButton(Text(translator, anomaly::MessageId::LauncherProxyCompleteUninstall),
            ImVec2(Scale(180.0f), ButtonHeight(62.0f)),
            can_uninstall)) {
        pending_directory = snapshot.game_directory;
        ImGui::OpenPopup(popup_name.c_str());
    }
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (CommandButton("install-proxy", 0xe8b7,
            Text(translator, snapshot.proxy.state == State::NotInstalled
                ? anomaly::MessageId::CommonInstall : anomaly::MessageId::CommonUpdate),
            true, can_install, ImVec2(180.0f, 62.0f))) {
        controller.InstallProxy();
    }
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (CommandButton("toggle-proxy", snapshot.proxy.proxy_enabled ? 0xe711 : 0xe768,
            Text(translator, snapshot.proxy.proxy_enabled
                ? anomaly::MessageId::CommonDisable : anomaly::MessageId::CommonEnable),
            false, !snapshot.busy && installed, ImVec2(180.0f, 62.0f))) {
        controller.SetProxyEnabled(!snapshot.proxy.proxy_enabled);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(Scale(540.0f, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(20.0f, 20.0f));
    if (ImGui::BeginPopupModal(popup_name.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherProxyConfirmUninstall));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", PathUtf8(pending_directory).c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherProxyUninstallHint));
        ImGui::Spacing();
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
            ImGui::GetContentRegionMax().x - Scale(312.0f)));
        if (LauncherChip(Text(translator, anomaly::MessageId::CommonCancel), Scale(120.0f, 42.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine(0.0f, Scale(12.0f));
        if (DestructiveButton(Text(translator, anomaly::MessageId::LauncherProxyCompleteUninstall),
                Scale(180.0f, 42.0f), can_uninstall && pending_directory == snapshot.game_directory)) {
            controller.UninstallProxy();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Hot selections change configuration; compatibility selections change filenames before startup.
// A mod's place in the mods page classification: the group its containers replace ("Player_051", a
// vehicle model, a content folder) and the skins inside that group ("female",
// "female_fashion4"). `skins` is empty for a mod that replaces the group as a whole, and holds
// every variant otherwise, so a mod covering two outfits of one character is listed once per
// outfit while the character stays a single category.
struct LauncherModGroup final {
    std::string name;
    std::vector<std::string> skins;
};
struct LauncherModEntry final {
    std::string name;          // folder name or loose-file base name
    std::string key;           // exact catalog key, including a loose container's _P suffix
    std::string display_name;  // mod.json "name" when present
    std::string author;
    std::string version;
    std::string link;          // mod.json "modLink" / "link" / "supportLink"
    std::string custom_name;   // user override stored in mod-notes.json
    std::string custom_group;  // user override stored in mod-notes.json
    // What the mod's own metadata calls the character or category. The container index is what
    // classifies the mod, so this is kept apart from `groups`: it is a name a mod author wrote, which
    // is what a category the name table does not know yet can be named with.
    std::string declared_category;
    std::vector<LauncherModGroup> groups;  // container index classification, two levels deep
    std::string notes;         // free-form note stored in mod-notes.json
    std::string remote_version;  // filled by the GameBanana update check
    std::filesystem::path path;
    std::filesystem::path icon_path;  // package directory for source metadata
    std::filesystem::path cover;
    std::filesystem::file_time_type cover_stamp{};
    bool directory{};
    bool enabled{true};
    bool selected{};           // batch selection, kept across refreshes by name
    std::uint64_t bytes{};
    std::size_t files{};
};

// Mod icons are decoded once and kept for the session: a mod folder only needs its icon.png read
// the first time the page lists it.
struct LauncherModIconCache {
    std::filesystem::file_time_type stamp{};
    ComPtr<ID3D11ShaderResourceView> texture;
    std::uint32_t width{};
    std::uint32_t height{};
};
std::map<std::filesystem::path, LauncherModIconCache> g_mod_icons;
// Remote versions learned by the update check, keyed by mod name: a refresh rebuilds the
// entries, and without this cache the "new version" marker would disappear with them.
std::map<std::string, std::string> g_mod_remote_versions;
// The force-load switch belongs to the row of page-wide switches at the top of the mods page, while
// the list it changes is rebuilt further down. Flipping it therefore asks the next draw of that list
// to re-classify its rows at once, instead of leaving the conflict marks stale until the timer runs.
bool g_mods_refresh_requested{};

// Category names are shipped data rather than UI text: a category token is a container identifier
// the classification and the conflict checks match on, so the table names it without renaming it.
// The shipped table is read first and `config/mod-groups.json` after it, which is how a new
// character or a car gets its name -- and its folder -- without an update to the package.
anomaly::ModGroupNames g_mod_group_names;

anomaly::ModGroupLanguage LauncherModGroupLanguage() {
    const auto translator = g_translator.load();
    return translator != nullptr && translator->locale() == anomaly::Locale::ZhCn
        ? anomaly::ModGroupLanguage::ZhCn : anomaly::ModGroupLanguage::EnUs;
}

void LoadLauncherModGroupNames() {
    const auto root = ExecutableDirectory() / L"Anomaly";
    for (const auto& path : {root / L"assets" / L"mod-groups" / L"mod-groups.json",
             // Written by the in-game name export: the names the game itself ships for the actors the
             // containers are classified by. It is read before the user's own file, so a curated name
             // still wins, and a game update renames categories without an edit here.
             root / L"state" / L"plugins" / L"anomaly.mod-name-export" / L"mod-group-names.json",
             root / L"config" / L"mod-groups.json"}) {
        std::ifstream stream(path);
        if (!stream) continue;
        const auto document = nlohmann::json::parse(stream, nullptr, false);
        if (document.is_discarded()) continue;
        g_mod_group_names.Load(document);
    }
}

// Decoding an image (WIC) and creating its D3D11 texture happen on the render thread; doing that
// for a whole page at once stutters. One image per frame keeps the UI smooth.
int g_decode_budget{1};

const LauncherModIconCache* LauncherModIcon(const LauncherModEntry& entry) {
    if (g_graphics == nullptr || entry.cover.empty()) return nullptr;
    const auto cached = g_mod_icons.find(entry.cover);
    if (cached != g_mod_icons.end() && cached->second.stamp == entry.cover_stamp)
        return &cached->second;
    if (g_decode_budget <= 0) return nullptr;
    --g_decode_budget;
    auto& value = g_mod_icons[entry.cover];
    value.stamp = entry.cover_stamp;
    value.texture.Reset();
    std::ifstream stream(entry.cover, std::ios::binary);
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (!bytes.empty()) {
        const auto decoded = anomaly::DecodeUiImageRgba8(bytes);
        if (decoded) {
            value.texture = CreateTextureFromPixels(*g_graphics, decoded.image.pixels,
                decoded.image.width, decoded.image.height);
            value.width = decoded.image.width;
            value.height = decoded.image.height;
        }
    }
    return &value;
}

// The shape of a grid card's cover slot, as a fraction of the card width. One shape for every
// card, because a grid whose cells change height as their pictures are decoded is unreadable:
// 4:3 is the compromise that leaves only a narrow band around either a 16:9 screenshot or a
// portrait cover, so nothing is cropped and no slot is mostly empty. The picture itself is
// fitted whole and centred inside it.
constexpr float kLauncherModCoverAspect = 3.0f / 4.0f;
// What a mod without a cover shows on every palette that is not Naiwa: the font's own picture
// mark, in the palette's muted ink. It is the glyph the rest of the interface draws its icons
// with, so the slot reads as "no picture here" in exactly the same hand as everything around it,
// and it is drawn at roughly one icon's size rather than as artwork of its own.
void DrawLauncherMissingCoverMark(
    ImDrawList* draw_list, const ImVec2 slot_min, const ImVec2 slot_size) {
    ImFont* const font = ImGui::GetFont();
    if (font == nullptr) return;
    const float size = (std::min)(ImGui::GetFontSize() * 1.25f,
        (std::min)(slot_size.x, slot_size.y) * 0.42f);
    if (size <= 0.0f) return;
    const char* const glyph = Glyph(0xeb9f);
    const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph);
    ImVec4 ink = LauncherMutedInk();
    ink.w = 0.55f;
    draw_list->AddText(font, size,
        ImVec2(slot_min.x + (slot_size.x - measured.x) * 0.5f,
            slot_min.y + (slot_size.y - measured.y) * 0.5f),
        ImGui::GetColorU32(ink), glyph);
}

// Draws a mod's cover inside a slot: whole, centred, aspect preserved, and on a placeholder until
// the icon has been decoded. A grid cell is the same slot a list row uses, just wider, so nothing
// is cropped to fill it: the artwork is shown as the author drew it.
void DrawLauncherModCoverImage(
    const LauncherModEntry& entry, const ImVec2 slot_min, const ImVec2 slot_size) {
    if (g_graphics == nullptr || slot_size.x <= 0.0f || slot_size.y <= 0.0f) return;
    ImTextureID texture{};
    float image_width = 0.0f;
    float image_height = 0.0f;
    if (const auto* icon = LauncherModIcon(entry); icon != nullptr && icon->texture != nullptr) {
        texture = reinterpret_cast<ImTextureID>(icon->texture.Get());
        image_width = static_cast<float>(icon->width);
        image_height = static_cast<float>(icon->height);
    } else if (ue5mem::GetPlatformUiPalette() == ue5mem::PlatformUiPalette::Naiwa &&
        g_graphics->mod_placeholder_width != 0 && g_graphics->mod_placeholder_height != 0) {
        // Naiwa is the palette that artwork was drawn for, so it is that palette's placeholder.
        // Every other palette draws the generic mark below instead of wearing another theme's
        // character.
        texture = reinterpret_cast<ImTextureID>(g_graphics->mod_placeholder.Get());
        image_width = static_cast<float>(g_graphics->mod_placeholder_width);
        image_height = static_cast<float>(g_graphics->mod_placeholder_height);
    }
    if (texture == 0 || image_width <= 0.0f || image_height <= 0.0f) {
        DrawLauncherMissingCoverMark(ImGui::GetWindowDrawList(), slot_min, slot_size);
        return;
    }
    const float scale = (std::min)(slot_size.x / image_width, slot_size.y / image_height);
    const ImVec2 image_size(image_width * scale, image_height * scale);
    const ImVec2 top_left(slot_min.x + (slot_size.x - image_size.x) * 0.5f,
        slot_min.y + (slot_size.y - image_size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddImage(texture, top_left,
        ImVec2(top_left.x + image_size.x, top_left.y + image_size.y));
}

void DrawLauncherModCover(const LauncherModEntry& entry) {
    const ImVec2 cover_size(Scale(160.0f), Scale(90.0f));
    ImGui::Dummy(cover_size);
    if (!ImGui::IsItemVisible()) return;
    DrawLauncherModCoverImage(entry, ImGui::GetItemRectMin(), cover_size);
}

// A cover slot: a faint rounded fill, so a picture that does not fill the slot is framed
// instead of floating in what looks like empty card, and then the picture itself, whole and
// centred.
void DrawLauncherModCoverSlot(
    const LauncherModEntry& entry, const ImVec2 slot_min, const ImVec2 slot_size) {
    const ImVec2 slot_max(slot_min.x + slot_size.x, slot_min.y + slot_size.y);
    ImGui::GetWindowDrawList()->AddRectFilled(slot_min, slot_max,
        ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.05f)), Scale(8.0f));
    DrawLauncherModCoverImage(entry, slot_min, slot_size);
}

// The same cover in a grid cell, where the slot is a button: in the grid the cover is the
// way into a mod's own details, because the row of buttons a list entry carries has no room
// inside a card.
bool LauncherModCoverCard(const LauncherModEntry& entry, const ImVec2 size) {
    ImGui::InvisibleButton("##cover", size);
    const bool clicked = ImGui::IsItemClicked();
    if (ImGui::IsItemVisible()) {
        DrawLauncherModCoverSlot(entry, ImGui::GetItemRectMin(), size);
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}




std::wstring LauncherWiden(const std::string& utf8);

constexpr wchar_t kLauncherModSourceFile[] = L"anomaly-source.json";

struct LauncherModSource final {
    std::string mod_id;
    std::string file_id;
    std::string file_md5;
    std::string author;        // recorded at download time, so rows can show it without mod.json
    std::string display_name;
};

LauncherModSource LauncherReadModSource(const std::filesystem::path& folder) {
    if (folder.empty()) return {};
    std::ifstream stream(folder / kLauncherModSourceFile);
    if (!stream) return {};
    const auto source = nlohmann::json::parse(stream, nullptr, false);
    if (!source.is_object()) return {};
    try {
        if (source.at("schemaVersion") != 1 || source.at("provider") != "gamebanana") return {};
        return {source.at("modId").get<std::string>(), source.at("fileId").get<std::string>(),
            source.value("fileMd5", std::string{}), source.value("author", std::string{}),
            source.value("displayName", std::string{})};
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

// User notes are separate from the download source and survive source updates.
std::filesystem::path LauncherModNotesPath(const LauncherModEntry& entry) {
    return entry.directory ? entry.path / L"mod-notes.json"
        : entry.path.parent_path() / LauncherWiden(entry.name + ".mod-notes.json");
}

bool LauncherWriteModNotes(const LauncherModEntry& entry, const std::string& display_name,
    const std::string& notes, const std::string& group) {
    std::ofstream out(LauncherModNotesPath(entry));
    out << "{\n  \"displayName\": " << nlohmann::json(display_name).dump()
        << ",\n  \"notes\": " << nlohmann::json(notes).dump()
        << ",\n  \"group\": " << nlohmann::json(group).dump() << "\n}\n";
    out.close();
    return static_cast<bool>(out);
}

void LauncherReadModNotes(LauncherModEntry& entry) {
    std::ifstream stream(LauncherModNotesPath(entry));
    if (!stream) return;
    const auto parsed = nlohmann::json::parse(stream, nullptr, false);
    if (!parsed.is_object()) return;
    entry.custom_name = parsed.value("displayName", std::string{});
    entry.notes = parsed.value("notes", std::string{});
    entry.custom_group = parsed.value("group", std::string{});
}


bool LauncherGameRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (_wcsicmp(entry.szExeFile, L"HTGame.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return running;
}

std::filesystem::path LauncherModsDirectory(const LauncherSnapshot& snapshot) {
    const auto& root = snapshot.game_directory;
    if (root.empty()) return {};
    std::error_code error;
    const std::array<std::filesystem::path, 4> candidates{
        root / L"Client" / L"WindowsNoEditor" / L"HT" / L"Content" / L"Paks",
        root / L"WindowsNoEditor" / L"HT" / L"Content" / L"Paks",
        root / L"HT" / L"Content" / L"Paks",
        root.lexically_normal().parent_path().parent_path() / L"Content" / L"Paks"};
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_directory(candidate, error)) return candidate / L"~mods";
    }
    return candidates[0] / L"~mods";
}


// path::string() converts through the ANSI code page, which turns Chinese mod names into "?".
std::string LauncherNarrow(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length,
        nullptr, nullptr);
    return utf8;
}

std::string LauncherStripDisabled(std::string name) {
    constexpr std::string_view kSuffix = ".disabled";
    if (name.size() > kSuffix.size() &&
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        name.resize(name.size() - kSuffix.size());
    }
    return name;
}

std::string LauncherModBaseName(const std::string& file_name) {
    std::string base = LauncherStripDisabled(file_name);
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    return base;
}

bool LauncherIsModFile(const std::filesystem::path& path) {
    // A disabled mod is "name.pak.disabled": strip the marker before looking at the extension,
    // otherwise it is filtered out and vanishes from the list.
    std::string extension =
        std::filesystem::path(LauncherWiden(LauncherStripDisabled(LauncherNarrow(path.filename())))).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".pak" || extension == ".utoc" || extension == ".ucas" || extension == ".sig";
}

// Minimal "field" reader for the small mod.json files mod authors ship. The launcher runs before
// the JSON service exists, and the schema only needs flat strings.

// JSON strings escape "/" as "\/" and may carry \uXXXX sequences; the raw slice would keep those
// escapes and produce unusable URLs (WinHttpCrackUrl then answers 12005).
std::string LauncherJsonUnescape(const std::string& text) {
    const auto decoded = nlohmann::json::parse('"' + text + '"', nullptr, false);
    return decoded.is_string() ? decoded.get<std::string>() : std::string{};
}

std::string LauncherJsonField(const std::string& text, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto key_at = text.find(needle);
    if (key_at == std::string::npos) return {};
    const auto colon = text.find(':', key_at + needle.size());
    if (colon == std::string::npos) return {};
    auto at = text.find_first_not_of(" \t\r\n", colon + 1);
    if (at == std::string::npos) return {};
    if (text[at] != '"') return {};
    // The closing quote has to skip escaped quotes inside the value.
    std::size_t end = at + 1;
    while (end < text.size()) {
        if (text[end] == '\\' && end + 1 < text.size()) { end += 2; continue; }
        if (text[end] == '"') break;
        ++end;
    }
    if (end >= text.size()) return {};
    return LauncherJsonUnescape(text.substr(at + 1, end - at - 1));
}

void LauncherReadModJson(const std::filesystem::path& folder, LauncherModEntry& entry) {
    std::ifstream stream(folder / L"mod.json");
    if (!stream) return;
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto name = LauncherJsonField(text, "name");
    if (!name.empty()) entry.display_name = name;
    const auto author = LauncherJsonField(text, "author");
    if (!author.empty()) entry.author = author;
    const auto version = LauncherJsonField(text, "version");
    if (!version.empty()) entry.version = version;
    const auto character = LauncherJsonField(text, "character");
    const auto category = LauncherJsonField(text, "category");
    const auto group = !character.empty() ? character : category;
    if (!group.empty()) entry.groups = {{group, {""}}};
    for (const char* key : {"modLink", "link", "supportLink"}) {
        const auto link = LauncherJsonField(text, key);
        if (!link.empty()) {
            entry.link = link;
            break;
        }
    }
}

std::vector<std::filesystem::path> LauncherPakConfigCandidates(const LauncherSnapshot& snapshot);

nlohmann::json LauncherPakSettings(const LauncherSnapshot& snapshot) {
    for (const auto& path : LauncherPakConfigCandidates(snapshot)) {
        if (std::filesystem::exists(path))
            return anomaly::ReadPakModSettings(path.parent_path().parent_path());
    }
    return nlohmann::json::object();
}

struct LauncherModSettingsCache final {
    std::filesystem::path game_directory;
    anomaly::launcher::NteClient client{anomaly::launcher::NteClient::MainlandChina};
    double refreshed_at{};
    bool initialized{};
    bool compatibility{true};
    bool bypass{};
    bool force_load{};
};

LauncherModSettingsCache& RefreshLauncherModSettings(
    const LauncherSnapshot& snapshot, const bool force = false,
    const nlohmann::json* loaded_settings = nullptr) {
    static LauncherModSettingsCache cache;
    const double now = ImGui::GetTime();
    if (!force && cache.initialized && cache.game_directory == snapshot.game_directory &&
        cache.client == snapshot.selected_client && now - cache.refreshed_at < 2.0) {
        return cache;
    }
    cache.game_directory = snapshot.game_directory;
    cache.client = snapshot.selected_client;
    cache.refreshed_at = now;
    cache.initialized = true;
    try {
        const auto settings = loaded_settings != nullptr
            ? *loaded_settings : LauncherPakSettings(snapshot);
        cache.compatibility = anomaly::PakModCompatibilityMode(settings);
        cache.bypass = settings.value("enabled", true);
        cache.force_load = anomaly::PakModForceLoad(settings);
    } catch (...) {
        cache.compatibility = true;
        cache.bypass = false;
        cache.force_load = false;
    }
    return cache;
}

bool LauncherCompatibilityMode(const LauncherSnapshot& snapshot) {
    try {
        return anomaly::PakModCompatibilityMode(LauncherPakSettings(snapshot));
    } catch (...) {
        return true;
    }
}

std::string LauncherModStateKey(const LauncherModEntry& entry) {
    if (!entry.key.empty()) return entry.key;
    return LauncherStripDisabled(entry.name) + (entry.directory ? "/" : ".pak");
}

void LauncherFindModCover(LauncherModEntry& entry, const std::filesystem::path& file) {
    auto extension = file.extension().wstring();
    auto stem = file.stem().wstring();
    for (auto* text : {&extension, &stem})
        std::transform(text->begin(), text->end(), text->begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (extension != L".png" && extension != L".jpg" && extension != L".jpeg" && extension != L".webp") return;
    if (entry.directory) {
        if (stem != L"icon" && stem != L"cover" && stem != L"preview" && stem != L"thumbnail") return;
    } else {
        auto name = LauncherWiden(entry.name);
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (stem != name) return;
    }
    if (!entry.cover.empty() && !(stem == L"icon" && file.parent_path() == entry.path)) return;
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(file, error);
    if (!error) { entry.cover = file; entry.cover_stamp = stamp; }
}

void LauncherRefreshMods(
    const LauncherSnapshot& snapshot, std::vector<LauncherModEntry>& mods,
    const nlohmann::json* loaded_settings = nullptr) {
    mods.clear();
    const auto directory = LauncherModsDirectory(snapshot);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return;
    struct Loose final {
        std::string base;
        std::string key;
        std::uint64_t bytes{};
        std::size_t files{};
        bool disabled{};
    };
    struct ContainerFile final {
        std::string logical;
        std::filesystem::path path;
    };
    std::vector<ContainerFile> containers;
    anomaly::PakModLayout layout;
    // The tree is read once before anything is classified: which folder a container belongs to is
    // the same question for a catalogue key and for this list, so `PakModLayout` answers it for
    // both. Without it a folder holding several mods would collapse into one entry named after it,
    // and the key the selection is stored under would not be the catalogue's key.
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory, error)) {
        if (!item.is_regular_file(error)) continue;
        std::string logical;
        for (const auto& part : item.path().lexically_relative(directory)) {
            if (!logical.empty()) logical += '/';
            logical += LauncherStripDisabled(LauncherNarrow(part));
        }
        if (LauncherIsModFile(item.path())) {
            layout.AddOwnerFile(logical);
            containers.push_back({std::move(logical), item.path()});
        } else if (anomaly::PakModShippedFile(logical)) {
            layout.AddOwnerFile(logical);
        }
    }
    std::vector<Loose> loose;
    // One folder per mod, keyed by its path relative to the mods directory.
    std::map<std::string, std::filesystem::path> roots;
    for (const auto& container : containers) {
        const std::string name = LauncherNarrow(container.path.filename());
        if (container.logical.find('/') == std::string::npos) {
            const std::string base = LauncherModBaseName(container.logical);
            auto found = std::find_if(loose.begin(), loose.end(),
                [&base](const Loose& value) { return value.base == base; });
            if (found == loose.end()) {
                auto key = std::filesystem::path(LauncherWiden(container.logical));
                key.replace_extension(L".pak");
                loose.push_back(Loose{base, LauncherNarrow(key), 0, 0,
                    LauncherStripDisabled(name) != name});
                found = loose.end() - 1;
            }
            found->bytes += std::filesystem::file_size(container.path, error);
            ++found->files;
            continue;
        }
        const auto root = layout.Root(container.logical);
        if (root.empty() || roots.contains(root)) continue;
        // The folder a mod's key names is reached from the container's real path, so a folder whose
        // own name carries the `.disabled` marker still resolves to a folder that exists.
        const auto depth = static_cast<std::size_t>(std::count(root.begin(), root.end(), '/')) + 1;
        auto folder = directory;
        std::size_t level{};
        for (const auto& part : container.path.lexically_relative(directory)) {
            if (level++ == depth) break;
            folder /= part;
        }
        roots.emplace(root, std::move(folder));
    }
    for (const auto& [relative, folder] : roots) {
        LauncherModEntry entry{};
        entry.name = LauncherNarrow(folder.filename());
        entry.key = relative + "/";
        entry.display_name = LauncherStripDisabled(entry.name);
        entry.path = folder;
        entry.directory = true;
        entry.enabled = LauncherStripDisabled(entry.name) == entry.name;
        {
            // A folder counts as disabled when every container inside carries the marker.
            bool has_container = false;
            bool all_disabled = true;
            std::error_code scan;
            for (const auto& file : std::filesystem::recursive_directory_iterator(folder, scan)) {
                if (!file.is_regular_file(scan)) continue;
                const std::string inner = LauncherNarrow(file.path().filename());
                std::string lower = inner;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool container = lower.ends_with(".pak") || lower.ends_with(".utoc") ||
                    lower.ends_with(".ucas");
                const bool disabled = lower.ends_with(".disabled");
                if (container || disabled) has_container = has_container || container || disabled;
                if (container) all_disabled = false;
            }
            if (has_container) entry.enabled = entry.enabled && !all_disabled;
        }
        entry.icon_path = folder;
        LauncherReadModJson(folder, entry);
        LauncherReadModNotes(entry);
        if (entry.author.empty() || entry.display_name.empty()) {
            const auto source = LauncherReadModSource(folder);
            if (entry.author.empty()) entry.author = source.author;
            if (entry.display_name.empty()) entry.display_name = source.display_name;
        }
        if (!entry.custom_name.empty()) entry.display_name = entry.custom_name;
        if (const auto cached = g_mod_remote_versions.find(LauncherModStateKey(entry));
            cached != g_mod_remote_versions.end()) {
            entry.remote_version = cached->second;
        }
        // Mods installed from the browse page have no mod.json; their source record still knows
        // the GameBanana id, which is enough to offer the "来源" button.
        const auto source = LauncherReadModSource(folder);
        if (entry.link.empty() && !source.mod_id.empty())
            entry.link = "https://gamebanana.com/mods/" + source.mod_id;
        for (const auto& file : std::filesystem::recursive_directory_iterator(folder, error)) {
            if (!file.is_regular_file(error)) continue;
            LauncherFindModCover(entry, file.path());
            ++entry.files;
            entry.bytes += file.file_size(error);
        }
        mods.push_back(std::move(entry));
    }
    for (const auto& group : loose) {
        LauncherModEntry entry{};
        entry.name = group.base;
        entry.key = group.key;
        entry.display_name = group.base;
        entry.path = directory / LauncherWiden(group.key);
        entry.enabled = !group.disabled;
        entry.bytes = group.bytes;
        entry.files = group.files;
        LauncherReadModNotes(entry);
        if (!entry.custom_name.empty()) entry.display_name = entry.custom_name;
        for (const auto* extension : {L".png", L".jpg", L".jpeg", L".webp"}) {
            const auto cover = directory / (LauncherWiden(group.base) + extension);
            if (std::filesystem::is_regular_file(cover, error)) LauncherFindModCover(entry, cover);
        }
        if (const auto cached = g_mod_remote_versions.find(LauncherModStateKey(entry));
            cached != g_mod_remote_versions.end()) {
            entry.remote_version = cached->second;
        }
        mods.push_back(std::move(entry));
    }
    try {
        const auto settings = loaded_settings ? *loaded_settings : LauncherPakSettings(snapshot);
        static anomaly::PakModCatalogCache catalog_cache;
        const auto catalog = catalog_cache.Read(directory, settings);
        std::erase_if(mods, [&](const auto& entry) {
            const auto key = anomaly::NormalizePakModKey(LauncherModStateKey(entry));
            return std::any_of(catalog->begin(), catalog->end(), [&](const auto& row) {
                return row.key == key && row.restricted;
            });
        });
        for (auto& entry : mods) {
            const auto key = anomaly::NormalizePakModKey(LauncherModStateKey(entry));
            for (const auto& row : *catalog) if (anomaly::NormalizePakModKey(row.key) == key) {
                if (!anomaly::PakModCompatibilityMode(settings)) entry.enabled = row.enabled;
                if (row.resource_groups.empty()) continue;
                // One group per character, with the outfits it covers inside it: a mod that
                // replaces two outfits names both skins and stays in the one character group.
                entry.groups.clear();
                for (const auto& token : row.resource_groups) {
                    const auto parsed = anomaly::ParsePakModGroup(token);
                    auto found = std::find_if(entry.groups.begin(), entry.groups.end(),
                        [&](const LauncherModGroup& group) { return group.name == parsed.group; });
                    if (found == entry.groups.end()) {
                        entry.groups.push_back({std::string(parsed.group), {}});
                        found = entry.groups.end() - 1;
                    }
                    const std::string skin(parsed.skin);
                    if (std::find(found->skins.begin(), found->skins.end(), skin) == found->skins.end())
                        found->skins.push_back(skin);
                }
                for (auto& group : entry.groups)
                    std::sort(group.skins.begin(), group.skins.end());
                std::sort(entry.groups.begin(), entry.groups.end(),
                    [](const LauncherModGroup& left, const LauncherModGroup& right) {
                        return left.name < right.name;
                    });
            }
        }
    } catch (...) {
        // Do not expose entries whose resource restrictions could not be determined.
        mods.clear();
    }
    std::erase_if(mods, [](const auto& entry) {
        return anomaly::PakModNameRestricted(entry.key) || anomaly::PakModNameRestricted(entry.display_name);
    });
    for (auto& entry : mods) {
        // A hand-written group replaces the automatic one: nothing can tell two classes of the
        // same name apart, and a renamed character has to keep all of its mods together.
        if (!entry.custom_group.empty()) entry.groups = {{entry.custom_group, {""}}};
    }
    std::sort(mods.begin(), mods.end(),
        [](const LauncherModEntry& left, const LauncherModEntry& right) {
            const std::string_view left_group =
                left.groups.empty() ? std::string_view{} : left.groups.front().name;
            const std::string_view right_group =
                right.groups.empty() ? std::string_view{} : right.groups.front().name;
            if (left_group != right_group) return left_group < right_group;
            return left.display_name < right.display_name;
        });
}

// A safe extraction directory does not make an existing installation safe to overwrite:
// junctions or file links under ~mods could redirect writes or renames outside the mod tree.
bool LauncherModTreeSafe(const LauncherSnapshot& snapshot, const LauncherModEntry& entry) {
    try {
        const auto root = std::filesystem::absolute(LauncherModsDirectory(snapshot)).lexically_normal();
        const auto relative = std::filesystem::absolute(entry.path).lexically_normal().lexically_relative(root);
        if (!anomaly::ValidatePluginPackageRelativePath(LauncherNarrow(relative)).Ok()) return false;
        const auto ordinary = [](const std::filesystem::path& path) {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const auto error = GetLastError();
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            }
            return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        };
        auto path = root;
        if (!ordinary(path)) return false;
        for (const auto& part : relative) {
            path /= part;
            if (!ordinary(path)) return false;
        }
        if (entry.directory && std::filesystem::exists(path))
            for (const auto& file : std::filesystem::recursive_directory_iterator(path))
                if (!ordinary(file.path())) return false;
        return true;
    } catch (...) { return false; }
}

bool LauncherSetModFilesEnabled(const LauncherSnapshot& snapshot, const LauncherModEntry& entry,
    const bool enabled) {
    if (!LauncherModTreeSafe(snapshot, entry)) return false;
    std::error_code error;
    const auto directory = LauncherModsDirectory(snapshot);
    if (!std::filesystem::is_directory(directory, error)) return false;
    if (entry.directory) {
        // Renaming the folder does nothing: the engine scans Paks recursively, so a `.disabled`
        // folder is still mounted. Toggle the container file names instead.
        try {
            for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path, error)) {
                if (!file.is_regular_file(error)) continue;
                const std::wstring full = file.path().wstring();
                const std::wstring name = file.path().filename().wstring();
                std::wstring lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](const wchar_t c) { return static_cast<wchar_t>(c >= L'A' && c <= L'Z' ? c + 32 : c); });
                const bool is_container = lower.ends_with(L".pak") || lower.ends_with(L".utoc") ||
                    lower.ends_with(L".ucas") || lower.ends_with(L".sig");
                const bool is_disabled = lower.ends_with(L".pak.disabled") ||
                    lower.ends_with(L".utoc.disabled") || lower.ends_with(L".ucas.disabled") ||
                    lower.ends_with(L".sig.disabled");
                if (enabled && is_disabled) {
                    std::wstring stripped = full.substr(0, full.size() - 9);  // ".disabled"
                    std::error_code inner;
                    std::filesystem::rename(file.path(), stripped, inner);
                    if (inner) return false;
                } else if (!enabled && is_container) {
                    std::error_code inner;
                    std::filesystem::rename(file.path(), full + L".disabled", inner);
                    if (inner) return false;
                }
            }
        } catch (...) {
            return false;
        }
        if (enabled && LauncherStripDisabled(entry.name) != entry.name) {
            std::filesystem::rename(entry.path,
                entry.path.parent_path() / LauncherWiden(LauncherStripDisabled(entry.name)), error);
        }
        return !error;
    }
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
        if (item.is_directory(error) || !LauncherIsModFile(item.path())) continue;
        const std::string file_name = LauncherNarrow(item.path().filename());
        if (LauncherModBaseName(file_name) != LauncherStripDisabled(entry.name)) continue;
        const bool is_disabled = LauncherStripDisabled(file_name) != file_name;
        if (is_disabled == !enabled) continue;
        std::filesystem::path target = item.path();
        if (enabled) {
            target = directory / LauncherWiden(LauncherStripDisabled(file_name));
        } else {
            target = item.path().wstring() + L".disabled";
        }
        std::filesystem::rename(item.path(), target, error);
        if (error) return false;
    }
    return true;
}

std::vector<std::filesystem::path> LauncherModFiles(const LauncherModEntry& entry) {
    std::vector<std::filesystem::path> files;
    if (entry.directory) {
        for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path))
            if (file.is_regular_file()) files.push_back(file.path());
    } else {
        for (const auto& file : std::filesystem::directory_iterator(entry.path.parent_path())) {
            if (file.is_regular_file() && LauncherIsModFile(file.path()) &&
                LauncherModBaseName(LauncherNarrow(file.path().filename())) == entry.name)
                files.push_back(file.path());
        }
        const auto notes = LauncherModNotesPath(entry);
        if (std::filesystem::exists(notes)) files.push_back(notes);
        if (!entry.cover.empty()) files.push_back(entry.cover);
    }
    for (const auto& file : files) {
        const auto attributes = GetFileAttributesW(file.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("mod file is unavailable or a reparse point");
    }
    return files;
}

// A saved OFF selection can precede the runtime's unmount. Check file access before deleting
// anything, so a still-open IoStore sidecar cannot leave a partially removed package.
bool LauncherModFilesAvailable(const LauncherModEntry& entry) {
    try {
        for (const auto& file : LauncherModFiles(entry)) {
            if (!LauncherIsModFile(file)) continue;
            const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            CloseHandle(handle);
        }
        return true;
    } catch (...) { return false; }
}

bool LauncherCanEditMod(const LauncherSnapshot& snapshot, const LauncherModEntry& entry) {
    try {
        if (!LauncherModTreeSafe(snapshot, entry)) return false;
        if (LauncherGameRunning()) {
            const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), LauncherPakSettings(snapshot));
            for (const auto& row : catalog)
                if (anomaly::NormalizePakModKey(row.key) == anomaly::NormalizePakModKey(LauncherModStateKey(entry)) &&
                    row.enabled) return false;
        }
        return LauncherModFilesAvailable(entry);
    } catch (...) { return false; }
}

bool LauncherRemoveMod(const LauncherSnapshot& snapshot, const LauncherModEntry& entry) {
    if (!LauncherCanEditMod(snapshot, entry)) return false;
    try {
        if (entry.directory) std::filesystem::remove_all(entry.path);
        else for (const auto& file : LauncherModFiles(entry)) std::filesystem::remove(file);
        return true;
    } catch (...) { return false; }
}

// Which runtime the game actually loads depends on the mode: the proxy install keeps its runtime
// inside the game folder, while live attach injects the launcher's own runtime. The mod switch has
// to edit the one that runs, otherwise it silently edits a folder nobody reads. It drives the
// runtime's own master switch, which is what mounts containers under Paks\~mods.
std::vector<std::filesystem::path> LauncherPakConfigCandidates(const LauncherSnapshot& snapshot) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    const auto in_game =
        snapshot.game_directory / L"Anomaly" / L"config" / L"pak-mods.json";
    paths.push_back(in_game);
    const auto launcher_side =
        ExecutableDirectory() / L"Anomaly" / L"config" / L"pak-mods.json";
    if (std::filesystem::is_regular_file(launcher_side, error) ||
        std::filesystem::is_regular_file(ExecutableDirectory() / L"Anomaly" / L"Anomaly.Core.dll", error)) {
        if (std::find(paths.begin(), paths.end(), launcher_side) == paths.end()) paths.push_back(launcher_side);
    }
    if (paths.empty()) paths.push_back(launcher_side);
    return paths;
}

// No configuration means "on": the runtime mounts ~mods by default, so the switch shows that state
// instead of inventing an off state nobody asked for.
bool LauncherReadBypassEnabled(const LauncherSnapshot& snapshot) {
    try { return LauncherPakSettings(snapshot).value("enabled", true); }
    catch (...) { return false; }
}

bool LauncherWritePakSettings(const LauncherSnapshot& snapshot,
    const std::function<void(nlohmann::json&)>& update) {
    try {
        const auto canonical = LauncherPakConfigCandidates(snapshot).front();
        anomaly::UpdatePakModSettings({canonical}, [&](auto& document) {
            if (document.empty()) document = LauncherPakSettings(snapshot);
            update(document);
        });
        return true;
    } catch (...) { return false; }
}

bool LauncherWriteLegacyBypass(const LauncherSnapshot& snapshot, const bool enabled) {
    // Preserve the already verified signature recipe. A missing recipe is an explicit failure,
    // not an empty pattern that makes a compatibility switch appear to work.
    std::vector<std::pair<std::filesystem::path, nlohmann::json>> documents;
    nlohmann::json recipe;
    for (const auto& config : LauncherPakConfigCandidates(snapshot)) {
        const auto path = config.parent_path() / L"pak-signature-bypass.json";
        std::ifstream input(path);
        auto document = input ? nlohmann::json::parse(input, nullptr, false)
                              : nlohmann::json::object();
        if (!document.is_object()) return false;
        const auto pattern = document.find("pattern");
        if (pattern != document.end() && pattern->is_string() &&
                !pattern->get_ref<const std::string&>().empty()) recipe = document;
        document["enabled"] = enabled;
        documents.emplace_back(path, std::move(document));
    }
    if (enabled && recipe.is_null()) return false;
    for (auto& [path, document] : documents) {
        const auto pattern = document.find("pattern");
        if (enabled && (pattern == document.end() || !pattern->is_string() ||
                pattern->get_ref<const std::string&>().empty())) document = recipe;
        document["enabled"] = enabled;
        const auto text = document.dump(2) + '\n';
        anomaly::ReliableStorage storage(path.parent_path().wstring());
        if (!storage.WriteAtomic(path.filename().wstring(),
                std::as_bytes(std::span(text.data(), text.size())))) return false;
    }
    return true;
}

bool LauncherWriteBypassEnabled(const LauncherSnapshot& snapshot, const bool enabled) {
    if (LauncherCompatibilityMode(snapshot)) {
        if (LauncherGameRunning() || !LauncherWriteLegacyBypass(snapshot, enabled)) return false;
    }
    return LauncherWritePakSettings(snapshot, [enabled](auto& document) {
        document["enabled"] = enabled;
    });
}

// The master bypass decides whether mods load at all; this decides what the exclusivity does with a
// mod that ships the same resources as another one. Off by default: two variants of one body are a
// mixture nobody asked for, while a patch mod next to the mod it patches is exactly the point.
bool LauncherWriteForceLoad(const LauncherSnapshot& snapshot, const bool enabled) {
    return LauncherWritePakSettings(snapshot, [enabled](auto& document) {
        document["forceLoad"] = enabled;
    });
}

bool LauncherSetModEnabled(const LauncherSnapshot& snapshot, const LauncherModEntry& entry,
    const bool enabled) {
    const auto key = LauncherModStateKey(entry);
    try {
        const bool compatibility = LauncherCompatibilityMode(snapshot);
        if (compatibility && LauncherGameRunning()) return false;
        const auto settings = LauncherPakSettings(snapshot);
        const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), settings);
        const auto selected = std::find_if(catalog.begin(), catalog.end(), [&](const auto& row) {
            return anomaly::NormalizePakModKey(row.key) == anomaly::NormalizePakModKey(key);
        });
        if (selected == catalog.end() || (enabled && selected->restricted)) return false;
        const auto write = [&](bool value) {
            return LauncherWritePakSettings(snapshot, [&](auto& document) {
                const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), document);
                anomaly::SelectPakMod(document, catalog, key, value);
            });
        };
        if (compatibility) {
            if (enabled && !anomaly::PakModForceLoad(settings)) {
                std::vector<LauncherModEntry> all;
                LauncherRefreshMods(snapshot, all);
                for (const auto& other : catalog) if (anomaly::PakModsConflict(*selected, other)) {
                    for (const auto& item : all)
                        if (anomaly::NormalizePakModKey(LauncherModStateKey(item)) == anomaly::NormalizePakModKey(other.key) &&
                            !LauncherSetModFilesEnabled(snapshot, item, false)) return false;
                }
            }
            if (!LauncherSetModFilesEnabled(snapshot, entry, enabled)) return false;
        } else if (enabled) {
            // Restore sidecars under OFF, then publish the complete exclusive selection.
            if (!write(false) || !LauncherSetModFilesEnabled(snapshot, entry, true)) return false;
        }
        return write(enabled);
    } catch (...) { return false; }
}

bool LauncherWriteCompatibilityMode(const LauncherSnapshot& snapshot, const bool compatibility) {
    if (LauncherGameRunning()) return false;
    try { anomaly::DisableRestrictedPakMods(LauncherModsDirectory(snapshot)); }
    catch (...) { return false; }
    std::vector<LauncherModEntry> mods;
    LauncherRefreshMods(snapshot, mods);
    if (!LauncherWriteLegacyBypass(snapshot,
            compatibility && LauncherReadBypassEnabled(snapshot)))
        return false;
    // Transfer the selection between filename markers and runtime configuration while stopped.
    // This keeps a disabled variant disabled when the engine takes over startup loading.
    if (!LauncherWritePakSettings(snapshot, [&](auto& document) {
            for (const auto& entry : mods)
                anomaly::SetPakModEnabled(document["modStates"], LauncherModStateKey(entry), entry.enabled);
        })) return false;
    for (const auto& entry : mods) {
        if (!LauncherSetModFilesEnabled(snapshot, entry, compatibility ? entry.enabled : true))
            return false;
    }
    return LauncherWritePakSettings(snapshot, [&](auto& document) {
        document["mode"] = compatibility ? "compatibility" : "hot";
    });
}

// The mods page files a mod under the category its containers belong to; this makes the folder tree
// say the same thing, so `~mods/Player_051 零/ModName/` is where that mod lives from then on. The
// selection travels with the mod: a key is a path, so a move that left the key behind would quietly
// turn a mod the player switched off back on. Mods already in place are left alone, and a target that
// already exists is skipped instead of merged -- two different mods can share a folder name.
struct LauncherOrganizeResult final {
    std::size_t moved{}, skipped{}, failed{};
    bool keys_unsaved{};
};

LauncherOrganizeResult LauncherOrganizeModFolders(const LauncherSnapshot& snapshot,
    const std::vector<LauncherModEntry>& mods) {
    LauncherOrganizeResult result;
    // A running engine may still hold a container open, and Windows refuses to rename a file that is
    // open without delete sharing. That is one folder failing, not the whole action: the rest are
    // organised and the page reports what was skipped, so a player who disabled everything does not
    // have to close the game first.
    const auto directory = LauncherModsDirectory(snapshot);
    const auto language = LauncherModGroupLanguage();
    struct Move final {
        std::filesystem::path from, to;
        std::string old_key, new_key;
    };
    std::vector<Move> moves;
    std::set<std::string> destinations;
    for (const auto& entry : mods) {
        if (!entry.directory || entry.groups.empty()) continue;
        // The category a mod is filed under first is the one its folder is named after; a mod that
        // covers two categories keeps its place in the page, but lives in one folder.
        const auto folder = g_mod_group_names.Folder(entry.groups.front().name, language);
        const auto target = directory / LauncherWiden(folder) / LauncherWiden(entry.name);
        if (entry.path == target) continue;
        if (!LauncherModTreeSafe(snapshot, entry)) { ++result.skipped; continue; }
        auto destination = entry;
        destination.path = target;
        if (!LauncherModTreeSafe(snapshot, destination)) { ++result.skipped; continue; }
        std::error_code error;
        if (std::filesystem::exists(target, error)) { ++result.skipped; continue; }
        const auto new_key = folder + "/" + LauncherStripDisabled(entry.name) + "/";
        if (!destinations.insert(anomaly::NormalizePakModKey(new_key)).second) {
            ++result.skipped;
            continue;
        }
        moves.push_back({entry.path, target, LauncherModStateKey(entry), new_key});
    }
    std::vector<Move> applied;
    applied.reserve(moves.size());
    // Publish the destination selections before moving files: the runtime can scan between the
    // rename and the next settings write. Preserve per-container overrides as well as inherited OFF.
    if (!moves.empty() && !LauncherWritePakSettings(snapshot, [&](auto& document) {
            auto& states = document["modStates"];
            if (states.is_null()) states = nlohmann::json::object();
            anomaly::ValidatePakModStates(states);
            const auto previous = states;
            for (const auto& move : moves) {
                const auto old_key = anomaly::NormalizePakModKey(move.old_key);
                anomaly::SetPakModEnabled(states, move.new_key,
                    anomaly::PakModEnabled(previous, move.old_key));
                for (const auto& [key, value] : previous.items()) {
                    const auto normalized = anomaly::NormalizePakModKey(key);
                    if (normalized.starts_with(old_key) && normalized.size() > old_key.size())
                        states[move.new_key + normalized.substr(old_key.size())] = value;
                }
            }
        })) {
        result.failed = moves.size();
        return result;
    }
    for (auto& move : moves) {
        std::error_code error;
        std::filesystem::create_directories(move.to.parent_path(), error);
        if (!error) std::filesystem::rename(move.from, move.to, error);
        if (error) { ++result.failed; continue; }
        ++result.moved;
        applied.push_back(std::move(move));
    }
    if (!applied.empty() && !LauncherWritePakSettings(snapshot, [&](auto& document) {
            auto& states = document["modStates"];
            anomaly::ValidatePakModStates(states);
            for (const auto& move : applied) {
                const auto old_key = anomaly::NormalizePakModKey(move.old_key);
                for (auto it = states.begin(); it != states.end();) {
                    const auto key = anomaly::NormalizePakModKey(it.key());
                    if (key.starts_with(old_key)) {
                        it = states.erase(it);
                        continue;
                    }
                    ++it;
                }
            }
        })) {
        result.keys_unsaved = true;
    }
    return result;
}

std::string LauncherOrganizeMessage(const LauncherOrganizeResult& result) {
    std::string text;
    if (result.moved || result.skipped) {
        text = Tr(anomaly::MessageId::LauncherModsOrganizeDone,
            {std::to_string(result.moved), std::to_string(result.skipped)});
    }
    const auto append = [&text](const std::string& part) {
        if (part.empty()) return;
        if (!text.empty()) text += ' ';
        text += part;
    };
    if (result.failed) {
        append(Tr(anomaly::MessageId::LauncherModsOrganizeFailed,
            {std::to_string(result.failed)}));
    }
    // Folders a running game still holds open were skipped; the next pass finishes them.
    if (result.failed != 0 && LauncherGameRunning())
        append(Tr(anomaly::MessageId::LauncherModsOrganizeBusy));
    if (result.keys_unsaved) append(Tr(anomaly::MessageId::LauncherModsOrganizeKeys));
    if (text.empty()) text = Tr(anomaly::MessageId::LauncherModsOrganizeNothing);
    return text;
}

bool LauncherPrepareModWrite(const LauncherSnapshot& snapshot, const LauncherModEntry& target,
    bool& enable_after) {
    if (!LauncherModTreeSafe(snapshot, target)) return false;
    const bool running = LauncherGameRunning();
    return LauncherWritePakSettings(snapshot, [&](auto& document) {
        const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), document);
        enable_after = !running || !anomaly::PakModCompatibilityMode(document);
        for (const auto& row : catalog) {
            if (anomaly::NormalizePakModKey(row.key) != anomaly::NormalizePakModKey(LauncherModStateKey(target))) continue;
            if (running && row.enabled) throw std::runtime_error("disable mod before replacing files");
            enable_after = row.enabled;
        }
        if (std::filesystem::exists(target.path) && !LauncherModFilesAvailable(target))
            throw std::runtime_error("mod files are still in use");
        anomaly::SetPakModEnabled(document["modStates"], LauncherModStateKey(target), false);
    });
}

bool LauncherFinishModWrite(const LauncherSnapshot& snapshot, const LauncherModEntry& target,
    const bool enable_after) {
    try {
        const bool compatibility = LauncherCompatibilityMode(snapshot);
        const auto settings = LauncherPakSettings(snapshot);
        const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), settings);
        const auto selected = std::find_if(catalog.begin(), catalog.end(), [&](const auto& row) {
            return anomaly::NormalizePakModKey(row.key) == anomaly::NormalizePakModKey(LauncherModStateKey(target));
        });
        if (selected == catalog.end()) return false;
        if (selected->restricted) {
            static_cast<void>(LauncherSetModFilesEnabled(snapshot, target, false));
            return false;
        }
        if (compatibility && enable_after && !anomaly::PakModForceLoad(settings)) {
            for (const auto& row : catalog) {
                if (!anomaly::PakModsConflict(*selected, row)) continue;
                LauncherModEntry other;
                other.key = row.key;
                other.directory = row.key.ends_with('/');
                other.path = LauncherModsDirectory(snapshot) / LauncherWiden(other.directory ? row.key.substr(0, row.key.size() - 1) : row.key);
                other.name = other.directory ? LauncherNarrow(other.path.filename()) : LauncherModBaseName(row.key);
                if (row.enabled && !LauncherSetModFilesEnabled(snapshot, other, false)) return false;
            }
        }
        if (!LauncherSetModFilesEnabled(snapshot, target, compatibility ? enable_after : true)) return false;
        return LauncherWritePakSettings(snapshot, [&](auto& document) {
            const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), document);
            anomaly::SelectPakMod(document, catalog, LauncherModStateKey(target), enable_after);
        });
    } catch (...) { return false; }
}

bool LauncherRemoveObsoleteContainers(const LauncherModEntry& target,
    const std::set<std::filesystem::path>& replacements) {
    try {
        for (const auto& file : LauncherModFiles(target))
            if (LauncherIsModFile(file) && !replacements.contains(file)) std::filesystem::remove(file);
        return true;
    } catch (...) { return false; }
}

LauncherModEntry LauncherDirectoryMod(const std::filesystem::path& path) {
    LauncherModEntry result;
    result.path = path;
    result.name = LauncherNarrow(path.filename());
    result.directory = true;
    return result;
}

// Forward declarations: the import path records the GameBanana file id, and those helpers live
// further down with the update check.
std::wstring LauncherSystemProxy();
std::string LauncherGameBananaId(const std::string& link);
std::string LauncherGameBananaFileId(const std::string& profile_body);
std::string LauncherGameBananaFileName(const std::string& profile_body);
std::string LauncherGameBananaAuthor(const std::string& profile_body);
std::string LauncherGameBananaFileMd5(const std::string& profile_body);
std::string LauncherMd5OfFile(const std::filesystem::path& path);
void LauncherWriteModSource(const std::filesystem::path& folder, const std::string& mod_id,
    const std::string& file_id, const std::string& file_name, const std::string& md5,
    const std::string& author, const std::string& name);

// --- import / rename -------------------------------------------------------
std::optional<std::filesystem::path> LauncherChooseArchive(HWND window) {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Mod archives (*.zip;*.7z;*.rar)\0*.zip;*.7z;*.rar\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog) == FALSE) return std::nullopt;
    return std::filesystem::path(file);
}

bool LauncherRunAndWait(const std::wstring& command_line, const DWORD timeout_ms) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = command_line;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) == FALSE) {
        return false;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        // The downloader must stop before its destination can be removed or reused.
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code{1};
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return waited == WAIT_OBJECT_0 && exit_code == 0;
}

// Sanitizes the destination folder name for both imported and downloaded mods.
std::wstring LauncherSafeFolderName(const std::string& utf8) {
    std::wstring wide = LauncherWiden(utf8);
    for (auto& c : wide) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
            c == L'<' || c == L'>' || c == L'|') {
            c = L'_';
        }
    }
    while (!wide.empty() && (wide.back() == L' ' || wide.back() == L'.')) wide.pop_back();
    return wide;
}

void LauncherUpdateLog(const std::string& line);

std::string LauncherArchiveFailure(const anomaly::launcher::ExtractedModArchive& archive) {
    using Error = anomaly::launcher::ModArchiveError;
    if (archive.error != Error::Cancelled) LauncherUpdateLog("extract: " + archive.detail);
    switch (archive.error) {
    case Error::Cancelled: return Tr(anomaly::MessageId::LauncherMsgCancelled);
    case Error::Unsupported: return Tr(anomaly::MessageId::LauncherMsgUnsupportedArchive);
    case Error::Encrypted: return Tr(anomaly::MessageId::LauncherMsgArchiveEncrypted);
    case Error::UnsafePath: return Tr(anomaly::MessageId::LauncherMsgArchiveUnsafe);
    case Error::LimitExceeded: return Tr(anomaly::MessageId::LauncherMsgArchiveLimit);
    case Error::Io: return Tr(anomaly::MessageId::LauncherMsgArchiveIo);
    default: return Tr(anomaly::MessageId::LauncherMsgExtractFailed);
    }
}

std::string LauncherImportArchive(
    const LauncherSnapshot& snapshot, HWND window, const std::filesystem::path& archive) {
    try {
        (void)window;
        std::error_code error;

        auto extracted = anomaly::launcher::ExtractModArchive(archive);
        if (!extracted.Ok()) return LauncherArchiveFailure(extracted);
        const auto& staging = extracted.directory;

        // Folder name: the archive stem, sanitised and converted properly (UTF-8 -> wide).
        std::wstring folder_name = LauncherSafeFolderName(LauncherNarrow(archive.stem()));
        if (folder_name.empty()) folder_name = L"imported-mod";
        const auto target = LauncherModsDirectory(snapshot) / folder_name;
        const bool target_existed = std::filesystem::exists(target, error);
        const auto target_mod = LauncherDirectoryMod(target);
        bool enable_after{};

        if (!LauncherPrepareModWrite(snapshot, target_mod, enable_after))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        std::filesystem::create_directories(target, error);
        if (error) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
        std::set<std::filesystem::path> containers;
        std::size_t copied{};
        std::size_t pak_files{};
        for (const auto& item : std::filesystem::recursive_directory_iterator(staging, error)) {
            if (!item.is_regular_file(error)) continue;
            std::wstring extension = item.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](const wchar_t c) { return static_cast<wchar_t>(c >= L'A' && c <= L'Z' ? c + 32 : c); });
            if (extension == L".pak" || extension == L".utoc" || extension == L".ucas") ++pak_files;
            const auto relative = std::filesystem::relative(item.path(), staging, error);
            if (error || relative.empty()) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
            auto destination = target / relative;
            if (LauncherIsModFile(destination) && !destination.wstring().ends_with(L".disabled"))
                destination += L".disabled";
            std::filesystem::create_directories(destination.parent_path(), error);
            std::filesystem::copy_file(item.path(), destination,
                std::filesystem::copy_options::overwrite_existing, error);
            if (error) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
            if (LauncherIsModFile(destination)) containers.insert(destination);
            ++copied;
        }

        if (copied == 0) {
            if (!target_existed) std::filesystem::remove_all(target, error);
            return Tr(anomaly::MessageId::LauncherMsgNoImportFiles);
        }

        // Source record: prefer mod.json's modLink, otherwise keep what we can.
        std::string display_name = LauncherNarrow(folder_name);
        std::string author;
        std::string mod_link;
        {
            std::ifstream mod_json(target / L"mod.json");
            if (mod_json) {
                const std::string text((std::istreambuf_iterator<char>(mod_json)),
                    std::istreambuf_iterator<char>());
                const auto name_field = LauncherJsonField(text, "name");
                if (!name_field.empty()) display_name = name_field;
                author = LauncherJsonField(text, "author");
                mod_link = LauncherJsonField(text, "modLink");
            }
        }
        const std::string mod_id = LauncherGameBananaId(mod_link);
        if (!mod_id.empty()) {
            const std::wstring path = std::wstring(L"/apiv11/Mod/") + std::wstring(mod_id.begin(), mod_id.end()) +
                L"/ProfilePage";
            const std::string body = LauncherHttpGet(L"gamebanana.com", path);
            if (!body.empty()) {
                LauncherWriteModSource(target, mod_id, LauncherGameBananaFileId(body),
                    LauncherGameBananaFileName(body), LauncherGameBananaFileMd5(body),
                    LauncherGameBananaAuthor(body), display_name);
            }
        }

        if (!containers.empty() && !LauncherRemoveObsoleteContainers(target_mod, containers))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        if (!containers.empty() && !LauncherFinishModWrite(snapshot, target_mod, enable_after))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        std::string message = Tr(anomaly::MessageId::LauncherMsgImported,
            {std::to_string(copied), display_name});
        if (pak_files == 0) {
            message = Tr(anomaly::MessageId::LauncherMsgNoPakImported);
        }
        return message;
    } catch (...) { return Tr(anomaly::MessageId::LauncherModsFileChangeFailed); }
}

bool LauncherRenameMod(
    const LauncherSnapshot& snapshot, const LauncherModEntry& entry, const std::string& new_name,
    std::string* renamed_key = nullptr) {
    try {
        if (!LauncherCanEditMod(snapshot, entry)) return false;
        if (new_name.empty() || new_name == entry.name || new_name == "." || new_name == ".." ||
            new_name.find_first_of("<>:\"/\\|?*") != std::string::npos ||
            new_name.back() == '.' || new_name.back() == ' ') return false;
        std::error_code error;
        const auto directory = LauncherModsDirectory(snapshot);
        const bool hot = !LauncherCompatibilityMode(snapshot);
        const auto old_key = LauncherModStateKey(entry);
        const bool priority_suffix = !entry.directory && LauncherModBaseName(old_key).ends_with("_P");
        const auto renamed_stem = new_name + (priority_suffix && !new_name.ends_with("_P") ? "_P" : "");
        const auto parent_end = entry.directory ? old_key.find_last_of('/', old_key.size() - 2)
                                                : std::string::npos;
        const auto new_key = (parent_end == std::string::npos ? std::string{} : old_key.substr(0, parent_end + 1)) +
            LauncherStripDisabled(renamed_stem) + (entry.directory ? "/" : ".pak");
        const auto target_directory = entry.path.parent_path() / LauncherWiden(new_name);
        if (entry.directory && std::filesystem::exists(target_directory, error) &&
            !std::filesystem::equivalent(entry.path, target_directory, error)) return false;
        const auto prepare = [&] {
            return !hot || LauncherWritePakSettings(snapshot, [&](auto& document) {
                anomaly::SetPakModEnabled(document["modStates"], new_key, entry.enabled);
            });
        };
        const auto finish = [&] {
            if (!hot) return true;
            if (anomaly::NormalizePakModKey(old_key) == anomaly::NormalizePakModKey(new_key)) return true;
            return LauncherWritePakSettings(snapshot, [&](auto& document) {
                auto& states = document["modStates"];
                for (auto it = states.begin(); it != states.end();) {
                    const auto key = anomaly::NormalizePakModKey(it.key());
                    const auto old = anomaly::NormalizePakModKey(old_key);
                    if (key == old || (old.back() == '/' && key.starts_with(old))) it = states.erase(it);
                    else ++it;
                }
            });
        };
        if (entry.directory) {
            if (!prepare()) return false;
            if (!hot && !entry.enabled && !LauncherSetModFilesEnabled(snapshot, entry, false)) return false;
            try {
                std::filesystem::rename(entry.path, target_directory, error);
            } catch (...) {
                return false;
            }
            const bool saved = !error && finish();
            if (saved && renamed_key != nullptr) *renamed_key = new_key;
            return saved;
        }
        // Prepare every destination before renaming so another loose mod is never overwritten.
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> renames;
        for (const auto& file : LauncherModFiles(entry)) {
            const auto old_name = LauncherNarrow(file.filename());
            std::string suffix;
            if (file == LauncherModNotesPath(entry)) suffix = ".mod-notes.json";
            else if (LauncherIsModFile(file)) {
                const auto plain = LauncherStripDisabled(old_name);
                suffix = LauncherNarrow(std::filesystem::path(LauncherWiden(plain)).extension()) +
                    (plain == old_name ? "" : ".disabled");
            } else suffix = LauncherNarrow(file.extension());
            const auto target = directory / LauncherWiden(renamed_stem + suffix);
            if (std::filesystem::exists(target, error) && !std::filesystem::equivalent(file, target, error)) return false;
            renames.emplace_back(file, target);
        }
        if (!prepare()) return false;
        for (const auto& [source, target] : renames) {
            std::filesystem::rename(source, target, error);
            if (error) return false;
        }
        const bool saved = finish();
        if (saved && renamed_key != nullptr) *renamed_key = new_key;
        return saved;
    } catch (...) { return false; }
}



// Compare the newest file id because the page's version string is often empty.
std::string LauncherGameBananaFileId(const std::string& profile_body) {
    const auto files = profile_body.find("\"_aFiles\"");
    if (files == std::string::npos) return {};
    const auto id_at = profile_body.find("\"_idRow\"", files);
    if (id_at == std::string::npos) return {};
    const auto colon = profile_body.find(':', id_at);
    if (colon == std::string::npos) return {};
    auto at = profile_body.find_first_of("0123456789", colon + 1);
    if (at == std::string::npos) return {};
    std::string digits;
    while (at < profile_body.size() && profile_body[at] >= '0' && profile_body[at] <= '9') {
        digits.push_back(profile_body[at++]);
    }
    return digits;
}



std::string LauncherMd5OfFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    HCRYPTPROV provider{};
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ==
        FALSE) {
        return {};
    }
    HCRYPTHASH hash{};
    std::string hex;
    if (CryptCreateHash(provider, CALG_MD5, 0, 0, &hash) != FALSE) {
        std::vector<char> buffer(1 << 16);
        while (stream) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto read = stream.gcount();
            if (read <= 0) break;
            if (CryptHashData(hash, reinterpret_cast<const BYTE*>(buffer.data()),
                    static_cast<DWORD>(read), 0) == FALSE) {
                break;
            }
        }
        BYTE digest[16]{};
        DWORD length = sizeof(digest);
        if (CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) != FALSE) {
            char text[3]{};
            for (DWORD i = 0; i < length; ++i) {
                std::snprintf(text, sizeof(text), "%02x", digest[i]);
                hex += text;
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(provider, 0);
    return hex;
}

void LauncherWriteModSource(const std::filesystem::path& folder, const std::string& mod_id,
    const std::string& file_id, const std::string& file_name, const std::string& md5,
    const std::string& author, const std::string& name) {
    if (folder.empty() || mod_id.empty() || file_id.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const nlohmann::json source{
        {"schemaVersion", 1}, {"provider", "gamebanana"}, {"modId", mod_id},
        {"fileId", file_id}, {"fileName", file_name}, {"fileMd5", md5},
        {"author", author}, {"displayName", name}};
    std::ofstream(folder / kLauncherModSourceFile) << source.dump(2) << '\n';
}


std::string LauncherGameBananaFileName(const std::string& profile_body) {
    const auto files = profile_body.find("\"_aFiles\"");
    if (files == std::string::npos) return {};
    return LauncherJsonField(profile_body.substr(files), "_sFile");
}


std::string LauncherGameBananaFileMd5(const std::string& profile_body) {
    const auto files = profile_body.find("\"_aFiles\"");
    if (files == std::string::npos) return {};
    return LauncherJsonField(profile_body.substr(files), "_sMd5Checksum");
}

std::string LauncherGameBananaAuthor(const std::string& profile_body) {
    const auto submitter = profile_body.find("\"_aSubmitter\"");
    if (submitter == std::string::npos) return {};
    return LauncherJsonField(profile_body.substr(submitter), "_sName");
}

// --- update check ----------------------------------------------------------
// Reads the current version of a GameBanana mod. WinHTTP so the launcher does not need libcurl;
// only the small ProfilePage payload is fetched and parsed.
void LauncherUpdateLog(const std::string& line) {
    std::error_code error;
    const auto directory = ExecutableDirectory() / L"Anomaly" / L"logs";
    std::filesystem::create_directories(directory, error);
    std::ofstream(directory / L"launcher-update.log", std::ios::app)
        << line << "\n";
}

// One GET to one endpoint. The callers split by how they know their address -- a host and a path, or
// a whole URL -- and both end up here. The timeout is theirs because an address known not to answer
// is not worth the wait a download is worth.
std::string LauncherHttpGetRequest(const std::wstring& host, const std::wstring& path,
    const INTERNET_PORT port, const bool secure, const bool use_system_proxy, int* status_code,
    const DWORD timeout_ms) {
    std::string result;
    if (status_code != nullptr) *status_code = 0;
    // Direct first: a stale or half-configured system proxy is a common cause of "works in curl,
    // fails in WinHTTP". The caller retries with the system proxy when this returns nothing.
    const std::wstring proxy = use_system_proxy ? std::wstring{} : LauncherSystemProxy();
    HINTERNET session = WinHttpOpen(L"AnomalyLauncher/1.0",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        LauncherUpdateLog("open session failed: " + std::to_string(GetLastError()));
        return result;
    }
    // TLS 1.2/1.3 explicitly: WinHTTP's default protocol set still excludes them on some builds,
    // which shows up as an intermittent handshake failure.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms),
        static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection != nullptr) {
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (request != nullptr) {
            // GameBanana answers with redirects for some hosts and rejects unknown clients, so the
            // policy follows redirects and the request presents a browser-style identity.
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                sizeof(redirect_policy));
            const wchar_t* headers =
                L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AnomalyLauncher/1.0\r\n"
                L"Accept: application/json\r\n"
                L"Accept-Encoding: identity\r\n";
            const BOOL sent = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            const DWORD send_error = sent == FALSE ? GetLastError() : 0;
            const BOOL received = sent != FALSE ? WinHttpReceiveResponse(request, nullptr) : FALSE;
            const DWORD receive_error = received == FALSE ? GetLastError() : 0;
            if (sent != FALSE && received != FALSE) {
                // The status is what separates "this repository publishes no release" from "the API
                // refused to answer at all", and the launcher reports the difference to the user.
                DWORD status{};
                DWORD status_size = sizeof(status);
                if (WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX) != FALSE && status_code != nullptr) {
                    *status_code = static_cast<int>(status);
                }
                // Read until the stream reports zero bytes: QueryDataAvailable returns 0 for
                // "nothing right now" too, which used to truncate the body mid-JSON.
                std::string chunk(16 * 1024, '\0');
                DWORD read{};
                while (result.size() <= (1u << 20) &&
                    WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read) != FALSE &&
                    read > 0) {
                    result.append(chunk.data(), read);
                    read = 0;
                }
            }
            if (result.empty()) {
                DWORD status{};
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                LauncherUpdateLog("get " + std::string(path.begin(), path.end()) +
                    " empty; status=" + std::to_string(status) +
                    " error=" + std::to_string(GetLastError()));
            }
            WinHttpCloseHandle(request);
        } else {
            LauncherUpdateLog("open request failed: " + std::to_string(GetLastError()));
        }
        WinHttpCloseHandle(connection);
    } else {
        LauncherUpdateLog("connect failed: " + std::to_string(GetLastError()));
    }
    WinHttpCloseHandle(session);
    return result;
}

std::string LauncherHttpGet(const std::wstring& host, const std::wstring& path,
    const bool use_system_proxy, int* status_code) {
    return LauncherHttpGetRequest(host, path, INTERNET_DEFAULT_HTTPS_PORT, true,
        use_system_proxy, status_code, 8000);
}

// A GET for a caller holding a whole URL: the update check addresses api.github.com and the mirrors
// that forward it, and every one of those is spelled as a URL rather than as a host and a path.
std::string LauncherHttpGetUrl(const std::string& url, const bool use_system_proxy,
    int* status_code, const DWORD timeout_ms) {
    if (status_code != nullptr) *status_code = 0;
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE) {
        LauncherUpdateLog("url get: crack failed " + std::to_string(GetLastError()) +
            " url=" + url);
        return {};
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const INTERNET_PORT port = parts.nPort == 0 ? INTERNET_DEFAULT_HTTPS_PORT : parts.nPort;
    return LauncherHttpGetRequest(host, path, port, parts.nScheme == INTERNET_SCHEME_HTTPS,
        use_system_proxy, status_code, timeout_ms);
}

// "https://gamebanana.com/mods/735632" -> 735632
std::string LauncherGameBananaId(const std::string& link) {
    const std::string marker = "gamebanana.com/mods/";
    const auto at = link.find(marker);
    if (at == std::string::npos) return {};
    std::string id;
    for (std::size_t i = at + marker.size(); i < link.size(); ++i) {
        const char c = link[i];
        if (c < '0' || c > '9') break;
        id.push_back(c);
    }
    return id;
}

std::string LauncherCheckUpdates(std::vector<LauncherModEntry>& mods,
    std::vector<std::pair<std::string, std::string>>& discovered) {
    int checked{}, updated{}, unchanged{}, failed{}, skipped{}, unrecorded{}, adopted{};
    for (auto& entry : mods) {
        const std::string id = LauncherGameBananaId(entry.link);
        const auto source = LauncherReadModSource(entry.icon_path);
        LauncherUpdateLog("check: '" + entry.name + "' link=" + entry.link + " id=" + id +
            " recorded=" + source.file_id);
        if (id.empty()) {
            ++skipped;
            continue;
        }
        if (source.file_id.empty()) {
            // No record yet: if mod.json names a GameBanana page, adopt the current file as the
            // baseline so later checks can tell whether it changed. Mods without a link stay
            // untracked.
            const std::wstring adopt_path = std::wstring(L"/apiv11/Mod/") +
                std::wstring(id.begin(), id.end()) + L"/ProfilePage";
            const std::string adopt_body = LauncherHttpGet(L"gamebanana.com", adopt_path);
            const std::string adopted_file = LauncherGameBananaFileId(adopt_body);
            if (adopted_file.empty()) {
                ++failed;
                continue;
            }
            LauncherWriteModSource(entry.icon_path, id, adopted_file,
                LauncherGameBananaFileName(adopt_body), LauncherGameBananaFileMd5(adopt_body),
                LauncherGameBananaAuthor(adopt_body), entry.display_name);
            ++adopted;
            continue;
        }
        ++checked;
        const std::wstring path = std::wstring(L"/apiv11/Mod/") +
            std::wstring(id.begin(), id.end()) + L"/ProfilePage";
        std::string body = LauncherHttpGet(L"gamebanana.com", path);
        if (body.empty()) body = LauncherHttpGet(L"gamebanana.com", path, true);  // retry via proxy
        if (body.empty()) {
            ++failed;
            continue;
        }
        std::string remote_version = LauncherJsonField(body, "_sVersion");
        std::string remote_file = LauncherGameBananaFileId(body);
        std::string remote_md5 = LauncherGameBananaFileMd5(body);
        if (remote_file.empty()) {
            // Some mods hide their files from ProfilePage; /Files still reports them.
            const std::wstring files_path = std::wstring(L"/apiv11/Mod/") +
                std::wstring(id.begin(), id.end()) + L"/Files";
            const std::string files_body = LauncherHttpGet(L"gamebanana.com", files_path);
            if (const auto parsed = nlohmann::json::parse(files_body, nullptr, false);
                !parsed.is_discarded() && parsed.is_array() && !parsed.empty()) {
                remote_file = std::to_string(parsed[0].value("_idRow", 0));
                remote_md5 = parsed[0].value("_sMd5Checksum", std::string{});
                if (remote_version.empty()) {
                    remote_version = parsed[0].value("_sVersion", std::string{});
                }
            }
        }
        const std::string& local_file = source.file_id;
        const std::string& local_md5 = source.file_md5;
        if (!remote_file.empty() && !local_file.empty()) {
            // File identity wins: versions are optional, file ids are not. A same-id file replaced
            // in place is caught by the checksum.
            if (remote_file != local_file ||
                (!remote_md5.empty() && !local_md5.empty() && remote_md5 != local_md5)) {
                entry.remote_version = remote_version.empty() ? "?" : remote_version;
                discovered.emplace_back(LauncherModStateKey(entry), entry.remote_version);
                ++updated;
            } else {
                ++unchanged;
            }
            continue;
        }
        // Missing file identity cannot establish whether the installed archive changed.
        ++unrecorded;
    }
    char summary[320]{};
    std::snprintf(summary, sizeof(summary), "%s",
        Tr(anomaly::MessageId::LauncherCheckSummary,
            {std::to_string(mods.size()), std::to_string(unchanged), std::to_string(updated),
                std::to_string(adopted), std::to_string(failed), std::to_string(skipped),
                std::to_string(unrecorded)}).c_str());
    return summary;
}


// Downloads the newest file of a GameBanana mod and swaps it in, keeping icon.png / mod.json.
std::string LauncherUpdateMod(
    const LauncherSnapshot& snapshot, const LauncherModEntry& entry) {
    try {
        if (!LauncherCanEditMod(snapshot, entry)) return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        const std::string id = LauncherGameBananaId(entry.link);
        if (id.empty()) return Tr(anomaly::MessageId::LauncherMsgNoSource);
        const std::wstring profile_path = std::wstring(L"/apiv11/Mod/") +
            std::wstring(id.begin(), id.end()) + L"/ProfilePage";
        const std::string body = LauncherHttpGet(L"gamebanana.com", profile_path);
        if (body.empty()) return Tr(anomaly::MessageId::LauncherMsgFetchDownloadFailed);
        const std::string download = LauncherJsonField(body, "_sDownloadUrl");
        if (download.empty()) return Tr(anomaly::MessageId::LauncherMsgNoDownloadableFile);

        std::error_code error;
        const auto archive = std::filesystem::temp_directory_path(error) / L"anomaly-mod-update.zip";
        if (!LauncherDownloadUrl(download, archive)) {
            return Tr(anomaly::MessageId::LauncherMsgDownloadFailed);
        }

        const auto target = entry.directory ? entry.path : LauncherModsDirectory(snapshot) / LauncherWiden(entry.name);
        if (!entry.directory && std::filesystem::exists(target, error))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        auto extracted = anomaly::launcher::ExtractModArchive(archive);
        std::filesystem::remove(archive, error);
        if (!extracted.Ok()) return LauncherArchiveFailure(extracted);
        const auto& staging = extracted.directory;
        // Inspect the replacement before changing installed files. An empty or broken archive must
        // not delete the working mod, and flattening duplicate basenames must not silently lose data.
        std::vector<std::filesystem::path> replacements;
        std::set<std::wstring> names;
        bool has_pak{};
        for (const auto& item : std::filesystem::recursive_directory_iterator(staging, error)) {
            if (!item.is_regular_file(error)) continue;
            const auto name = item.path().filename();
            auto lower = name.wstring();
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            const bool pak = lower.ends_with(L".pak");
            if (!pak && !lower.ends_with(L".utoc") && !lower.ends_with(L".ucas") &&
                !lower.ends_with(L".sig") && lower != L"mod.json") continue;
            if (!names.insert(lower).second) return Tr(anomaly::MessageId::LauncherMsgExtractFailed);
            has_pak = has_pak || pak;
            replacements.push_back(item.path());
        }
        if (error || !has_pak) return Tr(anomaly::MessageId::LauncherMsgUpdateNoPak);
        bool enable_after{};
        if (!LauncherPrepareModWrite(snapshot, entry, enable_after))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        const auto target_mod = LauncherDirectoryMod(target);
        if (!entry.directory && !LauncherWritePakSettings(snapshot, [&](auto& document) {
                anomaly::SetPakModEnabled(document["modStates"], LauncherModStateKey(target_mod), false);
            })) return Tr(anomaly::MessageId::SettingsSaveFailed);
        std::filesystem::create_directories(target, error);
        if (error) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
        std::set<std::filesystem::path> containers;
        std::size_t copied{};
        for (const auto& file : replacements) {
            auto destination = target / file.filename();
            if (LauncherIsModFile(file)) destination += L".disabled";
            std::filesystem::copy_file(file, destination,
                std::filesystem::copy_options::overwrite_existing, error);
            if (error) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
            if (LauncherIsModFile(file)) containers.insert(destination);
            ++copied;
        }
        if (!LauncherRemoveObsoleteContainers(target_mod, containers))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        if (!entry.directory) {
            if (!LauncherWriteModNotes(target_mod, entry.custom_name, entry.notes, entry.custom_group))
                return Tr(anomaly::MessageId::SettingsSaveFailed);
            if (!entry.cover.empty()) {
                std::filesystem::copy_file(entry.cover, target / (L"icon" + entry.cover.extension().wstring()),
                    std::filesystem::copy_options::overwrite_existing, error);
                if (error) return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
            }
            if (!LauncherRemoveMod(snapshot, entry)) return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        }
        std::filesystem::remove(archive, error);
        LauncherWriteModSource(target, id, LauncherGameBananaFileId(body),
            LauncherGameBananaFileName(body), LauncherGameBananaFileMd5(body),
            LauncherGameBananaAuthor(body), entry.display_name);
        if (!LauncherFinishModWrite(snapshot, target_mod, enable_after))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        return copied == 0
            ? Tr(anomaly::MessageId::LauncherMsgUpdateNoPak)
            : Tr(anomaly::MessageId::LauncherMsgUpdated, {std::to_string(copied)});
    } catch (...) { return Tr(anomaly::MessageId::LauncherModsFileChangeFailed); }
}



// --- online browse ---------------------------------------------------------
struct LauncherGbItem final {
    std::string id;
    std::string name;
    std::string author;
    std::string thumb_url;
    std::string large_url;  // 530px preview shown on hover
    std::string page_url;   // "_sProfileUrl": the GameBanana page for this mod
    std::string downloads;  // "_nDownloadCount" as text (only the /Files endpoint reports it)
    std::string category;      // "_aRootCategory._sName" (UI, Visuals, ...)
    std::string category_id;   // numeric id from the category profile URL
    bool mature{};          // "_bHasContentRatings"
    std::string likes;      // "_nLikeCount" from the index feed
    std::string views;      // "_nViewCount" from the index feed
    std::string added;      // "_tsDateAdded" as unix seconds
    std::string size;       // filled in later from the mod page ("_nFilesize")
};

// File sizes live on the mod page, not in the list feed; they are fetched in the background and
// kept for the session.
std::map<std::string, std::string> g_gb_sizes;
std::map<std::string, std::string> g_gb_downloads;
// Category id -> name, accumulated from the records the browse page has seen.
std::map<std::string, std::string> g_gb_categories;
std::set<std::string> g_gb_size_requested;
std::mutex g_gb_size_mutex;

std::string LauncherFormatDate(const std::string& unix_seconds) {
    if (unix_seconds.empty()) return {};
    const long long value = std::atoll(unix_seconds.c_str());
    if (value <= 0) return {};
    const std::time_t when = static_cast<std::time_t>(value);
    std::tm local{};
    if (localtime_s(&local, &when) != 0) return {};
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return text;
}

std::string LauncherFormatBytes(const std::string& bytes_text) {
    const double bytes = std::atof(bytes_text.c_str());
    if (bytes <= 0) return {};
    if (bytes >= 1024.0 * 1024.0 * 1024.0) {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return text;
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
    return text;
}

void LauncherRequestSize(const std::string& mod_id) {
    if (mod_id.empty()) return;
    {
        std::scoped_lock lock(g_gb_size_mutex);
        if (g_gb_size_requested.count(mod_id) != 0) return;
        g_gb_size_requested.insert(mod_id);
    }
    std::thread([mod_id] {
        const std::wstring path = std::wstring(L"/apiv11/Mod/") +
            std::wstring(mod_id.begin(), mod_id.end()) + L"/Files";
        const std::string body = LauncherHttpGet(L"gamebanana.com", path);
        std::string size;
        std::string downloads;
        if (const auto parsed = nlohmann::json::parse(body, nullptr, false);
            !parsed.is_discarded() && parsed.is_array() && !parsed.empty()) {
            size = std::to_string(parsed[0].value("_nFilesize", 0));
            // The list feed omits download counts; the per-mod file listing still reports them.
            downloads = std::to_string(parsed[0].value("_nDownloadCount", 0));
        }
        std::scoped_lock lock(g_gb_size_mutex);
        g_gb_sizes[mod_id] = size;
        g_gb_downloads[mod_id] = downloads;
    }).detach();
}

// Thumbnails download off the render thread: the UI thread only decodes bytes that already
// arrived, so scrolling never blocks on the network.
struct LauncherThumbCache final {
    std::mutex mutex;
    std::set<std::string> finished;   // thumbnails whose file exists in the cache
    std::set<std::string> requested;  // downloads already started
};
LauncherThumbCache g_thumb_cache;
std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> g_thumb_textures;
// Decoded pixel size per texture: GameBanana thumbs are 220x90, not 16:9, so the list draws them
// at their own aspect ratio instead of a fixed box.
std::map<std::string, std::pair<int, int>> g_thumb_sizes;

std::string LauncherUrlEncode(const std::string& text) {
    std::string out;
    char buffer[4]{};
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            std::snprintf(buffer, sizeof(buffer), "%%%02X", static_cast<unsigned char>(c));
            out += buffer;
        }
    }
    return out;
}

// NTE on GameBanana is game 23012 and the service forces 15 records per page (391 mods at the
// time of writing). The launcher loads one page at a time and filters locally, because the API
// ignores the search parameter on this endpoint.
constexpr int kNteGameBananaGameId = 23012;


std::filesystem::path LauncherThumbCacheDirectory() {
    return ExecutableDirectory() / L"Anomaly" / L"cache" / L"gb-thumbs";
}

std::string LauncherThumbKey(const std::string& url) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char c : url) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(text) + ".img";
}



// The local proxy is dramatically faster for GameBanana on this network (measured: ~5 MB/s via
// proxy vs ~14 KB/s direct), so it is tried first and direct is the fallback.
std::wstring LauncherSystemProxy() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_READ,
            &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD enabled{};
    DWORD size = sizeof(enabled);
    DWORD type{};
    std::wstring proxy;
    if (RegQueryValueExW(key, L"ProxyEnable", nullptr, &type,
            reinterpret_cast<LPBYTE>(&enabled), &size) == ERROR_SUCCESS && enabled != 0) {
        wchar_t buffer[512]{};
        DWORD bytes = sizeof(buffer);
        if (RegQueryValueExW(key, L"ProxyServer", nullptr, &type,
                reinterpret_cast<LPBYTE>(buffer), &bytes) == ERROR_SUCCESS) {
            proxy = buffer;
        }
    }
    RegCloseKey(key);
    return proxy;
}

// Splits a URL with WinHTTP itself instead of hand-rolled string slicing: the manual version
// produced a host of "https:\" and the request failed with 12005.
bool LauncherDownloadUrl(const std::string& url, const std::filesystem::path& destination,
    LauncherDownloadProgress* const progress) {
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE) {
        LauncherUpdateLog("crack failed " + std::to_string(GetLastError()) + " url=" + url);
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const INTERNET_PORT port = parts.nPort == 0 ? INTERNET_DEFAULT_HTTPS_PORT : parts.nPort;
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring proxy = LauncherSystemProxy();
    HINTERNET session = WinHttpOpen(L"AnomalyLauncher/1.0",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        LauncherUpdateLog("url dl: session failed " + std::to_string(GetLastError()));
        return false;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    bool ok{};
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection == nullptr) {
        LauncherUpdateLog("url dl: connect failed " + std::to_string(GetLastError()) +
            " host=" + std::string(host.begin(), host.end()));
    }
    if (connection != nullptr) {
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (request != nullptr) {
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                sizeof(redirect_policy));
            const wchar_t* headers =
                L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AnomalyLauncher/1.0\r\n"
                L"Accept: */*\r\n";
            if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                    0, 0, 0) != FALSE &&
                WinHttpReceiveResponse(request, nullptr) != FALSE) {
                if (progress != nullptr) {
                    progress->bytes.store(0);
                    wchar_t length_text[32]{};
                    DWORD length_size = sizeof(length_text);
                    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size,
                            WINHTTP_NO_HEADER_INDEX) != FALSE) {
                        progress->total.store(_wtoi64(length_text));
                    } else {
                        progress->total.store(0);
                    }
                }
                std::filesystem::create_directories(destination.parent_path());
                std::ofstream out(destination, std::ios::binary | std::ios::trunc);
                if (out) {
                    ok = true;
                    // Read until zero bytes come back (true EOF); QueryDataAvailable returning 0
                    // only means "nothing right now" and used to truncate the payload.
                    std::string chunk(64 * 1024, '\0');
                    const auto started = std::chrono::steady_clock::now();
                    long long received{};
                    DWORD read{};
                    for (;;) {
                        if (!WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                            ok = false;
                            break;
                        }
                        if (read == 0) break;
                        out.write(chunk.data(), static_cast<std::streamsize>(read));
                        if (!out) { ok = false; break; }
                        received += static_cast<long long>(read);
                        if (progress != nullptr) progress->bytes.fetch_add(static_cast<long long>(read));
                        read = 0;
                        if (progress != nullptr && progress->cancel.load()) {
                            LauncherUpdateLog("url dl: cancelled by user");
                            ok = false;
                            break;
                        }
                        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
                            LauncherUpdateLog("url dl: total timeout after " +
                                std::to_string(received) + " bytes");
                            ok = false;
                            break;
                        }
                    }
                    out.close();
                    if (!out) ok = false;
                    if (ok && received == 0) {
                        LauncherUpdateLog("url dl: empty body " + url);
                        ok = false;
                    }
                }
            } else {
                LauncherUpdateLog("url dl: request failed " + std::to_string(GetLastError()) +
                    " url=" + url);
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    if (!ok) {
        std::error_code error;
        std::filesystem::remove(destination, error);
    }
    return ok;
}

void LauncherRequestThumb(const std::string& url) {
    if (url.empty()) return;
    {
        std::scoped_lock lock(g_thumb_cache.mutex);
        if (g_thumb_cache.requested.count(url) != 0) return;
        g_thumb_cache.requested.insert(url);
    }
    const auto target = LauncherThumbCacheDirectory() / LauncherThumbKey(url);
    std::error_code error;
    if (std::filesystem::is_regular_file(target, error) &&
        std::filesystem::file_size(target, error) > 0) {
        std::scoped_lock lock(g_thumb_cache.mutex);
        g_thumb_cache.finished.insert(url);
        return;
    }
    std::thread([url, target] {
        const bool downloaded = LauncherDownloadUrl(url, target);
        if (downloaded) {
            std::scoped_lock lock(g_thumb_cache.mutex);
            g_thumb_cache.finished.insert(url);
        }
    }).detach();
}

ID3D11ShaderResourceView* LauncherThumbTexture(const std::string& url) {
    if (url.empty() || g_graphics == nullptr) return nullptr;
    if (const auto found = g_thumb_textures.find(url); found != g_thumb_textures.end()) {
        return found->second.Get();
    }
    bool ready{};
    {
        std::scoped_lock lock(g_thumb_cache.mutex);
        ready = g_thumb_cache.finished.count(url) != 0;
    }
    LauncherRequestThumb(url);
    if (!ready || g_decode_budget <= 0) return nullptr;
    --g_decode_budget;
    auto& texture = g_thumb_textures[url];
    std::ifstream stream(LauncherThumbCacheDirectory() / LauncherThumbKey(url), std::ios::binary);
    if (!stream) return nullptr;
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto decoded = anomaly::DecodeUiImageRgba8(std::span(bytes.data(), bytes.size()));
    if (!decoded) {
        LauncherUpdateLog("thumb decode failed: " + LauncherThumbKey(url) +
            " error=" + std::to_string(static_cast<int>(decoded.error)));
        return nullptr;
    }
    auto view = CreateTextureFromPixels(*g_graphics, decoded.image.pixels,
        decoded.image.width, decoded.image.height);
    if (view == nullptr) return nullptr;
    g_thumb_sizes[url] = {static_cast<int>(decoded.image.width),
        static_cast<int>(decoded.image.height)};
    texture = std::move(view);
    return texture.Get();
}

bool LauncherLoadGbPage(const int page, const std::string& query, const std::string& sort,
    const std::string& category_id, std::vector<LauncherGbItem>& items, bool& exhausted) {
    // Empty query browses the game feed; a query goes through the search endpoint. The game filter
    // is `_idGameRow`; `_idGame` is silently ignored.
    std::string path_text;
    // Browsing uses apiv13's index, the only endpoint that orders the whole game's mods by
    // downloads; keyword search only exists on apiv11, which has no sort parameter at all.
    if (query.empty()) {
        std::string sort_arg;
        if (sort == "new") {
            sort_arg = "&_sSort=Generic_Newest";
        } else if (sort == "downloads") {
            sort_arg = "&_sSort=Generic_MostDownloaded";
        } else if (sort == "views") {
            sort_arg = "&_sSort=Generic_MostViewed";
        } else if (sort == "likes") {
            sort_arg = "&_sSort=Generic_MostLiked";
        }
        const std::string category_arg = category_id.empty()
            ? std::string{}
            : "&_aFilters%5BGeneric_Category%5D=" + category_id;
        path_text = "/apiv13/Mod/Index?_nPage=" + std::to_string(page) +
            "&_nPerpage=15&_aFilters%5BGeneric_Game%5D=" +
            std::to_string(kNteGameBananaGameId) + sort_arg + category_arg;
    } else {
        // apiv13 search sorts by relevance (`_sOrder=best_match`; other values are rejected) and
        // returns like/view counts, which apiv11 omits.
        path_text = "/apiv13/Util/Search/Results?_sOrder=best_match&_idGameRow=" +
            std::to_string(kNteGameBananaGameId) + "&_sSearchString=" + LauncherUrlEncode(query) +
            "&_csvFields=name&_nPage=" + std::to_string(page);
    }
    const std::wstring path(path_text.begin(), path_text.end());
    const std::string body = LauncherHttpGet(L"gamebanana.com", path);
    if (body.empty()) return false;
    // Parsed properly: every record carries a nested `_aSubmitter` object that also has
    // `_idRow`/`_sName`, which is what the earlier hand-rolled scan mistook for extra mods.
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        LauncherUpdateLog("browse: json parse failed");
        return false;
    }
    const auto records = parsed.find("_aRecords");
    if (records == parsed.end() || !records->is_array()) return false;
    // Filtering every row on a page must not hide later pages of eligible mods.
    exhausted = records->empty();
    for (const auto& record : *records) {
        if (!record.is_object()) continue;
        const int id = record.value("_idRow", 0);
        if (id <= 0) continue;
        LauncherGbItem item;
        item.id = std::to_string(id);
        item.name = record.value("_sName", std::string{});
        if (anomaly::PakModNameRestricted(item.name)) continue;
        item.page_url = record.value("_sProfileUrl", std::string{});
        if (const auto root = record.find("_aRootCategory");
            root != record.end() && root->is_object()) {
            item.category = root->value("_sName", std::string{});
            const std::string url = root->value("_sProfileUrl", std::string{});
            const auto slash = url.find_last_of('/');
            if (slash != std::string::npos) {
                item.category_id = url.substr(slash + 1);
            }
        }
        // Check the content-rating flag, explicit NSFW flag, and an NSFW
        // category name (root or sub) — the category is what most NTE mods rely on.
        {
            const auto contains_nsfw = [](const nlohmann::json& node) {
                if (!node.is_object()) return false;
                std::string name = node.value("_sName", std::string{});
                std::transform(name.begin(), name.end(), name.begin(),
                    [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find("nsfw") != std::string::npos;
            };
            // `_bHasContentRatings` is the only signal NTE actually populates: the dedicated NSFW
            // flags are empty there, and no mod sits in an "nsfw" category.
            item.mature = record.value("_bHasNsfwContent", false) ||
                record.value("_bIsNsfw", false) ||
                record.value("_bHasContentRatings", false);
            if (!item.mature) {
                if (const auto root = record.find("_aRootCategory"); root != record.end()) {
                    item.mature = contains_nsfw(*root);
                }
            }
            if (!item.mature) {
                if (const auto sub = record.find("_aSubCategory"); sub != record.end()) {
                    item.mature = contains_nsfw(*sub);
                }
            }
        }
        item.likes = std::to_string(record.value("_nLikeCount", 0));
        item.views = std::to_string(record.value("_nViewCount", 0));
        item.added = std::to_string(record.value("_tsDateAdded", 0));
        if (const auto submitter = record.find("_aSubmitter");
            submitter != record.end() && submitter->is_object()) {
            item.author = submitter->value("_sName", std::string{});
        }
        if (const auto media = record.find("_aPreviewMedia");
            media != record.end() && media->is_object()) {
            // apiv11 shape: an array of images with jpg thumbnails.
            if (const auto images = media->find("_aImages");
                images != media->end() && images->is_array() && !images->empty()) {
                const auto& image = (*images)[0];
                const std::string base = image.value("_sBaseUrl", std::string{});
                if (!base.empty()) {
                    const auto thumb = image.value("_sFile220", std::string{});
                    const auto large = image.value("_sFile530", std::string{});
                    if (!thumb.empty()) item.thumb_url = base + "/" + thumb;
                    if (!large.empty()) item.large_url = base + "/" + large;
                }
            }
        } else if (const auto content = record.find("_aPreviewContent");
            content != record.end() && content->is_object()) {
            // apiv13 shape: `screenshot` is a single object, and its thumbnails are .webp — the
            // original is a jpg that WIC can always decode.
            for (auto it = content->begin(); it != content->end(); ++it) {
                if (!it.value().is_object()) continue;
                const std::string base = it.value().value("_sBaseUrl", std::string{});
                const std::string original = it.value().value("_sFile", std::string{});
                if (base.empty() || original.empty()) continue;
                item.thumb_url = base + "/" + original;
                item.large_url = item.thumb_url;
                break;
            }
        }
        if (item.name.empty()) continue;
        items.push_back(std::move(item));
    }
    return true;
}


// JSON strings are UTF-8, but std::filesystem::path(std::string) interprets the bytes in the ANSI
// code page, which turned Chinese mod names into mojibake folder names.
std::wstring LauncherWiden(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

// Folder names cannot carry the Windows-reserved characters.




// WinHTTP stalls on some GameBanana responses; curl is present on Windows 10+ and handles the
// redirect chain reliably, so it is the fallback when the built-in path fails.
bool LauncherDownloadWithCurl(const std::string& url, const std::filesystem::path& destination) {
    if (g_mod_download.cancel.load()) return false;
    const std::wstring wide(url.begin(), url.end());
    std::error_code error;
    std::filesystem::remove(destination, error);
    const std::wstring proxy = LauncherSystemProxy();
    const std::wstring proxy_arg = proxy.empty() ? std::wstring{}
        : (L"--proxy http://" + proxy + L" ");
    const std::wstring command = L"curl.exe -L --fail --silent --show-error --max-time 600 " +
        proxy_arg + L"-o \"" + destination.wstring() + L"\" \"" + wide + L"\"";
    if (!LauncherRunAndWait(command, 660000)) return false;
    return std::filesystem::is_regular_file(destination, error) &&
        std::filesystem::file_size(destination, error) > 0;
}

// Pulls the newest file entry of a mod page. nlohmann keeps the nested `_aFiles` array honest.
struct LauncherGbFile final {
    std::string id;
    std::string name;
    std::string url;
    std::string md5;
};

bool LauncherReadGbFile(const std::string& body, LauncherGbFile& file, std::string& author,
    std::string& version) {
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return false;
    version = parsed.value("_sVersion", std::string{});
    if (const auto submitter = parsed.find("_aSubmitter");
        submitter != parsed.end() && submitter->is_object()) {
        author = submitter->value("_sName", std::string{});
    }
    const auto files = parsed.find("_aFiles");
    if (files == parsed.end() || !files->is_array() || files->empty()) return false;
    const auto& first = (*files)[0];
    file.id = std::to_string(first.value("_idRow", 0));
    file.name = first.value("_sFile", std::string{});
    file.url = first.value("_sDownloadUrl", std::string{});
    file.md5 = first.value("_sMd5Checksum", std::string{});
    return !file.url.empty();
}


// Some mods hide their files from ProfilePage; `/Mod/<id>/Files` always lists them.
bool LauncherFetchFileFallback(const std::string& mod_id, LauncherGbFile& file,
    std::string& version) {
    const std::wstring path = std::wstring(L"/apiv11/Mod/") +
        std::wstring(mod_id.begin(), mod_id.end()) + L"/Files";
    const std::string body = LauncherHttpGet(L"gamebanana.com", path);
    if (body.empty()) return false;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array() || parsed.empty()) return false;
    const auto& first = parsed[0];
    if (!first.is_object()) return false;
    file.id = std::to_string(first.value("_idRow", 0));
    file.name = first.value("_sFile", std::string{});
    file.url = first.value("_sDownloadUrl", std::string{});
    file.md5 = first.value("_sMd5Checksum", std::string{});
    version = first.value("_sVersion", version);
    return !file.url.empty();
}

// Downloads one GameBanana mod straight into ~mods/<name>/ and records its source.
std::string LauncherInstallFromGameBanana(
    const LauncherSnapshot& snapshot, const std::string& mod_id, const std::string& display_name) {
    try {
        if (anomaly::PakModNameRestricted(display_name))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        const std::wstring profile_path = std::wstring(L"/apiv11/Mod/") +
            std::wstring(mod_id.begin(), mod_id.end()) + L"/ProfilePage";
        std::string body = LauncherHttpGet(L"gamebanana.com", profile_path);
        if (body.empty()) {
            LauncherUpdateLog("install: page fetch (proxy) empty for mod " + mod_id);
            body = LauncherHttpGet(L"gamebanana.com", profile_path, true);
        }
        if (body.empty()) {
            LauncherUpdateLog("install: page fetch (direct) empty for mod " + mod_id);
            return Tr(anomaly::MessageId::LauncherMsgFetchPageFailed);
        }
        LauncherGbFile file;
        std::string author;
        std::string version;
        if (!LauncherReadGbFile(body, file, author, version)) {
            // ProfilePage omits `_aFiles` for some mods; the /Files endpoint still has them.
            if (!LauncherFetchFileFallback(mod_id, file, version)) {
                LauncherUpdateLog("install: no files for mod " + mod_id);
                return Tr(anomaly::MessageId::LauncherMsgNoFilesAnywhere);
            }
            LauncherUpdateLog("install: used /Files fallback for mod " + mod_id);
        }
        LauncherUpdateLog("install: mod " + mod_id + " file=" + file.name + " url=" + file.url);

        std::error_code error;
        const auto archive = std::filesystem::temp_directory_path(error) / L"anomaly-browse-install.bin";
        std::filesystem::remove(archive, error);
        if (!LauncherDownloadUrl(file.url, archive, &g_mod_download)) {
            if (g_mod_download.cancel.load()) {
                std::filesystem::remove(archive, error);
                return Tr(anomaly::MessageId::LauncherMsgCancelled);
            }
            LauncherUpdateLog("install: WinHTTP failed, trying curl: " + file.url);
            if (!LauncherDownloadWithCurl(file.url, archive)) {
                LauncherUpdateLog("install: curl failed " + file.url);
                return Tr(anomaly::MessageId::LauncherMsgDownloadFailedBoth);
            }
        }
        auto extracted = anomaly::launcher::ExtractModArchive(archive, &g_mod_download.cancel);
        std::filesystem::remove(archive, error);
        if (!extracted.Ok()) return LauncherArchiveFailure(extracted);
        const auto& staging = extracted.directory;

        const std::wstring folder_name = LauncherSafeFolderName(display_name);
        const auto target = LauncherModsDirectory(snapshot) /
            (folder_name.empty() ? L"gamebanana-mod" : folder_name);
        const bool target_existed = std::filesystem::exists(target, error);
        const auto target_mod = LauncherDirectoryMod(target);
        bool enable_after{};
        // Copy the whole tree, not just the pak triplet: some mods ship companion assets (image sets,
        // scripts) that a tool reads at runtime, and dropping them silently breaks the mod.
        if (g_mod_download.cancel.load()) return Tr(anomaly::MessageId::LauncherMsgCancelled);
        if (!LauncherPrepareModWrite(snapshot, target_mod, enable_after))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        std::filesystem::create_directories(target, error);
        if (error) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
        std::set<std::filesystem::path> containers;
        std::size_t copied{};
        std::size_t pak_files{};
        for (const auto& item : std::filesystem::recursive_directory_iterator(staging, error)) {
            if (!item.is_regular_file(error)) continue;
            std::wstring extension = item.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](const wchar_t c) {
                    return static_cast<wchar_t>(c >= L'A' && c <= L'Z' ? c + 32 : c);
                });
            if (extension == L".pak" || extension == L".utoc" || extension == L".ucas") {
                ++pak_files;
            }
            auto relative = std::filesystem::relative(item.path(), staging, error);
            if (error || relative.empty()) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
            auto destination = target / relative;
            if (LauncherIsModFile(destination) && !destination.wstring().ends_with(L".disabled"))
                destination += L".disabled";
            std::filesystem::create_directories(destination.parent_path(), error);
            std::filesystem::copy_file(item.path(), destination,
                std::filesystem::copy_options::overwrite_existing, error);
            if (error) return Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
            if (LauncherIsModFile(destination)) containers.insert(destination);
            ++copied;
        }
        std::filesystem::remove(archive, error);
        if (copied == 0 || pak_files == 0) {
            if (!target_existed) std::filesystem::remove_all(target, error);
            return pak_files == 0
                ? Tr(anomaly::MessageId::LauncherMsgNoPakFiles)
                : Tr(anomaly::MessageId::LauncherMsgNoCopiedFiles);
        }
        // Archives rarely ship an icon; use the GameBanana cover so the mod list has a thumbnail.
        if (const auto parsed = nlohmann::json::parse(body, nullptr, false); parsed.is_object()) {
            if (const auto media = parsed.find("_aPreviewMedia");
                media != parsed.end() && media->is_object()) {
                if (const auto images = media->find("_aImages");
                    images != media->end() && images->is_array() && !images->empty()) {
                    const auto& image = (*images)[0];
                    const std::string base = image.value("_sBaseUrl", std::string{});
                    const std::string file_name = image.value("_sFile", image.value("_sFile530", std::string{}));
                    const std::filesystem::path icon_target = target / L"icon.png";
                    if (!base.empty() && !file_name.empty() &&
                        !std::filesystem::is_regular_file(icon_target, error)) {
                        LauncherDownloadUrl(base + "/" + file_name, icon_target);
                    }
                }
            }
        }
        LauncherWriteModSource(target, mod_id, file.id, file.name, file.md5, author, display_name);
        if (!containers.empty() && !LauncherRemoveObsoleteContainers(target_mod, containers))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        if (!LauncherFinishModWrite(snapshot, target_mod, enable_after))
            return Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
        return Tr(anomaly::MessageId::LauncherMsgInstalled);
    } catch (...) { return Tr(anomaly::MessageId::LauncherModsFileChangeFailed); }
}


// Which GameBanana mods are already in ~mods, taken from the recorded source files.
std::set<std::string> LauncherInstalledModIds(const LauncherSnapshot& snapshot) {
    std::set<std::string> ids;
    const auto directory = LauncherModsDirectory(snapshot);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return ids;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_directory(error)) continue;
        const auto source = LauncherReadModSource(entry.path());
        if (!source.mod_id.empty()) ids.insert(source.mod_id);
    }
    return ids;
}

void DrawBrowseMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    (void)window;
    if (snapshot.game_directory.empty()) {
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherStatusSelectGameDirectory));
        return;
    }
    g_decode_budget = 1;  // one image per frame
    struct PageResult final {
        int page{};
        bool ok{}, exhausted{};
        std::vector<LauncherGbItem> items;
    };
    static char search[96]{};
    static std::vector<LauncherGbItem> items;
    static int loaded_pages{};
    static bool exhausted{};
    static bool replace_pending{};
    static std::string status;
    static std::mutex browse_mutex;
    static bool browse_loading{};
    static std::optional<PageResult> browse_outcome;
    static std::mutex install_mutex;
    static std::atomic<bool> install_running{false};
    static std::string install_result;
    static bool install_ready{};
    static std::string active_query;   // "" = browse the game feed
    static std::string active_sort;    // "default" | "new" | "downloads" | "views" | "likes"
    static std::string active_category;  // numeric category id, empty = all
    static std::set<std::string> installed_ids;
    static double installed_refreshed{};
    static std::string install_target;   // id of the row currently installing
    static double install_started{};     // ImGui::GetTime() when it began
    // NSFW is a local filter because the feed has no maturity parameter.
    static bool hide_mature{true};
    // The order the loaded set is currently in. The client-side sorts read sizes and download counts
    // that background requests fill in later, so ordering has to be a decision taken once per sort
    // choice rather than a comparison re-run every frame: re-running it rearranged the whole list
    // whenever one of those numbers arrived, and appending a page moved every row already on screen.
    static int applied_sort_mode{-1};
    static bool applied_sort_descending{};

    const auto load_page = [&](const int page) {
        browse_loading = true;
        status.clear();
        if (page == 1) replace_pending = true;
        std::thread([page, query = active_query, sort = active_sort, category = active_category] {
            PageResult result;
            result.page = page;
            try {
                result.ok = LauncherLoadGbPage(page, query, sort, category, result.items, result.exhausted);
            } catch (...) {
                LauncherUpdateLog("browse: page load failed");
            }
            std::scoped_lock lock(browse_mutex);
            browse_outcome = std::move(result);
        }).detach();
    };
    // The sort, the category filter and the NSFW switch are stored in the launcher
    // configuration, so this page opens where it was left; the snapshot carries them.
    static bool prefs_loaded{};
    if (!prefs_loaded) {
        active_sort = snapshot.view.browse_sort;
        active_category = snapshot.view.browse_category;
        hide_mature = snapshot.view.browse_hide_mature;
        prefs_loaded = true;
        load_page(1);
    }
    {
        std::scoped_lock lock(browse_mutex);
        if (browse_outcome) {
            auto result = std::move(*browse_outcome);
            browse_outcome.reset();
            browse_loading = false;
            if (!result.ok) {
                status = Tr(anomaly::MessageId::LauncherMsgLoadFailedNetwork);
            } else {
                for (const auto& item : result.items) {
                    if (!item.category_id.empty() && !item.category.empty())
                        g_gb_categories[item.category_id] = item.category;
                }
                exhausted = result.exhausted;
                loaded_pages = result.page;
                if (result.page == 1) {
                    items = std::move(result.items);
                    replace_pending = false;
                    // A new feed is ordered on the next frame; the pages appended to it never are.
                    applied_sort_mode = -1;
                } else {
                    items.insert(items.end(), std::make_move_iterator(result.items.begin()),
                        std::make_move_iterator(result.items.end()));
                }
                status = exhausted
                    ? Tr(items.empty() ? anomaly::MessageId::LauncherMsgNoResults
                                       : anomaly::MessageId::LauncherMsgLastPage)
                    : Tr(anomaly::MessageId::LauncherMsgLoadedCount, {std::to_string(items.size())});
            }
        }
    }
    // The controls stay put and only the rows scroll, the way the mods page works: the toolbar is
    // part of the page, and the list below it is the one thing that moves.
    ImGui::BeginDisabled(browse_loading);
    const bool refresh_clicked = LauncherChip(Text(translator, anomaly::MessageId::LauncherModsRefreshList));
    ImGui::EndDisabled();
    if (refresh_clicked) load_page(1);
    ImGui::SameLine();
    if (browse_loading) {
        // Text that shares a line with a chip aligns to the frame padding, the same way a label
        // beside a framed widget does; left on the line's top edge it reads as misaligned.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Tr(anomaly::MessageId::LauncherMsgLoading).c_str());
    } else {
        ImGui::BeginDisabled(exhausted && !replace_pending);
        const bool more_clicked = LauncherChip(exhausted && !replace_pending
            ? Text(translator, anomaly::MessageId::LauncherBrowseAllLoaded)
            : Text(translator, anomaly::MessageId::LauncherBrowseLoadMore));
        ImGui::EndDisabled();
        // If a new feed failed, retry its first page instead of appending it to the old feed.
        if (more_clicked) load_page(replace_pending ? 1 : loaded_pages + 1);
    }
    ImGui::BeginDisabled(browse_loading);
    ImGui::SetNextItemWidth(Scale(280.0f));
    ImGui::InputTextWithHint("##gb-filter",
        Text(translator, anomaly::MessageId::LauncherBrowseSearchHint), search, sizeof(search));
    ImGui::EndDisabled();
    {
        std::scoped_lock lock(install_mutex);
        if (install_ready) {
            install_ready = false;
            status = install_result;
            install_target.clear();
            installed_refreshed = -5.0;
        }
    }
    static std::filesystem::path installed_game;
    if (installed_game != snapshot.game_directory || ImGui::GetTime() - installed_refreshed > 5.0) {
        installed_game = snapshot.game_directory;
        installed_refreshed = ImGui::GetTime();
        installed_ids = LauncherInstalledModIds(snapshot);
    }
    if (std::any_of(search, search + std::strlen(search),
            [](const char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
        ImGui::TextDisabled("%s",
            Tr(anomaly::MessageId::LauncherBrowseCjkHint).c_str());
    }
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
    ImGui::Separator();

    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Category browsing is a server-side filter (the id from the category profile URL).
    std::vector<std::string> category_ids;
    std::vector<const char*> category_labels;
    category_labels.push_back(Text(translator, anomaly::MessageId::LauncherBrowseAllCategories));
    for (const auto& [id, name] : g_gb_categories) {
        category_ids.push_back(id);
        category_labels.push_back(name.c_str());
    }
    int category_index = 0;
    for (std::size_t i = 0; i < category_ids.size(); ++i) {
        if (category_ids[i] == active_category) category_index = static_cast<int>(i) + 1;
    }
    // The search endpoint ignores category filters, so the control simply disappears while a query
    // is active.
    const bool searching = !active_query.empty();
    bool category_changed = false;
    if (!searching) {
        ImGui::BeginDisabled(browse_loading);
        ImGui::SetNextItemWidth(Scale(130.0f));
        category_changed = ImGui::Combo("##category", &category_index,
            category_labels.data(), static_cast<int>(category_labels.size()));
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (category_changed) {
        active_category = category_index == 0 ? std::string{} : category_ids[category_index - 1];
        controller.SetBrowseView(active_sort, active_category, hide_mature);
        load_page(1);
    }
    if (anomaly::glass::Switch(Text(translator, anomaly::MessageId::LauncherBrowseHideNsfw), &hide_mature)) {
        controller.SetBrowseView(active_sort, active_category, hide_mature);
    }
    ImGui::SameLine();

    // Two kinds of ordering: the feed endpoint can order the whole set (default / new / updated),
    // while download count and size only exist per mod, so those sort the loaded slice.
    static int sort_mode{1};  // client-side: 1 name, 2 downloads, 3 size, 4 time
    static bool sort_descending{true};
    // One combo instead of a row of buttons: the server-side sorts are mutually exclusive.
    static const anomaly::MessageId server_sort_labels[] = {
        anomaly::MessageId::LauncherSortDefault, anomaly::MessageId::LauncherSortNewest,
        anomaly::MessageId::LauncherSortDownloads, anomaly::MessageId::LauncherSortViews,
        anomaly::MessageId::LauncherSortLikes};
    static const char* server_sort_values[] = {"default", "new", "downloads", "views", "likes"};
    int server_sort_index = 0;
    for (int i = 0; i < 5; ++i) {
        if (active_sort == server_sort_values[i]) server_sort_index = i;
    }
    ImGui::BeginDisabled(browse_loading);
    ImGui::SetNextItemWidth(Scale(140.0f));
    const char* server_sort_text[5]{};
    for (int i = 0; i < 5; ++i) server_sort_text[i] = Text(translator, server_sort_labels[i]);
    const bool server_sort_changed = ImGui::Combo("##server-sort", &server_sort_index, server_sort_text, 5);
    ImGui::EndDisabled();
    if (server_sort_changed) {
        const std::string chosen = server_sort_values[server_sort_index];
        if (chosen != active_sort) {
            active_sort = chosen;
            controller.SetBrowseView(active_sort, active_category, hide_mature);
            load_page(1);
        }
    }
    const std::array<const char*, 4> client_labels{
        Text(translator, anomaly::MessageId::LauncherSortName),
        Text(translator, anomaly::MessageId::LauncherSortDownloads),
        Text(translator, anomaly::MessageId::LauncherSortSize),
        Text(translator, anomaly::MessageId::LauncherSortTime)};
    anomaly::glass::SegmentedOptions sort_options;
    sort_options.height = 46.0f;
    sort_options.padding_x = 30.0f;
    sort_options.scale = g_launcher_dpi_scale;
    sort_options.activate_selected = true;
    const float next_x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
        ImGui::GetStyle().ItemSpacing.x;
    if (next_x + anomaly::glass::SegmentedControlWidth(client_labels, sort_options) <=
        ImGui::GetContentRegionMax().x) ImGui::SameLine();
    int selected_sort = sort_mode - 1;
    if (anomaly::glass::SegmentedControl(
            "browse-sort", client_labels, selected_sort, sort_options)) {
        if (sort_mode == selected_sort + 1) sort_descending = !sort_descending;
        sort_mode = selected_sort + 1;
    }
    Tooltip(Text(translator, anomaly::MessageId::LauncherBrowseLoadedOnly));

    // The order is applied once per sort choice, in place on the loaded set. Sorting a filtered view
    // every frame is what made a page load look like a reshuffle: the appended page arrived and the
    // whole list re-arranged around it. Here an appended page simply lands at the end, and only
    // picking a different sort (or a new feed) reorders what is already loaded.
    const auto size_of = [](const LauncherGbItem& item) {
        const auto found = g_gb_sizes.find(item.id);
        return found == g_gb_sizes.end() ? 0.0 : std::atof(found->second.c_str());
    };
    if (applied_sort_mode != sort_mode || applied_sort_descending != sort_descending) {
        applied_sort_mode = sort_mode;
        applied_sort_descending = sort_descending;
        // The comparison keys must remain fixed for the whole sort while download workers publish
        // their results. Per-comparison locking can change the ordering halfway through a sort.
        std::unique_lock sort_lock(g_gb_size_mutex, std::defer_lock);
        if (sort_mode == 2 || sort_mode == 3) sort_lock.lock();
        std::stable_sort(items.begin(), items.end(),
            [&](const LauncherGbItem& left, const LauncherGbItem& right) {
                int result{};
                if (sort_mode == 2) {
                    std::string a_text = left.downloads;
                    std::string b_text = right.downloads;
                    {
                        const auto a = g_gb_downloads.find(left.id);
                        const auto b = g_gb_downloads.find(right.id);
                        if (a != g_gb_downloads.end() && !a->second.empty()) a_text = a->second;
                        if (b != g_gb_downloads.end() && !b->second.empty()) b_text = b->second;
                    }
                    const auto a = std::atoll(a_text.c_str());
                    const auto b = std::atoll(b_text.c_str());
                    result = a < b ? -1 : a > b ? 1 : 0;
                } else if (sort_mode == 3) {
                    const double a = size_of(left);
                    const double b = size_of(right);
                    result = a < b ? -1 : a > b ? 1 : 0;
                } else if (sort_mode == 4) {
                    const auto a = std::atoll(left.added.c_str());
                    const auto b = std::atoll(right.added.c_str());
                    result = a < b ? -1 : a > b ? 1 : 0;
                } else {
                    result = left.name < right.name ? -1 : left.name > right.name ? 1 : 0;
                }
                return sort_descending ? result > 0 : result < 0;
            });
    }

    // What the list draws: the loaded set in its order, with the local filters applied. The order was
    // just fixed above, so this view never re-arranges anything by itself.
    std::vector<const LauncherGbItem*> visible;
    for (const auto& item : items) {
        if (hide_mature && item.mature) continue;
        if (!needle.empty()) {
            std::string haystack = item.name + " " + item.author;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (haystack.find(needle) == std::string::npos) continue;
        }
        visible.push_back(&item);
    }

    // The rows live in their own scrolling region, so the search box, the sort and the filters above
    // them never move while a long feed is being read.
    ImGui::BeginChild("BrowseList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    std::size_t shown{};
    for (const auto* pointer : visible) {
        const auto& item = *pointer;
        if (!needle.empty()) {
            std::string haystack = item.name + " " + item.author;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (haystack.find(needle) == std::string::npos) continue;
        }
        ++shown;
        // Fixed box with the image scaled to fit inside it: portrait previews stay readable
        // instead of collapsing into a thin sliver, and every row keeps the same height.
        const ImVec2 box(Scale(120.0f), Scale(68.0f));
        const ImVec2 box_origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(box);
        if (auto* thumb = LauncherThumbTexture(item.thumb_url); thumb != nullptr) {
            float width = box.x;
            float height = box.y;
            if (const auto size = g_thumb_sizes.find(item.thumb_url);
                size != g_thumb_sizes.end() && size->second.first > 0 && size->second.second > 0) {
                const float scale = (std::min)(box.x / static_cast<float>(size->second.first),
                    box.y / static_cast<float>(size->second.second));
                width = static_cast<float>(size->second.first) * scale;
                height = static_cast<float>(size->second.second) * scale;
            }
            const ImVec2 offset((box.x - width) * 0.5f, (box.y - height) * 0.5f);
            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(thumb),
                ImVec2(box_origin.x + offset.x, box_origin.y + offset.y),
                ImVec2(box_origin.x + offset.x + width, box_origin.y + offset.y + height));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (auto* large = LauncherThumbTexture(item.large_url); large != nullptr) {
                float height = Scale(270.0f);
                float width = Scale(480.0f);
                if (const auto size = g_thumb_sizes.find(item.large_url);
                    size != g_thumb_sizes.end() && size->second.second > 0) {
                    width = height * static_cast<float>(size->second.first) /
                        static_cast<float>(size->second.second);
                    if (width > Scale(720.0f)) {
                        width = Scale(720.0f);
                        height = width * static_cast<float>(size->second.second) /
                            static_cast<float>(size->second.first);
                    }
                }
                ImGui::Image(reinterpret_cast<ImTextureID>(large), ImVec2(width, height));
            } else {
                ImGui::TextDisabled("%s",
                    Tr(anomaly::MessageId::LauncherMsgLoadingPreview).c_str());
            }
            ImGui::TextDisabled("%s", item.name.c_str());
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s", item.name.c_str());
        LauncherRequestSize(item.id);
        // The row's numbers are collected before anything is drawn: they sit between the name and
        // the two commands, centred, so the row reads name / facts / commands from top to bottom.
        std::string info;
        if (!item.author.empty() && item.author != item.name) info = "· " + item.author;
        std::string download_text = item.downloads;
        {
            std::scoped_lock lock(g_gb_size_mutex);
            const auto found = g_gb_downloads.find(item.id);
            if (found != g_gb_downloads.end() && !found->second.empty() && found->second != "0") {
                download_text = found->second;
            }
        }
        if (!download_text.empty() && download_text != "0") {
            info += Tr(anomaly::MessageId::LauncherBrowseDownloads, {download_text});
        }
        if (!item.likes.empty() && item.likes != "0") {
            info += Tr(anomaly::MessageId::LauncherBrowseLikes, {item.likes});
        }
        if (!item.views.empty() && item.views != "0") {
            info += Tr(anomaly::MessageId::LauncherBrowseViews, {item.views});
        }
        const std::string date = LauncherFormatDate(item.added);
        if (!date.empty()) info += "  ·  " + date;
        std::string size_text;
        {
            std::scoped_lock lock(g_gb_size_mutex);
            const auto found = g_gb_sizes.find(item.id);
            if (found != g_gb_sizes.end()) size_text = LauncherFormatBytes(found->second);
        }
        if (!size_text.empty()) info += "  ·  " + size_text;
        // The two commands sit under the name, one column each: a mod with no page leaves the page
        // column empty rather than letting the install button slide left, so the column reads the
        // same way down the list.
        const char* const page_label = Text(translator, anomaly::MessageId::LauncherModsPage);
        if (!item.page_url.empty()) {
            if (LauncherChip((std::string(page_label) + "##page" + item.id).c_str())) {
                const std::wstring url(item.page_url.begin(), item.page_url.end());
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else {
            ImGui::Dummy(ImVec2(ImGui::CalcTextSize(page_label).x + Scale(30.0f),
                ImGui::GetFrameHeight()));
        }
        ImGui::SameLine();
        const bool installed = installed_ids.count(item.id) != 0;
        if (install_running && install_target == item.id) {
            const double done = static_cast<double>(g_mod_download.bytes.load());
            const double total = static_cast<double>(g_mod_download.total.load());
            const double seconds = ImGui::GetTime() - install_started;
            if (total > 0.0) {
                const std::string overlay = Tr(anomaly::MessageId::LauncherBrowseProgressPercent,
                    {Num(done / (1024.0 * 1024.0), 1), Num(total / (1024.0 * 1024.0), 1),
                        Num(done * 100.0 / total, 0), Num(seconds, 0)});
                ImGui::ProgressBar(static_cast<float>(done / total), ImVec2(Scale(240.0f), 0.0f),
                    overlay.c_str());
            } else {
                ImGui::TextDisabled("%s",
                    Tr(anomaly::MessageId::LauncherBrowseInstalling,
                        {Tr(anomaly::MessageId::LauncherBrowseProgressBytes,
                            {Num(done / (1024.0 * 1024.0), 1), Num(seconds, 0)})}).c_str());
            }
            ImGui::SameLine();
            if (LauncherChip((Tr(anomaly::MessageId::LauncherModsCancel) + "##cancel" +
                    item.id).c_str())) {
                g_mod_download.cancel.store(true);
            }
        } else if (LauncherChip((std::string(installed ? Text(translator, anomaly::MessageId::LauncherModsReinstall)
                : Text(translator, anomaly::MessageId::LauncherModsInstall)) +
                "##ins" + item.id).c_str()) &&
            !install_running) {
            install_running = true;
            install_target = item.id;
            install_started = ImGui::GetTime();
            g_mod_download.cancel.store(false);
            g_mod_download.bytes.store(0);
            g_mod_download.total.store(0);
            const LauncherSnapshot snapshot_copy = snapshot;
            const LauncherGbItem target_item = item;
            std::thread([snapshot_copy, target_item] {
                const std::string result = LauncherInstallFromGameBanana(
                    snapshot_copy, target_item.id, target_item.name);
                std::scoped_lock lock(install_mutex);
                install_result = result;
                install_ready = true;
                install_running = false;
            }).detach();
        }
        if (installed && !(install_running && install_target == item.id)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsInstalled));
        }
        // The row's facts end the line, to the right of the commands, on the same baseline a label
        // beside a framed widget uses.
        if (!info.empty()) {
            ImGui::SameLine();
            const float remaining = ImGui::GetContentRegionAvail().x;
            if (remaining > Scale(24.0f)) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", Ellipsize(info, remaining).c_str());
            }
        }
        ImGui::EndGroup();
    }
    if (!items.empty() && shown == 0) {
        ImGui::TextDisabled("%s",
            Tr(anomaly::MessageId::LauncherBrowseNoMatches).c_str());
    }
    // Reading to the bottom of the list loads the next page. A list shorter than its own viewport has
    // no scroll range at all, so it also loads while there is nothing to scroll: otherwise the first
    // page of a short feed sat there with no scrollbar and therefore no way to ask for the rest.
    if (!browse_loading && !exhausted &&
        (ImGui::GetScrollMaxY() <= 0.0f ||
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - Scale(4.0f))) {
        load_page(replace_pending ? 1 : loaded_pages + 1);
    }
    ImGui::EndChild();
}

// A pager that floats over the covers instead of taking a row from them: a frosted glass
// capsule centred at the bottom of the list area, the way a viewer floats its page controls.
//
// It is a window of its own rather than a widget of the page, for two reasons that both come
// from how the material is drawn: a widget of the page would be recorded before the list (so
// the covers would paint over it), and it would sit under the list child for hit testing (so
// its buttons would never see the mouse). A window created after the list is drawn after it
// and takes the mouse first, which is exactly what a floating control needs.
//
// Returns true when the page changed, so the caller can scroll the list back to its top.
bool DrawModsPager(std::size_t& page, const std::size_t pages, const ImVec2 area_min,
    const ImVec2 area_size, const anomaly::Translator& translator) {
    if (pages <= 1) return false;
    const char* const previous = Text(translator, anomaly::MessageId::LauncherModsPreviousPage);
    const char* const next = Text(translator, anomaly::MessageId::LauncherModsNextPage);
    const std::string position = Tr(anomaly::MessageId::LauncherModsPagePosition,
        {std::to_string(page + 1), std::to_string(pages)});
    const float height = Scale(52.0f);
    const float inset = Scale(6.0f);
    const float padding = Scale(16.0f);
    const float gap = Scale(10.0f);
    const float previous_width = ImGui::CalcTextSize(previous).x + padding * 2.0f;
    const float next_width = ImGui::CalcTextSize(next).x + padding * 2.0f;
    const float position_width = ImGui::CalcTextSize(position.c_str()).x + gap * 2.0f;
    const float width =
        previous_width + next_width + position_width + gap * 2.0f + inset * 2.0f;
    const ImVec2 origin(area_min.x + (area_size.x - width) * 0.5f,
        area_min.y + area_size.y - height - Scale(12.0f));
    // A child of the page rather than a window of its own: a nested window cannot be opened
    // from inside a child, but a child created after the list is drawn after it (so the capsule
    // covers the covers) and is hit-tested before it (so the segments take the click).
    ImGui::SetCursorScreenPos(origin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("mods-pager", ImVec2(width, height), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    bool changed = false;
    {
        // A pane that has to be read on top of covers: the regular material is tuned for a
        // panel over a wallpaper, so it is deepened here rather than left half transparent --
        // heavy face blur, the body filled to the tint, and the shadow spilling past the
        // capsule the way a floating control's does.
        anomaly::glass::SurfaceOptions capsule;
        capsule.variant = anomaly::glass::Variant::Regular;
        capsule.rounding = height * 0.5f;
        capsule.scale = g_launcher_dpi_scale;
        capsule.face_blur = 2.0f;
        capsule.minimum_alpha = 0.62f;
        capsule.tint_strength = LauncherSurfaceIsLight() ? 0.22f : 0.20f;
        capsule.clip = false;
        const ImVec2 panel_min = ImGui::GetWindowPos();
        anomaly::glass::DrawSurface(ImGui::GetWindowDrawList(), panel_min,
            ImVec2(panel_min.x + width, panel_min.y + height), capsule);

        float x = inset;
        const auto segment = [&](const char* label, const float segment_width, const bool enabled) {
            ImGui::SetCursorPos(ImVec2(x, inset));
            ImGui::BeginDisabled(!enabled);
            const bool pressed = ImGui::InvisibleButton(
                label, ImVec2(segment_width, height - inset * 2.0f));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            const ImVec2 label_size = ImGui::CalcTextSize(label);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(panel_min.x + x + (segment_width - label_size.x) * 0.5f,
                    panel_min.y + (height - label_size.y) * 0.5f),
                ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
            x += segment_width + gap;
            return pressed && enabled;
        };
        if (segment(previous, previous_width, page > 0)) {
            --page;
            changed = true;
        }
        // The position is text, not a target: it is what the two segments act on.
        const ImVec2 position_size = ImGui::CalcTextSize(position.c_str());
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(panel_min.x + x + (position_width - position_size.x) * 0.5f,
                panel_min.y + (height - position_size.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Text), position.c_str());
        x += position_width + gap;
        if (segment(next, next_width, page + 1 < pages)) {
            ++page;
            changed = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return changed;
}

void DrawModsMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    (void)window;
    static std::vector<LauncherModEntry> mods;
    static bool refreshed{};
    static char search[128]{};
    static int status_filter{};  // 0 = all, 1 = enabled, 2 = disabled
    static std::size_t page{};
    static std::string import_message;
    static char notes_file_buffer[128]{};
    static char notes_name_buffer[128]{};
    static char notes_buffer[512]{};
    static char notes_group_buffer[64]{};
    static char notes_id_buffer[32]{};
    static std::string notes_source;
    static bool notes_directory{};
    static std::string pending_uninstall;   // single uninstall awaiting confirmation
    static std::set<std::string> selected_names;  // batch selection by mod key: one per mod, also across categories
    static bool bulk_uninstall_pending{};
    static std::map<std::string, bool> group_collapsed;
    static std::string update_status;
    static std::mutex check_mutex;
    static std::vector<std::pair<std::string, std::string>> check_versions;
    static std::string check_summary;
    static std::atomic<bool> check_running{false};
    static bool check_ready{};
    static double last_refresh{};
    static std::filesystem::path cached_game, directory;
    static auto cached_client = snapshot.selected_client;
    LauncherModSettingsCache& mod_settings = RefreshLauncherModSettings(snapshot);
    // The view, the pager and its page size are the launcher configuration's, so the settings
    // page and this page always read the same values; nothing is cached in a static here.
    const anomaly::launcher::LauncherViewSettings& view = snapshot.view;
    // How deep the grid is, and what is open at each level: 0 is the categories, 1 a category (its
    // skins, or its mods when it has no skins to separate) and 2 one of those skins. The drill-in
    // belongs to the session rather than to the configuration, because a launcher that reopened
    // inside one category would look empty.
    static int grid_level{};
    static std::string grid_group;
    static std::string grid_skin;
    // Set by anything that changes what the page shows; the list is scrolled back to its top on
    // the next frame, because the list is the only part of this page that scrolls.
    static bool scroll_to_top{};
    // A modal's ImGui name is both its title and its id: the visible half is translated, the half
    // after "##" keeps OpenPopup and BeginPopupModal paired. Without it the window shows the raw id
    // ("mod-notes"), which is neither translated nor a title.
    const auto modal_name = [&](const anomaly::MessageId title, const char* id) {
        return std::string(Text(translator, title)) + "##" + id;
    };
    // One image per frame keeps a list smooth; a grid shows a screen of cards at once, so it
    // decodes a few of them per frame and the covers appear within the first few frames.
    g_decode_budget = view.mods_grid ? 3 : 1;
    const auto refresh_mods = [&] {
        if (cached_game != snapshot.game_directory || cached_client != snapshot.selected_client) {
            page = 0;
            grid_level = 0;
            grid_group.clear();
            grid_skin.clear();
            selected_names.clear();
            group_collapsed.clear();
            import_message.clear();
            update_status.clear();
            scroll_to_top = true;
        }
        cached_game = snapshot.game_directory;
        cached_client = snapshot.selected_client;
        directory = LauncherModsDirectory(snapshot);
        if (directory.empty()) {
            mods.clear();
            refreshed = true;
            last_refresh = ImGui::GetTime();
            return;
        }
        nlohmann::json settings;
        try {
            settings = LauncherPakSettings(snapshot);
            mod_settings = RefreshLauncherModSettings(snapshot, true, &settings);
        } catch (...) {
            mod_settings.compatibility = true;
            mod_settings.bypass = false;
            mod_settings.force_load = false;
        }
        LauncherRefreshMods(snapshot, mods, &settings);
        for (auto& entry : mods) entry.selected = selected_names.count(LauncherModStateKey(entry)) != 0;
        refreshed = true;
        last_refresh = ImGui::GetTime();
    };
    const bool refresh_requested = std::exchange(g_mods_refresh_requested, false);
    if (refresh_requested || !refreshed ||
        cached_game != snapshot.game_directory || cached_client != snapshot.selected_client ||
        (ImGui::GetTime() - last_refresh > 2.0 && !check_running)) refresh_mods();

    if (directory.empty()) {
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherStatusSelectGameDirectory));
        return;
    }

    // Everything above the list is fixed: the page itself never scrolls, so the path, the
    // switches and the search box stay where they are while the covers move under them.
    ImGui::TextDisabled("%s", LauncherNarrow(directory).c_str());
    if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsRefresh))) {
        refresh_mods();
    }
    ImGui::SameLine();
    if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsOpenFolder))) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsImport))) {
        if (const auto archive = LauncherChooseArchive(window)) {
            import_message = LauncherImportArchive(snapshot, window, *archive);
            refresh_mods();
        }
    }
    ImGui::SameLine();
    if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsOrganize))) {
        import_message = LauncherOrganizeMessage(LauncherOrganizeModFolders(snapshot, mods));
        refresh_mods();
    }
    // The exclusivity of the mods page -- "load both anyway" -- is a switch of the page, not a
    // command on this list, so it is drawn on the first row next to compatibility mode; the row of
    // commands below stays with the list it acts on.
    ImGui::SameLine();
    if (check_running) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsChecking));
    } else if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsCheckUpdates))) {
        check_running = true;
        std::thread([copy = mods]() mutable {
            std::vector<std::pair<std::string, std::string>> discovered;
            const std::string summary = LauncherCheckUpdates(copy, discovered);
            std::scoped_lock lock(check_mutex);
            check_versions = std::move(discovered);
            check_summary = summary;
            check_ready = true;
            check_running = false;
        }).detach();
    }
    {
        std::scoped_lock lock(check_mutex);
        if (check_ready) {
            check_ready = false;
            for (const auto& [name, remote] : check_versions) {
                g_mod_remote_versions[name] = remote;
                for (auto& entry : mods) {
                    if (LauncherModStateKey(entry) == name) entry.remote_version = remote;
                }
            }
            check_versions.clear();
            update_status = check_summary;
        }
    }
    if (!update_status.empty()) {
        ImGui::TextDisabled("%s", update_status.c_str());
    }
    const bool game_running = !snapshot.processes.empty();
    const bool toggles_locked = game_running && mod_settings.compatibility;
    if (!import_message.empty()) {
        ImGui::TextDisabled("%s", import_message.c_str());
    }

    ImGui::SetNextItemWidth(Scale(260.0f));
    if (ImGui::InputTextWithHint("##mod-search",
            Text(translator, anomaly::MessageId::LauncherModsSearchHint), search, sizeof(search))) {
        page = 0;
        scroll_to_top = true;
    }
    ImGui::SameLine();
    const std::array<const char*, 3> filter_labels{
        Text(translator, anomaly::MessageId::LauncherModsFilterAll),
        Text(translator, anomaly::MessageId::LauncherModsFilterEnabled),
        Text(translator, anomaly::MessageId::LauncherModsFilterDisabled)};
    anomaly::glass::SegmentedOptions filter_options;
    filter_options.height = 38.0f;
    filter_options.padding_x = 30.0f;
    filter_options.scale = g_launcher_dpi_scale;
    if (anomaly::glass::SegmentedControl(
            "mod-status-filter", filter_labels, status_filter, filter_options)) {
        page = 0;
        scroll_to_top = true;
    }
    // The view switch sits with the filters because that is where the other choice about how
    // the mods are shown lives; both of them only re-shape this page. The grid comes first
    // because it is the view that opens: covers name a mod faster than a row of text does.
    ImGui::SameLine();
    const std::array<const char*, 2> view_labels{
        Text(translator, anomaly::MessageId::LauncherModsViewGrid),
        Text(translator, anomaly::MessageId::LauncherModsViewList)};
    anomaly::glass::SegmentedOptions view_options;
    view_options.height = 38.0f;
    view_options.padding_x = 24.0f;
    view_options.scale = g_launcher_dpi_scale;
    int view_index = view.mods_grid ? 0 : 1;
    if (anomaly::glass::SegmentedControl(
            "mod-view", view_labels, view_index, view_options)) {
        controller.SetModsView(
            view_index == 0, view.mods_pagination, view.mods_page_size);
        page = 0;
        scroll_to_top = true;
    }


    if (mods.empty()) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsEmpty));
        return;
    }

    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto matches = [&](const LauncherModEntry& entry) {
        if (status_filter == 1 && !entry.enabled) return false;
        if (status_filter == 2 && entry.enabled) return false;
        if (needle.empty()) return true;
        std::string haystack = entry.display_name + " " + entry.name + " " + entry.author;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
            [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return haystack.find(needle) != std::string::npos;
    };
    std::vector<const LauncherModEntry*> filtered;
    for (const auto& entry : mods) {
        if (matches(entry)) filtered.push_back(&entry);
    }
    // The grid opens on the categories rather than on the mods: a cover says less than the
    // character or the pack it belongs to, and one page holding covers from every category
    // at once is a pile. The categories are the ones the search and the filter left
    // visible, in the order the mods appear, so the overview matches the list beside it.
    // A character category carries a second level: the skins its mods replace. Every mod is filed
    // under exactly one of them -- the first skin it covers -- and carries its full skin list as
    // tags, so no mod is listed twice and the counts of the skins add up to the count of the
    // category. The skins stay separate groups, so two outfits of one character can be on at the
    // same time. `anomaly/launcher/mod_groups.hpp` owns that arithmetic.
    std::vector<anomaly::launcher::ModGroupingEntry> classified;
    classified.reserve(filtered.size());
    for (const auto* entry : filtered) {
        anomaly::launcher::ModGroupingEntry value;
        value.name = entry->display_name.empty() ? entry->name : entry->display_name;
        for (const auto& group : entry->groups) value.groups.push_back({group.name, group.skins});
        classified.push_back(std::move(value));
    }
    const auto layout = anomaly::launcher::BuildModGrouping(classified);
    const auto& groups = layout.categories;
    const auto& skins = layout.skins;
    // The labels live here, not in the layout: the two categories whose key is an empty string are
    // the ones this page has words for, and every other key is a container identifier that
    // `assets/mod-groups/mod-groups.json` names. A token with no entry still reads as a category
    // ("角色 051"), so a character added by a mod author never shows up as a bare id.
    const auto category_label = [language = LauncherModGroupLanguage()](const std::string& key) {
        return key.empty() ? Tr(anomaly::MessageId::LauncherModsUngrouped)
                           : g_mod_group_names.Group(key, language);
    };
    const auto skin_label = [language = LauncherModGroupLanguage()](const std::string& key) {
        return key.empty() ? Tr(anomaly::MessageId::LauncherModsGroupShared)
                           : g_mod_group_names.Skin(key, language);
    };
    // A category or skin the search no longer matches cannot stay open, and neither change can
    // keep the page the other view was on.
    const auto open_group = anomaly::launcher::FindModCategory(layout, grid_group);
    if (grid_level > 0 && open_group == nullptr) {
        grid_level = 0;
        page = 0;
        scroll_to_top = true;
    }
    const auto open_skin = [&]() -> const anomaly::launcher::ModGroupingSkin* {
        if (grid_level < 2 || open_group == nullptr) return nullptr;
        if (const auto* found = anomaly::launcher::FindModSkin(layout, *open_group, grid_skin))
            return found;
        grid_level = 1;
        page = 0;
        scroll_to_top = true;
        return nullptr;
    };
    const auto* const skin_view = open_skin();
    // What the pager counts: the categories in the overview, the skins of an open category (or its
    // mods when the category names no skins), the mods of one skin, and every row of the list.
    // Pagination is off unless the settings page turned it on; with it off the whole set is laid
    // out and the list is scrolled instead, which is what a launcher with a couple of dozen mods
    // wants.
    const std::size_t open_items = open_group == nullptr ? 0
        : open_group->named_skins ? open_group->skins_count
        : open_group->count;
    const std::size_t page_items = !view.mods_grid ? layout.placements.size()
        : grid_level == 0 ? groups.size()
        : grid_level == 1 ? open_items
        : skin_view != nullptr ? skin_view->count : 0;
    const std::size_t page_size = view.mods_pagination
        ? static_cast<std::size_t>(std::clamp(view.mods_page_size, 1, 200))
        : (std::max)(page_items, std::size_t{1});
    const std::size_t pages = (std::max)(std::size_t{1},
        (page_items + page_size - 1) / page_size);
    page = (std::min)(page, pages - 1);
    {
        // Batch operations act on the selection across the whole set, not just the rows on screen.
        std::size_t selected_count{};
        bool any_unselected{};
        for (const auto& entry : mods) {
            if (entry.selected) ++selected_count;
            else any_unselected = true;
        }
        // Same lock the per-row switch uses; that variable is declared further down.
        ImGui::BeginDisabled(!snapshot.processes.empty() && mod_settings.compatibility);
        // The button says what the next click does, one label at a time, and keeps the width of the
        // longer of the two so the buttons beside it never shift when the selection flips.
        const char* const select_label = Text(translator, any_unselected
            ? anomaly::MessageId::LauncherModsSelectAll
            : anomaly::MessageId::LauncherModsDeselectAll);
        const float select_width =
            (std::max)(ImGui::CalcTextSize(Text(translator, anomaly::MessageId::LauncherModsSelectAll)).x,
                ImGui::CalcTextSize(Text(translator, anomaly::MessageId::LauncherModsDeselectAll)).x) +
            Scale(30.0f);
        if (LauncherChip((std::string(select_label) + "##select-all").c_str(),
                ImVec2(select_width, 0.0f))) {
            for (auto& entry : mods) entry.selected = any_unselected;
            selected_names.clear();
            if (any_unselected) {
                for (const auto& entry : mods) selected_names.insert(LauncherModStateKey(entry));
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selected_count == 0);
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsBulkEnable))) {
            for (const auto& entry : mods) {
                if (entry.selected && !entry.enabled) LauncherSetModEnabled(snapshot, entry, true);
            }
            refreshed = false;
        }
        ImGui::SameLine();
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsBulkDisable))) {
            for (const auto& entry : mods) {
                if (entry.selected && entry.enabled) LauncherSetModEnabled(snapshot, entry, false);
            }
            refreshed = false;
        }
        ImGui::SameLine();
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsBulkUninstall))) {
            bulk_uninstall_pending = true;
            ImGui::OpenPopup(modal_name(anomaly::MessageId::LauncherModsBulkUninstall, "bulk-uninstall").c_str());
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    const auto draw_switch = [&](const LauncherModEntry& target) {
        bool enabled = target.enabled;
        ImGui::BeginDisabled(toggles_locked);
        const bool changed = anomaly::glass::Switch(("##mod-enabled-" + LauncherModStateKey(target)).c_str(), &enabled);
        ImGui::EndDisabled();
        if (!changed) return;
        if (!LauncherSetModEnabled(snapshot, target, enabled))
            import_message = Text(translator, anomaly::MessageId::SettingsSaveFailed);
        // Refresh next frame so the current rows keep valid references and their full scroll height.
        refreshed = false;
    };

    // The settings dialog is filled the same way from both views: the list opens it from a button,
    // the grid opens it from the card, and the fields have to carry the mod's current values
    // whichever one was used.
    const auto open_notes = [&](const LauncherModEntry& entry) {
        notes_source = LauncherModStateKey(entry);
        notes_directory = entry.directory;
        std::snprintf(notes_name_buffer, sizeof(notes_name_buffer), "%s",
            entry.custom_name.empty() ? entry.display_name.c_str() : entry.custom_name.c_str());
        // The name on disk: renaming lives in this dialog now, so the field starts on the current one.
        std::snprintf(notes_file_buffer, sizeof(notes_file_buffer), "%s", entry.name.c_str());
        std::snprintf(notes_buffer, sizeof(notes_buffer), "%s", entry.notes.c_str());
        // The manual group, not the classified one: saving a note must not freeze today's
        // automatic classification into a hand-written category.
        std::snprintf(notes_group_buffer, sizeof(notes_group_buffer), "%s",
            entry.custom_group.c_str());
        notes_id_buffer[0] = '\0';
        // Show the already bound GameBanana id so the field is not misleadingly empty.
        if (entry.directory) {
            const auto source = LauncherReadModSource(entry.icon_path);
            std::snprintf(notes_id_buffer, sizeof(notes_id_buffer), "%s", source.mod_id.c_str());
        }
        ImGui::OpenPopup(modal_name(anomaly::MessageId::LauncherModsNotesTitle, "mod-settings").c_str());
    };
    // Both views request work here instead of doing it inside the loop: a popup opened
    // under a card's ID is not the popup drawn after the loop, and updating a mod while
    // iterating would free the entry the loop is still holding.
    const LauncherModEntry* notes_request = nullptr;
    const LauncherModEntry* uninstall_request = nullptr;
    const LauncherModEntry* update_request = nullptr;
    // The category a clicked category card asked for, and the skin a clicked skin card asked for;
    // both are applied after the grid has been drawn so the loops keep iterating the set they
    // started with.
    std::string group_request;
    bool group_request_open{};
    std::string skin_request;
    bool skin_request_open{};
    // The level the path above the cards asked for, applied after the grid so the loops keep the
    // ranges they started with.
    int level_request{-1};

    // The skins a mod replaces. It is filed under the first of them, so every row and card has to
    // say what else it covers: that keeps one mod to one entry while the rest stays visible.
    const auto skin_tags = [](const LauncherModEntry& entry, const std::string& category) {
        std::string tags;
        for (const auto& group : entry.groups) {
            if (group.name != category) continue;
            for (const auto& skin : group.skins) {
                if (skin.empty()) continue;
                if (!tags.empty()) tags += "  ·  ";
                tags += skin;
            }
        }
        return tags;
    };

    // Measure inside the scrolling child so its scrollbar is excluded from the card widths.
    ImGui::BeginChild("ModsList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    if (scroll_to_top) {
        ImGui::SetScrollY(0.0f);
        scroll_to_top = false;
    }
    const float grid_spacing = Scale(12.0f);
    const float grid_width = ImGui::GetContentRegionAvail().x;
    const int grid_columns = std::clamp(static_cast<int>(
        (grid_width + grid_spacing) / (Scale(176.0f) + grid_spacing)), 2, 6);
    const float card_width = std::floor(
        (grid_width - grid_spacing * static_cast<float>(grid_columns - 1)) /
        static_cast<float>(grid_columns));

    // A grid card is a cover, the name, the metadata, and the same controls a list row
    // carries: the enable switch, the batch box and the commands, in two rows of two because
    // a card is narrower than a row. The commands are shortened to the card's width and the
    // tooltip keeps the full label. The card also carries a surface of its own, the same one the
    // categories use: its content is inset so the frame has a margin to show on every side.
    // Keep one cover aspect across category and mod cards so the grid stays visually consistent.
    const float card_padding = Scale(6.0f);
    const float card_inner = card_width - card_padding * 2.0f;
    const float category_caption_height = ImGui::GetTextLineHeight() * 2.0f;
    const ImVec2 card_cover(card_inner, card_inner * kLauncherModCoverAspect);
    const auto draw_grid_card = [&](const LauncherModEntry& entry, const std::string& category) {
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        // Two channels: the frame is drawn after the content, into the channel that renders
        // behind it, because a surface painted first would still be under the cover but the
        // card's height is only known once its content is.
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        ImGui::PushID(LauncherModStateKey(entry).c_str());
        // The top margin is a cursor shift rather than an empty item: an item advances the line by
        // its own height *plus* the style's line spacing, which is what made a mod card's cover sit
        // 10px lower than a category card's. The surface below is then widened by the same margin,
        // so the gap above the cover is the same number in both levels.
        const ImVec2 card_origin = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(card_origin.x, card_origin.y + card_padding));
        ImGui::BeginGroup();
        if (LauncherModCoverCard(entry, card_cover)) {
            notes_request = &entry;
        }
        const std::string title = entry.display_name.empty() ? entry.name : entry.display_name;
        ImGui::TextUnformatted(Ellipsize(title, card_inner).c_str());
        std::string meta = skin_tags(entry, category);
        if (!entry.version.empty()) {
            if (!meta.empty()) meta += "  ·  ";
            meta += "v" + entry.version;
        }
        if (!entry.author.empty()) {
            if (!meta.empty()) meta += "  ·  ";
            meta += entry.author;
        }
        if (!entry.remote_version.empty() && entry.remote_version != entry.version) {
            if (!meta.empty()) meta += "  ·  ";
            meta += Tr(anomaly::MessageId::LauncherModsNewVersionInline, {entry.remote_version});
        }
        if (!meta.empty()) ImGui::TextDisabled("%s", Ellipsize(meta, card_inner).c_str());
        // The first row is the mod's state and its two shortcuts: the enable switch, the batch box,
        // and -- when the mod has them -- its source and its update. The card is narrow, so each
        // shortcut is measured against the room that is left instead of pushing the row out; the
        // list below the same page has all the width it needs when one of them does not fit.
        draw_switch(entry);
        ImGui::SameLine();
        bool row_selected = entry.selected;
        if (ImGui::Checkbox("##select", &row_selected)) {
            const_cast<LauncherModEntry&>(entry).selected = row_selected;
            if (row_selected) selected_names.insert(LauncherModStateKey(entry));
            else selected_names.erase(LauncherModStateKey(entry));
        }
        const float row_limit = card_origin.x + card_inner;
        const auto row_fits = [&](const float width) {
            return ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= row_limit;
        };
        if (!entry.link.empty()) {
            const std::string label = Text(translator, anomaly::MessageId::LauncherModsOpenSource);
            if (row_fits(ImGui::CalcTextSize(label.c_str()).x + Scale(30.0f))) {
                ImGui::SameLine();
                if (LauncherChip((label + "##source").c_str())) {
                    const std::wstring url(entry.link.begin(), entry.link.end());
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }
        if (!entry.remote_version.empty() &&
            (entry.version.empty() || entry.remote_version != entry.version)) {
            const std::string label = Text(translator, anomaly::MessageId::LauncherModsUpdate);
            if (row_fits(ImGui::CalcTextSize(label.c_str()).x + Scale(30.0f))) {
                ImGui::SameLine();
                ImGui::BeginDisabled(game_running && entry.enabled);
                if (LauncherChip((label + "##update").c_str())) update_request = &entry;
                ImGui::EndDisabled();
            }
        }
        // Everything that edits the mod lives behind one command, so the card has exactly two
        // buttons under it: the settings dialog and the command that deletes the mod.
        const float action_width =
            (card_inner - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const bool editable = !(game_running && entry.enabled);
        if (LauncherChip(
                (std::string(Text(translator, anomaly::MessageId::LauncherModsSettings)) +
                    "##settings").c_str(),
                ImVec2(action_width, 0.0f))) {
            notes_request = &entry;
        }
        ImGui::SameLine();
        if (DestructiveButton(
                (std::string(Text(translator, anomaly::MessageId::LauncherModsUninstall)) +
                    "##uninstall").c_str(),
                ImVec2(action_width, 0.0f), editable)) {
            uninstall_request = &entry;
        }
        // The bottom margin is a cursor shift as well, so the gap under the last button is the same
        // number as the gap above the cover; a zero-height item at that position is what carries it
        // into the card's bounds.
        ImGui::SetCursorScreenPos(
            ImVec2(card_origin.x, ImGui::GetItemRectMax().y + card_padding));
        ImGui::Dummy(ImVec2(card_inner, 0.0f));
        ImGui::EndGroup();
        const ImVec2 bounds_min = ImGui::GetItemRectMin();
        const ImVec2 bounds_max = ImGui::GetItemRectMax();
        draw->ChannelsSetCurrent(0);
        // A card is the content layer, like the shell's cards: frost and a contact edge, no glow,
        // and no second refraction of the wallpaper behind the page.
        const auto surface = LauncherCardSurface(Scale(14.0f));
        // The frame spans the whole grid cell: the content is inset by the card's margin on every
        // side, so it is widened by that margin here and the cards of a row keep the gap between
        // them.
        const ImVec2 cell_min(bounds_min.x - card_padding, card_origin.y);
        const ImVec2 cell_max(bounds_max.x + card_padding, bounds_max.y);
        anomaly::glass::DrawSurface(draw, cell_min, cell_max, surface);
        draw->ChannelsMerge();
        ImGui::PopID();
    };

    std::string current_group{"\x01"};  // impossible marker: forces a header for the first entry
    bool hide_group{};
    const std::size_t page_end = (std::min)(layout.placements.size(), (page + 1) * page_size);
    // One skin inside an open category: a dimmed label and the number of mods filed under it, so
    // two outfits of one character read as two groups that can be on at the same time.
    const auto draw_skin_header = [&](const anomaly::launcher::ModGroupingSkin& section) {
        ImGui::TextUnformatted(skin_label(section.key).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(anomaly::MessageId::LauncherModsGroupCount,
            {std::to_string(section.count)}).c_str());
    };
    if (view.mods_grid) {
        // A category card is one invisible button covering the whole card, so the cover, the name
        // and the count are a single target instead of three. Its cover slot shows the category's
        // -- or the skin's -- first mod whole: a patch of shrunken thumbnails was unreadable at
        // this size, and cropping to fill the slot cut the artwork.
        const auto draw_category_card = [&](const char* id, const std::string& title,
                                            const LauncherModEntry* cover, const std::size_t count) {
            // The cover is inset from the card edge, which keeps the card's rounded frame
            // visible around it, and every card has the same slot, so a page of them reads as a
            // grid rather than as a shelf of different heights. The caption below the cover gets the
            // same margin the cover has above it: while it was 2 px the two lines crowded the picture
            // and only the top margin looked right.
            const float pad = card_padding;
            const ImVec2 cover_size = card_cover;
            const float caption_gap = card_padding;
            const float caption_height = category_caption_height + caption_gap;
            const std::string caption = Ellipsize(title, card_width - Scale(20.0f));
            ImGui::PushID(id);
            ImGui::InvisibleButton("##category",
                ImVec2(card_width, cover_size.y + caption_gap + caption_height + pad));
            const bool clicked = ImGui::IsItemClicked();
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (!ImGui::IsItemVisible()) {
                ImGui::PopID();
                return clicked;
            }
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max(min.x + card_width,
                min.y + cover_size.y + caption_gap + caption_height + pad);
            anomaly::glass::SurfaceOptions card = LauncherCardSurface(Scale(14.0f));
            anomaly::glass::DrawSurface(ImGui::GetWindowDrawList(), min, max, card);
            if (cover != nullptr) {
                DrawLauncherModCoverSlot(
                    *cover, ImVec2(min.x + pad, min.y + pad), cover_size);
            }
            ImDrawList* const list = ImGui::GetWindowDrawList();
            const float text_top = min.y + pad + cover_size.y + caption_gap;
            list->AddText(ImVec2(min.x + Scale(10.0f), text_top),
                ImGui::GetColorU32(ImGui::GetStyleColorVec4(ImGuiCol_Text)), caption.c_str());
            list->AddText(ImVec2(min.x + Scale(10.0f), text_top + ImGui::GetTextLineHeight()),
                ImGui::GetColorU32(LauncherMutedInk()),
                Tr(anomaly::MessageId::LauncherModsGroupCount,
                    {std::to_string(count)}).c_str());
            ImGui::PopID();
            return clicked;
        };
        if (grid_level == 0) {
            int column{};
            const std::size_t end = (std::min)(groups.size(), (page + 1) * page_size);
            for (std::size_t index = page * page_size; index < end; ++index) {
                const auto& group = groups[index];
                if (column != 0) ImGui::SameLine(0.0f, grid_spacing);
                column = (column + 1) % grid_columns;
                if (draw_category_card(group.key.c_str(), category_label(group.key),
                        group.count == 0 ? nullptr : filtered[layout.placements[group.first]],
                        group.count)) {
                    group_request = group.key;
                    group_request_open = true;
                }
            }
        } else {
            const auto& group = *open_group;
            // The path above the cards is also the way back: its levels are clickable, so one click
            // reaches the overview instead of one click per level. The level on screen stays plain
            // text, because it is where the page already is.
            bool jump_overview{};
            bool jump_category{};
            // The way back is a chip like every other command, so the page has one material.
            jump_overview = LauncherChip(
                (std::string(Text(translator, anomaly::MessageId::LauncherModsGroupBack)) +
                    "##back").c_str());
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            const std::string category_text = category_label(group.key);
            const std::string category_crumb = Ellipsize(category_text, grid_width * 0.4f);
            // A crumb is ordinary text that happens to answer clicks: a selectable has its own
            // text placement and its own full-width hit box, so it would sit off the line and
            // highlight the whole row.
            ImGui::TextUnformatted(category_crumb.c_str());
            const bool category_link = grid_level > 1;
            if (category_link) {
                jump_category = ImGui::IsItemClicked();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    const ImVec2 link_min = ImGui::GetItemRectMin();
                    const ImVec2 link_max = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddLine(
                        ImVec2(link_min.x, link_max.y - 1.0f),
                        ImVec2(link_max.x, link_max.y - 1.0f),
                        ImGui::GetColorU32(ImGuiCol_Text, 0.45f), 1.0f);
                }
            }
            if (grid_level > 1 && skin_view != nullptr) {
                ImGui::SameLine();
                ImGui::TextDisabled("/");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                const std::string skin_text = skin_label(skin_view->key);
                ImGui::TextUnformatted(Ellipsize(skin_text, grid_width * 0.4f).c_str());
            }
            ImGui::SameLine();
            // The count of mods the level stands for: the category's own mods while its skins are
            // on screen (they add up to it), and one skin's mods once that skin is open.
            const std::size_t header_count = grid_level > 1 && skin_view != nullptr
                ? skin_view->count : group.count;
            ImGui::TextDisabled("%s", Tr(anomaly::MessageId::LauncherModsGroupCount,
                {std::to_string(header_count)}).c_str());
            if (jump_overview) level_request = 0;
            else if (jump_category) level_request = 1;
            int column{};
            if (grid_level == 1 && group.named_skins) {
                // The skins of the category as cards of their own: the level that keeps one page
                // from mixing every outfit of a character into one shelf.
                const std::size_t end = (std::min)(group.skins_first + group.skins_count,
                    group.skins_first + (page + 1) * page_size);
                for (std::size_t index = group.skins_first + page * page_size; index < end; ++index) {
                    const auto& section = skins[index];
                    if (column != 0) ImGui::SameLine(0.0f, grid_spacing);
                    column = (column + 1) % grid_columns;
                    if (draw_category_card(section.key.c_str(), skin_label(section.key),
                            section.count == 0 ? nullptr : filtered[layout.placements[section.first]],
                            section.count)) {
                        skin_request = section.key;
                        skin_request_open = true;
                    }
                }
            } else {
                // The mods of the open skin, or of the whole category when it names no skins.
                const auto* const section = grid_level > 1
                    ? skin_view : &skins[group.skins_first];
                const std::size_t end = (std::min)(section->first + section->count,
                    section->first + (page + 1) * page_size);
                for (std::size_t index = section->first + page * page_size; index < end; ++index) {
                    // The card's surface is drawn wider than its content, so a row steps by that
                    // margin too and its first card starts one margin in: two neighbouring frames
                    // keep the same gap the category cards have.
                    if (column == 0) {
                        ImGui::Dummy(ImVec2(card_padding, 0.0f));
                        ImGui::SameLine(0.0f, 0.0f);
                    } else {
                        ImGui::SameLine(0.0f, grid_spacing + card_padding * 2.0f);
                    }
                    column = (column + 1) % grid_columns;
                    draw_grid_card(*filtered[layout.placements[index]], group.key);
                }
            }
        }
    } else {
        // One row per mod: the categories and their skins are already in display order, which is
        // what makes a plain scan of the ranges enough here. A mod is filed under one skin only,
        // so the same mod never shows up twice in the list.
        bool done{};
        std::string current_skin{"\x01"};
        for (const auto& group : groups) {
            const std::string category_text = category_label(group.key);
            for (std::size_t index = group.first; index < group.first + group.count; ++index) {
                if (index < page * page_size) continue;
                if (index >= page_end) {
                    done = true;
                    break;
                }
                const auto& entry = *filtered[layout.placements[index]];
                const auto* const section = anomaly::launcher::FindModSkinAt(layout, group, index);
                if (category_text != current_group) {
                    current_group = category_text;
                    current_skin = "\x01";
                    hide_group = group_collapsed[current_group];
                    ImGui::Spacing();
                    if (ImGui::Selectable(current_group.c_str())) {
                        group_collapsed[current_group] = !hide_group;
                    }
                    ImGui::Separator();
                }
                if (hide_group) continue;
                if (group.named_skins && section != nullptr && section->key != current_skin) {
                    current_skin = section->key;
                    ImGui::Indent(Scale(12.0f));
                    draw_skin_header(*section);
                    ImGui::Unindent(Scale(12.0f));
                }
                // The same mod can sit in two categories, and both sections are on screen at once,
                // so the category has to be part of every widget's identity here.
                ImGui::PushID(current_group.c_str());
                ImGui::PushID(LauncherModStateKey(entry).c_str());
                DrawLauncherModCover(entry);
                ImGui::SameLine();
                ImGui::BeginGroup();
                std::string title = entry.display_name.empty() ? entry.name : entry.display_name;
                std::string meta = skin_tags(entry, group.key);
                if (!entry.author.empty()) {
                    if (!meta.empty()) meta += "  ·  ";
                    meta += entry.author;
                }
                if (!entry.version.empty()) {
                    if (!meta.empty()) meta += "  ·  ";
                    meta += "v" + entry.version;
                }
                if (!entry.remote_version.empty() && entry.remote_version != entry.version) {
                    if (!meta.empty()) meta += "  ·  ";
                    meta += Tr(anomaly::MessageId::LauncherModsNewVersionInline,
                        {entry.remote_version});
                }
                ImGui::TextWrapped("%s", title.c_str());
                ImGui::TextDisabled("%zu  ·  %.1f MB%s", entry.files,
                    static_cast<double>(entry.bytes) / (1024.0 * 1024.0), meta.c_str());
                // Every action is a button inside a disabled scope: the click is evaluated first and the
                // action runs afterwards, so a disabled scope never spans an early return.
                const float actions_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                // A row keeps its commands on one line while they fit; past that they are clipped
                // rather than wrapped, so each one is measured before it is placed.
                const auto next_action = [&](const char* label) {
                    const auto& style = ImGui::GetStyle();
                    const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
                    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= actions_right)
                        ImGui::SameLine();
                };
                // A command that a mod does not have leaves its column empty instead of letting the
                // commands after it slide left, which is what keeps the trailing batch box in the
                // same column on every row. The width is a chip's own: its label plus its padding.
                const auto reserve_action = [&](const char* label) {
                    const ImVec2 size(
                        ImGui::CalcTextSize(label).x + Scale(30.0f), ImGui::GetFrameHeight());
                    next_action(label);
                    ImGui::Dummy(size);
                };
                draw_switch(entry);
                // The row reads: state, the update shortcut, the two commands a card also has, the
                // source, and the batch box last.
                if (!entry.remote_version.empty() &&
                    (entry.version.empty() || entry.remote_version != entry.version)) {
                    next_action(Text(translator, anomaly::MessageId::LauncherModsUpdate));
                    ImGui::BeginDisabled(game_running && entry.enabled);
                    const bool update_clicked = LauncherChip(
                        (std::string(Text(translator, anomaly::MessageId::LauncherModsUpdate)) + "##u" +
                        entry.name).c_str());
                    ImGui::EndDisabled();
                    if (update_clicked) {
                        import_message = LauncherUpdateMod(snapshot, entry);
                        refresh_mods();
                        ImGui::EndGroup();
                        ImGui::PopID();
                        ImGui::PopID();
                        ImGui::EndChild();
                        return;
                    }
                } else {
                    reserve_action(Text(translator, anomaly::MessageId::LauncherModsUpdate));
                }
                next_action(Text(translator, anomaly::MessageId::LauncherModsSettings));
                if (LauncherChip(
                        (std::string(Text(translator, anomaly::MessageId::LauncherModsSettings)) +
                            "##settings" + entry.name).c_str())) {
                    notes_request = &entry;
                }
                next_action(Text(translator, anomaly::MessageId::LauncherModsUninstall));
                const bool uninstall_clicked = DestructiveButton(
                    (std::string(Text(translator, anomaly::MessageId::LauncherModsUninstall)) +
                        "##" + entry.name).c_str(),
                    ImVec2(0.0f, 0.0f), !(game_running && entry.enabled));
                if (uninstall_clicked) uninstall_request = &entry;
                // The source comes last, after the two commands, and keeps its column even when the
                // mod has no link: the batch box after it must sit on the same x on every row.
                if (!entry.link.empty()) {
                    next_action(Text(translator, anomaly::MessageId::LauncherModsOpenSource));
                    if (LauncherChip((std::string(Text(translator, anomaly::MessageId::LauncherModsOpenSource)) +
                            "##" + entry.name).c_str())) {
                        const std::wstring url(entry.link.begin(), entry.link.end());
                        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                } else {
                    reserve_action(Text(translator, anomaly::MessageId::LauncherModsOpenSource));
                }
                ImGui::SameLine();
                bool row_selected = entry.selected;
                if (ImGui::Checkbox(("##select-" + entry.name).c_str(), &row_selected)) {
                    const_cast<LauncherModEntry&>(entry).selected = row_selected;
                    if (row_selected) selected_names.insert(LauncherModStateKey(entry));
                    else selected_names.erase(LauncherModStateKey(entry));
                }
                ImGui::EndGroup();
                ImGui::PopID();
                ImGui::PopID();
                ImGui::Spacing();
            }
            if (done) break;
        }
    }
    if (filtered.empty()) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsNoMatches));
    }
    // The pager floats over the bottom of the list rather than sitting above it: the covers can
    // run under it, and the fixed controls above keep their full width.
    const ImVec2 list_min = ImGui::GetWindowPos();
    const ImVec2 list_size = ImGui::GetWindowSize();
    ImGui::EndChild();
    if (view.mods_pagination && pages > 1) {
        if (DrawModsPager(page, pages, list_min, list_size, translator)) scroll_to_top = true;
    }
    // The drill-in a card and a crumb asked for is applied here, where the loops that raised them
    // have finished; the popups and the update run here too, where the ID stack the popup is drawn
    // under and the entry the update rewrites are both still valid.
    if (level_request >= 0) {
        grid_level = level_request;
        if (grid_level < 2) grid_skin.clear();
        if (grid_level < 1) grid_group.clear();
        page = 0;
        scroll_to_top = true;
    }
    if (group_request_open) {
        grid_group = std::move(group_request);
        grid_skin.clear();
        grid_level = 1;
        page = 0;
        scroll_to_top = true;
    }
    if (skin_request_open) {
        grid_skin = std::move(skin_request);
        grid_level = 2;
        page = 0;
        scroll_to_top = true;
    }
    if (notes_request != nullptr) open_notes(*notes_request);
    if (uninstall_request != nullptr) {
        pending_uninstall = LauncherModStateKey(*uninstall_request);
        ImGui::OpenPopup(modal_name(anomaly::MessageId::LauncherModsUninstall, "confirm-uninstall").c_str());
    }
    if (update_request != nullptr) {
        import_message = LauncherUpdateMod(snapshot, *update_request);
        refresh_mods();
    }
    // A modal keeps the answer to itself -- nothing behind it can be clicked while it is open --
    // but it does not dim: the window's own style has that veil switched off (see
    // ApplyLauncherDpiScale), so a question asks itself over a readable page.
    if (ImGui::BeginPopupModal(modal_name(anomaly::MessageId::LauncherModsUninstall, "confirm-uninstall").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", Format(translator, anomaly::MessageId::LauncherModsConfirmUninstall,
            {pending_uninstall}).c_str());
        if (LauncherChip(Tr(anomaly::MessageId::LauncherModsConfirmUninstallButton).c_str())) {
            for (const auto& entry : mods) {
                if (LauncherModStateKey(entry) != pending_uninstall) continue;
                if (!LauncherRemoveMod(snapshot, entry))
                    import_message = Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
                break;
            }
            pending_uninstall.clear();
            refresh_mods();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (LauncherChip((Tr(anomaly::MessageId::LauncherModsCancel) + "##uninstall").c_str()))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(modal_name(anomaly::MessageId::LauncherModsBulkUninstall, "bulk-uninstall").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::size_t count{};
        for (const auto& entry : mods) {
            if (entry.selected) ++count;
        }
        ImGui::Text("%s", Format(translator, anomaly::MessageId::LauncherModsConfirmUninstallMany,
            {std::to_string(count)}).c_str());
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsConfirmUninstallAll))) {
            for (const auto& entry : mods) {
                if (entry.selected) LauncherRemoveMod(snapshot, entry);
            }
            selected_names.clear();
            bulk_uninstall_pending = false;
            refresh_mods();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (LauncherChip((Tr(anomaly::MessageId::LauncherModsCancel) + "##bulk").c_str())) {
            bulk_uninstall_pending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(modal_name(anomaly::MessageId::LauncherModsNotesTitle, "mod-settings").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherModsDisplayName));
        ImGui::InputText("##notes-name", notes_name_buffer, sizeof(notes_name_buffer));
        // The name on disk, which used to be a dialog of its own: it is part of what a mod's
        // settings are, so it is edited here and applied with the rest.
        ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherModsFileName));
        ImGui::InputText("##notes-file", notes_file_buffer, sizeof(notes_file_buffer));
        ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherModsGroup));
        ImGui::InputText("##notes-group", notes_group_buffer, sizeof(notes_group_buffer));
        ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherModsNotesLabel));
        ImGui::InputTextMultiline("##notes-text", notes_buffer, sizeof(notes_buffer),
            ImVec2(Scale(360.0f), Scale(90.0f)));
        if (notes_directory) {
            ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherModsLinkHint));
            ImGui::SetNextItemWidth(Scale(240.0f));
            ImGui::InputText("##notes-id", notes_id_buffer, sizeof(notes_id_buffer));
            ImGui::SameLine();
            if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsAssociate))) {
                for (const auto& entry : mods) {
                    if (LauncherModStateKey(entry) != notes_source) continue;
                    std::string raw = notes_id_buffer;
                    const std::string parsed_id = LauncherGameBananaId(raw);
                    const std::string id = parsed_id.empty() ? raw : parsed_id;
                    if (!id.empty()) {
                        const std::wstring path = std::wstring(L"/apiv11/Mod/") +
                            std::wstring(id.begin(), id.end()) + L"/ProfilePage";
                        const std::string body = LauncherHttpGet(L"gamebanana.com", path);
                        if (!body.empty()) {
                            LauncherWriteModSource(entry.icon_path, id, LauncherGameBananaFileId(body),
                                LauncherGameBananaFileName(body), LauncherGameBananaFileMd5(body),
                                LauncherGameBananaAuthor(body), entry.display_name);
                            import_message = Tr(anomaly::MessageId::LauncherModsLinked, {id});
                        } else {
                            import_message = Tr(anomaly::MessageId::LauncherModsLinkFailed);
                        }
                    }
                    break;
                }
            }
        }
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherModsSave))) {
            // The rename goes first: it moves the mod's files, and the notes are written next to
            // them, so the fresh entry has to be read back before they are saved.
            const std::string wanted = notes_file_buffer;
            std::string renamed;
            for (const auto& entry : mods) {
                if (LauncherModStateKey(entry) != notes_source) continue;
                if (wanted.empty() || wanted == entry.name) break;
                if (!LauncherRenameMod(snapshot, entry, wanted, &renamed))
                    import_message = Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
                break;
            }
            if (!renamed.empty()) refresh_mods();
            for (const auto& entry : mods) {
                const bool target = LauncherModStateKey(entry) == (renamed.empty() ? notes_source : renamed);
                if (!target) continue;
                if (!LauncherWriteModNotes(entry, notes_name_buffer, notes_buffer, notes_group_buffer))
                    import_message = Tr(anomaly::MessageId::SettingsSaveFailed);
                break;
            }
            refresh_mods();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (LauncherChip((Tr(anomaly::MessageId::LauncherModsCancel) + "##notes").c_str()))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DrawAttachMode(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionProcesses));
    // The status is the controller's own last message, which already carries every
    // distinction this needs: waiting for HTGame.exe while the launch runs, then the
    // attached PID or the failure, coloured by its kind. It replaced a static "ready"
    // line that said the same thing no matter what the launcher was doing, and it
    // also restores the reporting the deleted footer used to provide.
    const bool show_message = snapshot.message_scope != MessageScope::Proxy;
    const ImVec4 status_ink = show_message && snapshot.message_kind == MessageKind::Error
        ? ThemeColor(theme.danger)
        : show_message && snapshot.message_kind == MessageKind::Success
            ? ThemeColor(theme.success)
            : LauncherMutedInk();
    const std::string process_id = std::to_string(snapshot.attached_process != 0
        ? snapshot.attached_process
        : snapshot.processes.empty() ? 0 : snapshot.processes.front().process_id);
    const std::string status = show_message ? RenderMessage(translator, snapshot.message)
        : !snapshot.core_available ? std::string(Text(translator, anomaly::MessageId::LauncherStatusCoreUnavailable))
        : snapshot.processes.empty() ? std::string(Text(translator, anomaly::MessageId::LauncherStatusNoProcesses))
        : Format(translator, snapshot.attached_process != 0
            ? anomaly::MessageId::LauncherStatusLaunchAttached
            : anomaly::MessageId::LauncherStatusProcessFound, {process_id});
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - StatusRefreshReserve());
    ImGui::TextColored(status_ink, "%s", status.c_str());
    ImGui::PopTextWrapPos();
    // The process summary needs a refresh of its own. Without one the launcher keeps
    // reporting the process it attached to after that process has exited, and the
    // stale list leaves the attach action disabled until the launcher is restarted.
    const float process_status_top =
        ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y;
    if (StatusRefreshButton(
            "refresh-processes",
            Text(translator, anomaly::MessageId::LauncherProcessRefresh),
            !snapshot.busy, process_status_top)) {
        controller.RefreshProcesses();
    }

    if (snapshot.proxy.proxy_enabled) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherInjectProxyEnabled));
    }
    const bool can_launch = !snapshot.busy && !snapshot.proxy.proxy_enabled && snapshot.core_available &&
        !snapshot.launcher_executable.empty() && snapshot.processes.empty();
    // A game that is already running is the other half of the same choice: the launch action needs an
    // empty process list, this one needs a target in it.
    DWORD inject_target{};
    for (const auto& process : snapshot.processes) {
        if (process.Compatible()) {
            inject_target = process.process_id;
            break;
        }
    }
    const bool can_inject = !snapshot.busy && !snapshot.proxy.proxy_enabled && snapshot.core_available && inject_target != 0;
    // The same bottom-right placement and size as the proxy mode's primary action, so switching modes does not
    // move the button under the cursor -- but the widths are measured from the labels, because a fixed width
    // ellipsized the English "Inject into running process" down to "Inject into running ...".
    const char* inject_label = Text(translator, anomaly::MessageId::LauncherActionInjectRunning);
    const char* launch_label = Text(translator, anomaly::MessageId::LauncherLaunchAttach);
    const float inject_width = (std::max)(200.0f, CommandButtonWidth(0xe8e5, inject_label));
    const float launch_width = (std::max)(240.0f, CommandButtonWidth(0xe768, launch_label));
    ImGui::SetCursorPosY((std::max)(ImGui::GetCursorPosY() + Scale(20.0f),
        ImGui::GetWindowHeight() - Scale(kLaunchActionRowBottom)));
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
        ImGui::GetWindowWidth() - Scale(inject_width + 12.0f + launch_width) - Scale(16.0f)));
    if (CommandButton(
            "inject-running-process", 0xe8e5, inject_label, false,
            can_inject, ImVec2(inject_width, 62.0f))) {
        controller.InjectIntoRunningProcess(inject_target);
    }
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (CommandButton(
            "launch-attach-core", 0xe768, launch_label, true, can_launch,
            ImVec2(launch_width, 62.0f))) {
        controller.LaunchAndAttach();
    }
}

// A support button is the platform's own artwork, drawn at the width it is published for and at the
// height that width implies, inside the capsule every command in this window is drawn in. The picture
// is embedded in the image rather than fetched, so the button also appears on a machine that never
// reaches the network -- and a launcher whose texture failed to upload falls back to a box of the same
// size carrying its label.
ImVec2 SupportButtonSize(const ComPtr<ID3D11ShaderResourceView>& texture,
    const std::uint32_t width, const std::uint32_t height) {
    (void)texture;
    const float button_width = Scale(kSupportButtonWidth);
    const float aspect = width != 0 && height != 0
        ? static_cast<float>(height) / static_cast<float>(width)
        : 0.25f;
    return ImVec2(button_width, button_width * aspect);
}

// The two controls in this window that leave the machine: they hand the address to the shell, which
// opens it in the user's browser, rather than reimplementing a payment flow in a game launcher.
bool SupportButton(const ComPtr<ID3D11ShaderResourceView>& texture, const std::uint32_t width,
    const std::uint32_t height, const char* id, const char* tooltip, const std::wstring_view url) {
    const ImVec2 size = SupportButtonSize(texture, width, height);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const float rounding = size.y * 0.5f;
    if (texture != nullptr) {
        // Full brightness under the pointer, a shade less without it: the artwork carries its own
        // colour, so the only feedback these buttons can give is how brightly they are painted.
        draw->AddImageRounded(
            static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture.Get())),
            min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, hovered ? 1.0f : 0.92f)),
            rounding, ImDrawFlags_RoundCornersAll);
    } else {
        // The palette's own accent, and the label centred in it.
        draw->AddRectFilled(min, max,
            ImGui::GetColorU32(ThemeColor(ue5mem::PlatformUiTheme().accent)), rounding);
        const ImVec2 caption = ImGui::CalcTextSize(tooltip);
        draw->AddText(ImVec2(min.x + (size.x - caption.x) * 0.5f,
                          min.y + (size.y - caption.y) * 0.5f),
            ImGui::GetColorU32(ThemeColor(ue5mem::PlatformUiTheme().inverse_text)), tooltip);
    }
    Tooltip(tooltip);
    if (pressed) {
        const std::wstring address(url);
        ShellExecuteW(nullptr, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return pressed;
}

void DrawLaunchMode(
    HWND window, LauncherController& controller,
    const LauncherSnapshot& snapshot, const anomaly::Translator& translator) {
    static int launch_mode = 0;

    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionNteLauncher));
    DrawReadOnlyPath("##NteLauncherDirectory", snapshot.launcher_executable.parent_path(), 38.0f, translator);
    ImGui::SameLine();
    const std::string choose_label = Format(translator, anomaly::MessageId::LauncherChooseNteLauncher,
        {ClientLauncherFileName(snapshot.selected_client)});
    if (IconButton("choose-nte-launcher", 0xe838, choose_label.c_str(), !snapshot.busy)) {
        if (const auto selected = ChooseLauncherExecutable(
                window, snapshot.launcher_executable, snapshot.selected_client, translator)) {
            controller.SelectLauncherExecutable(*selected);
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionGameDirectory));
    DrawReadOnlyPath("##GameDirectory", snapshot.game_directory, 38.0f, translator);
    ImGui::SameLine();
    if (IconButton("choose-game-directory", 0xe838,
            Text(translator, anomaly::MessageId::LauncherChooseGameDirectory), !snapshot.busy)) {
        if (const auto selected = ChooseGameDirectory(window, snapshot.game_directory, translator)) {
            controller.SelectGameDirectory(*selected);
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const std::array<const char*, 2> labels{
        Text(translator, anomaly::MessageId::LauncherModeLiveAttach),
        Text(translator, anomaly::MessageId::LauncherModeProxyInstall)};
    anomaly::glass::SegmentedOptions options;
    options.height = 54.0f;
    options.padding_x = 38.0f;
    options.scale = g_launcher_dpi_scale;
    anomaly::glass::SegmentedControl("launch-mode", labels, launch_mode, options);
    ImGui::Spacing();
    if (ImGui::BeginChild("LaunchModeContent", ImVec2(0.0f, 0.0f))) {
        if (launch_mode == 0) DrawAttachMode(controller, snapshot, translator);
        else DrawProxyMode(controller, snapshot, translator);
    }
    ImGui::EndChild();
}

void DrawModMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    static int mod_mode = 0;
    const std::array<const char*, 2> labels{
        Text(translator, anomaly::MessageId::LauncherModeManage),
        Text(translator, anomaly::MessageId::LauncherModeBrowse)};
    anomaly::glass::SegmentedOptions options;
    options.height = 54.0f;
    options.padding_x = 38.0f;
    options.scale = g_launcher_dpi_scale;
    anomaly::glass::SegmentedControl("mod-mode", labels, mod_mode, options);
    ImGui::SameLine(0.0f, Scale(32.0f));
    LauncherModSettingsCache& settings = RefreshLauncherModSettings(snapshot);
    const char* const enable_label = Text(translator, anomaly::MessageId::LauncherModsEnableMods);
    const char* const compatibility_label = Text(translator, anomaly::MessageId::LauncherModsCompatibilityMode);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
        (options.height * g_launcher_dpi_scale - ImGui::GetFrameHeight()) * 0.5f);
    const float switch_row_y = ImGui::GetCursorScreenPos().y;
    static std::string settings_error;
    const bool game_running = !snapshot.processes.empty();
    bool bypass = settings.bypass;
    ImGui::BeginDisabled(snapshot.game_directory.empty() || (game_running && settings.compatibility));
    if (anomaly::glass::Switch(enable_label, &bypass)) {
        if (LauncherWriteBypassEnabled(snapshot, bypass)) {
            settings.bypass = bypass;
            settings_error.clear();
        } else {
            settings_error = Text(translator, anomaly::MessageId::SettingsSaveFailed);
        }
    }
    const float enable_switch_right = ImGui::GetItemRectMax().x;
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(enable_switch_right + ImGui::GetStyle().ItemSpacing.x,
        switch_row_y));
    bool compatibility = settings.compatibility;
    ImGui::BeginDisabled(snapshot.game_directory.empty() || game_running);
    if (anomaly::glass::Switch(compatibility_label, &compatibility)) {
        if (LauncherWriteCompatibilityMode(snapshot, compatibility)) {
            settings.compatibility = compatibility;
            settings.bypass = LauncherReadBypassEnabled(snapshot);
            settings_error.clear();
        } else {
            settings_error = Text(translator, anomaly::MessageId::LauncherModsModeSaveFailed);
        }
    }
    const float compatibility_switch_right = ImGui::GetItemRectMax().x;
    ImGui::EndDisabled();
    Tooltip(Text(translator, anomaly::MessageId::LauncherModsCompatibilityHint));
    // The exclusivity switch lives on this row rather than next to the list: it answers the same
    // question as the two switches left of it -- how the mods are loaded -- and this row stays put
    // while the list below scrolls. It only changes what the next classification of that list does,
    // so it asks for one instead of rebuilding the rows from here.
    ImGui::SetCursorScreenPos(
        ImVec2(compatibility_switch_right + ImGui::GetStyle().ItemSpacing.x, switch_row_y));
    bool force_load = settings.force_load;
    ImGui::BeginDisabled(snapshot.game_directory.empty());
    if (anomaly::glass::Switch(
            Text(translator, anomaly::MessageId::LauncherModsForceLoad), &force_load)) {
        if (LauncherWriteForceLoad(snapshot, force_load)) {
            settings.force_load = force_load;
            g_mods_refresh_requested = true;
            settings_error.clear();
        } else {
            settings_error = Text(translator, anomaly::MessageId::SettingsSaveFailed);
        }
    }
    ImGui::EndDisabled();
    if (!settings_error.empty()) ImGui::TextDisabled("%s", settings_error.c_str());
    if (ImGui::BeginChild(mod_mode == 0 ? "ModManagementContent" : "ModDownloadContent", ImVec2(0.0f, 0.0f))) {
        if (mod_mode == 0) DrawModsMode(window, controller, snapshot, translator);
        else DrawBrowseMode(window, controller, snapshot, translator);
    }
    ImGui::EndChild();
}

// The settings page holds what belongs to the launcher window itself rather than to
// one of the install modes above: the theme decides how all four of them look, and the
// hotkey is the same key whichever page is showing. Both were written straight into
// the launcher configuration, so the page needs no apply step.
// One picture for the launcher's own theme. It is copied next to the runtime's own artwork and
// stored as a path relative to that root, so the theme resolver reads it exactly like the pictures
// the theme files name and the configuration survives the launcher folder being moved.
std::optional<std::string> ChooseLauncherThemeImage(
    HWND owner, const anomaly::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    }
    const COMDLG_FILTERSPEC filters[]{{L"Images", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp"}};
    dialog->SetFileTypes(1, filters);
    const std::wstring title = Utf8Wide(
        translator.Text(anomaly::MessageId::SettingsBackgroundOpen));
    dialog->SetTitle(title.c_str());
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path source(raw);
    CoTaskMemFree(raw);
    const auto directory = ExecutableDirectory() / L"Anomaly" / L"assets" / L"theme";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return std::nullopt;
    const auto target = directory / source.filename();
    bool same{};
    if (std::filesystem::exists(target, error))
        same = std::filesystem::equivalent(source, target, error) && !error;
    if (!same) {
        std::filesystem::copy_file(source, target,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) return std::nullopt;
    }
    return LauncherNarrow(std::filesystem::path(L"assets") / L"theme" / source.filename());
}

// The release page in the user's browser: the one place that explains a version in full, and where
// the dialog sends anyone the lookup itself could not help.
void LauncherOpenReleasePage(const LauncherUpdateStatus& update) {
    const std::wstring url = Utf8Wide(update.page_url.empty()
        ? std::string(anomaly::launcher::kReleasePageUrl) : update.page_url);
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The failure a check or an install reported, worded by the launcher where it can be. A rate-limited
// API, an unreachable host and an answer that is not a release are different problems with
// different answers, and the installer's own diagnostics are passed through as they were produced.
std::string UpdateFailureText(
    const anomaly::Translator& translator, const LauncherUpdateStatus& update) {
    const std::string status = std::to_string(update.http_status);
    switch (update.failure) {
    case LauncherUpdateFailure::Network:
        return Text(translator, anomaly::MessageId::LauncherUpdateNetworkFailed);
    case LauncherUpdateFailure::RateLimited:
        return Format(translator, anomaly::MessageId::LauncherUpdateRateLimited, {status});
    case LauncherUpdateFailure::Http:
        return Format(translator, anomaly::MessageId::LauncherUpdateHttpFailed, {status});
    case LauncherUpdateFailure::NotRelease:
        return Format(translator, anomaly::MessageId::LauncherUpdateNotRelease, {status});
    case LauncherUpdateFailure::Check:
        return Format(translator, anomaly::MessageId::LauncherUpdateCheckFailed, {update.detail});
    case LauncherUpdateFailure::Install:
        return Format(translator, anomaly::MessageId::LauncherUpdateFailed, {update.detail});
    case LauncherUpdateFailure::None:
        break;
    }
    return Format(translator, anomaly::MessageId::LauncherUpdateCheckFailed, {update.detail});
}

// The update's own byte counters and cancellation control, independent of a mod download.
// A transfer without a reported length shows received bytes instead of a percentage.
void LauncherDrawDownloadProgress(const anomaly::Translator& translator) {
    const double done = static_cast<double>(g_update_download.bytes.load());
    const double total = static_cast<double>(g_update_download.total.load());
    // Seconds since this download started: the bar only appears while one runs, so a static clock
    // reset when the transfer begins is all the elapsed time it needs.
    static double started{};
    static long long last_seen{};
    static bool running{};
    if (!running || done < last_seen) {
        started = ImGui::GetTime();
        running = true;
    }
    last_seen = static_cast<long long>(done);
    const double seconds = ImGui::GetTime() - started;
    const double megabytes = done / (1024.0 * 1024.0);
    if (total > 0.0) {
        const std::string overlay = Format(translator,
            anomaly::MessageId::LauncherBrowseProgressPercent,
            {Num(megabytes, 1), Num(total / (1024.0 * 1024.0), 1),
                Num(done * 100.0 / total, 0), Num(seconds, 0)});
        ImGui::ProgressBar(static_cast<float>(done / total), ImVec2(Scale(260.0f), 0.0f),
            overlay.c_str());
    } else {
        ImGui::TextDisabled("%s", Format(translator,
            anomaly::MessageId::LauncherBrowseProgressBytes,
            {Num(megabytes, 1), Num(seconds, 0)}).c_str());
    }
    ImGui::SameLine();
    // The one command that makes sense beside a running transfer.
    if (LauncherChip((Tr(anomaly::MessageId::LauncherModsCancel) +
            "##cancel-update-download").c_str())) {
        g_update_download.cancel.store(true);
    }
}

// The state of the running update, as both the page and the dialog report it.
std::string UpdatePhaseText(
    const anomaly::Translator& translator, const LauncherUpdateStatus& update) {
    switch (update.phase) {
    case LauncherUpdatePhase::Idle:
        return {};
    case LauncherUpdatePhase::Checking:
        return Text(translator, anomaly::MessageId::LauncherUpdateChecking);
    case LauncherUpdatePhase::UpToDate:
        return Format(translator, anomaly::MessageId::LauncherUpdateUpToDate,
            {update.remote_version});
    case LauncherUpdatePhase::Available:
        return Format(translator, anomaly::MessageId::LauncherUpdateAvailable,
            {update.remote_version});
    case LauncherUpdatePhase::Downloading:
        return Text(translator, anomaly::MessageId::LauncherUpdateDownloading);
    case LauncherUpdatePhase::Installing:
        return Text(translator, anomaly::MessageId::LauncherUpdateInstalling);
    case LauncherUpdatePhase::Installed:
        return Text(translator, anomaly::MessageId::LauncherUpdateInstalled);
    case LauncherUpdatePhase::Failed:
        return UpdateFailureText(translator, update);
    }
    return {};
}

ImVec4 UpdatePhaseInk(const LauncherUpdatePhase phase) {
    const auto& theme = ue5mem::PlatformUiTheme();
    switch (phase) {
    case LauncherUpdatePhase::Available: return ThemeColor(theme.warning);
    case LauncherUpdatePhase::Installed: return ThemeColor(theme.success);
    case LauncherUpdatePhase::Failed: return ThemeColor(theme.danger);
    default: return LauncherMutedInk();
    }
}

// The launcher's own updates: the version this build is, the newest release it found, and the two
// commands that follow from them. The update replaces the files of this folder and restarts the
// window, so the button that starts it names the version it installs.
void DrawUpdateSettings(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const LauncherUpdateStatus& update = snapshot.update;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionUpdates));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingAutoUpdate));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(70.0f) - Scale(10.0f));
    bool auto_check = update.auto_check;
    if (anomaly::glass::Switch("##launcher-auto-update", &auto_check)) {
        controller.SetAutoCheckUpdates(auto_check);
    }
    ImGui::TextDisabled("%s", Format(translator, anomaly::MessageId::LauncherUpdateCurrent,
        {ANOMALY_SDK_VERSION_STRING}).c_str());
    const bool busy = snapshot.busy || update.Busy();
    if (LauncherChip(Text(translator, update.phase == LauncherUpdatePhase::Checking
            ? anomaly::MessageId::LauncherUpdateChecking
            : anomaly::MessageId::LauncherUpdateCheck), {}, !busy)) {
        controller.CheckForUpdates();
    }
    // The update command exists only while there is a release to install, so the row never offers
    // something the launcher has no version for.
    if (update.CanInstall()) {
        ImGui::SameLine();
        if (LauncherChip(Format(translator, anomaly::MessageId::LauncherUpdateInstall,
                {update.remote_version}).c_str(), {}, !busy)) {
            controller.InstallUpdate();
        }
    }
    ImGui::SameLine();
    // The release page is the only place that says what a new version changes, and it is where a
    // user goes when they would rather read about it before installing.
    if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateOpenPage))) {
        LauncherOpenReleasePage(update);
    }
    const std::string status = UpdatePhaseText(translator, update);
    if (update.phase == LauncherUpdatePhase::Downloading) {
        LauncherDrawDownloadProgress(translator);
    } else if (!status.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(UpdatePhaseInk(update.phase), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
}

// The release notes are markdown, and the shell already owns the parser for the markdown it draws
// (the first-run notice is authored in it). The launcher reuses that parser and draws the blocks
// itself, in its own ink: the same document, rendered the way the rest of this window is.
//
// Emphasis is faux bold -- the run drawn twice -- because the launcher bakes one font weight, and a
// run that wrapped is not doubled: the second draw would land on the line above it.
void LauncherDrawEmphasis(const std::string& text) {
    ImGui::TextUnformatted(text.c_str());
    if (ImGui::GetItemRectSize().y > ImGui::GetTextLineHeight() * 1.6f) return;
    const ImVec2 origin = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(origin.x + (std::max)(0.5f, Scale(0.5f)), origin.y),
        ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
}

// One line of a block: the runs stay in a single sentence, so emphasis and links keep their place.
void LauncherDrawNoticeRuns(const std::vector<anomaly::NoticeRun>& runs) {
    const auto& theme = ue5mem::PlatformUiTheme();
    ImGui::PushTextWrapPos();
    for (std::size_t index = 0; index < runs.size(); ++index) {
        if (index > 0) ImGui::SameLine(0.0f, 0.0f);
        const anomaly::NoticeRun& run = runs[index];
        switch (run.style) {
        case anomaly::NoticeRunStyle::Text:
            ImGui::TextUnformatted(run.text.c_str());
            break;
        case anomaly::NoticeRunStyle::Emphasis:
            LauncherDrawEmphasis(run.text);
            break;
        case anomaly::NoticeRunStyle::Code:
            ImGui::TextColored(LauncherMutedInk(), "%s", run.text.c_str());
            break;
        case anomaly::NoticeRunStyle::Link:
            ImGui::TextColored(ThemeColor(theme.accent), "%s", run.text.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", run.url.c_str());
            }
            // A link in a release note is the one thing here that leaves the machine: it goes to the
            // shell like every other address this window opens.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !run.url.empty()) {
                const std::wstring url = Utf8Wide(run.url);
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
    }
    ImGui::PopTextWrapPos();
}

// A heading: the same text a little larger and painted twice a hair apart, which is the whole of the
// emphasis this window has -- it bakes one font weight, so "bold" is the glyphs drawn again and
// "larger" is the atlas drawn at a bigger size. A heading that would not fit the pane keeps the body
// size and wraps like any other line, because a clipped heading is worse than a plain one.
void LauncherDrawHeading(const std::vector<anomaly::NoticeRun>& runs, const bool muted) {
    std::string text;
    for (const anomaly::NoticeRun& run : runs) text += run.text;
    if (text.empty()) return;
    ImFont* const font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 1.18f;
    const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const ImVec4 ink = muted ? LauncherMutedInk() : LauncherInk();
    if (measured.x > ImGui::GetContentRegionAvail().x) {
        ImGui::TextWrapped("%s", text.c_str());
        return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ink);
    const float bold = (std::max)(0.6f, Scale(0.6f));
    draw->AddText(font, size, origin, color, text.c_str());
    draw->AddText(font, size, ImVec2(origin.x + bold, origin.y), color, text.c_str());
    ImGui::Dummy(measured);
}

void LauncherDrawReleaseNotes(const anomaly::NoticeDocument& document) {
    for (std::size_t index = 0; index < document.blocks.size(); ++index) {
        const anomaly::NoticeBlock& block = document.blocks[index];
        switch (block.kind) {
        case anomaly::NoticeBlockKind::Heading:
        case anomaly::NoticeBlockKind::Subheading: {
            // The release opens with its own name and date, which is context rather than a section;
            // every heading is the same emphasis, only the first is quieter.
            LauncherDrawHeading(block.runs, index == 0);
            ImGui::Spacing();
            break;
        }
        case anomaly::NoticeBlockKind::Paragraph:
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case anomaly::NoticeBlockKind::Bullet:
            ImGui::Bullet();
            ImGui::SameLine();
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case anomaly::NoticeBlockKind::Numbered:
            ImGui::TextUnformatted(block.marker.c_str());
            ImGui::SameLine();
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case anomaly::NoticeBlockKind::Separator:
            ImGui::Separator();
            break;
        }
    }
}

// The parsed notes, kept across frames: the document only changes when the release does, and parsing
// a few kilobytes of markdown every frame for a dialog that is usually closed is wasted work.
// git-cliff marks a change's scope with single asterisks -- "*(autotaxi)* Add ..." -- which is not
// emphasis in the shell's markdown subset, so the markers would reach the screen. Keeping "**" (real
// bold) and the asterisk a bullet starts with, and dropping every other one, leaves the scope text
// readable and the bold intact.
std::string LauncherNotesMarkdown(const std::string& notes) {
    std::string cleaned;
    cleaned.reserve(notes.size());
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const char character = notes[index];
        if (character != '*') {
            cleaned.push_back(character);
            continue;
        }
        const bool line_start = index == 0 || notes[index - 1] == '\n' ||
            (notes[index - 1] == ' ' && index > 1 && notes[index - 2] == '\n');
        if (line_start) {
            cleaned.push_back(character);
            continue;
        }
        if (index + 1 < notes.size() && notes[index + 1] == '*') {
            cleaned.append("**");
            ++index;
        }
    }
    return cleaned;
}

const anomaly::NoticeDocument* LauncherReleaseNotes(const std::string& version,
    const std::string& notes) {
    static std::string parsed_for;
    static anomaly::NoticeDocument document;
    const std::string key = version + ':' + std::to_string(notes.size());
    if (key != parsed_for) {
        document = anomaly::ParseNoticeMarkdown(LauncherNotesMarkdown(notes), 0);
        parsed_for = key;
    }
    return &document;
}

// The prompt an update check ends in: the release's own notes in a pane, and two answers below --
// skipping that version on the left, updating on the right. Nothing is installed until the right one
// is pressed, which is the difference between an update and a silent one.
void DrawUpdatePrompt(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const LauncherUpdateStatus& update = snapshot.update;
    // The dialog's name is its id, and nothing else: its title is drawn below, four pixels lower than
    // the title bar would have put it, so the line under it keeps the place it has always had. The two
    // strings still have to be the same one -- OpenPopup and BeginPopupModal pair on this id.
    const std::string title = Format(translator, anomaly::MessageId::LauncherUpdateAvailable,
        {update.remote_version});
    const std::string name = "##update-available";
    // One question per release, and none for a release the user already skipped: a version answered
    // once is not asked about again, this session or a later one. The band's chip asks for the same
    // dialog explicitly, which is why that path opens it even when this session already saw the
    // automatic one.
    static std::string prompted_version;
    const bool asked = std::exchange(g_update_prompt_requested, false);
    if (update.phase == LauncherUpdatePhase::Available && !update.remote_version.empty() &&
        (asked || (update.skipped_version != update.remote_version &&
            prompted_version != update.remote_version))) {
        prompted_version = update.remote_version;
        ImGui::OpenPopup(name.c_str());
    }
    // The popup's padding: its left edge is the inset the title bar uses (FramePadding.x), so the
    // first line of content sits exactly under the title instead of a few pixels left of it, and
    // the bottom of it gives the last row the same room the top gives the first.
    const ImGuiStyle& dialog_style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(dialog_style.FramePadding.x, Scale(2.0f)));
    // Centred on the launcher's canvas -- not on the OS window, which reaches up into the transparent
    // band above it -- and held there: a question is not a window the user should have to put back.
    const ImVec2 canvas_min = ImGui::GetWindowPos();
    const ImVec2 canvas_size = ImGui::GetWindowSize();
    ImGui::SetNextWindowPos(
        ImVec2(canvas_min.x + canvas_size.x * 0.5f, canvas_min.y + canvas_size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Sized by its contents, like the mod settings dialog, so it grows with the language it is drawn
    // in rather than clipping a localized label.
    if (!ImGui::BeginPopupModal(name.c_str(), nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize)) {
        ImGui::PopStyleVar();
        return;
    }
    // The title bar itself draws nothing: its text is put here instead, a few pixels lower than the
    // bar would have placed it, so the line below keeps the position it has always had. The clip is
    // widened for it -- a window clips its items to the content area, and the title bar is above that
    // -- or the text would be drawn and never seen.
    {
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 corner(origin.x + ImGui::GetWindowSize().x,
            origin.y + ImGui::GetFontSize() + dialog_style.FramePadding.y * 2.0f);
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, corner, false);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
            ImVec2(origin.x + dialog_style.FramePadding.x,
                origin.y + dialog_style.FramePadding.y + Scale(4.0f)),
            ImGui::GetColorU32(LauncherInk()), title.c_str());
        draw->PopClipRect();
    }
    // The version being left behind, under the title that names the one being installed.
    ImGui::TextDisabled("%s", Format(translator, anomaly::MessageId::LauncherUpdateCurrent,
        {ANOMALY_SDK_VERSION_STRING}).c_str());
    ImGui::Spacing();

    // The notes sit in a pane of their own, on the same material as the page's cards but quieter: it
    // is a recess inside the dialog, not another object floating over it.
    const float pane_width = Scale(760.0f);
    const bool has_notes = !update.notes.empty();
    if (has_notes) {
        const float pane_height = Scale(380.0f);
        const ImVec2 pane_min = ImGui::GetCursorScreenPos();
        const ImVec2 pane_max(pane_min.x + pane_width, pane_min.y + pane_height);
        // A plain recess, not a second pane of glass: the material belongs to the surface a window is
        // cut from, and another layer of it inside the first only muddies both. A tinted fill with a
        // hairline is what a panel inside a dialog reads as, and it keeps the notes legible.
        const auto& theme = ue5mem::PlatformUiTheme();
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pane_min, pane_max,
            ImGui::GetColorU32(ThemeColorWithAlpha(theme.child_background, 0.72f)),
            Scale(12.0f));
        draw->AddRect(pane_min, pane_max,
            ImGui::GetColorU32(ThemeColorWithAlpha(theme.border, 0.55f)),
            Scale(12.0f), ImDrawFlags_None, Scale(1.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        // A child without a border ignores style.WindowPadding unless it is asked to use it, which is
        // how the notes ended up written against the pane's edges. AlwaysUseWindowPadding gives the
        // text the inset it needs, and the wrap position follows that inset instead of the window edge.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(18.0f, 14.0f));
        ImGui::BeginChild("##update-notes", ImVec2(pane_width, pane_height),
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
        // The release publishes markdown; the shell's own parser turns it into blocks, and the blocks
        // are drawn here in this window's ink rather than dumped as a wall of hashes and asterisks.
        LauncherDrawReleaseNotes(*LauncherReleaseNotes(update.remote_version, update.notes));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    const bool busy = snapshot.busy || update.Busy();
    const std::string status = UpdatePhaseText(translator, update);
    if (update.phase == LauncherUpdatePhase::Downloading) {
        LauncherDrawDownloadProgress(translator);
    } else if (!status.empty() && update.phase != LauncherUpdatePhase::Available) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(UpdatePhaseInk(update.phase), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    // The two answers are centred as one pair with a gap between them: a dialog asks a question, and
    // its answers read as the pair that answers it rather than as two corners of the window. The update
    // is the launcher's primary command -- accent inside the glass, with the download mark -- so the
    // pair also carries the hierarchy.
    const float row_height = ButtonHeight(44.0f);
    const char* const skip_label = Text(translator, anomaly::MessageId::LauncherUpdateSkip);
    const std::string update_label = Format(translator, anomaly::MessageId::LauncherUpdateInstall,
        {update.remote_version});
    // Both widths are measured the way the two controls size themselves -- the chip from its caption,
    // the command from its mark and label -- so the pair stays centred in every language.
    const float skip_width = ImGui::CalcTextSize(skip_label).x + Scale(30.0f);
    const float update_width = ImGui::CalcTextSize(Glyph(0xe8b7)).x +
        ImGui::CalcTextSize(update_label.c_str()).x + Scale(46.0f);
    float pair_width = skip_width + update_width + Scale(16.0f);
    if (!has_notes) {
        // A release without notes has nothing to read here, so the page that does have something to
        // read joins the pair instead of leaving an empty pane on screen.
        pair_width += ImGui::CalcTextSize(
            Text(translator, anomaly::MessageId::LauncherUpdateOpenPage)).x + Scale(46.0f);
    }
    const float pair_left = ImGui::GetCursorPosX() +
        (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - pair_width) * 0.5f);
    ImGui::SetCursorPosX(pair_left);
    ImGui::BeginDisabled(busy);
    if (LauncherChip(skip_label, ImVec2(skip_width, row_height))) {
        controller.SkipUpdateVersion(update.remote_version);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (!has_notes) {
        ImGui::SameLine(0.0f, Scale(8.0f));
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateOpenPage),
                ImVec2(0.0f, row_height))) {
            LauncherOpenReleasePage(update);
        }
    }
    ImGui::SameLine(0.0f, Scale(16.0f));
    ImGui::BeginDisabled(busy);
    if (CommandButton("update-install", 0xe8b7, update_label.c_str(), true, true,
            ImVec2(update_width / g_launcher_dpi_scale, 44.0f))) {
        controller.InstallUpdate();
    }
    ImGui::EndDisabled();
    // The package is downloaded from github.com, which a network behind the wrong wall may not reach
    // at all. These are the same release published where it can be fetched, offered next to the
    // launcher's own download instead of in place of it. No extra spacing above them: the line after
    // the answers already carries the row spacing, and a Spacing here doubled it.
    {
        const float quark_width = ImGui::CalcTextSize(
            Text(translator, anomaly::MessageId::LauncherUpdateQuark)).x + Scale(30.0f);
        const float baidu_width = ImGui::CalcTextSize(
            Text(translator, anomaly::MessageId::LauncherUpdateBaidu)).x + Scale(30.0f);
        const float links_width = quark_width + baidu_width + Scale(8.0f);
        const float links_left = ImGui::GetCursorPosX() +
            (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - links_width) * 0.5f);
        ImGui::SetCursorPosX(links_left);
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateQuark),
                ImVec2(quark_width, row_height))) {
            const std::wstring url(anomaly::launcher::kReleaseQuarkUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0.0f, Scale(8.0f));
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateBaidu),
                ImVec2(baidu_width, row_height))) {
            const std::wstring url(anomaly::launcher::kReleaseBaiduUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    // The padding no longer gives the bottom the room the top does not need, so the last row keeps a
    // reserve of its own instead of sitting on the window's edge.
    ImGui::Dummy(ImVec2(0.0f, Scale(10.0f)));
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

// The support buttons: the platforms' own artwork, side by side under one heading. Both open in the
// browser; neither is a payment flow this window implements.
void DrawSupportSettings(const anomaly::Translator& translator) {
    if (g_graphics == nullptr) return;
    const Graphics& graphics = *g_graphics;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionSupport));
    const ImVec2 afdian = SupportButtonSize(graphics.sponsor,
        graphics.sponsor_width, graphics.sponsor_height);
    const ImVec2 kofi = SupportButtonSize(graphics.kofi,
        graphics.kofi_width, graphics.kofi_height);
    // The pair sits on one line at the section's own left edge, so it starts where the heading and
    // every row above it start. Each picture is still centred vertically against the taller of the
    // two, because the cards are not the same height and a shared baseline would stagger them.
    const float row_height = (std::max)(afdian.y, kofi.y);
    const float row_top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(row_top + (row_height - afdian.y) * 0.5f);
    SupportButton(graphics.sponsor, graphics.sponsor_width, graphics.sponsor_height,
        "##support-afdian", Text(translator, anomaly::MessageId::LauncherSupportAfdian),
        kSponsorUrl);
    ImGui::SameLine(0.0f, Scale(12.0f));
    ImGui::SetCursorPosY(row_top + (row_height - kofi.y) * 0.5f);
    SupportButton(graphics.kofi, graphics.kofi_width, graphics.kofi_height, "##support-kofi",
        Text(translator, anomaly::MessageId::LauncherSupportKofi), kKofiUrl);
}

void DrawSettingsMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    // The page may outgrow the body -- theme, language, the mods page options and the startup
    // settings together are taller than the canvas on a narrow window -- so it scrolls on its
    // own, and its scrollbar appears only when it actually needs one.
    ImGui::BeginChild("SettingsPage", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionAppearance));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingTheme));
    // Naiwa leads the row because it is the shipped default and the only preset with a
    // wallpaper of its own, so the segment a fresh launcher opens on is the first one.
    // AnomalyHub is the shell's own dark palette and closes the row, the same order the
    // in-game settings lists them in.
    constexpr std::array<anomaly::PlatformUiPalette, 7> palettes{
        anomaly::PlatformUiPalette::Naiwa,
        anomaly::PlatformUiPalette::Moss,
        anomaly::PlatformUiPalette::Aurora,
        anomaly::PlatformUiPalette::Ember,
        anomaly::PlatformUiPalette::Paper,
        anomaly::PlatformUiPalette::AnomalyHub,
        anomaly::PlatformUiPalette::Custom,
    };
    // The palette names are the in-game settings' own labels, already localized. The hub
    // palette is a product name in both locales there, and is spelled the same way here.
    const std::array<const char*, palettes.size()> theme_labels{
        Text(translator, anomaly::MessageId::SettingsPaletteNaiwa),
        Text(translator, anomaly::MessageId::SettingsPaletteMoss),
        Text(translator, anomaly::MessageId::SettingsPaletteAurora),
        Text(translator, anomaly::MessageId::SettingsPaletteEmber),
        Text(translator, anomaly::MessageId::SettingsPalettePaper),
        "AnomalyHub",
        Text(translator, anomaly::MessageId::SettingsPaletteCustom)};
    const anomaly::PlatformUiPalette current = snapshot.theme != nullptr
        ? snapshot.theme->palette : anomaly::PlatformUiPalette::Naiwa;
    int selected{};
    for (std::size_t index = 0; index < palettes.size(); ++index) {
        if (palettes[index] == current) {
            selected = static_cast<int>(index);
            break;
        }
    }
    anomaly::glass::SegmentedOptions theme_options;
    theme_options.height = 46.0f;
    theme_options.padding_x = 26.0f;
    theme_options.scale = g_launcher_dpi_scale;
    theme_options.disabled_mask = snapshot.busy ? 0x7fU : 0U;
    if (anomaly::glass::SegmentedControl(
            "launcher-theme", theme_labels, selected, theme_options)) {
        controller.SetLauncherTheme(palettes[static_cast<std::size_t>(selected)]);
    }

    // A palette of this window's own: the same five colours the in-game settings collect, stored with
    // the launcher configuration and applied on the frame they change, so dragging one repaints the
    // window instead of waiting for a reload.
    if (current == anomaly::PlatformUiPalette::Custom) {
        anomaly::PlatformUiCustomColors colors = snapshot.custom_colors;
        bool changed{};
        const auto color_row = [&](const char* id, const anomaly::MessageId label,
                                   anomaly::PlatformUiColor& color) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Text(translator, label));
            ImGui::SameLine(Scale(240.0f));
            ImGui::SetNextItemWidth(Scale(180.0f));
            std::array channels{color.red, color.green, color.blue};
            if (ImGui::ColorEdit3(id, channels.data())) {
                color = {channels[0], channels[1], channels[2], 1.0f};
                changed = true;
            }
        };
        color_row("##custom-accent", anomaly::MessageId::SettingsCustomAccent, colors.accent);
        color_row("##custom-text", anomaly::MessageId::SettingsCustomText, colors.text);
        color_row("##custom-surface", anomaly::MessageId::SettingsCustomWindowBackground,
            colors.window_background);
        color_row("##custom-child", anomaly::MessageId::SettingsCustomChildBackground,
            colors.child_background);
        color_row("##custom-border", anomaly::MessageId::SettingsCustomBorder, colors.border);
        if (changed) controller.SetLauncherCustomColors(colors);
    }

    // The custom background, laid out the way the in-game settings lay theirs out: the label in the
    // section's label column, then the enable switch, the picture picker and the fill mode on that
    // row, and the veil over it on the next. Stickers are not part of this -- the theme layout
    // editor authors those for both windows.
    constexpr int background_disabled =
        static_cast<int>(anomaly::PlatformUiBackgroundMode::Disabled);
    constexpr int background_crop = static_cast<int>(anomaly::PlatformUiBackgroundMode::Crop);
    const int background_mode = snapshot.theme != nullptr
        ? static_cast<int>(snapshot.theme->layout.background.mode) : background_disabled;
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::SettingsBackgroundImage));
    // The switch, the picker and the fill mode are one group, right-aligned on the section's edge, and
    // every slider below spans exactly its width. The group's extent is measured rather than guessed
    // -- the picker's label is localized, so its width is not a constant -- and the measurement is
    // reused next frame, which keeps the rows stable instead of reflowing on every label change.
    static float controls_width = Scale(380.0f);
    const float right_edge = ImGui::GetContentRegionMax().x;
    ImGui::SameLine(right_edge - controls_width - Scale(10.0f));
    const float controls_left = ImGui::GetCursorScreenPos().x;
    bool background_enabled = background_mode != background_disabled;
    if (anomaly::glass::Switch("##launcher-background-enabled", &background_enabled)) {
        controller.SetLauncherBackground(snapshot.artwork.background,
            background_enabled ? (background_mode == background_disabled
                ? background_crop : background_mode) : background_disabled);
    }
    ImGui::SameLine();
    if (LauncherChip((std::string(Text(translator, anomaly::MessageId::SettingsBackgroundOpen)) +
            "##launcher-background-open").c_str())) {
        if (const auto picked = ChooseLauncherThemeImage(window, translator)) {
            controller.SetLauncherBackground(*picked,
                background_mode == background_disabled ? background_crop : background_mode);
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Scale(200.0f));
    const std::array<const char*, 3> background_modes{
        Text(translator, anomaly::MessageId::SettingsBackgroundModeCenter),
        Text(translator, anomaly::MessageId::SettingsBackgroundModeCrop),
        Text(translator, anomaly::MessageId::SettingsBackgroundModeStretch)};
    int background_mode_index = std::clamp(background_mode, 1, 3) - 1;
    if (ImGui::Combo("##launcher-background-mode", &background_mode_index,
            background_modes.data(), static_cast<int>(background_modes.size()))) {
        controller.SetLauncherBackground(
            snapshot.artwork.background, background_mode_index + 1);
    }
    controls_width = ImGui::GetItemRectMax().x - controls_left;
    // The veil over that picture. The slider is the picture's transparency, not the veil's opacity:
    // 100% is a wallpaper you can see, 0% is the plain surface with nothing behind it. The layout
    // stores it the other way round (surfaceOpacity), so the value is flipped here.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::SettingsBackgroundVisibility));
    ImGui::SameLine(right_edge - controls_width - Scale(10.0f));
    // The veil's slider works the way the in-game one does: it shows a value the page holds, and the
    // snapshot only becomes the truth again once the reload it asked for has landed. Committing per
    // frame reloaded the theme (and decoded the wallpaper) under the pointer, and dropping the held
    // value on release showed the old value for a frame -- the bounce.
    static std::optional<float> held_opacity;
    static bool held_committed{true};
    static std::uint64_t held_version{};
    const float applied_opacity = snapshot.theme != nullptr
        ? snapshot.theme->layout.surface_opacity : 1.0f;
    const std::uint64_t theme_version = snapshot.theme != nullptr ? snapshot.theme->version : 0;
    if (held_opacity.has_value() && held_committed && theme_version != held_version) {
        held_opacity.reset();  // the reload reported the value; follow the snapshot again
    }
    int transparency = static_cast<int>(std::lround((1.0f - std::clamp(
        held_opacity.value_or(applied_opacity),
        anomaly::kPlatformUiLayoutSurfaceOpacityMinimum, 1.0f)) * 100.0f));
    ImGui::SetNextItemWidth(controls_width);
    if (anomaly::glass::SliderInt("##launcher-background-visible", &transparency, 0, 100, "%d%%")) {
        held_opacity = 1.0f - static_cast<float>(transparency) / 100.0f;
        held_committed = false;
    }
    // The release test the in-game settings use: the pointer has let go and the slider is no longer
    // active, which is also true when the drag ends outside the track.
    if (!held_committed && held_opacity.has_value() && !ImGui::IsItemActive() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        held_committed = true;
        held_version = theme_version;
        controller.SetLauncherSurfaceOpacity(*held_opacity);
    }

    // Language: the same three choices the in-game settings offer, but this one dresses the
    // launcher alone and applies on the next frame.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionLanguage));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingLanguage));
    constexpr std::array<anomaly::LanguagePreference, 3> languages{
        anomaly::LanguagePreference::Auto,
        anomaly::LanguagePreference::ZhCn,
        anomaly::LanguagePreference::EnUs,
    };
    const std::array<const char*, languages.size()> language_labels{
        Text(translator, anomaly::MessageId::SettingsAutomatic),
        Text(translator, anomaly::MessageId::SettingsSimplifiedChinese),
        Text(translator, anomaly::MessageId::SettingsEnglish)};
    const auto current_language =
        anomaly::ParseLanguagePreference(snapshot.language).preference;
    int language_index{};
    for (std::size_t index = 0; index < languages.size(); ++index) {
        if (languages[index] == current_language) {
            language_index = static_cast<int>(index);
            break;
        }
    }
    anomaly::glass::SegmentedOptions language_options;
    language_options.height = 46.0f;
    language_options.padding_x = 26.0f;
    language_options.scale = g_launcher_dpi_scale;
    if (anomaly::glass::SegmentedControl(
            "launcher-language", language_labels, language_index, language_options)) {
        controller.SetLanguage(languages[static_cast<std::size_t>(language_index)]);
    }

    // The mods page options. The grid is the view it opens with, and pagination is off until
    // it is asked for, so the pager below only exists when this switch is on.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionModsPage));
    // One row: the switch and the page-size choice are the same setting, and the pair sits on the
    // section's right edge like the controls above. The combo's width is fixed; the switch is measured
    // after its first draw so the group's right edge lands exactly on that line.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingPagination));
    static float pagination_switch_width = Scale(52.0f);
    const float page_size_width = Scale(200.0f);
    ImGui::SameLine(right_edge - (pagination_switch_width +
        ImGui::GetStyle().ItemSpacing.x + page_size_width) - Scale(10.0f));
    bool pagination = snapshot.view.mods_pagination;
    if (anomaly::glass::Switch("##launcher-pagination", &pagination)) {
        controller.SetModsView(
            snapshot.view.mods_grid, pagination, snapshot.view.mods_page_size);
    }
    pagination_switch_width = ImGui::GetItemRectSize().x;
    ImGui::SameLine();
    // A page size is a handful of choices rather than a range, so it is a combo: a slider with two
    // hundred stops implied a precision nobody wants, and every stop between them was noise. The
    // control stays live whether or not pagination is on -- using it stores the size and leaves the
    // switch alone.
    const auto& page_sizes = anomaly::launcher::kLauncherPageSizes;
    std::array<std::string, anomaly::launcher::kLauncherPageSizes.size()> page_size_labels;
    std::array<const char*, anomaly::launcher::kLauncherPageSizes.size()> page_size_items{};
    int page_size_selected{};
    for (std::size_t index = 0; index < page_sizes.size(); ++index) {
        page_size_labels[index] = std::to_string(page_sizes[index]);
        page_size_items[index] = page_size_labels[index].c_str();
        if (page_sizes[index] == snapshot.view.mods_page_size)
            page_size_selected = static_cast<int>(index);
    }
    ImGui::SetNextItemWidth(page_size_width);
    if (ImGui::Combo("##mods-page-size", &page_size_selected, page_size_items.data(),
            static_cast<int>(page_size_items.size()))) {
        controller.SetModsView(snapshot.view.mods_grid, snapshot.view.mods_pagination,
            page_sizes[static_cast<std::size_t>(page_size_selected)]);
    }

    DrawStartupSettings(controller, snapshot, translator);
    DrawUpdateSettings(controller, snapshot, translator);
    DrawSupportSettings(translator);

    // The page reports the outcome of its own writes. Everything it can change is saved
    // with the shared scope, so a failure is the only thing that has to be said out
    // loud here; the success line is what tells the user the theme was stored rather
    // than previewed.
    if (snapshot.message_kind != MessageKind::Neutral &&
        snapshot.message_scope == MessageScope::Both) {
        const ImVec4 settings_ink = snapshot.message_kind == MessageKind::Error
            ? ThemeColor(theme.danger)
            : ThemeColor(theme.success);
        const std::string settings_status = RenderMessage(translator, snapshot.message);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(settings_ink, "%s", settings_status.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

// The native caption is gone, so the launcher draws its own: a drag band holding
// the window title, then the minimise and close controls the frame used to own.
// Nothing here paints a background, so the wallpaper and the stickers show
// through the caption exactly as they do behind the rest of the window.
void DrawLauncherTitleBar(Graphics& graphics) {
    const auto& theme = ue5mem::PlatformUiTheme();
    const float height = Scale(64.0f);
    // The marks are narrower than the caption is tall. At the full height the two of
    // them sat a whole caption apart, which read as two unrelated buttons rather than
    // as one pair.
    const float button_width = Scale(44.0f);
    // Everything in the caption sits a couple of pixels below true centre: with a
    // mark this large, an exactly centred band reads as if it is crowding the top
    // edge.
    const float nudge = Scale(3.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    // The app mark and its name belong to the caption now. They are drawn straight
    // onto the window list rather than laid out, so they occupy no space and the
    // drag band still runs the full width behind them.
    {
        ImDrawList* const caption = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float label_x = origin.x + Scale(12.0f);
        if (graphics.logo != nullptr) {
            const float mark = Scale(48.0f);
            caption->AddImage(
                static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(graphics.logo.Get())),
                ImVec2(label_x, origin.y + (height - mark) * 0.5f + nudge),
                ImVec2(label_x + mark, origin.y + (height + mark) * 0.5f + nudge));
            label_x += mark + Scale(12.0f);
        }
        // The caption draws its own size rather than the atlas default, which is
        // baked for body text and reads as too small next to a mark this size.
        const float title_size = Scale(28.0f);
        caption->AddText(ImGui::GetFont(), title_size,
            ImVec2(label_x, origin.y + (height - title_size) * 0.5f + nudge),
            ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.text)),
            "AnomalyLauncher");
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    // No frame at all: the caption floats over the wallpaper, and a box around a
    // button that only shows a glyph is visual noise.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(theme.text));
    // The two caption marks are invisible items, so they draw their own feedback. It is
    // the mark itself that changes -- it turns white under the pointer -- because a
    // filled rectangle behind it reads as a button in a bar that has no other buttons.
    const auto caption_ink = [&theme]() {
        return ImGui::ColorConvertFloat4ToU32(ImGui::IsItemHovered()
            ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
            : ThemeColor(theme.text));
    };
    // The pair sits a little in from the right edge instead of flush against it.
    const float caption_margin = Scale(10.0f);
    ImGui::InvisibleButton("##launcher-drag",
        ImVec2((std::max)(1.0f, width - button_width * 2.0f - caption_margin), height));
    // The window is moved by hand instead of handing the press to the system drag
    // loop. WM_NCLBUTTONDOWN runs a modal loop that swallows the matching release,
    // which leaves ImGui holding a pressed item forever and silently kills every
    // button in the window from then on.
    //
    // The maths is deliberately absolute rather than per-frame: accumulating
    // MouseDelta feeds the window's own movement back into the next delta, since
    // moving the window moves the cursor relative to it, and the result jitters.
    // The screen cursor and the window origin captured at drag start have no such
    // feedback.
    // Drag state kept between frames: the screen cursor and the window origin
    // captured when the drag started, so the position is absolute rather than
    // accumulated frame by frame.
    static bool drag_active = false;
    static POINT drag_cursor{};
    static POINT drag_window{};
    if (ImGui::IsItemActivated()) {
        drag_cursor = POINT{};
        drag_window = POINT{};
        if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
            if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                RECT rect{};
                drag_active = GetCursorPos(&drag_cursor) != FALSE
                    && GetWindowRect(handle, &rect) != FALSE;
                if (drag_active) {
                    drag_window.x = rect.left;
                    drag_window.y = rect.top;
                }
            }
        }
    }
    if (drag_active && ImGui::IsItemActive()) {
        POINT cursor{};
        if (GetCursorPos(&cursor) != FALSE) {
            if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
                if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                    static_cast<void>(SetWindowPos(handle, nullptr,
                        drag_window.x + (cursor.x - drag_cursor.x),
                        drag_window.y + (cursor.y - drag_cursor.y),
                        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
                }
            }
        }
    }
    if (!ImGui::IsItemActive()) drag_active = false;
    ImGui::SameLine();
    ImGui::InvisibleButton("##launcher-minimize", ImVec2(button_width, height));
    const ImVec2 minimize_min = ImGui::GetItemRectMin();
    const ImVec2 minimize_max = ImGui::GetItemRectMax();
    const ImVec2 minimize_center((minimize_min.x + minimize_max.x) * 0.5f,
        (minimize_min.y + minimize_max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(minimize_center.x - Scale(9.0f), minimize_center.y),
        ImVec2(minimize_center.x + Scale(9.0f), minimize_center.y),
        caption_ink(), Scale(2.4f));
    if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered()) {
        if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
            if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                static_cast<void>(ShowWindow(handle, SW_MINIMIZE));
            }
        }
    }
    ImGui::SameLine();
    ImGui::InvisibleButton("##launcher-close", ImVec2(button_width, height));
    const ImVec2 close_min = ImGui::GetItemRectMin();
    const ImVec2 close_max = ImGui::GetItemRectMax();
    const ImVec2 close_center((close_min.x + close_max.x) * 0.5f,
        (close_min.y + close_max.y) * 0.5f);
    const ImU32 close_ink = caption_ink();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(close_center.x - Scale(9.0f), close_center.y - Scale(9.0f)),
        ImVec2(close_center.x + Scale(9.0f), close_center.y + Scale(9.0f)),
        close_ink, Scale(2.4f));
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(close_center.x - Scale(9.0f), close_center.y + Scale(9.0f)),
        ImVec2(close_center.x + Scale(9.0f), close_center.y - Scale(9.0f)),
        close_ink, Scale(2.4f));
    if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered()) {
        if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
            if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                static_cast<void>(PostMessageW(handle, WM_CLOSE, 0, 0));
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

void DrawLauncher(
    HWND window, Graphics& graphics, LauncherController& controller, LauncherMode& mode) {
    // Runs every frame whatever page is showing: a game that starts after this window is up has to appear
    // in the list without the user pressing anything, and the poll throttles itself to one scan every couple
    // of seconds on the worker thread.
    controller.PollProcesses();
    // A preference the user changed is written once the worker is free, so the settings page
    // needs no save button and a slider drag costs one write.
    controller.FlushSettings();
    // The update check this launch owes, if it owes one: it runs on the worker and reports into
    // the settings page, so nothing pops up over the page the user is reading.
    controller.AutoCheckUpdates();
    const LauncherSnapshot snapshot = controller.Snapshot();
    // The update has replaced the image this window was drawn by and started the new one. This
    // window closes rather than outliving the file it was loaded from, and so that the two
    // launchers never write the same configuration at the same time.
    if (snapshot.restart_pending) {
        static bool close_posted{};
        if (!close_posted) {
            close_posted = true;
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
    // The language is applied here rather than at start-up only: switching it on the settings page
    // repaints this frame, and the window title follows. The reference below is taken after the
    // swap, so every page draws with the catalog that is in hand.
    {
        const auto wanted = anomaly::ParseLanguagePreference(snapshot.language).preference;
        if (wanted != g_translator_language &&
            LoadLauncherCatalog(wanted, ExecutableDirectory() / L"Anomaly" / L"locales" / L"host")) {
            SetWindowTextW(window,
                Utf8Wide(g_translator.load()->Text(anomaly::MessageId::LauncherWindowTitle)).c_str());
        }
    }
    const auto frame_translator = g_translator.load();
    const anomaly::Translator& translator = *frame_translator;
    ApplyLauncherTheme(snapshot.theme, snapshot.custom_colors);
    UploadLauncherTheme(graphics, snapshot.theme);
    const ImGuiIO& io = ImGui::GetIO();
    // The root window is the shell canvas, pushed down by the transparent band above it: the band is
    // window, not canvas, so a sticker drawn past the top edge lands there instead of being cut.
    const float band_top = Scale(kLauncherBandTop);
    ImGui::SetNextWindowPos(ImVec2(0.0f, band_top));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - band_top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(
        "AnomalyLauncherRoot", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    const ImVec2 root_origin = ImGui::GetWindowPos();
    const ImVec2 root_size = ImGui::GetWindowSize();
    DrawLauncherThemeImages(graphics, snapshot.theme, root_origin, root_size, false);
    anomaly::glass::CaptureBackdrop(ImGui::GetWindowDrawList());
    DrawLauncherTitleBar(graphics);
    DrawModes(controller, snapshot, mode, translator);
    // The body is one frosted card inset from the canvas edge, so the wallpaper
    // and the stickers stay visible around it and refract through it.
    {
        const ImVec2 card_min(root_origin.x + Scale(14.0f), ImGui::GetCursorScreenPos().y);
        const ImVec2 card_max(root_origin.x + root_size.x - Scale(14.0f),
            root_origin.y + root_size.y - Scale(14.0f));
        anomaly::glass::SurfaceOptions card;
        card.variant = anomaly::glass::Variant::Regular;
        card.rounding = Scale(20.0f);
        card.scale = g_launcher_dpi_scale;
        // The pane is the one surface that refracts the wallpaper, so it keeps the lens; its glow is
        // switched off, because a glow around a shape this large only reads as a box in the corner.
        card.shadow = false;
        anomaly::glass::DrawSurface(ImGui::GetWindowDrawList(), card_min, card_max, card);
        ImGui::SetCursorScreenPos(card_min);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(20.0f, 18.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "LauncherBody", ImVec2(root_size.x - Scale(28.0f), -Scale(14.0f)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (mode == LauncherMode::Launch) {
        DrawLaunchMode(window, controller, snapshot, translator);
    } else if (mode == LauncherMode::Mods) {
        DrawModMode(window, controller, snapshot, translator);
    } else {
        DrawSettingsMode(window, controller, snapshot, translator);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    DrawLauncherThemeImages(graphics, snapshot.theme, root_origin, root_size, true);
    // The update prompt is drawn last, on the root window: an update check runs in the background, so
    // the question it ends in has to reach the user on whatever page they are reading -- and drawing it
    // after the page keeps the dialog the last window created in the frame, which is what puts it in
    // front of the page's own child windows instead of behind them.
    DrawUpdatePrompt(controller, snapshot, translator);
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const auto administrator = EnsureAdministrator(command_line);
    if (!administrator.run_current_process) return administrator.exit_code;

    int argument_count{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    DWORD parent_process{};
    if (arguments != nullptr) {
        if (argument_count == 3 && std::wstring_view(arguments[1]) == L"--wait-for-update") {
            wchar_t* end{};
            parent_process = wcstoul(arguments[2], &end, 10);
            if (end == arguments[2] || *end != L'\0' || parent_process == GetCurrentProcessId()) {
                LocalFree(arguments);
                return ERROR_INVALID_PARAMETER;
            }
        }
        LocalFree(arguments);
    }
    if (parent_process != 0) {
        const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_process);
        if (parent != nullptr) {
            const DWORD waited = WaitForSingleObject(parent, 60000);
            CloseHandle(parent);
            if (waited != WAIT_OBJECT_0) return ERROR_TIMEOUT;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
            return static_cast<int>(GetLastError());
        }
    }

    // The release directory also contains the proxy payload named dwmapi.dll.
    // Pin the system module before ImGui's delay imports are resolved.
    const HMODULE system_dwmapi = LoadSystemDwmapi();
    if (system_dwmapi == nullptr) return static_cast<int>(GetLastError());

    const std::filesystem::path runtime_root = ExecutableDirectory() / L"Anomaly";
    // The launcher's own settings live in this ini: it is the file this process reads at start-up,
    // so a language or hotkey change written here is what the next launch sees.
    const std::filesystem::path settings_path = runtime_root / L"anomaly.ini";
    const std::filesystem::path locale_directory = runtime_root / L"locales" / L"host";
    const ue5mem::AnalyzerConfig config = ue5mem::AnalyzerConfig::Load(settings_path);
    if (!LoadLauncherCatalog(config.platform_language, locale_directory)) {
        return ERROR_RESOURCE_DATA_NOT_FOUND;
    }
    // The category names are data, not translated text, so they are read once and do not change with
    // the language: the table holds every language and the page picks one at draw time.
    LoadLauncherModGroupNames();

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    WNDCLASSEXW window_class{
        .cbSize = sizeof(window_class),
        .style = CS_CLASSDC,
        .lpfnWndProc = WindowProc,
        .hInstance = instance,
        .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = L"AnomalyLauncherWindow",
        .hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
    };
    if (RegisterClassExW(&window_class) == 0) return 1;
    const std::wstring window_title = Utf8Wide(
        g_translator.load()->Text(anomaly::MessageId::LauncherWindowTitle));
    g_launcher_dpi_scale = DpiScale(GetDpiForSystem());
    // WS_OVERLAPPEDWINDOW cannot be used here: it carries a caption, and removing
    // the frame from the client area with WM_NCCALCSIZE does not stop the window
    // from owning one, so the native title bar comes straight back. A popup has no
    // caption to begin with; WS_EX_APPWINDOW keeps the taskbar entry that a plain
    // popup would lose, and the placement is computed because a popup ignores
    // CW_USEDEFAULT.
    // The window is the shell canvas plus the transparent band above it, where the launcher's own
    // sticker overhangs; the bands beside it are gone, so the width is the canvas width.
    const int window_width = static_cast<int>(Scale(kLauncherCanvasWidth));
    const int window_height = static_cast<int>(Scale(kLauncherCanvasHeight) +
        Scale(kLauncherBandTop));
    const int window_x = (GetSystemMetrics(SM_CXSCREEN) - window_width) / 2;
    const int window_y = (GetSystemMetrics(SM_CYSCREEN) - window_height) / 2;
    // WS_EX_NOREDIRECTIONBITMAP stops the window from painting a background of its
    // own, so the composition visual is the only thing that reaches the screen and
    // whatever the shell leaves untouched stays transparent, which is how the window's
    // rounded corners are made.
    const HWND window = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        window_class.lpszClassName, window_title.c_str(),
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        window_x, window_y, window_width, window_height,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(window_class.lpszClassName, instance);
        return 2;
    }

    Graphics graphics;
    g_graphics = &graphics;
    if (!CreateGraphics(window, graphics)) {
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 3;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // The launcher owns its whole surface, so it bakes only what it draws: one type
    // scale, its eight icons, an atlas no taller than the glyphs need, and none of the
    // symbol blocks plugin copy draws from. The host keeps its own ladder of sizes, the
    // whole icon range and those blocks for the in-game windows.
    static_cast<void>(ue5mem::ConfigurePlatformUiFontAtlas(runtime_root,
        ue5mem::PlatformUiFontBake{
            .scales = kLauncherFontScales, .icons = kLauncherIcons, .tight_atlas = true,
            .symbol_blocks = false}));
    g_launcher_dpi_scale = DpiScale(GetDpiForWindow(window));
    ApplyLauncherDpiScale();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAuto = false;
    io.ConfigNavEscapeClearFocusWindow = true;
    io.IniFilename = nullptr;
    if (!ImGui_ImplWin32_Init(window) ||
        !ImGui_ImplDX11_Init(graphics.device.Get(), graphics.context.Get())) {
        ImGui::DestroyContext();
        graphics = {};
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 4;
    }
    // Losing the glass renderer only costs the effect; every surface keeps a
    // solid fallback.
    // The launcher is a desktop window, not an overlay: its composition carries
    // real alpha, and its material stays neutral rather than palette-tinted.
    static_cast<void>(anomaly::glass::InitializeRenderer(
        graphics.device.Get(), graphics.context.Get(), false));
    // Upload the font atlas now rather than on the first frame, so what it costs
    // besides its texture can be released before the window is shown. The launcher
    // owns its device for its lifetime and never adds another font, so it keeps
    // neither the texture pixels nor the font files.
    static_cast<void>(ImGui_ImplDX11_CreateDeviceObjects());
    ue5mem::ReleasePlatformUiFontAtlas(ue5mem::PlatformUiFontRetention::Minimal);
    ApplyLauncherDpiScale();
    graphics.logo = LoadEmbeddedTexture(graphics, kLogoResourceId);
    graphics.mod_placeholder = LoadEmbeddedTexture(graphics, kModPlaceholderResourceId,
        &graphics.mod_placeholder_width, &graphics.mod_placeholder_height);
    graphics.sponsor = LoadEmbeddedTexture(graphics, kSponsorResourceId,
        &graphics.sponsor_width, &graphics.sponsor_height);
    graphics.kofi = LoadEmbeddedTexture(graphics, kKofiResourceId,
        &graphics.kofi_width, &graphics.kofi_height);
    LauncherController controller(ExecutableDirectory());
    LauncherMode mode = LauncherMode::Launch;

    bool running = true;
    bool window_shown = false;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (g_launcher_dpi_changed) ApplyLauncherDpiScale();
        if (IsIconic(window)) {
            Sleep(16);
            continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawLauncher(window, graphics, controller, mode);
        ImGui::Render();
        // What the shell leaves untouched has to be fully transparent -- and under premultiplied
        // alpha that means colour zero as well, or the rounded corners would composite as a tinted
        // margin.
        graphics.context->OMSetRenderTargets(1, graphics.render_target.GetAddressOf(), nullptr);
        const float transparent_clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
        graphics.context->ClearRenderTargetView(graphics.render_target.Get(), transparent_clear);
        anomaly::glass::PrepareDrawData(ImGui::GetDrawData());
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        graphics.swap_chain->Present(1, 0);
        if (!window_shown) {
            // The window stays hidden until a themed frame has been presented. It has no
            // redirection bitmap, so until the composition visual holds a frame the
            // desktop is looking at an empty window; and the theme arrives from a worker
            // thread, so the earliest frames are drawn against the palette's defaults.
            // Showing at that point is the flash that used to come before the launcher.
            // A theme that never arrives is not a reason to stay hidden, so a snapshot
            // without one shows the window too.
            const LauncherSnapshot frame = controller.Snapshot();
            if (frame.theme == nullptr || graphics.theme_version != 0) {
                window_shown = true;
                ShowWindow(window, SW_SHOWDEFAULT);
            }
        }
    }

    anomaly::glass::ShutdownRenderer();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_graphics = nullptr;
    graphics = {};
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
