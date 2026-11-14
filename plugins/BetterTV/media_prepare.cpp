#include "media_prepare.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace better_tv_video {
namespace {

constexpr wchar_t kTempFolderName[] = L"tmp";
constexpr wchar_t kTaskPrefix[] = L"task-";
constexpr wchar_t kFinalFileName[] = L"final.mp4";
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

std::wstring JoinPath(std::wstring_view directory, std::wstring_view name) {
    std::wstring joined(directory);
    if (!joined.empty() && joined.back() != L'\\' && joined.back() != L'/')
        joined.push_back(L'\\');
    joined.append(name);
    return joined;
}

std::string Utf8FromWide(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring WideFromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size) != size) {
        return {};
    }
    return result;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void RemoveTree(const std::wstring& path) {
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(path), error);
}

bool CreateDirectoryTree(const std::wstring& path) {
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path), error);
    return !error;
}

std::uint64_t DirectoryBytes(const std::wstring& root) {
    if (root.empty()) return 0;
    std::uint64_t total = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(std::filesystem::path(root), error);
    if (error) return total;
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        std::error_code entry_error;
        if (iterator->is_regular_file(entry_error)) {
            total += static_cast<std::uint64_t>(iterator->file_size(entry_error));
            if (entry_error) {
                entry_error.clear();
            }
        }
        iterator.increment(error);
        if (error) break;
    }
    return total;
}

// Safe cache file name derived from the normalized link/page key. The format
// policy is appended so a future profile never reuses an incompatible artifact.
std::wstring CacheFileName(std::string_view cache_key) {
    std::wstring name;
    for (const char character : cache_key) {
        const unsigned char value = static_cast<unsigned char>(character);
        const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                          (value >= '0' && value <= '9') || value == '.' || value == '-' ||
                          value == '_';
        name.push_back(safe ? static_cast<wchar_t>(value) : L'_');
        if (name.size() >= 80) break;
    }
    if (name.empty()) name = L"video";
    name.push_back(L'-');
    name += WideFromUtf8(kPrepareFormatPolicy);
    name += L".mp4";
    return name;
}

PrepareError MapApiError(const BilibiliApiError error) {
    switch (error) {
    case BilibiliApiError::None:
        return PrepareError::Internal;
    case BilibiliApiError::Network:
        return PrepareError::Network;
    case BilibiliApiError::HttpStatus:
        return PrepareError::Network;
    case BilibiliApiError::ApiRejected:
    case BilibiliApiError::InvalidDocument:
    case BilibiliApiError::PageOutOfRange:
        return PrepareError::Api;
    case BilibiliApiError::NoPlayableStream:
    case BilibiliApiError::UnsupportedQuality:
        return PrepareError::NoPlayableStream;
    case BilibiliApiError::SegmentedStream:
        return PrepareError::SegmentedStream;
    }
    return PrepareError::Internal;
}

}  // namespace

struct MediaPreparer::Impl final {
    struct PendingRequest final {
        std::uint64_t id{};
        std::string link;
    };

    mutable std::mutex mutex;
    // Serialises Stop callers so only one joins the worker.
    std::mutex join_mutex;
    std::condition_variable wake;
    std::condition_variable worker_done_cv;
    std::thread worker;
    PrepareOptions options;
    bool started{};
    bool stopping{};
    bool cancel_requested{};
    bool worker_done{};
    std::uint64_t latest_request_id{};
    std::optional<PendingRequest> pending;
    std::vector<std::string> discard_requests;
    PrepareSnapshot snapshot;
    std::map<std::string, std::wstring> cache;
    std::wstring temp_root;
    std::chrono::steady_clock::time_point last_progress;

    void Publish(std::uint64_t request_id, PrepareStage stage, PrepareError error,
                 std::string detail, std::string path) {
        std::scoped_lock lock(mutex);
        if (latest_request_id != request_id) return;
        snapshot.request_id = request_id;
        snapshot.stage = stage;
        snapshot.error = error;
        snapshot.detail = std::move(detail);
        snapshot.path = std::move(path);
        if (stage != PrepareStage::Downloading) {
            snapshot.received_bytes = 0;
            snapshot.total_bytes = 0;
            snapshot.progress = 0.0F;
        }
    }

    void PublishProgress(std::uint64_t request_id, std::uint64_t received,
                         std::uint64_t total) {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(mutex);
        if (latest_request_id != request_id) return;
        if (snapshot.stage != PrepareStage::Downloading) return;
        if (now - last_progress < kProgressInterval && received != total) return;
        last_progress = now;
        snapshot.received_bytes = received;
        snapshot.total_bytes = total;
        snapshot.progress = total > 0
                                ? std::min(1.0F, static_cast<float>(
                                                     static_cast<double>(received) /
                                                     static_cast<double>(total)))
                                : 0.0F;
    }

    bool IsCancelled(std::uint64_t request_id) const {
        std::scoped_lock lock(mutex);
        return stopping || cancel_requested || latest_request_id != request_id;
    }

    void Run() {
        for (;;) {
            std::optional<PendingRequest> request;
            std::vector<std::string> discard;
            bool stop{};
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || pending || !discard_requests.empty(); });
                discard.swap(discard_requests);
                stop = stopping;
                if (!stop) {
                    request = std::exchange(pending, {});
                    if (request) cancel_requested = false;
                }
            }
            for (const auto& path : discard) Discard(path);
            if (stop) break;
            if (request) Process(*request);
        }
        {
            std::scoped_lock lock(mutex);
            worker_done = true;
        }
        worker_done_cv.notify_all();
    }

    bool Discard(std::string_view path) {
        if (path.empty()) return false;
        const std::wstring target = WideFromUtf8(path);
        if (target.empty()) return false;
        {
            std::scoped_lock lock(mutex);
            auto entry = std::find_if(cache.begin(), cache.end(), [&target](const auto& item) {
                return CompareStringOrdinal(item.second.c_str(), static_cast<int>(item.second.size()),
                    target.c_str(), static_cast<int>(target.size()), TRUE) == CSTR_EQUAL;
            });
            if (entry == cache.end()) return false;
            cache.erase(entry);
        }
        return DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    void Process(const PendingRequest& request) {
        const std::uint64_t request_id = request.id;
        std::wstring task_directory;
        const auto cancelled = [this, request_id, &task_directory]() {
            RemoveTree(task_directory);
            Publish(request_id, PrepareStage::Cancelled, PrepareError::Cancelled, "cancelled",
                    std::string{});
        };
        const auto fail = [this, request_id, &task_directory](PrepareError error,
                                                             std::string detail) {
            RemoveTree(task_directory);
            Publish(request_id, PrepareStage::Failed, error, std::move(detail), std::string{});
        };

        Publish(request_id, PrepareStage::Resolving, PrepareError::None, std::string{},
                std::string{});
        if (IsCancelled(request_id)) return cancelled();

        BilibiliLinkResult link_result;
        try {
            if (options.resolve_link) {
                link_result = options.resolve_link(request.link);
            } else if (options.fetch_redirect) {
                link_result = ParseBilibiliLink(request.link, options.fetch_redirect);
            } else {
                link_result = ParseBilibiliLink(request.link);
            }
        } catch (...) {
            return fail(PrepareError::Internal, "link resolver raised an exception");
        }
        if (IsCancelled(request_id)) return cancelled();
        if (!link_result.page.has_value()) {
            const std::string detail = link_result.detail.empty() ? "link resolution failed"
                                                                  : link_result.detail;
            return fail(link_result.error == BilibiliLinkError::Network ? PrepareError::Network
                                                                       : PrepareError::InvalidLink,
                        detail);
        }
        const BilibiliPage page = *link_result.page;

        std::wstring cached_path;
        {
            std::scoped_lock lock(mutex);
            const auto entry = cache.find(page.cache_key);
            if (entry != cache.end()) cached_path = entry->second;
        }
        if (!cached_path.empty()) {
            if (FileExists(cached_path)) {
                Publish(request_id, PrepareStage::Ready, PrepareError::None, "media cache hit",
                        Utf8FromWide(cached_path));
                return;
            }
            std::scoped_lock lock(mutex);
            cache.erase(page.cache_key);
        }
        if (IsCancelled(request_id)) return cancelled();

        BilibiliApiResult api_result;
        try {
            const ApiFetcher fetch_api =
                !options.fetch_api
                    ? ApiFetcher([](std::string_view url) { return HttpGetText(url); })
                    : options.fetch_api;
            api_result = ResolveMuxedPlayInfo(page, fetch_api);
        } catch (...) {
            return fail(PrepareError::Internal, "bilibili api raised an exception");
        }
        if (IsCancelled(request_id)) return cancelled();
        if (!api_result.info.has_value()) {
            return fail(MapApiError(api_result.error),
                        api_result.detail.empty() ? "bilibili api failed" : api_result.detail);
        }
        const BilibiliPlayInfo info = *api_result.info;
        if (info.media_size > options.limits.max_artifact_bytes) {
            return fail(PrepareError::Oversize,
                        "muxed stream uses " + std::to_string(info.media_size) +
                            " bytes, over the " + std::to_string(options.limits.max_artifact_bytes) +
                            " byte artifact limit");
        }
        const std::uint64_t used = DirectoryBytes(options.cache_directory);
        if (used > options.limits.max_process_bytes ||
            info.media_size > options.limits.max_process_bytes - used) {
            return fail(PrepareError::Budget,
                        "media cache and temporary files use " + std::to_string(used) +
                            " bytes, over the " + std::to_string(options.limits.max_process_bytes) +
                            " byte budget");
        }

        task_directory = JoinPath(temp_root, kTaskPrefix + std::to_wstring(request_id));
        if (!CreateDirectoryTree(task_directory)) {
            return fail(PrepareError::Internal, "could not create the task temporary directory");
        }
        const std::wstring destination = JoinPath(task_directory, kFinalFileName);

        Publish(request_id, PrepareStage::Downloading, PrepareError::None, std::string{},
                std::string{});
        const HttpMediaStreamFactory factory =
            options.open_media_stream ? options.open_media_stream : DefaultMediaStreamFactory();
        const DownloadResult download = DownloadMedia(
            info.media_url, destination, info.media_size, options.limits.max_artifact_bytes,
            [this, request_id] { return IsCancelled(request_id); },
            [this, request_id](const DownloadProgress& progress) {
                PublishProgress(request_id, progress.received, progress.total);
            },
            factory);
        if (download.error == DownloadError::Cancelled) return cancelled();
        if (download.error != DownloadError::None) {
            PrepareError error = PrepareError::Download;
            if (download.error == DownloadError::Oversize) error = PrepareError::Oversize;
            else if (download.error == DownloadError::Network) error = PrepareError::Network;
            else if (download.error == DownloadError::InvalidUrl) error = PrepareError::Api;
            return fail(error, download.detail.empty() ? "media download failed" : download.detail);
        }
        {
            std::scoped_lock lock(mutex);
            if (latest_request_id == request_id) {
                snapshot.received_bytes = download.bytes;
                snapshot.total_bytes = download.bytes;
                snapshot.progress = 1.0F;
            }
        }
        if (IsCancelled(request_id)) return cancelled();

        Publish(request_id, PrepareStage::Validating, PrepareError::None, std::string{},
                std::string{});
        MediaInfo media;
        std::string validation_detail;
        const MediaValidationError validation = ValidateMediaFile(
            destination, info.media_size, info.duration_ms, options.limits.max_artifact_bytes,
            &media, &validation_detail);
        if (validation != MediaValidationError::None) {
            const PrepareError error =
                validation == MediaValidationError::Size ? PrepareError::Oversize
                                                         : PrepareError::InvalidMedia;
            return fail(error, validation_detail.empty() ? "media validation failed"
                                                         : validation_detail);
        }
        if (IsCancelled(request_id)) return cancelled();

        const std::wstring cache_path =
            JoinPath(options.cache_directory, CacheFileName(page.cache_key));
        if (!MoveFileExW(destination.c_str(), cache_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return fail(PrepareError::Internal,
                        "could not move the prepared file into the cache (error " +
                            std::to_string(GetLastError()) + ")");
        }
        {
            std::scoped_lock lock(mutex);
            if (latest_request_id == request_id) cache[page.cache_key] = cache_path;
        }
        RemoveTree(task_directory);
        std::string ready_detail = "prepared " + std::to_string(media.width) + "x" +
                                   std::to_string(media.height) + " " +
                                   std::to_string(media.duration_ms / 1000) + "s";
        Publish(request_id, PrepareStage::Ready, PrepareError::None, std::move(ready_detail),
                Utf8FromWide(cache_path));
    }
};

MediaPreparer::MediaPreparer() : impl_(std::make_unique<Impl>()) {}

MediaPreparer::~MediaPreparer() {
    if (!impl_ || !impl_->started) return;
    if (!Stop(2000)) {
        // The worker still owns its state; leaking it keeps a stuck worker from
        // touching freed memory while the host unloads the plugin.
        impl_.release();
    }
}

bool MediaPreparer::Start(const PrepareOptions& options) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->started) return true;
    if (options.cache_directory.empty()) return false;

    // Every plugin session owns its cache; leftovers from a previous process or
    // generation are reclaimed here (best effort while a player may hold one).
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(options.cache_directory), error);
    error.clear();
    std::filesystem::create_directories(std::filesystem::path(options.cache_directory), error);
    if (error) return false;

    impl_->options = options;
    impl_->temp_root = JoinPath(options.cache_directory, kTempFolderName);
    if (!CreateDirectoryTree(impl_->temp_root)) return false;

    impl_->snapshot = PrepareSnapshot{};
    impl_->cache.clear();
    impl_->stopping = false;
    impl_->cancel_requested = false;
    impl_->worker_done = false;
    impl_->pending.reset();
    try {
        impl_->worker = std::thread([this] { impl_->Run(); });
    } catch (...) {
        return false;
    }
    impl_->started = true;
    return true;
}

std::uint64_t MediaPreparer::Submit(std::string link) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->started) return 0;
    const std::uint64_t request_id = ++impl_->latest_request_id;
    impl_->cancel_requested = true;
    impl_->snapshot = PrepareSnapshot{request_id, PrepareStage::Resolving, PrepareError::None,
                                      std::string{}, std::string{}};
    impl_->pending = Impl::PendingRequest{request_id, std::move(link)};
    impl_->wake.notify_one();
    return request_id;
}

void MediaPreparer::Cancel() {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->started) return;
    impl_->cancel_requested = true;
    impl_->pending.reset();
    if (impl_->snapshot.stage == PrepareStage::Idle) return;
    if (!(impl_->snapshot.stage == PrepareStage::Resolving ||
          impl_->snapshot.stage == PrepareStage::Downloading ||
          impl_->snapshot.stage == PrepareStage::Validating)) {
        return;
    }
    impl_->snapshot.stage = PrepareStage::Cancelled;
    impl_->snapshot.error = PrepareError::Cancelled;
    impl_->snapshot.detail = "cancelled";
    impl_->snapshot.path.clear();
}

PrepareSnapshot MediaPreparer::Poll() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->snapshot;
}

bool MediaPreparer::Stop(std::uint32_t deadline_ms) {
    std::scoped_lock join_lock(impl_->join_mutex);
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->started) return true;
        impl_->stopping = true;
        impl_->cancel_requested = true;
        impl_->pending.reset();
        impl_->wake.notify_all();
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    bool finished = false;
    {
        std::unique_lock lock(impl_->mutex);
        finished = impl_->worker_done_cv.wait_until(lock, deadline,
                                                    [this] { return impl_->worker_done; });
    }
    if (!finished) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->started = false;
        impl_->stopping = false;
        impl_->cancel_requested = false;
        impl_->worker_done = false;
        impl_->pending.reset();
    }
    RemoveTree(impl_->temp_root);
    return true;
}

bool MediaPreparer::Started() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->started;
}

bool MediaPreparer::Discard(std::string_view path) { return impl_->Discard(path); }

void MediaPreparer::RequestDiscard(std::string path) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->started || impl_->stopping) return;
    impl_->discard_requests.push_back(std::move(path));
    impl_->wake.notify_all();
}

void MediaPreparer::DiscardAll() {
    std::map<std::string, std::wstring> ready;
    {
        std::scoped_lock lock(impl_->mutex);
        ready.swap(impl_->cache);
    }
    for (const auto& entry : ready) {
        if (!DeleteFileW(entry.second.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            // The player may still hold it; the next Start reclaims the cache root.
        }
    }
}

std::wstring DefaultMediaCacheDirectory() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                    &local_app_data)) ||
        local_app_data == nullptr) {
        return {};
    }
    std::wstring path(local_app_data);
    CoTaskMemFree(local_app_data);
    if (path.empty()) return {};
    return JoinPath(path, L"Anomaly\\BetterTV\\media-cache");
}

}  // namespace better_tv_video
