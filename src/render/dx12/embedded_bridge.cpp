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
std::atomic_bool g_performance_diagnostics_enabled{};

void EmbeddedPerformanceProbe::SetEnabled(const bool enabled) noexcept {
    const bool previous = enabled_.exchange(enabled, std::memory_order_acq_rel);
    g_performance_diagnostics_enabled.store(enabled, std::memory_order_release);
    if (previous == enabled) return;
    last_present_nanoseconds_.store(0, std::memory_order_relaxed);
    last_game_tick_nanoseconds_.store(0, std::memory_order_relaxed);
    reset_requested_.store(true, std::memory_order_release);
}

bool EmbeddedPerformanceProbe::Enabled() const noexcept {
    return enabled_.load(std::memory_order_relaxed);
}

void EmbeddedPerformanceProbe::ObservePresent() noexcept {
    if (!Enabled()) return;
    present_calls_.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto previous = last_present_nanoseconds_.exchange(now, std::memory_order_relaxed);
    if (previous > 0 && now > previous) {
        Record(
            EmbeddedPerformanceStage::PresentInterval,
            std::chrono::nanoseconds(now - previous));
    }
}

void EmbeddedPerformanceProbe::ObserveExecute() noexcept {
    if (!Enabled()) return;
    execute_calls_.fetch_add(1, std::memory_order_relaxed);
}

bool EmbeddedPerformanceProbe::SampleRender() noexcept {
    if (!Enabled()) return false;
    render_calls_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool EmbeddedPerformanceProbe::SampleGameTick() noexcept {
    if (!Enabled()) return false;
    game_tick_calls_.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto previous = last_game_tick_nanoseconds_.exchange(now, std::memory_order_relaxed);
    if (previous > 0 && now > previous) {
        Record(
            EmbeddedPerformanceStage::GameTickInterval,
            std::chrono::nanoseconds(now - previous));
    }
    return true;
}

void EmbeddedPerformanceProbe::Record(
    const EmbeddedPerformanceStage stage,
    const std::chrono::steady_clock::duration elapsed) noexcept {
    if (!Enabled()) return;
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (nanoseconds < 0) return;
    auto& bucket = buckets_[static_cast<std::size_t>(stage)];
    const auto value = static_cast<std::uint64_t>(nanoseconds);
    if (stage == EmbeddedPerformanceStage::PresentInterval) {
        if (elapsed >= std::chrono::milliseconds(25)) {
            present_gap_over_25ms_.fetch_add(1, std::memory_order_relaxed);
        }
        if (elapsed >= std::chrono::milliseconds(50)) {
            present_gap_over_50ms_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bucket.total_nanoseconds.fetch_add(value, std::memory_order_relaxed);
    std::uint64_t maximum = bucket.maximum_nanoseconds.load(std::memory_order_relaxed);
    while (maximum < value && !bucket.maximum_nanoseconds.compare_exchange_weak(
               maximum, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    std::uint64_t window_maximum =
        bucket.window_maximum_nanoseconds.load(std::memory_order_relaxed);
    while (window_maximum < value &&
           !bucket.window_maximum_nanoseconds.compare_exchange_weak(
               window_maximum, value,
               std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    bucket.window_total_nanoseconds.fetch_add(value, std::memory_order_relaxed);
    bucket.window_samples.fetch_add(1, std::memory_order_relaxed);
    bucket.samples.fetch_add(1, std::memory_order_release);
}

void EmbeddedPerformanceProbe::RecordMaintenance(
    const std::chrono::steady_clock::duration elapsed) noexcept {
    if (!Enabled()) return;
    maintenance_calls_.fetch_add(1, std::memory_order_relaxed);
    Record(EmbeddedPerformanceStage::WorkerMaintenance, elapsed);
}

void EmbeddedPerformanceProbe::RecordPersistence(
    const std::chrono::steady_clock::duration elapsed) noexcept {
    if (!Enabled()) return;
    persistence_calls_.fetch_add(1, std::memory_order_relaxed);
    Record(EmbeddedPerformanceStage::WorkerPersist, elapsed);
}

void EmbeddedPerformanceProbe::Publish(
    const std::shared_ptr<anomaly::StructuredLogger>& logger) noexcept {
    if (reset_requested_.exchange(false, std::memory_order_acq_rel)) {
        for (auto& bucket : buckets_) {
            bucket.samples.store(0, std::memory_order_relaxed);
            bucket.total_nanoseconds.store(0, std::memory_order_relaxed);
            bucket.maximum_nanoseconds.store(0, std::memory_order_relaxed);
            bucket.window_samples.store(0, std::memory_order_relaxed);
            bucket.window_total_nanoseconds.store(0, std::memory_order_relaxed);
            bucket.window_maximum_nanoseconds.store(0, std::memory_order_relaxed);
        }
        present_calls_.store(0, std::memory_order_relaxed);
        execute_calls_.store(0, std::memory_order_relaxed);
        render_calls_.store(0, std::memory_order_relaxed);
        game_tick_calls_.store(0, std::memory_order_relaxed);
        maintenance_calls_.store(0, std::memory_order_relaxed);
        persistence_calls_.store(0, std::memory_order_relaxed);
        present_gap_over_25ms_.store(0, std::memory_order_relaxed);
        present_gap_over_50ms_.store(0, std::memory_order_relaxed);
        last_publish_ = {};
    }
    if (!Enabled() || logger == nullptr) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_publish_ == std::chrono::steady_clock::time_point{}) {
        last_publish_ = now;
        return;
    }
    if (now - last_publish_ < std::chrono::seconds(2)) return;
    last_publish_ = now;

    static constexpr std::array<std::string_view,
        static_cast<std::size_t>(EmbeddedPerformanceStage::Count)> names{
        "present_interval", "present_lease", "present_render", "present_original", "present_tail",
        "execute_lease", "execute_capture", "execute_original", "render_lock_wait",
        "render_setup", "render_fence_wait", "render_frame_reset", "render_ui",
        "render_prepare_lock_wait", "render_prepare_locked", "render_frame_begin",
        "render_draw_lock_wait", "render_input", "render_platform_ui",
        "render_plugin_draw", "render_capture", "render_finalize",
        "render_commands", "render_submit", "render_total", "game_pump",
        "game_plugin_lock_wait", "game_plugin_update", "game_esc_menu", "game_total",
        "game_tick_interval", "worker_plugin_lock_wait", "worker_plugin_maintenance",
        "worker_retry_services", "worker_poll_changes", "worker_maintenance",
        "worker_persist", "platform_ui_submission_lock_wait",
        "platform_ui_operation_lock_wait", "platform_ui_window_state",
        "platform_ui_refresh_catalog", "platform_ui_runtime_plugins",
        "platform_ui_build_snapshot", "platform_ui_repository_snapshot",
        "platform_ui_service_graph_snapshot", "platform_ui_adapter_services_snapshot",
        "platform_ui_nte_compatibility_snapshot", "platform_ui_model_publish",
        "platform_ui_settings_refresh", "platform_ui_refresh_total",
        "platform_ui_frame_setup", "platform_ui_management_shell", "platform_ui_popups",
        "platform_ui_window_persist"};
    try {
        anomaly::LogDetails details;
        details.thread_domain = anomaly::LogThreadDomain::Worker;
        details.event_id = "performance.embedded";
        details.fields.push_back({
            "present_calls",
            std::to_string(present_calls_.load(std::memory_order_relaxed))});
        details.fields.push_back({
            "execute_calls",
            std::to_string(execute_calls_.load(std::memory_order_relaxed))});
        details.fields.push_back({
            "render_calls",
            std::to_string(render_calls_.load(std::memory_order_relaxed))});
        details.fields.push_back({
            "game_tick_calls",
            std::to_string(game_tick_calls_.load(std::memory_order_relaxed))});
        details.fields.push_back({
            "maintenance_calls",
            std::to_string(maintenance_calls_.load(std::memory_order_relaxed))});
        details.fields.push_back({
            "persistence_calls",
            std::to_string(persistence_calls_.load(std::memory_order_relaxed))});
        details.fields.push_back({
            "present_gap_over_25ms_window",
            std::to_string(present_gap_over_25ms_.exchange(0, std::memory_order_acq_rel))});
        details.fields.push_back({
            "present_gap_over_50ms_window",
            std::to_string(present_gap_over_50ms_.exchange(0, std::memory_order_acq_rel))});
        for (std::size_t index = 0; index < buckets_.size(); ++index) {
            auto& bucket = buckets_[index];
            const std::uint64_t samples =
                bucket.samples.load(std::memory_order_acquire);
            const std::uint64_t total =
                bucket.total_nanoseconds.load(std::memory_order_relaxed);
            const std::uint64_t maximum =
                bucket.maximum_nanoseconds.load(std::memory_order_relaxed);
            const std::uint64_t window_samples =
                bucket.window_samples.exchange(0, std::memory_order_acq_rel);
            const std::uint64_t window_total =
                bucket.window_total_nanoseconds.exchange(0, std::memory_order_acq_rel);
            const std::uint64_t window_maximum =
                bucket.window_maximum_nanoseconds.exchange(0, std::memory_order_acq_rel);
            if (samples == 0) continue;
            const std::string prefix(names[index]);
            details.fields.push_back({prefix + "_samples", std::to_string(samples)});
            details.fields.push_back({
                prefix + "_avg_us",
                std::to_string(static_cast<double>(total) /
                    static_cast<double>(samples) / 1000.0)});
            details.fields.push_back({
                prefix + "_max_us", std::to_string(static_cast<double>(maximum) / 1000.0)});
            details.fields.push_back({
                prefix + "_window_samples", std::to_string(window_samples)});
            details.fields.push_back({
                prefix + "_window_total_us",
                std::to_string(static_cast<double>(window_total) / 1000.0)});
            details.fields.push_back({
                prefix + "_window_avg_us",
                std::to_string(window_samples == 0 ? 0.0 :
                    static_cast<double>(window_total) /
                        static_cast<double>(window_samples) / 1000.0)});
            details.fields.push_back({
                prefix + "_window_max_us",
                std::to_string(static_cast<double>(window_maximum) / 1000.0)});
        }
        static_cast<void>(logger->Log(
            anomaly::LogLevel::Info, "runtime-performance",
            "embedded performance probe snapshot", std::move(details)));
    } catch (...) {
    }
}

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
    const bool requested = g_performance_diagnostics_enabled.load(std::memory_order_relaxed);
    const auto lease_started = requested ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    auto* const state = callback ? g_state.load(std::memory_order_acquire) : nullptr;
    const bool sampled = requested && state != nullptr && state->performance.Enabled();
    if (sampled) state->performance.ObservePresent();
    const auto render_started = sampled ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    static anomaly::ThreadLocalScalar<bool> rendering;
    if (callback && !rendering.Get()) {
        rendering.Set(true);
        RenderEmbedded(swap_chain, flags);
        rendering.Set(false);
    }
    const auto original_started = sampled ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const HRESULT result = g_present(swap_chain, interval, flags);
    const auto tail_started = sampled ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    if (callback) HandlePresentResult(swap_chain, result);
    if (sampled) {
        const auto completed = std::chrono::steady_clock::now();
        state->performance.Record(
            EmbeddedPerformanceStage::PresentLease, render_started - lease_started);
        state->performance.Record(
            EmbeddedPerformanceStage::PresentRender, original_started - render_started);
        state->performance.Record(
            EmbeddedPerformanceStage::PresentOriginal, tail_started - original_started);
        state->performance.Record(
            EmbeddedPerformanceStage::PresentTail, completed - tail_started);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent1(
    IDXGISwapChain1* swap_chain, UINT interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    const bool requested = g_performance_diagnostics_enabled.load(std::memory_order_relaxed);
    const auto lease_started = requested ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    auto* const state = callback ? g_state.load(std::memory_order_acquire) : nullptr;
    const bool sampled = requested && state != nullptr && state->performance.Enabled();
    if (sampled) state->performance.ObservePresent();
    const auto render_started = sampled ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    static anomaly::ThreadLocalScalar<bool> rendering;
    if (callback && !rendering.Get()) {
        rendering.Set(true);
        RenderEmbedded(swap_chain, flags);
        rendering.Set(false);
    }
    const auto original_started = sampled ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const HRESULT result = g_present1(swap_chain, interval, flags, parameters);
    const auto tail_started = sampled ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    if (callback) HandlePresentResult(swap_chain, result);
    if (sampled) {
        const auto completed = std::chrono::steady_clock::now();
        state->performance.Record(
            EmbeddedPerformanceStage::PresentLease, render_started - lease_started);
        state->performance.Record(
            EmbeddedPerformanceStage::PresentRender, original_started - render_started);
        state->performance.Record(
            EmbeddedPerformanceStage::PresentOriginal, tail_started - original_started);
        state->performance.Record(
            EmbeddedPerformanceStage::PresentTail, completed - tail_started);
    }
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
    const bool requested = g_performance_diagnostics_enabled.load(std::memory_order_relaxed);
    const auto lease_started = requested ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    auto callback = g_hooks != nullptr
        ? g_hooks->AcquireCallback(kRendererHookOwner, kRendererHookGeneration)
        : anomaly::PluginScope::CallbackLease{};
    auto* const state = callback ? g_state.load(std::memory_order_acquire) : nullptr;
    const bool sampled = requested && state != nullptr && state->performance.Enabled();
    if (sampled) state->performance.ObserveExecute();
    const auto capture_started = sampled ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    if (callback) CaptureCommandQueue(queue);
    const auto original_started = sampled ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    g_execute_command_lists(queue, count, lists);
    if (sampled) {
        const auto completed = std::chrono::steady_clock::now();
        state->performance.Record(
            EmbeddedPerformanceStage::ExecuteLease, capture_started - lease_started);
        state->performance.Record(
            EmbeddedPerformanceStage::ExecuteCapture, original_started - capture_started);
        state->performance.Record(
            EmbeddedPerformanceStage::ExecuteOriginal, completed - original_started);
    }
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
    state.diagnostics.performance_probe = [&performance = state.performance](
        const PlatformUiPerformanceStage stage,
        const std::chrono::steady_clock::duration elapsed) noexcept {
        static constexpr std::array<EmbeddedPerformanceStage,
            static_cast<std::size_t>(PlatformUiPerformanceStage::Count)> stages{
            EmbeddedPerformanceStage::PlatformUiSubmissionLockWait,
            EmbeddedPerformanceStage::PlatformUiOperationLockWait,
            EmbeddedPerformanceStage::PlatformUiWindowState,
            EmbeddedPerformanceStage::PlatformUiRefreshCatalog,
            EmbeddedPerformanceStage::PlatformUiRuntimePlugins,
            EmbeddedPerformanceStage::PlatformUiBuildSnapshot,
            EmbeddedPerformanceStage::PlatformUiRepositorySnapshot,
            EmbeddedPerformanceStage::PlatformUiServiceGraphSnapshot,
            EmbeddedPerformanceStage::PlatformUiAdapterServicesSnapshot,
            EmbeddedPerformanceStage::PlatformUiNteCompatibilitySnapshot,
            EmbeddedPerformanceStage::PlatformUiModelPublish,
            EmbeddedPerformanceStage::PlatformUiSettingsRefresh,
            EmbeddedPerformanceStage::PlatformUiRefreshTotal,
            EmbeddedPerformanceStage::PlatformUiFrameSetup,
            EmbeddedPerformanceStage::PlatformUiManagementShell,
            EmbeddedPerformanceStage::PlatformUiPopups,
            EmbeddedPerformanceStage::PlatformUiWindowPersist,
        };
        const auto index = static_cast<std::size_t>(stage);
        if (index < stages.size()) performance.Record(stages[index], elapsed);
    };
    state.diagnostics.performance_probe_enabled = [&performance = state.performance]() noexcept {
        return performance.Enabled();
    };
    anomaly::SetHostUiMenusCollapsed(!config.platform_visible);
    state.plugins = plugin_owner.get();
    state.plugin_owner = plugin_owner;
    state.plugins_loaded = true;
    if (g_hooks != nullptr || g_state.load(std::memory_order_acquire) != nullptr) return;
    bool performance_diagnostics_enabled{};
    if (state.diagnostics.settings_snapshot) {
        try {
            const auto settings = state.diagnostics.settings_snapshot();
            performance_diagnostics_enabled = settings.ready &&
                settings.values.advanced_detailed_performance_diagnostics;
        } catch (...) {
        }
    }
    state.performance.SetEnabled(performance_diagnostics_enabled);
    state.plugins->SetPerformanceDiagnosticsEnabled(performance_diagnostics_enabled);
    const auto upstream_settings_apply = state.diagnostics.settings_apply;
    if (upstream_settings_apply) {
        state.diagnostics.settings_apply = [
            upstream_settings_apply,
            performance = &state.performance,
            plugins = std::weak_ptr<PluginManager>(plugin_owner)](
                const anomaly::PlatformSettingsApplyRequest& request) {
            auto result = upstream_settings_apply(request);
            if (result.Applied()) {
                const bool enabled =
                    result.snapshot.values.advanced_detailed_performance_diagnostics;
                performance->SetEnabled(enabled);
                if (const auto active_plugins = plugins.lock()) {
                    active_plugins->SetPerformanceDiagnosticsEnabled(enabled);
                }
            }
            return result;
        };
    }
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
            state.performance.SetEnabled(false);
            if (state.plugins != nullptr) {
                state.plugins->SetPerformanceDiagnosticsEnabled(false);
            }
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
        auto* const performance = &state.performance;
        tick_bound = true;
        adapter->SetTickCallback([
            tick_plugins, plugin_mutex, tick_enabled, game_pump,
            esc_menu_bridge, performance](double delta_seconds) {
            if (!tick_enabled->load(std::memory_order_acquire)) return;
            const bool sampled = performance->SampleGameTick();
            const auto total_started = sampled ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
            if (game_pump) static_cast<void>(game_pump());
            const auto lock_started = sampled ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
            std::unique_lock plugin_lock(*plugin_mutex);
            const auto update_started = sampled ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
            if (tick_enabled->load(std::memory_order_relaxed)) {
                if (tick_plugins != nullptr) tick_plugins->GameUpdate(delta_seconds);
                const auto esc_started = sampled ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
                if (esc_menu_bridge != nullptr && esc_menu_bridge->Started()) {
                    esc_menu_bridge->Update(delta_seconds);
                }
                if (sampled) {
                    const auto completed = std::chrono::steady_clock::now();
                    performance->Record(
                        EmbeddedPerformanceStage::GamePump, lock_started - total_started);
                    performance->Record(
                        EmbeddedPerformanceStage::GamePluginLockWait,
                        update_started - lock_started);
                    performance->Record(
                        EmbeddedPerformanceStage::GamePluginUpdate,
                        esc_started - update_started);
                    performance->Record(
                        EmbeddedPerformanceStage::GameEscMenu, completed - esc_started);
                    performance->Record(
                        EmbeddedPerformanceStage::GameTotal, completed - total_started);
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
        const bool sampled = state.performance.Enabled();
        const auto maintenance_started = sampled ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
        const auto plugin_lock_started = sampled ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
        {
            std::unique_lock plugin_lock(*state.plugin_mutex);
            const auto maintenance_work_started = sampled ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            if (sampled) {
                state.performance.Record(
                    EmbeddedPerformanceStage::WorkerPluginLockWait,
                    maintenance_work_started - plugin_lock_started);
            }
            PluginMaintenanceTiming timing;
            if (state.plugins != nullptr) timing = state.plugins->MaintenancePluginState();
            if (sampled) {
                state.performance.Record(
                    EmbeddedPerformanceStage::WorkerRetryServices,
                    timing.retry_waiting_for_services);
                state.performance.Record(
                    EmbeddedPerformanceStage::WorkerPollChanges,
                    timing.poll_for_changes);
                state.performance.Record(
                    EmbeddedPerformanceStage::WorkerPluginMaintenance,
                    std::chrono::steady_clock::now() - maintenance_work_started);
            }
        }
        if (sampled) {
            state.performance.RecordMaintenance(
                std::chrono::steady_clock::now() - maintenance_started);
        }
        state.performance.Publish(state.diagnostics.logger);
        // Persist through the registry's own synchronization after releasing
        // the renderer gate. The shared owner remains live for this loop, and
        // the filesystem publish must never delay a Present callback.
        const auto persistence_started = sampled ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
        plugin_owner->PersistUiWindowState();
        if (sampled) {
            state.performance.RecordPersistence(
                std::chrono::steady_clock::now() - persistence_started);
        }
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
