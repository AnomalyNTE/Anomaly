#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anomaly::launcher {

inline constexpr std::size_t kMaxModGroupDepth = 5;
using ModGroupPath = std::vector<std::string>;

// The selection key names the mod itself. Its containing folders are the display groups.
inline ModGroupPath ModFolderGroups(std::string_view key) {
    if (key.ends_with('/')) key.remove_suffix(1);
    ModGroupPath path;
    for (auto slash = key.find('/'); slash != std::string_view::npos; slash = key.find('/')) {
        path.emplace_back(key.substr(0, slash));
        key.remove_prefix(slash + 1);
    }
    return path;
}

inline ModGroupPath NormalizeModGroupPath(ModGroupPath path) {
    path.resize((std::min)(path.size(), kMaxModGroupDepth));
    path.erase(std::find(path.begin(), path.end(), std::string{}), path.end());
    return path;
}

struct ModGroupingGroup {
    std::string name;
    std::vector<std::string> skins;
};
struct ModGroupingEntry {
    std::string name;
    std::vector<ModGroupingGroup> groups;
    ModGroupPath custom_path;
    ModGroupPath folder_path;
};
struct ModGroupingNode {
    ModGroupPath path;
    std::vector<std::size_t> children;
    std::size_t parent{};
    std::size_t first{}, count{}, direct_count{};
    bool explicit_group{};
};
struct ModGrouping {
    // Node zero is the root. Each subtree occupies one contiguous placement range,
    // with its direct mods first, followed by its children.
    std::vector<ModGroupingNode> nodes{1};
    std::vector<std::size_t> placements;
    std::vector<std::size_t> placement_nodes;
};

// Resolve the deepest surviving ancestor after search, filtering or regrouping.
inline std::size_t FindModGroup(const ModGrouping& layout, const ModGroupPath& path) {
    std::size_t node{};
    for (const auto& key : path) {
        const auto& children = layout.nodes[node].children;
        const auto found = std::find_if(children.begin(), children.end(), [&](const auto child) {
            return layout.nodes[child].path.back() == key;
        });
        if (found == children.end()) break;
        node = *found;
    }
    return node;
}

// Automatic classification keeps the existing shortcut for a single skin. An explicit
// custom level always remains navigable, even if it has just one child or one mod.
inline std::size_t ModGroupContent(const ModGrouping& layout, const std::size_t node) {
    const auto& group = layout.nodes[node];
    if (group.path.size() == 1 && group.direct_count == 0 && group.children.size() == 1) {
        const auto child = group.children.front();
        if (!layout.nodes[child].explicit_group && layout.nodes[child].children.empty()) return child;
    }
    return node;
}

inline ModGrouping BuildModGrouping(const std::vector<ModGroupingEntry>& entries, const bool by_folder = false) {
    ModGrouping result;
    std::vector<std::vector<std::size_t>> direct(1);
    const auto add = [&](const ModGroupPath& path, const std::size_t entry, const bool explicit_group) {
        std::size_t parent{};
        for (const auto& key : path) {
            const auto& children = result.nodes[parent].children;
            const auto found = std::find_if(children.begin(), children.end(), [&](const auto child) {
                return result.nodes[child].path.back() == key;
            });
            std::size_t node;
            if (found != children.end()) {
                node = *found;
            } else {
                node = result.nodes.size();
                auto child_path = result.nodes[parent].path;
                child_path.push_back(key);
                result.nodes[parent].children.push_back(node);
                result.nodes.push_back({std::move(child_path), {}, parent});
                direct.emplace_back();
            }
            result.nodes[node].explicit_group |= explicit_group;
            parent = node;
        }
        if (std::find(direct[parent].begin(), direct[parent].end(), entry) == direct[parent].end())
            direct[parent].push_back(entry);
    };
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (by_folder) {
            add(entry.folder_path, index, true);
            continue;
        }
        const auto custom = NormalizeModGroupPath(entry.custom_path);
        if (!custom.empty()) {
            add(custom, index, true);
        } else if (entry.groups.empty()) {
            add({""}, index, false);
        } else {
            for (const auto& group : entry.groups) {
                ModGroupPath path{group.name};
                if (!group.skins.empty() && !group.skins.front().empty()) path.push_back(group.skins.front());
                add(path, index, false);
            }
        }
    }
    const auto flatten = [&](auto&& self, const std::size_t node) -> void {
        auto& group = result.nodes[node];
        std::sort(group.children.begin(), group.children.end(), [&](const auto a, const auto b) {
            return result.nodes[a].path.back() < result.nodes[b].path.back();
        });
        std::sort(direct[node].begin(), direct[node].end(), [&](const auto a, const auto b) {
            return entries[a].name < entries[b].name;
        });
        group.first = result.placements.size();
        group.direct_count = direct[node].size();
        for (const auto entry : direct[node]) {
            result.placements.push_back(entry);
            result.placement_nodes.push_back(node);
        }
        for (const auto child : group.children) self(self, child);
        group.count = result.placements.size() - group.first;
    };
    flatten(flatten, 0);
    return result;
}

} // namespace anomaly::launcher
