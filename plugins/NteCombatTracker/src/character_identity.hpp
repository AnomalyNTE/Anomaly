#pragma once

// Identity helpers shared by the runtime character-attribute lookup and its
// tests. Deliberately free of host headers so it can be checked without a host.

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace nte::combat_stats {

// A participant object path spells a character as player_003_sagiri, while
// DT_Character keys rows by the numeric id 1003. Object paths spell the segment
// either way (Player_003_Sagiri and player_021_edgar both occur), so the match
// is case-insensitive. Returns the row key, or an empty string when the path
// carries no character segment.
[[nodiscard]] inline std::string CharacterIdFromPath(const std::string_view object_path) {
    constexpr std::string_view marker = "player_";
    for (std::size_t start = 0; start + marker.size() <= object_path.size(); ++start) {
        bool matches = true;
        for (std::size_t i = 0; i < marker.size(); ++i) {
            const auto character = static_cast<unsigned char>(object_path[start + i]);
            if (std::tolower(character) != marker[i]) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;

        std::size_t cursor = start + marker.size();
        std::string digits;
        while (cursor < object_path.size() &&
               std::isdigit(static_cast<unsigned char>(object_path[cursor])) &&
               digits.size() <= 4) {
            digits.push_back(object_path[cursor]);
            ++cursor;
        }
        if (!digits.empty() && digits.size() <= 4 && cursor < object_path.size() &&
            object_path[cursor] == '_') {
            // Dict keys are four digits wide: player_003 is row 1003, so the
            // segment is padded to three and prefixed with the series digit.
            // A segment that already fills the width is left as it is.
            if (digits.size() >= 4) return digits;
            return "1" + std::string(3 - digits.size(), '0') + digits;
        }
    }
    return {};
}

}  // namespace nte::combat_stats
