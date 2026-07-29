#include "anomaly/repository_policy.hpp"

#include "anomaly/artifact_crypto.hpp"
#include "repository_cache_schema.hpp"

#include <Windows.h>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <utility>

namespace anomaly {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::system_clock;

struct CacheRecord {
    std::string source_id;
    std::string repository_id;
    std::uint64_t sequence{};
    std::string generated_at;
    std::string expires_at;
    std::string accepted_at;
    std::string payload_sha256;
    std::string index_json;
};

struct ValidatedIndex {
    RepositoryIndex index;
    Clock::time_point generated_at;
    Clock::time_point expires_at;
    std::string payload_sha256;
};

RepositoryPolicyResult Failure(RepositoryPolicyError error, std::string message) {
    RepositoryPolicyResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

const nlohmann::json_schema::json_validator& CacheValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        Json schema = Json::parse(
            detail::kRepositoryCacheSchemaJson.begin(),
            detail::kRepositoryCacheSchemaJson.end());
        return nlohmann::json_schema::json_validator(std::move(schema));
    }();
    return validator;
}

std::string PayloadHash(const RepositoryIndex& index) {
    const std::string payload = CanonicalRepositoryIndexPayload(index);
    return Sha256Hex({reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
}

const RepositoryTrustKeyConfiguration* FindTrustKey(
    const RepositoryConfiguration& configuration, std::string_view key_id) noexcept {
    const auto found = std::ranges::find_if(
        configuration.trust_keys,
        [key_id](const RepositoryTrustKeyConfiguration& key) {
            return key.key_id == key_id;
        });
    return found == configuration.trust_keys.end() ? nullptr : &*found;
}

bool SourceAllowsKey(
    const RepositorySourceConfiguration& source, std::string_view key_id) noexcept {
    return std::ranges::find(source.key_ids, key_id) != source.key_ids.end();
}

bool KeyValidAt(
    const RepositoryTrustKeyConfiguration& key, Clock::time_point timestamp) noexcept {
    Clock::time_point valid_from;
    Clock::time_point valid_until;
    return ParseRepositoryUtcTimestamp(key.valid_from, valid_from) &&
        ParseRepositoryUtcTimestamp(key.valid_until, valid_until) &&
        valid_from <= timestamp && timestamp < valid_until;
}

RepositoryPolicyResult ValidateIndex(
    const RepositorySourceConfiguration& source,
    const RepositoryConfiguration& configuration,
    const RepositoryTrustStore& trust,
    std::string_view index_json,
    Clock::time_point now,
    bool allow_expired,
    ValidatedIndex& validated) {
    auto parsed = ParseRepositoryIndex(index_json);
    if (!parsed.Ok()) {
        const std::string reason = parsed.diagnostics.empty()
            ? "repository index parse failed" : parsed.diagnostics.front().message;
        return Failure(RepositoryPolicyError::InvalidIndex, reason);
    }
    validated.index = std::move(*parsed.index);
    if (validated.index.repository_id != source.id) {
        return Failure(
            RepositoryPolicyError::RepositoryMismatch,
            "repository index identity does not match the configured source");
    }
    if (!ParseRepositoryUtcTimestamp(validated.index.generated_at, validated.generated_at) ||
        !ParseRepositoryUtcTimestamp(validated.index.expires_at, validated.expires_at)) {
        return Failure(RepositoryPolicyError::InvalidTimestamp, "repository index timestamp is invalid");
    }

    const auto clock_skew =
        std::chrono::seconds{configuration.freshness.maximum_clock_skew_seconds};
    const auto maximum_age =
        std::chrono::seconds{configuration.freshness.maximum_index_age_seconds};
    if (validated.generated_at > now + clock_skew) {
        return Failure(RepositoryPolicyError::FutureIndex, "repository index is too far in the future");
    }
    if (validated.expires_at - validated.generated_at > maximum_age) {
        return Failure(
            RepositoryPolicyError::LifetimeExceeded,
            "repository index validity exceeds the configured maximum age");
    }
    if (!allow_expired && now > validated.expires_at + clock_skew) {
        return Failure(RepositoryPolicyError::ExpiredIndex, "repository index has expired");
    }

    const auto* index_key = FindTrustKey(configuration, validated.index.signature.key_id);
    if (!SourceAllowsKey(source, validated.index.signature.key_id) || index_key == nullptr ||
        !KeyValidAt(*index_key, validated.generated_at) ||
        !VerifyRepositoryIndexSignature(validated.index, trust)) {
        return Failure(
            RepositoryPolicyError::UntrustedSignature,
            "repository index signature is not trusted for this source and signing time");
    }

    for (const auto& artifact : validated.index.artifacts) {
        Clock::time_point published_at;
        const auto* artifact_key = FindTrustKey(configuration, artifact.signature.key_id);
        if (!ParseRepositoryUtcTimestamp(artifact.published_at, published_at) ||
            published_at > validated.generated_at + clock_skew ||
            !SourceAllowsKey(source, artifact.signature.key_id) || artifact_key == nullptr ||
            !KeyValidAt(*artifact_key, published_at) ||
            !VerifyRepositoryArtifactSignature(artifact, trust) ||
            (!configuration.allow_file_sources && artifact.uri.starts_with("file://"))) {
            return Failure(
                RepositoryPolicyError::InvalidArtifact,
                "repository index contains an invalid or unauthorized artifact descriptor");
        }
    }

    validated.payload_sha256 = PayloadHash(validated.index);
    return {};
}

bool LoadCacheRecord(
    const std::filesystem::path& path,
    std::optional<CacheRecord>& record,
    std::string& message) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        message = "repository cache state cannot be inspected";
        return false;
    }
    if (!exists) {
        record.reset();
        return true;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumRepositoryCacheBytes) {
        message = "repository cache state is unreadable or oversized";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(contents.size())) {
        message = "repository cache state could not be read";
        return false;
    }
    try {
        const Json document = Json::parse(contents);
        CacheValidator().validate(document);
        CacheRecord loaded;
        loaded.source_id = document.at("sourceId").get<std::string>();
        loaded.repository_id = document.at("repositoryId").get<std::string>();
        loaded.sequence = document.at("sequence").get<std::uint64_t>();
        loaded.generated_at = document.at("generatedAt").get<std::string>();
        loaded.expires_at = document.at("expiresAt").get<std::string>();
        loaded.accepted_at = document.at("acceptedAt").get<std::string>();
        loaded.payload_sha256 = document.at("payloadSha256").get<std::string>();
        loaded.index_json = document.at("index").dump();
        record = std::move(loaded);
        return true;
    } catch (const std::exception& exception) {
        message = exception.what();
        return false;
    }
}

bool PersistCacheRecord(
    const std::filesystem::path& path,
    const RepositorySourceConfiguration& source,
    const ValidatedIndex& validated,
    std::string_view accepted_at,
    std::string_view index_json,
    std::string& message) {
    try {
        Json document{
            {"schemaVersion", kRepositoryCacheSchemaVersion},
            {"sourceId", source.id},
            {"repositoryId", validated.index.repository_id},
            {"sequence", validated.index.sequence},
            {"generatedAt", validated.index.generated_at},
            {"expiresAt", validated.index.expires_at},
            {"acceptedAt", accepted_at},
            {"payloadSha256", validated.payload_sha256},
            {"index", Json::parse(index_json)}};
        CacheValidator().validate(document);

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            message = "repository cache directory could not be created";
            return false;
        }
        const std::filesystem::path temporary = path.wstring() + L".tmp-" +
            std::to_wstring(GetCurrentProcessId());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << document.dump() << '\n';
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            message = "repository cache state could not be written";
            return false;
        }
        output.close();
        if (MoveFileExW(
                temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            std::filesystem::remove(temporary, error);
            message = "repository cache state could not be published";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        message = exception.what();
        return false;
    }
}

RepositoryPolicyResult Success(
    ValidatedIndex validated,
    RepositoryIndexFreshness freshness,
    bool installs_allowed,
    std::string accepted_at) {
    RepositoryPolicyResult result;
    result.accepted = RepositoryAcceptedIndex{
        std::move(validated.index), freshness, installs_allowed,
        std::move(accepted_at), std::move(validated.payload_sha256)};
    return result;
}

}  // namespace

RepositoryIndexPolicy::RepositoryIndexPolicy(RepositoryIndexPolicyOptions options)
    : options_(std::move(options)) {}

std::filesystem::path RepositoryIndexPolicy::CachePath(
    const RepositorySourceConfiguration& source) const {
    std::wstring filename(source.id.begin(), source.id.end());
    filename += L".json";
    return options_.state_directory / filename;
}

RepositoryPolicyResult RepositoryIndexPolicy::AcceptOnline(
    const RepositorySourceConfiguration& source,
    const RepositoryConfiguration& configuration,
    const RepositoryTrustStore& trust,
    std::string_view index_json,
    Clock::time_point now) const {
    ValidatedIndex validated;
    if (auto checked = ValidateIndex(
            source, configuration, trust, index_json, now, false, validated);
        checked.error != RepositoryPolicyError::None) {
        return checked;
    }

    std::optional<CacheRecord> previous;
    std::string state_error;
    if (!LoadCacheRecord(CachePath(source), previous, state_error)) {
        return Failure(RepositoryPolicyError::CacheInvalid, std::move(state_error));
    }

    std::string accepted_at = FormatRepositoryUtcTimestamp(now);
    if (previous) {
        if (previous->source_id != source.id ||
            previous->repository_id != validated.index.repository_id) {
            return Failure(
                RepositoryPolicyError::CacheInvalid,
                "repository cache identity does not match its configured source");
        }
        Clock::time_point previous_generated_at;
        Clock::time_point previous_accepted_at;
        if (!ParseRepositoryUtcTimestamp(previous->generated_at, previous_generated_at) ||
            !ParseRepositoryUtcTimestamp(previous->accepted_at, previous_accepted_at)) {
            return Failure(
                RepositoryPolicyError::CacheInvalid,
                "repository cache monotonic state contains an invalid timestamp");
        }
        const auto clock_skew =
            std::chrono::seconds{configuration.freshness.maximum_clock_skew_seconds};
        if (previous_accepted_at > now + clock_skew ||
            previous_accepted_at + clock_skew < previous_generated_at) {
            return Failure(
                RepositoryPolicyError::CacheInvalid,
                "repository cache monotonic state has inconsistent timestamps");
        }
        if (validated.index.sequence < previous->sequence) {
            return Failure(RepositoryPolicyError::Replay, "repository index sequence regressed");
        }
        if (validated.index.sequence == previous->sequence &&
            validated.payload_sha256 != previous->payload_sha256) {
            return Failure(
                RepositoryPolicyError::Equivocation,
                "repository index changed without advancing its sequence");
        }
        if (validated.index.sequence > previous->sequence &&
            validated.generated_at < previous_generated_at) {
            return Failure(
                RepositoryPolicyError::Downgrade,
                "repository index signing time regressed while its sequence advanced");
        }
        if (validated.index.sequence == previous->sequence) {
            accepted_at = previous->accepted_at;
        }
    }

    if (!PersistCacheRecord(
            CachePath(source), source, validated, accepted_at, index_json, state_error)) {
        return Failure(RepositoryPolicyError::IoFailure, std::move(state_error));
    }
    return Success(
        std::move(validated), RepositoryIndexFreshness::NetworkFresh, true,
        std::move(accepted_at));
}

RepositoryPolicyResult RepositoryIndexPolicy::LoadCache(
    const RepositorySourceConfiguration& source,
    const RepositoryConfiguration& configuration,
    const RepositoryTrustStore& trust,
    Clock::time_point now) const {
    std::optional<CacheRecord> record;
    std::string state_error;
    if (!LoadCacheRecord(CachePath(source), record, state_error)) {
        return Failure(RepositoryPolicyError::CacheInvalid, std::move(state_error));
    }
    if (!record) {
        return Failure(RepositoryPolicyError::CacheMissing, "repository cache is not available");
    }

    ValidatedIndex validated;
    if (auto checked = ValidateIndex(
            source, configuration, trust, record->index_json, now, true, validated);
        checked.error != RepositoryPolicyError::None) {
        return checked;
    }
    if (record->source_id != source.id ||
        record->repository_id != validated.index.repository_id ||
        record->sequence != validated.index.sequence ||
        record->generated_at != validated.index.generated_at ||
        record->expires_at != validated.index.expires_at ||
        record->payload_sha256 != validated.payload_sha256) {
        return Failure(
            RepositoryPolicyError::CacheInvalid,
            "repository cache metadata does not match its signed index");
    }

    Clock::time_point accepted_at;
    if (!ParseRepositoryUtcTimestamp(record->accepted_at, accepted_at)) {
        return Failure(RepositoryPolicyError::CacheInvalid, "repository cache acceptance time is invalid");
    }
    const auto clock_skew =
        std::chrono::seconds{configuration.freshness.maximum_clock_skew_seconds};
    if (accepted_at > now + clock_skew || accepted_at + clock_skew < validated.generated_at) {
        return Failure(
            RepositoryPolicyError::CacheInvalid,
            "repository cache acceptance time is inconsistent with its signed index");
    }
    if (now <= validated.expires_at + clock_skew) {
        return Success(
            std::move(validated), RepositoryIndexFreshness::CacheFresh, true,
            std::move(record->accepted_at));
    }

    const auto offline_age =
        std::chrono::seconds{configuration.freshness.maximum_offline_age_seconds};
    if (offline_age == std::chrono::seconds::zero() ||
        now > accepted_at + offline_age + clock_skew) {
        return Failure(
            RepositoryPolicyError::CacheExpired,
            "repository cache exceeded the configured offline window");
    }
    return Success(
        std::move(validated), RepositoryIndexFreshness::CacheStale, false,
        std::move(record->accepted_at));
}

std::string_view RepositoryIndexFreshnessName(
    RepositoryIndexFreshness freshness) noexcept {
    switch (freshness) {
    case RepositoryIndexFreshness::NetworkFresh: return "network-fresh";
    case RepositoryIndexFreshness::CacheFresh: return "cache-fresh";
    case RepositoryIndexFreshness::CacheStale: return "cache-stale";
    }
    return "network-fresh";
}

std::string_view RepositoryPolicyErrorName(RepositoryPolicyError error) noexcept {
    switch (error) {
    case RepositoryPolicyError::None: return "none";
    case RepositoryPolicyError::InvalidIndex: return "invalid-index";
    case RepositoryPolicyError::RepositoryMismatch: return "repository-mismatch";
    case RepositoryPolicyError::UntrustedSignature: return "untrusted-signature";
    case RepositoryPolicyError::InvalidTimestamp: return "invalid-timestamp";
    case RepositoryPolicyError::FutureIndex: return "future-index";
    case RepositoryPolicyError::ExpiredIndex: return "expired-index";
    case RepositoryPolicyError::LifetimeExceeded: return "lifetime-exceeded";
    case RepositoryPolicyError::Replay: return "replay";
    case RepositoryPolicyError::Equivocation: return "equivocation";
    case RepositoryPolicyError::Downgrade: return "downgrade";
    case RepositoryPolicyError::InvalidArtifact: return "invalid-artifact";
    case RepositoryPolicyError::CacheMissing: return "cache-missing";
    case RepositoryPolicyError::CacheInvalid: return "cache-invalid";
    case RepositoryPolicyError::CacheExpired: return "cache-expired";
    case RepositoryPolicyError::IoFailure: return "io-failure";
    }
    return "invalid-index";
}

}  // namespace anomaly
