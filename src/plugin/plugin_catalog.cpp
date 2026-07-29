#include "anomaly/plugin_catalog.hpp"

#include "anomaly/plugin_package.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>

namespace anomaly {
namespace {

std::optional<std::string> ReadManifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaximumPluginManifestBytes) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!data.empty()) input.read(data.data(), size);
    if (!input && size != 0) return std::nullopt;
    return data;
}

std::string ManifestCode(PluginManifestErrorCode code) {
    return "manifest-" + std::to_string(static_cast<std::uint16_t>(code));
}

}  // namespace

PluginCatalogSnapshot::PluginCatalogSnapshot(std::vector<PluginCatalogEntry> entries)
    : entries_(std::move(entries)) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const PluginCatalogEntry& entry = entries_[index];
        if (entry.LoadCandidate() && entry.manifest) index_.emplace(entry.manifest->id, index);
    }
}

const PluginCatalogEntry* PluginCatalogSnapshot::Find(std::string_view id) const noexcept {
    const auto found = index_.find(std::string(id));
    return found == index_.end() ? nullptr : &entries_[found->second];
}

std::vector<AvailablePluginVersion> PluginCatalogSnapshot::AvailablePlugins() const {
    std::vector<AvailablePluginVersion> result;
    result.reserve(index_.size());
    for (const PluginCatalogEntry& entry : entries_) {
        if (entry.LoadCandidate() && entry.manifest) {
            result.push_back({entry.manifest->id, entry.manifest->version});
        }
    }
    std::sort(result.begin(), result.end(),
        [](const AvailablePluginVersion& left, const AvailablePluginVersion& right) {
            return left.id < right.id;
        });
    return result;
}

PluginCatalogSnapshot DiscoverPluginCatalog(
    const std::filesystem::path& plugin_root,
    const std::optional<PluginCompatibilityContext>& compatibility) {
    std::vector<std::filesystem::path> packages;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(plugin_root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->path().filename() == L".cache") continue;
        if (iterator->is_directory(error) && !iterator->is_symlink(error)) {
            packages.push_back(iterator->path());
        }
        error.clear();
    }
    std::sort(packages.begin(), packages.end());

    std::vector<PluginCatalogEntry> entries;
    entries.reserve(packages.size());
    for (const std::filesystem::path& package : packages) {
        PluginCatalogEntry entry;
        entry.package_root = package;
        entry.manifest_file = package / L"manifest.json";
        const std::optional<std::string> text = ReadManifest(entry.manifest_file);
        if (!text) {
            entry.issues.push_back({"manifest-read", "/", "manifest.json is missing or unreadable"});
            entries.push_back(std::move(entry));
            continue;
        }
        PluginManifestParseResult parsed = ParsePluginManifest(*text);
        if (!parsed.Ok()) {
            for (const PluginManifestDiagnostic& diagnostic : parsed.diagnostics) {
                entry.issues.push_back(
                    {ManifestCode(diagnostic.code), diagnostic.path, diagnostic.message});
            }
            entries.push_back(std::move(entry));
            continue;
        }
        entry.manifest = std::move(parsed.manifest);
        const PluginPackagePathResult confined = OpenConfinedPluginPackageFile(
            package, entry.manifest->entry, true);
        if (!confined.Ok()) {
            entry.status = PluginCatalogStatus::InvalidPackage;
            entry.issues.push_back({
                "package-" + std::to_string(static_cast<std::uint16_t>(confined.error)),
                "/entry", confined.message});
            entries.push_back(std::move(entry));
            continue;
        }
        entry.entry_file = confined.path;
        entry.status = PluginCatalogStatus::Valid;
        if (compatibility) {
            entry.compatibility = EvaluatePluginCompatibility(*entry.manifest, *compatibility);
            if (!entry.compatibility->Compatible()) {
                entry.status = PluginCatalogStatus::Incompatible;
                for (const PluginCompatibilityIssue& issue : entry.compatibility->issues) {
                    if (!issue.blocking) continue;
                    entry.issues.push_back({
                        "compatibility-" +
                            std::to_string(static_cast<std::uint16_t>(issue.code)),
                        issue.path, issue.subject + " does not satisfy " + issue.expected});
                }
            }
        }
        entries.push_back(std::move(entry));
    }

    std::unordered_map<std::string, std::vector<std::size_t>> by_id;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].manifest) by_id[entries[index].manifest->id].push_back(index);
    }
    for (const auto& [id, matches] : by_id) {
        if (matches.size() < 2) continue;
        for (const std::size_t index : matches) {
            entries[index].status = PluginCatalogStatus::DuplicateId;
            entries[index].issues.push_back(
                {"duplicate-id", "/id", "plugin id appears in more than one package: " + id});
        }
    }
    return PluginCatalogSnapshot(std::move(entries));
}

}  // namespace anomaly
