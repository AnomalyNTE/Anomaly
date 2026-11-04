#pragma once

#include <anomaly/sdk/localization.hpp>

#include <charconv>
#include <string>
#include <string_view>

namespace summon_lab {

struct BlueprintNames {
    std::string family;
    std::string specific;
};

// Names are verified against the FModel export's Text/ST_ActorName.json and
// DataTable_1.3/DT_MonsterManualConfig.json. Boss 28, Boss 32 and Monster 51
// additionally use their blueprint attribute component's DescriptiveName.
// Keep family aliases even when a quest or summoned variant has its own name.
inline int BlueprintFamilyNumber(std::string_view name, std::string_view prefix) {
    for (std::size_t pos = name.find(prefix); pos != std::string_view::npos;
         pos = name.find(prefix, pos + 1)) {
        if (pos != 0 && name[pos - 1] != '_') continue;
        int number{};
        const char* begin = name.data() + pos + prefix.size();
        const auto result = std::from_chars(begin, name.data() + name.size(), number);
        if (result.ec == std::errc{}) return number;
    }
    return -1;
}

inline std::string BlueprintFamilyName(std::string_view name,
                                       const anomaly::sdk::Localizer& localizer) {
    if (name.starts_with("mon_nanally_"))
        return localizer.Text("catalog.name.nanally", "Nanally");
    switch (BlueprintFamilyNumber(name, "boss_")) {
        case 1: return localizer.Text("catalog.name.boss_01", "Boss 01");
        case 4: return localizer.Text("catalog.name.boss_04", "Boss 04");
        case 5: return localizer.Text("catalog.name.boss_05", "Boss 05");
        case 6: return localizer.Text("catalog.name.boss_06", "Boss 06");
        case 7: return localizer.Text("catalog.name.boss_07", "Boss 07");
        case 8: return localizer.Text("catalog.name.boss_08", "Boss 08");
        case 9: return localizer.Text("catalog.name.boss_09", "Boss 09");
        case 10: return localizer.Text("catalog.name.boss_10", "Boss 10");
        case 13: return localizer.Text("catalog.name.boss_13", "Boss 13");
        case 15: return localizer.Text("catalog.name.boss_15", "Boss 15");
        case 16: return localizer.Text("catalog.name.boss_16", "Boss 16");
        case 17: return localizer.Text("catalog.name.boss_17", "Boss 17");
        case 18: return localizer.Text("catalog.name.boss_18", "Boss 18");
        case 19: return localizer.Text("catalog.name.boss_19", "Boss 19");
        case 26: return localizer.Text("catalog.name.boss_26", "Boss 26");
        case 27: return localizer.Text("catalog.name.boss_27", "Boss 27");
        case 28: return localizer.Text("catalog.name.boss_28", "Boss 28");
        case 32: return localizer.Text("catalog.name.boss_32", "Boss 32");
        case 33: return localizer.Text("catalog.name.boss_33", "Boss 33");
        case 34: return localizer.Text("catalog.name.boss_34", "Boss 34");
        case 37: return localizer.Text("catalog.name.boss_37", "Boss 37");
    }
    switch (BlueprintFamilyNumber(name, "mon_")) {
        case 1: return localizer.Text("catalog.name.mon_01", "Monster 01");
        case 2: return localizer.Text("catalog.name.mon_02", "Monster 02");
        case 3: return localizer.Text("catalog.name.mon_03", "Monster 03");
        case 4: return localizer.Text("catalog.name.mon_04", "Monster 04");
        case 5: return localizer.Text("catalog.name.mon_05", "Monster 05");
        case 8: return localizer.Text("catalog.name.mon_08", "Monster 08");
        case 11: return localizer.Text("catalog.name.mon_11", "Monster 11");
        case 12: return localizer.Text("catalog.name.mon_12", "Monster 12");
        case 13: return localizer.Text("catalog.name.mon_13", "Monster 13");
        case 14: return localizer.Text("catalog.name.mon_14", "Monster 14");
        case 15: return localizer.Text("catalog.name.mon_15", "Monster 15");
        case 16: return localizer.Text("catalog.name.mon_16", "Monster 16");
        case 17: return localizer.Text("catalog.name.mon_17", "Monster 17");
        case 18: return localizer.Text("catalog.name.mon_18", "Monster 18");
        case 19: return localizer.Text("catalog.name.mon_19", "Monster 19");
        case 20: return localizer.Text("catalog.name.mon_20", "Monster 20");
        case 21: return localizer.Text("catalog.name.mon_21", "Monster 21");
        case 22: return localizer.Text("catalog.name.mon_22", "Monster 22");
        case 23: return localizer.Text("catalog.name.mon_23", "Monster 23");
        case 24: return localizer.Text("catalog.name.mon_24", "Monster 24");
        case 25: return localizer.Text("catalog.name.mon_25", "Monster 25");
        case 26: return localizer.Text("catalog.name.mon_26", "Monster 26");
        case 27: return localizer.Text("catalog.name.mon_27", "Monster 27");
        case 28: return localizer.Text("catalog.name.mon_28", "Monster 28");
        case 29: return localizer.Text("catalog.name.mon_29", "Monster 29");
        case 30: return localizer.Text("catalog.name.mon_30", "Monster 30");
        case 31: return localizer.Text("catalog.name.mon_31", "Monster 31");
        case 33: return localizer.Text("catalog.name.mon_33", "Monster 33");
        case 34: return localizer.Text("catalog.name.mon_34", "Monster 34");
        case 35: return localizer.Text("catalog.name.mon_35", "Monster 35");
        case 36: return localizer.Text("catalog.name.mon_36", "Monster 36");
        case 37: return localizer.Text("catalog.name.mon_37", "Monster 37");
        case 38: return localizer.Text("catalog.name.mon_38", "Monster 38");
        case 39: return localizer.Text("catalog.name.mon_39", "Monster 39");
        case 40: return localizer.Text("catalog.name.mon_40", "Monster 40");
        case 41: return localizer.Text("catalog.name.mon_41", "Monster 41");
        case 46: return localizer.Text("catalog.name.mon_46", "Monster 46");
        case 48: return localizer.Text("catalog.name.mon_48", "Monster 48");
        case 50: return localizer.Text("catalog.name.mon_50", "Monster 50");
        case 51: return localizer.Text("catalog.name.mon_51", "Monster 51");
        case 56: return localizer.Text("catalog.name.mon_56", "Monster 56");
    }
    return {};
}

inline std::string BlueprintSpecificName(std::string_view name,
                                         const anomaly::sdk::Localizer& localizer) {
    // These overrides are verified DescriptiveName/string-table identities.
    if (name.find("boss_33_056summon_") != std::string_view::npos)
        return localizer.Text("catalog.name.mon_56", "Monster 56");
    if (name.find("boss_34b_maki_") != std::string_view::npos)
        return localizer.Text("catalog.name.boss_34_maki", "Boss 34 (Maki)");
    if (BlueprintFamilyNumber(name, "boss_") == 28) {
        if (name.find("_challenge_easy_") != std::string_view::npos)
            return localizer.Text("catalog.name.boss_28_easy", "Boss 28 (Easy)");
        if (name.find("_challenge_normal_") != std::string_view::npos)
            return localizer.Text("catalog.name.boss_28_normal", "Boss 28 (Normal)");
        if (name.find("_challenge_hard_") != std::string_view::npos)
            return localizer.Text("catalog.name.boss_28_hard", "Boss 28 (Hard)");
    }
    const int monster = BlueprintFamilyNumber(name, "mon_");
    if (monster == 2 && name.find("_ghost_") != std::string_view::npos)
        return localizer.Text("catalog.name.mon_02_ghost", "Monster 02 (Ghost)");
    if (name.find("mon_18_2_") != std::string_view::npos)
        return localizer.Text("catalog.name.mon_18_2", "Monster 18 (Modified)");
    if (monster == 35) {
        if (name.find("_red_") != std::string_view::npos)
            return localizer.Text("catalog.name.mon_35_red", "Monster 35 (Red)");
        if (name.find("_blue_") != std::string_view::npos)
            return localizer.Text("catalog.name.mon_35_blue", "Monster 35 (Blue)");
    }
    return {};
}

inline BlueprintNames ResolveBlueprintNames(std::string_view name,
                                            const anomaly::sdk::Localizer& localizer) {
    std::string folded(name);
    for (char& ch : folded) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return {BlueprintFamilyName(folded, localizer), BlueprintSpecificName(folded, localizer)};
}

}  // namespace summon_lab
