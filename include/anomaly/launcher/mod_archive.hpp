#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace anomaly::launcher {

enum class ModArchiveError { None, Unsupported, Corrupt, Encrypted, UnsafePath, LimitExceeded, Io, Cancelled };

// Owns an isolated extraction directory, removed on failure or when the caller has finished copying.
// Neither format detection nor extraction depends on the downloaded file's extension.
struct ExtractedModArchive final {
    ModArchiveError error{ModArchiveError::None};
    std::string detail;
    std::filesystem::path directory;

    ExtractedModArchive() = default;
    ~ExtractedModArchive();
    ExtractedModArchive(ExtractedModArchive&& other) noexcept;
    ExtractedModArchive(const ExtractedModArchive&) = delete;
    ExtractedModArchive& operator=(const ExtractedModArchive&) = delete;

    [[nodiscard]] bool Ok() const noexcept { return error == ModArchiveError::None; }
};

// Reads ZIP, 7z, RAR and RAR5 into regular files and directories only.
[[nodiscard]] ExtractedModArchive ExtractModArchive(const std::filesystem::path& archive,
    const std::atomic<bool>* cancelled = nullptr);

}  // namespace anomaly::launcher
