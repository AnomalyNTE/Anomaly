#include "resource_name_resolver.hpp"
#include "event_damage_source_resolver.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void RepositoryDictionaryResolvesSdkTechnicalPaths() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/dict", error);
    if (!error.empty()) {
        std::cerr << "dictionary load error: " << error << '\n';
    }
    Check(error.empty() && !resolver.version().empty(),
        "the checked-in dictionary should load with its asset revision");

    const auto shinku = resolver.ResolveParticipant(
        "/Game/NTE/Characters/player_076_shinku_C");
    Check(shinku.display_name == "真红" && shinku.character_id == 1076u &&
              shinku.attribute == "光" &&
              shinku.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "player number and codename should resolve 真红 with its attribute");
    Check(resolver.ResolveParticipant("Player_075_Oneiroi_C").display_name == "伊洛伊",
        "participant matching should be case-insensitive");
    Check(resolver.ResolveParticipant("Player_051_Female_C").display_name == "「零」",
        "Female051 should resolve through its stable character id and codename");
    Check(resolver.ResolveParticipant("player_025_hathor_C").display_name == "哈索尔",
        "Hathor should resolve through its stable character id and codename");

    const auto boss = resolver.ResolveParticipant("Boss_07_BP_WorldBoss_C");
    Check(boss.display_name == "塞润尼缇" &&
              boss.enemy_config_id == "Boss_07_BP_WorldBoss" &&
              boss.monster_id == "Boss_07" &&
              boss.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "the monster dictionary should resolve enemy config and monster identities");

    const auto damage = resolver.ResolveDamageSource(
        "/Game/NTE/GE_Player_Shinku_WatchEx_Damage."
        "Default__GE_Player_Shinku_WatchEx_Damage_C");
    Check(damage.gameplay_effect_name == "GE_Player_Shinku_WatchEx_Damage" &&
              damage.ability_name == "GA_Shinku_Melee" &&
              damage.display_name == "普通攻击：特种格斗术" &&
              damage.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "damage should resolve through skill damage and ability tips");
    const auto passive_damage = resolver.ResolveDamageSource(
        "GE_Player_Shinku_ReactionAOE_Damage");
    Check(passive_damage.ability_name == "GA_Shinku_Melee" &&
              passive_damage.display_name == "普通攻击：特种格斗术" &&
              passive_damage.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "damage without a component overlay should retain the skill-damage parent");
    const auto mixed_case = resolver.ResolveDamageSource(
        "default__ge_player_shinku_watchex_damage_c");
    Check(mixed_case.display_name == damage.display_name &&
              mixed_case.gameplay_effect_name == damage.gameplay_effect_name,
        "GE dictionary matching should be case-insensitive and canonical");
    const auto hidden_parent = resolver.ResolveDamageSource(
        "Default__GE_Player_Mint_Skill4_Damage_New_C");
    Check(hidden_parent.display_name == "满分收容术",
        "damage without a component overlay should display the parent ability");
    const auto semantic_parent = resolver.ResolveDamageSource(
        "GE_ActorReaction_1_1019_Damage");
    Check(semantic_parent.ability_name.empty() &&
              semantic_parent.display_name == "GE_ActorReaction_1_1019_Damage",
        "damage without skill-damage attribution should fall back to its GE name");
    const auto unmapped = resolver.ResolveDamageSource("GE_Boss_Unmapped_Damage");
    Check(unmapped.display_name == "GE_Boss_Unmapped_Damage" &&
              unmapped.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "unmapped GE names should fall back to the technical identifier");

    const auto ability = resolver.ResolveAbilityDisplay(
        "/Game/Blueprints/Abilities/Player/GA_Shinku_Melee."
        "GA_Shinku_Melee_C");
    Check(ability.display_name == "普通攻击：特种格斗术" &&
              ability.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "ability paths should localize through the dictionary's ability tips");
}

void EventSourceResolutionDoesNotCacheReusableSdkIds() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/dict", error);
    Check(error.empty(), "event source fixture dictionary should load");

    std::string current =
        "Default__GE_Player_Shinku_Melee1_1_Damage_C";
    std::uint32_t lookups{};
    const auto lookup = [&](const std::uint64_t source_id) {
        Check(source_id == 0x8000000000000005ULL,
            "event source resolver should forward the opaque SDK id");
        ++lookups;
        return current;
    };
    const auto before_reset = nte::combat_stats::ResolveEventDamageSource(
        0x8000000000000005ULL, lookup, resolver);
    current = "Default__GE_Player_Lacrimosa_Melee1_Damage_C";
    const auto after_reset = nte::combat_stats::ResolveEventDamageSource(
        0x8000000000000005ULL, lookup, resolver);

    Check(lookups == 2,
        "the same source id must be looked up again for every accepted event");
    Check(before_reset.display_name == "普通攻击：特种格斗术" &&
              after_reset.display_name == "酸甜口味的制裁" &&
              after_reset.technical_name == current,
        "a reused source id must resolve from its current technical path");
}

std::filesystem::path TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-resource-name-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void Write(const std::filesystem::path& path, const std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

void CorruptAndAmbiguousDictionaryDegradeSafely() {
    const auto root = TemporaryRoot();
    const auto character_directory = root / "character";
    std::filesystem::create_directories(character_directory);
    Write(root / "monster.json",
        R"({"version":1,"hash":"fnv1a64_ascii_lower","sources":[],"enemies":[]})");
    const auto file = character_directory / "1001-test.json";
    Write(file,
        R"({"format_version":1,"source":{"assets_git_revision":"test-rev"},)"
        R"("data":{"characters":{"1001":{"attribute":"灵","codename":"Shared","name_zh":"甲"},)"
        R"("1002":{"attribute":"相","codename":"shared","name_zh":"乙"}}}})");

    std::string error;
    auto resolver = nte::combat_stats::ResourceNameResolver::Load(root, error);
    Check(error.empty(), "runtime loading should tolerate ambiguous codenames safely");
    const auto ambiguous = resolver.ResolveParticipant("Player_999_Shared_C");
    Check(ambiguous.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "ambiguous codename fallback must not guess a Chinese character name");
    Check(resolver.ResolveDamageSource("Default__GE-Bad-Path_C").origin ==
              nte::combat_stats::DisplayNameOrigin::Technical,
        "malformed GE paths should use technical-name fallback");

    Write(file,
        R"({"format_version":1,"source":{"assets_git_revision":"test-rev"},)"
        R"("data":{"characters":{"1001":{"codename":"NoAttribute","name_zh":"无属性"}}}})");
    resolver = nte::combat_stats::ResourceNameResolver::Load(root, error);
    const auto without_attribute = resolver.ResolveParticipant(
        "Player_001_NoAttribute_C");
    Check(error.empty() && without_attribute.display_name == "无属性" &&
              without_attribute.attribute.empty() &&
              without_attribute.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "a missing character attribute should preserve name resolution for color fallback");

    Write(file, "{invalid");
    resolver = nte::combat_stats::ResourceNameResolver::Load(root, error);
    Check(!error.empty() && resolver.version().empty(),
        "corrupt dictionary JSON should reject the set and expose a quality error");

    const auto other = character_directory / "1002-other.json";
    Write(file,
        R"({"format_version":1,"source":{"assets_git_revision":"test-rev-a"},)"
        R"("data":{"characters":{"1001":{"attribute":"灵","codename":"A","name_zh":"甲"}}}})");
    Write(other,
        R"({"format_version":1,"source":{"assets_git_revision":"test-rev-b"},)"
        R"("data":{"characters":{"1002":{"attribute":"相","codename":"B","name_zh":"乙"}}}})");
    resolver = nte::combat_stats::ResourceNameResolver::Load(root, error);
    Check(!error.empty() && resolver.version().empty(),
        "mixed dictionary revisions should be rejected atomically");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void MissingDictionaryDegradesSafely() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/dict/missing-characters",
        error);
    Check(!error.empty() && resolver.version().empty(),
        "missing dictionary should report a clear load error");
    const auto fallback = resolver.ResolveParticipant("Player_076_shinku_C");
    Check(fallback.display_name == "Player_076_shinku_C" &&
              fallback.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "a failed dictionary load must still allow safe technical-name fallback");
    const auto source = resolver.ResolveDamageSource(
        "Default__GE_Player_Shinku_WatchEx_Damage_C");
    Check(source.display_name == "GE_Player_Shinku_WatchEx_Damage" &&
              source.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "damage-source fallback should preserve the compact GE identifier");
}

void SanitizedSdkSourceRegressionKeepsPlayerCoverage() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/dict", error);
    Check(error.empty(), "dictionary should load for source coverage regression");
    std::ifstream sources(std::filesystem::path(NTE_TEST_SOURCE_DIR) /
        "tests/fixtures/sdk-source-names.txt", std::ios::binary);
    std::size_t total{};
    std::size_t player_related{};
    std::size_t localized_player_related{};
    std::string name;
    while (std::getline(sources, name)) {
        if (name.empty()) continue;
        ++total;
        const bool player = !name.starts_with("GE_Boss_");
        if (player) ++player_related;
        const auto resolved = resolver.ResolveDamageSource(name);
        if (player && resolved.origin ==
                nte::combat_stats::DisplayNameOrigin::Resource) {
            ++localized_player_related;
        }
    }
    Check(total == 52 && player_related == 48 && localized_player_related == 46,
        "the sanitized SDK sample should localize every mapped player source");
}

void UnknownNamesFallBackWithoutChangingIdentity() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/dict", error);
    Check(error.empty(), "dictionary should load for unknown-name checks");
    const auto participant = resolver.ResolveParticipant(
        "/Game/NTE/Characters/Player_999_Missing_C");
    Check(participant.technical_name ==
              "/Game/NTE/Characters/Player_999_Missing_C" &&
              participant.display_name == "Player_999_Missing_C" &&
              participant.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "unmapped participants should keep full identity and compact display text");
    Check(participant.enemy_config_id.empty() && participant.monster_id.empty(),
        "unmapped participants should not invent enemy identifiers");
    const auto unknown = resolver.ResolveDamageSource("Unknown source");
    Check(unknown.display_name == "Unknown" &&
              unknown.origin == nte::combat_stats::DisplayNameOrigin::Unknown,
        "unresolved SDK values should retain an explicit unknown state");
}

}  // namespace

int main() {
    RepositoryDictionaryResolvesSdkTechnicalPaths();
    EventSourceResolutionDoesNotCacheReusableSdkIds();
    UnknownNamesFallBackWithoutChangingIdentity();
    MissingDictionaryDegradesSafely();
    CorruptAndAmbiguousDictionaryDegradeSafely();
    SanitizedSdkSourceRegressionKeepsPlayerCoverage();
    if (failures != 0) return 1;
    std::cout << "All resource name resolver tests passed\n";
    return 0;
}
