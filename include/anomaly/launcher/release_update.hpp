#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly::launcher {

// The repository that publishes this launcher, and the asset its runtime ZIP is published as.
// Both are release data rather than translation: a translated repository name would point at a
// release that does not exist.
inline constexpr std::string_view kReleaseRepository{"AnomalyNTE/Anomaly"};
inline constexpr std::string_view kReleasePageUrl{
    "https://github.com/AnomalyNTE/Anomaly/releases/latest"};
inline constexpr std::string_view kReleaseRuntimeAssetSuffix{"-runtime.zip"};
inline constexpr std::wstring_view kLauncherExecutableName{L"AnomalyLauncher.exe"};
// The mirrors the release is also published on. They are for the reader who cannot download a 28 MB
// package from github.com at all: the launcher's own download stays on the release's address, and
// these are offered as the alternative rather than used silently.
inline constexpr std::wstring_view kReleaseQuarkUrl{
    L"https://pan.quark.cn/s/81a25f50de52?pwd=gGdV"};
inline constexpr std::wstring_view kReleaseBaiduUrl{
    L"https://pan.baidu.com/s/1x5PpiYT8GM2z0_axmcAJ9w?pwd=hfs5"};

// The release lookup and the package download both address github.com, which a restricted network
// may not reach at all. These mirrors take an absolute github.com URL behind their own host and
// forward it, so the same request can be asked through them. They carry the lookup only: the package
// itself is downloaded from the release's own address, and the netdisk buttons are what a user who
// cannot reach it at all is offered instead.
inline constexpr std::array<std::string_view, 5> kGithubProxyPrefixes{
    "https://v4.gh-proxy.org/",
    "https://gh-proxy.org/",
    "https://v6.gh-proxy.org/",
    "https://cdn.gh-proxy.org/",
    "https://axisnow.gh-proxy.org/",
};

struct ReleaseAsset final {
    std::string name;
    std::string download_url;
    std::uint64_t size{};
    // The digest GitHub publishes for the asset, as the hex of a SHA-256. It is the only checksum a
    // release carries, so it is required before a package can be installed automatically.
    std::string sha256;
};

struct ReleaseInfo final {
    // The tag as published ("v1.2.3") and the version it names ("1.2.3").
    std::string tag;
    std::string version;
    std::string page_url;
    // The release notes, as the release publishes them. They are markdown, and the launcher shows
    // them as text: the point is to let the user read what changes before installing.
    std::string notes;
    std::vector<ReleaseAsset> assets;
};

// Parses the body of GET /repos/<repository>/releases/latest. An empty result means the answer
// was not a release at all: the API answers {"message": "..."} for a repository without one and
// for an exhausted rate limit, and neither of those is an update.
[[nodiscard]] std::optional<ReleaseInfo> ParseLatestRelease(std::string_view body);

// The runtime ZIP of a release, and only that asset: a release also publishes a tools ZIP and an
// SDK ZIP, and installing either of them over the launcher would leave a broken installation.
[[nodiscard]] const ReleaseAsset* FindRuntimeAsset(const ReleaseInfo& release);

enum class ReleaseComparison : std::uint8_t {
    UpToDate,
    Newer,
    // One of the two versions is not a semantic version, so nothing may be concluded from them.
    Unknown,
};

[[nodiscard]] ReleaseComparison CompareRelease(
    std::string_view local_version, const ReleaseInfo& release) noexcept;

enum class PackageCheck : std::uint8_t {
    Ok,
    // What arrived is not the length the release published.
    Size,
    // What arrived hashes to something else than the digest the release published.
    Digest,
    // The file could not be read at all.
    Unreadable,
};

struct PackageVerification final {
    PackageCheck check{PackageCheck::Ok};
    // What the download actually is, kept for the log line and manual comparison.
    std::uint64_t size{};
    std::string sha256;

    [[nodiscard]] bool Ok() const noexcept { return check == PackageCheck::Ok; }
};

// Checks a downloaded package against what the release published for it. This is what stands between
// a mirror -- which may answer with a truncated file, an error page or someone else's bytes -- and a
// launcher that would otherwise extract and run whatever arrived.
[[nodiscard]] PackageVerification VerifyReleasePackage(
    const std::filesystem::path& archive, const ReleaseAsset& asset);

struct PublishResult final {
    // Why the package was not installed, empty when it was.
    std::string failure;
    // The running image was replaced: the caller has to start the new one, because this process
    // cannot outlive the image it was loaded from.
    bool restart_required{};

    [[nodiscard]] bool Ok() const noexcept { return failure.empty(); }
};

// Copies an extracted launcher package over the installation it belongs to. Files the user owns
// -- the launcher configuration and the runtime's own ini, repository, state and log files --
// keep their contents, and every other file the package ships is replaced. The running image is
// moved aside rather than overwritten, which Windows allows while the image is mapped.
[[nodiscard]] PublishResult PublishStagedRelease(
    const std::filesystem::path& staging, const std::filesystem::path& install_root);

// Removes the images a previous update renamed and left behind. They cannot be deleted while the
// launcher that produced them is running, so this succeeds at the start of the next launch.
void RemoveUpdateLeftovers(const std::filesystem::path& install_root) noexcept;

}  // namespace anomaly::launcher
