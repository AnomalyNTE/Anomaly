// Offline fixture for log retention.
//
// Size-based rotation bounds only the file a session is writing, so the logs
// directory grew with every launch instead of shrinking. This fixture pins the
// rules that now delete what earlier sessions left: the age rule, the total
// size rule, protection of the file still being written, and the sweep the
// structured logger performs when its worker opens the current log.
#include "anomaly/log_retention.hpp"
#include "anomaly/structured_logger.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool value, const std::string& message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}

void WriteBytes(const std::filesystem::path& path, std::size_t bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << std::string(bytes, 'x');
}

void AgeFile(const std::filesystem::path& path, std::chrono::hours age) {
    std::error_code error;
    const auto written = std::filesystem::file_time_type::clock::now() - age;
    std::filesystem::last_write_time(path, written, error);
    Check(!error, "age a fixture log file");
}

struct TempDirectory {
    std::filesystem::path path;

    explicit TempDirectory(std::string_view name) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("anomaly-log-retention-" + std::string{name} + "-" + std::to_string(stamp));
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
        Check(!error, "create the fixture directory");
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
};

bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

// Files left behind by earlier sessions disappear once they are older than the
// policy age; recent logs, non-log files and the file still being written stay.
void AgeRuleDeletesStaleLogs() {
    TempDirectory root{"age"};
    const auto text_log = root.path / L"anomaly-runtime.log";
    const auto archive = root.path / L"anomaly-runtime.jsonl.2";
    const auto current = root.path / L"anomaly-runtime.jsonl";
    const auto unrelated = root.path / L"notes.txt";
    const auto directory = root.path / L"archive.log";
    WriteBytes(text_log, 256);
    WriteBytes(archive, 256);
    WriteBytes(current, 256);
    WriteBytes(unrelated, 256);
    std::error_code directory_error;
    std::filesystem::create_directories(directory, directory_error);
    Check(!directory_error, "create a directory that looks like a log");
    AgeFile(text_log, std::chrono::hours{240});
    AgeFile(archive, std::chrono::hours{240});
    AgeFile(unrelated, std::chrono::hours{240});

    anomaly::LogRetentionPolicy policy;
    policy.max_age = std::chrono::hours{72};
    const auto result = anomaly::PruneLogFiles(root.path, policy);
    Check(!result.error, "the age pass reports no error");
    Check(result.removed_files == 2, "the age pass removes the stale text log and archive");
    Check(result.removed_bytes == 512, "the age pass counts the removed bytes");
    Check(!Exists(text_log), "a stale text log is deleted");
    Check(!Exists(archive), "a stale rotation archive is deleted");
    Check(Exists(current), "a recent log survives the age pass");
    Check(Exists(unrelated), "a non-log file survives the age pass");
    Check(Exists(directory), "a directory named like a log survives the age pass");
}

// The open log file has to survive its own sweep, otherwise the worker keeps
// writing into a deleted file and the next pass has nothing to read.
void ActiveFileSurvives() {
    TempDirectory root{"active"};
    const auto active = root.path / L"anomaly-runtime.jsonl";
    const auto stale = root.path / L"anomaly-runtime.jsonl.1";
    WriteBytes(active, 512);
    WriteBytes(stale, 512);
    AgeFile(active, std::chrono::hours{240});
    AgeFile(stale, std::chrono::hours{240});

    anomaly::LogRetentionPolicy policy;
    policy.max_age = std::chrono::hours{72};
    policy.active_path = active;
    const auto result = anomaly::PruneLogFiles(root.path, policy);
    Check(!result.error, "the active-file pass reports no error");
    Check(Exists(active), "the active log survives the age pass");
    Check(!Exists(stale), "the stale archive next to the active log is deleted");
    Check(result.retained_bytes == 512, "the active log still counts towards the retained bytes");

    const auto current = root.path / L"anomaly-runtime.log";
    WriteBytes(current, 4096);
    anomaly::LogRetentionPolicy size_policy;
    size_policy.max_total_bytes = 1024;
    size_policy.active_path = active;
    const auto capped = anomaly::PruneLogFiles(root.path, size_policy);
    Check(!capped.error, "the active-file pass reports no error under the size cap");
    Check(Exists(active), "the active log survives the size cap");
    Check(!Exists(current), "the size cap removes the other log first");
}

// Nothing in the age rule bounds a directory that is written every day, so the
// total size rule deletes from oldest to newest until the budget fits.
void SizeRuleDeletesOldestFirst() {
    TempDirectory root{"size"};
    const auto oldest = root.path / L"anomaly-runtime.jsonl.1";
    const auto middle = root.path / L"anomaly-runtime.jsonl";
    const auto newest = root.path / L"anomaly-runtime.log";
    WriteBytes(oldest, 4096);
    AgeFile(oldest, std::chrono::hours{30});
    WriteBytes(middle, 4096);
    AgeFile(middle, std::chrono::hours{20});
    WriteBytes(newest, 4096);

    anomaly::LogRetentionPolicy policy;
    policy.max_total_bytes = 9000;
    const auto result = anomaly::PruneLogFiles(root.path, policy);
    Check(!result.error, "the size pass reports no error");
    Check(result.removed_files == 1, "the size pass removes one file to fit the budget");
    Check(!Exists(oldest), "the oldest log is removed first");
    Check(Exists(middle), "the size pass stops before the newer log");
    Check(Exists(newest), "the newest log survives the size pass");
    Check(result.retained_bytes == 8192, "the size pass reports the surviving bytes");
}

// The logger drives the same pass at startup and then once per interval, so a
// player who never restarts the game still loses the old sessions' files.
void LoggerPrunesAtStartupAndPeriodically() {
    TempDirectory root{"logger"};
    const auto text_log = root.path / L"anomaly-runtime.log";
    const auto hook_log = root.path / L"nte_game_hook.log";
    WriteBytes(text_log, 256);
    WriteBytes(hook_log, 256);
    AgeFile(text_log, std::chrono::hours{240});
    AgeFile(hook_log, std::chrono::hours{240});

    anomaly::StructuredLoggerOptions options;
    options.retention_directories = {root.path};
    options.retention.max_age = std::chrono::hours{72};
    options.retention_interval = std::chrono::minutes{0};
    anomaly::StructuredLogger logger(std::move(options));
    const auto active = root.path / L"anomaly-runtime.jsonl";
    Check(logger.Start(active), "the structured logger starts");
    Check(!Exists(text_log), "the logger start deletes the expired runtime log");
    Check(!Exists(hook_log), "the logger start deletes the expired hook log");
    Check(Exists(active), "the logger start keeps its own log");

    const auto late = root.path / L"anomaly-runtime.log";
    WriteBytes(late, 256);
    AgeFile(late, std::chrono::hours{240});
    bool pruned{};
    for (int attempt = 0; attempt < 200 && !pruned; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        pruned = !Exists(late);
    }
    Check(pruned, "the running logger deletes expired logs without a restart");
    Check(logger.Stop(), "the structured logger stops");
}

// Another process can still hold an old log open. That must not turn into a
// logger failure, and the file has to go on the pass after it is released.
void BusyFileIsSkipped() {
    TempDirectory root{"busy"};
    const auto held = root.path / L"anomaly-runtime.log";
    WriteBytes(held, 64);
    AgeFile(held, std::chrono::hours{240});

    anomaly::LogRetentionPolicy policy;
    policy.max_age = std::chrono::hours{72};
    {
        std::ifstream holder(held, std::ios::binary);
        Check(holder.is_open(), "the fixture keeps the old log open");
        const auto result = anomaly::PruneLogFiles(root.path, policy);
        Check(Exists(held), "a log another process holds open is left for the next pass");
        Check(result.removed_files == 0, "the held log is not counted as removed");
        Check(!result.error, "holding a log open is not a retention failure");
    }

    const auto retried = anomaly::PruneLogFiles(root.path, policy);
    Check(!Exists(held), "the next pass deletes the released log");
    Check(retried.removed_files == 1, "the next pass counts the released log");
}

}  // namespace

int main() {
    AgeRuleDeletesStaleLogs();
    ActiveFileSurvives();
    SizeRuleDeletesOldestFirst();
    BusyFileIsSkipped();
    LoggerPrunesAtStartupAndPeriodically();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "log retention fixture passed\n";
    return 0;
}
