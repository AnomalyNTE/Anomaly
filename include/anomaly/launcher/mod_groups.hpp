#pragma once
// The mods page classifies every mod from the containers it replaces: a category (a character, a
// vehicle model, a content folder) and, inside it, the skins of that character. This turns that
// per-mod classification into the flat ranges both views draw, so the page itself stays a
// rendering problem and the arithmetic has one place to be checked.
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace anomaly::launcher {

// One mod's classification: the categories it belongs to, each with the skins it covers. An empty
// skin list means the mod replaces the category as a whole.
struct ModGroupingGroup {
    std::string name;
    std::vector<std::string> skins;
};
struct ModGroupingEntry {
    std::string name;  // display name, orders the mods inside one skin
    std::vector<ModGroupingGroup> groups;
};
// A skin of a category. `key` is the raw skin name, and empty for the category's own resources.
struct ModGroupingSkin {
    std::string key;
    std::size_t first{}, count{};
};
struct ModGroupingCategory {
    std::string key;
    std::size_t first{}, count{};
    std::size_t skins_first{}, skins_count{};
    // The skins deserve a level of their own only when there is more than one and at least one of
    // them is named.
    bool named_skins{};
};
struct ModGrouping {
    std::vector<ModGroupingCategory> categories;
    std::vector<ModGroupingSkin> skins;
    // Indices into the entries, in display order: one per mod per category. A mod is filed under
    // the first skin it covers, so no category lists a mod twice and the skins of a category add
    // up to its count.
    std::vector<std::size_t> placements;
};

inline const ModGroupingCategory* FindModCategory(const ModGrouping& layout, const std::string& key) {
    for (const auto& category : layout.categories)
        if (category.key == key) return &category;
    return nullptr;
}
inline const ModGroupingSkin* FindModSkin(const ModGrouping& layout,
    const ModGroupingCategory& category, const std::string& key) {
    for (std::size_t index = category.skins_first;
         index < category.skins_first + category.skins_count; ++index) {
        if (layout.skins[index].key == key) return &layout.skins[index];
    }
    return nullptr;
}
// The skin one placement belongs to: a list row knows its placement and needs the header it sits
// under, and the skins of a category are a partition of its placements.
inline const ModGroupingSkin* FindModSkinAt(const ModGrouping& layout,
    const ModGroupingCategory& category, const std::size_t placement) {
    for (std::size_t index = category.skins_first;
         index < category.skins_first + category.skins_count; ++index) {
        const auto& skin = layout.skins[index];
        if (placement >= skin.first && placement < skin.first + skin.count) return &skin;
    }
    return nullptr;
}
inline ModGrouping BuildModGrouping(const std::vector<ModGroupingEntry>& entries) {
    ModGrouping result;
    std::vector<std::string> keys;
    std::vector<std::vector<std::pair<std::string, std::size_t>>> pending;
    const auto slot = [&](const std::string& key) {
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (keys[index] == key) return index;
        }
        keys.push_back(key);
        pending.emplace_back();
        result.categories.push_back(ModGroupingCategory{key, 0, 0, 0, 0, false});
        return keys.size() - 1;
    };
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (entry.groups.empty()) {
            pending[slot(std::string{})].emplace_back(std::string{}, index);
            continue;
        }
        for (const auto& group : entry.groups) {
            const auto category = slot(group.name);
            pending[category].emplace_back(
                group.skins.empty() ? std::string{} : group.skins.front(), index);
        }
    }
    for (std::size_t index = 0; index < result.categories.size(); ++index) {
        auto& category = result.categories[index];
        auto rows = std::move(pending[index]);
        // One skin is one run of rows in both views.
        std::sort(rows.begin(), rows.end(), [&](const auto& left, const auto& right) {
            if (left.first != right.first) return left.first < right.first;
            return entries[left.second].name < entries[right.second].name;
        });
        category.first = result.placements.size();
        category.count = rows.size();
        category.skins_first = result.skins.size();
        for (auto& row : rows) {
            if (result.skins.size() == category.skins_first || result.skins.back().key != row.first)
                result.skins.push_back(ModGroupingSkin{row.first, result.placements.size(), 0});
            ++result.skins.back().count;
            result.placements.push_back(row.second);
        }
        category.skins_count = result.skins.size() - category.skins_first;
        if (category.skins_count < 2) continue;
        for (std::size_t skin = category.skins_first;
             skin < category.skins_first + category.skins_count; ++skin) {
            if (!result.skins[skin].key.empty()) {
                category.named_skins = true;
                break;
            }
        }
    }
    return result;
}

} // namespace anomaly::launcher
