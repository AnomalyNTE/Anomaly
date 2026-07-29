#pragma once

#include "anomaly/repository_configuration.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anomaly {

inline constexpr std::uint32_t kRepositoryCacheSchemaVersion = 1;
inline constexpr std::size_t kMaximumRepositoryCacheBytes =
    kMaximumRepositoryIndexBytes + 64U * 1024U;

enum class RepositoryIndexFreshness : std::uint8_t {
    NetworkFresh,
    CacheFresh,
    CacheStale,
};

enum class RepositoryPolicyError : std::uint8_t {
    None,
    InvalidIndex,
    RepositoryMismatch,
    UntrustedSignature,
    InvalidTimestamp,
    FutureIndex,
    ExpiredIndex,
    LifetimeExceeded,
    Replay,
    Equivocation,
    Downgrade,
    InvalidArtifact,
    CacheMissing,
    CacheInvalid,
    CacheExpired,
    IoFailure,
};

struct RepositoryAcceptedIndex {
    RepositoryIndex index;
    RepositoryIndexFreshness freshness{RepositoryIndexFreshness::NetworkFresh};
    bool installs_allowed{};
    std::string accepted_at;
    std::string payload_sha256;
};

struct RepositoryPolicyResult {
    RepositoryPolicyError error{RepositoryPolicyError::None};
    std::string message;
    std::optional<RepositoryAcceptedIndex> accepted;

    [[nodiscard]] bool Ok() const noexcept {
        return error == RepositoryPolicyError::None && accepted.has_value();
    }
};

struct RepositoryIndexPolicyOptions {
    std::filesystem::path state_directory;
};

class RepositoryIndexPolicy final {
public:
    explicit RepositoryIndexPolicy(RepositoryIndexPolicyOptions options);

    [[nodiscard]] RepositoryPolicyResult AcceptOnline(
        const RepositorySourceConfiguration& source,
        const RepositoryConfiguration& configuration,
        const RepositoryTrustStore& trust,
        std::string_view index_json,
        std::chrono::system_clock::time_point now) const;
    [[nodiscard]] RepositoryPolicyResult LoadCache(
        const RepositorySourceConfiguration& source,
        const RepositoryConfiguration& configuration,
        const RepositoryTrustStore& trust,
        std::chrono::system_clock::time_point now) const;

    [[nodiscard]] std::filesystem::path CachePath(
        const RepositorySourceConfiguration& source) const;

private:
    RepositoryIndexPolicyOptions options_;
};

[[nodiscard]] std::string_view RepositoryIndexFreshnessName(
    RepositoryIndexFreshness freshness) noexcept;
[[nodiscard]] std::string_view RepositoryPolicyErrorName(
    RepositoryPolicyError error) noexcept;

}  // namespace anomaly
