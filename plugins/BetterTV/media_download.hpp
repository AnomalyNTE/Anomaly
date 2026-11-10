#pragma once

#include "http_client.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace better_tv_video {

enum class DownloadError {
    None,
    InvalidUrl,
    Network,
    HttpStatus,
    LengthMismatch,
    Oversize,
    Cancelled,
    Io
};

struct DownloadProgress final {
    std::uint64_t received{};
    std::uint64_t total{};
};

struct DownloadResult final {
    DownloadError error{DownloadError::None};
    // English diagnostic for the log only.
    std::string detail;
    std::uint64_t bytes{};
};

// Streams an HTTPS media URL into `destination` (a temporary path). Enforces
// the expected API size and the single-artifact limit, reports received bytes,
// and returns Cancelled without leaving the partial file behind.
DownloadResult DownloadMedia(std::string_view url, const std::wstring& destination,
                             std::uint64_t expected_size,
                             std::uint64_t max_artifact_bytes,
                             const std::function<bool()>& cancelled,
                             const std::function<void(const DownloadProgress&)>& progress,
                             const HttpMediaStreamFactory& factory);

}  // namespace better_tv_video
