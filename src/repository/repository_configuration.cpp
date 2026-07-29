#include "anomaly/repository_configuration.hpp"

#include "repository_config_schema.hpp"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <utility>

namespace anomaly {
namespace {

using Json = nlohmann::json;

const nlohmann::json_schema::json_validator& ConfigurationValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        Json schema = Json::parse(
            detail::kRepositoryConfigurationSchemaJson.begin(),
            detail::kRepositoryConfigurationSchemaJson.end());
        return nlohmann::json_schema::json_validator(std::move(schema));
    }();
    return validator;
}

RepositoryChannel ParseChannel(const std::string_view value) noexcept {
    if (value == "preview") return RepositoryChannel::Preview;
    if (value == "nightly") return RepositoryChannel::Nightly;
    return RepositoryChannel::Stable;
}

bool IsFileUri(const std::string_view value) noexcept {
    return value.starts_with("file://");
}

bool ValidateSourceUris(
    const RepositoryConfiguration& configuration,
    const RepositorySourceConfiguration& source,
    RepositoryConfigurationResult& result,
    const std::size_t source_index) {
    const std::string path = "/sources/" + std::to_string(source_index);
    if (!configuration.allow_file_sources && IsFileUri(source.index_uri)) {
        result.diagnostics.push_back(
            {path + "/indexUri", "file repository sources are disabled"});
        return false;
    }
    if (std::ranges::find(source.mirrors, source.index_uri) != source.mirrors.end()) {
        result.diagnostics.push_back(
            {path + "/mirrors", "primary repository URI cannot also be a mirror"});
        return false;
    }
    if (!configuration.allow_file_sources &&
        std::ranges::any_of(source.mirrors, IsFileUri)) {
        result.diagnostics.push_back(
            {path + "/mirrors", "file repository mirrors are disabled"});
        return false;
    }
    if (source.mirror_policy == RepositoryMirrorPolicy::PrimaryOnly &&
        !source.mirrors.empty()) {
        result.diagnostics.push_back(
            {path + "/mirrorPolicy", "primary-only source cannot declare mirrors"});
        return false;
    }
    return true;
}

}  // namespace

std::string_view RepositoryMirrorPolicyName(RepositoryMirrorPolicy policy) noexcept {
    return policy == RepositoryMirrorPolicy::OrderedFallback
        ? "ordered-fallback" : "primary-only";
}

std::string_view RepositoryWithdrawalPolicyName(
    RepositoryWithdrawalPolicy) noexcept {
    return "block-new";
}

std::string_view RepositoryDowngradePolicyName(
    RepositoryDowngradePolicy) noexcept {
    return "reject";
}

RepositoryConfigurationResult ParseRepositoryConfiguration(std::string_view json) {
    RepositoryConfigurationResult result;
    if (json.size() > kMaximumRepositoryConfigurationBytes) {
        result.diagnostics.push_back(
            {"", "repository configuration exceeds maximum size"});
        return result;
    }
    try {
        const Json document = Json::parse(json.begin(), json.end());
        ConfigurationValidator().validate(document);

        RepositoryConfiguration configuration;
        configuration.schema_version = document.at("schemaVersion").get<std::uint32_t>();
        configuration.enabled = document.at("enabled").get<bool>();
        configuration.allow_file_sources = document.at("allowFileSources").get<bool>();
        const Json& freshness = document.at("freshness");
        configuration.freshness.maximum_clock_skew_seconds =
            freshness.at("maximumClockSkewSeconds").get<std::uint32_t>();
        configuration.freshness.maximum_index_age_seconds =
            freshness.at("maximumIndexAgeSeconds").get<std::uint32_t>();
        configuration.freshness.maximum_offline_age_seconds =
            freshness.at("maximumOfflineAgeSeconds").get<std::uint32_t>();

        std::set<std::string, std::less<>> key_ids;
        for (const Json& value : document.at("trustKeys")) {
            RepositoryTrustKeyConfiguration key;
            key.key_id = value.at("keyId").get<std::string>();
            key.algorithm = value.at("algorithm").get<std::string>();
            key.public_key_hex = value.at("publicKey").get<std::string>();
            key.valid_from = value.at("validFrom").get<std::string>();
            key.valid_until = value.at("validUntil").get<std::string>();
            const std::size_t key_index = configuration.trust_keys.size();
            if (!key_ids.insert(key.key_id).second) {
                result.diagnostics.push_back(
                    {"/trustKeys/" + std::to_string(key_index) + "/keyId",
                     "duplicate repository trust key ID"});
                return result;
            }
            std::chrono::system_clock::time_point valid_from;
            std::chrono::system_clock::time_point valid_until;
            if (!ParseRepositoryUtcTimestamp(key.valid_from, valid_from) ||
                !ParseRepositoryUtcTimestamp(key.valid_until, valid_until) ||
                valid_from >= valid_until) {
                result.diagnostics.push_back(
                    {"/trustKeys/" + std::to_string(key_index),
                     "repository trust key validity interval is empty or reversed"});
                return result;
            }
            configuration.trust_keys.push_back(std::move(key));
        }

        std::set<std::string, std::less<>> source_ids;
        for (const Json& value : document.at("sources")) {
            RepositorySourceConfiguration source;
            source.id = value.at("id").get<std::string>();
            source.index_uri = value.at("indexUri").get<std::string>();
            source.channel = ParseChannel(value.at("channel").get<std::string>());
            source.key_ids = value.at("keyIds").get<std::vector<std::string>>();
            source.mirrors = value.at("mirrors").get<std::vector<std::string>>();
            source.mirror_policy = value.at("mirrorPolicy") == "ordered-fallback"
                ? RepositoryMirrorPolicy::OrderedFallback
                : RepositoryMirrorPolicy::PrimaryOnly;
            const std::size_t source_index = configuration.sources.size();
            if (!source_ids.insert(source.id).second) {
                result.diagnostics.push_back(
                    {"/sources/" + std::to_string(source_index) + "/id",
                     "duplicate repository source ID"});
                return result;
            }
            if (!ValidateSourceUris(configuration, source, result, source_index)) {
                return result;
            }
            for (const std::string& key_id : source.key_ids) {
                if (!key_ids.contains(key_id)) {
                    result.diagnostics.push_back(
                        {"/sources/" + std::to_string(source_index) + "/keyIds",
                         "repository source references an unknown trust key"});
                    return result;
                }
            }
            configuration.sources.push_back(std::move(source));
        }

        for (const auto& key : configuration.trust_keys) {
            if (!result.trust_store.AddEcdsaP256Key(key.key_id, key.public_key_hex)) {
                result.diagnostics.push_back(
                    {"/trustKeys", "repository trust key could not be loaded"});
                return result;
            }
        }
        result.configuration = std::move(configuration);
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"", error.what()});
    }
    return result;
}

RepositoryConfigurationResult LoadRepositoryConfiguration(
    const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        RepositoryConfigurationResult result;
        result.diagnostics.push_back({"", "repository configuration is unreadable"});
        return result;
    }
    if (size > kMaximumRepositoryConfigurationBytes) {
        RepositoryConfigurationResult result;
        result.diagnostics.push_back(
            {"", "repository configuration exceeds maximum size"});
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        RepositoryConfigurationResult result;
        result.diagnostics.push_back({"", "repository configuration is unreadable"});
        return result;
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input && !contents.empty()) {
        RepositoryConfigurationResult result;
        result.diagnostics.push_back({"", "repository configuration could not be read"});
        return result;
    }
    return ParseRepositoryConfiguration(contents);
}

std::string_view RepositoryConfigurationSchemaJson() noexcept {
    return detail::kRepositoryConfigurationSchemaJson;
}

}  // namespace anomaly
