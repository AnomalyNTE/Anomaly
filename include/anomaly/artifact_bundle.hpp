#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace anomaly {

inline constexpr std::uint32_t kMaximumArtifactBundleEntries = 8192;
inline constexpr std::uint64_t kMaximumArtifactBundleBytes = 1024ULL * 1024ULL * 1024ULL;

enum class ArtifactBundleError : std::uint8_t {
    None,
    SourceUnavailable,
    InvalidFormat,
    InvalidPath,
    ReparsePoint,
    LimitExceeded,
    HashMismatch,
    DuplicateEntry,
    IoFailure,
};

struct ArtifactBundleResult {
    ArtifactBundleError error{ArtifactBundleError::None};
    std::string message;
    std::uint32_t entries{};
    std::uint64_t bytes{};
    [[nodiscard]] bool Ok() const noexcept { return error == ArtifactBundleError::None; }
};

[[nodiscard]] ArtifactBundleResult CreateArtifactBundle(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_file);
[[nodiscard]] ArtifactBundleResult ExtractArtifactBundle(
    const std::filesystem::path& bundle_file,
    const std::filesystem::path& empty_destination_directory);

}  // namespace anomaly
