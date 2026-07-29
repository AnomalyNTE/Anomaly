#include "anomaly/structured_logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace anomaly {
namespace {

enum class LoggerState : std::uint8_t {
    Stopped,
    Starting,
    Running,
    Stopping,
};

std::size_t NormalizeQueueCapacity(std::size_t capacity) noexcept {
    return (std::max)(capacity, std::size_t{2});
}

std::chrono::milliseconds NormalizeFlushInterval(
    std::chrono::milliseconds interval) noexcept {
    return interval <= std::chrono::milliseconds::zero()
        ? std::chrono::milliseconds{1}
        : interval;
}

std::error_code CurrentIoError() noexcept {
    if (errno != 0) return {errno, std::generic_category()};
    return std::make_error_code(std::errc::io_error);
}

HANDLE CreateWakeEvent() {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "failed to create structured logger wake event");
    }
    return event;
}

DWORD WaitTimeoutUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return 0;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    constexpr auto maximum = static_cast<long long>(INFINITE) - 1;
    return static_cast<DWORD>((std::min)(remaining.count(), maximum));
}

void AppendUnsigned(std::string& output, std::uint64_t value) {
    char buffer[32]{};
    const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (converted.ec == std::errc{}) output.append(buffer, converted.ptr);
}

bool IsUtf8Continuation(unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

void AppendJsonString(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    std::size_t offset{};
    while (offset < value.size()) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        if (byte >= 0x80U) {
            std::size_t length{};
            bool valid{};
            if (byte >= 0xC2U && byte <= 0xDFU) {
                length = 2;
                valid = offset + length <= value.size() &&
                    IsUtf8Continuation(static_cast<unsigned char>(value[offset + 1]));
            } else if (byte >= 0xE0U && byte <= 0xEFU) {
                length = 3;
                if (offset + length <= value.size()) {
                    const auto second = static_cast<unsigned char>(value[offset + 1]);
                    const auto third = static_cast<unsigned char>(value[offset + 2]);
                    const bool second_valid = byte == 0xE0U
                        ? second >= 0xA0U && second <= 0xBFU
                        : byte == 0xEDU
                            ? second >= 0x80U && second <= 0x9FU
                            : IsUtf8Continuation(second);
                    valid = second_valid && IsUtf8Continuation(third);
                }
            } else if (byte >= 0xF0U && byte <= 0xF4U) {
                length = 4;
                if (offset + length <= value.size()) {
                    const auto second = static_cast<unsigned char>(value[offset + 1]);
                    const auto third = static_cast<unsigned char>(value[offset + 2]);
                    const auto fourth = static_cast<unsigned char>(value[offset + 3]);
                    const bool second_valid = byte == 0xF0U
                        ? second >= 0x90U && second <= 0xBFU
                        : byte == 0xF4U
                            ? second >= 0x80U && second <= 0x8FU
                            : IsUtf8Continuation(second);
                    valid = second_valid && IsUtf8Continuation(third) &&
                        IsUtf8Continuation(fourth);
                }
            }

            if (valid) {
                output.append(value.data() + offset, length);
                offset += length;
            } else {
                output.append("\\ufffd");
                ++offset;
            }
            continue;
        }

        switch (byte) {
            case '"': output.append("\\\""); break;
            case '\\': output.append("\\\\"); break;
            case '\b': output.append("\\b"); break;
            case '\f': output.append("\\f"); break;
            case '\n': output.append("\\n"); break;
            case '\r': output.append("\\r"); break;
            case '\t': output.append("\\t"); break;
            default:
                if (byte < 0x20) {
                    output.append("\\u00");
                    output.push_back(hex[(byte >> 4U) & 0x0FU]);
                    output.push_back(hex[byte & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
                break;
        }
        ++offset;
    }
    output.push_back('"');
}

std::string GenerateLocalSessionId() {
    static std::atomic<std::uint64_t> sequence{};
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::string result{"local-"};
    AppendUnsigned(result, static_cast<std::uint64_t>(GetCurrentProcessId()));
    result.push_back('-');
    AppendUnsigned(result, static_cast<std::uint64_t>(now));
    result.push_back('-');
    AppendUnsigned(result, sequence.fetch_add(1, std::memory_order_relaxed) + 1);
    return result;
}

std::string TimestampString(std::chrono::system_clock::time_point timestamp) {
    using namespace std::chrono;
    const auto whole_seconds = floor<seconds>(timestamp);
    const auto micros = duration_cast<microseconds>(timestamp - whole_seconds).count();
    const std::time_t raw_time = system_clock::to_time_t(whole_seconds);
    std::tm utc{};
    if (gmtime_s(&utc, &raw_time) != 0) return "1970-01-01T00:00:00.000000Z";

    char buffer[40]{};
    const int written = std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        static_cast<long long>(micros));
    if (written <= 0) return "1970-01-01T00:00:00.000000Z";
    return {buffer, static_cast<std::size_t>(written)};
}

std::string SerializeJson(const LogRecord& record, std::string_view session_id) {
    std::string output;
    output.reserve(record.component.size() + record.message.size() + session_id.size() + 320);
    output.append("{\"timestamp\":");
    AppendJsonString(output, TimestampString(record.timestamp));
    output.append(",\"sequence\":");
    AppendUnsigned(output, record.sequence);
    output.append(",\"session_id\":");
    AppendJsonString(output, session_id);
    output.append(",\"pid\":");
    AppendUnsigned(output, record.process_id);
    output.append(",\"tid\":");
    AppendUnsigned(output, record.thread_id);
    output.append(",\"thread_domain\":");
    AppendJsonString(output, LogThreadDomainName(record.details.thread_domain));
    output.append(",\"level\":");
    AppendJsonString(output, LogLevelName(record.level));
    output.append(",\"component\":");
    AppendJsonString(output, record.component);
    output.append(",\"plugin_id\":");
    if (record.details.plugin) {
        AppendJsonString(output, record.details.plugin->owner);
    } else {
        output.append("null");
    }
    output.append(",\"plugin_generation\":");
    if (record.details.plugin) {
        AppendUnsigned(output, record.details.plugin->generation);
    } else {
        output.append("null");
    }
    output.append(",\"event_id\":");
    AppendJsonString(output, record.details.event_id);
    output.append(",\"message\":");
    AppendJsonString(output, record.message);
    output.append(",\"fields\":{");
    bool first_field{true};
    for (const auto& field : record.details.fields) {
        if (!first_field) output.push_back(',');
        first_field = false;
        AppendJsonString(output, field.key);
        output.push_back(':');
        AppendJsonString(output, field.value);
    }
    output.push_back('}');
    output.push_back('}');
    return output;
}

template <typename T>
void StoreMaximum(std::atomic<T>& destination, T value) noexcept {
    T current = destination.load(std::memory_order_relaxed);
    while (current < value &&
           !destination.compare_exchange_weak(
               current, value, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

}  // namespace

std::string_view LogLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
        case LogLevel::Critical: return "critical";
    }
    return "unknown";
}

std::string_view LogThreadDomainName(LogThreadDomain domain) noexcept {
    switch (domain) {
        case LogThreadDomain::Unknown: return "unknown";
        case LogThreadDomain::Bootstrap: return "bootstrap";
        case LogThreadDomain::Lifecycle: return "lifecycle";
        case LogThreadDomain::Game: return "game";
        case LogThreadDomain::Render: return "render";
        case LogThreadDomain::Worker: return "worker";
        case LogThreadDomain::Diagnostics: return "diagnostics";
    }
    return "unknown";
}

class StructuredLogger::Impl final {
public:
    explicit Impl(StructuredLoggerOptions options)
        : queue_capacity_(NormalizeQueueCapacity(options.queue_capacity)),
          ring_capacity_(options.ring_capacity),
          minimum_level_(options.minimum_level),
          flush_interval_(NormalizeFlushInterval(options.flush_interval)),
          session_id_(options.session_id.empty()
                  ? GenerateLocalSessionId()
                  : std::move(options.session_id)),
          max_file_size_bytes_(options.max_file_size_bytes),
          retained_archive_count_((std::min)(
              options.retained_archive_count, kMaximumRetainedLogArchives)),
          queue_(std::make_unique<QueueCell[]>(queue_capacity_)),
          ring_(ring_capacity_ == 0 ? nullptr : std::make_unique<RingCell[]>(ring_capacity_)),
          wake_event_(CreateWakeEvent()) {
        ResetQueue();
    }

    ~Impl() {
        try {
            static_cast<void>(Stop());
        } catch (...) {
        }
        if (wake_event_ != nullptr) static_cast<void>(CloseHandle(wake_event_));
    }

    bool Start(const std::filesystem::path& path) {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        if (worker_.joinable() || state_.load(std::memory_order_acquire) != LoggerState::Stopped) {
            return false;
        }

        ResetForStart();
        path_ = path;
        state_.store(LoggerState::Starting, std::memory_order_release);
        {
            std::scoped_lock startup_lock(startup_mutex_);
            startup_complete_ = false;
            startup_succeeded_ = false;
        }

        try {
            worker_ = std::jthread([this, worker_path = path_](std::stop_token stop_token) {
                WorkerMain(stop_token, worker_path);
            });
        } catch (const std::system_error& error) {
            state_.store(LoggerState::Stopped, std::memory_order_release);
            SetError(LoggerErrorOperation::Worker, error.code(), error.what());
            return false;
        } catch (const std::exception& error) {
            state_.store(LoggerState::Stopped, std::memory_order_release);
            SetError(
                LoggerErrorOperation::Worker,
                std::make_error_code(std::errc::resource_unavailable_try_again),
                error.what());
            return false;
        }

        bool started{};
        {
            std::unique_lock startup_lock(startup_mutex_);
            startup_condition_.wait(startup_lock, [this] { return startup_complete_; });
            started = startup_succeeded_;
        }
        if (!started && worker_.joinable()) worker_.join();
        return started;
    }

    bool Log(
        LogLevel level,
        std::string component,
        std::string message,
        LogDetails details) noexcept {
        if (static_cast<std::uint8_t>(level) <
            static_cast<std::uint8_t>(minimum_level_.load(std::memory_order_acquire))) {
            return false;
        }
        if (state_.load(std::memory_order_acquire) != LoggerState::Running) {
            DropInactive();
            return false;
        }

        active_producers_.fetch_add(1, std::memory_order_acq_rel);
        if (state_.load(std::memory_order_acquire) != LoggerState::Running) {
            active_producers_.fetch_sub(1, std::memory_order_acq_rel);
            DropInactive();
            NotifyWorker();
            return false;
        }

        LogRecord record{
            std::chrono::system_clock::now(),
            0,
            static_cast<std::uint32_t>(GetCurrentProcessId()),
            static_cast<std::uint32_t>(GetCurrentThreadId()),
            level,
            std::move(component),
            std::move(message),
            std::move(details)};
        const bool accepted = TryEnqueue(std::move(record));
        active_producers_.fetch_sub(1, std::memory_order_acq_rel);

        if (!accepted) {
            queue_full_dropped_.fetch_add(1, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        NotifyWorker();
        return accepted;
    }

    bool Flush(std::chrono::milliseconds timeout) {
        const std::uint64_t target = accepted_.load(std::memory_order_acquire);
        StoreMaximum(requested_flush_, target);
        NotifyWorker();

        if (flushed_.load(std::memory_order_acquire) < target) {
            std::unique_lock completion_lock(completion_mutex_);
            const bool completed = completion_condition_.wait_for(
                completion_lock,
                (std::max)(timeout, std::chrono::milliseconds::zero()),
                [this, target] {
                    return flushed_.load(std::memory_order_acquire) >= target ||
                        worker_exited_.load(std::memory_order_acquire);
                });
            if (!completed || flushed_.load(std::memory_order_acquire) < target) return false;
        }
        return error_count_.load(std::memory_order_acquire) == 0;
    }

    bool Stop() {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        if (!worker_.joinable()) {
            state_.store(LoggerState::Stopped, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            return error_count_.load(std::memory_order_acquire) == 0;
        }

        state_.store(LoggerState::Stopping, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        worker_.request_stop();
        NotifyWorker();
        worker_.join();
        return error_count_.load(std::memory_order_acquire) == 0;
    }

    bool Reconfigure(const LogLevel minimum_level, const std::size_t ring_capacity) noexcept {
        try {
            std::unique_ptr<RingCell[]> replacement = ring_capacity == 0
                ? nullptr
                : std::make_unique<RingCell[]>(ring_capacity);
            std::scoped_lock ring_lock(ring_mutex_);
            const std::size_t retained = (std::min)(ring_count_, ring_capacity);
            if (retained != 0 && ring_capacity_ != 0) {
                const std::size_t first =
                    (ring_write_position_ + ring_capacity_ - retained) % ring_capacity_;
                for (std::size_t index = 0; index < retained; ++index) {
                    replacement[index].record = ring_[(first + index) % ring_capacity_].record;
                }
            }
            ring_ = std::move(replacement);
            ring_capacity_ = ring_capacity;
            ring_count_ = retained;
            ring_write_position_ = ring_capacity == 0 ? 0 : retained % ring_capacity;
            ring_size_.store(retained, std::memory_order_release);
            minimum_level_.store(minimum_level, std::memory_order_release);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::vector<LogRecord> RingSnapshot() const {
        std::vector<LogRecord> result;
        std::scoped_lock ring_lock(ring_mutex_);
        result.reserve(ring_count_);
        if (ring_capacity_ == 0) return result;

        const std::size_t first =
            (ring_write_position_ + ring_capacity_ - ring_count_) % ring_capacity_;
        for (std::size_t index = 0; index < ring_count_; ++index) {
            const auto& cell = ring_[(first + index) % ring_capacity_];
            if (cell.record) result.push_back(*cell.record);
        }
        return result;
    }

    StructuredLoggerStats Stats() const noexcept {
        StructuredLoggerStats result;
        result.accepted = accepted_.load(std::memory_order_acquire);
        result.processed = processed_.load(std::memory_order_acquire);
        result.dropped = dropped_.load(std::memory_order_relaxed);
        result.queue_full_dropped = queue_full_dropped_.load(std::memory_order_relaxed);
        result.inactive_dropped = inactive_dropped_.load(std::memory_order_relaxed);
        result.error_count = error_count_.load(std::memory_order_acquire);
        result.worker_wakeups = worker_wakeups_.load(std::memory_order_relaxed);
        const auto outstanding = result.accepted - (std::min)(result.accepted, result.processed);
        result.queued = outstanding > (std::numeric_limits<std::size_t>::max)()
            ? (std::numeric_limits<std::size_t>::max)()
            : static_cast<std::size_t>(outstanding);
        result.ring_size = ring_size_.load(std::memory_order_acquire);
        result.running = running_.load(std::memory_order_acquire);
        return result;
    }

    std::optional<LoggerError> LastError() const {
        std::scoped_lock error_lock(error_mutex_);
        return last_error_;
    }

    bool IsRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    std::string_view SessionId() const noexcept {
        return session_id_;
    }

private:
    struct QueueCell {
        std::atomic<std::size_t> turn{};
        std::optional<LogRecord> record;
    };

    struct RingCell {
        std::optional<LogRecord> record;
    };

    static_assert(std::is_nothrow_move_constructible_v<LogRecord>);

    void ResetForStart() {
        ResetQueue();
        {
            std::scoped_lock ring_lock(ring_mutex_);
            for (std::size_t index = 0; index < ring_capacity_; ++index) {
                ring_[index].record.reset();
            }
            ring_write_position_ = 0;
            ring_count_ = 0;
        }
        {
            std::scoped_lock error_lock(error_mutex_);
            last_error_.reset();
        }
        accepted_.store(0, std::memory_order_relaxed);
        processed_.store(0, std::memory_order_relaxed);
        requested_flush_.store(0, std::memory_order_relaxed);
        flushed_.store(0, std::memory_order_relaxed);
        active_producers_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        queue_full_dropped_.store(0, std::memory_order_relaxed);
        inactive_dropped_.store(0, std::memory_order_relaxed);
        error_count_.store(0, std::memory_order_relaxed);
        worker_wakeups_.store(0, std::memory_order_relaxed);
        ring_size_.store(0, std::memory_order_relaxed);
        worker_exited_.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
    }

    void ResetQueue() noexcept {
        for (std::size_t index = 0; index < queue_capacity_; ++index) {
            queue_[index].record.reset();
            queue_[index].turn.store(index, std::memory_order_relaxed);
        }
        enqueue_position_.store(0, std::memory_order_relaxed);
        dequeue_position_.store(0, std::memory_order_relaxed);
    }

    bool TryEnqueue(LogRecord record) noexcept {
        std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
        QueueCell* cell{};
        for (;;) {
            cell = &queue_[position % queue_capacity_];
            const std::size_t turn = cell->turn.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(turn) -
                static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_position_.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_position_.load(std::memory_order_relaxed);
            }
        }

        record.sequence = static_cast<std::uint64_t>(position) + 1;
        cell->record.emplace(std::move(record));
        cell->turn.store(position + 1, std::memory_order_release);
        StoreMaximum(accepted_, static_cast<std::uint64_t>(position) + 1);
        return true;
    }

    bool TryDequeue(LogRecord& record) noexcept {
        const std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
        QueueCell& cell = queue_[position % queue_capacity_];
        if (cell.turn.load(std::memory_order_acquire) != position + 1) return false;

        record = std::move(*cell.record);
        cell.record.reset();
        dequeue_position_.store(position + 1, std::memory_order_relaxed);
        cell.turn.store(position + queue_capacity_, std::memory_order_release);
        return true;
    }

    bool HasReadyRecord() const noexcept {
        const std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
        return queue_[position % queue_capacity_].turn.load(std::memory_order_acquire) ==
            position + 1;
    }

    void AppendRing(LogRecord record) noexcept {
        std::scoped_lock ring_lock(ring_mutex_);
        if (ring_capacity_ == 0) return;
        ring_[ring_write_position_].record.emplace(std::move(record));
        ring_write_position_ = (ring_write_position_ + 1) % ring_capacity_;
        ring_count_ = (std::min)(ring_count_ + 1, ring_capacity_);
        ring_size_.store(ring_count_, std::memory_order_release);
    }

    void DropInactive() noexcept {
        inactive_dropped_.fetch_add(1, std::memory_order_relaxed);
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    void NotifyWorker() noexcept {
        static_cast<void>(SetEvent(wake_event_));
    }

    void SetError(
        LoggerErrorOperation operation,
        std::error_code code,
        std::string message) noexcept {
        SetErrorAtPath(operation, path_, code, std::move(message));
    }

    void SetErrorAtPath(
        LoggerErrorOperation operation,
        const std::filesystem::path& error_path,
        std::error_code code,
        std::string message) noexcept {
        try {
            LoggerError error{operation, error_path, code, std::move(message)};
            {
                std::scoped_lock error_lock(error_mutex_);
                last_error_ = std::move(error);
            }
            error_count_.fetch_add(1, std::memory_order_release);
        } catch (...) {
            error_count_.fetch_add(1, std::memory_order_release);
        }
    }

    void CompleteStartup(bool succeeded) noexcept {
        {
            std::scoped_lock startup_lock(startup_mutex_);
            startup_succeeded_ = succeeded;
            startup_complete_ = true;
        }
        startup_condition_.notify_all();
    }

    void FlushStream(
        std::ofstream& stream, std::uint64_t processed, bool& file_healthy) noexcept {
        if (file_healthy) {
            errno = 0;
            stream.flush();
            if (!stream) {
                file_healthy = false;
                SetError(LoggerErrorOperation::Flush, CurrentIoError(),
                         "failed to flush structured log file");
            }
        }
        flushed_.store(processed, std::memory_order_release);
        completion_condition_.notify_all();
    }

    static std::filesystem::path ArchivePath(
        const std::filesystem::path& current_path, std::size_t index) {
        auto archive_path = current_path;
        archive_path += std::filesystem::path{"." + std::to_string(index)};
        return archive_path;
    }

    enum class RotationOutcome {
        Rotated,
        Deferred,
        Fatal,
    };

    static bool ArchiveIndex(
        std::wstring_view filename,
        std::wstring_view prefix,
        std::uintmax_t& index) noexcept {
        if (!filename.starts_with(prefix) || filename.size() == prefix.size()) return false;
        index = 0;
        for (const wchar_t character : filename.substr(prefix.size())) {
            if (character < L'0' || character > L'9') return false;
            const std::uintmax_t digit = static_cast<std::uintmax_t>(character - L'0');
            const auto maximum = (std::numeric_limits<std::uintmax_t>::max)();
            if (index > (maximum - digit) / 10) {
                index = maximum;
            } else if (index != maximum) {
                index = index * 10 + digit;
            }
        }
        return index != 0;
    }

    static bool IsDeferredRotationError(DWORD error) noexcept {
        return error == ERROR_SHARING_VIOLATION ||
            error == ERROR_LOCK_VIOLATION ||
            error == ERROR_USER_MAPPED_FILE;
    }

    RotationOutcome ReportRotationPathError(
        const std::filesystem::path& error_path,
        DWORD error,
        std::string message) noexcept {
        SetErrorAtPath(
            LoggerErrorOperation::Rotate,
            error_path,
            {static_cast<int>(error), std::system_category()},
            std::move(message));
        return IsDeferredRotationError(error)
            ? RotationOutcome::Deferred
            : RotationOutcome::Fatal;
    }

    static bool PathExists(
        const std::filesystem::path& path, bool& exists, std::error_code& error) noexcept {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            exists = true;
            error.clear();
            return true;
        }

        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            exists = false;
            error.clear();
            return true;
        }
        exists = false;
        error = {static_cast<int>(code), std::system_category()};
        return false;
    }

    RotationOutcome PrepareArchiveMutation(
        const std::filesystem::path& archive_path) {
        const DWORD attributes = GetFileAttributesW(archive_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                return RotationOutcome::Rotated;
            }
            return ReportRotationPathError(
                archive_path, error, "failed to inspect structured log archive");
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return ReportRotationPathError(
                archive_path, ERROR_DIRECTORY,
                "structured log archive path is a directory");
        }
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            return ReportRotationPathError(
                archive_path, ERROR_ACCESS_DENIED,
                "structured log archive is read-only");
        }

        const HANDLE probe = CreateFileW(
            archive_path.c_str(),
            DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            return ReportRotationPathError(
                archive_path, GetLastError(),
                "structured log archive is not ready for rotation");
        }
        if (CloseHandle(probe) == FALSE) {
            return ReportRotationPathError(
                archive_path, GetLastError(),
                "failed to release structured log archive probe");
        }
        return RotationOutcome::Rotated;
    }

    RotationOutcome PrepareRetainedArchives() {
        for (std::size_t index = 1; index <= retained_archive_count_; ++index) {
            const RotationOutcome outcome = PrepareArchiveMutation(ArchivePath(path_, index));
            if (outcome != RotationOutcome::Rotated) return outcome;
        }
        return RotationOutcome::Rotated;
    }

    RotationOutcome RemoveArchivesBeyondRetention() {
        try {
            std::filesystem::path directory = path_.parent_path();
            if (directory.empty()) directory = L".";
            std::wstring prefix = path_.filename().native();
            prefix.push_back(L'.');

            std::vector<std::filesystem::path> excess_archives;
            std::error_code error;
            std::filesystem::directory_iterator iterator(directory, error);
            const std::filesystem::directory_iterator end;
            if (error) {
                SetErrorAtPath(LoggerErrorOperation::Rotate, directory, error,
                               "failed to enumerate structured log archives");
                return RotationOutcome::Fatal;
            }
            while (iterator != end) {
                const auto archive_path = iterator->path();
                std::uintmax_t index{};
                if (ArchiveIndex(
                        archive_path.filename().native(), prefix, index) &&
                    index > retained_archive_count_) {
                    excess_archives.push_back(archive_path);
                }
                iterator.increment(error);
                if (error) {
                    SetErrorAtPath(LoggerErrorOperation::Rotate, directory, error,
                                   "failed to enumerate structured log archives");
                    return RotationOutcome::Fatal;
                }
            }

            std::sort(excess_archives.begin(), excess_archives.end());
            for (const auto& archive_path : excess_archives) {
                const RotationOutcome outcome = PrepareArchiveMutation(archive_path);
                if (outcome != RotationOutcome::Rotated) return outcome;
            }
            for (const auto& archive_path : excess_archives) {
                if (DeleteFileW(archive_path.c_str()) == FALSE) {
                    const DWORD delete_error = GetLastError();
                    if (delete_error == ERROR_FILE_NOT_FOUND ||
                        delete_error == ERROR_PATH_NOT_FOUND) {
                        continue;
                    }
                    return ReportRotationPathError(
                        archive_path, delete_error,
                        "failed to remove excess structured log archive");
                }
            }
            return RotationOutcome::Rotated;
        } catch (...) {
            SetError(LoggerErrorOperation::Rotate,
                     std::make_error_code(std::errc::io_error),
                     "failed to clean structured log archives");
            return RotationOutcome::Fatal;
        }
    }

    RotationOutcome RemoveArchiveIfPresent(
        const std::filesystem::path& archive_path) {
        bool exists{};
        std::error_code error;
        if (!PathExists(archive_path, exists, error)) {
            SetErrorAtPath(LoggerErrorOperation::Rotate, archive_path, error,
                           "failed to inspect oldest structured log archive");
            return RotationOutcome::Fatal;
        }
        if (!exists) return RotationOutcome::Rotated;
        if (DeleteFileW(archive_path.c_str()) != FALSE) return RotationOutcome::Rotated;

        const DWORD delete_error = GetLastError();
        if (delete_error == ERROR_FILE_NOT_FOUND || delete_error == ERROR_PATH_NOT_FOUND) {
            return RotationOutcome::Rotated;
        }
        return ReportRotationPathError(
            archive_path, delete_error,
            "failed to remove oldest structured log archive");
    }

    RotationOutcome MoveArchive(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) {
        bool exists{};
        std::error_code error;
        if (!PathExists(source, exists, error)) {
            SetErrorAtPath(LoggerErrorOperation::Rotate, source, error,
                           "failed to inspect structured log archive");
            return RotationOutcome::Fatal;
        }
        if (!exists) return RotationOutcome::Rotated;

        constexpr DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
        if (MoveFileExW(source.c_str(), destination.c_str(), flags) != FALSE) {
            return RotationOutcome::Rotated;
        }

        const DWORD move_error = GetLastError();
        const auto& error_path =
            move_error == ERROR_ALREADY_EXISTS || move_error == ERROR_FILE_EXISTS
            ? destination
            : source;
        return ReportRotationPathError(
            error_path, move_error, "failed to move structured log archive");
    }

    bool OpenCurrentFile(std::ofstream& stream, std::ios::openmode mode) {
        errno = 0;
        stream = std::ofstream(path_, std::ios::binary | std::ios::out | mode);
        if (stream.is_open()) return true;
        SetError(LoggerErrorOperation::Rotate, CurrentIoError(),
                 "failed to open current structured log file after rotation");
        return false;
    }

    bool RestoreAppendStream(
        std::ofstream& stream, std::uintmax_t& current_bytes) {
        errno = 0;
        stream = std::ofstream(path_, std::ios::binary | std::ios::out | std::ios::app);
        if (!stream.is_open()) {
            SetError(LoggerErrorOperation::Rotate, CurrentIoError(),
                     "failed to restore current structured log file for append");
            return false;
        }

        std::error_code size_error;
        current_bytes = std::filesystem::file_size(path_, size_error);
        if (!size_error) return true;

        SetError(LoggerErrorOperation::Rotate, size_error,
                 "failed to determine restored structured log file size");
        stream.close();
        return false;
    }

    RotationOutcome RotateStream(
        std::ofstream& stream, std::uintmax_t& current_bytes) {
        errno = 0;
        stream.flush();
        if (!stream) {
            SetError(LoggerErrorOperation::Flush, CurrentIoError(),
                     "failed to flush structured log file before rotation");
            return RotationOutcome::Fatal;
        }

        if (retained_archive_count_ != 0) {
            const RotationOutcome preparation = PrepareRetainedArchives();
            if (preparation != RotationOutcome::Rotated) return preparation;
        }

        errno = 0;
        stream.close();
        if (stream.fail()) {
            SetError(LoggerErrorOperation::Rotate, CurrentIoError(),
                     "failed to close structured log file before rotation");
            return RestoreAppendStream(stream, current_bytes)
                ? RotationOutcome::Deferred
                : RotationOutcome::Fatal;
        }

        if (retained_archive_count_ != 0) {
            const RotationOutcome current_preparation = PrepareArchiveMutation(path_);
            if (current_preparation != RotationOutcome::Rotated) {
                if (!RestoreAppendStream(stream, current_bytes)) {
                    return RotationOutcome::Fatal;
                }
                return current_preparation;
            }

            const RotationOutcome removed =
                RemoveArchiveIfPresent(ArchivePath(path_, retained_archive_count_));
            if (removed != RotationOutcome::Rotated) {
                if (!RestoreAppendStream(stream, current_bytes)) {
                    return RotationOutcome::Fatal;
                }
                return removed;
            }
            for (std::size_t index = retained_archive_count_; index > 1; --index) {
                const RotationOutcome archive_move = MoveArchive(
                    ArchivePath(path_, index - 1), ArchivePath(path_, index));
                if (archive_move != RotationOutcome::Rotated) {
                    if (!RestoreAppendStream(stream, current_bytes)) {
                        return RotationOutcome::Fatal;
                    }
                    return archive_move;
                }
            }

            const RotationOutcome moved = MoveArchive(path_, ArchivePath(path_, 1));
            if (moved != RotationOutcome::Rotated) {
                if (!RestoreAppendStream(stream, current_bytes)) {
                    return RotationOutcome::Fatal;
                }
                return moved;
            }
        }

        if (OpenCurrentFile(stream, std::ios::trunc)) {
            current_bytes = 0;
            return RotationOutcome::Rotated;
        }
        return RestoreAppendStream(stream, current_bytes)
            ? RotationOutcome::Deferred
            : RotationOutcome::Fatal;
    }

    bool ShouldRotate(std::uintmax_t current_bytes, std::uintmax_t record_bytes) const noexcept {
        if (max_file_size_bytes_ == 0 || current_bytes == 0) return false;
        if (current_bytes >= max_file_size_bytes_) return true;
        return record_bytes > max_file_size_bytes_ - current_bytes;
    }

    void ProcessRecord(
        std::ofstream& stream,
        LogRecord record,
        bool& file_healthy,
        std::uintmax_t& current_bytes,
        bool& retention_cleanup_pending) {
        const std::uint64_t sequence = record.sequence;
        std::string line = SerializeJson(record, session_id_);
        AppendRing(std::move(record));
        const auto serialized_bytes = static_cast<std::uintmax_t>(line.size());
        const auto maximum = (std::numeric_limits<std::uintmax_t>::max)();
        const auto record_bytes = serialized_bytes == maximum
            ? maximum
            : serialized_bytes + 1;

        if (file_healthy && retention_cleanup_pending) {
            const RotationOutcome cleanup = RemoveArchivesBeyondRetention();
            retention_cleanup_pending = cleanup == RotationOutcome::Deferred;
            if (cleanup == RotationOutcome::Fatal) file_healthy = false;
        }

        if (file_healthy && !retention_cleanup_pending &&
            ShouldRotate(current_bytes, record_bytes)) {
            if (RotateStream(stream, current_bytes) == RotationOutcome::Fatal) {
                file_healthy = false;
            }
        }

        if (file_healthy) {
            errno = 0;
            stream.write(line.data(), static_cast<std::streamsize>(line.size()));
            stream.put('\n');
            if (!stream) {
                file_healthy = false;
                SetError(LoggerErrorOperation::Write, CurrentIoError(),
                         "failed to write structured log file");
            } else {
                current_bytes = record_bytes > maximum - current_bytes
                    ? maximum
                    : current_bytes + record_bytes;
            }
        }
        processed_.store(sequence, std::memory_order_release);
    }

    void WorkerMain(
        std::stop_token stop_token, const std::filesystem::path& worker_path) noexcept {
        bool startup_reported{};
        try {
            errno = 0;
            std::ofstream stream(worker_path, std::ios::binary | std::ios::out | std::ios::app);
            if (!stream.is_open()) {
                SetError(LoggerErrorOperation::Open, CurrentIoError(),
                         "failed to open structured log file");
                state_.store(LoggerState::Stopped, std::memory_order_release);
                worker_exited_.store(true, std::memory_order_release);
                CompleteStartup(false);
                completion_condition_.notify_all();
                return;
            }

            std::error_code size_error;
            std::uintmax_t current_bytes = std::filesystem::file_size(worker_path, size_error);
            if (size_error) {
                SetError(LoggerErrorOperation::Open, size_error,
                         "failed to determine structured log file size");
                state_.store(LoggerState::Stopped, std::memory_order_release);
                worker_exited_.store(true, std::memory_order_release);
                CompleteStartup(false);
                completion_condition_.notify_all();
                return;
            }

            const RotationOutcome cleanup = RemoveArchivesBeyondRetention();
            if (cleanup == RotationOutcome::Fatal) {
                state_.store(LoggerState::Stopped, std::memory_order_release);
                worker_exited_.store(true, std::memory_order_release);
                CompleteStartup(false);
                completion_condition_.notify_all();
                return;
            }
            bool retention_cleanup_pending = cleanup == RotationOutcome::Deferred;

            if (!retention_cleanup_pending && max_file_size_bytes_ != 0 &&
                current_bytes > max_file_size_bytes_) {
                const RotationOutcome rotation = RotateStream(stream, current_bytes);
                if (rotation == RotationOutcome::Fatal) {
                    state_.store(LoggerState::Stopped, std::memory_order_release);
                    worker_exited_.store(true, std::memory_order_release);
                    CompleteStartup(false);
                    completion_condition_.notify_all();
                    return;
                }
            }

            bool file_healthy{true};
            state_.store(LoggerState::Running, std::memory_order_release);
            running_.store(true, std::memory_order_release);
            CompleteStartup(true);
            startup_reported = true;

            auto next_periodic_flush = std::chrono::steady_clock::now() + flush_interval_;
            for (;;) {
                LogRecord record;
                while (TryDequeue(record)) {
                    ProcessRecord(
                        stream,
                        std::move(record),
                        file_healthy,
                        current_bytes,
                        retention_cleanup_pending);
                }

                const std::uint64_t processed = processed_.load(std::memory_order_acquire);
                const std::uint64_t requested = requested_flush_.load(std::memory_order_acquire);
                const auto now = std::chrono::steady_clock::now();
                const bool stopping = stop_token.stop_requested() ||
                    state_.load(std::memory_order_acquire) != LoggerState::Running;
                const bool producers_done =
                    active_producers_.load(std::memory_order_acquire) == 0;
                const bool queue_drained = producers_done && !HasReadyRecord() &&
                    processed >= accepted_.load(std::memory_order_acquire);
                const bool requested_flush_ready =
                    requested > flushed_.load(std::memory_order_acquire) &&
                    processed >= requested;
                const bool periodic_deadline_reached = now >= next_periodic_flush;
                const bool periodic_flush_ready = periodic_deadline_reached &&
                    processed > flushed_.load(std::memory_order_acquire);

                if (requested_flush_ready || periodic_flush_ready ||
                    (stopping && queue_drained)) {
                    FlushStream(stream, processed, file_healthy);
                }
                if (periodic_deadline_reached || requested_flush_ready ||
                    (stopping && queue_drained)) {
                    next_periodic_flush = std::chrono::steady_clock::now() + flush_interval_;
                }
                if (stopping && queue_drained) break;

                const DWORD wait_result =
                    WaitForSingleObject(wake_event_, WaitTimeoutUntil(next_periodic_flush));
                worker_wakeups_.fetch_add(1, std::memory_order_relaxed);
                if (wait_result == WAIT_FAILED) {
                    SetError(LoggerErrorOperation::Worker,
                             std::error_code(
                                 static_cast<int>(GetLastError()), std::system_category()),
                             "failed to wait for structured logger work");
                    break;
                }
            }
        } catch (const std::system_error& error) {
            SetError(LoggerErrorOperation::Worker, error.code(), error.what());
        } catch (const std::exception& error) {
            SetError(LoggerErrorOperation::Worker, std::make_error_code(std::errc::io_error),
                     error.what());
        } catch (...) {
            SetError(LoggerErrorOperation::Worker, std::make_error_code(std::errc::io_error),
                     "structured logger worker failed");
        }

        running_.store(false, std::memory_order_release);
        state_.store(LoggerState::Stopped, std::memory_order_release);
        worker_exited_.store(true, std::memory_order_release);
        if (!startup_reported) CompleteStartup(false);
        completion_condition_.notify_all();
    }

    const std::size_t queue_capacity_;
    std::size_t ring_capacity_;
    std::atomic<LogLevel> minimum_level_;
    const std::chrono::milliseconds flush_interval_;
    const std::string session_id_;
    const std::uintmax_t max_file_size_bytes_;
    const std::size_t retained_archive_count_;
    std::unique_ptr<QueueCell[]> queue_;
    std::unique_ptr<RingCell[]> ring_;

    std::atomic<std::size_t> enqueue_position_{};
    std::atomic<std::size_t> dequeue_position_{};
    std::atomic<LoggerState> state_{LoggerState::Stopped};
    std::atomic<std::uint64_t> accepted_{};
    std::atomic<std::uint64_t> processed_{};
    std::atomic<std::uint64_t> requested_flush_{};
    std::atomic<std::uint64_t> flushed_{};
    std::atomic<std::uint64_t> active_producers_{};
    std::atomic<std::uint64_t> dropped_{};
    std::atomic<std::uint64_t> queue_full_dropped_{};
    std::atomic<std::uint64_t> inactive_dropped_{};
    std::atomic<std::uint64_t> error_count_{};
    std::atomic<std::uint64_t> worker_wakeups_{};
    std::atomic<std::size_t> ring_size_{};
    std::atomic_bool running_{};
    std::atomic_bool worker_exited_{true};

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex ring_mutex_;
    mutable std::mutex error_mutex_;
    std::mutex startup_mutex_;
    std::mutex completion_mutex_;
    std::condition_variable startup_condition_;
    std::condition_variable completion_condition_;
    std::jthread worker_;
    HANDLE wake_event_{};
    std::filesystem::path path_;
    std::optional<LoggerError> last_error_;
    std::size_t ring_write_position_{};
    std::size_t ring_count_{};
    bool startup_complete_{};
    bool startup_succeeded_{};
};

StructuredLogger::StructuredLogger(StructuredLoggerOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

StructuredLogger::~StructuredLogger() = default;

bool StructuredLogger::Start(const std::filesystem::path& path) {
    return impl_->Start(path);
}

bool StructuredLogger::Log(
    LogLevel level,
    std::string component,
    std::string message,
    LogDetails details) noexcept {
    return impl_->Log(level, std::move(component), std::move(message), std::move(details));
}

bool StructuredLogger::Log(
    LogLevel level,
    std::string component,
    std::string message,
    std::optional<PluginLogOwner> plugin) noexcept {
    LogDetails details;
    details.plugin = std::move(plugin);
    return impl_->Log(level, std::move(component), std::move(message), std::move(details));
}

bool StructuredLogger::Flush(std::chrono::milliseconds timeout) {
    return impl_->Flush(timeout);
}

bool StructuredLogger::Stop() {
    return impl_->Stop();
}

bool StructuredLogger::Reconfigure(
    const LogLevel minimum_level, const std::size_t ring_capacity) noexcept {
    return impl_->Reconfigure(minimum_level, ring_capacity);
}

std::vector<LogRecord> StructuredLogger::RingSnapshot() const {
    return impl_->RingSnapshot();
}

StructuredLoggerStats StructuredLogger::Stats() const noexcept {
    return impl_->Stats();
}

std::optional<LoggerError> StructuredLogger::LastError() const {
    return impl_->LastError();
}

bool StructuredLogger::IsRunning() const noexcept {
    return impl_->IsRunning();
}

std::string_view StructuredLogger::SessionId() const noexcept {
    return impl_->SessionId();
}

}  // namespace anomaly
