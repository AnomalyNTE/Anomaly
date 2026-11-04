#include "resource_name_resolver.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace nte::combat_stats {

namespace {

struct CharacterEntry {
    std::uint32_t id{};
    std::string codename;
    std::string display_name;
    std::string attribute;
};

struct DamageSkillEntry {
    std::string technical_name;
    std::string ability;
};

struct EnemyEntry {
    std::string config_id;
    std::string monster_id;
    std::string display_name;
};

[[nodiscard]] nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("required name resource is unavailable: " + path.string());
    return nlohmann::json::parse(stream);
}

[[nodiscard]] std::string Lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] std::string CompactTechnicalName(const std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    const auto position = slash == std::string_view::npos
        ? dot
        : (dot == std::string_view::npos ? slash : (std::max)(slash, dot));
    return std::string(position == std::string_view::npos ? path : path.substr(position + 1));
}

[[nodiscard]] bool IsUnknown(const std::string_view value) {
    return value.empty() || Lower(value).starts_with("unknown");
}

[[nodiscard]] std::string StripClassSuffix(std::string value) {
    if (value.size() > 2 && Lower(std::string_view(value).substr(value.size() - 2)) == "_c") {
        value.resize(value.size() - 2);
    }
    return value;
}

[[nodiscard]] bool IsIdentifier(const std::string_view value) {
    if (value.empty()) return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
    });
}

[[nodiscard]] std::string ExtractGameplayEffect(const std::string_view path) {
    const auto marker = path.rfind(".Default__");
    auto candidate = marker == std::string_view::npos
        ? CompactTechnicalName(path)
        : std::string(path.substr(marker + 10));
    if (Lower(candidate).starts_with("default__")) candidate.erase(0, 9);
    candidate = StripClassSuffix(std::move(candidate));
    return IsIdentifier(candidate) && Lower(candidate).starts_with("ge_")
        ? candidate
        : std::string{};
}

}  // namespace

struct ResourceNameResolver::Entries {
    std::unordered_map<std::uint32_t, CharacterEntry> characters_by_id;
    std::unordered_map<std::string, std::vector<std::uint32_t>> character_ids_by_codename;
    std::unordered_map<std::string, EnemyEntry> enemies;
    std::unordered_map<std::string, DamageSkillEntry> damage_skills;
    std::unordered_map<std::string, std::string> ability_names;
};

std::string_view DisplayNameOriginName(const DisplayNameOrigin origin) noexcept {
    switch (origin) {
    case DisplayNameOrigin::Resource: return "resource";
    case DisplayNameOrigin::Technical: return "technical";
    case DisplayNameOrigin::Unknown: return "unknown";
    }
    return "unknown";
}

ResourceNameResolver ResourceNameResolver::Load(
    const std::filesystem::path& dict_root,
    std::string& error) {
    error.clear();
    ResourceNameResolver result;
    std::string stage{"dictionary directory"};
    try {
        auto entries = std::make_shared<Entries>();
        std::string expected_revision;
        std::size_t loaded_files{};
        const auto character_directory = dict_root / "character";
        if (!std::filesystem::is_directory(character_directory)) {
            throw std::runtime_error(
                "character dictionary directory is unavailable: " +
                character_directory.string());
        }
        for (const auto& item :
            std::filesystem::directory_iterator(character_directory)) {
            if (!item.is_regular_file() || item.path().extension() != ".json") {
                continue;
            }
            stage = "dictionary file " + item.path().filename().string();
            const auto document = ReadJson(item.path());
            if (document.value("format_version", 0) != 1) {
                throw std::runtime_error("character dictionary format_version is invalid");
            }
            const auto revision = document.at("source")
                .at("assets_git_revision").get<std::string>();
            if (revision.empty()) {
                throw std::runtime_error("character dictionary revision is empty");
            }
            if (!expected_revision.empty() && revision != expected_revision) {
                throw std::runtime_error("character dictionary revisions disagree");
            }
            expected_revision = revision;

            const auto& data = document.at("data");
            if (data.contains("characters")) {
                for (const auto& [id_text, value] : data.at("characters").items()) {
                    std::size_t consumed{};
                    const auto parsed = std::stoul(id_text, &consumed);
                    if (consumed != id_text.size() || parsed > UINT32_MAX) {
                        throw std::runtime_error("character id is invalid: " + id_text);
                    }
                    CharacterEntry entry{
                        static_cast<std::uint32_t>(parsed),
                        value.at("codename").get<std::string>(),
                        value.at("name_zh").get<std::string>(),
                        value.value("attribute", std::string{})};
                    if (entry.codename.empty() || entry.display_name.empty()) {
                        throw std::runtime_error(
                            "character codename or Chinese name is empty: " + id_text);
                    }
                    entries->character_ids_by_codename[Lower(entry.codename)]
                        .push_back(entry.id);
                    if (!entries->characters_by_id.emplace(
                            entry.id, std::move(entry)).second) {
                        throw std::runtime_error("duplicate character id: " + id_text);
                    }
                }
            }
            if (data.contains("skill_damage")) {
                for (const auto& [effect, value] : data.at("skill_damage").items()) {
                    if (const auto ability = value.find("GAName");
                        ability != value.end() && ability->is_string() &&
                        !ability->get_ref<const std::string&>().empty() &&
                        ability->get_ref<const std::string&>() != "None") {
                        entries->damage_skills.emplace(Lower(effect), DamageSkillEntry{
                            effect, ability->get<std::string>()});
                    }
                }
            }
            if (data.contains("ability_tips")) {
                for (const auto& [ability, value] : data.at("ability_tips").items()) {
                    const auto name = value.value("name_zh", std::string{});
                    if (!name.empty()) {
                        entries->ability_names[Lower(ability)] = name;
                    }
                }
            }
            ++loaded_files;
        }
        if (loaded_files == 0 || expected_revision.empty()) {
            throw std::runtime_error("character dictionary contains no JSON files");
        }

        stage = "monster dictionary";
        const auto monster = ReadJson(dict_root / "monster.json");
        if (!monster.contains("enemies") || !monster.at("enemies").is_array()) {
            throw std::runtime_error("monster dictionary enemies are invalid");
        }
        for (const auto& value : monster.at("enemies")) {
            EnemyEntry entry{
                value.at("config_id").get<std::string>(),
                value.at("monster_id").get<std::string>(),
                value.at("name_zh").get<std::string>()};
            if (entry.config_id.empty() || entry.display_name.empty()) {
                throw std::runtime_error(
                    "monster config id or Chinese name is empty");
            }
            if (!entries->enemies.emplace(
                    Lower(entry.config_id), std::move(entry)).second) {
                throw std::runtime_error(
                    "duplicate monster config id: " + value.at("config_id").get<std::string>());
            }
        }
        result.version_ = expected_revision;
        result.entries_ = std::move(entries);
    } catch (const std::exception& exception) {
        result.version_.clear();
        result.entries_.reset();
        error = stage + ": " + exception.what();
    }
    return result;
}

ResolvedAbilityName ResourceNameResolver::ResolveAbilityDisplay(
    const std::string_view technical_path) const {
    ResolvedAbilityName result;
    result.technical_name.assign(technical_path);
    result.resource_version = version_;
    if (IsUnknown(technical_path)) {
        result.display_name = "Unknown";
        return result;
    }

    auto compact = StripClassSuffix(CompactTechnicalName(technical_path));
    if (entries_) {
        const auto found = entries_->ability_names.find(Lower(compact));
        if (found != entries_->ability_names.end()) {
            result.display_name = found->second;
            result.origin = DisplayNameOrigin::Resource;
            return result;
        }
    }
    result.display_name = std::move(compact);
    result.origin = DisplayNameOrigin::Technical;
    return result;
}

ResolvedParticipantName ResourceNameResolver::ResolveParticipant(
    const std::string_view technical_path) const {
    ResolvedParticipantName result;
    result.technical_name.assign(technical_path);
    result.resource_version = version_;
    if (IsUnknown(technical_path)) {
        result.display_name = "Unknown";
        return result;
    }

    auto compact = CompactTechnicalName(technical_path);
    const auto without_suffix = StripClassSuffix(compact);
    if (entries_) {
        const auto lower = Lower(without_suffix);
        if (lower.starts_with("player_")) {
            const auto first = without_suffix.find('_');
            const auto second = without_suffix.find('_', first + 1);
            if (second != std::string::npos) {
                const auto number = without_suffix.substr(first + 1, second - first - 1);
                const auto codename = without_suffix.substr(second + 1);
                std::uint32_t candidate{};
                try {
                    std::size_t consumed{};
                    const auto parsed = std::stoul(number, &consumed);
                    if (consumed == number.size() && parsed <= 999) {
                        candidate = static_cast<std::uint32_t>(1000 + parsed);
                    }
                } catch (const std::exception&) {
                }
                if (const auto found = entries_->characters_by_id.find(candidate);
                    found != entries_->characters_by_id.end() &&
                    Lower(found->second.codename) == Lower(codename)) {
                    result.display_name = found->second.display_name;
                    result.attribute = found->second.attribute;
                    result.character_id = found->second.id;
                    result.origin = DisplayNameOrigin::Resource;
                    return result;
                }
                if (const auto found = entries_->character_ids_by_codename.find(Lower(codename));
                    found != entries_->character_ids_by_codename.end() && found->second.size() == 1) {
                    const auto& character = entries_->characters_by_id.at(found->second.front());
                    result.display_name = character.display_name;
                    result.attribute = character.attribute;
                    result.character_id = character.id;
                    result.origin = DisplayNameOrigin::Resource;
                    return result;
                }
            }
        }
        if (const auto enemy = entries_->enemies.find(lower); enemy != entries_->enemies.end()) {
            result.display_name = enemy->second.display_name;
            result.enemy_config_id = enemy->second.config_id;
            result.monster_id = enemy->second.monster_id;
            result.origin = DisplayNameOrigin::Resource;
            return result;
        }
    }
    result.display_name = std::move(compact);
    result.origin = DisplayNameOrigin::Technical;
    return result;
}

ResolvedDamageName ResourceNameResolver::ResolveDamageSource(
    const std::string_view technical_path) const {
    ResolvedDamageName result;
    result.technical_name.assign(technical_path);
    result.resource_version = version_;
    if (IsUnknown(technical_path)) {
        result.display_name = "Unknown";
        return result;
    }
    result.gameplay_effect_name = ExtractGameplayEffect(technical_path);
    if (!entries_ || result.gameplay_effect_name.empty()) {
        result.display_name = result.gameplay_effect_name.empty()
            ? CompactTechnicalName(technical_path)
            : result.gameplay_effect_name;
        result.origin = DisplayNameOrigin::Technical;
        return result;
    }

    const auto effect_key = Lower(result.gameplay_effect_name);
    if (const auto found = entries_->damage_skills.find(effect_key);
        found != entries_->damage_skills.end()) {
        result.gameplay_effect_name = found->second.technical_name;
        result.ability_name = found->second.ability;
    }
    std::string parent;
    if (const auto found = entries_->ability_names.find(Lower(result.ability_name));
        found != entries_->ability_names.end()) {
        parent = found->second;
    }
    if (!parent.empty()) {
        result.display_name = parent;
    } else if (!result.ability_name.empty()) {
        result.display_name = result.ability_name;
    } else {
        result.display_name = result.gameplay_effect_name;
    }
    result.origin = result.display_name == result.gameplay_effect_name ||
            result.display_name == result.ability_name
        ? DisplayNameOrigin::Technical
        : DisplayNameOrigin::Resource;
    return result;
}

}  // namespace nte::combat_stats
