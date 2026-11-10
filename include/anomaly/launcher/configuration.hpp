#pragma once

#include "anomaly/platform_ui_theme.hpp"

#include <Windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace anomaly::launcher {

enum class NteClient : std::uint8_t {
    MainlandChina,
    Global,
};

struct LauncherClientConfiguration final {
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
};

// The launcher's own background picture, named the way the theme files name theirs (relative to the
// Anomaly root). Stickers are the layout editor's job, as they are for the in-game window, so this
// holds the background and the two values that sit beside it: the fill mode and the veil drawn over
// it. The veil starts at zero -- the launcher paints its own body, and a veil on top of that body
// changes nothing anyone can see -- and a fill mode of -1 keeps whatever the layout says.
struct LauncherThemeArtwork final {
    std::string background;
    int background_mode{-1};
    float surface_opacity{0.0f};
};

// Classifies the effective, enabled background without reporting its path.
[[nodiscard]] bool IsCustomLauncherBackground(PlatformUiPalette palette,
    std::string_view background_path, bool enabled);

// How the launcher lays out its own pages. These are choices about this window -- the mods
// grid, its pager and the download page filters -- so they are stored with the rest of the
// launcher configuration instead of in a second file next to the payload.
// The page sizes the settings page offers. The value is stored as one of them: an older build let the
// size be any number from 1 to 200 through a slider, and a stored 134 would otherwise surface in a
// settings box that only lists four choices.
inline constexpr std::array<int, 4> kLauncherPageSizes{10, 20, 50, 100};

[[nodiscard]] inline int LauncherNearestPageSize(const int value) noexcept {
    int nearest = kLauncherPageSizes.front();
    for (const int size : kLauncherPageSizes) {
        if (std::abs(size - value) < std::abs(nearest - value)) nearest = size;
    }
    return nearest;
}

struct LauncherViewSettings final {
    // The grid is the default view, and it lists every mod at once unless pagination is asked
    // for: a page of covers is easy to scroll, and a pager over it only gets in the way.
    bool mods_grid{true};
    bool mods_pagination{false};
    int mods_page_size{10};
    std::string browse_sort{"default"};
    std::string browse_category;
    bool browse_hide_mature{true};
};

// Whether the launcher looks for a newer release on its own. It is the user's answer to the one
// question an update check asks -- whether this window may talk to the network unasked -- so it is
// stored with the launcher configuration rather than assumed, and a launcher that has never stored
// it checks: finding out that a build is outdated is the point of shipping the check at all.
// The endpoint is the mirror prefix that answered the last lookup (empty for the direct address).
// Asking it first is what makes a check take a second on a network where github.com itself is
// blocked, instead of spending the timeout budget on an address that is known not to answer here.
// The skipped version is the release the user answered "skip" to: the check may keep reporting it --
// the settings page still offers it -- but the launcher stops asking about that one version.
struct LauncherUpdateSettings final {
    bool auto_check{true};
    std::string endpoint;
    std::string skipped_version;
};

struct LauncherConfiguration final {
    NteClient selected_client{NteClient::MainlandChina};
    LauncherClientConfiguration mainland_china;
    LauncherClientConfiguration global;
    // The palette the launcher window wears, as the palette name the theme files use.
    // It is the launcher's own choice and is deliberately independent of the in-game
    // palette: the two windows are separate surfaces. An unknown or missing name keeps
    // the shipped default, Naiwa.
    std::string theme{"naiwa"};
    // The launcher's interface language: "auto" follows the Windows display language, and the
    // other two name a catalog explicitly. The in-game interface has its own setting, so this
    // one dresses this window alone. Empty means "never stored": a launcher that upgrades from a
    // build without this field starts from the language its runtime ini already names.
    std::string language;
    // Pictures of this window's own, and the colours the "custom" palette is built from. Both are
    // the launcher's choices: the in-game shell keeps its own theme files untouched.
    LauncherThemeArtwork artwork;
    anomaly::PlatformUiCustomColors custom_colors;
    LauncherViewSettings view;
    LauncherUpdateSettings update;

    [[nodiscard]] LauncherClientConfiguration& Selected() noexcept {
        return selected_client == NteClient::Global ? global : mainland_china;
    }

    [[nodiscard]] const LauncherClientConfiguration& Selected() const noexcept {
        return selected_client == NteClient::Global ? global : mainland_china;
    }
};

struct LauncherConfigurationLoadResult final {
    LauncherConfiguration configuration;
    bool loaded{};
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;
};

struct LauncherConfigurationSaveResult final {
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return win32_error == ERROR_SUCCESS; }
};

// What the launcher bootstrap starts. NTELauncher.exe is only a self-updating bootstrap: the
// launcher window and the platform pipe server live in the client it starts, and the bootstrap names
// that client in its own Config\Config.ini.
struct NteClientLaunchCommand final {
    std::filesystem::path executable;
    std::wstring arguments;
    // Set only when no client could be resolved, in which case `executable` is empty. It names what
    // was looked for and where: a caller that refuses to start the bootstrap has to say why, or the
    // refusal is indistinguishable from the launcher being misconfigured.
    std::string failure;
};

[[nodiscard]] NteClientLaunchCommand ResolveClientLaunchCommand(
    const std::filesystem::path& launcher_executable);

struct LauncherDiscoveryOptions final {
    NteClient client{NteClient::MainlandChina};
    std::filesystem::path payload_root;
    std::vector<std::filesystem::path> running_game_executables;
    std::vector<std::filesystem::path> running_launcher_executables;
    std::vector<std::filesystem::path> search_roots;
    bool include_system_locations{true};
    bool allow_unpaired_game_discovery{};
    // Never assign the other client's known installation to this client.
    std::filesystem::path other_game_directory;
};

[[nodiscard]] std::filesystem::path LauncherConfigurationPath(
    const std::filesystem::path& payload_root) noexcept;
[[nodiscard]] LauncherConfigurationLoadResult LoadLauncherConfiguration(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] LauncherConfigurationSaveResult SaveLauncherConfiguration(
    const std::filesystem::path& path,
    const LauncherConfiguration& configuration) noexcept;

[[nodiscard]] bool IsNteGameDirectory(
    const std::filesystem::path& directory) noexcept;
[[nodiscard]] bool IsNteLauncherExecutable(
    const std::filesystem::path& executable, NteClient client) noexcept;
[[nodiscard]] LauncherClientConfiguration DiscoverLauncherConfiguration(
    const LauncherClientConfiguration& preferred,
    const LauncherDiscoveryOptions& options);

}  // namespace anomaly::launcher
