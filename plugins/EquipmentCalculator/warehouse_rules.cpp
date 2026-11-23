#include "warehouse_rules.hpp"
#include "algorithm_support.hpp"
#include <array>
#include <map>
#include <vector>

namespace equipment_calculator {
namespace {
using namespace algorithm_detail;
struct UnknownRuleData {};
int Grade(const Json& value) {
    const auto grade = Id(value);
    constexpr std::array<std::string_view, 8> grades{"D", "C", "B", "A", "S", "SS", "SSS", "ACE"};
    const auto found = std::find(grades.begin(), grades.end(), grade); if (found == grades.end()) Fail();
    return static_cast<int>(found - grades.begin());
}
std::vector<std::string> Strings(const Json& input, const char* field) {
    if (!input.contains(field)) return {};
    if (!input.at(field).is_array() || input.at(field).size() > 128) Fail();
    std::vector<std::string> result;
    for (const auto& value : input.at(field)) {
        const auto id = Id(value); if (std::find(result.begin(), result.end(), id) != result.end()) Fail(); result.push_back(id);
    }
    return result;
}
bool Includes(const std::vector<std::string>& values, std::string_view id) {
    return std::find(values.begin(), values.end(), id) != values.end();
}
bool Boolean(const Json& input, const char* key, bool fallback) {
    if (!input.contains(key)) return fallback;
    if (!input.at(key).is_boolean()) Fail(); return input.at(key).get<bool>();
}
struct Rule {
    std::string id, action;
    std::vector<std::string> kinds, qualities, shapes, suits, states, roles, main, sub, required;
    bool enabled{}, has_minimum{}, has_maximum{}, preserve_locked{}, skip_locked{}, skip_discarded{}, has_shapes{}, has_suits{};
    int minimum{}, maximum{}, sub_match{};
};
Rule ParseRule(const Json& input) {
    if (!input.is_object()) Fail(); Rule result;
    result.id = Id(input.at("id")); result.enabled = Boolean(input, "enabled", true);
    if (!result.enabled) return result;
    result.action = Id(input.at("action"));
    if (result.action != "lock" && result.action != "discard" && result.action != "normal") Fail();
    result.kinds = Strings(input, "kinds"); result.qualities = Strings(input, "qualities");
    result.shapes = Strings(input, "shape_ids"); result.suits = Strings(input, "suit_ids");
    result.states = Strings(input, "states"); result.roles = Strings(input, "role_ids");
    result.main = Strings(input, "main_ids"); result.sub = Strings(input, "sub_ids"); result.required = Strings(input, "required_sub_ids");
    for (const auto& id : result.kinds) if (id != "module" && id != "core") Fail();
    for (const auto& id : result.states) if (id != "lock" && id != "discard" && id != "normal") Fail();
    for (const auto& id : result.required) if (!Includes(result.sub, id)) Fail();
    result.has_shapes = input.contains("shape_ids"); result.has_suits = input.contains("suit_ids");
    result.has_minimum = input.contains("min_grade"); result.has_maximum = input.contains("max_grade");
    if (result.has_minimum) result.minimum = Grade(input.at("min_grade"));
    if (result.has_maximum) result.maximum = Grade(input.at("max_grade"));
    if (result.has_minimum && result.has_maximum && result.minimum > result.maximum) Fail();
    result.sub_match = input.contains("sub_match") ? Integer(input.at("sub_match"), 1, 4) : std::min(4, static_cast<int>(result.sub.size()));
    const auto on_locked = input.value("on_locked", result.action == "normal" ? std::string("normal") : std::string("skip"));
    const auto on_discarded = input.value("on_discarded", std::string("normal"));
    if ((on_locked != "skip" && on_locked != "normal") || (on_discarded != "skip" && on_discarded != "normal")) Fail();
    result.skip_locked = on_locked == "skip"; result.skip_discarded = on_discarded == "skip";
    result.preserve_locked = Boolean(input, "preserve_locked", false);
    return result;
}
bool Matches(const Rule& rule, const Json& item, std::string_view state) {
    if (!rule.enabled) return false;
    const auto kind = Id(item.at("kind"));
    if ((!rule.kinds.empty() && !Includes(rule.kinds, kind)) ||
        (!rule.qualities.empty() && !Includes(rule.qualities, item.value("quality", std::string{}))) ||
        (!rule.states.empty() && !Includes(rule.states, state)) ||
        (state == "lock" && rule.skip_locked) || (state == "discard" && rule.skip_discarded)) return false;
    if (kind == "module" && rule.has_shapes && !Includes(rule.shapes, item.value("shape_id", std::string{}))) return false;
    if (kind == "core" && rule.has_suits && !Includes(rule.suits, item.value("suit_id", std::string{}))) return false;
    if (!rule.main.empty()) {
        if (kind != "core") return false;
        if (!item.contains("main_stat") || !item.at("main_stat").is_object() || !item.at("main_stat").contains("id")) throw UnknownRuleData{};
        if (!Includes(rule.main, item.at("main_stat").at("id").get<std::string>())) return false;
    }
    if (!rule.sub.empty()) {
        if (!item.value("sub_stats_complete", false) || !item.contains("sub_stats") || !item.at("sub_stats").is_array()) throw UnknownRuleData{};
        std::set<std::string> actual;
        for (const auto& stat : item.at("sub_stats")) actual.insert(Id(stat.at("id")));
        for (const auto& id : rule.required) if (!actual.contains(id)) return false;
        const auto matched = std::count_if(rule.sub.begin(), rule.sub.end(), [&](const auto& id) { return actual.contains(id); });
        if (matched < rule.sub_match) return false;
    }
    return true;
}
Json Preview(const Json& request, const Json& inventory, const AlgorithmCancelCheck& cancel) {
    Snapshot(request, inventory); Budget budget(request, cancel);
    if (!request.contains("rules") || !request.at("rules").is_array() || request.at("rules").size() > 256 ||
        !request.contains("roles") || !request.at("roles").is_array() || request.at("roles").size() > 64) Fail();
    std::vector<Rule> rules; std::set<std::string> rule_ids;
    for (const auto& input : request.at("rules")) {
        auto rule = ParseRule(input); if (!rule_ids.insert(rule.id).second) Fail(); rules.push_back(std::move(rule));
    }
    std::map<std::string, const Json*, std::less<>> roles;
    for (const auto& role : request.at("roles")) {
        const auto id = role.contains("id") ? Id(role.at("id")) : Id(role.at("role_id"));
        if (!roles.emplace(id, &role).second) Fail();
    }
    std::set<std::uint64_t> excluded, seen;
    if (request.contains("exclude_uids")) {
        if (!request.at("exclude_uids").is_array()) Fail();
        for (const auto& uid : request.at("exclude_uids")) excluded.insert(Key(uid));
    }
    const bool include_equipped = Boolean(request, "include_equipped", false);
    const bool strict_compatibility = Boolean(request, "strict_compatibility", false);
    const auto protected_shapes = Strings(request, "protected_shape_ids"), protected_suits = Strings(request, "protected_suit_ids");
    std::map<std::string, std::size_t> skipped; Json changes = Json::array(); bool complete{true}; std::size_t evaluated{};
    for (const auto& item : inventory.at("items")) {
        if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted);
        const auto uid = Key(item.at("uid")); if (!seen.insert(uid).second) Fail(AlgorithmFailure::invalid_inventory);
        if (excluded.contains(uid)) { ++skipped["reserved"]; continue; }
        if (!include_equipped && item.contains("character") && !item.at("character").is_null()) { ++skipped["equipped"]; continue; }
        if (Includes(protected_shapes, item.value("shape_id", std::string{})) || Includes(protected_suits, item.value("suit_id", std::string{}))) { ++skipped["protected"]; continue; }
        if (!item.contains("locked") || !item.at("locked").is_boolean() || !item.contains("discarded") || !item.at("discarded").is_boolean())
            Fail(AlgorithmFailure::invalid_inventory);
        const std::string current = item.at("discarded") == true ? "discard" : item.at("locked") == true ? "lock" : "normal";
        bool matched{}; ++evaluated;
        // Scoring is independent of allocation/rewind preferences. A role's
        // grade is reused across rules, but candidate blacklists, crit limits
        // and selected allocation target sets do not leak into warehouse scope.
        std::map<std::string, Json, std::less<>> scores;
        for (const auto& rule : rules) {
            if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted); ++budget.nodes;
            try { if (!Matches(rule, item, current)) continue; }
            catch (const UnknownRuleData&) { ++skipped["incomplete_data"]; complete = false; matched = true; break; }
            Json best; std::string best_role; bool unknown{}, eligible{};
            const bool needs_score = rule.has_minimum || rule.has_maximum || rule.action == "discard";
            if (needs_score) {
                std::vector<std::string> selected = rule.roles;
                if (selected.empty()) for (const auto& [id, role] : roles) { static_cast<void>(role); selected.push_back(id); }
                for (const auto& role_id : selected) {
                    if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted); ++budget.nodes;
                    const auto found = roles.find(role_id);
                    if (found == roles.end() || !found->second->contains("weights") || !found->second->at("weights").is_object()) { unknown = true; continue; }
                    const auto& role = *found->second;
                    const auto shape = item.value("shape_id", std::string{}), suit = item.value("suit_id", std::string{});
                    const auto compatibility_key = item.at("kind") == "module" ? "usable_shape_ids" : "usable_suit_ids";
                    if (strict_compatibility && !role.contains(compatibility_key)) { unknown = true; continue; }
                    if (role.contains(compatibility_key)) {
                        const auto compatible = Strings(role, compatibility_key);
                        if (!Includes(compatible, item.at("kind") == "module" ? shape : suit)) continue;
                    }
                    eligible = true;
                    auto score = scores.find(role_id);
                    if (score == scores.end()) {
                        Json preferences{{"weights", role.at("weights")}};
                        if (role.contains("main_weights")) preferences["main_weights"] = role.at("main_weights");
                        try { score = scores.emplace(role_id, ScoreEquipment(item, preferences)).first; }
                        catch (const InvalidAlgorithm& failure) {
                            if (failure.reason == AlgorithmFailure::cancelled || failure.reason == AlgorithmFailure::budget_exhausted) throw;
                            scores.emplace(role_id, Json{{"error_code", AlgorithmErrorCode(failure.reason)}}); unknown = true; continue;
                        }
                    }
                    if (score->second.contains("error_code") || !score->second.value("eligible", false)) { unknown = true; continue; }
                    if (best.is_null() || score->second.at("score") > best.at("score")) { best = score->second; best_role = role_id; }
                }
                // An uncertain earlier rule blocks fallthrough to a broader
                // destructive rule; absent values never stand in for grade D.
                if (unknown) { ++skipped["incomplete_score"]; complete = false; matched = true; break; }
                if (!eligible || best.is_null()) { ++skipped["no_eligible_role"]; matched = true; break; }
                const auto grade = Grade(best.at("grade"));
                if ((rule.has_minimum && grade < rule.minimum) || (rule.has_maximum && grade > rule.maximum)) continue;
            }
            matched = true;
            const auto state = rule.action == "normal" && rule.preserve_locked && current == "lock" ? current : rule.action;
            const bool already = state == "lock" ? item.at("locked") == true && item.at("discarded") == false :
                state == "discard" ? item.at("discarded") == true && item.at("locked") == false : item.at("locked") == false && item.at("discarded") == false;
            if (already) ++skipped["already_target"];
            else {
                Json change{{"equipment", item.at("uid")}, {"state", state}, {"rule_id", rule.id}, {"previous_state", current},
                    {"score", best.is_null() ? Json(nullptr) : best.at("score")}, {"grade", best.is_null() ? Json(nullptr) : best.at("grade")}};
                if (!best_role.empty()) change["role_id"] = best_role;
                changes.push_back(std::move(change));
            }
            break;
        }
        if (!matched) ++skipped["no_rule"];
    }
    return {{"source_snapshot_id", request.at("snapshot_id")}, {"complete", complete}, {"changes", changes}, {"skipped", skipped},
        {"evaluated", evaluated}, {"total", inventory.at("items").size()}, {"search", budget.Report()}};
}
} // namespace
nlohmann::json PreviewWarehouseRules(const nlohmann::json& request, const nlohmann::json& frozen_inventory,
                                    const AlgorithmCancelCheck& cancel_check) {
    try { return Preview(request, frozen_inventory, cancel_check); } catch (const Json::exception&) { Fail(); }
}
} // namespace equipment_calculator
