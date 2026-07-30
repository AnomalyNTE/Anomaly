#pragma once

#include "anomaly/pattern_service.hpp"
#include "anomaly/hook_manager.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/platform_ui_model.hpp"
#include "anomaly/platform_settings.hpp"
#include "anomaly/repository_coordinator.hpp"
#include <anomaly/sdk/version.h>
#include "anomaly/service_graph.hpp"
#include "config.hpp"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <memory>
#include <stop_token>

namespace anomaly {
class Ue5NteAdapter;
class StructuredLogger;
}

namespace ue5mem {

class PluginManager;

struct PlatformDiagnostics {
    std::shared_ptr<const anomaly::Translator> translator;
    std::string runtime_version{ANOMALY_SDK_VERSION_STRING};
    std::filesystem::path runtime_root;
    std::filesystem::path log_file;
    std::function<anomaly::ServiceGraphSnapshot()> service_graph;
    std::function<std::string()> profile_json;
    std::function<anomaly::NteCompatibilitySnapshot()> nte_compatibility;
    std::function<std::vector<anomaly::HookRecordView>()> hooks;
    std::function<anomaly::RepositoryCoordinatorSnapshot()> repository_snapshot;
    std::function<anomaly::RepositoryOperationSubmission()> repository_refresh;
    std::function<anomaly::RepositoryOperationSubmission(
        std::string_view plugin_id, std::string_view version)> repository_install;
    std::function<anomaly::RepositoryOperationSubmission(
        std::string_view plugin_id)> repository_uninstall;
    std::function<anomaly::PluginRepositoryConfig()> repository_config;
    std::function<anomaly::RepositoryOperationSubmission(
        const anomaly::PluginRepositoryConfig&)> repository_configure;
    std::function<anomaly::PlatformSettingsSnapshot()> settings_snapshot;
    std::function<anomaly::PlatformSettingsApplyResult(
        const anomaly::PlatformSettingsApplyRequest&)> settings_apply;
    std::function<bool(std::string_view route)> settings_record_route;
    // Phase 10 production evidence. The capture provider gates the GPU
    // readback work; the remaining callbacks are cheap no-op observations
    // when the evidence session is idle.
    // Zero means idle; a new non-zero token identifies every capture window.
    std::function<std::uint64_t()> capture_generation;
    std::function<void(
        std::uint64_t capture_generation,
        std::uint32_t thread_id)> render;
    std::function<void(
        std::uint64_t capture_generation,
        std::uint32_t thread_id,
        std::chrono::nanoseconds latency,
        bool success)> resize;
    std::function<void(
        std::uint64_t capture_generation,
        bool non_empty,
        bool success)> pixel_probe;
    // Called by the validated game tick anchor before plugin Update callbacks.
    // This binds and drains the RuntimeDispatchers Game domain on that thread.
    std::function<std::size_t()> game_pump;
    // Mutation work is routed to the lifecycle domain once the production
    // composition root publishes an invoker. Standalone fixtures may leave
    // this empty and use the local fallback executor.
    std::function<std::uint32_t(std::function<void()>)> lifecycle_invoke;
    // Render callbacks use a fire-and-drain submission path so they never
    // synchronously wait for plugin load/reload or lifecycle I/O.
    std::function<std::uint32_t(std::function<void()>)> lifecycle_post;
    // Called during UI teardown after new actions are gated. It waits for
    // lifecycle invocations that may have outlived their bounded caller.
    std::function<bool(std::chrono::milliseconds)> lifecycle_drain;
    std::shared_ptr<anomaly::StructuredLogger> logger;
};

void RunPlatform(
    const std::filesystem::path& root,
    const AnalyzerConfig& config,
    std::stop_token stop_token = {},
    anomaly::CoreMemoryServices memory_services = {},
    std::shared_ptr<anomaly::Ue5NteAdapter> adapter = {},
    PlatformDiagnostics diagnostics = {},
    std::shared_ptr<PluginManager> plugins = {});
void RunEmbeddedPlatform(
    const std::filesystem::path& root,
    const AnalyzerConfig& config,
    std::stop_token stop_token = {},
    anomaly::CoreMemoryServices memory_services = {},
    std::shared_ptr<anomaly::Ue5NteAdapter> adapter = {},
    PlatformDiagnostics diagnostics = {},
    std::shared_ptr<PluginManager> plugins = {});
[[nodiscard]] bool InitializePlatformUi(
    PluginManager& plugins,
    PlatformDiagnostics diagnostics,
    std::shared_ptr<PluginManager> plugin_owner);
// Returns true only when the host-owned management shell changed to open.
[[nodiscard]] bool RevealPlatformUi() noexcept;
// Consumes a game-thread request on the UI thread, restores a closed
// management shell, and expands it. Returns true when a request was consumed.
[[nodiscard]] bool ApplyHostUiManagementExpansionRequest() noexcept;
// Uploads management-shell resources after the plugin scopes are prepared and
// before ImGui begins the next frame.
void PreparePlatformUiResources() noexcept;
void DrawPlatformUi();
// The renderer consumes the active menu key while this is true, but must not
// collapse the surface before the settings recorder accepts that key.
[[nodiscard]] bool PlatformUiCapturingHotkey() noexcept;
// Flushes intents and deferred memory work after the render lock is released.
void FlushPlatformUiActions();
// Returns true only when the active UI owner has drained all callbacks and
// its ImGui context can be destroyed by the caller. A quarantined owner (or a
// shutdown handoff still in progress) returns false; the caller must retain
// the host generation instead of tearing down the context.
[[nodiscard]] bool ShutdownPlatformUi();
// Closes and retires the active owner without waiting for callbacks. The
// owner remains reachable in quarantine while late callbacks release captures.
// A true result means the handoff to quarantine was recorded; it does not
// authorize destruction of the ImGui context or host generation.
[[nodiscard]] bool QuarantinePlatformUi(
    std::chrono::milliseconds wait_timeout = std::chrono::milliseconds(100)) noexcept;
[[nodiscard]] bool PlatformUiQuarantined(const PluginManager* owner = nullptr) noexcept;
[[nodiscard]] bool StandaloneHostQuarantined(const PluginManager* owner = nullptr) noexcept;
[[nodiscard]] bool PlatformHostQuarantined(const PluginManager* owner = nullptr) noexcept;

}  // namespace ue5mem
