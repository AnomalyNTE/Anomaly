#pragma once
#include "anomaly/pak_mod_settings.hpp"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace anomaly {
struct PakModCatalogEntry {
    std::string key, name;
    std::set<std::string> assets, vehicles, model_vehicles, resource_groups;
    // Index-only content fingerprints. Empty means unknown or inconsistent across containers.
    // IoStore chunk keys are raw 12-byte IDs, independent of directory mount-point spelling.
    std::map<std::string, std::string> asset_fingerprints, chunk_fingerprints;
    bool enabled{}, disabled_files{}, index_complete{true};
    std::string analysis_error;
};
// Reads only container indices, never resource payloads. Unsupported/encrypted indices are explicit.
std::vector<std::string> ReadModContainerIndex(const std::filesystem::path& file);
std::vector<PakModCatalogEntry> ReadPakModCatalog(const std::filesystem::path& directory,
    const nlohmann::json& settings);
// Owned by a caller that serializes Read. Stable polls inspect metadata only and share the
// immutable snapshot; paths, file sizes, write times and selection changes invalidate it.
class PakModCatalogCache {
public:
    std::shared_ptr<const std::vector<PakModCatalogEntry>> Read(
        const std::filesystem::path& directory, const nlohmann::json& settings);
private:
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
