#include "anomaly/update_transaction.hpp"

#include "anomaly/artifact_bundle.hpp"
#include "anomaly/artifact_crypto.hpp"
#include "anomaly/build_profile.hpp"
#include "anomaly/plugin_manifest.hpp"
#include "anomaly/plugin_package.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <set>
#include <span>

namespace anomaly {
namespace {

using Json = nlohmann::json;

UpdateTransactionResult Failure(UpdateTransactionError error, std::string message) {
    return {error, std::move(message), {}, {}};
}

std::optional<std::string> ReadText(
    const std::filesystem::path& path, std::size_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input && !text.empty()) return std::nullopt;
    return text;
}

std::filesystem::path Resolve(
    const std::filesystem::path& root, const std::filesystem::path& value) {
    return value.is_absolute() ? value : root / value;
}

bool SimpleId(std::string_view value) {
    if (value.empty() || value.size() > 255) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '.' || character == '-';
    });
}

std::filesystem::path Utf8Path(std::string_view value) {
    const std::u8string encoded(
        reinterpret_cast<const char8_t*>(value.data()), value.size());
    return std::filesystem::path(encoded);
}

bool VerifyDownloaded(
    const RepositoryArtifact& artifact, const std::filesystem::path& downloaded,
    const RepositoryTrustStore& trust, UpdateTransactionResult& failure) {
    if (artifact.withdrawn) {
        failure = Failure(UpdateTransactionError::Withdrawn, "repository artifact is withdrawn");
        return false;
    }
    if (!VerifyRepositoryArtifactSignature(artifact, trust)) {
        failure = Failure(UpdateTransactionError::SignatureRejected, "artifact signature is not trusted");
        return false;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(downloaded, error);
    if (error || size != artifact.size) {
        failure = Failure(UpdateTransactionError::SizeMismatch, "downloaded artifact size mismatch");
        return false;
    }
    const std::string digest = Sha256FileHex(downloaded, artifact.size);
    if (digest.empty() || !ConstantTimeHexEqual(artifact.sha256, digest)) {
        failure = Failure(UpdateTransactionError::HashMismatch, "downloaded artifact hash mismatch");
        return false;
    }
    return true;
}

bool ValidatePackageContent(
    const std::filesystem::path& root, std::string& message) {
    const auto list = ReadText(root / L"package.sha256", 4U * 1024U * 1024U);
    if (!list) {
        message = "package.sha256 is missing or unreadable";
        return false;
    }
    std::set<std::string> expected_paths;
    std::size_t cursor{};
    while (cursor < list->size()) {
        const std::size_t end = list->find('\n', cursor);
        std::string_view line(*list);
        line = line.substr(cursor, end == std::string::npos ? line.size() - cursor : end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) {
            if (line.size() < 67 || line[64] != ' ' || line[65] != ' ') {
                message = "package.sha256 contains an invalid record";
                return false;
            }
            const std::string digest(line.substr(0, 64));
            const std::string path(line.substr(66));
            std::vector<std::uint8_t> decoded;
            const auto validated = ValidatePluginPackageRelativePath(path, false);
            if (!DecodeHex(digest, decoded) || decoded.size() != 32 || !validated.Ok() ||
                !expected_paths.insert(path).second) {
                message = "package.sha256 contains an invalid path or digest";
                return false;
            }
            const std::string actual = Sha256FileHex(root / validated.path);
            if (actual.empty() || !ConstantTimeHexEqual(digest, actual)) {
                message = "package content hash mismatch: " + path;
                return false;
            }
        }
        cursor = end == std::string::npos ? list->size() : end + 1;
    }
    std::error_code error;
    std::set<std::string> actual_paths;
    for (std::filesystem::recursive_directory_iterator iterator(
             root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        const auto relative = std::filesystem::relative(iterator->path(), root, error).generic_u8string();
        const std::string path(reinterpret_cast<const char*>(relative.data()), relative.size());
        if (path != "package.sha256") actual_paths.insert(path);
    }
    if (error || actual_paths != expected_paths) {
        message = "package.sha256 does not cover the exact package content";
        return false;
    }
    return true;
}

bool WriteJournal(
    const std::filesystem::path& journal, std::string_view phase,
    const std::filesystem::path& destination, const std::filesystem::path& backup,
    const std::filesystem::path& staging) {
    std::error_code error;
    std::filesystem::create_directories(journal.parent_path(), error);
    if (error) return false;
    const Json document{{"schemaVersion", 1}, {"phase", phase},
                        {"destination", destination.generic_string()},
                        {"backup", backup.generic_string()},
                        {"staging", staging.generic_string()}};
    const auto temporary = journal.wstring() + L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << document.dump(2) << '\n';
    output.flush();
    output.close();
    if (!output) return false;
    std::filesystem::remove(journal, error);
    error.clear();
    std::filesystem::rename(temporary, journal, error);
    return !error;
}

bool RelativeBelowRoot(
    const std::filesystem::path& root, const std::filesystem::path& path,
    std::filesystem::path& relative) {
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    const std::filesystem::path canonical_path = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    relative = canonical_path.lexically_relative(canonical_root);
    if (error || relative.empty() || relative.is_absolute()) return false;
    for (const auto& component : relative) {
        if (component == L"." || component == L"..") return false;
    }
    return true;
}

bool PathsBelowRoot(
    const std::filesystem::path& root,
    const std::initializer_list<std::filesystem::path>& paths) {
    std::filesystem::path relative;
    return std::all_of(paths.begin(), paths.end(), [&](const auto& path) {
        return RelativeBelowRoot(root, path, relative);
    });
}

bool StrictJournalRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == L"." || component == L"..") return false;
    }
    return true;
}

UpdateTransactionResult CommitPath(
    const std::filesystem::path& root, const std::filesystem::path& journal,
    const std::filesystem::path& staging, const std::filesystem::path& destination,
    const std::filesystem::path& backup) {
    std::filesystem::path destination_relative;
    std::filesystem::path backup_relative;
    std::filesystem::path staging_relative;
    if (!PathsBelowRoot(root, {journal, destination, backup, staging}) ||
        !RelativeBelowRoot(root, destination, destination_relative) ||
        !RelativeBelowRoot(root, backup, backup_relative) ||
        !RelativeBelowRoot(root, staging, staging_relative)) {
        return Failure(UpdateTransactionError::CommitFailure, "transaction path escapes runtime root");
    }
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    std::filesystem::create_directories(backup.parent_path(), error);
    if (error || !WriteJournal(
            journal, "prepared", destination_relative, backup_relative, staging_relative)) {
        return Failure(UpdateTransactionError::CommitFailure, "transaction journal prepare failed");
    }
    std::filesystem::remove_all(backup, error);
    if (error) return Failure(UpdateTransactionError::CommitFailure, "old rollback cleanup failed");
    const bool had_current = std::filesystem::exists(destination, error);
    if (error) return Failure(UpdateTransactionError::CommitFailure, "active artifact inspection failed");
    if (had_current) {
        std::filesystem::rename(destination, backup, error);
        if (error) return Failure(UpdateTransactionError::CommitFailure, "active artifact backup failed");
    }
    if (!WriteJournal(
            journal, "old-moved", destination_relative, backup_relative, staging_relative)) {
        if (had_current) std::filesystem::rename(backup, destination, error);
        return Failure(UpdateTransactionError::CommitFailure, "transaction journal commit failed");
    }
    std::filesystem::rename(staging, destination, error);
    if (error) {
        std::error_code restore_error;
        if (had_current) std::filesystem::rename(backup, destination, restore_error);
        return Failure(UpdateTransactionError::CommitFailure, "atomic artifact publish failed");
    }
    std::filesystem::remove(journal, error);
    return {UpdateTransactionError::None, "artifact committed", destination,
            had_current ? backup : std::filesystem::path{}};
}

UpdateTransactionResult SwapRollback(
    const std::filesystem::path& root, const std::filesystem::path& active,
    const std::filesystem::path& rollback,
    const std::filesystem::path& scratch) {
    if (!PathsBelowRoot(root, {active, rollback, scratch})) {
        return Failure(UpdateTransactionError::CommitFailure, "rollback path escapes runtime root");
    }
    std::error_code error;
    if (!std::filesystem::exists(rollback, error)) {
        return Failure(UpdateTransactionError::RollbackUnavailable, "rollback artifact is unavailable");
    }
    std::filesystem::remove_all(scratch, error);
    const bool had_active = std::filesystem::exists(active, error);
    if (had_active) {
        std::filesystem::create_directories(scratch.parent_path(), error);
        std::filesystem::rename(active, scratch, error);
        if (error) return Failure(UpdateTransactionError::CommitFailure, "active rollback staging failed");
    }
    std::filesystem::rename(rollback, active, error);
    if (error) {
        if (had_active) std::filesystem::rename(scratch, active, error);
        return Failure(UpdateTransactionError::CommitFailure, "rollback publish failed");
    }
    if (had_active) std::filesystem::rename(scratch, rollback, error);
    return {UpdateTransactionError::None, "rollback committed", active,
            had_active ? rollback : std::filesystem::path{}};
}

}  // namespace

RepositoryUpdateTransaction::RepositoryUpdateTransaction(
    UpdateTransactionOptions options, const RepositoryTrustStore& trust)
    : options_(std::move(options)), trust_(trust) {
    options_.runtime_root = std::filesystem::absolute(options_.runtime_root);
    options_.plugin_directory = Resolve(options_.runtime_root, options_.plugin_directory);
    options_.managed_profile_directory = Resolve(options_.runtime_root, options_.managed_profile_directory);
    options_.staging_directory = Resolve(options_.runtime_root, options_.staging_directory);
    options_.rollback_directory = Resolve(options_.runtime_root, options_.rollback_directory);
    options_.journal_file = Resolve(options_.runtime_root, options_.journal_file);
}

UpdateTransactionResult RepositoryUpdateTransaction::RecoverInterrupted() {
    try {
        std::error_code error;
        if (!PathsBelowRoot(options_.runtime_root, {options_.journal_file})) {
            return Failure(UpdateTransactionError::RecoveryFailure,
                "transaction journal escapes runtime root");
        }
        if (!std::filesystem::exists(options_.journal_file, error)) {
            return {UpdateTransactionError::None, "no interrupted transaction", {}, {}};
        }
        const auto text = ReadText(options_.journal_file, 64U * 1024U);
        if (!text) return Failure(UpdateTransactionError::RecoveryFailure, "transaction journal is unreadable");
        const Json journal = Json::parse(*text);
        if (journal.at("schemaVersion") != 1) {
            return Failure(UpdateTransactionError::RecoveryFailure, "transaction journal version is invalid");
        }
        const auto resolve_relative = [&](const char* key, std::filesystem::path& output) {
            const std::filesystem::path relative = journal.at(key).get<std::string>();
            if (!StrictJournalRelativePath(relative)) return false;
            std::filesystem::path confined_relative;
            if (!RelativeBelowRoot(
                    options_.runtime_root, options_.runtime_root / relative, confined_relative)) {
                return false;
            }
            output = options_.runtime_root / confined_relative;
            return true;
        };
        std::filesystem::path destination, backup, staging;
        if (!resolve_relative("destination", destination) || !resolve_relative("backup", backup) ||
            !resolve_relative("staging", staging)) {
            return Failure(UpdateTransactionError::RecoveryFailure, "transaction journal path is invalid");
        }
        const std::string phase = journal.at("phase").get<std::string>();
        if (phase == "old-moved" && !std::filesystem::exists(destination, error) &&
            std::filesystem::exists(backup, error)) {
            std::filesystem::create_directories(destination.parent_path(), error);
            std::filesystem::rename(backup, destination, error);
            if (error) return Failure(UpdateTransactionError::RecoveryFailure, "previous artifact restore failed");
        }
        std::filesystem::remove_all(staging, error);
        error.clear();
        std::filesystem::remove(options_.journal_file, error);
        if (error) return Failure(UpdateTransactionError::RecoveryFailure, "transaction journal cleanup failed");
        return {UpdateTransactionError::None, "interrupted transaction recovered", destination, backup};
    } catch (const std::exception& error) {
        return Failure(UpdateTransactionError::RecoveryFailure, error.what());
    }
}

UpdateTransactionResult RepositoryUpdateTransaction::InstallPlugin(
    const RepositoryArtifact& artifact,
    const std::filesystem::path& downloaded_bundle) {
    if (artifact.kind != RepositoryArtifactKind::Plugin || !SimpleId(artifact.id)) {
        return Failure(UpdateTransactionError::InvalidArtifact, "plugin artifact identity is invalid");
    }
    if (!PathsBelowRoot(options_.runtime_root,
            {options_.plugin_directory, options_.staging_directory,
                options_.rollback_directory, options_.journal_file})) {
        return Failure(UpdateTransactionError::StagingFailure,
            "plugin update path escapes runtime root");
    }
    UpdateTransactionResult verification;
    if (!VerifyDownloaded(artifact, downloaded_bundle, trust_, verification)) return verification;
    std::error_code error;
    std::filesystem::create_directories(options_.staging_directory, error);
    const auto staging = options_.staging_directory /
        (L"plugin-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::remove_all(staging, error);
    const auto extracted = ExtractArtifactBundle(downloaded_bundle, staging);
    if (!extracted.Ok()) {
        std::filesystem::remove_all(staging, error);
        return Failure(UpdateTransactionError::BundleRejected, extracted.message);
    }
    const auto manifest_text = ReadText(staging / L"manifest.json", kMaximumPluginManifestBytes);
    if (!manifest_text || !ConstantTimeHexEqual(
            artifact.manifest_sha256, Sha256FileHex(staging / L"manifest.json"))) {
        std::filesystem::remove_all(staging, error);
        return Failure(UpdateTransactionError::ManifestRejected, "plugin manifest hash mismatch");
    }
    auto manifest = ParsePluginManifest(*manifest_text);
    if (!manifest.Ok() || manifest.manifest->id != artifact.id ||
        manifest.manifest->version.ToString() != artifact.version.ToString()) {
        std::filesystem::remove_all(staging, error);
        return Failure(UpdateTransactionError::ManifestRejected, "plugin manifest identity mismatch");
    }
    const auto entry = OpenConfinedPluginPackageFile(
        staging, manifest.manifest->entry, true);
    std::string content_error;
    if (!entry.Ok() || !ValidatePackageContent(staging, content_error)) {
        std::filesystem::remove_all(staging, error);
        return Failure(UpdateTransactionError::ContentRejected,
            entry.Ok() ? content_error : entry.message);
    }
    return CommitPath(options_.runtime_root, options_.journal_file, staging,
        options_.plugin_directory / Utf8Path(artifact.id),
        options_.rollback_directory / L"plugins" / Utf8Path(artifact.id));
}

UpdateTransactionResult RepositoryUpdateTransaction::InstallNteProfile(
    const RepositoryArtifact& artifact,
    const std::filesystem::path& downloaded_profile_json) {
    if (artifact.kind != RepositoryArtifactKind::NteProfile) {
        return Failure(UpdateTransactionError::InvalidArtifact, "NTE Profile artifact identity is invalid");
    }
    if (!PathsBelowRoot(options_.runtime_root,
            {options_.managed_profile_directory, options_.staging_directory,
                options_.rollback_directory, options_.journal_file})) {
        return Failure(UpdateTransactionError::StagingFailure,
            "NTE Profile update path escapes runtime root");
    }
    UpdateTransactionResult verification;
    if (!VerifyDownloaded(artifact, downloaded_profile_json, trust_, verification)) return verification;
    if (!ConstantTimeHexEqual(artifact.manifest_sha256, artifact.sha256)) {
        return Failure(UpdateTransactionError::ManifestRejected, "NTE Profile manifest hash mismatch");
    }
    const auto text = ReadText(downloaded_profile_json, kMaximumBuildProfileBytes);
    auto parsed = text ? ParseBuildProfile(*text, downloaded_profile_json) : BuildProfileParseResult{};
    if (!parsed.Ok() || parsed.profile->game != "nte") {
        return Failure(UpdateTransactionError::ManifestRejected, "NTE Profile game identity mismatch");
    }
    std::error_code error;
    std::filesystem::create_directories(options_.staging_directory, error);
    const auto staging = options_.staging_directory /
        (L"profile-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L".json");
    std::filesystem::copy_file(downloaded_profile_json, staging,
        std::filesystem::copy_options::overwrite_existing, error);
    if (error) return Failure(UpdateTransactionError::StagingFailure, "NTE Profile staging failed");
    return CommitPath(options_.runtime_root, options_.journal_file, staging,
        options_.managed_profile_directory / L"nte" / L"active.json",
        options_.rollback_directory / L"profiles" / L"nte" / L"active.json");
}

UpdateTransactionResult RepositoryUpdateTransaction::RollbackPlugin(std::string_view plugin_id) {
    if (!SimpleId(plugin_id)) return Failure(UpdateTransactionError::InvalidArtifact, "plugin id is invalid");
    const auto id = Utf8Path(plugin_id);
    return SwapRollback(options_.runtime_root, options_.plugin_directory / id,
        options_.rollback_directory / L"plugins" / id,
        options_.staging_directory / L"rollback-plugin");
}

UpdateTransactionResult RepositoryUpdateTransaction::RollbackNteProfile(
    std::string_view game_id) {
    if (!SimpleId(game_id)) {
        return Failure(UpdateTransactionError::InvalidArtifact, "profile identity is invalid");
    }
    const auto game = Utf8Path(game_id);
    return SwapRollback(options_.runtime_root,
        options_.managed_profile_directory / game / L"active.json",
        options_.rollback_directory / L"profiles" / game / L"active.json",
        options_.staging_directory / L"rollback-profile.json");
}

}  // namespace anomaly
