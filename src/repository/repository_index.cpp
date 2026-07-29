#include "anomaly/repository_index.hpp"

#include "anomaly/artifact_crypto.hpp"
#include "repository_index_schema.hpp"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <set>

namespace anomaly {
namespace {

using Json = nlohmann::json;

const nlohmann::json_schema::json_validator& IndexValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        Json schema = Json::parse(
            detail::kRepositoryIndexSchemaJson.begin(),
            detail::kRepositoryIndexSchemaJson.end());
        return nlohmann::json_schema::json_validator(std::move(schema));
    }();
    return validator;
}

RepositorySignature ParseSignature(const Json& document) {
    return {
        document.at("algorithm").get<std::string>(),
        document.at("keyId").get<std::string>(),
        document.at("value").get<std::string>()};
}

Json SignatureJson(const RepositorySignature& signature) {
    return Json{{"algorithm", signature.algorithm}, {"keyId", signature.key_id},
                {"value", signature.value_hex}};
}

Json ArtifactJson(const RepositoryArtifact& artifact, bool include_signature) {
    Json value{
        {"kind", RepositoryArtifactKindName(artifact.kind)},
        {"id", artifact.id},
        {"version", artifact.version.ToString()},
        {"channel", RepositoryChannelName(artifact.channel)},
        {"uri", artifact.uri},
        {"size", artifact.size},
        {"sha256", artifact.sha256},
        {"manifestSha256", artifact.manifest_sha256},
        {"publishedAt", artifact.published_at},
        {"withdrawn", artifact.withdrawn}};
    if (!artifact.minimum_runtime.empty()) value["minimumRuntime"] = artifact.minimum_runtime;
    if (artifact.minimum_api != 0) value["minimumApi"] = artifact.minimum_api;
    if (!artifact.withdrawn_at.empty()) value["withdrawnAt"] = artifact.withdrawn_at;
    if (!artifact.withdrawal_reason.empty()) {
        value["withdrawalReason"] = artifact.withdrawal_reason;
    }
    if (include_signature) value["signature"] = SignatureJson(artifact.signature);
    return value;
}

std::span<const std::byte> AsBytes(const std::string& value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

bool ParseDecimal(std::string_view value, int& parsed) noexcept {
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

}  // namespace

std::string_view RepositoryArtifactKindName(RepositoryArtifactKind kind) noexcept {
    switch (kind) {
    case RepositoryArtifactKind::Plugin: return "plugin";
    case RepositoryArtifactKind::NteProfile: return "nte-profile";
    }
    return "plugin";
}

std::string_view RepositoryChannelName(RepositoryChannel channel) noexcept {
    switch (channel) {
    case RepositoryChannel::Stable: return "stable";
    case RepositoryChannel::Preview: return "preview";
    case RepositoryChannel::Nightly: return "nightly";
    }
    return "stable";
}

bool RepositoryTrustStore::AddEcdsaP256Key(
    std::string key_id, std::string public_key_hex) {
    if (key_id.empty() || !ValidateEcdsaP256PublicKey(public_key_hex)) {
        return false;
    }
    return keys_.emplace(std::move(key_id), std::move(public_key_hex)).second;
}

std::string_view RepositoryTrustStore::Find(std::string_view key_id) const noexcept {
    const auto found = keys_.find(key_id);
    return found == keys_.end() ? std::string_view{} : std::string_view(found->second);
}

RepositoryIndexParseResult ParseRepositoryIndex(std::string_view json) {
    RepositoryIndexParseResult result;
    if (json.size() > kMaximumRepositoryIndexBytes) {
        result.diagnostics.push_back({"", "repository index exceeds maximum size"});
        return result;
    }
    try {
        const Json document = Json::parse(json.begin(), json.end());
        IndexValidator().validate(document);
        RepositoryIndex index;
        index.schema_version = document.at("schemaVersion").get<std::uint32_t>();
        index.repository_id = document.at("repositoryId").get<std::string>();
        index.sequence = document.at("sequence").get<std::uint64_t>();
        index.generated_at = document.at("generatedAt").get<std::string>();
        index.expires_at = document.at("expiresAt").get<std::string>();
        std::chrono::system_clock::time_point generated_at;
        std::chrono::system_clock::time_point expires_at;
        if (!ParseRepositoryUtcTimestamp(index.generated_at, generated_at) ||
            !ParseRepositoryUtcTimestamp(index.expires_at, expires_at) ||
            generated_at >= expires_at) {
            result.diagnostics.push_back(
                {"/expiresAt", "repository index validity interval is invalid"});
            return result;
        }
        index.signature = ParseSignature(document.at("signature"));
        std::set<std::string> identities;
        for (const Json& item : document.at("artifacts")) {
            RepositoryArtifact artifact;
            const std::string kind = item.at("kind").get<std::string>();
            artifact.kind = kind == "plugin" ? RepositoryArtifactKind::Plugin
                                               : RepositoryArtifactKind::NteProfile;
            artifact.id = item.at("id").get<std::string>();
            SemVerParseError version_error;
            auto version = ParseSemanticVersion(item.at("version").get<std::string>(), &version_error);
            if (!version) {
                result.diagnostics.push_back({"/artifacts/version", version_error.message});
                return result;
            }
            artifact.version = std::move(*version);
            const std::string channel = item.at("channel").get<std::string>();
            artifact.channel = channel == "stable" ? RepositoryChannel::Stable
                : channel == "preview" ? RepositoryChannel::Preview
                : RepositoryChannel::Nightly;
            artifact.uri = item.at("uri").get<std::string>();
            artifact.size = item.at("size").get<std::uint64_t>();
            artifact.sha256 = item.at("sha256").get<std::string>();
            artifact.manifest_sha256 = item.at("manifestSha256").get<std::string>();
            artifact.published_at = item.at("publishedAt").get<std::string>();
            artifact.minimum_runtime = item.value("minimumRuntime", std::string{});
            artifact.minimum_api = item.value("minimumApi", 0U);
            artifact.withdrawn = item.at("withdrawn").get<bool>();
            artifact.withdrawn_at = item.value("withdrawnAt", std::string{});
            artifact.withdrawal_reason = item.value("withdrawalReason", std::string{});
            artifact.signature = ParseSignature(item.at("signature"));
            const std::string identity = std::string(RepositoryArtifactKindName(artifact.kind)) +
                "\n" + artifact.id + "\n" + artifact.version.ToString() + "\n" + channel;
            if (!identities.insert(identity).second) {
                result.diagnostics.push_back({"/artifacts", "duplicate artifact identity"});
                return result;
            }
            index.artifacts.push_back(std::move(artifact));
        }
        result.index = std::move(index);
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"", error.what()});
    }
    return result;
}

std::string_view RepositoryIndexSchemaJson() noexcept {
    return detail::kRepositoryIndexSchemaJson;
}

std::string CanonicalRepositoryArtifactPayload(const RepositoryArtifact& artifact) {
    return ArtifactJson(artifact, false).dump();
}

std::string CanonicalRepositoryIndexPayload(const RepositoryIndex& index) {
    Json document{{"schemaVersion", index.schema_version},
                  {"repositoryId", index.repository_id},
                  {"sequence", index.sequence},
                  {"generatedAt", index.generated_at},
                  {"expiresAt", index.expires_at}};
    document["artifacts"] = Json::array();
    for (const auto& artifact : index.artifacts) {
        document["artifacts"].push_back(ArtifactJson(artifact, true));
    }
    return document.dump();
}

bool VerifyRepositoryArtifactSignature(
    const RepositoryArtifact& artifact, const RepositoryTrustStore& trust) noexcept {
    if (artifact.signature.algorithm != "ecdsa-p256-sha256") return false;
    const std::string_view key = trust.Find(artifact.signature.key_id);
    if (key.empty()) return false;
    try {
        const std::string payload = CanonicalRepositoryArtifactPayload(artifact);
        return VerifyEcdsaP256Sha256(AsBytes(payload), key, artifact.signature.value_hex);
    } catch (...) {
        return false;
    }
}

bool VerifyRepositoryIndexSignature(
    const RepositoryIndex& index, const RepositoryTrustStore& trust) noexcept {
    if (index.signature.algorithm != "ecdsa-p256-sha256") return false;
    const std::string_view key = trust.Find(index.signature.key_id);
    if (key.empty()) return false;
    try {
        const std::string payload = CanonicalRepositoryIndexPayload(index);
        return VerifyEcdsaP256Sha256(AsBytes(payload), key, index.signature.value_hex);
    } catch (...) {
        return false;
    }
}

bool ParseRepositoryUtcTimestamp(
    std::string_view value,
    std::chrono::system_clock::time_point& timestamp) noexcept {
    if (value.size() < 20 || value.size() > 30 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
        return false;
    }
    int year_value{};
    int month_value{};
    int day_value{};
    int hour_value{};
    int minute_value{};
    int second_value{};
    if (!ParseDecimal(value.substr(0, 4), year_value) ||
        !ParseDecimal(value.substr(5, 2), month_value) ||
        !ParseDecimal(value.substr(8, 2), day_value) ||
        !ParseDecimal(value.substr(11, 2), hour_value) ||
        !ParseDecimal(value.substr(14, 2), minute_value) ||
        !ParseDecimal(value.substr(17, 2), second_value) ||
        hour_value > 23 || minute_value > 59 || second_value > 59) {
        return false;
    }
    const std::chrono::year_month_day date{
        std::chrono::year{year_value}, std::chrono::month{static_cast<unsigned>(month_value)},
        std::chrono::day{static_cast<unsigned>(day_value)}};
    if (!date.ok()) return false;

    std::chrono::nanoseconds fraction{};
    if (value.size() != 20) {
        if (value[19] != '.' || value.size() < 22 || value.size() > 30) return false;
        const std::string_view digits = value.substr(20, value.size() - 21);
        std::uint32_t fractional_value{};
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), fractional_value);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return false;
        for (std::size_t index = digits.size(); index < 9; ++index) fractional_value *= 10;
        fraction = std::chrono::nanoseconds{fractional_value};
    }
    const auto seconds = std::chrono::sys_days{date} + std::chrono::hours{hour_value} +
        std::chrono::minutes{minute_value} + std::chrono::seconds{second_value};
    timestamp = std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            seconds.time_since_epoch() + fraction)};
    return true;
}

std::string FormatRepositoryUtcTimestamp(
    std::chrono::system_clock::time_point timestamp) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
    const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(seconds)};
    const std::chrono::hh_mm_ss time{seconds - std::chrono::floor<std::chrono::days>(seconds)};
    char buffer[32]{};
    std::snprintf(
        buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02uZ",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()), static_cast<unsigned>(time.hours().count()),
        static_cast<unsigned>(time.minutes().count()),
        static_cast<unsigned>(time.seconds().count()));
    return buffer;
}

}  // namespace anomaly
