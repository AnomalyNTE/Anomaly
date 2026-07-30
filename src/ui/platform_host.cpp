#include "platform_host.hpp"

#include "anomaly/host_ui_service.hpp"
#include "anomaly/adapter_service_registry.hpp"
#include "anomaly/platform_ui_model.hpp"
#include "anomaly/platform_ui_input_policy.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/plugin_scope.hpp"
#include "anomaly/ui_resource_decoder.hpp"
#include "anomaly/ui_resource_registry.hpp"
#include "anomaly/ue5_nte_adapter.hpp"
#include "plugin_manager.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace ue5mem {
namespace {

struct HostWindow {
    HWND window{};
    HWND target{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* swap_chain{};
    ID3D11RenderTargetView* render_target{};
    ID3D11ShaderResourceView* header_logo{};
    std::shared_ptr<std::string> imgui_ini_path;
    // Keeps the composition-root owner mapped if an in-flight game tick
    // exceeds the bounded host shutdown handoff.
    std::shared_ptr<PluginManager> plugin_owner;
    bool visible{};
    unsigned toggle_key{VK_INSERT};
    bool attached{};
};

HostWindow* g_window{};
ID3D11ShaderResourceView* g_standalone_header_logo{};

// A standalone host can outlive its worker when a game tick is still inside
// plugin code. Keep a process-boundary owner token so the composition root can
// report that generation as quarantined even though the native window itself
// is retained through a raw Win32 handle/global pointer.
struct StandaloneHostQuarantine final {
    std::mutex mutex;
    std::vector<std::shared_ptr<PluginManager>> owners;
};

StandaloneHostQuarantine* GetStandaloneHostQuarantine() noexcept {
    static auto* registry = []() noexcept -> StandaloneHostQuarantine* {
        try {
            return new StandaloneHostQuarantine();
        } catch (...) {
            return nullptr;
        }
    }();
    return registry;
}

void RetainStandaloneHostOwner(const std::shared_ptr<PluginManager>& owner) noexcept {
    if (owner == nullptr) return;
    auto* registry = GetStandaloneHostQuarantine();
    if (registry == nullptr) return;
    try {
        std::lock_guard<std::mutex> lock(registry->mutex);
        const auto found = std::ranges::find_if(registry->owners, [&owner](const auto& retained) {
            return retained.get() == owner.get();
        });
        if (found == registry->owners.end()) registry->owners.push_back(owner);
    } catch (...) {
        // HostWindow::plugin_owner remains a second lifetime fence when the
        // diagnostic registry cannot allocate.
    }
}

bool IsStandaloneHostQuarantined(const PluginManager* owner) noexcept {
    auto* registry = GetStandaloneHostQuarantine();
    if (registry == nullptr) return false;
    std::lock_guard<std::mutex> lock(registry->mutex);
    if (owner == nullptr) return !registry->owners.empty();
    return std::ranges::any_of(registry->owners, [owner](const auto& retained) {
        return retained != nullptr && retained.get() == owner;
    });
}

struct WindowCandidate {
    HWND window{};
    long long area{};
};

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter) {
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId() || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT rectangle{};
    if (!GetClientRect(window, &rectangle)) return TRUE;
    const auto area = static_cast<long long>(rectangle.right - rectangle.left) *
                      static_cast<long long>(rectangle.bottom - rectangle.top);
    auto& candidate = *reinterpret_cast<WindowCandidate*>(parameter);
    if (area > candidate.area) candidate = {window, area};
    return TRUE;
}

HWND FindProcessWindow() {
    WindowCandidate candidate;
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&candidate));
    return candidate.window;
}

bool PlaceAttachedWindow(HostWindow& host) {
    if (!host.attached) return true;
    if (!IsWindow(host.target)) return false;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(host.target, &client) || !ClientToScreen(host.target, &origin)) return false;
    const int client_width = client.right - client.left;
    const int client_height = client.bottom - client.top;
    if (client_width <= 0 || client_height <= 0) return true;
    const int width = std::max(320, std::min(1080, client_width - 32));
    const int height = std::max(240, std::min(720, client_height - 32));
    const int left = origin.x + (client_width - width) / 2;
    const int top = origin.y + (client_height - height) / 2;
    return SetWindowPos(
               host.window, HWND_TOP, left, top, width, height,
               SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
}

void ReleaseRenderTarget(HostWindow& host) {
    if (host.render_target != nullptr) {
        host.render_target->Release();
        host.render_target = nullptr;
    }
}

bool CreateRenderTarget(HostWindow& host) {
    ID3D11Texture2D* back_buffer{};
    if (FAILED(host.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    const HRESULT result = host.device->CreateRenderTargetView(back_buffer, nullptr, &host.render_target);
    back_buffer->Release();
    return SUCCEEDED(result);
}

LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_SIZE:
        if (g_window != nullptr && g_window->device != nullptr && wparam != SIZE_MINIMIZED) {
            // ResizeBuffers requires every backbuffer reference to be unbound.
            // Leaving the RTV on the output-merger can expose the default white
            // client background while Windows resizes the standalone preview.
            g_window->context->OMSetRenderTargets(0, nullptr, nullptr);
            g_window->context->ClearState();
            g_window->context->Flush();
            ReleaseRenderTarget(*g_window);
            if (SUCCEEDED(g_window->swap_chain->ResizeBuffers(
                    0, static_cast<UINT>(LOWORD(lparam)), static_cast<UINT>(HIWORD(lparam)),
                    DXGI_FORMAT_UNKNOWN, 0))) {
                static_cast<void>(CreateRenderTarget(*g_window));
            }
        }
        return 0;
    case WM_ERASEBKGND:
        // The D3D swap chain owns client painting. Suppress a GDI white flash
        // between a native resize and the next rendered frame.
        return 1;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        if (g_window != nullptr) {
            g_window->visible = false;
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void DestroyDevice(HostWindow& host);

bool CreateDevice(HostWindow& host) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = host.window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const std::array<D3D_FEATURE_LEVEL, 2> levels{
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &description, &host.swap_chain,
        &host.device, &selected, &host.context);
    if (FAILED(result) || !CreateRenderTarget(host)) {
        // D3D11CreateDeviceAndSwapChain may have produced a partially usable
        // device before render-target creation fails. Release every interface
        // here so callers can safely destroy only the window/class.
        DestroyDevice(host);
        return false;
    }
    return true;
}

void DestroyDevice(HostWindow& host) {
    ReleaseRenderTarget(host);
    if (host.header_logo != nullptr) {
        if (g_standalone_header_logo == host.header_logo) g_standalone_header_logo = nullptr;
        host.header_logo->Release();
        host.header_logo = nullptr;
    }
    if (host.swap_chain != nullptr) {
        host.swap_chain->Release();
        host.swap_chain = nullptr;
    }
    if (host.context != nullptr) {
        host.context->Release();
        host.context = nullptr;
    }
    if (host.device != nullptr) {
        host.device->Release();
        host.device = nullptr;
    }
}

std::optional<std::uintptr_t> ParseAddress(const char* text) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    std::string_view value(text);
    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
        base = 16;
    }
    std::uintptr_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
        ? std::optional<std::uintptr_t>(result)
        : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> ParseBytes(std::string_view text) {
    std::vector<std::uint8_t> result;
    std::size_t cursor{};
    while (cursor < text.size()) {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor == text.size()) break;
        const auto end = text.find_first_of(" \t\r\n,", cursor);
        const auto token = text.substr(cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
        unsigned value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 0xff) {
            return std::nullopt;
        }
        result.push_back(static_cast<std::uint8_t>(value));
        cursor = end == std::string_view::npos ? text.size() : end + 1;
    }
    return result.empty() ? std::nullopt : std::optional<std::vector<std::uint8_t>>(std::move(result));
}

[[nodiscard]] ImVec2 Offset(const ImVec2 position, const float x, const float y) noexcept {
    return {position.x + x, position.y + y};
}

[[nodiscard]] float AvailableItemWidth(
    const float reserved = 0.0f,
    const float maximum = (std::numeric_limits<float>::max)()) noexcept {
    const float available = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - reserved);
    return (std::min)(available, maximum);
}

[[nodiscard]] ImVec2 FillAvailableSize(
    const float height,
    const float reserved = 0.0f,
    const float maximum = (std::numeric_limits<float>::max)()) noexcept {
    return {AvailableItemWidth(reserved, maximum), height};
}

void SetAvailableItemWidth(
    const float reserved = 0.0f,
    const float maximum = (std::numeric_limits<float>::max)()) noexcept {
    ImGui::SetNextItemWidth(AvailableItemWidth(reserved, maximum));
}

[[nodiscard]] std::string EncodeUtf8(const char32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7fU) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return result;
}

enum class ShellGlyph : std::size_t {
    Package,
    Layers,
    Shield,
    Terminal,
    Settings,
    Refresh,
    More,
    ChevronDown,
    ChevronUp,
    ChevronLeft,
    ChevronRight,
    Pin,
    Search,
    Close,
    Play,
    Stop,
    Eye,
    EyeOff,
    Check,
    Warning,
    Info,
    Copy,
    Activity,
    Count,
};

[[nodiscard]] const char* ShellGlyphText(const ShellGlyph glyph) {
    static const std::array<std::string, static_cast<std::size_t>(ShellGlyph::Count)> glyphs = [] {
        std::array<std::string, static_cast<std::size_t>(ShellGlyph::Count)> values;
        values[static_cast<std::size_t>(ShellGlyph::Package)] = EncodeUtf8(0xe7b8);
        values[static_cast<std::size_t>(ShellGlyph::Layers)] = EncodeUtf8(0xe81e);
        values[static_cast<std::size_t>(ShellGlyph::Shield)] = EncodeUtf8(0xea18);
        values[static_cast<std::size_t>(ShellGlyph::Terminal)] = EncodeUtf8(0xe756);
        values[static_cast<std::size_t>(ShellGlyph::Settings)] = EncodeUtf8(0xe713);
        values[static_cast<std::size_t>(ShellGlyph::Refresh)] = EncodeUtf8(0xe72c);
        values[static_cast<std::size_t>(ShellGlyph::More)] = EncodeUtf8(0xe712);
        values[static_cast<std::size_t>(ShellGlyph::ChevronDown)] = EncodeUtf8(0xe70d);
        values[static_cast<std::size_t>(ShellGlyph::ChevronUp)] = EncodeUtf8(0xe70e);
        values[static_cast<std::size_t>(ShellGlyph::ChevronLeft)] = EncodeUtf8(0xe76b);
        values[static_cast<std::size_t>(ShellGlyph::ChevronRight)] = EncodeUtf8(0xe76c);
        values[static_cast<std::size_t>(ShellGlyph::Pin)] = EncodeUtf8(0xe718);
        values[static_cast<std::size_t>(ShellGlyph::Search)] = EncodeUtf8(0xe721);
        values[static_cast<std::size_t>(ShellGlyph::Close)] = EncodeUtf8(0xe711);
        values[static_cast<std::size_t>(ShellGlyph::Play)] = EncodeUtf8(0xe768);
        values[static_cast<std::size_t>(ShellGlyph::Stop)] = EncodeUtf8(0xe71a);
        values[static_cast<std::size_t>(ShellGlyph::Eye)] = EncodeUtf8(0xe890);
        values[static_cast<std::size_t>(ShellGlyph::EyeOff)] = EncodeUtf8(0xe8a9);
        values[static_cast<std::size_t>(ShellGlyph::Check)] = EncodeUtf8(0xe73e);
        values[static_cast<std::size_t>(ShellGlyph::Warning)] = EncodeUtf8(0xe7ba);
        values[static_cast<std::size_t>(ShellGlyph::Info)] = EncodeUtf8(0xe946);
        values[static_cast<std::size_t>(ShellGlyph::Copy)] = EncodeUtf8(0xe8c8);
        values[static_cast<std::size_t>(ShellGlyph::Activity)] = EncodeUtf8(0xe9d9);
        return values;
    }();
    return glyphs[static_cast<std::size_t>(glyph)].c_str();
}

constexpr std::string_view kPlatformUiWindowOwner = "anomaly.host.platform-ui";
constexpr std::string_view kPlatformUiWindowId = "management-shell";
constexpr std::uint64_t kPlatformUiWindowGeneration = 1;
constexpr int kPlatformLogoResourceId = 101;
constexpr float kPlatformHeaderHeight = 52.0f;
constexpr float kPlatformHeaderLogoSize = 30.0f;
constexpr float kPlatformHeaderActionColumnWidth = 110.0f;
constexpr float kPlatformToastHeight = 38.0f;
constexpr float kPlatformToastBottomMargin = 14.0f;
constexpr float kPlatformGloballyCollapsedWidth = 132.0f;
constexpr float kPlatformMinimumShellWidth = 760.0f;
constexpr float kPlatformMinimumShellHeight = 500.0f;
constexpr float kPlatformShellViewportMargin = 12.0f;

struct ContactInformation final {
    std::string repository_label;
    std::string repository_url;
    std::string qq_group;
};

[[nodiscard]] std::optional<ContactInformation> LoadContactInformation(
    const std::filesystem::path& path) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        const nlohmann::json document = nlohmann::json::parse(input);
        if (document.at("schemaVersion").get<int>() != 1) return std::nullopt;
        const nlohmann::json& repository = document.at("repository");
        ContactInformation contact{
            repository.at("label").get<std::string>(),
            repository.at("url").get<std::string>(),
            document.at("qqGroup").get<std::string>(),
        };
        if (contact.repository_label.empty() || contact.repository_url.empty() ||
            contact.qq_group.empty()) {
            return std::nullopt;
        }
        return contact;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::vector<std::uint8_t> LoadEmbeddedLogoBytes() noexcept {
    try {
        const auto module = reinterpret_cast<HMODULE>(&__ImageBase);
        const HRSRC resource = FindResourceW(
            module, MAKEINTRESOURCEW(kPlatformLogoResourceId), RT_RCDATA);
        if (resource == nullptr) return {};
        const HGLOBAL loaded = LoadResource(module, resource);
        const DWORD size = SizeofResource(module, resource);
        const void* const bytes = loaded == nullptr ? nullptr : LockResource(loaded);
        if (bytes == nullptr || size == 0) return {};
        const auto* const first = static_cast<const std::uint8_t*>(bytes);
        return {first, first + size};
    } catch (...) {
        return {};
    }
}

[[nodiscard]] bool StartsWith(
    const std::string_view value, const std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool OpenExternalUrl(const std::string& url) noexcept {
    if (!StartsWith(url, "https://") && !StartsWith(url, "http://")) return false;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, url.c_str(), static_cast<int>(url.size()),
        nullptr, 0);
    if (required <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, url.c_str(), static_cast<int>(url.size()),
            wide.data(), required) != required) {
        return false;
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
               nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

// The game path uploads this host-owned image through the D3D12 resource
// backend. The standalone D3D11 host has no generic resource backend, so keep
// an equivalent local shader-resource view for the same embedded bytes.
[[nodiscard]] bool CreateStandaloneHeaderLogo(HostWindow& host) noexcept {
    try {
        if (host.device == nullptr) return false;
        const std::vector<std::uint8_t> encoded = LoadEmbeddedLogoBytes();
        const anomaly::UiImageDecodeResult decoded = anomaly::DecodeUiImageRgba8(encoded);
        if (!decoded || decoded.image.width == 0 || decoded.image.height == 0 ||
            decoded.image.pixels.empty()) {
            return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = decoded.image.width;
        description.Height = decoded.image.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = decoded.image.pixels.data();
        data.SysMemPitch = decoded.image.width * 4U;

        ID3D11Texture2D* texture{};
        if (FAILED(host.device->CreateTexture2D(&description, &data, &texture)) || texture == nullptr) {
            return false;
        }
        ID3D11ShaderResourceView* view{};
        const HRESULT result = host.device->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();
        if (FAILED(result) || view == nullptr) return false;

        if (host.header_logo != nullptr) host.header_logo->Release();
        host.header_logo = view;
        g_standalone_header_logo = view;
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool DrawStandaloneHeaderLogo(const float width, const float height) noexcept {
    if (g_standalone_header_logo == nullptr) return false;
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(g_standalone_header_logo)),
        ImVec2(width, height));
    return true;
}

anomaly::UiWindowRequest PlatformUiWindowRequest() {
    anomaly::UiWindowRequest request;
    request.id = std::string(kPlatformUiWindowId);
    request.title = "Anomaly Plugin Platform";
    request.persist_settings = true;
    request.initial_width = 1040.0F;
    request.initial_height = 700.0F;
    // Viewport-specific minimum and maximum dimensions are applied while
    // drawing. Persisting them here would reject a valid size on a smaller
    // display or after a DPI change.
    request.constraints = {};
    request.default_open = true;
    return request;
}

class PlatformUi final : public std::enable_shared_from_this<PlatformUi> {
public:
    PlatformUi(
        PluginManager& plugins,
        PlatformDiagnostics diagnostics,
        anomaly::PlatformUiState state = {},
        std::shared_ptr<PluginManager> plugin_owner = {})
        : plugins_(plugins), plugin_owner_(std::move(plugin_owner)),
          management_window_scope_(std::make_shared<anomaly::PluginScope>(
              std::make_shared<anomaly::ResourceLedger>(),
              std::string(kPlatformUiWindowOwner), kPlatformUiWindowGeneration)),
          diagnostics_(std::move(diagnostics)),
          contact_(LoadContactInformation(diagnostics_.runtime_root / L"CONTACT")) {
        if (diagnostics_.translator == nullptr) {
            diagnostics_.translator =
                anomaly::ParseHostCatalog(anomaly::Locale::EnUs).translator;
        }
        anomaly::SetHostUiDeveloperMode(false);
        // Keep route, tab and selection outside the renderer-owned ImGui
        // context so a device rebuild does not reset the management surface.
        model_.State() = std::move(state);
        SyncInputBuffers();
        anomaly::UiTextureRequest logo_request;
        logo_request.encoded_bytes = LoadEmbeddedLogoBytes();
        logo_request.format = anomaly::UiTextureFormat::Auto;
        if (!logo_request.encoded_bytes.empty()) {
            logo_texture_ = plugins_.UiResources().RequestTexture(
                management_window_scope_, std::move(logo_request));
            if (logo_texture_) {
                static_cast<void>(plugins_.QueueUiTextureLoad(
                    management_window_scope_, logo_texture_));
            }
        }
        const std::filesystem::path directory = plugins_.Directory();
        const anomaly::Locale locale = diagnostics_.translator->locale();
        catalog_worker_ = std::jthread([this, directory, locale](std::stop_token stop_token) {
            while (!stop_token.stop_requested()) {
                try {
                    auto next = std::make_shared<CatalogCache>();
                    next->catalog = anomaly::DiscoverPluginCatalog(directory);
                    next->dependencies = anomaly::ResolvePluginDependencies(*next->catalog);
                    for (const auto& entry : next->catalog->Entries()) {
                        if (!entry.manifest) continue;
                        const auto localization = anomaly::LoadPluginCatalog(
                            locale, entry.package_root);
                        if (localization.catalog == nullptr) continue;
                        auto name = localization.catalog->Translate(
                            "window.title", entry.manifest->name, {});
                        if (!name.text.empty()) {
                            next->display_names.emplace(entry.manifest->id, std::move(name.text));
                        }
                        auto description = localization.catalog->Translate(
                            "plugin.description", entry.manifest->description, {});
                        if (!description.text.empty()) {
                            next->descriptions.emplace(
                                entry.manifest->id, std::move(description.text));
                        }
                    }
                    {
                        std::scoped_lock lock(catalog_mutex_);
                        catalog_cache_ = std::move(next);
                    }
                } catch (...) {
                    // Keep the last good catalog visible when a package is being
                    // replaced or a manifest is temporarily unreadable.
                }
                for (int tick = 0; tick != 10 && !stop_token.stop_requested(); ++tick) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
        management_window_ = plugins_.UiResources().RegisterWindow(
            management_window_scope_, PlatformUiWindowRequest());
        if (management_window_) {
            if (const auto window = plugins_.UiResources().WindowState(
                    management_window_scope_, management_window_)) {
                if (!window->open) anomaly::SetHostUiMenusCollapsed(true);
                if (window->width > 0.0F && window->height > 0.0F) {
                    management_shell_expanded_size_ = {window->width, window->height};
                }
            }
        }
    }

    ~PlatformUi() {
        catalog_worker_.request_stop();
        RevokeManagementWindow();
    }

    [[nodiscard]] bool Ready() const noexcept { return static_cast<bool>(management_window_); }

    [[nodiscard]] bool Reveal() noexcept {
        std::scoped_lock lifetime_lock(lifetime_mutex_);
        if (closing_) return false;
        try {
            const auto window = plugins_.UiResources().WindowState(
                management_window_scope_, management_window_);
            return window && !window->open && plugins_.UiResources().OpenWindow(
                management_window_scope_, management_window_);
        } catch (...) {
            return false;
        }
    }

    void ExpandManagementShell() noexcept {
        std::scoped_lock operation_lock(operation_mutex_);
        management_shell_collapsed_ = false;
    }

    [[nodiscard]] bool BelongsTo(const PluginManager& plugins) const noexcept {
        return &plugins_ == &plugins;
    }

    [[nodiscard]] bool CapturingSettingsHotkey() {
        std::scoped_lock operation_lock(operation_mutex_);
        return settings_hotkey_capture_;
    }

    [[nodiscard]] bool CanReap() const noexcept {
        std::scoped_lock lock(lifetime_mutex_);
        return active_callbacks_ == 0 && outstanding_invocations_ == 0;
    }

    [[nodiscard]] bool Closing() const noexcept {
        std::scoped_lock lock(lifetime_mutex_);
        return closing_;
    }

    void Quarantine() noexcept {
        anomaly::SetHostUiDeveloperMode(false);
        {
            std::scoped_lock lock(lifetime_mutex_);
            closing_ = true;
        }
        catalog_worker_.request_stop();
        RevokeManagementWindow();
    }

    [[nodiscard]] anomaly::PlatformUiState State() const {
        std::scoped_lock lock(operation_mutex_);
        return model_.State();
    }

    void Flush() {
        std::scoped_lock submission_lock(submission_mutex_);
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (closing_) return;
        }
        FlushActions();
    }

    // Stop accepting new callbacks before teardown. A shared owner captured by
    // an already-running callback keeps this object alive until it returns.
    struct ShutdownResult final {
        std::optional<anomaly::PlatformUiState> state;
        bool callbacks_drained{};
    };

    [[nodiscard]] ShutdownResult BeginShutdown(
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(5)) {
        anomaly::SetHostUiDeveloperMode(false);
        const auto bounded_timeout = (std::max)(drain_timeout, std::chrono::milliseconds::zero());
        const auto deadline = bounded_timeout == std::chrono::milliseconds::max()
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + bounded_timeout;
        const auto remaining = [&] {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                return std::chrono::milliseconds::max();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::chrono::milliseconds::zero();
            return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        };
        {
            // Serialize with a frame that is already drawing or flushing,
            // then release the submission gate before waiting on lifecycle
            // callbacks. A callback may re-enter the public Flush entrypoint;
            // keeping this mutex held while draining would deadlock it.
            std::scoped_lock submission_lock(submission_mutex_);
            std::scoped_lock lock(lifetime_mutex_);
            closing_ = true;
        }
        // A retired owner must not keep scanning the package directory while
        // its last lifecycle callback is being drained. The owner itself is
        // retained by the global handoff below until this method succeeds.
        catalog_worker_.request_stop();
        if (diagnostics_.lifecycle_drain) {
            const bool drained = diagnostics_.lifecycle_drain(remaining());
            if (!drained) {
                // A callback may be executing plugin code beyond the host
                // deadline. Keep this owner as a quarantine fence and let the
                // renderer release its current ImGui/D3D generation; the
                // owner and plugin manager remain mapped until callback drain
                // is observed by a later process boundary.
                return QuarantineResult();
            }
        }
        // Submission is closed before this point, so no new Draw/Flush call
        // can acquire the operation lock. Wait for callback scopes first;
        // callbacks release operation_mutex_ before taking lifetime_mutex_, so
        // taking the locks in the opposite order here would deadlock teardown.
        {
            std::unique_lock lifetime_lock(lifetime_mutex_);
            if (!lifetime_condition_.wait_for(
                    lifetime_lock, remaining(), [this] {
                        return active_callbacks_ == 0 && outstanding_invocations_ == 0;
                    })) {
                return QuarantineResult();
            }
        }
        std::scoped_lock operation_lock(operation_mutex_);
        CancelOutstandingOperations();
        CancelMemoryWork();
        RevokeManagementWindow();
        return {model_.State(), true};
    }

    void Draw() {
        std::scoped_lock submission_lock(submission_mutex_);
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (closing_) return;
        }
        std::scoped_lock operation_lock(operation_mutex_);
        const auto window = plugins_.UiResources().WindowState(
            management_window_scope_, management_window_);
        if (!window || !window->open) return;
        RefreshSnapshot();
        UpdateStatusToast();
        const AnomalyUiServiceV1* ui = anomaly::HostUiServiceTable();
        if (ui == nullptr || ui->set_next_window_size == nullptr || ui->begin_window == nullptr ||
            ui->end_window == nullptr || ui->set_next_window_size_constraints == nullptr ||
            ui->get_window_size == nullptr) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;
        // Keep a consistent margin around the responsive product surface and
        // cap it to the preview's 1560x900 desktop bounds. The registry
        // supplies the persisted expanded size after first use.
        const float maximum_shell_width =
            std::min(1560.0f,
                std::max(0.0f, io.DisplaySize.x - 2.0f * kPlatformShellViewportMargin));
        const float maximum_shell_height =
            std::min(900.0f,
                std::max(0.0f, io.DisplaySize.y - 2.0f * kPlatformShellViewportMargin));
        const float minimum_shell_width =
            std::min(kPlatformMinimumShellWidth, maximum_shell_width);
        const float minimum_shell_height =
            std::min(kPlatformMinimumShellHeight, maximum_shell_height);
        const ImVec2 default_expanded_size(maximum_shell_width, maximum_shell_height);
        ImVec2 expanded_size = management_shell_expanded_size_;
        if (expanded_size.x <= 0.0f || expanded_size.y <= 0.0f) {
            expanded_size = default_expanded_size;
        }
        expanded_size.x = std::clamp(expanded_size.x, minimum_shell_width, maximum_shell_width);
        expanded_size.y = std::clamp(expanded_size.y, minimum_shell_height, maximum_shell_height);
        const bool globally_collapsed = ManagementShellGloballyCollapsed();
        const bool shell_collapsed = ManagementShellCollapsed();
        const bool restoring_from_collapse = management_shell_was_collapsed_ && !shell_collapsed;
        const float shell_width = globally_collapsed
            ? std::min(kPlatformGloballyCollapsedWidth, maximum_shell_width)
            : expanded_size.x;
        const float shell_height = shell_collapsed
            ? std::min(kPlatformHeaderHeight, maximum_shell_height) : expanded_size.y;
        ImGui::SetNextWindowPos(
            ImVec2(kPlatformShellViewportMargin, kPlatformShellViewportMargin),
            ImGuiCond_FirstUseEver);
        if (globally_collapsed) {
            ui->set_next_window_size_constraints(
                ui->user, shell_width, shell_height, shell_width, shell_height);
            ui->set_next_window_size(
                ui->user, shell_width, shell_height, static_cast<std::uint32_t>(ImGuiCond_Always));
        } else if (management_shell_collapsed_) {
            ui->set_next_window_size_constraints(
                ui->user, minimum_shell_width, shell_height, maximum_shell_width, shell_height);
            ui->set_next_window_size(
                ui->user, expanded_size.x, shell_height,
                static_cast<std::uint32_t>(ImGuiCond_Always));
        } else {
            ui->set_next_window_size_constraints(
                ui->user, minimum_shell_width, minimum_shell_height,
                maximum_shell_width, maximum_shell_height);
            if (management_shell_apply_initial_size_ || restoring_from_collapse) {
                ui->set_next_window_size(
                    ui->user, expanded_size.x, expanded_size.y,
                    static_cast<std::uint32_t>(ImGuiCond_Always));
            }
        }
        const std::string title = window->title + "###" + window->stable_id;
        // Keep the native host title bar only. The shell owns its outer border,
        // its 4px layout grid, and every internal fixed-height band.
        const float interface_alpha = settings_draft_
            ? static_cast<float>(settings_draft_->interface_opacity_percent) / 100.0f
            : 1.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, interface_alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ui->begin_window(
            ui->user, {title.data(), title.size()}, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse) != 0;
        ImGui::PopStyleVar();
        if (visible) {
            management_shell_locked_ = anomaly::HostUiCurrentWindowLocked();
            UpdateLayout();
            HandleShellShortcuts();
            const bool shell_was_collapsed = ManagementShellCollapsed();
            DrawManagementShell();
            if (!shell_was_collapsed && !ManagementShellCollapsed()) {
                DrawConfirmationPopup();
                DrawRepositoryUninstallPopup();
                DrawOperationDetailsPopup();
                DrawMemoryConfirmationPopup();
                DrawSettingsLeavePopup();
            }
            DrawStatusToast();
        }
        if (!shell_collapsed && !ManagementShellCollapsed()) {
            float width{};
            float height{};
            ui->get_window_size(ui->user, &width, &height);
            if (width > 0.0f && height > 0.0f) {
                management_shell_expanded_size_ = {width, height};
            }
            static_cast<void>(plugins_.UiResources().SetWindowSize(
                management_window_scope_, management_window_, width, height));
        }
        if (!globally_collapsed) management_shell_apply_initial_size_ = false;
        management_shell_was_collapsed_ = shell_collapsed;
        ui->end_window(ui->user);
        ImGui::PopStyleVar();

        // Mutations are flushed by FlushPlatformUiActions after the render
        // lock is released. Draw itself only consumes a snapshot and queues
        // typed intents.
    }

    void PrepareResources() noexcept {
        std::scoped_lock submission_lock(submission_mutex_);
        std::shared_ptr<anomaly::PluginScope> scope;
        anomaly::UiResourceHandle texture;
        {
            std::scoped_lock lifetime_lock(lifetime_mutex_);
            if (closing_) return;
            scope = management_window_scope_;
            texture = logo_texture_;
        }
        plugins_.PrepareUiTexture(scope, texture);
    }

private:
    using Route = anomaly::PlatformUiRoute;
    using Tab = anomaly::PlatformUiPluginTab;
    using Filter = anomaly::PlatformUiPluginFilter;
    using Sort = anomaly::PlatformUiPluginSort;
    using DiagnosticTab = anomaly::PlatformUiDiagnosticsTab;
    using Intent = anomaly::PlatformUiIntent;
    using Mutation = anomaly::PlatformUiPluginMutation;

    enum class IntentExecutionState : std::uint8_t {
        Pending,
        Running,
        Settled,
    };

    enum class LayoutMode : std::uint8_t {
        Wide,
        Standard,
        Compact,
    };

    enum class DeveloperPanel : std::uint8_t {
        Plugins,
        Services,
        Hooks,
        Memory,
        NteProfile,
    };

    enum class DeveloperPluginTab : std::uint8_t {
        Overview,
        Capabilities,
        Performance,
        Logs,
    };

    enum class SettingsSection : std::uint8_t {
        Interface,
        Input,
        Updates,
        Diagnostics,
        Advanced,
        About,
    };

    // One editable row in the third-party plugin source editor. The URL lives
    // in a fixed buffer because the host does not vendor imgui_stdlib.
    struct RepositoryChannelRow {
        std::array<char, 1024> url{};
        bool enabled{true};
    };

    struct SettingsApplyMailbox final {
        std::mutex mutex;
        std::optional<anomaly::PlatformSettingsApplyResult> result;
    };

    struct RepositoryConfigureResult final {
        anomaly::PluginRepositoryConfig config;
        anomaly::RepositoryOperationSubmission submission;
    };

    struct RepositoryConfigureMailbox final {
        std::mutex mutex;
        std::optional<RepositoryConfigureResult> result;
    };

    struct QueuedIntent final {
        Intent intent;
        std::uint64_t operation_id{};
        std::vector<anomaly::AffectedPlugin> affected;
        std::map<std::string, std::uint64_t, std::less<>> generations;
        std::shared_ptr<std::atomic<IntentExecutionState>> execution_state;
    };

    // Keeps the PlatformUi owner alive for the complete dispatcher invocation,
    // including the interval after Invoke reports a timeout but before the
    // queued callback is finally cancelled or completes.
    struct InvocationGuard final {
        std::shared_ptr<PlatformUi> owner;
        std::function<void()> on_abandon;
        std::atomic_bool callback_started{};
        ~InvocationGuard() {
            if (!callback_started.load(std::memory_order_acquire) && on_abandon) {
                try {
                    on_abandon();
                } catch (...) {
                }
            }
            if (owner != nullptr) owner->LeaveInvocation();
        }

        void MarkCallbackStarted() noexcept {
            callback_started.store(true, std::memory_order_release);
        }
    };

    struct CatalogCache final {
        std::optional<anomaly::PluginCatalogSnapshot> catalog;
        std::optional<anomaly::PluginDependencyPlan> dependencies;
        anomaly::PluginDisplayNameMap display_names;
        anomaly::PluginDescriptionMap descriptions;
    };

    struct AwaitingBatch final {
        QueuedIntent queued;
        std::map<std::string, std::uint64_t, std::less<>> generations;
        struct Observation final {
            bool present{};
            std::uint64_t generation{};
            anomaly::PlatformUiPluginState state{anomaly::PlatformUiPluginState::Unknown};
            bool enabled{};

            friend bool operator==(const Observation&, const Observation&) = default;
        };
        std::map<std::string, Observation, std::less<>> last_observations;
        bool observed_once{};
        std::chrono::steady_clock::time_point deadline{};
    };

    struct PendingMemoryWrite final {
        std::uintptr_t address{};
        std::vector<std::uint8_t> bytes;
        bool patch{};
    };

    struct PerformanceRow final {
        const anomaly::InstalledPluginView* plugin{};
        const CallbackMetricsView* metrics{};
        bool update{};
    };

    [[nodiscard]] const char* Text(const anomaly::MessageId id) const noexcept {
        return diagnostics_.translator->Text(id).data();
    }

    [[nodiscard]] std::string Format(
        const anomaly::MessageId id,
        const std::span<const std::string_view> arguments) const {
        return diagnostics_.translator->Format(id, arguments);
    }

    [[nodiscard]] std::string StableLabel(
        const anomaly::MessageId id, const std::string_view stable_id) const {
        return anomaly::StableDisplayLabel(diagnostics_.translator->Text(id), stable_id);
    }

    const char* RouteLabel(Route route) const noexcept {
        switch (route) {
        case Route::Plugins: return Text(anomaly::MessageId::ShellRoutePlugins);
        case Route::NteCompatibility:
            return Text(anomaly::MessageId::ShellRouteNteCompatibility);
        case Route::Diagnostics: return Text(anomaly::MessageId::ShellRouteDiagnostics);
        case Route::Settings: return Text(anomaly::MessageId::ShellRouteSettings);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    const char* PluginTabLabel(Tab tab) const noexcept {
        switch (tab) {
        case Tab::Installed: return Text(anomaly::MessageId::PluginsTabInstalled);
        case Tab::Available: return Text(anomaly::MessageId::PluginsTabAvailable);
        case Tab::Updates: return Text(anomaly::MessageId::PluginsTabUpdates);
        case Tab::ThirdParty: return Text(anomaly::MessageId::PluginsTabThirdParty);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    const char* FilterLabel(Filter filter) const noexcept {
        switch (filter) {
        case Filter::All: return Text(anomaly::MessageId::PluginsFilterAll);
        case Filter::Running: return Text(anomaly::MessageId::PluginsFilterRunning);
        case Filter::Disabled: return Text(anomaly::MessageId::PluginsFilterDisabled);
        case Filter::Issues: return Text(anomaly::MessageId::PluginsFilterIssues);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    const char* SortLabel(Sort sort) const noexcept {
        switch (sort) {
        case Sort::Name: return Text(anomaly::MessageId::PluginsSortName);
        case Sort::State: return Text(anomaly::MessageId::PluginsSortState);
        case Sort::Author: return Text(anomaly::MessageId::PluginsSortAuthor);
        }
        return Text(anomaly::MessageId::PluginsSortName);
    }

    const char* DiagnosticTabLabel(DiagnosticTab tab) const noexcept {
        switch (tab) {
        case DiagnosticTab::Overview: return Text(anomaly::MessageId::DiagnosticsTabOverview);
        case DiagnosticTab::PluginPerformance:
            return Text(anomaly::MessageId::DiagnosticsTabPluginPerformance);
        case DiagnosticTab::Logs: return Text(anomaly::MessageId::DiagnosticsTabLogs);
        case DiagnosticTab::Developer: return Text(anomaly::MessageId::DiagnosticsTabDeveloper);
        }
        return Text(anomaly::MessageId::DiagnosticsTabOverview);
    }

    const char* ServiceStateName(anomaly::ServiceState state) const noexcept {
        switch (state) {
        case anomaly::ServiceState::Registered:
            return Text(anomaly::MessageId::ServiceStateRegistered);
        case anomaly::ServiceState::Starting:
            return Text(anomaly::MessageId::ServiceStateStarting);
        case anomaly::ServiceState::Ready: return Text(anomaly::MessageId::ServiceStateReady);
        case anomaly::ServiceState::Degraded:
            return Text(anomaly::MessageId::ServiceStateDegraded);
        case anomaly::ServiceState::Failed: return Text(anomaly::MessageId::ServiceStateFailed);
        case anomaly::ServiceState::Stopping:
            return Text(anomaly::MessageId::ServiceStateStopping);
        case anomaly::ServiceState::Stopped: return Text(anomaly::MessageId::ServiceStateStopped);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    const char* NteLevelName(anomaly::NteCompatibilityLevel level) const noexcept {
        switch (level) {
        case anomaly::NteCompatibilityLevel::Unknown:
            return Text(anomaly::MessageId::CommonUnknown);
        case anomaly::NteCompatibilityLevel::CoreOnly:
            return Text(anomaly::MessageId::NteLevelCoreOnly);
        case anomaly::NteCompatibilityLevel::Partial:
            return Text(anomaly::MessageId::NteLevelPartial);
        case anomaly::NteCompatibilityLevel::Supported:
            return Text(anomaly::MessageId::NteLevelSupported);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    static ImVec4 StateColor(anomaly::PlatformUiPluginState state) noexcept {
        switch (state) {
        case anomaly::PlatformUiPluginState::Active:
            return ImVec4(0.25f, 0.83f, 0.52f, 1.0f);
        case anomaly::PlatformUiPluginState::Disabled:
            return ImVec4(0.66f, 0.68f, 0.70f, 1.0f);
        case anomaly::PlatformUiPluginState::Loaded:
            return ImVec4(0.35f, 0.70f, 0.90f, 1.0f);
        default:
            return ImVec4(0.95f, 0.61f, 0.24f, 1.0f);
        }
    }

    static anomaly::PluginOperationReason OperationReasonForState(
        anomaly::PlatformUiPluginState state) noexcept {
        switch (state) {
        case anomaly::PlatformUiPluginState::Disabled:
            return anomaly::PluginOperationReason::Disabled;
        case anomaly::PlatformUiPluginState::Rejected:
            return anomaly::PluginOperationReason::Rejected;
        case anomaly::PlatformUiPluginState::Incompatible:
            return anomaly::PluginOperationReason::Incompatible;
        case anomaly::PlatformUiPluginState::DependencyBlocked:
            return anomaly::PluginOperationReason::DependencyBlocked;
        case anomaly::PlatformUiPluginState::WaitingForService:
            return anomaly::PluginOperationReason::ProviderUnavailable;
        case anomaly::PlatformUiPluginState::Faulted:
        case anomaly::PlatformUiPluginState::Quarantined:
        case anomaly::PlatformUiPluginState::Stopping:
        case anomaly::PlatformUiPluginState::Unknown:
            return anomaly::PluginOperationReason::BackendFailure;
        case anomaly::PlatformUiPluginState::Active:
        case anomaly::PlatformUiPluginState::Loaded:
            return anomaly::PluginOperationReason::None;
        }
        return anomaly::PluginOperationReason::BackendFailure;
    }

    static bool IsHealthyPluginState(anomaly::PlatformUiPluginState state) noexcept {
        return state == anomaly::PlatformUiPluginState::Active ||
            state == anomaly::PlatformUiPluginState::Loaded;
    }

    static bool TryClaimIntent(const QueuedIntent& queued) noexcept {
        if (queued.execution_state == nullptr) return true;
        auto expected = IntentExecutionState::Pending;
        return queued.execution_state->compare_exchange_strong(
            expected, IntentExecutionState::Running,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    static bool TrySettleIntent(
        const QueuedIntent& queued, IntentExecutionState expected_state) noexcept {
        if (queued.execution_state == nullptr) return true;
        auto expected = expected_state;
        return queued.execution_state->compare_exchange_strong(
            expected, IntentExecutionState::Settled,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] std::shared_ptr<InvocationGuard> BeginInvocation() {
        const auto self = shared_from_this();
        auto guard = std::make_shared<InvocationGuard>();
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (closing_) return {};
            ++outstanding_invocations_;
        }
        guard->owner = self;
        return guard;
    }

    void LeaveInvocation() noexcept {
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (outstanding_invocations_ != 0) --outstanding_invocations_;
        }
        lifetime_condition_.notify_all();
    }

    [[nodiscard]] bool EnterCallback(
        std::shared_ptr<QueuedIntent> rejected = {}) noexcept {
        std::scoped_lock lock(lifetime_mutex_);
        if (closing_) {
            if (rejected != nullptr) {
                try {
                    rejected_callbacks_.push_back(std::move(rejected));
                } catch (...) {
                    // The owner is already closing. A rejected intent has no
                    // executable callback left; leave its shared execution
                    // state for the dispatcher completion path.
                }
            }
            return false;
        }
        ++active_callbacks_;
        return true;
    }

    void LeaveCallback() noexcept {
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (active_callbacks_ != 0) --active_callbacks_;
        }
        lifetime_condition_.notify_all();
    }

    void ExecuteIntentCallback(const std::shared_ptr<QueuedIntent>& pending) {
        if (!EnterCallback(pending)) return;
        struct CallbackScope final {
            PlatformUi* owner;
            ~CallbackScope() { owner->LeaveCallback(); }
        } scope{this};
        std::scoped_lock operation_lock(operation_mutex_);
        ExecuteIntent(*pending);
    }

    void FlushMemoryCallback() {
        if (!EnterCallback()) {
            CancelMemoryWork();
            return;
        }
        struct CallbackScope final {
            PlatformUi* owner;
            ~CallbackScope() { owner->LeaveCallback(); }
        } scope{this};
        struct MemoryInvocationScope final {
            PlatformUi* owner;
            ~MemoryInvocationScope() { owner->CompleteMemoryInvocation(); }
        } memory_scope{this};
        std::scoped_lock operation_lock(operation_mutex_);
        FlushDeferredMemory();
    }

    void CompleteMemoryInvocation() noexcept {
        std::scoped_lock operation_lock(operation_mutex_);
        memory_invocation_pending_ = false;
    }

    void CancelOutstandingOperations() {
        // The caller owns operation_mutex_. The recursive mutex also keeps
        // ApplyIntentFailure safe when it publishes cancellation results.
        std::vector<QueuedIntent> queued;
        std::vector<AwaitingBatch> batches;
        std::vector<std::shared_ptr<QueuedIntent>> rejected;
        queued.swap(queued_intents_);
        batches.swap(awaiting_batches_);
        {
            std::scoped_lock lock(lifetime_mutex_);
            rejected.swap(rejected_callbacks_);
        }
        const auto cancel = [this](const QueuedIntent& item, IntentExecutionState state) {
            static_cast<void>(ApplyIntentFailure(
                item, state, anomaly::PlatformUiResultCode::ProviderUnavailable,
                anomaly::PluginOperationReason::ProviderUnavailable,
                "UI owner closed before the lifecycle operation settled", true));
        };
        for (const auto& item : queued) cancel(item, IntentExecutionState::Pending);
        for (const auto& batch : batches) cancel(batch.queued, IntentExecutionState::Running);
        for (const auto& item : rejected) {
            if (item != nullptr) cancel(*item, IntentExecutionState::Pending);
        }
    }

    [[nodiscard]] ShutdownResult QuarantineResult() {
        RevokeManagementWindow();
        std::optional<anomaly::PlatformUiState> state;
        if (operation_mutex_.try_lock()) {
            state = model_.State();
            operation_mutex_.unlock();
        }
        return {std::move(state), false};
    }

    void RevokeManagementWindow() noexcept {
        if (management_window_scope_ == nullptr) return;
        static_cast<void>(management_window_scope_->FreezeCallbackSources());
        static_cast<void>(management_window_scope_->RevokeAll());
    }

    void SyncInputBuffers() {
        std::snprintf(search_.data(), search_.size(), "%s", model_.State().search.c_str());
        std::snprintf(log_filter_.data(), log_filter_.size(), "%s",
            model_.State().diagnostics_log_filter.c_str());
    }

    void RefreshCatalog() {
        std::shared_ptr<const CatalogCache> cached;
        {
            std::scoped_lock lock(catalog_mutex_);
            cached = catalog_cache_;
        }
        if (!cached || !cached->catalog || !cached->dependencies) return;
        catalog_ = cached->catalog;
        dependencies_ = cached->dependencies;
        plugin_display_names_ = cached->display_names;
        plugin_descriptions_ = cached->descriptions;
        catalog_ready_ = true;
    }

    void RefreshSnapshot() {
        RefreshCatalog();
        const auto runtime_plugins = plugins_.Plugins();
        std::map<std::string, anomaly::PluginEnablementDecision, std::less<>> enablement;
        for (const auto& plugin : runtime_plugins) {
            enablement.emplace(plugin.id, anomaly::PluginEnablementDecision{
                plugin.enabled, true, plugin.enabled ? "enabled by runtime" : "disabled by configuration"});
        }

        anomaly::PlatformUiSnapshot snapshot;
        if (catalog_ready_) {
            snapshot = anomaly::BuildPlatformUiSnapshot(
                ++revision_, runtime_plugins, *catalog_, *dependencies_,
                enablement, plugin_display_names_, plugin_descriptions_);
        } else {
            snapshot = anomaly::BuildPlatformUiSnapshot(++revision_, runtime_plugins);
        }
        snapshot.diagnostics.runtime_version = diagnostics_.runtime_version;
        snapshot.diagnostics.process_id = GetCurrentProcessId();
        snapshot.diagnostics.healthy = true;
        if (diagnostics_.repository_snapshot) {
            snapshot.repository = diagnostics_.repository_snapshot();
        }
        if (diagnostics_.service_graph) {
            const auto graph = diagnostics_.service_graph();
            snapshot.diagnostics.healthy = graph.error == ERROR_SUCCESS && graph.failures.empty();
            for (const auto& failure : graph.failures) {
                snapshot.diagnostics.recent_faults.push_back(
                    failure.service_id + ": error " + std::to_string(failure.error));
            }
        }
        std::vector<anomaly::AvailableServiceVersion> services;
        for (const auto& service : anomaly::ProcessAdapterServices().Snapshot()) {
            services.push_back({service.id, service.version});
        }
        if (diagnostics_.nte_compatibility) {
            snapshot.nte_compatibility = diagnostics_.nte_compatibility();
            snapshot.nte_compatibility.services = std::move(services);
        } else {
            snapshot.nte_compatibility = anomaly::BuildNteCompatibilitySnapshot(
                {}, {}, {}, std::move(services));
            snapshot.nte_compatibility.reason =
                "NTE compatibility provider is not published by this host";
        }
        snapshot.operation_results = model_.Snapshot().operation_results;
        snapshot.operation_plans = model_.Snapshot().operation_plans;
        ResolveAwaitingBatch(snapshot);
        model_.Publish(std::move(snapshot));
        RefreshSettingsState();
    }

    [[nodiscard]] bool SettingsDirty() const noexcept {
        return settings_draft_.has_value() && settings_snapshot_.ready &&
            *settings_draft_ != settings_snapshot_.values;
    }

    static Route RouteFromKey(const std::string_view route) noexcept {
        if (route == "diagnostics") return Route::Diagnostics;
        if (route == "settings") return Route::Settings;
        return Route::Plugins;
    }

    void RefreshSettingsState() {
        if (settings_apply_mailbox_ != nullptr) {
            std::optional<anomaly::PlatformSettingsApplyResult> result;
            {
                std::scoped_lock lock(settings_apply_mailbox_->mutex);
                result.swap(settings_apply_mailbox_->result);
            }
            if (result) {
                settings_save_pending_ = false;
                if (result->Applied()) {
                    settings_snapshot_ = result->snapshot;
                    settings_draft_ = result->snapshot.values;
                    settings_base_revision_ = result->snapshot.revision;
                    settings_apply_error_.clear();
                    status_ = Text(anomaly::MessageId::SettingsSaved);
                    status_failure_ = false;
                    if (settings_route_after_save_) {
                        const Route route = *settings_route_after_save_;
                        settings_route_after_save_.reset();
                        Navigate(route);
                    }
                } else {
                    settings_apply_error_ = result->message;
                    settings_validation_errors_ = std::move(result->validation_errors);
                    status_ = Text(anomaly::MessageId::SettingsSaveFailed);
                    status_failure_ = true;
                }
            }
        }

        if (repository_configure_mailbox_ != nullptr) {
            std::optional<RepositoryConfigureResult> result;
            {
                std::scoped_lock lock(repository_configure_mailbox_->mutex);
                result.swap(repository_configure_mailbox_->result);
            }
            if (result) {
                repo_editor_save_pending_ = false;
                if (result->submission.accepted) {
                    LoadRepositoryEditor(result->config);
                    repo_editor_status_ = Text(anomaly::MessageId::SettingsRepositoriesApplied);
                    repo_editor_status_failure_ = false;
                    status_ = repo_editor_status_;
                    status_failure_ = false;
                } else {
                    const std::array<std::string_view, 1> arguments{
                        result->submission.message};
                    repo_editor_status_ = Format(
                        anomaly::MessageId::SettingsRepositoriesApplyFailed, arguments);
                    repo_editor_status_failure_ = true;
                }
            }
        }

        if (!diagnostics_.settings_snapshot) return;
        const anomaly::PlatformSettingsSnapshot published = diagnostics_.settings_snapshot();
        const bool was_ready = settings_snapshot_.ready;
        const bool dirty = SettingsDirty();
        settings_snapshot_ = published;
        if (published.ready && (!settings_draft_ || (!dirty && !settings_save_pending_))) {
            settings_draft_ = published.values;
            settings_base_revision_ = published.revision;
            settings_apply_error_.clear();
        }
        if (!settings_route_restored_ && published.ready) {
            settings_route_restored_ = true;
            if (!was_ready && published.values.interface_remember_last_route &&
                model_.State().route == Route::Plugins) {
                model_.State().route = RouteFromKey(published.last_route);
            }
        }
        ApplySettingsPreview();
    }

    void ApplySettingsPreview() {
        if (!settings_draft_) return;
        const auto& values = *settings_draft_;
        SetDeveloperMode(values.advanced_developer_mode);
        anomaly::SetHostUiInputCapturePolicy(values.input_capture_policy);
        ImGuiIO& io = ImGui::GetIO();
        static_cast<void>(ApplyPlatformUiFontScale(
            static_cast<float>(values.interface_scale_percent) / 100.0f));
        if (values.input_gamepad_navigation) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }

    static ImVec4 AccentColor() noexcept { return {0.345f, 0.718f, 0.647f, 1.0f}; }
    static ImVec4 SuccessColor() noexcept { return {0.380f, 0.788f, 0.545f, 1.0f}; }
    static ImVec4 WarningColor() noexcept { return {0.882f, 0.675f, 0.322f, 1.0f}; }
    static ImVec4 ErrorColor() noexcept { return {0.898f, 0.435f, 0.447f, 1.0f}; }
    static ImVec4 InfoColor() noexcept { return {0.431f, 0.659f, 0.996f, 1.0f}; }
    static ImVec4 SurfaceColor() noexcept { return {0.106f, 0.125f, 0.145f, 1.0f}; }
    static ImVec4 RaisedColor() noexcept { return {0.137f, 0.165f, 0.192f, 1.0f}; }
    static ImVec4 NavColor() noexcept { return {0.063f, 0.075f, 0.090f, 1.0f}; }

    [[nodiscard]] const char* DisplayPluginState(
        const anomaly::PlatformUiPluginState state) const noexcept {
        switch (state) {
        case anomaly::PlatformUiPluginState::Active:
            return Text(anomaly::MessageId::PluginStateRunning);
        case anomaly::PlatformUiPluginState::Loaded:
            return Text(anomaly::MessageId::PluginStateLoaded);
        case anomaly::PlatformUiPluginState::Disabled:
            return Text(anomaly::MessageId::PluginStateDisabled);
        case anomaly::PlatformUiPluginState::Faulted:
            return Text(anomaly::MessageId::PluginStateFaulted);
        case anomaly::PlatformUiPluginState::Quarantined:
            return Text(anomaly::MessageId::PluginStateQuarantined);
        case anomaly::PlatformUiPluginState::WaitingForService:
            return Text(anomaly::MessageId::PluginStateWaitingForService);
        case anomaly::PlatformUiPluginState::Rejected:
            return Text(anomaly::MessageId::PluginStatePackageRejected);
        case anomaly::PlatformUiPluginState::DependencyBlocked:
            return Text(anomaly::MessageId::PluginStateDependencyBlocked);
        case anomaly::PlatformUiPluginState::Incompatible:
            return Text(anomaly::MessageId::PluginStateIncompatible);
        case anomaly::PlatformUiPluginState::Stopping:
            return Text(anomaly::MessageId::PluginStateStopping);
        case anomaly::PlatformUiPluginState::Unknown:
            return Text(anomaly::MessageId::CommonUnknown);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    [[nodiscard]] const char* DisplayRepositoryState(
        const anomaly::RepositoryCoordinatorState state) const noexcept {
        switch (state) {
        case anomaly::RepositoryCoordinatorState::Disabled:
            return Text(anomaly::MessageId::RepositoryStateDisabled);
        case anomaly::RepositoryCoordinatorState::Refreshing:
            return Text(anomaly::MessageId::RepositoryStateRefreshing);
        case anomaly::RepositoryCoordinatorState::Ready:
            return Text(anomaly::MessageId::RepositoryStateReady);
        case anomaly::RepositoryCoordinatorState::Degraded:
            return Text(anomaly::MessageId::RepositoryStateDegraded);
        case anomaly::RepositoryCoordinatorState::Unavailable:
            return Text(anomaly::MessageId::RepositoryStateUnavailable);
        case anomaly::RepositoryCoordinatorState::Stopped:
            return Text(anomaly::MessageId::RepositoryStateStopped);
        }
        return Text(anomaly::MessageId::RepositoryStateUnavailable);
    }

    [[nodiscard]] const char* DisplayRepositoryOperationState(
        const anomaly::RepositoryOperationState state) const noexcept {
        switch (state) {
        case anomaly::RepositoryOperationState::Queued:
            return Text(anomaly::MessageId::RepositoryOperationQueued);
        case anomaly::RepositoryOperationState::Downloading:
            return Text(anomaly::MessageId::RepositoryOperationDownloading);
        case anomaly::RepositoryOperationState::Installing:
            return Text(anomaly::MessageId::RepositoryOperationInstalling);
        case anomaly::RepositoryOperationState::Uninstalling:
            return Text(anomaly::MessageId::RepositoryOperationUninstalling);
        case anomaly::RepositoryOperationState::Succeeded:
            return Text(anomaly::MessageId::RepositoryOperationSucceeded);
        case anomaly::RepositoryOperationState::Failed:
            return Text(anomaly::MessageId::RepositoryOperationFailed);
        case anomaly::RepositoryOperationState::Cancelled:
            return Text(anomaly::MessageId::RepositoryOperationCancelled);
        }
        return Text(anomaly::MessageId::RepositoryOperationFailed);
    }

    [[nodiscard]] static ImVec4 PluginStateColor(
        const anomaly::PlatformUiPluginState state) noexcept {
        switch (state) {
        case anomaly::PlatformUiPluginState::Active: return SuccessColor();
        case anomaly::PlatformUiPluginState::Loaded: return InfoColor();
        case anomaly::PlatformUiPluginState::Faulted:
        case anomaly::PlatformUiPluginState::Quarantined:
        case anomaly::PlatformUiPluginState::Rejected:
            return ErrorColor();
        case anomaly::PlatformUiPluginState::Disabled: return {0.451f, 0.490f, 0.533f, 1.0f};
        default: return WarningColor();
        }
    }

    [[nodiscard]] static ShellGlyph RouteGlyph(const Route route) noexcept {
        switch (route) {
        case Route::Plugins: return ShellGlyph::Package;
        case Route::NteCompatibility: return ShellGlyph::Shield;
        case Route::Diagnostics: return ShellGlyph::Terminal;
        case Route::Settings: return ShellGlyph::Settings;
        }
        return ShellGlyph::Info;
    }

    [[nodiscard]] static ShellGlyph StateGlyph(
        const anomaly::PlatformUiPluginState state) noexcept {
        switch (state) {
        case anomaly::PlatformUiPluginState::Active: return ShellGlyph::Check;
        case anomaly::PlatformUiPluginState::Loaded: return ShellGlyph::Info;
        case anomaly::PlatformUiPluginState::Disabled: return ShellGlyph::Stop;
        default: return ShellGlyph::Warning;
        }
    }

    [[nodiscard]] const char* MutationLabel(const Mutation mutation) const noexcept {
        switch (mutation) {
        case Mutation::Start: return Text(anomaly::MessageId::OperationStartingPlugin);
        case Mutation::Stop: return Text(anomaly::MessageId::OperationStoppingPlugin);
        case Mutation::Reload: return Text(anomaly::MessageId::OperationReloadingPlugin);
        case Mutation::Enable: return Text(anomaly::MessageId::OperationEnablingPlugin);
        case Mutation::Disable: return Text(anomaly::MessageId::OperationDisablingPlugin);
        case Mutation::SetVisible:
            return Text(anomaly::MessageId::OperationUpdatingPluginWindow);
        case Mutation::ReloadAll:
            return Text(anomaly::MessageId::OperationReloadingInstalledPlugins);
        case Mutation::None: return Text(anomaly::MessageId::OperationPlugin);
        }
        return Text(anomaly::MessageId::OperationPlugin);
    }

    [[nodiscard]] const char* MutationActionLabel(const Mutation mutation) const noexcept {
        switch (mutation) {
        case Mutation::Start: return Text(anomaly::MessageId::CommonStart);
        case Mutation::Stop: return Text(anomaly::MessageId::CommonStop);
        case Mutation::Reload:
        case Mutation::ReloadAll: return Text(anomaly::MessageId::CommonReload);
        case Mutation::Enable: return Text(anomaly::MessageId::CommonEnable);
        case Mutation::Disable: return Text(anomaly::MessageId::CommonDisable);
        case Mutation::SetVisible: return Text(anomaly::MessageId::CommonUpdate);
        case Mutation::None: return Text(anomaly::MessageId::OperationPlugin);
        }
        return Text(anomaly::MessageId::OperationPlugin);
    }

    [[nodiscard]] const char* OperationStateLabel(
        const anomaly::PlatformUiOperationState state) const noexcept {
        switch (state) {
        case anomaly::PlatformUiOperationState::Succeeded:
            return Text(anomaly::MessageId::CommonSucceeded);
        case anomaly::PlatformUiOperationState::PartiallyFailed:
        case anomaly::PlatformUiOperationState::Failed:
            return Text(anomaly::MessageId::CommonFailed);
        case anomaly::PlatformUiOperationState::Cancelled:
            return Text(anomaly::MessageId::OperationCancelled);
        case anomaly::PlatformUiOperationState::Idle:
        case anomaly::PlatformUiOperationState::Submitted:
        case anomaly::PlatformUiOperationState::Running:
            return Text(anomaly::MessageId::OperationInProgress);
        }
        return Text(anomaly::MessageId::OperationInProgress);
    }

    [[nodiscard]] std::string OperationDisplayLabel(
        const anomaly::PlatformUiOperationResult& operation) const {
        if (operation.state == anomaly::PlatformUiOperationState::Idle ||
            operation.state == anomaly::PlatformUiOperationState::Submitted ||
            operation.state == anomaly::PlatformUiOperationState::Running) {
            return MutationLabel(operation.mutation);
        }
        return std::string(MutationActionLabel(operation.mutation)) + " - " +
            OperationStateLabel(operation.state);
    }

    [[nodiscard]] static ImVec4 OperationStateColor(
        const anomaly::PlatformUiOperationState state) noexcept {
        switch (state) {
        case anomaly::PlatformUiOperationState::Succeeded: return SuccessColor();
        case anomaly::PlatformUiOperationState::Failed: return ErrorColor();
        case anomaly::PlatformUiOperationState::PartiallyFailed: return WarningColor();
        case anomaly::PlatformUiOperationState::Cancelled:
            return ImVec4(0.451f, 0.490f, 0.533f, 1.0f);
        case anomaly::PlatformUiOperationState::Idle:
        case anomaly::PlatformUiOperationState::Submitted:
        case anomaly::PlatformUiOperationState::Running:
            return InfoColor();
        }
        return InfoColor();
    }

    [[nodiscard]] LayoutMode ComputeLayoutMode() const noexcept {
        const ImVec2 size = ImGui::GetWindowSize();
        if (size.x >= 1180.0f && size.y >= 720.0f) return LayoutMode::Wide;
        if (size.x >= 900.0f && size.y >= 600.0f) return LayoutMode::Standard;
        return LayoutMode::Compact;
    }

    void UpdateLayout() noexcept {
        const LayoutMode next = ComputeLayoutMode();
        if (next != LayoutMode::Compact) {
            compact_plugin_detail_ = false;
            compact_plugin_detail_pending_id_.clear();
        }
        layout_mode_ = next;
    }

    [[nodiscard]] bool IsCompact() const noexcept { return layout_mode_ == LayoutMode::Compact; }

    [[nodiscard]] bool ManagementShellGloballyCollapsed() const noexcept {
        return anomaly::HostUiMenusCollapsed() && !management_shell_locked_;
    }

    [[nodiscard]] bool ManagementShellCollapsed() const noexcept {
        return management_shell_collapsed_ || ManagementShellGloballyCollapsed();
    }

    [[nodiscard]] bool NavigationCollapsed() const noexcept {
        return IsCompact() || navigation_collapsed_;
    }

    [[nodiscard]] float NavigationWidth() const noexcept {
        if (NavigationCollapsed()) return 56.0f;
        return layout_mode_ == LayoutMode::Wide ? 184.0f : 164.0f;
    }

    [[nodiscard]] float PluginListWidth(const float available) const noexcept {
        const float default_width = layout_mode_ == LayoutMode::Wide ? 360.0f : 320.0f;
        const float requested = plugin_list_width_ > 0.0f ? plugin_list_width_ : default_width;
        const float minimum = layout_mode_ == LayoutMode::Wide ? 300.0f : 280.0f;
        const float maximum = layout_mode_ == LayoutMode::Wide ? 440.0f : 380.0f;
        return std::clamp(requested, minimum, std::min(maximum, std::max(minimum, available - 360.0f)));
    }

    void DrawTooltip(const char* text) const {
        if (text == nullptr || *text == '\0' ||
            !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            return;
        }
        ImGui::SetTooltip("%s", text);
    }

    [[nodiscard]] bool PrimaryButton(const char* label, ImVec2 size = {}) const {
        ImGui::PushStyleColor(ImGuiCol_Button, AccentColor());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.390f, 0.790f, 0.700f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.270f, 0.610f, 0.540f, 1.0f));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        return pressed;
    }

    [[nodiscard]] bool DestructiveButton(const char* label, ImVec2 size = {}) const {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.650f, 0.220f, 0.220f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.760f, 0.270f, 0.270f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.560f, 0.170f, 0.170f, 1.0f));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        return pressed;
    }

    [[nodiscard]] bool IconButton(const char* label, const char* tooltip) const {
        const bool pressed = ImGui::Button(label, ImVec2(30.0f, 30.0f));
        DrawTooltip(tooltip);
        return pressed;
    }

    [[nodiscard]] bool DrawShellIconButton(const char* id, const ShellGlyph glyph,
        const char* tooltip, const bool enabled = true, const bool selected = false) const {
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button,
            selected ? ImVec4(0.345f, 0.718f, 0.647f, 0.16f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            selected ? ImVec4(0.345f, 0.718f, 0.647f, 0.26f) : RaisedColor());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.180f, 0.220f, 0.250f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? AccentColor()
            : ImVec4(0.667f, 0.698f, 0.737f, 1.0f));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(ShellGlyphText(glyph), ImVec2(30.0f, 30.0f));
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        if (hovered && tooltip != nullptr) ImGui::SetTooltip("%s", tooltip);
        return pressed && enabled;
    }

    [[nodiscard]] bool DrawShellCommandButton(const char* id, const char* label,
        const ShellGlyph glyph, const bool primary = false, const bool enabled = true,
        const ImVec2 size = {}) const {
        const std::string text = std::string(ShellGlyphText(glyph)) + "  " + label;
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button, primary ? AccentColor() : SurfaceColor());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            primary ? ImVec4(0.415f, 0.773f, 0.706f, 1.0f) : RaisedColor());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            primary ? ImVec4(0.290f, 0.635f, 0.570f, 1.0f)
                    : ImVec4(0.180f, 0.220f, 0.250f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            primary ? ImVec4(0.063f, 0.129f, 0.122f, 1.0f)
                    : ImVec4(0.667f, 0.698f, 0.737f, 1.0f));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(text.c_str(), size.x > 0.0f ? size : ImVec2(0.0f, 30.0f));
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        return pressed && enabled;
    }

    void BeginShellBodyChild(const char* id, const ImVec2 size = {},
        const bool bordered = false, const ImGuiWindowFlags window_flags = 0) const {
        const float padding = IsCompact() ? 12.0f : 16.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        ImGui::BeginChild(id, size,
            bordered ? ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding
                     : ImGuiChildFlags_AlwaysUseWindowPadding,
            window_flags);
    }

    void EndShellBodyChild() const {
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    enum class ShellHeaderControl { Collapse, Lock, Close };

    [[nodiscard]] bool DrawShellHeaderControl(const char* id, const ShellHeaderControl control,
        const bool active, const bool disabled, const char* tooltip) const {
        const ShellGlyph glyph = control == ShellHeaderControl::Collapse
            ? (active ? ShellGlyph::ChevronUp : ShellGlyph::ChevronDown)
            : control == ShellHeaderControl::Lock ? ShellGlyph::Pin : ShellGlyph::Close;
        return DrawShellIconButton(id, glyph, tooltip, !disabled, active);
    }

    [[nodiscard]] static std::string Ellipsize(std::string_view text, const float width) {
        if (text.empty() || ImGui::CalcTextSize(text.data(), text.data() + text.size()).x <= width) {
            return std::string(text);
        }
        constexpr std::string_view suffix{"..."};
        if (ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x > width) return {};
        std::size_t end = text.size();
        while (end > 0) {
            --end;
            while (end > 0 &&
                   (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) {
                --end;
            }
            std::string candidate{text.substr(0, end)};
            candidate += suffix;
            if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
        }
        return std::string(suffix);
    }

    static void DrawGenericPluginIcon(const ImVec2 position, const float size) {
        ImDrawList* const draw_list = ImGui::GetWindowDrawList();
        const ImU32 border = ImGui::ColorConvertFloat4ToU32({0.306f, 0.349f, 0.396f, 1.0f});
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32({0.125f, 0.149f, 0.173f, 1.0f});
        draw_list->AddRectFilled(position, Offset(position, size, size), fill, 4.0f);
        draw_list->AddRect(position, Offset(position, size, size), border, 4.0f);
        const ImVec2 glyph_size = ImGui::CalcTextSize(ShellGlyphText(ShellGlyph::Package));
        draw_list->AddText(Offset(position, (size - glyph_size.x) * 0.5f,
            (size - glyph_size.y) * 0.5f - 1.0f),
            ImGui::ColorConvertFloat4ToU32({0.667f, 0.698f, 0.737f, 1.0f}),
            ShellGlyphText(ShellGlyph::Package));
    }

    void DrawPluginStateBadge(const ImVec2 position,
        const anomaly::PlatformUiPluginState state) const {
        const char* const glyph = ShellGlyphText(StateGlyph(state));
        const char* const label = DisplayPluginState(state);
        const float glyph_width = ImGui::CalcTextSize(glyph).x;
        const float width = glyph_width + 5.0f + ImGui::CalcTextSize(label).x + 14.0f;
        const ImVec4 color = PluginStateColor(state);
        ImDrawList* const draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(position, Offset(position, width, 20.0f),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.15f)), 3.0f);
        draw_list->AddRect(position, Offset(position, width, 20.0f),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.34f)), 3.0f);
        draw_list->AddText(Offset(position, 7.0f, 4.0f),
            ImGui::ColorConvertFloat4ToU32(color), glyph);
        draw_list->AddText(Offset(position, 7.0f + glyph_width + 5.0f, 3.0f),
            ImGui::ColorConvertFloat4ToU32(color), label);
    }

    [[nodiscard]] const anomaly::PlatformUiOperationResult* PresentedOperation() const noexcept {
        for (auto it = model_.PendingOperations().rbegin(); it != model_.PendingOperations().rend(); ++it) {
            return &*it;
        }
        const auto& results = model_.Snapshot().operation_results;
        for (auto it = results.rbegin(); it != results.rend(); ++it) {
            if (it->state == anomaly::PlatformUiOperationState::PartiallyFailed ||
                it->state == anomaly::PlatformUiOperationState::Failed) {
                return &*it;
            }
        }
        return nullptr;
    }

    void HandleShellShortcuts() {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F) &&
            model_.State().route == Route::Plugins &&
            model_.State().plugin_tab == Tab::Installed) {
            focus_plugin_search_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && IsCompact() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            if (compact_plugin_detail_) compact_plugin_detail_ = false;
            else compact_plugin_detail_pending_id_.clear();
        }
    }

    void DrawManagementShell() {
        RedirectHiddenNteCompatibilityRoute();
        const bool was_collapsed = ManagementShellCollapsed();
        DrawShellHeader();
        if (was_collapsed || ManagementShellCollapsed()) return;

        const float workspace_height = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("PlatformShellWorkspace", ImVec2(0.0f, workspace_height), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float navigation_width = NavigationWidth();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, NavColor());
        // Shell navigation applies its own 8px inset so the selected state
        // has the same width in compact and expanded layouts.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("PlatformShellNavigation", ImVec2(navigation_width, 0.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawShellNavigation();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("PlatformShellRoute", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawShellRoute();
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void DrawShellWindowControls() {
        const bool locked = management_shell_locked_;
        const bool collapsed = ManagementShellCollapsed();
        const char* const collapse_tooltip = locked
            ? Text(anomaly::MessageId::ShellCollapseLocked)
            : Text(collapsed ? anomaly::MessageId::ShellExpand
                             : anomaly::MessageId::ShellCollapse);
        if (DrawShellHeaderControl("##shell-collapse", ShellHeaderControl::Collapse,
                collapsed, locked, collapse_tooltip)) {
            management_shell_collapsed_ = !management_shell_collapsed_;
        }
        ImGui::SameLine(0.0f, 4.0f);
        const char* const lock_tooltip = locked
            ? Text(anomaly::MessageId::ShellUnlock)
            : Text(anomaly::MessageId::ShellLock);
        if (DrawShellHeaderControl("##shell-lock", ShellHeaderControl::Lock, locked,
                false, lock_tooltip)) {
            management_shell_locked_ = !locked;
            anomaly::SetHostUiCurrentWindowLocked(management_shell_locked_);
            if (!locked) management_shell_collapsed_ = false;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (DrawShellHeaderControl("##shell-close", ShellHeaderControl::Close,
                false, false, Text(anomaly::MessageId::ShellClose))) {
            if (plugins_.UiResources().CloseWindow(
                    management_window_scope_, management_window_)) {
                anomaly::SetHostUiMenusCollapsed(true);
            }
        }
    }

    void DrawShellDragRegion(const ImVec2 origin, const ImVec2 size) const {
        ImGuiWindow* const header = ImGui::GetCurrentWindow();
        if (header == nullptr) return;
        ImGuiWindow* const root = header->RootWindow == nullptr ? header : header->RootWindow;
        const float drag_width = ManagementShellGloballyCollapsed()
            ? size.x : (std::max)(0.0f, size.x - kPlatformHeaderActionColumnWidth);
        if (drag_width <= 0.0f) return;
        const ImRect bounds(origin, Offset(origin, drag_width, kPlatformHeaderHeight));
        bool hovered{};
        bool held{};
        const ImGuiID id = header->GetID("##platform-shell-drag");
        ImGui::KeepAliveID(id);
        const bool pressed = ImGui::ButtonBehavior(
            bounds, id, &hovered, &held,
            ImGuiButtonFlags_NoNavFocus | ImGuiButtonFlags_PressedOnClick);
        if (pressed) ImGui::StartMouseMovingWindow(root);
    }

    void DrawShellHeader() {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.086f, 0.102f, 0.118f, 1.0f));
        ImGui::BeginChild("PlatformGlobalHeader", ImVec2(0.0f, kPlatformHeaderHeight), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        DrawShellDragRegion(origin, size);
        ImGui::SetCursorScreenPos(Offset(origin, 16.0f, 11.0f));
        const bool logo_drawn = (logo_texture_ && plugins_.DrawUiTexture(
            management_window_scope_, logo_texture_, kPlatformHeaderLogoSize,
            kPlatformHeaderLogoSize, 0xffffffffU)) ||
            DrawStandaloneHeaderLogo(kPlatformHeaderLogoSize, kPlatformHeaderLogoSize);
        if (!logo_drawn) ImGui::Dummy(ImVec2(kPlatformHeaderLogoSize, kPlatformHeaderLogoSize));
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.35f,
            Offset(origin, 56.0f, 16.0f),
            ImGui::ColorConvertFloat4ToU32({0.949f, 0.957f, 0.965f, 1.0f}),
            Text(anomaly::MessageId::ApplicationTitle));
        if (!ManagementShellGloballyCollapsed()) {
            ImGui::SetCursorScreenPos(
                Offset(origin, size.x - kPlatformHeaderActionColumnWidth, 11.0f));
            DrawShellWindowControls();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void DrawShellNavigation() {
        constexpr std::array<Route, 3> routes{
            Route::Plugins, Route::Diagnostics, Route::Settings};
        const bool collapsed = NavigationCollapsed();
        const auto& snapshot = model_.Snapshot();
        const float width = ImGui::GetContentRegionAvail().x;
        float y = 10.0f;
        for (const Route route : routes) {
            ImGui::SetCursorPos(ImVec2(8.0f, y));
            const bool selected = model_.State().route == route;
            std::string item_id{"##nav-"};
            item_id += anomaly::ToString(route);
            ImGui::PushStyleColor(ImGuiCol_Header, selected
                ? ImVec4(0.345f, 0.718f, 0.647f, 0.16f) : ImVec4(0, 0, 0, 0));
            const bool clicked = ImGui::Selectable(item_id.c_str(), selected, 0,
                ImVec2(std::max(0.0f, width - 16.0f), 40.0f));
            ImGui::PopStyleColor();
            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            ImDrawList* const draw_list = ImGui::GetWindowDrawList();
            if (selected) {
                draw_list->AddRectFilled(
                    item_min, ImVec2(item_min.x + 3.0f, item_max.y),
                    ImGui::ColorConvertFloat4ToU32(AccentColor()), 2.0f);
            }
            const ImU32 primary_text = ImGui::ColorConvertFloat4ToU32(selected
                ? ImVec4(0.949f, 0.957f, 0.965f, 1.0f)
                : ImVec4(0.667f, 0.698f, 0.737f, 1.0f));
            const char* const glyph = ShellGlyphText(RouteGlyph(route));
            // Keep the route icons on one left-aligned grid when the rail changes width.
            const float icon_x = 12.0f;
            draw_list->AddText(Offset(item_min, icon_x, 12.0f), primary_text, glyph);
            if (!collapsed) {
                const char* route_label = RouteLabel(route);
                draw_list->AddText(Offset(item_min, 38.0f, 12.0f), primary_text, route_label);
                std::string indicator;
                if (route == Route::Plugins && snapshot.runtime_summary.issues != 0) {
                    indicator = std::to_string(snapshot.runtime_summary.issues);
                } else if (route == Route::Settings && SettingsDirty()) {
                    indicator = "*";
                }
                if (!indicator.empty()) {
                    const float indicator_width = ImGui::CalcTextSize(indicator.c_str()).x;
                    constexpr float indicator_slot_width = 16.0f;
                    const float indicator_x = item_max.x - 12.0f - indicator_slot_width +
                        (indicator_slot_width - indicator_width) * 0.5f;
                    draw_list->AddText(ImVec2(indicator_x, item_min.y + 12.0f),
                        ImGui::ColorConvertFloat4ToU32(WarningColor()), indicator.c_str());
                }
            }
            if (collapsed) {
                std::string tooltip{RouteLabel(route)};
                if (route == Route::Plugins && snapshot.runtime_summary.issues != 0) {
                    const std::string count = std::to_string(snapshot.runtime_summary.issues);
                    const std::array<std::string_view, 1> arguments{count};
                    tooltip += ": " + Format(anomaly::MessageId::ShellIssues, arguments);
                }
                DrawTooltip(tooltip.c_str());
            }
            if (clicked) Navigate(route);
            y += 44.0f;
        }

        ImGui::SetCursorPos(ImVec2(8.0f, std::max(y + 8.0f, ImGui::GetWindowSize().y - 48.0f)));
        if (DrawShellIconButton("navigation-collapse",
                collapsed ? ShellGlyph::ChevronRight : ShellGlyph::ChevronLeft,
                Text(collapsed ? anomaly::MessageId::ShellExpandNavigation
                               : anomaly::MessageId::ShellCollapseNavigation),
                !IsCompact())) {
            navigation_collapsed_ = !navigation_collapsed_;
        }
    }

    template <typename DrawActions>
    void DrawShellPageHeader(
        const char* title, const std::string_view subtitle, DrawActions&& actions,
        const float title_inset = 0.0f) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.078f, 0.090f, 0.102f, 1.0f));
        ImGui::BeginChild("PlatformPageHeader", ImVec2(0.0f, 48.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float title_x = (IsCompact() ? 10.0f : 16.0f) + title_inset;
        ImGui::SetCursorScreenPos(Offset(origin, title_x, subtitle.empty() ? 14.0f : 7.0f));
        ImGui::SetWindowFontScale(IsCompact() ? 1.12f : 1.35f);
        ImGui::TextUnformatted(title);
        ImGui::SetWindowFontScale(1.0f);
        if (!subtitle.empty() && !IsCompact()) {
            ImGui::SetCursorScreenPos(Offset(origin, title_x, 28.0f));
            ImGui::TextDisabled("%.*s", static_cast<int>(subtitle.size()), subtitle.data());
        }
        actions(origin, size);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void DrawShellProviderUnavailable(const char* subject, const char* reason) {
        BeginShellBodyChild("PlatformProviderUnavailable");
        ImGui::SetCursorPosY(std::max(24.0f, ImGui::GetContentRegionAvail().y * 0.18f));
        const std::array<std::string_view, 1> arguments{subject};
        const std::string unavailable =
            Format(anomaly::MessageId::PluginsProviderUnavailable, arguments);
        ImGui::TextColored(WarningColor(), "!  %s", unavailable.c_str());
        ImGui::PushTextWrapPos(std::min(620.0f, ImGui::GetContentRegionAvail().x));
        ImGui::TextDisabled("%s", reason);
        ImGui::PopTextWrapPos();
        const std::string open_diagnostics = StableLabel(
            anomaly::MessageId::PluginsOpenDiagnostics, "provider-open-diagnostics");
        if (ImGui::Button(open_diagnostics.c_str())) {
            Navigate(Route::Diagnostics);
            model_.State().diagnostics_tab = DiagnosticTab::Overview;
        }
        EndShellBodyChild();
    }

    [[nodiscard]] const anomaly::RepositoryPluginView* RepositoryPlugin(
        std::string_view plugin_id) const noexcept {
        const auto& plugins = model_.Snapshot().repository.plugins;
        const auto found = std::ranges::find_if(plugins, [&](const auto& plugin) {
            return plugin.entry.internal_name == plugin_id;
        });
        return found == plugins.end() ? nullptr : &*found;
    }

    [[nodiscard]] const anomaly::RepositoryOperationView* RepositoryOperation(
        std::string_view plugin_id) const noexcept {
        const auto& operations = model_.Snapshot().repository.operations;
        const auto found = std::find_if(operations.rbegin(), operations.rend(),
            [&](const auto& operation) { return operation.plugin_id == plugin_id; });
        return found == operations.rend() ? nullptr : &*found;
    }

    void SubmitRepositoryInstall(const anomaly::RepositoryPluginView& plugin) {
        if (!diagnostics_.repository_install) {
            status_ = Text(anomaly::MessageId::RepositoryInstallProviderUnavailable);
            status_failure_ = true;
            return;
        }
        const auto submission = diagnostics_.repository_install(
            plugin.entry.internal_name, plugin.entry.version);
        if (submission.accepted) {
            status_ = Text(anomaly::MessageId::RepositoryDownloadQueued);
            status_failure_ = false;
        } else {
            const std::array<std::string_view, 1> arguments{submission.message};
            status_ = Format(anomaly::MessageId::RepositoryInstallFailed, arguments);
            status_failure_ = true;
        }
    }

    void RequestRepositoryUninstall(std::string_view plugin_id, std::string_view display_name) {
        const auto* operation = RepositoryOperation(plugin_id);
        if (operation != nullptr &&
            operation->state != anomaly::RepositoryOperationState::Succeeded &&
            operation->state != anomaly::RepositoryOperationState::Failed &&
            operation->state != anomaly::RepositoryOperationState::Cancelled) {
            status_ = Text(anomaly::MessageId::OperationInProgress);
            status_failure_ = false;
            return;
        }
        repository_uninstall_plugin_id_ = plugin_id;
        repository_uninstall_plugin_name_ = display_name.empty() ? plugin_id : display_name;
        repository_uninstall_popup_requested_ = true;
    }

    void SubmitRepositoryUninstall() {
        const std::string plugin_id = std::exchange(repository_uninstall_plugin_id_, {});
        repository_uninstall_plugin_name_.clear();
        if (!diagnostics_.repository_uninstall) {
            status_ = Text(anomaly::MessageId::RepositoryUninstallProviderUnavailable);
            status_failure_ = true;
            return;
        }
        const auto submission = diagnostics_.repository_uninstall(plugin_id);
        if (submission.accepted) {
            status_ = Text(anomaly::MessageId::RepositoryUninstallQueued);
            status_failure_ = false;
        } else {
            const std::array<std::string_view, 1> arguments{submission.message};
            status_ = Format(anomaly::MessageId::RepositoryUninstallFailed, arguments);
            status_failure_ = true;
        }
    }

    void DrawRepositoryWorkspace(bool updates_only) {
        auto& state = model_.State();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        SetAvailableItemWidth(0.0f, 360.0f);
        if (ImGui::InputTextWithHint("##RepositorySearch",
                Text(anomaly::MessageId::PluginsSearchRepositoryHint), search_.data(),
                search_.size())) {
            state.search = search_.data();
        }
        ImGui::PopStyleVar();
        ImGui::Separator();

        std::size_t visible{};
        for (const auto& plugin : model_.Snapshot().repository.plugins) {
            if (!state.search.empty() &&
                plugin.entry.internal_name.find(state.search) == std::string::npos &&
                plugin.entry.name.find(state.search) == std::string::npos) {
                continue;
            }
            const auto* installed = model_.Snapshot().FindPlugin(plugin.entry.internal_name);
            const auto* operation = RepositoryOperation(plugin.entry.internal_name);
            const auto install_state = anomaly::ResolveRepositoryPluginInstallState(
                installed, plugin);
            if (updates_only && !install_state.update_available) continue;
            if (updates_only && installed != nullptr && settings_snapshot_.ready &&
                !settings_snapshot_.values.updates_include_disabled && installed->IsDisabled()) {
                continue;
            }
            ++visible;

            ImGui::PushID(plugin.entry.internal_name.c_str());
            const bool pending = operation != nullptr &&
                operation->state != anomaly::RepositoryOperationState::Succeeded &&
                operation->state != anomaly::RepositoryOperationState::Failed &&
                operation->state != anomaly::RepositoryOperationState::Cancelled;
            const bool current = install_state.installed && !install_state.update_available;
            const bool enabled = !pending && (current || plugin.compatible);
            const char* label = pending ? DisplayRepositoryOperationState(operation->state)
                : current ? Text(anomaly::MessageId::CommonUninstall)
                : install_state.update_available ? Text(anomaly::MessageId::CommonUpdate)
                         : Text(anomaly::MessageId::CommonInstall);
            const float button_width = 104.0f;
            if (ImGui::BeginTable("RepositoryRow", 2,
                    ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 0.0f))) {
                ImGui::TableSetupColumn(
                    Text(anomaly::MessageId::CommonPlugin), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(
                    Text(anomaly::MessageId::CommonAction),
                    ImGuiTableColumnFlags_WidthFixed, button_width);
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 52.0f);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(plugin.entry.name.empty()
                    ? plugin.entry.internal_name.c_str()
                    : plugin.entry.name.c_str());
                const std::array<std::string_view, 3> metadata_arguments{
                    plugin.entry.version, plugin.entry.author, plugin.entry.punchline};
                ImGui::TextDisabled("%s", Format(
                    anomaly::MessageId::RepositoryVersionMetadata,
                    metadata_arguments).c_str());
                if (!plugin.compatible) {
                    ImGui::TextColored(WarningColor(), "%s", plugin.compatibility_reason.c_str());
                }
                if (operation != nullptr &&
                    (operation->state == anomaly::RepositoryOperationState::Failed ||
                     operation->state == anomaly::RepositoryOperationState::Succeeded)) {
                    ImGui::TextDisabled("%s", operation->message.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
                ImGui::BeginDisabled(!enabled);
                if (ImGui::Button(label, ImVec2(button_width, 30.0f))) {
                    if (current) {
                        RequestRepositoryUninstall(plugin.entry.internal_name,
                            plugin.entry.name);
                    } else {
                        SubmitRepositoryInstall(plugin);
                    }
                }
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (visible == 0) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", Text(updates_only
                ? anomaly::MessageId::PluginsInstalledUpToDate
                : anomaly::MessageId::PluginsNoRepositoryMatch));
        }
    }

    void DrawShellRoute() {
        switch (model_.State().route) {
        case Route::Plugins: DrawPluginsShell(); break;
        case Route::NteCompatibility:
            RedirectHiddenNteCompatibilityRoute();
            DrawDiagnosticsShell();
            break;
        case Route::Diagnostics: DrawDiagnosticsShell(); break;
        case Route::Settings: DrawSettingsShell(); break;
        }
    }

    void ResolveCompactPluginDetail() {
        if (!IsCompact() || compact_plugin_detail_pending_id_.empty()) return;
        if (ImGui::GetTime() - compact_plugin_detail_requested_at_ <
            ImGui::GetIO().MouseDoubleClickTime) {
            return;
        }
        if (model_.State().route == Route::Plugins &&
            model_.State().plugin_tab == Tab::Installed &&
            model_.State().selected_plugin_id == compact_plugin_detail_pending_id_) {
            compact_plugin_detail_ = true;
        }
        compact_plugin_detail_pending_id_.clear();
    }

    void DrawPluginsShell() {
        auto& state = model_.State();
        ResolveCompactPluginDetail();
        const bool compact_detail = IsCompact() && compact_plugin_detail_ &&
            state.plugin_tab == Tab::Installed;
        const auto* selected = model_.Snapshot().FindPlugin(state.selected_plugin_id);
        if (compact_detail && selected != nullptr) {
            DrawShellPageHeader(selected->name.empty() ? selected->id.c_str() : selected->name.c_str(), {},
                [this](const ImVec2& origin, const ImVec2&) {
                    ImGui::SetCursorScreenPos(Offset(origin, 10.0f, 9.0f));
                    if (DrawShellIconButton("back-to-plugin-list", ShellGlyph::ChevronLeft,
                            Text(anomaly::MessageId::CommonBack))) {
                        compact_plugin_detail_ = false;
                    }
                }, 42.0f);
            ImGui::BeginChild("PlatformCompactPluginDetail", ImVec2(0.0f, 0.0f), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawPluginPublicDetail(*selected);
            ImGui::EndChild();
            return;
        }

        const bool installed = state.plugin_tab == Tab::Installed;
        const bool third_party = state.plugin_tab == Tab::ThirdParty;
        const bool repository_catalog =
            state.plugin_tab == Tab::Available || state.plugin_tab == Tab::Updates;
        const auto& repository = model_.Snapshot().repository;
        const std::string_view repository_subtitle = repository.BrowseAvailable()
            ? DisplayRepositoryState(repository.state)
            : repository.reason;
        DrawShellPageHeader(Text(anomaly::MessageId::ShellRoutePlugins),
            installed ? std::string_view{} : repository_subtitle,
            [this, installed, repository_catalog](const ImVec2& origin, const ImVec2& size) {
                const float more_x = size.x - 46.0f;
                ImGui::SetCursorScreenPos(Offset(origin, more_x, 9.0f));
                if (repository_catalog) {
                    if (DrawShellIconButton("repository-refresh", ShellGlyph::Refresh,
                            Text(anomaly::MessageId::PluginsRefreshRepositories)) &&
                        diagnostics_.repository_refresh) {
                        const auto submission = diagnostics_.repository_refresh();
                        if (submission.accepted) {
                            status_ = Text(anomaly::MessageId::RepositoryRefreshQueued);
                            status_failure_ = false;
                        } else {
                            const std::array<std::string_view, 1> arguments{submission.message};
                            status_ = Format(
                                anomaly::MessageId::RepositoryRefreshFailed, arguments);
                            status_failure_ = true;
                        }
                    }
                    return;
                }
                if (installed && DrawShellIconButton("plugin-page-more", ShellGlyph::More,
                        Text(anomaly::MessageId::PluginsMoreActions))) {
                    ImGui::OpenPopup("Plugin page actions");
                }
                if (ImGui::BeginPopup("Plugin page actions")) {
                    const bool reload_pending = HasPendingMutation({}, Mutation::ReloadAll);
                    if (ImGui::MenuItem(Text(anomaly::MessageId::PluginsReloadAll),
                            nullptr, false, !reload_pending)) {
                        SubmitIntent(model_.NewIntent(
                            anomaly::PlatformUiIntentKind::ReloadAllInstalled, {}, Mutation::ReloadAll));
                    }
                    if (reload_pending) {
                        DrawTooltip(Text(anomaly::MessageId::PluginsReloadAllPending));
                    }
                    ImGui::EndPopup();
                }
            });

        ImGui::BeginChild("PlatformPluginsRoute", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawPluginTabs();
        if (third_party) {
            BeginShellBodyChild("PlatformThirdPartyPlugins");
            DrawRepositoryChannelsEditor();
            EndShellBodyChild();
        } else if (!installed) {
            if (repository.BrowseAvailable()) {
                BeginShellBodyChild("PlatformRepositoryCatalog");
                DrawRepositoryWorkspace(state.plugin_tab == Tab::Updates);
                EndShellBodyChild();
            } else {
                DrawShellProviderUnavailable(
                    Text(state.plugin_tab == Tab::Available
                        ? anomaly::MessageId::PluginsCatalogTitle
                        : anomaly::MessageId::PluginsUpdatesTitle),
                    repository.reason.c_str());
            }
        } else {
            DrawPluginToolbar();
            DrawInstalledPluginWorkspace();
        }
        ImGui::EndChild();
    }

    void DrawPluginTabs() {
        const std::size_t installed = model_.Snapshot().runtime_summary.installed;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.090f, 0.106f, 0.122f, 1.0f));
        ImGui::BeginChild("PlatformPluginTabs", ImVec2(0.0f, 36.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetWindowPos();
        float x = IsCompact() ? 8.0f : 16.0f;
        const auto draw_tab = [this, &origin, &x](const Tab tab, const std::string& label) {
            const bool selected = model_.State().plugin_tab == tab;
            const float width = ImGui::CalcTextSize(label.c_str()).x + 20.0f;
            ImGui::SetCursorScreenPos(Offset(origin, x, 3.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.949f, 0.957f, 0.965f, 0.03f));
            const std::string stable_label = anomaly::StableDisplayLabel(
                label, "plugin-tab-" + std::to_string(static_cast<unsigned>(tab)));
            if (ImGui::Button(stable_label.c_str(), ImVec2(width, 30.0f))) {
                model_.State().plugin_tab = tab;
                compact_plugin_detail_ = false;
                compact_plugin_detail_pending_id_.clear();
            }
            ImGui::PopStyleColor(2);
            if (selected) {
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    Offset(minimum, 0.0f, 28.0f), maximum,
                    ImGui::ColorConvertFloat4ToU32(AccentColor()));
            }
            x += width + 2.0f;
        };
        draw_tab(Tab::Installed,
            std::string(PluginTabLabel(Tab::Installed)) + "  " + std::to_string(installed));
        draw_tab(Tab::Available, PluginTabLabel(Tab::Available));
        draw_tab(Tab::Updates, PluginTabLabel(Tab::Updates));
        draw_tab(Tab::ThirdParty, PluginTabLabel(Tab::ThirdParty));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void DrawPluginToolbar() {
        const float toolbar_height = layout_mode_ == LayoutMode::Wide ? 46.0f : 84.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.078f, 0.090f, 0.102f, 1.0f));
        ImGui::BeginChild("PlatformPluginToolbar", ImVec2(0.0f, toolbar_height), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto& state = model_.State();
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float padding = IsCompact() ? 8.0f : 12.0f;
        const bool wide = layout_mode_ == LayoutMode::Wide;
        const float search_width = wide ? 280.0f : size.x - padding * 2.0f - 38.0f;
        ImGui::SetCursorScreenPos(Offset(origin, padding, 8.0f));
        if (focus_plugin_search_) {
            ImGui::SetKeyboardFocusHere();
            focus_plugin_search_ = false;
        }
        ImGui::SetNextItemWidth(std::max(80.0f, search_width));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28.0f, 6.0f));
        if (ImGui::InputTextWithHint("##plugin-search",
                Text(anomaly::MessageId::PluginsSearchHint), search_.data(), search_.size())) {
            state.search = search_.data();
            anomaly::ReconcileUiStateSelection(model_.Snapshot(), state, developer_mode_);
        }
        ImGui::PopStyleVar();
        const ImVec2 search_min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(Offset(search_min, 8.0f, 7.0f),
            ImGui::ColorConvertFloat4ToU32({0.667f, 0.698f, 0.737f, 1.0f}),
            ShellGlyphText(ShellGlyph::Search));
        ImGui::SetCursorScreenPos(Offset(origin, padding + search_width + 8.0f, 8.0f));
        if (DrawShellIconButton("clear-plugin-search", ShellGlyph::Close,
                Text(anomaly::MessageId::PluginsClearSearch),
                !state.search.empty())) {
            search_.fill('\0');
            state.search.clear();
            anomaly::ReconcileUiStateSelection(model_.Snapshot(), state, developer_mode_);
            focus_plugin_search_ = true;
        }
        const float controls_y = wide ? 8.0f : 46.0f;
        float x = wide ? padding + search_width + 46.0f : padding;
        ImGui::SetCursorScreenPos(Offset(origin, x, controls_y));
        if (IsCompact()) {
            DrawPluginFilterControl(true);
        } else {
            DrawPluginFilterControl(false);
        }
        const float filter_width = IsCompact() ? 100.0f :
            ImGui::CalcTextSize(FilterLabel(Filter::All)).x + 18.0f +
            ImGui::CalcTextSize(FilterLabel(Filter::Running)).x + 18.0f +
            ImGui::CalcTextSize(FilterLabel(Filter::Disabled)).x + 18.0f +
            ImGui::CalcTextSize(FilterLabel(Filter::Issues)).x + 18.0f;
        x += filter_width + 8.0f;
        ImGui::SetCursorScreenPos(Offset(origin, x, controls_y));
        DrawPluginSortControl();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void DrawPluginFilterControl(const bool compact_control) {
        auto& state = model_.State();
        constexpr std::array<Filter, 4> filters{
            Filter::All, Filter::Running, Filter::Disabled, Filter::Issues};
        if (compact_control) {
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::BeginCombo("##plugin-filter", FilterLabel(state.plugin_filter))) {
                for (const Filter filter : filters) {
                    if (ImGui::Selectable(FilterLabel(filter), state.plugin_filter == filter)) {
                        state.plugin_filter = filter;
                        anomaly::ReconcileUiStateSelection(
                            model_.Snapshot(), state, developer_mode_);
                    }
                }
                ImGui::EndCombo();
            }
            DrawTooltip(Text(anomaly::MessageId::PluginsFilterAll));
            return;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        for (std::size_t index = 0; index < filters.size(); ++index) {
            if (index != 0) ImGui::SameLine(0.0f, 0.0f);
            const Filter filter = filters[index];
            const bool selected = state.plugin_filter == filter;
            const char* const label = FilterLabel(filter);
            ImGui::PushStyleColor(ImGuiCol_Button, selected
                ? ImVec4(0.345f, 0.718f, 0.647f, 0.18f) : SurfaceColor());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, RaisedColor());
            ImGui::PushID(static_cast<int>(filter));
            if (ImGui::Button(label, ImVec2(ImGui::CalcTextSize(label).x + 18.0f, 30.0f))) {
                state.plugin_filter = filter;
                anomaly::ReconcileUiStateSelection(model_.Snapshot(), state, developer_mode_);
            }
            ImGui::PopID();
            ImGui::PopStyleColor(2);
        }
        ImGui::PopStyleVar();
    }

    void DrawPluginSortControl() {
        auto& state = model_.State();
        ImGui::SetNextItemWidth(IsCompact() ? 86.0f : 112.0f);
        if (ImGui::BeginCombo("##plugin-sort", SortLabel(state.plugin_sort))) {
            constexpr std::array<Sort, 3> sorts{Sort::Name, Sort::State, Sort::Author};
            for (const Sort sort : sorts) {
                const char* label = sort == Sort::State
                    ? Text(anomaly::MessageId::PluginsSortStateIssuesFirst)
                    : SortLabel(sort);
                ImGui::PushID(static_cast<int>(sort));
                if (ImGui::Selectable(label, state.plugin_sort == sort)) {
                    state.plugin_sort = sort;
                    anomaly::ReconcileUiStateSelection(
                        model_.Snapshot(), state, developer_mode_);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        DrawTooltip(Text(anomaly::MessageId::PluginsSortName));
    }

    void DrawInstalledPluginWorkspace() {
        const auto visible = model_.VisiblePlugins(developer_mode_);
        if (!visible.empty() && std::ranges::none_of(visible, [&](const auto& plugin) {
                return plugin.id == model_.State().selected_plugin_id;
            })) {
            model_.State().selected_plugin_id = visible.front().id;
        }
        keyboard_plugin_navigation_consumed_ = false;
        if (visible.empty()) {
            keyboard_plugin_focus_id_.clear();
            BeginShellBodyChild("PlatformPluginEmpty", {}, true);
            if (model_.Snapshot().installed_plugins.empty()) {
                ImGui::TextDisabled("%s", Text(anomaly::MessageId::PluginsNoInstalled));
            } else if (!model_.State().search.empty()) {
                const std::array<std::string_view, 1> arguments{model_.State().search};
                const std::string no_match =
                    Format(anomaly::MessageId::PluginsNoSearchMatch, arguments);
                ImGui::TextUnformatted(no_match.c_str());
                const std::string clear_search = StableLabel(
                    anomaly::MessageId::PluginsClearSearch, "empty-clear-search");
                if (ImGui::Button(clear_search.c_str())) {
                    search_.fill('\0');
                    model_.State().search.clear();
                    focus_plugin_search_ = true;
                }
            } else {
                ImGui::TextDisabled("%s", Text(anomaly::MessageId::PluginsNoFilterMatch));
                const std::string reset_filters = StableLabel(
                    anomaly::MessageId::PluginsResetFilters, "empty-reset-filters");
                if (ImGui::Button(reset_filters.c_str())) {
                    model_.State().plugin_filter = Filter::All;
                    anomaly::ReconcileUiStateSelection(
                        model_.Snapshot(), model_.State(), developer_mode_);
                }
            }
            EndShellBodyChild();
            return;
        }

        if (IsCompact()) {
            ImGui::BeginChild("PlatformPluginList", ImVec2(0.0f, 0.0f), true);
            for (std::size_t index = 0; index < visible.size(); ++index) {
                DrawShellPluginRow(visible[index],
                    index == 0 ? std::string_view{} : std::string_view{visible[index - 1].id},
                    index + 1 == visible.size() ? std::string_view{}
                                               : std::string_view{visible[index + 1].id});
            }
            ImGui::EndChild();
            return;
        }

        const float available = ImGui::GetContentRegionAvail().x;
        const float list_width = PluginListWidth(available);
        ImGui::BeginChild("PlatformPluginList", ImVec2(list_width, 0.0f), true);
        for (std::size_t index = 0; index < visible.size(); ++index) {
            DrawShellPluginRow(visible[index],
                index == 0 ? std::string_view{} : std::string_view{visible[index - 1].id},
                index + 1 == visible.size() ? std::string_view{}
                                           : std::string_view{visible[index + 1].id});
        }
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 splitter_origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("PlatformPluginSplit", ImVec2(6.0f, ImGui::GetContentRegionAvail().y));
        if (ImGui::IsItemActive()) {
            plugin_list_width_ = list_width + ImGui::GetIO().MouseDelta.x;
        }
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(splitter_origin.x + 2.5f, splitter_origin.y),
            ImVec2(splitter_origin.x + 2.5f, splitter_origin.y + ImGui::GetItemRectSize().y),
            ImGui::ColorConvertFloat4ToU32({0.204f, 0.235f, 0.271f, 1.0f}));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("PlatformPluginDetail", ImVec2(0.0f, 0.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const auto* selected = model_.Snapshot().FindPlugin(model_.State().selected_plugin_id);
        if (selected != nullptr) DrawPluginPublicDetail(*selected);
        else ImGui::TextDisabled("%s", Text(anomaly::MessageId::PluginsSelectDetails));
        ImGui::EndChild();
    }

    [[nodiscard]] bool PluginToggleAllowed(
        const anomaly::InstalledPluginView& plugin) const noexcept {
        if (HasPendingMutation(plugin.id, Mutation::None)) {
            return false;
        }
        const auto state = plugin.UiState();
        if (state == anomaly::PlatformUiPluginState::Quarantined ||
            state == anomaly::PlatformUiPluginState::Stopping) {
            return false;
        }
        return plugin.enabled || state == anomaly::PlatformUiPluginState::Disabled;
    }

    [[nodiscard]] std::string PluginToggleDisabledReason(
        const anomaly::InstalledPluginView& plugin) const {
        if (HasPendingMutation(plugin.id, Mutation::None)) {
            return Text(anomaly::MessageId::OperationInProgress);
        }
        switch (plugin.UiState()) {
        case anomaly::PlatformUiPluginState::Quarantined:
            return Text(anomaly::MessageId::PluginStateQuarantined);
        case anomaly::PlatformUiPluginState::Stopping:
            return Text(anomaly::MessageId::PluginStateStopping);
        default: return Text(anomaly::MessageId::PluginsNeedsAttention);
        }
    }

    void SubmitPluginToggle(const anomaly::InstalledPluginView& plugin) {
        Intent intent = model_.NewIntent(anomaly::PlatformUiIntentKind::SetPluginEnabled, plugin.id);
        intent.bool_value = !plugin.enabled;
        SubmitIntent(std::move(intent));
    }

    void DrawShellPluginRow(const anomaly::InstalledPluginView& plugin,
        const std::string_view previous_plugin_id, const std::string_view next_plugin_id) {
        const float height = IsCompact() ? 60.0f : 64.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float toggle_x = width - (IsCompact() ? 40.0f : 44.0f);
        const ImVec2 toggle_position = Offset(origin, toggle_x, (height - 18.0f) * 0.5f);
        const bool toggle_hovered = ImGui::IsMouseHoveringRect(
            toggle_position, Offset(toggle_position, 32.0f, 18.0f));
        const bool selected = model_.State().selected_plugin_id == plugin.id;
        ImGui::PushID(plugin.id.c_str());
        // Keep the row hit target clear of the toggle. Overlapping invisible
        // buttons otherwise leave the later toggle unable to own the click.
        if (keyboard_plugin_focus_id_ == plugin.id) {
            ImGui::SetKeyboardFocusHere();
            keyboard_plugin_focus_id_.clear();
        }
        const bool row_clicked = ImGui::InvisibleButton("row", ImVec2(
            std::max(0.0f, toggle_x - 4.0f), height));
        const bool row_focused = ImGui::IsItemFocused();
        const bool row_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        const bool enter_pressed = row_focused && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        const bool row_hovered = ImGui::IsMouseHoveringRect(origin, Offset(origin, width, height));
        if (row_clicked) {
            model_.State().selected_plugin_id = plugin.id;
            if (IsCompact() && !enter_pressed) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    compact_plugin_detail_pending_id_.clear();
                    if (!plugin.IsRunning() || !plugin.visibility_control) {
                        compact_plugin_detail_ = true;
                    }
                } else {
                    compact_plugin_detail_pending_id_ = plugin.id;
                    compact_plugin_detail_requested_at_ = ImGui::GetTime();
                }
            }
        }
        if (row_focused && !keyboard_plugin_navigation_consumed_) {
            std::string_view next_focus;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) next_focus = previous_plugin_id;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next_focus = next_plugin_id;
            if (!next_focus.empty()) {
                model_.State().selected_plugin_id = std::string(next_focus);
                keyboard_plugin_focus_id_ = next_focus;
                compact_plugin_detail_pending_id_.clear();
                keyboard_plugin_navigation_consumed_ = true;
            }
            if (enter_pressed && plugin.IsRunning() &&
                plugin.visibility_control) {
                Intent intent = model_.NewIntent(
                    anomaly::PlatformUiIntentKind::SetPluginVisible, plugin.id,
                    Mutation::SetVisible);
                intent.bool_value = !plugin.visible;
                SubmitIntent(std::move(intent));
            }
        }
        if (row_hovered && !toggle_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            plugin.IsRunning() && plugin.visibility_control) {
            compact_plugin_detail_pending_id_.clear();
            Intent intent = model_.NewIntent(
                anomaly::PlatformUiIntentKind::SetPluginVisible, plugin.id, Mutation::SetVisible);
            intent.bool_value = !plugin.visible;
            SubmitIntent(std::move(intent));
        }
        if (row_right_clicked) {
            model_.State().selected_plugin_id = plugin.id;
            compact_plugin_detail_pending_id_.clear();
            ImGui::OpenPopup("Platform plugin context");
        }

        const ImVec4 background = selected ? ImVec4(0.345f, 0.718f, 0.647f, 0.14f)
            : (row_hovered ? ImVec4(0.125f, 0.149f, 0.173f, 1.0f) : SurfaceColor());
        ImDrawList* const draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(origin, Offset(origin, width, height),
            ImGui::ColorConvertFloat4ToU32(background));
        draw_list->AddRectFilled(origin, Offset(origin, 3.0f, height),
            ImGui::ColorConvertFloat4ToU32(PluginStateColor(plugin.UiState())));
        const float icon_size = IsCompact() ? 36.0f : 40.0f;
        const ImVec2 icon_position = Offset(origin, 11.0f, (height - icon_size) * 0.5f);
        DrawGenericPluginIcon(icon_position, icon_size);
        const float text_x = icon_position.x + icon_size + 8.0f;
        const float status_x = toggle_x - 27.0f;
        const float text_width = std::max(20.0f, origin.x + status_x - text_x - 8.0f);
        const std::string name = Ellipsize(plugin.name.empty() ? plugin.id : plugin.name, text_width);
        draw_list->AddText(ImVec2(text_x, origin.y + 12.0f),
            ImGui::ColorConvertFloat4ToU32({0.949f, 0.957f, 0.965f, 1.0f}), name.c_str());
        if (!plugin.author.empty() || !plugin.version.empty()) {
            std::string metadata;
            if (!plugin.author.empty()) metadata = plugin.author;
            if (!plugin.version.empty()) {
                if (!metadata.empty()) metadata += "  ";
                metadata += "v" + plugin.version;
            }
            metadata = Ellipsize(metadata, text_width);
            draw_list->AddText(ImVec2(text_x, origin.y + 33.0f),
                ImGui::ColorConvertFloat4ToU32({0.667f, 0.698f, 0.737f, 1.0f}), metadata.c_str());
        }
        draw_list->AddText(Offset(origin, status_x,
                (height - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(PluginStateColor(plugin.UiState())),
            ShellGlyphText(StateGlyph(plugin.UiState())));

        const bool toggle_allowed = PluginToggleAllowed(plugin);
        ImGui::SetCursorScreenPos(toggle_position);
        ImGui::BeginDisabled(!toggle_allowed);
        const bool toggle_clicked = ImGui::InvisibleButton("enable-toggle", ImVec2(32.0f, 18.0f));
        ImGui::EndDisabled();
        const bool current_toggle_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (toggle_clicked && toggle_allowed) {
            model_.State().selected_plugin_id = plugin.id;
            SubmitPluginToggle(plugin);
        }
        const ImVec4 toggle_background = plugin.enabled ? ImVec4(0.345f, 0.718f, 0.647f, 0.35f)
            : ImVec4(0.094f, 0.114f, 0.133f, 1.0f);
        const ImVec4 toggle_border = plugin.enabled ? AccentColor()
            : ImVec4(0.298f, 0.337f, 0.380f, 1.0f);
        draw_list->AddRectFilled(toggle_position, Offset(toggle_position, 32.0f, 18.0f),
            ImGui::ColorConvertFloat4ToU32(toggle_background), 9.0f);
        draw_list->AddRect(toggle_position, Offset(toggle_position, 32.0f, 18.0f),
            ImGui::ColorConvertFloat4ToU32(toggle_border), 9.0f);
        const float knob_x = plugin.enabled ? 23.0f : 9.0f;
        draw_list->AddCircleFilled(Offset(toggle_position, knob_x, 9.0f), 6.0f,
            ImGui::ColorConvertFloat4ToU32(plugin.enabled
                ? ImVec4(0.851f, 0.973f, 0.945f, 1.0f)
                : ImVec4(0.667f, 0.698f, 0.737f, 1.0f)));
        if (current_toggle_hovered && !toggle_allowed) {
            const std::string reason = PluginToggleDisabledReason(plugin);
            ImGui::SetTooltip("%s", reason.c_str());
        } else if (row_hovered && !toggle_hovered) {
            ImGui::SetTooltip("%s", DisplayPluginState(plugin.UiState()));
        }
        ImGui::SetCursorScreenPos(Offset(origin, 0.0f, height));
        DrawPluginContextMenu(plugin);
        ImGui::PopID();
    }

    void DrawPluginContextMenu(const anomaly::InstalledPluginView& plugin) {
        if (!ImGui::BeginPopup("Platform plugin context")) return;
        const bool frozen = HasPendingMutation(plugin.id, Mutation::None);
        const auto* repository_plugin = RepositoryPlugin(plugin.id);
        const auto* repository_operation = RepositoryOperation(plugin.id);
        const bool repository_removed = repository_operation != nullptr &&
            repository_operation->kind == anomaly::RepositoryOperationKind::Uninstall &&
            repository_operation->state == anomaly::RepositoryOperationState::Succeeded;
        const bool repository_pending = repository_operation != nullptr &&
            repository_operation->state != anomaly::RepositoryOperationState::Succeeded &&
            repository_operation->state != anomaly::RepositoryOperationState::Failed &&
            repository_operation->state != anomaly::RepositoryOperationState::Cancelled;
        const auto submit_visibility = [this, &plugin] {
            Intent intent = model_.NewIntent(
                anomaly::PlatformUiIntentKind::SetPluginVisible, plugin.id, Mutation::SetVisible);
            intent.bool_value = !plugin.visible;
            SubmitIntent(std::move(intent));
        };
        if (plugin.UiState() == anomaly::PlatformUiPluginState::Active &&
            plugin.visibility_control) {
            if (ImGui::MenuItem(Text(plugin.visible ? anomaly::MessageId::CommonHide
                                                   : anomaly::MessageId::CommonOpen),
                    nullptr, false, !frozen)) {
                submit_visibility();
            }
            if (frozen) DrawTooltip(Text(anomaly::MessageId::OperationInProgress));
        }

        const bool toggle_allowed = PluginToggleAllowed(plugin);
        if (ImGui::MenuItem(Text(plugin.enabled ? anomaly::MessageId::CommonDisable
                                               : anomaly::MessageId::CommonEnable),
                nullptr, false, toggle_allowed)) {
            SubmitPluginToggle(plugin);
        }
        if (!toggle_allowed) {
            const std::string reason = PluginToggleDisabledReason(plugin);
            DrawTooltip(reason.c_str());
        }

        if (plugin.has_runtime_view) {
            if (ImGui::MenuItem(Text(anomaly::MessageId::CommonReload),
                    nullptr, false, !frozen)) {
                SubmitIntent(model_.NewIntent(
                    anomaly::PlatformUiIntentKind::ReloadPlugin, plugin.id, Mutation::Reload));
            }
            if (frozen) DrawTooltip(Text(anomaly::MessageId::OperationInProgress));
        }

        if (repository_plugin != nullptr && !repository_removed) {
            ImGui::Separator();
            if (ImGui::MenuItem(Text(anomaly::MessageId::CommonUninstall),
                    nullptr, false, !repository_pending)) {
                RequestRepositoryUninstall(plugin.id,
                    plugin.name.empty() ? plugin.id : plugin.name);
            }
            if (repository_pending) {
                DrawTooltip(Text(anomaly::MessageId::OperationInProgress));
            }
        }

        if (developer_mode_) {
            ImGui::Separator();
            if (ImGui::MenuItem(Text(anomaly::MessageId::PluginsOpenDeveloperDetails))) {
                OpenDeveloperPlugin(plugin.id);
            }
            if (ImGui::MenuItem(Text(anomaly::MessageId::PluginsViewLogs))) {
                OpenPluginLogs(plugin.id);
            }
            if (ImGui::MenuItem(Text(anomaly::MessageId::PluginsCopyId))) {
                ImGui::SetClipboardText(plugin.id.c_str());
                status_ = Text(anomaly::MessageId::PluginsIdCopied);
                status_failure_ = false;
            }
        }
        ImGui::EndPopup();
    }

    void DrawPluginPublicDetail(const anomaly::InstalledPluginView& plugin) {
        ImGui::PushID(plugin.id.c_str());
        const float height = IsCompact() ? 84.0f : 96.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f, 0.141f, 0.165f, 1.0f));
        ImGui::BeginChild("PlatformPluginDetailHeader", ImVec2(0.0f, height), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float icon_size = IsCompact() ? 44.0f : 56.0f;
        const float inset = IsCompact() ? 12.0f : 16.0f;
        DrawGenericPluginIcon(Offset(origin, inset, (height - icon_size) * 0.5f), icon_size);
        const float title_x = inset + icon_size + 12.0f;
        const float actions_width = IsCompact() ? 184.0f : 220.0f;
        const float identity_width = std::max(80.0f, size.x - title_x - actions_width - 12.0f);
        const std::string name = Ellipsize(plugin.name.empty() ? plugin.id : plugin.name, identity_width);
        ImGui::SetCursorScreenPos(Offset(origin, title_x, IsCompact() ? 17.0f : 20.0f));
        ImGui::SetWindowFontScale(1.15f);
        ImGui::TextUnformatted(name.c_str());
        ImGui::SetWindowFontScale(1.0f);
        DrawPluginStateBadge(Offset(ImGui::GetItemRectMax(), 8.0f, -20.0f), plugin.UiState());
        if (!plugin.author.empty() || !plugin.version.empty()) {
            std::string metadata = plugin.author;
            if (!plugin.version.empty()) {
                if (!metadata.empty()) metadata += "  ";
                metadata += "v" + plugin.version;
            }
            ImGui::SetCursorScreenPos(Offset(origin, title_x, IsCompact() ? 43.0f : 49.0f));
            ImGui::TextDisabled("%s", metadata.c_str());
        }
        DrawPluginPublicActions(plugin,
            Offset(origin, size.x - actions_width, IsCompact() ? 16.0f : 33.0f), actions_width);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        const float body_padding = IsCompact() ? 12.0f : 16.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(body_padding, body_padding));
        ImGui::BeginChild("PlatformPluginDetailBody", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextUnformatted(Text(anomaly::MessageId::PluginsAbout));
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(620.0f, ImGui::GetContentRegionAvail().x));
        ImGui::TextDisabled("%s", plugin.description.empty()
            ? Text(anomaly::MessageId::PluginsNoIntroduction)
            : plugin.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    void DrawPluginPublicActions(const anomaly::InstalledPluginView& plugin,
        const ImVec2 origin, const float available) {
        const bool frozen = HasPendingMutation(plugin.id, Mutation::None);
        const auto* repository_plugin = RepositoryPlugin(plugin.id);
        const auto* repository_operation = RepositoryOperation(plugin.id);
        const bool repository_removed = repository_operation != nullptr &&
            repository_operation->kind == anomaly::RepositoryOperationKind::Uninstall &&
            repository_operation->state == anomaly::RepositoryOperationState::Succeeded;
        const bool repository_pending = repository_operation != nullptr &&
            repository_operation->state != anomaly::RepositoryOperationState::Succeeded &&
            repository_operation->state != anomaly::RepositoryOperationState::Failed &&
            repository_operation->state != anomaly::RepositoryOperationState::Cancelled;
        const auto state = plugin.UiState();
        const auto submit_visibility = [this, &plugin] {
            Intent intent = model_.NewIntent(
                anomaly::PlatformUiIntentKind::SetPluginVisible, plugin.id, Mutation::SetVisible);
            intent.bool_value = !plugin.visible;
            SubmitIntent(std::move(intent));
        };
        const auto submit_reload = [this, &plugin] {
            SubmitIntent(model_.NewIntent(
                anomaly::PlatformUiIntentKind::ReloadPlugin, plugin.id, Mutation::Reload));
        };
        const bool can_open = state == anomaly::PlatformUiPluginState::Active && plugin.visibility_control;
        const char* primary_label = can_open
            ? Text(plugin.visible ? anomaly::MessageId::CommonHide
                                  : anomaly::MessageId::CommonOpen)
            : state == anomaly::PlatformUiPluginState::Disabled
                ? Text(anomaly::MessageId::CommonEnable)
            : state == anomaly::PlatformUiPluginState::Faulted
                ? Text(anomaly::MessageId::CommonReload)
            : state == anomaly::PlatformUiPluginState::DependencyBlocked
                ? Text(anomaly::MessageId::PluginsViewStatus)
                : Text(anomaly::MessageId::CommonDisable);
        const ShellGlyph primary_glyph = can_open ? (plugin.visible ? ShellGlyph::EyeOff : ShellGlyph::Eye)
            : state == anomaly::PlatformUiPluginState::Disabled ? ShellGlyph::Play
            : state == anomaly::PlatformUiPluginState::Faulted ? ShellGlyph::Refresh
            : state == anomaly::PlatformUiPluginState::DependencyBlocked ? ShellGlyph::Info
            : ShellGlyph::Stop;
        const bool primary_allowed = can_open ? !frozen
            : state == anomaly::PlatformUiPluginState::Faulted
                ? !frozen && plugin.has_runtime_view
            : state == anomaly::PlatformUiPluginState::DependencyBlocked
                ? true
                : PluginToggleAllowed(plugin);
        const float primary_width = std::max(74.0f, ImGui::CalcTextSize(primary_label).x + 38.0f);
        ImGui::SetCursorScreenPos(origin);
        if (DrawShellCommandButton("detail-primary", primary_label, primary_glyph, true,
                primary_allowed, ImVec2(primary_width, 30.0f))) {
            if (can_open) submit_visibility();
            else if (state == anomaly::PlatformUiPluginState::Faulted) submit_reload();
            else if (state == anomaly::PlatformUiPluginState::DependencyBlocked) OpenDeveloperPlugin(plugin.id);
            else if (PluginToggleAllowed(plugin)) {
                SubmitPluginToggle(plugin);
            }
        }
        if (!primary_allowed) DrawTooltip(
            frozen ? Text(anomaly::MessageId::OperationInProgress)
                   : PluginToggleDisabledReason(plugin).c_str());
        const bool show_reload = plugin.has_runtime_view &&
            state != anomaly::PlatformUiPluginState::Stopping &&
            state != anomaly::PlatformUiPluginState::Faulted;
        if (show_reload && primary_width + 82.0f <= available) {
            ImGui::SameLine(0.0f, 6.0f);
            if (DrawShellCommandButton("detail-reload", Text(anomaly::MessageId::CommonReload),
                    ShellGlyph::Refresh, false,
                    !frozen, ImVec2(76.0f, 30.0f))) {
                submit_reload();
            }
        }
        const bool show_uninstall = repository_plugin != nullptr && !repository_removed;
        const bool show_more = developer_mode_ || show_uninstall;
        if (show_more && primary_width + (show_reload ? 118.0f : 38.0f) <= available) {
            ImGui::SameLine(0.0f, 4.0f);
            if (DrawShellIconButton("detail-menu", ShellGlyph::More,
                    Text(anomaly::MessageId::PluginsMoreActions))) {
                ImGui::OpenPopup("Platform plugin actions");
            }
        }
        if (show_more) {
            if (ImGui::BeginPopup("Platform plugin actions")) {
                if (show_uninstall) {
                    if (ImGui::MenuItem(Text(anomaly::MessageId::CommonUninstall),
                            nullptr, false, !repository_pending)) {
                        RequestRepositoryUninstall(plugin.id,
                            plugin.name.empty() ? plugin.id : plugin.name);
                    }
                    if (repository_pending) {
                        DrawTooltip(Text(anomaly::MessageId::OperationInProgress));
                    }
                }
                if (developer_mode_) {
                    if (show_uninstall) ImGui::Separator();
                    if (ImGui::MenuItem(Text(anomaly::MessageId::PluginsOpenDeveloperDetails))) {
                        OpenDeveloperPlugin(plugin.id);
                    }
                    if (ImGui::MenuItem(Text(anomaly::MessageId::PluginsViewLogs))) {
                        OpenPluginLogs(plugin.id);
                    }
                    if (ImGui::MenuItem(Text(anomaly::MessageId::PluginsCopyId))) {
                        ImGui::SetClipboardText(plugin.id.c_str());
                        status_ = Text(anomaly::MessageId::PluginsIdCopied);
                        status_failure_ = false;
                    }
                }
                ImGui::EndPopup();
            }
        }
        if (frozen) DrawTooltip(Text(anomaly::MessageId::OperationInProgress));
    }

    void OpenDeveloperPlugin(const std::string_view plugin_id) {
        model_.State().selected_plugin_id = std::string(plugin_id);
        model_.State().route = Route::Diagnostics;
        model_.State().diagnostics_tab = DiagnosticTab::Developer;
        developer_panel_ = DeveloperPanel::Plugins;
        developer_plugin_tab_ = DeveloperPluginTab::Overview;
        compact_plugin_detail_ = false;
        compact_plugin_detail_pending_id_.clear();
    }

    void DrawNteFeatureMatrix() {
        const auto& compatibility = model_.Snapshot().nte_compatibility;
        ImGui::TextUnformatted(Text(anomaly::MessageId::PluginsFeatureMatrix));
        if (compatibility.features.empty()) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::PluginsFeatureProviderMissing));
        } else {
            const int columns = IsCompact() ? 3 : 4;
            if (ImGui::BeginTable("PlatformNteFeatures", columns,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonFeature),
                    ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonAvailability),
                    ImGuiTableColumnFlags_WidthFixed, 96.0f);
                ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonValidation),
                    ImGuiTableColumnFlags_WidthFixed, 88.0f);
                if (!IsCompact()) {
                    ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonReason),
                        ImGuiTableColumnFlags_WidthStretch, 1.2f);
                }
                ImGui::TableHeadersRow();
                for (const auto& feature : compatibility.features) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(feature.id.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(feature.available ? SuccessColor() : WarningColor(),
                        "%s", Text(feature.available ? anomaly::MessageId::PluginsTabAvailable
                                                     : anomaly::MessageId::CommonUnavailable));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Text(feature.validated
                        ? anomaly::MessageId::PluginsValidated
                        : anomaly::MessageId::PluginsMissing));
                    if (!IsCompact()) {
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", feature.reason.empty() ? "-" : feature.reason.c_str());
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(anomaly::MessageId::PluginsPublishedServices));
        if (compatibility.services.empty()) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::PluginsNoPublishedServices));
        } else {
            for (const auto& service : compatibility.services) {
                ImGui::Separator();
                ImGui::TextUnformatted(service.id.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("v%u", service.version);
            }
        }
    }

    void DrawNteBuildPanel() {
        const auto& compatibility = model_.Snapshot().nte_compatibility;
        ImGui::TextUnformatted(Text(anomaly::MessageId::PluginsBuildDetails));
        if (ImGui::BeginTable("PlatformNteBuild", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const auto row = [this](const char* label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.empty()
                    ? Text(anomaly::MessageId::CommonUnavailable) : value.c_str());
            };
            row(Text(anomaly::MessageId::PluginsBuildId), compatibility.build_id);
            row(Text(anomaly::MessageId::PluginsProfileSource), compatibility.profile_source);
            row(Text(anomaly::MessageId::PluginsProfileHash), compatibility.profile_hash);
            row(Text(anomaly::MessageId::PluginsResolverReason), compatibility.reason);
            ImGui::EndTable();
        }
    }

    void DrawDiagnosticsShell() {
        if (!developer_mode_ && model_.State().diagnostics_tab == DiagnosticTab::Developer) {
            model_.State().diagnostics_tab = DiagnosticTab::Overview;
        }
        DrawShellPageHeader(Text(anomaly::MessageId::ShellRouteDiagnostics), {},
            [](const ImVec2&, const ImVec2&) {});
        ImGui::BeginChild("PlatformDiagnosticsRoute", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawDiagnosticTabs();
        switch (model_.State().diagnostics_tab) {
        case DiagnosticTab::Overview: DrawDiagnosticsOverviewShell(); break;
        case DiagnosticTab::PluginPerformance: DrawPerformanceShell(); break;
        case DiagnosticTab::Logs: DrawLogsShell(); break;
        case DiagnosticTab::Developer: DrawDeveloperShell(); break;
        }
        ImGui::EndChild();
    }

    void DrawDiagnosticTabs() {
        ImGui::BeginChild("PlatformDiagnosticTabs", ImVec2(0.0f, 36.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const auto draw_tab = [this](const DiagnosticTab tab, const char* label) {
            const bool selected = model_.State().diagnostics_tab == tab;
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? SurfaceColor() : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, RaisedColor());
            const std::string stable_label = anomaly::StableDisplayLabel(
                label, "diagnostics-tab-" + std::to_string(static_cast<unsigned>(tab)));
            if (ImGui::Button(stable_label.c_str(), ImVec2(0.0f, 30.0f))) {
                model_.State().diagnostics_tab = tab;
            }
            ImGui::PopStyleColor(2);
            if (selected) {
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(minimum.x, maximum.y - 2.0f), maximum,
                    ImGui::ColorConvertFloat4ToU32(AccentColor()));
            }
        };
        draw_tab(DiagnosticTab::Overview, DiagnosticTabLabel(DiagnosticTab::Overview));
        ImGui::SameLine();
        draw_tab(DiagnosticTab::PluginPerformance,
            DiagnosticTabLabel(DiagnosticTab::PluginPerformance));
        ImGui::SameLine();
        draw_tab(DiagnosticTab::Logs, DiagnosticTabLabel(DiagnosticTab::Logs));
        if (developer_mode_) {
            ImGui::SameLine();
            draw_tab(DiagnosticTab::Developer, DiagnosticTabLabel(DiagnosticTab::Developer));
        }
        ImGui::EndChild();
    }

    void DrawDiagnosticsOverviewShell() {
        const auto& snapshot = model_.Snapshot();
        BeginShellBodyChild("PlatformDiagnosticsOverview");
        const int summary_columns = IsCompact() ? 2 : 4;
        if (ImGui::BeginTable("PlatformDiagnosticsSummary", summary_columns,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const auto summary = [](const char* label, const std::string& value) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TextUnformatted(value.c_str());
            };
            summary(Text(anomaly::MessageId::DiagnosticsRuntime),
                snapshot.diagnostics.runtime_version.empty()
                    ? std::string{Text(anomaly::MessageId::CommonUnavailable)}
                    : snapshot.diagnostics.runtime_version);
            summary(Text(anomaly::MessageId::DiagnosticsProcess),
                "PID " + std::to_string(snapshot.diagnostics.process_id));
            summary(Text(anomaly::MessageId::DiagnosticsInstalledRunningIssues),
                std::to_string(snapshot.runtime_summary.installed) +
                " / " + std::to_string(snapshot.runtime_summary.running) +
                " / " + std::to_string(snapshot.runtime_summary.issues));
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(anomaly::MessageId::DiagnosticsRecentFaults));
        ImGui::Separator();
        if (snapshot.diagnostics.recent_faults.empty()) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DiagnosticsNoRecentFaults));
        } else {
            for (const auto& fault : snapshot.diagnostics.recent_faults) {
                ImGui::TextColored(ErrorColor(), "!");
                ImGui::SameLine();
                ImGui::TextWrapped("%s", fault.c_str());
            }
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(anomaly::MessageId::DiagnosticsRecentOperations));
        ImGui::Separator();
        if (const auto* operation = PresentedOperation()) {
            const std::string label = OperationDisplayLabel(*operation);
            ImGui::TextColored(OperationStateColor(operation->state), "%s", label.c_str());
            ImGui::SameLine();
            const std::string view_details = StableLabel(
                anomaly::MessageId::DiagnosticsViewDetails, "diagnostics-view-operation");
            if (ImGui::Button(view_details.c_str())) RequestOperationDetailsPopup();
        } else {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DiagnosticsNoCurrentOperations));
        }
        EndShellBodyChild();
    }

    void DrawPerformanceShell() {
        ImGui::BeginChild("PlatformPerformanceToolbar", ImVec2(0.0f, IsCompact() ? 82.0f : 50.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        SetAvailableItemWidth(IsCompact() ? 40.0f : 0.0f, 260.0f);
        ImGui::InputTextWithHint("##performance-search",
            Text(anomaly::MessageId::PluginsSearchHint), performance_search_.data(),
            performance_search_.size());
        if (IsCompact()) ImGui::NewLine();
        else ImGui::SameLine();
        ImGui::SetNextItemWidth(88.0f);
        const char* callbacks[] = {
            Text(anomaly::MessageId::PluginsFilterAll),
            Text(anomaly::MessageId::CommonUpdate),
            Text(anomaly::MessageId::CommonDraw)};
        ImGui::Combo("##performance-callback", &performance_callback_filter_, callbacks, IM_ARRAYSIZE(callbacks));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        const char* states[] = {
            Text(anomaly::MessageId::PluginsFilterAll),
            Text(anomaly::MessageId::PluginStateRunning),
            Text(anomaly::MessageId::PluginsFilterIssues),
            Text(anomaly::MessageId::PluginStateDisabled)};
        ImGui::Combo("##performance-state", &performance_state_filter_, states, IM_ARRAYSIZE(states));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(106.0f);
        const char* sorts[] = {"p95", "p99", Text(anomaly::MessageId::CommonFaults),
            Text(anomaly::MessageId::DiagnosticsSlowCalls),
            Text(anomaly::MessageId::CommonName)};
        ImGui::Combo("##performance-sort", &performance_sort_, sorts, IM_ARRAYSIZE(sorts));
        ImGui::EndChild();

        std::vector<PerformanceRow> rows;
        const std::string_view query(performance_search_.data());
        for (const auto& plugin : model_.Snapshot().installed_plugins) {
            if (!developer_mode_ && plugin.audience == anomaly::PluginAudience::Developer) continue;
            if (!query.empty() && !anomaly::MatchesPluginSearch(plugin, query)) continue;
            if (performance_state_filter_ == 1 && !plugin.IsRunning()) continue;
            if (performance_state_filter_ == 2 && !plugin.HasIssue()) continue;
            if (performance_state_filter_ == 3 && !plugin.IsDisabled()) continue;
            if (performance_callback_filter_ != 2) {
                rows.push_back({&plugin, &plugin.update_metrics, true});
            }
            if (performance_callback_filter_ != 1) {
                rows.push_back({&plugin, &plugin.draw_metrics, false});
            }
        }
        const auto metric_sort_value = [this](const PerformanceRow& row) {
            switch (performance_sort_) {
            case 0: return row.metrics->p95_milliseconds;
            case 1: return row.metrics->p99_milliseconds;
            case 2: return static_cast<double>(row.metrics->faults);
            case 3: return static_cast<double>(row.metrics->slow_calls);
            default: return 0.0;
            }
        };
        std::sort(rows.begin(), rows.end(), [&](const PerformanceRow& left, const PerformanceRow& right) {
            if (performance_sort_ == 4) {
                const std::string& left_name = left.plugin->name.empty() ? left.plugin->id : left.plugin->name;
                const std::string& right_name = right.plugin->name.empty() ? right.plugin->id : right.plugin->name;
                return left_name == right_name ? left.update > right.update : left_name < right_name;
            }
            const double left_value = metric_sort_value(left);
            const double right_value = metric_sort_value(right);
            if (left_value != right_value) return left_value > right_value;
            return left.plugin->id == right.plugin->id
                ? left.update > right.update : left.plugin->id < right.plugin->id;
        });

        if (layout_mode_ == LayoutMode::Wide) {
            const float table_width = std::max(420.0f, ImGui::GetContentRegionAvail().x * 0.62f);
            BeginShellBodyChild("PlatformPerformanceTablePane", ImVec2(table_width, 0.0f), true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawPerformanceTable(rows);
            EndShellBodyChild();
            ImGui::SameLine(0.0f, 0.0f);
            BeginShellBodyChild("PlatformPerformanceDetail", {}, true);
            DrawSelectedPerformanceDetail();
            EndShellBodyChild();
            return;
        }

        const float detail_height = IsCompact() ? 0.0f : 168.0f;
        BeginShellBodyChild("PlatformPerformanceTablePane", ImVec2(0.0f,
            detail_height == 0.0f ? 0.0f : -detail_height - 4.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawPerformanceTable(rows);
        EndShellBodyChild();
        if (!IsCompact()) {
            BeginShellBodyChild("PlatformPerformanceDetail", ImVec2(0.0f, detail_height), true);
            DrawSelectedPerformanceDetail();
            EndShellBodyChild();
        }
    }

    void DrawPerformanceTable(const std::vector<PerformanceRow>& rows) {
        const int columns = IsCompact() ? 5 : 9;
        if (!ImGui::BeginTable("PlatformPerformanceTable", columns,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 0.0f))) {
            return;
        }
        ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonPlugin),
            ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonCallback),
            ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonCalls),
            ImGuiTableColumnFlags_WidthFixed, 64.0f);
        if (IsCompact()) {
            ImGui::TableSetupColumn("p95", ImGuiTableColumnFlags_WidthFixed, 66.0f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonState),
                ImGuiTableColumnFlags_WidthFixed, 72.0f);
        } else {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonFaults),
                ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonSlow),
                ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableSetupColumn("p50", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn("p95", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn("p99", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonState),
                ImGuiTableColumnFlags_WidthFixed, 82.0f);
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const PerformanceRow& row = rows[static_cast<std::size_t>(index)];
                const CallbackMetricsView& metrics = *row.metrics;
                ImGui::PushID((row.plugin->id + (row.update ? "-update" : "-draw")).c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string& name = row.plugin->name.empty() ? row.plugin->id : row.plugin->name;
                if (ImGui::Selectable(name.c_str(), performance_selected_plugin_id_ == row.plugin->id,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    performance_selected_plugin_id_ = row.plugin->id;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Text(row.update
                    ? anomaly::MessageId::CommonUpdate : anomaly::MessageId::CommonDraw));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(metrics.calls));
                if (IsCompact()) {
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) {
                        ImGui::TextDisabled("%s", Text(anomaly::MessageId::CommonNone));
                    }
                    else ImGui::Text("%.3f", metrics.p95_milliseconds);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(PluginStateColor(row.plugin->UiState()), "%s",
                        DisplayPluginState(row.plugin->UiState()));
                } else {
                    ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.faults));
                    ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.slow_calls));
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(anomaly::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f", metrics.p50_milliseconds);
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(anomaly::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f", metrics.p95_milliseconds);
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(anomaly::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f", metrics.p99_milliseconds);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(PluginStateColor(row.plugin->UiState()), "%s",
                        DisplayPluginState(row.plugin->UiState()));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void DrawSelectedPerformanceDetail() {
        const auto* plugin = model_.Snapshot().FindPlugin(performance_selected_plugin_id_);
        if (plugin == nullptr) {
            ImGui::TextUnformatted(Text(anomaly::MessageId::DiagnosticsSelectedCallback));
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DiagnosticsSelectCallback));
            return;
        }
        ImGui::Text("%s", plugin->name.empty() ? plugin->id.c_str() : plugin->name.c_str());
        const std::string generation = std::to_string(plugin->generation);
        const std::array<std::string_view, 1> arguments{generation};
        const std::string generation_text =
            Format(anomaly::MessageId::DiagnosticsGeneration, arguments);
        ImGui::TextDisabled("%s", generation_text.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DiagnosticsUpdateBudget));
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DiagnosticsDrawBudget));
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DiagnosticsDeveloperHint));
    }

    void DrawLogsShell() {
        ImGui::BeginChild("PlatformLogsToolbar", ImVec2(0.0f, IsCompact() ? 82.0f : 50.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        SetAvailableItemWidth(IsCompact() ? 40.0f : 0.0f, 280.0f);
        if (ImGui::InputTextWithHint("##diagnostic-log-filter",
                Text(anomaly::MessageId::DiagnosticsSearchLogs), log_filter_.data(),
                log_filter_.size())) {
            model_.State().diagnostics_log_filter = log_filter_.data();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(model_.State().diagnostics_log_filter.empty());
        if (IconButton("x##clear-log-filter", Text(anomaly::MessageId::PluginsClearSearch))) {
            log_filter_.fill('\0');
            model_.State().diagnostics_log_filter.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, logs_follow_live_ ? AccentColor() : SurfaceColor());
        const std::string live_label = StableLabel(
            anomaly::MessageId::DiagnosticsLive, "diagnostics-live");
        if (ImGui::Button(live_label.c_str())) logs_follow_live_ = !logs_follow_live_;
        ImGui::PopStyleColor();

        const auto events = plugins_.Events();
        const std::string_view text_filter(log_filter_.data());
        const std::string_view plugin_filter(model_.State().diagnostics_plugin_id);
        std::vector<std::size_t> filtered;
        filtered.reserve(events.size());
        for (std::size_t index = 0; index < events.size(); ++index) {
            if (!text_filter.empty() && events[index].find(text_filter) == std::string::npos) continue;
            if (!plugin_filter.empty() && events[index].find(plugin_filter) == std::string::npos) continue;
            filtered.push_back(index);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(filtered.empty());
        const std::string copy_filtered = StableLabel(
            anomaly::MessageId::DiagnosticsCopyFiltered, "diagnostics-copy-filtered");
        if (ImGui::Button(copy_filtered.c_str())) {
            std::string copied;
            for (const std::size_t index : filtered) {
                copied += events[index];
                copied += '\n';
            }
            ImGui::SetClipboardText(copied.c_str());
            status_ = Text(anomaly::MessageId::DiagnosticsFilteredCopied);
            status_failure_ = false;
        }
        ImGui::EndDisabled();
        ImGui::EndChild();

        if (selected_log_index_ >= events.size() && !filtered.empty()) selected_log_index_ = filtered.front();
        const float detail_height = IsCompact() ? 0.0f : 160.0f;
        BeginShellBodyChild("PlatformLogRecords", ImVec2(0.0f,
            detail_height == 0.0f ? 0.0f : -detail_height - 4.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted(Text(anomaly::MessageId::DiagnosticsRawLogStream));
        ImGui::SameLine();
        const std::string event_count = std::to_string(events.size());
        const std::array<std::string_view, 1> count_arguments{event_count};
        const std::string buffered =
            Format(anomaly::MessageId::DiagnosticsBufferedRecords, count_arguments);
        ImGui::TextDisabled("%s", buffered.c_str());
        if (ImGui::BeginTable("PlatformLogTable", IsCompact() ? 1 : 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 0.0f))) {
            if (!IsCompact()) ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonRecord),
                ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonMessage),
                ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(filtered.size()));
            while (clipper.Step()) {
                for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                    const std::size_t event_index = filtered[static_cast<std::size_t>(display_index)];
                    ImGui::PushID(static_cast<int>(event_index));
                    ImGui::TableNextRow();
                    if (!IsCompact()) {
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", event_index + 1);
                    }
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(events[event_index].c_str(), selected_log_index_ == event_index,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_log_index_ = event_index;
                    }
                    ImGui::PopID();
                }
            }
            if (logs_follow_live_ && !filtered.empty()) ImGui::SetScrollHereY(1.0f);
            ImGui::EndTable();
        }
        EndShellBodyChild();
        if (!IsCompact()) {
            BeginShellBodyChild("PlatformLogDetail", ImVec2(0.0f, detail_height), true);
            ImGui::TextUnformatted(Text(anomaly::MessageId::DiagnosticsSelectedRecord));
            if (selected_log_index_ < events.size()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", events[selected_log_index_].c_str());
            } else {
                ImGui::TextDisabled("%s", Text(anomaly::MessageId::DiagnosticsSelectRecord));
            }
            EndShellBodyChild();
        }
    }

    [[nodiscard]] const char* DeveloperPanelLabel(const DeveloperPanel panel) const noexcept {
        switch (panel) {
        case DeveloperPanel::Plugins: return Text(anomaly::MessageId::DeveloperPanelPlugins);
        case DeveloperPanel::Services: return Text(anomaly::MessageId::DeveloperPanelServices);
        case DeveloperPanel::Hooks: return Text(anomaly::MessageId::DeveloperPanelHooks);
        case DeveloperPanel::Memory: return Text(anomaly::MessageId::DeveloperPanelMemory);
        case DeveloperPanel::NteProfile:
            return Text(anomaly::MessageId::DeveloperPanelNteProfile);
        }
        return Text(anomaly::MessageId::DeveloperPanelPlugins);
    }

    void DrawDeveloperShell() {
        const auto draw_content = [this] {
            switch (developer_panel_) {
            case DeveloperPanel::Plugins: DrawDeveloperPluginShell(); break;
            case DeveloperPanel::Services: DrawServices(); break;
            case DeveloperPanel::Hooks: DrawHooks(); break;
            case DeveloperPanel::Memory: DrawMemory(); break;
            case DeveloperPanel::NteProfile: DrawDeveloperNteCompatibility(); break;
            }
        };
        const auto draw_content_panel = [this, &draw_content] {
            if (developer_panel_ == DeveloperPanel::Plugins) {
                ImGui::BeginChild("PlatformDeveloperContent", ImVec2(0.0f, 0.0f), false);
                draw_content();
                ImGui::EndChild();
                return;
            }
            BeginShellBodyChild("PlatformDeveloperContent");
            draw_content();
            EndShellBodyChild();
        };
        if (IsCompact()) {
            ImGui::BeginChild("PlatformDeveloperPicker", ImVec2(0.0f, 48.0f), true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            SetAvailableItemWidth();
            if (ImGui::BeginCombo("##developer-panel", DeveloperPanelLabel(developer_panel_))) {
                constexpr std::array<DeveloperPanel, 5> panels{
                    DeveloperPanel::Plugins, DeveloperPanel::Services, DeveloperPanel::Hooks,
                    DeveloperPanel::Memory, DeveloperPanel::NteProfile};
                for (const DeveloperPanel panel : panels) {
                    ImGui::PushID(static_cast<int>(panel));
                    if (ImGui::Selectable(DeveloperPanelLabel(panel), developer_panel_ == panel)) {
                        developer_panel_ = panel;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::EndChild();
            draw_content_panel();
            return;
        }

        ImGui::BeginChild("PlatformDeveloperRail", ImVec2(168.0f, 0.0f), true);
        constexpr std::array<DeveloperPanel, 5> panels{
            DeveloperPanel::Plugins, DeveloperPanel::Services, DeveloperPanel::Hooks,
            DeveloperPanel::Memory, DeveloperPanel::NteProfile};
        for (const DeveloperPanel panel : panels) {
            const bool selected = developer_panel_ == panel;
            ImGui::PushStyleColor(ImGuiCol_Header, selected
                ? ImVec4(0.345f, 0.718f, 0.647f, 0.16f) : ImVec4(0, 0, 0, 0));
            ImGui::PushID(static_cast<int>(panel));
            if (ImGui::Selectable(
                    DeveloperPanelLabel(panel), selected, 0, ImVec2(0.0f, 32.0f))) {
                developer_panel_ = panel;
            }
            ImGui::PopID();
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        draw_content_panel();
    }

    void DrawDeveloperPluginShell() {
        const auto& plugins = model_.Snapshot().installed_plugins;
        if (plugins.empty()) {
            DrawShellProviderUnavailable(
                Text(anomaly::MessageId::DeveloperPluginsTitle),
                Text(anomaly::MessageId::DeveloperCatalogUnavailable));
            return;
        }
        const auto* selected = model_.Snapshot().FindPlugin(model_.State().selected_plugin_id);
        if (selected == nullptr) selected = &plugins.front();
        ImGui::BeginChild("PlatformDeveloperPluginToolbar", ImVec2(0.0f, 50.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::CommonPlugin));
        ImGui::SameLine();
        SetAvailableItemWidth(56.0f, 300.0f);
        if (ImGui::BeginCombo("##developer-plugin", selected->name.empty()
                ? selected->id.c_str() : selected->name.c_str())) {
            for (const auto& plugin : plugins) {
                const char* name = plugin.name.empty() ? plugin.id.c_str() : plugin.name.c_str();
                if (ImGui::Selectable(name, plugin.id == selected->id)) {
                    model_.State().selected_plugin_id = plugin.id;
                    selected = &plugin;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndChild();

        ImGui::PushID(selected->id.c_str());
        ImGui::BeginChild("PlatformDeveloperPluginHeader", ImVec2(0.0f, 84.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 icon_position = Offset(ImGui::GetCursorScreenPos(), 12.0f, 14.0f);
        DrawGenericPluginIcon(icon_position, 56.0f);
        ImGui::Dummy(ImVec2(68.0f, 0.0f));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SetWindowFontScale(1.12f);
        ImGui::TextUnformatted(selected->name.empty() ? selected->id.c_str() : selected->name.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
        ImGui::TextColored(PluginStateColor(selected->UiState()), "%s",
            DisplayPluginState(selected->UiState()));
        ImGui::TextDisabled("%s", selected->id.c_str());
        ImGui::EndGroup();
        ImGui::EndChild();

        ImGui::BeginChild("PlatformDeveloperPluginTabs", ImVec2(0.0f, 36.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const auto draw_tab = [this](const DeveloperPluginTab tab, const char* label) {
            const bool active = developer_plugin_tab_ == tab;
            ImGui::PushStyleColor(ImGuiCol_Button, active ? SurfaceColor() : ImVec4(0, 0, 0, 0));
            const std::string stable_label = anomaly::StableDisplayLabel(
                label, "developer-plugin-tab-" + std::to_string(static_cast<unsigned>(tab)));
            if (ImGui::Button(stable_label.c_str(), ImVec2(0.0f, 30.0f))) {
                developer_plugin_tab_ = tab;
            }
            ImGui::PopStyleColor();
        };
        draw_tab(DeveloperPluginTab::Overview, Text(anomaly::MessageId::DiagnosticsTabOverview));
        if (selected->has_runtime_view) {
            ImGui::SameLine();
            draw_tab(DeveloperPluginTab::Capabilities,
                Text(anomaly::MessageId::DeveloperTabCapabilities));
            ImGui::SameLine();
            draw_tab(DeveloperPluginTab::Performance,
                Text(anomaly::MessageId::DeveloperTabPerformance));
        }
        ImGui::SameLine();
        draw_tab(DeveloperPluginTab::Logs, Text(anomaly::MessageId::DiagnosticsTabLogs));
        ImGui::EndChild();

        if (!selected->has_runtime_view &&
            (developer_plugin_tab_ == DeveloperPluginTab::Capabilities ||
             developer_plugin_tab_ == DeveloperPluginTab::Performance)) {
            developer_plugin_tab_ = DeveloperPluginTab::Overview;
        }
        BeginShellBodyChild("PlatformDeveloperPluginBody");
        switch (developer_plugin_tab_) {
        case DeveloperPluginTab::Overview: DrawDeveloperPluginOverview(*selected); break;
        case DeveloperPluginTab::Capabilities: DrawDeveloperPluginCapabilities(*selected); break;
        case DeveloperPluginTab::Performance: DrawDeveloperPluginPerformance(*selected); break;
        case DeveloperPluginTab::Logs: DrawDeveloperPluginLogs(*selected); break;
        }
        EndShellBodyChild();
        ImGui::PopID();
    }

    void DrawDeveloperPluginOverview(const anomaly::InstalledPluginView& plugin) {
        std::string dependencies;
        for (const auto& dependency : plugin.dependency_ids) {
            if (!dependencies.empty()) dependencies += ", ";
            dependencies += dependency;
        }
        if (dependencies.empty()) dependencies = Text(anomaly::MessageId::CommonNone);
        const std::string generation = std::to_string(plugin.generation);
        const std::array<std::string_view, 1> generation_arguments{generation};
        const std::string runtime_identity = plugin.has_runtime_view
            ? Format(anomaly::MessageId::DeveloperCurrentAbi, generation_arguments)
            : Text(anomaly::MessageId::DeveloperNoGeneration);
        if (ImGui::BeginTable("PlatformDeveloperOverview", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const auto row = [](const char* label, const std::string_view value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
            };
            row(Text(anomaly::MessageId::CommonState), DisplayPluginState(plugin.UiState()));
            const std::string_view reason = plugin.Reason();
            row(Text(anomaly::MessageId::DeveloperStatusReason), reason.empty()
                ? Text(anomaly::MessageId::DeveloperNoCompatibilityIssues) : reason);
            row(Text(anomaly::MessageId::DeveloperDependencies), dependencies);
            row(Text(anomaly::MessageId::DeveloperRuntimeIdentity), runtime_identity);
            row(Text(anomaly::MessageId::DeveloperPackage), plugin.package_directory.empty()
                ? Text(anomaly::MessageId::CommonUnavailable)
                : std::string_view{plugin.package_directory.string()});
            row(Text(anomaly::MessageId::DeveloperPluginId), plugin.id);
            ImGui::EndTable();
        }
        const std::string copy_id = StableLabel(
            anomaly::MessageId::PluginsCopyId, "developer-copy-plugin-id");
        if (ImGui::Button(copy_id.c_str())) {
            ImGui::SetClipboardText(plugin.id.c_str());
            status_ = Text(anomaly::MessageId::PluginsIdCopied);
            status_failure_ = false;
        }
    }

    void DrawDeveloperPluginCapabilities(const anomaly::InstalledPluginView& plugin) {
        if (!plugin.has_runtime_view) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperNoGeneration));
            return;
        }
        const auto& diagnostics = plugin.platform_diagnostics;
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperGrantedCapabilities));
        ImGui::Separator();
        if (diagnostics.capabilities.empty()) ImGui::TextDisabled(
            "%s", Text(anomaly::MessageId::DeveloperNoCapabilities));
        else for (const auto& capability : diagnostics.capabilities) ImGui::BulletText("%s", capability.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperServiceVersions));
        if (diagnostics.services.empty()) ImGui::TextDisabled(
            "%s", Text(anomaly::MessageId::DeveloperNoServices));
        else for (const auto& service : diagnostics.services) {
            ImGui::BulletText("%s v%u", service.id.c_str(), service.version);
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperResourceCounts));
        if (ImGui::BeginTable("PlatformDeveloperResources", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const PluginResourceCountsView& resources = diagnostics.resources;
            const std::array<std::pair<const char*, std::size_t>, 13> rows{{
                {Text(anomaly::MessageId::DeveloperResourceLedger), resources.ledger_resources},
                {Text(anomaly::MessageId::DeveloperResourceConfig), resources.configs},
                {Text(anomaly::MessageId::DeveloperResourceSelfTest), resources.self_tests},
                {Text(anomaly::MessageId::DeveloperResourceTask), resources.tasks},
                {Text(anomaly::MessageId::DeveloperResourceIpc), resources.ipc_resources},
                {Text(anomaly::MessageId::DeveloperResourceCommand), resources.commands},
                {Text(anomaly::MessageId::DeveloperResourceNotification), resources.notifications},
                {Text(anomaly::MessageId::DeveloperResourceHook), resources.hooks},
                {Text(anomaly::MessageId::DeveloperResourcePatch), resources.patches},
                {Text(anomaly::MessageId::DeveloperResourceWindow), resources.windows},
                {Text(anomaly::MessageId::DeveloperResourceFont), resources.fonts},
                {Text(anomaly::MessageId::DeveloperResourceTexture), resources.textures},
                {Text(anomaly::MessageId::DeveloperResourceHotkey), resources.hotkeys},
            }};
            for (const auto& [label, count] : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::Text("%zu", count);
            }
            ImGui::EndTable();
        }
        if (!diagnostics.deny_reasons.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperCapabilityDenies));
            for (const auto& reason : diagnostics.deny_reasons) ImGui::BulletText("%s", reason.c_str());
        }
    }

    void DrawDeveloperPluginPerformance(const anomaly::InstalledPluginView& plugin) {
        if (!plugin.has_runtime_view) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperNoGeneration));
            return;
        }
        if (ImGui::BeginTable("PlatformDeveloperPluginPerformance", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonCallback));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonCalls));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonFaults));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonSlow));
            ImGui::TableSetupColumn("p50"); ImGui::TableSetupColumn("p95");
            ImGui::TableSetupColumn("p99"); ImGui::TableHeadersRow();
            const auto row = [this](const char* label, const CallbackMetricsView& metrics) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.calls));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.faults));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.slow_calls));
                for (const double percentile : {metrics.p50_milliseconds, metrics.p95_milliseconds,
                                                metrics.p99_milliseconds}) {
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(anomaly::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f ms", percentile);
                }
            };
            row(Text(anomaly::MessageId::CommonUpdate), plugin.update_metrics);
            row(Text(anomaly::MessageId::CommonDraw), plugin.draw_metrics);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperCallbackBudget));
        const auto& endpoints = plugin.platform_diagnostics.ipc_endpoints;
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperIpcEndpoints));
        if (endpoints.empty()) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperNoIpcEndpoints));
            return;
        }
        if (ImGui::BeginTable("PlatformDeveloperPluginIpc", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperEndpoint));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperRole));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonVersion));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperConsumers));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonCalls));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperFailuresTimeouts));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperSubscriptionsPending));
            ImGui::TableSetupColumn("p95");
            ImGui::TableHeadersRow();
            for (const anomaly::IpcEndpointDiagnostics& endpoint : endpoints) {
                std::string consumers;
                for (const std::string& consumer : endpoint.consumers) {
                    if (!consumers.empty()) consumers += ", ";
                    consumers += consumer;
                }
                if (consumers.empty()) consumers = Text(anomaly::MessageId::CommonNone);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(endpoint.id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(
                    Text(endpoint.provider == plugin.id
                        ? anomaly::MessageId::CommonProvider
                        : anomaly::MessageId::CommonConsumer));
                ImGui::TableNextColumn(); ImGui::Text("%u.%u",
                    endpoint.major_version, endpoint.minor_version);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", consumers.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu",
                    static_cast<unsigned long long>(endpoint.calls));
                ImGui::TableNextColumn(); ImGui::Text("%llu / %llu",
                    static_cast<unsigned long long>(endpoint.failures),
                    static_cast<unsigned long long>(endpoint.timeouts));
                ImGui::TableNextColumn(); ImGui::Text("%zu / %zu",
                    endpoint.subscriptions, endpoint.pending_calls);
                ImGui::TableNextColumn(); ImGui::Text("%.3f ms", endpoint.p95_milliseconds);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        for (const anomaly::IpcEndpointDiagnostics& endpoint : endpoints) {
            ImGui::TextUnformatted(endpoint.id.c_str());
            const std::array<std::string_view, 1> request_arguments{
                endpoint.request_schema_hash};
            const std::array<std::string_view, 1> response_arguments{
                endpoint.response_schema_hash};
            const std::array<std::string_view, 1> event_arguments{
                endpoint.event_schema_hash};
            ImGui::TextWrapped("%s", Format(
                anomaly::MessageId::DeveloperRequestSchema, request_arguments).c_str());
            ImGui::TextWrapped("%s", Format(
                anomaly::MessageId::DeveloperResponseSchema, response_arguments).c_str());
            ImGui::TextWrapped("%s", Format(
                anomaly::MessageId::DeveloperEventSchema, event_arguments).c_str());
        }
    }

    void DrawDeveloperPluginLogs(const anomaly::InstalledPluginView& plugin) {
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperRecentRecords));
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperRawRecordsHint));
        const std::string open_logs = StableLabel(
            anomaly::MessageId::DeveloperOpenFullLogs, "developer-open-full-logs");
        if (ImGui::Button(open_logs.c_str())) OpenPluginLogs(plugin.id);
    }

    void DrawDeveloperNteCompatibility() {
        const auto& compatibility = model_.Snapshot().nte_compatibility;
        ImGui::TextUnformatted(Text(anomaly::MessageId::ShellRouteNteCompatibility));
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperExactBuildHint));
        ImGui::Separator();
        const std::array<std::string_view, 1> level_arguments{
            NteLevelName(compatibility.level)};
        const std::string level = Format(anomaly::MessageId::DeveloperLevel, level_arguments);
        ImGui::TextUnformatted(level.c_str());
        const std::string_view reason = compatibility.reason.empty()
            ? Text(anomaly::MessageId::DeveloperNoCompatibilityProvider)
            : std::string_view{compatibility.reason};
        const std::array<std::string_view, 1> reason_arguments{reason};
        const std::string reason_text =
            Format(anomaly::MessageId::DeveloperReason, reason_arguments);
        ImGui::TextWrapped("%s", reason_text.c_str());
        ImGui::Spacing();
        if (IsCompact()) {
            DrawNteFeatureMatrix();
            DrawNteBuildPanel();
        } else if (ImGui::BeginTable("PlatformDeveloperNteColumns", 2,
                       ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperMatrix),
                ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperBuild),
                ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawNteFeatureMatrix();
            ImGui::TableNextColumn();
            DrawNteBuildPanel();
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperRawProfileJson));
        if (!diagnostics_.profile_json) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperProfileUnavailable));
            return;
        }
        const std::string profile = diagnostics_.profile_json();
        if (profile.empty()) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperProfileUnavailable));
            return;
        }
        ImGui::TextWrapped("%s", profile.c_str());
    }

    const char* SettingsSectionName(const SettingsSection section) const noexcept {
        switch (section) {
        case SettingsSection::Interface: return Text(anomaly::MessageId::SettingsSectionInterface);
        case SettingsSection::Input: return Text(anomaly::MessageId::SettingsSectionInput);
        case SettingsSection::Updates: return Text(anomaly::MessageId::SettingsSectionUpdates);
        case SettingsSection::Diagnostics:
            return Text(anomaly::MessageId::SettingsSectionDiagnostics);
        case SettingsSection::Advanced: return Text(anomaly::MessageId::SettingsSectionAdvanced);
        case SettingsSection::About: return Text(anomaly::MessageId::SettingsSectionAbout);
        }
        return Text(anomaly::MessageId::ShellRouteSettings);
    }

    [[nodiscard]] bool SettingMatches(
        const std::string_view label,
        const std::string_view description,
        const std::string_view keywords = {}) const {
        const std::string_view query(settings_search_.data());
        if (query.empty()) return true;
        const auto contains = [](const std::string_view value, const std::string_view needle) {
            std::string haystack(value);
            std::string lowered(needle);
            std::ranges::transform(haystack, haystack.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            std::ranges::transform(lowered, lowered.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return haystack.find(lowered) != std::string::npos;
        };
        return contains(label, query) || contains(description, query) || contains(keywords, query);
    }

    [[nodiscard]] const char* SettingError(const std::string_view id) const noexcept {
        const auto found = std::ranges::find(settings_validation_errors_, id,
            &anomaly::PlatformSettingsValidationError::setting_id);
        if (found == settings_validation_errors_.end()) return nullptr;
        if (id == "interface.language") {
            return Text(anomaly::MessageId::SettingsValidationLanguage);
        }
        if (id == "interface.scale_percent") {
            return Text(anomaly::MessageId::SettingsValidationScale);
        }
        if (id == "interface.opacity_percent") {
            return Text(anomaly::MessageId::SettingsValidationOpacity);
        }
        if (id == "input.menu_toggle") {
            return Text(anomaly::MessageId::SettingsValidationMenuToggle);
        }
        if (id == "diagnostics.ring_capacity") {
            return Text(anomaly::MessageId::SettingsValidationRingCapacity);
        }
        return found->message.c_str();
    }

    template <typename DrawControl>
    bool DrawSettingRow(
        const char* id,
        const char* label,
        const char* consequence,
        const char* keywords,
        DrawControl&& draw_control,
        const char* badge = nullptr) {
        if (!SettingMatches(label, consequence, keywords)) return false;
        if (!settings_section_heading_drawn_) {
            ImGui::SetWindowFontScale(1.15f);
            ImGui::TextUnformatted(SettingsSectionName(settings_drawing_section_));
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
            settings_section_heading_drawn_ = true;
        }
        ImGui::PushID(id);
        const bool compact = IsCompact();
        if (ImGui::BeginTable("##setting-row", compact ? 1 : 2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("##setting", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            if (!compact) {
                ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthFixed, 250.0f);
            }
            ImGui::TableNextRow(ImGuiTableRowFlags_None, compact ? 78.0f : 58.0f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            if (badge != nullptr) {
                ImGui::SameLine();
                ImGui::TextColored(InfoColor(), "%s", badge);
            }
            ImGui::TextDisabled("%s", consequence);
            ImGui::TableNextColumn();
            SetAvailableItemWidth(0.0f, compact
                ? (std::numeric_limits<float>::max)()
                : 238.0f);
            draw_control();
            if (const char* error = SettingError(id)) {
                ImGui::TextColored(ErrorColor(), "%s", error);
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
        return true;
    }

    std::string VirtualKeyName(const std::uint32_t key) const {
        const auto fallback = [&] {
            const std::string key_text = std::to_string(key);
            const std::array<std::string_view, 1> arguments{key_text};
            return Format(anomaly::MessageId::SettingsVirtualKey, arguments);
        };
        const UINT scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        wchar_t buffer[64]{};
        const LONG parameter = static_cast<LONG>(scan_code << 16U);
        if (GetKeyNameTextW(parameter, buffer, static_cast<int>(std::size(buffer))) <= 0) {
            return fallback();
        }
        const int size = WideCharToMultiByte(
            CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) return fallback();
        std::string result(static_cast<std::size_t>(size), '\0');
        static_cast<void>(WideCharToMultiByte(
            CP_UTF8, 0, buffer, -1, result.data(), size, nullptr, nullptr));
        result.pop_back();
        return result;
    }

    [[nodiscard]] const char* LanguagePreferenceLabel(
        const anomaly::LanguagePreference value) const noexcept {
        switch (value) {
        case anomaly::LanguagePreference::Auto:
            return Text(anomaly::MessageId::SettingsAutomatic);
        case anomaly::LanguagePreference::EnUs:
            return Text(anomaly::MessageId::SettingsEnglish);
        case anomaly::LanguagePreference::ZhCn:
            return Text(anomaly::MessageId::SettingsSimplifiedChinese);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    [[nodiscard]] const char* MinimumLogLevelLabel(
        const anomaly::PlatformMinimumLogLevel value) const noexcept {
        switch (value) {
        case anomaly::PlatformMinimumLogLevel::Trace:
            return Text(anomaly::MessageId::SettingsTrace);
        case anomaly::PlatformMinimumLogLevel::Debug:
            return Text(anomaly::MessageId::SettingsDebug);
        case anomaly::PlatformMinimumLogLevel::Info:
            return Text(anomaly::MessageId::SettingsInfo);
        case anomaly::PlatformMinimumLogLevel::Warning:
            return Text(anomaly::MessageId::SettingsWarning);
        case anomaly::PlatformMinimumLogLevel::Error:
            return Text(anomaly::MessageId::SettingsError);
        }
        return Text(anomaly::MessageId::CommonUnknown);
    }

    // Snapshot the real-time down state of every key so CaptureSettingsHotkey can
    // detect a fresh press by its rising edge. The async-input reconciler polls
    // GetAsyncKeyState every frame, which clears the "pressed since last call"
    // low-order bit, so that bit cannot be used to detect key presses here.
    void BeginSettingsHotkeyCapture() {
        settings_hotkey_capture_ = true;
        for (std::uint32_t key = 0; key <= 0xff; ++key) {
            settings_hotkey_down_[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        }
    }

    void CaptureSettingsHotkey() {
        if (!settings_hotkey_capture_ || !settings_draft_) return;
        const auto is_down = [](std::uint32_t key) {
            return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        };
        for (std::uint32_t key = 8; key <= 0xff; ++key) {
            if (key >= VK_LBUTTON && key <= VK_XBUTTON2) continue;
            const bool down = is_down(key);
            const bool pressed = down && !settings_hotkey_down_[key];
            settings_hotkey_down_[key] = down;
            if (!pressed) continue;
            if (key == VK_ESCAPE) {
                settings_hotkey_capture_ = false;
                return;
            }
            settings_draft_->input_menu_toggle = key;
            settings_hotkey_capture_ = false;
            return;
        }
    }

    // --- Third-party plugin source editor --------------------------------------

    static std::string TrimAscii(std::string_view value) {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return std::string(value.substr(begin, end - begin + 1));
    }

    static void CopyToBuffer(std::array<char, 1024>& buffer, std::string_view value) {
        const std::size_t count = (std::min)(value.size(), buffer.size() - 1);
        value.copy(buffer.data(), count);
        buffer[count] = '\0';
    }

    [[nodiscard]] anomaly::PluginRepositoryConfig BuildRepositoryConfigFromEditor() const {
        anomaly::PluginRepositoryConfig config;
        config.enabled = repo_editor_master_enabled_;
        config.allow_insecure_sources = repo_editor_allow_insecure_;
        for (const auto& row : repo_editor_rows_) {
            std::string url = TrimAscii(row.url.data());
            if (url.empty()) continue;
            config.repositories.push_back({std::move(url), row.enabled});
        }
        return config;
    }

    void LoadRepositoryEditor(const anomaly::PluginRepositoryConfig& config) {
        repo_editor_master_enabled_ = config.enabled;
        repo_editor_allow_insecure_ = config.allow_insecure_sources;
        repo_editor_rows_.clear();
        for (const auto& entry : config.repositories) {
            RepositoryChannelRow row;
            row.enabled = entry.enabled;
            CopyToBuffer(row.url, entry.url);
            repo_editor_rows_.push_back(row);
        }
        repo_editor_baseline_ = config;
        repo_editor_loaded_ = true;
    }

    void EnsureRepositoryEditorLoaded() {
        if (repo_editor_loaded_ || !diagnostics_.repository_config) return;
        LoadRepositoryEditor(diagnostics_.repository_config());
    }

    [[nodiscard]] bool RepositoryEditorDirty() const {
        return repo_editor_loaded_ &&
            BuildRepositoryConfigFromEditor() != repo_editor_baseline_;
    }

    void ApplyRepositoryEditor() {
        if (!diagnostics_.repository_configure) {
            repo_editor_status_ = Text(anomaly::MessageId::SettingsRepositoriesProviderUnavailable);
            repo_editor_status_failure_ = true;
            return;
        }
        const auto config = BuildRepositoryConfigFromEditor();
        for (const auto& entry : config.repositories) {
            if (entry.enabled && !anomaly::IsPluginRepositoryUriAllowed(
                    entry.url, config.allow_insecure_sources)) {
                repo_editor_status_ = Text(anomaly::MessageId::SettingsRepositoriesInvalidUrl);
                repo_editor_status_failure_ = true;
                return;
            }
        }
        {
            std::scoped_lock operation_lock(operation_mutex_);
            pending_repository_configure_ = config;
        }
        repo_editor_save_pending_ = true;
        repo_editor_status_.clear();
    }

    void DrawRepositoryChannelsEditor() {
        if (!diagnostics_.repository_config || !diagnostics_.repository_configure) {
            ImGui::TextDisabled("%s",
                Text(anomaly::MessageId::SettingsRepositoriesProviderUnavailable));
            return;
        }
        EnsureRepositoryEditorLoaded();

        ImGui::BeginDisabled(repo_editor_save_pending_);
        ImGui::Checkbox(StableLabel(anomaly::MessageId::SettingsRepositoriesEnable,
            "repo-master-enable").c_str(), &repo_editor_master_enabled_);
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::SettingsRepositoriesEnableHint));
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(WarningColor(), "%s",
            Text(anomaly::MessageId::SettingsRepositoriesWarning));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // Live channel status from the coordinator snapshot.
        const auto& repository = model_.Snapshot().repository;
        const std::string online = std::to_string(repository.online_sources);
        const std::string cached = std::to_string(repository.cached_sources);
        const std::array<std::string_view, 2> source_arguments{online, cached};
        ImGui::TextDisabled("%s", DisplayRepositoryState(repository.state));
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("%s",
            Format(anomaly::MessageId::SettingsRepositoriesSources, source_arguments).c_str());
        ImGui::Separator();
        ImGui::Spacing();

        // One editable row per channel.
        int remove_index = -1;
        for (std::size_t index = 0; index < repo_editor_rows_.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            auto& row = repo_editor_rows_[index];
            ImGui::Checkbox("##channel-enabled", &row.enabled);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Text(anomaly::MessageId::SettingsRepositoriesToggleHint));
            }
            ImGui::SameLine();
            const float remove_width = 34.0f;
            ImGui::SetNextItemWidth(
                AvailableItemWidth(remove_width + ImGui::GetStyle().ItemSpacing.x));
            ImGui::InputTextWithHint("##channel-url",
                Text(anomaly::MessageId::SettingsRepositoriesUrlHint),
                row.url.data(), row.url.size());
            ImGui::SameLine();
            if (DrawShellIconButton("remove-channel", ShellGlyph::Close,
                    Text(anomaly::MessageId::SettingsRepositoriesRemove))) {
                remove_index = static_cast<int>(index);
            }
            ImGui::PopID();
        }
        if (remove_index >= 0) {
            repo_editor_rows_.erase(repo_editor_rows_.begin() + remove_index);
        }
        if (repo_editor_rows_.empty()) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::SettingsRepositoriesEmpty));
        }

        ImGui::Spacing();
        const std::string add_label = anomaly::StableDisplayLabel(
            "+  " + std::string(Text(anomaly::MessageId::SettingsRepositoriesAdd)),
            "repo-add-channel");
        if (ImGui::Button(add_label.c_str())) {
            repo_editor_rows_.emplace_back();
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        const bool dirty = RepositoryEditorDirty();
        ImGui::BeginDisabled(!dirty || repo_editor_save_pending_);
        if (PrimaryButton(StableLabel(anomaly::MessageId::SettingsRepositoriesApply,
                "repo-apply").c_str(), ImVec2(0.0f, 32.0f))) {
            ApplyRepositoryEditor();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty || repo_editor_save_pending_);
        if (ImGui::Button(StableLabel(anomaly::MessageId::SettingsRepositoriesReset,
                "repo-reset").c_str(), ImVec2(0.0f, 32.0f))) {
            LoadRepositoryEditor(repo_editor_baseline_);
            repo_editor_status_.clear();
        }
        ImGui::EndDisabled();
        if (!repo_editor_status_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(repo_editor_status_failure_ ? ErrorColor() : InfoColor(),
                "%s", repo_editor_status_.c_str());
        }
    }

    bool DrawSettingsSection(const SettingsSection section) {
        if (!settings_draft_) return false;
        auto& values = *settings_draft_;
        bool visible{};
        settings_drawing_section_ = section;
        settings_section_heading_drawn_ = false;
        const auto heading = [&] {
            if (!settings_section_heading_drawn_) {
                ImGui::SetWindowFontScale(1.15f);
                ImGui::TextUnformatted(SettingsSectionName(section));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::Separator();
                settings_section_heading_drawn_ = true;
            }
            visible = true;
        };
        const auto row = [&](auto&&... arguments) {
            visible = DrawSettingRow(std::forward<decltype(arguments)>(arguments)...) || visible;
        };

        switch (section) {
        case SettingsSection::Interface:
            row("interface.language", Text(anomaly::MessageId::SettingsLanguage),
                Text(anomaly::MessageId::SettingsLanguageHint), "locale language", [&] {
                    if (ImGui::BeginCombo(
                            "##value", LanguagePreferenceLabel(values.interface_language))) {
                        constexpr std::array<anomaly::LanguagePreference, 3> options{
                            anomaly::LanguagePreference::Auto,
                            anomaly::LanguagePreference::EnUs,
                            anomaly::LanguagePreference::ZhCn};
                        constexpr std::array<std::string_view, 3> ids{
                            "language-auto", "language-en-us", "language-zh-cn"};
                        for (std::size_t index = 0; index < options.size(); ++index) {
                            const std::string option = anomaly::StableDisplayLabel(
                                LanguagePreferenceLabel(options[index]), ids[index]);
                            if (ImGui::Selectable(option.c_str(),
                                    values.interface_language == options[index])) {
                                values.interface_language = options[index];
                            }
                        }
                        ImGui::EndCombo();
                    }
                }, Text(anomaly::MessageId::SettingsRestartRequired));
            row("interface.scale_percent", Text(anomaly::MessageId::SettingsInterfaceScale),
                Text(anomaly::MessageId::SettingsInterfaceScaleHint),
                "dpi font size zoom", [&] {
                    int value = static_cast<int>(values.interface_scale_percent);
                    if (ImGui::SliderInt("##value", &value, 75, 200, "%d%%")) {
                        value = ((value + 2) / 5) * 5;
                        values.interface_scale_percent = static_cast<std::uint32_t>(value);
                    }
                }, Text(anomaly::MessageId::SettingsUiRebuild));
            row("interface.opacity_percent", Text(anomaly::MessageId::SettingsWindowOpacity),
                Text(anomaly::MessageId::SettingsWindowOpacityHint),
                "alpha transparency", [&] {
                    int value = static_cast<int>(values.interface_opacity_percent);
                    if (ImGui::SliderInt("##value", &value, 10, 100, "%d%%")) {
                        value = ((value + 2) / 5) * 5;
                        values.interface_opacity_percent = static_cast<std::uint32_t>(value);
                    }
                });
            row("interface.reduced_motion", Text(anomaly::MessageId::SettingsReduceMotion),
                Text(anomaly::MessageId::SettingsReduceMotionHint),
                "animation accessibility", [&] {
                    ImGui::Checkbox("##value", &values.interface_reduced_motion);
                });
            row("interface.remember_last_route", Text(anomaly::MessageId::SettingsRememberLastPage),
                Text(anomaly::MessageId::SettingsRememberLastPageHint),
                "startup route page", [&] {
                    ImGui::Checkbox("##value", &values.interface_remember_last_route);
                });
            break;
        case SettingsSection::Input:
            row("input.menu_toggle", Text(anomaly::MessageId::SettingsMenuToggle),
                Text(anomaly::MessageId::SettingsMenuToggleHint),
                "hotkey keyboard insert", [&] {
                    CaptureSettingsHotkey();
                    const std::string label = settings_hotkey_capture_
                        ? Text(anomaly::MessageId::SettingsPressKey)
                        : VirtualKeyName(values.input_menu_toggle);
                    if (ImGui::Button(label.c_str(), FillAvailableSize(0.0f))) {
                        BeginSettingsHotkeyCapture();
                    }
                });
            row("input.gamepad_navigation", Text(anomaly::MessageId::SettingsGamepadNavigation),
                Text(anomaly::MessageId::SettingsGamepadNavigationHint),
                "controller", [&] { ImGui::Checkbox("##value", &values.input_gamepad_navigation); });
            break;
        case SettingsSection::Updates:
            row("updates.channel", Text(anomaly::MessageId::SettingsUpdateChannel),
                Text(anomaly::MessageId::SettingsUpdateChannelHint),
                "stable preview nightly", [&] {
                    const auto channel = values.updates_channel;
                    const std::string stable = StableLabel(
                        anomaly::MessageId::SettingsStable, "channel-stable");
                    if (ImGui::RadioButton(
                            stable.c_str(), channel == anomaly::PlatformUpdateChannel::Stable)) {
                        values.updates_channel = anomaly::PlatformUpdateChannel::Stable;
                    }
                    ImGui::SameLine();
                    const std::string preview = StableLabel(
                        anomaly::MessageId::SettingsPreview, "channel-preview");
                    if (ImGui::RadioButton(
                            preview.c_str(), channel == anomaly::PlatformUpdateChannel::Preview)) {
                        values.updates_channel = anomaly::PlatformUpdateChannel::Preview;
                    }
                    ImGui::SameLine();
                    const std::string nightly = StableLabel(
                        anomaly::MessageId::SettingsNightly, "channel-nightly");
                    if (ImGui::RadioButton(
                            nightly.c_str(), channel == anomaly::PlatformUpdateChannel::Nightly)) {
                        values.updates_channel = anomaly::PlatformUpdateChannel::Nightly;
                    }
                });
            row("updates.automatic_check", Text(anomaly::MessageId::SettingsAutomaticUpdates),
                Text(anomaly::MessageId::SettingsAutomaticUpdatesHint),
                "repository network", [&] { ImGui::Checkbox("##value", &values.updates_automatic_check); });
            row("updates.include_disabled", Text(anomaly::MessageId::SettingsIncludeDisabled),
                Text(anomaly::MessageId::SettingsIncludeDisabledHint),
                "repository profile", [&] { ImGui::Checkbox("##value", &values.updates_include_disabled); });
            break;
        case SettingsSection::Diagnostics:
            row("diagnostics.log_level", Text(anomaly::MessageId::SettingsMinimumLogLevel),
                Text(anomaly::MessageId::SettingsLogLevelHint),
                "trace debug info warning error", [&] {
                    if (ImGui::BeginCombo(
                            "##value", MinimumLogLevelLabel(values.diagnostics_log_level))) {
                        constexpr std::array<anomaly::PlatformMinimumLogLevel, 5> levels{
                            anomaly::PlatformMinimumLogLevel::Trace,
                            anomaly::PlatformMinimumLogLevel::Debug,
                            anomaly::PlatformMinimumLogLevel::Info,
                            anomaly::PlatformMinimumLogLevel::Warning,
                            anomaly::PlatformMinimumLogLevel::Error};
                        constexpr std::array<std::string_view, 5> ids{
                            "log-trace", "log-debug", "log-info", "log-warning", "log-error"};
                        for (std::size_t index = 0; index < levels.size(); ++index) {
                            const std::string label = anomaly::StableDisplayLabel(
                                MinimumLogLevelLabel(levels[index]), ids[index]);
                            if (ImGui::Selectable(label.c_str(),
                                    values.diagnostics_log_level == levels[index])) {
                                values.diagnostics_log_level = levels[index];
                            }
                        }
                        ImGui::EndCombo();
                    }
                });
            row("diagnostics.ring_capacity", Text(anomaly::MessageId::SettingsRingCapacity),
                Text(anomaly::MessageId::SettingsRingCapacityHint),
                "ring buffer capacity", [&] {
                    int value = static_cast<int>(values.diagnostics_ring_capacity);
                    if (ImGui::InputInt("##value", &value, 1000, 10000)) {
                        values.diagnostics_ring_capacity = static_cast<std::uint32_t>(
                            (std::max)(0, value));
                    }
                });
            break;
        case SettingsSection::Advanced:
            row("advanced.developer_mode", Text(anomaly::MessageId::SettingsDeveloperMode),
                Text(anomaly::MessageId::SettingsDeveloperModeHint),
                "services hooks memory nte", [&] {
                    ImGui::Checkbox("##value", &values.advanced_developer_mode);
                });
            break;
        case SettingsSection::About:
            if (SettingMatches(Text(anomaly::MessageId::SettingsRuntimeIdentity),
                    Text(anomaly::MessageId::SettingsBuildVersions),
                    "about version sdk profile github contact qq")) {
                heading();
                if (ImGui::BeginTable("##about", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp)) {
                    const auto fact = [this](const char* label, const std::string& value) {
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, 38.0f);
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", label);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(
                            value.empty() ? Text(anomaly::MessageId::CommonUnavailable) : value.c_str());
                    };
                    fact(Text(anomaly::MessageId::SettingsRuntime),
                        model_.Snapshot().diagnostics.runtime_version);
                    fact(Text(anomaly::MessageId::SettingsSdkApi),
                        "V" + std::to_string(ANOMALY_PLUGIN_API_V1_MAJOR));
                    fact(Text(anomaly::MessageId::SettingsRepository),
                        DisplayRepositoryState(model_.Snapshot().repository.state));
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 38.0f);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled(
                        "%s", Text(anomaly::MessageId::SettingsProjectRepository));
                    ImGui::TableNextColumn();
                    if (contact_ && ImGui::TextLink(contact_->repository_label.c_str())) {
                        pending_external_url_ = contact_->repository_url;
                    } else if (!contact_) {
                        ImGui::TextUnformatted(Text(anomaly::MessageId::CommonUnavailable));
                    }
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 38.0f);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", Text(anomaly::MessageId::SettingsQqGroup));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(contact_
                            ? contact_->qq_group.c_str()
                            : Text(anomaly::MessageId::CommonUnavailable));
                    ImGui::EndTable();
                }
            }
            break;
        }
        return visible;
    }

    void QueueSettingsSave(const std::optional<Route> route_after_save = std::nullopt) {
        if (!settings_draft_ || settings_save_pending_) return;
            settings_validation_errors_ = anomaly::ValidatePlatformSettings(*settings_draft_);
        if (!settings_validation_errors_.empty()) return;
        anomaly::PlatformSettingsApplyRequest request;
        request.expected_revision = settings_base_revision_;
        request.values = *settings_draft_;
        pending_settings_apply_ = request;
        settings_save_pending_ = true;
        settings_route_after_save_ = route_after_save;
    }

    void DiscardSettingsDraft() {
        if (!settings_snapshot_.ready) return;
        settings_draft_ = settings_snapshot_.values;
        settings_base_revision_ = settings_snapshot_.revision;
        settings_apply_error_.clear();
        settings_validation_errors_.clear();
        settings_hotkey_capture_ = false;
        ApplySettingsPreview();
    }

    void DrawSettingsLeavePopup() {
        const std::string popup_title = StableLabel(
            anomaly::MessageId::SettingsLeaveTitle, "settings-leave-popup");
        if (settings_leave_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            settings_leave_popup_requested_ = false;
        }
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        ImGui::TextUnformatted(Text(anomaly::MessageId::SettingsLeaveMessage));
        ImGui::Spacing();
        const std::string keep_editing = StableLabel(
            anomaly::MessageId::CommonKeepEditing, "settings-keep-editing");
        if (ImGui::Button(keep_editing.c_str())) {
            settings_pending_route_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const std::string discard = StableLabel(
            anomaly::MessageId::CommonDiscard, "settings-discard-leave");
        if (ImGui::Button(discard.c_str())) {
            const auto route = settings_pending_route_;
            settings_pending_route_.reset();
            DiscardSettingsDraft();
            ImGui::CloseCurrentPopup();
            if (route) Navigate(*route);
        }
        ImGui::SameLine();
        const auto errors = settings_draft_
            ? anomaly::ValidatePlatformSettings(*settings_draft_)
            : std::vector<anomaly::PlatformSettingsValidationError>{};
        ImGui::BeginDisabled(!settings_pending_route_ || !errors.empty() || settings_save_pending_);
        const std::string save = StableLabel(settings_save_pending_
                ? anomaly::MessageId::CommonSavingEllipsis
                : anomaly::MessageId::CommonSave,
            "settings-save-leave");
        if (PrimaryButton(save.c_str())) {
            QueueSettingsSave(settings_pending_route_);
            settings_pending_route_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    void DrawSettingsShell() {
        const std::string subtitle = settings_snapshot_.ready
            ? std::string{}
            : std::string(Text(anomaly::MessageId::SettingsProviderUnavailable));
        DrawShellPageHeader(Text(anomaly::MessageId::ShellRouteSettings), subtitle,
            [](const ImVec2&, const ImVec2&) {});
        if (!settings_snapshot_.ready || !settings_draft_) {
            BeginShellBodyChild("PlatformSettingsRoute");
            ImGui::SetCursorPosY(std::max(32.0f, ImGui::GetContentRegionAvail().y * 0.16f));
            ImGui::TextColored(WarningColor(), "!  %s",
                Text(anomaly::MessageId::SettingsUnavailable));
            ImGui::PushTextWrapPos(std::min(620.0f, ImGui::GetContentRegionAvail().x));
            ImGui::TextDisabled("%s", settings_snapshot_.reason.empty()
                ? Text(anomaly::MessageId::SettingsFacadeUnavailable)
                : settings_snapshot_.reason.c_str());
            ImGui::PopTextWrapPos();
            const std::string open_diagnostics = StableLabel(
                anomaly::MessageId::PluginsOpenDiagnostics, "settings-open-diagnostics");
            if (ImGui::Button(open_diagnostics.c_str())) {
                Navigate(Route::Diagnostics);
                model_.State().diagnostics_tab = DiagnosticTab::Overview;
            }
            ImGui::Separator();
            bool developer_mode = developer_mode_;
            const std::string developer_session = StableLabel(
                anomaly::MessageId::SettingsEnableDeveloperSession,
                "settings-developer-session");
            if (ImGui::Checkbox(developer_session.c_str(), &developer_mode)) {
                SetDeveloperMode(developer_mode);
            }
            EndShellBodyChild();
            return;
        }

        settings_validation_errors_ = anomaly::ValidatePlatformSettings(*settings_draft_);
        ImGui::BeginChild("PlatformSettingsRoute", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::BeginChild("PlatformSettingsSearch", ImVec2(0.0f, 44.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(IsCompact() ? 10.0f : 16.0f, 7.0f));
        SetAvailableItemWidth(16.0f, 520.0f);
        ImGui::InputTextWithHint("##settings-search", Text(anomaly::MessageId::SettingsSearchHint),
            settings_search_.data(), settings_search_.size());
        ImGui::EndChild();

        const bool dirty = SettingsDirty();
        const float footer_height = dirty || settings_save_pending_ || !settings_apply_error_.empty()
            ? 54.0f : 0.0f;
        ImGui::BeginChild("PlatformSettingsWorkspace", ImVec2(0.0f, -footer_height), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        constexpr std::array<SettingsSection, 6> sections{
            SettingsSection::Interface, SettingsSection::Input, SettingsSection::Updates,
            SettingsSection::Diagnostics, SettingsSection::Advanced, SettingsSection::About};
        if (IsCompact()) {
            ImGui::SetCursorPos(ImVec2(10.0f, 8.0f));
            SetAvailableItemWidth(10.0f);
            if (ImGui::BeginCombo("##settings-section", SettingsSectionName(settings_section_))) {
                for (const auto section : sections) {
                    const std::string label = anomaly::StableDisplayLabel(
                        SettingsSectionName(section), std::to_string(static_cast<int>(section)));
                    if (ImGui::Selectable(label.c_str(), settings_section_ == section)) {
                        settings_section_ = section;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::BeginChild("PlatformSettingsContent", ImVec2(0.0f, 0.0f), false,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
        } else {
            const float rail_width = layout_mode_ == LayoutMode::Wide ? 200.0f : 180.0f;
            ImGui::BeginChild("PlatformSettingsSections", ImVec2(rail_width, 0.0f), true);
            for (const auto section : sections) {
                const std::string label = anomaly::StableDisplayLabel(
                    SettingsSectionName(section), std::to_string(static_cast<int>(section)));
                if (ImGui::Selectable(label.c_str(), settings_section_ == section,
                        0, FillAvailableSize(34.0f))) {
                    settings_section_ = section;
                }
            }
            ImGui::EndChild();
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::BeginChild("PlatformSettingsContent", ImVec2(0.0f, 0.0f), true,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        bool any_visible{};
        if (settings_search_[0] == '\0') {
            any_visible = DrawSettingsSection(settings_section_);
        } else {
            for (const auto section : sections) {
                const bool section_visible = DrawSettingsSection(section);
                if (section_visible) ImGui::Spacing();
                any_visible = any_visible || section_visible;
            }
        }
        if (!any_visible) {
            ImGui::SetCursorPosY(28.0f);
            const std::array<std::string_view, 1> search_arguments{
                std::string_view(settings_search_.data())};
            const std::string no_match = Format(
                anomaly::MessageId::SettingsNoSearchMatch, search_arguments);
            ImGui::TextDisabled("%s", no_match.c_str());
            const std::string clear_search = StableLabel(
                anomaly::MessageId::CommonClearSearch, "settings-clear-search");
            if (ImGui::Button(clear_search.c_str())) settings_search_.fill('\0');
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::EndChild();

        if (footer_height > 0.0f) {
            ImGui::BeginChild("PlatformSettingsDraftBar", ImVec2(0.0f, footer_height), true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::SetCursorPos(ImVec2(12.0f, 17.0f));
            if (!settings_apply_error_.empty()) {
                ImGui::TextColored(ErrorColor(), "%s", settings_apply_error_.c_str());
            } else if (!settings_validation_errors_.empty()) {
                const std::string error_count =
                    std::to_string(settings_validation_errors_.size());
                const std::array<std::string_view, 1> error_arguments{error_count};
                const std::string error_text = Format(
                    anomaly::MessageId::SettingsValidationErrors, error_arguments);
                ImGui::TextColored(ErrorColor(), "%s", error_text.c_str());
            } else {
                ImGui::TextDisabled("%s", Text(settings_save_pending_
                    ? anomaly::MessageId::SettingsSavingChanges
                    : anomaly::MessageId::SettingsUnsavedChanges));
            }
            const float save_width = 76.0f;
            const float discard_width = 82.0f;
            ImGui::SetCursorPos(ImVec2((std::max)(12.0f,
                ImGui::GetWindowSize().x - save_width - discard_width - 28.0f), 10.0f));
            ImGui::BeginDisabled(settings_save_pending_);
            const std::string discard = StableLabel(
                anomaly::MessageId::CommonDiscard, "settings-discard");
            if (ImGui::Button(discard.c_str(), ImVec2(discard_width, 32.0f))) {
                DiscardSettingsDraft();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool stale = settings_base_revision_ != settings_snapshot_.revision;
            ImGui::BeginDisabled(settings_save_pending_ || !settings_validation_errors_.empty() || stale);
            const std::string save = StableLabel(settings_save_pending_
                    ? anomaly::MessageId::CommonSaving
                    : anomaly::MessageId::CommonSave,
                "settings-save");
            if (PrimaryButton(save.c_str(),
                    ImVec2(save_width, 32.0f))) {
                QueueSettingsSave();
            }
            ImGui::EndDisabled();
            ImGui::EndChild();
        }
        ImGui::EndChild();
    }

    void UpdateStatusToast() {
        if (status_.empty() || status_ == last_toast_status_) return;
        last_toast_status_ = status_;
        toast_status_ = status_;
        toast_failure_ = status_failure_;
        toast_expires_at_ = ImGui::GetTime() + 3.2;
    }

    void DrawStatusToast() {
        if (toast_status_.empty() || ImGui::GetTime() >= toast_expires_at_) {
            toast_status_.clear();
            return;
        }
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float text_width = ImGui::CalcTextSize(toast_status_.c_str()).x;
        const float width = std::min(std::max(220.0f, text_width + 58.0f),
            std::max(220.0f, size.x - 28.0f));
        const ImVec2 position = Offset(origin, size.x - width - kPlatformToastBottomMargin,
            size.y - kPlatformToastHeight - kPlatformToastBottomMargin);
        const ImVec4 color = toast_failure_ ? ErrorColor() : SuccessColor();
        ImDrawList* const draw_list = ImGui::GetForegroundDrawList();
        draw_list->AddRectFilled(position, Offset(position, width, kPlatformToastHeight),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.118f, 0.141f, 0.165f, 0.98f)), 4.0f);
        draw_list->AddRect(position, Offset(position, width, kPlatformToastHeight),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.42f)), 4.0f);
        draw_list->AddText(Offset(position, 12.0f, 11.0f),
            ImGui::ColorConvertFloat4ToU32(color),
            ShellGlyphText(toast_failure_ ? ShellGlyph::Warning : ShellGlyph::Check));
        draw_list->AddText(Offset(position, 34.0f, 11.0f),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.949f, 0.957f, 0.965f, 1.0f)),
            Ellipsize(toast_status_, width - 46.0f).c_str());
    }

    void RequestOperationDetailsPopup() noexcept {
        operation_details_open_ = true;
        operation_details_popup_requested_ = true;
    }

    void DrawOperationDetailsPopup() {
        if (!operation_details_open_) return;
        const std::string popup_title = StableLabel(
            anomaly::MessageId::OperationDetails, "operation-details-popup");
        if (operation_details_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            operation_details_popup_requested_ = false;
        }
        const ImVec2 host_size = ImGui::GetWindowSize();
        const float modal_width = std::min(640.0f, std::max(320.0f, host_size.x - 48.0f));
        const float modal_height = std::min(520.0f, std::max(220.0f, host_size.y - 48.0f));
        ImGui::SetNextWindowSize(ImVec2(modal_width, modal_height), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(popup_title.c_str(), &operation_details_open_,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            operation_details_open_ = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const auto* operation = PresentedOperation();
        ImGui::SetWindowFontScale(1.12f);
        ImGui::TextUnformatted(Text(anomaly::MessageId::OperationDetails));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", operation == nullptr
            ? Text(anomaly::MessageId::OperationNoneSelected)
            : OperationDisplayLabel(*operation).c_str());
        if (operation != nullptr && !operation->message.empty()) {
            ImGui::TextWrapped("%s", operation->message.c_str());
        }
        ImGui::Separator();
        if (operation != nullptr && ImGui::BeginTable("OperationDetails", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, std::max(80.0f,
                    std::min(250.0f, ImGui::GetContentRegionAvail().y - 48.0f))))) {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonPlugin),
                ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonResult),
                ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonReason),
                ImGuiTableColumnFlags_WidthStretch, 0.62f);
            ImGui::TableHeadersRow();
            for (const auto& plugin : operation->plugins) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(plugin.plugin_id.c_str());
                ImGui::TableNextColumn();
                const bool failure = plugin.outcome == anomaly::PluginOperationOutcome::Failed;
                ImGui::TextColored(failure ? ErrorColor() : SuccessColor(), "%s",
                    Text(plugin.outcome == anomaly::PluginOperationOutcome::Succeeded
                        ? anomaly::MessageId::CommonSucceeded
                        : failure ? anomaly::MessageId::CommonFailed
                                  : anomaly::MessageId::CommonSkipped));
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", plugin.message.empty()
                    ? anomaly::ToString(plugin.reason).data() : plugin.message.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        const std::string close = StableLabel(
            anomaly::MessageId::CommonClose, "operation-details-close");
        const ImVec2 close_size = ImGui::CalcTextSize(close.c_str());
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
            ImGui::GetWindowContentRegionMax().x - close_size.x - ImGui::GetStyle().FramePadding.x * 2.0f));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(close.c_str())) {
            operation_details_open_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void DrawServices() {
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperServiceGraph));
        if (!diagnostics_.service_graph) {
            ImGui::TextDisabled("%s",
                Text(anomaly::MessageId::DeveloperServiceProviderUnavailable));
            return;
        }
        const auto graph = diagnostics_.service_graph();
        const std::array<std::string, 4> summary_values{
            graph.built ? "1" : "0",
            graph.blocking_startup_complete ? "1" : "0",
            graph.async_startup_complete ? "1" : "0",
            std::to_string(graph.error)};
        const std::array<std::string_view, 4> summary_arguments{
            summary_values[0], summary_values[1], summary_values[2], summary_values[3]};
        ImGui::TextDisabled("%s", Format(
            anomaly::MessageId::DeveloperServiceSummary, summary_arguments).c_str());
        if (ImGui::BeginTable("DeveloperServices", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonService));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonState));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperServiceAffinity));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonVersion));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperServiceStartMs));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperServiceStartThread));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::DeveloperServiceStopThread));
            ImGui::TableHeadersRow();
            for (const auto& service : graph.services) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(service.id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ServiceStateName(service.state));
                ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<unsigned>(service.affinity));
                ImGui::TableNextColumn(); ImGui::Text("%u", service.version);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", service.startup_duration.count() / 1000.0);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(service.start_thread_id));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(service.stop_thread_id));
            }
            ImGui::EndTable();
        }
    }

    void DrawHooks() {
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperHooksTitle));
        const auto hooks = diagnostics_.hooks ? diagnostics_.hooks() : std::vector<anomaly::HookRecordView>{};
        if (hooks.empty()) {
            ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperHooksNone));
            return;
        }
        if (ImGui::BeginTable("DeveloperHooks", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonOwner));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonLabel));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonGeneration));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonTarget));
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonState));
            ImGui::TableHeadersRow();
            for (const auto& hook : hooks) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(hook.owner.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(hook.label.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(hook.generation));
                ImGui::TableNextColumn(); ImGui::Text("0x%llX", reinterpret_cast<unsigned long long>(hook.target));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Text(hook.enabled
                    ? anomaly::MessageId::CommonEnabled
                    : anomaly::MessageId::CommonDisabled));
            }
            ImGui::EndTable();
        }
    }

    void DrawMemory() {
        ImGui::TextUnformatted(Text(anomaly::MessageId::DeveloperMemoryTitle));
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperMemoryWarning));
        ImGui::Separator();
        ImGui::SetNextItemWidth(230.0f);
        ImGui::InputTextWithHint("##memory-address",
            Text(anomaly::MessageId::DeveloperMemoryAddressHint),
            address_.data(), address_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const std::string bytes_label = StableLabel(
            anomaly::MessageId::CommonBytes, "memory-read-bytes");
        ImGui::InputInt(bytes_label.c_str(), &read_size_, 16, 64);
        read_size_ = std::clamp(read_size_, 1, 4096);
        ImGui::SameLine();
        const std::string read = StableLabel(
            anomaly::MessageId::CommonRead, "memory-read");
        if (ImGui::Button(read.c_str())) read_requested_ = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", memory_status_.c_str());
        ImGui::BeginChild("MemoryHex", ImVec2(0, 220), ImGuiChildFlags_Borders);
        for (std::size_t row = 0; row < bytes_.size(); row += 16) {
            ImGui::TextDisabled("%08llX", static_cast<unsigned long long>(last_address_ + row));
            ImGui::SameLine(100.0f);
            std::string hex;
            std::string ascii;
            for (std::size_t column = 0; column < 16; ++column) {
                if (row + column < bytes_.size()) {
                    char byte[4]{};
                    std::snprintf(byte, sizeof(byte), "%02X ", bytes_[row + column]);
                    hex += byte;
                    const auto value = bytes_[row + column];
                    ascii += value >= 32 && value < 127 ? static_cast<char>(value) : '.';
                } else {
                    hex += "   ";
                }
            }
            ImGui::TextUnformatted(hex.c_str());
            ImGui::SameLine(520.0f);
            ImGui::TextDisabled("%s", ascii.c_str());
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperMemoryWrite));
        SetAvailableItemWidth(220.0f);
        ImGui::InputTextWithHint("##memory-write",
            Text(anomaly::MessageId::DeveloperMemoryHexHint),
            write_bytes_.data(), write_bytes_.size());
        ImGui::SameLine();
        const std::string patch_label = StableLabel(
            anomaly::MessageId::DeveloperMemoryPatchProtection, "memory-patch-protection");
        ImGui::Checkbox(patch_label.c_str(), &patch_);
        ImGui::SameLine();
        const std::string apply = StableLabel(
            anomaly::MessageId::CommonApply, "memory-apply");
        if (ImGui::Button(apply.c_str())) RequestMemoryWrite();
    }

    void SetDeveloperMode(const bool enabled) noexcept {
        if (developer_mode_ == enabled) return;
        developer_mode_ = enabled;
        anomaly::SetHostUiDeveloperMode(enabled);
        anomaly::ReconcileUiStateSelection(
            model_.Snapshot(), model_.State(), developer_mode_);
    }

    bool HasPendingMutation(std::string_view subject, Mutation mutation) const {
        for (const auto& operation : model_.PendingOperations()) {
            if (!subject.empty() && operation.subject_id != subject &&
                std::none_of(operation.plugins.begin(), operation.plugins.end(),
                    [&](const auto& plugin) { return plugin.plugin_id == subject; })) {
                continue;
            }
            if (mutation != Mutation::None && operation.mutation != mutation) continue;
            if (subject.empty() && mutation == Mutation::None) continue;
            return true;
        }
        return false;
    }

    void Navigate(Route route) {
        if (model_.State().route == Route::Settings && route != Route::Settings &&
            SettingsDirty()) {
            settings_pending_route_ = route;
            settings_leave_popup_requested_ = true;
            return;
        }
        if (route == Route::NteCompatibility) {
            route = Route::Diagnostics;
            if (developer_mode_) {
                model_.State().diagnostics_tab = DiagnosticTab::Developer;
                developer_panel_ = DeveloperPanel::NteProfile;
            } else {
                model_.State().diagnostics_tab = DiagnosticTab::Overview;
            }
        }
        model_.State().route = route;
        if (diagnostics_.settings_record_route) {
            settings_route_to_record_ = std::string(anomaly::ToString(route));
        }
        if (route != Route::Plugins) compact_plugin_detail_pending_id_.clear();
        status_.clear();
        status_failure_ = false;
    }

    void RedirectHiddenNteCompatibilityRoute() {
        if (model_.State().route != Route::NteCompatibility) return;
        Navigate(Route::NteCompatibility);
    }

    void OpenPluginLogs(std::string_view plugin_id) {
        model_.State().route = Route::Diagnostics;
        model_.State().diagnostics_tab = DiagnosticTab::Logs;
        model_.State().diagnostics_plugin_id = std::string(plugin_id);
        status_ = Text(anomaly::MessageId::OperationLogsFiltered);
        status_failure_ = false;
    }

    void SubmitIntent(Intent intent, bool confirmed_submission = false) {
        const auto submission = model_.Submit(intent);
        if (!submission.accepted) {
            if (submission.code == anomaly::PlatformUiResultCode::PreflightRequired &&
                submission.preflight && !confirmed_submission) {
                confirmation_intent_ = std::move(intent);
                confirmation_plan_ = *submission.preflight;
                confirmation_popup_requested_ = true;
                return;
            }
            status_ = submission.reason.empty()
                ? std::string(anomaly::ToString(submission.code)) : submission.reason;
            status_failure_ = true;
            return;
        }
        status_ = Text(anomaly::MessageId::OperationSubmitted);
        status_failure_ = false;
        QueuedIntent queued{std::move(intent), submission.operation_id};
        if (submission.preflight) {
            queued.affected = submission.preflight->affected;
        }
        if (queued.affected.empty() && !queued.intent.subject_id.empty()) {
            queued.affected.push_back({queued.intent.subject_id, true, "requested object"});
        }
        for (const auto& affected : queued.affected) {
            const auto* plugin = model_.Snapshot().FindPlugin(affected.id);
            queued.generations.emplace(
                affected.id, plugin == nullptr ? 0 : plugin->generation);
        }
        queued.execution_state = std::make_shared<std::atomic<IntentExecutionState>>(
            IntentExecutionState::Pending);
        queued_intents_.push_back(std::move(queued));
    }

    void DismissConfirmation(const char* status) {
        confirmation_intent_.reset();
        confirmation_plan_ = {};
        confirmation_popup_requested_ = false;
        if (status != nullptr) {
            status_ = status;
            status_failure_ = false;
        }
    }

    void DrawConfirmationPopup() {
        const std::string popup_title = StableLabel(
            anomaly::MessageId::OperationConfirmTitle, "confirm-plugin-operation");
        if (confirmation_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            confirmation_popup_requested_ = false;
        }
        if (!confirmation_intent_) return;
        const ImVec2 host_size = ImGui::GetWindowSize();
        const float modal_width = std::min(640.0f, std::max(320.0f, host_size.x - 48.0f));
        const float table_height = std::clamp(
            34.0f + static_cast<float>(confirmation_plan_.affected.size()) * 31.0f,
            66.0f, 252.0f);
        const float requested_height = std::min(
            520.0f, std::max(210.0f, 126.0f + table_height));
        const float modal_height = std::min(
            requested_height, std::max(220.0f, host_size.y - 48.0f));
        ImGui::SetNextWindowSize(ImVec2(modal_width, modal_height), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;

        const auto action_verb = [this](const Mutation mutation) -> const char* {
            switch (mutation) {
            case Mutation::Start: return Text(anomaly::MessageId::CommonStart);
            case Mutation::Stop: return Text(anomaly::MessageId::CommonStop);
            case Mutation::Reload:
            case Mutation::ReloadAll: return Text(anomaly::MessageId::CommonReload);
            case Mutation::Enable: return Text(anomaly::MessageId::CommonEnable);
            case Mutation::Disable: return Text(anomaly::MessageId::CommonDisable);
            case Mutation::SetVisible: return Text(anomaly::MessageId::CommonUpdate);
            case Mutation::None: return Text(anomaly::MessageId::CommonConfirm);
            }
            return Text(anomaly::MessageId::CommonConfirm);
        };
        const Mutation mutation = confirmation_plan_.mutation;
        const std::size_t affected_count = confirmation_plan_.affected.size();
        std::string title{Text(anomaly::MessageId::OperationConfirmTitle)};
        std::string consequence;
        if (mutation == Mutation::ReloadAll) {
            title = Text(anomaly::MessageId::OperationReloadAllTitle);
            consequence = Text(anomaly::MessageId::OperationReloadAllConsequence);
        } else if (!confirmation_plan_.reason.empty()) {
            consequence = confirmation_plan_.reason;
        } else {
            consequence = Text(anomaly::MessageId::OperationStateConsequence);
        }
        const std::string affected_count_text = std::to_string(affected_count);
        const std::array<std::string_view, 2> action_arguments{
            action_verb(mutation), affected_count_text};
        const std::array<std::string_view, 1> single_action_argument{
            action_verb(mutation)};
        const std::string action_label = affected_count == 1
            ? Format(anomaly::MessageId::OperationAffectedOne, single_action_argument)
            : Format(anomaly::MessageId::OperationAffectedMany, action_arguments);
        const std::string action_button_label = action_label + "###confirm-operation-action";

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            DismissConfirmation(Text(anomaly::MessageId::OperationCancelled));
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::SetWindowFontScale(1.15f);
        ImGui::TextUnformatted(title.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", consequence.c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("ConfirmationImpact", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, table_height))) {
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonPlugin),
                ImGuiTableColumnFlags_WidthStretch, 0.36f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonScope),
                ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableSetupColumn(Text(anomaly::MessageId::CommonWhy),
                ImGuiTableColumnFlags_WidthStretch, 0.64f);
            ImGui::TableHeadersRow();
            for (const auto& plugin : confirmation_plan_.affected) {
                const char* scope = Text(plugin.directly_requested
                    ? anomaly::MessageId::CommonRequested
                    : anomaly::MessageId::CommonDependency);
                const char* fallback_reason = Text(plugin.directly_requested
                    ? anomaly::MessageId::OperationSelectedByAction
                    : anomaly::MessageId::OperationRequiredByPlugin);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(plugin.id.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(scope);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", plugin.reason.empty()
                    ? fallback_reason : plugin.reason.c_str());
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        const std::string cancel_label = StableLabel(
            anomaly::MessageId::CommonCancel, "confirm-operation-cancel");
        const ImVec2 cancel_text_size = ImGui::CalcTextSize(cancel_label.c_str());
        const ImVec2 action_text_size = ImGui::CalcTextSize(action_button_label.c_str());
        const float cancel_width = cancel_text_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float action_width = action_text_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float footer_width = cancel_width + ImGui::GetStyle().ItemSpacing.x + action_width;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
            ImGui::GetWindowContentRegionMax().x - footer_width));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool cancel = ImGui::Button(cancel_label.c_str(), ImVec2(cancel_width, 0.0f));
        ImGui::SameLine();
        const bool confirm = PrimaryButton(
            action_button_label.c_str(), ImVec2(action_width, 0.0f));
        if (confirm) {
            Intent intent = std::move(*confirmation_intent_);
            intent.expected_revision = model_.Snapshot().revision;
            intent.confirmed = true;
            DismissConfirmation(nullptr);
            ImGui::CloseCurrentPopup();
            SubmitIntent(std::move(intent), true);
        }
        if (cancel) {
            DismissConfirmation(Text(anomaly::MessageId::OperationCancelled));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void DrawRepositoryUninstallPopup() {
        const std::string popup_title = StableLabel(
            anomaly::MessageId::PluginsUninstallTitle, "repository-uninstall-popup");
        if (repository_uninstall_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            repository_uninstall_popup_requested_ = false;
        }
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        if (repository_uninstall_plugin_id_.empty()) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            repository_uninstall_plugin_id_.clear();
            repository_uninstall_plugin_name_.clear();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const std::array<std::string_view, 1> arguments{repository_uninstall_plugin_name_};
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f);
        ImGui::TextWrapped("%s", Format(
            anomaly::MessageId::PluginsUninstallBody, arguments).c_str());
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::PluginsUninstallDataKept));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();

        const std::string cancel = StableLabel(
            anomaly::MessageId::CommonCancel, "repository-uninstall-cancel");
        const std::string uninstall = StableLabel(
            anomaly::MessageId::CommonUninstall, "repository-uninstall-confirm");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(cancel.c_str())) {
            repository_uninstall_plugin_id_.clear();
            repository_uninstall_plugin_name_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (DestructiveButton(uninstall.c_str())) {
            ImGui::CloseCurrentPopup();
            SubmitRepositoryUninstall();
        }
        ImGui::EndPopup();
    }

    void RequestMemoryWrite() {
        const auto address = ParseAddress(address_.data());
        const auto bytes = ParseBytes(write_bytes_.data());
        if (!address || !bytes) {
            memory_status_ = Text(anomaly::MessageId::DeveloperMemoryInvalidInput);
            return;
        }
        pending_memory_write_ = PendingMemoryWrite{*address, std::move(*bytes), patch_};
        memory_confirmation_popup_requested_ = true;
    }

    void DrawMemoryConfirmationPopup() {
        const std::string popup_title = StableLabel(
            anomaly::MessageId::DeveloperMemoryConfirmTitle, "confirm-memory-write");
        if (memory_confirmation_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            memory_confirmation_popup_requested_ = false;
        }
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) return;
        if (!pending_memory_write_) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const auto& write = *pending_memory_write_;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pending_memory_write_.reset();
            memory_status_ = Text(anomaly::MessageId::DeveloperMemoryWriteCancelled);
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const std::string byte_count = std::to_string(write.bytes.size());
        std::array<char, 32> address_text{};
        std::snprintf(address_text.data(), address_text.size(), "0x%llX",
            static_cast<unsigned long long>(write.address));
        const std::array<std::string_view, 2> write_arguments{
            byte_count, address_text.data()};
        ImGui::TextWrapped("%s", Format(
            anomaly::MessageId::DeveloperMemoryApplyBytes, write_arguments).c_str());
        const std::array<std::string_view, 1> mode_arguments{Text(write.patch
            ? anomaly::MessageId::DeveloperMemoryTrackedPatch
            : anomaly::MessageId::DeveloperMemoryRawWrite)};
        ImGui::Text("%s", Format(
            anomaly::MessageId::DeveloperMemoryMode, mode_arguments).c_str());
        ImGui::TextDisabled("%s", Text(anomaly::MessageId::DeveloperMemoryIrrecoverable));
        const std::string confirm_write = StableLabel(
            anomaly::MessageId::DeveloperMemoryConfirmWrite, "memory-confirm-write");
        if (ImGui::Button(confirm_write.c_str())) {
            memory_write_requested_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const std::string cancel_write = StableLabel(
            anomaly::MessageId::DeveloperMemoryCancelWrite, "memory-cancel-write");
        if (ImGui::Button(cancel_write.c_str())) {
            pending_memory_write_.reset();
            memory_status_ = Text(anomaly::MessageId::DeveloperMemoryWriteCancelled);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void FlushIntents() {
        std::vector<QueuedIntent> queued;
        {
            std::scoped_lock operation_lock(operation_mutex_);
            queued.swap(queued_intents_);
        }
        for (auto& item : queued) {
            auto pending = std::make_shared<QueuedIntent>(std::move(item));
            try {
                const auto invocation = BeginInvocation();
                if (invocation == nullptr) {
                    static_cast<void>(ApplyDispatcherFailure(*pending, ERROR_CANCELLED));
                    SetStatus(anomaly::MessageId::OperationLifecycleRejected, true);
                    continue;
                }
                const auto self = invocation->owner;
                invocation->on_abandon = [self, pending] {
                    static_cast<void>(self->ApplyIntentFailure(
                        *pending, IntentExecutionState::Pending,
                        anomaly::PlatformUiResultCode::ProviderUnavailable,
                        anomaly::PluginOperationReason::ProviderUnavailable,
                        "lifecycle dispatcher cancelled operation before callback start", true));
                };
                const auto submit = diagnostics_.lifecycle_post
                    ? diagnostics_.lifecycle_post
                    : diagnostics_.lifecycle_invoke;
                if (submit) {
                    const auto result = submit(
                        [self, pending, invocation]() mutable {
                            invocation->MarkCallbackStarted();
                            self->ExecuteIntentCallback(pending);
                        });
                    if (result != ERROR_SUCCESS) {
                        if (ApplyDispatcherFailure(*pending, result)) {
                            SetStatus(anomaly::MessageId::OperationLifecycleRejected, true);
                        } else {
                            SetStatus(anomaly::MessageId::OperationStillSettling, false);
                        }
                    }
                } else {
                    invocation->MarkCallbackStarted();
                    ExecuteIntentCallback(pending);
                }
            } catch (...) {
                if (ApplyIntentFailure(
                        *pending, IntentExecutionState::Pending,
                        anomaly::PlatformUiResultCode::BackendFailure,
                        anomaly::PluginOperationReason::BackendFailure,
                        "lifecycle invocation raised an exception", true)) {
                    SetStatus(anomaly::MessageId::OperationFailedDiagnostics, true);
                } else {
                    SetStatus(anomaly::MessageId::OperationStillSettling, false);
                }
            }
        }
    }

    bool ApplyIntentFailure(
        const QueuedIntent& queued,
        IntentExecutionState expected_state,
        anomaly::PlatformUiResultCode code,
        anomaly::PluginOperationReason reason,
        std::string message,
        bool retryable) {
        if (!TrySettleIntent(queued, expected_state)) return false;
        std::scoped_lock operation_lock(operation_mutex_);
        anomaly::PlatformUiOperationResult result;
        result.operation_id = queued.operation_id;
        result.intent_id = queued.intent.intent_id;
        result.expected_revision = queued.intent.expected_revision;
        result.observed_revision = model_.Snapshot().revision;
        result.mutation = anomaly::ResolvePlatformUiMutation(queued.intent);
        result.subject_id = queued.intent.subject_id;
        result.message = std::move(message);
        result.retryable = retryable;
        for (const auto& affected : queued.affected) {
            const auto generation = queued.generations.contains(affected.id)
                ? queued.generations.at(affected.id) : 0;
            result.plugins.push_back({affected.id,
                affected.directly_requested
                    ? anomaly::PluginOperationOutcome::Failed
                    : anomaly::PluginOperationOutcome::Skipped,
                reason, result.message, generation, generation,
                retryable});
        }
        if (result.plugins.empty()) {
            result.plugins.push_back({queued.intent.subject_id,
                anomaly::PluginOperationOutcome::Failed,
                reason, result.message, 0, 0, retryable});
        }
        result.state = anomaly::PlatformUiOperationState::Failed;
        result.code = code;
        model_.ApplyOperationResult(std::move(result));
        return true;
    }

    bool ApplyDispatcherFailure(
        const QueuedIntent& queued, std::uint32_t error) {
        const bool unavailable = error == ERROR_NOT_READY || error == ERROR_CANCELLED;
        return ApplyIntentFailure(
            queued, IntentExecutionState::Pending,
            unavailable ? anomaly::PlatformUiResultCode::ProviderUnavailable
                        : anomaly::PlatformUiResultCode::BackendFailure,
            unavailable ? anomaly::PluginOperationReason::ProviderUnavailable
                        : anomaly::PluginOperationReason::BackendFailure,
            "lifecycle dispatcher rejected operation: " + std::to_string(error), true);
    }

    void FlushSettingsActions() {
        std::optional<anomaly::PlatformSettingsApplyRequest> apply;
        std::optional<anomaly::PluginRepositoryConfig> repository_configure;
        std::optional<std::string> route;
        std::optional<std::string> external_url;
        {
            std::scoped_lock operation_lock(operation_mutex_);
            apply.swap(pending_settings_apply_);
            repository_configure.swap(pending_repository_configure_);
            route.swap(settings_route_to_record_);
            external_url.swap(pending_external_url_);
        }
        const auto submit = diagnostics_.lifecycle_post
            ? diagnostics_.lifecycle_post
            : diagnostics_.lifecycle_invoke;
        if (apply) {
            const auto mailbox = settings_apply_mailbox_;
            const auto provider = diagnostics_.settings_apply;
            const auto operation = [mailbox, provider, request = *apply] {
                anomaly::PlatformSettingsApplyResult result;
                if (provider) {
                    result = provider(request);
                } else {
                    result.code = anomaly::PlatformSettingsApplyCode::ProviderUnavailable;
                    result.message = "settings apply provider is unavailable";
                }
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = std::move(result);
            };
            std::uint32_t error = ERROR_SUCCESS;
            if (submit) error = submit(operation);
            else operation();
            if (error != ERROR_SUCCESS) {
                anomaly::PlatformSettingsApplyResult result;
                result.code = anomaly::PlatformSettingsApplyCode::ProviderUnavailable;
                result.message = "lifecycle dispatcher rejected settings save: " +
                    std::to_string(error);
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = std::move(result);
            }
        }
        if (repository_configure) {
            const auto mailbox = repository_configure_mailbox_;
            const auto provider = diagnostics_.repository_configure;
            const auto operation = [mailbox, provider, config = *repository_configure] {
                anomaly::RepositoryOperationSubmission submission;
                if (provider) {
                    submission = provider(config);
                } else {
                    submission.message = "repository configuration provider is unavailable";
                }
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = RepositoryConfigureResult{config, std::move(submission)};
            };
            std::uint32_t error = ERROR_SUCCESS;
            if (submit) error = submit(operation);
            else operation();
            if (error != ERROR_SUCCESS) {
                anomaly::RepositoryOperationSubmission submission;
                submission.message = "lifecycle dispatcher rejected repository configuration: " +
                    std::to_string(error);
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = RepositoryConfigureResult{
                    *repository_configure, std::move(submission)};
            }
        }
        if (route && diagnostics_.settings_record_route) {
            const auto provider = diagnostics_.settings_record_route;
            const auto operation = [provider, route = *route] {
                static_cast<void>(provider(route));
            };
            if (submit) static_cast<void>(submit(operation));
            else operation();
        }
        if (external_url) {
            const auto operation = [url = std::move(*external_url)] {
                static_cast<void>(OpenExternalUrl(url));
            };
            if (submit) static_cast<void>(submit(operation));
            else operation();
        }
    }

    void FlushActions() {
        FlushIntents();
        FlushSettingsActions();
        bool memory_pending{};
        {
            std::scoped_lock operation_lock(operation_mutex_);
            memory_pending = (read_requested_ || memory_write_requested_) &&
                !memory_invocation_pending_;
            if (memory_pending) memory_invocation_pending_ = true;
        }
        if (!memory_pending) return;
        try {
            const auto invocation = BeginInvocation();
            if (invocation == nullptr) {
                CancelMemoryWork();
                return;
            }
            const auto self = invocation->owner;
            invocation->on_abandon = [self] { self->CancelMemoryWork(); };
            const bool asynchronous = static_cast<bool>(diagnostics_.lifecycle_post);
            const auto submit = diagnostics_.lifecycle_post
                ? diagnostics_.lifecycle_post
                : diagnostics_.lifecycle_invoke;
            if (submit) {
                const auto result = submit(
                    [self, invocation] {
                        invocation->MarkCallbackStarted();
                        self->FlushMemoryCallback();
                    });
                if (result != ERROR_SUCCESS) {
                    if (asynchronous) CancelMemoryWork();
                    SetMemoryStatus(anomaly::MessageId::DeveloperMemoryLifecycleRejected);
                }
            } else {
                invocation->MarkCallbackStarted();
                FlushMemoryCallback();
            }
        } catch (...) {
            CancelMemoryWork();
            SetMemoryStatus(anomaly::MessageId::DeveloperMemoryOperationFailed);
        }
    }

    void SetStatus(const anomaly::MessageId message, const bool failure) {
        std::scoped_lock operation_lock(operation_mutex_);
        status_ = Text(message);
        status_failure_ = failure;
    }

    void SetMemoryStatus(const anomaly::MessageId message) {
        std::scoped_lock operation_lock(operation_mutex_);
        memory_status_ = Text(message);
    }

    void CancelMemoryWork() {
        std::scoped_lock operation_lock(operation_mutex_);
        read_requested_ = false;
        memory_write_requested_ = false;
        memory_invocation_pending_ = false;
        pending_memory_write_.reset();
        memory_status_ = Text(anomaly::MessageId::DeveloperMemoryOwnerClosed);
    }

    void ExecuteIntent(QueuedIntent queued) {
        if (!TryClaimIntent(queued)) return;
        try {
            const auto& intent = queued.intent;
            const auto observed_revision = model_.Snapshot().revision;
            if (intent.expected_revision != observed_revision) {
                ApplyIntentFailure(
                    queued, IntentExecutionState::Running,
                    anomaly::PlatformUiResultCode::RevisionConflict,
                    anomaly::PluginOperationReason::RevisionConflict,
                    "the queued intent was based on an older Installed snapshot", false);
                return;
            }

            if (intent.kind == anomaly::PlatformUiIntentKind::ReloadAllInstalled) {
                AwaitingBatch batch;
                batch.generations = queued.generations;
                if (batch.generations.empty()) {
                    for (const auto& plugin : model_.Snapshot().installed_plugins) {
                        batch.generations.emplace(plugin.id, plugin.generation);
                    }
                }
                // ReloadAll is synchronous at the PluginManager boundary, but
                // snapshot publication can lag one render frame. Keep the
                // operation Running until the resulting generations are stable.
                plugins_.ReloadAll();
                batch.queued = queued;
                batch.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                awaiting_batches_.push_back(std::move(batch));
                status_ = Text(anomaly::MessageId::OperationReloadAllSubmitted);
                status_failure_ = false;
                return;
            }

            const auto* target = intent.subject_id.empty()
                ? nullptr : model_.Snapshot().FindPlugin(intent.subject_id);
            bool succeeded = false;
            bool has_backend = true;
            switch (intent.kind) {
            case anomaly::PlatformUiIntentKind::SetPluginEnabled:
                succeeded = target != nullptr &&
                    plugins_.SetEnabled(intent.subject_id, intent.bool_value);
                break;
            case anomaly::PlatformUiIntentKind::ReloadPlugin:
                succeeded = target != nullptr && plugins_.Reload(intent.subject_id);
                break;
            case anomaly::PlatformUiIntentKind::SetPluginVisible:
                succeeded = target != nullptr &&
                    plugins_.SetVisible(intent.subject_id, intent.bool_value);
                break;
            default:
                has_backend = false;
                break;
            }
            if (!has_backend) {
                ApplyIntentFailure(
                    queued, IntentExecutionState::Running,
                    anomaly::PlatformUiResultCode::InvalidIntent,
                    anomaly::PluginOperationReason::NotSupported,
                    "intent has no lifecycle backend", false);
                return;
            }

            anomaly::PlatformUiOperationResult result;
            result.operation_id = queued.operation_id;
            result.intent_id = intent.intent_id;
            result.expected_revision = intent.expected_revision;
            result.observed_revision = model_.Snapshot().revision;
            result.mutation = anomaly::ResolvePlatformUiMutation(intent);
            result.subject_id = intent.subject_id;
            result.message = succeeded
                ? "backend accepted operation" : "backend rejected operation";

            const auto after = plugins_.Plugins();
            auto affected = queued.affected;
            if (affected.empty() && !intent.subject_id.empty()) {
                affected.push_back({intent.subject_id, true, "requested object"});
            }
            for (const auto& item : affected) {
                const auto before = queued.generations.contains(item.id)
                    ? queued.generations.at(item.id) : 0;
                const auto found = std::find_if(after.begin(), after.end(), [&](const auto& plugin) {
                    return plugin.id == item.id;
                });
                const bool direct = item.directly_requested;
                const std::uint64_t generation = found == after.end() ? 0 : found->generation;
                const auto state = found == after.end()
                    ? anomaly::PlatformUiPluginState::Unknown
                    : anomaly::ClassifyPluginState(found->state);
                const auto state_reason = OperationReasonForState(state);
                anomaly::PluginOperationOutcome outcome;
                anomaly::PluginOperationReason reason;
                std::string message;
                bool retryable{};

                const auto mark_failure_or_skip = [&](anomaly::PluginOperationReason failure_reason,
                                                        std::string failure_message,
                                                        bool can_retry) {
                    outcome = direct ? anomaly::PluginOperationOutcome::Failed
                                     : anomaly::PluginOperationOutcome::Skipped;
                    reason = failure_reason;
                    message = std::move(failure_message);
                    retryable = can_retry;
                };
                const auto mark_success = [&](std::string success_message) {
                    outcome = anomaly::PluginOperationOutcome::Succeeded;
                    reason = anomaly::PluginOperationReason::None;
                    message = std::move(success_message);
                    retryable = false;
                };

                if (!succeeded) {
                    mark_failure_or_skip(
                        found == after.end() || state_reason == anomaly::PluginOperationReason::None
                            ? (found == after.end() ? anomaly::PluginOperationReason::NotFound
                                                    : anomaly::PluginOperationReason::BackendFailure)
                            : state_reason,
                        found == after.end() ? "plugin was not present after backend operation"
                                             : "backend rejected operation",
                        true);
                } else {
                    switch (result.mutation) {
                    case Mutation::Reload:
                        if (found == after.end()) {
                            mark_failure_or_skip(anomaly::PluginOperationReason::NotFound,
                                "plugin was not present after reload", true);
                        } else if (state == anomaly::PlatformUiPluginState::Disabled) {
                            mark_success("disabled plugin package refreshed");
                        } else if (state_reason != anomaly::PluginOperationReason::None) {
                            mark_failure_or_skip(state_reason,
                                "plugin was not reloadable in the resulting snapshot", false);
                        } else if (before != 0 && generation != before) {
                            mark_success("new generation published");
                        } else {
                            mark_failure_or_skip(
                                anomaly::PluginOperationReason::BackendFailure,
                                "reload did not publish a replacement generation", true);
                        }
                        break;
                    case Mutation::Enable:
                        if (found == after.end() || !found->enabled ||
                            state == anomaly::PlatformUiPluginState::Disabled) {
                            mark_failure_or_skip(anomaly::PluginOperationReason::Disabled,
                                "plugin remained disabled after enable operation", false);
                        } else if (!IsHealthyPluginState(state)) {
                            mark_failure_or_skip(state_reason,
                                "plugin is not runnable after enable operation", false);
                        } else {
                            mark_success(item.reason.empty() ? "plugin is enabled" : item.reason);
                        }
                        break;
                    case Mutation::Disable:
                        if (found == after.end() && !direct) {
                            mark_success("dependent plugin is no longer installed");
                        } else if (found == after.end()) {
                            mark_failure_or_skip(anomaly::PluginOperationReason::NotFound,
                                "plugin was not present after disable operation", true);
                        } else if (state != anomaly::PlatformUiPluginState::Disabled &&
                                   state_reason != anomaly::PluginOperationReason::None) {
                            mark_failure_or_skip(state_reason,
                                "plugin entered a blocked state while disabling", false);
                        } else if (found->enabled) {
                            mark_failure_or_skip(anomaly::PluginOperationReason::BackendFailure,
                                "plugin remained enabled after disable operation", true);
                        } else if (state == anomaly::PlatformUiPluginState::Quarantined ||
                                   state == anomaly::PlatformUiPluginState::Faulted) {
                            mark_failure_or_skip(state_reason,
                                "plugin entered a failed state while disabling", true);
                        } else {
                            mark_success("plugin is disabled");
                        }
                        break;
                    case Mutation::SetVisible:
                        if (found == after.end()) {
                            mark_failure_or_skip(anomaly::PluginOperationReason::NotFound,
                                "plugin was not present after visibility operation", true);
                        } else if (!IsHealthyPluginState(state) || !found->visibility_control ||
                                   found->visible != intent.bool_value) {
                            mark_failure_or_skip(state_reason == anomaly::PluginOperationReason::None
                                    ? anomaly::PluginOperationReason::BackendFailure : state_reason,
                                "visibility state did not match the requested value", true);
                        } else {
                            mark_success("visibility state updated");
                        }
                        break;
                    default:
                        mark_success(result.message);
                        break;
                    }
                }
                result.plugins.push_back({item.id, outcome, reason, std::move(message),
                    before, generation, retryable});
            }
            if (result.plugins.empty()) {
                result.plugins.push_back({intent.subject_id,
                    succeeded ? anomaly::PluginOperationOutcome::Succeeded
                              : anomaly::PluginOperationOutcome::Failed,
                    succeeded ? anomaly::PluginOperationReason::None
                              : anomaly::PluginOperationReason::BackendFailure,
                    result.message, 0, 0, !succeeded});
            }
            if (!TrySettleIntent(queued, IntentExecutionState::Running)) return;
            anomaly::FinalizeOperation(result);
            model_.ApplyOperationResult(std::move(result));
            status_ = Text(succeeded
                ? anomaly::MessageId::OperationAccepted
                : anomaly::MessageId::OperationFailedDiagnostics);
            status_failure_ = !succeeded;
        } catch (...) {
            ApplyIntentFailure(
                queued, IntentExecutionState::Running,
                anomaly::PlatformUiResultCode::BackendFailure,
                anomaly::PluginOperationReason::BackendFailure,
                "backend operation raised an exception", true);
        }
    }

    void ResolveAwaitingBatch(anomaly::PlatformUiSnapshot& next_snapshot) {
        if (awaiting_batches_.empty()) return;
        for (auto iterator = awaiting_batches_.begin(); iterator != awaiting_batches_.end();) {
            std::map<std::string, AwaitingBatch::Observation, std::less<>> observations;
            bool explicit_outcome = true;
            for (const auto& [id, before_generation] : iterator->generations) {
                const auto* after = next_snapshot.FindPlugin(id);
                AwaitingBatch::Observation observation;
                observation.present = after != nullptr;
                observation.generation = after == nullptr ? 0 : after->generation;
                observation.state = after == nullptr
                    ? anomaly::PlatformUiPluginState::Unknown : after->UiState();
                observation.enabled = after != nullptr && after->enabled;
                observations.emplace(id, observation);
                if (after == nullptr ||
                    ((observation.state == anomaly::PlatformUiPluginState::Active ||
                      observation.state == anomaly::PlatformUiPluginState::Loaded) &&
                     observation.generation == before_generation) ||
                    observation.state == anomaly::PlatformUiPluginState::Stopping ||
                    observation.state == anomaly::PlatformUiPluginState::Unknown) {
                    explicit_outcome = false;
                }
            }
            const bool stable = iterator->observed_once &&
                observations == iterator->last_observations;
            const bool timed_out = iterator->deadline != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() >= iterator->deadline;
            iterator->last_observations = observations;
            iterator->observed_once = true;
            if (!explicit_outcome && !stable && !timed_out) {
                status_ = Text(anomaly::MessageId::OperationReloadAllAwaiting);
                status_failure_ = false;
                ++iterator;
                continue;
            }

            anomaly::PlatformUiOperationResult result;
            result.operation_id = iterator->queued.operation_id;
            result.intent_id = iterator->queued.intent.intent_id;
            result.expected_revision = iterator->queued.intent.expected_revision;
            result.observed_revision = next_snapshot.revision;
            result.state = anomaly::PlatformUiOperationState::Running;
            result.code = anomaly::PlatformUiResultCode::Accepted;
            result.mutation = Mutation::ReloadAll;
            result.message = timed_out
                ? "Reload all snapshot did not stabilize before the deadline"
                : "Reload all snapshot is complete";
            for (const auto& [id, before_generation] : iterator->generations) {
                const auto* after = next_snapshot.FindPlugin(id);
                if (after == nullptr) {
                    const auto reason = before_generation == 0
                        ? anomaly::PluginOperationReason::NotFound
                        : anomaly::PluginOperationReason::BackendFailure;
                    result.plugins.push_back({id,
                        before_generation == 0
                            ? anomaly::PluginOperationOutcome::Skipped
                            : anomaly::PluginOperationOutcome::Failed,
                        reason, "plugin was not present after reload", before_generation, 0, true});
                } else if (after->UiState() != anomaly::PlatformUiPluginState::Active &&
                           after->UiState() != anomaly::PlatformUiPluginState::Loaded) {
                    const auto reason = OperationReasonForState(after->UiState());
                    result.plugins.push_back({id, anomaly::PluginOperationOutcome::Skipped,
                        reason, "plugin was not reloadable in the resulting snapshot",
                        before_generation, after->generation, reason ==
                            anomaly::PluginOperationReason::BackendFailure});
                } else if (after->generation != before_generation) {
                    result.plugins.push_back({id, anomaly::PluginOperationOutcome::Succeeded,
                        anomaly::PluginOperationReason::None, "new generation published",
                        before_generation, after->generation, false});
                } else {
                    result.plugins.push_back({id, anomaly::PluginOperationOutcome::Failed,
                        anomaly::PluginOperationReason::BackendFailure,
                        "ReloadAll did not publish a per-plugin result", before_generation,
                        after->generation, true});
                }
            }
            if (result.plugins.empty()) {
                result.plugins.push_back({{}, anomaly::PluginOperationOutcome::Skipped,
                    anomaly::PluginOperationReason::NotFound,
                    "no installed plugins were available for reload", 0, 0, false});
            }
            if (!TrySettleIntent(iterator->queued, IntentExecutionState::Running)) {
                iterator = awaiting_batches_.erase(iterator);
                continue;
            }
            anomaly::FinalizeOperation(result);
            next_snapshot.operation_results.push_back(std::move(result));
            status_ = Text(anomaly::MessageId::OperationReloadAllPublished);
            status_failure_ = false;
            iterator = awaiting_batches_.erase(iterator);
        }
    }

    void FlushDeferredMemory() {
        if (read_requested_) {
            read_requested_ = false;
            const auto address = ParseAddress(address_.data());
            const auto& memory = plugins_.MemoryServices().memory;
            if (!address || memory == nullptr) {
                memory_status_ = Text(anomaly::MessageId::DeveloperMemoryInvalidAddress);
            } else {
                const auto bytes = memory->ReadMemory(*address, static_cast<std::size_t>(read_size_));
                if (!bytes) {
                    memory_status_ = Text(anomaly::MessageId::DeveloperMemoryReadFailed);
                } else {
                    last_address_ = *address;
                    bytes_ = *bytes;
                    memory_status_ = Text(anomaly::MessageId::DeveloperMemoryReadComplete);
                }
            }
        }
        if (!memory_write_requested_) return;
        memory_write_requested_ = false;
        if (!pending_memory_write_) return;
        const auto write = std::move(*pending_memory_write_);
        pending_memory_write_.reset();
        const auto& memory = plugins_.MemoryServices().memory;
        if (memory == nullptr) {
            memory_status_ = Text(anomaly::MessageId::DeveloperMemoryProviderUnavailable);
            return;
        }
        const bool result = write.patch
            ? memory->PatchMemory(write.address, write.bytes.data(), write.bytes.size())
            : memory->WriteMemory(write.address, write.bytes.data(), write.bytes.size());
        memory_status_ = Text(result
            ? (write.patch ? anomaly::MessageId::DeveloperMemoryTrackedPatchApplied
                           : anomaly::MessageId::DeveloperMemoryRawWriteApplied)
            : anomaly::MessageId::DeveloperMemoryWriteFailed);
        if (result) {
            std::snprintf(address_.data(), address_.size(), "0x%llX",
                static_cast<unsigned long long>(write.address));
            read_requested_ = true;
        }
    }

    PluginManager& plugins_;
    // A quarantined UI callback may outlive the render worker. Keep the
    // composition-root PluginManager alive until that owner is retired.
    std::shared_ptr<PluginManager> plugin_owner_;
    std::shared_ptr<anomaly::PluginScope> management_window_scope_;
    anomaly::UiResourceHandle management_window_;
    anomaly::UiResourceHandle logo_texture_;
    PlatformDiagnostics diagnostics_;
    std::optional<ContactInformation> contact_;
    anomaly::PlatformUiModel model_;
    std::uint64_t revision_{};
    bool catalog_ready_{};
    std::optional<anomaly::PluginCatalogSnapshot> catalog_;
    std::optional<anomaly::PluginDependencyPlan> dependencies_;
    anomaly::PluginDisplayNameMap plugin_display_names_;
    anomaly::PluginDescriptionMap plugin_descriptions_;
    mutable std::mutex catalog_mutex_;
    mutable std::mutex submission_mutex_;
    mutable std::recursive_mutex operation_mutex_;
    mutable std::mutex lifetime_mutex_;
    std::condition_variable lifetime_condition_;
    std::size_t active_callbacks_{};
    std::size_t outstanding_invocations_{};
    bool closing_{};
    std::shared_ptr<const CatalogCache> catalog_cache_;
    std::jthread catalog_worker_;
    std::vector<QueuedIntent> queued_intents_;
    std::vector<AwaitingBatch> awaiting_batches_;
    std::vector<std::shared_ptr<QueuedIntent>> rejected_callbacks_;
    std::optional<Intent> confirmation_intent_;
    anomaly::PlatformUiAffectedSetPreflight confirmation_plan_;
    bool confirmation_popup_requested_{};
    std::string repository_uninstall_plugin_id_;
    std::string repository_uninstall_plugin_name_;
    bool repository_uninstall_popup_requested_{};
    LayoutMode layout_mode_{LayoutMode::Standard};
    DeveloperPanel developer_panel_{DeveloperPanel::Plugins};
    DeveloperPluginTab developer_plugin_tab_{DeveloperPluginTab::Overview};
    bool management_shell_collapsed_{};
    bool management_shell_locked_{};
    bool management_shell_apply_initial_size_{true};
    bool management_shell_was_collapsed_{};
    ImVec2 management_shell_expanded_size_{};
    bool navigation_collapsed_{};
    bool compact_plugin_detail_{};
    std::string compact_plugin_detail_pending_id_;
    double compact_plugin_detail_requested_at_{};
    bool focus_plugin_search_{};
    std::string keyboard_plugin_focus_id_;
    bool keyboard_plugin_navigation_consumed_{};
    bool logs_follow_live_{};
    bool operation_details_open_{};
    bool operation_details_popup_requested_{};
    float plugin_list_width_{};
    std::string performance_selected_plugin_id_;
    std::size_t selected_log_index_{};
    int performance_callback_filter_{};
    int performance_state_filter_{};
    int performance_sort_{};
    SettingsSection settings_section_{SettingsSection::Interface};
    SettingsSection settings_drawing_section_{SettingsSection::Interface};
    bool settings_section_heading_drawn_{};
    anomaly::PlatformSettingsSnapshot settings_snapshot_;
    std::optional<anomaly::PlatformSettingsValues> settings_draft_;
    std::uint64_t settings_base_revision_{};
    std::shared_ptr<SettingsApplyMailbox> settings_apply_mailbox_{
        std::make_shared<SettingsApplyMailbox>()};
    std::shared_ptr<RepositoryConfigureMailbox> repository_configure_mailbox_{
        std::make_shared<RepositoryConfigureMailbox>()};
    std::optional<anomaly::PlatformSettingsApplyRequest> pending_settings_apply_;
    std::optional<anomaly::PluginRepositoryConfig> pending_repository_configure_;
    std::optional<std::string> settings_route_to_record_;
    std::optional<Route> settings_pending_route_;
    std::optional<Route> settings_route_after_save_;
    std::vector<anomaly::PlatformSettingsValidationError> settings_validation_errors_;
    std::string settings_apply_error_;
    bool settings_save_pending_{};
    bool settings_route_restored_{};
    std::optional<std::string> pending_external_url_;
    bool settings_hotkey_capture_{};
    std::array<bool, 256> settings_hotkey_down_{};
    bool settings_leave_popup_requested_{};
    bool repo_editor_loaded_{};
    bool repo_editor_master_enabled_{true};
    bool repo_editor_allow_insecure_{};
    std::vector<RepositoryChannelRow> repo_editor_rows_;
    anomaly::PluginRepositoryConfig repo_editor_baseline_;
    std::string repo_editor_status_;
    bool repo_editor_status_failure_{};
    bool repo_editor_save_pending_{};
    std::array<char, 256> search_{};
    std::array<char, 256> performance_search_{};
    std::array<char, 256> settings_search_{};
    std::array<char, 128> log_filter_{};
    std::array<char, 32> address_{};
    std::array<char, 1024> write_bytes_{};
    int read_size_{128};
    bool patch_{true};
    bool developer_mode_{};
    bool read_requested_{};
    bool memory_write_requested_{};
    bool memory_invocation_pending_{};
    bool memory_confirmation_popup_requested_{};
    std::optional<PendingMemoryWrite> pending_memory_write_;
    std::uintptr_t last_address_{};
    std::vector<std::uint8_t> bytes_;
    std::string status_;
    std::string last_toast_status_;
    std::string toast_status_;
    bool status_failure_{};
    bool toast_failure_{};
    double toast_expires_at_{};
    std::string memory_status_{"Ready"};
};

std::shared_ptr<PlatformUi> g_platform_ui;
std::vector<std::shared_ptr<PlatformUi>> g_platform_ui_quarantine;
std::optional<anomaly::PlatformUiState> g_platform_ui_state;
std::mutex g_platform_ui_lifecycle_mutex;
bool g_platform_ui_shutdown_in_progress{};
bool g_platform_ui_shutdown_pending{};

void ReapPlatformUiQuarantineLocked() {
    std::erase_if(g_platform_ui_quarantine, [](const auto& owner) {
        return owner == nullptr || owner->CanReap();
    });
}

}  // namespace

bool StandaloneHostQuarantined(const PluginManager* owner) noexcept {
    return IsStandaloneHostQuarantined(owner);
}

bool InitializePlatformUi(
    PluginManager& plugins,
    PlatformDiagnostics diagnostics,
    std::shared_ptr<PluginManager> plugin_owner) {
    if (plugin_owner == nullptr || plugin_owner.get() != &plugins) return false;
    // A failed teardown leaves the old owner in place as a quarantine fence.
    // Retry its finalization before creating a new owner; otherwise a device
    // rebuild could run an old lifecycle callback beside a new UI instance.
    {
        std::shared_ptr<PlatformUi> previous;
        {
            std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
            ReapPlatformUiQuarantineLocked();
            for (const auto& quarantined : g_platform_ui_quarantine) {
                if (quarantined->BelongsTo(plugins)) return false;
            }
            if (g_platform_ui_shutdown_in_progress) return false;
            previous = g_platform_ui;
            if (previous == nullptr && g_platform_ui_shutdown_pending) return false;
            if (previous != nullptr && !g_platform_ui_shutdown_pending) {
                return previous->BelongsTo(plugins);
            }
            if (previous != nullptr) g_platform_ui_shutdown_in_progress = true;
        }
        if (previous != nullptr) {
            PlatformUi::ShutdownResult result;
            try {
                result = previous->BeginShutdown();
            } catch (...) {
                previous->Quarantine();
                result = {};
            }
            {
                std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
                g_platform_ui_shutdown_in_progress = false;
                if (!result.callbacks_drained) {
                    if (result.state) g_platform_ui_state = *result.state;
                    if (g_platform_ui == previous) g_platform_ui.reset();
                    try {
                        g_platform_ui_quarantine.push_back(previous);
                    } catch (...) {
                        g_platform_ui = std::move(previous);
                        g_platform_ui_shutdown_pending = true;
                        return false;
                    }
                    g_platform_ui_shutdown_pending = false;
                    return false;
                }
                if (!result.state || g_platform_ui != previous) {
                    g_platform_ui_shutdown_pending = true;
                    return false;
                }
                g_platform_ui_state = *result.state;
                g_platform_ui.reset();
                g_platform_ui_shutdown_pending = false;
            }
        }
    }

    ApplyPlatformUiStyle();
    anomaly::PlatformUiState state;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (g_platform_ui_shutdown_in_progress) return false;
        if (g_platform_ui != nullptr) return g_platform_ui->BelongsTo(plugins);
        if (g_platform_ui_state) state = std::move(*g_platform_ui_state);
        auto ui = std::make_shared<PlatformUi>(
            plugins, std::move(diagnostics), std::move(state), std::move(plugin_owner));
        if (!ui->Ready()) return false;
        g_platform_ui = std::move(ui);
    }
    return true;
}

bool RevealPlatformUi() noexcept {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (g_platform_ui_shutdown_in_progress) return false;
        ui = g_platform_ui;
    }
    return ui != nullptr && ui->Reveal();
}

bool ApplyHostUiManagementExpansionRequest() noexcept {
    if (!anomaly::ConsumeHostUiManagementExpansionRequest()) return false;
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (!g_platform_ui_shutdown_in_progress) ui = g_platform_ui;
    }
    if (ui != nullptr) {
        static_cast<void>(ui->Reveal());
        ui->ExpandManagementShell();
    }
    anomaly::SetHostUiMenusCollapsed(false);
    return true;
}

void PreparePlatformUiResources() noexcept {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    if (ui != nullptr) ui->PrepareResources();
}

void DrawPlatformUi() {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    if (ui != nullptr) ui->Draw();
}

bool PlatformUiCapturingHotkey() noexcept {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    try {
        return ui != nullptr && ui->CapturingSettingsHotkey();
    } catch (...) {
        return false;
    }
}

void FlushPlatformUiActions() {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    if (ui != nullptr) ui->Flush();
}

bool ShutdownPlatformUi() {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ReapPlatformUiQuarantineLocked();
        if (g_platform_ui_shutdown_in_progress) return false;
        ui = g_platform_ui;
        // A quarantined owner still owns callbacks and the ImGui context's
        // lifetime fence. Treat it as an incomplete shutdown even though no
        // active owner is published, so callers cannot destroy a context that
        // late callbacks may still reference.
        if (ui == nullptr) {
            return g_platform_ui_quarantine.empty() && !g_platform_ui_shutdown_pending;
        }
        g_platform_ui_shutdown_in_progress = true;
    }

    PlatformUi::ShutdownResult result;
    try {
        result = ui->BeginShutdown();
    } catch (...) {
        ui->Quarantine();
        result = {};
    }

    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        g_platform_ui_shutdown_in_progress = false;
        if (!result.callbacks_drained) {
            if (result.state) g_platform_ui_state = *result.state;
            if (g_platform_ui == ui) g_platform_ui.reset();
            try {
                g_platform_ui_quarantine.push_back(ui);
            } catch (...) {
                g_platform_ui = std::move(ui);
                g_platform_ui_shutdown_pending = true;
                return false;
            }
            g_platform_ui_shutdown_pending = false;
            // The owner is now quarantined, not released. The caller must
            // retain the graphics generation until the quarantine is reaped.
            return false;
        }
        if (!result.state || g_platform_ui != ui) {
            g_platform_ui_shutdown_pending = true;
            return false;
        }
        g_platform_ui_state = *result.state;
        g_platform_ui.reset();
        g_platform_ui_shutdown_pending = false;
    }
    return true;
}

bool QuarantinePlatformUi(std::chrono::milliseconds wait_timeout) noexcept {
    const auto bounded_timeout = (std::max)(wait_timeout, std::chrono::milliseconds::zero());
    const auto deadline = bounded_timeout == std::chrono::milliseconds::max()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + bounded_timeout;
    for (;;) {
        {
            std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
            if (!g_platform_ui_shutdown_in_progress) break;
        }
        if (deadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (g_platform_ui_shutdown_in_progress) return false;
        ReapPlatformUiQuarantineLocked();
        ui = std::move(g_platform_ui);
        g_platform_ui_shutdown_pending = false;
        if (ui == nullptr) return true;
        ui->Quarantine();
        try {
            g_platform_ui_quarantine.push_back(ui);
        } catch (...) {
            // Keep the closed owner published so a later bounded shutdown can
            // retry the handoff instead of destroying it on allocation failure.
            g_platform_ui = std::move(ui);
            g_platform_ui_shutdown_pending = true;
            return false;
        }
    }
    return true;
}

bool PlatformUiQuarantined(const PluginManager* owner) noexcept {
    std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
    ReapPlatformUiQuarantineLocked();
    const bool active_closing = g_platform_ui != nullptr && g_platform_ui->Closing() &&
        (owner == nullptr || g_platform_ui->BelongsTo(*owner));
    if (active_closing) return true;
    if (owner == nullptr) return !g_platform_ui_quarantine.empty();
    return std::any_of(g_platform_ui_quarantine.begin(), g_platform_ui_quarantine.end(),
        [owner](const auto& quarantined) {
            return quarantined != nullptr && quarantined->BelongsTo(*owner);
        });
}

void RunPlatform(
    const std::filesystem::path& root,
    const AnalyzerConfig& config,
    std::stop_token stop_token,
    anomaly::CoreMemoryServices memory_services,
    std::shared_ptr<anomaly::Ue5NteAdapter> adapter,
    PlatformDiagnostics diagnostics,
    std::shared_ptr<PluginManager> plugin_owner) {
    static_cast<void>(memory_services);
    if (plugin_owner == nullptr) return;
    PluginManager& plugins = *plugin_owner;
    // Keep the native host state heap-backed so a deferred quarantine can
    // retain the ImGui/D3D generation after this worker returns.
    auto host_owner = std::make_unique<HostWindow>();
    HostWindow& host = *host_owner;
    host.plugin_owner = plugin_owner;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW), CS_CLASSDC, WindowProc, 0, 0, instance, nullptr, nullptr, nullptr,
        nullptr, L"AnomalyPluginPlatformWindow", nullptr};
    bool class_registered{};
    bool imgui_context_created{};
    bool imgui_win32_initialized{};
    bool imgui_dx11_initialized{};
    bool plugin_context_published{};
    bool plugin_service_published{};
    bool tick_callback_installed{};
    bool ui_initialized{};
    bool cleanup_finished{};
    std::shared_ptr<std::mutex> plugin_mutex;
    auto retain_generation = [&]() noexcept {
        if (host.window != nullptr) ShowWindow(host.window, SW_HIDE);
        RetainStandaloneHostOwner(host.plugin_owner);
        // The heap owner keeps g_window, the ImGui context and D3D interfaces
        // valid until the process boundary if another transition still holds
        // the UI lifecycle lock.
        host_owner.release();
        cleanup_finished = true;
    };
    struct CleanupGuard final {
        std::function<void()> cleanup;
        ~CleanupGuard() noexcept {
            if (cleanup) cleanup();
        }
    } guard;
    guard.cleanup = [&]() noexcept {
        if (cleanup_finished) return;
        bool can_release = true;
        if (tick_callback_installed && adapter != nullptr) {
            const bool tick_drained = adapter->ClearTickCallback(std::chrono::seconds(5));
            tick_callback_installed = false;
            if (!tick_drained) {
                std::ofstream(root / L"anomaly-platform.log", std::ios::app)
                    << "standalone tick shutdown deadline exceeded; "
                       "retaining host generation\n";
                if (ui_initialized) {
                    try { static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100))); }
                    catch (...) { }
                }
                retain_generation();
                return;
            }
        }
        if (ui_initialized) {
            bool ui_released{};
            try {
                ui_released = ShutdownPlatformUi();
            } catch (...) {
                ui_released = false;
            }
            if (!ui_released) {
                try {
                    // Quarantine is a successful handoff, but it is not a
                    // release: callbacks may still hold the old ImGui
                    // context. Retain this host generation unconditionally.
                    static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100)));
                } catch (...) {
                }
                ui_released = false;
            }
            if (ui_released) {
                ui_initialized = false;
            } else {
                can_release = false;
            }
        }
        if (can_release && (plugin_context_published || plugin_service_published) &&
            plugin_mutex != nullptr) {
            std::unique_lock plugin_lock(*plugin_mutex, std::defer_lock);
            if (!plugin_lock.try_lock()) {
                can_release = false;
            } else {
                try {
                    if (plugin_service_published) {
                        plugins.SetNteEscMenuHostAction({});
                        plugins.SetUiService(nullptr);
                        plugin_service_published = false;
                    }
                    if (plugin_context_published) {
                        plugins.SetImGuiContext(nullptr);
                        plugin_context_published = false;
                    }
                } catch (...) {
                    can_release = false;
                }
            }
        }
        if (!can_release) {
            retain_generation();
            return;
        }
        if (imgui_dx11_initialized) {
            try { ImGui_ImplDX11_Shutdown(); } catch (...) { }
            imgui_dx11_initialized = false;
        }
        if (imgui_win32_initialized) {
            try { ImGui_ImplWin32_Shutdown(); } catch (...) { }
            imgui_win32_initialized = false;
        }
        if (imgui_context_created) {
            try {
                if (ImGui::GetCurrentContext() != nullptr) ImGui::DestroyContext();
            } catch (...) { }
            imgui_context_created = false;
        }
        try { DestroyDevice(host); } catch (...) { }
        if (host.window != nullptr) {
            try { DestroyWindow(host.window); } catch (...) { }
            host.window = nullptr;
        }
        if (class_registered) {
            try { UnregisterClassW(window_class.lpszClassName, instance); } catch (...) { }
            class_registered = false;
        }
        if (g_window == &host) g_window = nullptr;
        cleanup_finished = true;
    };
    host.visible = config.platform_visible;
    host.toggle_key = config.platform_toggle_key;
    const auto settings_snapshot = diagnostics.settings_snapshot;
    if (settings_snapshot) {
        const auto settings = settings_snapshot();
        if (settings.ready) host.toggle_key = settings.values.input_menu_toggle;
    }
    if (config.platform_attach_to_process_window) {
        for (int attempt = 0;
             attempt < 100 && host.target == nullptr && !stop_token.stop_requested();
             ++attempt) {
            host.target = FindProcessWindow();
            if (host.target == nullptr) Sleep(100);
        }
        if (stop_token.stop_requested()) {
            return;
        }
        host.attached = host.target != nullptr;
    }
    g_window = &host;

    class_registered = RegisterClassExW(&window_class) != 0;
    const DWORD extended_style = WS_EX_TOOLWINDOW;
    const DWORD window_style = host.attached ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    host.window = CreateWindowExW(
        extended_style, window_class.lpszClassName, L"Anomaly Plugin Platform", window_style,
        120, 100, 1080, 720, host.attached ? host.target : nullptr, nullptr, instance, nullptr);
    if (host.window == nullptr || !CreateDevice(host)) {
        std::ofstream(root / L"anomaly-platform.log", std::ios::app)
            << "platform window/device creation failed: " << GetLastError() << '\n';
        return;
    }
    {
        std::ofstream log(root / L"anomaly-platform.log", std::ios::app);
        log << "pid=" << GetCurrentProcessId() << " window=" << host.window
            << " attached=" << (host.attached ? 1 : 0) << " target=" << host.target << '\n';
    }

    IMGUI_CHECKVERSION();
    imgui_context_created = ImGui::CreateContext() != nullptr;
    if (!imgui_context_created) return;
    static_cast<void>(ConfigurePlatformUiFontAtlas(root));
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    host.imgui_ini_path = std::make_shared<std::string>((root / L"anomaly-imgui.ini").string());
    io.IniFilename = host.imgui_ini_path->c_str();
    imgui_win32_initialized = ImGui_ImplWin32_Init(host.window);
    if (!imgui_win32_initialized) return;
    imgui_dx11_initialized = ImGui_ImplDX11_Init(host.device, host.context);
    if (!imgui_dx11_initialized) return;
    static_cast<void>(CreateStandaloneHeaderLogo(host));

    plugin_mutex = std::make_shared<std::mutex>();
    const auto lifecycle_invoke = diagnostics.lifecycle_invoke;
    diagnostics.lifecycle_invoke = [lifecycle_invoke, plugin_mutex](
        std::function<void()> operation) -> std::uint32_t {
        auto guarded = [plugin_mutex, operation = std::move(operation)]() mutable {
            std::scoped_lock lock(*plugin_mutex);
            operation();
        };
        if (lifecycle_invoke) return lifecycle_invoke(std::move(guarded));
        try {
            guarded();
            return ERROR_SUCCESS;
        } catch (...) {
            return ERROR_UNHANDLED_EXCEPTION;
        }
    };
    const auto lifecycle_post = diagnostics.lifecycle_post;
    diagnostics.lifecycle_post = [lifecycle_post, lifecycle_invoke, plugin_mutex](
        std::function<void()> operation) -> std::uint32_t {
        auto guarded = [plugin_mutex, operation = std::move(operation)]() mutable {
            std::scoped_lock lock(*plugin_mutex);
            operation();
        };
        if (lifecycle_post) return lifecycle_post(std::move(guarded));
        if (lifecycle_invoke) return lifecycle_invoke(std::move(guarded));
        try {
            guarded();
            return ERROR_SUCCESS;
        } catch (...) {
            return ERROR_UNHANDLED_EXCEPTION;
        }
    };
    if (adapter != nullptr) {
        tick_callback_installed = true;
        const auto game_pump = diagnostics.game_pump;
        adapter->SetTickCallback([plugin_mutex, &plugins, game_pump](double delta_seconds) {
            if (game_pump) static_cast<void>(game_pump());
            std::scoped_lock lock(*plugin_mutex);
            plugins.GameUpdate(delta_seconds);
        });
    }
    ui_initialized = InitializePlatformUi(plugins, std::move(diagnostics), plugin_owner);
    if (!ui_initialized) {
        // A pending owner is a teardown fence. Do not expose this new ImGui
        // context as a second management surface when the handoff is blocked.
        return;
    }
    {
        std::scoped_lock lock(*plugin_mutex);
        plugin_context_published = true;
        plugins.SetImGuiContext(ImGui::GetCurrentContext());
        plugin_service_published = true;
        plugins.SetNteEscMenuHostAction(anomaly::RequestHostUiManagementExpansion);
        plugins.SetUiService(anomaly::HostUiServiceTable());
    }
    // Match the embedded renderer: the native host remains available while
    // Insert collapses the management and plugin surfaces into their chrome.
    anomaly::SetHostUiMenusCollapsed(false);
    if (host.attached) PlaceAttachedWindow(host);
    ShowWindow(host.window, host.visible ? (host.attached ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT) : SW_HIDE);
    UpdateWindow(host.window);

    auto previous = std::chrono::steady_clock::now();
    bool running = true;
    while (running && !stop_token.stop_requested()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (settings_snapshot) {
            const auto settings = settings_snapshot();
            if (settings.ready) host.toggle_key = settings.values.input_menu_toggle;
        }
        const int toggle_state = GetAsyncKeyState(static_cast<int>(host.toggle_key));
        if (anomaly::ShouldTogglePlatformMenus(
                PlatformUiCapturingHotkey(), toggle_state)) {
            if (!host.visible) {
                host.visible = true;
                if (host.attached) PlaceAttachedWindow(host);
                ShowWindow(host.window, host.attached ? SW_SHOWNOACTIVATE : SW_SHOW);
                anomaly::SetHostUiMenusCollapsed(false);
            } else {
                anomaly::SetHostUiMenusCollapsed(!anomaly::HostUiMenusCollapsed());
            }
        }
        if (ApplyHostUiManagementExpansionRequest() && !host.visible) {
            host.visible = true;
            if (host.attached) PlaceAttachedWindow(host);
            ShowWindow(host.window, host.attached ? SW_SHOWNOACTIVATE : SW_SHOW);
        }
        const auto now = std::chrono::steady_clock::now();
        const double delta = std::chrono::duration<double>(now - previous).count();
        previous = now;
        {
            std::scoped_lock lock(*plugin_mutex);
            if (adapter != nullptr) {
                plugins.Maintenance();
            } else {
                plugins.Maintenance();
                plugins.GameUpdate(delta);
            }
        }
        if (host.attached && !IsWindow(host.target)) running = false;
        if (host.visible && host.attached && !IsIconic(host.target)) PlaceAttachedWindow(host);
        if (!host.visible || IsIconic(host.window) || (host.attached && IsIconic(host.target))) {
            Sleep(50);
            continue;
        }
        if (host.render_target == nullptr && !CreateRenderTarget(host)) {
            Sleep(10);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        {
            std::scoped_lock lock(*plugin_mutex);
            PreparePlatformUiResources();
        }
        ImGui::NewFrame();
        anomaly::PrepareHostUiFrame();
        {
            std::scoped_lock lock(*plugin_mutex);
            DrawPlatformUi();
            plugins.Draw(ImGui::GetCurrentContext());
        }
        ImGui::Render();
        constexpr float clear_color[4]{0.06f, 0.065f, 0.07f, 1.0f};
        host.context->OMSetRenderTargets(1, &host.render_target, nullptr);
        host.context->ClearRenderTargetView(host.render_target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        host.swap_chain->Present(1, 0);
        FlushPlatformUiActions();
    }

    // Stop the external tick source before draining lifecycle UI callbacks so
    // it cannot reacquire the shared plugin gate during the handoff.
    if (adapter != nullptr) {
        const bool tick_drained = adapter->ClearTickCallback(std::chrono::seconds(5));
        tick_callback_installed = false;
        if (!tick_drained) {
            std::ofstream(root / L"anomaly-platform.log", std::ios::app)
                << "standalone tick shutdown deadline exceeded; retaining host generation\n";
            if (ui_initialized) {
                try { static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100))); }
                catch (...) { }
            }
            retain_generation();
            return;
        }
    }
    const auto shutdown_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool ui_shutdown{};
    while (!ui_shutdown && std::chrono::steady_clock::now() < shutdown_deadline) {
        ui_shutdown = ShutdownPlatformUi();
        if (ui_shutdown) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!ui_shutdown) {
        std::ofstream(root / L"anomaly-platform.log", std::ios::app)
            << "standalone UI shutdown deadline exceeded; continuing host cleanup\n";
        // Quarantine never proves that callbacks have drained. Do not destroy
        // the ImGui context from this worker regardless of whether the bounded
        // handoff succeeds or another transition still owns the active UI.
        static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100)));
        std::ofstream(root / L"anomaly-platform.log", std::ios::app)
            << "standalone UI quarantine recorded; retaining graphics generation\n";
        retain_generation();
        return;
    }
    ui_initialized = false;
    guard.cleanup();
}

}  // namespace ue5mem
