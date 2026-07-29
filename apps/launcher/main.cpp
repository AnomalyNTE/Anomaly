#include "anomaly/launcher/manual_map.hpp"
#include "anomaly/launcher/configuration.hpp"
#include "anomaly/launcher/proxy_installation.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/runtime_launch.hpp"
#include "anomaly/runtime_recovery.hpp"
#include "anomaly/ui_resource_decoder.hpp"
#include "config.hpp"

#include <Windows.h>
#include <d3d11.h>
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
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
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
constexpr float kHeaderHeight = 56.0f;
constexpr float kModeHeight = 48.0f;
constexpr float kFooterHeight = 42.0f;
constexpr float kProxyActionLeftPadding = 4.0f;

const ImVec4 kAccent{0.345f, 0.718f, 0.647f, 1.0f};
const ImVec4 kAccentHover{0.415f, 0.773f, 0.706f, 1.0f};
const ImVec4 kSurface{0.106f, 0.125f, 0.145f, 1.0f};
const ImVec4 kRaised{0.137f, 0.165f, 0.192f, 1.0f};
const ImVec4 kMuted{0.667f, 0.698f, 0.737f, 1.0f};
const ImVec4 kSuccess{0.333f, 0.733f, 0.482f, 1.0f};
const ImVec4 kWarning{0.941f, 0.706f, 0.314f, 1.0f};
const ImVec4 kDanger{0.910f, 0.365f, 0.365f, 1.0f};

enum class LauncherMode : std::uint8_t { Proxy, Attach };
enum class MessageKind : std::uint8_t { Neutral, Success, Error };

struct LauncherMessage final {
    anomaly::MessageId id{anomaly::MessageId::LauncherStateReady};
    std::vector<std::string> arguments;
    std::string detail;
};

struct LauncherSnapshot final {
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
    bool busy{};
    LauncherMessage message;
    MessageKind message_kind{MessageKind::Neutral};
};

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
    std::size_t end = value.size();
    while (end > 0) {
        --end;
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
        Queue(anomaly::MessageId::LauncherStatusScanningLocal, [this] {
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
        }
        Queue(anomaly::MessageId::LauncherStatusInspectingProxy, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
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
        Queue(anomaly::MessageId::LauncherStatusInspectingProxy, [this] {
            RefreshProxyImpl();
            RefreshRecoveryImpl();
            RefreshProcessesImpl(false);
        });
    }

    void InstallProxy() {
        Queue(anomaly::MessageId::LauncherStatusInstallingRuntime, [this] {
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
            [this, enabled] {
            const auto game = GameDirectory();
            const auto result = anomaly::launcher::SetProxyEnabled(
                game, source_, enabled);
            PublishProxy(result);
        });
    }

    void RestoreRecovery(anomaly::RuntimeRecoveryAxis axis) {
        Queue(anomaly::MessageId::LauncherStatusRestoringRecovery, [this, axis] {
            const auto runtime_root = GameDirectory() / L"Anomaly";
            anomaly::RuntimeRecoveryStore store(runtime_root);
            PublishRecovery(store.Restore(axis), true);
        });
    }

    void RefreshProcesses() {
        Queue(anomaly::MessageId::LauncherStatusScanningProcesses,
            [this] { RefreshProcessesImpl(); });
    }

    void SelectLauncherExecutable(std::filesystem::path executable) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.launcher_executable = std::move(executable);
        }
        Queue(anomaly::MessageId::LauncherStatusScanningLocal, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshProcessesImpl();
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void LaunchAndAttach() {
        Queue(anomaly::MessageId::LauncherStatusLaunchingAttach, [this] {
            const auto launcher = LauncherExecutable();
            const auto selected = ResolveAttachRuntime();
            if (!selected.Ok()) {
                PublishRuntimeFailure(selected);
                return;
            }
            anomaly::launcher::ManualMapLaunchOptions options;
            options.launcher_path = launcher;
            options.working_directory = launcher.parent_path();
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

    bool Queue(anomaly::MessageId activity, Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
            state_.message = MakeLauncherMessage(activity);
            state_.message_kind = MessageKind::Neutral;
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
        const auto launcher_processes =
            anomaly::launcher::EnumerateAttachableProcesses(L"NTELauncher.exe");
        anomaly::launcher::LauncherDiscoveryOptions options;
        options.payload_root = payload_root_;
        for (const auto& process : game_processes) {
            if (!process.executable_path.empty()) {
                options.running_game_executables.push_back(process.executable_path);
            }
        }
        for (const auto& process : launcher_processes) {
            if (!process.executable_path.empty()) {
                options.running_launcher_executables.push_back(process.executable_path);
            }
        }
        const auto discovered = anomaly::launcher::DiscoverLauncherConfiguration(
            loaded.configuration, options);
        {
            std::scoped_lock lock(state_mutex_);
            state_.game_directory = discovered.game_directory;
            state_.launcher_executable = discovered.launcher_executable;
        }
        const auto saved = PersistConfigurationImpl();
        RefreshProxyImpl();
        RefreshRecoveryImpl();
        RefreshProcessesImpl();
        if (!saved.Ok()) {
            PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                MessageKind::Error, saved.message);
        }
    }

    void ReconcileRelatedPathsImpl() {
        anomaly::launcher::LauncherConfiguration preferred;
        {
            std::scoped_lock lock(state_mutex_);
            preferred.game_directory = state_.game_directory;
            preferred.launcher_executable = state_.launcher_executable;
        }
        anomaly::launcher::LauncherDiscoveryOptions options;
        options.payload_root = payload_root_;
        const auto discovered = anomaly::launcher::DiscoverLauncherConfiguration(
            preferred, options);
        std::scoped_lock lock(state_mutex_);
        state_.game_directory = discovered.game_directory;
        state_.launcher_executable = discovered.launcher_executable;
    }

    [[nodiscard]] anomaly::launcher::LauncherConfigurationSaveResult
    PersistConfigurationImpl() const {
        anomaly::launcher::LauncherConfiguration configuration;
        {
            std::scoped_lock lock(state_mutex_);
            configuration.game_directory = state_.game_directory;
            configuration.launcher_executable = state_.launcher_executable;
        }
        return anomaly::launcher::SaveLauncherConfiguration(
            configuration_path_, configuration);
    }

    void RefreshProcessesImpl(bool announce = true) {
        auto processes = anomaly::launcher::EnumerateAttachableProcesses();
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
        } else {
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusProcessesRefreshed);
            state_.message_kind = MessageKind::Neutral;
        }
    }

    std::filesystem::path payload_root_;
    anomaly::launcher::ProxyInstallationSource source_;
    std::filesystem::path configuration_path_;
    mutable std::mutex state_mutex_;
    LauncherSnapshot state_;
    std::mutex queue_mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Work> queue_;
    std::jthread worker_;
};

struct Graphics final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11ShaderResourceView> logo;
};

Graphics* g_graphics{};

bool CreateRenderTarget(Graphics& graphics) {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(graphics.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    return SUCCEEDED(graphics.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &graphics.render_target));
}

bool CreateGraphics(HWND window, Graphics& graphics) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION, &description, &graphics.swap_chain, &graphics.device,
        &selected, &graphics.context);
    return SUCCEEDED(result) && CreateRenderTarget(graphics);
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
    D3D11_TEXTURE2D_DESC description{};
    description.Width = decoded.image.width;
    description.Height = decoded.image.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = decoded.image.pixels.data();
    initial.SysMemPitch = decoded.image.width * 4U;
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(graphics.device->CreateTexture2D(&description, &initial, &texture))) {
        static_cast<void>(graphics.device->CreateShaderResourceView(
            texture.Get(), nullptr, &graphics.logo));
    }
}

LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
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
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {720, 500};
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
    const anomaly::Translator& translator) {
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
    const COMDLG_FILTERSPEC filters[]{
        {L"NTE Launcher", L"NTELauncher.exe"},
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
    const bool pressed = ImGui::Button(Glyph(glyph), ImVec2(30.0f, 30.0f));
    ImGui::EndDisabled();
    Tooltip(tooltip);
    ImGui::PopID();
    return pressed && enabled;
}

bool CommandButton(
    const char* id, char32_t glyph, const char* label,
    bool primary, bool enabled, ImVec2 size = {}) {
    const std::string text = std::string(Glyph(glyph)) + "  " + label;
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? kAccent : kSurface);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? kAccentHover : kRaised);
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        primary ? ImVec4(0.290f, 0.635f, 0.570f, 1.0f)
                : ImVec4(0.180f, 0.220f, 0.250f, 1.0f));
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        primary ? ImVec4(0.063f, 0.129f, 0.122f, 1.0f) : kMuted);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(text.c_str(), size.x > 0.0f ? size : ImVec2(0.0f, 30.0f));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return pressed && enabled;
}

bool ModeButton(const char* id, const char* label, bool selected, float width) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? ImVec4(0.345f, 0.718f, 0.647f, 0.16f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        selected ? ImVec4(0.345f, 0.718f, 0.647f, 0.26f) : kRaised);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? kAccent : kMuted);
    const bool pressed = ImGui::Button(label, ImVec2(width, 30.0f));
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
    using State = anomaly::launcher::ProxyInstallationState;
    switch (state) {
    case State::Enabled: return kSuccess;
    case State::UpdateAvailable: return kWarning;
    case State::Disabled:
    case State::NotInstalled: return kMuted;
    case State::Conflict:
    case State::Unavailable: return kDanger;
    }
    return kMuted;
}

void DrawHeader(
    Graphics& graphics, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.086f, 0.102f, 0.118f, 1.0f));
    ImGui::BeginChild("LauncherHeader", ImVec2(0.0f, kHeaderHeight), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(16.0f, 13.0f));
    if (graphics.logo != nullptr) {
        ImGui::Image(
            static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(graphics.logo.Get())),
            ImVec2(30.0f, 30.0f));
        ImGui::SameLine(0.0f, 10.0f);
    }
    ImGui::SetCursorPosY(18.0f);
    ImGui::TextUnformatted("AnomalyLauncher");
    const char* state = Text(translator, snapshot.busy
        ? anomaly::MessageId::LauncherStateWorking
        : anomaly::MessageId::LauncherStateReady);
    const ImVec2 state_size = ImGui::CalcTextSize(state);
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - state_size.x - 18.0f, 19.0f));
    ImGui::TextColored(snapshot.busy ? kWarning : kSuccess, "%s", state);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawModes(LauncherMode& mode, const anomaly::Translator& translator) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.078f, 0.090f, 0.102f, 1.0f));
    ImGui::BeginChild(
        "LauncherModes", ImVec2(0.0f, kModeHeight),
        ImGuiChildFlags_AlwaysUseWindowPadding);
    if (ModeButton("proxy-install", Text(translator,
            anomaly::MessageId::LauncherModeProxyInstall),
            mode == LauncherMode::Proxy, 126.0f)) {
        mode = LauncherMode::Proxy;
    }
    ImGui::SameLine();
    if (ModeButton("live-attach", Text(translator,
            anomaly::MessageId::LauncherModeLiveAttach),
            mode == LauncherMode::Attach, 126.0f)) {
        mode = LauncherMode::Attach;
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
    ImGui::SetNextItemWidth((std::max)(120.0f, ImGui::GetContentRegionAvail().x - trailing_width));
    ImGui::InputText(
        id, value.data(), value.size() + 1,
        ImGuiInputTextFlags_ReadOnly);
}

void DrawRecoveryAxis(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator, const char* stable_id,
    anomaly::MessageId label, anomaly::MessageId action,
    anomaly::RuntimeRecoveryAxis axis) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, 28.0f);
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
    const bool active = snapshot.recovery && snapshot.recovery->safe_mode.Active();
    if (!active && snapshot.recovery_message.empty()) return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionRecovery));
    if (!snapshot.recovery_message.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(kDanger, "%s", snapshot.recovery_message.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    const auto& safe_mode = snapshot.recovery->safe_mode;
    ImGui::TextColored(kWarning, "%s",
        Text(translator, anomaly::MessageId::LauncherRecoverySafeModeActive));
    if (!safe_mode.reason.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s", safe_mode.reason.c_str());
    }
    if (ImGui::BeginTable(
            "RecoveryAxes", 2, ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.0f, 0.0f))) {
        const std::string axis_column = StableLabel(translator,
            anomaly::MessageId::LauncherRecoveryAxis, "recovery-axis");
        const std::string action_column = StableLabel(translator,
            anomaly::MessageId::LauncherRecoveryAction, "recovery-action");
        ImGui::TableSetupColumn(axis_column.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(action_column.c_str(), ImGuiTableColumnFlags_WidthFixed, 72.0f);
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

void DrawProxyMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionInstallation));
    ImGui::TextColored(ProxyStateColor(snapshot.proxy.state), "%s",
        Text(translator, ProxyStateMessageId(snapshot.proxy.state)));
    if (snapshot.proxy.state == State::UpdateAvailable) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(kMuted, "%s", Text(
            translator, anomaly::MessageId::LauncherProxyUpdateDescription));
        ImGui::PopTextWrapPos();
    } else if (!snapshot.proxy.message.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(kMuted, "%s", snapshot.proxy.message.c_str());
        ImGui::PopTextWrapPos();
    }

    DrawRecoveryState(controller, snapshot, translator);

    const float action_y = (std::max)(
        ImGui::GetCursorPosY() + 16.0f, ImGui::GetWindowHeight() - 48.0f);
    ImGui::SetCursorPosY(action_y);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kProxyActionLeftPadding);
    const auto action = anomaly::launcher::ProxyInstallationActionForState(
        snapshot.proxy.state);
    bool has_action = true;
    bool action_pressed{};
    switch (action) {
    case anomaly::launcher::ProxyInstallationAction::Install:
        action_pressed = CommandButton(
            "install-proxy", 0xe8b7,
            Text(translator, anomaly::MessageId::CommonInstall), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(132.0f, 32.0f));
        break;
    case anomaly::launcher::ProxyInstallationAction::Enable:
        action_pressed = CommandButton(
            "enable-proxy", 0xe768,
            Text(translator, anomaly::MessageId::CommonEnable), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(132.0f, 32.0f));
        break;
    case anomaly::launcher::ProxyInstallationAction::Disable:
        action_pressed = CommandButton(
            "disable-proxy", 0xe711,
            Text(translator, anomaly::MessageId::CommonDisable), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(132.0f, 32.0f));
        break;
    case anomaly::launcher::ProxyInstallationAction::Update:
        action_pressed = CommandButton(
            "update-proxy", 0xe8b7,
            Text(translator, anomaly::MessageId::CommonUpdate), true,
            !snapshot.busy && !snapshot.game_directory.empty(), ImVec2(132.0f, 32.0f));
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
    if (has_action) ImGui::SameLine();
    if (IconButton(
            "refresh-proxy", 0xe72c,
            Text(translator, anomaly::MessageId::LauncherProxyRefresh),
            !snapshot.busy && !snapshot.game_directory.empty())) {
        controller.RefreshProxy();
    }
}

void DrawAttachMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
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
                window, snapshot.launcher_executable, translator)) {
            controller.SelectLauncherExecutable(*selected);
        }
    }

    ImGui::Spacing();
    if (snapshot.runtime_version.empty()) {
        ImGui::TextColored(kMuted, "%s", snapshot.runtime_message.c_str());
    } else {
        const std::string runtime = Format(translator,
            anomaly::MessageId::LauncherRuntimeVersion, {snapshot.runtime_version});
        ImGui::TextColored(kMuted, "%s", runtime.c_str());
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionProcesses));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 30.0f);
    if (IconButton("refresh-processes", 0xe72c,
            Text(translator, anomaly::MessageId::LauncherProcessRefresh), !snapshot.busy)) {
        controller.RefreshProcesses();
    }

    const float table_height = (std::max)(140.0f, ImGui::GetContentRegionAvail().y - 62.0f);
    if (ImGui::BeginTable(
            "AttachProcesses", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0.0f, table_height))) {
        const std::string process_column = StableLabel(translator,
            anomaly::MessageId::LauncherProcessColumnProcess, "process-column");
        const std::string pid_column = StableLabel(translator,
            anomaly::MessageId::LauncherProcessColumnPid, "pid-column");
        const std::string path_column = StableLabel(translator,
            anomaly::MessageId::LauncherProcessColumnPath, "path-column");
        const std::string state_column = StableLabel(translator,
            anomaly::MessageId::LauncherProcessColumnState, "state-column");
        ImGui::TableSetupColumn(
            process_column.c_str(), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn(
            pid_column.c_str(), ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn(path_column.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            state_column.c_str(), ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();
        for (const auto& process : snapshot.processes) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 30.0f);
            ImGui::TableSetColumnIndex(0);
            const std::string process_label = WideUtf8(process.executable_name);
            ImGui::TextUnformatted(process_label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%lu", process.process_id);
            ImGui::TableSetColumnIndex(2);
            const std::string path = PathUtf8(process.executable_path);
            ImGui::TextUnformatted(path.c_str());
            ImGui::TableSetColumnIndex(3);
            const bool attached = process.process_id == snapshot.attached_process;
            const anomaly::MessageId state = attached
                ? anomaly::MessageId::LauncherProcessStateAttached
                : process.Compatible() ? anomaly::MessageId::LauncherProcessStateDetected
                : process.inspection_error == ERROR_ACCESS_DENIED
                    ? anomaly::MessageId::LauncherProcessStateDenied
                : !process.owned_by_current_user
                    ? anomaly::MessageId::LauncherProcessStateOtherUser
                : !process.x64 ? anomaly::MessageId::LauncherProcessStateNotX64
                               : anomaly::MessageId::LauncherProcessStateUnavailable;
            ImGui::TextColored(
                attached ? kSuccess : process.Compatible() ? kMuted : kDanger,
                "%s", Text(translator, state));
        }
        ImGui::EndTable();
    }

    const bool can_launch = !snapshot.busy && snapshot.core_available &&
        !snapshot.launcher_executable.empty() && snapshot.processes.empty();
    if (CommandButton(
            "launch-attach-core", 0xe768,
            Text(translator, anomaly::MessageId::LauncherLaunchAttach), true, can_launch,
            ImVec2(174.0f, 32.0f))) {
        controller.LaunchAndAttach();
    }
}

void DrawFooter(
    const LauncherSnapshot& snapshot, const anomaly::Translator& translator) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 11.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.075f, 0.090f, 1.0f));
    ImGui::BeginChild(
        "LauncherFooter", ImVec2(0.0f, kFooterHeight),
        ImGuiChildFlags_AlwaysUseWindowPadding);
    const ImVec4 color = snapshot.message_kind == MessageKind::Success ? kSuccess
        : snapshot.message_kind == MessageKind::Error ? kDanger : kMuted;
    const char32_t icon = snapshot.message_kind == MessageKind::Success ? 0xe73e
        : snapshot.message_kind == MessageKind::Error ? 0xe7ba : 0xe72c;
    ImGui::TextColored(color, "%s", Glyph(icon));
    ImGui::SameLine();
    const std::string rendered = RenderMessage(translator, snapshot.message);
    const std::string message = Ellipsize(
        rendered, (std::max)(32.0f, ImGui::GetContentRegionAvail().x));
    ImGui::TextColored(color, "%s", message.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DrawLauncher(
    HWND window, Graphics& graphics, LauncherController& controller, LauncherMode& mode,
    const anomaly::Translator& translator) {
    const LauncherSnapshot snapshot = controller.Snapshot();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(
        "AnomalyLauncherRoot", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawHeader(graphics, snapshot, translator);
    DrawModes(mode, translator);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.078f, 0.090f, 0.102f, 1.0f));
    ImGui::BeginChild(
        "LauncherBody", ImVec2(0.0f, -kFooterHeight),
        ImGuiChildFlags_AlwaysUseWindowPadding);
    if (mode == LauncherMode::Proxy) {
        DrawProxyMode(window, controller, snapshot, translator);
    } else {
        DrawAttachMode(window, controller, snapshot, translator);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    DrawFooter(snapshot, translator);
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
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

    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
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
    const HWND window = CreateWindowExW(
        0, window_class.lpszClassName, window_title.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 860, 600,
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
    ue5mem::ApplyPlatformUiStyle();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    bool running = true;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(window)) {
            Sleep(16);
            continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawLauncher(window, graphics, controller, mode, *translator);
        ImGui::Render();
        constexpr float clear[]{0.078f, 0.090f, 0.102f, 1.0f};
        graphics.context->OMSetRenderTargets(1, graphics.render_target.GetAddressOf(), nullptr);
        graphics.context->ClearRenderTargetView(graphics.render_target.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        graphics.swap_chain->Present(1, 0);
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
