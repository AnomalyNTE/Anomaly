#pragma once

#include "bilibili_api.hpp"
#include "bilibili_source.hpp"
#include "http_client.hpp"
#include "media_download.hpp"
#include "media_validate.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace better_tv_video {

// The only playback profile this pipeline produces: muxed H.264/AAC MP4 at up
// to 720p.
constexpr std::string_view kPrepareFormatPolicy = "muxed-720p";
constexpr std::uint64_t kPrepareMaxArtifactBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kPrepareMaxProcessBytes = 2ull * 1024ull * 1024ull * 1024ull;

enum class PrepareStage {
    Idle,
    Resolving,
    Downloading,
    Validating,
    Ready,
    Failed,
    Cancelled
};

enum class PrepareError {
    None,
    InvalidLink,
    Network,
    Api,
    NoPlayableStream,
    SegmentedStream,
    Download,
    InvalidMedia,
    Oversize,
    Budget,
    Cancelled,
    Internal
};

struct PrepareSnapshot final {
    std::uint64_t request_id{};
    PrepareStage stage{PrepareStage::Idle};
    PrepareError error{PrepareError::None};
    // English technical detail for the plugin log only.
    std::string detail;
    // Validated local MP4 path when stage == Ready.
    std::string path;
    // Download progress; total is 0 when the server did not report a length.
    std::uint64_t received_bytes{};
    std::uint64_t total_bytes{};
    float progress{};
};

struct PrepareLimits final {
    std::uint64_t max_artifact_bytes{kPrepareMaxArtifactBytes};
    std::uint64_t max_process_bytes{kPrepareMaxProcessBytes};
};

struct PrepareOptions final {
    // Cache root. A per-session temporary directory lives below it and ready
    // artifacts are reused from the in-process map only.
    std::wstring cache_directory;
    PrepareLimits limits;
    // Short-link redirect fetcher; empty uses WinHTTP.
    RedirectFetcher fetch_redirect;
    // Bilibili API fetcher; empty uses HttpGetText.
    ApiFetcher fetch_api;
    // Media transport factory; empty uses DefaultMediaStreamFactory().
    HttpMediaStreamFactory open_media_stream;
    // Injected for fixtures; empty uses ParseBilibiliLink(input, fetch_redirect).
    std::function<BilibiliLinkResult(std::string_view)> resolve_link;
};

// Owns at most one preparation task and one worker thread. The pipeline is
// entirely in-process: no external executables and no Job Objects.
class MediaPreparer final {
public:
    MediaPreparer();
    ~MediaPreparer();
    MediaPreparer(const MediaPreparer&) = delete;
    MediaPreparer& operator=(const MediaPreparer&) = delete;

    // Creates the worker, cache root and temporary directory. Does not touch the
    // network. Returns false when the cache could not be prepared.
    bool Start(const PrepareOptions& options);
    // Cancels the active task and queues the newest request. Returns its id, or 0
    // when the preparer was not started.
    std::uint64_t Submit(std::string link);
    // Cancels the active task; a late result is dropped by request id.
    void Cancel();
    // Latest progress, or the final snapshot of the newest task.
    PrepareSnapshot Poll() const;
    // Cancels, joins the worker within `deadline_ms`, removes the temporary
    // directory and closes the cache session. Returns false on timeout.
    bool Stop(std::uint32_t deadline_ms);
    bool Started() const;

    // Removes one ready artifact from this session's cache, but only when the
    // path is a registered ready artifact inside the cache directory.
    bool Discard(std::string_view path);
    // Nonblocking Game/Render intent; deletion runs on the owned media worker.
    void RequestDiscard(std::string path);
    // Removes every ready artifact of this session (best effort).
    void DiscardAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// %LOCALAPPDATA%\Anomaly\BetterTV\media-cache
std::wstring DefaultMediaCacheDirectory();

}  // namespace better_tv_video
