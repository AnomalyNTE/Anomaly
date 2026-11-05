#pragma once
#include "ue.hpp"
#include <set>

namespace character_studio {
struct Character {
    Name id{};
    std::string code, chinese;
};
struct CatalogRow {
    Character character;
    Name default_fashion{};
};

// Join authored table references only. Mesh residency and player ownership do not
// determine whether a character belongs in the catalog.
inline std::vector<Character> BuildCatalog(
    std::vector<CatalogRow> rows,
    const std::set<std::uint64_t>& defaults,
    const std::map<std::uint64_t, Name>& fashion_owners) {
    std::sort(rows.begin(), rows.end(), [](const CatalogRow& a, const CatalogRow& b) {
        return a.character.code < b.character.code;
    });
    std::set<std::string> names;
    std::vector<Character> result;
    for (auto& row : rows) {
        auto& item = row.character;
        if (!defaults.contains(Key(item.id))) {
            const auto fashion = fashion_owners.find(Key(row.default_fashion));
            if (!row.default_fashion.index || fashion == fashion_owners.end() ||
                fashion->second != item.id) continue;
        }
        // Filter before deduplicating: an unusable alias must never hide the
        // actual character. Missing display names are not a shared identity.
        if (!item.chinese.empty()) {
            if (!names.insert(item.chinese).second) continue;
        } else {
            item.chinese = item.code;
        }
        result.push_back(std::move(item));
    }
    return result;
}
} // namespace character_studio
