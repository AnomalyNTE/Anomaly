#include "anomaly/dispatcher.hpp"
#include "anomaly/plugin_file_watcher.hpp"
#include "anomaly/plugin_runtime.hpp"
#include "anomaly/runtime_session.hpp"
#include "anomaly/structured_logger.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint64_t kRuntimeMemoryBudget = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kIdleMemoryDriftBudget = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kReloadMemoryBudget = 10ULL * 1024ULL * 1024ULL;
constexpr double kEmptyTickP95BudgetMilliseconds = 0.15;

std::uint64_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) == FALSE) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.PrivateUsage);
}

std::uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::uint64_t ProcessCpu100ns() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE) return 0;
    return FileTimeValue(kernel) + FileTimeValue(user);
}

double Percentile95Milliseconds(std::vector<Clock::duration> samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t index = (samples.size() * 95U + 99U) / 100U - 1U;
    return std::chrono::duration<double, std::milli>(samples[index]).count();
}

class NoopModule final : public anomaly::PluginModule {
public:
    bool Prepare() override { return true; }
    bool Load(const std::shared_ptr<anomaly::PluginScope>& scope) override {
        scope_ = scope;
        token_ = scope->Register(anomaly::PluginResourceKind::Task, "performance-fixture");
        return token_ != 0;
    }
    bool Start() override { return true; }
    void Stop() override {}
    void Unload() noexcept override { scope_.reset(); }
    void Update(double) override {}
    void Draw() override {}

private:
    std::shared_ptr<anomaly::PluginScope> scope_;
    std::uint64_t token_{};
};

bool Check(bool condition, std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned long idle_window_ms = 1000;
    double idle_budget_percent = 2.0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--idle-window-ms" && index + 1 < argc) {
            idle_window_ms = std::strtoul(argv[++index], nullptr, 10);
        } else if (argument == "--idle-budget-percent" && index + 1 < argc) {
            idle_budget_percent = std::strtod(argv[++index], nullptr);
        } else {
            return 2;
        }
    }
    if (idle_window_ms < 100 || idle_window_ms > 7UL * 24UL * 60UL * 60UL * 1000UL ||
        !std::isfinite(idle_budget_percent) || idle_budget_percent <= 0.0) {
        return 2;
    }

    bool result = true;
    anomaly::Dispatcher dispatcher;
    for (std::size_t warmup = 0; warmup < 1000; ++warmup) {
        static_cast<void>(dispatcher.Pump(1));
    }
    std::vector<Clock::duration> tick_samples;
    tick_samples.reserve(20000);
    for (std::size_t sample = 0; sample < 20000; ++sample) {
        const auto started = Clock::now();
        static_cast<void>(dispatcher.Pump(1));
        tick_samples.push_back(Clock::now() - started);
    }
    const double tick_p95_ms = Percentile95Milliseconds(std::move(tick_samples));
    result = Check(tick_p95_ms <= kEmptyTickP95BudgetMilliseconds,
                   "empty game-tick dispatch exceeded 0.15 ms p95") && result;

    anomaly::RuntimeStartContext start;
    start.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL;
    start.bootstrap_module = GetModuleHandleW(nullptr);
    start.game_module = GetModuleHandleW(nullptr);
    const std::filesystem::path runtime_root = std::filesystem::temp_directory_path() /
        (L"anomaly-performance-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code filesystem_error;
    std::filesystem::remove_all(runtime_root, filesystem_error);
    std::filesystem::create_directories(runtime_root / L"plugins", filesystem_error);
    start.runtime_root = runtime_root;
    start.log_directory = runtime_root / L"logs";
    const std::uint64_t runtime_before = PrivateBytes();
    auto logger = std::make_shared<anomaly::StructuredLogger>(
        anomaly::StructuredLoggerOptions{256, 128, 100ms, "performance-fixture"});
    auto watcher = std::make_shared<anomaly::PluginFileWatcher>(
        runtime_root / L"plugins", anomaly::PluginFileWatcherOptions{100ms, 250ms});
    std::atomic_bool diagnostics_stopped{};
    anomaly::RuntimeSessionOptions runtime_options;
    runtime_options.initialize = [logger, watcher, runtime_root](std::stop_token) {
        std::error_code error;
        std::filesystem::create_directories(runtime_root / L"logs", error);
        if (error || !logger->Start(runtime_root / L"logs" / L"runtime.jsonl")) {
            return static_cast<DWORD>(ERROR_WRITE_FAULT);
        }
        if (!watcher->Start([](std::vector<std::string>) {})) {
            static_cast<void>(logger->Stop());
            return static_cast<DWORD>(ERROR_OPEN_FAILED);
        }
        static_cast<void>(logger->Log(
            anomaly::LogLevel::Info, "runtime", "performance fixture started"));
        return static_cast<DWORD>(ERROR_SUCCESS);
    };
    runtime_options.workers.push_back({"diagnostics", [&diagnostics_stopped](std::stop_token token) {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        condition.wait(lock, token, [] { return false; });
        diagnostics_stopped.store(true);
        return static_cast<DWORD>(ERROR_SUCCESS);
    }});
    runtime_options.shutdown = [logger, watcher] {
        watcher->Stop();
        static_cast<void>(logger->Flush(2s));
        static_cast<void>(logger->Stop());
    };
    anomaly::RuntimeSession session(std::move(start), std::move(runtime_options));
    result = Check(session.Start() == ERROR_SUCCESS, "runtime performance fixture failed to start") && result;
    const auto running_deadline = Clock::now() + 2s;
    while (session.Snapshot().state != ANOMALY_RUNTIME_STATE_RUNNING &&
           Clock::now() < running_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const std::uint64_t runtime_after = PrivateBytes();
    const std::uint64_t runtime_growth = runtime_after > runtime_before
        ? runtime_after - runtime_before
        : 0;
    result = Check(runtime_growth <= kRuntimeMemoryBudget,
                   "empty runtime exceeded the 64 MiB private-memory budget") && result;

    const std::uint64_t cpu_before = ProcessCpu100ns();
    const auto idle_started = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(idle_window_ms));
    const auto idle_elapsed = Clock::now() - idle_started;
    const std::uint64_t cpu_after = ProcessCpu100ns();
    const std::uint64_t runtime_idle = PrivateBytes();
    const double elapsed_seconds = std::chrono::duration<double>(idle_elapsed).count();
    const double cpu_seconds = static_cast<double>(cpu_after - cpu_before) / 10000000.0;
    const double idle_cpu_percent = elapsed_seconds > 0.0 ? cpu_seconds * 100.0 / elapsed_seconds : 100.0;
    result = Check(idle_cpu_percent <= idle_budget_percent,
                    "idle runtime exceeded the configured single-core CPU budget") && result;
    const std::uint64_t runtime_peak = (std::max)(runtime_after, runtime_idle);
    const std::uint64_t runtime_peak_growth = runtime_peak > runtime_before
        ? runtime_peak - runtime_before
        : 0;
    const std::uint64_t idle_memory_drift = runtime_idle > runtime_after
        ? runtime_idle - runtime_after
        : 0;
    result = Check(runtime_peak_growth <= kRuntimeMemoryBudget,
                   "idle runtime exceeded the 64 MiB private-memory budget") && result;
    result = Check(idle_memory_drift <= kIdleMemoryDriftBudget,
                   "idle runtime exceeded the 16 MiB memory-drift budget") && result;

    session.RequestStop();
    result = Check(session.WaitForStop(2s), "runtime performance fixture failed to stop") && result;
    session.Join();
    const auto logger_stats = logger->Stats();
    result = Check(diagnostics_stopped.load(), "diagnostics worker did not observe stop") && result;
    result = Check(!watcher->Running(), "plugin watcher remained active after shutdown") && result;
    result = Check(logger_stats.accepted >= 1 && logger_stats.processed == logger_stats.accepted,
                   "structured logger did not drain accepted records") && result;
    std::filesystem::remove_all(runtime_root, filesystem_error);

    auto ledger = std::make_shared<anomaly::ResourceLedger>();
    anomaly::PluginRuntime runtime("performance.reload", ledger, 1s);
    const auto factory = [] { return std::make_shared<NoopModule>(); };
    result = Check(runtime.Activate(factory), "reload performance fixture failed to activate") && result;
    const std::uint64_t reload_before = PrivateBytes();
    for (std::size_t reload = 0; reload < 100; ++reload) {
        if (!runtime.Reload(factory)) {
            result = Check(false, "100-reload performance fixture failed") && result;
            break;
        }
    }
    result = Check(runtime.Stop(), "reload performance fixture failed to stop") && result;
    const std::uint64_t reload_after = PrivateBytes();
    const std::uint64_t reload_growth = reload_after > reload_before
        ? reload_after - reload_before
        : 0;
    result = Check(reload_growth <= kReloadMemoryBudget,
                   "100 reloads exceeded the 10 MiB private-memory budget") && result;
    result = Check(ledger->Snapshot().empty(), "100 reloads left resources in the ledger") && result;

    std::cout << "{\"tickP95Ms\":" << tick_p95_ms
              << ",\"runtimePrivateGrowthBytes\":" << runtime_growth
              << ",\"runtimePeakGrowthBytes\":" << runtime_peak_growth
              << ",\"idleMemoryDriftBytes\":" << idle_memory_drift
              << ",\"idleCpuPercent\":" << idle_cpu_percent
              << ",\"loggerRecords\":" << logger_stats.processed
              << ",\"reloadPrivateGrowthBytes\":" << reload_growth
              << ",\"reloads\":100,\"resourcesAfterStop\":"
              << ledger->Snapshot().size() << "}\n";
    return result ? 0 : 1;
}
