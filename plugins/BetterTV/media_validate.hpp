#pragma once

#include <cstdint>
#include <string>

namespace better_tv_video {

struct MediaInfo final {
    std::int64_t duration_ticks{};
    std::int64_t duration_ms{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t channels{};
    std::uint32_t sample_rate{};
};

enum class MediaValidationError {
    None,
    Size,
    OpenFailed,
    Container,
    VideoCodec,
    AudioCodec,
    Channels,
    Resolution,
    Duration
};

// Validates a downloaded muxed MP4 with Windows Media Foundation: H.264 video
// at most 720p, AAC audio with one or two channels, a positive duration matching
// the playurl duration, and a size inside the configured limits.
MediaValidationError ValidateMediaFile(const std::wstring& path,
                                       std::uint64_t expected_bytes,
                                       std::int64_t expected_duration_ms,
                                       std::uint64_t max_artifact_bytes,
                                       MediaInfo* info, std::string* detail);

}  // namespace better_tv_video
