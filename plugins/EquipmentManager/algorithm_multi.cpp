#include "algorithm_support.hpp"
#include <array>
#include <deque>
#include <map>
#include <queue>
#include <vector>

namespace equipment_manager {
namespace {
using namespace algorithm_detail;
using Priority = std::array<int, 4>;
Priority PriorityOf(const Json& plan) {
    Priority result{};
    if (plan.contains("priority_counts"))
        for (std::size_t i{}; i < result.size(); ++i) result[i] = plan.at("priority_counts").at(i).get<int>();
    return result;
}
bool BetterPlan(const Json& a, const Json& b) {
    if (PriorityOf(a) != PriorityOf(b)) return PriorityOf(a) > PriorityOf(b);
    return a.value("ranking_score", a.value("score", 0.)) > b.value("ranking_score", b.value("score", 0.));
}
std::string Signature(const std::set<std::uint64_t>& ids) {
    std::string result;
    for (const auto id : ids) { result += std::to_string(id); result.push_back(','); }
    return result;
}
std::set<std::uint64_t> Excluded(const Json& request) {
    std::set<std::uint64_t> result;
    if (request.contains("exclude_uids")) {
        if (!request.at("exclude_uids").is_array()) Fail();
        for (const auto& uid : request.at("exclude_uids")) result.insert(Key(uid));
    }
    return result;
}
void SetExcluded(Json& request, const std::set<std::uint64_t>& ids) {
    request["exclude_uids"] = Json::array();
    for (const auto uid : ids) request["exclude_uids"].push_back(Uid(uid));
}
std::set<std::uint64_t> Kept(const Json& request) {
    if (!request.contains("keep")) return {};
    Json plan = request.at("keep");
    if (!plan.contains("placements")) plan["placements"] = Json::array();
    return Used(plan);
}
struct Candidate { Json plan; std::set<std::uint64_t> excluded; };
struct Worse { bool operator()(const Candidate& a, const Candidate& b) const { return BetterPlan(b.plan, a.plan); } };
using PreparedSolvers = std::map<std::string, std::unique_ptr<PreparedLoadout>>;

// Enumerating UID exclusions is the same public Top-K boundary used by the
// reference facade. The visitor lets joint search try its first complete plan
// before spending the shared budget on the remaining alternatives.
bool Enumerate(const Json& request, const Json& inventory, Budget& budget, std::size_t maximum,
               const std::function<bool(const Json&)>& visitor, SearchExecutor* executor = nullptr, bool feasibility = false,
               std::size_t remaining_roles = 1, std::size_t minimum_nodes = 1, const Json* exact_seed = nullptr,
               const InventoryCapacities& capacities = {}, std::unique_ptr<PreparedLoadout>* prepared = nullptr,
               const std::function<std::string(const std::set<std::uint64_t>&)>& stock_signature = {}) {
    const auto frozen = Kept(request);
    const auto initial_excluded = Excluded(request);
    std::set<std::string> seen, emitted;
    std::priority_queue<Candidate, std::vector<Candidate>, Worse> pending;
    bool child_limited{};
    std::unique_ptr<PreparedLoadout> local_solver;
    auto& solver = prepared ? *prepared : local_solver;
    const auto solve = [&](const std::set<std::uint64_t>& excluded) {
        if (budget.Stop()) return;
        auto signature = [&] {
            if (!stock_signature) return Signature(excluded);
            auto additional = excluded;
            for (const auto uid : initial_excluded) additional.erase(uid);
            return stock_signature(additional);
        }();
        if (!seen.insert(std::move(signature)).second) return;
        auto child = request; SetExcluded(child, excluded); budget.Child(child);
        if (feasibility) {
            child["max_nodes"] = std::min(budget.maximum_nodes - budget.nodes,
                std::max(minimum_nodes, (budget.maximum_nodes - budget.nodes) / remaining_roles));
            child["time_limit_ms"] = std::max<std::int64_t>(1,
                child.at("time_limit_ms").get<std::int64_t>() / static_cast<std::int64_t>(remaining_roles));
        }
        if (!solver) solver = std::make_unique<PreparedLoadout>(child, inventory, executor);
        auto plan = solver->Solve(child, capacities, budget.cancel, budget.ChildProgress(), feasibility);
        child_limited = child_limited || !plan.at("search").value("exhaustive", false);
        budget.Account(plan, !feasibility);
        if (plan.value("valid", false)) pending.push({std::move(plan), excluded});
    };
    bool reusable = exact_seed && exact_seed->value("valid", false) && exact_seed->at("search").value("exhaustive", false);
    if (reusable) {
        const auto ids = Used(*exact_seed);
        for (const auto uid : ids) if (initial_excluded.contains(uid)) { reusable = false; break; }
        for (const auto& capacity : capacities) {
            const auto count = static_cast<std::size_t>(std::count_if(ids.begin(), ids.end(), [&](auto uid) { return capacity.items.contains(uid); }));
            if (count > capacity.maximum) { reusable = false; break; }
        }
    }
    if (reusable) {
        // A proven optimum that remains feasible under the new exclusions is
        // still optimal. Its work was already charged by the independent pass.
        seen.insert(stock_signature ? stock_signature({}) : Signature(initial_excluded)); pending.push({*exact_seed, initial_excluded});
    } else solve(initial_excluded);
    std::size_t count{};
    while (!pending.empty()) {
        if (budget.cancel && budget.cancel()) Fail(AlgorithmFailure::cancelled);
        const auto candidate = pending.top(); pending.pop();
        const auto ids = Used(candidate.plan);
        if (emitted.insert(stock_signature ? stock_signature(ids) : Signature(ids)).second) {
            ++count;
            if (!visitor(candidate.plan)) return false;
        }
        if (count >= maximum || budget.Stop()) return false;
        for (const auto uid : ids) if (!frozen.contains(uid)) {
            auto excluded = candidate.excluded; excluded.insert(uid); solve(excluded);
            if (!budget.exhaustive) break;
        }
    }
    return budget.exhaustive && !child_limited;
}

Json TopK(const Json& request, const Json& inventory, const AlgorithmCancelCheck& cancel, const AlgorithmProgressCallback& progress) {
    Snapshot(request, inventory);
    Budget budget(request, cancel, progress);
    SearchExecutor executor(SearchWorkers(request));
    budget.Phase(AlgorithmProgressPhase::alternatives);
    const auto count = request.contains("top_k") ? Integer(request.at("top_k"), 1, algorithm_limits::kMaximumTopK) : 5;
    Json options = Json::array();
    const auto complete = Enumerate(request, inventory, budget, count, [&](const Json& plan) { options.push_back(plan); return true; }, &executor);
    Json result{{"snapshot_id", request.at("snapshot_id")}, {"character", request.value("character", Json(nullptr))},
        {"native_assemblable", !SoftwareRole(request)}, {"valid", !options.empty()}, {"options", options}, {"enumeration_complete", complete}, {"search", budget.Report()}};
    if (request.contains("role_id")) result["role_id"] = request.at("role_id");
    return result;
}

struct Reservation {
    std::size_t plan{};
    std::uint64_t original{};
    std::vector<std::uint64_t> equivalents;
};
using ItemIndex = std::map<std::uint64_t, const Json*>;
using EquivalenceCache = std::map<std::string, std::vector<std::uint64_t>>;
using ScoreCache = std::map<std::pair<std::string, std::uint64_t>, Json>;

// Each frozen higher-priority slot keeps its shape, base score, ranking score,
// priority and constrained values. A bipartite matching can change only the UID
// assigned to those equivalent slots; it cannot lower the earlier objective.
class ReservationMatcher {
public:
    ReservationMatcher(const Json& prior, const std::vector<Json>& preferences, const Json& inventory,
                       const std::set<std::uint64_t>& globally_excluded, Budget& budget,
                       const ItemIndex& item_index, ScoreCache& scores)
        : budget_(budget), plan_slots_(prior.size()) {
        EquivalenceCache equivalents;
        for (std::size_t p{}; p < prior.size(); ++p) {
            const auto& plan = prior[p]; const auto& prefs = preferences[p];
            const auto fixed = Kept(prefs), role_excluded = Excluded(prefs);
            const auto role_key = RoleKey(prefs);
            for (const auto& assignment : plan.at("assignment_scores")) {
                if (budget_.Stop()) return;
                Reservation slot{p, Key(assignment.at("equipment"))};
                const auto found_original = item_index.find(slot.original);
                if (found_original == item_index.end()) Fail(AlgorithmFailure::invalid_inventory);
                const auto* original = found_original->second;
                // Preallocated cores and explicit keep slots cannot move.
                if (original->at("kind") == "core" || fixed.contains(slot.original)) slot.equivalents.push_back(slot.original);
                else {
                    const auto signature = Json::array({role_key, original->at("shape_id"), original->at("cells"),
                        assignment.at("score"), assignment.at("priority_depth"), assignment.at("ranking_score"), assignment.at("constraint_stats")}).dump();
                    const auto cached = equivalents.find(signature);
                    if (cached != equivalents.end()) slot.equivalents = cached->second;
                    else {
                        for (const auto& item : inventory.at("items")) {
                            if (budget_.Stop()) return;
                            const auto uid = Key(item.at("uid"));
                            if (globally_excluded.contains(uid) || role_excluded.contains(uid) || item.at("kind") != "module" ||
                                item.value("shape_id", std::string{}) != original->value("shape_id", std::string{}) ||
                                item.at("cells") != original->at("cells") || (!prefs.value("include_discarded", false) && item.value("discarded", false))) continue;
                            if (!prefs.value("allow_borrow", true) && item.contains("character") && !item.at("character").is_null() &&
                                (SoftwareRole(prefs) || Key(item.at("character")) != Key(prefs.at("character")))) continue;
                            // Only immutable scoring is shared across groups. Dynamic
                            // exclusions and ownership are checked before the lookup.
                            const auto key = std::make_pair(role_key, uid);
                            auto scored = scores.find(key);
                            if (scored == scores.end()) scored = scores.emplace(key, ScoreEquipment(item, prefs)).first;
                            const auto& score = scored->second;
                            if (!score.value("eligible", false)) continue;
                            if (score.at("score") == assignment.at("score") && score.at("priority_depth") == assignment.at("priority_depth") &&
                                score.at("ranking_score") == assignment.at("ranking_score") && score.at("constraint_stats") == assignment.at("constraint_stats"))
                                slot.equivalents.push_back(uid);
                        }
                        equivalents.emplace(signature, slot.equivalents);
                    }
                    std::sort(slot.equivalents.begin(), slot.equivalents.end(), [&](auto a, auto b) {
                        if (a == slot.original) return b != slot.original;
                        if (b == slot.original) return false;
                        return a < b;
                    });
                }
                slots_.push_back(std::move(slot));
            }
        }
        std::stable_sort(slots_.begin(), slots_.end(), [](const Reservation& a, const Reservation& b) { return a.equivalents.size() < b.equivalents.size(); });
        for (std::size_t i{}; i < slots_.size(); ++i) plan_slots_[slots_[i].plan].push_back(i);
        ready_ = true;
    }

    bool Ready() const { return ready_; }

    std::vector<std::size_t> Membership(std::uint64_t uid) const {
        std::vector<std::size_t> result;
        for (std::size_t index{}; index < slots_.size(); ++index)
            if (std::find(slots_[index].equivalents.begin(), slots_[index].equivalents.end(), uid) != slots_[index].equivalents.end())
                result.push_back(index);
        return result;
    }

    std::set<std::uint64_t> Essential() {
        if (!ready_) return {};
        std::set<std::uint64_t> originals;
        std::map<std::uint64_t, std::vector<std::size_t>> reverse;
        for (std::size_t i{}; i < slots_.size(); ++i) {
            if (budget_.Stop()) return {};
            originals.insert(slots_[i].original);
            for (const auto uid : slots_[i].equivalents) reverse[uid].push_back(i);
        }
        // Original UIDs already form a valid matching. Reverse alternating
        // paths from unused candidates identify every original UID that can
        // be released without losing a slot; closed cycles cannot release one.
        std::set<std::uint64_t> reached;
        std::deque<std::uint64_t> pending;
        for (const auto& entry : reverse) if (!originals.contains(entry.first)) {
            reached.insert(entry.first); pending.push_back(entry.first);
        }
        while (!pending.empty()) {
            if (budget_.Stop()) return {};
            const auto uid = pending.front(); pending.pop_front();
            for (const auto index : reverse.at(uid)) {
                const auto original = slots_[index].original;
                if (reached.insert(original).second) pending.push_back(original);
            }
        }
        for (const auto uid : reached) originals.erase(uid);
        return originals;
    }

    std::vector<InventoryCapacity> Capacities(const std::set<std::uint64_t>& occupied, std::size_t maximum_items) {
        if (!ready_) return {};
        if (!capacities_ready_) {
            std::set<std::set<std::uint64_t>> unique;
            std::vector<const std::set<std::uint64_t>*> neighborhoods;
            for (const auto& slot : slots_) {
                if (budget_.Stop()) return {};
                const auto inserted = unique.emplace(slot.equivalents.begin(), slot.equivalents.end());
                neighborhoods.push_back(&*inserted.first);
            }
            std::vector<InventoryCapacity> pools;
            for (const auto& pool : unique) {
                if (budget_.Stop()) return {};
                std::size_t demand{};
                for (const auto* neighborhood : neighborhoods)
                    if (std::includes(pool.begin(), pool.end(), neighborhood->begin(), neighborhood->end())) ++demand;
                // Every slot whose whole neighborhood is inside this pool
                // must reserve a distinct member, even if its current UID moves.
                pools.push_back({pool, pool.size() > demand ? pool.size() - demand : 0});
            }
            capacity_pools_ = std::move(pools); capacities_ready_ = true;
        }
        std::vector<InventoryCapacity> result;
        for (const auto& pool : capacity_pools_) {
            if (budget_.Stop()) return {};
            const auto& smaller = occupied.size() < pool.items.size() ? occupied : pool.items;
            const auto& larger = occupied.size() < pool.items.size() ? pool.items : occupied;
            std::size_t used{};
            for (const auto uid : smaller) if (larger.contains(uid)) ++used;
            const auto remaining = used < pool.maximum ? pool.maximum - used : 0;
            if (remaining < maximum_items) result.push_back({pool.items, remaining});
        }
        return result;
    }

    bool Refill(Json& prior, const std::set<std::uint64_t>& occupied) {
        if (!ready_) return false;
        std::map<std::uint64_t, std::size_t> owners;
        std::vector<std::uint64_t> selected(slots_.size());
        std::function<bool(std::size_t, std::set<std::uint64_t>&)> match = [&](std::size_t index, std::set<std::uint64_t>& visited) {
            if (budget_.Stop()) return false;
            for (const auto uid : slots_[index].equivalents) {
                if (occupied.contains(uid) || !visited.insert(uid).second) continue;
                const auto previous = owners.find(uid);
                if (previous == owners.end() || match(previous->second, visited)) {
                    owners[uid] = index; selected[index] = uid; return true;
                }
            }
            return false;
        };
        for (std::size_t i{}; i < slots_.size(); ++i) { std::set<std::uint64_t> visited; if (!match(i, visited)) return false; }
        // Apply by original identity so a UID swap cannot overwrite another
        // slot before its original assignment has been found.
        for (std::size_t p{}; p < prior.size(); ++p) {
            std::map<std::uint64_t, std::uint64_t> replacements;
            for (const auto i : plan_slots_[p]) replacements.emplace(slots_[i].original, selected[i]);
            if (!prior[p].at("core").is_null()) prior[p]["core"] = Uid(replacements.at(Key(prior[p].at("core"))));
            for (auto& place : prior[p]["placements"]) place["equipment"] = Uid(replacements.at(Key(place.at("equipment"))));
            for (auto& score : prior[p]["assignment_scores"]) score["equipment"] = Uid(replacements.at(Key(score.at("equipment"))));
        }
        return true;
    }

private:
    Budget& budget_;
    std::vector<Reservation> slots_;
    std::vector<std::vector<std::size_t>> plan_slots_;
    std::vector<InventoryCapacity> capacity_pools_;
    bool ready_{}, capacities_ready_{};
};

// UID aliases are interchangeable only when every remaining role and every
// frozen matching slot sees the same stock. Keep identities are never aliases.
// This is internal to joint allocation; public Top-K still enumerates UIDs.
class StockClasses {
public:
    StockClasses(const std::vector<Json>& roles, const ItemIndex& items, ScoreCache& scores,
                 const ReservationMatcher* matcher)
        : roles_(roles), items_(items), scores_(scores), matcher_(matcher), classes_(roles.size() + 1), ids_(roles.size() + 1) {
        for (const auto& role : roles_) {
            excluded_.push_back(Excluded(role));
            const auto kept = Kept(role); fixed_.insert(kept.begin(), kept.end());
        }
    }

    std::vector<std::uint64_t> KeyFor(std::size_t index, const std::set<std::uint64_t>& uids) {
        std::vector<std::uint64_t> result;
        result.reserve(uids.size());
        for (const auto uid : uids) result.push_back(Class(index, uid));
        std::sort(result.begin(), result.end());
        return result;
    }

    std::string Signature(std::size_t index, const std::set<std::uint64_t>& uids) {
        std::string result;
        for (const auto id : KeyFor(index, uids)) { result += std::to_string(id); result.push_back(','); }
        return result;
    }

private:
    std::uint64_t Class(std::size_t index, std::uint64_t uid) {
        const auto found = ids_[index].find(uid);
        if (found != ids_[index].end()) return found->second;
        const auto& item = *items_.at(uid);
        Json descriptor = Json::array();
        if (fixed_.contains(uid)) descriptor.push_back(Uid(uid));
        else {
            for (const auto* field : {"kind", "shape_id", "cells", "suit_id", "required_shapes", "required_shapes_complete"})
                descriptor.push_back(item.value(field, Json(nullptr)));
            descriptor.push_back(matcher_ ? Json(matcher_->Membership(uid)) : Json::array());
            try {
                for (std::size_t role = index; role < roles_.size(); ++role) {
                    const auto& prefs = roles_[role];
                    const bool core = item.at("kind") == "core";
                    bool available = !excluded_[role].contains(uid) &&
                        (prefs.value("include_discarded", false) || !item.value("discarded", false));
                    if (!prefs.value("allow_borrow", true) && item.contains("character") && !item.at("character").is_null() &&
                        (SoftwareRole(prefs) || Key(item.at("character")) != Key(prefs.at("character")))) available = false;
                    if (core) {
                        if (!prefs.value("require_core", true)) available = false;
                        if (prefs.contains("core_suit_id") && item.value("suit_id", std::string{}) != prefs.at("core_suit_id").get<std::string>()) available = false;
                        if (prefs.contains("core_main_stats") && !prefs.at("core_main_stats").empty() && item.contains("main_stat")) {
                            const auto& allowed = prefs.at("core_main_stats");
                            if (std::find(allowed.begin(), allowed.end(), item.at("main_stat").value("id", std::string{})) == allowed.end()) available = false;
                        }
                    }
                    if (!available) { descriptor.push_back(nullptr); continue; }
                    const auto key = std::make_pair(RoleKey(prefs), uid);
                    auto scored = scores_.find(key);
                    if (scored == scores_.end()) scored = scores_.emplace(key, ScoreEquipment(item, prefs)).first;
                    const auto& score = scored->second;
                    if (!score.value("eligible", false)) descriptor.push_back(nullptr);
                    else descriptor.push_back(Json::array({score.at("score"), score.at("ranking_score"), score.at("priority_depth"), score.at("constraint_stats")}));
                }
            } catch (const InvalidAlgorithm&) {
                // Equivalence discovery must not eagerly validate an item that
                // a later solve might exclude. Its ordinary solver still owns
                // validation; a unique class skips only this optimization.
                descriptor = Json::array({Uid(uid)});
            }
        }
        const auto inserted = classes_[index].emplace(descriptor.dump(), classes_[index].size() + 1);
        const auto id = static_cast<std::uint64_t>(inserted.first->second);
        ids_[index].emplace(uid, id);
        return id;
    }

    const std::vector<Json>& roles_;
    const ItemIndex& items_;
    ScoreCache& scores_;
    const ReservationMatcher* matcher_;
    std::vector<std::set<std::uint64_t>> excluded_;
    std::set<std::uint64_t> fixed_;
    std::vector<std::map<std::string, std::size_t>> classes_;
    std::vector<std::map<std::uint64_t, std::uint64_t>> ids_;
};

struct GroupSolution {
    Json prior = Json::array(), current = Json::array();
    Priority priority{};
    double score{-1};
    std::size_t count{};
    bool limited{};
    std::string reason{"no_solution"};
};

std::size_t MinimumVisits(const Json& role, const Json& inventory) {
    if (SoftwareRole(role)) return role.at("allowed_cells").size() + 1;
    for (const auto& character : inventory.at("characters"))
        if (Key(character.at("uid")) == Key(role.at("character"))) return character.at("allowed_cells").size() + 1;
    Fail(AlgorithmFailure::missing_character);
}

// A transport relaxation assigns each module UID at most once and fills each
// remaining role's area, while ignoring geometry, sets and attribute limits.
// Any nonnegative area prices give an upper bound. Freeze those prices per
// suffix so a prefix only subtracts the surplus of its already consumed UIDs.
class JointScoreBound {
    using Value = std::int64_t;
    static constexpr Value scale = 12;
    static constexpr Value maximum_cents = 1000000000;
    struct Module { std::uint64_t uid; int area{}; std::vector<Value> costs; };
    struct Suffix { Value total{}; std::vector<Value> surplus; };
    struct Breakpoint { Value margin; int area; };
    std::vector<Module> modules_;
    std::map<std::uint64_t, std::size_t> indices_;
    std::vector<Suffix> suffixes_;
    bool ready_{};

    Value Price(std::size_t area, const std::vector<Value>& margins) const {
        std::vector<Breakpoint> points;
        std::size_t available{};
        for (std::size_t i{}; i < modules_.size(); ++i) if (margins[i] > 0) {
            points.push_back({margins[i], modules_[i].area}); available += modules_[i].area;
        }
        if (available <= area) return 0;
        std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
            return a.margin * b.area > b.margin * a.area;
        });
        std::size_t occupied{};
        Value low{}, high{};
        for (const auto& point : points) {
            occupied += point.area;
            // Prefer the lower end of a flat minimum; it can release a
            // contested UID to another role in the next coordinate update.
            if (occupied > area) {
                low = point.margin / point.area;
                high = (point.margin + point.area - 1) / point.area;
                break;
            }
        }
        const auto cost = [&](Value price) {
            Value total = static_cast<Value>(area) * price;
            for (std::size_t i{}; i < modules_.size(); ++i)
                total += std::max<Value>(0, margins[i] - modules_[i].area * price);
            return total;
        };
        return cost(low) <= cost(high) ? low : high;
    }

public:
    JointScoreBound(const std::vector<Json>& roles, const Json& inventory, ScoreCache& scores, Budget& budget) {
        if (roles.size() < 2) return;
        for (const auto& role : roles)
            if (role.contains("keep") || role.contains("extra_shape") ||
                (role.contains("priority_stats") && !role.at("priority_stats").empty())) return;
        // ScoreJson's cents -> double -> cents round trip is exact in this
        // range. Larger legal scores retain the existing search without this
        // optional bound. The same limit also bounds objective roundoff below
        // one cent for at most 32 roles of 25 modules and one core each.
        if (inventory.at("items").size() > static_cast<std::size_t>(INT64_MAX / (2 * scale * maximum_cents)) - 32 * 26) return;
        try {
            std::vector<std::size_t> areas;
            std::vector<std::set<std::uint64_t>> excluded;
            std::vector<std::string> identities;
            std::vector<Value> cores(roles.size());
            for (const auto& role : roles) {
                areas.push_back(MinimumVisits(role, inventory) - 1);
                excluded.push_back(Excluded(role)); identities.push_back(RoleKey(role));
            }
            for (const auto& item : inventory.at("items")) {
                if (budget.Stop()) return;
                const auto uid = Key(item.at("uid"));
                const bool core = item.at("kind") == "core";
                Module module{uid, 0, std::vector<Value>(roles.size(), -1)};
                bool eligible{};
                for (std::size_t r{}; r < roles.size(); ++r) {
                    const auto& role = roles[r];
                    if (excluded[r].contains(uid) || (!role.value("include_discarded", false) && item.value("discarded", false))) continue;
                    if (!role.value("allow_borrow", true) && item.contains("character") && !item.at("character").is_null() &&
                        (SoftwareRole(role) || Key(item.at("character")) != Key(role.at("character")))) continue;
                    if (core) {
                        if (!role.value("require_core", true)) continue;
                        if (role.contains("core_suit_id") && item.value("suit_id", std::string{}) != role.at("core_suit_id").get<std::string>()) continue;
                        if (role.contains("core_main_stats") && !role.at("core_main_stats").empty() && item.contains("main_stat") && item.at("main_stat").is_object()) {
                            const auto& allowed = role.at("core_main_stats");
                            if (std::find(allowed.begin(), allowed.end(), item.at("main_stat").value("id", std::string{})) == allowed.end()) continue;
                        }
                    }
                    const auto key = std::make_pair(identities[r], uid);
                    auto found = scores.find(key);
                    if (found == scores.end()) found = scores.emplace(key, ScoreEquipment(item, role)).first;
                    const auto& score = found->second;
                    if (!score.value("eligible", false)) continue;
                    const auto value = Number(score.at("score"));
                    if (value < 0 || value > static_cast<double>(maximum_cents) / 100) return;
                    const auto cents = static_cast<Value>(std::llround(value * 100));
                    if (core) cores[r] = std::max(cores[r], cents * scale);
                    else {
                        module.area = Integer(score.at("area"), 1, 4);
                        module.costs[r] = cents * scale; eligible = true;
                    }
                }
                if (eligible) { indices_.emplace(uid, modules_.size()); modules_.push_back(std::move(module)); }
            }
            suffixes_.resize(roles.size());
            for (std::size_t start = roles.size(); start-- > 0;) {
                if (budget.Stop()) return;
                std::size_t area{};
                for (std::size_t r = start; r < roles.size(); ++r) area += areas[r];
                std::vector<Value> margins(modules_.size()), prices(roles.size());
                for (std::size_t u{}; u < modules_.size(); ++u)
                    for (std::size_t r = start; r < roles.size(); ++r) margins[u] = std::max(margins[u], modules_[u].costs[r]);
                const auto common = Price(area, margins);
                for (std::size_t r = start; r < roles.size(); ++r) prices[r] = common;
                for (int pass{}; pass < 2; ++pass) for (std::size_t r = start; r < roles.size(); ++r) {
                    if (budget.Stop()) return;
                    for (std::size_t u{}; u < modules_.size(); ++u) {
                        Value other{};
                        for (std::size_t s = start; s < roles.size(); ++s) if (s != r)
                            other = std::max(other, modules_[u].costs[s] - modules_[u].area * prices[s]);
                        margins[u] = modules_[u].costs[r] - other;
                    }
                    prices[r] = Price(areas[r], margins);
                }
                auto& suffix = suffixes_[start]; suffix.surplus.resize(modules_.size());
                for (std::size_t r = start; r < roles.size(); ++r)
                    suffix.total += static_cast<Value>(areas[r]) * prices[r] + cores[r];
                for (std::size_t u{}; u < modules_.size(); ++u) {
                    auto& surplus = suffix.surplus[u];
                    for (std::size_t r = start; r < roles.size(); ++r)
                        surplus = std::max(surplus, modules_[u].costs[r] - modules_[u].area * prices[r]);
                    suffix.total += surplus;
                }
            }
            ready_ = true;
        } catch (const InvalidAlgorithm& failure) {
            if (failure.reason == AlgorithmFailure::cancelled) throw;
            // Scoring is normally lazy. A relaxation must not surface an
            // invalid item that no actual candidate search needed to inspect.
        } catch (const Json::exception&) {
            // Leave malformed or incomplete values to the normal solver path.
        }
    }

    bool CannotImprove(std::size_t index, const std::set<std::uint64_t>& used, double prefix, double best) const {
        if (!ready_ || index >= suffixes_.size()) return false;
        const auto& suffix = suffixes_[index]; auto remaining = suffix.total;
        for (const auto uid : used) {
            const auto found = indices_.find(uid);
            if (found != indices_.end()) remaining -= suffix.surplus[found->second];
        }
        // The objective adds binary doubles, so do not prune exact-score ties.
        // Outward cent rounding and a cent on either side cover that sum's
        // roundoff in the bounded score range used to build this relaxation.
        const auto upper = static_cast<Value>(std::ceil(prefix * 100)) + 1 + (remaining + scale - 1) / scale;
        const auto lower = static_cast<Value>(std::floor(best * 100)) - 1;
        return upper < lower;
    }
};

GroupSolution SolveGroup(std::vector<Json> roles, const Json& prior,
                         const std::vector<Json>& prior_preferences, const Json& inventory,
                         const std::set<std::uint64_t>& excluded, Budget& budget,
                         const ItemIndex& item_index, ScoreCache& scores, PreparedSolvers& solvers, SearchExecutor& executor,
                         std::size_t candidates, bool feasibility) {
    GroupSolution best;
    std::unique_ptr<ReservationMatcher> matcher;
    std::set<std::uint64_t> fixed_prior;
    for (const auto& plan : prior) {
        if (feasibility) { const auto ids = Used(plan); fixed_prior.insert(ids.begin(), ids.end()); }
        else if (!plan.at("core").is_null()) fixed_prior.insert(Key(plan.at("core")));
    }
    for (const auto& preference : prior_preferences) { const auto fixed = Kept(preference); fixed_prior.insert(fixed.begin(), fixed.end()); }
    if (!feasibility && !prior.empty()) {
        matcher = std::make_unique<ReservationMatcher>(prior, prior_preferences, inventory, excluded, budget, item_index, scores);
        if (matcher->Ready()) {
            const auto essential = matcher->Essential(); fixed_prior.insert(essential.begin(), essential.end());
        }
    }
    for (auto& role : roles) {
        auto blocked = Excluded(role); blocked.insert(excluded.begin(), excluded.end());
        blocked.insert(fixed_prior.begin(), fixed_prior.end()); SetExcluded(role, blocked);
    }
    StockClasses stock(roles, item_index, scores, matcher.get());
    std::vector<Json> bounds(roles.size());
    if (!feasibility && roles.size() > 1) for (std::size_t index{}; index < roles.size(); ++index) {
        if (budget.Stop()) break;
        const auto capacities = matcher ? matcher->Capacities({}, MinimumVisits(roles[index], inventory) - 1) : InventoryCapacities{};
        if (budget.Stop()) break;
        auto independent = roles[index]; budget.Child(independent);
        const auto units = roles.size() - index + 1;
        independent["max_nodes"] = std::min(budget.maximum_nodes - budget.nodes,
            std::max(MinimumVisits(roles[index], inventory), (budget.maximum_nodes - budget.nodes) / units));
        independent["time_limit_ms"] = std::max<std::int64_t>(1,
            independent.at("time_limit_ms").get<std::int64_t>() / static_cast<std::int64_t>(units));
        auto& solver = solvers[RoleKey(roles[index])];
        if (!solver) solver = std::make_unique<PreparedLoadout>(independent, inventory, &executor);
        auto optimum = solver->Solve(independent, capacities, budget.cancel, budget.ChildProgress());
        // An incomplete solo search supplies no bound, and must not consume
        // the entire group's remaining opportunity to search jointly.
        budget.Account(optimum, false);
        if (optimum.value("valid", false) && optimum.at("search").value("exhaustive", false)) bounds[index] = std::move(optimum);
    }
    std::unique_ptr<JointScoreBound> joint_bound;
    if (!feasibility) joint_bound = std::make_unique<JointScoreBound>(roles, inventory, scores, budget);
    Json current = Json::array();
    struct PrefixState { std::size_t count; Priority priority; double score; };
    using AssignmentState = std::pair<std::size_t, std::vector<std::uint64_t>>;
    std::map<AssignmentState, PrefixState> prefixes;
    std::vector<bool> any_core(roles.size() + 1);
    std::vector<std::set<std::string>> core_suits(roles.size() + 1);
    for (std::size_t i = roles.size(); i-- > 0;) {
        any_core[i] = any_core[i + 1]; core_suits[i] = core_suits[i + 1];
        const auto required = roles[i].find("require_core"), suit = roles[i].find("core_suit_id");
        if (required != roles[i].end() && required->is_boolean() && !required->get<bool>()) continue;
        if (suit != roles[i].end() && suit->is_string() && !suit->get_ref<const std::string&>().empty())
            core_suits[i].insert(suit->get<std::string>());
        else any_core[i] = true;
    }
    std::size_t cached_uids{};
    constexpr std::size_t maximum_cached_uids = 256U * 1024U;
    bool done{}, allow_partial = feasibility;
    std::size_t candidate_limit = candidates;
    std::function<void(std::size_t, std::set<std::uint64_t>, Priority, double)> assign;
    assign = [&](std::size_t index, std::set<std::uint64_t> used, Priority priority, double score) {
        if (budget.cancel && budget.cancel()) Fail(AlgorithmFailure::cancelled);
        // The same role index and occupied UID set have identical remaining
        // stock, exclusions and refill possibilities, regardless of prior layouts.
        std::set<std::uint64_t> relevant;
        for (const auto uid : used) {
            const auto& item = *item_index.at(uid);
            if (!any_core[index] && item.at("kind") == "core") {
                const auto suit = item.find("suit_id");
                // Prior cores are fixed and already excluded. A consumed core
                // whose suit no remaining role accepts cannot affect the suffix.
                if (core_suits[index].empty() || (suit != item.end() && suit->is_string() && !core_suits[index].contains(suit->get_ref<const std::string&>()))) continue;
            }
            relevant.insert(uid);
        }
        auto state = std::make_pair(index, stock.KeyFor(index, relevant));
        const PrefixState prefix{current.size(), priority, score};
        const auto cached = prefixes.find(state);
        if (cached != prefixes.end()) {
            const auto& previous = cached->second;
            if (previous.count > prefix.count || (previous.count == prefix.count &&
                (previous.priority > prefix.priority || (previous.priority == prefix.priority && previous.score >= prefix.score)))) return;
            cached->second = prefix;
        } else if (state.second.size() <= maximum_cached_uids - cached_uids) {
            // Work budgets can permit millions of combinations, so bound the
            // UID payload independently without changing uncached exploration.
            cached_uids += state.second.size(); prefixes.emplace(std::move(state), prefix);
        }
        if (index == roles.size()) {
            auto refilled = prior;
            bool overlap{};
            for (const auto& plan : prior) for (const auto uid : Used(plan)) if (used.contains(uid)) overlap = true;
            if (overlap && (!matcher || !matcher->Refill(refilled, used))) return;
            if (!current.empty() && (current.size() > best.count || (current.size() == best.count &&
                (best.score < 0 || priority > best.priority || (priority == best.priority && score > best.score))))) {
                best.score = score; best.priority = priority; best.current = current; best.prior = std::move(refilled); best.count = current.size();
            }
            if (best.count == roles.size()) {
                bool optimal = !feasibility;
                for (std::size_t i{}; optimal && i < roles.size(); ++i)
                    if (bounds[i].is_null() || BetterPlan(bounds[i], best.current.at(i))) optimal = false;
                if (feasibility || optimal) { done = true; if (optimal) best.limited = false; }
            }
            return;
        }
        if (done) return;
        if (!feasibility && best.count == roles.size()) {
            auto upper_priority = priority; auto upper_score = score;
            bool proven{true};
            for (std::size_t i = index; i < roles.size(); ++i) {
                if (bounds[i].is_null()) { proven = false; break; }
                const auto values = PriorityOf(bounds[i]);
                for (std::size_t p{}; p < upper_priority.size(); ++p) upper_priority[p] += values[p];
                // Keep the same left-to-right addition order as the objective.
                upper_score += bounds[i].value("ranking_score", bounds[i].value("score", 0.));
            }
            if (proven && (upper_priority < best.priority || (upper_priority == best.priority && upper_score <= best.score))) return;
            if (joint_bound && joint_bound->CannotImprove(index, used, score, best.score)) return;
        }
        if (budget.Stop()) {
            // A budget boundary must not discard complete boards already
            // found for this group's prefix during coverage.
            if (feasibility && !current.empty()) assign(roles.size(), std::move(used), priority, score);
            return;
        }
        auto role = roles[index]; auto blocked = Excluded(role); blocked.insert(used.begin(), used.end()); SetExcluded(role, blocked);
        const auto capacities = matcher ? matcher->Capacities(used, MinimumVisits(role, inventory) - 1) : InventoryCapacities{};
        bool exact_solo{};
        const auto complete = Enumerate(role, inventory, budget, candidate_limit, [&](const Json& plan) {
            auto next = used; const auto ids = Used(plan); next.insert(ids.begin(), ids.end());
            auto next_priority = priority; const auto values = PriorityOf(plan);
            for (std::size_t i{}; i < priority.size(); ++i) next_priority[i] += values[i];
            current.push_back(plan);
            assign(index + 1, std::move(next), next_priority, score + plan.value("ranking_score", plan.value("score", 0.)));
            current.erase(current.end() - 1);
            if (!feasibility && roles.size() == 1 && best.count == 1 && plan.at("search").value("exhaustive", false)) { exact_solo = true; return false; }
            return !done && budget.exhaustive;
        }, &executor, feasibility, roles.size() - index, MinimumVisits(role, inventory), bounds[index].is_null() ? nullptr : &bounds[index], capacities, &solvers[RoleKey(role)],
        [&](const std::set<std::uint64_t>& ids) { return stock.Signature(index, ids); });
        best.limited = best.limited || (!complete && !exact_solo && !done);
        if (allow_partial && !done && budget.exhaustive) assign(index + 1, std::move(used), priority, score);
    };
    assign(0, {}, {}, 0);
    if (!feasibility) {
        while (best.count == 0 && best.limited && budget.exhaustive && candidate_limit < budget.maximum_calls) {
            candidate_limit = std::min(budget.maximum_calls, candidate_limit * 2);
            prefixes.clear(); cached_uids = 0;
            best.limited = false; assign(0, {}, {}, 0);
        }
        if (best.count == 0 && budget.exhaustive) {
            allow_partial = true; prefixes.clear(); cached_uids = 0; assign(0, {}, {}, 0);
        }
    }
    best.reason = !budget.exhaustive ? budget.reason : best.limited ? "candidate_limit" : "no_solution";
    return best;
}

Json Multi(const Json& request, const Json& inventory, const AlgorithmCancelCheck& cancel, const AlgorithmProgressCallback& progress) {
    Snapshot(request, inventory);
    if (!request.contains("groups") || !request.at("groups").is_array() || request.at("groups").empty() || request.at("groups").size() > 64) Fail();
    Budget budget(request, cancel, progress);
    const auto candidates = static_cast<std::size_t>(request.contains("top_k") ? Integer(request.at("top_k"), 1, algorithm_limits::kMaximumTopK) : 20);
    const auto global_excluded = Excluded(request);
    std::vector<std::vector<Json>> requests;
    std::map<std::string, std::size_t> group_for_role;
    std::map<std::string, Json> preference_for_role;
    std::size_t remaining_roles{};
    for (const auto& values : request.at("groups")) {
        if (!values.is_array() || values.empty() || values.size() > 32) Fail();
        std::vector<Json> roles;
        for (auto role : values) {
            if (!role.is_object()) Fail();
            const auto identity = RoleKey(role);
            if (!group_for_role.emplace(identity, requests.size()).second) Fail();
            role["snapshot_id"] = request.at("snapshot_id");
            auto blocked = Excluded(role); blocked.insert(global_excluded.begin(), global_excluded.end()); SetExcluded(role, blocked);
            preference_for_role.emplace(identity, role); roles.push_back(std::move(role));
        }
        remaining_roles += roles.size(); requests.push_back(std::move(roles));
    }
    std::set<std::uint64_t> team_kept;
    for (const auto& group : requests) for (const auto& role : group) {
        const auto fixed = Kept(role); team_kept.insert(fixed.begin(), fixed.end());
    }
    for (auto& group : requests) for (auto& role : group) {
        auto blocked = Excluded(role); const auto own_kept = Kept(role);
        for (const auto uid : team_kept) if (!own_kept.contains(uid)) blocked.insert(uid);
        SetExcluded(role, blocked); preference_for_role.at(RoleKey(role)) = role;
    }
    budget.Team(requests.size(), remaining_roles);
    ItemIndex item_index;
    ScoreCache scores;
    SearchExecutor executor(SearchWorkers(request));
    PreparedSolvers solvers;
    for (const auto& item : inventory.at("items")) {
        if (cancel && cancel()) Fail(AlgorithmFailure::cancelled);
        if (!item_index.emplace(Key(item.at("uid")), &item).second) Fail(AlgorithmFailure::invalid_inventory);
    }
    Json plans = Json::array();
    std::vector<std::string> group_reasons(requests.size(), "candidate_limit");
    bool candidate_limited{};
    std::string limited_reason;
    std::size_t coverage_visited{};
    // Find useful coverage before spending the shared budget on score variants.
    // A local slice reaching its limit does not terminate the parent budget.
    for (std::size_t index{}; index < requests.size(); ++index) {
        if (budget.Stop()) break;
        budget.Phase(AlgorithmProgressPhase::coverage, static_cast<std::uint32_t>(index + 1));
        const auto& roles = requests[index];
        std::size_t minimum_nodes{};
        for (const auto& role : roles) minimum_nodes += MinimumVisits(role, inventory);
        Budget slice(budget, roles.size(), remaining_roles, minimum_nodes);
        std::vector<Json> prior_preferences;
        for (const auto& plan : plans) prior_preferences.push_back(preference_for_role.at(RoleKey(plan)));
        auto solved = SolveGroup(roles, plans, prior_preferences, inventory, global_excluded, slice,
                                 item_index, scores, solvers, executor, std::max<std::size_t>(2, candidates), true);
        if (solved.limited && !slice.exhaustive) limited_reason = slice.reason;
        budget.Absorb(slice); candidate_limited = candidate_limited || solved.limited;
        group_reasons[index] = solved.reason; ++coverage_visited;
        if (solved.score >= 0) {
            plans = std::move(solved.prior);
            for (auto& plan : solved.current) plans.push_back(std::move(plan));
        }
        budget.Covered(coverage_visited, plans.size());
        remaining_roles -= roles.size();
    }
    // Improve in priority order without displacing coverage already obtained
    // for later groups. Earlier slots can still be refilled by equivalent UIDs.
    for (std::size_t index{}; index < requests.size(); ++index) {
        if (budget.Stop()) break;
        budget.Phase(AlgorithmProgressPhase::improving, static_cast<std::uint32_t>(index + 1));
        Json prior = Json::array(), own = Json::array(), later = Json::array();
        std::vector<Json> prior_preferences;
        auto blocked = global_excluded;
        Priority old_priority{}; double old_score{};
        for (const auto& plan : plans) {
            const auto group = group_for_role.at(RoleKey(plan));
            if (group < index) { prior.push_back(plan); prior_preferences.push_back(preference_for_role.at(RoleKey(plan))); }
            else if (group == index) {
                own.push_back(plan); old_score += plan.value("ranking_score", plan.value("score", 0.));
                const auto values = PriorityOf(plan); for (std::size_t i{}; i < values.size(); ++i) old_priority[i] += values[i];
            } else { later.push_back(plan); const auto ids = Used(plan); blocked.insert(ids.begin(), ids.end()); }
        }
        Budget slice(budget, 1, requests.size() - index, 1);
        auto solved = SolveGroup(requests[index], prior, prior_preferences, inventory, blocked, slice, item_index, scores, solvers, executor, candidates, false);
        if (solved.limited && !slice.exhaustive) limited_reason = slice.reason;
        budget.Absorb(slice); candidate_limited = candidate_limited || solved.limited;
        if (solved.count > own.size() || (solved.count == own.size() && solved.score >= 0 &&
            (solved.priority > old_priority || (solved.priority == old_priority && solved.score > old_score)))) {
            plans = std::move(solved.prior);
            for (auto& plan : solved.current) plans.push_back(std::move(plan));
            for (auto& plan : later) plans.push_back(std::move(plan));
        }
        budget.Covered(coverage_visited, plans.size());
        group_reasons[index] = solved.reason;
    }
    static_cast<void>(budget.Stop());
    Json groups = Json::array(), unassigned = Json::array(), details = Json::array();
    std::set<std::string> assigned;
    for (const auto& plan : plans) assigned.insert(RoleKey(plan));
    for (std::size_t index{}; index < requests.size(); ++index) {
        Json identities = Json::array(); std::size_t count{}; double score{}; Priority priority{};
        for (const auto& role : requests[index]) {
            identities.push_back(RoleIdentity(role));
            if (!assigned.contains(RoleKey(role))) {
                unassigned.push_back(RoleIdentity(role));
                details.push_back({{"identity", RoleIdentity(role)}, {"reason_code", !budget.exhaustive ? budget.reason : group_reasons[index]}});
            }
        }
        for (const auto& plan : plans) if (group_for_role.at(RoleKey(plan)) == index) {
            ++count; score += plan.value("ranking_score", plan.value("score", 0.));
            const auto values = PriorityOf(plan); for (std::size_t i{}; i < values.size(); ++i) priority[i] += values[i];
        }
        groups.push_back({{"characters", identities}, {"valid", count == requests[index].size()}, {"assigned_count", count},
            {"ranking_score", score}, {"priority_counts", priority}, {"stop_reason", count == requests[index].size() ? "complete" : group_reasons[index]}});
    }
    Json role_top_k = Json::array();
    if (request.value("include_role_top_k", false)) {
        for (const auto& group : requests) for (const auto& role : group) {
            if (budget.Stop()) break;
            budget.Phase(AlgorithmProgressPhase::alternatives, static_cast<std::uint32_t>(group_for_role.at(RoleKey(role)) + 1));
            Json options = Json::array();
            const auto complete = Enumerate(role, inventory, budget, candidates, [&](const Json& plan) { options.push_back(plan); return true; }, &executor);
            role_top_k.push_back({{"identity", RoleIdentity(role)}, {"options", options}, {"enumeration_complete", complete}});
        }
    }
    static_cast<void>(budget.Stop());
    auto report = budget.Report();
    report["coverage_groups_visited"] = coverage_visited;
    if (candidate_limited && report.at("exhaustive") == true) {
        report["exhaustive"] = false;
        report["stop_reason"] = limited_reason.empty() ? "candidate_limit" : limited_reason;
    }
    double score{}; for (const auto& plan : plans) score += plan.value("score", 0.);
    return {{"snapshot_id", request.at("snapshot_id")}, {"valid", unassigned.empty()}, {"plans", plans}, {"groups", groups},
        {"unassigned", unassigned}, {"unassigned_details", details}, {"score", score}, {"role_top_k", role_top_k}, {"search", report}};
}
Json Assessment(const Json& request, const Json& inventory, const AlgorithmCancelCheck& cancel, const AlgorithmProgressCallback& progress) {
    Snapshot(request, inventory);
    if (!request.contains("roles") || !request.at("roles").is_array() || request.at("roles").empty() || request.at("roles").size() > 64) Fail();
    Budget budget(request, cancel, progress, false);
    budget.Phase(AlgorithmProgressPhase::computing);
    const auto offset = request.contains("offset") ? static_cast<std::size_t>(Integer(request.at("offset"), 0, INT32_MAX)) : 0;
    const auto limit = request.contains("limit") ? static_cast<std::size_t>(Integer(request.at("limit"), 1, 500)) : 200;
    if (offset > inventory.at("items").size()) Fail();
    const auto end = std::min(inventory.at("items").size(), offset + limit);
    Json items = Json::array(); bool complete{true};
    std::size_t cursor = offset;
    for (; cursor < end; ++cursor) {
        if (budget.Stop()) break;
        const auto& item = inventory.at("items")[cursor];
        Json scores = Json::array(); Json best = nullptr;
        bool finished{true};
        for (const auto& role : request.at("roles")) {
            if (budget.Stop()) { finished = false; complete = false; break; }
            Json score;
            try {
                score = ScoreEquipment(item, role);
                if (score.value("eligible", false) && (best.is_null() || score.at("score") > best)) best = score.at("score");
            } catch (const InvalidAlgorithm& failure) {
                complete = false; score = {{"eligible", false}, {"error_code", AlgorithmErrorCode(failure.reason)}};
            }
            if (role.contains("character")) score["character"] = role.at("character");
            if (role.contains("id")) score["role_id"] = role.at("id");
            else if (role.contains("role_id")) score["role_id"] = role.at("role_id");
            scores.push_back(std::move(score));
            ++budget.nodes;
        }
        if (!finished) break;
        items.push_back({{"equipment", item.at("uid")}, {"roles", scores}, {"best_score", best}});
    }
    return {{"snapshot_id", request.at("snapshot_id")}, {"complete", complete}, {"items", items},
        {"total", inventory.at("items").size()}, {"offset", offset},
        {"next_offset", cursor < inventory.at("items").size() ? Json(cursor) : Json(nullptr)}, {"search", budget.Report()}};
}
} // namespace
Json OptimizeTopK(const Json& request, const Json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
                  const AlgorithmProgressCallback& progress) {
    try { return TopK(request, frozen_inventory, cancel_check, progress); } catch (const Json::exception&) { Fail(); }
}
Json OptimizeLoadouts(const Json& request, const Json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
                     const AlgorithmProgressCallback& progress) {
    try { return Multi(request, frozen_inventory, cancel_check, progress); } catch (const Json::exception&) { Fail(); }
}
Json AssessInventory(const Json& request, const Json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
                     const AlgorithmProgressCallback& progress) {
    try { return Assessment(request, frozen_inventory, cancel_check, progress); } catch (const Json::exception&) { Fail(); }
}
} // namespace equipment_manager
