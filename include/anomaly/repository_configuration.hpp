#pragma once

#include "anomaly/repository_index.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

inline constexpr std::uint32_t kRepositoryConfigurationSchemaVersion = 1;
inline constexpr std::size_t kMaximumRepositoryConfigurationBytes = 1024U * 1024U;

enum class RepositoryMirrorPolicy : std::uint8_t {
    PrimaryOnly,
    OrderedFallback,
};

enum class RepositoryWithdrawalPolicy : std::uint8_t {
    BlockNew,
};

enum class RepositoryDowngradePolicy : std::uint8_t {
    Reject,
};

struct RepositoryFreshnessConfiguration {
    std::uint32_t maximum_clock_skew_seconds{300};
    std::uint32_t maximum_index_age_seconds{86400};
    std::uint32_t maximum_offline_age_seconds{604800};
    RepositoryDowngradePolicy downgrade_policy{RepositoryDowngradePolicy::Reject};
};

struct RepositoryTrustKeyConfiguration {
    std::string key_id;
    std::string algorithm;
    std::string public_key_hex;
    std::string valid_from;
    std::string valid_until;
};

struct RepositorySourceConfiguration {
    std::string id;
    std::string index_uri;
    RepositoryChannel channel{RepositoryChannel::Stable};
    std::vector<std::string> key_ids;
    std::vector<std::string> mirrors;
    RepositoryMirrorPolicy mirror_policy{RepositoryMirrorPolicy::PrimaryOnly};
};

struct RepositoryConfiguration {
    std::uint32_t schema_version{kRepositoryConfigurationSchemaVersion};
    bool enabled{};
    bool allow_file_sources{};
    RepositoryWithdrawalPolicy withdrawal_policy{RepositoryWithdrawalPolicy::BlockNew};
    RepositoryFreshnessConfiguration freshness;
    std::vector<RepositorySourceConfiguration> sources;
    std::vector<RepositoryTrustKeyConfiguration> trust_keys;
};

struct RepositoryConfigurationDiagnostic {
    std::string path;
    std::string message;
};

struct RepositoryConfigurationResult {
    std::optional<RepositoryConfiguration> configuration;
    RepositoryTrustStore trust_store;
    std::vector<RepositoryConfigurationDiagnostic> diagnostics;

    [[nodiscard]] bool Ok() const noexcept {
        return configuration.has_value() && diagnostics.empty();
    }
};

[[nodiscard]] RepositoryConfigurationResult ParseRepositoryConfiguration(
    std::string_view json);
[[nodiscard]] RepositoryConfigurationResult LoadRepositoryConfiguration(
    const std::filesystem::path& path);
[[nodiscard]] std::string_view RepositoryConfigurationSchemaJson() noexcept;
[[nodiscard]] std::string_view RepositoryMirrorPolicyName(
    RepositoryMirrorPolicy policy) noexcept;
[[nodiscard]] std::string_view RepositoryWithdrawalPolicyName(
    RepositoryWithdrawalPolicy policy) noexcept;
[[nodiscard]] std::string_view RepositoryDowngradePolicyName(
    RepositoryDowngradePolicy policy) noexcept;

}  // namespace anomaly
