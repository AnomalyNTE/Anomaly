#include "anomaly/repository_network.hpp"
#include "anomaly/sdk/version.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <winhttp.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <thread>

namespace anomaly {
namespace {

#define ANOMALY_WIDEN_VERSION_IMPL(value) L##value
#define ANOMALY_WIDEN_VERSION(value) ANOMALY_WIDEN_VERSION_IMPL(value)

constexpr wchar_t kRepositoryUserAgent[] =
    L"Anomaly/" ANOMALY_WIDEN_VERSION(ANOMALY_SDK_VERSION_STRING) L" repository";

#undef ANOMALY_WIDEN_VERSION
#undef ANOMALY_WIDEN_VERSION_IMPL

RepositoryNetworkResult Failure(
    RepositoryNetworkError error, std::string message, std::uint32_t thread_id) {
    return {error, std::move(message), 0, thread_id};
}

class WinHttpHandle final {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET value) : value_(value) {}
    ~WinHttpHandle() { if (value_ != nullptr) WinHttpCloseHandle(value_); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
private:
    HINTERNET value_{};
};

bool PublishTemporary(
    const std::filesystem::path& temporary, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return false;
    std::filesystem::remove(destination, error);
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    return !error;
}

RepositoryNetworkResult CopyLocal(
    const RepositoryNetworkRequest& request, std::stop_token stop_token,
    std::uint32_t thread_id) {
    std::wstring uri;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, request.uri.data(),
        static_cast<int>(request.uri.size()), nullptr, 0);
    if (required <= 0) return Failure(RepositoryNetworkError::InvalidUri, "file URI is invalid", thread_id);
    uri.resize(required);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request.uri.data(),
        static_cast<int>(request.uri.size()), uri.data(), required);
    std::wstring path(32768, L'\0');
    DWORD path_size = static_cast<DWORD>(path.size());
    if (FAILED(PathCreateFromUrlW(uri.c_str(), path.data(), &path_size, 0))) {
        return Failure(RepositoryNetworkError::InvalidUri, "file URI path is invalid", thread_id);
    }
    path.resize(path_size);
    std::ifstream input(path, std::ios::binary);
    if (!input) return Failure(RepositoryNetworkError::IoFailure, "local artifact open failed", thread_id);
    std::error_code error;
    std::filesystem::create_directories(request.destination.parent_path(), error);
    const auto temporary = request.destination.wstring() + L".download";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t total{};
    while (input && !stop_token.stop_requested()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        total += static_cast<std::uint64_t>(count);
        if (total > request.maximum_bytes) {
            output.close(); std::filesystem::remove(temporary, error);
            return Failure(RepositoryNetworkError::SizeLimitExceeded, "local artifact exceeds limit", thread_id);
        }
        output.write(buffer.data(), count);
        if (!output) break;
    }
    output.close();
    if (stop_token.stop_requested()) {
        std::filesystem::remove(temporary, error);
        return Failure(RepositoryNetworkError::Stopped, "transfer stopped", thread_id);
    }
    if (!input.eof() || !output || !PublishTemporary(temporary, request.destination)) {
        std::filesystem::remove(temporary, error);
        return Failure(RepositoryNetworkError::IoFailure, "local artifact copy failed", thread_id);
    }
    return {RepositoryNetworkError::None, {}, total, thread_id};
}

RepositoryNetworkResult DownloadHttps(
    const RepositoryNetworkRequest& request, std::stop_token stop_token,
    std::uint32_t thread_id) {
    std::wstring uri;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        request.uri.data(), static_cast<int>(request.uri.size()), nullptr, 0);
    if (required <= 0) return Failure(RepositoryNetworkError::InvalidUri, "HTTPS URI is invalid", thread_id);
    uri.resize(required);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request.uri.data(),
        static_cast<int>(request.uri.size()), uri.data(), required);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = DWORD(-1);
    components.dwHostNameLength = DWORD(-1);
    components.dwUrlPathLength = DWORD(-1);
    components.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(uri.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        return Failure(RepositoryNetworkError::InsecureTransport, "only HTTPS repositories are accepted", thread_id);
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    WinHttpHandle session(WinHttpOpen(kRepositoryUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.get() == nullptr) return Failure(RepositoryNetworkError::ConnectionFailure, "WinHTTP session failed", thread_id);
    WinHttpSetTimeouts(session.get(), 10000, 10000, 30000, 30000);
    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    WinHttpHandle response(connection.get() == nullptr ? nullptr : WinHttpOpenRequest(
        connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (response.get() == nullptr || !WinHttpSendRequest(response.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(response.get(), nullptr)) {
        return Failure(RepositoryNetworkError::ConnectionFailure, "HTTPS request failed", thread_id);
    }
    DWORD status{};
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(response.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        return Failure(RepositoryNetworkError::HttpFailure, "repository returned a non-success status", thread_id);
    }
    std::error_code error;
    std::filesystem::create_directories(request.destination.parent_path(), error);
    const auto temporary = request.destination.wstring() + L".download";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    std::uint64_t total{};
    while (!stop_token.stop_requested()) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(response.get(), &available)) break;
        if (available == 0) {
            output.close();
            if (!PublishTemporary(temporary, request.destination)) {
                return Failure(RepositoryNetworkError::IoFailure, "download publish failed", thread_id);
            }
            return {RepositoryNetworkError::None, {}, total, thread_id};
        }
        if (total > request.maximum_bytes ||
            static_cast<std::uint64_t>(available) > request.maximum_bytes - total) {
            output.close(); std::filesystem::remove(temporary, error);
            return Failure(RepositoryNetworkError::SizeLimitExceeded, "download exceeds limit", thread_id);
        }
        std::vector<std::uint8_t> buffer(available);
        DWORD received{};
        if (!WinHttpReadData(response.get(), buffer.data(), available, &received) || received == 0) break;
        total += received;
        if (total > request.maximum_bytes) {
            output.close(); std::filesystem::remove(temporary, error);
            return Failure(RepositoryNetworkError::SizeLimitExceeded, "download exceeds limit", thread_id);
        }
        output.write(reinterpret_cast<const char*>(buffer.data()), received);
        if (!output) break;
    }
    output.close();
    std::filesystem::remove(temporary, error);
    return Failure(stop_token.stop_requested() ? RepositoryNetworkError::Stopped
        : RepositoryNetworkError::ConnectionFailure,
        stop_token.stop_requested() ? "transfer stopped" : "HTTPS response read failed", thread_id);
}

}  // namespace

class RepositoryNetworkService::Impl final {
public:
    Impl() : worker_([this](std::stop_token token) { Run(token); }) {}
    ~Impl() { Stop(); }

    std::future<RepositoryNetworkResult> Submit(RepositoryNetworkRequest request) {
        auto task = std::make_shared<Task>();
        task->request = std::move(request);
        auto future = task->promise.get_future();
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) {
                task->promise.set_value(Failure(
                    RepositoryNetworkError::Stopped, "network service is stopped", 0));
                return future;
            }
            queue_.push_back(std::move(task));
        }
        ready_.notify_one();
        return future;
    }

    void Stop() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        worker_.request_stop();
        ready_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    RepositoryNetworkSnapshot Snapshot() const noexcept {
        std::scoped_lock lock(mutex_);
        return {!stopping_, worker_thread_id_, completed_, failed_, queue_.size()};
    }

private:
    struct Task {
        RepositoryNetworkRequest request;
        std::promise<RepositoryNetworkResult> promise;
    };

    void Run(std::stop_token token) noexcept {
        {
            std::scoped_lock lock(mutex_);
            worker_thread_id_ = GetCurrentThreadId();
        }
        while (!token.stop_requested()) {
            std::shared_ptr<Task> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, token, [&] { return !queue_.empty() || stopping_; });
                if (queue_.empty()) break;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            RepositoryNetworkResult result;
            if (task->request.uri.starts_with("file://")) {
                result = CopyLocal(task->request, token, GetCurrentThreadId());
            } else if (task->request.uri.starts_with("https://")) {
                result = DownloadHttps(task->request, token, GetCurrentThreadId());
            } else {
                result = Failure(RepositoryNetworkError::InsecureTransport,
                    "repository URI must use HTTPS", GetCurrentThreadId());
            }
            task->promise.set_value(result);
            std::scoped_lock lock(mutex_);
            result.Ok() ? ++completed_ : ++failed_;
        }
        std::deque<std::shared_ptr<Task>> cancelled;
        {
            std::scoped_lock lock(mutex_);
            cancelled.swap(queue_);
        }
        for (auto& task : cancelled) task->promise.set_value(Failure(
            RepositoryNetworkError::Stopped, "network service stopped", GetCurrentThreadId()));
    }

    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::shared_ptr<Task>> queue_;
    std::jthread worker_;
    bool stopping_{};
    std::uint32_t worker_thread_id_{};
    std::uint64_t completed_{};
    std::uint64_t failed_{};
};

RepositoryNetworkService::RepositoryNetworkService() : impl_(std::make_unique<Impl>()) {}
RepositoryNetworkService::~RepositoryNetworkService() { Stop(); }
std::future<RepositoryNetworkResult> RepositoryNetworkService::FetchToFile(
    RepositoryNetworkRequest request) { return impl_->Submit(std::move(request)); }
void RepositoryNetworkService::Stop() noexcept { impl_->Stop(); }
RepositoryNetworkSnapshot RepositoryNetworkService::Snapshot() const noexcept {
    return impl_->Snapshot();
}

}  // namespace anomaly
