#include "id_name_export.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

namespace {

using nte::combat_stats::BuildIdNameExport;
using nte::combat_stats::IdNameExportDocument;
using nte::combat_stats::RuntimeIdNameExport;
using nte::combat_stats::SerializeIdNameExport;

void Write(const std::filesystem::path& path, const nlohmann::json& json) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("could not create fixture: " + path.string());
    stream << json.dump();
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("nte_id_name_export_tests_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "character");

    Write(root / "character/1003-sagiri.json", {
        {"format_version", 1},
        {"source", {{"assets_git_revision", "rev1"}}},
        {"data", {
            {"characters", {
                {"1003", {{"codename", "Sagiri"}, {"name_zh", "早雾"}}},
            }},
            {"ability_tips", {
                {"GA_Player_Test_Skill", {{"name_zh", "测试技能"}}},
            }},
            {"skill_damage", {
                {"GE_Player_Test_Skill_Damage", {{"GAName", "GA_Player_Test_Skill"}}},
                {"GE_None_Damage", {{"GAName", "None"}}},
            }},
            {"gameplay_effect_mapping", {
                {"GE_Player_Test_Skill_Damage", {{"UniqueIndex", 7}}},
                {"GE_None_Damage", {{"UniqueIndex", 8}}},
            }},
        }},
    });
    Write(root / "monster.json", {
        {"version", 1},
        {"hash", "fnv1a64_ascii_lower"},
        {"sources", nlohmann::json::array()},
        {"enemies", nlohmann::json::array({
            {{"config_id", "Boss_01_BP"},
             {"monster_id", "Boss_01"},
             {"config_hash", "hash1"},
             {"name_zh", "测试Boss"}},
        })},
    });

    std::vector<RuntimeIdNameExport> runtime;
    runtime.push_back({
        "11", "combat_name_id", "damage", "技能中文", "/Game/GE_Player_Test",
        3, 11, 13, 101, 103});
    runtime.push_back({
        "2", "combat_name_id", "buff_add", "Buff中文", "",
        1, 2, 2, 20, 20});

    std::string error;
    IdNameExportDocument document = BuildIdNameExport(root, runtime, error);
    if (!error.empty()) {
        std::fprintf(stderr, "BuildIdNameExport failed: %s\n", error.c_str());
        return 1;
    }
    if (document.asset_revision != "rev1") return 2;
    if (document.skills.size() != 2) return 3;
    if (document.skills[0].id != "7" ||
        document.skills[0].ga_name != "GA_Player_Test_Skill" ||
        document.skills[0].zh_name != "测试技能") return 4;
    if (document.skills[1].id != "8" ||
        document.skills[1].ga_name != "None" ||
        !document.skills[1].zh_name.empty()) return 5;
    if (document.characters.size() != 1 ||
        document.characters[0].id != "1003" ||
        document.characters[0].zh_name != "早雾") return 6;
    if (document.enemies.size() != 1 ||
        document.enemies[0].id != "Boss_01_BP" ||
        document.enemies[0].zh_name != "测试Boss") return 7;
    if (document.runtime.size() != 2 ||
        document.runtime[0].id != "2" ||
        document.runtime[1].id != "11") return 8;

    const auto serialized = SerializeIdNameExport(document);
    const auto parsed = nlohmann::json::parse(serialized);
    if (parsed.at("format") != "nte-id-name-export") return 9;
    if (parsed.at("schema_version") != 1) return 10;
    if (parsed.at("asset_revision") != "rev1") return 11;
    if (serialized.find("测试技能") == std::string::npos) return 12;
    if (serialized.find("语义覆盖") != std::string::npos) return 13;

    std::filesystem::remove_all(root);
    return 0;
}
