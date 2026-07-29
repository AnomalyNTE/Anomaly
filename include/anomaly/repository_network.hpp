#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>

namespace anomaly {

enum class RepositoryNetworkError : std::uint8_t {
    None,
    Stopped,
    InvalidUri,
    InsecureTransport,
    ConnectionFailure,
    HttpFailure,
    SizeLimitExceeded,
    IoFailure,
};

struct RepositoryNetworkResult {
    RepositoryNetworkError error{RepositoryNetworkError::None};
    std::string message;
    std::uint64_t bytes{};
    std::uint32_t worker_thread_id{};
    [[nodiscard]] bool Ok() const noexcept { return error == RepositoryNetworkError::None; }
};

struct RepositoryNetworkRequest {
    std::string uri;
    std::filesystem::path destination;
    std::uint64_t maximum_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct RepositoryNetworkSnapshot {
    bool running{};
    std::uint32_t worker_thread_id{};
    std::uint64_t completed{};
    std::uint64_t failed{};
    std::size_t queued{};
};

// Every transfer runs on one owned worker. Futures are completed without invoking
// callbacks on Runtime, Game, or Render threads.
class RepositoryNetworkService final {
public:
    RepositoryNetworkService();
    ~RepositoryNetworkService();
    RepositoryNetworkService(const RepositoryNetworkService&) = delete;
    RepositoryNetworkService& operator=(const RepositoryNetworkService&) = delete;

    [[nodiscard]] std::future<RepositoryNetworkResult> FetchToFile(
        RepositoryNetworkRequest request);
    void Stop() noexcept;
    [[nodiscard]] RepositoryNetworkSnapshot Snapshot() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
