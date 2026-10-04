#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace accessory {
struct Name {
    std::uint32_t index{}, number{};
    bool operator==(const Name&) const = default;
};
struct NameArray { Name* data{}; std::int32_t count{}, capacity{}; };
struct Appearance { Name character, fashion; NameArray decorations; };
static_assert(sizeof(Name) == 8 && sizeof(NameArray) == 16 && sizeof(Appearance) == 32);
struct ItemKey { Name id; std::uint8_t part{}; Name type{}; };
enum class Mode { Original, ReplacePart, HideAll };
struct Choice { Mode mode{Mode::Original}; ItemKey item; };
constexpr std::size_t MaxAccessories = 64;
inline bool SameSlot(ItemKey a, ItemKey b) {
    return a.part == b.part && a.type == b.type;
}
inline bool ComposeSelections(std::span<const Name> original, std::span<const ItemKey> catalog,
                              std::span<const ItemKey> selected, bool hide_all,
                              std::array<Name, MaxAccessories>& result, int& count) {
    count = 0;
    if (original.size() > MaxAccessories || selected.size() > MaxAccessories) return false;
    if (hide_all) return true;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        bool listed = false;
        for (auto item : catalog)
            if (item.id == selected[i].id && SameSlot(item, selected[i])) listed = true;
        if (!listed || !selected[i].id.index || selected[i].part < 1 || selected[i].part > 4) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (SameSlot(selected[i], selected[j])) return false;
    }
    for (auto name : original) {
        bool replace = false;
        for (auto item : catalog) if (item.id == name)
            for (auto chosen : selected) if (SameSlot(item, chosen)) replace = true;
        if (!replace) result[count++] = name;
    }
    for (auto chosen : selected) {
        if (count == static_cast<int>(MaxAccessories)) return false;
        result[count++] = chosen.id;
    }
    return true;
}
inline std::string_view Placement(std::string_view type, std::uint8_t part) {
    if (part == 1 && type == "EYE") return "眼部";
    if (part == 1 && type == "OverHead") return "头顶";
    switch (part) {
    case 1: return "头部";
    case 2: return "身体前侧";
    case 3: return "背部";
    case 4: return "腿部";
    default: return "未知";
    }
}

// Preserve unknown accessories and every other slot. Never mutate the original array.
inline bool Compose(std::span<const Name> original, std::span<const ItemKey> catalog,
                    Choice choice, std::array<Name, MaxAccessories>& result, int& count) {
    count = 0;
    if (original.size() > MaxAccessories) return false;
    if (choice.mode == Mode::HideAll) return true;
    if (choice.mode == Mode::ReplacePart) {
        bool found = false;
        for (auto key : catalog) if (key.id == choice.item.id && key.part == choice.item.part) found = true;
        if (!found || choice.item.id.index == 0 || choice.item.part < 1 || choice.item.part > 4) return false;
    }
    for (auto name : original) {
        bool replace = false;
        if (choice.mode == Mode::ReplacePart) {
            for (auto key : catalog) if (key.id == name && key.part == choice.item.part) { replace = true; break; }
        }
        if (!replace) result[count++] = name;
    }
    if (choice.mode == Mode::ReplacePart) {
        if (count == static_cast<int>(MaxAccessories)) return false;
        result[count++] = choice.item.id;
    }
    return true;
}
} // namespace accessory
