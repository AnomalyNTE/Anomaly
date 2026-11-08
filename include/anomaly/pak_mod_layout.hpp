#pragma once
// `~mods` holds two kinds of folder: a mod, which is the unit a selection is switched by, and a
// category folder, which only exists to group mods ("角色/", "Vehicles/"). What tells them apart is
// what a folder contains, never what it is called:
//  * a folder that directly holds a container is the mod;
//  * a folder that ships the files a mod is distributed with (`mod.json`, a cover, notes) is the mod;
//  * a folder of folders is a category when at least one child holds a mod deeper down, unless that
//    child is named like the container layout the engine expects ("Paks/", "Content/", "HT/"),
//    which belongs to the mod above it.
// A folder above a layout folder therefore stays one mod (`MyMod/Paks/x.pak`), a hand-made category
// folder becomes a category (`角色/ModA/x.pak`, `角色/ModB/Paks/y.pak`), and a mod that replaces two
// outfits is still one key.
#include <cstddef>
#include <set>
#include <string>
#include <string_view>

namespace anomaly {

// One folder name, lowercased, that is part of a container layout rather than a folder of mods.
// A mod shipped as `Name/Paks/…` or `Name/Content/Paks/…` keeps its own folder as its key.
inline bool PakModLayoutFolder(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (const char c : name)
        lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    for (const std::string_view known : {"paks", "content", "ht", "windowsnoeditor", "logicmods",
             "paks_mods", "binaries", "data", "config"})
        if (lower == known) return true;
    return false;
}

// A file a mod ships besides its containers: the metadata and the cover the mods page reads. They
// mark the folder that holds them as a mod even when the containers sit deeper inside it.
inline bool PakModShippedFile(std::string_view logical_name) {
    const auto slash = logical_name.rfind('/');
    std::string name(logical_name.substr(slash == std::string_view::npos ? 0 : slash + 1));
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (name == "mod.json" || name == "anomaly-source.json" || name == "mod-notes.json") return true;
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const auto extension = name.substr(dot);
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" &&
        extension != ".webp")
        return false;
    const auto stem = name.substr(0, dot);
    return stem == "icon" || stem == "cover" || stem == "preview" || stem == "thumbnail";
}

// The folder the last path segment names: what a mod is listed and organised by.
inline std::string PakModFolderName(std::string_view relative) {
    const auto slash = relative.rfind('/');
    return std::string(slash == std::string_view::npos ? relative : relative.substr(slash + 1));
}

// Classifies folders by the files registered in them. Callers register every file of the tree once
// and then ask for the mod folder of each container, so both the catalogue and the launcher agree on
// what one mod is without either of them deciding it alone.
class PakModLayout final {
public:
    // Registers a container or a shipped file: `logical` is its path relative to the mods
    // directory with every ".disabled" marker stripped and '/' separators.
    void AddOwnerFile(const std::string& logical) {
        const auto slash = logical.rfind('/');
        const std::string directory =
            slash == std::string::npos ? std::string{} : logical.substr(0, slash);
        owners_.insert(directory);
        if (directory.empty()) return;
        // The folders above an owner are categories, up to the first layout folder: everything
        // under one of those belongs to a single mod.
        std::size_t begin = 0;
        std::string_view prefix;
        while (true) {
            const auto end = directory.find('/', begin);
            const std::string_view segment = std::string_view(directory).substr(begin,
                end == std::string::npos ? std::string::npos : end - begin);
            if (PakModLayoutFolder(segment)) break;
            groupings_.emplace(prefix);
            if (end == std::string::npos) break;
            prefix = std::string_view(directory).substr(0, end);
            begin = end + 1;
        }
    }
    // The folder that owns the container at `logical`, as a path relative to the mods directory.
    // Empty when the container lies in the mods directory itself, which is a mod of its own.
    [[nodiscard]] std::string Root(const std::string& logical) const {
        const auto slash = logical.rfind('/');
        if (slash == std::string::npos) return {};
        const std::string_view directory = std::string_view(logical).substr(0, slash);
        for (std::size_t begin = 0;;) {
            const auto end = directory.find('/', begin);
            const std::string prefix(directory.substr(0, end));
            if (owners_.contains(prefix)) return prefix;
            if (!groupings_.contains(prefix)) return prefix;
            if (end == std::string_view::npos) return prefix;
            begin = end + 1;
        }
    }

private:
    // Folders that directly hold a container or a shipped file.
    std::set<std::string> owners_;
    // Folders whose child folders are mods: registered for every folder on the way down to an
    // owner, so a category is found at any depth without a second walk of the tree.
    std::set<std::string> groupings_;
};

} // namespace anomaly
