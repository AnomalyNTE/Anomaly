#include "anomaly/liquid_glass.hpp"
#include "anomaly/launcher/manual_map.hpp"
#include "anomaly/launcher/mod_archive.hpp"
#include "anomaly/launcher/configuration.hpp"
#include "anomaly/launcher/proxy_installation.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/platform_ui_layout.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/runtime_launch.hpp"
#include "anomaly/reliable_storage.hpp"
#include "anomaly/pak_mod_settings.hpp"
#include "anomaly/pak_mod_catalog.hpp"
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
constexpr int kIconResourceId = 201;
// Sized for the launcher's 17px font. These were laid out for 13px text, and a
// child that cannot fit its own contents grows a scrollbar instead of clipping,
// which is what a mode row of 48px did to a 30px button plus its padding.
// The launcher draws on a light themed surface with a wallpaper and its veil
// behind it, where the palette's muted ink is too pale to read. Every secondary
// label here uses this tone instead, which is also what ImGuiCol_TextDisabled is
// set to, so disabled and secondary text stay consistent.
const ImVec4 kLauncherMutedInk(0.24f, 0.21f, 0.15f, 1.0f);
constexpr float kHeaderHeight = 44.0f;
// The transparent bands beside the shell canvas, in canvas pixels. Both Naiwa
// stickers overhang the canvas horizontally, and each side has to reach the furthest
// *visible* pixel of the sticker on that side.
//
// That is deliberately not the placement's own edge: both PNGs carry a wide
// transparent margin, so the artwork stops well inside the rectangle.
//
//   naiwa-01  not flipped. 592.95 wide from canvas x 800.93; its opaque columns are
//             189..759 of 978, so the visible right edge lands 81.10 past the canvas
//             even though the placement itself reaches 1393.88.
//   naiwa-02  flipped horizontally, which is what makes it the wider side. 537.61
//             wide from canvas x -115.40; its opaque columns are 67..1043 of 1044,
//             and the flip turns those into 0..976, so the visible left edge is the
//             placement's own edge: 115.40 past the canvas.
//
// The two sides genuinely differ, so a single shared band would either clip the left
// sticker or leave 34 px of empty margin on the right.
constexpr float kLauncherBandLeft = 115.4f;
constexpr float kLauncherBandRight = 81.1f;
constexpr float kModeHeight = 68.0f;
constexpr float kProxyActionLeftPadding = 4.0f;
constexpr float kLauncherFontScale = 20.0f / 13.0f;
constexpr float kDefaultDpi = 96.0f;

ImVec4 ThemeColor(const ue5mem::PlatformUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

ImVec4 ThemeColorWithAlpha(
    const ue5mem::PlatformUiColor& color, const float alpha) noexcept {
    return {color.red, color.green, color.blue, alpha};
}

enum class LauncherMode : std::uint8_t { Proxy, Attach, Mods, Browse };
enum class MessageKind : std::uint8_t { Neutral, Success, Error };

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

// Everything the launcher needs to dress itself: the palette chosen in the
// in-game settings, plus the background and stickers from config/themes.
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
    bool busy{};
    LauncherMessage message;
    MessageKind message_kind{MessageKind::Neutral};
    MessageScope message_scope{MessageScope::Both};
    std::shared_ptr<const LauncherTheme> theme;
};

// The launcher wears the Naiwa preset and nothing else. It deliberately does not
// follow the palette chosen in the in-game settings: the launcher is the first
// thing a user sees, so it keeps one fixed identity instead of inheriting
// whichever palette -- or custom colour set -- the current session happens to
// use. Resolving the layout for that palette is what picks
// config/themes/naiwa.json, the theme's own preset.
std::shared_ptr<const LauncherTheme> LoadLauncherTheme(
    const std::filesystem::path& runtime_root, const std::uint64_t version) {
    auto theme = std::make_shared<LauncherTheme>();
    theme->version = version;
    theme->palette = anomaly::PlatformUiPalette::Naiwa;
    try {
        const auto read = anomaly::ReadUiResourceBytes(
            anomaly::ResolvePlatformUiLayoutFile(
                runtime_root, anomaly::ToString(theme->palette)),
            anomaly::kPlatformUiLayoutMaximumBytes);
        if (!read) return theme;
        const std::string text(
            reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
        auto parsed = anomaly::ParsePlatformUiLayout(text);
        if (!parsed.ok) return theme;
        theme->layout = std::move(parsed.layout);

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

// The hook that goes into the official launcher. It is built with this executable and ships in the
// runtime payload directory. The path is absolute because the launcher it is injected into runs from
// an install location this process does not own.
std::filesystem::path GameHookLibrary() {
    return ExecutableDirectory() / L"Anomaly" / L"NTEGameHook.dll";
}

// Auto-starting the official client is off. The launcher is started the way its own shortcut starts
// it, and the player presses start game in the window that opens, so capture waits for the HTGame.exe
// that press produces. Everything auto-starting needs -- resolving the client out of the bootstrap's
// update section, injecting the hook, and the hook itself -- is kept behind this switch, still
// compiled and still shipped, so turning it back on is this one line.
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
// available as well as visible.
constexpr std::array<char32_t, 8> kLauncherIcons{
    0xe838, 0xe72c, 0xe768, 0xe73e, 0xe7ba, 0xe711, 0xe8b7, 0xe8e5};

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
        worker_.request_stop();
        queue_changed_.notify_all();
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
            const auto launcher = LauncherExecutable();
            const auto selected = ResolveAttachRuntime();
            if (!selected.Ok()) {
                PublishRuntimeFailure(selected);
                return;
            }
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

private:
    using Work = std::function<void()>;

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

    [[nodiscard]] std::filesystem::path RuntimeSettingsRoot() const {
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            const auto installed = game_directory / L"Anomaly";
            std::error_code error;
            if (std::filesystem::is_regular_file(
                    installed / L"Anomaly.Core.dll", error) && !error) {
                return installed;
            }
        }
        return source_.runtime_directory;
    }

    void RefreshHotkeyImpl() {
        const auto root = RuntimeSettingsRoot();
        const auto config = ue5mem::AnalyzerConfig::Load(root / L"anomaly.ini");
        std::scoped_lock lock(state_mutex_);
        state_.toggle_key = config.platform_toggle_key;
    }

    [[nodiscard]] bool SaveToggleKeyImpl(const std::uint32_t key) const {
        const std::wstring value = std::to_wstring(key);
        return WritePrivateProfileStringW(
            L"Platform", L"ToggleKey", value.c_str(),
            (RuntimeSettingsRoot() / L"anomaly.ini").c_str()) != FALSE;
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
        const auto discover = [this, &game_processes, &configuration](
                                  const anomaly::launcher::NteClient client,
                                  const auto& launcher_processes) {
            anomaly::launcher::LauncherDiscoveryOptions options;
            options.client = client;
            options.payload_root = payload_root_;
            options.allow_unpaired_game_discovery =
                configuration.selected_client == client;
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
        {
            std::scoped_lock lock(state_mutex_);
            configuration_ = std::move(configuration);
            const auto& selected = configuration_.Selected();
            state_.selected_client = configuration_.selected_client;
            state_.game_directory = selected.game_directory;
            state_.launcher_executable = selected.launcher_executable;
        }
        const auto saved = PersistConfigurationImpl();
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
        anomaly::launcher::NteClient client{};
        {
            std::scoped_lock lock(state_mutex_);
            preferred.game_directory = state_.game_directory;
            preferred.launcher_executable = state_.launcher_executable;
            client = configuration_.selected_client;
        }
        anomaly::launcher::LauncherDiscoveryOptions options;
        options.client = client;
        options.payload_root = payload_root_;
        options.allow_unpaired_game_discovery = true;
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

    void RefreshThemeImpl() {
        // The launcher reads the theme next to itself, exactly like its locales:
        // payload_root_ points at wherever the payload is installed, which is not
        // necessarily where this launcher keeps config/themes and assets.
        auto theme = LoadLauncherTheme(
            ExecutableDirectory() / L"Anomaly", theme_version_ + 1);
        ++theme_version_;
        std::scoped_lock lock(state_mutex_);
        state_.theme = std::move(theme);
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
    std::mutex queue_mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Work> queue_;
    // Throttles PollProcesses: a per-frame caller must not turn into a per-frame process scan.
    std::chrono::steady_clock::time_point last_process_poll_{};
    std::jthread worker_;
};

struct Graphics final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    // The window is created without a redirection bitmap, so it draws nothing of its
    // own: the swap chain is bound to a composition visual, and that visual is the
    // window's content. The indirection is what lets the back buffer's alpha reach
    // the desktop, which is what the transparent side bands are made of.
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<IDCompositionDevice> composition_device;
    ComPtr<IDCompositionTarget> composition_target;
    ComPtr<IDCompositionVisual> composition_visual;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11ShaderResourceView> logo;
    ComPtr<ID3D11ShaderResourceView> mod_placeholder;
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

void ApplyLauncherDpiScale() noexcept {
    ue5mem::ApplyPlatformUiStyle();
    ImGui::GetStyle().ScaleAllSizes(g_launcher_dpi_scale);
    static_cast<void>(ue5mem::ApplyPlatformUiFontScale(
        g_launcher_dpi_scale * kLauncherFontScale));
    // The launcher sits on a light themed surface with a wallpaper and its veil
    // behind it, which washes out every ink tone the palette offers. Plain black
    // keeps a label readable over any of them, and the muted tone stays dark
    // enough to read as secondary rather than as disabled.
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = kLauncherMutedInk;
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
    // and an alpha channel is the entire point of the window's side bands.
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

ComPtr<ID3D11ShaderResourceView> CreateTextureFromThemeImage(
    Graphics& graphics, const LauncherThemeImage& image, std::uint32_t& width,
    std::uint32_t& height) {
    width = 0;
    height = 0;
    auto view = CreateTextureFromPixels(
        graphics, image.pixels, image.width, image.height);
    if (view != nullptr) {
        width = image.width;
        height = image.height;
    }
    return view;
}

ComPtr<ID3D11ShaderResourceView> LoadEmbeddedTexture(Graphics& graphics, const int resource_id) {
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
    return CreateTextureFromPixels(graphics, decoded.image.pixels,
        decoded.image.width, decoded.image.height);
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
// only has to be authored once.
void ApplyLauncherTheme(const std::shared_ptr<const LauncherTheme>& theme) {
    if (theme == nullptr) return;
    // The preset is fixed, so the custom-colour store is never consulted here.
    ue5mem::SetPlatformUiPalette(theme->palette);
    ue5mem::SetPlatformUiSurfaceAlpha(
        theme->HasBackground() ? theme->layout.surface_opacity : 1.0f);
    ApplyLauncherDpiScale();
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
    // The wallpaper and the veil are the shell's own body and stop at its edges: the
    // bands beside it are transparent margin, and painting either of them there would
    // fill those bands in.
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
    // Stickers, unlike the body, are meant to spill: the root window's own clip rect
    // is inset by its padding, which would slice off whatever lies past the canvas
    // and leave the plain surface showing instead. This is what carries them into the
    // transparent bands.
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
const anomaly::Translator* g_translator{};

std::string Num(const double value, const int precision) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

std::string Tr(const anomaly::MessageId id) {
    return g_translator != nullptr ? std::string(g_translator->Text(id)) : std::string{};
}

std::string Tr(const anomaly::MessageId id, std::initializer_list<std::string_view> arguments) {
    if (g_translator == nullptr) return {};
    return g_translator->Format(id, std::span<const std::string_view>(arguments.begin(),
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

bool IconButton(const char* id, char32_t glyph, const char* tooltip, bool enabled = true) {
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const float extent = ButtonHeight(38.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, extent * 0.36f);
    const bool pressed = ImGui::Button("##icon", ImVec2(extent, extent));
    ImGui::PopStyleVar();
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
        ImGui::GetColorU32(ImGuiCol_Text), mark);
    ImGui::EndDisabled();
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
        text = Ellipsize(text,
            (std::max)(0.0f, scaled_size.x - ImGui::GetStyle().FramePadding.x * 2.0f));
    }
    const auto& theme = ue5mem::PlatformUiTheme();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button,
        primary ? ThemeColor(theme.accent) : ImVec4(0.0f, 0.0f, 0.0f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        primary ? ThemeColor(theme.accent_hovered) : ImVec4(0.0f, 0.0f, 0.0f, 0.11f));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        primary ? ThemeColor(theme.accent_active) : ImVec4(0.0f, 0.0f, 0.0f, 0.16f));
    // Plain black on the light themed surface. The palette's muted ink is tuned
    // for the shell's own panels; over a wallpaper and its veil it reads as washed
    // out to the point of being unreadable.
    ImGui::PushStyleColor(ImGuiCol_Text, primary
        ? ThemeColor(theme.inverse_text) : ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, scaled_size.y * 0.36f);
    const bool pressed = ImGui::Button(text.c_str(), scaled_size);
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return pressed && enabled;
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
    case State::NotInstalled: return kLauncherMutedInk;
    case State::Conflict:
    case State::Unavailable: return ThemeColor(theme.danger);
    }
    return kLauncherMutedInk;
}

void DrawModes(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    LauncherMode& mode, const anomaly::Translator& translator) {
    const bool proxy_enabled = snapshot.proxy.state ==
        anomaly::launcher::ProxyInstallationState::Enabled;
    if (proxy_enabled && mode == LauncherMode::Attach) {
        mode = LauncherMode::Proxy;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "LauncherModes", ImVec2(0.0f, Scale(kModeHeight)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    const std::array<const char*, 4> labels{
        Text(translator, anomaly::MessageId::LauncherModeProxyInstall),
        Text(translator, anomaly::MessageId::LauncherModeLiveAttach),
        Text(translator, anomaly::MessageId::LauncherModeMods),
        Text(translator, anomaly::MessageId::LauncherModeBrowse)};
    anomaly::glass::SegmentedOptions options;
    options.height = 46.0f;
    options.padding_x = 30.0f;
    options.scale = g_launcher_dpi_scale;
    options.disabled_mask = snapshot.busy || proxy_enabled ? 0b10U : 0U;
    int selected = mode == LauncherMode::Attach ? 1
        : mode == LauncherMode::Mods ? 2
        : mode == LauncherMode::Browse ? 3 : 0;
    if (anomaly::glass::SegmentedControl("launcher-modes", labels, selected, options)) {
        mode = selected == 1 ? LauncherMode::Attach
            : selected == 2 ? LauncherMode::Mods
            : selected == 3 ? LauncherMode::Browse : LauncherMode::Proxy;
    }
    // The client sits right beside the mode, on the same band: it decides which game directory the process
    // list, the proxy install and the launch all read from, and the two installs live in different
    // directories -- a game under the client that is not selected is not in the list at all. The labels are
    // the clients' own names in both locales, so they are not translated.
    ImGui::SameLine(0.0f, Scale(12.0f));
    const std::array<const char*, 2> clients{"CN", "Global"};
    anomaly::glass::SegmentedOptions client_options;
    client_options.height = 46.0f;
    client_options.padding_x = 20.0f;
    client_options.scale = g_launcher_dpi_scale;
    client_options.disabled_mask = snapshot.busy ? 0b11U : 0U;
    int client = snapshot.selected_client == anomaly::launcher::NteClient::Global ? 1 : 0;
    if (anomaly::glass::SegmentedControl("launcher-client", clients, client, client_options)) {
        controller.SelectClient(client == 1
            ? anomaly::launcher::NteClient::Global
            : anomaly::launcher::NteClient::MainlandChina);
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
    if (ImGui::Button(label.c_str(), ImVec2(Scale(180.0f), ButtonHeight(38.0f)))) {
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
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    using State = anomaly::launcher::ProxyInstallationState;

    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionGameDirectory));
    DrawReadOnlyPath("##GameDirectory", snapshot.game_directory, 38.0f, translator);
    ImGui::SameLine();
    if (IconButton("choose-game-directory", 0xe838,
            Text(translator, anomaly::MessageId::LauncherChooseGameDirectory),
            !snapshot.busy)) {
        if (const auto selected = ChooseGameDirectory(
                window, snapshot.game_directory, translator)) {
            controller.SelectGameDirectory(*selected);
        }
    }

    // Both modes put the startup settings directly under the path they apply to,
    // and their own status block below that.
    DrawStartupSettings(controller, snapshot, translator);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
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
        ImGui::TextColored(kLauncherMutedInk, "%s", Text(
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

    // Pinned to a fixed band rather than derived from the cursor: taking the
    // larger of the two made each mode's button land at a different height,
    // because the content above them differs in length. This is the same band the
    // attach mode uses, so the primary action never moves between modes.
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - Scale(74.0f));
    // The primary action is the last thing anyone reaches for, so it sits in the
    // bottom-right corner at the end of the row rather than at the left edge.
    const float action_width = Scale(240.0f);
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
        ImGui::GetWindowWidth() - action_width - Scale(16.0f)));
    const auto action = anomaly::launcher::ProxyInstallationActionForState(
        snapshot.proxy.state);
    bool has_action = true;
    bool action_pressed{};
    switch (action) {
    case anomaly::launcher::ProxyInstallationAction::Install:
        action_pressed = CommandButton(
            "install-proxy", 0xe8b7,
            Text(translator, anomaly::MessageId::CommonInstall), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(240.0f, 62.0f));
        break;
    case anomaly::launcher::ProxyInstallationAction::Enable:
        action_pressed = CommandButton(
            "enable-proxy", 0xe768,
            Text(translator, anomaly::MessageId::CommonEnable), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(240.0f, 62.0f));
        break;
    case anomaly::launcher::ProxyInstallationAction::Disable:
        action_pressed = CommandButton(
            "disable-proxy", 0xe711,
            Text(translator, anomaly::MessageId::CommonDisable), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(240.0f, 62.0f));
        break;
    case anomaly::launcher::ProxyInstallationAction::Update:
        action_pressed = CommandButton(
            "update-proxy", 0xe8b7,
            Text(translator, anomaly::MessageId::CommonUpdate), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(240.0f, 62.0f));
        break;
    case anomaly::launcher::ProxyInstallationAction::None:
        has_action = false;
        break;
    }
    if (action_pressed) {
        if (action == anomaly::launcher::ProxyInstallationAction::Install ||
            action == anomaly::launcher::ProxyInstallationAction::Update) {
            controller.InstallProxy();
        } else {
            controller.SetProxyEnabled(
                action == anomaly::launcher::ProxyInstallationAction::Enable);
        }
    }
    if (has_action) static_cast<void>(0);
}

// ---------------------------------------------------------------------------
// Hot selections change configuration; compatibility selections change filenames before startup.
struct LauncherModEntry final {
    std::string name;          // folder name or loose-file base name
    std::string key;           // exact catalog key, including a loose container's _P suffix
    std::string display_name;  // mod.json "name" when present
    std::string author;
    std::string version;
    std::string link;          // mod.json "modLink" / "link" / "supportLink"
    std::string group;         // mod.json "character" / "category"
    std::string custom_name;   // user override stored in mod-notes.json
    std::string custom_group;
    std::string notes;         // free-form note stored in mod-notes.json
    std::string remote_version;  // filled by the GameBanana update check
    std::filesystem::path path;
    std::filesystem::path icon_path;  // package directory for source metadata
    std::filesystem::path cover;
    std::filesystem::file_time_type cover_stamp{};
    bool directory{};
    bool enabled{true};
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

void DrawLauncherModCover(const LauncherModEntry& entry) {
    const ImVec2 cover_size(Scale(160.0f), Scale(90.0f));
    ImGui::Dummy(cover_size);
    if (!ImGui::IsItemVisible() || g_graphics == nullptr) return;
    auto* texture = g_graphics->mod_placeholder.Get();
    ImVec2 image_size = cover_size;
    if (const auto* icon = LauncherModIcon(entry); icon != nullptr && icon->texture != nullptr) {
        texture = icon->texture.Get();
        const float scale = (std::min)(cover_size.x / icon->width, cover_size.y / icon->height);
        image_size = ImVec2(icon->width * scale, icon->height * scale);
    }
    if (texture == nullptr) return;
    const ImVec2 slot = ImGui::GetItemRectMin();
    const ImVec2 top_left(slot.x + (cover_size.x - image_size.x) * 0.5f,
        slot.y + (cover_size.y - image_size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture), top_left,
        ImVec2(top_left.x + image_size.x, top_left.y + image_size.y));
}




std::wstring LauncherWiden(const std::string& utf8);

constexpr wchar_t kLauncherModSourceFile[] = L"anomaly-source.json";

struct LauncherModSource final {
    std::string mod_id;
    std::string file_id;
    std::string file_md5;
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
            source.value("fileMd5", std::string{})};
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


// Small UI state that should survive restarts: sort order, category, filters, view mode.
std::filesystem::path LauncherPrefsPath() {
    return ExecutableDirectory() / L"Anomaly" / L"config" / L"launcher-prefs.json";
}

nlohmann::json LauncherLoadPrefs() {
    std::ifstream stream(LauncherPrefsPath());
    if (!stream) return nlohmann::json::object();
    auto parsed = nlohmann::json::parse(stream, nullptr, false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

void LauncherSavePref(const std::string& key, const nlohmann::json& value) {
    auto prefs = LauncherLoadPrefs();
    prefs[key] = value;
    std::error_code error;
    std::filesystem::create_directories(LauncherPrefsPath().parent_path(), error);
    std::ofstream(LauncherPrefsPath()) << prefs.dump(2) << "\n";
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
    entry.group = !character.empty() ? character : category;
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
    std::vector<Loose> loose;
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = LauncherNarrow(item.path().filename());
        if (item.is_directory(error)) {
            LauncherModEntry entry{};
            entry.name = name;
            entry.display_name = LauncherStripDisabled(name);
            entry.path = item.path();
            entry.directory = true;
            entry.enabled = LauncherStripDisabled(name) == name;
            {
                // A folder counts as disabled when every container inside carries the marker.
                bool has_container = false;
                bool all_disabled = true;
                std::error_code scan;
                for (const auto& file :
                    std::filesystem::recursive_directory_iterator(item.path(), scan)) {
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
            entry.icon_path = item.path();
            LauncherReadModJson(item.path(), entry);
            LauncherReadModNotes(entry);
            if (!entry.custom_name.empty()) entry.display_name = entry.custom_name;
            if (const auto cached = g_mod_remote_versions.find(entry.name);
                cached != g_mod_remote_versions.end()) {
                entry.remote_version = cached->second;
            }
            // Mods installed from the browse page have no mod.json; their source record still knows
            // the GameBanana id, which is enough to offer the "来源" button.
            const auto source = LauncherReadModSource(item.path());
            if (entry.link.empty() && !source.mod_id.empty())
                entry.link = "https://gamebanana.com/mods/" + source.mod_id;
            for (const auto& file : std::filesystem::recursive_directory_iterator(item.path(), error)) {
                if (!file.is_regular_file(error)) continue;
                LauncherFindModCover(entry, file.path());
                ++entry.files;
                entry.bytes += file.file_size(error);
            }
            mods.push_back(std::move(entry));
            continue;
        }
        if (!LauncherIsModFile(item.path())) continue;
        const std::string base = LauncherModBaseName(name);
        auto found = std::find_if(loose.begin(), loose.end(),
            [&base](const Loose& value) { return value.base == base; });
        if (found == loose.end()) {
            auto logical = std::filesystem::path(LauncherWiden(LauncherStripDisabled(name)));
            logical.replace_extension(L".pak");
            loose.push_back(Loose{base, LauncherNarrow(logical), 0, 0, LauncherStripDisabled(name) != name});
            found = loose.end() - 1;
        }
        found->bytes += item.file_size(error);
        ++found->files;
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
        if (const auto cached = g_mod_remote_versions.find(entry.name);
            cached != g_mod_remote_versions.end()) {
            entry.remote_version = cached->second;
        }
        mods.push_back(std::move(entry));
    }
    try {
        const auto settings = loaded_settings ? *loaded_settings : LauncherPakSettings(snapshot);
        static anomaly::PakModCatalogCache catalog_cache;
        const auto catalog = catalog_cache.Read(directory, settings);
        for (auto& entry : mods) {
            const auto key = anomaly::NormalizePakModKey(LauncherModStateKey(entry));
            for (const auto& row : *catalog) if (anomaly::NormalizePakModKey(row.key) == key) {
                if (!anomaly::PakModCompatibilityMode(settings)) entry.enabled = row.enabled;
                if (!row.resource_groups.empty()) {
                    entry.group.clear();
                    for (const auto& group : row.resource_groups) {
                        if (!entry.group.empty()) entry.group += ", ";
                        entry.group += group;
                    }
                }
            }
        }
    } catch (...) {
        // A malformed configuration must not replace a previously saved selection.
    }
    for (auto& entry : mods) if (!entry.custom_group.empty()) entry.group = entry.custom_group;
    std::sort(mods.begin(), mods.end(),
        [](const LauncherModEntry& left, const LauncherModEntry& right) {
            if (left.group != right.group) return left.group < right.group;
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

bool LauncherSetModEnabled(const LauncherSnapshot& snapshot, const LauncherModEntry& entry,
    const bool enabled) {
    const auto key = LauncherModStateKey(entry);
    try {
        const bool compatibility = LauncherCompatibilityMode(snapshot);
        if (compatibility && LauncherGameRunning()) return false;
        const auto write = [&](bool value) {
            return LauncherWritePakSettings(snapshot, [&](auto& document) {
                const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), document);
                anomaly::SelectPakMod(document, catalog, key, value);
            });
        };
        if (compatibility) {
            const auto settings = LauncherPakSettings(snapshot);
            const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), settings);
            const auto selected = std::find_if(catalog.begin(), catalog.end(), [&](const auto& row) {
                return anomaly::NormalizePakModKey(row.key) == anomaly::NormalizePakModKey(key);
            });
            if (selected == catalog.end()) return false;
            if (enabled) {
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
        if (compatibility && enable_after) {
            const auto catalog = anomaly::ReadPakModCatalog(LauncherModsDirectory(snapshot), LauncherPakSettings(snapshot));
            const auto selected = std::find_if(catalog.begin(), catalog.end(), [&](const auto& row) {
                return anomaly::NormalizePakModKey(row.key) == anomaly::NormalizePakModKey(LauncherModStateKey(target));
            });
            if (selected != catalog.end()) for (const auto& row : catalog) {
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
std::string LauncherHttpGet(const std::wstring& host, const std::wstring& path,
    bool use_system_proxy = false);
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
    const LauncherSnapshot& snapshot, const LauncherModEntry& entry, const std::string& new_name) {
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
        const auto new_key = renamed_stem + (entry.directory ? "/" : ".pak");
        if (entry.directory && std::filesystem::exists(directory / LauncherWiden(new_name), error) &&
            !std::filesystem::equivalent(entry.path, directory / LauncherWiden(new_name), error)) return false;
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
            const std::wstring base = LauncherWiden(new_name);
            if (!hot && !entry.enabled && !LauncherSetModFilesEnabled(snapshot, entry, false)) return false;
            try {
                std::filesystem::rename(entry.path, directory / base, error);
            } catch (...) {
                return false;
            }
            return !error && finish();
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
        return finish();
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

std::string LauncherHttpGet(const std::wstring& host, const std::wstring& path,
    const bool use_system_proxy) {
    std::string result;
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
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection != nullptr) {
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
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
                discovered.emplace_back(entry.name, entry.remote_version);
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


// Live byte counter and cancel flag for the browse installer's progress bar.
std::atomic<long long> g_download_bytes{0};
std::atomic<long long> g_download_total{0};
std::atomic<bool> g_download_cancel{false};


bool LauncherDownloadUrl(const std::string& url, const std::filesystem::path& destination,
    bool track_install);

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
        if (!LauncherDownloadUrl(download, archive, false)) {
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
    const bool track_install = false) {
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
                if (track_install) {
                    g_download_bytes.store(0);
                    wchar_t length_text[32]{};
                    DWORD length_size = sizeof(length_text);
                    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size,
                            WINHTTP_NO_HEADER_INDEX) != FALSE) {
                        g_download_total.store(_wtoi64(length_text));
                    } else {
                        g_download_total.store(0);
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
                        if (track_install) g_download_bytes.fetch_add(static_cast<long long>(read));
                        read = 0;
                        if (track_install && g_download_cancel.load()) {
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
    const std::string& category_id, std::vector<LauncherGbItem>& items) {
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
    for (const auto& record : *records) {
        if (!record.is_object()) continue;
        const int id = record.value("_idRow", 0);
        if (id <= 0) continue;
        LauncherGbItem item;
        item.id = std::to_string(id);
        item.name = record.value("_sName", std::string{});
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
    if (g_download_cancel.load()) return false;
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
        if (!LauncherDownloadUrl(file.url, archive, true)) {
            if (g_download_cancel.load()) {
                std::filesystem::remove(archive, error);
                return Tr(anomaly::MessageId::LauncherMsgCancelled);
            }
            LauncherUpdateLog("install: WinHTTP failed, trying curl: " + file.url);
            if (!LauncherDownloadWithCurl(file.url, archive)) {
                LauncherUpdateLog("install: curl failed " + file.url);
                return Tr(anomaly::MessageId::LauncherMsgDownloadFailedBoth);
            }
        }
        auto extracted = anomaly::launcher::ExtractModArchive(archive, &g_download_cancel);
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
        if (g_download_cancel.load()) return Tr(anomaly::MessageId::LauncherMsgCancelled);
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
    (void)controller;
    (void)window;
    g_decode_budget = 1;  // one image per frame
    struct PageResult final {
        int page{};
        bool ok{};
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

    const auto load_page = [&](const int page) {
        browse_loading = true;
        status.clear();
        if (page == 1) replace_pending = true;
        std::thread([page, query = active_query, sort = active_sort, category = active_category] {
            PageResult result;
            result.page = page;
            try {
                result.ok = LauncherLoadGbPage(page, query, sort, category, result.items);
            } catch (...) {
                LauncherUpdateLog("browse: page load failed");
            }
            std::scoped_lock lock(browse_mutex);
            browse_outcome = std::move(result);
        }).detach();
    };
    static bool prefs_loaded{};
    if (!prefs_loaded) {
        const auto prefs = LauncherLoadPrefs();
        active_sort = prefs.value("browseSort", std::string{});
        active_category = prefs.value("browseCategory", std::string{});
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
                exhausted = result.items.empty();
                loaded_pages = result.page;
                if (result.page == 1) {
                    items = std::move(result.items);
                    replace_pending = false;
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
    ImGui::BeginChild("BrowsePage", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::BeginDisabled(browse_loading);
    const bool refresh_clicked = ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsRefreshList));
    ImGui::EndDisabled();
    if (refresh_clicked) load_page(1);
    ImGui::SameLine();
    if (browse_loading) {
        ImGui::TextDisabled("%s", Tr(anomaly::MessageId::LauncherMsgLoading).c_str());
    } else {
        ImGui::BeginDisabled(exhausted && !replace_pending);
        const bool more_clicked = ImGui::Button(exhausted && !replace_pending
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

    // Category browsing is a server-side filter (the id from the category profile URL); NSFW is a
    // local filter because the feed has no maturity parameter.
    static bool hide_mature{true};  // loaded from prefs on first frame
    static bool mature_loaded{};
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
    if (!mature_loaded) {
        hide_mature = LauncherLoadPrefs().value("hideMature", true);
        mature_loaded = true;
    }
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
        LauncherSavePref("browseCategory", active_category);
        load_page(1);
    }
    if (anomaly::glass::Switch(Text(translator, anomaly::MessageId::LauncherBrowseHideNsfw), &hide_mature)) {
        LauncherSavePref("hideMature", hide_mature);
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
            LauncherSavePref("browseSort", active_sort);
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

    std::vector<const LauncherGbItem*> visible;
    for (const auto& item : items) {
        if (!needle.empty()) {
            std::string haystack = item.name + " " + item.author;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (haystack.find(needle) == std::string::npos) continue;
        }
        if (hide_mature && item.mature) continue;
        visible.push_back(&item);
    }
    const auto size_of = [](const LauncherGbItem& item) {
        std::scoped_lock lock(g_gb_size_mutex);
        const auto found = g_gb_sizes.find(item.id);
        return found == g_gb_sizes.end() ? 0.0 : std::atof(found->second.c_str());
    };
    std::stable_sort(visible.begin(), visible.end(),
        [&](const LauncherGbItem* left, const LauncherGbItem* right) {
            int result{};
            if (sort_mode == 2) {
                std::string a_text = left->downloads;
                std::string b_text = right->downloads;
                {
                    std::scoped_lock lock(g_gb_size_mutex);
                    const auto a = g_gb_downloads.find(left->id);
                    const auto b = g_gb_downloads.find(right->id);
                    if (a != g_gb_downloads.end() && !a->second.empty()) a_text = a->second;
                    if (b != g_gb_downloads.end() && !b->second.empty()) b_text = b->second;
                }
                const auto a = std::atoll(a_text.c_str());
                const auto b = std::atoll(b_text.c_str());
                result = a < b ? -1 : a > b ? 1 : 0;
            } else if (sort_mode == 3) {
                const double a = size_of(*left);
                const double b = size_of(*right);
                result = a < b ? -1 : a > b ? 1 : 0;
            } else if (sort_mode == 4) {
                const auto a = std::atoll(left->added.c_str());
                const auto b = std::atoll(right->added.c_str());
                result = a < b ? -1 : a > b ? 1 : 0;
            } else {
                result = left->name < right->name ? -1 : left->name > right->name ? 1 : 0;
            }
            return sort_descending ? result > 0 : result < 0;
        });

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
        ImGui::Text("%s", item.name.c_str());
        ImGui::SameLine();
        LauncherRequestSize(item.id);
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
        ImGui::TextDisabled("%s", info.c_str());
        ImGui::SameLine();
        if (!item.page_url.empty()) {
            if (ImGui::Button((std::string(Text(translator, anomaly::MessageId::LauncherModsPage)) + "##page" +
                item.id).c_str())) {
                const std::wstring url(item.page_url.begin(), item.page_url.end());
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::SameLine();
        }
        const bool installed = installed_ids.count(item.id) != 0;
        if (installed && !(install_running && install_target == item.id)) {
            ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsInstalled));
            ImGui::SameLine();
        }
        if (install_running && install_target == item.id) {
            // The row is already wide; put the progress on its own line so the cancel button is
            // never pushed outside the window.
            ImGui::NewLine();
            const double done = static_cast<double>(g_download_bytes.load());
            const double total = static_cast<double>(g_download_total.load());
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
            if (ImGui::Button((Tr(anomaly::MessageId::LauncherModsCancel) + "##cancel" +
                    item.id).c_str())) {
                g_download_cancel.store(true);
            }
        } else if (ImGui::Button((std::string(installed ? Text(translator, anomaly::MessageId::LauncherModsReinstall)
                : Text(translator, anomaly::MessageId::LauncherModsInstall)) +
                "##ins" + item.id).c_str()) &&
            !install_running) {
            install_running = true;
            install_target = item.id;
            install_started = ImGui::GetTime();
            g_download_cancel.store(false);
            g_download_bytes.store(0);
            g_download_total.store(0);
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
    }
    if (!items.empty() && shown == 0) {
        ImGui::TextDisabled("%s",
            Tr(anomaly::MessageId::LauncherBrowseNoMatches).c_str());
    }
    ImGui::EndChild();
}

void DrawModsMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    (void)window;
    (void)controller;
    g_decode_budget = 1;  // one image per frame
    static std::vector<LauncherModEntry> mods;
    static bool refreshed{};
    static char search[128]{};
    static int status_filter{};  // 0 = all, 1 = enabled, 2 = disabled
    static std::size_t page{};
    static std::string import_message;
    static char rename_buffer[128]{};
    static std::string rename_source;
    static char notes_name_buffer[128]{};
    static char notes_buffer[512]{};
    static char notes_group_buffer[64]{};
    static char notes_id_buffer[32]{};
    static std::string notes_source;
    static bool notes_directory{};
    static std::string pending_uninstall;   // single uninstall awaiting confirmation
    static std::map<std::string, bool> group_collapsed;
    static std::string update_status;
    static std::mutex check_mutex;
    static std::vector<std::pair<std::string, std::string>> check_versions;
    static std::string check_summary;
    static std::atomic<bool> check_running{false};
    static bool check_ready{};
    static double last_refresh{};
    static std::filesystem::path cached_game, directory;
    static bool compatibility_mode{}, bypass{};
    const auto refresh_mods = [&] {
        if (cached_game != snapshot.game_directory) page = 0;
        cached_game = snapshot.game_directory;
        directory = LauncherModsDirectory(snapshot);
        nlohmann::json settings;
        try {
            settings = LauncherPakSettings(snapshot);
            compatibility_mode = anomaly::PakModCompatibilityMode(settings);
            bypass = settings.value("enabled", true);
        } catch (...) {
            compatibility_mode = true;
            bypass = false;
        }
        LauncherRefreshMods(snapshot, mods, &settings);
        refreshed = true;
        last_refresh = ImGui::GetTime();
    };
    if (!refreshed || cached_game != snapshot.game_directory ||
        (ImGui::GetTime() - last_refresh > 2.0 && !check_running)) refresh_mods();

    // The launcher body is a fixed-height child without a scrollbar, so the page scrolls on its
    // own: path, controls, the signature switch and the list do not fit at once on smaller windows.
    ImGui::BeginChild("ModsPage", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextDisabled("%s", LauncherNarrow(directory).c_str());
    if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsRefresh))) {
        refresh_mods();
    }
    ImGui::SameLine();
    if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsOpenFolder))) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsImport))) {
        if (const auto archive = LauncherChooseArchive(window)) {
            import_message = LauncherImportArchive(snapshot, window, *archive);
            refresh_mods();
        }
    }
    ImGui::SameLine();
    if (check_running) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsChecking));
    } else if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsCheckUpdates))) {
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
                    if (entry.name == name) entry.remote_version = remote;
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
    const bool toggles_locked = game_running && compatibility_mode;
    bool mods_enabled = bypass;
    ImGui::BeginDisabled(toggles_locked);
    const bool enabled_changed = anomaly::glass::Switch(
        Text(translator, anomaly::MessageId::LauncherModsEnableMods), &mods_enabled);
    ImGui::EndDisabled();
    if (enabled_changed) {
        if (!LauncherWriteBypassEnabled(snapshot, mods_enabled))
            import_message = Text(translator, anomaly::MessageId::SettingsSaveFailed);
        refresh_mods();
    }
    ImGui::SameLine();
    bool compatibility = compatibility_mode;
    ImGui::BeginDisabled(game_running);
    const bool mode_changed = anomaly::glass::Switch(
        Text(translator, anomaly::MessageId::LauncherModsCompatibilityMode), &compatibility);
    ImGui::EndDisabled();
    if (mode_changed) {
        if (!LauncherWriteCompatibilityMode(snapshot, compatibility))
            import_message = Text(translator, anomaly::MessageId::LauncherModsModeSaveFailed);
        refresh_mods();
    }
    if (compatibility_mode)
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherModsCompatibilityHint));
    if (!import_message.empty()) {
        ImGui::TextDisabled("%s", import_message.c_str());
    }

    ImGui::SetNextItemWidth(Scale(260.0f));
    if (ImGui::InputTextWithHint("##mod-search",
            Text(translator, anomaly::MessageId::LauncherModsSearchHint), search, sizeof(search))) {
        page = 0;
    }
    ImGui::SameLine();
    const std::array<const char*, 3> filter_labels{
        Text(translator, anomaly::MessageId::LauncherModsFilterAll),
        Text(translator, anomaly::MessageId::LauncherModsFilterEnabled),
        Text(translator, anomaly::MessageId::LauncherModsFilterDisabled)};
    anomaly::glass::SegmentedOptions filter_options;
    filter_options.height = 46.0f;
    filter_options.padding_x = 30.0f;
    filter_options.scale = g_launcher_dpi_scale;
    if (anomaly::glass::SegmentedControl(
            "mod-status-filter", filter_labels, status_filter, filter_options)) {
        page = 0;
    }

    if (mods.empty()) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsEmpty));
        ImGui::EndChild();
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
    constexpr std::size_t page_size = 10;
    const std::size_t pages = (std::max)(std::size_t{1},
        (filtered.size() + page_size - 1) / page_size);
    page = (std::min)(page, pages - 1);
    ImGui::BeginDisabled(page == 0);
    if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsPreviousPage))) {
        --page;
        ImGui::SetScrollY(0.0f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(Tr(anomaly::MessageId::LauncherModsPagePosition,
        {std::to_string(page + 1), std::to_string(pages)}).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(page + 1 >= pages);
    if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsNextPage))) {
        ++page;
        ImGui::SetScrollY(0.0f);
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    const auto draw_switch = [&](const LauncherModEntry& target) {
        bool enabled = target.enabled;
        ImGui::BeginDisabled(toggles_locked);
        const bool changed = anomaly::glass::Switch(("##mod-enabled-" + target.name).c_str(), &enabled);
        ImGui::EndDisabled();
        if (!changed) return;
        if (!LauncherSetModEnabled(snapshot, target, enabled))
            import_message = Text(translator, anomaly::MessageId::SettingsSaveFailed);
        // Refresh next frame so the current rows keep valid references and their full scroll height.
        refreshed = false;
    };

    std::string current_group{"\x01"};  // impossible marker: forces a header for the first entry
    bool hide_group{};
    const std::size_t page_end = (std::min)(filtered.size(), (page + 1) * page_size);
    for (std::size_t index = page * page_size; index < page_end; ++index) {
        const auto& entry = *filtered[index];
        const std::string group_name = entry.group.empty()
            ? Tr(anomaly::MessageId::LauncherModsUngrouped)
            : entry.group;
        if (group_name != current_group) {
            current_group = group_name;
            hide_group = group_collapsed[group_name];
            ImGui::Spacing();
            if (ImGui::Selectable(group_name.c_str())) {
                group_collapsed[group_name] = !hide_group;
            }
            ImGui::Separator();
        }
        if (hide_group) continue;
        DrawLauncherModCover(entry);
        ImGui::SameLine();
        ImGui::BeginGroup();
        std::string title = entry.display_name.empty() ? entry.name : entry.display_name;
        std::string meta;
        if (!entry.author.empty()) meta += "  ·  " + entry.author;
        if (!entry.version.empty()) meta += "  ·  v" + entry.version;
        if (!entry.remote_version.empty() && entry.remote_version != entry.version) {
            meta += Tr(anomaly::MessageId::LauncherModsNewVersionInline,
                {entry.remote_version});
        }
        ImGui::TextWrapped("%s", title.c_str());
        ImGui::TextDisabled("%zu  ·  %.1f MB%s", entry.files,
            static_cast<double>(entry.bytes) / (1024.0 * 1024.0), meta.c_str());
        // Every action is a button inside a disabled scope: the click is evaluated first and the
        // action runs afterwards, so a disabled scope never spans an early return.
        const float actions_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const auto next_action = [&](const char* label) {
            const auto& style = ImGui::GetStyle();
            const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
            if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= actions_right)
                ImGui::SameLine();
        };
        draw_switch(entry);
        next_action(Text(translator, anomaly::MessageId::LauncherModsUninstall));
        ImGui::BeginDisabled(game_running && entry.enabled);
        const bool uninstall_clicked = ImGui::Button(
            (std::string(Text(translator, anomaly::MessageId::LauncherModsUninstall)) +
                "##" + entry.name).c_str());
        ImGui::EndDisabled();
        if (uninstall_clicked) {
            pending_uninstall = entry.name;
            ImGui::OpenPopup("confirm-uninstall");
        }
        next_action(Text(translator, anomaly::MessageId::LauncherModsNotes));
        const bool notes_clicked = ImGui::Button(
            (std::string(Text(translator, anomaly::MessageId::LauncherModsNotes)) + "##notes" +
                entry.name).c_str());
        if (notes_clicked) {
            notes_source = entry.name;
            notes_directory = entry.directory;
            std::snprintf(notes_name_buffer, sizeof(notes_name_buffer), "%s",
                entry.custom_name.empty() ? entry.display_name.c_str() : entry.custom_name.c_str());
            std::snprintf(notes_buffer, sizeof(notes_buffer), "%s", entry.notes.c_str());
            std::snprintf(notes_group_buffer, sizeof(notes_group_buffer), "%s",
                entry.group.c_str());
            notes_id_buffer[0] = '\0';
            // Show the already bound GameBanana id so the field is not misleadingly empty.
            if (entry.directory) {
                const auto source = LauncherReadModSource(entry.icon_path);
                std::snprintf(notes_id_buffer, sizeof(notes_id_buffer), "%s", source.mod_id.c_str());
            }
            ImGui::OpenPopup("mod-notes");
        }
        next_action(Text(translator, anomaly::MessageId::LauncherModsRename));
        ImGui::BeginDisabled(game_running && entry.enabled);
        const bool rename_clicked = ImGui::Button(
            (std::string(Text(translator, anomaly::MessageId::LauncherModsRename)) +
                "##" + entry.name).c_str());
        ImGui::EndDisabled();
        if (rename_clicked) {
            rename_source = entry.name;
            std::snprintf(rename_buffer, sizeof(rename_buffer), "%s", entry.name.c_str());
            ImGui::OpenPopup("rename-mod");
        }
        if (!entry.remote_version.empty() &&
            (entry.version.empty() || entry.remote_version != entry.version)) {
            next_action(Text(translator, anomaly::MessageId::LauncherModsUpdate));
            ImGui::BeginDisabled(game_running && entry.enabled);
            const bool update_clicked = ImGui::Button(
                (std::string(Text(translator, anomaly::MessageId::LauncherModsUpdate)) + "##u" +
                entry.name).c_str());
            ImGui::EndDisabled();
            if (update_clicked) {
                import_message = LauncherUpdateMod(snapshot, entry);
                refresh_mods();
                ImGui::EndGroup();
                ImGui::EndChild();
                return;
            }
        }
        if (!entry.link.empty()) {
            next_action(Text(translator, anomaly::MessageId::LauncherModsOpenSource));
            if (ImGui::Button((std::string(Text(translator, anomaly::MessageId::LauncherModsOpenSource)) +
                    "##" + entry.name).c_str())) {
                const std::wstring url(entry.link.begin(), entry.link.end());
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::EndGroup();
        ImGui::Spacing();
    }
    if (filtered.empty()) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherModsNoMatches));
    }
    if (ImGui::BeginPopupModal("rename-mod", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("##rename", rename_buffer, sizeof(rename_buffer));
        if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsConfirm))) {
            for (const auto& entry : mods) {
                if (entry.name != rename_source) continue;
                if (LauncherRenameMod(snapshot, entry, rename_buffer)) refresh_mods();
                else import_message = Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
                break;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsCancel))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("confirm-uninstall", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", Format(translator, anomaly::MessageId::LauncherModsConfirmUninstall,
            {pending_uninstall}).c_str());
        if (ImGui::Button(Tr(anomaly::MessageId::LauncherModsConfirmUninstallButton).c_str())) {
            for (const auto& entry : mods) {
                if (entry.name != pending_uninstall) continue;
                if (!LauncherRemoveMod(snapshot, entry))
                    import_message = Tr(anomaly::MessageId::LauncherModsFileChangeFailed);
                break;
            }
            pending_uninstall.clear();
            refresh_mods();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button((Tr(anomaly::MessageId::LauncherModsCancel) + "##uninstall").c_str()))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("mod-notes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherModsDisplayName));
        ImGui::InputText("##notes-name", notes_name_buffer, sizeof(notes_name_buffer));
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
            if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsAssociate))) {
                for (const auto& entry : mods) {
                    if (entry.name != notes_source) continue;
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
        if (ImGui::Button(Text(translator, anomaly::MessageId::LauncherModsSave))) {
            for (const auto& entry : mods) {
                if (entry.name != notes_source) continue;
                if (!LauncherWriteModNotes(entry, notes_name_buffer, notes_buffer, notes_group_buffer))
                    import_message = Tr(anomaly::MessageId::SettingsSaveFailed);
                break;
            }
            refresh_mods();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button((Tr(anomaly::MessageId::LauncherModsCancel) + "##notes").c_str()))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

void DrawAttachMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionNteLauncher));
    DrawReadOnlyPath(
        "##NteLauncherExecutable", snapshot.launcher_executable, 38.0f, translator);
    ImGui::SameLine();
    const std::string choose_label = Format(
        translator, anomaly::MessageId::LauncherChooseNteLauncher,
        {ClientLauncherFileName(snapshot.selected_client)});
    if (IconButton(
            "choose-nte-launcher", 0xe838, choose_label.c_str(),
            !snapshot.busy)) {
        if (const auto selected = ChooseLauncherExecutable(
                window, snapshot.launcher_executable, snapshot.selected_client, translator)) {
            controller.SelectLauncherExecutable(*selected);
        }
    }

    DrawStartupSettings(controller, snapshot, translator);

    // The simple process summary that replaces the old full table: the startup
    // settings sit under the path, then each mode's own status, then the action.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionProcesses));
    // The status is the controller's own last message, which already carries every
    // distinction this needs: waiting for HTGame.exe while the launch runs, then the
    // attached PID or the failure, coloured by its kind. It replaced a static "ready"
    // line that said the same thing no matter what the launcher was doing, and it
    // also restores the reporting the deleted footer used to provide.
    const ImVec4 status_ink = snapshot.message_kind == MessageKind::Error
        ? ThemeColor(theme.danger)
        : snapshot.message_kind == MessageKind::Success
            ? ThemeColor(theme.success)
            : kLauncherMutedInk;
    const std::string status = RenderMessage(translator, snapshot.message);
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

    const bool can_launch = !snapshot.busy && snapshot.core_available &&
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
    const bool can_inject = !snapshot.busy && snapshot.core_available && inject_target != 0;
    // The same bottom-right placement and size as the proxy mode's primary action, so switching modes does not
    // move the button under the cursor -- but the widths are measured from the labels, because a fixed width
    // ellipsized the English "Inject into running process" down to "Inject into running ...".
    const char* inject_label = Text(translator, anomaly::MessageId::LauncherActionInjectRunning);
    const char* launch_label = Text(translator, anomaly::MessageId::LauncherLaunchAttach);
    const float inject_width = (std::max)(200.0f, CommandButtonWidth(0xe8e5, inject_label));
    const float launch_width = (std::max)(240.0f, CommandButtonWidth(0xe768, launch_label));
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - Scale(74.0f));
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

// The native caption is gone, so the launcher draws its own: a drag band holding
// the window title, then the minimise and close controls the frame used to own.
// Nothing here paints a background, so the wallpaper and the stickers show
// through the caption exactly as they do behind the rest of the window.
void DrawLauncherTitleBar(
    Graphics& graphics, const anomaly::Translator& translator) {
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
    HWND window, Graphics& graphics, LauncherController& controller, LauncherMode& mode,
    const anomaly::Translator& translator) {
    // Runs every frame whatever page is showing: a game that starts after this window is up has to appear
    // in the list without the user pressing anything, and the poll throttles itself to one scan every couple
    // of seconds on the worker thread.
    controller.PollProcesses();
    const LauncherSnapshot snapshot = controller.Snapshot();
    ApplyLauncherTheme(snapshot.theme);
    UploadLauncherTheme(graphics, snapshot.theme);
    const ImGuiIO& io = ImGui::GetIO();
    // The root window is the shell canvas. The bands beside it are transparent margin,
    // so the canvas starts after the left one and stops short of the right one; every
    // element inside keeps the size and the geometry it had before they existed.
    const float band_left = Scale(kLauncherBandLeft);
    const float band_right = Scale(kLauncherBandRight);
    ImGui::SetNextWindowPos(ImVec2(band_left, 0.0f));
    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x - band_left - band_right, io.DisplaySize.y));
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
    DrawLauncherTitleBar(graphics, translator);
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
        anomaly::glass::DrawSurface(ImGui::GetWindowDrawList(), card_min, card_max, card);
        ImGui::SetCursorScreenPos(card_min);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(20.0f, 18.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "LauncherBody", ImVec2(root_size.x - Scale(28.0f), -Scale(14.0f)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (mode == LauncherMode::Proxy) {
        DrawProxyMode(window, controller, snapshot, translator);
    } else if (mode == LauncherMode::Attach) {
        DrawAttachMode(window, controller, snapshot, translator);
    } else if (mode == LauncherMode::Mods) {
        DrawModsMode(window, controller, snapshot, translator);
    } else {
        DrawBrowseMode(window, controller, snapshot, translator);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    DrawLauncherThemeImages(graphics, snapshot.theme, root_origin, root_size, true);
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const auto administrator = EnsureAdministrator(command_line);
    if (!administrator.run_current_process) return administrator.exit_code;

    // The release directory also contains the proxy payload named dwmapi.dll.
    // Pin the system module before ImGui's delay imports are resolved.
    const HMODULE system_dwmapi = LoadSystemDwmapi();
    if (system_dwmapi == nullptr) return static_cast<int>(GetLastError());

    const std::filesystem::path runtime_root = ExecutableDirectory() / L"Anomaly";
    const ue5mem::AnalyzerConfig config =
        ue5mem::AnalyzerConfig::Load(runtime_root / L"anomaly.ini");
    const auto locale = anomaly::ResolveUserLocale(config.platform_language);
    const auto translator_result = anomaly::LoadHostCatalog(
        locale.locale, runtime_root / L"locales" / L"host");
    if (translator_result.translator == nullptr) return ERROR_RESOURCE_DATA_NOT_FOUND;
    const std::shared_ptr<const anomaly::Translator> translator =
        translator_result.translator;
    g_translator = translator.get();

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
        translator->Text(anomaly::MessageId::LauncherWindowTitle));
    g_launcher_dpi_scale = DpiScale(GetDpiForSystem());
    // WS_OVERLAPPEDWINDOW cannot be used here: it carries a caption, and removing
    // the frame from the client area with WM_NCCALCSIZE does not stop the window
    // from owning one, so the native title bar comes straight back. A popup has no
    // caption to begin with; WS_EX_APPWINDOW keeps the taskbar entry that a plain
    // popup would lose, and the placement is computed because a popup ignores
    // CW_USEDEFAULT.
    // The window is the shell canvas plus one band on each side. The bands are where
    // a sticker that overflows the canvas is allowed to be seen, and they are the
    // reason the window carries an alpha channel at all.
    const int window_width = static_cast<int>(
        Scale(kLauncherBandLeft) + Scale(1180.0f) + Scale(kLauncherBandRight));
    const int window_height = static_cast<int>(Scale(700.0f));
    const int window_x = (GetSystemMetrics(SM_CXSCREEN) - window_width) / 2;
    const int window_y = (GetSystemMetrics(SM_CYSCREEN) - window_height) / 2;
    // WS_EX_NOREDIRECTIONBITMAP stops the window from painting a background of its
    // own, so the composition visual is the only thing that reaches the screen and
    // whatever the shell leaves untouched stays transparent. It is not click-through:
    // the bands still belong to the window and still take the mouse.
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
    graphics.mod_placeholder = LoadEmbeddedTexture(graphics, kModPlaceholderResourceId);
    LauncherController controller(ExecutableDirectory());
    LauncherMode mode = LauncherMode::Proxy;

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
        DrawLauncher(window, graphics, controller, mode, *translator);
        ImGui::Render();
        // What the shell leaves untouched is the side bands, so the clear has to be
        // fully transparent -- and under premultiplied alpha that means colour zero as
        // well, or the bands would composite as a tinted margin.
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
