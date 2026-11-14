// Media Foundation validation for a downloaded muxed MP4.
//
// The prepare pipeline trusts neither the container nor the codecs: the file is
// opened through the Media Foundation source reader and its native stream types
// are checked against the only playback profile the plugin supports (H.264 at
// most 720p plus AAC mono or stereo, duration matching the playurl document).
// Every diagnostic is English log data.

#include "media_validate.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <utility>

namespace better_tv_video {
namespace {
using Microsoft::WRL::ComPtr;

constexpr UINT32 kMaxWidth = 1280;
constexpr UINT32 kMaxHeight = 720;
// Five percent of the playurl duration, but never less than three seconds.
constexpr std::int64_t kMinDurationToleranceMs = 3000;
constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kFirstAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

// Initializes COM (MTA) and the Media Foundation platform for one validation.
class Foundation final {
public:
    Foundation() {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(apartment)) {
            apartment_initialized_ = true;
            if (SUCCEEDED(MFStartup(MF_VERSION))) started_ = true;
        }
    }
    Foundation(const Foundation&) = delete;
    Foundation& operator=(const Foundation&) = delete;
    ~Foundation() {
        if (started_) MFShutdown();
        if (apartment_initialized_) CoUninitialize();
    }

    bool started() const { return started_; }

private:
    bool apartment_initialized_{};
    bool started_{};
};

std::string HexStatus(const HRESULT status) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    const auto value = static_cast<std::uint32_t>(status);
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> shift) & 0xF]);
    }
    return text;
}

MediaValidationError Failure(const MediaValidationError error, std::string message,
                             std::string* detail) {
    if (detail != nullptr) *detail = std::move(message);
    return error;
}

std::string Dimensions(const UINT32 width, const UINT32 height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

}  // namespace

MediaValidationError ValidateMediaFile(const std::wstring& path,
                                       std::uint64_t expected_bytes,
                                       std::int64_t expected_duration_ms,
                                       std::uint64_t max_artifact_bytes,
                                       MediaInfo* info, std::string* detail) {
    if (info != nullptr) *info = MediaInfo{};
    if (detail != nullptr) detail->clear();

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return Failure(MediaValidationError::OpenFailed,
                       "media file could not be opened (win32 error " +
                           std::to_string(GetLastError()) + ")",
                       detail);
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return Failure(MediaValidationError::OpenFailed, "media path is a directory", detail);
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
                               attributes.nFileSizeLow;
    if (size == 0) {
        return Failure(MediaValidationError::Size, "media file is empty", detail);
    }
    if (size > max_artifact_bytes) {
        return Failure(MediaValidationError::Size,
                       "media file is " + std::to_string(size) + " bytes above the " +
                           std::to_string(max_artifact_bytes) + "-byte artifact limit",
                       detail);
    }
    if (expected_bytes != 0 && size != expected_bytes) {
        return Failure(MediaValidationError::Size,
                       "media file is " + std::to_string(size) + " bytes but " +
                           std::to_string(expected_bytes) + " were expected",
                       detail);
    }

    Foundation foundation;
    if (!foundation.started()) {
        return Failure(MediaValidationError::OpenFailed,
                       "Media Foundation could not be initialized", detail);
    }
    ComPtr<IMFSourceReader> reader;
    HRESULT status = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(status)) {
        return Failure(MediaValidationError::Container,
                       "media container could not be opened (HRESULT " + HexStatus(status) + ")",
                       detail);
    }

    ComPtr<IMFMediaType> video;
    status = reader->GetNativeMediaType(kFirstVideoStream, 0, &video);
    if (status == MF_E_INVALIDSTREAMNUMBER) {
        return Failure(MediaValidationError::VideoCodec, "media has no video stream", detail);
    }
    if (FAILED(status)) {
        return Failure(MediaValidationError::Container,
                       "video stream could not be inspected (HRESULT " + HexStatus(status) + ")",
                       detail);
    }
    GUID video_subtype{};
    if (FAILED(video->GetGUID(MF_MT_SUBTYPE, &video_subtype)) ||
        video_subtype != MFVideoFormat_H264) {
        return Failure(MediaValidationError::VideoCodec, "video stream is not H.264", detail);
    }
    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(video.Get(), MF_MT_FRAME_SIZE, &width, &height)) || width == 0 ||
        height == 0) {
        return Failure(MediaValidationError::Resolution, "video resolution is missing", detail);
    }
    if (width > kMaxWidth || height > kMaxHeight) {
        return Failure(MediaValidationError::Resolution,
                       "video resolution " + Dimensions(width, height) + " exceeds 1280x720",
                       detail);
    }

    ComPtr<IMFMediaType> audio;
    status = reader->GetNativeMediaType(kFirstAudioStream, 0, &audio);
    if (status == MF_E_INVALIDSTREAMNUMBER) {
        return Failure(MediaValidationError::AudioCodec, "media has no audio stream", detail);
    }
    if (FAILED(status)) {
        return Failure(MediaValidationError::Container,
                       "audio stream could not be inspected (HRESULT " + HexStatus(status) + ")",
                       detail);
    }
    GUID audio_subtype{};
    if (FAILED(audio->GetGUID(MF_MT_SUBTYPE, &audio_subtype)) ||
        audio_subtype != MFAudioFormat_AAC) {
        return Failure(MediaValidationError::AudioCodec, "audio stream is not AAC", detail);
    }
    UINT32 channels = 0;
    if (FAILED(audio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) || channels < 1 ||
        channels > 2) {
        return Failure(MediaValidationError::Channels,
                       "audio stream has " + std::to_string(channels) +
                           " channels; only mono or stereo is supported",
                       detail);
    }
    UINT32 sample_rate = 0;
    if (FAILED(audio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate)) ||
        sample_rate == 0) {
        return Failure(MediaValidationError::AudioCodec, "audio sample rate is missing", detail);
    }

    PROPVARIANT duration;
    PropVariantInit(&duration);
    status = reader->GetPresentationAttribute(kMediaSource, MF_PD_DURATION, &duration);
    std::int64_t ticks = 0;
    if (SUCCEEDED(status)) {
        if (duration.vt == VT_UI8) {
            ticks = static_cast<std::int64_t>(duration.uhVal.QuadPart);
        } else if (duration.vt == VT_I8) {
            ticks = duration.hVal.QuadPart;
        }
    }
    PropVariantClear(&duration);
    if (ticks <= 0) {
        return Failure(MediaValidationError::Duration, "media duration is missing or zero",
                       detail);
    }
    const std::int64_t duration_ms = ticks / kTicksPerMillisecond;
    const std::int64_t expected = expected_duration_ms > 0 ? expected_duration_ms : 0;
    const std::int64_t tolerance =
        expected / 20 > kMinDurationToleranceMs ? expected / 20 : kMinDurationToleranceMs;
    if (duration_ms > expected + tolerance || expected > duration_ms + tolerance) {
        return Failure(MediaValidationError::Duration,
                       "media duration is " + std::to_string(duration_ms) +
                           " ms but the playurl document declared " +
                           std::to_string(expected_duration_ms) + " ms",
                       detail);
    }

    MediaInfo parsed;
    parsed.duration_ticks = ticks;
    parsed.duration_ms = duration_ms;
    parsed.width = width;
    parsed.height = height;
    parsed.channels = channels;
    parsed.sample_rate = sample_rate;
    if (info != nullptr) *info = parsed;
    return MediaValidationError::None;
}

}  // namespace better_tv_video
