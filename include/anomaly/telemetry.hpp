#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace anomaly {

// Internal host service, not part of the plugin ABI. No diagnostics/log inputs.
struct TelemetryOptions {
    bool enabled{true};
    std::wstring endpoint{L"https://telemetry.anomalynte.com/v1/collect"};
    bool allow_loopback_http{};
    // Empty selects the per-Windows-user LocalAppData identity shared by installations.
    std::filesystem::path identity_file;
    // Effective host UI locale, never an OS locale or IP-derived location.
    std::string language{"en-US"};
    // Empty for callers without a known bootstrap source.
    std::string mode;
};

struct LauncherTelemetryUsage {
    std::string theme;
    bool custom_background{};
};

// Call on a cancellable background worker. Shares the core UUID, but reserves a
// separate persistent daily attempt before networking, including failed sends.
void ReportLauncherUsage(const TelemetryOptions& options,
    const LauncherTelemetryUsage& usage, std::stop_token stop) noexcept;

// Calendar day in Asia/Shanghai (UTC+08:00), shared with the ingestion contract.
[[nodiscard]] std::string TelemetryDay(std::chrono::system_clock::time_point now);

class TelemetryClient final {
public:
    explicit TelemetryClient(TelemetryOptions options);
    ~TelemetryClient();
    TelemetryClient(const TelemetryClient&) = delete;
    TelemetryClient& operator=(const TelemetryClient&) = delete;

    // Called through the Worker dispatcher with host-owned copies of plugin IDs.
    void ObservePlugins(const std::vector<std::string>& ids,
        std::chrono::system_clock::time_point observed_at) noexcept;
    // Owned/joined by RuntimeSession. Failure never stops the runtime.
    void Run(std::stop_token stop) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace anomaly
