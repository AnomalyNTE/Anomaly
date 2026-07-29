#include "anomaly/crash_reporter.hpp"
#include "anomaly/thread_local_value.hpp"

#include <DbgHelp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>

namespace anomaly {
namespace {

std::atomic<CrashReporter*> g_reporter{};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previous_filter{};
std::atomic<bool> g_filter_in_flight{};
std::mutex g_registration_mutex;
ThreadLocalScalar<bool> g_current_thread_in_filter;

class AtomicBoolReset final {
public:
    explicit AtomicBoolReset(std::atomic<bool>& value) noexcept : value_(value) {}
    ~AtomicBoolReset() { value_.store(false, std::memory_order_release); }

    AtomicBoolReset(const AtomicBoolReset&) = delete;
    AtomicBoolReset& operator=(const AtomicBoolReset&) = delete;

private:
    std::atomic<bool>& value_;
};

std::string JsonString(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                char escaped[7]{};
                static_cast<void>(std::snprintf(
                    escaped, sizeof(escaped), "\\u%04x",
                    static_cast<unsigned int>(character)));
                result += escaped;
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    result.push_back('"');
    return result;
}

std::wstring TimestampUtc() noexcept {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t buffer[32]{};
    static_cast<void>(swprintf_s(
        buffer, L"%04u%02u%02uT%02u%02u%02u.%03uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds));
    return buffer;
}

}  // namespace

CrashReporter::CrashReporter(CrashReporterOptions options)
    : options_(std::move(options)) {}

CrashReporter::~CrashReporter() {
    Uninstall();
}

bool CrashReporter::Install(std::string* error) noexcept {
    try {
        std::scoped_lock registration_lock(g_registration_mutex);
        std::scoped_lock lock(mutex_);
        if (installed_) return true;
        std::error_code directory_error;
        std::filesystem::create_directories(options_.dump_directory, directory_error);
        if (directory_error) {
            if (error) *error = "crash dump directory create failed: " + directory_error.message();
            return false;
        }

        if (g_reporter.load(std::memory_order_acquire) != nullptr) {
            if (error) *error = "another crash reporter is already installed";
            return false;
        }

        const auto previous = SetUnhandledExceptionFilter(&CrashReporter::UnhandledFilter);
        previous_filter_.store(previous, std::memory_order_release);
        g_previous_filter.store(previous, std::memory_order_release);
        g_reporter.store(this, std::memory_order_release);
        installed_ = true;
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        if (error) *error = "crash reporter install failed";
        return false;
    }
}

void CrashReporter::Uninstall() noexcept {
    std::scoped_lock registration_lock(g_registration_mutex);
    std::scoped_lock lock(mutex_);
    if (!installed_) return;

    CrashReporter* expected = this;
    const bool removed = g_reporter.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    const auto previous = previous_filter_.load(std::memory_order_acquire);
    if (removed) {
        // Preserve a filter installed after this reporter rather than overwriting it
        // during teardown. SetUnhandledExceptionFilter has no read-only query API.
        const auto current = SetUnhandledExceptionFilter(nullptr);
        SetUnhandledExceptionFilter(
            current == &CrashReporter::UnhandledFilter ? previous : current);

        // The handler never takes mutex_. At most one handler owns the raw reporter
        // pointer, and teardown waits until that bounded lease is released.
        while (g_filter_in_flight.load(std::memory_order_acquire)) {
            SwitchToThread();
        }
    }
    g_previous_filter.store(nullptr, std::memory_order_release);
    previous_filter_.store(nullptr, std::memory_order_release);
    installed_ = false;
}

bool CrashReporter::Installed() const noexcept {
    std::scoped_lock lock(mutex_);
    return installed_;
}

CrashDumpResult CrashReporter::WriteDump(
    EXCEPTION_POINTERS* exception, std::string_view reason) noexcept {
    CrashDumpResult result;
    result.exception_code = exception != nullptr && exception->ExceptionRecord != nullptr
        ? exception->ExceptionRecord->ExceptionCode
        : 0U;
    bool expected{};
    if (!dump_in_progress_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        result.error = ERROR_BUSY;
        return result;
    }
    const AtomicBoolReset dump_guard(dump_in_progress_);
    try {
        std::error_code directory_error;
        std::filesystem::create_directories(options_.dump_directory, directory_error);
        if (directory_error) {
            result.error = ERROR_CANNOT_MAKE;
            return result;
        }

        const std::wstring stem = L"anomaly-" + TimestampUtc() + L"-p" +
            std::to_wstring(GetCurrentProcessId()) + L"-t" +
            std::to_wstring(GetCurrentThreadId()) + L"-" +
            std::to_wstring(sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
        result.dump_path = options_.dump_directory / (stem + L".dmp");
        result.metadata_path = options_.dump_directory / (stem + L".json");

        const HANDLE file = CreateFileW(
            result.dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            return result;
        }

        MINIDUMP_EXCEPTION_INFORMATION exception_information{};
        MINIDUMP_EXCEPTION_INFORMATION* exception_information_pointer{};
        if (exception != nullptr) {
            exception_information.ThreadId = GetCurrentThreadId();
            exception_information.ExceptionPointers = exception;
            exception_information.ClientPointers = FALSE;
            exception_information_pointer = &exception_information;
        }
        const BOOL written = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
            exception_information_pointer, nullptr, nullptr);
        result.error = written ? ERROR_SUCCESS : GetLastError();
        static_cast<void>(FlushFileBuffers(file));
        CloseHandle(file);
        if (!written) {
            std::error_code ignored;
            std::filesystem::remove(result.dump_path, ignored);
            return result;
        }

        std::ofstream metadata(result.metadata_path, std::ios::binary | std::ios::trunc);
        metadata << "{\n"
                 << "  \"schemaVersion\": 1,\n"
                 << "  \"runtimeVersion\": " << JsonString(options_.runtime_version) << ",\n"
                 << "  \"processId\": " << GetCurrentProcessId() << ",\n"
                 << "  \"threadId\": " << GetCurrentThreadId() << ",\n"
                 << "  \"exceptionCode\": " << result.exception_code << ",\n"
                 << "  \"reasonCode\": " << JsonString(reason) << ",\n"
                 << "  \"dumpType\": \"MiniDumpNormal\",\n"
                 << "  \"privacy\": {\n"
                 << "    \"containsFullMemory\": false,\n"
                 << "    \"containsHandleData\": false,\n"
                 << "    \"metadataContainsPaths\": false,\n"
                 << "    \"metadataContainsCommandLine\": false,\n"
                 << "    \"metadataContainsEnvironment\": false,\n"
                 << "    \"metadataContainsPluginPrivateConfiguration\": false,\n"
                 << "    \"dumpMayContainModulePathsAndStackMemory\": true\n"
                 << "  }\n"
                 << "}\n";
        metadata.flush();
        if (!metadata) {
            result.error = ERROR_WRITE_FAULT;
            return result;
        }
        result.written = true;
        return result;
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
        return result;
    }
}

LONG WINAPI CrashReporter::UnhandledFilter(EXCEPTION_POINTERS* exception) noexcept {
    // A previous filter that faults must not recursively re-enter the reporter on
    // the same thread. The outer call will retain its original chaining decision.
    if (g_current_thread_in_filter.Get()) return EXCEPTION_CONTINUE_SEARCH;

    bool expected{};
    if (!g_filter_in_flight.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        const auto previous = g_previous_filter.load(std::memory_order_acquire);
        return previous != nullptr && previous != &CrashReporter::UnhandledFilter
            ? previous(exception)
            : EXCEPTION_CONTINUE_SEARCH;
    }
    g_current_thread_in_filter.Set(true);

    LPTOP_LEVEL_EXCEPTION_FILTER previous =
        g_previous_filter.load(std::memory_order_acquire);
    CrashReporter* reporter = g_reporter.load(std::memory_order_acquire);
    if (reporter != nullptr) {
        previous = reporter->previous_filter_.load(std::memory_order_acquire);
        static_cast<void>(reporter->WriteDump(exception, "unhandled-exception"));
    }

    // Release the lifetime lease before invoking foreign filter code. A previous
    // filter is then free to uninstall this reporter without self-deadlocking.
    g_filter_in_flight.store(false, std::memory_order_release);
    const LONG disposition = previous != nullptr && previous != &CrashReporter::UnhandledFilter
        ? previous(exception)
        : EXCEPTION_CONTINUE_SEARCH;
    g_current_thread_in_filter.Set(false);
    return disposition;
}

}  // namespace anomaly
