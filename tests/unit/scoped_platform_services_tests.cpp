#include "anomaly/hook_manager.hpp"
#include "anomaly/pattern_service.hpp"
#include "anomaly/plugin_scope.hpp"
#include "anomaly/scoped_platform_services.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::atomic_uint32_t g_task_calls{};
std::atomic_uint32_t g_preempted_task_calls{};
std::atomic_uint32_t g_hook_calls{};
std::atomic_uint32_t g_second_hook_calls{};
anomaly::ScopedPlatformServices* g_services{};
anomaly::ScopedPluginServiceOwner g_owner;
AnomalyGenerationHandleV1 g_hook_handle{};
AnomalyGenerationHandleV1 g_second_hook_handle{};
using HookTargetFn = int(ANOMALY_CALL *)(int);
HookTargetFn g_hook_original{};
HookTargetFn g_second_hook_original{};

struct PublishingBackendState final {
    std::uintptr_t* original{};
    AnomalyGenerationHandleV1* handle{};
    std::uintptr_t trampoline{};
    std::uint64_t generation{};
    bool observed_published_outputs{};
};

class PublishingBackend final : public anomaly::HookBackend {
public:
    explicit PublishingBackend(std::shared_ptr<PublishingBackendState> state)
        : state_(std::move(state)) {}

    bool Initialize() noexcept override { return true; }
    void Uninitialize() noexcept override {}
    bool Create(void*, void*, void** original) noexcept override {
        *original = reinterpret_cast<void*>(state_->trampoline);
        return true;
    }
    bool Enable(void*) noexcept override {
        state_->observed_published_outputs =
            state_->original != nullptr && *state_->original == state_->trampoline &&
            state_->handle != nullptr && state_->handle->id != 0 &&
            state_->handle->generation == state_->generation;
        return true;
    }
    bool Disable(void*) noexcept override { return true; }
    bool Remove(void*) noexcept override { return true; }

private:
    std::shared_ptr<PublishingBackendState> state_;
};

bool Check(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

AnomalyStatusV1 Status(const std::uint32_t code) {
    return {code, 0, {nullptr, 0}};
}

AnomalyStringViewV1 View(const std::string_view value) {
    return {value.data(), value.size()};
}

bool WaitFor(
    const std::function<bool()>& predicate,
    const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        Sleep(10);
    }
    return predicate();
}

AnomalyStatusV1 ANOMALY_CALL Migrate(
    void*, std::uint32_t, AnomalyByteSpanV1,
    AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    constexpr std::string_view kDocument = "{\"enabled\":false}";
    if (inout_size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (destination.data == nullptr || destination.size < kDocument.size() ||
        *inout_size < kDocument.size()) {
        *inout_size = kDocument.size();
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    std::memcpy(destination.data, kDocument.data(), kDocument.size());
    *inout_size = kDocument.size();
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL SelfTest(
    void*, AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    constexpr std::string_view kResult = "ok";
    if (inout_size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (destination.data == nullptr || destination.size < kResult.size() ||
        *inout_size < kResult.size()) {
        *inout_size = kResult.size();
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    std::memcpy(destination.data, kResult.data(), kResult.size());
    *inout_size = kResult.size();
    return Status(ANOMALY_STATUS_V1_OK);
}

void ANOMALY_CALL Task(void*, AnomalyGenerationHandleV1) {
    ++g_task_calls;
}

void ANOMALY_CALL PreemptedTask(void*, AnomalyGenerationHandleV1) {
    ++g_preempted_task_calls;
}

AnomalyStatusV1 ANOMALY_CALL Command(
    void*, AnomalyStringViewV1 arguments,
    AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    if (arguments.size != 4 || std::string_view(arguments.data, arguments.size) != "ping") {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    constexpr std::string_view kResult = "pong";
    if (inout_size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (destination.data == nullptr || destination.size < kResult.size() ||
        *inout_size < kResult.size()) {
        *inout_size = kResult.size();
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    std::memcpy(destination.data, kResult.data(), kResult.size());
    *inout_size = kResult.size();
    return Status(ANOMALY_STATUS_V1_OK);
}

__declspec(noinline) int ANOMALY_CALL HookTarget(const int value) {
    return value + 1;
}

__declspec(noinline) int ANOMALY_CALL SecondHookTarget(const int value) {
    return value + 2;
}

__declspec(noinline) int ANOMALY_CALL HookDetour(const int value) {
    AnomalyGenerationHandleV1 lease{};
    if (g_services != nullptr &&
        g_services->BeginHookCallback(g_owner, g_hook_handle, &lease).code ==
            ANOMALY_STATUS_V1_OK) {
        ++g_hook_calls;
        static_cast<void>(g_services->EndHookCallback(g_owner, lease));
    }
    return g_hook_original == nullptr ? value : g_hook_original(value) + 1;
}

__declspec(noinline) int ANOMALY_CALL SecondHookDetour(const int value) {
    AnomalyGenerationHandleV1 lease{};
    if (g_services != nullptr &&
        g_services->BeginHookCallback(g_owner, g_second_hook_handle, &lease).code ==
            ANOMALY_STATUS_V1_OK) {
        ++g_second_hook_calls;
        static_cast<void>(g_services->EndHookCallback(g_owner, lease));
    }
    return g_second_hook_original == nullptr ? value : g_second_hook_original(value) + 2;
}

bool TestHookOutputsPublishedBeforeEnable() {
    const auto memory_services = anomaly::CreateCoreMemoryServices();
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.hook-publication", 73);
    const anomaly::ScopedPluginServiceOwner owner{scope, {}, {}};
    auto backend_state = std::make_shared<PublishingBackendState>();
    anomaly::ScopedPlatformServices services(
        memory_services, ledger, std::make_unique<PublishingBackend>(backend_state));

    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = reinterpret_cast<std::uintptr_t>(&HookTarget);
    request.detour = reinterpret_cast<void*>(&HookDetour);
    request.label = View("publication-order");
    std::uintptr_t original{};
    AnomalyGenerationHandleV1 handle{};
    backend_state->original = &original;
    backend_state->handle = &handle;
    backend_state->trampoline = request.target;
    backend_state->generation = scope->Generation();

    const auto created = services.CreateHook(owner, &request, &original, &handle);
    return Check(
        created.code == ANOMALY_STATUS_V1_OK && backend_state->observed_published_outputs &&
            services.ReleaseHook(owner, handle).code == ANOMALY_STATUS_V1_OK,
        "hook outputs were not published before the backend enabled the target");
}

bool TestScopedPlatformServices() {
    const auto memory_services = anomaly::CreateCoreMemoryServices();
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.scoped-platform", 41);
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("anomaly-scoped-platform-" + std::to_string(GetCurrentProcessId()));
    const std::filesystem::path state_root = root / L"state";
    const std::filesystem::path configuration_root = root / L"config";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const anomaly::ScopedPluginServiceOwner owner{scope, state_root, configuration_root};
    anomaly::ScopedPlatformServices services(memory_services, ledger);
    g_services = &services;
    g_owner = owner;

    constexpr std::string_view kSchema =
        "{\"type\":\"object\",\"required\":[\"enabled\"],\"properties\":{\"enabled\":{\"type\":\"boolean\"}}}";
    constexpr std::string_view kInvalidSchema =
        "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\",\"pattern\":\"[\"}}}";
    AnomalyGenerationHandleV1 invalid_config{};
    bool result = Check(
        services.RegisterConfigSchema(owner, "invalid", 1,
            {reinterpret_cast<const std::uint8_t*>(kInvalidSchema.data()), kInvalidSchema.size()},
            &invalid_config).code == ANOMALY_STATUS_V1_FAILED && invalid_config.id == 0 &&
            scope->Resources().empty() && services.Snapshot(owner).resources.configs == 0,
        "invalid configuration schema was accepted or leaked a scope resource");
    AnomalyGenerationHandleV1 mixed_case_config{};
    result = Check(
                 services.RegisterConfigSchema(owner, "Settings", 1,
                     {reinterpret_cast<const std::uint8_t*>(kSchema.data()), kSchema.size()},
                     &mixed_case_config).code == ANOMALY_STATUS_V1_OK &&
                     mixed_case_config.id != 0 &&
                     services.UnregisterConfigSchema(owner, mixed_case_config).code ==
                         ANOMALY_STATUS_V1_OK &&
                     scope->Resources().empty(),
                 "existing mixed-case configuration schema id was rejected") && result;

    const auto unregistered_scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.unregistered-config", 42);
    const std::filesystem::path unregistered_state_root = root / L"unregistered-state";
    const std::filesystem::path unregistered_configuration_root = root / L"unregistered-config";
    const anomaly::ScopedPluginServiceOwner unregistered_owner{
        unregistered_scope, unregistered_state_root, unregistered_configuration_root};
    constexpr std::string_view kEnabled = "{\"enabled\":true}";
    std::error_code unregistered_error;
    result = Check(
                 services.WriteConfig(unregistered_owner, "settings", 1,
                     {reinterpret_cast<const std::uint8_t*>(kEnabled.data()), kEnabled.size()}).code ==
                         ANOMALY_STATUS_V1_NOT_FOUND &&
                     !std::filesystem::exists(unregistered_state_root, unregistered_error) &&
                     !std::filesystem::exists(unregistered_configuration_root, unregistered_error) &&
                     !unregistered_error,
                 "unregistered configuration write created a managed directory") && result;

    AnomalyGenerationHandleV1 config{};
    result = Check(
        services.RegisterConfigSchema(owner, "settings", 1,
            {reinterpret_cast<const std::uint8_t*>(kSchema.data()), kSchema.size()}, &config).code ==
            ANOMALY_STATUS_V1_OK,
        "config schema registration failed");
    constexpr std::size_t kResourcesAfterConfigRegistration = 1;
    AnomalyGenerationHandleV1 duplicate_config{};
    result = Check(
                 services.RegisterConfigSchema(owner, "settings", 1,
                     {reinterpret_cast<const std::uint8_t*>(kSchema.data()), kSchema.size()},
                     &duplicate_config).code == ANOMALY_STATUS_V1_CONFLICT &&
                     duplicate_config.id == 0 &&
                     scope->Resources().size() == kResourcesAfterConfigRegistration &&
                     services.Snapshot(owner).resources.configs == 1,
                 "duplicate configuration schema leaked a scope resource") && result;

    constexpr std::string_view kConcurrentSchemaId = "parallel-settings";
    std::array<AnomalyStatusV1, 2> concurrent_status{};
    std::array<AnomalyGenerationHandleV1, 2> concurrent_handles{};
    std::atomic_uint32_t concurrent_ready{};
    std::atomic_bool start_concurrent_registration{};
    std::array<std::thread, 2> concurrent_registration;
    for (std::size_t index{}; index < concurrent_registration.size(); ++index) {
        concurrent_registration[index] = std::thread([&, index] {
            concurrent_ready.fetch_add(1, std::memory_order_release);
            while (!start_concurrent_registration.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            concurrent_status[index] = services.RegisterConfigSchema(owner, kConcurrentSchemaId, 1,
                {reinterpret_cast<const std::uint8_t*>(kSchema.data()), kSchema.size()},
                &concurrent_handles[index]);
        });
    }
    while (concurrent_ready.load(std::memory_order_acquire) != concurrent_registration.size()) {
        std::this_thread::yield();
    }
    start_concurrent_registration.store(true, std::memory_order_release);
    for (auto& thread : concurrent_registration) thread.join();
    const std::size_t concurrent_successes = static_cast<std::size_t>(std::count_if(
        concurrent_status.begin(), concurrent_status.end(), [](const AnomalyStatusV1 status) {
            return status.code == ANOMALY_STATUS_V1_OK;
        }));
    const std::size_t concurrent_conflicts = static_cast<std::size_t>(std::count_if(
        concurrent_status.begin(), concurrent_status.end(), [](const AnomalyStatusV1 status) {
            return status.code == ANOMALY_STATUS_V1_CONFLICT;
        }));
    const auto successful_concurrent_handle = std::find_if(
        concurrent_handles.begin(), concurrent_handles.end(), [](const AnomalyGenerationHandleV1 handle) {
            return handle.id != 0;
        });
    result = Check(
                 concurrent_successes == 1 && concurrent_conflicts == 1 &&
                     successful_concurrent_handle != concurrent_handles.end() &&
                     scope->Resources().size() == kResourcesAfterConfigRegistration + 1 &&
                     services.Snapshot(owner).resources.configs == 2 &&
                     services.UnregisterConfigSchema(owner, *successful_concurrent_handle).code ==
                         ANOMALY_STATUS_V1_OK &&
                     scope->Resources().size() == kResourcesAfterConfigRegistration &&
                     services.Snapshot(owner).resources.configs == 1,
                 "concurrent configuration registration did not leave one scoped schema") && result;
    result = Check(
                 services.WriteConfig(owner, "settings", 1,
                     {reinterpret_cast<const std::uint8_t*>(kEnabled.data()), kEnabled.size()}).code ==
                         ANOMALY_STATUS_V1_OK &&
                     std::filesystem::exists(configuration_root / L"config-settings.json") &&
                     !std::filesystem::exists(state_root / L"config-settings.json"),
                 "validated config write did not use the configuration root") && result;
    constexpr std::string_view kInvalid = "{\"enabled\":1}";
    result = Check(
                 services.WriteConfig(owner, "settings", 1,
                     {reinterpret_cast<const std::uint8_t*>(kInvalid.data()), kInvalid.size()}).code ==
                     ANOMALY_STATUS_V1_FAILED,
                 "invalid config document was accepted") && result;
    std::uint32_t version{};
    std::size_t config_size{};
    result = Check(
                 services.ReadConfig(owner, "settings", &version, {nullptr, 0}, &config_size).code ==
                     ANOMALY_STATUS_V1_OK && version == 1 && config_size == kEnabled.size(),
                 "config read size query failed") && result;
    std::vector<std::uint8_t> config_bytes(config_size);
    result = Check(
                 services.ReadConfig(owner, "settings", &version,
                     {config_bytes.data(), config_bytes.size()}, &config_size).code ==
                     ANOMALY_STATUS_V1_OK &&
                     std::string_view(reinterpret_cast<const char*>(config_bytes.data()), config_size) ==
                         kEnabled,
                 "config read did not preserve the document") && result;
    result = Check(
                 services.UnregisterConfigSchema(owner, config).code == ANOMALY_STATUS_V1_OK,
                 "config schema unregistration failed") && result;
    result = Check(
                 services.RegisterConfigSchema(owner, "settings", 2,
                     {reinterpret_cast<const std::uint8_t*>(kSchema.data()), kSchema.size()}, &config).code ==
                     ANOMALY_STATUS_V1_OK &&
                     services.MigrateConfig(owner, "settings", Migrate, nullptr).code ==
                         ANOMALY_STATUS_V1_OK,
                 "config migration failed") && result;
    result = Check(
                 services.ReadConfig(owner, "settings", &version, {nullptr, 0}, &config_size).code ==
                     ANOMALY_STATUS_V1_OK && version == 2,
                 "config migration did not persist the new schema version") && result;

    constexpr std::size_t kMaximumConfigDocumentBytes = 1024U * 1024U;
    std::string maximum_document = "{\"enabled\":true,\"payload\":\"";
    maximum_document.append(
        kMaximumConfigDocumentBytes - maximum_document.size() - 2U, 'x');
    maximum_document += "\"}";
    config_size = 0;
    result = Check(
                 maximum_document.size() == kMaximumConfigDocumentBytes &&
                     services.WriteConfig(owner, "settings", 2,
                         {reinterpret_cast<const std::uint8_t*>(maximum_document.data()),
                             maximum_document.size()}).code == ANOMALY_STATUS_V1_OK &&
                     services.ReadConfig(owner, "settings", &version, {nullptr, 0}, &config_size).code ==
                         ANOMALY_STATUS_V1_OK &&
                     version == 2 && config_size == maximum_document.size(),
                 "maximum-sized configuration document did not survive its JSON envelope") && result;
    std::vector<std::uint8_t> maximum_document_copy(config_size);
    result = Check(
                 services.ReadConfig(owner, "settings", &version,
                     {maximum_document_copy.data(), maximum_document_copy.size()}, &config_size).code ==
                         ANOMALY_STATUS_V1_OK &&
                     std::string_view(
                         reinterpret_cast<const char*>(maximum_document_copy.data()), config_size) ==
                         maximum_document,
                 "maximum-sized configuration document did not round-trip") && result;

    std::string expanded_document = "{\"enabled\":true,\"payload\":[";
    constexpr std::size_t kExpandedNumberCount = 210000;
    for (std::size_t index{}; index < kExpandedNumberCount; ++index) {
        if (index != 0) expanded_document.push_back(',');
        expanded_document += "1e1";
    }
    expanded_document += "]}";
    result = Check(
                 expanded_document.size() <= kMaximumConfigDocumentBytes &&
                     services.WriteConfig(owner, "settings", 2,
                         {reinterpret_cast<const std::uint8_t*>(expanded_document.data()),
                             expanded_document.size()}).code == ANOMALY_STATUS_V1_FAILED,
                 "noncanonical configuration expanded beyond the persisted size limit") && result;

    AnomalyRuntimeInfoV1 runtime_info{};
    runtime_info.struct_size = sizeof(runtime_info);
    std::array<char, 32> runtime_version{};
    std::size_t runtime_version_size = runtime_version.size();
    result = Check(
                 services.RuntimeInfo(owner, &runtime_info).code == ANOMALY_STATUS_V1_OK &&
                     runtime_info.plugin_generation == 41 && runtime_info.process_id == GetCurrentProcessId() &&
                     services.RuntimeVersion(runtime_version.data(), &runtime_version_size).code ==
                         ANOMALY_STATUS_V1_OK &&
                     runtime_version_size > 1,
                 "runtime info service did not return the scoped runtime identity") && result;

    constexpr std::array<std::uint8_t, 3> kStorage{{1, 2, 3}};
    result = Check(
                 services.WriteStorage(owner, "cache.bin", {kStorage.data(), kStorage.size()}).code ==
                     ANOMALY_STATUS_V1_OK,
                 "storage write failed") && result;
    std::array<std::uint8_t, 3> storage{};
    std::size_t storage_size = storage.size();
    result = Check(
                 services.ReadStorage(owner, "cache.bin", {storage.data(), storage.size()}, &storage_size).code ==
                     ANOMALY_STATUS_V1_OK && storage == kStorage,
                 "storage read failed") && result;
    result = Check(
                 services.WriteStorage(owner, "..\\outside.bin", {kStorage.data(), kStorage.size()}).code ==
                     ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                 "storage traversal was accepted") && result;

    AnomalyGenerationHandleV1 self_test{};
    result = Check(
                 services.RegisterSelfTest(owner, "health", SelfTest, nullptr, &self_test).code ==
                     ANOMALY_STATUS_V1_OK &&
                     services.RegisterSelfTest(owner, "health", SelfTest, nullptr, &self_test).code ==
                         ANOMALY_STATUS_V1_CONFLICT,
                 "self-test duplicate handling failed") && result;
    std::array<std::uint8_t, 8> test_output{};
    std::size_t test_size = test_output.size();
    result = Check(
                 services.RunSelfTest(owner, "health", {test_output.data(), test_output.size()}, &test_size).code ==
                     ANOMALY_STATUS_V1_OK &&
                     std::string_view(reinterpret_cast<const char*>(test_output.data()), test_size) == "ok",
                 "self-test execution failed") && result;

    AnomalyGenerationHandleV1 task{};
    result = Check(
                 services.Schedule(owner, 1, Task, nullptr, &task).code == ANOMALY_STATUS_V1_OK &&
                     WaitFor([] { return g_task_calls.load() == 1; }),
                 "scheduled task did not execute") && result;
    AnomalyGenerationHandleV1 cancelled_task{};
    result = Check(
                 services.Schedule(owner, 1000, Task, nullptr, &cancelled_task).code == ANOMALY_STATUS_V1_OK,
                 "delayed task scheduling failed") && result;
    const auto queued_snapshot = services.Snapshot(owner);
    result = Check(
                 queued_snapshot.ledger_resources >= 3 &&
                     queued_snapshot.resources.tasks == 1 && queued_snapshot.queued_tasks == 1,
                 "typed scoped diagnostics did not report the queued task") && result;
    result = Check(
                 services.CancelTask(owner, cancelled_task).code == ANOMALY_STATUS_V1_CANCELLED &&
                     services.Snapshot(owner).queued_tasks == 0,
                 "scheduled task cancellation did not report a typed cancellation") && result;
    g_preempted_task_calls.store(0);
    AnomalyGenerationHandleV1 parked_task{};
    AnomalyGenerationHandleV1 immediate_task{};
    const bool parked_scheduled = services.Schedule(
        owner, 5000, Task, nullptr, &parked_task).code == ANOMALY_STATUS_V1_OK;
    Sleep(25);
    const bool immediate_scheduled = services.Schedule(
        owner, 0, PreemptedTask, nullptr, &immediate_task).code == ANOMALY_STATUS_V1_OK;
    const bool immediate_executed = immediate_scheduled && WaitFor(
        [] { return g_preempted_task_calls.load() == 1; }, 300ms);
    const bool parked_cancelled = parked_scheduled &&
        services.CancelTask(owner, parked_task).code == ANOMALY_STATUS_V1_CANCELLED;
    result = Check(
                 parked_scheduled && immediate_executed && parked_cancelled,
                 "scheduler did not wake for an earlier queued task") && result;
    const auto callback_snapshot = services.Snapshot(owner);
    result = Check(
                 callback_snapshot.callback_calls >= 1 &&
                     callback_snapshot.callback_faults == 0,
                 "typed scoped diagnostics did not retain callback and resource metrics") && result;

    AnomalyGenerationHandleV1 command{};
    result = Check(
                 services.RegisterCommand(owner, "ping", "test command", Command, nullptr, &command).code ==
                     ANOMALY_STATUS_V1_OK,
                 "command registration failed") && result;
    std::array<std::uint8_t, 8> command_output{};
    std::size_t command_size = command_output.size();
    result = Check(
                 services.InvokeCommand(owner, "ping", "ping",
                     {command_output.data(), command_output.size()}, &command_size).code ==
                     ANOMALY_STATUS_V1_OK &&
                     std::string_view(
                         reinterpret_cast<const char*>(command_output.data()), command_size) == "pong",
                 "command invocation failed") && result;
    AnomalyGenerationHandleV1 notification{};
    result = Check(
                 services.PostNotification(owner, ANOMALY_NOTIFICATION_V1_INFO,
                     "Scoped services", "active", 0, &notification).code == ANOMALY_STATUS_V1_OK &&
                     services.DismissNotification(owner, notification).code == ANOMALY_STATUS_V1_OK,
                 "notification lifecycle failed") && result;
    std::size_t diagnostics_size{};
    result = Check(
                 services.DiagnosticsSnapshot(owner, {nullptr, 0}, &diagnostics_size).code ==
                     ANOMALY_STATUS_V1_OK && diagnostics_size != 0,
                 "diagnostics snapshot size query failed") && result;
    std::vector<std::uint8_t> diagnostics(diagnostics_size);
    result = Check(
                 services.DiagnosticsSnapshot(
                     owner, {diagnostics.data(), diagnostics.size()}, &diagnostics_size).code ==
                     ANOMALY_STATUS_V1_OK &&
                     std::string_view(
                         reinterpret_cast<const char*>(diagnostics.data()), diagnostics_size).find(
                         "anomaly.test.scoped-platform") != std::string_view::npos,
                 "diagnostics snapshot omitted the scoped plugin identity") && result;

    auto* patch_target = static_cast<std::uint8_t*>(memory_services.memory->AllocateMemory(4096));
    if (!Check(patch_target != nullptr, "patch allocation failed")) return false;
    patch_target[0] = 0x10;
    patch_target[1] = 0x20;
    constexpr std::array<std::uint8_t, 2> kReplacement{{0xaa, 0xbb}};
    AnomalyGenerationHandleV1 patch{};
    result = Check(
                 services.ApplyPatch(owner, reinterpret_cast<std::uintptr_t>(patch_target),
                     {kReplacement.data(), kReplacement.size()}, "test.patch", &patch).code ==
                     ANOMALY_STATUS_V1_OK && patch_target[0] == 0xaa && patch_target[1] == 0xbb &&
                     services.ReleasePatch(owner, patch).code == ANOMALY_STATUS_V1_OK &&
                     patch_target[0] == 0x10 && patch_target[1] == 0x20,
                 "tracked patch release did not restore bytes") && result;
    result = Check(
                 services.ApplyPatch(owner, reinterpret_cast<std::uintptr_t>(patch_target),
                     {kReplacement.data(), kReplacement.size()}, "test.patch", &patch).code ==
                     ANOMALY_STATUS_V1_OK,
                 "second tracked patch failed") && result;
    const auto stale_scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.scoped-platform", 42);
    const anomaly::ScopedPluginServiceOwner stale_owner{
        stale_scope, state_root, configuration_root};
    result = Check(
                 services.ReleasePatch(stale_owner, patch).code == ANOMALY_STATUS_V1_NOT_FOUND,
                 "old generation could release a current patch") && result;

    AnomalyHookRequestV1 hook_request{};
    hook_request.struct_size = sizeof(hook_request);
    hook_request.kind = ANOMALY_HOOK_V1_FUNCTION;
    hook_request.target = reinterpret_cast<std::uintptr_t>(&HookTarget);
    hook_request.detour = reinterpret_cast<void*>(&HookDetour);
    hook_request.label = View("test-hook");
    std::uintptr_t original{};
    result = Check(
                 services.CreateHook(owner, &hook_request, &original, &g_hook_handle).code ==
                     ANOMALY_STATUS_V1_OK && original != 0,
                 "function hook creation failed") && result;
    AnomalyHookRequestV1 second_hook = hook_request;
    second_hook.target = reinterpret_cast<std::uintptr_t>(&SecondHookTarget);
    second_hook.detour = reinterpret_cast<void*>(&SecondHookDetour);
    second_hook.label = View("test-second-hook");
    std::uintptr_t second_original{};
    result = Check(
                 services.CreateHook(owner, &second_hook, &second_original, &g_second_hook_handle).code ==
                         ANOMALY_STATUS_V1_OK &&
                     second_original != 0,
                 "second function hook creation failed") && result;
    AnomalyHookRequestV1 duplicate_hook = hook_request;
    duplicate_hook.label = View("duplicate-hook");
    AnomalyGenerationHandleV1 duplicate_hook_handle{};
    result = Check(
                 services.CreateHook(owner, &duplicate_hook, &original, &duplicate_hook_handle).code ==
                     ANOMALY_STATUS_V1_CONFLICT,
                 "duplicate function target was not rejected") && result;
    g_hook_original = reinterpret_cast<HookTargetFn>(original);
    g_second_hook_original = reinterpret_cast<HookTargetFn>(second_original);
    HookTargetFn hooked_target = &HookTarget;
    HookTargetFn second_hooked_target = &SecondHookTarget;
    result = Check(
                 hooked_target(4) == 6 && g_hook_calls.load() == 1 &&
                 second_hooked_target(4) == 8 && g_second_hook_calls.load() == 1 &&
                 services.ReleaseHook(owner, g_hook_handle).code == ANOMALY_STATUS_V1_OK &&
                 hooked_target(4) == 5 && second_hooked_target(4) == 8 &&
                     g_second_hook_calls.load() == 2,
                 "multiple function hook lease contract failed") && result;

    AnomalyGenerationHandleV1 held_hook_callback{};
    const auto callbacks_before_hook_lease = scope->InFlightCallbacks();
    const auto begin_held_hook_callback =
        services.BeginHookCallback(owner, g_second_hook_handle, &held_hook_callback);
    bool scope_lease_held{};
    bool failed_revocation_retained_hook{};
    bool stop_waits_for_hook_callback{};
    AnomalyStatusV1 end_held_hook_callback = Status(ANOMALY_STATUS_V1_FAILED);
    bool scope_drained{};
    if (begin_held_hook_callback.code == ANOMALY_STATUS_V1_OK) {
        scope_lease_held = scope->InFlightCallbacks() == callbacks_before_hook_lease + 1;
        const std::size_t resources_before_failed_revocation = scope->Resources().size();
        failed_revocation_retained_hook =
            !services.RevokeScope(owner, std::chrono::steady_clock::now()) &&
            scope->Resources().size() == resources_before_failed_revocation;
        stop_waits_for_hook_callback = !scope->BeginStop(0ms);
        end_held_hook_callback = services.EndHookCallback(owner, held_hook_callback);
        scope_drained = scope->InFlightCallbacks() == callbacks_before_hook_lease &&
            scope->BeginStop(50ms);
    }
    result = Check(
                 begin_held_hook_callback.code == ANOMALY_STATUS_V1_OK && scope_lease_held &&
                     failed_revocation_retained_hook && stop_waits_for_hook_callback &&
                     end_held_hook_callback.code == ANOMALY_STATUS_V1_OK && scope_drained,
                 "hook callback lease did not keep the scope resource alive through drain") && result;
    result = Check(
                 services.ReleaseHook(owner, g_second_hook_handle).code == ANOMALY_STATUS_V1_OK &&
                     second_hooked_target(4) == 6,
                 "second function hook release failed after callback drain") && result;

    const bool prestop_revoked = services.RevokeScope(
        owner, std::chrono::steady_clock::now() + 1s,
        anomaly::ScopedPlatformRevokePhase::PreStop);
    constexpr std::string_view kFinalEnabled = "{\"enabled\":true}";
    version = 0;
    config_size = 0;
    const bool config_retained_through_prestop =
        prestop_revoked && scope->Resources().size() == 1 &&
        services.Snapshot(owner).resources.configs == 1 &&
        services.WriteConfig(owner, "settings", 2,
            {reinterpret_cast<const std::uint8_t*>(kFinalEnabled.data()), kFinalEnabled.size()}).code ==
            ANOMALY_STATUS_V1_OK &&
        services.ReadConfig(owner, "settings", &version, {nullptr, 0}, &config_size).code ==
            ANOMALY_STATUS_V1_OK &&
        version == 2 && config_size == kFinalEnabled.size();
    result = Check(
                 config_retained_through_prestop,
                 "pre-stop revocation did not retain a writable config schema") && result;

    const bool scope_revoked = services.RevokeScope(
        owner, std::chrono::steady_clock::now() + 1s,
        anomaly::ScopedPlatformRevokePhase::Final);
    version = 0;
    config_size = 0;
    result = Check(
                 scope_revoked && patch_target[0] == 0x10 && patch_target[1] == 0x20 &&
                      scope->Resources().empty() &&
                      services.ReadConfig(owner, "settings", &version, {nullptr, 0}, &config_size).code ==
                          ANOMALY_STATUS_V1_NOT_FOUND &&
                      services.WriteConfig(owner, "settings", 2,
                          {reinterpret_cast<const std::uint8_t*>(kFinalEnabled.data()),
                              kFinalEnabled.size()}).code == ANOMALY_STATUS_V1_NOT_FOUND &&
                      services.ReleasePatch(owner, patch).code == ANOMALY_STATUS_V1_NOT_FOUND,
                 "scope revocation did not restore tracked resources") && result;
    static_cast<void>(memory_services.memory->FreeMemory(patch_target));
    g_hook_original = nullptr;
    g_second_hook_original = nullptr;
    g_services = nullptr;
    std::filesystem::remove_all(root, error);
    return result;
}

}  // namespace

int main() {
    return TestHookOutputsPublishedBeforeEnable() && TestScopedPlatformServices() ? 0 : 1;
}
