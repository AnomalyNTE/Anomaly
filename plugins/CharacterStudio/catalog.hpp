#pragma once
#include "ue.hpp"
#include <map>
#include <set>

namespace character_studio {
struct Fashion {
    Name id{};
    std::string label;
};
struct Character {
    Name id{};
    std::string code, display_name;
    std::vector<Fashion> fashions;
};
struct CatalogRow {
    Character character;
    Name default_fashion{};
    std::vector<Fashion> fashions;
};

// Join authored table references only. Mesh residency and player ownership do not
// determine whether a character belongs in the catalog.
inline std::vector<Character> BuildCatalog(
    std::vector<CatalogRow> rows,
    const std::set<std::uint64_t>& defaults,
    const std::map<std::uint64_t, Name>& fashion_owners,
    const std::map<std::uint64_t, std::vector<Fashion>>& fashion_options) {
    std::sort(rows.begin(), rows.end(), [](const CatalogRow& a, const CatalogRow& b) {
        return a.character.code < b.character.code;
    });
    std::vector<Character> result;
    for (auto& row : rows) {
        auto& item = row.character;
        if (!defaults.contains(Key(item.id))) {
            const auto fashion = fashion_owners.find(Key(row.default_fashion));
            const auto options = fashion_options.find(Key(item.id));
            const bool has_matching_default = row.default_fashion.index &&
                fashion != fashion_owners.end() && fashion->second == item.id;
            if (!has_matching_default && (options == fashion_options.end() ||
                options->second.empty())) continue;
        }
        if (item.display_name.empty())
            item.display_name = item.code;
        item.fashions = std::move(row.fashions);
        result.push_back(std::move(item));
    }
    return result;
}
} // namespace character_studio
