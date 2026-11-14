#pragma once
// The mods page classifies every mod from the containers it replaces, so its categories are container
// identifiers: "Player_051", "Vehicle_037", a content directory. Those identifiers are what
// classification and conflict detection match on and must never change, but they are not what a
// player reads, and they are not a folder name either. This table maps an identifier to the name the
// UI shows and to the folder a category is organised into. It is data, not UI text: adding an entry
// renames a category, it never reclassifies or unmaps a mod.
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace anomaly {

// The language a name is shown in, resolved wherever the caller already knows the UI language.
enum class ModGroupLanguage : std::uint8_t { EnUs, ZhCn };

class ModGroupNames final {
public:
    // Reads {"schemaVersion": 1, "groups": {"Player_051": {"en-US": "Zero", "zh-CN": "零"}},
    // "skins": {"female": {"en-US": "Female", "zh-CN": "女装"}}}. Entries accumulate, so the shipped
    // table and a user table can be read one after the other. A malformed document, an unknown schema
    // version or an unusable entry leaves the names in place rather than throwing: a broken name table
    // must not cost the mods page its classification.
    void Load(const nlohmann::json& document);
    // The name of a category. An unmapped identifier still reads as a category: the known id
    // families become "角色 051" / "Character 051", and anything else keeps its own spelling. A name
    // is shown in the language it is written in: an entry with no name for the requested language is
    // generated instead of reading the other language's name, so an English page never shows a
    // Chinese one.
    [[nodiscard]] std::string Group(std::string_view token, ModGroupLanguage language) const;
    // The name of one skin inside a category. Unmapped skins keep their own spelling: they are
    // identifiers a mod author chose ("female_fashion4"), not a name the game assigned.
    [[nodiscard]] std::string Skin(std::string_view token, ModGroupLanguage language) const;
    // Whether the table names this category already, so a name learned from the game or from a mod's
    // own metadata is added without replacing a curated entry.
    [[nodiscard]] bool HasGroup(std::string_view token) const {
        return groups_.contains(std::string(token));
    }
    // The folder a category is organised into: the identifier first, so two categories can never
    // share a folder and a language switch never moves one, then the mapped name when there is one.
    [[nodiscard]] std::string Folder(std::string_view token, ModGroupLanguage language) const;

private:
    struct Name final {
        std::string en_us, zh_cn;
    };
    std::map<std::string, Name> groups_, skins_;
};

} // namespace anomaly
