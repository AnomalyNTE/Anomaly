#include "attribute_color_map.hpp"

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

void RepositoryDictionaryProvidesAllAttributeColors() {
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/dict/attribute_colors.json",
        error);

    Check(error.empty(), "the checked-in attribute color dictionary should load");
    Check(colors.Find("灵") == 0x3EBDA1U, "灵 should use its teal representative color");
    Check(colors.Find("相") == 0xCBBD39U, "相 should use its gold representative color");
    Check(colors.Find("暗") == 0x5E58BDU, "暗 should use its violet representative color");
    Check(colors.Find("光") == 0xBCBCBCU, "光 should use its silver representative color");
    Check(colors.Find("魂") == 0x3E98C5U, "魂 should use its blue representative color");
    Check(colors.Find("咒") == 0xD0455EU, "咒 should use its crimson representative color");
    Check(!colors.Find("未知").has_value(),
        "an unknown attribute should not invent a representative color");
}

void CharacterDictionaryIsUsedOnlyForAttributeColorLookup() {
    const auto root = std::filesystem::path(NTE_TEST_SOURCE_DIR);
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(
        root / "assets/dict/attribute_colors.json",
        root / "assets/dict/character", error);
    Check(error.empty() &&
              colors.FindParticipant("/Game/Player/player_003_Sagiri_C") == 0xD0455EU,
        "participant technical paths should resolve character attribute colors");
    Check(!colors.FindParticipant("/Game/Enemy/monster_C").has_value(),
        "non-character paths should not resolve localized identity data");
}

void MissingCharacterDirectoryKeepsColorFallback() {
    const auto root = std::filesystem::path(NTE_TEST_SOURCE_DIR);
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(
        root / "assets/dict/attribute_colors.json",
        root / "assets/dict/missing-characters",
        error);
    Check(!error.empty(),
        "a missing character dictionary should expose a clear error");
    Check(colors.Find("灵") == 0x3EBDA1U &&
              !colors.FindParticipant("/Game/Player/player_003_Sagiri_C").has_value(),
        "attribute colors should stay usable while participant colors fall back");
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
    Write(path, R"({"灵":"#2f9f89","相":"#b67817","暗":"#625daf","光":"#699a87","魂":"#326fbf","咒":"#b61e3a"})");
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(error.empty() && colors.Find("灵") == 0x2F9F89U &&
              colors.Find("咒") == 0xB61E3AU,
        "lowercase hexadecimal colors should be accepted");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void InvalidDictionariesDegradeSafely() {
    const auto root = TemporaryRoot();
    const auto path = root / "attribute_colors.json";
    std::string error;

    auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "a missing dictionary should return an empty map with an error");

    Write(path, R"({"灵":"#2F9F89","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "a dictionary missing a required attribute should be rejected atomically");

    Write(path, R"({"灵":"#2F9F89","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF","咒":""})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "an empty color should reject the complete dictionary");

    Write(path, R"({"灵":"#2F9F89","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF","咒":"#B61E3Z"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "a non-hexadecimal color should reject the complete dictionary");

    Write(path, R"({"灵":"#2F9F89","灵":"#000000","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF","咒":"#B61E3A"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "duplicate attribute keys should be rejected");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    RepositoryDictionaryProvidesAllAttributeColors();
    CharacterDictionaryIsUsedOnlyForAttributeColorLookup();
    MissingCharacterDirectoryKeepsColorFallback();
    LowercaseHexIsAccepted();
    InvalidDictionariesDegradeSafely();
    if (failures != 0) return 1;
    std::cout << "All attribute color map tests passed\n";
    return 0;
}
