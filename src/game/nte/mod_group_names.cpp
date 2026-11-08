#include "anomaly/mod_group_names.hpp"
#include <array>
#include <cstddef>

namespace anomaly {
namespace {
// An identifier with no entry still reads as a category instead of as a bare id. Only the families
// whose ids the classifier emits are generated; anything else keeps its own spelling, which for a
// content directory is its name and is already readable.
std::string GeneratedCategory(const std::string_view token, const ModGroupLanguage language) {
    struct Family final {
        std::string_view prefix, en_us, zh_cn;
    };
    constexpr std::array<Family, 3> families{{
        {"Player_", "Character ", "角色 "},
        {"Vehicle_", "Vehicle ", "车辆 "},
        {"Pendant_", "Pendant ", "挂件 "},
    }};
    for (const auto& family : families) {
        if (!token.starts_with(family.prefix)) continue;
        const auto id = token.substr(family.prefix.size());
        if (id.empty() || id.find_first_not_of("0123456789") != std::string_view::npos) continue;
        return std::string(language == ModGroupLanguage::ZhCn ? family.zh_cn : family.en_us) +
            std::string(id);
    }
    return std::string(token);
}

// A folder name taken from an identifier: characters Windows rejects become '_', and the name is cut
// on a UTF-8 boundary well before the file-system limit.
std::string Sanitize(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || std::string_view{"<>:\"/\\|?*"}.find(c) != std::string_view::npos)
            result += '_';
        else
            result += c;
    }
    const auto first = result.find_first_not_of(" .");
    result.erase(0, first == std::string::npos ? result.size() : first);
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    constexpr std::size_t max_bytes = 120;
    if (result.size() > max_bytes) {
        std::size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xc0) == 0x80) --cut;
        result.resize(cut);
        while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    }
    return result;
}

std::string Pick(const std::string& en_us, const std::string& zh_cn,
    const ModGroupLanguage language) {
    // A name is shown in the language it is written in. The table carries Chinese names for the
    // characters and mixed spellings for the vehicles, so a missing entry is a reason to fall back to
    // the generated category name, never to show the other language's name in an English UI. Chinese
    // still reads a Latin name that has no Chinese spelling of its own.
    if (language == ModGroupLanguage::ZhCn) return !zh_cn.empty() ? zh_cn : en_us;
    return en_us;
}
} // namespace

void ModGroupNames::Load(const nlohmann::json& document) {
    // Entries accumulate: the shipped table is read first and a user table read after it only adds
    // or replaces the names it mentions, so extending one character never drops the rest.
    if (!document.is_object()) return;
    const auto version = document.find("schemaVersion");
    if (version != document.end() && (!version->is_number_integer() || version->get<int>() != 1))
        return;
    const auto read = [](const nlohmann::json& table, std::map<std::string, Name>& into) {
        if (!table.is_object()) return;
        for (const auto& [key, value] : table.items()) {
            if (key.empty() || !value.is_object()) continue;
            Name name;
            if (const auto item = value.find("en-US"); item != value.end() && item->is_string())
                name.en_us = item->get<std::string>();
            if (const auto item = value.find("zh-CN"); item != value.end() && item->is_string())
                name.zh_cn = item->get<std::string>();
            if (name.en_us.empty() && name.zh_cn.empty()) continue;
            into.insert_or_assign(key, std::move(name));
        }
    };
    if (const auto table = document.find("groups"); table != document.end()) read(*table, groups_);
    if (const auto table = document.find("skins"); table != document.end()) read(*table, skins_);
}

std::string ModGroupNames::Group(const std::string_view token, const ModGroupLanguage language) const {
    if (const auto found = groups_.find(std::string(token)); found != groups_.end()) {
        const auto name = Pick(found->second.en_us, found->second.zh_cn, language);
        if (!name.empty()) return name;
    }
    return GeneratedCategory(token, language);
}

std::string ModGroupNames::Skin(const std::string_view token, const ModGroupLanguage language) const {
    if (const auto found = skins_.find(std::string(token)); found != skins_.end()) {
        const auto name = Pick(found->second.en_us, found->second.zh_cn, language);
        if (!name.empty()) return name;
    }
    return std::string(token);
}

std::string ModGroupNames::Folder(const std::string_view token, const ModGroupLanguage language) const {
    // A content directory ("/game/wwiseaudio") is a path rather than a name, so the folder it files
    // into is its last segment: the category reads as "wwiseaudio" instead of a mangled path.
    auto name = token;
    if (name.starts_with('/')) {
        const auto slash = name.rfind('/');
        if (slash != std::string_view::npos && slash + 1 < name.size())
            name = name.substr(slash + 1);
    }
    auto folder = Sanitize(name);
    if (folder.empty()) folder = Sanitize(GeneratedCategory(token, language));
    if (folder.empty()) folder = "resources";
    const auto found = groups_.find(std::string(token));
    if (found == groups_.end()) return folder;
    const auto display = Sanitize(Pick(found->second.en_us, found->second.zh_cn, language));
    if (display.empty()) return folder;
    return folder + " " + display;
}

} // namespace anomaly
