#include "anomaly/launcher/proxy_installation.hpp"

#include "anomaly/artifact_crypto.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace anomaly::launcher {
namespace {

constexpr wchar_t kGameExecutable[] = L"HTGame.exe";
constexpr wchar_t kProxyName[] = L"dwmapi.dll";
constexpr wchar_t kDisabledProxyName[] = L"dwmapi.dll.disabled";
constexpr wchar_t kRuntimeDirectoryName[] = L"Anomaly";
constexpr wchar_t kCoreName[] = L"Anomaly.Core.dll";
constexpr DWORD kMoveRetryDelayMilliseconds = 50;
constexpr DWORD kMoveRetryWindowMilliseconds = 2000;
constexpr std::size_t kMoveAttempts =
    1 + kMoveRetryWindowMilliseconds / kMoveRetryDelayMilliseconds;

ProxyInstallationStatus Failure(
    const std::filesystem::path& game_directory,
    ProxyInstallationState state,
    ProxyInstallationError error,
    std::string message) {
    return {state, error, game_directory, std::move(message)};
}

std::string Win32Failure(std::string message, DWORD error) {
    message += " (Win32 ";
    message += std::to_string(error);
    message += ')';
    return message;
}

bool IsTransientMoveFailure(DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION;
}

// Filesystem filters can briefly deny delete sharing after Runtime inventory reads.
DWORD MovePath(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) noexcept {
    DWORD error = ERROR_GEN_FAILURE;
    for (std::size_t attempt = 0; attempt < kMoveAttempts; ++attempt) {
        if (MoveFileExW(
                source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE) {
            return ERROR_SUCCESS;
        }
        error = GetLastError();
        if (!IsTransientMoveFailure(error) || attempt + 1 == kMoveAttempts) break;
        Sleep(kMoveRetryDelayMilliseconds);
    }
    return error;
}

bool IsReparsePoint(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
        !IsReparsePoint(path);
}

bool IsGameDirectory(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error &&
        !IsReparsePoint(path) && IsRegularFile(path / kGameExecutable);
}

std::string Digest(const std::filesystem::path& path) noexcept {
    return IsRegularFile(path) ? anomaly::Sha256FileHex(path) : std::string{};
}

bool Matches(const std::filesystem::path& path, std::string_view expected) noexcept {
    if (expected.empty()) return false;
    const std::string actual = Digest(path);
    return !actual.empty() && anomaly::ConstantTimeHexEqual(expected, actual);
}

bool IsPortableExecutable(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return false;
    const auto offset = static_cast<std::size_t>(dos.e_lfanew);
    if (offset > bytes.size() || sizeof(DWORD) > bytes.size() - offset) return false;
    DWORD signature{};
    std::memcpy(&signature, bytes.data() + offset, sizeof(signature));
    return signature == IMAGE_NT_SIGNATURE;
}

bool ContainsBytes(
    std::span<const std::byte> bytes, std::span<const std::byte> pattern) noexcept {
    return !pattern.empty() && std::search(
        bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}

bool IsAnomalyProxy(const std::filesystem::path& path) noexcept {
    if (!IsRegularFile(path)) return false;
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto end = input.tellg();
        if (!input || end <= 0 || end > 32 * 1024 * 1024) return false;
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        input.seekg(0);
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!input || !IsPortableExecutable(bytes)) return false;

        constexpr std::array core_path{
            std::byte{'A'}, std::byte{0}, std::byte{'n'}, std::byte{0},
            std::byte{'o'}, std::byte{0}, std::byte{'m'}, std::byte{0},
            std::byte{'a'}, std::byte{0}, std::byte{'l'}, std::byte{0},
            std::byte{'y'}, std::byte{0}, std::byte{'.'}, std::byte{0},
            std::byte{'C'}, std::byte{0}, std::byte{'o'}, std::byte{0},
            std::byte{'r'}, std::byte{0}, std::byte{'e'}, std::byte{0},
            std::byte{'.'}, std::byte{0}, std::byte{'d'}, std::byte{0},
            std::byte{'l'}, std::byte{0}, std::byte{'l'}, std::byte{0}};
        constexpr std::string_view start_entry{"AnomalyStart"};
        const auto start_bytes = std::as_bytes(
            std::span(start_entry.data(), start_entry.size()));
        return ContainsBytes(bytes, core_path) && ContainsBytes(bytes, start_bytes);
    } catch (...) {
        return false;
    }
}

bool CopyRuntimeTree(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& error_message) {
    std::error_code error;
    if (!std::filesystem::is_directory(source, error) || error ||
        IsReparsePoint(source) || !IsRegularFile(source / kCoreName)) {
        error_message = "runtime source does not contain Anomaly.Core.dll";
        return false;
    }

    std::filesystem::create_directory(destination, error);
    if (error) {
        error_message = "runtime staging directory could not be created";
        return false;
    }

    for (std::filesystem::recursive_directory_iterator iterator(source, error), end;
         iterator != end && !error; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (IsReparsePoint(entry.path())) {
            error_message = "runtime source contains a reparse point";
            return false;
        }
        const auto relative = std::filesystem::relative(entry.path(), source, error);
        if (error || relative.empty()) {
            error_message = "runtime source path is not confined";
            return false;
        }
        const auto target = destination / relative;
        if (entry.is_directory(error)) {
            std::filesystem::create_directory(target, error);
        } else if (!error && entry.is_regular_file(error)) {
            std::filesystem::create_directories(target.parent_path(), error);
            if (!error) {
                std::filesystem::copy_file(
                    entry.path(), target, std::filesystem::copy_options::none, error);
            }
        } else if (!error) {
            error_message = "runtime source contains an unsupported entry";
            return false;
        }
        if (error) {
            error_message = "runtime source could not be copied";
            return false;
        }
    }
    if (error || !IsRegularFile(destination / kCoreName)) {
        error_message = "runtime staging inventory is incomplete";
        return false;
    }
    return true;
}

bool PreservesInstalledFile(const std::filesystem::path& relative) noexcept {
    if (relative == L"anomaly.ini" || relative == L"repository.json" ||
        relative == L"plugin-repositories.json" ||
        relative == L"anomaly-platform.log") {
        return true;
    }
    const auto first = relative.begin();
    return first != relative.end() &&
        (*first == L"config" || *first == L"state" || *first == L"logs" ||
         *first == L"crashes");
}

bool MergeInstalledRuntime(
    const std::filesystem::path& installed,
    const std::filesystem::path& staging,
    std::string& error_message) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(installed, error), end;
         iterator != end && !error; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (IsReparsePoint(entry.path())) {
            error_message = "installed runtime contains a reparse point";
            return false;
        }
        const auto relative = std::filesystem::relative(entry.path(), installed, error);
        if (error || relative.empty()) {
            error_message = "installed runtime path is not confined";
            return false;
        }
        const auto target = staging / relative;
        if (entry.is_directory(error)) {
            std::filesystem::create_directories(target, error);
        } else if (!error && entry.is_regular_file(error)) {
            const bool target_exists = std::filesystem::exists(target, error);
            if (!error && (!target_exists || PreservesInstalledFile(relative))) {
                std::filesystem::create_directories(target.parent_path(), error);
                if (!error) {
                    std::filesystem::copy_file(
                        entry.path(), target,
                        target_exists ? std::filesystem::copy_options::overwrite_existing
                                      : std::filesystem::copy_options::none,
                        error);
                }
            }
        } else if (!error) {
            error_message = "installed runtime contains an unsupported entry";
            return false;
        }
        if (error) {
            error_message = "installed runtime state could not be preserved";
            return false;
        }
    }
    if (error) {
        error_message = "installed runtime state could not be read";
        return false;
    }
    return true;
}

std::filesystem::path UniqueSibling(
    const std::filesystem::path& directory, std::wstring_view stem) {
    return directory /
        (std::wstring(stem) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
}

}  // namespace

ProxyInstallationStatus InspectProxyInstallation(
    const std::filesystem::path& game_directory,
    const ProxyInstallationSource& source) {
    if (!IsGameDirectory(game_directory)) {
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::InvalidGameDirectory,
            "selected directory does not contain HTGame.exe");
    }

    const auto enabled = game_directory / kProxyName;
    const auto disabled = game_directory / kDisabledProxyName;
    std::error_code error;
    const bool has_enabled = std::filesystem::exists(enabled, error) && !error;
    if (error) {
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::IoFailure, "proxy state could not be read");
    }
    const bool has_disabled = std::filesystem::exists(disabled, error) && !error;
    if (error) {
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::IoFailure, "proxy state could not be read");
    }
    if (!has_enabled && !has_disabled) {
        return {ProxyInstallationState::NotInstalled, ProxyInstallationError::None,
                game_directory, "Anomaly proxy is not installed"};
    }

    const std::string expected_proxy = Digest(source.proxy);
    const std::string expected_core = Digest(source.runtime_directory / kCoreName);
    if (expected_proxy.empty() || expected_core.empty()) {
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::SourceUnavailable,
            "launcher proxy payload is unavailable");
    }
    if (has_enabled && has_disabled) {
        return Failure(
            game_directory, ProxyInstallationState::Conflict,
            ProxyInstallationError::Conflict,
            "enabled and disabled proxy files both exist");
    }
    const auto& candidate = has_enabled ? enabled : disabled;
    if (!Matches(candidate, expected_proxy) && !IsAnomalyProxy(candidate)) {
        return Failure(
            game_directory, ProxyInstallationState::Conflict,
            ProxyInstallationError::Conflict,
            "existing dwmapi proxy is not recognized as an Anomaly installation");
    }
    const auto installed_core = game_directory / kRuntimeDirectoryName / kCoreName;
    const auto installed_runtime = game_directory / kRuntimeDirectoryName;
    std::error_code runtime_error;
    if (!std::filesystem::is_directory(installed_runtime, runtime_error) || runtime_error ||
        IsReparsePoint(installed_runtime) || !IsRegularFile(installed_core)) {
        return Failure(
            game_directory, ProxyInstallationState::Conflict,
            ProxyInstallationError::RuntimeUnavailable,
            "Anomaly.Core.dll is missing from the game directory");
    }
    if (!Matches(installed_core, expected_core)) {
        return {ProxyInstallationState::UpdateAvailable,
                ProxyInstallationError::None, game_directory,
                "installed Anomaly Core differs from this launcher and can be updated"};
    }
    return {has_enabled ? ProxyInstallationState::Enabled
                        : ProxyInstallationState::Disabled,
            ProxyInstallationError::None, game_directory,
            has_enabled ? "Anomaly proxy is enabled"
                        : "Anomaly proxy is disabled for the next launch"};
}

ProxyInstallationStatus InstallProxyRuntime(
    const std::filesystem::path& game_directory,
    const ProxyInstallationSource& source) {
    if (!IsGameDirectory(game_directory)) {
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::InvalidGameDirectory,
            "selected directory does not contain HTGame.exe");
    }
    const std::string expected = Digest(source.proxy);
    const std::string expected_core = Digest(source.runtime_directory / kCoreName);
    if (expected.empty() || expected_core.empty()) {
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::SourceUnavailable,
            "launcher runtime payload is unavailable");
    }

    const auto current = InspectProxyInstallation(game_directory, source);
    if (current.state == ProxyInstallationState::Enabled ||
        current.state == ProxyInstallationState::Disabled ||
        current.state == ProxyInstallationState::Unavailable ||
        current.state == ProxyInstallationState::Conflict) {
        return current;
    }

    const auto runtime_target = game_directory / kRuntimeDirectoryName;
    const auto proxy_target = game_directory / kProxyName;
    const auto disabled_target = game_directory / kDisabledProxyName;
    std::error_code error;
    const bool has_enabled = std::filesystem::exists(proxy_target, error) && !error;
    const bool has_disabled = std::filesystem::exists(disabled_target, error) && !error;
    if (error || (has_enabled && has_disabled)) {
        return Failure(
            game_directory, ProxyInstallationState::Conflict,
            ProxyInstallationError::Conflict,
            "proxy state changed while installation was starting");
    }

    const bool updating = current.state == ProxyInstallationState::UpdateAvailable;
    if (updating != (has_enabled || has_disabled)) {
        return Failure(
            game_directory, ProxyInstallationState::Conflict,
            ProxyInstallationError::Conflict,
            "proxy state changed while installation was starting");
    }

    const bool has_runtime = std::filesystem::exists(runtime_target, error) && !error;
    if (error) {
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::IoFailure, "runtime state could not be read");
    }
    if (has_runtime && !updating) {
        if (!std::filesystem::is_directory(runtime_target, error) || error ||
            IsReparsePoint(runtime_target) || !IsRegularFile(runtime_target / kCoreName)) {
            return Failure(
                game_directory, ProxyInstallationState::Conflict,
                ProxyInstallationError::RuntimeUnavailable,
                "existing Anomaly runtime directory is incomplete");
        }
        if (!Matches(runtime_target / kCoreName, expected_core)) {
            return Failure(
                game_directory, ProxyInstallationState::Conflict,
                ProxyInstallationError::Conflict,
                "existing Anomaly.Core.dll does not match the launcher payload");
        }
    }

    const bool replace_runtime = updating || !has_runtime;
    const auto runtime_staging = UniqueSibling(game_directory, L".Anomaly.installing");
    const auto runtime_backup = UniqueSibling(game_directory, L".Anomaly.backup");
    if (replace_runtime) {
        std::string copy_error;
        if (!CopyRuntimeTree(source.runtime_directory, runtime_staging, copy_error) ||
            (updating && !MergeInstalledRuntime(
                runtime_target, runtime_staging, copy_error))) {
            std::filesystem::remove_all(runtime_staging, error);
            return Failure(
                game_directory, ProxyInstallationState::Unavailable,
                ProxyInstallationError::RuntimeUnavailable, std::move(copy_error));
        }
    }

    const auto proxy_staging = UniqueSibling(game_directory, L".dwmapi.dll.installing");
    std::filesystem::copy_file(
        source.proxy, proxy_staging, std::filesystem::copy_options::none, error);
    const bool copied = !error;
    const bool verified = copied && Matches(proxy_staging, expected);
    if (!verified) {
        std::filesystem::remove(proxy_staging, error);
        std::filesystem::remove_all(runtime_staging, error);
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            copied && !verified ? ProxyInstallationError::IntegrityFailure
                                : ProxyInstallationError::IoFailure,
            "proxy staging failed");
    }

    bool runtime_backed_up{};
    bool runtime_published{};
    const auto rollback_runtime = [&] {
        if (!replace_runtime) return;
        if (runtime_published) std::filesystem::remove_all(runtime_target, error);
        if (runtime_backed_up) {
            static_cast<void>(MovePath(runtime_backup, runtime_target));
        }
    };
    if (replace_runtime && has_runtime) {
        const DWORD move_error = MovePath(runtime_target, runtime_backup);
        if (move_error != ERROR_SUCCESS) {
            std::filesystem::remove_all(runtime_staging, error);
            std::filesystem::remove(proxy_staging, error);
            return Failure(
                game_directory, ProxyInstallationState::Unavailable,
                ProxyInstallationError::IoFailure,
                Win32Failure(
                    "installed runtime could not be prepared for update", move_error));
        }
        runtime_backed_up = true;
    }
    const DWORD publish_runtime_error = replace_runtime
        ? MovePath(runtime_staging, runtime_target) : ERROR_SUCCESS;
    if (publish_runtime_error != ERROR_SUCCESS) {
        rollback_runtime();
        std::filesystem::remove_all(runtime_staging, error);
        std::filesystem::remove(proxy_staging, error);
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::IoFailure,
            Win32Failure(
                "runtime directory could not be published", publish_runtime_error));
    }
    runtime_published = replace_runtime;

    const auto proxy_destination = has_disabled ? disabled_target : proxy_target;
    const auto proxy_backup = UniqueSibling(game_directory, L".dwmapi.dll.backup");
    bool proxy_backed_up{};
    const DWORD backup_proxy_error = updating
        ? MovePath(proxy_destination, proxy_backup) : ERROR_SUCCESS;
    if (backup_proxy_error != ERROR_SUCCESS) {
        rollback_runtime();
        std::filesystem::remove(proxy_staging, error);
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::IoFailure,
            Win32Failure(
                "installed proxy could not be prepared for update", backup_proxy_error));
    } else {
        proxy_backed_up = updating;
    }
    const DWORD publish_proxy_error = MovePath(proxy_staging, proxy_destination);
    if (publish_proxy_error != ERROR_SUCCESS) {
        if (proxy_backed_up) {
            static_cast<void>(MovePath(proxy_backup, proxy_destination));
        }
        rollback_runtime();
        std::filesystem::remove(proxy_staging, error);
        return Failure(
            game_directory, ProxyInstallationState::Unavailable,
            ProxyInstallationError::IoFailure,
            Win32Failure("proxy could not be published", publish_proxy_error));
    }
    if (proxy_backed_up) std::filesystem::remove(proxy_backup, error);
    if (runtime_backed_up) std::filesystem::remove_all(runtime_backup, error);
    return InspectProxyInstallation(game_directory, source);
}

ProxyInstallationStatus SetProxyEnabled(
    const std::filesystem::path& game_directory,
    const ProxyInstallationSource& source,
    bool enabled) {
    const auto current = InspectProxyInstallation(game_directory, source);
    if (current.state != ProxyInstallationState::Enabled &&
        current.state != ProxyInstallationState::Disabled) {
        return current;
    }
    if ((enabled && current.state == ProxyInstallationState::Enabled) ||
        (!enabled && current.state == ProxyInstallationState::Disabled)) {
        return current;
    }

    const auto proxy_source = game_directory /
        (enabled ? kDisabledProxyName : kProxyName);
    const auto destination = game_directory /
        (enabled ? kProxyName : kDisabledProxyName);
    if (MovePath(proxy_source, destination) != ERROR_SUCCESS) {
        return Failure(
            game_directory, current.state, ProxyInstallationError::IoFailure,
            "proxy state could not be changed");
    }
    return InspectProxyInstallation(game_directory, source);
}

}  // namespace anomaly::launcher
