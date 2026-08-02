#include "anomaly/artifact_crypto.hpp"
#include "anomaly/repository_policy.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::string Hex(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = alphabet[bytes[index] >> 4];
        result[index * 2 + 1] = alphabet[bytes[index] & 15];
    }
    return result;
}

class SigningKey final {
public:
    SigningKey() {
        BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        BCryptGenerateKeyPair(algorithm_, &key_, 256, 0);
        BCryptFinalizeKeyPair(key_, 0);
        DWORD size{};
        BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &size, 0);
        std::vector<std::uint8_t> blob(size);
        BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), size, &size, 0);
        public_key_ = Hex(std::span(blob).subspan(sizeof(BCRYPT_ECCKEY_BLOB)));
    }

    ~SigningKey() {
        if (key_ != nullptr) BCryptDestroyKey(key_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    [[nodiscard]] const std::string& PublicKey() const noexcept { return public_key_; }

    [[nodiscard]] std::string Sign(std::string_view payload) const {
        const std::string digest_hex = anomaly::Sha256Hex({
            reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
        std::vector<std::uint8_t> digest;
        if (!anomaly::DecodeHex(digest_hex, digest)) return {};
        DWORD size{};
        BCryptSignHash(
            key_, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
            nullptr, 0, &size, 0);
        std::vector<std::uint8_t> signature(size);
        BCryptSignHash(
            key_, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
            signature.data(), size, &size, 0);
        signature.resize(size);
        return Hex(signature);
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_KEY_HANDLE key_{};
    std::string public_key_;
};

std::chrono::system_clock::time_point At(std::string_view value) {
    std::chrono::system_clock::time_point result;
    if (!anomaly::ParseRepositoryUtcTimestamp(value, result)) {
        std::cerr << "invalid fixture timestamp\n";
    }
    return result;
}

std::string IndexJson(
    std::uint64_t sequence,
    std::string generated_at,
    std::string expires_at,
    const SigningKey& key,
    std::string repository_id = "dev.anomaly.repository") {
    anomaly::RepositoryIndex index;
    index.repository_id = std::move(repository_id);
    index.sequence = sequence;
    index.generated_at = std::move(generated_at);
    index.expires_at = std::move(expires_at);
    index.signature = {"ecdsa-p256-sha256", "test", {}};
    index.signature.value_hex = key.Sign(anomaly::CanonicalRepositoryIndexPayload(index));
    return nlohmann::json{
        {"schemaVersion", 1},
        {"repositoryId", index.repository_id},
        {"sequence", index.sequence},
        {"generatedAt", index.generated_at},
        {"expiresAt", index.expires_at},
        {"artifacts", nlohmann::json::array()},
        {"signature", {
            {"algorithm", index.signature.algorithm},
            {"keyId", index.signature.key_id},
            {"value", index.signature.value_hex}}}}
        .dump();
}

}  // namespace

int main() {
    bool result = true;
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-repository-policy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));

    SigningKey key;
    anomaly::RepositoryConfiguration configuration;
    configuration.enabled = true;
    configuration.freshness.maximum_clock_skew_seconds = 300;
    configuration.freshness.maximum_index_age_seconds = 3600;
    configuration.freshness.maximum_offline_age_seconds = 7200;
    configuration.trust_keys.push_back({
        "test", "ecdsa-p256-sha256", key.PublicKey(),
        "2025-01-01T00:00:00Z", "2028-01-01T00:00:00Z"});
    anomaly::RepositorySourceConfiguration source;
    source.id = "dev.anomaly.repository";
    source.index_uri = "https://repo.anomaly.dev/index.json";
    source.key_ids.push_back("test");
    configuration.sources.push_back(source);
    anomaly::RepositoryTrustStore trust;
    result = Check(trust.AddEcdsaP256Key("test", key.PublicKey()),
                   "fixture trust key was rejected") && result;

    anomaly::RepositoryIndexPolicy policy({root});
    const std::string first = IndexJson(
        10, "2026-07-26T12:00:00Z", "2026-07-26T13:00:00Z", key);
    const auto accepted = policy.AcceptOnline(
        source, configuration, trust, first, At("2026-07-26T12:05:00Z"));
    if (!accepted.Ok()) {
        std::cerr << anomaly::RepositoryPolicyErrorName(accepted.error) << ": "
                  << accepted.message << '\n';
    }
    result = Check(
        accepted.Ok() && accepted.accepted->installs_allowed &&
            accepted.accepted->freshness == anomaly::RepositoryIndexFreshness::NetworkFresh &&
            accepted.accepted->accepted_at == "2026-07-26T12:05:00Z" &&
            std::filesystem::exists(policy.CachePath(source)),
        "fresh repository index was not accepted and cached") && result;

    const auto idempotent = policy.AcceptOnline(
        source, configuration, trust, first, At("2026-07-26T12:10:00Z"));
    result = Check(
        idempotent.Ok() && idempotent.accepted->accepted_at == "2026-07-26T12:05:00Z",
        "same-sequence refresh extended the offline acceptance window") && result;

    const auto replay = policy.AcceptOnline(
        source, configuration, trust,
        IndexJson(9, "2026-07-26T12:01:00Z", "2026-07-26T13:01:00Z", key),
        At("2026-07-26T12:10:00Z"));
    result = Check(replay.error == anomaly::RepositoryPolicyError::Replay,
                   "lower repository sequence was not rejected as replay") && result;

    auto wrong_source = source;
    wrong_source.id = "dev.anomaly.other";
    const auto mismatched = policy.AcceptOnline(
        wrong_source, configuration, trust, first, At("2026-07-26T12:10:00Z"));
    result = Check(mismatched.error == anomaly::RepositoryPolicyError::RepositoryMismatch,
                   "repository index was accepted for a different source identity") && result;

    auto wrong_key = source;
    wrong_key.key_ids = {"other"};
    const auto unauthorized = policy.AcceptOnline(
        wrong_key, configuration, trust, first, At("2026-07-26T12:10:00Z"));
    result = Check(unauthorized.error == anomaly::RepositoryPolicyError::UntrustedSignature,
                   "repository index bypassed the source key allowlist") && result;

    const auto equivocation = policy.AcceptOnline(
        source, configuration, trust,
        IndexJson(10, "2026-07-26T12:01:00Z", "2026-07-26T13:01:00Z", key),
        At("2026-07-26T12:10:00Z"));
    result = Check(equivocation.error == anomaly::RepositoryPolicyError::Equivocation,
                   "same repository sequence accepted different signed content") && result;

    const auto downgrade = policy.AcceptOnline(
        source, configuration, trust,
        IndexJson(11, "2026-07-26T11:59:00Z", "2026-07-26T12:30:00Z", key),
        At("2026-07-26T12:10:00Z"));
    result = Check(downgrade.error == anomaly::RepositoryPolicyError::Downgrade,
                   "repository signing-time downgrade was accepted") && result;

    const auto future = policy.AcceptOnline(
        source, configuration, trust,
        IndexJson(11, "2026-07-26T12:16:00Z", "2026-07-26T13:16:00Z", key),
        At("2026-07-26T12:10:00Z"));
    result = Check(future.error == anomaly::RepositoryPolicyError::FutureIndex,
                   "repository index beyond clock skew was accepted") && result;

    const auto lifetime = policy.AcceptOnline(
        source, configuration, trust,
        IndexJson(11, "2026-07-26T12:10:00Z", "2026-07-26T14:10:00Z", key),
        At("2026-07-26T12:10:00Z"));
    result = Check(lifetime.error == anomaly::RepositoryPolicyError::LifetimeExceeded,
                   "repository index exceeded the configured validity window") && result;

    const auto expired = policy.AcceptOnline(
        source, configuration, trust,
        IndexJson(11, "2026-07-26T10:00:00Z", "2026-07-26T10:30:00Z", key),
        At("2026-07-26T12:10:00Z"));
    result = Check(expired.error == anomaly::RepositoryPolicyError::ExpiredIndex,
                   "expired online repository index was accepted") && result;

    const std::string second = IndexJson(
        11, "2026-07-26T12:10:00Z", "2026-07-26T13:10:00Z", key);
    result = Check(
        policy.AcceptOnline(
            source, configuration, trust, second, At("2026-07-26T12:10:00Z")).Ok(),
        "next monotonic repository index was rejected") && result;

    const auto fresh_cache = policy.LoadCache(
        source, configuration, trust, At("2026-07-26T12:20:00Z"));
    result = Check(
        fresh_cache.Ok() && fresh_cache.accepted->installs_allowed &&
            fresh_cache.accepted->freshness == anomaly::RepositoryIndexFreshness::CacheFresh,
        "unexpired offline repository cache was not usable") && result;

    const auto stale_cache = policy.LoadCache(
        source, configuration, trust, At("2026-07-26T13:20:00Z"));
    result = Check(
        stale_cache.Ok() && !stale_cache.accepted->installs_allowed &&
            stale_cache.accepted->freshness == anomaly::RepositoryIndexFreshness::CacheStale,
        "stale repository cache did not become browse-only") && result;

    const auto dead_cache = policy.LoadCache(
        source, configuration, trust, At("2026-07-26T14:20:01Z"));
    result = Check(dead_cache.error == anomaly::RepositoryPolicyError::CacheExpired,
                   "repository cache exceeded its offline window") && result;

    std::ofstream(policy.CachePath(source), std::ios::binary | std::ios::trunc) << "{}";
    const auto corrupted = policy.LoadCache(
        source, configuration, trust, At("2026-07-26T12:20:00Z"));
    result = Check(corrupted.error == anomaly::RepositoryPolicyError::CacheInvalid,
                   "corrupt repository cache state was accepted") && result;

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result ? 0 : 2;
}
