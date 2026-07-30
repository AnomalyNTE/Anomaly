#include "embedded_host_internal.hpp"

#include "anomaly/host_ui_service.hpp"
#include "anomaly/nte_esc_menu_bridge.hpp"
#include "anomaly/structured_logger.hpp"
#include "anomaly/thread_local_value.hpp"
#include "anomaly/ue5_nte_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <stop_token>
#include <thread>

namespace ue5mem::embedded {

std::atomic<EmbeddedState*> g_state{};
PresentFn g_present{};
Present1Fn g_present1{};
ResizeBuffersFn g_resize_buffers{};
ResizeBuffers1Fn g_resize_buffers1{};
ExecuteCommandListsFn g_execute_command_lists{};
std::unique_ptr<anomaly::HookManager> g_hooks;

namespace {

struct ScopeExit final {
    std::function<void()> callback;
    ~ScopeExit() {
        if (callback) callback();
    }
};

std::uint64_t CaptureGeneration(const EmbeddedState* state) noexcept {
    if (state == nullptr || !state->diagnostics.capture_generation) return 0;
    try {
        return state->diagnostics.capture_generation();
    } catch (...) {
        return 0;
    }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap_chain, UINT interval, UINT flags) {
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    static anomaly::ThreadLocalScalar<bool> rendering;
    if (callback && !rendering.Get()) {
        rendering.Set(true);
        RenderEmbedded(swap_chain, flags);
        rendering.Set(false);
    }
    const HRESULT result = g_present(swap_chain, interval, flags);
    if (callback) HandlePresentResult(swap_chain, result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent1(
    IDXGISwapChain1* swap_chain, UINT interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    static anomaly::ThreadLocalScalar<bool> rendering;
    if (callback && !rendering.Get()) {
        rendering.Set(true);
        RenderEmbedded(swap_chain, flags);
        rendering.Set(false);
    }
    const HRESULT result = g_present1(swap_chain, interval, flags, parameters);
    if (callback) HandlePresentResult(swap_chain, result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(
    IDXGISwapChain* swap_chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) {
    const auto started = std::chrono::steady_clock::now();
    const DWORD thread_id = GetCurrentThreadId();
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    auto* state = g_state.load(std::memory_order_acquire);
    const std::uint64_t capture_generation = CaptureGeneration(state);
    const bool affected = callback && BeforeResize(swap_chain);
    const HRESULT result = g_resize_buffers(swap_chain, count, width, height, format, flags);
    const bool success = callback && AfterResize(affected, result);
    if (affected && state != nullptr && state->diagnostics.resize) {
        try {
            state->diagnostics.resize(
                capture_generation,
                thread_id,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started),
                success);
        } catch (...) {
        }
    }
    FinishResizeEvidenceHandoff(success);
    return result;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers1(
    IDXGISwapChain3* swap_chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags, const UINT* creation_node_mask,
    IUnknown* const* present_queue) {
    const auto started = std::chrono::steady_clock::now();
    const DWORD thread_id = GetCurrentThreadId();
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    auto* state = g_state.load(std::memory_order_acquire);
    const std::uint64_t capture_generation = CaptureGeneration(state);
    const bool affected = callback && BeforeResize(
        swap_chain, count, present_queue, true);
    const HRESULT result = g_resize_buffers1(
        swap_chain, count, width, height, format, flags,
        creation_node_mask, present_queue);
    const bool success = callback && AfterResize(affected, result);
    if (affected && state != nullptr && state->diagnostics.resize) {
        try {
            state->diagnostics.resize(
                capture_generation,
                thread_id,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started),
                success);
        } catch (...) {
        }
    }
    FinishResizeEvidenceHandoff(success);
    return result;
}

void STDMETHODCALLTYPE HookExecuteCommandLists(
    ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    if (callback) CaptureCommandQueue(queue);
    g_execute_command_lists(queue, count, lists);
}

bool InstallHooks() {
    if (g_hooks != nullptr) return false;
    const HookTargets targets = DiscoverD3D12HookTargets();
    if (!targets) return false;
    g_hooks = std::make_unique<anomaly::HookManager>(anomaly::CreateMinHookBackend());
    return g_hooks->Create(
               std::string(kRendererHookOwner), kRendererHookGeneration, "execute-command-lists",
               targets.execute_command_lists, reinterpret_cast<void*>(HookExecuteCommandLists),
               reinterpret_cast<void**>(&g_execute_command_lists)) &&
        g_hooks->Create(
            std::string(kRendererHookOwner), kRendererHookGeneration, "present",
            targets.present, reinterpret_cast<void*>(HookPresent),
            reinterpret_cast<void**>(&g_present)) &&
        g_hooks->Create(
            std::string(kRendererHookOwner), kRendererHookGeneration, "resize-buffers",
            targets.resize_buffers, reinterpret_cast<void*>(HookResizeBuffers),
            reinterpret_cast<void**>(&g_resize_buffers)) &&
        g_hooks->Create(
            std::string(kRendererHookOwner), kRendererHookGeneration, "resize-buffers1",
            targets.resize_buffers1, reinterpret_cast<void*>(HookResizeBuffers1),
            reinterpret_cast<void**>(&g_resize_buffers1)) &&
        g_hooks->Create(
            std::string(kRendererHookOwner), kRendererHookGeneration, "present1",
            targets.present1, reinterpret_cast<void*>(HookPresent1),
            reinterpret_cast<void**>(&g_present1)) &&
        g_hooks->EnableOwner(kRendererHookOwner, kRendererHookGeneration);
}

[[nodiscard]] bool RemoveHooks() noexcept {
    if (g_hooks != nullptr) {
        bool owner_registered{};
        try {
            const auto snapshot = g_hooks->Snapshot();
            owner_registered = std::ranges::any_of(snapshot, [](const auto& hook) {
                return hook.owner == kRendererHookOwner &&
                    hook.generation == kRendererHookGeneration;
            });
        } catch (...) {
            return false;
        }
        if (!owner_registered) {
            g_hooks.reset();
        } else {
            static_cast<void>(
                g_hooks->DisableOwner(kRendererHookOwner, kRendererHookGeneration));
            if (!g_hooks->RemoveOwner(
                    kRendererHookOwner, kRendererHookGeneration,
                    std::chrono::seconds(5))) {
                // Keep the HookManager and its generation mapped until every
                // callback lease drains. Callers retain the heap-backed
                // renderer state when this bounded handoff fails.
                return false;
            }
            g_hooks.reset();
        }
    }
    g_present = nullptr;
    g_present1 = nullptr;
    g_resize_buffers = nullptr;
    g_resize_buffers1 = nullptr;
    g_execute_command_lists = nullptr;
    return true;
}

}  // namespace

}  // namespace ue5mem::embedded

namespace ue5mem {

void RunEmbeddedPlatform(
    const std::filesystem::path& root,
    const AnalyzerConfig& config,
    std::stop_token stop_token,
    anomaly::CoreMemoryServices memory_services,
    std::shared_ptr<anomaly::Ue5NteAdapter> adapter,
    PlatformDiagnostics diagnostics,
    std::shared_ptr<PluginManager> plugin_owner) {
    using namespace embedded;
    static_cast<void>(memory_services);
    if (plugin_owner == nullptr) return;
    // Keep the state heap-backed so a failed final quarantine can retain the
    // live graphics generation without leaving g_state pointing at a stack
    // object after this worker returns.
    auto state_owner = std::make_unique<EmbeddedState>();
    EmbeddedState& state = *state_owner;
    state.root = root;
    state.config = config;
    state.diagnostics = std::move(diagnostics);
    anomaly::SetHostUiMenusCollapsed(!config.platform_visible);
    state.plugins = plugin_owner.get();
    state.plugin_owner = plugin_owner;
    state.plugins_loaded = true;
    if (g_hooks != nullptr || g_state.load(std::memory_order_acquire) != nullptr) return;
    const auto tick_plugins = plugin_owner;
    const auto tick_enabled = std::make_shared<std::atomic_bool>(true);
    std::shared_ptr<anomaly::NteEscMenuBridge> esc_menu_bridge;
    bool tick_bound{};
    bool esc_menu_bridge_started{};
    bool hooks_owned{};
    bool cleanup_done{};
    ScopeExit cleanup_guard;
    cleanup_guard.callback = [&]() noexcept {
        if (cleanup_done) return;
        cleanup_done = true;
        try {
            tick_enabled->store(false, std::memory_order_release);
            if (tick_bound && adapter != nullptr) {
                const bool tick_drained = adapter->ClearTickCallback(std::chrono::seconds(5));
                tick_bound = false;
                if (!tick_drained) {
                    // The callback slot is detached, but a game tick is still
                    // executing. Disable new renderer entries and retain the
                    // heap generation until that callback's owner is retired.
                    if (g_hooks != nullptr) {
                        static_cast<void>(g_hooks->DisableOwner(
                            kRendererHookOwner, kRendererHookGeneration));
                    }
                    std::ofstream(root / L"anomaly-platform.log", std::ios::app)
                        << "embedded tick shutdown deadline exceeded; "
                           "retaining renderer generation\n";
                    state.quarantined_plugin_owner = plugin_owner;
                    state_owner.release();
                    return;
                }
            }
            if (esc_menu_bridge_started && esc_menu_bridge != nullptr) {
                esc_menu_bridge_started = false;
                if (!esc_menu_bridge->Stop(std::chrono::seconds(5))) {
                    std::ofstream(root / L"anomaly-platform.log", std::ios::app)
                        << "NTE ESC menu bridge shutdown deadline exceeded\n";
                }
            }
            if (hooks_owned && !RemoveHooks()) {
                std::ofstream(root / L"anomaly-platform.log", std::ios::app)
                    << "embedded hook shutdown deadline exceeded; "
                       "retaining renderer generation\n";
                state.quarantined_plugin_owner = plugin_owner;
                state_owner.release();
                return;
            }
            const bool graphics_active = state.platform_ui_initialized ||
                state.dx12_initialized || state.win32_initialized ||
                state.device != nullptr || state.fence != nullptr;
            if (graphics_active) {
                std::scoped_lock render_lock(state.render_mutex);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
                bool released{};
                while (!released && std::chrono::steady_clock::now() < deadline) {
                    released = ReleaseGraphics(state, RendererLifecycle::Stopping);
                    if (!released) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                if (!released && !ReleaseGraphics(
                        state, RendererLifecycle::Stopping, true)) {
                    std::ofstream(root / L"anomaly-platform.log", std::ios::app)
                        << "embedded UI quarantine deferred; "
                           "retaining graphics generation\n";
                    state.quarantined_plugin_owner = plugin_owner;
                    state_owner.release();
                    return;
                }
            }
            {
                std::scoped_lock plugin_lock(*state.plugin_mutex);
                if (state.plugins_loaded && state.plugins != nullptr) {
                    state.plugins->SetUiService(nullptr);
                    state.plugins->SetImGuiContext(nullptr);
                    state.plugins_loaded = false;
                }
            }
            {
                std::scoped_lock queue_lock(state.queue_mutex);
                Release(state.captured_queue);
            }
            state.renderer = RendererLifecycle::Stopped;
            auto* expected = &state;
            static_cast<void>(g_state.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire));
        } catch (...) {
            // Preserve the heap-backed state when cleanup itself encounters a
            // host exception; in-flight callbacks must never observe a freed
            // renderer generation.
            state.quarantined_plugin_owner = plugin_owner;
            state_owner.release();
        }
    };
    if (adapter != nullptr) {
        const auto bridge_logger = state.diagnostics.logger;
        esc_menu_bridge = std::make_shared<anomaly::NteEscMenuBridge>(
            memory_services, adapter->Resolution(),
            [weak = std::weak_ptr<PluginManager>(plugin_owner)] {
                const auto plugins = weak.lock();
                return plugins == nullptr
                    ? std::vector<anomaly::NteEscMenuButtonSnapshot>{}
                    : plugins->NteEscMenuButtons();
            },
            [weak = std::weak_ptr<PluginManager>(plugin_owner)](
                const AnomalyGenerationHandleV1 handle) {
                if (const auto plugins = weak.lock()) {
                    static_cast<void>(plugins->InvokeNteEscMenuButton(handle));
                }
            },
            [bridge_logger](const std::uint32_t level, std::string message) {
                if (bridge_logger == nullptr) return;
                anomaly::LogLevel mapped = anomaly::LogLevel::Info;
                if (level >= ANOMALY_CORE_LOG_LEVEL_V1_ERROR) {
                    mapped = anomaly::LogLevel::Error;
                } else if (level >= ANOMALY_CORE_LOG_LEVEL_V1_WARNING) {
                    mapped = anomaly::LogLevel::Warning;
                } else if (level == ANOMALY_CORE_LOG_LEVEL_V1_TRACE) {
                    mapped = anomaly::LogLevel::Trace;
                }
                anomaly::LogDetails details;
                details.thread_domain = anomaly::LogThreadDomain::Game;
                details.event_id = "nte.esc-menu-bridge";
                static_cast<void>(bridge_logger->Log(
                    mapped, "nte.esc-menu", std::move(message), std::move(details)));
            });
        esc_menu_bridge_started = esc_menu_bridge->Start();
        if (!esc_menu_bridge_started) {
            std::ofstream(root / L"anomaly-platform.log", std::ios::app)
                << "NTE ESC menu bridge unavailable for active Profile\n";
        }
        const auto plugin_mutex = state.plugin_mutex;
        const auto game_pump = state.diagnostics.game_pump;
        tick_bound = true;
        adapter->SetTickCallback([
            tick_plugins, plugin_mutex, tick_enabled, game_pump,
            esc_menu_bridge](double delta_seconds) {
            if (!tick_enabled->load(std::memory_order_acquire)) return;
            if (game_pump) static_cast<void>(game_pump());
            std::scoped_lock plugin_lock(*plugin_mutex);
            if (tick_enabled->load(std::memory_order_relaxed)) {
                if (tick_plugins != nullptr) tick_plugins->GameUpdate(delta_seconds);
                if (esc_menu_bridge != nullptr && esc_menu_bridge->Started()) {
                    esc_menu_bridge->Update(delta_seconds);
                }
            }
        });
    }
    EmbeddedState* expected_state{};
    if (!g_state.compare_exchange_strong(
            expected_state, &state, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    hooks_owned = true;
    const bool installed = InstallHooks();
    if (g_hooks == nullptr) hooks_owned = false;
    const auto upstream_hooks = state.diagnostics.hooks;
    state.diagnostics.hooks = [upstream_hooks] {
        auto result = upstream_hooks ? upstream_hooks() : std::vector<anomaly::HookRecordView>{};
        if (g_hooks != nullptr) {
            auto renderer = g_hooks->Snapshot();
            result.insert(result.end(), renderer.begin(), renderer.end());
        }
        return result;
    };
    std::ofstream(root / L"anomaly-platform.log", std::ios::app)
        << "pid=" << GetCurrentProcessId() << " d3d12_hooks=" << (installed ? 1 : 0)
        << " plugins_decoupled=1\n";
    if (!installed) {
        return;
    }

    while (!stop_token.stop_requested()) {
        {
            std::scoped_lock plugin_lock(*state.plugin_mutex);
            if (state.plugins != nullptr) state.plugins->MaintenancePluginState();
        }
        if (esc_menu_bridge_started && esc_menu_bridge != nullptr) {
            esc_menu_bridge->Discover();
        }
        // Persist through the registry's own synchronization after releasing
        // the renderer gate. The shared owner remains live for this loop, and
        // the filesystem publish must never delay a Present callback.
        plugin_owner->PersistUiWindowState();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    cleanup_guard.callback();
}

bool PlatformHostQuarantined(const PluginManager* owner) noexcept {
    if (PlatformUiQuarantined(owner)) return true;
    if (StandaloneHostQuarantined(owner)) return true;
    const auto* state = embedded::g_state.load(std::memory_order_acquire);
    if (state == nullptr || state->quarantined_plugin_owner == nullptr) return false;
    return owner == nullptr || state->quarantined_plugin_owner.get() == owner;
}

}  // namespace ue5mem
