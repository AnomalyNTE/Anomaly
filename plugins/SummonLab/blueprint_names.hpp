#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>

namespace summon_lab {

struct BlueprintNames { std::string family, specific; };
using ActorNameTable = std::map<std::string, std::string, std::less<>>;

inline std::string NameIdentity(std::string_view name) {
    std::string value(name);
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    if (value.ends_with("_name")) value.resize(value.size() - 5);
    if (value.ends_with("_c")) value.resize(value.size() - 2);
    for (auto at = value.find("_bp"); at != std::string::npos; at = value.find("_bp", at)) {
        if (at + 3 == value.size() || value[at + 3] == '_') value.erase(at, 3);
        else at += 3;
    }
    // Numeric table keys mix mon_012 / mon_12 and single-digit spellings.
    for (const auto prefix : {"mon_", "boss_"}) {
        const auto at = value.find(prefix);
        if (at == std::string::npos) continue;
        const auto start = at + std::char_traits<char>::length(prefix);
        auto end = value.find_first_not_of("0123456789", start);
        if (end == std::string::npos) end = value.size();
        while (end > start + 1 && value[start] == '0') { value.erase(start, 1); --end; }
    }
    return value;
}

// Asset Registry paths provide candidates without loading their classes. Keep
// phase folders separate and require a suffix boundary (Boss_3 != Boss_38).
// This narrows the normal source menu; advanced selection uses the full catalog.
inline bool IsCombatConfigCandidate(std::string_view base, std::string_view candidate) {
    if (base == candidate) return false;
    const auto base_slash = base.rfind('/'), candidate_slash = candidate.rfind('/');
    const auto base_dot = base.rfind('.'), candidate_dot = candidate.rfind('.');
    if (base_slash == std::string_view::npos || candidate_slash == std::string_view::npos ||
        base_dot == std::string_view::npos || candidate_dot == std::string_view::npos ||
        base.substr(0, base_slash) != candidate.substr(0, candidate_slash)) return false;
    const auto identity = NameIdentity(base.substr(base_dot + 1));
    const auto variant = NameIdentity(candidate.substr(candidate_dot + 1));
    return !identity.empty() && variant.starts_with(identity + "_");
}

inline BlueprintNames ResolveBlueprintNames(std::string_view name, const ActorNameTable& table) {
    const auto identity = NameIdentity(name);
    BlueprintNames result;
    std::size_t shortest = std::string::npos, longest{};
    for (const auto& [key, text] : table) {
        if (key.empty() || text.empty()) continue;
        auto at = identity.find(key);
        while (at != std::string::npos) {
            const auto end = at + key.size();
            if ((at == 0 || identity[at - 1] == '_') &&
                (end == identity.size() || identity[end] == '_')) {
                if (key.size() < shortest) { shortest = key.size(); result.family = text; }
                if (key.size() > longest) { longest = key.size(); result.specific = text; }
                break;
            }
            at = identity.find(key, at + 1);
        }
    }
    return result;
}

} // namespace summon_lab
