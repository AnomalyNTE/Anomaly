#pragma once

#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/plugin_file_watcher.hpp"
#include "anomaly/plugin_enablement.hpp"
#include "anomaly/input_service.hpp"
#include "anomaly/ipc_registry.hpp"
#include "anomaly/nte_esc_menu_button.hpp"
#include "anomaly/plugin_scope.hpp"
#include "anomaly/plugin_shadow_store.hpp"
#include "anomaly/pattern_service.hpp"
#include "anomaly/ui_resource_registry.hpp"
#include "anomaly/ui_resource_render_backend.hpp"

#include <filesystem>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {
class Translator;
class StructuredLogger;
class ScopedPlatformServices;
}

namespace ue5mem {

struct UiResourceWorkerGate;
struct PluginCacheOwnerLease;
struct NteEscMenuButtonRegistry;

struct CallbackMetricsView {
    std::uint64_t calls{};
    std::uint64_t faults{};
    std::uint64_t slow_calls{};
    double p50_milliseconds{};
    double p95_milliseconds{};
    double p99_milliseconds{};
};

struct PluginServiceVersionView {
    std::string id;
    std::uint32_t version{};
};

struct PluginResourceCountsView {
    std::size_t ledger_resources{};
    std::size_t configs{};
    std::size_t self_tests{};
    std::size_t tasks{};
    std::size_t ipc_resources{};
    std::size_t commands{};
    std::size_t notifications{};
    std::size_t hooks{};
    std::size_t patches{};
    std::size_t windows{};
    std::size_t fonts{};
    std::size_t textures{};
    std::size_t hotkeys{};
    std::size_t nte_esc_menu_buttons{};
};

struct PluginScopedCallbackMetricsView {
    std::uint64_t calls{};
    std::uint64_t faults{};
    std::uint64_t slow_calls{};
};

struct PluginPlatformDiagnosticsView {
    bool capability_enforced{};
    std::vector<std::string> capabilities;
    std::vector<PluginServiceVersionView> services;
    PluginResourceCountsView resources;
    std::size_t queued_tasks{};
    PluginScopedCallbackMetricsView scoped_callbacks;
    std::vector<anomaly::IpcEndpointDiagnostics> ipc_endpoints;
    std::vector<std::string> deny_reasons;
};

struct PluginView {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::filesystem::path source;
    std::filesystem::path package_directory;
    bool visible{true};
    bool visibility_control{};
    bool enabled{true};
    std::uint64_t generation{};
    std::string state{"active"};
    std::string status_reason;
    CallbackMetricsView update_metrics;
    CallbackMetricsView draw_metrics;
    PluginPlatformDiagnosticsView platform_diagnostics;
};

struct PluginRuntimeDiagnosticsSnapshot {
    std::uint32_t schema_version{1};
    std::vector<PluginView> plugins;
};

struct PluginCallbackBudgets {
    double update_slow_milliseconds{2.0};
    double draw_slow_milliseconds{4.0};
};

enum class PluginCallbackEvidenceKind : std::uint8_t {
    Update,
    Draw,
};

struct PluginCallbackEvidence {
    PluginCallbackEvidenceKind kind{PluginCallbackEvidenceKind::Update};
    std::string_view plugin_id;
    std::uint64_t generation{};
    std::uint32_t thread_id{};
    double duration_micros{};
    bool fault{};
};

struct PluginStopDiagnostic {
    std::string id;
    std::uint64_t generation{};
    bool drained{};
    bool timed_out{};
    std::size_t in_flight_callbacks{};
    std::size_t resources{};
    std::string reason;
};

// Returns true only after the Worker domain has accepted ownership of the
// callback. A rejected post leaves the resource request failed rather than
// indefinitely queued.
using UiResourceWorkerDispatcher = std::function<bool(
    std::string owner, std::uint64_t generation, std::function<void()> callback)>;
using PluginLoadPredicate = std::function<bool(const anomaly::PluginManifest& manifest)>;
using PluginActivationObserver = std::function<void(
    std::string_view plugin_id, std::uint64_t generation, bool entering)>;

class PluginManager {
public:
    PluginManager(
        std::filesystem::path root,
        std::filesystem::path plugin_directory,
        anomaly::CoreMemoryServices memory_services = {},
        PluginCallbackBudgets callback_budgets = {},
        std::shared_ptr<anomaly::StructuredLogger> logger = {},
        anomaly::HotkeyDispatcher input_dispatcher = {},
        UiResourceWorkerDispatcher ui_resource_worker_dispatcher = {},
        anomaly::IpcPost ipc_post = {},
        PluginLoadPredicate load_predicate = {},
        PluginActivationObserver activation_observer = {});
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void LoadAll();
    void ReloadAll();
    bool Reload(std::string_view plugin_id);
    bool SetEnabled(std::string_view plugin_id, bool enabled);
    void UnloadAll();
    // Stops and unloads every active generation using the supplied host deadline.
    // A false result means at least one generation was quarantined and its module
    // mapping was intentionally retained.
    bool StopForRuntime(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    void SetQueuedCallbackCanceller(
        std::function<void(std::string_view, std::uint64_t)> canceller);
    void SetCallbackEvidenceObserver(
        std::function<void(const PluginCallbackEvidence&)> observer);
    [[nodiscard]] anomaly::PluginScope::CallbackLease AcquireCallback(
        std::string_view plugin_id, std::uint64_t generation) noexcept;
    [[nodiscard]] std::vector<PluginStopDiagnostic> StopDiagnostics() const;
    // Combined worker/lifecycle maintenance for plugin generations and
    // persisted UI window state.
    void Maintenance();
    // Worker/lifecycle maintenance that can mutate plugin generations. The
    // host must serialize this with GameUpdate and Draw.
    void MaintenancePluginState();
    // Worker-only window-state persistence. This uses the registry's own
    // synchronization, but callers must not hold a renderer/plugin gate: it
    // can perform filesystem I/O.
    void PersistUiWindowState();
    // Must be called only from the validated game-thread anchor.
    void GameUpdate(double delta_seconds);
    void Draw(void* imgui_context);
    void SetImGuiContext(void* imgui_context) noexcept;
    void SetUiService(const AnomalyUiServiceV1* service);
    void SetNteEscMenuHostAction(std::function<void()> action) noexcept;
    [[nodiscard]] bool InstallDefaultNteEscMenuButton(
        std::span<const std::uint8_t> png_bytes) noexcept;
    // Must be set before LoadAll; locale and plugin catalogs are frozen per generation.
    void SetTranslator(std::shared_ptr<const anomaly::Translator> translator) noexcept;
    // Render ingress publishes host-normalized state. Persistent input state is
    // deliberately independent from the frame-local UI capture arbitration.
    void PublishInputFrame(
        const anomaly::InputFrameState& frame, anomaly::InputUiCaptureState capture = {});
    void PublishUiCapture(anomaly::InputUiCaptureState capture);
    void ResetInput(anomaly::InputResetReason reason) noexcept;
    void OnUiDeviceLost() noexcept;
    [[nodiscard]] bool OnUiDeviceRebuilt() noexcept;
    // Render infrastructure installs this bridge after an ImGui/D3D generation
    // is ready. The PluginManager never includes backend implementation headers.
    void SetUiResourceRenderBackend(
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend) noexcept;
    void PrepareUiResources() noexcept;
    // Prepares a host-owned texture whose scope is not part of a loaded
    // plugin generation.
    void PrepareUiTexture(
        const std::shared_ptr<anomaly::PluginScope>& scope,
        anomaly::UiResourceHandle handle) noexcept;
    [[nodiscard]] bool PushUiFont(
        const std::shared_ptr<anomaly::PluginScope>& scope,
        anomaly::UiResourceHandle handle) noexcept;
    [[nodiscard]] bool PopUiFont() noexcept;
    [[nodiscard]] bool DrawUiTexture(
        const std::shared_ptr<anomaly::PluginScope>& scope,
        anomaly::UiResourceHandle handle, float width, float height,
        std::uint32_t tint_rgba) noexcept;
    [[nodiscard]] bool QueueUiFontLoad(
        const std::shared_ptr<anomaly::PluginScope>& scope,
        anomaly::UiResourceHandle handle) noexcept;
    [[nodiscard]] bool QueueUiTextureLoad(
        const std::shared_ptr<anomaly::PluginScope>& scope,
        anomaly::UiResourceHandle handle) noexcept;

    [[nodiscard]] std::vector<PluginView> Plugins() const;
    [[nodiscard]] PluginRuntimeDiagnosticsSnapshot DiagnosticsSnapshot() const;
    [[nodiscard]] std::string DiagnosticsJson() const;
    [[nodiscard]] std::vector<std::string> Events() const;
    [[nodiscard]] const std::filesystem::path& Directory() const noexcept { return plugin_directory_; }
    [[nodiscard]] const anomaly::CoreMemoryServices& MemoryServices() const noexcept {
        return memory_services_;
    }
    [[nodiscard]] const AnomalyUiServiceV1* UiService() const noexcept { return ui_service_; }
    [[nodiscard]] anomaly::UiResourceRegistry& UiResources() noexcept { return *ui_resources_; }
    [[nodiscard]] const anomaly::UiResourceRegistry& UiResources() const noexcept {
        return *ui_resources_;
    }
    [[nodiscard]] anomaly::InputService& Input() noexcept { return input_service_; }
    [[nodiscard]] const anomaly::InputService& Input() const noexcept { return input_service_; }
    [[nodiscard]] const PluginCallbackBudgets& CallbackBudgets() const noexcept {
        return callback_budgets_;
    }
    [[nodiscard]] std::filesystem::path PackageDirectory(std::string_view plugin_id) const;
    bool SetVisible(std::string_view plugin_id, bool visible);
    void Log(AnomalyCoreLogLevelV1 level, std::string message);
    void LogPlugin(
        AnomalyCoreLogLevelV1 level,
        std::string message,
        std::string_view plugin_id,
        std::uint64_t generation);

    [[nodiscard]] AnomalyStatusV1 RegisterNteEscMenuButton(
        const std::shared_ptr<anomaly::PluginScope>& scope,
        const AnomalyNteEscMenuButtonSpecV1* spec,
        AnomalyNteEscMenuButtonCallbackV1 callback, void* callback_user,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 UnregisterNteEscMenuButton(
        const std::shared_ptr<anomaly::PluginScope>& scope,
        AnomalyGenerationHandleV1 handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 InvokeNteEscMenuButton(
        AnomalyGenerationHandleV1 handle) noexcept;
    [[nodiscard]] std::vector<anomaly::NteEscMenuButtonSnapshot>
        NteEscMenuButtons() const;

private:
    struct LoadedPlugin;
    [[nodiscard]] bool LoadAllowed(const anomaly::PluginCatalogEntry& entry) const;
    void PublishSuspended(const anomaly::PluginCatalogEntry& entry);
    bool LoadCatalogEntry(const anomaly::PluginCatalogEntry& entry);
    void LogImpl(
        AnomalyCoreLogLevelV1 level,
        std::string message,
        std::string plugin_id,
        std::uint64_t generation);
    bool LoadBinary(
        const std::filesystem::path& source,
        const std::filesystem::path& binary,
        const std::filesystem::path& package_directory,
        anomaly::PluginShadowGeneration shadow_generation);
    bool Activate(LoadedPlugin& plugin);
    void RetryWaitingForAdapterServices();
    void RetryWaitingPlugins();
    bool ReconcileEnablement(const anomaly::PluginCatalogSnapshot& catalog);
    void ReconcileWindowVisibility(LoadedPlugin& plugin) noexcept;
    void SetPersistentPluginWindowVisibility(
        std::string_view plugin_id, bool visible) noexcept;
    void UnloadIndices(
        const std::vector<std::size_t>& indices, bool retire_shadow_generations = true);
    bool UnloadIndicesWithDeadline(
        const std::vector<std::size_t>& indices,
        bool retire_shadow_generations,
        std::chrono::milliseconds timeout);
    [[nodiscard]] bool ReloadPackages(const std::vector<std::string>& package_names);
    void PollForChanges();
    void QueuePackageChanges(std::vector<std::string> package_names) noexcept;
    void LoadPersistentUiWindowState();
    void SavePersistentUiWindowState(bool force = false) noexcept;

    std::filesystem::path root_;
    std::filesystem::path plugin_directory_;
    std::filesystem::path cache_directory_;
    std::unique_ptr<PluginCacheOwnerLease> cache_owner_;
    anomaly::CoreMemoryServices memory_services_;
    PluginCallbackBudgets callback_budgets_;
    std::shared_ptr<anomaly::StructuredLogger> logger_;
    std::shared_ptr<const anomaly::Translator> translator_;
    std::shared_ptr<anomaly::ResourceLedger> lifecycle_ledger_ =
        std::make_shared<anomaly::ResourceLedger>();
    std::unique_ptr<anomaly::ScopedPlatformServices> platform_services_;
    std::unique_ptr<anomaly::IpcRegistry> ipc_registry_;
    std::shared_ptr<NteEscMenuButtonRegistry> nte_esc_menu_buttons_;
    std::shared_ptr<anomaly::PluginScope> host_nte_esc_menu_scope_;
    AnomalyGenerationHandleV1 host_nte_esc_menu_button_{};
    std::vector<std::unique_ptr<LoadedPlugin>> plugins_;
    std::vector<std::unique_ptr<LoadedPlugin>> quarantined_plugins_;
    mutable std::mutex events_mutex_;
    std::vector<std::string> events_;
    std::unordered_map<std::string, PluginView> disabled_plugins_;
    void* imgui_context_{};
    const AnomalyUiServiceV1* ui_service_{};
    std::uint64_t observed_adapter_service_revision_{};
    anomaly::PluginShadowStore shadow_store_;
    anomaly::PluginFileWatcher file_watcher_;
    mutable std::mutex pending_package_changes_mutex_;
    std::vector<std::string> pending_package_changes_;
    anomaly::PluginEnablementStore enablement_store_;
    std::filesystem::path ui_window_state_file_;
    mutable std::mutex ui_window_state_mutex_;
    std::string ui_window_state_last_document_;
    std::chrono::steady_clock::time_point ui_window_state_last_save_{};
    mutable std::mutex plugin_window_visibility_mutex_;
    std::unordered_map<std::string, bool> plugin_window_visibility_;
    std::shared_ptr<anomaly::UiResourceRegistry> ui_resources_ =
        std::make_shared<anomaly::UiResourceRegistry>();
    anomaly::InputService input_service_;
    UiResourceWorkerDispatcher ui_resource_worker_dispatcher_;
    std::shared_ptr<UiResourceWorkerGate> ui_resource_worker_gate_;
    mutable std::mutex ui_resource_backend_mutex_;
    std::shared_ptr<anomaly::UiResourceRenderBackend> ui_resource_render_backend_;
    std::function<void(std::string_view, std::uint64_t)> queued_callback_canceller_;
    std::function<void(const PluginCallbackEvidence&)> callback_evidence_observer_;
    PluginLoadPredicate load_predicate_;
    PluginActivationObserver activation_observer_;
    mutable std::mutex stop_diagnostics_mutex_;
    std::vector<PluginStopDiagnostic> stop_diagnostics_;
};

}  // namespace ue5mem
