#pragma once

#include "anomaly/pattern_service.hpp"
#include "anomaly/plugin_scope.hpp"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace anomaly {

class HookBackend;

struct ScopedPluginServiceOwner final {
    std::shared_ptr<PluginScope> scope;
    std::filesystem::path state_directory;
    std::filesystem::path configuration_directory;
};

struct ScopedPlatformResourceCounts final {
    std::size_t configs{};
    std::size_t self_tests{};
    std::size_t tasks{};
    std::size_t commands{};
    std::size_t notifications{};
    std::size_t hooks{};
    std::size_t patches{};
};

struct ScopedPlatformDiagnosticsView final {
    std::size_t ledger_resources{};
    ScopedPlatformResourceCounts resources;
    std::size_t queued_tasks{};
    std::uint64_t callback_calls{};
    std::uint64_t callback_faults{};
    std::uint64_t slow_callbacks{};
};

enum class ScopedPlatformRevokePhase : std::uint8_t {
    PreStop,
    Final,
};

// Host-owned service implementation for one PluginManager.  The SDK tables are
// per-plugin views, but their records live here so a resource can be revoked by
// the PluginScope ledger before an old DLL generation is released.
class ScopedPlatformServices final {
public:
    class Impl;

    ScopedPlatformServices(
        CoreMemoryServices memory_services,
        std::shared_ptr<ResourceLedger> ledger);
    ScopedPlatformServices(
        CoreMemoryServices memory_services,
        std::shared_ptr<ResourceLedger> ledger,
        std::unique_ptr<HookBackend> hook_backend);
    ~ScopedPlatformServices();

    ScopedPlatformServices(const ScopedPlatformServices&) = delete;
    ScopedPlatformServices& operator=(const ScopedPlatformServices&) = delete;

    [[nodiscard]] AnomalyStatusV1 RegisterConfigSchema(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        std::uint32_t schema_version,
        AnomalyByteSpanV1 schema_json,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 UnregisterConfigSchema(
        const ScopedPluginServiceOwner& owner, AnomalyGenerationHandleV1 handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 ReadConfig(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        std::uint32_t* schema_version,
        AnomalyMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    [[nodiscard]] AnomalyStatusV1 WriteConfig(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        std::uint32_t schema_version,
        AnomalyByteSpanV1 document) noexcept;
    [[nodiscard]] AnomalyStatusV1 MigrateConfig(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        AnomalyConfigMigrationV1 migration,
        void* migration_user) noexcept;

    [[nodiscard]] AnomalyStatusV1 ReadStorage(
        const ScopedPluginServiceOwner& owner,
        std::string_view relative_path,
        AnomalyMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    [[nodiscard]] AnomalyStatusV1 WriteStorage(
        const ScopedPluginServiceOwner& owner,
        std::string_view relative_path,
        AnomalyByteSpanV1 source) noexcept;
    [[nodiscard]] AnomalyStatusV1 RemoveStorage(
        const ScopedPluginServiceOwner& owner, std::string_view relative_path) noexcept;

    [[nodiscard]] AnomalyStatusV1 RuntimeInfo(
        const ScopedPluginServiceOwner& owner, AnomalyRuntimeInfoV1* snapshot) noexcept;
    [[nodiscard]] AnomalyStatusV1 RuntimeVersion(
        char* destination, std::size_t* inout_size) noexcept;

    [[nodiscard]] AnomalyStatusV1 RegisterSelfTest(
        const ScopedPluginServiceOwner& owner,
        std::string_view id,
        AnomalyDiagnosticSelfTestV1 callback,
        void* callback_user,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 UnregisterSelfTest(
        const ScopedPluginServiceOwner& owner, AnomalyGenerationHandleV1 handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 RunSelfTest(
        const ScopedPluginServiceOwner& owner,
        std::string_view id,
        AnomalyMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    [[nodiscard]] AnomalyStatusV1 DiagnosticsSnapshot(
        const ScopedPluginServiceOwner& owner,
        AnomalyMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    // Host-only typed view used by the manager, pipe, and UI. It never calls
    // plugin code and is safe to collect while worker callbacks are queued.
    [[nodiscard]] ScopedPlatformDiagnosticsView Snapshot(
        const ScopedPluginServiceOwner& owner) const noexcept;

    [[nodiscard]] AnomalyStatusV1 Schedule(
        const ScopedPluginServiceOwner& owner,
        std::uint32_t delay_milliseconds,
        AnomalyTaskCallbackV1 callback,
        void* callback_user,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 CancelTask(
        const ScopedPluginServiceOwner& owner, AnomalyGenerationHandleV1 handle) noexcept;

    [[nodiscard]] AnomalyStatusV1 RegisterCommand(
        const ScopedPluginServiceOwner& owner,
        std::string_view name,
        std::string_view description,
        AnomalyCommandCallbackV1 callback,
        void* callback_user,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 UnregisterCommand(
        const ScopedPluginServiceOwner& owner, AnomalyGenerationHandleV1 handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 InvokeCommand(
        const ScopedPluginServiceOwner& owner,
        std::string_view name,
        std::string_view arguments,
        AnomalyMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;

    [[nodiscard]] AnomalyStatusV1 PostNotification(
        const ScopedPluginServiceOwner& owner,
        AnomalyNotificationSeverityV1 severity,
        std::string_view title,
        std::string_view body,
        std::uint32_t timeout_milliseconds,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 DismissNotification(
        const ScopedPluginServiceOwner& owner, AnomalyGenerationHandleV1 handle) noexcept;

    [[nodiscard]] AnomalyStatusV1 ResolveSignature(
        const ScopedPluginServiceOwner& owner,
        std::string_view module_name,
        std::string_view section_name,
        std::string_view pattern,
        std::uintptr_t* address) noexcept;
    [[nodiscard]] AnomalyStatusV1 CreateHook(
        const ScopedPluginServiceOwner& owner,
        const AnomalyHookRequestV1* request,
        std::uintptr_t* original,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 ReleaseHook(
        const ScopedPluginServiceOwner& owner, AnomalyGenerationHandleV1 handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 BeginHookCallback(
        const ScopedPluginServiceOwner& owner,
        AnomalyGenerationHandleV1 hook,
        AnomalyGenerationHandleV1* callback_lease) noexcept;
    [[nodiscard]] AnomalyStatusV1 EndHookCallback(
        const ScopedPluginServiceOwner& owner,
        AnomalyGenerationHandleV1 callback_lease) noexcept;
    [[nodiscard]] AnomalyStatusV1 ApplyPatch(
        const ScopedPluginServiceOwner& owner,
        std::uintptr_t address,
        AnomalyByteSpanV1 replacement,
        std::string_view label,
        AnomalyGenerationHandleV1* handle) noexcept;
    [[nodiscard]] AnomalyStatusV1 ReleasePatch(
        const ScopedPluginServiceOwner& owner, AnomalyGenerationHandleV1 handle) noexcept;

    // Stops tracked hooks before releasing their outer scope records. PreStop
    // retains Config schemas so an on_stop lifecycle callback can persist its
    // final settings; Final revokes every remaining scoped resource before
    // on_unload. A failed hook drain/removal leaves its record intact so the
    // caller can quarantine the generation instead of unmapping its DLL.
    [[nodiscard]] bool RevokeScope(
        const ScopedPluginServiceOwner& owner,
        std::chrono::steady_clock::time_point deadline,
        ScopedPlatformRevokePhase phase = ScopedPlatformRevokePhase::Final) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
