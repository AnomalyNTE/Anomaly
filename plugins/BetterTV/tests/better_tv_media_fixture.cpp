// Fixture for the WinHTTP media download and the Media Foundation validation.
//
// The download side runs against a scripted in-memory HttpMediaStream, so the
// whole transfer path (chunking, size limits, cancellation, cleanup) is
// exercised offline. The validation side runs Media Foundation against the
// fixed MP4 files under plugins/BetterTV/tests/data. Assertions only use the public
// results and independently computed expectations.

#include "plugins/BetterTV/media_download.hpp"
#include "plugins/BetterTV/media_validate.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using better_tv_video::DownloadError;
using better_tv_video::DownloadProgress;
using better_tv_video::DownloadResult;
using better_tv_video::HttpMediaStream;
using better_tv_video::HttpMediaStreamFactory;
using better_tv_video::MediaInfo;
using better_tv_video::MediaValidationError;
using better_tv_video::ValidateMediaFile;

constexpr std::string_view kSafeUrl = "https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/media.mp4";
constexpr std::uint64_t kMegabyte = 1024 * 1024;

int g_checks = 0;
int g_failures = 0;

void Fail(const std::string& message) {
    ++g_failures;
    std::cout << "FAIL: " << message << '\n';
}

void Expect(const bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) Fail(message);
}

void ExpectNumber(const std::uint64_t actual, const std::uint64_t expected,
                  const std::string& label) {
    ++g_checks;
    if (actual != expected) {
        Fail(label + ": expected " + std::to_string(expected) + ", got " +
             std::to_string(actual));
    }
}

const char* DownloadErrorName(const DownloadError error) {
    switch (error) {
        case DownloadError::None: return "None";
        case DownloadError::InvalidUrl: return "InvalidUrl";
        case DownloadError::Network: return "Network";
        case DownloadError::HttpStatus: return "HttpStatus";
        case DownloadError::LengthMismatch: return "LengthMismatch";
        case DownloadError::Oversize: return "Oversize";
        case DownloadError::Cancelled: return "Cancelled";
        case DownloadError::Io: return "Io";
    }
    return "Unknown";
}

const char* ValidationErrorName(const MediaValidationError error) {
    switch (error) {
        case MediaValidationError::None: return "None";
        case MediaValidationError::Size: return "Size";
        case MediaValidationError::OpenFailed: return "OpenFailed";
        case MediaValidationError::Container: return "Container";
        case MediaValidationError::VideoCodec: return "VideoCodec";
        case MediaValidationError::AudioCodec: return "AudioCodec";
        case MediaValidationError::Channels: return "Channels";
        case MediaValidationError::Resolution: return "Resolution";
        case MediaValidationError::Duration: return "Duration";
    }
    return "Unknown";
}

void ExpectDownload(const DownloadResult& result, const DownloadError expected,
                    const std::string& label) {
    ++g_checks;
    if (result.error != expected) {
        Fail(label + ": expected " + DownloadErrorName(expected) + ", got " +
             DownloadErrorName(result.error) + " (" + result.detail + ")");
    }
}

void ExpectValidation(const MediaValidationError actual, const MediaValidationError expected,
                      const std::string& label) {
    ++g_checks;
    if (actual != expected) {
        Fail(label + ": expected " + ValidationErrorName(expected) + ", got " +
             ValidationErrorName(actual));
    }
}

std::uint8_t MediaByte(const std::uint64_t offset) {
    return static_cast<std::uint8_t>((offset * 131u + 17u) & 0xFFu);
}

// Observations the test keeps after DownloadMedia has destroyed its transport.
struct StreamStats final {
    int factory_calls{};
    int open_calls{};
    int reads{};
    int close_calls{};
    int destructions{};
    std::uint64_t served{};
    std::string opened_url;
};

// Scripted transport. Every Read returns at most `chunk_bytes` from `data_bytes`
// patterned bytes, optionally with a delay so cancellation is observable.
class FakeMediaStream final : public HttpMediaStream {
public:
    struct Options final {
        std::uint64_t total_bytes{};
        std::uint64_t data_bytes{};
        std::size_t chunk_bytes{64 * 1024};
        DWORD delay_ms{};
        bool fail_open{};
        std::uint64_t fail_read_at{std::numeric_limits<std::uint64_t>::max()};
    };

    FakeMediaStream(const Options& options, StreamStats* stats)
        : options_(options), stats_(stats) {}
    ~FakeMediaStream() override { ++stats_->destructions; }

    bool Open(std::string_view url, std::uint64_t* total_bytes, std::string* detail) override {
        ++stats_->open_calls;
        stats_->opened_url.assign(url);
        if (options_.fail_open) {
            if (detail != nullptr) *detail = "scripted open failure";
            return false;
        }
        if (total_bytes != nullptr) *total_bytes = options_.total_bytes;
        return true;
    }

    int Read(std::span<std::uint8_t> buffer, std::string* detail) override {
        ++stats_->reads;
        if (options_.delay_ms != 0) Sleep(options_.delay_ms);
        if (stats_->served >= options_.data_bytes) return 0;
        if (stats_->served >= options_.fail_read_at) {
            if (detail != nullptr) *detail = "scripted read failure";
            return -1;
        }
        const std::uint64_t remaining =
            std::min(options_.data_bytes - stats_->served, options_.fail_read_at - stats_->served);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            {remaining, options_.chunk_bytes, buffer.size()}));
        for (std::size_t index = 0; index < count; ++index) {
            buffer[index] = MediaByte(stats_->served + index);
        }
        stats_->served += count;
        return static_cast<int>(count);
    }

    void Close() override { ++stats_->close_calls; }

private:
    Options options_;
    StreamStats* stats_;
};

HttpMediaStreamFactory FactoryFor(StreamStats* stats, const FakeMediaStream::Options& options) {
    return [stats, options] {
        ++stats->factory_calls;
        return std::unique_ptr<HttpMediaStream>(std::make_unique<FakeMediaStream>(options, stats));
    };
}

bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

std::uint64_t FileSize(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::uint64_t>(size);
}

bool FileMatchesMediaPattern(const std::filesystem::path& path, const std::uint64_t expected) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::array<char, 8192> buffer{};
    std::uint64_t offset = 0;
    while (offset < expected) {
        const auto wanted = static_cast<std::streamsize>(
            std::min<std::uint64_t>(buffer.size(), expected - offset));
        stream.read(buffer.data(), wanted);
        if (stream.gcount() != wanted) return false;
        for (std::streamsize index = 0; index < wanted; ++index) {
            if (static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]) !=
                MediaByte(offset + static_cast<std::uint64_t>(index))) {
                return false;
            }
        }
        offset += static_cast<std::uint64_t>(wanted);
    }
    char extra = 0;
    stream.read(&extra, 1);
    return stream.gcount() == 0;
}

std::filesystem::path CreateTempDirectory() {
    std::wstring buffer(MAX_PATH + 1, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    std::filesystem::path root(buffer);
    root /= L"anomaly-nte-tv-media-" + std::to_wstring(GetCurrentProcessId());
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error) return {};
    return root;
}

std::wstring Widen(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), size);
    while (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return wide;
}

void WriteBytes(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void WriteFirstBytes(const std::filesystem::path& source, const std::filesystem::path& destination,
                     const std::size_t count) {
    std::ifstream input(source, std::ios::binary);
    std::vector<char> bytes(count);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto read = static_cast<std::size_t>(input.gcount());
    WriteBytes(destination, std::string_view(bytes.data(), read));
}

// ---------------------------------------------------------------------------
// Download tests
// ---------------------------------------------------------------------------

void TestDownloadRejectsUnsafeUrl(const std::filesystem::path& directory) {
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        "http://upos-sz-mirrorcos.bilivideo.com/upgcxcode/media.mp4",
        directory / L"unsafe.mp4", 1024, kMegabyte, {}, {}, FactoryFor(&stats, {}));
    ExpectDownload(result, DownloadError::InvalidUrl, "plain HTTP media URL");
    Expect(!result.detail.empty(), "plain HTTP media URL: detail is empty");
    Expect(stats.factory_calls == 0, "plain HTTP media URL: transport was created");
    Expect(!Exists(directory / L"unsafe.mp4"), "plain HTTP media URL: destination exists");
}

void TestDownloadRejectsDeclaredOversize(const std::filesystem::path& directory) {
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"declared-oversize.mp4", kMegabyte, kMegabyte / 2, {}, {},
        FactoryFor(&stats, {}));
    ExpectDownload(result, DownloadError::Oversize, "declared oversize");
    Expect(stats.factory_calls == 0, "declared oversize: transport was created");
}

void TestDownloadNormal(const std::filesystem::path& directory) {
    const std::uint64_t expected = 300000;
    FakeMediaStream::Options options;
    options.total_bytes = expected;
    options.data_bytes = expected;
    StreamStats stats;
    std::vector<DownloadProgress> progress;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"normal.mp4", expected, kMegabyte, {},
        [&progress](const DownloadProgress& value) { progress.push_back(value); },
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::None, "normal download");
    ExpectNumber(result.bytes, expected, "normal download: returned bytes");
    ExpectNumber(stats.served, expected, "normal download: stream bytes");
    ExpectNumber(FileSize(directory / L"normal.mp4"), expected, "normal download: file size");
    Expect(FileMatchesMediaPattern(directory / L"normal.mp4", expected),
           "normal download: file content does not match the stream");
    Expect(progress.size() >= 2, "normal download: progress was not reported per chunk");
    if (!progress.empty()) {
        ExpectNumber(progress.back().received, expected, "normal download: final progress bytes");
        ExpectNumber(progress.back().total, expected, "normal download: final progress total");
    }
    bool monotonic = true;
    for (std::size_t index = 1; index < progress.size(); ++index) {
        if (progress[index].received < progress[index - 1].received) monotonic = false;
    }
    Expect(monotonic, "normal download: progress went backwards");
}

void TestDownloadWithoutContentLength(const std::filesystem::path& directory) {
    const std::uint64_t expected = 12345;
    FakeMediaStream::Options options;
    options.total_bytes = 0;
    options.data_bytes = expected;
    StreamStats stats;
    DownloadProgress last;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"no-length.mp4", expected, kMegabyte, {},
        [&last](const DownloadProgress& value) { last = value; }, FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::None, "download without HTTP length");
    ExpectNumber(FileSize(directory / L"no-length.mp4"), expected,
                 "download without HTTP length: file size");
    ExpectNumber(last.received, expected, "download without HTTP length: progress bytes");
    ExpectNumber(last.total, expected, "download without HTTP length: progress total");
}

void TestDownloadSlowChunks(const std::filesystem::path& directory) {
    const std::uint64_t expected = 32 * 1024;
    FakeMediaStream::Options options;
    options.total_bytes = expected;
    options.data_bytes = expected;
    options.chunk_bytes = 1024;
    options.delay_ms = 1;
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"slow.mp4", expected, kMegabyte, {}, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::None, "slow chunked download");
    ExpectNumber(FileSize(directory / L"slow.mp4"), expected, "slow chunked download: file size");
    ExpectNumber(stats.served, expected, "slow chunked download: stream bytes");
    Expect(stats.reads >= 32, "slow chunked download: chunk size was not honoured");
}

void TestDownloadShortBody(const std::filesystem::path& directory) {
    FakeMediaStream::Options options;
    options.total_bytes = 0;
    options.data_bytes = 900;
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"short.mp4", 1000, kMegabyte, {}, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::LengthMismatch, "short body");
    Expect(!result.detail.empty(), "short body: detail is empty");
    Expect(!Exists(directory / L"short.mp4"), "short body: partial file was left behind");
}

void TestDownloadReportedLengthMismatch(const std::filesystem::path& directory) {
    FakeMediaStream::Options options;
    options.total_bytes = 1001;
    options.data_bytes = 1001;
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"reported-mismatch.mp4", 1000, kMegabyte, {}, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::LengthMismatch, "reported length mismatch");
    Expect(stats.reads == 0, "reported length mismatch: transfer did not fail fast");
    Expect(!Exists(directory / L"reported-mismatch.mp4"),
           "reported length mismatch: partial file was left behind");
}

void TestDownloadOversizeStream(const std::filesystem::path& directory) {
    const std::uint64_t limit = 128 * 1024;
    FakeMediaStream::Options options;
    options.total_bytes = 0;
    options.data_bytes = 512 * 1024;
    options.chunk_bytes = 16 * 1024;
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"oversize.mp4", 64 * 1024, limit, {}, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::Oversize, "oversize stream");
    Expect(stats.served <= limit + options.chunk_bytes,
           "oversize stream: the whole body was transferred before aborting");
    Expect(!Exists(directory / L"oversize.mp4"), "oversize stream: partial file was left behind");
}

void TestDownloadCancelled(const std::filesystem::path& directory) {
    const std::uint64_t expected = kMegabyte;
    FakeMediaStream::Options options;
    options.total_bytes = expected;
    options.data_bytes = expected;
    options.chunk_bytes = 8 * 1024;
    options.delay_ms = 1;
    StreamStats stats;
    const auto cancel = [&stats] { return stats.reads >= 3; };
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"cancelled.mp4", expected, 2 * kMegabyte, cancel, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::Cancelled, "cancelled download");
    Expect(!result.detail.empty(), "cancelled download: detail is empty");
    Expect(!Exists(directory / L"cancelled.mp4"),
           "cancelled download: partial file was left behind");
    Expect(stats.close_calls == 1, "cancelled download: stream was not closed");
    Expect(stats.served < expected, "cancelled download: transfer ran to completion");
}

void TestDownloadOpenFailure(const std::filesystem::path& directory) {
    FakeMediaStream::Options options;
    options.fail_open = true;
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"open-failed.mp4", 1024, kMegabyte, {}, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::Network, "open failure");
    Expect(stats.reads == 0, "open failure: stream was read");
    Expect(!Exists(directory / L"open-failed.mp4"), "open failure: destination exists");
}

void TestDownloadReadFailure(const std::filesystem::path& directory) {
    FakeMediaStream::Options options;
    options.total_bytes = 1000;
    options.data_bytes = 1000;
    options.chunk_bytes = 256;
    options.fail_read_at = 500;
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"read-failed.mp4", 1000, kMegabyte, {}, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::Network, "read failure");
    Expect(!Exists(directory / L"read-failed.mp4"), "read failure: partial file was left behind");
}

void TestDownloadIoFailure(const std::filesystem::path& directory) {
    FakeMediaStream::Options options;
    options.total_bytes = 1024;
    options.data_bytes = 1024;
    StreamStats stats;
    const auto result = better_tv_video::DownloadMedia(
        kSafeUrl, directory / L"missing-directory" / L"io.mp4", 1024, kMegabyte, {}, {},
        FactoryFor(&stats, options));
    ExpectDownload(result, DownloadError::Io, "unwritable destination");
    Expect(stats.open_calls == 1 && stats.reads == 0,
           "unwritable destination: stream was read before the file was created");
}

// ---------------------------------------------------------------------------
// Validation tests
// ---------------------------------------------------------------------------

struct FileFacts final {
    std::filesystem::path path;
    std::uint64_t size{};
};

FileFacts DataFile(const std::filesystem::path& directory, const wchar_t* name) {
    FileFacts facts;
    facts.path = directory / name;
    facts.size = FileSize(facts.path);
    return facts;
}

void TestValidMedia(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_h264_aac.mp4");
    Expect(file.size != 0, "valid media fixture is missing: " + file.path.string());
    MediaInfo info;
    std::string detail;
    const auto error = ValidateMediaFile(file.path.wstring(), file.size, 1000, kMegabyte, &info,
                                         &detail);
    ExpectValidation(error, MediaValidationError::None, "valid media (" + detail + ")");
    if (error != MediaValidationError::None) return;
    Expect(info.duration_ticks > 0, "valid media: duration ticks are zero");
    Expect(info.duration_ms >= 800 && info.duration_ms <= 1400,
           "valid media: unexpected duration " + std::to_string(info.duration_ms) + " ms");
    ExpectNumber(info.width, 640, "valid media: width");
    ExpectNumber(info.height, 360, "valid media: height");
    ExpectNumber(info.channels, 2, "valid media: channels");
    ExpectNumber(info.sample_rate, 48000, "valid media: sample rate");
    Expect(detail.empty(), "valid media: detail is not empty");
}

void TestValidMediaTolerance(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_h264_aac.mp4");
    const auto error = ValidateMediaFile(file.path.wstring(), file.size, 2500, kMegabyte,
                                         nullptr, nullptr);
    ExpectValidation(error, MediaValidationError::None, "duration inside tolerance");
}

void TestWrongExpectedBytes(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_h264_aac.mp4");
    std::string detail;
    const auto error = ValidateMediaFile(file.path.wstring(), file.size + 1, 1000, kMegabyte,
                                         nullptr, &detail);
    ExpectValidation(error, MediaValidationError::Size, "wrong expected bytes");
    Expect(!detail.empty(), "wrong expected bytes: detail is empty");
}

void TestTooSmallLimit(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_h264_aac.mp4");
    const auto error = ValidateMediaFile(file.path.wstring(), 0, 1000, file.size - 1, nullptr,
                                         nullptr);
    ExpectValidation(error, MediaValidationError::Size, "artifact limit below file size");
}

void TestWrongDuration(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_h264_aac.mp4");
    std::string detail;
    const auto error = ValidateMediaFile(file.path.wstring(), file.size, 61000, kMegabyte, nullptr,
                                         &detail);
    ExpectValidation(error, MediaValidationError::Duration, "wrong expected duration");
    Expect(!detail.empty(), "wrong expected duration: detail is empty");
}

void TestMissingFile(const std::filesystem::path& directory) {
    MediaInfo info;
    info.width = 1234;
    std::string detail;
    const auto error = ValidateMediaFile((directory / L"does-not-exist.mp4").wstring(), 0, 1000,
                                         kMegabyte, &info, &detail);
    ExpectValidation(error, MediaValidationError::OpenFailed, "missing media file");
    Expect(!detail.empty(), "missing media file: detail is empty");
    Expect(info.width == 0 && info.duration_ticks == 0,
           "missing media file: info was not reset");
}

void TestEmptyFile(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / L"empty.mp4";
    WriteBytes(path, "");
    const auto error = ValidateMediaFile(path.wstring(), 0, 1000, kMegabyte, nullptr, nullptr);
    ExpectValidation(error, MediaValidationError::Size, "empty media file");
}

void TestNonH264(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_non_h264.mp4");
    std::string detail;
    const auto error = ValidateMediaFile(file.path.wstring(), file.size, 1000, kMegabyte, nullptr,
                                         &detail);
    ExpectValidation(error, MediaValidationError::VideoCodec, "non-H.264 media");
    Expect(!detail.empty(), "non-H.264 media: detail is empty");
}

void TestVideoOnly(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_video_only.mp4");
    const auto error = ValidateMediaFile(file.path.wstring(), file.size, 1000, kMegabyte, nullptr,
                                         nullptr);
    ExpectValidation(error, MediaValidationError::AudioCodec, "video without audio");
}

void TestSurroundAudio(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_5ch.mp4");
    const auto error = ValidateMediaFile(file.path.wstring(), file.size, 1000, kMegabyte, nullptr,
                                         nullptr);
    ExpectValidation(error, MediaValidationError::Channels, "5.1 audio");
}

void TestOversizedResolution(const std::filesystem::path& directory) {
    const FileFacts file = DataFile(directory, L"tv_muxed_720p_exceeded.mp4");
    const auto error = ValidateMediaFile(file.path.wstring(), file.size, 500, kMegabyte, nullptr,
                                         nullptr);
    ExpectValidation(error, MediaValidationError::Resolution, "video above 720p");
}

void TestTruncatedMedia(const std::filesystem::path& data_directory,
                        const std::filesystem::path& temporary) {
    const FileFacts valid = DataFile(data_directory, L"tv_muxed_h264_aac.mp4");
    const std::filesystem::path truncated = temporary / L"truncated.mp4";
    WriteFirstBytes(valid.path, truncated, 512);
    const auto error = ValidateMediaFile(truncated.wstring(), FileSize(truncated), 1000, kMegabyte,
                                         nullptr, nullptr);
    ExpectValidation(error, MediaValidationError::Container, "truncated MP4");
}

void TestCorruptMedia(const std::filesystem::path& temporary) {
    const std::filesystem::path path = temporary / L"corrupt.mp4";
    WriteBytes(path, std::string(1024, '\x5A'));
    const auto error = ValidateMediaFile(path.wstring(), 1024, 1000, kMegabyte, nullptr, nullptr);
    ExpectValidation(error, MediaValidationError::Container, "corrupted MP4");
}

}  // namespace

int main(int argc, char** argv) {
    const std::wstring supplied = argc > 1 ? Widen(argv[1]) : std::wstring{};
    const std::filesystem::path data_directory =
        supplied.empty() ? std::filesystem::path(L"plugins/BetterTV/tests/data")
                         : std::filesystem::path(supplied);
    const std::filesystem::path temporary = CreateTempDirectory();
    if (temporary.empty()) {
        std::cout << "FAIL: could not create the fixture temporary directory\n";
        return 1;
    }

    TestDownloadRejectsUnsafeUrl(temporary);
    TestDownloadRejectsDeclaredOversize(temporary);
    TestDownloadNormal(temporary);
    TestDownloadWithoutContentLength(temporary);
    TestDownloadSlowChunks(temporary);
    TestDownloadShortBody(temporary);
    TestDownloadReportedLengthMismatch(temporary);
    TestDownloadOversizeStream(temporary);
    TestDownloadCancelled(temporary);
    TestDownloadOpenFailure(temporary);
    TestDownloadReadFailure(temporary);
    TestDownloadIoFailure(temporary);

    TestValidMedia(data_directory);
    TestValidMediaTolerance(data_directory);
    TestWrongExpectedBytes(data_directory);
    TestTooSmallLimit(data_directory);
    TestWrongDuration(data_directory);
    TestMissingFile(data_directory);
    TestEmptyFile(temporary);
    TestNonH264(data_directory);
    TestVideoOnly(data_directory);
    TestSurroundAudio(data_directory);
    TestOversizedResolution(data_directory);
    TestTruncatedMedia(data_directory, temporary);
    TestCorruptMedia(temporary);

    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);

    if (g_failures != 0) {
        std::cout << "FAIL: " << g_failures << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "PASS: " << g_checks << " checks\n";
    return 0;
}
