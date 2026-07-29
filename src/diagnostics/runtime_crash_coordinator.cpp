#include "anomaly/runtime_crash_coordinator.hpp"

#include "anomaly/reliable_storage.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kMarkerSchemaVersion = 1;
constexpr std::size_t kMaximumMarkerBytes = 64U * 1024U;

RuntimeCrashCoordinatorResult Failure(
    RuntimeCrashCoordinatorError error,
    std::string message,
    DWORD win32_error = ERROR_SUCCESS,
    std::string incident_id = {}) {
    return {error, win32_error, std::move(message), std::move(incident_id)};
}

std::span<const std::byte> Bytes(const std::string& value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::string Text(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::wstring MarkerRelativePath(std::string_view incident_id) {
    std::wstring wide(incident_id.begin(), incident_id.end());
    return L"state/crash-coordinator/" + wide + L".json";
}

bool IsReparsePoint(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
        !IsReparsePoint(path);
}

bool PrepareDirectory(
    const std::filesystem::path& parent,
    const std::filesystem::path& child) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_directory(parent, error) || error ||
            IsReparsePoint(parent)) {
            return false;
        }
        if (!std::filesystem::exists(child, error)) {
            if (error || !std::filesystem::create_directory(child, error) || error) return false;
        }
        return std::filesystem::is_directory(child, error) && !error &&
            !IsReparsePoint(child);
    } catch (...) {
        return false;
    }
}

bool PrepareCoordinatorDirectories(
    const std::filesystem::path& root,
    bool include_incidents = false) noexcept {
    const auto state = root / L"state";
    const auto coordinator = state / L"crash-coordinator";
    if (!PrepareDirectory(root, state) || !PrepareDirectory(state, coordinator)) return false;
    return !include_incidents ||
        PrepareDirectory(coordinator, coordinator / L"incidents");
}

std::optional<std::uint64_t> ProcessCreationTime(HANDLE process) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (process == nullptr || process == INVALID_HANDLE_VALUE ||
        GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE) {
        return std::nullopt;
    }
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
}

std::string BuildIncidentId(
    DWORD process_id,
    std::uint64_t creation_time,
    std::uint64_t generation) {
    return std::format(
        "runtime-p{}-c{:x}-g{}", process_id, creation_time, generation);
}

std::wstring QuoteArgument(std::wstring_view value) {
    std::wstring result{L"\""};
    std::size_t slashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

struct Marker {
    std::string incident_id;
    DWORD process_id{};
    std::uint64_t process_creation_time{};
    std::uint64_t session_generation{};
    std::string stage{"starting"};
    RuntimeFailureObservation observation;
};

Json MarkerJson(const Marker& marker) {
    return {
        {"schemaVersion", kMarkerSchemaVersion},
        {"incidentId", marker.incident_id},
        {"processId", marker.process_id},
        {"processCreationTime", marker.process_creation_time},
        {"sessionGeneration", marker.session_generation},
        {"stage", marker.stage},
        {"failure", {
            {"source", RuntimeFailureSourceName(marker.observation.source)},
            {"runtimeVersion", marker.observation.runtime_version},
            {"profileId", marker.observation.profile_id},
            {"pluginId", marker.observation.plugin_id},
            {"pluginGeneration", marker.observation.plugin_generation},
        }},
    };
}

std::optional<RuntimeFailureSource> ParseSource(std::string_view value) noexcept {
    if (value == "unknown") return RuntimeFailureSource::Unknown;
    if (value == "runtime-startup") return RuntimeFailureSource::RuntimeStartup;
    if (value == "profile-override") return RuntimeFailureSource::ProfileOverride;
    if (value == "plugin-generation") return RuntimeFailureSource::PluginGeneration;
    if (value == "render-initialization") return RuntimeFailureSource::RenderInitialization;
    return std::nullopt;
}

std::optional<Marker> ParseMarker(std::string_view text) {
    try {
        const Json document = Json::parse(text);
        if (!document.is_object() ||
            document.at("schemaVersion").get<std::uint32_t>() != kMarkerSchemaVersion ||
            !document.at("failure").is_object()) {
            return std::nullopt;
        }
        const auto& failure = document.at("failure");
        const auto source = ParseSource(failure.at("source").get<std::string>());
        if (!source) return std::nullopt;
        Marker marker;
        marker.incident_id = document.at("incidentId").get<std::string>();
        marker.process_id = document.at("processId").get<DWORD>();
        marker.process_creation_time =
            document.at("processCreationTime").get<std::uint64_t>();
        marker.session_generation =
            document.at("sessionGeneration").get<std::uint64_t>();
        marker.stage = document.at("stage").get<std::string>();
        if (marker.stage != "starting" && marker.stage != "healthy" &&
            marker.stage != "stopping") {
            return std::nullopt;
        }
        marker.observation.incident_id = marker.incident_id;
        marker.observation.source = *source;
        marker.observation.runtime_version =
            failure.at("runtimeVersion").get<std::string>();
        marker.observation.profile_id = failure.at("profileId").get<std::string>();
        marker.observation.plugin_id = failure.at("pluginId").get<std::string>();
        marker.observation.plugin_generation =
            failure.at("pluginGeneration").get<std::uint64_t>();
        return marker;
    } catch (...) {
        return std::nullopt;
    }
}

RuntimeCrashCoordinatorResult WriteMarker(
    ReliableStorage& storage,
    const Marker& marker) {
    const std::string document = MarkerJson(marker).dump() + '\n';
    const auto written = storage.WriteAtomic(
        MarkerRelativePath(marker.incident_id), Bytes(document));
    return written
        ? RuntimeCrashCoordinatorResult{
              RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, marker.incident_id}
        : Failure(
              RuntimeCrashCoordinatorError::IoFailure,
              "crash coordinator marker could not be committed",
              written.win32_error,
              marker.incident_id);
}

RuntimeCrashCoordinatorResult LoadMarker(
    ReliableStorage& storage,
    std::string_view incident_id,
    Marker& marker) {
    const auto loaded = storage.Read(MarkerRelativePath(incident_id), kMaximumMarkerBytes);
    if (!loaded) {
        return Failure(
            RuntimeCrashCoordinatorError::MarkerRejected,
            "crash coordinator marker could not be read",
            loaded.result.win32_error,
            std::string(incident_id));
    }
    auto parsed = ParseMarker(Text(loaded.bytes));
    if (!parsed) {
        return Failure(
            RuntimeCrashCoordinatorError::MarkerRejected,
            "crash coordinator marker is invalid",
            ERROR_INVALID_DATA,
            std::string(incident_id));
    }
    marker = std::move(*parsed);
    return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, std::string(incident_id)};
}

void WriteIncidentInventory(
    ReliableStorage& storage,
    const std::filesystem::path& root,
    const Marker& marker,
    DWORD exit_code) noexcept {
    try {
        if (!PrepareCoordinatorDirectories(root, true)) return;
        Json artifacts = Json::array();
        const auto add_if_regular = [&](std::string kind, const std::filesystem::path& relative,
                                        bool manual_review) {
            const auto path = root / relative;
            if (!IsRegularFile(path)) return;
            artifacts.push_back({
                {"kind", std::move(kind)},
                {"relativePath", relative.generic_string()},
                {"manualPrivacyReviewRequired", manual_review},
            });
        };
        add_if_regular("runtime-state", L"state/diagnostics-summary.json", false);
        add_if_regular("structured-log", L"logs/anomaly-runtime.jsonl", true);

        const auto crashes = root / L"crashes";
        std::filesystem::path newest_metadata;
        std::filesystem::file_time_type newest_time{};
        std::error_code error;
        if (std::filesystem::is_directory(crashes, error) && !error &&
            !IsReparsePoint(crashes)) {
            for (std::filesystem::directory_iterator iterator(crashes, error), end;
                 !error && iterator != end; iterator.increment(error)) {
                if (iterator->path().extension() != L".json" ||
                    !iterator->is_regular_file(error) || error ||
                    IsReparsePoint(iterator->path())) {
                    error.clear();
                    continue;
                }
                ReliableStorage crash_storage(crashes.wstring());
                const auto metadata = crash_storage.Read(
                    iterator->path().filename().wstring(), kMaximumMarkerBytes);
                if (!metadata) continue;
                const Json document = Json::parse(Text(metadata.bytes), nullptr, false);
                if (document.is_discarded() || !document.is_object() ||
                    document.value("processId", 0U) != marker.process_id ||
                    document.value("dumpType", "") != "MiniDumpNormal") {
                    continue;
                }
                const auto modified = iterator->last_write_time(error);
                if (error) {
                    error.clear();
                    continue;
                }
                if (newest_metadata.empty() || modified > newest_time) {
                    newest_metadata = iterator->path();
                    newest_time = modified;
                }
            }
        }
        if (!newest_metadata.empty()) {
            const auto metadata_relative =
                std::filesystem::path(L"crashes") / newest_metadata.filename();
            add_if_regular("minidump-metadata", metadata_relative, true);
            auto dump_relative = metadata_relative;
            dump_relative.replace_extension(L".dmp");
            add_if_regular("minidump", dump_relative, true);
        }

        const Json inventory{
            {"schemaVersion", 1},
            {"incidentId", marker.incident_id},
            {"observedAtUnixSeconds", marker.observation.observed_at_unix_seconds},
            {"processId", marker.process_id},
            {"processExitCode", exit_code},
            {"attribution", {
                {"source", RuntimeFailureSourceName(marker.observation.source)},
                {"runtimeVersion", marker.observation.runtime_version},
                {"profileId", marker.observation.profile_id},
                {"pluginId", marker.observation.plugin_id},
                {"pluginGeneration", marker.observation.plugin_generation},
            }},
            {"artifacts", std::move(artifacts)},
            {"privacy", {
                {"diagnosticBundleUsesAllowlist", true},
                {"minidumpType", "MiniDumpNormal"},
                {"fullMemoryIncluded", false},
                {"handleDataIncluded", false},
                {"minidumpExcludedFromAutomaticBundle", true},
                {"minidumpManualReviewRequired", true},
                {"pluginPrivateConfigurationIncluded", false},
            }},
        };
        const std::string document = inventory.dump(2) + '\n';
        const std::wstring relative = L"state/crash-coordinator/incidents/" +
            std::wstring(marker.incident_id.begin(), marker.incident_id.end()) + L".json";
        static_cast<void>(storage.WriteAtomic(relative, Bytes(document)));
    } catch (...) {
    }
}

}  // namespace

class RuntimeCrashCoordinatorClient::Impl final {
public:
    explicit Impl(RuntimeCrashCoordinatorOptions options)
        : options_(std::move(options)) {}

    ~Impl() {
        static_cast<void>(Mark("stopping"));
        if (monitor_process_ != nullptr) CloseHandle(monitor_process_);
    }

    RuntimeCrashCoordinatorResult Start() noexcept {
        try {
            std::scoped_lock lock(mutex_);
            if (started_) {
                return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, marker_.incident_id};
            }
            if (options_.runtime_root.empty() || options_.monitor_executable.empty() ||
                options_.session_generation == 0 || options_.runtime_version.empty()) {
                return Failure(
                    RuntimeCrashCoordinatorError::InvalidOptions,
                    "crash coordinator options are incomplete");
            }
            options_.runtime_root = std::filesystem::absolute(options_.runtime_root);
            options_.monitor_executable =
                std::filesystem::absolute(options_.monitor_executable);
            if (!IsRegularFile(options_.monitor_executable)) {
                return Failure(
                    RuntimeCrashCoordinatorError::MonitorUnavailable,
                    "crash coordinator executable is unavailable");
            }
            if (!PrepareCoordinatorDirectories(options_.runtime_root)) {
                return Failure(
                    RuntimeCrashCoordinatorError::IoFailure,
                    "crash coordinator state directory could not be created",
                    ERROR_CANNOT_MAKE);
            }
            const DWORD process_id = options_.process_id == 0
                ? GetCurrentProcessId() : options_.process_id;
            const HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
            if (process == nullptr) {
                return Failure(
                    RuntimeCrashCoordinatorError::ProcessUnavailable,
                    "crash coordinator target process is unavailable",
                    GetLastError());
            }
            const auto creation_time = ProcessCreationTime(process);
            CloseHandle(process);
            if (!creation_time) {
                return Failure(
                    RuntimeCrashCoordinatorError::ProcessUnavailable,
                    "crash coordinator target identity is unavailable",
                    GetLastError());
            }

            storage_ = std::make_unique<ReliableStorage>(options_.runtime_root.wstring());
            const auto initialized = storage_->InitializationResult();
            if (!initialized) {
                storage_.reset();
                return Failure(
                    RuntimeCrashCoordinatorError::IoFailure,
                    "crash coordinator storage is unavailable",
                    initialized.win32_error);
            }
            marker_.incident_id = BuildIncidentId(
                process_id, *creation_time, options_.session_generation);
            marker_.process_id = process_id;
            marker_.process_creation_time = *creation_time;
            marker_.session_generation = options_.session_generation;
            marker_.observation.source = RuntimeFailureSource::RuntimeStartup;
            marker_.observation.runtime_version = options_.runtime_version;
            auto written = WriteMarker(*storage_, marker_);
            if (!written.Ok()) return written;

            std::wstring command = QuoteArgument(options_.monitor_executable.wstring()) +
                L" --runtime-root " + QuoteArgument(options_.runtime_root.wstring()) +
                L" --process-id " + std::to_wstring(process_id) +
                L" --creation-time " + std::to_wstring(*creation_time) +
                L" --generation " + std::to_wstring(options_.session_generation) +
                L" --incident " + QuoteArgument(std::wstring(
                    marker_.incident_id.begin(), marker_.incident_id.end()));
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION created{};
            if (CreateProcessW(
                    options_.monitor_executable.c_str(), command.data(), nullptr, nullptr,
                    FALSE, CREATE_NO_WINDOW, nullptr,
                    options_.monitor_executable.parent_path().c_str(),
                    &startup, &created) == FALSE) {
                const DWORD error = GetLastError();
                static_cast<void>(storage_->Delete(MarkerRelativePath(marker_.incident_id)));
                return Failure(
                    RuntimeCrashCoordinatorError::MonitorLaunchFailed,
                    "crash coordinator process could not be started",
                    error,
                    marker_.incident_id);
            }
            CloseHandle(created.hThread);
            monitor_process_ = created.hProcess;
            started_ = true;
            return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, marker_.incident_id};
        } catch (const std::exception& error) {
            return Failure(RuntimeCrashCoordinatorError::IoFailure, error.what());
        } catch (...) {
            return Failure(
                RuntimeCrashCoordinatorError::IoFailure,
                "crash coordinator startup failed unexpectedly");
        }
    }

    RuntimeCrashCoordinatorResult SetFailureContext(
        RuntimeFailureSource source,
        std::string profile_id,
        std::string plugin_id,
        std::uint64_t plugin_generation) noexcept {
        std::scoped_lock lock(mutex_);
        if (!started_ || !storage_ || marker_.stage != "starting") {
            return Failure(
                RuntimeCrashCoordinatorError::InvalidOptions,
                "crash coordinator session is not accepting context",
                ERROR_INVALID_STATE,
                marker_.incident_id);
        }
        marker_.observation.source = source;
        marker_.observation.profile_id = std::move(profile_id);
        marker_.observation.plugin_id = std::move(plugin_id);
        marker_.observation.plugin_generation = plugin_generation;
        return WriteMarker(*storage_, marker_);
    }

    RuntimeCrashCoordinatorResult Mark(std::string stage) noexcept {
        std::scoped_lock lock(mutex_);
        if (!started_ || monitor_finished_ || !storage_) {
            return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, marker_.incident_id};
        }
        if (marker_.stage == "healthy" || marker_.stage == "stopping") {
            return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, marker_.incident_id};
        }
        marker_.stage = std::move(stage);
        return WriteMarker(*storage_, marker_);
    }

    bool WaitForMonitor(std::chrono::milliseconds timeout) noexcept {
        std::scoped_lock lock(mutex_);
        if (monitor_process_ == nullptr) return true;
        const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
        const DWORD milliseconds = bounded == std::chrono::milliseconds::max()
            ? INFINITE
            : static_cast<DWORD>((std::min<std::int64_t>)(
                  bounded.count(), static_cast<std::int64_t>(MAXDWORD - 1)));
        const bool finished =
            WaitForSingleObject(monitor_process_, milliseconds) == WAIT_OBJECT_0;
        if (finished) monitor_finished_ = true;
        return finished;
    }

    bool Started() const noexcept {
        std::scoped_lock lock(mutex_);
        return started_;
    }

    std::string IncidentId() const {
        std::scoped_lock lock(mutex_);
        return marker_.incident_id;
    }

private:
    RuntimeCrashCoordinatorOptions options_;
    mutable std::mutex mutex_;
    std::unique_ptr<ReliableStorage> storage_;
    Marker marker_;
    HANDLE monitor_process_{};
    bool started_{};
    bool monitor_finished_{};
};

RuntimeCrashCoordinatorClient::RuntimeCrashCoordinatorClient(
    RuntimeCrashCoordinatorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

RuntimeCrashCoordinatorClient::~RuntimeCrashCoordinatorClient() = default;

RuntimeCrashCoordinatorResult RuntimeCrashCoordinatorClient::Start() noexcept {
    return impl_->Start();
}

RuntimeCrashCoordinatorResult RuntimeCrashCoordinatorClient::SetFailureContext(
    RuntimeFailureSource source,
    std::string profile_id,
    std::string plugin_id,
    std::uint64_t plugin_generation) noexcept {
    return impl_->SetFailureContext(
        source, std::move(profile_id), std::move(plugin_id), plugin_generation);
}

RuntimeCrashCoordinatorResult RuntimeCrashCoordinatorClient::MarkHealthy() noexcept {
    return impl_->Mark("healthy");
}

RuntimeCrashCoordinatorResult RuntimeCrashCoordinatorClient::MarkStopping() noexcept {
    return impl_->Mark("stopping");
}

bool RuntimeCrashCoordinatorClient::WaitForMonitor(
    std::chrono::milliseconds timeout) noexcept {
    return impl_->WaitForMonitor(timeout);
}

bool RuntimeCrashCoordinatorClient::Started() const noexcept {
    return impl_->Started();
}

std::string RuntimeCrashCoordinatorClient::IncidentId() const {
    return impl_->IncidentId();
}

RuntimeCrashCoordinatorResult RunRuntimeCrashMonitor(
    const RuntimeCrashMonitorOptions& options) noexcept {
    try {
        if (options.runtime_root.empty() || options.process_id == 0 ||
            options.process_creation_time == 0 || options.session_generation == 0 ||
            options.incident_id.empty()) {
            return Failure(
                RuntimeCrashCoordinatorError::InvalidOptions,
                "crash monitor options are incomplete");
        }
        ReliableStorage storage(std::filesystem::absolute(
            options.runtime_root).wstring());
        const auto initialized = storage.InitializationResult();
        if (!initialized) {
            return Failure(
                RuntimeCrashCoordinatorError::IoFailure,
                "crash monitor storage is unavailable",
                initialized.win32_error,
                options.incident_id);
        }
        Marker marker;
        auto loaded = LoadMarker(storage, options.incident_id, marker);
        if (!loaded.Ok()) return loaded;
        if (marker.incident_id != options.incident_id ||
            marker.process_id != options.process_id ||
            marker.process_creation_time != options.process_creation_time ||
            marker.session_generation != options.session_generation) {
            return Failure(
                RuntimeCrashCoordinatorError::MarkerRejected,
                "crash monitor marker identity does not match its process",
                ERROR_INVALID_DATA,
                options.incident_id);
        }

        const HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, options.process_id);
        if (process == nullptr) {
            return Failure(
                RuntimeCrashCoordinatorError::ProcessUnavailable,
                "crash monitor target process is unavailable",
                GetLastError(),
                options.incident_id);
        }
        const auto creation_time = ProcessCreationTime(process);
        if (!creation_time || *creation_time != options.process_creation_time) {
            CloseHandle(process);
            return Failure(
                RuntimeCrashCoordinatorError::ProcessUnavailable,
                "crash monitor rejected a reused process identifier",
                ERROR_INVALID_DATA,
                options.incident_id);
        }

        for (;;) {
            const DWORD wait = WaitForSingleObject(process, 100);
            loaded = LoadMarker(storage, options.incident_id, marker);
            if (!loaded.Ok()) {
                CloseHandle(process);
                return loaded;
            }
            if (marker.stage == "healthy" || marker.stage == "stopping") {
                CloseHandle(process);
                static_cast<void>(storage.Delete(MarkerRelativePath(options.incident_id)));
                return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, options.incident_id};
            }
            if (wait == WAIT_TIMEOUT) continue;
            if (wait != WAIT_OBJECT_0) {
                const DWORD error = GetLastError();
                CloseHandle(process);
                return Failure(
                    RuntimeCrashCoordinatorError::ProcessUnavailable,
                    "crash monitor wait failed",
                    error,
                    options.incident_id);
            }

            DWORD exit_code{};
            const bool exit_available = GetExitCodeProcess(process, &exit_code) != FALSE;
            CloseHandle(process);
            if (!exit_available) {
                return Failure(
                    RuntimeCrashCoordinatorError::ProcessUnavailable,
                    "crash monitor exit status is unavailable",
                    GetLastError(),
                    options.incident_id);
            }
            if (exit_code == ERROR_SUCCESS) {
                static_cast<void>(storage.Delete(MarkerRelativePath(options.incident_id)));
                return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, options.incident_id};
            }

            marker.observation.observed_at_unix_seconds =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            WriteIncidentInventory(
                storage, std::filesystem::absolute(options.runtime_root), marker, exit_code);
            RuntimeRecoveryStore recovery(std::filesystem::absolute(options.runtime_root));
            const auto recorded = recovery.RecordFailure(marker.observation);
            if (!recorded.Ok()) {
                return Failure(
                    RuntimeCrashCoordinatorError::RecoveryRejected,
                    recorded.message.empty()
                        ? "crash monitor incident could not be recorded"
                        : recorded.message,
                    ERROR_INVALID_DATA,
                    options.incident_id);
            }
            static_cast<void>(storage.Delete(MarkerRelativePath(options.incident_id)));
            return {RuntimeCrashCoordinatorError::None, ERROR_SUCCESS, {}, options.incident_id};
        }
    } catch (const std::exception& error) {
        return Failure(RuntimeCrashCoordinatorError::IoFailure, error.what());
    } catch (...) {
        return Failure(
            RuntimeCrashCoordinatorError::IoFailure,
            "crash monitor failed unexpectedly");
    }
}

const char* RuntimeCrashCoordinatorErrorName(
    RuntimeCrashCoordinatorError error) noexcept {
    switch (error) {
    case RuntimeCrashCoordinatorError::None: return "none";
    case RuntimeCrashCoordinatorError::InvalidOptions: return "invalid-options";
    case RuntimeCrashCoordinatorError::ProcessUnavailable: return "process-unavailable";
    case RuntimeCrashCoordinatorError::MarkerRejected: return "marker-rejected";
    case RuntimeCrashCoordinatorError::MonitorUnavailable: return "monitor-unavailable";
    case RuntimeCrashCoordinatorError::MonitorLaunchFailed: return "monitor-launch-failed";
    case RuntimeCrashCoordinatorError::RecoveryRejected: return "recovery-rejected";
    case RuntimeCrashCoordinatorError::IoFailure: return "io-failure";
    }
    return "unknown";
}

}  // namespace anomaly
