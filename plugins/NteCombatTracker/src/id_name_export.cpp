#include "id_name_export.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include <windows.h>

namespace nte::combat_stats {
namespace {

using Json = nlohmann::json;

template <typename Integer>
std::string Decimal(const Integer value) {
    return std::to_string(value);
}

Json ReadJson(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("required id-name resource is unavailable: " +
            path.string());
    }
    return Json::parse(stream);
}

bool IsNoneName(const std::string_view value) {
    return value.empty() || value == "None";
}

std::uint64_t ParseId(const std::string& value) {
    std::size_t consumed{};
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::runtime_error("id is not an integer: " + value);
    }
    return parsed;
}

std::string UtcNowMillis() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return Decimal(std::chrono::duration_cast<std::chrono::milliseconds>(
        now).count());
}

std::filesystem::path SuggestedIdNameExportFilename() {
    const std::time_t seconds = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &seconds);
    std::array<char, 32> timestamp{};
    std::strftime(timestamp.data(), timestamp.size(), "%Y%m%d-%H%M%S", &local);
    return std::filesystem::path(
        std::string("nte-id-name-export-") + timestamp.data() + ".json");
}

Json SkillToJson(const SkillIdNameExport& record) {
    return {
        {"id", record.id},
        {"id_kind", record.id_kind},
        {"ge_name", record.ge_name},
        {"ga_name", record.ga_name},
        {"zh_name", record.zh_name},
    };
}

Json CharacterToJson(const CharacterIdNameExport& record) {
    return {
        {"id", record.id},
        {"id_kind", record.id_kind},
        {"codename", record.codename},
        {"zh_name", record.zh_name},
    };
}

Json EnemyToJson(const EnemyIdNameExport& record) {
    return {
        {"id", record.id},
        {"id_kind", record.id_kind},
        {"monster_id", record.monster_id},
        {"config_hash", record.config_hash},
        {"zh_name", record.zh_name},
    };
}

Json RuntimeToJson(const RuntimeIdNameExport& record) {
    return {
        {"id", record.id},
        {"id_kind", record.id_kind},
        {"kind", record.kind},
        {"display_name", record.display_name},
        {"source_path", record.source_path},
        {"observed_count", Decimal(record.observed_count)},
        {"first_sequence", Decimal(record.first_sequence)},
        {"last_sequence", Decimal(record.last_sequence)},
        {"first_tick_sequence", Decimal(record.first_tick_sequence)},
        {"last_tick_sequence", Decimal(record.last_tick_sequence)},
    };
}

}  // namespace

IdNameExportDocument BuildIdNameExport(
    const std::filesystem::path& dict_root,
    std::vector<RuntimeIdNameExport> runtime,
    std::string& error) {
    error.clear();
    IdNameExportDocument result;
    try {
        const auto character_directory = dict_root / "character";
        if (!std::filesystem::is_directory(character_directory)) {
            throw std::runtime_error(
                "character dictionary directory is unavailable");
        }

        std::unordered_map<std::string, std::string> damage_abilities;
        std::unordered_map<std::string, std::string> ability_names;
        std::unordered_map<std::string, Json> character_rows;
        std::unordered_map<std::string, Json> mapping_rows;
        for (const auto& item :
            std::filesystem::directory_iterator(character_directory)) {
            if (!item.is_regular_file() || item.path().extension() != ".json") {
                continue;
            }
            const auto document = ReadJson(item.path());
            const auto revision =
                document.at("source").at("assets_git_revision").get<std::string>();
            if (revision.empty()) {
                throw std::runtime_error("character dictionary revision is empty");
            }
            if (result.asset_revision.empty()) {
                result.asset_revision = revision;
            } else if (result.asset_revision != revision) {
                throw std::runtime_error(
                    "character dictionary revisions disagree");
            }
            const auto& data = document.at("data");
            for (const auto& [effect, row] : data.at("skill_damage").items()) {
                const auto ganame = row.find("GAName");
                if (ganame != row.end() && ganame->is_string()) {
                    damage_abilities.emplace(effect, ganame->get<std::string>());
                }
            }
            for (const auto& [ability, row] : data.at("ability_tips").items()) {
                ability_names.emplace(
                    ability, row.value("name_zh", std::string{}));
            }
            for (const auto& [id, row] : data.at("characters").items()) {
                character_rows.emplace(id, row);
            }
            for (const auto& [ge_name, row] :
                data.at("gameplay_effect_mapping").items()) {
                mapping_rows.emplace(ge_name, row);
            }
        }
        if (result.asset_revision.empty() || mapping_rows.empty() ||
            damage_abilities.empty()) {
            throw std::runtime_error("character dictionary is empty");
        }
        result.generated_at_utc_ms =
            std::stoll(UtcNowMillis());

        for (const auto& [ge_name, row] : mapping_rows) {
            SkillIdNameExport record;
            record.id = Decimal(row.at("UniqueIndex").get<std::uint32_t>());
            record.id_kind = "gameplay_effect_unique_index";
            record.ge_name = ge_name;
            const auto ability = damage_abilities.find(ge_name);
            record.ga_name = ability == damage_abilities.end()
                ? std::string{} : ability->second;
            if (!IsNoneName(record.ga_name)) {
                const auto name = ability_names.find(record.ga_name);
                if (name != ability_names.end()) {
                    record.zh_name = name->second;
                }
            }
            result.skills.push_back(std::move(record));
        }

        for (const auto& [id, row] : character_rows) {
            CharacterIdNameExport record;
            record.id = id;
            record.id_kind = "character_id";
            record.codename = row.at("codename").get<std::string>();
            record.zh_name = row.at("name_zh").get<std::string>();
            result.characters.push_back(std::move(record));
        }

        const auto monsters = ReadJson(dict_root / "monster.json");
        for (const auto& row : monsters.at("enemies")) {
            EnemyIdNameExport record;
            record.id = row.at("config_id").get<std::string>();
            record.id_kind = "enemy_config_id";
            record.monster_id = row.at("monster_id").get<std::string>();
            record.config_hash = row.value("config_hash", std::string{});
            record.zh_name = row.at("name_zh").get<std::string>();
            result.enemies.push_back(std::move(record));
        }

        std::ranges::sort(result.skills, [](const auto& left, const auto& right) {
            return ParseId(left.id) < ParseId(right.id);
        });
        std::ranges::sort(result.characters, [](const auto& left, const auto& right) {
            return ParseId(left.id) < ParseId(right.id);
        });
        std::ranges::sort(result.enemies, [](const auto& left, const auto& right) {
            return left.id < right.id;
        });

        result.runtime = std::move(runtime);
        std::ranges::sort(result.runtime, [](const auto& left, const auto& right) {
            return ParseId(left.id) < ParseId(right.id);
        });
    } catch (const std::exception& exception) {
        error = exception.what();
        result = {};
    }
    return result;
}

std::string SerializeIdNameExport(const IdNameExportDocument& document) {
    Json skills = Json::array();
    for (const auto& record : document.skills) {
        skills.push_back(SkillToJson(record));
    }
    Json characters = Json::array();
    for (const auto& record : document.characters) {
        characters.push_back(CharacterToJson(record));
    }
    Json enemies = Json::array();
    for (const auto& record : document.enemies) {
        enemies.push_back(EnemyToJson(record));
    }
    Json runtime = Json::array();
    for (const auto& record : document.runtime) {
        runtime.push_back(RuntimeToJson(record));
    }
    const Json root{
        {"format", document.format},
        {"schema_version", document.schema_version},
        {"generated_at_utc_ms", Decimal(document.generated_at_utc_ms)},
        {"asset_revision", document.asset_revision},
        {"static", {
            {"skills", std::move(skills)},
            {"characters", std::move(characters)},
            {"enemies", std::move(enemies)},
        }},
        {"runtime", std::move(runtime)},
    };
    return root.dump(2) + '\n';
}

bool WriteIdNameExport(
    const std::filesystem::path& directory,
    const IdNameExportDocument& document,
    std::filesystem::path& written_path,
    std::string& error) {
    error.clear();
    written_path.clear();
    if (directory.empty() || !directory.is_absolute()) {
        error = "export directory must be an absolute path";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(directory, filesystem_error)) {
        error = "export directory is unavailable: " + filesystem_error.message();
        return false;
    }

    const auto filename = SuggestedIdNameExportFilename();
    const auto temporary = directory /
        (L".nte-id-name-export-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetCurrentThreadId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "id-name export file could not be created";
            return false;
        }
        try {
            stream << SerializeIdNameExport(document);
        } catch (const std::exception& exception) {
            error = exception.what();
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
        stream.flush();
        if (!stream) {
            error = "id-name export file could not be written";
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
    }

    const auto stem = filename.stem().wstring();
    const auto extension = filename.extension().wstring();
    for (std::uint32_t suffix = 1; suffix != 0; ++suffix) {
        const auto target = directory /
            (suffix == 1 ? filename.wstring() :
                stem + L"-" + std::to_wstring(suffix) + extension);
        if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            written_path = target;
            return true;
        }
        const DWORD move_error = GetLastError();
        if (move_error != ERROR_FILE_EXISTS && move_error != ERROR_ALREADY_EXISTS) {
            std::filesystem::remove(temporary, filesystem_error);
            error = "id-name export file could not be published: " +
                std::to_string(move_error);
            return false;
        }
    }
    std::filesystem::remove(temporary, filesystem_error);
    error = "no available id-name export filename";
    return false;
}

}  // namespace nte::combat_stats
