#include "anomaly/repository_configuration.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

constexpr std::string_view kPublicKey =
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::string EnabledConfiguration(
    std::string_view source_key = "anomaly-official-2026",
    std::string_view index_uri = "https://repo.anomaly.dev/v1/index.json",
    std::string_view mirror_policy = "ordered-fallback",
    std::string_view mirrors = R"(["https://mirror.anomaly.dev/v1/index.json"])") {
    return std::string(R"({
  "schemaVersion": 1,
  "enabled": true,
  "allowFileSources": false,
  "withdrawalPolicy": "block-new",
  "freshness": {
    "maximumClockSkewSeconds": 300,
    "maximumIndexAgeSeconds": 86400,
    "maximumOfflineAgeSeconds": 604800,
    "downgradePolicy": "reject"
  },
  "sources": [{
    "id": "dev.anomaly.official",
    "indexUri": ")") + std::string(index_uri) + R"(",
    "channel": "stable",
    "keyIds": [")" + std::string(source_key) + R"("],
    "mirrors": )" + std::string(mirrors) + R"(,
    "mirrorPolicy": ")" + std::string(mirror_policy) + R"("
  }],
  "trustKeys": [{
    "keyId": "anomaly-official-2026",
    "algorithm": "ecdsa-p256-sha256",
    "publicKey": ")" + std::string(kPublicKey) + R"(",
    "validFrom": "2026-01-01T00:00:00Z",
    "validUntil": "2027-01-01T00:00:00Z"
  }]
})";
}

}  // namespace

int main() {
    bool result = true;

    const auto disabled = anomaly::ParseRepositoryConfiguration(R"({
      "schemaVersion":1,
      "enabled":false,
      "allowFileSources":false,
      "withdrawalPolicy":"block-new",
      "freshness":{
        "maximumClockSkewSeconds":300,
        "maximumIndexAgeSeconds":86400,
        "maximumOfflineAgeSeconds":604800,
        "downgradePolicy":"reject"
      },
      "sources":[],
      "trustKeys":[]
    })");
    result = Check(disabled.Ok() && !disabled.configuration->enabled &&
                       disabled.trust_store.Empty(),
                   "explicit disabled repository configuration was rejected") && result;

    const auto enabled = anomaly::ParseRepositoryConfiguration(EnabledConfiguration());
    result = Check(enabled.Ok() && enabled.configuration->enabled &&
                       enabled.configuration->sources.size() == 1 &&
                       enabled.configuration->sources.front().channel ==
                           anomaly::RepositoryChannel::Stable &&
                       enabled.configuration->sources.front().mirror_policy ==
                           anomaly::RepositoryMirrorPolicy::OrderedFallback &&
                       enabled.configuration->freshness.maximum_clock_skew_seconds == 300 &&
                       enabled.configuration->freshness.maximum_index_age_seconds == 86400 &&
                       enabled.configuration->freshness.maximum_offline_age_seconds == 604800 &&
                       enabled.trust_store.Size() == 1 &&
                       !enabled.trust_store.Find("anomaly-official-2026").empty(),
                   "enabled repository configuration did not build pinned trust") && result;

    result = Check(
        !anomaly::ParseRepositoryConfiguration(EnabledConfiguration("unknown-key")).Ok(),
        "repository source accepted an unknown trust key") && result;
    result = Check(
        !anomaly::ParseRepositoryConfiguration(EnabledConfiguration(
            "anomaly-official-2026", "file:///repository/index.json")).Ok(),
        "file repository source bypassed the opt-in policy") && result;
    result = Check(
        !anomaly::ParseRepositoryConfiguration(EnabledConfiguration(
            "anomaly-official-2026", "https://repo.anomaly.dev/v1/index.json",
            "primary-only")).Ok(),
        "primary-only repository source accepted mirrors") && result;
    std::string invalid_key = EnabledConfiguration();
    invalid_key.replace(
        invalid_key.find(kPublicKey), kPublicKey.size(), std::string(128, '0'));
    result = Check(!anomaly::ParseRepositoryConfiguration(invalid_key).Ok(),
                   "invalid P-256 trust root was accepted") && result;

    std::string reversed = EnabledConfiguration();
    const std::string from = "2026-01-01T00:00:00Z";
    const std::string until = "2027-01-01T00:00:00Z";
    reversed.replace(reversed.find(from), from.size(), "2028-01-01T00:00:00Z");
    reversed.replace(reversed.find(until), until.size(), "2027-01-01T00:00:00Z");
    result = Check(!anomaly::ParseRepositoryConfiguration(reversed).Ok(),
                   "reversed trust-key validity interval was accepted") && result;

    std::string invalid_date = EnabledConfiguration();
    invalid_date.replace(invalid_date.find(from), from.size(), "2026-02-30T00:00:00Z");
    result = Check(!anomaly::ParseRepositoryConfiguration(invalid_date).Ok(),
                   "invalid trust-key calendar date was accepted") && result;

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-repository-config-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const std::filesystem::path file = root / L"repository.json";
    std::ofstream(file, std::ios::binary | std::ios::trunc) << EnabledConfiguration();
    const auto loaded = anomaly::LoadRepositoryConfiguration(file);
    result = Check(loaded.Ok() && loaded.configuration->sources.front().mirrors.size() == 1,
                   "repository configuration file load failed") && result;
    result = Check(!anomaly::LoadRepositoryConfiguration(root / L"missing.json").Ok(),
                   "missing repository configuration was accepted") && result;

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result ? 0 : 2;
}
