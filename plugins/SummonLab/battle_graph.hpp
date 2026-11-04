#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace summon_lab {

// Stable record IDs, independent of list order and pagination. Each edge is
// undirected; there is no shared opponent selection or exclusive pairing.
struct BattleGraph {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> edges;

    bool Linked(std::uint64_t a, std::uint64_t b) const {
        if (a > b) std::swap(a, b);
        return std::binary_search(edges.begin(), edges.end(), std::pair{a, b});
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
        const auto position = std::lower_bound(edges.begin(), edges.end(), edge);
        if (enabled) {
            if (position == edges.end() || *position != edge) edges.insert(position, edge);
        } else if (position != edges.end() && *position == edge) edges.erase(position);
    }
    void SetBetween(const std::vector<std::uint64_t>& first,
                    const std::vector<std::uint64_t>& second, bool enabled) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> changes;
        changes.reserve(first.size() * second.size());
        for (const auto a : first)
            for (const auto b : second)
                if (a != 0 && b != 0 && a != b) changes.emplace_back(std::min(a, b), std::max(a, b));
        std::sort(changes.begin(), changes.end());
        changes.erase(std::unique(changes.begin(), changes.end()), changes.end());
        std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
        if (enabled)
            std::set_union(edges.begin(), edges.end(), changes.begin(), changes.end(),
                std::back_inserter(result));
        else
            std::set_difference(edges.begin(), edges.end(), changes.begin(), changes.end(),
                std::back_inserter(result));
        edges = std::move(result);
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
    // Build adjacency once: hundreds of actors must not search the complete
    // edge list again for every candidate at every coloring step.
    std::vector<std::vector<std::size_t>> neighbors(actors.size());
    for (std::size_t a = 0; a < actors.size(); ++a)
        for (std::size_t b = a + 1; b < actors.size(); ++b)
            if (graph.Linked(actors[a].id, actors[b].id)) {
                neighbors[a].push_back(b);
                neighbors[b].push_back(a);
            }
    auto solve = [&](auto&& self) -> bool {
        BattleCamp* selected{};
        std::size_t selected_index{};
        std::size_t best_saturation{}, best_degree{};
        for (std::size_t i = 0; i < actors.size(); ++i) {
            auto& actor = actors[i];
            if (actor.assigned >= 0) continue;
            bool colors[256]{};
            std::size_t saturation{};
            for (const auto index : neighbors[i]) {
                const auto camp = actors[index].assigned;
                if (camp >= 0 && !colors[camp]) {
                    colors[camp] = true;
                    ++saturation;
                }
            }
            const auto degree = neighbors[i].size();
            if (selected == nullptr || saturation > best_saturation ||
                (saturation == best_saturation && degree > best_degree)) {
                selected = &actor;
                selected_index = i;
                best_saturation = saturation;
                best_degree = degree;
            }
        }
        if (selected == nullptr) return true;
        auto choices = palette;
        const auto previous = std::find(choices.begin(), choices.end(), selected->previous);
        if (previous != choices.end()) std::rotate(choices.begin(), previous, previous + 1);
        bool tried_unused{};
        for (const auto camp : choices) {
            const bool conflict = std::any_of(neighbors[selected_index].begin(),
                neighbors[selected_index].end(), [&](auto index) { return actors[index].assigned == camp; });
            bool used{};
            for (const auto& other : actors) {
                if (other.assigned != camp) continue;
                used = true;
                break;
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
