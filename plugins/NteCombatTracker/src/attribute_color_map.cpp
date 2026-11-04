#include "attribute_color_map.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace nte::combat_stats {
namespace {

[[nodiscard]] std::uint32_t ParseColor(const std::string_view value) {
    if (value.size() != 7 || value.front() != '#') {
        throw std::runtime_error("color must use #RRGGBB format");
    }
    std::uint32_t color{};
    const auto [end, error] = std::from_chars(
        value.data() + 1, value.data() + value.size(), color, 16);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("color must use hexadecimal #RRGGBB format");
    }
    return color;
}

}  // namespace

AttributeColorMap AttributeColorMap::Load(
    const std::filesystem::path& path,
    std::string& error) {
    error.clear();
    AttributeColorMap result;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("attribute color dictionary is unavailable: " +
                path.string());
        }

        std::unordered_set<std::string> keys;
        const auto reject_duplicates = [&keys](
            const int,
            const nlohmann::json::parse_event_t event,
            nlohmann::json& parsed) {
            if (event == nlohmann::json::parse_event_t::key &&
                !keys.insert(parsed.get<std::string>()).second) {
                throw std::runtime_error(
                    "duplicate attribute color key: " + parsed.get<std::string>());
            }
            return true;
        };
        const auto dictionary = nlohmann::json::parse(stream, reject_duplicates);
        if (!dictionary.is_object()) {
            throw std::runtime_error("attribute color dictionary root must be an object");
        }

        // Keys are the ECharacterElementType suffixes the plugin resolves from
        // the live DT_Character rows; they are game identifiers, not UI copy.
        static constexpr std::array<std::string_view, 6> required{
            "CHAOS", "COSMOS", "INCANTATION", "LAKSHANA", "NATURE", "PSYCHE"};
        if (dictionary.size() != required.size()) {
            throw std::runtime_error(
                "attribute color dictionary must contain exactly six attributes");
        }
        for (const auto attribute : required) {
            const auto found = dictionary.find(attribute);
            if (found == dictionary.end() || !found->is_string()) {
                throw std::runtime_error(
                    "missing or invalid attribute color: " + std::string(attribute));
            }
            result.colors_.emplace(
                attribute, ParseColor(found->get_ref<const std::string&>()));
        }
    } catch (const std::exception& exception) {
        result.colors_.clear();
        error = exception.what();
    }
    return result;
}

std::optional<std::uint32_t> AttributeColorMap::Find(
    const std::string_view attribute) const {
    const auto found = colors_.find(std::string(attribute));
    return found == colors_.end()
        ? std::nullopt
        : std::optional<std::uint32_t>{found->second};
}

}  // namespace nte::combat_stats
