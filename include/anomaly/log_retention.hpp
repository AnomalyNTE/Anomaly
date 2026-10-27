#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace anomaly {

// Deletion policy for log files left behind by earlier sessions. Size-based
// rotation already bounds the file the Runtime is still writing, but nothing
// bounded the files that piled up next to it, so a player's log directory only
// ever grew.
struct LogRetentionPolicy {
    // Zero disables the age rule.
    std::chrono::hours max_age{};
    // Zero disables the total size rule.
    std::uintmax_t max_total_bytes{};
    // Never removed. Callers sweeping the directory holding their own open file
    // set this to that file; an empty value protects nothing. The path has to be
    // spelled the way the directory is enumerated (the same directory joined
    // with the file name), because it is compared after lexical normalization.
    std::filesystem::path active_path;
};

struct LogRetentionResult {
    std::size_t removed_files{};
    std::uintmax_t removed_bytes{};
    // Bytes of the log files that survived, including the protected active file.
    std::uintmax_t retained_bytes{};
    // First deletion that failed. A log file another process holds open is left
    // alone and retried on the next pass, so this is diagnostic, not fatal.
    std::error_code error;
};

// Deletes log files directly under `directory` until both rules hold: files
// whose last write is older than policy.max_age go first, then the oldest
// survivors are removed until the remaining total fits policy.max_total_bytes.
// Only "*.log" and "*.jsonl" files count, including numeric rotation suffixes
// such as "anomaly-runtime.jsonl.2"; every other file in the directory is left
// untouched. Directories are not removed and subdirectories are not descended.
[[nodiscard]] LogRetentionResult PruneLogFiles(
    const std::filesystem::path& directory,
    const LogRetentionPolicy& policy) noexcept;

}  // namespace anomaly
