#pragma once

#include "anomaly/repository_index.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace anomaly {

enum class UpdateTransactionError : std::uint8_t {
    None,
    InvalidArtifact,
    Withdrawn,
    SignatureRejected,
    SizeMismatch,
    HashMismatch,
    BundleRejected,
    ManifestRejected,
    ContentRejected,
    StagingFailure,
    CommitFailure,
    RollbackUnavailable,
    RecoveryFailure,
};

struct UpdateTransactionResult {
    UpdateTransactionError error{UpdateTransactionError::None};
    std::string message;
    std::filesystem::path active_path;
    std::filesystem::path rollback_path;
    [[nodiscard]] bool Ok() const noexcept { return error == UpdateTransactionError::None; }
};

struct UpdateTransactionOptions {
    std::filesystem::path runtime_root;
    std::filesystem::path plugin_directory{L"plugins"};
    std::filesystem::path managed_profile_directory{L"state/profiles/managed"};
    std::filesystem::path staging_directory{L"state/update-staging"};
    std::filesystem::path rollback_directory{L"state/update-rollback"};
    std::filesystem::path journal_file{L"state/update-transaction.json"};
};

class RepositoryUpdateTransaction final {
public:
    RepositoryUpdateTransaction(
        UpdateTransactionOptions options, const RepositoryTrustStore& trust);

    [[nodiscard]] UpdateTransactionResult RecoverInterrupted();
    [[nodiscard]] UpdateTransactionResult InstallPlugin(
        const RepositoryArtifact& artifact,
        const std::filesystem::path& downloaded_bundle);
    [[nodiscard]] UpdateTransactionResult InstallNteProfile(
        const RepositoryArtifact& artifact,
        const std::filesystem::path& downloaded_profile_json);
    [[nodiscard]] UpdateTransactionResult RollbackPlugin(std::string_view plugin_id);
    [[nodiscard]] UpdateTransactionResult RollbackNteProfile(std::string_view game_id);

private:
    UpdateTransactionOptions options_;
    const RepositoryTrustStore& trust_;
};

}  // namespace anomaly
