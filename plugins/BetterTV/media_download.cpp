// Streaming media download for the television source.
//
// The URL comes from the bilibili playurl API, but the downloader still re-checks
// the transport and the destination before writing anything. Bytes are streamed
// into the caller-supplied temporary path in fixed-size chunks, so the declared
// API size, the artifact limit and cancellation are enforced while data flows.
// A cancelled or failed transfer removes the partial file.

#include "media_download.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace better_tv_video {
namespace {

constexpr std::size_t kChunkBytes = 64 * 1024;

// Owns the temporary destination file and removes anything left behind unless
// the download completes.
class PartialFile final {
public:
    PartialFile() = default;
    PartialFile(const PartialFile&) = delete;
    PartialFile& operator=(const PartialFile&) = delete;
    ~PartialFile() { Close(); }

    bool Create(const std::wstring& path) {
        path_ = path;
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool Write(const std::uint8_t* data, const std::size_t size) {
        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(size);
        return WriteFile(handle_, data, bytes, &written, nullptr) && written == bytes;
    }

    bool Flush() {
        return FlushFileBuffers(handle_) != FALSE;
    }

    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    void Discard() {
        Close();
        if (!path_.empty()) DeleteFileW(path_.c_str());
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::wstring path_;
};

DownloadResult Failure(const DownloadError error, std::string detail) {
    DownloadResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

std::string WithTransportDetail(const std::string_view prefix, const std::string& detail) {
    if (detail.empty()) return std::string(prefix);
    std::string text(prefix);
    text += ": ";
    text += detail;
    return text;
}

}  // namespace

DownloadResult DownloadMedia(std::string_view url, const std::wstring& destination,
                             std::uint64_t expected_size,
                             std::uint64_t max_artifact_bytes,
                             const std::function<bool()>& cancelled,
                             const std::function<void(const DownloadProgress&)>& progress,
                             const HttpMediaStreamFactory& factory) {
    std::string safety_detail;
    if (!IsSafeMediaUrl(url, &safety_detail)) {
        std::string detail = "media URL rejected";
        if (!safety_detail.empty()) {
            detail += ": ";
            detail += safety_detail;
        }
        return Failure(DownloadError::InvalidUrl, std::move(detail));
    }
    // Reject the declared size before opening a connection; the API size is
    // authoritative for this artifact even when the server omits a length.
    if (expected_size > max_artifact_bytes) {
        return Failure(DownloadError::Oversize,
                       "declared media size " + std::to_string(expected_size) +
                           " bytes exceeds the " + std::to_string(max_artifact_bytes) +
                           "-byte artifact limit");
    }
    if (!factory) return Failure(DownloadError::Network, "no media transport is configured");
    std::unique_ptr<HttpMediaStream> stream = factory();
    if (!stream) return Failure(DownloadError::Network, "the media transport could not be created");
    if (cancelled && cancelled()) return Failure(DownloadError::Cancelled, "media download cancelled");

    std::uint64_t reported_total = 0;
    std::string transport_detail;
    if (!stream->Open(url, &reported_total, &transport_detail)) {
        return Failure(DownloadError::Network,
                       WithTransportDetail("media request failed", transport_detail));
    }
    if (reported_total > max_artifact_bytes) {
        stream->Close();
        return Failure(DownloadError::Oversize,
                       "server reported a " + std::to_string(reported_total) +
                           "-byte media file above the " + std::to_string(max_artifact_bytes) +
                           "-byte artifact limit");
    }
    if (reported_total != 0 && reported_total != expected_size) {
        stream->Close();
        return Failure(DownloadError::LengthMismatch,
                       "server reported " + std::to_string(reported_total) +
                           " bytes but the API declared " + std::to_string(expected_size));
    }

    PartialFile file;
    if (!file.Create(destination)) {
        stream->Close();
        return Failure(DownloadError::Io,
                       "could not create the temporary media file (win32 error " +
                           std::to_string(GetLastError()) + ")");
    }
    const std::uint64_t progress_total = reported_total != 0 ? reported_total : expected_size;
    std::array<std::uint8_t, kChunkBytes> buffer{};
    std::uint64_t received = 0;
    DownloadResult result;
    for (;;) {
        if (cancelled && cancelled()) {
            result = Failure(DownloadError::Cancelled, "media download cancelled");
            break;
        }
        std::string read_detail;
        const int read = stream->Read(buffer, &read_detail);
        if (read < 0) {
            result = Failure(DownloadError::Network,
                             WithTransportDetail("media stream read failed", read_detail));
            break;
        }
        if (read == 0) break;
        const auto count = static_cast<std::size_t>(read);
        received += static_cast<std::uint64_t>(count);
        if (received > max_artifact_bytes) {
            result = Failure(DownloadError::Oversize,
                             "media stream exceeded the " +
                                 std::to_string(max_artifact_bytes) + "-byte artifact limit");
            break;
        }
        if (!file.Write(buffer.data(), count)) {
            result = Failure(DownloadError::Io,
                             "could not write the temporary media file (win32 error " +
                                 std::to_string(GetLastError()) + ")");
            break;
        }
        if (progress) progress(DownloadProgress{received, progress_total});
    }
    stream->Close();

    if (result.error == DownloadError::None) {
        if (received != expected_size) {
            result = Failure(DownloadError::LengthMismatch,
                             "downloaded " + std::to_string(received) +
                                 " bytes but the API declared " + std::to_string(expected_size));
        } else if (!file.Flush()) {
            result = Failure(DownloadError::Io,
                             "could not flush the temporary media file (win32 error " +
                                 std::to_string(GetLastError()) + ")");
        }
    }
    if (result.error != DownloadError::None) {
        file.Discard();
        return result;
    }
    file.Close();
    if (progress) progress(DownloadProgress{received, progress_total});
    DownloadResult success;
    success.bytes = received;
    return success;
}

}  // namespace better_tv_video
