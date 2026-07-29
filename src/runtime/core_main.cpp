#include "analyzer.hpp"
#include "anomaly/core_api.hpp"
#include "anomaly/crash_reporter.hpp"
#include "anomaly/diagnostic_pipe_service.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/nte_profile_runtime.hpp"
#include "anomaly/platform_settings.hpp"
#include "anomaly/repository_coordinator.hpp"
#include "anomaly/runtime_crash_coordinator.hpp"
#include "anomaly/runtime_recovery.hpp"
#include "anomaly/runtime_session.hpp"
#include "anomaly/sdk/version.h"
#include "anomaly/service_graph.hpp"
#include "anomaly/service_graph_diagnostics.hpp"
#include "anomaly/structured_logger.hpp"
#include "config.hpp"
#include "pipe_server.hpp"
#include "platform_host.hpp"
#include "plugin_manager.hpp"
#include "build_provenance.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct CoreContext {
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    ue5mem::AnalyzerConfig config;
    std::shared_ptr<const anomaly::Translator> translator;
    anomaly::CoreMemoryServices memory_services;
    std::unique_ptr<anomaly::CrashReporter> crash_reporter;
    std::shared_ptr<anomaly::DiagnosticPipeService> diagnostic_pipe;
    std::shared_ptr<anomaly::StructuredLogger> logger;
    std::shared_ptr<anomaly::PlatformSettingsStore> settings;
    HMODULE game_module{};
    std::shared_ptr<anomaly::NteProfileRuntime> profile_runtime;
    std::shared_ptr<anomaly::RepositoryCoordinator> repository;
    std::string repository_diagnostics{
        "{\"schemaVersion\":1,\"state\":\"unavailable\",\"reason\":\"not-started\"}"};
    anomaly::RuntimeSafeModeState safe_mode;
    bool recovery_state_conservative{};
    mutable std::mutex recovery_mutex;
    std::unique_ptr<anomaly::RuntimeCrashCoordinatorClient> crash_coordinator;
    std::string crash_coordinator_state{"unavailable"};
    std::string recovery_diagnostics{
        "{\"schemaVersion\":1,\"state\":\"normal\",\"minimalCore\":false,"
        "\"thirdPartyPluginsSuspended\":false,\"profileOverridesSuspended\":false,"
        "\"reason\":\"\"}"};
    mutable std::mutex plugins_mutex;
    std::shared_ptr<ue5mem::PluginManager> plugins;
    std::weak_ptr<anomaly::RuntimeSession> session;
    std::string plugin_stop_diagnostics{"[]"};
    std::string profile_diagnostics{"null"};
    std::weak_ptr<anomaly::ServiceGraph> services;
};

std::string EscapeJson(std::string_view value);

std::string RecoveryDiagnosticsJson(const CoreContext& context) {
    const auto& safe_mode = context.safe_mode;
    const char* state = context.recovery_state_conservative
        ? "conservative" : safe_mode.Active() ? "safe-mode" : "normal";
    return "{\"schemaVersion\":1,\"state\":\"" + std::string(state) +
        "\",\"minimalCore\":" + (safe_mode.minimal_core ? "true" : "false") +
        ",\"thirdPartyPluginsSuspended\":" +
        (safe_mode.third_party_plugins_suspended ? "true" : "false") +
        ",\"profileOverridesSuspended\":" +
        (safe_mode.profile_overrides_suspended ? "true" : "false") +
        ",\"crashCoordinator\":\"" + EscapeJson(context.crash_coordinator_state) + "\"" +
        ",\"reason\":\"" + EscapeJson(safe_mode.reason) + "\"}";
}

std::string RecoveryDiagnosticsSnapshot(const CoreContext& context) {
    std::scoped_lock lock(context.recovery_mutex);
    return context.recovery_diagnostics;
}

struct RuntimeControl {
    std::mutex mutex;
    std::shared_ptr<anomaly::RuntimeSession> session;
    anomaly::RuntimeSessionSnapshot last_snapshot;
};

HMODULE g_core_module{};
std::unique_ptr<RuntimeControl> g_runtime_control = std::make_unique<RuntimeControl>();

DWORD CurrentExceptionError() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::filesystem::filesystem_error&) {
        return ERROR_PATH_NOT_FOUND;
    } catch (const std::system_error&) {
        return ERROR_GEN_FAILURE;
    } catch (...) {
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

std::filesystem::path ModulePath(HMODULE module) {
    if (module == nullptr) return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::filesystem::path ModuleDirectory(HMODULE module) {
    return ModulePath(module).parent_path();
}

void RuntimeLog(
    const std::shared_ptr<CoreContext>& context,
    anomaly::LogLevel level,
    std::string event_id,
    std::string message,
    anomaly::LogThreadDomain thread_domain = anomaly::LogThreadDomain::Lifecycle) {
    std::ofstream(context->log_directory / L"anomaly-runtime.log", std::ios::app)
        << "pid=" << GetCurrentProcessId() << ' ' << message << '\n';
    if (context->logger == nullptr) return;
    anomaly::LogDetails details;
    details.thread_domain = thread_domain;
    details.event_id = std::move(event_id);
    static_cast<void>(context->logger->Log(
        level, "runtime", std::move(message), std::move(details)));
}

std::shared_ptr<ue5mem::PluginManager> PluginHostSnapshot(
    const std::shared_ptr<CoreContext>& context) {
    std::scoped_lock lock(context->plugins_mutex);
    return context->plugins;
}

void PublishPluginHost(
    const std::shared_ptr<CoreContext>& context,
    std::shared_ptr<ue5mem::PluginManager> plugins) {
    std::scoped_lock lock(context->plugins_mutex);
    context->plugins = std::move(plugins);
}

std::shared_ptr<ue5mem::PluginManager> TakePluginHost(
    const std::shared_ptr<CoreContext>& context) {
    std::scoped_lock lock(context->plugins_mutex);
    return std::exchange(context->plugins, {});
}

bool WriteDiagnosticsSummary(
    const std::shared_ptr<CoreContext>& context,
    std::string_view runtime_state) noexcept {
    try {
        const std::filesystem::path state_directory = context->runtime_root / L"state";
        const std::filesystem::path destination = state_directory / L"diagnostics-summary.json";
        const std::filesystem::path temporary = state_directory /
            (L"diagnostics-summary.json.tmp-" + std::to_wstring(GetCurrentProcessId()));

        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        const std::string repository = context->repository == nullptr
            ? context->repository_diagnostics
            : anomaly::SerializeRepositoryCoordinatorSnapshotJson(
                  context->repository->Snapshot());
        output << "{\"schemaVersion\":1,\"runtimeVersion\":\""
               << ANOMALY_SDK_VERSION_STRING << "\",\"runtimeState\":\""
               << runtime_state << "\",\"profile\":"
               << context->profile_diagnostics << ",\"repository\":" << repository
               << ",\"recovery\":" << RecoveryDiagnosticsSnapshot(*context)
               << ",\"plugins\":"
               << context->plugin_stop_diagnostics << "}\n";
        output.flush();
        if (!output) {
            output.close();
            static_cast<void>(DeleteFileW(temporary.c_str()));
            return false;
        }
        output.close();
        if (!output || MoveFileExW(
                temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            static_cast<void>(DeleteFileW(temporary.c_str()));
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void LogDiagnosticsSummaryFailure(
    const std::shared_ptr<CoreContext>& context,
    anomaly::LogThreadDomain thread_domain = anomaly::LogThreadDomain::Lifecycle) {
    RuntimeLog(
        context, anomaly::LogLevel::Warning,
        "diagnostics.summary.write_failed", "diagnostics_summary=unavailable",
        thread_domain);
}

DWORD InitializeCore(const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;

    std::error_code error;
    std::filesystem::create_directories(context->runtime_root / L"plugins", error);
    if (error) return ERROR_CANNOT_MAKE;
    std::filesystem::create_directories(context->log_directory, error);
    if (error) return ERROR_CANNOT_MAKE;
    std::filesystem::create_directories(context->runtime_root / L"state", error);
    if (error) return ERROR_CANNOT_MAKE;
    std::filesystem::create_directories(context->runtime_root / L"config", error);
    if (error) return ERROR_CANNOT_MAKE;

    context->settings = std::make_shared<anomaly::PlatformSettingsStore>(context->runtime_root);
    static_cast<void>(context->settings->Start());
    const auto platform_settings = context->settings->Snapshot();

    std::string logger_failure{"startup exception"};
    try {
        anomaly::StructuredLoggerOptions logger_options;
        if (platform_settings.ready) {
            logger_options.ring_capacity = platform_settings.values.diagnostics_ring_capacity;
            logger_options.minimum_level = static_cast<anomaly::LogLevel>(
                platform_settings.values.diagnostics_log_level);
        }
        logger_options.max_file_size_bytes = 16U * 1024U * 1024U;
        logger_options.retained_archive_count = 4;
        auto logger = std::make_shared<anomaly::StructuredLogger>(std::move(logger_options));
        if (logger->Start(context->log_directory / L"anomaly-runtime.jsonl")) {
            context->logger = std::move(logger);
        } else if (const auto failure = logger->LastError()) {
            logger_failure = "operation=" +
                std::to_string(static_cast<unsigned>(failure->operation)) +
                " code=" + std::to_string(failure->code.value()) +
                " message=" + failure->message;
        }
    } catch (const std::exception& exception) {
        logger_failure = exception.what();
    } catch (...) {
    }
    if (context->logger == nullptr) {
        std::ofstream(context->log_directory / L"anomaly-runtime.log", std::ios::app)
            << "pid=" << GetCurrentProcessId() << " structured_logger=disabled error="
            << logger_failure << '\n';
    }

    auto crash_reporter = std::make_unique<anomaly::CrashReporter>(
        anomaly::CrashReporterOptions{
            context->runtime_root / L"crashes", ANOMALY_SDK_VERSION_STRING});
    std::string crash_error;
    if (!crash_reporter->Install(&crash_error)) {
        RuntimeLog(
            context, anomaly::LogLevel::Warning, "crash_reporter.disabled",
            "crash_reporter=disabled error=" + crash_error);
    } else {
        context->crash_reporter = std::move(crash_reporter);
    }

    context->config = ue5mem::AnalyzerConfig::Load(context->runtime_root / L"anomaly.ini");
    for (const auto& diagnostic : context->config.diagnostics) {
        RuntimeLog(
            context, anomaly::LogLevel::Warning, "config.invalid_value",
            "config_key=" + diagnostic.key + " reason=" + diagnostic.message);
    }
    const auto locale = anomaly::ResolveUserLocale(context->config.platform_language);
    if (locale.system_query_failed) {
        RuntimeLog(
            context, anomaly::LogLevel::Warning, "i18n.system_locale_unavailable",
            "requested_locale=auto fallback_locale=en-US");
    }
    auto translator = anomaly::LoadHostCatalog(
        locale.locale, context->runtime_root / L"locales" / L"host");
    for (const auto& diagnostic : translator.diagnostics) {
        RuntimeLog(
            context, anomaly::LogLevel::Warning, "i18n.host_catalog_invalid",
            "catalog_path=" + diagnostic.path + " reason=" + diagnostic.message);
    }
    context->translator = std::move(translator.translator);
    anomaly::RuntimeRecoveryStore recovery(context->runtime_root);
    const auto recovery_state = recovery.Load();
    if (recovery_state.Ok()) {
        context->safe_mode = recovery_state.state->safe_mode;
    } else if (recovery_state.error != anomaly::RuntimeRecoveryError::StateUnavailable) {
        context->safe_mode.minimal_core = true;
        context->safe_mode.reason = recovery_state.message.empty()
            ? "Runtime recovery state is unavailable"
            : "Runtime recovery state rejected: " + recovery_state.message;
        context->recovery_state_conservative = true;
    }

    const auto session = context->session.lock();
    const auto generation = session == nullptr ? 0 : session->Snapshot().generation;
    std::filesystem::path coordinator_executable =
        ModuleDirectory(g_core_module) / L"AnomalyCrashCoordinator.exe";
    if (!std::filesystem::is_regular_file(coordinator_executable)) {
        coordinator_executable = context->runtime_root / L"AnomalyCrashCoordinator.exe";
    }
    auto crash_coordinator = std::make_unique<anomaly::RuntimeCrashCoordinatorClient>(
        anomaly::RuntimeCrashCoordinatorOptions{
            context->runtime_root,
            std::move(coordinator_executable),
            GetCurrentProcessId(),
            generation,
            ANOMALY_SDK_VERSION_STRING});
    const auto coordinator_started = crash_coordinator->Start();
    if (coordinator_started.Ok()) {
        context->crash_coordinator_state = "monitoring";
        context->crash_coordinator = std::move(crash_coordinator);
    } else {
        context->crash_coordinator_state =
            std::string(anomaly::RuntimeCrashCoordinatorErrorName(
                coordinator_started.error));
        RuntimeLog(
            context, anomaly::LogLevel::Warning,
            "runtime.crash_coordinator_unavailable",
            "crash_coordinator=unavailable reason=" + coordinator_started.message);
    }
    {
        std::scoped_lock lock(context->recovery_mutex);
        context->recovery_diagnostics = RecoveryDiagnosticsJson(*context);
    }
    context->profile_diagnostics = "null";
    if (!WriteDiagnosticsSummary(context, "starting")) {
        LogDiagnosticsSummaryFailure(context);
    }
    RuntimeLog(
        context, anomaly::LogLevel::Info, "runtime.start",
        "runtime=start version=" ANOMALY_SDK_VERSION_STRING);
    if (context->safe_mode.Active()) {
        RuntimeLog(
            context, anomaly::LogLevel::Warning, "runtime.safe_mode",
            "runtime_safe_mode=" + RecoveryDiagnosticsSnapshot(*context));
    }
    return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_SUCCESS;
}

DWORD PrepareRepository(
    const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    anomaly::RepositoryCoordinatorOptions options;
    options.runtime_root = context->runtime_root;
    options.plugin_directory = context->config.plugin_directory;
    options.game = context->config.game_id;
    options.api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    if (context->settings != nullptr) {
        const auto settings = context->settings->Snapshot();
        if (settings.ready) options.automatic_refresh = settings.values.updates_automatic_check;
    }
    auto repository = std::make_shared<anomaly::RepositoryCoordinator>(std::move(options));
    const bool started = repository->Start();
    const auto snapshot = repository->Snapshot();
    context->repository = std::move(repository);
    context->repository_diagnostics =
        anomaly::SerializeRepositoryCoordinatorSnapshotJson(snapshot);
    RuntimeLog(
        context,
        started ? anomaly::LogLevel::Info : anomaly::LogLevel::Warning,
        "repository.coordinator",
        "repository=" + std::string(anomaly::RepositoryCoordinatorStateName(snapshot.state)) +
            " sources=" + std::to_string(snapshot.configured_sources) +
            " plugins=" + std::to_string(snapshot.plugins.size()) +
            " reason=" + snapshot.reason,
        anomaly::LogThreadDomain::Worker);
    if (!started) return ERROR_GEN_FAILURE;
    return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_SUCCESS;
}

void StopRepository(const std::shared_ptr<CoreContext>& context) noexcept {
    auto repository = std::exchange(context->repository, {});
    if (repository != nullptr) {
        context->repository_diagnostics =
            anomaly::SerializeRepositoryCoordinatorSnapshotJson(repository->Snapshot());
        repository->Stop();
    }
}

DWORD PrepareDiagnosticPipe(
    const std::shared_ptr<CoreContext>& context,
    const std::weak_ptr<anomaly::ServiceGraph>& services,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    const auto pipe_name =
        ue5mem::BuildPipeName(context->config.pipe_prefix, GetCurrentProcessId());
    auto analyzer = std::make_shared<const ue5mem::Analyzer>(
        context->runtime_root,
        context->config,
        [context, services] {
            const auto graph = services.lock();
            std::string runtime = graph == nullptr
                ? std::string("null")
                : anomaly::SerializeServiceGraphSnapshotJson(graph->Snapshot());
            std::string plugin_diagnostics{"{\"schemaVersion\":1,\"plugins\":[]}"};
            try {
                if (const auto plugins = PluginHostSnapshot(context)) {
                    plugin_diagnostics = plugins->DiagnosticsJson();
                }
            } catch (...) {
                plugin_diagnostics = "{\"schemaVersion\":1,\"plugins\":[]}";
            }
            if (runtime.empty() || runtime.back() != '}') return runtime;
            runtime.pop_back();
            const std::string repository = context->repository == nullptr
                ? context->repository_diagnostics
                : anomaly::SerializeRepositoryCoordinatorSnapshotJson(
                      context->repository->Snapshot());
            runtime += ",\"repository\":" + repository +
                ",\"recovery\":" + RecoveryDiagnosticsSnapshot(*context) +
                ",\"plugin_diagnostics\":" + plugin_diagnostics + '}';
            return runtime;
        },
        context->memory_services,
        [context] {
            return context->profile_runtime == nullptr
                ? std::string("{\"ok\":true,\"state\":\"no-profile\",\"symbols\":[],\"features\":[]}")
                : context->profile_runtime->DiagnosticsJson();
        },
        [weak = std::weak_ptr<anomaly::NteProfileRuntime>(context->profile_runtime)](
            std::string_view arguments) {
            const auto runtime = weak.lock();
            return runtime == nullptr
                ? std::string{"{\"ok\":false,\"error\":\"UE reflection queries are unavailable\"}"}
                : runtime->ExecuteReflectionQuery(arguments);
        });
    auto pipe = std::make_shared<anomaly::DiagnosticPipeService>(
        anomaly::PipeServiceOptions{std::move(analyzer), pipe_name});
    const DWORD result = pipe->Prepare();
    if (result != ERROR_SUCCESS) {
        RuntimeLog(
            context, anomaly::LogLevel::Error, "diagnostics.pipe_prepare_failed",
            "pipe_prepare=failed code=" + std::to_string(result));
        return ERROR_SUCCESS;
    }
    if (stop_token.stop_requested()) {
        pipe->Stop();
        return ERROR_CANCELLED;
    }
    std::string pipe_message{"pipe="};
    for (const auto character : pipe_name) {
        pipe_message.push_back(static_cast<char>(character));
    }
    RuntimeLog(
        context, anomaly::LogLevel::Info, "diagnostics.pipe_ready",
        std::move(pipe_message));
    context->diagnostic_pipe = std::move(pipe);
    return ERROR_SUCCESS;
}

DWORD RunDiagnosticPipe(
    const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    const auto pipe = context->diagnostic_pipe;
    if (pipe == nullptr) return ERROR_SUCCESS;
    const DWORD result = pipe->Run(stop_token);
    if (result != ERROR_SUCCESS && !stop_token.stop_requested()) {
        RuntimeLog(
            context, anomaly::LogLevel::Error, "diagnostics.pipe_run_failed",
            "pipe_run=failed code=" + std::to_string(result),
            anomaly::LogThreadDomain::Worker);
        return ERROR_SUCCESS;
    }
    return result;
}

void StopDiagnosticPipe(const std::shared_ptr<CoreContext>& context) noexcept {
    if (context->diagnostic_pipe != nullptr) context->diagnostic_pipe->Stop();
}

DWORD PrepareNteProfile(
    const std::shared_ptr<CoreContext>& context,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    if (context->safe_mode.minimal_core) {
        context->profile_diagnostics =
            "{\"state\":\"suspended\",\"reason\":\"minimal-core recovery mode\"}";
        RuntimeLog(
            context, anomaly::LogLevel::Warning, "profile.suspended",
            "profile=suspended reason=minimal-core");
        if (!WriteDiagnosticsSummary(context, "running")) {
            LogDiagnosticsSummaryFailure(context);
        }
        return ERROR_SUCCESS;
    }
    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = context->runtime_root;
    options.profile_directory = context->config.profile_directory;
    options.local_profile_directory = context->config.local_profile_directory;
    options.managed_profile_directory = context->config.managed_profile_directory;
    options.profile_overrides_enabled =
        !context->safe_mode.profile_overrides_suspended;
    options.game_id = context->config.game_id;
    options.game_module = context->game_module;
    options.memory_services = context->memory_services;
    options.section_readiness_timeout = std::chrono::seconds(5);
    options.snapshot_sampling.player_tick_interval = static_cast<std::uint32_t>((std::min)(
        context->config.player_snapshot_tick_interval,
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    options.snapshot_sampling.entity_tick_interval = static_cast<std::uint32_t>((std::min)(
        context->config.entity_snapshot_tick_interval,
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    bool override_candidate{};
    if (options.profile_overrides_enabled) {
        const auto has_files = [](const std::filesystem::path& path) {
            std::error_code error;
            const auto absolute = std::filesystem::absolute(path, error);
            if (error || !std::filesystem::is_directory(absolute, error) || error) return false;
            for (std::filesystem::directory_iterator iterator(absolute, error), end;
                 !error && iterator != end; iterator.increment(error)) {
                if (iterator->is_regular_file(error) && !error) return true;
            }
            return false;
        };
        const auto rooted = [&](const std::filesystem::path& path) {
            return path.is_absolute() ? path : context->runtime_root / path;
        };
        override_candidate = has_files(
            rooted(options.local_profile_directory) / options.game_id) || has_files(
            rooted(options.managed_profile_directory) / options.game_id);
    }
    if (override_candidate && context->crash_coordinator != nullptr) {
        static_cast<void>(context->crash_coordinator->SetFailureContext(
            anomaly::RuntimeFailureSource::ProfileOverride,
            "active-profile-override"));
    }
    auto runtime = std::make_shared<anomaly::NteProfileRuntime>(std::move(options));
    if (!runtime->Start(stop_token)) {
        return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_GEN_FAILURE;
    }
    if (stop_token.stop_requested()) {
        runtime->Stop();
        return ERROR_CANCELLED;
    }
    context->profile_diagnostics = runtime->DiagnosticsJson();
    if (!WriteDiagnosticsSummary(context, "running")) {
        LogDiagnosticsSummaryFailure(context);
    }
    RuntimeLog(
        context, anomaly::LogLevel::Info, "profile.ready",
        "profile=" + context->profile_diagnostics);
    context->profile_runtime = std::move(runtime);
    if (context->crash_coordinator != nullptr) {
        static_cast<void>(context->crash_coordinator->SetFailureContext(
            anomaly::RuntimeFailureSource::RuntimeStartup));
    }
    return ERROR_SUCCESS;
}

void StopNteProfile(const std::shared_ptr<CoreContext>& context) noexcept {
    bool stopped{};
    if (context->profile_runtime != nullptr) {
        const auto runtime = context->profile_runtime;
        stopped = runtime->Stop(std::chrono::seconds(5));
        context->profile_diagnostics = runtime->DiagnosticsJson();
        if (!stopped) {
            RuntimeLog(
                context, anomaly::LogLevel::Warning, "profile.stop_deferred",
                "profile=game_tick_generation_quarantined");
        }
    }
    context->profile_runtime.reset();
}

std::string EscapeJson(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) result += '?';
            else result.push_back(static_cast<char>(character));
            break;
        }
    }
    return result;
}

DWORD PreparePluginHost(
    const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    try {
        const bool minimal_core = context->safe_mode.minimal_core;
        const bool suspend_third_party =
            context->safe_mode.third_party_plugins_suspended;
        auto plugins = std::make_shared<ue5mem::PluginManager>(
            context->runtime_root, context->config.plugin_directory,
            context->memory_services,
            ue5mem::PluginCallbackBudgets{
                context->config.update_slow_milliseconds,
                context->config.draw_slow_milliseconds},
            context->logger,
            [session = context->session](
                std::string owner, std::uint64_t generation,
                std::function<void()> callback) -> bool {
                const auto runtime = session.lock();
                if (runtime == nullptr || !callback) return false;
                return static_cast<bool>(runtime->Dispatchers().Post(
                    anomaly::ExecutionDomain::Lifecycle,
                    std::move(owner), generation, std::move(callback)));
            },
            [session = context->session](
                std::string owner, std::uint64_t generation,
                std::function<void()> callback) -> bool {
                const auto runtime = session.lock();
                if (runtime == nullptr || !callback) return false;
                return static_cast<bool>(runtime->Dispatchers().Post(
                    anomaly::ExecutionDomain::Worker,
                    std::move(owner), generation, std::move(callback)));
            },
            [session = context->session](
                const std::uint32_t affinity,
                std::string owner, std::uint64_t generation,
                std::function<void()> callback) -> bool {
                const auto runtime = session.lock();
                if (runtime == nullptr || !callback) return false;
                anomaly::ExecutionDomain domain{};
                switch (affinity) {
                case ANOMALY_IPC_AFFINITY_V1_LIFECYCLE:
                    domain = anomaly::ExecutionDomain::Lifecycle;
                    break;
                case ANOMALY_IPC_AFFINITY_V1_GAME:
                    domain = anomaly::ExecutionDomain::Game;
                    break;
                case ANOMALY_IPC_AFFINITY_V1_RENDER:
                    domain = anomaly::ExecutionDomain::Render;
                    break;
                default:
                    domain = anomaly::ExecutionDomain::Worker;
                    break;
                }
                return static_cast<bool>(runtime->Dispatchers().Post(
                    domain, std::move(owner), generation, std::move(callback)));
            },
            [minimal_core, suspend_third_party](
                const anomaly::PluginManifest& manifest) {
                if (minimal_core) return false;
                return !suspend_third_party ||
                    manifest.id.starts_with("anomaly.builtin.");
            },
            [weak = std::weak_ptr<CoreContext>(context)](
                std::string_view plugin_id,
                std::uint64_t generation,
                bool entering) {
                const auto current = weak.lock();
                if (current == nullptr || current->crash_coordinator == nullptr) return;
                if (entering) {
                    static_cast<void>(current->crash_coordinator->SetFailureContext(
                        anomaly::RuntimeFailureSource::PluginGeneration,
                        {}, std::string(plugin_id), generation));
                } else {
                    static_cast<void>(current->crash_coordinator->SetFailureContext(
                        anomaly::RuntimeFailureSource::RuntimeStartup));
                }
            });
        plugins->SetTranslator(context->translator);
        const auto session = context->session;
        plugins->SetQueuedCallbackCanceller(
            [session](std::string_view owner, std::uint64_t generation) {
                if (const auto runtime = session.lock()) {
                    static_cast<void>(runtime->Dispatchers().CancelOwnerGeneration(
                        owner, generation));
                }
            });
        plugins->LoadAll();
        PublishPluginHost(context, std::move(plugins));
        const auto published = PluginHostSnapshot(context);
        RuntimeLog(
            context, anomaly::LogLevel::Info, "plugin.host.ready",
            "plugin_host=ready count=" +
                std::to_string(published == nullptr ? 0 : published->Plugins().size()));
        return ERROR_SUCCESS;
    } catch (...) {
        RuntimeLog(
            context, anomaly::LogLevel::Error, "plugin.host.start_failed",
            "plugin_host=start_failed");
        return CurrentExceptionError();
    }
}

DWORD StopPluginHost(
    const std::shared_ptr<CoreContext>& context,
    std::chrono::milliseconds timeout) noexcept {
    // A bounded UI teardown may leave a lifecycle callback owner in
    // quarantine. Do not start unloading the corresponding PluginManager
    // until that owner has either drained or been retired; the owner keeps
    // the module mapping alive for late callbacks.
    const auto current = PluginHostSnapshot(context);
    if (current != nullptr && ue5mem::PlatformHostQuarantined(current.get())) {
        RuntimeLog(
            context, anomaly::LogLevel::Warning, "plugin.host.stop_deferred",
            "plugin_host=stop_deferred reason=platform_ui_quarantine");
        return ERROR_TIMEOUT;
    }
    const auto plugins = TakePluginHost(context);
    if (plugins == nullptr) {
        // RuntimeSession invokes this callback before ServiceGraph::StopAll;
        // the plugin-host service callback can therefore arrive a second time.
        // Keep the diagnostics captured by the first stop instead of erasing
        // them when the ownership slot is already empty.
        return ERROR_SUCCESS;
    }
    bool stopped{};
    try {
        stopped = plugins->StopForRuntime(timeout);
        const auto diagnostics = plugins->StopDiagnostics();
        std::ostringstream json;
        json << '[';
        for (std::size_t index = 0; index < diagnostics.size(); ++index) {
            if (index != 0) json << ',';
            const auto& diagnostic = diagnostics[index];
            json << "{\"id\":\"" << EscapeJson(diagnostic.id)
                 << "\",\"generation\":" << diagnostic.generation
                 << ",\"drained\":" << (diagnostic.drained ? "true" : "false")
                 << ",\"timedOut\":" << (diagnostic.timed_out ? "true" : "false")
                 << ",\"inFlight\":" << diagnostic.in_flight_callbacks
                 << ",\"resources\":" << diagnostic.resources
                 << ",\"reason\":\"" << EscapeJson(diagnostic.reason) << "\"}";
        }
        json << ']';
        context->plugin_stop_diagnostics = json.str();
        RuntimeLog(
            context, stopped ? anomaly::LogLevel::Info : anomaly::LogLevel::Warning,
            "plugin.host.stopped",
            "plugin_host=stopped drained=" + std::to_string(stopped ? 1 : 0) +
                " generations=" + std::to_string(diagnostics.size()));
    } catch (...) {
        context->plugin_stop_diagnostics = "[]";
        RuntimeLog(
            context, anomaly::LogLevel::Error, "plugin.host.stop_failed",
            "plugin_host=stop_exception");
        return CurrentExceptionError();
    }
    return stopped ? ERROR_SUCCESS : ERROR_TIMEOUT;
}

DWORD RunPlatform(const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested() || !context->config.platform_enabled) return ERROR_SUCCESS;
    if (context->crash_coordinator != nullptr) {
        static_cast<void>(context->crash_coordinator->SetFailureContext(
            anomaly::RuntimeFailureSource::RenderInitialization));
    }
    ue5mem::PlatformDiagnostics diagnostics;
    diagnostics.translator = context->translator;
    diagnostics.runtime_root = context->runtime_root;
    diagnostics.log_file = context->runtime_root / L"anomaly-platform.log";
    diagnostics.service_graph = [weak = context->services] {
        const auto graph = weak.lock();
        return graph ? graph->Snapshot() : anomaly::ServiceGraphSnapshot{};
    };
    diagnostics.profile_json = [weak = std::weak_ptr<anomaly::NteProfileRuntime>(context->profile_runtime)] {
        const auto runtime = weak.lock();
        return runtime ? runtime->DiagnosticsJson() : std::string{"{\"state\":\"unavailable\"}"};
    };
    diagnostics.nte_compatibility =
        [profile = std::weak_ptr<anomaly::NteProfileRuntime>(context->profile_runtime)] {
            const auto runtime = profile.lock();
            if (runtime == nullptr) return anomaly::BuildNteCompatibilitySnapshot(
                {}, {}, {});
            const auto current = runtime->Evidence();
            return anomaly::BuildNteCompatibilitySnapshot(
                current.fingerprint, current.profile, current.resolution);
        };
    diagnostics.hooks = [weak = std::weak_ptr<anomaly::NteProfileRuntime>(context->profile_runtime)] {
        const auto runtime = weak.lock();
        return runtime ? runtime->Hooks() : std::vector<anomaly::HookRecordView>{};
    };
    diagnostics.repository_snapshot =
        [weak = std::weak_ptr<anomaly::RepositoryCoordinator>(context->repository)] {
            const auto repository = weak.lock();
            return repository == nullptr
                ? anomaly::RepositoryCoordinatorSnapshot{}
                : repository->Snapshot();
        };
    diagnostics.repository_refresh =
        [weak = std::weak_ptr<anomaly::RepositoryCoordinator>(context->repository)] {
            const auto repository = weak.lock();
            return repository == nullptr
                ? anomaly::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->Refresh();
        };
    diagnostics.repository_install =
        [weak = std::weak_ptr<anomaly::RepositoryCoordinator>(context->repository)](
            std::string_view plugin_id, std::string_view version) {
            const auto repository = weak.lock();
            return repository == nullptr
                ? anomaly::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->InstallPlugin(plugin_id, version);
        };
    diagnostics.repository_uninstall =
        [weak = std::weak_ptr<anomaly::RepositoryCoordinator>(context->repository)](
            std::string_view plugin_id) {
            const auto repository = weak.lock();
            return repository == nullptr
                ? anomaly::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->UninstallPlugin(plugin_id);
        };
    diagnostics.repository_config =
        [weak = std::weak_ptr<anomaly::RepositoryCoordinator>(context->repository)] {
            const auto repository = weak.lock();
            return repository == nullptr ? anomaly::PluginRepositoryConfig{}
                                         : repository->Configuration();
        };
    diagnostics.repository_configure =
        [weak = std::weak_ptr<anomaly::RepositoryCoordinator>(context->repository)](
            const anomaly::PluginRepositoryConfig& config) {
            const auto repository = weak.lock();
            return repository == nullptr
                ? anomaly::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->Configure(config);
        };
    diagnostics.settings_snapshot =
        [weak = std::weak_ptr<anomaly::PlatformSettingsStore>(context->settings)] {
            const auto settings = weak.lock();
            return settings == nullptr
                ? anomaly::PlatformSettingsSnapshot{}
                : settings->Snapshot();
        };
    diagnostics.settings_apply =
        [weak = std::weak_ptr<anomaly::PlatformSettingsStore>(context->settings),
         logger = std::weak_ptr<anomaly::StructuredLogger>(context->logger),
         repository = std::weak_ptr<anomaly::RepositoryCoordinator>(context->repository)](
            const anomaly::PlatformSettingsApplyRequest& request) {
            const auto settings = weak.lock();
            if (settings == nullptr) return anomaly::PlatformSettingsApplyResult{};
            auto result = settings->Apply(request);
            if (!result.Applied()) return result;
            if (const auto active_logger = logger.lock()) {
                static_cast<void>(active_logger->Reconfigure(
                    static_cast<anomaly::LogLevel>(
                        result.snapshot.values.diagnostics_log_level),
                    result.snapshot.values.diagnostics_ring_capacity));
            }
            if (const auto active_repository = repository.lock()) {
                static_cast<void>(active_repository->Refresh());
            }
            return result;
        };
    diagnostics.settings_record_route =
        [weak = std::weak_ptr<anomaly::PlatformSettingsStore>(context->settings)](
            const std::string_view route) {
            const auto settings = weak.lock();
            return settings != nullptr && settings->RecordLastRoute(route);
        };
    diagnostics.game_pump = [weak = context->session]() -> std::size_t {
        const auto session = weak.lock();
        return session == nullptr ? 0 : session->Dispatchers().PumpGame();
    };
    diagnostics.lifecycle_invoke = [context](std::function<void()> operation) -> std::uint32_t {
        const auto session = context->session.lock();
        if (session == nullptr) return ERROR_NOT_READY;
        const auto state = session->Snapshot().state;
        if (state == ANOMALY_RUNTIME_STATE_STOP_REQUESTED ||
            state == ANOMALY_RUNTIME_STATE_STOPPING_PLUGINS ||
            state == ANOMALY_RUNTIME_STATE_STOPPING_SERVICES ||
            state == ANOMALY_RUNTIME_STATE_FAILED ||
            state == ANOMALY_RUNTIME_STATE_STOPPED) {
            return ERROR_CANCELLED;
        }
        // UI submission is bounded; the lifecycle callback itself owns the
        // plugin serialization lock and reports a typed failure on timeout.
        return session->Dispatchers().Invoke(
            anomaly::ExecutionDomain::Lifecycle, std::move(operation), std::chrono::seconds(5));
    };
    diagnostics.lifecycle_post = [context](std::function<void()> operation) -> std::uint32_t {
        const auto session = context->session.lock();
        if (session == nullptr) return ERROR_NOT_READY;
        const auto state = session->Snapshot().state;
        if (state == ANOMALY_RUNTIME_STATE_STOP_REQUESTED ||
            state == ANOMALY_RUNTIME_STATE_STOPPING_PLUGINS ||
            state == ANOMALY_RUNTIME_STATE_STOPPING_SERVICES ||
            state == ANOMALY_RUNTIME_STATE_FAILED ||
            state == ANOMALY_RUNTIME_STATE_STOPPED) {
            return ERROR_CANCELLED;
        }
        const auto task = session->Dispatchers().Post(
            anomaly::ExecutionDomain::Lifecycle,
            "anomaly.platform.ui", 1, std::move(operation));
        return task ? ERROR_SUCCESS : ERROR_NOT_READY;
    };
    diagnostics.lifecycle_drain = [context](std::chrono::milliseconds timeout) -> bool {
        const auto session = context->session.lock();
        if (session == nullptr) return true;
        const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
        const auto deadline = bounded == std::chrono::milliseconds::max()
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + bounded;
        const auto remaining = [&] {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                return std::chrono::milliseconds::max();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::chrono::milliseconds::zero();
            return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        };
        // The lifecycle pump may already be leaving RUNNING when the render
        // worker begins its teardown. Cancel queued UI posts before draining;
        // otherwise a stopped pump would strand their owner captures until
        // dispatcher destruction.
        static_cast<void>(session->Dispatchers().CancelOwnerGeneration(
            "anomaly.platform.ui", 1));
        if (!session->Dispatchers().Drain("anomaly.platform.ui", 1, remaining())) {
            return false;
        }
        return session->Dispatchers().DrainInvocations(remaining());
    };
    diagnostics.logger = context->logger;
    if (context->config.platform_embedded) {
        ue5mem::RunEmbeddedPlatform(
            context->runtime_root, context->config, stop_token, context->memory_services,
            context->profile_runtime == nullptr ? nullptr : context->profile_runtime->Adapter(),
            std::move(diagnostics), PluginHostSnapshot(context));
    } else {
        ue5mem::RunPlatform(
            context->runtime_root, context->config, stop_token, context->memory_services,
            context->profile_runtime == nullptr ? nullptr : context->profile_runtime->Adapter(),
            std::move(diagnostics), PluginHostSnapshot(context));
    }
    const auto plugins = PluginHostSnapshot(context);
    const bool stopped = !ue5mem::PlatformHostQuarantined(plugins.get());
    return stopped ? ERROR_SUCCESS : ERROR_TIMEOUT;
}

DWORD ConfirmRuntimeHealth(
    const std::shared_ptr<CoreContext>& context,
    std::stop_token stop_token) {
    constexpr auto stability_window = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + stability_window;
    while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        Sleep(50);
    }
    if (stop_token.stop_requested()) return ERROR_SUCCESS;
    const auto session = context->session.lock();
    if (session == nullptr ||
        session->Snapshot().state != ANOMALY_RUNTIME_STATE_RUNNING) {
        return ERROR_SUCCESS;
    }

    if (context->crash_coordinator != nullptr) {
        const auto marked = context->crash_coordinator->MarkHealthy();
        if (!marked.Ok()) {
            RuntimeLog(
                context, anomaly::LogLevel::Warning,
                "runtime.crash_coordinator_health_rejected",
                "crash_coordinator_health=rejected reason=" + marked.message,
                anomaly::LogThreadDomain::Worker);
        } else {
            std::scoped_lock lock(context->recovery_mutex);
            context->crash_coordinator_state = "healthy";
            context->recovery_diagnostics = RecoveryDiagnosticsJson(*context);
        }
    }

    anomaly::RuntimeRecoveryStore recovery(context->runtime_root);
    const auto recovery_state = recovery.Load();
    if (recovery_state.Ok()) {
        const auto healthy = recovery.MarkHealthy();
        if (!healthy.Ok()) {
            RuntimeLog(
                context, anomaly::LogLevel::Warning,
                "runtime.recovery_health_rejected",
                "runtime_recovery_health=rejected reason=" + healthy.message,
                anomaly::LogThreadDomain::Worker);
        }
    }
    RuntimeLog(
        context, anomaly::LogLevel::Info,
        "runtime.health_confirmed",
        "runtime_health=confirmed version=" ANOMALY_SDK_VERSION_STRING,
        anomaly::LogThreadDomain::Worker);
    return ERROR_SUCCESS;
}

void ShutdownCore(const std::shared_ptr<CoreContext>& context) noexcept {
    try {
        if (context->crash_coordinator != nullptr) {
            static_cast<void>(context->crash_coordinator->MarkStopping());
            static_cast<void>(context->crash_coordinator->WaitForMonitor(
                std::chrono::milliseconds(500)));
            std::scoped_lock lock(context->recovery_mutex);
            context->crash_coordinator_state = "stopped";
            context->recovery_diagnostics = RecoveryDiagnosticsJson(*context);
        }
    } catch (...) {
    }
    try {
        if (context->crash_reporter != nullptr) context->crash_reporter->Uninstall();
        context->crash_reporter.reset();
    } catch (...) {
    }
    try {
        if (!WriteDiagnosticsSummary(context, "stopped")) {
            LogDiagnosticsSummaryFailure(context);
        }
        RuntimeLog(
            context, anomaly::LogLevel::Info, "runtime.stop", "runtime=stopped");
    } catch (...) {
    }
    if (context->logger != nullptr) {
        try {
            const bool stopped = context->logger->Stop();
            const auto stats = context->logger->Stats();
            if (!stopped || stats.error_count != 0 || stats.dropped != 0) {
                std::ofstream output(
                    context->log_directory / L"anomaly-runtime.log", std::ios::app);
                output << "pid=" << GetCurrentProcessId()
                       << " structured_logger=degraded stopped=" << (stopped ? 1 : 0)
                       << " errors=" << stats.error_count
                       << " dropped=" << stats.dropped;
                if (const auto failure = context->logger->LastError()) {
                    output << " operation=" << static_cast<unsigned>(failure->operation)
                           << " code=" << failure->code.value();
                }
                output << '\n';
            }
        } catch (...) {
            std::ofstream(context->log_directory / L"anomaly-runtime.log", std::ios::app)
                << "pid=" << GetCurrentProcessId()
                << " structured_logger=stop_exception\n";
        }
        context->logger.reset();
    }
}

DWORD ValidateStartInfo(const AnomalyStartInfo* start_info) noexcept {
    if (start_info == nullptr) return ERROR_INVALID_PARAMETER;
    if (start_info->struct_size < ANOMALY_START_INFO_V1_SIZE) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (start_info->bootstrap_abi_version != ANOMALY_BOOTSTRAP_ABI_VERSION) {
        return ERROR_REVISION_MISMATCH;
    }
    if (start_info->flags != 0) return ERROR_INVALID_FLAGS;
    if (start_info->bootstrap_type > ANOMALY_BOOTSTRAP_TYPE_EXTERNAL) {
        return ERROR_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS;
}

anomaly::RuntimeStartContext BuildStartContext(const AnomalyStartInfo& start_info) {
    anomaly::RuntimeStartContext result;
    result.bootstrap_abi_version = start_info.bootstrap_abi_version;
    result.bootstrap_type = start_info.bootstrap_type;
    result.bootstrap_module = start_info.bootstrap_module;
    result.game_module = start_info.game_module != nullptr
        ? start_info.game_module
        : GetModuleHandleW(nullptr);

    if (start_info.runtime_root != nullptr && start_info.runtime_root[0] != L'\0') {
        result.runtime_root = start_info.runtime_root;
    } else if (start_info.bootstrap_module != nullptr) {
        result.runtime_root = ModuleDirectory(start_info.bootstrap_module) / L"Anomaly";
    } else {
        result.runtime_root = ModuleDirectory(g_core_module);
    }
    if (result.runtime_root.empty()) {
        throw std::filesystem::filesystem_error(
            "runtime root is unavailable", std::error_code{});
    }
    result.runtime_root = std::filesystem::absolute(result.runtime_root);

    if (start_info.log_directory != nullptr && start_info.log_directory[0] != L'\0') {
        result.log_directory = std::filesystem::absolute(start_info.log_directory);
    } else {
        result.log_directory = result.runtime_root / L"logs";
    }
    result.external_stop_event = start_info.external_stop_event;
    return result;
}

anomaly::RuntimeSessionOptions BuildSessionOptions(
    const std::shared_ptr<CoreContext>& context) {
    anomaly::RuntimeSessionOptions options;
    auto services = std::make_shared<anomaly::ServiceGraph>();
    context->services = services;
    context->memory_services = anomaly::CreateCoreMemoryServices();

    anomaly::ServiceDescriptor runtime_info;
    runtime_info.id = "anomaly.runtime.info";
    runtime_info.lifetime = anomaly::ServiceLifetime::Provided;
    DWORD result = services->Register(std::move(runtime_info));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(), "register runtime info service");
    }

    anomaly::ServiceDescriptor config;
    config.id = "anomaly.config";
    config.startup = anomaly::ServiceStartup::Blocking;
    config.affinity = anomaly::ServiceAffinity::Lifecycle;
    config.required_dependencies.push_back({"anomaly.runtime.info", 1});
    config.start = [context](std::stop_token stop_token) {
        return InitializeCore(context, stop_token);
    };
    config.stop = [] {};
    result = services->Register(std::move(config));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(), "register config service");
    }

    anomaly::ServiceDescriptor module_memory;
    module_memory.id = "anomaly.internal.module-memory";
    module_memory.lifetime = anomaly::ServiceLifetime::Provided;
    module_memory.affinity = anomaly::ServiceAffinity::Any;
    result = services->Register(std::move(module_memory));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register module memory service");
    }

    anomaly::ServiceDescriptor pattern;
    pattern.id = "anomaly.internal.pattern";
    pattern.lifetime = anomaly::ServiceLifetime::Provided;
    pattern.affinity = anomaly::ServiceAffinity::Any;
    pattern.required_dependencies.push_back({"anomaly.internal.module-memory", 1});
    result = services->Register(std::move(pattern));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register pattern service");
    }

    anomaly::ServiceDescriptor pipe;
    pipe.id = "anomaly.internal.pipe";
    pipe.startup = anomaly::ServiceStartup::Blocking;
    pipe.affinity = anomaly::ServiceAffinity::Worker;
    pipe.required_dependencies.push_back({"anomaly.config", 1});
    pipe.required_dependencies.push_back({"anomaly.internal.module-memory", 1});
    pipe.required_dependencies.push_back({"anomaly.internal.pattern", 1});
    pipe.required_dependencies.push_back({"anomaly.internal.nte-profile", 1});
    const std::weak_ptr<anomaly::ServiceGraph> weak_services = services;
    pipe.start = [context, weak_services](std::stop_token stop_token) {
        return PrepareDiagnosticPipe(context, weak_services, stop_token);
    };
    pipe.stop = [context] { StopDiagnosticPipe(context); };
    result = services->Register(std::move(pipe));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register diagnostic pipe service");
    }

    anomaly::ServiceDescriptor profile;
    profile.id = "anomaly.internal.nte-profile";
    profile.startup = anomaly::ServiceStartup::Blocking;
    profile.affinity = anomaly::ServiceAffinity::Worker;
    profile.required_dependencies.push_back({"anomaly.config", 1});
    profile.required_dependencies.push_back({"anomaly.internal.module-memory", 1});
    profile.required_dependencies.push_back({"anomaly.internal.pattern", 1});
    profile.required_dependencies.push_back({"anomaly.repository.coordinator", 1});
    profile.start = [context](std::stop_token stop_token) {
        return PrepareNteProfile(context, stop_token);
    };
    profile.stop = [context] { StopNteProfile(context); };
    result = services->Register(std::move(profile));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register NTE profile service");
    }

    anomaly::ServiceDescriptor repository;
    repository.id = "anomaly.repository.coordinator";
    repository.startup = anomaly::ServiceStartup::Blocking;
    repository.affinity = anomaly::ServiceAffinity::Worker;
    repository.required_dependencies.push_back({"anomaly.config", 1});
    repository.start = [context](std::stop_token stop_token) {
        return PrepareRepository(context, stop_token);
    };
    repository.stop = [context] { StopRepository(context); };
    result = services->Register(std::move(repository));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register repository coordinator service");
    }

    anomaly::ServiceDescriptor plugin_host;
    plugin_host.id = "anomaly.plugin.host";
    plugin_host.startup = anomaly::ServiceStartup::Blocking;
    plugin_host.affinity = anomaly::ServiceAffinity::Lifecycle;
    plugin_host.required_dependencies.push_back({"anomaly.config", 1});
    plugin_host.required_dependencies.push_back({"anomaly.internal.nte-profile", 1});
    plugin_host.required_dependencies.push_back({"anomaly.repository.coordinator", 1});
    plugin_host.start = [context](std::stop_token stop_token) {
        return PreparePluginHost(context, stop_token);
    };
    plugin_host.stop = [context] {
        static_cast<void>(StopPluginHost(context, std::chrono::seconds(1)));
    };
    result = services->Register(std::move(plugin_host));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register plugin host service");
    }

    // These provided nodes make the production ownership boundaries explicit
    // in diagnostics; their concrete pumps are owned by RuntimeSession and
    // the platform worker below.
    const auto register_provided = [&](std::string id, anomaly::ServiceAffinity affinity,
                                       std::vector<anomaly::ServiceDependency> dependencies = {}) {
        anomaly::ServiceDescriptor descriptor;
        descriptor.id = std::move(id);
        descriptor.lifetime = anomaly::ServiceLifetime::Provided;
        descriptor.affinity = affinity;
        descriptor.required_dependencies = std::move(dependencies);
        const DWORD registration = services->Register(std::move(descriptor));
        if (registration != ERROR_SUCCESS) {
            throw std::system_error(
                static_cast<int>(registration), std::system_category(),
                "register production boundary service");
        }
    };
    register_provided("anomaly.dispatchers.lifecycle", anomaly::ServiceAffinity::Lifecycle);
    register_provided("anomaly.dispatchers.worker", anomaly::ServiceAffinity::Worker);
    register_provided("anomaly.dispatchers.game", anomaly::ServiceAffinity::Game);
    register_provided("anomaly.dispatchers.render", anomaly::ServiceAffinity::Render);
    register_provided(
        "anomaly.platform.host", anomaly::ServiceAffinity::Render,
        {{"anomaly.plugin.host", 1}});
    register_provided(
        "anomaly.platform.input", anomaly::ServiceAffinity::Render,
        {{"anomaly.platform.host", 1}});
    register_provided(
        "anomaly.platform.ui", anomaly::ServiceAffinity::Render,
        {{"anomaly.platform.host", 1}});
    options.services = std::move(services);
    options.stop_plugins = [context](std::chrono::milliseconds timeout) {
        return StopPluginHost(context, timeout);
    };
    options.shutdown = [context] { ShutdownCore(context); };
    options.workers.push_back({"pipe", [context](std::stop_token stop_token) {
        return RunDiagnosticPipe(context, stop_token);
    }});
    options.workers.push_back({"platform", [context](std::stop_token stop_token) {
        return RunPlatform(context, stop_token);
    }});
    options.workers.push_back({"runtime-health", [context](std::stop_token stop_token) {
        return ConfirmRuntimeHealth(context, stop_token);
    }});
    return options;
}

anomaly::RuntimeSessionSnapshot CurrentSnapshotLocked(const RuntimeControl& control) {
    return control.session != nullptr ? control.session->Snapshot() : control.last_snapshot;
}

void ArchiveStoppedSessionLocked(RuntimeControl& control) {
    if (control.session == nullptr ||
        control.session->Snapshot().state != ANOMALY_RUNTIME_STATE_STOPPED) {
        return;
    }
    control.session->Join();
    control.last_snapshot = control.session->Snapshot();
    control.session.reset();
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyStart(
    const AnomalyStartInfo* start_info) {
    const DWORD validation = ValidateStartInfo(start_info);
    if (validation != ERROR_SUCCESS) return validation;
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;

    try {
        const auto start_context = BuildStartContext(*start_info);
        auto core_context = std::make_shared<CoreContext>();
        core_context->runtime_root = start_context.runtime_root;
        core_context->log_directory = start_context.log_directory;
        core_context->game_module = start_context.game_module;
        auto session = std::make_shared<anomaly::RuntimeSession>(
            start_context, BuildSessionOptions(core_context));
        core_context->session = session;

        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        ArchiveStoppedSessionLocked(control);
        if (control.session != nullptr) return ERROR_ALREADY_INITIALIZED;

        const DWORD result = session->Start();
        control.session = session;
        if (result != ERROR_SUCCESS) ArchiveStoppedSessionLocked(control);
        return result;
    } catch (...) {
        return CurrentExceptionError();
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyRequestStop() {
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;
    std::shared_ptr<anomaly::RuntimeSession> session;
    {
        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        session = control.session;
        if (session == nullptr &&
            control.last_snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED) {
            return ERROR_SUCCESS;
        }
    }
    if (session == nullptr) return ERROR_NOT_READY;
    session->RequestStop();
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyGetState(
    AnomalyRuntimeStateInfo* state_info) {
    if (state_info == nullptr) return ERROR_INVALID_PARAMETER;
    if (state_info->struct_size < ANOMALY_RUNTIME_STATE_INFO_V1_SIZE) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (state_info->state_info_version != ANOMALY_RUNTIME_STATE_INFO_VERSION) {
        return ERROR_REVISION_MISMATCH;
    }
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;

    anomaly::RuntimeSessionSnapshot snapshot;
    {
        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        snapshot = CurrentSnapshotLocked(control);
    }
    state_info->state = snapshot.state;
    state_info->last_error = snapshot.last_error;
    state_info->session_generation = snapshot.generation;
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyWaitForStop(DWORD timeout_ms) {
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;
    std::shared_ptr<anomaly::RuntimeSession> session;
    {
        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        session = control.session;
        if (session == nullptr) {
            return control.last_snapshot.state == ANOMALY_RUNTIME_STATE_STOPPED
                ? ERROR_SUCCESS
                : ERROR_NOT_READY;
        }
    }

    const auto timeout = timeout_ms == INFINITE
        ? std::chrono::milliseconds::max()
        : std::chrono::milliseconds(timeout_ms);
    if (!session->WaitForStop(timeout)) return ERROR_TIMEOUT;
    session->Join();

    auto& control = *g_runtime_control;
    std::scoped_lock lock(control.mutex);
    if (control.session == session) {
        control.last_snapshot = session->Snapshot();
        control.session.reset();
    }
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_core_module = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_core_module = nullptr;
        static_cast<void>(g_runtime_control.release());
    }
    return TRUE;
}
