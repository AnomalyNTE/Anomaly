#include "attribute_color_map.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
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

std::string Lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
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

        // 属性名是 dict/attribute_colors.json 的键，必须与上游数据逐字一致；
        // 改变它们会破坏字典匹配，因此不参与本地化（i18n-data）。
        static constexpr std::array<std::string_view, 6> required{
            "灵", "相", "暗", "光", "魂", "咒"};  // i18n-data
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

AttributeColorMap AttributeColorMap::Load(
    const std::filesystem::path& color_path,
    const std::filesystem::path& character_directory,
    std::string& error) {
    AttributeColorMap result = Load(color_path, error);
    if (!error.empty()) return result;
    try {
        if (!std::filesystem::is_directory(character_directory)) {
            throw std::runtime_error("character dictionary directory is unavailable: " +
                character_directory.string());
        }
        std::size_t loaded_files{};
        for (const auto& item :
            std::filesystem::directory_iterator(character_directory)) {
            if (!item.is_regular_file() || item.path().extension() != ".json") {
                continue;
            }
            std::ifstream stream(item.path(), std::ios::binary);
            if (!stream) {
                throw std::runtime_error("character dictionary file is unavailable: " +
                    item.path().string());
            }
            const auto document = nlohmann::json::parse(stream);
            if (!document.contains("data") ||
                !document.at("data").contains("characters")) {
                ++loaded_files;
                continue;
            }
            for (const auto& [id_text, value] :
                document.at("data").at("characters").items()) {
                const auto id = std::stoul(id_text);
                if (id < 1000 || id > 1999) continue;
                const std::string codename =
                    value.at("codename").get<std::string>();
                const std::string attribute =
                    value.at("attribute").get<std::string>();
                if (!result.colors_.contains(attribute)) {
                    throw std::runtime_error(
                        "unknown character attribute: " + attribute);
                }
                std::ostringstream token;
                token << "player_" << std::setw(3) << std::setfill('0')
                      << id - 1000 << '_' << codename;
                result.participant_attributes_.emplace(
                    Lower(token.str()), attribute);
            }
            ++loaded_files;
        }
        if (loaded_files == 0) {
            throw std::runtime_error(
                "character dictionary directory contains no JSON files");
        }
    } catch (const std::exception& exception) {
        result.participant_attributes_.clear();
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

std::optional<std::uint32_t> AttributeColorMap::FindParticipant(
    const std::string_view technical_path) const {
    const std::string lower = Lower(std::string(technical_path));
    for (const auto& [token, attribute] : participant_attributes_) {
        if (lower.find(token) != std::string::npos) return Find(attribute);
    }
    return std::nullopt;
}

}  // namespace nte::combat_stats
