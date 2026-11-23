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

// Enumerating UID exclusions is the same public Top-K boundary used by the
// reference facade. The visitor lets joint search try its first complete plan
// before spending the shared budget on the remaining alternatives.
bool Enumerate(const Json& request, const Json& inventory, Budget& budget, std::size_t maximum,
               const std::function<bool(const Json&)>& visitor, bool feasibility = false,
               std::size_t remaining_roles = 1, std::size_t minimum_nodes = 1) {
    const auto frozen = Kept(request);
    std::set<std::string> seen, emitted;
    std::priority_queue<Candidate, std::vector<Candidate>, Worse> pending;
    bool child_limited{};
    const auto solve = [&](const std::set<std::uint64_t>& excluded) {
        if (budget.Stop()) return;
        if (!seen.insert(Signature(excluded)).second) return;
        auto child = request; SetExcluded(child, excluded); budget.Child(child);
        if (feasibility) {
            child["max_nodes"] = std::min(budget.maximum_nodes - budget.nodes,
                std::max(minimum_nodes, (budget.maximum_nodes - budget.nodes) / remaining_roles));
            child["time_limit_ms"] = std::max<std::int64_t>(1,
                child.at("time_limit_ms").get<std::int64_t>() / static_cast<std::int64_t>(remaining_roles));
        }
        auto plan = feasibility ? OptimizeFirstLoadout(child, inventory, budget.cancel, budget.ChildProgress())
            : OptimizeLoadout(child, inventory, budget.cancel, budget.ChildProgress());
        child_limited = child_limited || !plan.at("search").value("exhaustive", false);
        budget.Account(plan, !feasibility);
        if (plan.value("valid", false)) pending.push({std::move(plan), excluded});
    };
    solve(Excluded(request));
    std::size_t count{};
    while (!pending.empty()) {
        if (budget.cancel && budget.cancel()) Fail(AlgorithmFailure::cancelled);
        const auto candidate = pending.top(); pending.pop();
        const auto ids = Used(candidate.plan);
        if (emitted.insert(Signature(ids)).second) {
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
    budget.Phase(AlgorithmProgressPhase::alternatives);
    const auto count = request.contains("top_k") ? Integer(request.at("top_k"), 1, algorithm_limits::kMaximumTopK) : 5;
    Json options = Json::array();
    const auto complete = Enumerate(request, inventory, budget, count, [&](const Json& plan) { options.push_back(plan); return true; });
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

// Each frozen higher-priority slot keeps its shape, base score, ranking score,
// priority and constrained values. A bipartite matching can change only the UID
// assigned to those equivalent slots; it cannot lower the earlier objective.
bool Refill(Json& prior, const std::vector<Json>& preferences, const Json& inventory,
            const std::set<std::uint64_t>& occupied, const std::set<std::uint64_t>& globally_excluded,
            Budget& budget, const ItemIndex& item_index, EquivalenceCache& equivalents) {
    std::vector<Reservation> slots;
    for (std::size_t p{}; p < prior.size(); ++p) {
        const auto& plan = prior[p]; const auto& prefs = preferences[p]; const auto fixed = Kept(prefs);
        for (const auto& assignment : plan.at("assignment_scores")) {
            if (budget.Stop()) return false;
            Reservation slot{p, Key(assignment.at("equipment"))};
            const auto found_original = item_index.find(slot.original);
            if (found_original == item_index.end()) Fail(AlgorithmFailure::invalid_inventory);
            const auto* original = found_original->second;
            // The reference defers equivalent drives; its preallocated cores
            // remain fixed across groups.
            if (original->at("kind") == "core" || fixed.contains(slot.original)) slot.equivalents.push_back(slot.original);
            else {
                const auto signature = Json::array({RoleKey(prefs), original->at("shape_id"), original->at("cells"),
                    assignment.at("score"), assignment.at("priority_depth"), assignment.at("ranking_score"), assignment.at("constraint_stats")}).dump();
                const auto cached = equivalents.find(signature);
                if (cached != equivalents.end()) slot.equivalents = cached->second;
                else {
                const auto role_excluded = Excluded(prefs);
                for (const auto& item : inventory.at("items")) {
                    if (budget.Stop()) return false;
                    const auto uid = Key(item.at("uid"));
                    if (globally_excluded.contains(uid) || role_excluded.contains(uid) || item.at("kind") != "module" ||
                        item.value("shape_id", std::string{}) != original->value("shape_id", std::string{}) ||
                        item.at("cells") != original->at("cells") || (!prefs.value("include_discarded", false) && item.value("discarded", false))) continue;
                    if (!prefs.value("allow_borrow", true) && item.contains("character") && !item.at("character").is_null() &&
                        (SoftwareRole(prefs) || Key(item.at("character")) != Key(prefs.at("character")))) continue;
                    const auto score = ScoreEquipment(item, prefs);
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
            slots.push_back(std::move(slot));
        }
    }
    std::stable_sort(slots.begin(), slots.end(), [](const Reservation& a, const Reservation& b) { return a.equivalents.size() < b.equivalents.size(); });
    std::map<std::uint64_t, std::size_t> owners;
    std::vector<std::uint64_t> selected(slots.size());
    std::function<bool(std::size_t, std::set<std::uint64_t>&)> match = [&](std::size_t index, std::set<std::uint64_t>& visited) {
        if (budget.Stop()) return false;
        for (const auto uid : slots[index].equivalents) {
            if (occupied.contains(uid) || !visited.insert(uid).second) continue;
            const auto previous = owners.find(uid);
            if (previous == owners.end() || match(previous->second, visited)) {
                owners[uid] = index; selected[index] = uid; return true;
            }
        }
        return false;
    };
    for (std::size_t i{}; i < slots.size(); ++i) { std::set<std::uint64_t> visited; if (!match(i, visited)) return false; }
    // Apply by original identity in one pass so a UID swap cannot overwrite a
    // second slot before its original assignment has been found.
    for (std::size_t p{}; p < prior.size(); ++p) {
        std::map<std::uint64_t, std::uint64_t> replacements;
        for (std::size_t i{}; i < slots.size(); ++i) if (slots[i].plan == p) replacements.emplace(slots[i].original, selected[i]);
        if (!prior[p].at("core").is_null()) prior[p]["core"] = Uid(replacements.at(Key(prior[p].at("core"))));
        for (auto& place : prior[p]["placements"]) place["equipment"] = Uid(replacements.at(Key(place.at("equipment"))));
        for (auto& score : prior[p]["assignment_scores"]) score["equipment"] = Uid(replacements.at(Key(score.at("equipment"))));
    }
    return true;
}

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

GroupSolution SolveGroup(std::vector<Json> roles, const Json& prior,
                         const std::vector<Json>& prior_preferences, const Json& inventory,
                         const std::set<std::uint64_t>& excluded, Budget& budget,
                         const ItemIndex& item_index, std::size_t candidates, bool feasibility) {
    GroupSolution best;
    EquivalenceCache equivalence_cache;
    std::set<std::uint64_t> fixed_prior;
    for (const auto& plan : prior) {
        if (feasibility) { const auto ids = Used(plan); fixed_prior.insert(ids.begin(), ids.end()); }
        else if (!plan.at("core").is_null()) fixed_prior.insert(Key(plan.at("core")));
    }
    for (const auto& preference : prior_preferences) { const auto fixed = Kept(preference); fixed_prior.insert(fixed.begin(), fixed.end()); }
    if (!feasibility) for (const auto& plan : prior) for (const auto uid : Used(plan)) {
        if (fixed_prior.contains(uid) || budget.Stop()) continue;
        auto refilled = prior;
        if (!Refill(refilled, prior_preferences, inventory, {uid}, excluded, budget, item_index, equivalence_cache) && budget.exhaustive)
            fixed_prior.insert(uid);
    }
    for (auto& role : roles) {
        auto blocked = Excluded(role); blocked.insert(excluded.begin(), excluded.end());
        blocked.insert(fixed_prior.begin(), fixed_prior.end()); SetExcluded(role, blocked);
    }
    Json current = Json::array();
    bool done{}, allow_partial = feasibility;
    std::size_t candidate_limit = candidates;
    std::function<void(std::size_t, std::set<std::uint64_t>, Priority, double)> assign;
    assign = [&](std::size_t index, std::set<std::uint64_t> used, Priority priority, double score) {
        if (budget.cancel && budget.cancel()) Fail(AlgorithmFailure::cancelled);
        if (index == roles.size()) {
            auto refilled = prior;
            bool overlap{};
            for (const auto& plan : prior) for (const auto uid : Used(plan)) if (used.contains(uid)) overlap = true;
            if (overlap && !Refill(refilled, prior_preferences, inventory, used, excluded, budget, item_index, equivalence_cache)) return;
            if (!current.empty() && (current.size() > best.count || (current.size() == best.count &&
                (best.score < 0 || priority > best.priority || (priority == best.priority && score > best.score))))) {
                best.score = score; best.priority = priority; best.current = current; best.prior = std::move(refilled); best.count = current.size();
            }
            if (feasibility && best.count == roles.size()) done = true;
            return;
        }
        if (done) return;
        if (budget.Stop()) {
            // A budget boundary must not discard complete boards already
            // found for this group's prefix during coverage.
            if (feasibility && !current.empty()) assign(roles.size(), std::move(used), priority, score);
            return;
        }
        auto role = roles[index]; auto blocked = Excluded(role); blocked.insert(used.begin(), used.end()); SetExcluded(role, blocked);
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
        }, feasibility, roles.size() - index, MinimumVisits(role, inventory));
        best.limited = best.limited || (!complete && !exact_solo);
        if (allow_partial && !done && budget.exhaustive) assign(index + 1, std::move(used), priority, score);
    };
    assign(0, {}, {}, 0);
    if (!feasibility) {
        while (best.count == 0 && best.limited && budget.exhaustive && candidate_limit < budget.maximum_calls) {
            candidate_limit = std::min(budget.maximum_calls, candidate_limit * 2);
            best.limited = false; assign(0, {}, {}, 0);
        }
        if (best.count == 0 && budget.exhaustive) { allow_partial = true; assign(0, {}, {}, 0); }
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
    budget.Team(requests.size(), remaining_roles);
    ItemIndex item_index;
    for (const auto& item : inventory.at("items")) {
        if (cancel && cancel()) Fail(AlgorithmFailure::cancelled);
        if (!item_index.emplace(Key(item.at("uid")), &item).second) Fail(AlgorithmFailure::invalid_inventory);
    }
    Json plans = Json::array();
    std::vector<std::string> group_reasons(requests.size(), "candidate_limit");
    bool candidate_limited{};
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
                                 item_index, std::max<std::size_t>(2, candidates), true);
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
        auto solved = SolveGroup(requests[index], prior, prior_preferences, inventory, blocked, slice, item_index, candidates, false);
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
            const auto complete = Enumerate(role, inventory, budget, candidates, [&](const Json& plan) { options.push_back(plan); return true; });
            role_top_k.push_back({{"identity", RoleIdentity(role)}, {"options", options}, {"enumeration_complete", complete}});
        }
    }
    static_cast<void>(budget.Stop());
    auto report = budget.Report();
    report["coverage_groups_visited"] = coverage_visited;
    if (candidate_limited && report.at("exhaustive") == true) { report["exhaustive"] = false; report["stop_reason"] = "candidate_limit"; }
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
