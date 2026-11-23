#include "character_math.hpp"
#include "algorithm_support.hpp"
#include <array>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <vector>

namespace equipment_manager {
namespace {
using namespace algorithm_detail;
using Quantity = std::int64_t;
using Stock = std::map<std::string, Quantity, std::less<>>;
Quantity Count(const Json& value) {
    if (!value.is_number_integer()) Fail();
    const auto number = Number(value);
    if (number < 0 || number > 1000000000000000.) Fail();
    return value.get<Quantity>();
}
Quantity Add(Quantity a, Quantity b) { if (b > INT64_MAX - a) Fail(); return a + b; }
Quantity Multiply(Quantity a, Quantity b) { if (a && b > INT64_MAX / a) Fail(); return a * b; }
Stock ReadStock(const Json& value) {
    if (!value.is_object()) Fail(); Stock result;
    for (const auto& [id, quantity] : value.items()) { if (id.empty() || id.size() > 128) Fail(); result[id] = Count(quantity); }
    return result;
}
Quantity Get(const Stock& stock, std::string_view id) { const auto found = stock.find(id); return found == stock.end() ? 0 : found->second; }
struct Material { std::string id; Quantity experience{}, gold{}; bool gold_known{}; };
Json Experience(const Json& request, Budget& budget) {
    const auto required = Count(request.at("required_experience"));
    if (!required) return {{"status", "complete"}, {"materials", Json::array()}, {"required_experience", 0}, {"overflow", 0},
        {"item_count", 0}, {"gold_cost", 0}, {"known_gold", 0}};
    if (!request.contains("materials") || !request.at("materials").is_array()) Fail();
    std::vector<Material> materials; std::set<std::string> ids;
    for (const auto& row : request.at("materials")) {
        Material material; material.id = Id(row.at("item_id")); material.experience = Count(row.at("experience"));
        if (!ids.insert(material.id).second || !material.experience) Fail();
        if (row.contains("gold_cost") && !row.at("gold_cost").is_null()) { material.gold = Count(row.at("gold_cost")); material.gold_known = true; }
        materials.push_back(material);
    }
    std::sort(materials.begin(), materials.end(), [](const Material& a, const Material& b) {
        return a.experience == b.experience ? a.id < b.id : a.experience > b.experience;
    });
    if (materials.empty()) Fail(AlgorithmFailure::incomplete_attributes);
    Quantity divisor{};
    for (const auto& material : materials) divisor = std::gcd(divisor, material.experience);
    const auto minimum = required / divisor + (required % divisor ? 1 : 0);
    const auto largest = materials.front().experience / divisor;
    // A multiple of the largest denomination always reaches the target with
    // less than one such coin of overflow. The reference's M^2 bound is looser.
    const auto theoretical = Add(minimum, largest - 1);
    const auto remaining = budget.maximum_nodes - std::min(budget.nodes, budget.maximum_nodes);
    const auto limit = std::min<Quantity>(theoretical, static_cast<Quantity>(remaining));
    if (minimum > limit) {
        budget.exhaustive = false; budget.reason = "node_budget";
        return {{"status", "unavailable"}, {"reason", "optimization_limit"}, {"materials", Json::array()}, {"required_experience", required}, {"overflow", nullptr}, {"gold_cost", nullptr}};
    }
    const auto size = static_cast<std::size_t>(limit + 1);
    std::vector<Quantity> counts(size, INT64_MAX);
    std::vector<int> choices(size, -1); counts[0] = 0;
    Quantity target{-1};
    for (Quantity amount = 1; amount <= limit; ++amount) {
        if (budget.Stop()) break;
        ++budget.nodes;
        for (std::size_t i{}; i < materials.size(); ++i) {
            const auto value = materials[i].experience / divisor;
            if (value > amount || counts[static_cast<std::size_t>(amount - value)] == INT64_MAX) continue;
            const auto candidate = counts[static_cast<std::size_t>(amount - value)] + 1;
            if (candidate < counts[static_cast<std::size_t>(amount)]) { counts[static_cast<std::size_t>(amount)] = candidate; choices[static_cast<std::size_t>(amount)] = static_cast<int>(i); }
        }
        if (amount >= minimum && choices[static_cast<std::size_t>(amount)] >= 0) { target = amount; break; }
    }
    if (target < 0) {
        if (budget.exhaustive) { budget.exhaustive = false; budget.reason = "node_budget"; }
        return {{"status", "unavailable"}, {"reason", "optimization_limit"}, {"materials", Json::array()}, {"required_experience", required}, {"overflow", nullptr}, {"gold_cost", nullptr}};
    }
    std::vector<Quantity> selected(materials.size());
    for (auto cursor = target; cursor > 0;) {
        const auto index = static_cast<std::size_t>(choices[static_cast<std::size_t>(cursor)]); ++selected[index]; cursor -= materials[index].experience / divisor;
    }
    Json rows = Json::array(); Quantity gold{}, number{}; bool known{true};
    for (std::size_t i{}; i < materials.size(); ++i) if (selected[i]) {
        const auto& material = materials[i]; number = Add(number, selected[i]); known = known && material.gold_known;
        if (material.gold_known) gold = Add(gold, Multiply(selected[i], material.gold));
        rows.push_back({{"item_id", material.id}, {"quantity", selected[i]}, {"experience", material.experience}});
    }
    return {{"status", known ? "complete" : "partial"}, {"materials", rows}, {"required_experience", required},
        {"overflow", Multiply(target, divisor) - required}, {"item_count", number}, {"gold_cost", known ? Json(gold) : Json(nullptr)}, {"known_gold", gold}};
}

std::pair<std::string, int> Tier(std::string_view id) {
    if (id.size() < 5 || id.substr(id.size() - 4, 3) != "_lv" || id.back() < '1' || id.back() > '3') return {};
    const auto family = id.substr(0, id.size() - 4);
    bool allowed = family == "EquipmentUpMaterial";
    for (const auto& [prefix, last] : std::array<std::pair<std::string_view, char>, 3>{{
        {"OrdinaryMonMaterial_0", '4'}, {"SkillUpMaterial_0", '5'}, {"WeaponBreakMaterial_0", '5'}}})
        if (family.size() == prefix.size() + 1 && family.starts_with(prefix) && family.back() >= '1' && family.back() <= last) allowed = true;
    return allowed ? std::pair<std::string, int>{std::string(family), id.back() - '0'} : std::pair<std::string, int>{};
}
std::string TierId(std::string_view family, int tier) { return std::string(family) + "_lv" + std::to_string(tier); }
struct Recipe { Quantity gold{}; bool known{}; };
using Recipes = std::map<std::string, Recipe, std::less<>>;
Recipes ReadRecipes(const Json& request) {
    Recipes result;
    if (!request.contains("recipes")) return result;
    if (!request.at("recipes").is_array()) Fail();
    for (const auto& row : request.at("recipes")) {
        const auto from = Id(row.at("from")), to = Id(row.at("to")); const auto lower = Tier(from), higher = Tier(to);
        if (lower.first.empty() || lower.first != higher.first || higher.second != lower.second + 1 ||
            (row.contains("ratio") && Count(row.at("ratio")) != 3)) Fail();
        Recipe recipe;
        if (row.contains("gold_cost") && !row.at("gold_cost").is_null()) { recipe.gold = Count(row.at("gold_cost")); recipe.known = true; }
        if (!result.emplace(to, recipe).second) Fail();
    }
    return result;
}
bool CraftExact(const std::string& family, int tier, Quantity required, Stock& stock, Json* crafts,
                const Recipes& recipes, bool& unknown) {
    if (required <= 0) return true;
    if (tier <= 1) return false;
    const auto lower = TierId(family, tier - 1), higher = TierId(family, tier);
    const auto recipe = recipes.find(higher);
    if (recipe == recipes.end() || !recipe->second.known) { unknown = true; return false; }
    const auto gold = Multiply(required, recipe->second.gold);
    if (Get(stock, "Gold") < gold) return false;
    stock["Gold"] = Get(stock, "Gold") - gold;
    const auto shortfall = std::max<Quantity>(0, Multiply(required, 3) - Get(stock, lower));
    if (shortfall && !CraftExact(family, tier - 1, shortfall, stock, crafts, recipes, unknown)) return false;
    stock[lower] -= required * 3; stock[higher] = Add(Get(stock, higher), required);
    if (crafts) crafts->push_back({{"from", lower}, {"to", higher}, {"quantity", required}, {"consumed", required * 3}, {"gold_cost", gold}});
    return true;
}
void Craft(const std::string& family, int tier, Quantity required, Stock& stock, Json* crafts,
           const Recipes& recipes, bool& unknown, bool partial) {
    auto candidate = stock; Json operations = Json::array();
    if (CraftExact(family, tier, required, candidate, crafts ? &operations : nullptr, recipes, unknown)) {
        stock = std::move(candidate); if (crafts) for (auto& operation : operations) crafts->push_back(std::move(operation)); return;
    }
    if (!partial || required <= 1) return;
    Quantity low{}, high = required - 1;
    while (low < high) {
        const auto middle = low + (high - low + 1) / 2; candidate = stock;
        if (CraftExact(family, tier, middle, candidate, nullptr, recipes, unknown)) low = middle;
        else high = middle - 1;
    }
    if (low) {
        candidate = stock; operations = Json::array();
        if (!CraftExact(family, tier, low, candidate, crafts ? &operations : nullptr, recipes, unknown)) Fail(AlgorithmFailure::invalid_inventory);
        stock = std::move(candidate); if (crafts) for (auto& operation : operations) crafts->push_back(std::move(operation));
    }
}
Stock Allocate(const Stock& required, Stock& available, bool crafting, const Recipes& recipes,
               bool& unknown, Json* crafts = nullptr, bool partial = true) {
    std::vector<std::string> ids; for (const auto& [id, q] : required) if (q) ids.push_back(id);
    std::sort(ids.begin(), ids.end(), [](const auto& a, const auto& b) {
        auto left = Tier(a), right = Tier(b);
        if (left.first.empty()) left = {a, 0}; if (right.first.empty()) right = {b, 0};
        return left == right ? a < b : left < right;
    });
    Stock credited;
    for (const auto& id : ids) {
        const auto tier = Tier(id);
        if (crafting && !tier.first.empty() && required.at(id) > Get(available, id))
            Craft(tier.first, tier.second, required.at(id) - Get(available, id), available, crafts, recipes, unknown, partial);
        const auto count = std::min(required.at(id), Get(available, id)); credited[id] = count; available[id] = Get(available, id) - count;
    }
    return credited;
}
bool Satisfied(const Stock& required, Stock stock, bool crafting, const Recipes& recipes) {
    bool unknown{}; const auto credited = Allocate(required, stock, crafting, recipes, unknown, nullptr, false);
    return std::all_of(required.begin(), required.end(), [&](const auto& item) { return Get(credited, item.first) >= item.second; });
}

Json Stamina(const Json& request, Budget& budget) {
    auto required = ReadStock(request.at("requirements")); Stock outside_scope;
    const auto scope = request.value("scope", std::string("all_materials"));
    if (scope != "all_materials" && scope != "cultivation_material_stages") Fail();
    if (scope == "cultivation_material_stages") {
        std::set<std::string> yielded;
        if (request.contains("stages")) {
            if (!request.at("stages").is_array()) Fail(AlgorithmFailure::incomplete_attributes);
            for (const auto& stage : request.at("stages")) for (const auto& [id, count] : ReadStock(stage.at("yields"))) if (count) yielded.insert(id);
        }
        for (auto item = required.begin(); item != required.end();) {
            const auto& id = item->first;
            if (id == "Fons" || ((id.starts_with("OrdinaryMonMaterial_") || id.starts_with("Worldboss_material_")) && !yielded.contains(id))) {
                outside_scope.emplace(*item); item = required.erase(item);
            } else ++item;
        }
    }
    const auto owned = request.contains("owned") ? ReadStock(request.at("owned")) : Stock{};
    const auto hunter = Integer(request.at("hunter_level"), 1, 60);
    const std::array thresholds{10, 20, 30, 40, 45, 50, 55};
    const auto native = static_cast<int>(std::count_if(thresholds.begin(), thresholds.end(), [&](int level) { return hunter >= level; }));
    const auto effective = request.contains("effective_identification_level") ? Integer(request.at("effective_identification_level"), 0, 7) : native;
    if (effective != native && !(native >= 3 && effective == native - 1)) Fail();
    const Json identification{{"hunter_level", hunter}, {"native_level", native}, {"effective_level", effective}, {"lowered", effective != native}};
    const bool crafting = request.value("allow_crafting", true);
    const auto recipes = ReadRecipes(request); bool unknown_recipe{};
    auto remainder = owned; Json initial_crafts = Json::array(); const auto credited = Allocate(required, remainder, crafting, recipes, unknown_recipe, &initial_crafts);
    Json deficits = Json::array(); Stock pending;
    for (const auto& [id, quantity] : required) {
        const auto deficit = quantity - Get(credited, id);
        deficits.push_back({{"item_id", id}, {"required_quantity", quantity}, {"owned_quantity", Get(credited, id)}, {"deficit_quantity", deficit}});
        if (deficit) pending[id] = quantity;
    }
    const auto result = [&](const char* status, const Json& runs, Quantity stamina, bool total_known, const Json& unresolved, const Json& gaps, const Json& crafts) {
        return Json{{"status", status}, {"identification", identification}, {"deficits", deficits}, {"runs", runs},
            {"known_stamina", stamina}, {"total_stamina", total_known ? Json(stamina) : Json(nullptr)},
            {"unresolved_item_ids", unresolved}, {"gaps", gaps}, {"crafts", crafts}, {"scope", scope}, {"outside_stamina_materials", outside_scope}};
    };
    if (pending.empty()) return result("complete", Json::array(), 0, true, Json::array(), Json::array(), initial_crafts);
    if (!request.contains("stages") || !request.at("stages").is_array()) Fail(AlgorithmFailure::incomplete_attributes);
    struct Stage { std::string id, source; Quantity stamina{}; Stock yields; };
    std::vector<Stage> stages; std::set<std::string> stage_ids, reachable;
    for (const auto& row : request.at("stages")) {
        const auto id = Id(row.at("stage_id")); if (!stage_ids.insert(id).second) Fail();
        const auto minimum_hunter = Integer(row.at("minimum_hunter_level"), 0, 60);
        const auto minimum_identification = Integer(row.at("minimum_identification_level"), 0, 7);
        const auto cost = Count(row.at("stamina_cost")); if (!cost) Fail();
        const auto yields = ReadStock(row.at("yields"));
        if (hunter < minimum_hunter || effective < minimum_identification) continue;
        stages.push_back({id, row.value("source", std::string("user_supplied")), cost, yields});
        for (const auto& [item, amount] : yields) if (amount) reachable.insert(item);
    }
    if (crafting) {
        std::vector<std::string> initial(reachable.begin(), reachable.end());
        // Owned lower-tier stock can combine with a future Gold drop. It is
        // not a renewable direct yield, but its upper conversion types must
        // remain in the exact search rather than being pruned as unavailable.
        for (const auto& [id, count] : owned) if (count) initial.push_back(id);
        for (const auto& id : initial) {
            const auto [family, tier] = Tier(id);
            if (!family.empty()) for (auto upper = tier + 1; upper <= 3; ++upper) reachable.insert(TierId(family, upper));
        }
    }
    Json unresolved = Json::array(); Stock solvable = required;
    for (const auto& [id, quantity] : pending) if (!reachable.contains(id)) { unresolved.push_back(id); solvable.erase(id); }
    if (solvable.empty()) return result("unavailable", Json::array(), 0, false, unresolved, Json::array({"material_yield_unavailable"}), initial_crafts);
    std::set<std::string> relevant;
    for (const auto& [id, quantity] : solvable) if (quantity) {
        relevant.insert(id); const auto [family, tier] = Tier(id);
        if (crafting && !family.empty()) for (int level = 1; level <= 3; ++level) relevant.insert(TierId(family, level));
    }
    Quantity maximum_crafting_gold{};
    if (crafting) for (const auto& [id, quantity] : solvable) {
        const auto [family, tier] = Tier(id);
        if (family.empty()) continue;
        Quantity multiplicity{1};
        for (int lower = tier; lower > 1; --lower, multiplicity *= 3) {
            const auto recipe = recipes.find(TierId(family, lower));
            if (recipe != recipes.end() && recipe->second.known)
                maximum_crafting_gold = Add(maximum_crafting_gold, Multiply(Multiply(quantity, multiplicity), recipe->second.gold));
        }
    }
    if (maximum_crafting_gold) relevant.insert("Gold");
    std::vector<std::string> ids(relevant.begin(), relevant.end());
    std::vector<Quantity> cap, initial;
    for (const auto& id : ids) {
        auto maximum = Get(solvable, id); const auto [family, tier] = Tier(id);
        if (id == "Gold") maximum = Add(maximum, maximum_crafting_gold);
        if (crafting && !family.empty()) {
            maximum = 0; Quantity factor{1};
            for (int upper = tier; upper <= 3; ++upper, factor *= 3) maximum = Add(maximum, Multiply(Get(solvable, TierId(family, upper)), factor));
        }
        cap.push_back(maximum); initial.push_back(std::min(Get(owned, id), maximum));
    }
    struct Node { Quantity stamina{}, runs{}; std::vector<Quantity> stock; };
    struct Greater { bool operator()(const Node& a, const Node& b) const {
        if (a.stamina != b.stamina) return a.stamina > b.stamina;
        if (a.runs != b.runs) return a.runs > b.runs;
        return a.stock > b.stock;
    } };
    using Distance = std::pair<Quantity, Quantity>;
    std::map<std::vector<Quantity>, Distance> distance;
    std::map<std::vector<Quantity>, std::pair<std::vector<Quantity>, std::size_t>> previous;
    std::priority_queue<Node, std::vector<Node>, Greater> queue;
    distance[initial] = {0, 0}; queue.push({0, 0, initial});
    std::vector<Quantity> final; Quantity minimum{}; bool found{};
    while (!queue.empty()) {
        if (budget.Stop()) break;
        auto node = queue.top(); queue.pop();
        if (distance.at(node.stock) != Distance(node.stamina, node.runs)) continue;
        Stock available; for (std::size_t i{}; i < ids.size(); ++i) available[ids[i]] = node.stock[i];
        if (Satisfied(solvable, std::move(available), crafting, recipes)) { final = std::move(node.stock); minimum = node.stamina; found = true; break; }
        ++budget.nodes;
        for (std::size_t s{}; s < stages.size(); ++s) {
            auto next = node.stock;
            for (std::size_t i{}; i < ids.size(); ++i) next[i] = std::min(cap[i], Add(next[i], Get(stages[s].yields, ids[i])));
            if (next == node.stock) continue;
            const Distance candidate{Add(node.stamina, stages[s].stamina), Add(node.runs, 1)};
            const auto existing = distance.find(next);
            if (existing != distance.end() && candidate >= existing->second) continue;
            distance[next] = candidate; previous[next] = {node.stock, s}; queue.push({candidate.first, candidate.second, std::move(next)});
        }
    }
    if (!found) {
        if (unknown_recipe) return result("unavailable", Json::array(), 0, false, unresolved, Json::array({"craft_recipe_unavailable"}), initial_crafts);
        if (maximum_crafting_gold && !reachable.contains("Gold") && Get(owned, "Gold") < maximum_crafting_gold && budget.exhaustive) {
            unresolved.push_back("Gold");
            return result("unavailable", Json::array(), 0, false, unresolved, Json::array({"material_yield_unavailable"}), initial_crafts);
        }
        if (budget.exhaustive && queue.empty()) {
            std::set<std::string> reported; for (const auto& value : unresolved) reported.insert(value.get<std::string>());
            for (const auto& [id, count] : pending) if (count && reported.insert(id).second) unresolved.push_back(id);
            return result("unavailable", Json::array(), 0, false, unresolved, Json::array({"material_yield_unavailable"}), initial_crafts);
        }
        return result("unavailable", Json::array(), 0, false, unresolved, Json::array({"optimization_limit"}), initial_crafts);
    }
    std::vector<Quantity> counts(stages.size());
    for (auto cursor = final; cursor != initial;) { const auto& prior = previous.at(cursor); ++counts[prior.second]; cursor = prior.first; }
    Json runs = Json::array(); auto produced = owned;
    for (std::size_t s{}; s < stages.size(); ++s) if (counts[s]) {
        Stock outputs;
        for (const auto& [id, count] : stages[s].yields) { outputs[id] = Multiply(count, counts[s]); produced[id] = Add(Get(produced, id), outputs[id]); }
        runs.push_back({{"stage_id", stages[s].id}, {"runs", counts[s]}, {"stamina_cost_per_run", stages[s].stamina},
            {"total_stamina", Multiply(counts[s], stages[s].stamina)}, {"produced", outputs}, {"source", stages[s].source}});
    }
    Json crafts = Json::array(); bool unused_unknown{}; Allocate(solvable, produced, crafting, recipes, unused_unknown, &crafts);
    const bool complete = unresolved.empty() && !unknown_recipe;
    Json gaps = Json::array();
    if (!unresolved.empty()) gaps.push_back("material_yield_unavailable");
    if (unknown_recipe) gaps.push_back("craft_recipe_unavailable");
    return result(complete ? "complete" : "partial", runs, minimum, complete, unresolved, gaps, crafts);
}

Json Cultivation(const Json& request, Budget& budget) {
    if (!request.contains("plans") || !request.at("plans").is_array() || request.at("plans").empty() || request.at("plans").size() > 64) Fail();
    if (request.contains("owned_complete") && !request.at("owned_complete").is_boolean()) Fail();
    const bool owned_known = request.value("owned_complete", true) && (!request.contains("owned") || !request.at("owned").is_null());
    Stock available = owned_known && request.contains("owned") ? ReadStock(request.at("owned")) : Stock{};
    const bool crafting = request.value("allow_crafting", true);
    if (request.contains("compare_stamina") && !request.at("compare_stamina").is_boolean()) Fail();
    const bool compare = request.value("compare_stamina", false);
    if (compare && !request.contains("stamina")) Fail();
    const auto recipes = ReadRecipes(request);
    Stock total; Json plans = Json::array(); Quantity total_gold{}, known_gold{}; bool all_gold_known{true}, all_materials_known{true};
    std::vector<Stock> comparison_requirements, comparison_owned;
    std::set<std::string> plan_ids;
    for (const auto& plan : request.at("plans")) {
        if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted);
        const auto id = Id(plan.at("id")); if (!plan_ids.insert(id).second || !plan.at("tracks").is_array()) Fail();
        Stock needs; Quantity gold{}; bool gold_known{true}, materials_known{true}; Json tracks = Json::array();
        std::map<std::string, Quantity> targets;
        std::set<std::string> track_ids, incomplete;
        if (plan.contains("incomplete_tracks")) {
            if (!plan.at("incomplete_tracks").is_array()) Fail();
            for (const auto& value : plan.at("incomplete_tracks")) incomplete.insert(Id(value));
        }
        for (const auto& track : plan.at("tracks")) {
            const auto track_id = Id(track.at("id"));
            if (!track_ids.insert(track_id).second) Fail();
            if (track.contains("complete") && !track.at("complete").is_boolean()) Fail();
            if (!track.value("complete", true) || !track.contains("current") || track.at("current").is_null() ||
                !track.contains("target") || track.at("target").is_null()) incomplete.insert(track_id);
            if (track.contains("target") && !track.at("target").is_null()) targets.emplace(track_id, Count(track.at("target")));
            if (!incomplete.contains(track_id) && targets.at(track_id) < Count(track.at("current"))) Fail();
        }
        const auto details_for = [](const Json& input) {
            auto details = input; details.erase("steps"); details.erase("experience_materials"); return details;
        };
        const auto unknown_track = [&](const std::string& track_id, const Json* source = nullptr) {
            materials_known = false; gold_known = false;
            auto details = source ? details_for(*source) : Json{{"id", track_id}};
            details.update({{"status", "unavailable"}, {"required_experience", nullptr}, {"materials", nullptr},
                {"gold", nullptr}, {"error_code", "algorithm.incomplete_attributes"}});
            tracks.push_back(std::move(details));
        };
        for (const auto& track_id : incomplete) if (!track_ids.contains(track_id)) unknown_track(track_id);
        for (const auto& track : plan.at("tracks")) {
            const auto track_id = Id(track.at("id"));
            if (incomplete.contains(track_id)) { unknown_track(track_id, &track); continue; }
            Stock track_needs; Quantity track_gold{}; bool track_gold_known{true};
            try {
            const auto target = Count(track.at("target")); auto current = Count(track.at("current")); Quantity experience{};
            if (current < target && (!track.contains("steps") || track.at("steps").is_null())) Fail(AlgorithmFailure::incomplete_attributes);
            if (track.contains("steps") && !track.at("steps").is_null() && !track.at("steps").is_array()) Fail();
            while (current < target) {
                if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted); ++budget.nodes;
                const Json* step{};
                for (const auto& row : track.at("steps")) if (Count(row.at("from")) == current && Count(row.at("to")) <= target) {
                    if (step) Fail(); step = &row;
                }
                if (!step || Count(step->at("to")) <= current) Fail(AlgorithmFailure::incomplete_attributes);
                if (step->contains("requires")) for (const auto& prerequisite : step->at("requires")) {
                    const auto other = Id(prerequisite.at("track"));
                    if (!targets.contains(other)) Fail(AlgorithmFailure::incomplete_attributes);
                    if (targets.at(other) < Count(prerequisite.at("minimum"))) Fail();
                }
                if (step->contains("items")) {
                    if (step->at("items").is_null()) Fail(AlgorithmFailure::incomplete_attributes);
                    for (const auto& [item, q] : ReadStock(step->at("items"))) track_needs[item] = Add(Get(track_needs, item), q);
                }
                if (step->contains("gold") && !step->at("gold").is_null()) track_gold = Add(track_gold, Count(step->at("gold")));
                else track_gold_known = false;
                if (step->contains("experience")) {
                    if (step->at("experience").is_null()) Fail(AlgorithmFailure::incomplete_attributes);
                    experience = Add(experience, Count(step->at("experience")));
                }
                current = Count(step->at("to"));
            }
            if (experience && track.contains("already_experience")) {
                if (track.at("already_experience").is_null()) Fail(AlgorithmFailure::incomplete_attributes);
                experience -= std::min(experience, Count(track.at("already_experience")));
            }
            auto details = details_for(track); details["required_experience"] = experience;
            if (experience) {
                if (!track.contains("experience_materials") || track.at("experience_materials").is_null()) Fail(AlgorithmFailure::incomplete_attributes);
                auto books = Experience({{"required_experience", experience}, {"materials", track.at("experience_materials")}}, budget);
                if (books.at("status") == "unavailable") Fail(AlgorithmFailure::budget_exhausted);
                for (const auto& row : books.at("materials")) { const auto item = Id(row.at("item_id")); track_needs[item] = Add(Get(track_needs, item), Count(row.at("quantity"))); }
                track_gold = Add(track_gold, Count(books.at("known_gold"))); track_gold_known = track_gold_known && !books.at("gold_cost").is_null();
                details["experience_materials"] = std::move(books);
            }
            for (const auto& [item, q] : track_needs) needs[item] = Add(Get(needs, item), q);
            gold = Add(gold, track_gold); gold_known = gold_known && track_gold_known;
            details["materials"] = track_needs; details["gold"] = track_gold_known ? Json(track_gold) : Json(nullptr);
            details["status"] = track_gold_known ? "complete" : "partial";
            tracks.push_back(std::move(details));
            } catch (const InvalidAlgorithm& failure) {
                if (failure.reason != AlgorithmFailure::incomplete_attributes && failure.reason != AlgorithmFailure::incomplete_main_stat) throw;
                unknown_track(track_id, &track);
            }
        }
        needs["Gold"] = Add(Get(needs, "Gold"), gold);
        Json crafts = Json::array(); bool unknown_recipe{};
        const auto before_gold = Get(available, "Gold");
        const auto owned = owned_known ? Allocate(needs, available, crafting, recipes, unknown_recipe, &crafts) : Stock{}; Stock deficits;
        if (compare) { comparison_requirements.push_back(needs); comparison_owned.push_back(owned); }
        Quantity craft_gold{}; for (const auto& operation : crafts) craft_gold = Add(craft_gold, Count(operation.at("gold_cost")));
        gold = Add(gold, craft_gold); gold_known = gold_known && !unknown_recipe;
        for (const auto& [item, q] : needs) { total[item] = Add(Get(total, item), q); deficits[item] = q - Get(owned, item); }
        known_gold = Add(known_gold, gold); all_gold_known = all_gold_known && gold_known;
        all_materials_known = all_materials_known && materials_known;
        if (gold_known) total_gold = Add(total_gold, gold);
        auto shown_needs = Json(needs), shown_owned = Json(owned), shown_deficits = Json(deficits);
        shown_needs["Gold"] = gold_known ? Json(gold) : Json(nullptr);
        shown_owned["Gold"] = before_gold - Get(available, "Gold");
        shown_deficits["Gold"] = gold_known ? Json(std::max<Quantity>(0, gold - (before_gold - Get(available, "Gold")))) : Json(nullptr);
        auto plan_known_needs = Json(needs); plan_known_needs["Gold"] = gold;
        auto plan_known_deficits = Json(deficits); plan_known_deficits["Gold"] = std::max<Quantity>(0, gold - (before_gold - Get(available, "Gold")));
        auto plan_result = plan;
        plan_result.update({{"materials", materials_known ? shown_needs : Json(nullptr)}, {"known_materials", plan_known_needs},
            {"materials_complete", materials_known}, {"owned_allocation_complete", owned_known && all_materials_known && all_gold_known},
            {"owned_allocated", owned_known ? shown_owned : Json(nullptr)}, {"deficits", owned_known && materials_known ? shown_deficits : Json(nullptr)},
            {"known_deficits", owned_known ? plan_known_deficits : Json(nullptr)}, {"crafts", owned_known ? crafts : Json(nullptr)},
            {"gold", gold_known ? Json(gold) : Json(nullptr)}, {"known_gold", gold},
            {"craft_gold", owned_known && !unknown_recipe ? Json(craft_gold) : Json(nullptr)}, {"tracks", tracks}});
        plans.push_back(std::move(plan_result));
    }
    Json shown_materials = total; shown_materials["Gold"] = all_gold_known ? Json(total_gold) : Json(nullptr);
    Json known_materials = total; known_materials["Gold"] = known_gold;
    Json result{{"status", owned_known && all_gold_known && all_materials_known ? "complete" : "partial"}, {"plans", plans},
        {"materials", all_materials_known ? shown_materials : Json(nullptr)}, {"known_materials", known_materials}, {"materials_complete", all_materials_known},
        {"gold", all_gold_known ? Json(total_gold) : Json(nullptr)}, {"known_gold", known_gold}, {"owned_complete", owned_known},
        {"remaining_owned", owned_known && all_gold_known && all_materials_known ? Json(available) : Json(nullptr)},
        {"unallocated_owned_after_known_costs", owned_known ? Json(available) : Json(nullptr)}};
    result["base_materials"] = total;
    const auto unavailable_stamina = [](std::string_view reason) {
        return Json{{"status", "unavailable"}, {"known_stamina", nullptr}, {"total_stamina", nullptr}, {"deficits", nullptr},
            {"runs", nullptr}, {"crafts", nullptr}, {"gaps", Json::array({reason})}};
    };
    if (request.contains("stamina")) {
        if (!owned_known) {
            result["stamina"] = unavailable_stamina("owned_materials_unavailable"); result["stamina"]["owned_complete"] = false;
        } else {
            auto stamina = request.at("stamina"); stamina["requirements"] = total;
            stamina["owned"] = request.contains("owned") ? request.at("owned") : Json::object(); stamina["allow_crafting"] = crafting;
            if (request.contains("recipes")) stamina["recipes"] = request.at("recipes");
            result["stamina"] = Stamina(stamina, budget);
            if (!all_gold_known) {
                result["stamina"]["total_stamina"] = nullptr;
                if (result["stamina"]["status"] == "complete") result["stamina"]["status"] = "partial";
                result["stamina"]["gaps"].push_back("gold_cost_unavailable");
            }
            if (!all_materials_known) {
                result["stamina"]["total_stamina"] = nullptr;
                if (result["stamina"]["status"] == "complete") result["stamina"]["status"] = "partial";
                result["stamina"]["gaps"].push_back("cultivation_cost_unavailable");
            }
        }
    }
    if (compare) {
        Quantity separate{}, known_separate{}; bool complete{true}, separate_complete{true}, any_known{}, ledger_complete{true}; Json ledger = Json::array();
        const auto solve = [&](const Stock& needs, const Stock& allocated, bool allocation_known) {
            if (!owned_known) return unavailable_stamina("owned_materials_unavailable");
            if (!allocation_known) return unavailable_stamina("cultivation_cost_unavailable");
            if (budget.Stop()) return unavailable_stamina("optimization_limit");
            auto stamina = request.at("stamina"); stamina["requirements"] = needs; stamina["owned"] = allocated; stamina["allow_crafting"] = crafting;
            if (request.contains("recipes")) stamina["recipes"] = request.at("recipes");
            return Stamina(stamina, budget);
        };
        for (std::size_t p{}; p < result["plans"].size(); ++p) {
            auto& plan = result["plans"][p]; const bool allocation_known = plan.at("owned_allocation_complete") == true;
            auto stamina = solve(comparison_requirements[p], comparison_owned[p], allocation_known);
            if (stamina.at("total_stamina").is_null()) { complete = false; separate_complete = false; }
            else separate = Add(separate, Count(stamina.at("total_stamina")));
            if (!stamina.at("known_stamina").is_null()) { any_known = true; known_separate = Add(known_separate, Count(stamina.at("known_stamina"))); }
            plan["stamina"] = std::move(stamina); auto available_for_tracks = comparison_owned[p];
            ledger_complete = ledger_complete && allocation_known;
            for (auto& track : plan["tracks"]) {
                const bool track_known = allocation_known && track.at("status") == "complete";
                Stock track_needs, track_owned;
                if (!track.at("materials").is_null()) {
                    track_needs = ReadStock(track.at("materials"));
                    if (!track.at("gold").is_null()) track_needs["Gold"] = Add(Get(track_needs, "Gold"), Count(track.at("gold")));
                    if (track_known) { bool unused{}; track_owned = Allocate(track_needs, available_for_tracks, false, recipes, unused); }
                    for (const auto& [item, q] : track_needs) ledger.push_back({{"plan_id", plan.at("id")}, {"track_id", track.at("id")}, {"item_id", item},
                        {"required_quantity", q}, {"allocated_owned", track_known ? Json(Get(track_owned, item)) : Json(nullptr)},
                        {"remaining_quantity", track_known ? Json(q - Get(track_owned, item)) : Json(nullptr)}});
                }
                track["stamina"] = solve(track_needs, track_owned, track_known);
                if (track["stamina"]["total_stamina"].is_null()) complete = false;
            }
        }
        result["separate_stamina_total"] = separate_complete ? Json(separate) : Json(nullptr);
        result["known_separate_stamina"] = any_known ? Json(known_separate) : Json(nullptr);
        result["saved_stamina"] = separate_complete && !result["stamina"]["total_stamina"].is_null() ?
            Json(std::max<Quantity>(0, separate - Count(result["stamina"]["total_stamina"]))) : Json(nullptr);
        result["comparison_complete"] = complete; result["source_ledger"] = std::move(ledger); result["source_ledger_complete"] = ledger_complete;
    }
    return result;
}
} // namespace
Json CalculateExperienceMaterials(const Json& request, const AlgorithmCancelCheck& cancel_check) {
    try { Budget budget(request, cancel_check); auto result = Experience(request, budget); result["search"] = budget.Report(); return result; }
    catch (const Json::exception&) { Fail(); }
}
Json CalculateStamina(const Json& request, const AlgorithmCancelCheck& cancel_check) {
    try { Budget budget(request, cancel_check); auto result = Stamina(request, budget); result["search"] = budget.Report(); return result; }
    catch (const Json::exception&) { Fail(); }
}
Json CalculateCultivation(const Json& request, const AlgorithmCancelCheck& cancel_check) {
    try { Budget budget(request, cancel_check); auto result = Cultivation(request, budget); result["search"] = budget.Report(); return result; }
    catch (const Json::exception&) { Fail(); }
}
} // namespace equipment_manager
