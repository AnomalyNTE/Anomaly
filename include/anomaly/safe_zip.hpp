#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace anomaly {

inline constexpr std::uint32_t kMaximumZipEntries = 8192;
inline constexpr std::uint64_t kMaximumZipArchiveBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumZipBytes = 512ULL * 1024ULL * 1024ULL;

enum class SafeZipError : std::uint8_t {
    None,
    SourceUnavailable,  // archive missing or unreadable
    InvalidArchive,     // not a valid zip / corrupt entry
    UnsafePath,         // traversal, absolute, or drive-qualified entry name
    LimitExceeded,      // too many entries or too many uncompressed bytes
    IoFailure,          // could not create a directory or write a file
};

struct SafeZipResult {
    SafeZipError error{SafeZipError::None};
    std::string message;
    std::uint32_t entries{};
    std::uint64_t bytes{};

    [[nodiscard]] bool Ok() const noexcept { return error == SafeZipError::None; }
};

// Extracts a standard zip (stored or DEFLATE) into destination_directory,
// creating it if needed. Every entry name is confined under the destination:
// absolute paths, drive-qualified names, and any component that would escape
// the root ("..") are rejected, aborting the whole extraction. Entry count and
// total uncompressed size are capped. Reading and writing go through
// std::filesystem/std::fstream so Unicode destination paths are handled on
// Windows regardless of the archive library's own file IO.
[[nodiscard]] SafeZipResult ExtractZip(
    const std::filesystem::path& zip_file,
    const std::filesystem::path& destination_directory);

}  // namespace anomaly
