#include "anomaly/launcher/manual_map.hpp"
#include "anomaly/launcher/configuration.hpp"
#include "anomaly/launcher/proxy_installation.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/platform_ui_layout.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/runtime_launch.hpp"
#include "anomaly/runtime_recovery.hpp"
#include "anomaly/ui_resource_decoder.hpp"
#include "config.hpp"

#include <Windows.h>
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
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
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

enum class LauncherMode : std::uint8_t { Proxy, Attach };
enum class MessageKind : std::uint8_t { Neutral, Success, Error };

// One decoded theme image. The worker decodes and the render thread only
// uploads, so a large background never stalls on WIC during a frame.
struct LauncherThemeImage final {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool Ok() const noexcept {
        return width != 0 && height != 0 && !pixels.empty();
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

struct LauncherSnapshot final {
    anomaly::launcher::NteClient selected_client{
        anomaly::launcher::NteClient::MainlandChina};
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
    anomaly::launcher::ProxyInstallationStatus proxy;
    std::optional<anomaly::RuntimeRecoveryState> recovery;
    std::string recovery_message;
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

const char* Glyph(char32_t codepoint) {
    struct Entry final { char32_t codepoint; std::string text; };
    static const std::array entries{
        Entry{0xe838, EncodeUtf8(0xe838)},
        Entry{0xe72c, EncodeUtf8(0xe72c)},
        Entry{0xe768, EncodeUtf8(0xe768)},
        Entry{0xe73e, EncodeUtf8(0xe73e)},
        Entry{0xe7ba, EncodeUtf8(0xe7ba)},
        Entry{0xe711, EncodeUtf8(0xe711)},
        Entry{0xe8b7, EncodeUtf8(0xe8b7)},
    };
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
            RefreshRecoveryImpl();
            RefreshProcessesImpl(false);
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void RefreshProxy() {
        Queue(anomaly::MessageId::LauncherStatusInspectingProxy, MessageScope::Proxy, [this] {
            RefreshProxyImpl();
            RefreshRecoveryImpl();
            RefreshProcessesImpl(false);
        });
    }

    void InstallProxy() {
        Queue(anomaly::MessageId::LauncherStatusInstallingRuntime, MessageScope::Proxy, [this] {
            const auto game = GameDirectory();
            const auto result = anomaly::launcher::InstallProxyRuntime(game, source_);
            PublishProxy(result);
            RefreshRecoveryImpl();
            RefreshProcessesImpl(false);
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

    void RestoreRecovery(anomaly::RuntimeRecoveryAxis axis) {
        Queue(anomaly::MessageId::LauncherStatusRestoringRecovery, MessageScope::Proxy, [this, axis] {
            const auto runtime_root = GameDirectory() / L"Anomaly";
            anomaly::RuntimeRecoveryStore store(runtime_root);
            PublishRecovery(store.Restore(axis), true);
        });
    }

    void RefreshProcesses() {
        Queue(anomaly::MessageId::LauncherStatusScanningProcesses,
            MessageScope::Attach, [this] { RefreshProcessesImpl(); });
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
            state_.recovery.reset();
            state_.recovery_message.clear();
            state_.attached_process = 0;
        }
        Queue(anomaly::MessageId::LauncherStatusScanningLocal, MessageScope::Both, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshHotkeyImpl();
            RefreshProxyImpl();
            RefreshRecoveryImpl();
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

    void RefreshRecoveryImpl() {
        const auto runtime_root = GameDirectory() / L"Anomaly";
        std::error_code error;
        if (!std::filesystem::is_directory(runtime_root, error) || error) {
            std::scoped_lock lock(state_mutex_);
            state_.recovery.reset();
            state_.recovery_message.clear();
            return;
        }
        anomaly::RuntimeRecoveryStore store(runtime_root);
        PublishRecovery(store.Load(), false);
    }

    void PublishRecovery(anomaly::RuntimeRecoveryResult result, bool announce) {
        std::scoped_lock lock(state_mutex_);
        if (result.Ok()) {
            state_.recovery = std::move(result.state);
            state_.recovery_message.clear();
            if (announce) {
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusRecoveryRestored);
                state_.message_kind = MessageKind::Success;
            }
            return;
        }
        state_.recovery.reset();
        if (result.error == anomaly::RuntimeRecoveryError::StateUnavailable) {
            state_.recovery_message.clear();
            return;
        }
        state_.recovery_message = std::move(result.message);
        if (announce) {
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusRecoveryRestoreFailed, {},
                state_.recovery_message);
            state_.message_kind = MessageKind::Error;
        }
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
        RefreshRecoveryImpl();
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

    void RefreshProcessesImpl(bool announce = true) {
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
        if (!announce) {
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

void LoadLogo(Graphics& graphics) {
    const HRSRC resource = FindResourceW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kLogoResourceId), RT_RCDATA);
    if (resource == nullptr) return;
    const HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const void* data = loaded == nullptr ? nullptr : LockResource(loaded);
    if (data == nullptr || size == 0) return;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto decoded = anomaly::DecodeUiImageRgba8(std::span(bytes, size));
    if (!decoded) return;
    graphics.logo = CreateTextureFromPixels(graphics, decoded.image.pixels,
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
    if (!front) {
        // The window contributes no body of its own: it has no redirection bitmap, and
        // every panel fill is empty while a theme is active. The shell therefore paints
        // its own opaque base first -- the layer the in-game shell gets for free from
        // its opaque window. Without it the veil would composite against the desktop
        // instead of against the surface colour, and whatever sits behind the launcher
        // would show through the window.
        list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
            tint(ue5mem::PlatformUiSurfaceColor(), 1.0f));
    }
    if (!front && graphics.theme_background != nullptr &&
        graphics.theme_background_width != 0 && graphics.theme_background_height != 0) {
        const auto placement = anomaly::ComputePlatformUiBackgroundPlacement(
            layout.background.mode,
            static_cast<float>(graphics.theme_background_width),
            static_cast<float>(graphics.theme_background_height),
            origin.x, origin.y, size.x, size.y);
        if (placement.Visible()) {
            list->AddImage(
                static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(graphics.theme_background.Get())),
                ImVec2(placement.x, placement.y),
                ImVec2(placement.x + placement.width, placement.y + placement.height),
                ImVec2(placement.uv0_x, placement.uv0_y),
                ImVec2(placement.uv1_x, placement.uv1_y),
                tint(layout.background.tint, layout.background.opacity));
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
                tint(ue5mem::PlatformUiSurfaceColor(), veil_alpha));
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
    const std::wstring title = Utf8Wide(
        translator.Text(anomaly::MessageId::LauncherDialogNteLauncher));
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
    const bool pressed = ImGui::Button("##icon", ImVec2(extent, extent));
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
        ThemeColor(primary ? theme.accent : theme.button));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ThemeColor(primary ? theme.accent_hovered : theme.button_hovered));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        ThemeColor(primary ? theme.accent_active : theme.button_active));
    // Plain black on the light themed surface. The palette's muted ink is tuned
    // for the shell's own panels; over a wallpaper and its veil it reads as washed
    // out to the point of being unreadable.
    ImGui::PushStyleColor(ImGuiCol_Text, primary
        ? ThemeColor(theme.inverse_text) : ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(text.c_str(), scaled_size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return pressed && enabled;
}

bool ModeButton(const char* id, const char* label, bool selected, float width) {
    const auto& theme = ue5mem::PlatformUiTheme();
    const ImVec2 size(Scale(width), ButtonHeight(38.0f));
    const std::string text = Ellipsize(label,
        (std::max)(0.0f, size.x - ImGui::GetStyle().FramePadding.x * 2.0f));
    ImGui::PushID(id);
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? ThemeColorWithAlpha(theme.accent, 0.16f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        selected ? ThemeColorWithAlpha(theme.accent, 0.26f)
                 : ThemeColor(theme.button_hovered));
    // Selection is carried by the accent tint behind the label, so the label
    // itself stays black and readable in both states.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    const bool pressed = ImGui::Button(text.c_str(), size);
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return pressed;
}

const char* Text(const anomaly::Translator& translator, anomaly::MessageId id) noexcept {
    return translator.Text(id).data();
}

std::string StableLabel(
    const anomaly::Translator& translator, anomaly::MessageId id,
    std::string_view stable_id) {
    return anomaly::StableDisplayLabel(translator.Text(id), stable_id);
}

std::string Format(
    const anomaly::Translator& translator, anomaly::MessageId id,
    std::initializer_list<std::string_view> arguments) {
    return translator.Format(id, std::span<const std::string_view>(
        arguments.begin(), arguments.size()));
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
    const auto& theme = ue5mem::PlatformUiTheme();
    const bool proxy_enabled = snapshot.proxy.state ==
        anomaly::launcher::ProxyInstallationState::Enabled;
    if (proxy_enabled && mode == LauncherMode::Attach) {
        mode = LauncherMode::Proxy;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColor(theme.toolbar_background));
    ImGui::BeginChild(
        "LauncherModes", ImVec2(0.0f, Scale(kModeHeight)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginTable("LauncherToolbar", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Modes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthFixed, Scale(190.0f));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ModeButton("proxy-install", Text(translator,
                anomaly::MessageId::LauncherModeProxyInstall),
                mode == LauncherMode::Proxy, 126.0f)) {
            mode = LauncherMode::Proxy;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(snapshot.busy || proxy_enabled);
        if (ModeButton("live-attach", Text(translator,
                anomaly::MessageId::LauncherModeLiveAttach),
                mode == LauncherMode::Attach, 126.0f)) {
            mode = LauncherMode::Attach;
        }
        ImGui::EndDisabled();
        // The client is always the mainland one. The switch that used to live in
        // the second column only ever had one useful setting, and NteClient's
        // default already is MainlandChina, so the column is simply gone.
        ImGui::EndTable();
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

void DrawRecoveryAxis(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator, const char* stable_id,
    anomaly::MessageId label, anomaly::MessageId action,
    anomaly::RuntimeRecoveryAxis axis) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, Scale(28.0f));
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, label));
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginDisabled(snapshot.busy);
    ImGui::PushID(stable_id);
    if (ImGui::SmallButton(Text(translator, action))) controller.RestoreRecovery(axis);
    ImGui::PopID();
    ImGui::EndDisabled();
}

void DrawRecoveryState(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    const bool active = snapshot.recovery && snapshot.recovery->safe_mode.Active();
    if (!active && snapshot.recovery_message.empty()) return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionRecovery));
    if (!snapshot.recovery_message.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ThemeColor(theme.danger), "%s", snapshot.recovery_message.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    const auto& safe_mode = snapshot.recovery->safe_mode;
    ImGui::TextColored(ThemeColor(theme.warning), "%s",
        Text(translator, anomaly::MessageId::LauncherRecoverySafeModeActive));
    if (!safe_mode.reason.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kLauncherMutedInk, "%s", safe_mode.reason.c_str());
    }
    if (ImGui::BeginTable(
            "RecoveryAxes", 2, ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.0f, 0.0f))) {
        const std::string axis_column = StableLabel(translator,
            anomaly::MessageId::LauncherRecoveryAxis, "recovery-axis");
        const std::string action_column = StableLabel(translator,
            anomaly::MessageId::LauncherRecoveryAction, "recovery-action");
        ImGui::TableSetupColumn(axis_column.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            action_column.c_str(), ImGuiTableColumnFlags_WidthFixed, Scale(72.0f));
        if (safe_mode.minimal_core) {
            DrawRecoveryAxis(
                controller, snapshot, translator, "minimal-core",
                anomaly::MessageId::LauncherRecoveryMinimalCore,
                anomaly::MessageId::LauncherRecoveryRestore,
                anomaly::RuntimeRecoveryAxis::MinimalCore);
        }
        if (safe_mode.third_party_plugins_suspended) {
            DrawRecoveryAxis(
                controller, snapshot, translator, "third-party-plugins",
                anomaly::MessageId::LauncherRecoveryThirdPartyPlugins,
                anomaly::MessageId::LauncherRecoveryRestore,
                anomaly::RuntimeRecoveryAxis::ThirdPartyPlugins);
        }
        if (safe_mode.profile_overrides_suspended) {
            DrawRecoveryAxis(
                controller, snapshot, translator, "profile-overrides",
                anomaly::MessageId::LauncherRecoveryProfileOverrides,
                anomaly::MessageId::LauncherRecoveryRestore,
                anomaly::RuntimeRecoveryAxis::ProfileOverrides);
        }
        ImGui::EndTable();
    }
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

    DrawRecoveryState(controller, snapshot, translator);

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

void DrawAttachMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionNteLauncher));
    DrawReadOnlyPath(
        "##NteLauncherExecutable", snapshot.launcher_executable, 38.0f, translator);
    ImGui::SameLine();
    if (IconButton(
            "choose-nte-launcher", 0xe838,
            Text(translator, anomaly::MessageId::LauncherChooseNteLauncher),
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
    // The same bottom-right placement and size as the proxy mode's primary action,
    // so switching modes does not move the button under the cursor.
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - Scale(74.0f));
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
        ImGui::GetWindowWidth() - Scale(240.0f) - Scale(16.0f)));
    if (CommandButton(
            "launch-attach-core", 0xe768,
            Text(translator, anomaly::MessageId::LauncherLaunchAttach), true, can_launch,
            ImVec2(240.0f, 62.0f))) {
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
    const auto& theme = ue5mem::PlatformUiTheme();
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
    DrawLauncherTitleBar(graphics, translator);
    DrawModes(controller, snapshot, mode, translator);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColor(theme.window_background));
    ImGui::BeginChild(
        "LauncherBody", ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (mode == LauncherMode::Proxy) {
        DrawProxyMode(window, controller, snapshot, translator);
    } else {
        DrawAttachMode(window, controller, snapshot, translator);
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
    static_cast<void>(ue5mem::ConfigurePlatformUiFontAtlas(runtime_root));
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
    LoadLogo(graphics);
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
