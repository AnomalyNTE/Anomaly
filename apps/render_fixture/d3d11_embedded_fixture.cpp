// Drives the embedded overlay against a swap chain owned by a D3D11 device.
//
// The existing render fixture exercises the dispatcher; nothing exercised the
// Present interception path, and nothing at all exercised it for D3D11. That
// gap is why an overlay that only ever knew how to draw with D3D12 could sit in
// a D3D11 title reporting nothing but "waiting for a command queue".
//
// The fixture stands in for the game: it owns the window, the device and the
// swap chain, and it presents frames. Everything else is the real embedded
// platform, hooks included.
#include "anomaly/i18n.hpp"
#include "anomaly/platform_settings.hpp"
#include "anomaly/platform_ui_theme.hpp"

#include "config.hpp"
#include "platform_host.hpp"
#include "plugin_manager.hpp"

#include <Windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;

// Counts clicks that reached the window standing in for the game.
//
// An overlay covering the whole client area has to let these through wherever it
// is not drawing, and the only way to see that is from the other side: a click
// the overlay swallowed never arrives here.
std::atomic<int> g_target_clicks{};

LRESULT CALLBACK FixtureWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_LBUTTONDOWN) g_target_clicks.fetch_add(1);
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

// Clears to a recognisable colour so a screenshot can tell overlay pixels from
// the fixture's own.
constexpr float kSceneColor[4]{0.12f, 0.28f, 0.55f, 1.0f};

}  // namespace

// Stands in for the runtime's Worker execution domain.
//
// Texture payloads arrive encoded and are decoded off the render thread, so
// without a dispatcher a requested image stays in its Auto format forever and
// the upload path is never reached. The fixture needs one for the same reason
// the game does: it is what makes an icon observable rather than absent.
class FixtureWorker final {
public:
    [[nodiscard]] bool Post(std::function<void()> callback) {
        if (!callback) return false;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) return false;
            queue_.push_back(std::move(callback));
        }
        signal_.notify_one();
        return true;
    }

    void Run(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::function<void()> callback;
            {
                std::unique_lock lock(mutex_);
                signal_.wait_for(lock, std::chrono::milliseconds(50),
                    [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                callback = std::move(queue_.front());
                queue_.pop_front();
            }
            try { callback(); } catch (...) { }
        }
    }

    void Stop() {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        signal_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable signal_;
    std::deque<std::function<void()>> queue_;
    bool stopping_{};
};

int main(int argc, char** argv) {
    // "standalone" drives the other half of the same problem: the attached host
    // window that sits over the game instead of drawing into its swap chain.
    // Both modes need a real D3D11 window to sit on, and this fixture already
    // owns one.
    bool standalone = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && std::string_view(argv[index]) == "standalone") {
            standalone = true;
        }
    }
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW), CS_CLASSDC, FixtureWindowProc, 0, 0, instance, nullptr, nullptr,
        nullptr, nullptr, L"AnomalyD3D11Fixture", nullptr};
    if (RegisterClassExW(&window_class) == 0) {
        std::printf("fail RegisterClassExW %lu\n", GetLastError());
        return 1;
    }
    RECT desired{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
    const HWND window = CreateWindowExW(
        0, window_class.lpszClassName, L"Anomaly D3D11 Fixture", WS_OVERLAPPEDWINDOW, 80, 80,
        desired.right - desired.left, desired.bottom - desired.top, nullptr, nullptr, instance,
        nullptr);
    if (window == nullptr) {
        std::printf("fail CreateWindowExW %lu\n", GetLastError());
        return 1;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    DXGI_SWAP_CHAIN_DESC swap_description{};
    swap_description.BufferCount = 2;
    swap_description.BufferDesc.Width = kWidth;
    swap_description.BufferDesc.Height = kHeight;
    // The game presents a ten bit back buffer, so match it: a format the
    // overlay mishandles would otherwise only show up in the game.
    swap_description.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_description.OutputWindow = window;
    swap_description.SampleDesc.Count = 1;
    swap_description.Windowed = TRUE;
    swap_description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* swap_chain{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &swap_description, &swap_chain, &device, &selected, &context);
    if (FAILED(result)) {
        std::printf("fail D3D11CreateDeviceAndSwapChain 0x%08lX\n",
                    static_cast<unsigned long>(result));
        return 1;
    }

    const auto root = ExecutableDirectory() / L"Anomaly";
    auto config = ue5mem::AnalyzerConfig::Load(root / L"anomaly.ini");
    config.platform_enabled = true;
    config.platform_visible = true;
    config.platform_embedded = !standalone;
    config.platform_attach_to_process_window = standalone;
    const auto locale = anomaly::ResolveUserLocale(config.platform_language);
    ue5mem::PlatformDiagnostics diagnostics;
    diagnostics.runtime_root = root;
    diagnostics.translator =
        anomaly::LoadHostCatalog(locale.locale, root / L"locales" / L"host").translator;
    auto worker = std::make_shared<FixtureWorker>();
    std::stop_source worker_stop;
    std::thread worker_thread([worker, token = worker_stop.get_token()] {
        worker->Run(token);
    });
    auto plugins = std::make_shared<ue5mem::PluginManager>(
        root, config.plugin_directory, anomaly::CoreMemoryServices{},
        ue5mem::PluginCallbackBudgets{}, nullptr, anomaly::HotkeyDispatcher{},
        [worker](std::string, std::uint64_t, std::function<void()> callback) -> bool {
            return worker->Post(std::move(callback));
        });
    plugins->SetTranslator(diagnostics.translator);
    plugins->LoadAll();
    std::filesystem::create_directories(root / L"config");
    auto settings = std::make_shared<anomaly::PlatformSettingsStore>(root);
    static_cast<void>(settings->Start());
    diagnostics.settings_snapshot = [settings] { return settings->Snapshot(); };

    std::stop_source stop;
    std::thread platform([&] {
        // Embedded draws into the fixture's own swap chain; standalone puts an
        // attached window over it. They are different entry points, not one
        // function reading a flag.
        if (standalone) {
            ue5mem::RunPlatform(
                root, config, stop.get_token(), {}, {}, diagnostics, plugins);
        } else {
            ue5mem::RunEmbeddedPlatform(
                root, config, stop.get_token(), {}, {}, diagnostics, plugins);
        }
    });

    // Give the bridge time to publish its state and install the DXGI hooks
    // before the first Present arrives.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    ID3D11RenderTargetView* render_target{};
    ID3D11Texture2D* back_buffer{};
    if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        device->CreateRenderTargetView(back_buffer, nullptr, &render_target);
        back_buffer->Release();
    }

    int presented = 0;
    int resizes = 0;
    UINT width = kWidth;
    UINT height = kHeight;
    // Standalone brings up a second window and attaches it, which takes longer
    // to settle than drawing into a swap chain that is already presenting.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(standalone ? 30 : 12);
    bool quit = false;
    while (!quit && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) quit = true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // Two resizes part way through. A game does this on every alt-tab and
        // resolution change, and the overlay has to drop its view of the back
        // buffer first or ResizeBuffers fails outright.
        if ((presented == 240 || presented == 420) && resizes < 2) {
            ++resizes;
            width = presented == 240 ? 1024u : kWidth;
            height = presented == 240 ? 640u : kHeight;
            if (render_target != nullptr) {
                context->OMSetRenderTargets(0, nullptr, nullptr);
                render_target->Release();
                render_target = nullptr;
            }
            const HRESULT resized = swap_chain->ResizeBuffers(
                0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            std::printf(
                "resize %d -> %ux%u hr=0x%08lX\n", resizes, width, height,
                static_cast<unsigned long>(resized));
            if (FAILED(resized)) break;
            if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
                device->CreateRenderTargetView(back_buffer, nullptr, &render_target);
                back_buffer->Release();
                back_buffer = nullptr;
            }
        }
        if (render_target != nullptr) {
            context->OMSetRenderTargets(1, &render_target, nullptr);
            context->ClearRenderTargetView(render_target, kSceneColor);
        }
        const HRESULT present = swap_chain->Present(1, 0);
        if (FAILED(present)) {
            std::printf("Present failed 0x%08lX after %d frames\n",
                        static_cast<unsigned long>(present), presented);
            break;
        }
        ++presented;
    }
    std::printf("presented %d frames, resizes %d, target clicks %d\n",
                presented, resizes, g_target_clicks.load());

    stop.request_stop();
    if (platform.joinable()) platform.join();
    static_cast<void>(plugins->StopForRuntime());
    worker->Stop();
    worker_stop.request_stop();
    if (worker_thread.joinable()) worker_thread.join();
    if (render_target != nullptr) render_target->Release();
    swap_chain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);
    std::printf("ok\n");
    return 0;
}
