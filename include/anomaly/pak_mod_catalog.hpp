#pragma once
#include "anomaly/directory_change_monitor.hpp"
#include "anomaly/pak_mod_settings.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace anomaly {
// A group token is two levels deep for character mods: `Player_051/female` is the `female` skin of
// the `Player_051` group, so every outfit of one character stays inside that character. `Vehicle_037`,
// `Pendant_07` and content directories name a group as a whole; content directories are the only
// tokens that start with '/', which is what makes the split unambiguous instead of a guess.
struct PakModGroupName {
    std::string_view group;
    std::string_view skin;
};
PakModGroupName ParsePakModGroup(std::string_view token);

struct PakModCatalogEntry {
    std::string key, name;
    std::set<std::string> assets, vehicles, model_vehicles;
    // Classified group tokens, see PakModGroupName. A mod that replaces two outfits of one
    // character carries both skins of that character, never a token per pair of them.
    std::set<std::string> resource_groups;
    // Index-only content fingerprints. Empty means unknown or inconsistent across containers.
    // IoStore chunk keys are raw 12-byte IDs, independent of directory mount-point spelling.
    std::map<std::string, std::string> asset_fingerprints, chunk_fingerprints;
    // Keep restricted rows internally so every container of a multi-pak mod is refused together.
    // User-facing enumerations omit them; forceLoad only overrides conflicts.
    bool enabled{}, disabled_files{}, index_complete{true}, restricted{};
    std::vector<std::filesystem::path> containers;
    std::string analysis_error;
};
// Reads only container indices, never resource payloads. Unsupported/encrypted indices are explicit.
std::vector<std::string> ReadModContainerIndex(const std::filesystem::path& file);
std::vector<PakModCatalogEntry> ReadPakModCatalog(const std::filesystem::path& directory,
    const nlohmann::json& settings);
bool PakModNameRestricted(std::string_view name);
// For explicit/manifest paths outside the managed mod directory, including their IoStore sidecar.
bool PakModFileRestricted(const std::filesystem::path& file);
// Before native startup scanning: rename restricted pak/utoc files to .disabled, never delete them.
// Throws on a failed rename so the caller cannot enable native loading after a partial operation.
void DisableRestrictedPakMods(const std::filesystem::path& directory);
// Owned by a caller that serializes Read. Stable polls share the immutable snapshot without walking
// the directory. File notifications and selection changes invalidate it; failed watches scan normally.
class PakModCatalogCache {
public:
    std::shared_ptr<const std::vector<PakModCatalogEntry>> Read(
        const std::filesystem::path& directory, const nlohmann::json& settings);
    [[nodiscard]] std::uint64_t MetadataScans() const noexcept { return metadata_scans_; }
private:
    DirectoryChangeMonitor changes_;
    bool dirty_{true};
    std::uint64_t metadata_scans_{};
    std::filesystem::path directory_;
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>> files_;
    nlohmann::json states_;
    std::shared_ptr<const std::vector<PakModCatalogEntry>> snapshot_;
};
bool PakModsConflict(const PakModCatalogEntry& a, const PakModCatalogEntry& b);
void SelectPakMod(nlohmann::json& settings, const std::vector<PakModCatalogEntry>& catalog,
    std::string_view key, bool enabled);
// Serializes read/modify/publish across Launcher and Runtime, preserving unrelated JSON keys.
void UpdatePakModSettings(const std::vector<std::filesystem::path>& paths,
    const std::function<void(nlohmann::json&)>& update);
} // namespace anomaly
