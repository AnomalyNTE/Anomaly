#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace awakening_data {
struct Name { std::uint32_t id{}, number{}; bool operator==(const Name&) const = default; };
struct Effect { std::uint8_t unlocked{}, padding[3]{}; Name name{}; bool operator==(const Effect&) const = default; };
using Effects = std::array<Effect, 6>;
static_assert(sizeof(Effect) == 12 && sizeof(Effects) == 72);

// Existing choices may be in a different order. Keep them and fill locked slots
// with the unused effects rather than changing a player's selected upgrades.
inline std::optional<Effects> Unlock(const Effects& original, const std::array<Name, 6>& names) {
    std::array<bool, 6> used{};
    for (size_t i = 0; i < names.size(); ++i) {
        if (!names[i].id || names[i].number) return {};
        for (size_t j = 0; j < i; ++j) if (names[i] == names[j]) return {};
    }
    auto result = original;
    for (const auto& effect : original) {
        if (effect.unlocked > 1) return {};
        if (!effect.unlocked) continue;
        size_t match = names.size();
        for (size_t i = 0; i < names.size(); ++i) if (effect.name == names[i]) match = i;
        if (match == names.size() || used[match]) return {};
        used[match] = true;
    }
    size_t next{};
    for (auto& effect : result) if (!effect.unlocked) {
        while (next < used.size() && used[next]) ++next;
        if (next == used.size()) return {};
        effect.unlocked = 1; effect.name = names[next]; used[next] = true;
    }
    return result;
}
// Restore only bytes still owned by the preview. An inventory update from the
// game takes precedence over our backup, independently for each node and level.
inline Effects Restore(const Effects& current, const Effects& applied, const Effects& before) {
    auto result = current;
    for (size_t i = 0; i < result.size(); ++i)
        if (current[i] == applied[i]) result[i] = before[i];
    return result;
}
}
