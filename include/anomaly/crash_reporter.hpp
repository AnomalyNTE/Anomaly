#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace anomaly {

struct CrashReporterOptions {
    std::filesystem::path dump_directory;
    std::string runtime_version;
};

struct CrashDumpResult {
    bool written{};
    DWORD error{ERROR_SUCCESS};
    std::uint32_t exception_code{};
    std::filesystem::path dump_path;
    std::filesystem::path metadata_path;
};

// Process-wide crash reporter. The generated dump intentionally uses MiniDumpNormal;
// full process memory and handle data are excluded from the default artifact. Metadata
// is limited to fixed diagnostic fields and a caller-supplied, non-sensitive reason code;
// it does not include paths, command lines, environment data, logs, or plugin settings.
class CrashReporter final {
public:
    explicit CrashReporter(CrashReporterOptions options);
    ~CrashReporter();

    CrashReporter(const CrashReporter&) = delete;
    CrashReporter& operator=(const CrashReporter&) = delete;

    [[nodiscard]] bool Install(std::string* error = nullptr) noexcept;
    void Uninstall() noexcept;
    [[nodiscard]] bool Installed() const noexcept;

    [[nodiscard]] CrashDumpResult WriteDump(
        EXCEPTION_POINTERS* exception, std::string_view reason) noexcept;

private:
    static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* exception) noexcept;

    CrashReporterOptions options_;
    mutable std::mutex mutex_;
    std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> previous_filter_{};
    std::atomic<bool> dump_in_progress_{};
    bool installed_{};
    std::atomic<std::uint64_t> sequence_{};
};

}  // namespace anomaly
