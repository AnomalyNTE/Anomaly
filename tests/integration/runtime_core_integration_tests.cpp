#include "anomaly/core_api.h"
#include "anomaly/runtime_recovery.hpp"
#include "anomaly/sdk/version.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;

class RuntimeFixture {
public:
    ~RuntimeFixture() {
        if (module_ != nullptr) {
            if (request_stop_ != nullptr) static_cast<void>(request_stop_());
            if (wait_for_stop_ != nullptr) static_cast<void>(wait_for_stop_(2000));
            FreeLibrary(module_);
        }
        if (external_stop_event_ != nullptr) CloseHandle(external_stop_event_);
        if (!runtime_root_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(runtime_root_, error);
        }
    }

    RuntimeFixture(const RuntimeFixture&) = delete;
    RuntimeFixture& operator=(const RuntimeFixture&) = delete;

    RuntimeFixture() = default;

    bool CreateRuntimeRoot() {
        std::error_code error;
        const auto temporary_directory = std::filesystem::temp_directory_path(error);
        if (error) {
            std::cerr << "temp_directory_path failed: " << error.message() << '\n';
            return false;
        }

        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        pipe_prefix_ascii_ = "AnomalyCoreIntegration-" +
            std::to_string(GetCurrentProcessId()) + "-" + std::to_string(timestamp);
        pipe_prefix_.assign(pipe_prefix_ascii_.begin(), pipe_prefix_ascii_.end());
        runtime_root_ = temporary_directory / std::filesystem::path(pipe_prefix_);
        log_directory_ = runtime_root_ / L"logs";

        if (!std::filesystem::create_directory(runtime_root_, error) || error) {
            std::cerr << "create runtime root failed: " << error.message() << '\n';
            runtime_root_.clear();
            return false;
        }
        if (!std::filesystem::create_directories(log_directory_, error) || error) {
            std::cerr << "create log directory failed: " << error.message() << '\n';
            return false;
        }

        std::ofstream configuration(runtime_root_ / L"anomaly.ini", std::ios::binary);
        configuration << "[Analyzer]\n"
                      << "PipePrefix=" << pipe_prefix_ascii_ << "\n"
                      << "MaxScanResults=4\n\n"
                      << "[Platform]\n"
                      << "Enabled=0\n"
                      << "Visible=0\n"
                      << "Embedded=0\n"
                      << "AttachToProcessWindow=0\n"
                      << "PluginDirectory=plugins\n";
        configuration.close();
        if (!configuration) {
            std::cerr << "write anomaly.ini failed\n";
            return false;
        }
        std::ofstream repository(runtime_root_ / L"repository.json", std::ios::binary);
        repository << R"({
          "schemaVersion":1,
          "enabled":false,
          "allowFileSources":false,
          "withdrawalPolicy":"block-new",
          "freshness":{
            "maximumClockSkewSeconds":300,
            "maximumIndexAgeSeconds":86400,
            "maximumOfflineAgeSeconds":604800,
            "downgradePolicy":"reject"
          },
          "sources":[],
          "trustKeys":[]
        })";
        repository.close();
        if (!repository) {
            std::cerr << "write repository.json failed\n";
            return false;
        }
        std::ofstream plugin_repositories(
            runtime_root_ / L"plugin-repositories.json", std::ios::binary);
        plugin_repositories << R"({
          "schemaVersion":1,
          "enabled":false,
          "allowInsecureSources":false,
          "repositories":[]
        })";
        plugin_repositories.close();
        if (!plugin_repositories) {
            std::cerr << "write plugin-repositories.json failed\n";
            return false;
        }
        return true;
    }

    bool Load(const std::filesystem::path& core_path) {
        module_ = LoadLibraryW(core_path.c_str());
        if (module_ == nullptr) {
            std::cerr << "LoadLibraryW failed: " << GetLastError() << '\n';
            return false;
        }

        start_ = Resolve<AnomalyStartFn>(ANOMALY_CORE_START_ENTRY);
        get_state_ = Resolve<AnomalyGetStateFn>(ANOMALY_CORE_GET_STATE_ENTRY);
        request_stop_ = Resolve<AnomalyRequestStopFn>(ANOMALY_CORE_REQUEST_STOP_ENTRY);
        wait_for_stop_ = Resolve<AnomalyWaitForStopFn>(ANOMALY_CORE_WAIT_FOR_STOP_ENTRY);
        return start_ != nullptr && get_state_ != nullptr && request_stop_ != nullptr &&
            wait_for_stop_ != nullptr;
    }

    bool ValidateContract() const {
        if (start_(nullptr) != ERROR_INVALID_PARAMETER ||
            get_state_(nullptr) != ERROR_INVALID_PARAMETER) {
            std::cerr << "null ABI arguments were not rejected\n";
            return false;
        }

        AnomalyStartInfo invalid_start = StartInfo();
        invalid_start.struct_size = ANOMALY_START_INFO_V1_SIZE - 1;
        if (start_(&invalid_start) != ERROR_INSUFFICIENT_BUFFER) {
            std::cerr << "short start info was not rejected\n";
            return false;
        }
        invalid_start = StartInfo();
        ++invalid_start.bootstrap_abi_version;
        if (start_(&invalid_start) != ERROR_REVISION_MISMATCH) {
            std::cerr << "bootstrap ABI mismatch was not rejected\n";
            return false;
        }
        invalid_start = StartInfo();
        invalid_start.flags = 1;
        if (start_(&invalid_start) != ERROR_INVALID_FLAGS) {
            std::cerr << "unknown bootstrap flags were not rejected\n";
            return false;
        }
        invalid_start = StartInfo();
        invalid_start.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL + 1;
        if (start_(&invalid_start) != ERROR_INVALID_PARAMETER) {
            std::cerr << "unknown bootstrap type was not rejected\n";
            return false;
        }

        AnomalyRuntimeStateInfo state_info{};
        state_info.struct_size = ANOMALY_RUNTIME_STATE_INFO_V1_SIZE - 1;
        state_info.state_info_version = ANOMALY_RUNTIME_STATE_INFO_VERSION;
        if (get_state_(&state_info) != ERROR_INSUFFICIENT_BUFFER) {
            std::cerr << "short state info was not rejected\n";
            return false;
        }
        state_info.struct_size = ANOMALY_RUNTIME_STATE_INFO_V1_SIZE;
        ++state_info.state_info_version;
        if (get_state_(&state_info) != ERROR_REVISION_MISMATCH) {
            std::cerr << "state ABI mismatch was not rejected\n";
            return false;
        }

        struct ExtendedStateInfo {
            AnomalyRuntimeStateInfo state;
            std::uint64_t tail;
        } extended{{ANOMALY_RUNTIME_STATE_INFO_V1_SIZE + sizeof(std::uint64_t),
                    ANOMALY_RUNTIME_STATE_INFO_VERSION},
                   0x1122334455667788ULL};
        if (get_state_(&extended.state) != ERROR_SUCCESS ||
            extended.state.state != ANOMALY_RUNTIME_STATE_DORMANT ||
            extended.state.session_generation != 0 ||
            extended.tail != 0x1122334455667788ULL) {
            std::cerr << "extended state prefix contract failed\n";
            return false;
        }
        return true;
    }

    bool CreateExternalStopEvent() {
        external_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (external_stop_event_ != nullptr) return true;
        std::cerr << "CreateEventW failed: " << GetLastError() << '\n';
        return false;
    }

    bool EnablePluginRepository() const {
        const auto list = runtime_root_ / L"integration-pluginmaster.json";
        std::ofstream list_output(list, std::ios::binary | std::ios::trunc);
        list_output << "[]\n";
        list_output.close();
        if (!list_output) return false;

        const std::string list_uri = "file:///" + list.generic_string();
        const nlohmann::json repository{
            {"schemaVersion", 1},
            {"enabled", true},
            {"allowInsecureSources", true},
            {"repositories", nlohmann::json::array({{
                {"url", list_uri}, {"enabled", true}}})}};
        std::ofstream output(
            runtime_root_ / L"plugin-repositories.json", std::ios::binary | std::ios::trunc);
        output << repository.dump() << '\n';
        return static_cast<bool>(output);
    }

    bool DisablePluginRepository() const {
        std::ofstream output(
            runtime_root_ / L"plugin-repositories.json", std::ios::binary | std::ios::trunc);
        output << R"({
          "schemaVersion":1,
          "enabled":false,
          "allowInsecureSources":false,
          "repositories":[]
        })";
        return static_cast<bool>(output);
    }

    AnomalyStartInfo StartInfo(HANDLE external_stop_event = nullptr) const {
        AnomalyStartInfo start_info{};
        start_info.struct_size = ANOMALY_START_INFO_V1_SIZE;
        start_info.bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION;
        start_info.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL;
        start_info.bootstrap_module = GetModuleHandleW(nullptr);
        start_info.game_module = GetModuleHandleW(nullptr);
        start_info.runtime_root = runtime_root_.c_str();
        start_info.log_directory = log_directory_.c_str();
        start_info.external_stop_event = external_stop_event;
        return start_info;
    }

    bool Start(const AnomalyStartInfo& start_info, std::string_view phase) const {
        const DWORD result = start_(&start_info);
        if (result == ERROR_SUCCESS) return true;
        AnomalyRuntimeStateInfo state_info{};
        const DWORD state_result = GetState(state_info);
        std::cerr << phase << " AnomalyStart was not accepted: " << result
                  << " state_result=" << state_result
                  << " state=" << state_info.state
                  << " last_error=" << state_info.last_error
                  << " generation=" << state_info.session_generation << '\n';
        return false;
    }

    bool DuplicateStartIsRejected(const AnomalyStartInfo& start_info) const {
        const DWORD result = start_(&start_info);
        if (result == ERROR_ALREADY_INITIALIZED) return true;
        std::cerr << "duplicate AnomalyStart returned: " << result << '\n';
        return false;
    }

    bool RunningWaitTimesOut() const {
        const DWORD result = wait_for_stop_(0);
        if (result == ERROR_TIMEOUT) return true;
        std::cerr << "zero-time running wait returned: " << result << '\n';
        return false;
    }

    bool WaitForState(
        AnomalyRuntimeState expected,
        AnomalyRuntimeStateInfo& state_info,
        std::chrono::milliseconds timeout = 2s) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        DWORD query_result = ERROR_SUCCESS;
        do {
            query_result = GetState(state_info);
            if (query_result == ERROR_SUCCESS && state_info.state == expected) return true;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);

        std::cerr << "state wait failed: expected=" << expected
                  << " actual=" << state_info.state
                  << " query_result=" << query_result
                  << " last_error=" << state_info.last_error
                  << " generation=" << state_info.session_generation << '\n';
        return false;
    }

    bool Ping() const {
        const std::string response = QueryPipe("ping");
        if (response.find("\"ok\":true") != std::string::npos) return true;
        std::cerr << "ping response was invalid: "
                  << (response.empty() ? "<empty>" : response) << '\n';
        return false;
    }

    bool StatusHasServices(std::string_view repository_state) const {
        const std::string response_text = QueryPipe("status");
        const auto response = nlohmann::json::parse(response_text, nullptr, false);
        if (response.is_discarded() || !response.is_object() ||
            !response.value("ok", false) || !response.contains("result") ||
            !response["result"].is_object()) {
            std::cerr << "runtime service diagnostics envelope was invalid: "
                      << (response_text.empty() ? "<empty>" : response_text) << '\n';
            return false;
        }
        const auto& result = response["result"];
        if (!result.contains("runtime") || !result["runtime"].is_object()) return false;
        const auto& runtime = result["runtime"];
        if (!runtime.contains("services") || !runtime["services"].is_array()) return false;
        const auto find_service = [&runtime](std::string_view id) -> const nlohmann::json* {
            const auto found = std::ranges::find(
                runtime["services"], id,
                [](const nlohmann::json& service) { return service.value("id", ""); });
            return found == runtime["services"].end() ? nullptr : &*found;
        };
        const auto ready = [](const nlohmann::json* service) {
            return service != nullptr && service->value("state", "") == "Ready";
        };
        const auto provided_ready = [&ready](const nlohmann::json* service) {
            return ready(service) && service->value("lifetime", "") == "Provided";
        };
        const auto singleton_ready = [&ready](
            const nlohmann::json* service, std::string_view affinity) {
            return ready(service) && service->value("lifetime", "") == "Singleton" &&
                service->value("startup", "") == "Blocking" &&
                service->value("affinity", "") == affinity;
        };
        const auto has_ready_dependency = [](const nlohmann::json* service, std::string_view id) {
            if (service == nullptr || !service->contains("dependencies") ||
                !(*service)["dependencies"].is_array()) {
                return false;
            }
            return std::ranges::any_of(
                (*service)["dependencies"], [id](const nlohmann::json& dependency) {
                    return dependency.value("id", "") == id &&
                        dependency.value("minimum_version", 0U) == 1 &&
                        !dependency.value("optional", true) &&
                        dependency.value("resolved", false) &&
                        dependency.value("resolved_version", 0U) == 1 &&
                        dependency.value("state", "") == "Ready";
                });
        };
        const auto* runtime_info = find_service("anomaly.runtime.info");
        const auto* config = find_service("anomaly.config");
        const auto* module_memory = find_service("anomaly.internal.module-memory");
        const auto* pattern = find_service("anomaly.internal.pattern");
        const auto* pipe = find_service("anomaly.internal.pipe");
        const auto* profile = find_service("anomaly.internal.nte-profile");
        const auto* plugin_host = find_service("anomaly.plugin.host");
        const auto* lifecycle_dispatcher = find_service("anomaly.dispatchers.lifecycle");
        const auto* worker_dispatcher = find_service("anomaly.dispatchers.worker");
        const auto* game_dispatcher = find_service("anomaly.dispatchers.game");
        const auto* render_dispatcher = find_service("anomaly.dispatchers.render");
        const auto* platform_host = find_service("anomaly.platform.host");
        const auto* platform_input = find_service("anomaly.platform.input");
        const auto* platform_ui = find_service("anomaly.platform.ui");
        const auto* repository = find_service("anomaly.repository.coordinator");
        const bool valid = runtime.value("built", false) &&
            runtime.contains("startup_active") &&
            runtime.value("blocking_startup_complete", false) &&
            runtime.contains("async_startup_complete") && runtime.contains("async_startup_error") &&
            runtime.value("failures", nlohmann::json::array()).empty() &&
            runtime.value("repository", nlohmann::json::object()).value("state", "") ==
                repository_state &&
            runtime.value("plugin_diagnostics", nlohmann::json::object()) ==
                nlohmann::json{{"schemaVersion", 1}, {"plugins", nlohmann::json::array()}} &&
            ready(runtime_info) && ready(config) && ready(module_memory) && ready(pattern) &&
            singleton_ready(pipe, "Worker") && ready(profile) &&
            singleton_ready(plugin_host, "Lifecycle") &&
            provided_ready(lifecycle_dispatcher) && provided_ready(worker_dispatcher) &&
            provided_ready(game_dispatcher) && provided_ready(render_dispatcher) &&
            provided_ready(platform_host) && provided_ready(platform_input) &&
            provided_ready(platform_ui) && singleton_ready(repository, "Worker") &&
            has_ready_dependency(config, "anomaly.runtime.info") &&
            has_ready_dependency(pattern, "anomaly.internal.module-memory") &&
            has_ready_dependency(profile, "anomaly.repository.coordinator") &&
            has_ready_dependency(pipe, "anomaly.config") &&
            has_ready_dependency(pipe, "anomaly.internal.module-memory") &&
            has_ready_dependency(pipe, "anomaly.internal.pattern") &&
            has_ready_dependency(plugin_host, "anomaly.config") &&
            has_ready_dependency(plugin_host, "anomaly.internal.nte-profile") &&
            has_ready_dependency(plugin_host, "anomaly.repository.coordinator") &&
            has_ready_dependency(platform_host, "anomaly.plugin.host") &&
            has_ready_dependency(platform_input, "anomaly.platform.host") &&
            has_ready_dependency(platform_ui, "anomaly.platform.host") &&
            has_ready_dependency(repository, "anomaly.config");
        if (!valid) {
            std::cerr << "runtime service diagnostics were incomplete: " << response_text << '\n';
        }
        return valid;
    }

    bool DiagnosticsSummaryHasState(
        std::string_view runtime_state,
        std::string_view repository_state) const {
        std::ifstream input(
            runtime_root_ / L"state" / L"diagnostics-summary.json", std::ios::binary);
        const std::string summary(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const bool valid = !summary.empty() &&
            summary.find("\"schemaVersion\":1") != std::string::npos &&
            summary.find(
                "\"runtimeVersion\":\"" ANOMALY_SDK_VERSION_STRING "\"") !=
                std::string::npos &&
            summary.find(
                "\"runtimeState\":\"" + std::string(runtime_state) + "\"") !=
                std::string::npos &&
            summary.find(
                "\"repository\":{\"schemaVersion\":1,\"state\":\"" +
                std::string(repository_state) + "\"") !=
                std::string::npos &&
            summary.find("\"plugins\":") != std::string::npos &&
            summary.find("\"buildId\":") != std::string::npos &&
            summary.find("\"profileHash\":") != std::string::npos;
        if (!valid) {
            std::cerr << "diagnostics summary was incomplete for state " << runtime_state
                      << ": " << (summary.empty() ? "<empty>" : summary) << '\n';
        }
        return valid;
    }

    bool StructuredLogHasLifecycle(std::size_t expected_sessions) const {
        std::ifstream input(
            log_directory_ / L"anomaly-runtime.jsonl", std::ios::binary);
        const std::string records(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto count = [&records](std::string_view needle) {
            std::size_t result{};
            std::size_t offset{};
            while ((offset = records.find(needle, offset)) != std::string::npos) {
                ++result;
                offset += needle.size();
            }
            return result;
        };
        std::unordered_set<std::string> session_ids;
        constexpr std::string_view session_marker = "\"session_id\":\"";
        std::size_t session_offset{};
        while ((session_offset = records.find(session_marker, session_offset)) !=
               std::string::npos) {
            session_offset += session_marker.size();
            const std::size_t end = records.find('"', session_offset);
            if (end == std::string::npos) break;
            session_ids.emplace(records.substr(session_offset, end - session_offset));
            session_offset = end + 1;
        }
        const bool valid =
            count("\"event_id\":\"runtime.start\"") == expected_sessions &&
            count("\"event_id\":\"profile.ready\"") == expected_sessions &&
            count("\"event_id\":\"runtime.stop\"") == expected_sessions &&
            count("\"event_id\":\"plugin.host.ready\"") == expected_sessions &&
            count("\"event_id\":\"plugin.host.stopped\"") == expected_sessions &&
            count("\"component\":\"runtime\"") >= expected_sessions * 3 &&
            count("\"thread_domain\":\"lifecycle\"") >= expected_sessions * 3 &&
            session_ids.size() == expected_sessions;
        if (!valid) {
            std::cerr << "structured runtime lifecycle log count was incomplete: "
                      << (records.empty() ? "<empty>" : records) << '\n';
        }
        return valid;
    }

    bool ActivateMinimalCoreRecovery() const {
        anomaly::RuntimeRecoveryStore store(runtime_root_);
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        anomaly::RuntimeRecoveryResult recorded;
        for (std::uint64_t index = 0; index < 3; ++index) {
            recorded = store.RecordFailure({
                now - (2 - index),
                "runtime-core-integration-" + std::to_string(index),
                anomaly::RuntimeFailureSource::RuntimeStartup,
                ANOMALY_SDK_VERSION_STRING,
                {}, {}, 0});
        }
        return recorded.Ok() && recorded.state->safe_mode.minimal_core;
    }

    bool RestoreMinimalCoreRecovery() const {
        anomaly::RuntimeRecoveryStore store(runtime_root_);
        const auto restored = store.Restore(anomaly::RuntimeRecoveryAxis::MinimalCore);
        return restored.Ok() && !restored.state->safe_mode.minimal_core;
    }

    bool WriteInvalidRecoveryState() const {
        std::ofstream output(
            runtime_root_ / L"state" / L"runtime-recovery.json",
            std::ios::binary | std::ios::trunc);
        output << "{invalid";
        return static_cast<bool>(output);
    }

    bool RemoveRecoveryState() const {
        std::error_code error;
        return std::filesystem::remove(
                   runtime_root_ / L"state" / L"runtime-recovery.json", error) && !error;
    }

    bool DiagnosticsSummaryHasRecovery(bool conservative) const {
        std::ifstream input(
            runtime_root_ / L"state" / L"diagnostics-summary.json", std::ios::binary);
        const std::string summary(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return summary.find(
                   conservative ? "\"state\":\"conservative\""
                                : "\"state\":\"safe-mode\"") != std::string::npos &&
            summary.find("\"minimalCore\":true") != std::string::npos &&
            summary.find("\"profile\":{\"state\":\"suspended\"") !=
                std::string::npos;
    }

    bool PipeIsUnavailable() const {
        const std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\" + pipe_prefix_ + L"-" +
            std::to_wstring(GetCurrentProcessId());
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            if (WaitNamedPipeW(pipe_name.c_str(), 0) == FALSE &&
                GetLastError() == ERROR_FILE_NOT_FOUND) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        std::cerr << "runtime pipe remained available after shutdown\n";
        return false;
    }

    bool RequestStopAndWait() const {
        const DWORD request_result = request_stop_();
        if (request_result != ERROR_SUCCESS) {
            std::cerr << "AnomalyRequestStop failed: " << request_result << '\n';
            return false;
        }
        const DWORD wait_result = wait_for_stop_(2000);
        if (wait_result == ERROR_SUCCESS) return true;
        std::cerr << "AnomalyWaitForStop failed: " << wait_result << '\n';
        return false;
    }

    bool SignalExternalStopAndWait() const {
        if (SetEvent(external_stop_event_) == FALSE) {
            std::cerr << "SetEvent failed: " << GetLastError() << '\n';
            return false;
        }
        const DWORD wait_result = wait_for_stop_(2000);
        if (wait_result == ERROR_SUCCESS) return true;
        std::cerr << "external-event AnomalyWaitForStop failed: " << wait_result << '\n';
        return false;
    }

    HANDLE external_stop_event() const noexcept { return external_stop_event_; }

    bool ReleaseAndClean() {
        bool success = true;
        if (external_stop_event_ != nullptr) {
            success = CloseHandle(external_stop_event_) != FALSE && success;
            external_stop_event_ = nullptr;
        }
        if (module_ != nullptr) {
            if (FreeLibrary(module_) == FALSE) {
                std::cerr << "FreeLibrary failed: " << GetLastError() << '\n';
                success = false;
            }
            module_ = nullptr;
        }

        std::error_code error;
        std::filesystem::remove_all(runtime_root_, error);
        if (error || std::filesystem::exists(runtime_root_)) {
            std::cerr << "remove runtime root failed: " << error.message() << '\n';
            success = false;
        } else {
            runtime_root_.clear();
        }
        return success;
    }

private:
    std::string QueryPipe(std::string_view request) const {
        const std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\" + pipe_prefix_ + L"-" +
            std::to_wstring(GetCurrentProcessId());
        HANDLE pipe = INVALID_HANDLE_VALUE;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            pipe = CreateFileW(
                pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            if (GetLastError() == ERROR_PIPE_BUSY) {
                static_cast<void>(WaitNamedPipeW(pipe_name.c_str(), 50));
            } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
                std::this_thread::sleep_for(10ms);
            } else {
                break;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        if (pipe == INVALID_HANDLE_VALUE) return {};
        std::string request_line = nlohmann::json{
            {"protocol", "anomaly.diagnostics"},
            {"version", 1},
            {"type", "request"},
            {"id", "runtime-core-integration"},
            {"command", request},
        }.dump();
        request_line.push_back('\n');
        DWORD written{};
        if (WriteFile(pipe, request_line.data(), static_cast<DWORD>(request_line.size()),
                      &written, nullptr) == FALSE ||
            written != static_cast<DWORD>(request_line.size())) {
            CloseHandle(pipe);
            return {};
        }
        std::string response;
        std::array<char, 4096> buffer{};
        DWORD read{};
        while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) !=
                   FALSE && read != 0) {
            response.append(buffer.data(), read);
        }
        CloseHandle(pipe);
        return response;
    }

    template <typename Function>
    Function Resolve(const char* name, bool required = true) const {
        const auto function = reinterpret_cast<Function>(GetProcAddress(module_, name));
        if (required && function == nullptr) {
            std::cerr << "required export is missing: " << name << '\n';
        }
        return function;
    }

    DWORD GetState(AnomalyRuntimeStateInfo& state_info) const {
        state_info = {};
        state_info.struct_size = ANOMALY_RUNTIME_STATE_INFO_V1_SIZE;
        state_info.state_info_version = ANOMALY_RUNTIME_STATE_INFO_VERSION;
        return get_state_(&state_info);
    }

    std::filesystem::path runtime_root_;
    std::filesystem::path log_directory_;
    std::string pipe_prefix_ascii_;
    std::wstring pipe_prefix_;
    HMODULE module_{};
    HANDLE external_stop_event_{};
    AnomalyStartFn start_{};
    AnomalyGetStateFn get_state_{};
    AnomalyRequestStopFn request_stop_{};
    AnomalyWaitForStopFn wait_for_stop_{};
};

bool CheckStopped(
    const RuntimeFixture& fixture,
    std::uint64_t expected_generation,
    const char* message) {
    AnomalyRuntimeStateInfo state_info{};
    if (!fixture.WaitForState(ANOMALY_RUNTIME_STATE_STOPPED, state_info, 0ms)) {
        std::cerr << message << '\n';
        return false;
    }
    if (state_info.session_generation == expected_generation) return true;
    std::cerr << "stopped generation changed: expected=" << expected_generation
              << " actual=" << state_info.session_generation << '\n';
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "usage: runtime_core_integration_tests <Anomaly.Core.dll>\n";
        return 2;
    }

    RuntimeFixture fixture;
    if (!fixture.CreateRuntimeRoot()) return 3;
    if (!fixture.Load(argv[1])) return 4;
    if (!fixture.ValidateContract()) return 5;

    const AnomalyStartInfo first_start = fixture.StartInfo();
    if (!fixture.Start(first_start, "first")) return 6;

    AnomalyRuntimeStateInfo first_running{};
    if (!fixture.WaitForState(ANOMALY_RUNTIME_STATE_RUNNING, first_running)) return 7;
    if (first_running.session_generation == 0) {
        std::cerr << "first session generation is zero\n";
        return 8;
    }
    if (!fixture.DuplicateStartIsRejected(first_start)) return 9;
    if (!fixture.RunningWaitTimesOut()) return 10;
    if (!fixture.Ping()) return 11;
    if (!fixture.StatusHasServices("disabled")) return 12;
    if (!fixture.DiagnosticsSummaryHasState("running", "disabled")) return 13;
    if (!fixture.RequestStopAndWait()) return 14;
    if (!fixture.DiagnosticsSummaryHasState("stopped", "disabled")) return 15;
    if (!fixture.StructuredLogHasLifecycle(1)) return 28;
    if (!fixture.PipeIsUnavailable()) return 16;
    if (!CheckStopped(fixture, first_running.session_generation, "first session is not Stopped")) {
        return 17;
    }

    if (!fixture.EnablePluginRepository()) return 34;
    if (!fixture.CreateExternalStopEvent()) return 18;
    const AnomalyStartInfo second_start = fixture.StartInfo(fixture.external_stop_event());
    if (!fixture.Start(second_start, "second")) return 19;

    AnomalyRuntimeStateInfo second_running{};
    if (!fixture.WaitForState(ANOMALY_RUNTIME_STATE_RUNNING, second_running)) return 20;
    if (second_running.session_generation <= first_running.session_generation) {
        std::cerr << "session generation did not increase: first="
                  << first_running.session_generation << " second="
                  << second_running.session_generation << '\n';
        return 21;
    }
    if (!fixture.StatusHasServices("configured")) return 35;
    if (!fixture.DiagnosticsSummaryHasState("running", "configured")) return 22;
    if (!fixture.SignalExternalStopAndWait()) return 23;
    if (!fixture.DiagnosticsSummaryHasState("stopped", "configured")) return 24;
    if (!fixture.StructuredLogHasLifecycle(2)) return 29;
    if (!fixture.PipeIsUnavailable()) return 25;
    if (!CheckStopped(
            fixture, second_running.session_generation, "external event session is not Stopped")) {
        return 26;
    }
    if (!fixture.DisablePluginRepository()) return 48;

    if (!fixture.ActivateMinimalCoreRecovery()) return 36;
    const AnomalyStartInfo safe_start = fixture.StartInfo();
    if (!fixture.Start(safe_start, "safe-mode")) return 37;
    AnomalyRuntimeStateInfo safe_running{};
    if (!fixture.WaitForState(ANOMALY_RUNTIME_STATE_RUNNING, safe_running)) return 38;
    if (!fixture.DiagnosticsSummaryHasRecovery(false)) return 39;
    if (!fixture.RequestStopAndWait()) return 40;
    if (!fixture.RestoreMinimalCoreRecovery()) return 41;

    if (!fixture.WriteInvalidRecoveryState()) return 42;
    const AnomalyStartInfo conservative_start = fixture.StartInfo();
    if (!fixture.Start(conservative_start, "conservative-safe-mode")) return 43;
    AnomalyRuntimeStateInfo conservative_running{};
    if (!fixture.WaitForState(
            ANOMALY_RUNTIME_STATE_RUNNING, conservative_running)) return 44;
    if (!fixture.DiagnosticsSummaryHasRecovery(true)) return 45;
    if (!fixture.RequestStopAndWait()) return 46;
    if (!fixture.RemoveRecoveryState()) return 47;

    return fixture.ReleaseAndClean() ? 0 : 27;
}
