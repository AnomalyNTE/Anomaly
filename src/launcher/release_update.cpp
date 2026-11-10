#include "anomaly/launcher/release_update.hpp"

#include "anomaly/artifact_crypto.hpp"
#include "anomaly/launcher/proxy_installation.hpp"
#include "anomaly/semver.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace anomaly::launcher {
namespace {

using Json = nlohmann::json;

// Leftover images of this executable. The name carries what produced them so a directory that
// happens to hold another file called AnomalyLauncher.exe.old-* is never touched by mistake.
constexpr std::wstring_view kReplacedImagePrefix{L"AnomalyLauncher.exe.old-"};

const std::filesystem::path kImageName{std::wstring(kLauncherExecutableName)};
// The core beside the launcher. A package without it is not a launcher package, and publishing one
// would replace a working launcher with a tree that cannot start a runtime.
const std::filesystem::path kCorePath = std::filesystem::path(L"Anomaly") / L"Anomaly.Core.dll";

// A package is extracted data, but a link inside it is still a link: following one would let the
// archive write outside the installation. The same test the proxy installer applies to its own
// package, because both are the same kind of input.
bool IsReparsePoint(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsSha256(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// SemVer tags contain only URL-safe characters except '+', which GitHub may percent-encode.
std::string EncodeVersionPath(std::string_view value) {
    std::string encoded;
    for (const char c : value) {
        if (c == '+') encoded += "%2B";
        else encoded += c;
    }
    return encoded;
}

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// The release tag is the version with the "v" the workflow prepends for tags.
std::string TagToVersion(std::string_view tag) {
    if (tag.starts_with('v') || tag.starts_with('V')) return std::string(tag.substr(1));
    return std::string(tag);
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

// Files of the installation the package must not overwrite. The launcher configuration is one of
// them, and the runtime subtree keeps the same list the proxy installer does: its ini, repository
// configuration and the state, log and crash directories belong to the user.
bool PreservesUserFile(const std::filesystem::path& relative) {
    if (relative == L"AnomalyLauncher.json") return true;
    auto component = relative.begin();
    if (component == relative.end() || *component != L"Anomaly") return false;
    std::filesystem::path inner;
    for (++component; component != relative.end(); ++component) inner /= *component;
    return !inner.empty() && PreservesInstalledRuntimeFile(inner);
}

}  // namespace

std::optional<ReleaseInfo> ParseLatestRelease(std::string_view body) {
    if (body.empty()) return std::nullopt;
    try {
        const Json document = Json::parse(body);
        if (!document.is_object()) return std::nullopt;
        const auto tag = document.find("tag_name");
        if (tag == document.end() || !tag->is_string()) return std::nullopt;
        ReleaseInfo release;
        release.tag = tag->get<std::string>();
        if (release.tag.empty()) return std::nullopt;
        release.version = TagToVersion(release.tag);
        // Mirror metadata is not a trusted ShellExecute target. Build the page address ourselves.
        release.page_url = ParseSemanticVersion(release.version).has_value()
            ? "https://github.com/" + std::string(kReleaseRepository) + "/releases/tag/" +
                EncodeVersionPath(release.tag)
            : std::string(kReleasePageUrl);
        const auto notes = document.find("body");
        if (notes != document.end() && notes->is_string()) release.notes = notes->get<std::string>();
        const auto assets = document.find("assets");
        if (assets != document.end() && assets->is_array()) {
            for (const auto& entry : *assets) {
                if (!entry.is_object()) continue;
                const auto name = entry.find("name");
                const auto url = entry.find("browser_download_url");
                if (name == entry.end() || !name->is_string() ||
                    url == entry.end() || !url->is_string()) {
                    continue;
                }
                ReleaseAsset asset;
                asset.name = name->get<std::string>();
                asset.download_url = url->get<std::string>();
                if (const auto size = entry.find("size");
                    size != entry.end() && size->is_number_unsigned()) {
                    asset.size = size->get<std::uint64_t>();
                }
                // Missing or unsupported digests leave the asset ineligible for automatic install.
                if (const auto digest = entry.find("digest");
                    digest != entry.end() && digest->is_string()) {
                    const std::string value = digest->get<std::string>();
                    constexpr std::string_view prefix{"sha256:"};
                    if (value.starts_with(prefix)) asset.sha256 = value.substr(prefix.size());
                }
                release.assets.push_back(std::move(asset));
            }
        }
        return release;
    } catch (...) {
        // A rate-limit or error answer is an object without a tag; anything unparsable is not a
        // release either, and an update check is not the place to fail loudly.
        return std::nullopt;
    }
}

const ReleaseAsset* FindRuntimeAsset(const ReleaseInfo& release) {
    if (!ParseSemanticVersion(release.version).has_value() ||
        TagToVersion(release.tag) != release.version) return nullptr;
    const std::string name = "Anomaly-" + release.version + std::string(kReleaseRuntimeAssetSuffix);
    const std::string prefix = "https://github.com/" + std::string(kReleaseRepository) +
        "/releases/download/";
    const std::string path = release.tag + '/' + name;
    for (const auto& asset : release.assets) {
        // A github.com host alone also permits executable packages from unrelated repositories.
        if (asset.name == name && asset.size != 0 && IsSha256(asset.sha256) &&
            (asset.download_url == prefix + path ||
             asset.download_url == prefix + EncodeVersionPath(path))) {
            return &asset;
        }
    }
    return nullptr;
}

ReleaseComparison CompareRelease(
    std::string_view local_version, const ReleaseInfo& release) noexcept {
    const auto local = ParseSemanticVersion(local_version);
    const auto remote = ParseSemanticVersion(release.version);
    if (!local.has_value() || !remote.has_value()) return ReleaseComparison::Unknown;
    return CompareSemanticVersionPrecedence(*remote, *local) > 0
        ? ReleaseComparison::Newer : ReleaseComparison::UpToDate;
}

PackageVerification VerifyReleasePackage(
    const std::filesystem::path& archive, const ReleaseAsset& asset) {
    PackageVerification verification;
    if (!IsRegularFile(archive)) {
        verification.check = PackageCheck::Unreadable;
        return verification;
    }
    std::error_code error;
    verification.size = std::filesystem::file_size(archive, error);
    if (error) {
        verification.check = PackageCheck::Unreadable;
        return verification;
    }
    // The length first: it is published for every asset and it already catches a truncated transfer.
    if (asset.size == 0 || verification.size != asset.size) {
        verification.check = PackageCheck::Size;
        return verification;
    }
    verification.sha256 = anomaly::Sha256FileHex(archive);
    if (verification.sha256.empty()) {
        verification.check = PackageCheck::Unreadable;
        return verification;
    }
    if (!IsSha256(asset.sha256) ||
        !anomaly::ConstantTimeHexEqual(asset.sha256, verification.sha256)) {
        verification.check = PackageCheck::Digest;
        return verification;
    }
    verification.check = PackageCheck::Ok;
    return verification;
}

PublishResult PublishStagedRelease(
    const std::filesystem::path& staging, const std::filesystem::path& install_root) {
    PublishResult result;
    std::error_code error;
    if (staging.empty() || install_root.empty()) {
        result.failure = "update staging or installation path is unavailable";
        return result;
    }
    const auto staged_image = staging / kImageName;
    if (!IsRegularFile(staged_image)) {
        result.failure = "update package does not contain AnomalyLauncher.exe";
        return result;
    }
    if (!IsRegularFile(staging / kCorePath)) {
        result.failure = "update package does not contain Anomaly\\Anomaly.Core.dll";
        return result;
    }
    if (!std::filesystem::is_directory(install_root, error) || error) {
        result.failure = "launcher installation directory is unavailable";
        return result;
    }
    if (IsReparsePoint(staging) || IsReparsePoint(install_root)) {
        result.failure = "update staging or installation directory is a link";
        return result;
    }
    if (!IsRegularFile(install_root / kImageName)) {
        result.failure = "installed AnomalyLauncher.exe is unavailable";
        return result;
    }

    // Check the complete destination before replacing any file. A loaded DLL, a locked launcher or
    // a file where the package expects a directory must not leave the core and launcher mismatched.
    struct Entry { std::filesystem::path relative; bool directory; };
    std::vector<Entry> entries;
    for (std::filesystem::recursive_directory_iterator iterator(staging, error), end;
         iterator != end && !error; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (IsReparsePoint(entry.path())) {
            result.failure = "update package contains a link";
            return result;
        }
        const auto relative = entry.path().lexically_relative(staging);
        if (error || relative.empty()) {
            result.failure = "update package path is not confined";
            return result;
        }
        const auto target = install_root / relative;
        const bool directory = entry.is_directory(error);
        if (error || (!directory && !entry.is_regular_file(error))) {
            result.failure = "update package contains an unsupported entry";
            return result;
        }
        if (IsReparsePoint(target)) {
            result.failure = "update destination contains a link: " + PathText(relative);
            return result;
        }
        const bool exists = std::filesystem::exists(target, error);
        if (error || (exists && directory != std::filesystem::is_directory(target, error))) {
            result.failure = "update destination has an incompatible type: " + PathText(relative);
            return result;
        }
        if (exists && !directory) {
            if (PreservesUserFile(relative)) continue;
            const DWORD access = relative == kImageName ? DELETE : GENERIC_WRITE;
            const HANDLE file = CreateFileW(target.c_str(), access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                result.failure = "update destination is not writable: " + PathText(relative) +
                    " (Win32 " + std::to_string(GetLastError()) + ')';
                return result;
            }
            CloseHandle(file);
        }
        entries.push_back({relative, directory});
    }
    if (error) {
        result.failure = "update package could not be read";
        return result;
    }
    // Create missing directories before copying payloads, so a directory creation failure also
    // leaves existing binaries untouched. Preserved files already present were excluded above.
    for (const auto& entry : entries) {
        if (!entry.directory) continue;
        std::filesystem::create_directories(install_root / entry.relative, error);
        if (error) {
            result.failure = "update directory could not be created: " + PathText(entry.relative);
            return result;
        }
    }
    for (const auto& entry : entries) {
        if (entry.directory || entry.relative == kImageName) continue;
        std::filesystem::copy_file(staging / entry.relative, install_root / entry.relative,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            result.failure = "update file could not be written: " + PathText(entry.relative);
            return result;
        }
    }

    // The image itself. Windows lets a mapped image be renamed but not overwritten, so the running
    // launcher steps aside and the new one takes its name; the caller starts it, and this process
    // exits holding the renamed image, which the next launch removes.
    const auto installed_image = install_root / kImageName;
    const auto replaced_image = install_root / (std::wstring(kReplacedImagePrefix) +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (MoveFileExW(installed_image.c_str(), replaced_image.c_str(), MOVEFILE_WRITE_THROUGH) ==
        FALSE) {
        result.failure = "running launcher could not be moved aside (Win32 " +
            std::to_string(GetLastError()) + ')';
        return result;
    }
    if (CopyFileW(staged_image.c_str(), installed_image.c_str(), FALSE) == FALSE) {
        const DWORD copy_error = GetLastError();
        MoveFileExW(replaced_image.c_str(), installed_image.c_str(), MOVEFILE_REPLACE_EXISTING);
        result.failure = "new launcher could not be installed (Win32 " +
            std::to_string(copy_error) + ')';
        return result;
    }
    result.restart_required = true;
    return result;
}

void RemoveUpdateLeftovers(const std::filesystem::path& install_root) noexcept {
    std::error_code error;
    if (install_root.empty() || !std::filesystem::is_directory(install_root, error) || error) {
        return;
    }
    for (std::filesystem::directory_iterator iterator(install_root, error), end;
         iterator != end && !error; iterator.increment(error)) {
        std::error_code entry_error;
        if (!iterator->is_regular_file(entry_error) || entry_error) continue;
        if (!iterator->path().filename().wstring().starts_with(kReplacedImagePrefix)) continue;
        std::error_code remove_error;
        std::filesystem::remove(iterator->path(), remove_error);
    }
}

}  // namespace anomaly::launcher
