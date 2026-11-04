#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nte::combat_stats {

struct SkillIdNameExport {
    std::string id;
    std::string id_kind;
    std::string ge_name;
    std::string ga_name;
    std::string zh_name;
};

struct CharacterIdNameExport {
    std::string id;
    std::string id_kind;
    std::string codename;
    std::string zh_name;
};

struct EnemyIdNameExport {
    std::string id;
    std::string id_kind;
    std::string monster_id;
    std::string config_hash;
    std::string zh_name;
};

struct RuntimeIdNameExport {
    std::string id;
    std::string id_kind;
    std::string kind;
    std::string display_name;
    std::string source_path;
    std::uint64_t observed_count{};
    std::uint64_t first_sequence{};
    std::uint64_t last_sequence{};
    std::uint64_t first_tick_sequence{};
    std::uint64_t last_tick_sequence{};
};

struct IdNameExportDocument {
    std::string format{"nte-id-name-export"};
    std::int32_t schema_version{1};
    std::int64_t generated_at_utc_ms{};
    std::string asset_revision;
    std::vector<SkillIdNameExport> skills;
    std::vector<CharacterIdNameExport> characters;
    std::vector<EnemyIdNameExport> enemies;
    std::vector<RuntimeIdNameExport> runtime;
};

[[nodiscard]] IdNameExportDocument BuildIdNameExport(
    const std::filesystem::path& dict_root,
    std::vector<RuntimeIdNameExport> runtime,
    std::string& error);

[[nodiscard]] std::string SerializeIdNameExport(
    const IdNameExportDocument& document);

[[nodiscard]] bool WriteIdNameExport(
    const std::filesystem::path& directory,
    const IdNameExportDocument& document,
    std::filesystem::path& written_path,
    std::string& error);

}  // namespace nte::combat_stats
