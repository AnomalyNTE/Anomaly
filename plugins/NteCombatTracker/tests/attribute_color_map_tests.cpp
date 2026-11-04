#include "attribute_color_map.hpp"
#include "character_identity.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void RepositoryAssetProvidesAllAttributeColors() {
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/attribute_colors.json",
        error);

    Check(error.empty(), "the checked-in attribute color asset should load");
    Check(colors.Find("NATURE") == 0x3EBDA1U, "NATURE should use its teal color");
    Check(colors.Find("LAKSHANA") == 0xCBBD39U, "LAKSHANA should use its gold color");
    Check(colors.Find("CHAOS") == 0x5E58BDU, "CHAOS should use its violet color");
    Check(colors.Find("COSMOS") == 0xBCBCBCU, "COSMOS should use its silver color");
    Check(colors.Find("PSYCHE") == 0x3E98C5U, "PSYCHE should use its blue color");
    Check(colors.Find("INCANTATION") == 0xD0455EU, "INCANTATION should use its crimson color");
    Check(!colors.Find("UNKNOWN").has_value(),
        "an unknown attribute should not invent a color");
}

// The character id is derived from the participant object path, which is the
// only identity the combat stream carries. Guard the mapping the runtime lookup
// depends on.
void CharacterIdIsRecoveredFromParticipantPath() {
    Check(nte::combat_stats::CharacterIdFromPath(
              "/Game/Blueprints/Character/Player/Player_003_Sagiri.Player_003_Sagiri_C") ==
              "1003",
        "player_003 should map to the DT_Character row 1003");
    Check(nte::combat_stats::CharacterIdFromPath("player_021_edgar") == "1021",
        "player_021 should map to the DT_Character row 1021");
    Check(nte::combat_stats::CharacterIdFromPath("/Game/Enemy/monster_C").empty(),
        "a non-character path has no character id");
    Check(nte::combat_stats::CharacterIdFromPath("player_CC_001").empty(),
        "a malformed player segment has no character id");
}

std::filesystem::path TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-attribute-colors-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void Write(const std::filesystem::path& path, const std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

void LowercaseHexIsAccepted() {
    const auto root = TemporaryRoot();
    const auto path = root / "attribute_colors.json";
    Write(path, R"({"NATURE":"#2f9f89","LAKSHANA":"#b67817","CHAOS":"#625daf","COSMOS":"#699a87","PSYCHE":"#326fbf","INCANTATION":"#b61e3a"})");
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(error.empty() && colors.Find("NATURE") == 0x2F9F89U &&
              colors.Find("INCANTATION") == 0xB61E3AU,
        "lowercase hexadecimal colors should be accepted");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void InvalidDictionariesDegradeSafely() {
    const auto root = TemporaryRoot();
    const auto path = root / "attribute_colors.json";
    std::string error;

    auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("NATURE").has_value(),
        "a missing dictionary should return an empty map with an error");

    Write(path, R"({"NATURE":"#2F9F89","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("NATURE").has_value(),
        "a dictionary missing a required attribute should be rejected atomically");

    Write(path, R"({"NATURE":"#2F9F89","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF","INCANTATION":""})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("NATURE").has_value(),
        "an empty color should reject the complete dictionary");

    Write(path, R"({"NATURE":"#2F9F89","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF","INCANTATION":"#B61E3Z"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("NATURE").has_value(),
        "a non-hexadecimal color should reject the complete dictionary");

    Write(path, R"({"NATURE":"#2F9F89","NATURE":"#000000","LAKSHANA":"#B67817","CHAOS":"#625DAF","COSMOS":"#699A87","PSYCHE":"#326FBF","INCANTATION":"#B61E3A"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("NATURE").has_value(),
        "duplicate attribute keys should be rejected");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    RepositoryAssetProvidesAllAttributeColors();
    CharacterIdIsRecoveredFromParticipantPath();
    LowercaseHexIsAccepted();
    InvalidDictionariesDegradeSafely();
    if (failures != 0) return 1;
    std::cout << "All attribute color map tests passed\n";
    return 0;
}
