#include "anomaly/runtime_crash_coordinator.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

class Handle final {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { if (value_ != nullptr) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
private:
    HANDLE value_{};
};

bool Check(bool condition, std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::wstring Quote(std::wstring_view value) {
    return L"\"" + std::wstring(value) + L"\"";
}

struct Scenario {
    bool healthy{};
    bool stopping{};
    DWORD exit_code{ERROR_PROCESS_ABORTED};
    anomaly::RuntimeFailureSource source{anomaly::RuntimeFailureSource::RuntimeStartup};
};

bool RunScenario(
    const std::filesystem::path& self,
    const std::filesystem::path& monitor,
    const std::filesystem::path& root,
    std::uint64_t generation,
    const Scenario& scenario) {
    const std::wstring event_name = L"Local\\AnomalyCrashCoordinatorTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(generation);
    Handle event(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    if (event.Get() == nullptr) return false;

    std::wstring command = Quote(self.wstring()) + L" --target " +
        Quote(event_name) + L" " + std::to_wstring(scenario.exit_code);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION created{};
    if (CreateProcessW(
            self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, self.parent_path().c_str(), &startup, &created) == FALSE) {
        return false;
    }
    Handle process(created.hProcess);
    Handle thread(created.hThread);

    anomaly::RuntimeCrashCoordinatorClient client({
        root, monitor, created.dwProcessId, generation, "1.0.0"});
    const auto started = client.Start();
    if (!started.Ok()) {
        std::cerr << "coordinator start failed: " << started.message << '\n';
        TerminateProcess(process.Get(), ERROR_PROCESS_ABORTED);
        return false;
    }
    if (scenario.source == anomaly::RuntimeFailureSource::PluginGeneration) {
        const auto context = client.SetFailureContext(
            scenario.source, "nte-current", "example.third-party", 7);
        if (!context.Ok()) return false;
    } else if (scenario.source == anomaly::RuntimeFailureSource::ProfileOverride) {
        const auto context = client.SetFailureContext(
            scenario.source, "nte-local");
        if (!context.Ok()) return false;
    }
    if (scenario.healthy && !client.MarkHealthy().Ok()) return false;
    if (scenario.stopping && !client.MarkStopping().Ok()) return false;
    if (SetEvent(event.Get()) == FALSE || WaitForSingleObject(process.Get(), 5000) != WAIT_OBJECT_0) {
        return false;
    }
    return client.WaitForMonitor(5s);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--target") {
        Handle event(OpenEventW(SYNCHRONIZE, FALSE, argv[2]));
        if (event.Get() == nullptr || WaitForSingleObject(event.Get(), 5000) != WAIT_OBJECT_0) {
            return ERROR_TIMEOUT;
        }
        wchar_t* end{};
        const unsigned long exit_code = wcstoul(argv[3], &end, 10);
        return end != nullptr && *end == L'\0' ? static_cast<int>(exit_code)
                                               : ERROR_INVALID_DATA;
    }
    if (argc != 2) return 2;

    const auto self = std::filesystem::absolute(argv[0]);
    const auto monitor = std::filesystem::absolute(argv[1]);
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-crash-coordinator-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(root / L"logs");
    std::filesystem::create_directories(root / L"state");
    std::ofstream(root / L"logs" / L"anomaly-runtime.jsonl") << "{}\n";
    std::ofstream(root / L"state" / L"diagnostics-summary.json") << "{}\n";
    bool result = true;
    std::uint64_t generation = 1;

    result = Check(
        RunScenario(self, monitor, root, generation++, {.healthy = true}),
        "healthy coordinator scenario failed") && result;
    result = Check(
        RunScenario(self, monitor, root, generation++, {.stopping = true}),
        "stopping coordinator scenario failed") && result;
    result = Check(
        RunScenario(self, monitor, root, generation++, {.exit_code = ERROR_SUCCESS}),
        "normal process exit scenario failed") && result;
    anomaly::RuntimeRecoveryStore store(root);
    result = Check(
        store.Load().error == anomaly::RuntimeRecoveryError::StateUnavailable,
        "healthy, stopping, or normal exit created a recovery incident") && result;

    for (int index = 0; index < 3; ++index) {
        result = Check(
            RunScenario(self, monitor, root, generation++, {}),
            "runtime startup incident scenario failed") && result;
    }
    auto state = store.Load();
    result = Check(
        state.Ok() && state.state->recent_failures.size() == 3 &&
            state.state->safe_mode.minimal_core,
        "external startup incidents did not activate Minimal Core") && result;
    if (state.Ok()) {
        result = Check(
            state.state->recent_failures[0].incident_id !=
                state.state->recent_failures[1].incident_id,
            "distinct process sessions reused an incident identity") && result;
        const auto inventory = root / L"state" / L"crash-coordinator" / L"incidents" /
            (std::wstring(
                 state.state->recent_failures.back().incident_id.begin(),
                 state.state->recent_failures.back().incident_id.end()) + L".json");
        std::ifstream input(inventory, std::ios::binary);
        const std::string document(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        result = Check(
            document.find("\"minidumpType\": \"MiniDumpNormal\"") !=
                    std::string::npos &&
                document.find("\"fullMemoryIncluded\": false") !=
                    std::string::npos &&
                document.find("logs/anomaly-runtime.jsonl") != std::string::npos &&
                document.find(root.string()) == std::string::npos,
            "incident inventory violated the diagnostic privacy allowlist") && result;
    }
    static_cast<void>(store.Restore(anomaly::RuntimeRecoveryAxis::MinimalCore));
    static_cast<void>(store.MarkHealthy());

    for (int index = 0; index < 3; ++index) {
        Scenario plugin;
        plugin.source = anomaly::RuntimeFailureSource::PluginGeneration;
        result = Check(
            RunScenario(self, monitor, root, generation++, plugin),
            "plugin incident scenario failed") && result;
    }
    state = store.Load();
    result = Check(
        state.Ok() && state.state->safe_mode.third_party_plugins_suspended &&
            !state.state->safe_mode.minimal_core &&
            state.state->recent_failures.back().plugin_id == "example.third-party" &&
            state.state->recent_failures.back().plugin_generation == 7,
        "external plugin incidents were not attributed to their generation") && result;

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    return result && !cleanup_error ? 0 : 1;
}
