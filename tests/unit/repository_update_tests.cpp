#include "anomaly/artifact_bundle.hpp"
#include "anomaly/artifact_crypto.hpp"
#include "anomaly/repository_index.hpp"
#include "anomaly/update_transaction.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::filesystem::path TemporaryDirectory() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"anomaly-phase7-update-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path);
    return path;
}

void Write(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
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
        if (key_) BCryptDestroyKey(key_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    [[nodiscard]] const std::string& PublicKey() const noexcept { return public_key_; }
    [[nodiscard]] std::string Sign(std::string_view payload) const {
        const std::string digest_hex = anomaly::Sha256Hex({
            reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
        std::vector<std::uint8_t> digest;
        if (!anomaly::DecodeHex(digest_hex, digest)) return {};
        DWORD size{};
        BCryptSignHash(key_, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
            nullptr, 0, &size, 0);
        std::vector<std::uint8_t> signature(size);
        BCryptSignHash(key_, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
            signature.data(), size, &size, 0);
        signature.resize(size);
        return Hex(signature);
    }
private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_KEY_HANDLE key_{};
    std::string public_key_;
};

std::string Manifest(std::string_view version) {
    return std::string(R"({
  "schemaVersion": 2,
  "id": "com.example.phase7",
  "name": "Phase 7 Fixture",
  "version": ")") + std::string(version) + R"(",
  "author": "Tests",
  "entry": "plugin.dll",
  "api": {"major": 1, "minMinor": 0, "maxMinor": 0},
  "games": ["nte"],
  "builds": ["nte-win64-*"],
  "loadPhase": "game-ready",
  "capabilities": []
})";
}

void WritePackedPlugin(const std::filesystem::path& root, std::string_view version) {
    Write(root / L"manifest.json", Manifest(version));
    Write(root / L"plugin.dll", "fixture-binary-" + std::string(version));
    const std::array<std::filesystem::path, 2> files{L"manifest.json", L"plugin.dll"};
    std::ofstream hashes(root / L"package.sha256", std::ios::binary | std::ios::trunc);
    for (const auto& file : files) {
        hashes << anomaly::Sha256FileHex(root / file) << "  " << file.generic_string() << '\n';
    }
}

anomaly::RepositoryArtifact PluginArtifact(
    const std::filesystem::path& bundle, std::string_view version, const SigningKey& key) {
    anomaly::RepositoryArtifact artifact;
    artifact.kind = anomaly::RepositoryArtifactKind::Plugin;
    artifact.id = "com.example.phase7";
    artifact.version = *anomaly::ParseSemanticVersion(version);
    artifact.channel = anomaly::RepositoryChannel::Stable;
    artifact.uri = "https://repo.example/phase7.anomaly-package";
    artifact.size = std::filesystem::file_size(bundle);
    artifact.sha256 = anomaly::Sha256FileHex(bundle);
    artifact.manifest_sha256 =
        anomaly::Sha256FileHex(bundle.parent_path() / L"package" / L"manifest.json");
    artifact.published_at = "2026-07-17T00:00:00Z";
    artifact.minimum_runtime = "1.0.0";
    artifact.minimum_api = 3;
    artifact.signature = {"ecdsa-p256-sha256", "test", {}};
    artifact.signature.value_hex =
        key.Sign(anomaly::CanonicalRepositoryArtifactPayload(artifact));
    return artifact;
}

std::string ProfileJson() {
    return R"({
  "schemaVersion": 1,
  "game": "nte",
  "symbols": {
    "ue5.GameTick": {
      "module": "HTGame.exe", "section": ".text", "pattern": "48 8B ?? ??",
      "resolve": {"kind": "direct"},
      "validators": ["address-in-module"], "requiredBy": ["anomaly.ue5.framework"]
    }
  },
  "features": {"ue5.framework": ["ue5.GameTick"]},
  "layout": {}
})";
}

nlohmann::json SignatureJson(const anomaly::RepositorySignature& signature) {
    return {{"algorithm", signature.algorithm}, {"keyId", signature.key_id},
            {"value", signature.value_hex}};
}

nlohmann::json ArtifactJson(const anomaly::RepositoryArtifact& artifact) {
    nlohmann::json result{
        {"kind", anomaly::RepositoryArtifactKindName(artifact.kind)}, {"id", artifact.id},
        {"version", artifact.version.ToString()},
        {"channel", anomaly::RepositoryChannelName(artifact.channel)}, {"uri", artifact.uri},
        {"size", artifact.size}, {"sha256", artifact.sha256},
        {"manifestSha256", artifact.manifest_sha256}, {"publishedAt", artifact.published_at},
        {"withdrawn", artifact.withdrawn}, {"signature", SignatureJson(artifact.signature)}};
    if (!artifact.minimum_runtime.empty()) result["minimumRuntime"] = artifact.minimum_runtime;
    if (artifact.minimum_api != 0) result["minimumApi"] = artifact.minimum_api;
    return result;
}

}  // namespace

int main() {
    bool result = true;
    const auto root = TemporaryDirectory();
    const auto package = root / L"package";
    WritePackedPlugin(package, "2.0.0");
    const auto bundle = root / L"phase7.anomaly-package";
    const auto bundled = anomaly::CreateArtifactBundle(package, bundle);
    result = Check(bundled.Ok() && bundled.entries == 3, "artifact bundle create failed") && result;
    const auto extracted = root / L"extracted";
    const auto extracted_result = anomaly::ExtractArtifactBundle(bundle, extracted);
    result = Check(extracted_result.Ok() &&
        Read(extracted / L"plugin.dll") == "fixture-binary-2.0.0",
        "artifact bundle extraction failed") && result;

    SigningKey key;
    anomaly::RepositoryTrustStore trust;
    result = Check(trust.AddEcdsaP256Key("test", key.PublicKey()), "trust key rejected") && result;
    auto artifact = PluginArtifact(bundle, "2.0.0", key);
    result = Check(anomaly::VerifyRepositoryArtifactSignature(artifact, trust),
                   "artifact signature verification failed") && result;
    auto tampered_descriptor = artifact;
    tampered_descriptor.size++;
    result = Check(!anomaly::VerifyRepositoryArtifactSignature(tampered_descriptor, trust),
                   "tampered artifact descriptor passed signature verification") && result;

    anomaly::RepositoryIndex index;
    index.repository_id = "dev.anomaly.repository";
    index.sequence = 1;
    index.generated_at = "2026-07-17T00:00:00Z";
    index.expires_at = "2026-07-18T00:00:00Z";
    index.artifacts.push_back(artifact);
    index.signature = {"ecdsa-p256-sha256", "test", {}};
    index.signature.value_hex = key.Sign(anomaly::CanonicalRepositoryIndexPayload(index));
    result = Check(anomaly::VerifyRepositoryIndexSignature(index, trust),
                   "repository index signature verification failed") && result;
    nlohmann::json index_json{{"schemaVersion", 1}, {"repositoryId", index.repository_id},
        {"sequence", index.sequence},
        {"generatedAt", index.generated_at},
        {"expiresAt", index.expires_at},
        {"artifacts", nlohmann::json::array({ArtifactJson(artifact)})},
        {"signature", SignatureJson(index.signature)}};
    const auto parsed_index = anomaly::ParseRepositoryIndex(index_json.dump());
    if (!parsed_index.Ok()) {
        for (const auto& diagnostic : parsed_index.diagnostics) {
            std::cerr << diagnostic.path << ": " << diagnostic.message << '\n';
        }
    }
    result = Check(parsed_index.Ok() && parsed_index.index->artifacts.size() == 1 &&
                       parsed_index.index->artifacts.front().minimum_runtime == "1.0.0" &&
                       parsed_index.index->artifacts.front().minimum_api == 3,
                   "repository index schema parse failed") && result;
    result = Check(parsed_index.Ok() &&
                   anomaly::VerifyRepositoryIndexSignature(*parsed_index.index, trust),
                   "parsed repository index signature failed") && result;
    auto runtime_index = index_json;
    runtime_index["artifacts"][0]["kind"] = "runtime";
    result = Check(!anomaly::ParseRepositoryIndex(runtime_index.dump()).Ok(),
                   "repository index accepted a removed Runtime artifact") && result;
    auto invalid_time_index = index_json;
    invalid_time_index["expiresAt"] = "2026-02-30T00:00:00Z";
    result = Check(!anomaly::ParseRepositoryIndex(invalid_time_index.dump()).Ok(),
                   "repository index accepted an invalid calendar date") && result;
    index_json["artifacts"][0]["withdrawn"] = true;
    result = Check(!anomaly::ParseRepositoryIndex(index_json.dump()).Ok(),
                   "withdrawn artifact without reason passed schema") && result;

    const auto runtime = root / L"runtime";
    Write(runtime / L"plugins" / L"com.example.phase7" / L"old.txt", "old-version");
    anomaly::RepositoryUpdateTransaction transaction({runtime}, trust);
    result = Check(transaction.RecoverInterrupted().Ok(), "empty recovery failed") && result;
    const auto installed = transaction.InstallPlugin(artifact, bundle);
    result = Check(installed.Ok() &&
        Read(runtime / L"plugins" / L"com.example.phase7" / L"plugin.dll") ==
            "fixture-binary-2.0.0" &&
        Read(runtime / L"state" / L"update-rollback" / L"plugins" /
             L"com.example.phase7" / L"old.txt") == "old-version",
        "plugin update transaction failed") && result;

    const auto corrupted = root / L"corrupted.anomaly-package";
    std::filesystem::copy_file(bundle, corrupted);
    {
        std::fstream stream(corrupted, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(-1, std::ios::end);
        const char byte = '\xff';
        stream.write(&byte, 1);
    }
    const auto rejected = transaction.InstallPlugin(artifact, corrupted);
    result = Check(!rejected.Ok() &&
        Read(runtime / L"plugins" / L"com.example.phase7" / L"plugin.dll") ==
            "fixture-binary-2.0.0",
        "failed update damaged active plugin") && result;
    result = Check(transaction.RollbackPlugin("com.example.phase7").Ok() &&
        Read(runtime / L"plugins" / L"com.example.phase7" / L"old.txt") == "old-version",
        "plugin rollback failed") && result;

    const auto interrupted_backup =
        runtime / L"state" / L"update-rollback" / L"plugins" / L"interrupted";
    Write(interrupted_backup / L"old.txt", "recover-me");
    Write(runtime / L"state" / L"update-staging" / L"interrupted" / L"partial.txt", "partial");
    Write(runtime / L"state" / L"update-transaction.json", R"({
      "schemaVersion":1,
      "phase":"old-moved",
      "destination":"plugins/interrupted",
      "backup":"state/update-rollback/plugins/interrupted",
      "staging":"state/update-staging/interrupted"
    })");
    const auto recovered = transaction.RecoverInterrupted();
    result = Check(recovered.Ok() &&
        Read(runtime / L"plugins" / L"interrupted" / L"old.txt") == "recover-me" &&
        !std::filesystem::exists(runtime / L"state" / L"update-staging" / L"interrupted"),
        "interrupted update did not restore previous plugin") && result;

    const auto runtime_sentinel = runtime / L"runtime-sentinel.txt";
    Write(runtime_sentinel, "must-survive-invalid-journal");
    Write(runtime / L"state" / L"update-transaction.json", R"({
      "schemaVersion":1,
      "phase":"old-moved",
      "destination":"plugins/interrupted",
      "backup":"state/update-rollback/plugins/interrupted",
      "staging":"."
    })");
    const auto root_journal_rejected = transaction.RecoverInterrupted();
    result = Check(!root_journal_rejected.Ok() &&
        root_journal_rejected.error == anomaly::UpdateTransactionError::RecoveryFailure &&
        Read(runtime_sentinel) == "must-survive-invalid-journal",
        "recovery journal accepted the runtime root as staging") && result;
    Write(runtime / L"state" / L"update-transaction.json", R"({
      "schemaVersion":1,
      "phase":"old-moved",
      "destination":"plugins/interrupted",
      "backup":"state/update-rollback/plugins/interrupted",
      "staging":"C:outside"
    })");
    const auto drive_relative_journal_rejected = transaction.RecoverInterrupted();
    result = Check(!drive_relative_journal_rejected.Ok() &&
        drive_relative_journal_rejected.error == anomaly::UpdateTransactionError::RecoveryFailure &&
        Read(runtime_sentinel) == "must-survive-invalid-journal",
        "recovery journal accepted a drive-relative path") && result;
    std::error_code journal_cleanup_error;
    std::filesystem::remove(runtime / L"state" / L"update-transaction.json", journal_cleanup_error);

    const auto root_journal_temporary = std::filesystem::path(runtime.wstring() + L".tmp");
    std::filesystem::remove(root_journal_temporary, journal_cleanup_error);
    anomaly::UpdateTransactionOptions root_journal_options{runtime};
    root_journal_options.journal_file = runtime;
    anomaly::RepositoryUpdateTransaction root_journal_transaction(root_journal_options, trust);
    const auto root_journal_option_rejected =
        root_journal_transaction.InstallPlugin(artifact, bundle);
    result = Check(!root_journal_option_rejected.Ok() &&
        root_journal_option_rejected.error == anomaly::UpdateTransactionError::StagingFailure &&
        Read(runtime_sentinel) == "must-survive-invalid-journal" &&
        !std::filesystem::exists(root_journal_temporary),
        "transaction journal accepted the runtime root") && result;
    std::filesystem::remove(root_journal_temporary, journal_cleanup_error);

    const auto external_journal = root / L"external-journal.json";
    anomaly::UpdateTransactionOptions external_journal_options{runtime};
    external_journal_options.journal_file = external_journal;
    anomaly::RepositoryUpdateTransaction external_journal_transaction(external_journal_options, trust);
    const auto external_journal_rejected = external_journal_transaction.RecoverInterrupted();
    result = Check(!external_journal_rejected.Ok() &&
        external_journal_rejected.error == anomaly::UpdateTransactionError::RecoveryFailure &&
        !std::filesystem::exists(external_journal),
        "external transaction journal was accepted") && result;

    const auto external_staging = root / L"external-staging";
    const auto external_staging_sentinel = external_staging / L"sentinel.txt";
    Write(external_staging_sentinel, "must-survive-invalid-rollback");
    anomaly::UpdateTransactionOptions external_staging_options{runtime};
    external_staging_options.staging_directory = external_staging;
    anomaly::RepositoryUpdateTransaction external_staging_transaction(external_staging_options, trust);
    const auto external_rollback_rejected =
        external_staging_transaction.RollbackPlugin("com.example.phase7");
    result = Check(!external_rollback_rejected.Ok() &&
        external_rollback_rejected.error == anomaly::UpdateTransactionError::CommitFailure &&
        Read(external_staging_sentinel) == "must-survive-invalid-rollback",
        "rollback accepted an external staging path") && result;

    const auto profile_file = root / L"profile.json";
    Write(profile_file, ProfileJson());
    anomaly::RepositoryArtifact profile_artifact;
    profile_artifact.kind = anomaly::RepositoryArtifactKind::NteProfile;
    profile_artifact.id = "dev.anomaly.nte-profile";
    profile_artifact.version = *anomaly::ParseSemanticVersion("1.0.0");
    profile_artifact.uri = "https://repo.example/profile.json";
    profile_artifact.size = std::filesystem::file_size(profile_file);
    profile_artifact.sha256 = anomaly::Sha256FileHex(profile_file);
    profile_artifact.manifest_sha256 = profile_artifact.sha256;
    profile_artifact.published_at = "2026-07-17T00:00:00Z";
    profile_artifact.signature = {"ecdsa-p256-sha256", "test", {}};
    profile_artifact.signature.value_hex =
        key.Sign(anomaly::CanonicalRepositoryArtifactPayload(profile_artifact));

    const auto external_managed_profiles = root / L"external-managed-profiles";
    const auto external_profile_sentinel = external_managed_profiles / L"sentinel.txt";
    Write(external_profile_sentinel, "must-survive-invalid-profile-update");
    anomaly::UpdateTransactionOptions external_profile_options{runtime};
    external_profile_options.managed_profile_directory = external_managed_profiles;
    anomaly::RepositoryUpdateTransaction external_profile_transaction(external_profile_options, trust);
    const auto external_profile_rejected =
        external_profile_transaction.InstallNteProfile(profile_artifact, profile_file);
    result = Check(!external_profile_rejected.Ok() &&
        external_profile_rejected.error == anomaly::UpdateTransactionError::StagingFailure &&
        Read(external_profile_sentinel) == "must-survive-invalid-profile-update",
        "NTE Profile update accepted an external managed profile directory") && result;

    const auto previous_profile = runtime / L"state" / L"profiles" / L"managed" /
        L"nte" / L"active.json";
    Write(previous_profile, "previous-profile");
    const auto profile_installed =
        transaction.InstallNteProfile(profile_artifact, profile_file);
    result = Check(profile_installed.Ok() &&
        std::filesystem::exists(profile_installed.active_path),
        "independent NTE Profile update failed") && result;
    result = Check(transaction.RollbackNteProfile("nte").Ok() &&
        Read(previous_profile) == "previous-profile",
        "independent NTE Profile rollback failed") && result;

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result ? 0 : 2;
}
