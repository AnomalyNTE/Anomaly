#include "algorithm_support.hpp"
#include <array>
#include <bit>
#include <map>
#include <vector>

namespace equipment_manager {
namespace {
using namespace algorithm_detail;
struct Tile {
    std::string id;
    std::vector<std::array<int, 2>> cells;
    int count{}, used{};
};
struct Position { std::size_t tile{}; int row{}, column{}; std::uint32_t mask{}; };
struct BlueprintSearch {
    Budget budget;
    std::vector<Tile> tiles;
    std::array<std::vector<Position>, 25> choices;
    std::map<std::string, int, std::less<>> required;
    std::uint32_t board{};
    int required_count{}, extra_area{}, best_extra{-1};
    std::size_t maximum_solutions{};
    bool variants{};
    Json solutions = Json::array();
    std::vector<Position> positions;
    std::set<std::string> signatures;
    BlueprintSearch(const Json& request, const AlgorithmCancelCheck& check, const AlgorithmProgressCallback& progress)
        : budget(request, check, progress, false) {}
    void Visit(std::uint32_t occupied) {
        if (budget.Stop()) return;
        ++budget.nodes;
        if (occupied == board) {
            int matched{}, extra{};
            Json counts = Json::object();
            for (const auto& tile : tiles) if (tile.used) {
                counts[tile.id] = tile.used;
                const auto found = required.find(tile.id);
                if (found != required.end()) matched += std::min(found->second, tile.used);
                if (static_cast<int>(tile.cells.size()) == extra_area) extra += tile.used;
            }
            if (matched < required_count) return;
            if (extra_area && extra < best_extra) return;
            if (extra_area && extra > best_extra) { solutions = Json::array(); signatures.clear(); best_extra = extra; }
            const auto signature = counts.dump();
            if (!variants && !signatures.insert(signature).second) return;
            Json placements = Json::array();
            for (const auto& place : positions) placements.push_back({{"shape_id", tiles[place.tile].id}, {"row", place.row}, {"column", place.column}});
            solutions.push_back({{"placements", placements}, {"shape_counts", counts}, {"extra_count", extra}});
            if (solutions.size() >= maximum_solutions) { budget.exhaustive = false; budget.reason = "solution_limit"; }
            return;
        }
        const auto target = std::countr_zero(board & ~occupied);
        for (const auto& place : choices[target]) {
            auto& tile = tiles[place.tile];
            if (tile.used >= tile.count || (place.mask & occupied)) continue;
            ++tile.used; positions.push_back(place);
            Visit(occupied | place.mask);
            positions.pop_back(); --tile.used;
            if (!budget.exhaustive) return;
        }
    }
};
Json Blueprints(const Json& request, const AlgorithmCancelCheck& cancel, const AlgorithmProgressCallback& progress) {
    if (!request.is_object() || !request.contains("allowed_cells") || !request.at("allowed_cells").is_array() ||
        !request.contains("shapes") || !request.at("shapes").is_array()) Fail();
    BlueprintSearch search(request, cancel, progress);
    search.maximum_solutions = request.contains("max_solutions") ? static_cast<std::size_t>(Integer(request.at("max_solutions"), 1, 2000)) : 100;
    search.variants = request.value("layout_variants", false);
    search.extra_area = request.contains("extra_shape_area") ? Integer(request.at("extra_shape_area"), 1, 4) : 0;
    for (const auto& cell : request.at("allowed_cells")) {
        if (!cell.is_array() || cell.size() != 2) Fail();
        const auto bit = 1U << ((Integer(cell[0], 1, 5) - 1) * 5 + Integer(cell[1], 1, 5) - 1);
        if (search.board & bit) Fail();
        search.board |= bit;
    }
    if (!search.board || request.at("shapes").size() > 64) Fail();
    std::set<std::string> ids;
    for (const auto& value : request.at("shapes")) {
        Tile tile;
        tile.id = Id(value.at("shape_id"));
        if (!ids.insert(tile.id).second || !value.at("cells").is_array() || value.at("cells").empty() || value.at("cells").size() > 4) Fail();
        tile.count = value.contains("count") ? Integer(value.at("count"), 0, 25) : 25;
        for (const auto& point : value.at("cells")) {
            if (!point.is_array() || point.size() != 2) Fail();
            const std::array<int, 2> cell{Integer(point[0], -4, 4), Integer(point[1], -4, 4)};
            if (std::find(tile.cells.begin(), tile.cells.end(), cell) != tile.cells.end()) Fail();
            tile.cells.push_back(cell);
        }
        search.tiles.push_back(std::move(tile));
    }
    const auto mode = request.value("set_effect_mode", std::string("four_piece"));
    if (mode != "four_piece" && mode != "two_piece" && mode != "none") Fail();
    if (request.contains("required_shapes") && mode != "none") {
        if (!request.at("required_shapes").is_array()) Fail();
        for (const auto& value : request.at("required_shapes")) {
            const auto id = Id(value); if (!ids.contains(id)) Fail();
            ++search.required[id]; ++search.required_count;
        }
        if (mode == "two_piece") search.required_count = std::min(2, search.required_count);
    }
    for (std::size_t index{}; index < search.tiles.size(); ++index) {
        if (search.budget.Stop()) break;
        for (int row = 1; row <= 5; ++row) for (int column = 1; column <= 5; ++column) {
            std::uint32_t mask{};
            for (const auto& cell : search.tiles[index].cells) {
                const auto r = row + cell[0], c = column + cell[1];
                if (r < 1 || r > 5 || c < 1 || c > 5) { mask = 0; break; }
                mask |= 1U << ((r - 1) * 5 + c - 1);
            }
            if (!mask || (mask & search.board) != mask) continue;
            for (std::size_t bit{}; bit < search.choices.size(); ++bit)
                if (mask & (1U << bit)) search.choices[bit].push_back({index, row, column, mask});
        }
    }
    search.budget.Phase(AlgorithmProgressPhase::blueprints);
    search.Visit(0);
    const bool valid = !search.solutions.empty();
    return {{"valid", valid}, {"blueprints", search.solutions}, {"search", search.budget.Report()},
        {"reason", valid ? "" : search.budget.exhaustive ? "no_solution" : "budget_exhausted"}};
}
} // namespace
Json GenerateBlueprints(const Json& request, const AlgorithmCancelCheck& cancel_check, const AlgorithmProgressCallback& progress) {
    try { return Blueprints(request, cancel_check, progress); } catch (const Json::exception&) { Fail(); }
}
} // namespace equipment_manager
