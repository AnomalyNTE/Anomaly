#include "anomaly/structured_logger.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::filesystem::path TestRoot() {
    return std::filesystem::current_path() /
        ("structured-logger-test-" + std::to_string(GetCurrentProcessId()));
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path ArchivePath(const std::filesystem::path& path, std::size_t index) {
    auto archive = path;
    archive += std::filesystem::path{"." + std::to_string(index)};
    return archive;
}

std::size_t CountLines(const std::string& value) {
    std::size_t lines{};
    for (const char character : value) {
        if (character == '\n') ++lines;
    }
    return lines;
}

std::size_t CountOccurrences(std::string_view value, std::string_view needle) {
    std::size_t count{};
    std::size_t offset{};
    while ((offset = value.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::vector<std::string_view> Lines(const std::string& value) {
    std::vector<std::string_view> result;
    std::size_t start{};
    while (start < value.size()) {
        const std::size_t end = value.find('\n', start);
        if (end == std::string::npos) {
            result.emplace_back(value.data() + start, value.size() - start);
            break;
        }
        result.emplace_back(value.data() + start, end - start);
        start = end + 1;
    }
    return result;
}

bool HasRequiredSchema(std::string_view line) {
    constexpr std::string_view required[] = {
        "\"timestamp\":",
        "\"session_id\":",
        "\"pid\":",
        "\"tid\":",
        "\"thread_domain\":",
        "\"level\":",
        "\"component\":",
        "\"plugin_id\":",
        "\"plugin_generation\":",
        "\"event_id\":",
        "\"message\":",
        "\"fields\":",
    };
    for (const auto field : required) {
        if (line.find(field) == std::string_view::npos) return false;
    }
    return true;
}

bool IsValidUtf8(std::string_view value) {
    std::size_t offset{};
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first < 0x80U) {
            ++offset;
            continue;
        }

        std::size_t length{};
        if (first >= 0xC2U && first <= 0xDFU) length = 2;
        else if (first >= 0xE0U && first <= 0xEFU) length = 3;
        else if (first >= 0xF0U && first <= 0xF4U) length = 4;
        else return false;
        if (offset + length > value.size()) return false;

        const auto second = static_cast<unsigned char>(value[offset + 1]);
        if ((second & 0xC0U) != 0x80U) return false;
        if (first == 0xE0U && second < 0xA0U) return false;
        if (first == 0xEDU && second > 0x9FU) return false;
        if (first == 0xF0U && second < 0x90U) return false;
        if (first == 0xF4U && second > 0x8FU) return false;
        for (std::size_t index = 2; index < length; ++index) {
            if ((static_cast<unsigned char>(value[offset + index]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        offset += length;
    }
    return true;
}

bool TestConcurrentSequenceAndPersistence(const std::filesystem::path& root) {
    constexpr std::size_t producer_count = 8;
    constexpr std::size_t records_per_producer = 300;
    constexpr std::size_t total_records = producer_count * records_per_producer;
    const auto path = root / "concurrent.jsonl";
    anomaly::StructuredLogger logger({total_records + 32, total_records + 32, 1s});
    if (!Check(logger.Start(path), "Concurrent logger did not start")) return false;

    std::atomic_bool accepted{true};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (std::size_t index = 0; index < records_per_producer; ++index) {
                if (!logger.Log(anomaly::LogLevel::Info,
                                "producer-" + std::to_string(producer),
                                "record-" + std::to_string(index))) {
                    accepted.store(false);
                }
            }
        });
    }
    for (auto& producer : producers) producer.join();

    bool result = Check(accepted.load(), "Large queue dropped a concurrent record") &&
        Check(logger.Flush(5s), "Concurrent Flush timed out or failed");
    result = Check(CountLines(ReadFile(path)) == total_records,
                   "Flush did not make every accepted JSONL record durable") && result;
    result = Check(logger.Stop(), "Concurrent Stop failed") && result;
    const auto snapshot = logger.RingSnapshot();
    result = Check(snapshot.size() == total_records,
                   "Concurrent ring snapshot has the wrong size") && result;
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        result = Check(snapshot[index].sequence == index + 1,
                       "Concurrent sequence is not gap-free and ordered") && result;
        result = Check(snapshot[index].thread_id != 0,
                       "Concurrent record lost its thread ID") && result;
        result = Check(snapshot[index].process_id == GetCurrentProcessId(),
                       "Concurrent record lost its process ID") && result;
    }
    const auto stats = logger.Stats();
    result = Check(stats.accepted == total_records && stats.processed == total_records,
                   "Concurrent stats lost accepted records") && result;
    result = Check(stats.dropped == 0 && stats.queued == 0 && !stats.running,
                   "Concurrent stats reported a drop or pending record") && result;
    return Check(CountLines(ReadFile(path)) == total_records,
                 "Concurrent JSONL file has the wrong line count") && result;
}

bool TestRingLimitAndStopDrain(const std::filesystem::path& root) {
    const auto path = root / "ring.jsonl";
    anomaly::StructuredLogger logger({64, 5, 10s});
    if (!Check(logger.Start(path), "Ring logger did not start")) return false;
    for (int index = 0; index < 12; ++index) {
        if (!Check(logger.Log(anomaly::LogLevel::Debug, "ring", std::to_string(index)),
                   "Ring logger dropped a record")) {
            return false;
        }
    }
    if (!Check(logger.Stop(), "Stop did not drain the ring logger")) return false;

    const auto snapshot = logger.RingSnapshot();
    bool result = Check(snapshot.size() == 5, "Ring capacity was not enforced");
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        result = Check(snapshot[index].sequence == index + 8,
                       "Ring did not retain the newest sequence window") && result;
    }
    return Check(CountLines(ReadFile(path)) == 12,
                 "Stop did not persist every accepted record") && result;
}

bool TestRuntimeReconfiguration(const std::filesystem::path& root) {
    const auto path = root / "reconfigure.jsonl";
    anomaly::StructuredLogger logger({32, 8, 1s});
    if (!Check(logger.Start(path), "Reconfiguration logger did not start")) return false;
    bool result = Check(logger.Log(anomaly::LogLevel::Debug, "before", "debug"),
        "Logger rejected a pre-reconfiguration record");
    result = Check(logger.Log(anomaly::LogLevel::Info, "before", "info"),
        "Logger rejected a second pre-reconfiguration record") && result;
    result = Check(logger.Flush(2s), "Pre-reconfiguration records did not flush") && result;
    result = Check(logger.Reconfigure(anomaly::LogLevel::Warning, 2),
        "Logger reconfiguration failed") && result;
    result = Check(!logger.Log(anomaly::LogLevel::Info, "after", "filtered"),
        "Minimum log level did not filter an info record") && result;
    result = Check(logger.Log(anomaly::LogLevel::Warning, "after", "warning") &&
        logger.Log(anomaly::LogLevel::Error, "after", "error"),
        "Logger rejected records at or above the new level") && result;
    result = Check(logger.Stop(), "Reconfigured logger did not stop") && result;
    const auto snapshot = logger.RingSnapshot();
    result = Check(snapshot.size() == 2 && snapshot[0].level == anomaly::LogLevel::Warning &&
            snapshot[1].level == anomaly::LogLevel::Error,
        "Reconfigured ring did not retain the newest two records") && result;
    return Check(CountLines(ReadFile(path)) == 4,
        "Filtered record was written after reconfiguration") && result;
}

bool TestJsonEscapingAndOwner(const std::filesystem::path& root) {
    const auto path = root / "escaping.jsonl";
    anomaly::StructuredLogger logger({16, 16, 1s});
    if (!Check(logger.Start(path), "Escaping logger did not start")) return false;

    std::string message = "line\n\t\b\f\r\"\\";
    message.push_back('\x01');
    const std::string utf8 = "\xE4\xB8\xAD\xE6\x96\x87";
    message.append(utf8);
    const anomaly::PluginLogOwner owner{"plugin\"\\", 42};
    bool result = Check(logger.Log(anomaly::LogLevel::Warning, "cat\"\\", message, owner),
                        "Escaping record was dropped") &&
        Check(logger.Flush(2s), "Escaping Flush failed");

    const std::string flushed_file = ReadFile(path);
    result = Check(CountLines(flushed_file) == 1,
                   "Flush did not expose the escaped record before Stop") && result;
    result = Check(logger.Stop(), "Escaping Stop failed") && result;

    const std::string file = ReadFile(path);
    result = Check(file.find("\"component\":\"cat\\\"\\\\\"") != std::string::npos,
                   "Component JSON escaping is incorrect") && result;
    result = Check(file.find("line\\n\\t\\b\\f\\r\\\"\\\\\\u0001") !=
                       std::string::npos,
                   "Message JSON escaping is incorrect") && result;
    result = Check(file.find(utf8) != std::string::npos,
                   "UTF-8 bytes were modified") && result;
    result = Check(file.find("\"plugin_id\":\"plugin\\\"\\\\\","
                             "\"plugin_generation\":42") != std::string::npos,
                   "Plugin owner or generation was not serialized") && result;
    result = Check(file.find("\"level\":\"warning\"") != std::string::npos,
                   "Log level was not serialized") && result;
    return Check(CountLines(file) == 1, "Escaped control bytes broke JSONL framing") && result;
}

bool TestCorrelationSchemaAndDetails(const std::filesystem::path& root) {
    const auto path = root / "correlation.jsonl";
    anomaly::StructuredLoggerOptions options{16, 16, 1s, "session-fixed"};
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Correlation logger did not start")) return false;

    anomaly::LogDetails details;
    details.thread_domain = anomaly::LogThreadDomain::Render;
    details.event_id = "renderer.present";
    details.fields = {{"swap_chain", "primary"}, {"frame", "17"}};
    details.plugin = anomaly::PluginLogOwner{"sample.plugin", 9};
    const auto producer_thread_id = GetCurrentThreadId();
    bool result = Check(
        logger.Log(anomaly::LogLevel::Info, "renderer", "detailed", std::move(details)),
        "Correlation logger dropped the detailed record");
    result = Check(logger.Log(anomaly::LogLevel::Debug, "runtime", "default"),
                   "Correlation logger dropped the default record") && result;
    result = Check(logger.Stop(), "Correlation logger Stop failed") && result;

    const auto file = ReadFile(path);
    const auto lines = Lines(file);
    result = Check(lines.size() == 2, "Correlation logger wrote the wrong line count") && result;
    for (const auto line : lines) {
        result = Check(HasRequiredSchema(line),
                       "A JSONL record is missing a required correlation field") && result;
        result = Check(line.find("\"session_id\":\"session-fixed\"") !=
                           std::string_view::npos,
                       "A JSONL record lost the configured session ID") && result;
        result = Check(line.find("\"pid\":" +
                                 std::to_string(GetCurrentProcessId())) !=
                           std::string_view::npos,
                       "A JSONL record lost its process ID") && result;
        result = Check(line.find("\"tid\":" + std::to_string(producer_thread_id)) !=
                           std::string_view::npos,
                       "A JSONL record lost its producer thread ID") && result;
    }
    if (lines.size() == 2) {
        result = Check(lines[0].find("\"timestamp\":\"20") !=
                           std::string_view::npos,
                       "Detailed record lost its timestamp") && result;
        result = Check(lines[0].find("\"thread_domain\":\"render\"") !=
                           std::string_view::npos,
                       "Detailed record lost its thread domain") && result;
        result = Check(lines[0].find("\"level\":\"info\"") !=
                           std::string_view::npos,
                       "Detailed record lost its level") && result;
        result = Check(lines[0].find("\"component\":\"renderer\"") !=
                           std::string_view::npos,
                       "Detailed record lost its component") && result;
        result = Check(lines[0].find("\"plugin_id\":\"sample.plugin\",") !=
                           std::string_view::npos,
                       "Detailed record lost its plugin ID") && result;
        result = Check(lines[0].find("\"plugin_generation\":9") !=
                           std::string_view::npos,
                       "Detailed record lost its plugin generation") && result;
        result = Check(lines[0].find("\"event_id\":\"renderer.present\"") !=
                           std::string_view::npos,
                       "Detailed record lost its event ID") && result;
        result = Check(lines[0].find("\"message\":\"detailed\"") !=
                           std::string_view::npos,
                       "Detailed record lost its message") && result;
        result = Check(lines[0].find(
                           "\"fields\":{\"swap_chain\":\"primary\",\"frame\":\"17\"}") !=
                           std::string_view::npos,
                       "Detailed record lost its fields") && result;
        result = Check(lines[1].find("\"thread_domain\":\"unknown\"") !=
                           std::string_view::npos,
                       "Default record has the wrong thread domain") && result;
        result = Check(lines[1].find(
                           "\"plugin_id\":null,\"plugin_generation\":null") !=
                           std::string_view::npos,
                       "Default record does not use consistent null plugin fields") && result;
        result = Check(lines[1].find("\"event_id\":\"\"") !=
                           std::string_view::npos &&
                           lines[1].find("\"fields\":{}") != std::string_view::npos,
                       "Default record does not include empty event and fields values") && result;
    }

    const auto snapshot = logger.RingSnapshot();
    result = Check(snapshot.size() == 2, "Correlation ring snapshot has the wrong size") && result;
    if (snapshot.size() == 2) {
        result = Check(snapshot[0].component == "renderer" &&
                           snapshot[0].details.event_id == "renderer.present" &&
                           snapshot[0].details.fields.size() == 2,
                       "Correlation ring snapshot lost structured details") && result;
    }
    return result;
}

bool TestGeneratedSessionSurvivesRestart(const std::filesystem::path& root) {
    const auto first_path = root / "session-first.jsonl";
    const auto second_path = root / "session-second.jsonl";
    anomaly::StructuredLogger logger({16, 16, 1s});
    const std::string session_id{logger.SessionId()};
    bool result = Check(!session_id.empty(), "Generated session ID is empty");
    if (!Check(logger.Start(first_path), "First session logger Start failed")) return false;
    result = Check(logger.Log(anomaly::LogLevel::Info, "session", "first"),
                   "First session record was dropped") && result;
    result = Check(logger.Stop(), "First session logger Stop failed") && result;
    result = Check(logger.SessionId() == session_id,
                   "Session ID changed after the first Stop") && result;

    if (!Check(logger.Start(second_path), "Second session logger Start failed")) return false;
    result = Check(logger.Log(anomaly::LogLevel::Info, "session", "second"),
                   "Second session record was dropped") && result;
    result = Check(logger.Stop(), "Second session logger Stop failed") && result;
    result = Check(logger.SessionId() == session_id,
                   "Session ID changed across Start calls") && result;

    const std::string serialized = "\"session_id\":\"" + session_id + "\"";
    result = Check(ReadFile(first_path).find(serialized) != std::string::npos,
                   "First Start did not serialize the generated session ID") && result;
    return Check(ReadFile(second_path).find(serialized) != std::string::npos,
                 "Second Start did not reuse the generated session ID") && result;
}

bool TestInvalidUtf8IsReplaced(const std::filesystem::path& root) {
    const auto path = root / "invalid-utf8.jsonl";
    const std::string invalid_byte(1, static_cast<char>(0x80));
    anomaly::StructuredLoggerOptions options{16, 16, 1s, "session-" + invalid_byte};
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Invalid UTF-8 logger did not start")) return false;

    anomaly::LogDetails details;
    details.thread_domain = anomaly::LogThreadDomain::Diagnostics;
    details.event_id = "event-" + invalid_byte;
    details.fields = {{"key-" + invalid_byte, "value-" + invalid_byte}};
    details.plugin = anomaly::PluginLogOwner{"plugin-" + invalid_byte, 4};
    std::string message = "valid-\xE4\xB8\xAD-";
    message.append("\xC0\xAF", 2);
    message.append("\xED\xA0\x80", 3);
    message.append("\xE2\x82", 2);
    bool result = Check(logger.Log(
                            anomaly::LogLevel::Error,
                            "component-" + invalid_byte,
                            std::move(message),
                            std::move(details)),
                        "Invalid UTF-8 record was dropped");
    result = Check(logger.Stop(), "Invalid UTF-8 logger Stop failed") && result;

    const auto file = ReadFile(path);
    result = Check(IsValidUtf8(file), "JSONL output contains invalid UTF-8") && result;
    result = Check(CountLines(file) == 1 && HasRequiredSchema(file),
                   "Invalid UTF-8 record broke the JSONL schema") && result;
    result = Check(CountOccurrences(file, "\\ufffd") >= 8,
                   "Invalid UTF-8 sequences were not replaced consistently") && result;
    return Check(file.find("valid-\xE4\xB8\xAD-") != std::string::npos,
                 "Valid UTF-8 adjacent to invalid input was modified") && result;
}

bool TestBoundedDropCounter(const std::filesystem::path& root) {
    const auto path = root / "drops.jsonl";
    anomaly::StructuredLogger logger({2, 8, 10s});
    if (!Check(logger.Start(path), "Drop logger did not start")) return false;

    constexpr std::size_t producer_count = 6;
    constexpr std::size_t attempts_per_producer = 80;
    std::vector<std::vector<std::string>> payloads(producer_count);
    for (auto& producer_payloads : payloads) {
        producer_payloads.reserve(attempts_per_producer);
        for (std::size_t index = 0; index < attempts_per_producer; ++index) {
            producer_payloads.emplace_back(64 * 1024, 'x');
        }
    }

    std::vector<std::thread> producers;
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (auto& payload : payloads[producer]) {
                static_cast<void>(logger.Log(
                    anomaly::LogLevel::Trace, "drop", std::move(payload)));
            }
        });
    }
    for (auto& producer : producers) producer.join();
    bool result = Check(logger.Stop(), "Drop logger Stop failed");
    const auto stats = logger.Stats();
    result = Check(stats.queue_full_dropped > 0,
                   "Bounded queue did not report a full-queue drop") && result;
    result = Check(stats.dropped == stats.queue_full_dropped,
                   "Drop counters are inconsistent") && result;
    result = Check(stats.accepted + stats.dropped ==
                       producer_count * attempts_per_producer,
                   "Drop accounting did not cover every attempt") && result;
    return Check(CountLines(ReadFile(path)) == stats.accepted,
                 "Dropped records appeared in the JSONL file") && result;
}

bool TestOpenErrorIsQueryable(const std::filesystem::path& root) {
    const auto directory_path = root / "not-a-file";
    std::filesystem::create_directory(directory_path);
    anomaly::StructuredLogger logger({8, 8, 10ms});
    bool result = Check(!logger.Start(directory_path),
                        "Opening a directory as JSONL unexpectedly succeeded");
    const auto error = logger.LastError();
    result = Check(error.has_value(), "Open failure was not queryable") && result;
    result = Check(error && error->operation == anomaly::LoggerErrorOperation::Open,
                   "Open failure reported the wrong operation") && result;
    result = Check(error && error->path == directory_path,
                   "Open failure lost its path") && result;
    result = Check(!logger.Log(anomaly::LogLevel::Error, "error", "inactive"),
                   "Failed-start logger accepted a record") && result;
    const auto stats = logger.Stats();
    return Check(stats.error_count > 0 && stats.inactive_dropped == 1,
                 "Open error or inactive drop was not counted") && result;
}

bool TestIdleWaitAndPromptWake(const std::filesystem::path& root) {
    const auto idle_path = root / "idle.jsonl";
    anomaly::StructuredLogger idle_logger({16, 16, 20ms});
    if (!Check(idle_logger.Start(idle_path), "Idle logger did not start")) return false;
    std::this_thread::sleep_for(120ms);
    const auto idle_stats = idle_logger.Stats();
    bool result = Check(idle_stats.worker_wakeups <= 12,
                        "Idle logger worker is waking continuously");
    result = Check(idle_logger.Stop(), "Idle logger did not stop") && result;

    const auto wake_path = root / "prompt-wake.jsonl";
    anomaly::StructuredLogger wake_logger({64, 16, std::chrono::hours{1}});
    if (!Check(wake_logger.Start(wake_path), "Prompt-wake logger did not start")) {
        return false;
    }
    for (int index = 0; index < 100; ++index) {
        result = Check(wake_logger.Log(
                           anomaly::LogLevel::Info, "wake", std::to_string(index)),
                       "Prompt-wake logger dropped a record") &&
            result;
        result = Check(wake_logger.Flush(500ms),
                       "Flush missed a worker notification") && result;
    }
    const auto stop_started = std::chrono::steady_clock::now();
    result = Check(wake_logger.Stop(), "Prompt-wake logger did not stop") && result;
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    result = Check(stop_elapsed < 500ms, "Stop missed a worker notification") && result;
    return Check(CountLines(ReadFile(wake_path)) == 100,
                 "Prompt-wake logger did not persist every record") && result;
}

bool TestMultipleRotationAndRetention(const std::filesystem::path& root) {
    const auto path = root / "rolling.jsonl";
    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 32;
    options.ring_capacity = 32;
    options.flush_interval = 1h;
    options.max_file_size_bytes = 1;
    options.retained_archive_count = 2;
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Rolling logger did not start")) return false;

    bool result{true};
    for (int index = 0; index < 6; ++index) {
        result = Check(logger.Log(
                           anomaly::LogLevel::Info,
                           "rolling",
                           "roll-" + std::to_string(index)),
                       "Rolling logger dropped a record") && result;
    }
    result = Check(logger.Flush(2s), "Rolling Flush failed") && result;
    result = Check(ReadFile(path).find("\"message\":\"roll-5\"") !=
                           std::string::npos &&
                       ReadFile(ArchivePath(path, 1)).find(
                           "\"message\":\"roll-4\"") != std::string::npos &&
                       ReadFile(ArchivePath(path, 2)).find(
                           "\"message\":\"roll-3\"") != std::string::npos,
                   "Flush did not persist records across current and archives") && result;
    result = Check(logger.Stop(), "Rolling Stop failed") && result;

    const auto current = ReadFile(path);
    const auto newest = ReadFile(ArchivePath(path, 1));
    const auto oldest = ReadFile(ArchivePath(path, 2));
    result = Check(CountLines(current) == 1 &&
                       current.find("\"message\":\"roll-5\"") != std::string::npos,
                   "Current rolling file does not contain only the newest record") && result;
    result = Check(CountLines(newest) == 1 &&
                       newest.find("\"message\":\"roll-4\"") != std::string::npos,
                   "PATH.1 is not the newest archive") && result;
    result = Check(CountLines(oldest) == 1 &&
                       oldest.find("\"message\":\"roll-3\"") != std::string::npos,
                   "PATH.2 is not the oldest retained archive") && result;
    result = Check(!std::filesystem::exists(ArchivePath(path, 3)),
                   "Rolling logger exceeded its archive retention count") && result;
    const auto stats = logger.Stats();
    return Check(stats.accepted == 6 && stats.processed == 6 && stats.error_count == 0,
                 "Rolling logger stats report lost records or an I/O error") && result;
}

bool TestProjectedSizeTriggersRotation(const std::filesystem::path& root) {
    const auto measurement_path = root / "rolling-measurement.jsonl";
    anomaly::StructuredLoggerOptions measurement_options;
    measurement_options.queue_capacity = 8;
    measurement_options.ring_capacity = 8;
    measurement_options.flush_interval = 1h;
    measurement_options.session_id = "rolling-size-session";
    anomaly::StructuredLogger measurement_logger(std::move(measurement_options));
    if (!Check(measurement_logger.Start(measurement_path),
               "Rolling-size measurement logger did not start")) {
        return false;
    }
    bool result = Check(measurement_logger.Log(
                            anomaly::LogLevel::Info,
                            "rolling-size",
                            std::string(64, 'm')),
                        "Rolling-size measurement record was dropped");
    result = Check(measurement_logger.Stop(),
                   "Rolling-size measurement Stop failed") && result;
    const std::uintmax_t one_record_bytes = std::filesystem::file_size(measurement_path);
    if (!Check(one_record_bytes > 0, "Rolling-size measurement file is empty")) {
        return false;
    }

    const auto path = root / "rolling-projected.jsonl";
    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 8;
    options.ring_capacity = 8;
    options.flush_interval = 1h;
    options.session_id = "rolling-size-session";
    options.max_file_size_bytes = one_record_bytes + one_record_bytes / 2;
    options.retained_archive_count = 1;
    const std::uintmax_t configured_limit = options.max_file_size_bytes;
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Projected-size logger did not start")) return false;
    for (int index = 0; index < 2; ++index) {
        result = Check(logger.Log(
                           anomaly::LogLevel::Info,
                           "rolling-size",
                           std::string(64, 'm')),
                       "Projected-size logger dropped a record") && result;
    }
    result = Check(logger.Stop(), "Projected-size Stop failed") && result;
    result = Check(CountLines(ReadFile(path)) == 1 &&
                       CountLines(ReadFile(ArchivePath(path, 1))) == 1,
                   "Projected file size did not trigger a pre-write rotation") && result;
    result = Check(std::filesystem::file_size(path) <= configured_limit &&
                       std::filesystem::file_size(ArchivePath(path, 1)) <=
                           configured_limit,
                   "Normal-sized records exceeded the configured file size limit") && result;
    return Check(!std::filesystem::exists(ArchivePath(path, 2)),
                 "Projected-size logger exceeded its archive retention count") && result;
}

bool TestZeroArchiveRetentionTruncatesCurrent(const std::filesystem::path& root) {
    const auto path = root / "zero-retention.jsonl";
    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 16;
    options.ring_capacity = 16;
    options.flush_interval = 1h;
    options.max_file_size_bytes = 1;
    options.retained_archive_count = 0;
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Zero-retention logger did not start")) return false;

    bool result = Check(logger.Log(anomaly::LogLevel::Info, "rolling", "discarded"),
                        "Zero-retention logger dropped its first record");
    result = Check(logger.Log(anomaly::LogLevel::Info, "rolling", "retained"),
                   "Zero-retention logger dropped its second record") && result;
    result = Check(logger.Stop(), "Zero-retention Stop failed") && result;

    const auto current = ReadFile(path);
    result = Check(CountLines(current) == 1 &&
                       current.find("\"message\":\"retained\"") != std::string::npos,
                   "Zero archive retention did not truncate current on roll") && result;
    return Check(!std::filesystem::exists(ArchivePath(path, 1)),
                 "Zero archive retention unexpectedly created PATH.1") && result;
}

bool TestAppendAndStartupRotationAcrossStarts(const std::filesystem::path& root) {
    const auto append_path = root / "append-disabled.jsonl";
    anomaly::StructuredLoggerOptions append_options;
    append_options.queue_capacity = 16;
    append_options.ring_capacity = 16;
    append_options.flush_interval = 1h;
    append_options.max_file_size_bytes = 0;
    append_options.retained_archive_count = 2;
    anomaly::StructuredLogger append_logger(std::move(append_options));
    if (!Check(append_logger.Start(append_path), "Append logger first Start failed")) {
        return false;
    }
    bool result = Check(append_logger.Log(anomaly::LogLevel::Info, "append", "first"),
                        "Append logger dropped its first record");
    result = Check(append_logger.Stop(), "Append logger first Stop failed") && result;
    if (!Check(append_logger.Start(append_path), "Append logger second Start failed")) {
        return false;
    }
    result = Check(append_logger.Log(anomaly::LogLevel::Info, "append", "second"),
                   "Append logger dropped its second record") && result;
    result = Check(append_logger.Stop(), "Append logger second Stop failed") && result;
    const auto appended = ReadFile(append_path);
    result = Check(CountLines(appended) == 2 &&
                       appended.find("\"message\":\"first\"") != std::string::npos &&
                       appended.find("\"message\":\"second\"") != std::string::npos,
                   "A zero size limit did not preserve append behavior") && result;
    result = Check(!std::filesystem::exists(ArchivePath(append_path, 1)),
                   "A zero size limit unexpectedly rolled an archive") && result;

    const auto rolling_path = root / "startup-roll.jsonl";
    anomaly::StructuredLoggerOptions rolling_options;
    rolling_options.queue_capacity = 16;
    rolling_options.ring_capacity = 16;
    rolling_options.flush_interval = 1h;
    rolling_options.max_file_size_bytes = 1;
    rolling_options.retained_archive_count = 2;
    anomaly::StructuredLogger rolling_logger(std::move(rolling_options));
    if (!Check(rolling_logger.Start(rolling_path), "Startup-roll first Start failed")) {
        return false;
    }
    result = Check(rolling_logger.Log(anomaly::LogLevel::Info, "startup", "before"),
                   "Startup-roll logger dropped its first record") && result;
    result = Check(rolling_logger.Stop(), "Startup-roll first Stop failed") && result;
    if (!Check(rolling_logger.Start(rolling_path), "Startup-roll second Start failed")) {
        return false;
    }
    result = Check(ReadFile(rolling_path).empty(),
                   "An oversized append target was not rolled during Start") && result;
    result = Check(ReadFile(ArchivePath(rolling_path, 1)).find(
                       "\"message\":\"before\"") != std::string::npos,
                   "Startup rotation did not preserve the previous current file") && result;
    result = Check(rolling_logger.Log(anomaly::LogLevel::Info, "startup", "after"),
                   "Startup-roll logger dropped its second record") && result;
    result = Check(rolling_logger.Stop(), "Startup-roll second Stop failed") && result;
    return Check(ReadFile(rolling_path).find("\"message\":\"after\"") !=
                     std::string::npos,
                 "Startup rotation did not reopen a writable current file") && result;
}

bool TestSingleOversizedRecordStaysIntact(const std::filesystem::path& root) {
    const auto path = root / "oversized-record.jsonl";
    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 8;
    options.ring_capacity = 8;
    options.flush_interval = 1h;
    options.max_file_size_bytes = 512;
    options.retained_archive_count = 2;
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Oversized-record logger did not start")) return false;

    const std::string payload(4096, 'z');
    bool result = Check(logger.Log(anomaly::LogLevel::Warning, "oversized", payload),
                        "Oversized-record logger dropped the record");
    result = Check(logger.Flush(2s), "Oversized-record Flush failed") && result;
    result = Check(logger.Stop(), "Oversized-record Stop failed") && result;
    const auto current = ReadFile(path);
    result = Check(CountLines(current) == 1 && current.find(payload) != std::string::npos,
                   "A single oversized record was split or truncated") && result;
    result = Check(std::filesystem::file_size(path) > 512,
                   "A single oversized record did not exceed the configured limit intact") &&
        result;
    return Check(!std::filesystem::exists(ArchivePath(path, 1)),
                 "A single oversized record was rolled out of an empty current file") && result;
}

bool SeedThreeArchives(const std::filesystem::path& path) {
    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 16;
    options.ring_capacity = 16;
    options.flush_interval = 1h;
    options.max_file_size_bytes = 1;
    options.retained_archive_count = 3;
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Archive seed logger did not start")) return false;

    bool result{true};
    for (int index = 0; index < 4; ++index) {
        result = Check(logger.Log(
                           anomaly::LogLevel::Info,
                           "retention",
                           "seed-" + std::to_string(index)),
                       "Archive seed logger dropped a record") && result;
    }
    result = Check(logger.Stop(), "Archive seed logger did not stop") && result;
    return Check(
               std::filesystem::exists(ArchivePath(path, 1)) &&
                   std::filesystem::exists(ArchivePath(path, 2)) &&
                   std::filesystem::exists(ArchivePath(path, 3)),
               "Archive seed logger did not create three archives") && result;
}

bool TestStartupCleansReducedRetention(const std::filesystem::path& root) {
    const auto retain_one_path = root / "retention-reduced-to-one.jsonl";
    if (!SeedThreeArchives(retain_one_path)) return false;

    anomaly::StructuredLoggerOptions retain_one_options;
    retain_one_options.queue_capacity = 8;
    retain_one_options.ring_capacity = 8;
    retain_one_options.flush_interval = 1h;
    retain_one_options.retained_archive_count = 1;
    anomaly::StructuredLogger retain_one(std::move(retain_one_options));
    if (!Check(retain_one.Start(retain_one_path),
               "Retention-one cleanup logger did not start")) {
        return false;
    }
    bool result = Check(retain_one.Stop(),
                        "Retention-one cleanup logger did not stop");
    result = Check(std::filesystem::exists(ArchivePath(retain_one_path, 1)) &&
                       !std::filesystem::exists(ArchivePath(retain_one_path, 2)) &&
                       !std::filesystem::exists(ArchivePath(retain_one_path, 3)),
                   "Startup did not clean archives after retention changed from three to one") &&
        result;

    const auto retain_zero_path = root / "retention-reduced-to-zero.jsonl";
    if (!SeedThreeArchives(retain_zero_path)) return false;

    anomaly::StructuredLoggerOptions retain_zero_options;
    retain_zero_options.queue_capacity = 8;
    retain_zero_options.ring_capacity = 8;
    retain_zero_options.flush_interval = 1h;
    retain_zero_options.retained_archive_count = 0;
    anomaly::StructuredLogger retain_zero(std::move(retain_zero_options));
    if (!Check(retain_zero.Start(retain_zero_path),
               "Retention-zero cleanup logger did not start")) {
        return false;
    }
    result = Check(retain_zero.Stop(),
                   "Retention-zero cleanup logger did not stop") && result;
    return Check(!std::filesystem::exists(ArchivePath(retain_zero_path, 1)) &&
                     !std::filesystem::exists(ArchivePath(retain_zero_path, 2)) &&
                     !std::filesystem::exists(ArchivePath(retain_zero_path, 3)),
                 "Startup did not clean archives after retention changed from three to zero") &&
        result;
}

bool TestArchiveRetentionClamp(const std::filesystem::path& root) {
    const auto path = root / "retention-clamp.jsonl";
    {
        std::ofstream retained(
            ArchivePath(path, anomaly::kMaximumRetainedLogArchives),
            std::ios::binary | std::ios::trunc);
        retained << "retained\n";
        std::ofstream excess(
            ArchivePath(path, anomaly::kMaximumRetainedLogArchives + 1),
            std::ios::binary | std::ios::trunc);
        excess << "excess\n";
    }

    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 8;
    options.ring_capacity = 8;
    options.flush_interval = 1h;
    options.retained_archive_count = (std::numeric_limits<std::size_t>::max)();
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Retention-clamp logger did not start")) return false;

    bool result = Check(logger.Stop(), "Retention-clamp logger did not stop");
    return Check(
               std::filesystem::exists(
                   ArchivePath(path, anomaly::kMaximumRetainedLogArchives)) &&
                   !std::filesystem::exists(
                       ArchivePath(path, anomaly::kMaximumRetainedLogArchives + 1)),
               "Archive retention was not clamped to the documented maximum") &&
        result;
}

bool TestLockedArchiveDefersRotation(const std::filesystem::path& root) {
    const auto path = root / "locked-archive.jsonl";
    const auto archive = ArchivePath(path, 1);
    {
        std::ofstream stream(archive, std::ios::binary | std::ios::trunc);
        stream << "previous archive\n";
    }

    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 16;
    options.ring_capacity = 16;
    options.flush_interval = 1h;
    options.max_file_size_bytes = 1;
    options.retained_archive_count = 1;
    anomaly::StructuredLogger logger(std::move(options));
    if (!Check(logger.Start(path), "Locked-archive logger did not start")) return false;

    bool result = Check(logger.Log(
                            anomaly::LogLevel::Info, "locked-archive", "before-lock"),
                        "Locked-archive logger dropped its first record");
    result = Check(logger.Flush(2s),
                   "Locked-archive logger did not flush its first record") && result;

    const HANDLE archive_lock = CreateFileW(
        archive.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (!Check(archive_lock != INVALID_HANDLE_VALUE,
               "Failed to lock archive against deletion")) {
        static_cast<void>(logger.Stop());
        return false;
    }

    result = Check(logger.Log(
                       anomaly::LogLevel::Info, "locked-archive", "during-lock"),
                   "Locked-archive logger dropped the deferred record") && result;
    static_cast<void>(logger.Flush(2s));
    const auto deferred_current = ReadFile(path);
    const auto deferred_error = logger.LastError();
    result = Check(logger.IsRunning(),
                   "A locked archive stopped the logger worker") && result;
    result = Check(deferred_current.find("\"message\":\"before-lock\"") !=
                           std::string::npos &&
                       deferred_current.find("\"message\":\"during-lock\"") !=
                           std::string::npos,
                   "A deferred rotation did not keep writing the current file") && result;
    result = Check(deferred_error &&
                       deferred_error->operation == anomaly::LoggerErrorOperation::Rotate &&
                       deferred_error->path == archive,
                   "A deferred rotation did not report the locked archive path") && result;

    CloseHandle(archive_lock);
    result = Check(logger.Log(
                       anomaly::LogLevel::Info, "locked-archive", "after-unlock"),
                   "Locked-archive logger dropped the retry record") && result;
    static_cast<void>(logger.Flush(2s));
    static_cast<void>(logger.Stop());

    const auto current = ReadFile(path);
    const auto newest_archive = ReadFile(archive);
    result = Check(current.find("\"message\":\"after-unlock\"") !=
                           std::string::npos &&
                       current.find("\"message\":\"during-lock\"") ==
                           std::string::npos,
                   "The unlocked retry did not create a new current file") && result;
    result = Check(newest_archive.find("\"message\":\"before-lock\"") !=
                           std::string::npos &&
                       newest_archive.find("\"message\":\"during-lock\"") !=
                           std::string::npos,
                   "The unlocked retry did not preserve deferred records in PATH.1") && result;
    const auto stats = logger.Stats();
    return Check(stats.accepted == 3 && stats.processed == 3 &&
                     stats.error_count > 0 && !stats.running,
                 "Deferred rotation stats do not reflect the recovered write sequence") &&
        result;
}

bool TestRotationErrorIsQueryable(const std::filesystem::path& root) {
    const auto path = root / "rotation-error.jsonl";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << std::string(128, 'x');
    }
    std::filesystem::create_directory(ArchivePath(path, 1));

    anomaly::StructuredLoggerOptions options;
    options.queue_capacity = 8;
    options.ring_capacity = 8;
    options.flush_interval = 1h;
    options.max_file_size_bytes = 1;
    options.retained_archive_count = 1;
    anomaly::StructuredLogger logger(std::move(options));
    bool result = Check(!logger.Start(path),
                        "Startup rotation unexpectedly replaced an archive directory");
    const auto error = logger.LastError();
    result = Check(error.has_value(), "Rotation failure was not queryable") && result;
    result = Check(error && error->operation == anomaly::LoggerErrorOperation::Rotate,
                   "Rotation failure reported the wrong operation") && result;
    result = Check(error && error->path == ArchivePath(path, 1),
                   "Rotation failure lost the failing archive path") && result;
    const auto stats = logger.Stats();
    return Check(stats.error_count > 0 && !stats.running,
                 "Rotation failure did not update logger health") && result;
}

}  // namespace

int main() {
    const auto root = TestRoot();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    int result{};
    if (!TestConcurrentSequenceAndPersistence(root)) result = 1;
    else if (!TestRingLimitAndStopDrain(root)) result = 2;
    else if (!TestRuntimeReconfiguration(root)) result = 3;
    else if (!TestJsonEscapingAndOwner(root)) result = 4;
    else if (!TestCorrelationSchemaAndDetails(root)) result = 5;
    else if (!TestGeneratedSessionSurvivesRestart(root)) result = 6;
    else if (!TestInvalidUtf8IsReplaced(root)) result = 7;
    else if (!TestBoundedDropCounter(root)) result = 8;
    else if (!TestOpenErrorIsQueryable(root)) result = 9;
    else if (!TestIdleWaitAndPromptWake(root)) result = 10;
    else if (!TestMultipleRotationAndRetention(root)) result = 11;
    else if (!TestProjectedSizeTriggersRotation(root)) result = 12;
    else if (!TestZeroArchiveRetentionTruncatesCurrent(root)) result = 13;
    else if (!TestAppendAndStartupRotationAcrossStarts(root)) result = 14;
    else if (!TestSingleOversizedRecordStaysIntact(root)) result = 15;
    else if (!TestStartupCleansReducedRetention(root)) result = 16;
    else if (!TestArchiveRetentionClamp(root)) result = 17;
    else if (!TestLockedArchiveDefersRotation(root)) result = 18;
    else if (!TestRotationErrorIsQueryable(root)) result = 19;

    std::filesystem::remove_all(root, ignored);
    return result;
}
