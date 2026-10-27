#include "anomaly/log_retention.hpp"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

std::uintmax_t SaturatingAdd(std::uintmax_t left, std::uintmax_t right) noexcept {
    const auto maximum = (std::numeric_limits<std::uintmax_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

// Another process can still hold a log open: the game hook, an archive copy, or
// the platform host writing its own line. Retention is best effort, so such a
// file is skipped and retried on the next pass instead of being reported as a
// failure the Runtime would surface as a degraded logger.
bool IsBusyError(const std::error_code& error) noexcept {
    if (error.category() != std::system_category()) return false;
    const int code = error.value();
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
        code == ERROR_USER_MAPPED_FILE;
}

bool IsDigits(std::wstring_view text) noexcept {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](wchar_t character) {
        return character >= L'0' && character <= L'9';
    });
}

// A rotation suffix belongs to the log file it extends, so "anomaly-runtime.jsonl.2"
// is retained with the same rules as "anomaly-runtime.jsonl".
bool IsLogFileName(const std::wstring& name) noexcept {
    std::wstring_view base{name};
    const std::size_t dot = base.rfind(L'.');
    if (dot != std::wstring_view::npos && IsDigits(base.substr(dot + 1))) {
        base = base.substr(0, dot);
    }
    return base.ends_with(L".log") || base.ends_with(L".jsonl");
}

struct LogFile {
    std::filesystem::path path;
    std::uintmax_t bytes{};
    std::filesystem::file_time_type written{};
};

struct Pass {
    std::size_t removed_files{};
    std::uintmax_t removed_bytes{};
    std::error_code first_error;

    bool Remove(const LogFile& file) noexcept {
        std::error_code error;
        const bool removed = std::filesystem::remove(file.path, error);
        if (error) {
            if (!IsBusyError(error) && !first_error) first_error = error;
            return false;
        }
        if (!removed) return false;
        ++removed_files;
        removed_bytes = SaturatingAdd(removed_bytes, file.bytes);
        return true;
    }
};

}  // namespace

LogRetentionResult PruneLogFiles(
    const std::filesystem::path& directory,
    const LogRetentionPolicy& policy) noexcept {
    LogRetentionResult result;
    const bool age_rule = policy.max_age > std::chrono::hours::zero();
    const bool size_rule = policy.max_total_bytes != 0;
    if (!age_rule && !size_rule) return result;

    try {
        std::error_code error;
        std::filesystem::directory_iterator iterator(directory, error);
        if (error) {
            result.error = error;
            return result;
        }

        const auto active = policy.active_path.lexically_normal();
        const auto oldest_allowed =
            std::filesystem::file_time_type::clock::now() - policy.max_age;

        Pass pass;
        std::vector<LogFile> survivors;
        const std::filesystem::directory_iterator end;
        while (iterator != end) {
            const std::filesystem::directory_entry entry = *iterator;
            iterator.increment(error);
            if (error) {
                // The listed entries are still worth pruning; the rest arrives on
                // the next pass.
                result.error = error;
                break;
            }

            std::error_code entry_error;
            if (!entry.is_regular_file(entry_error) || entry_error) continue;
            const std::filesystem::path path = entry.path();
            if (!IsLogFileName(path.filename().native())) continue;

            LogFile file{path, 0, {}};
            file.bytes = std::filesystem::file_size(path, entry_error);
            if (entry_error) continue;
            file.written = std::filesystem::last_write_time(path, entry_error);
            if (entry_error) continue;

            if (!active.empty() && path.lexically_normal() == active) {
                result.retained_bytes = SaturatingAdd(result.retained_bytes, file.bytes);
                continue;
            }
            if (age_rule && file.written < oldest_allowed) {
                pass.Remove(file);
                continue;
            }
            result.retained_bytes = SaturatingAdd(result.retained_bytes, file.bytes);
            survivors.push_back(std::move(file));
        }

        if (size_rule && result.retained_bytes > policy.max_total_bytes) {
            std::sort(
                survivors.begin(), survivors.end(),
                [](const LogFile& left, const LogFile& right) {
                    return left.written < right.written;
                });
            for (const auto& file : survivors) {
                if (result.retained_bytes <= policy.max_total_bytes) break;
                if (!pass.Remove(file)) continue;
                result.retained_bytes -= (std::min)(result.retained_bytes, file.bytes);
            }
        }

        result.removed_files = pass.removed_files;
        result.removed_bytes = pass.removed_bytes;
        if (!result.error) result.error = pass.first_error;
        return result;
    } catch (...) {
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

}  // namespace anomaly
