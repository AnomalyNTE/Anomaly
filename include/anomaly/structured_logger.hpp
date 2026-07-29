#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace anomaly {

inline constexpr std::size_t kMaximumRetainedLogArchives = 128;

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

[[nodiscard]] std::string_view LogLevelName(LogLevel level) noexcept;

enum class LogThreadDomain : std::uint8_t {
    Unknown,
    Bootstrap,
    Lifecycle,
    Game,
    Render,
    Worker,
    Diagnostics,
};

[[nodiscard]] std::string_view LogThreadDomainName(LogThreadDomain domain) noexcept;

struct PluginLogOwner {
    std::string owner;
    std::uint64_t generation{};
};

struct LogField {
    std::string key;
    std::string value;
};

struct LogDetails {
    LogThreadDomain thread_domain{LogThreadDomain::Unknown};
    std::string event_id;
    std::vector<LogField> fields;
    // JSONL keeps a stable schema: absence writes null plugin ID and generation.
    std::optional<PluginLogOwner> plugin;
};

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    std::uint64_t sequence{};
    std::uint32_t process_id{};
    std::uint32_t thread_id{};
    LogLevel level{LogLevel::Info};
    std::string component;
    std::string message;
    LogDetails details;
};

struct StructuredLoggerOptions {
    std::size_t queue_capacity{4096};
    std::size_t ring_capacity{1024};
    std::chrono::milliseconds flush_interval{250};
    // An empty value is replaced once at construction with a process-local ID.
    std::string session_id;
    // Zero disables size-based rolling. A single record larger than the limit is
    // kept intact in an empty current file and is rolled before the next record.
    std::uintmax_t max_file_size_bytes{};
    // Archives are PATH.1 (newest) through PATH.N (oldest); PATH is the current
    // file and is not included in this count. Values are clamped to
    // kMaximumRetainedLogArchives. Zero retains no archives, so a size-triggered
    // roll truncates PATH in place and removes existing numeric archives.
    std::size_t retained_archive_count{};
    LogLevel minimum_level{LogLevel::Trace};
};

enum class LoggerErrorOperation : std::uint8_t {
    Open,
    Write,
    Flush,
    Worker,
    Rotate,
};

struct LoggerError {
    LoggerErrorOperation operation{LoggerErrorOperation::Worker};
    std::filesystem::path path;
    std::error_code code;
    std::string message;
};

struct StructuredLoggerStats {
    std::uint64_t accepted{};
    std::uint64_t processed{};
    std::uint64_t dropped{};
    std::uint64_t queue_full_dropped{};
    std::uint64_t inactive_dropped{};
    std::uint64_t error_count{};
    std::uint64_t worker_wakeups{};
    std::size_t queued{};
    std::size_t ring_size{};
    bool running{};
};

class StructuredLogger final {
public:
    explicit StructuredLogger(StructuredLoggerOptions options = {});
    ~StructuredLogger();

    StructuredLogger(const StructuredLogger&) = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;
    StructuredLogger(StructuredLogger&&) = delete;
    StructuredLogger& operator=(StructuredLogger&&) = delete;

    // Start waits only for the background worker to open the JSONL file.
    [[nodiscard]] bool Start(const std::filesystem::path& path);

    // Log never waits for the writer. A full queue or inactive logger drops the record.
    [[nodiscard]] bool Log(
        LogLevel level,
        std::string component,
        std::string message,
        LogDetails details = {}) noexcept;

    // Compatibility overload for callers that only attach plugin ownership.
    [[nodiscard]] bool Log(
        LogLevel level,
        std::string component,
        std::string message,
        std::optional<PluginLogOwner> plugin) noexcept;

    // Flush persists every record accepted before this call, up to timeout.
    [[nodiscard]] bool Flush(std::chrono::milliseconds timeout);

    // Stop rejects new records, drains accepted records, flushes, and joins the worker.
    [[nodiscard]] bool Stop();

    // Lifecycle-only reconfiguration. Existing buffered records are retained
    // up to the new capacity, newest first when the ring must shrink.
    [[nodiscard]] bool Reconfigure(LogLevel minimum_level, std::size_t ring_capacity) noexcept;

    [[nodiscard]] std::vector<LogRecord> RingSnapshot() const;
    [[nodiscard]] StructuredLoggerStats Stats() const noexcept;
    [[nodiscard]] std::optional<LoggerError> LastError() const;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::string_view SessionId() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
