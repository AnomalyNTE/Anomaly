#pragma once

#include "anomaly/semver.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

inline constexpr std::uint32_t kRepositoryIndexSchemaVersion = 1;
inline constexpr std::size_t kMaximumRepositoryIndexBytes = 4U * 1024U * 1024U;

enum class RepositoryArtifactKind : std::uint8_t { Plugin, NteProfile };
enum class RepositoryChannel : std::uint8_t { Stable, Preview, Nightly };

struct RepositorySignature {
    std::string algorithm;
    std::string key_id;
    std::string value_hex;
};

struct RepositoryArtifact {
    RepositoryArtifactKind kind{RepositoryArtifactKind::Plugin};
    std::string id;
    SemanticVersion version;
    RepositoryChannel channel{RepositoryChannel::Stable};
    std::string uri;
    std::uint64_t size{};
    std::string sha256;
    std::string manifest_sha256;
    std::string published_at;
    std::string minimum_runtime;
    std::uint32_t minimum_api{};
    bool withdrawn{};
    std::string withdrawn_at;
    std::string withdrawal_reason;
    RepositorySignature signature;
};

struct RepositoryIndex {
    std::uint32_t schema_version{kRepositoryIndexSchemaVersion};
    std::string repository_id;
    std::uint64_t sequence{};
    std::string generated_at;
    std::string expires_at;
    std::vector<RepositoryArtifact> artifacts;
    RepositorySignature signature;
};

struct RepositoryDiagnostic {
    std::string path;
    std::string message;
};

struct RepositoryIndexParseResult {
    std::optional<RepositoryIndex> index;
    std::vector<RepositoryDiagnostic> diagnostics;
    [[nodiscard]] bool Ok() const noexcept {
        return index.has_value() && diagnostics.empty();
    }
};

class RepositoryTrustStore final {
public:
    [[nodiscard]] bool AddEcdsaP256Key(
        std::string key_id, std::string public_key_hex);
    [[nodiscard]] std::string_view Find(std::string_view key_id) const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept { return keys_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return keys_.empty(); }
private:
    std::map<std::string, std::string, std::less<>> keys_;
};

[[nodiscard]] RepositoryIndexParseResult ParseRepositoryIndex(std::string_view json);
[[nodiscard]] std::string_view RepositoryIndexSchemaJson() noexcept;
[[nodiscard]] std::string CanonicalRepositoryArtifactPayload(
    const RepositoryArtifact& artifact);
[[nodiscard]] std::string CanonicalRepositoryIndexPayload(
    const RepositoryIndex& index);
[[nodiscard]] bool VerifyRepositoryArtifactSignature(
    const RepositoryArtifact& artifact, const RepositoryTrustStore& trust) noexcept;
[[nodiscard]] bool VerifyRepositoryIndexSignature(
    const RepositoryIndex& index, const RepositoryTrustStore& trust) noexcept;
[[nodiscard]] std::string_view RepositoryArtifactKindName(
    RepositoryArtifactKind kind) noexcept;
[[nodiscard]] std::string_view RepositoryChannelName(RepositoryChannel channel) noexcept;
[[nodiscard]] bool ParseRepositoryUtcTimestamp(
    std::string_view value,
    std::chrono::system_clock::time_point& timestamp) noexcept;
[[nodiscard]] std::string FormatRepositoryUtcTimestamp(
    std::chrono::system_clock::time_point timestamp);

}  // namespace anomaly
