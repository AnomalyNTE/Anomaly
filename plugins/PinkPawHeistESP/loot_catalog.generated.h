// Generated from the exported Pink Paw BankBox data. See LOOT_CATALOG_SOURCES.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pink_paw_heist_esp::catalog {

struct ItemDefinition {
    std::string_view item_id;
    std::string_view name_utf8;
    std::optional<std::uint32_t> value;
    std::string_view icon_filename;
};

struct ClassAlias {
    std::string_view class_name;
    std::uint16_t item_index;
};

inline constexpr std::array<ItemDefinition, 51> kItemDefinitions{{
    {"RobBankItem_002", "「永恒之心」", 666666u, "RobBankItem_002.png"},
    {"RobBankItem_003", "绯红宝石", 131400u, "RobBankItem_003.png"},
    {"RobBankItem_004", "莹碧翡翠", 88888u, "RobBankItem_004.png"},
    {"RobBankItem_005", "烟紫水晶", 9999u, "RobBankItem_005.png"},
    {"RobBankItem_006", "金条", 1000u, "RobBankItem_006.png"},
    {"RobBankItem_008", "青花花瓶", 15000u, "RobBankItem_008.png"},
    {"RobBankItem_009", "留声机", 2800u, "RobBankItem_009.png"},
    {"RobBankItem_010", "寻星者望远镜", 1200u, "RobBankItem_010.png"},
    {"RobBankItem_011", "红釉花瓶", 6400u, "RobBankItem_011.png"},
    {"RobBankItem_012", "黛绿花瓶", 5000u, "RobBankItem_012.png"},
    {"RobBankItem_013", "艺术画「深林」", 7500u, "RobBankItem_013.png"},
    {"RobBankItem_014", "艺术画「古堡」", 6400u, "RobBankItem_014.png"},
    {"RobBankItem_015", "艺术画「绿野」", 5200u, "RobBankItem_015.png"},
    {"RobBankItem_020", "艺术画「解构」", 200u, "RobBankItem_020.png"},
    {"RobBankItem_021", "「几何」摆件", 5000u, "RobBankItem_021.png"},
    {"RobBankItem_023", "少量的纸钞", 1000u, "RobBankItem_023.png"},
    {"RobBankItem_024", "特制金币", 10u, "RobBankItem_024.png"},
    {"RobBankItem_025", "满袋的特制金币", 200u, "RobBankItem_025.png"},
    {"RobBankItem_027", "「方盒科技」手机", 750u, "RobBankItem_027.png"},
    {"RobBankItem_028", "数码相机", 600u, "RobBankItem_028.png"},
    {"RobBankItem_029", "浅绯祈手办", 300u, "RobBankItem_029.png"},
    {"RobBankItem_030", "纳库佩达模型", 400u, "RobBankItem_030.png"},
    {"RobBankItem_031", "团三郎玩偶", 250u, "RobBankItem_031.png"},
    {"RobBankItem_038", "纳财喵摆件", 100u, "RobBankItem_038.png"},
    {"RobBankItem_040", "「切玉」", 2500u, "RobBankItem_040.png"},
    {"RobBankItem_041", "游戏机", 999u, "RobBankItem_041.png"},
    {"RobBankItem_042", "漫画期刊", 30u, "RobBankItem_042.png"},
    {"RobBankItem_044", "漫画书", 50u, "RobBankItem_044.png"},
    {"RobBankItem_048", "蓝釉瓶", 640u, "RobBankItem_048.png"},
    {"RobBankItem_049", "青花碗", 720u, "RobBankItem_049.png"},
    {"RobBankItem_050", "花雕碗", 560u, "RobBankItem_050.png"},
    {"RobBankItem_051", "鎏金盂", 800u, "RobBankItem_051.png"},
    {"RobBankItem_052", "银行文件·其一", 80u, "RobBankItem_052.png"},
    {"RobBankItem_056", "墨染瓶", 2000u, "RobBankItem_056.png"},
    {"RobBankItem_057", "墨染罐", 3000u, "RobBankItem_057.png"},
    {"RobBankItem_058", "「摇星」", 20000u, "RobBankItem_058.png"},
    {"RobBankItem_059", "万有星仪", 27500u, "RobBankItem_059.png"},
    {"RobBankItem_060", "引航者望远镜", 1500u, "RobBankItem_060.png"},
    {"RobBankItem_G001", "临时访客卡", std::nullopt, "RobBankItem_G001.png"},
    {"RobBankItem_G003", "普通权限卡", std::nullopt, "RobBankItem_G003.png"},
    {"RobBankItem_G004", "高级权限卡·一", std::nullopt, "RobBankItem_G004.png"},
    {"RobBankItem_G006", "金库门禁卡·一", std::nullopt, "RobBankItem_G006.png"},
    {"RobBankItem_G007", "金库门禁卡·二", std::nullopt, "RobBankItem_G007.png"},
    {"RobBankItem_G008", "金库门禁卡·三", std::nullopt, "RobBankItem_G008.png"},
    {"RobBankItem_G009", "金库门禁卡·四", std::nullopt, "RobBankItem_G009.png"},
    {"RobBankItem_G010", "金库门禁卡·五", std::nullopt, "RobBankItem_G010.png"},
    {"RobBankItem_G011", "金库门禁卡·六", std::nullopt, "RobBankItem_G011.png"},
    {"RobBankItem_G012", "金库门禁卡·七", std::nullopt, "RobBankItem_G012.png"},
    {"RobBankItem_G013", "金库门禁卡·八", std::nullopt, "RobBankItem_G013.png"},
    {"RobBankItem_G014", "金库门禁卡·九", std::nullopt, "RobBankItem_G014.png"},
    {"RobBankItem_G015", "金库门禁卡·十", std::nullopt, "RobBankItem_G015.png"},
}};

inline constexpr std::array<ClassAlias, 142> kClassAliases{{
    {"BankBox_Bag_Coins_Lv2", 17u},
    {"BankBox_Camera_Lv2", 19u},
    {"BankBox_Coin_Lv1_01", 16u},
    {"BankBox_Coin_Lv1_02", 16u},
    {"BankBox_Coin_Lv1_03", 16u},
    {"BankBox_Coin_Lv1_04", 16u},
    {"BankBox_Coin_Lv1_05", 16u},
    {"BankBox_Decoration_01_Lv2", 20u},
    {"BankBox_Decoration_02_Lv2", 21u},
    {"BankBox_Decoration_03_Lv2", 22u},
    {"BankBox_Decoration_04_Lv2", 23u},
    {"BankBox_Decoration_05_Lv2", 22u},
    {"BankBox_Door_KeyCard_01_LV4", 40u},
    {"BankBox_Door_KeyCard_02_LV1", 38u},
    {"BankBox_Door_KeyCard_03_LV2", 39u},
    {"BankBox_Door_KeyCard_LG1_01_LV2", 40u},
    {"BankBox_Door_KeyCard_LG1_01_LV3", 40u},
    {"BankBox_Door_KeyCard_LG1_01_LV4", 40u},
    {"BankBox_Door_KeyCard_LG2_01_LV2", 40u},
    {"BankBox_Door_KeyCard_LG2_01_LV3", 40u},
    {"BankBox_Door_KeyCard_LG2_01_LV4", 40u},
    {"BankBox_Door_KeyCard_LG2_02_LV3", 40u},
    {"BankBox_Door_KeyCard_Safetyroom_01_LV3", 41u},
    {"BankBox_Door_KeyCard_Safetyroom_02_LV3", 42u},
    {"BankBox_Door_KeyCard_Safetyroom_03_LV4", 43u},
    {"BankBox_Door_KeyCard_Safetyroom_04_LV4", 44u},
    {"BankBox_Door_KeyCard_Safetyroom_05_LV4", 45u},
    {"BankBox_Door_KeyCard_Safetyroom_06_LV4", 46u},
    {"BankBox_Door_KeyCard_Safetyroom_07_LV4", 47u},
    {"BankBox_Door_KeyCard_Safetyroom_08_LV4", 48u},
    {"BankBox_Door_KeyCard_Safetyroom_09_LV4", 49u},
    {"BankBox_Door_KeyCard_Safetyroom_10_LV4", 50u},
    {"BankBox_Folder_01_Lv1", 32u},
    {"BankBox_Folder_02_Lv1", 32u},
    {"BankBox_Folder_03_Lv1", 32u},
    {"BankBox_Folder_04_Lv1", 32u},
    {"BankBox_Folder_05_Lv1", 26u},
    {"BankBox_Folder_07_Lv1", 27u},
    {"BankBox_Gameboy_Lv2", 25u},
    {"BankBox_Gem_01_Lv4", 2u},
    {"BankBox_Gem_02_Lv4", 0u},
    {"BankBox_Gem_03_Lv4", 1u},
    {"BankBox_Gem_Lv3", 3u},
    {"BankBox_gold_bar_Lv3", 4u},
    {"BankBox_gold_bar_Lv3_02", 4u},
    {"BankBox_gold_bar_Lv3_03", 4u},
    {"BankBox_gold_bar_Lv3_04", 4u},
    {"BankBox_gold_barS_Lv4", 4u},
    {"BankBox_gold_barS_Lv4_02", 4u},
    {"BankBox_GoldCoinPile_Lv4", 16u},
    {"BankBox_GoldCoinPile_Lv4_02", 16u},
    {"BankBox_GoldCoinPile_Lv4_03", 16u},
    {"BankBox_GoldCoinPile_Lv4_04", 16u},
    {"BankBox_Jewelry_Lv3_01", 3u},
    {"BankBox_Jewelry_Lv4_01", 3u},
    {"BankBox_Jewelry_Lv4_02", 3u},
    {"BankBox_LittlePicture_01_Lv2", 13u},
    {"BankBox_LittlePicture_02_Lv2", 13u},
    {"BankBox_LittlePicture_03_Lv2", 13u},
    {"BankBox_LittlePicture_04_Lv2", 13u},
    {"BankBox_LittlePicture_05_Lv3", 10u},
    {"BankBox_LittlePicture_07_Lv3", 12u},
    {"BankBox_LittlePicture_10_Lv3", 11u},
    {"BankBox_LittlePicture_12_Lv3", 12u},
    {"BankBox_Money_Lv2", 15u},
    {"BankBox_Money_Lv2_02", 15u},
    {"BankBox_Money_Lv2_03", 15u},
    {"BankBox_Money_Lv2_04", 15u},
    {"BankBox_money_Lv3", 15u},
    {"BankBox_money_Lv3_02", 15u},
    {"BankBox_money_Lv3_03", 15u},
    {"BankBox_money_Lv3_04", 15u},
    {"BankBox_money_Lv3_05", 15u},
    {"BankBox_money_Lv3_06", 15u},
    {"BankBox_money_Lv4", 15u},
    {"BankBox_money_Lv4_02", 15u},
    {"BankBox_money_Lv4_03", 15u},
    {"BankBox_money_Lv4_MissionGhost", 15u},
    {"BankBox_Monster_Bag_Coins_Lv2", 17u},
    {"BankBox_Monster_Camera_Lv2", 19u},
    {"BankBox_Monster_Folder_05_Lv1", 26u},
    {"BankBox_Monster_Folder_07_Lv1", 27u},
    {"BankBox_Monster_Gameboy_Lv2", 25u},
    {"BankBox_Monster_Gem_Lv3", 3u},
    {"BankBox_Monster_gold_bar_Lv3", 4u},
    {"BankBox_Monster_gold_bar_Lv3_02", 4u},
    {"BankBox_Monster_gold_bar_Lv3_03", 4u},
    {"BankBox_Monster_gold_bar_Lv3_04", 4u},
    {"BankBox_Monster_gold_barS_Lv4", 4u},
    {"BankBox_Monster_gold_barS_Lv4_02", 4u},
    {"BankBox_Monster_Jewelry_Lv3_01", 3u},
    {"BankBox_Monster_Jewelry_Lv4_01", 3u},
    {"BankBox_Monster_Jewelry_Lv4_02", 3u},
    {"BankBox_Monster_LittlePicture_01_Lv2", 13u},
    {"BankBox_Monster_LittlePicture_02_Lv2", 13u},
    {"BankBox_Monster_LittlePicture_03_Lv2", 13u},
    {"BankBox_Monster_LittlePicture_04_Lv2", 13u},
    {"BankBox_Monster_LittlePicture_09_Lv3", 10u},
    {"BankBox_Monster_LittlePicture_10_Lv3", 11u},
    {"BankBox_Monster_LittlePicture_11_Lv3", 12u},
    {"BankBox_Monster_LittlePicture_12_Lv3", 12u},
    {"BankBox_Monster_Money_Lv2", 15u},
    {"BankBox_Monster_Money_Lv2_02", 15u},
    {"BankBox_Monster_Money_Lv2_03", 15u},
    {"BankBox_Monster_Money_Lv2_04", 15u},
    {"BankBox_Monster_money_Lv3", 15u},
    {"BankBox_Monster_money_Lv3_02", 15u},
    {"BankBox_Monster_money_Lv3_03", 15u},
    {"BankBox_Monster_money_Lv3_04", 15u},
    {"BankBox_Monster_money_Lv3_05", 15u},
    {"BankBox_Monster_money_Lv3_06", 15u},
    {"BankBox_Monster_money_Lv4", 15u},
    {"BankBox_Monster_money_Lv4_02", 15u},
    {"BankBox_Monster_money_Lv4_03", 15u},
    {"BankBox_Monster_Phone_Lv2", 18u},
    {"BankBox_Monster_Treasure_01_Lv3", 34u},
    {"BankBox_Monster_Treasure_02_Lv3", 33u},
    {"BankBox_Monster_Treasure_02_Lv4", 14u},
    {"BankBox_Monster_Treasure_03_Lv3", 5u},
    {"BankBox_Monster_Treasure_03_Lv4", 36u},
    {"BankBox_Monster_Treasure_04_Lv3", 8u},
    {"BankBox_Monster_Treasure_05_Lv3", 9u},
    {"BankBox_Monster_Treasure_06_Lv3", 37u},
    {"BankBox_Monster_Treasure_07_Lv3", 7u},
    {"BankBox_Monster_Treasure_08_Lv3", 6u},
    {"BankBox_Phone_Lv2", 18u},
    {"BankBox_Sword_Lv3", 24u},
    {"BankBox_Treasure_01_Lv2", 28u},
    {"BankBox_Treasure_01_Lv3", 34u},
    {"BankBox_Treasure_02_Lv2", 29u},
    {"BankBox_Treasure_02_Lv3", 33u},
    {"BankBox_Treasure_02_Lv4", 14u},
    {"BankBox_Treasure_03_Lv2", 30u},
    {"BankBox_Treasure_03_Lv3", 5u},
    {"BankBox_Treasure_03_Lv4", 36u},
    {"BankBox_Treasure_04_Lv2", 31u},
    {"BankBox_Treasure_04_Lv3", 8u},
    {"BankBox_Treasure_04_Lv4", 35u},
    {"BankBox_Treasure_05_Lv3", 9u},
    {"BankBox_Treasure_06_Lv3", 37u},
    {"BankBox_Treasure_07_Lv3", 7u},
    {"BankBox_Treasure_08_Lv3", 6u},
}};

inline constexpr std::array<std::string_view, 10> kUnresolvedClassAliases{{
    "BankBox_Drink_01_LV1",
    "BankBox_Drink_01_LV2",
    "BankBox_Food_01_LV2",
    "BankBox_Medicine_01_LV2",
    "BankBox_Medicine_01_LV4",
    "BankBox_Mendicine_01_LV3",
    "BankBox_Monster_Treasure_05_LV4",
    "BankBox_Once_Base",
    "BankBox_SafeBox",
    "BankBox_Treasure_05_LV4",
}};

constexpr char AsciiLower(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr bool EqualsAsciiInsensitive(
    const std::string_view left,
    const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[index])) return false;
    }
    return true;
}

constexpr std::size_t FindAsciiInsensitive(
    const std::string_view haystack,
    const std::string_view needle) noexcept {
    if (needle.empty()) return 0;
    if (needle.size() > haystack.size()) return std::string_view::npos;
    for (std::size_t index = 0; index + needle.size() <= haystack.size(); ++index) {
        if (EqualsAsciiInsensitive(haystack.substr(index, needle.size()), needle)) return index;
    }
    return std::string_view::npos;
}

// Accepts BankBox_Foo, BankBox_Foo_C, and Default__BankBox_Foo_C.
constexpr std::string_view NormalizeRuntimeClassName(std::string_view class_name) noexcept {
    constexpr std::string_view kBankBoxPrefix = "BankBox_";
    if (const std::size_t start = FindAsciiInsensitive(class_name, kBankBoxPrefix);
        start != std::string_view::npos) {
        class_name.remove_prefix(start);
    }
    if (class_name.size() >= 2 &&
        EqualsAsciiInsensitive(class_name.substr(class_name.size() - 2), "_C")) {
        class_name.remove_suffix(2);
    }
    return class_name;
}

constexpr std::optional<std::size_t> FindItemIndex(
    const std::string_view runtime_class_name) noexcept {
    const std::string_view normalized = NormalizeRuntimeClassName(runtime_class_name);
    for (const ClassAlias& alias : kClassAliases) {
        if (EqualsAsciiInsensitive(normalized, alias.class_name)) {
            return static_cast<std::size_t>(alias.item_index);
        }
    }
    return std::nullopt;
}

constexpr const ItemDefinition* FindItemDefinition(
    const std::string_view runtime_class_name) noexcept {
    const std::optional<std::size_t> item_index = FindItemIndex(runtime_class_name);
    return item_index.has_value() ? &kItemDefinitions[*item_index] : nullptr;
}

static_assert(kItemDefinitions.size() == 51);
static_assert(kClassAliases.size() == 142);

}  // namespace pink_paw_heist_esp::catalog
