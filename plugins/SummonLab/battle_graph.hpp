#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace summon_lab {

// Stable record IDs, independent of list order and pagination. Each edge is
// undirected; there is no shared opponent selection or exclusive pairing.
struct BattleGraph {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> edges;

    bool Linked(std::uint64_t a, std::uint64_t b) const {
        if (a > b) std::swap(a, b);
        return std::find(edges.begin(), edges.end(), std::pair{a, b}) != edges.end();
    }
    bool Contains(std::uint64_t id) const {
        return std::any_of(edges.begin(), edges.end(), [id](const auto& edge) {
            return edge.first == id || edge.second == id;
        });
    }
    void Set(std::uint64_t a, std::uint64_t b, bool enabled) {
        if (a == 0 || b == 0 || a == b) return;
        if (a > b) std::swap(a, b);
        const auto edge = std::pair{a, b};
        if (enabled) {
            if (!Linked(a, b)) edges.push_back(edge);
        } else std::erase(edges, edge);
    }
    void Remove(std::uint64_t id) {
        std::erase_if(edges, [id](const auto& edge) {
            return edge.first == id || edge.second == id;
        });
    }
    std::vector<std::uint64_t> Component(std::uint64_t root) const {
        std::vector<std::uint64_t> ids{root};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            for (const auto& [a, b] : edges) {
                const auto other = a == ids[i] ? b : b == ids[i] ? a : 0;
                if (other != 0 && std::find(ids.begin(), ids.end(), other) == ids.end())
                    ids.push_back(other);
            }
        }
        return ids;
    }
};

struct BattleCamp {
    std::uint64_t id{};
    std::uint8_t previous{};
    int assigned{-1};
};

// The palette comes from the live native attitude table and contains only
// mutually hostile camps. Prefer existing camps; unused colors are symmetric.
inline bool AssignBattleCamps(const BattleGraph& graph, const std::vector<std::uint8_t>& palette,
                              std::vector<BattleCamp>& actors) {
    auto solve = [&](auto&& self) -> bool {
        BattleCamp* selected{};
        std::size_t best_saturation{}, best_degree{};
        for (auto& actor : actors) {
            if (actor.assigned >= 0) continue;
            std::vector<int> neighbors;
            std::size_t degree{};
            for (const auto& other : actors) {
                if (!graph.Linked(actor.id, other.id)) continue;
                ++degree;
                if (other.assigned >= 0 &&
                    std::find(neighbors.begin(), neighbors.end(), other.assigned) == neighbors.end())
                    neighbors.push_back(other.assigned);
            }
            if (selected == nullptr || neighbors.size() > best_saturation ||
                (neighbors.size() == best_saturation && degree > best_degree)) {
                selected = &actor;
                best_saturation = neighbors.size();
                best_degree = degree;
            }
        }
        if (selected == nullptr) return true;
        auto choices = palette;
        const auto previous = std::find(choices.begin(), choices.end(), selected->previous);
        if (previous != choices.end()) std::rotate(choices.begin(), previous, previous + 1);
        bool tried_unused{};
        for (const auto camp : choices) {
            bool conflict{}, used{};
            for (const auto& other : actors) {
                if (other.assigned != camp) continue;
                used = true;
                conflict = conflict || graph.Linked(selected->id, other.id);
            }
            if (conflict || (!used && tried_unused)) continue;
            if (!used) tried_unused = true;
            selected->assigned = camp;
            if (self(self)) return true;
            selected->assigned = -1;
        }
        return false;
    };
    return solve(solve);
}

} // namespace summon_lab
