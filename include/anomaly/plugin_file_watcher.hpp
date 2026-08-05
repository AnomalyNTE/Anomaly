#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace anomaly {

struct PluginFileWatcherOptions {
    std::chrono::milliseconds poll_interval{100};
    std::chrono::milliseconds debounce{750};
};

struct PluginFileWatcherDiagnostics {
    std::uint64_t notifications{};
    std::uint64_t scans{};
    std::uint64_t changed_packages{};
    std::uint64_t total_scan_nanoseconds{};
    std::uint64_t maximum_scan_nanoseconds{};
};

class PluginFileWatcher final {
public:
    using Callback = std::function<void(std::vector<std::string>)>;
    using Clock = std::chrono::steady_clock;

    PluginFileWatcher(
        std::filesystem::path plugin_root,
        PluginFileWatcherOptions options = {});
    ~PluginFileWatcher();

    PluginFileWatcher(const PluginFileWatcher&) = delete;
    PluginFileWatcher& operator=(const PluginFileWatcher&) = delete;

    bool Start(Callback callback);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    void SetDiagnosticsEnabled(bool enabled) noexcept;
    [[nodiscard]] PluginFileWatcherDiagnostics Diagnostics() const noexcept;
    void ResetBaseline() noexcept;

    // Deterministic seam used by tests and diagnostic hosts.
    [[nodiscard]] std::vector<std::string> PollForTests(Clock::time_point now);

private:
    struct Observation {
        std::string signature;
        Clock::time_point changed_at{};
        bool pending{};
    };

    [[nodiscard]] std::unordered_map<std::string, std::string> Scan() const;
    [[nodiscard]] std::vector<std::string> PollLocked(Clock::time_point now);
    void Run(std::stop_token stop_token, void* change_handle);

    std::filesystem::path plugin_root_;
    PluginFileWatcherOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Observation> observations_;
    bool initialized_{};
    Callback callback_;
    std::jthread worker_;
    std::atomic_bool diagnostics_enabled_{};
    mutable std::atomic_uint64_t notifications_{};
    mutable std::atomic_uint64_t scans_{};
    mutable std::atomic_uint64_t changed_packages_{};
    mutable std::atomic_uint64_t total_scan_nanoseconds_{};
    mutable std::atomic_uint64_t maximum_scan_nanoseconds_{};
};

}  // namespace anomaly
