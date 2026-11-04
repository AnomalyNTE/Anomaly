#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nte::combat_stats {

// The six attribute colours. This is pure presentation and stays a checked-in
// asset keyed by the game's ECharacterElementType suffix. The character to
// attribute relation is not stored here: the plugin resolves it from the live
// DT_Character rows and ships the element with each player row.
class AttributeColorMap {
public:
    [[nodiscard]] static AttributeColorMap Load(
        const std::filesystem::path& path,
        std::string& error);

    [[nodiscard]] std::optional<std::uint32_t> Find(
        std::string_view attribute) const;

private:
    std::unordered_map<std::string, std::uint32_t> colors_;
};

}  // namespace nte::combat_stats
