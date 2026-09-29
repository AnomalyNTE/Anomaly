#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace cosmetic_profiles {
struct Look {
    std::map<std::string, std::string> accessories;
    bool hidden{};
    std::string glider;
};
using Profiles = std::map<std::string, Look>;

inline void SetAccessory(Profiles& profiles, const std::string& character,
                         std::string slot, std::string id) {
    auto& look = profiles[character];
    look.accessories[std::move(slot)] = std::move(id);
    look.hidden = false;
}
inline void ResetAccessorySlot(Profiles& profiles, const std::string& character,
                               const std::string& slot) {
    profiles[character].accessories.erase(slot);
}
inline void HideAccessories(Profiles& profiles, const std::string& character) {
    auto& look = profiles[character];
    look.accessories.clear();
    look.hidden = true;
}
inline void ResetAccessories(Profiles& profiles, const std::string& character) {
    auto& look = profiles[character];
    look.accessories.clear();
    look.hidden = false;
}
inline void SetGlider(Profiles& profiles, const std::string& character, std::string id) {
    profiles[character].glider = std::move(id);
}
inline void ResetGlider(Profiles& profiles, const std::string& character) {
    profiles[character].glider.clear();
}
} // namespace cosmetic_profiles
